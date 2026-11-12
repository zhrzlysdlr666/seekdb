#!/usr/bin/env python3
"""Probe the Lab2 SQL contract without editing the official test suite.

This is a focused diagnostic, not a replacement for file_sql_basic mysqltest.
The server must already be running with secure_file_priv allowing --work-dir.
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone


def manifest(directory):
    return {
        str(path.relative_to(directory)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(directory.rglob("*")) if path.is_file()
    }


def sql_literal(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=10006)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--user", default="root")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[], help="Only run named cases (repeatable)")
    parser.add_argument("--restricted-root", action="store_true",
                        help="Also check /etc and symlink rejection; secure_file_priv must exclude /etc")
    parser.add_argument("--copy-race-log", type=Path, help="Require the publication race hook to run")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    suite = repo / "tools/deploy/mysql_test/test_suite/file_sql"
    client = repo / "deps/3rd/u01/obclient/bin/obclient"
    before = manifest(suite)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    records = []
    blocked_by = None
    with tempfile.TemporaryDirectory(prefix="contract-", dir=args.work_dir) as temp:
        fixtures = Path(temp) / "official"
        fixtures.mkdir()
        for source in (suite / "data").iterdir():
            if source.suffix == ".b64":
                (fixtures / source.stem).write_bytes(base64.b64decode(source.read_bytes()))
            elif source.is_file():
                shutil.copyfile(source, fixtures / source.name)
        probe = Path(temp) / "probe"
        probe.mkdir()
        unique = probe / "unique.csv"
        unique.write_text("id,city,amount\n1,Osaka,10.5\n2,Tokyo,20\n3,Osaka,7.5\n", encoding="utf-8")
        countries = probe / "countries.csv"
        countries.write_text("city,country\nOsaka,Japan\n", encoding="utf-8")
        options_csv = probe / "options.csv"
        options_csv.write_text('id;label;value\n1;"A;B";NA\n2;"";4\n', encoding="utf-8")
        typed_csv = probe / "typed.tsv"
        typed_csv.write_text('n,x,b,s,d,dt\n1,1.5,true,ok,2026-08-01,2026-08-01 10:11:12\n', encoding="utf-8")
        batch_csv = probe / "batch.tsv"
        batch_csv.write_text("id,payload,group_id\n" + "".join(
            f'{i},row-{i:05d}' + "x" * 1100 + f'-tail,{i % 3}\n' for i in range(257)), encoding="utf-8")
        (probe / "directory.csv").mkdir()
        escape_dir = probe / "escape"
        escape_dir.mkdir()
        (escape_dir / "outside.txt").symlink_to("/etc/passwd")
        exports = probe / "exports"
        exports.mkdir()
        existing_link = exports / "existing-link.csv"
        existing_link.symlink_to(unique)
        original_unique_hash = hashlib.sha256(unique.read_bytes()).hexdigest()
        copy_table = "test.file_sql_copy_" + Path(temp).name.replace("-", "_")
        basic = "read_csv(" + sql_literal(fixtures / "basic.csv") + ")"
        simple = "read_csv(" + sql_literal(unique) + ")"

        # expected_rows=None checks success only; exact outputs are used elsewhere.
        cases = [
            ("default_off", "SELECT @@session.enable_file_sql", False, None, "0\n"),
            ("disabled_error", "SELECT count(*) FROM " + basic, False, 1235, None),
            ("official_csv_count", "SELECT count(*) FROM " + basic, True, None, "3\n"),
            ("positional_option", "SELECT * FROM read_csv(" + sql_literal(unique) + ", 'xml')", True, 1064, None),
            ("sample_size_10", "SELECT * FROM read_csv(" + sql_literal(unique) + ", sample_size = 10)", True, 1235, None),
            ("named_csv_options", "SELECT count(*) FROM read_csv(" + sql_literal(unique) + ", header = true, sample_size = -1)", True, None, "3\n"),
            ("official_csv_projection", "SELECT id, name, score, active, day, created, replace(note, char(10), '<NL>'), id__2 FROM " + basic + " f ORDER BY id", True, None,
             '1\tAlice\t98.5\t1\t2026-08-01\t2026-08-01 10:11:12\thello,<NL>world\t101\n2\tBob\t87\t0\tNULL\tNULL\tplain\t102\n3\tChen\t90.25\t1\t2026-08-03\t2026-08-03 12:13:14\tsay "hi"\t103\n'),
            ("jsonl", "SELECT id, name, score, active, day, note FROM read_json(" + sql_literal(fixtures / "basic.jsonl") + ", format = 'newline_delimited') f ORDER BY id", True, None,
             "1\tAlice\t98.5\t1\tNULL\tNULL\n2\tBob\tNULL\t0\t2026-08-02\tNULL\n3\tChen\t90\t1\tNULL\tok\n"),
            ("parquet", "SELECT count(*), min(c1), max(c1), sum(c1) FROM read_parquet(" + sql_literal(fixtures / "simple.parquet") + ")", True, None, "7\t0\t6\t21\n"),
            ("glob", "SELECT substring_index(file, '/', -1) FROM glob(" + sql_literal(fixtures / "*") + ") ORDER BY file", True, None,
             "basic.csv\nbasic.jsonl\ninvalid.txt\nsimple.parquet\n"),
            ("describe_csv", "DESCRIBE SELECT * FROM " + simple, True, None,
             "id\tBIGINT\tNO\tNULL\tNULL\tNULL\ncity\tVARCHAR\tNO\tNULL\tNULL\tNULL\namount\tDOUBLE\tNO\tNULL\tNULL\tNULL\n"),
            ("missing_file", "SELECT * FROM read_csv(" + sql_literal(probe / "nope.csv") + ")", True, 1017, None),
            ("directory_input", "SELECT * FROM read_csv(" + sql_literal(probe) + ")", True, 1210, None),
            ("csv_group_by", "SELECT city, SUM(amount) FROM " + simple + " f GROUP BY city ORDER BY city", True, None, "Osaka\t18\nTokyo\t20\n"),
            ("csv_left_join", "SELECT f.id, c.country FROM " + simple + " f LEFT JOIN read_csv(" + sql_literal(countries) + ") c ON f.city=c.city ORDER BY f.id", True, None,
             "1\tJapan\n2\tNULL\n3\tJapan\n"),
            ("csv_correlated_rescan", "SELECT f.id, (SELECT count(*) FROM " + simple + " g WHERE g.id<=f.id) FROM " + simple + " f ORDER BY f.id", True, None,
             "1\t1\n2\t2\n3\t3\n"),
            ("copy_csv", "COPY (SELECT id, city FROM " + simple + ") TO " + sql_literal(exports / "out.csv") + " (format csv, header, nullstr 'NULL')", True, None, ""),
            ("ordinary_desc", "DESC SELECT 1", False, None, None),
            ("disabled_json", "SELECT * FROM read_json('/missing')", False, 1235, None),
            ("disabled_ndjson", "SELECT * FROM read_ndjson('/missing')", False, 1235, None),
            ("disabled_parquet", "SELECT * FROM read_parquet('/missing')", False, 1235, None),
            ("disabled_glob", "SELECT * FROM glob('/missing/*')", False, 1235, None),
            ("csv_custom_options", "SELECT * FROM read_csv(" + sql_literal(options_csv) + ", delimiter=';', nullstr='NA') AS f ORDER BY id", True, None,
             "1\tA;B\tNULL\n2\t\t4\n"),
            ("csv_without_header", "SELECT * FROM read_csv(" + sql_literal(unique) + ", header=false) LIMIT 1", True, None, "id\tcity\tamount\n"),
            ("csv_unknown_option", "SELECT * FROM read_csv(" + sql_literal(unique) + ", unknown_option=true)", True, 1210, None),
            ("csv_duplicate_option", "SELECT * FROM read_csv(" + sql_literal(unique) + ", header=true, header=false)", True, 1210, None),
            ("csv_bad_delimiter", "SELECT * FROM read_csv(" + sql_literal(unique) + ", delimiter='too long')", True, 1210, None),
            ("sample_size_uint64", "SELECT * FROM read_csv(" + sql_literal(unique) + ", sample_size=18446744073709551615)", True, 1235, None),
            ("csv_option_identifier", "SELECT * FROM read_csv(" + sql_literal(unique) + ", delimiter=missing_identifier)", True, 1210, None),
            ("csv_option_function", "SELECT * FROM read_csv(" + sql_literal(unique) + ", delimiter=no_such_function())", True, 1210, None),
            ("describe_projection", "DESC SELECT amount AS total, city FROM " + simple, True, None,
             "total\tDOUBLE\tNO\tNULL\tNULL\tNULL\ncity\tVARCHAR\tNO\tNULL\tNULL\tNULL\n"),
            ("describe_nullable", "DESCRIBE SELECT * FROM read_csv(" + sql_literal(options_csv) + ", delimiter=';', nullstr='NA')", True, None,
             "id\tBIGINT\tNO\tNULL\tNULL\tNULL\nlabel\tVARCHAR\tNO\tNULL\tNULL\tNULL\nvalue\tBIGINT\tYES\tNULL\tNULL\tNULL\n"),
            ("glob_empty", "SELECT count(*) FROM glob(" + sql_literal(probe / "*.unmatched") + ")", True, None, "0\n"),
            ("glob_regular_sorted", "SELECT substring_index(file, '/', -1) FROM glob(" + sql_literal(probe / "*.csv") + ")", True, None,
             "countries.csv\noptions.csv\nunique.csv\n"),
            ("describe_glob", "DESCRIBE SELECT * FROM glob(" + sql_literal(probe / "*.csv") + ")", True, None,
             "file\tVARCHAR\tNO\tNULL\tNULL\tNULL\n"),
            ("explain_file", "EXPLAIN SELECT city, SUM(amount) FROM " + simple + " GROUP BY city", True, None, None),
            ("describe_empty_result", "DESCRIBE SELECT id FROM " + simple + " WHERE 1=0", True, None,
             "id\tBIGINT\tNO\tNULL\tNULL\tNULL\n"),
            ("describe_derived", "DESC SELECT id FROM (SELECT id FROM " + simple + ") AS nested_file", True, None,
             "id\tBIGINT\tNO\tNULL\tNULL\tNULL\n"),
            ("describe_outer_join", "DESC SELECT f.id, c.country FROM " + simple + " f LEFT JOIN read_csv(" + sql_literal(countries) + ") c ON f.city=c.city", True, None,
             "id\tBIGINT\tNO\tNULL\tNULL\tNULL\ncountry\tVARCHAR\tYES\tNULL\tNULL\tNULL\n"),
            ("describe_headers", "DESCRIBE SELECT id FROM " + simple, True, None,
             "column_name\tcolumn_type\tnull\tkey\tdefault\textra\nid\tBIGINT\tNO\tNULL\tNULL\tNULL\n"),
            ("describe_six_types", "DESCRIBE SELECT * FROM read_csv(" + sql_literal(typed_csv) + ")", True, None,
             "n\tBIGINT\tNO\tNULL\tNULL\tNULL\nx\tDOUBLE\tNO\tNULL\tNULL\tNULL\nb\tBOOLEAN\tNO\tNULL\tNULL\tNULL\ns\tVARCHAR\tNO\tNULL\tNULL\tNULL\nd\tDATE\tNO\tNULL\tNULL\tNULL\ndt\tDATETIME\tNO\tNULL\tNULL\tNULL\n"),
            ("copy_disabled", "COPY (SELECT 1) TO " + sql_literal(exports / "disabled.csv"), False, 1235, None),
            ("copy_disabled_missing_table", "COPY test.file_sql_missing_table TO '/missing/output.csv'", False, 1235, None),
            ("copy_disabled_bad_query", "COPY (SELECT missing_identifier) TO '/missing/output.csv'", False, 1235, None),
            ("copy_readback", "COPY (SELECT id, city FROM " + simple + " ORDER BY id) TO " + sql_literal(exports / "roundtrip.csv") + " WITH (format='csv', header=true); SELECT * FROM read_csv(" + sql_literal(exports / "roundtrip.csv") + ") ORDER BY id", True, None,
             "1\tOsaka\n2\tTokyo\n3\tOsaka\n"),
            ("copy_existing", "COPY (SELECT 1 AS id) TO " + sql_literal(unique) + " (format csv, header)", True, 1086, None),
            ("copy_empty", "COPY (SELECT id FROM " + simple + " WHERE 1=0) TO " + sql_literal(exports / "empty.csv") + " (format csv, header); SELECT count(*) FROM read_csv(" + sql_literal(exports / "empty.csv") + ")", True, None, "0\n"),
            ("copy_empty_no_header", "COPY (SELECT 1 WHERE 1=0) TO " + sql_literal(exports / "empty-no-header.csv"), True, None, ""),
            ("copy_empty_existing", "COPY (SELECT 1 WHERE 1=0) TO " + sql_literal(unique), True, 1086, None),
            ("copy_custom_options", "COPY (SELECT * FROM read_csv(" + sql_literal(options_csv) + ", delimiter=';', nullstr='NA')) TO " + sql_literal(exports / "custom.csv") + " (format csv, header, delimiter=';', nullstr='NA'); SELECT * FROM read_csv(" + sql_literal(exports / "custom.csv") + ", delimiter=';', nullstr='NA') ORDER BY id", True, None,
             "1\tA;B\tNULL\n2\t\t4\n"),
            ("copy_unsupported_format", "COPY (SELECT 1) TO " + sql_literal(exports / "unsupported.csv") + " (format xml)", True, 1235, None),
            ("copy_unknown_option", "COPY (SELECT 1) TO " + sql_literal(exports / "unknown.csv") + " (bad_option true)", True, 1210, None),
            ("copy_null_empty", "COPY (SELECT 1 AS id, 'NULL' AS s, '' AS e, NULL AS n) TO " + sql_literal(exports / "nulls.csv") + " (format csv, header); SELECT id, s, length(e), n IS NULL FROM read_csv(" + sql_literal(exports / "nulls.csv") + ")", True, None,
             "1\tNULL\t0\t1\n"),
            ("copy_null_token", "COPY (SELECT 1 AS id, NULL AS a, '' AS b, 'NULL' AS c) TO " + sql_literal(exports / "tokens.csv") + " (format csv, header, nullstr 'NULL'); SELECT a IS NULL, b IS NULL, c IS NULL, c FROM read_csv(" + sql_literal(exports / "tokens.csv") + ", nullstr='NULL')", True, None,
             "1\t0\t0\tNULL\n"),
            ("copy_escape_roundtrip", "COPY (SELECT 1 AS id, concat('say ', char(34), 'hi', char(34), char(92), char(10), 'world') AS s) TO " + sql_literal(exports / "escaped.csv") + " (format csv, header); SELECT id, hex(s) FROM read_csv(" + sql_literal(exports / "escaped.csv") + ")", True, None,
             "1\t73617920226869225C0A776F726C64\n"),
            ("copy_union", "COPY (SELECT id FROM " + simple + " WHERE id<=2 UNION ALL SELECT 9) TO " + sql_literal(exports / "union.csv") + " WITH (header false, delimiter '|'); SELECT * FROM read_csv(" + sql_literal(exports / "union.csv") + ", header=false, delimiter='|')", True, None,
             "1\n2\n9\n"),
            ("copy_constant_union", "COPY (SELECT 1 AS id UNION ALL SELECT 2) TO " + sql_literal(exports / "constant-union.csv") + " (header); SELECT id FROM read_csv(" + sql_literal(exports / "constant-union.csv") + ") ORDER BY id", True, None,
             "1\n2\n"),
            ("copy_invalid_utf8", "COPY (SELECT CAST(X'E9' AS BINARY) AS s) TO " + sql_literal(exports / "invalid-utf8.csv") + " (header)", True, 1210, None),
            ("copy_publish_identity", "COPY (SELECT 1 AS id) TO " + sql_literal(exports / "race.csv") + " (header); SELECT id FROM read_csv(" + sql_literal(exports / "race.csv") + ")", True, None,
             "1\n"),
            ("copy_table", "CREATE TABLE " + copy_table + " (id INT PRIMARY KEY, label VARCHAR(32)); INSERT INTO " + copy_table + " VALUES (1,'hello'), (2,''); COPY " + copy_table + " TO " + sql_literal(exports / "table.csv") + " (header); DROP TABLE " + copy_table + "; SELECT * FROM read_csv(" + sql_literal(exports / "table.csv") + ") ORDER BY id", True, None,
             "1\thello\n2\t\n"),
            ("copy_existing_symlink", "COPY (SELECT 1) TO " + sql_literal(existing_link), True, 1086, None),
            ("copy_invalid_option_pair", "COPY (SELECT 1) TO " + sql_literal(exports / "bad-pair.csv") + " (delimiter='|', quote='|')", True, 1210, None),
            ("copy_duplicate_option", "COPY (SELECT 1) TO " + sql_literal(exports / "duplicate.csv") + " (header, header false)", True, 1210, None),
            ("copy_failure_cleanup", "COPY (SELECT JSON_OBJECT('x', 1)) TO " + sql_literal(exports / "failed.csv"), True, 1235, None),
            ("ordinary_outfile", "SELECT 1 INTO OUTFILE " + sql_literal(exports / "native-outfile.txt"), False, None, ""),
            ("scan_batch_count", "SELECT count(*), sum(id) FROM read_csv(" + sql_literal(batch_csv) + ")", True, None,
             "257\t32896\n"),
            ("scan_batch_projection", "SELECT payload, id FROM read_csv(" + sql_literal(batch_csv) + ") WHERE id>=254 ORDER BY payload DESC", True, None,
             "".join(f'row-{i:05d}' + "x" * 1100 + f'-tail\t{i}\n' for i in (256, 255, 254))),
            ("scan_batch_filter_aggregate", "SELECT group_id, count(*), sum(id) FROM read_csv(" + sql_literal(batch_csv) + ") WHERE id>=16 GROUP BY group_id HAVING count(*)>=80 ORDER BY group_id", True, None,
             "0\t80\t10920\n1\t81\t11016\n2\t80\t10840\n"),
            ("scan_batch_join", "SELECT count(*), sum(a.id) FROM read_csv(" + sql_literal(batch_csv) + ") a JOIN read_csv(" + sql_literal(batch_csv) + ") b ON a.id=b.id", True, None,
             "257\t32896\n"),
            ("scan_ambiguous_column", "SELECT id FROM " + simple + " a JOIN " + simple + " b ON a.id=b.id", True, 1052, None),
            ("scan_count_no_columns", "SELECT count(*) FROM read_csv(" + sql_literal(batch_csv) + ")", True, None, "257\n"),
            ("explain_file_scalar", "EXPLAIN SELECT /*+ OPT_PARAM('ROWSETS_MAX_ROWS', 0) */ id FROM " + simple, True, None, None),
            ("scan_plan_isolation", "SELECT count(*) FROM " + simple + "; SELECT count(*) FROM read_csv(" + sql_literal(countries) + ")", True, None, "3\n1\n"),
            ("scan_forced_nlj", "SELECT /*+ LEADING(a b) USE_NL(b) */ a.id, b.city FROM " + simple + " a JOIN " + simple + " b ON a.id=b.id ORDER BY a.id", True, None,
             "1\tOsaka\n2\tTokyo\n3\tOsaka\n"),
            ("explain_file_join", "EXPLAIN SELECT /*+ LEADING(a b) USE_NL(b) */ a.id, b.city FROM " + simple + " a JOIN " + simple + " b ON a.id=b.id", True, None, None),
            ("scan_correlated_no_unnest", "SELECT f.id, (SELECT /*+ NO_UNNEST */ count(*) FROM " + simple + " g WHERE g.id<=f.id) FROM " + simple + " f ORDER BY f.id", True, None,
             "1\t1\n2\t2\n3\t3\n"),
            ("glob_count_projection", "SELECT count(*) FROM glob(" + sql_literal(probe / "*.csv") + ")", True, None, "3\n"),
            ("file_read_only_update", "UPDATE " + simple + " SET amount=0 WHERE id=1", True, (1235, 1288), None),
            ("file_read_only_delete", "DELETE FROM " + simple + " WHERE id=1", True, (1235, 1103), None),
            ("file_read_only_lock", "SELECT * FROM " + simple + " FOR UPDATE", True, 1235, None),
            ("file_read_only_derived_lock", "SELECT * FROM (SELECT * FROM " + simple + ") f FOR UPDATE", True, 1235, None),
        ]
        for batch_size in (0, 1, 16, 31):
            cases.append(("scan_rows_" + str(batch_size),
                "SELECT /*+ OPT_PARAM('ROWSETS_MAX_ROWS', " + str(batch_size) + ") */ id, length(payload), left(payload,9) FROM read_csv(" + sql_literal(batch_csv) + ") WHERE id IN (0,16,256) ORDER BY payload DESC",
                True, None, "256\t1114\trow-00256\n16\t1114\trow-00016\n0\t1114\trow-00000\n"))
        if args.restricted_root:
            cases.extend([
                ("glob_escape_parent", "SELECT * FROM glob('/etc/*')", True, 1227, None),
                ("glob_escape_symlink", "SELECT * FROM glob(" + sql_literal(escape_dir / "*") + ")", True, 1227, None),
                ("copy_escape_parent", "COPY (SELECT 1) TO '/etc/file_sql_contract_forbidden.csv' (format csv)", True, 1227, None),
            ])
        if args.case:
            known = {case[0] for case in cases}
            if set(args.case) - known:
                parser.error("unknown cases: " + ", ".join(sorted(set(args.case) - known)))
            cases = [case for case in cases if case[0] in args.case]
        for name, sql, enabled, expected_error, expected_rows in cases:
            if blocked_by is not None:
                records.append(dict(name=name, sql=sql, passed=False,
                                    status="skipped", reason=blocked_by))
                print("SKIP " + name + ": " + blocked_by, flush=True)
                continue
            query = ("SET SESSION enable_file_sql = ON;\n" if enabled else "") + sql
            try:
                command = [str(client), "--host=" + args.host, "--port=" + str(args.port),
                     "--user=" + args.user, "--batch", "--raw",
                     "--connect-timeout=5", "--default-character-set=utf8mb4", "--execute", query]
                if name != "describe_headers":
                    command.append("--skip-column-names")
                proc = subprocess.run(command,
                    capture_output=True, text=True, timeout=30)
            except subprocess.TimeoutExpired:
                blocked_by = "client timed out during " + name
                records.append(dict(name=name, sql=sql, passed=False,
                                    status="transport_error", reason=blocked_by))
                print("BLOCKED " + blocked_by, flush=True)
                continue
            match = re.search(r"ERROR\s+(\d+)", proc.stderr)
            actual_error = int(match.group(1)) if match else None
            error_matches = (actual_error in expected_error if isinstance(expected_error, tuple)
                             else actual_error == expected_error)
            passed = (error_matches and (
                expected_error is not None or (
                    proc.returncode == 0 and (expected_rows is None or proc.stdout == expected_rows))))
            if name == "explain_file":
                passed = passed and "FILE_SCAN" in proc.stdout
                passed = passed and re.search(r"1 - output.*rowset=\d+", proc.stdout) is not None
                scan_lines = [line.split("|") for line in proc.stdout.splitlines() if "FILE_SCAN" in line]
                passed = passed and len(scan_lines) == 1 and scan_lines[0][4].strip() == "3"
            if name == "explain_file_scalar":
                passed = passed and "FILE_SCAN" in proc.stdout and "rowset=" not in proc.stdout
            if name == "explain_file_join":
                passed = passed and proc.stdout.count("FILE_SCAN") == 2 and "NESTED-LOOP" in proc.stdout
            if name == "ordinary_desc":
                passed = passed and bool(proc.stdout.strip()) and "FILE SCAN" not in proc.stdout
            file_checks = {}
            if name.startswith(("copy_", "file_read_only_")):
                file_checks["no_staging_files"] = not list(exports.glob(".seekdb-copy-*"))
                file_checks["existing_input_preserved"] = (
                    hashlib.sha256(unique.read_bytes()).hexdigest() == original_unique_hash)
                if name == "copy_null_token" and actual_error is None:
                    file_checks["exact_csv_bytes"] = (exports / "tokens.csv").read_bytes() == (
                        b'"id","a","b","c"\n1,NULL,"","NULL"\n')
                if name == "copy_publish_identity" and args.copy_race_log:
                    file_checks["race_injected"] = args.copy_race_log.exists() and (
                        "staging entry replaced" in args.copy_race_log.read_text())
                if name == "copy_empty_no_header":
                    file_checks["empty_file_published"] = (exports / "empty-no-header.csv").exists() and (
                        (exports / "empty-no-header.csv").read_bytes() == b"")
                absent = {
                    "copy_disabled": "disabled.csv", "copy_failure_cleanup": "failed.csv",
                    "copy_unsupported_format": "unsupported.csv", "copy_unknown_option": "unknown.csv",
                    "copy_duplicate_option": "duplicate.csv", "copy_invalid_option_pair": "bad-pair.csv",
                    "copy_invalid_utf8": "invalid-utf8.csv",
                }
                if name in absent:
                    file_checks["failed_output_absent"] = not (exports / absent[name]).exists()
                passed = passed and all(file_checks.values())
            record = dict(name=name, sql=sql, expected_error=expected_error,
                          expected_rows=expected_rows, actual_error=actual_error,
                          returncode=proc.returncode, stdout=proc.stdout,
                          stderr=proc.stderr, file_checks=file_checks, passed=passed)
            records.append(record)
            if actual_error in (2002, 2003, 2006, 2013):
                record["status"] = "transport_error"
                blocked_by = "connection failed during " + name
            else:
                record["status"] = "passed" if passed else "failed"
            print(record["status"].upper() + " " + name + " error=" + str(actual_error), flush=True)
        # A failed table-copy case must not leave a test table behind.
        if any(case[0] == "copy_table" for case in cases) and blocked_by is None:
            subprocess.run([str(client), "--host=" + args.host, "--port=" + str(args.port),
                            "--user=" + args.user, "--execute", "DROP TABLE IF EXISTS " + copy_table],
                           capture_output=True, text=True, timeout=30)
    after = manifest(suite)
    report = dict(kind="focused SQL contract audit, not complete mysqltest", port=args.port,
                  recorded_at=datetime.now(timezone.utc).isoformat(),
                  valid_feature_baseline=blocked_by is None,
                  blocked_by=blocked_by,
                  fixture_source=str(suite), official_files_unchanged=before == after,
                  official_file_sha256=before, cases=records,
                  passed=sum(item["passed"] for item in records), total=len(records))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Official files unchanged: " + str(report["official_files_unchanged"]))
    print("Report: " + str(args.report))
    return 0 if all(item["passed"] for item in records) and before == after else 1


if __name__ == "__main__":
    raise SystemExit(main())
