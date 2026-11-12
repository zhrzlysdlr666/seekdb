#!/usr/bin/env python3
"""Exercise format boundaries and cross-format SQL on an already owned server."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from datetime import datetime, timezone

from file_sql_contract_audit import manifest, sql_literal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=10006)
    parser.add_argument("--fixture-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    suite = repo / "tools/deploy/mysql_test/test_suite/file_sql"
    before = manifest(suite)
    fixtures = args.fixture_dir.resolve()
    client = repo / "deps/3rd/u01/obclient/bin/obclient"
    cases = []

    def add(name, sql, rows=None, error=None, contains=None, discard_stdout=False):
        case = dict(name=name, sql=sql, expected_rows=rows,
                    expected_error=error, error_contains=contains)
        if discard_stdout:
            case["discard_stdout"] = True
        cases.append(case)

    def table(name, function="read_parquet"):
        return function + "(" + sql_literal(fixtures / name) + ")"

    with tempfile.TemporaryDirectory(prefix="extra-", dir=fixtures) as temp:
        temp = Path(temp)
        for item in json.loads((repo / "unittest/sql/file_sql_jsonl_cases.json").read_text())["cases"]:
            path = temp / (item["name"] + ".jsonl")
            path.write_text(item["fixture_utf8"], encoding="utf-8")
            add(item["name"], item["sql"].replace("{path}", sql_literal(path)),
                item.get("expected_rows"), item.get("expected_error"), item.get("error_contains"))
        csv_cases = [
            ("csv_bom_crlf_multiline", b'\xef\xbb\xbfid,note\r\n1,"a\r\nb"\r\n2,"say ""hi"""',
             "SELECT id, replace(replace(note,char(13),'<CR>'),char(10),'<LF>') FROM {table}",
             '1\ta<CR><LF>b\n2\tsay "hi"\n', None),
            ("csv_header_names", b'id,id,id\n1,2,3\n', "SELECT id,id__2,id__3 FROM {table}", "1\t2\t3\n", None),
            ("csv_empty_header", b',id\na,1\n', "SELECT column0,id FROM {table}", "a\t1\n", None),
            ("csv_int_bounds", b'n\n9223372036854775807\n-9223372036854775808\n',
             "SELECT n FROM {table}", "9223372036854775807\n-9223372036854775808\n", None),
            ("csv_overflow_schema", b'n\n9223372036854775808\n', "DESCRIBE SELECT * FROM {table}",
             "n\tVARCHAR\tNO\tNULL\tNULL\tNULL\n", None),
            ("csv_all_null", b'a,b\n,1\n,2\n', "DESCRIBE SELECT * FROM {table}",
             "a\tVARCHAR\tYES\tNULL\tNULL\tNULL\nb\tBIGINT\tNO\tNULL\tNULL\tNULL\n", None),
            ("csv_date_widen", b'd\n2024-02-29\n2024-03-01T10:11:12.123456\n',
             "SELECT date_format(d,'%Y-%m-%d %H:%i:%s.%f') FROM {table}",
             "2024-02-29 00:00:00.000000\n2024-03-01 10:11:12.123456\n", None),
            ("csv_invalid_calendar", b'd\n1900-02-29\n', "DESCRIBE SELECT * FROM {table}",
             "d\tVARCHAR\tNO\tNULL\tNULL\tNULL\n", None),
            ("csv_null_tokens", b'a,b,c,d\nNULL,,"","NULL"\n',
             "SELECT a IS NULL,b IS NULL,c IS NULL,d IS NULL,length(c),d FROM {table}",
             "0\t1\t0\t0\t0\tNULL\n", None),
            ("csv_unclosed_quote", b'a\n"bad\n', "SELECT count(*) FROM {table}", None, 1210),
            ("csv_post_quote_junk", b'a\n"ok"junk\n', "SELECT count(*) FROM {table}", None, 1210),
            ("csv_width_error", b'a,b\n1\n', "SELECT count(*) FROM {table}", None, 1210),
            ("csv_bad_utf8", b'a\n\xff\n', "SELECT count(*) FROM {table}", None, 1210),
            ("csv_too_many_columns", b','.join([b'a'] * 4097) + b'\n',
             "SELECT count(*) FROM {table}", None, 1210),
            ("csv_field_limit", b'a\n' + b'x' * (16 * 1024 * 1024 + 1) + b'\n',
             "SELECT count(*) FROM {table}", None, 1210),
        ]
        for name, data, sql, rows, error in csv_cases:
            path = temp / (name + ".csv")
            path.write_bytes(data)
            add(name, sql.replace("{table}", "read_csv(" + sql_literal(path) + ")"), rows, error)
        no_header = temp / "no-header.csv"
        no_header.write_text("1,x\n2,y\n")
        add("csv_no_header_names", "SELECT column0,column1 FROM read_csv(" + sql_literal(no_header)
            + ",header=false)", "1\tx\n2\ty\n")
        fifo = temp / "input.fifo"
        os.mkfifo(fifo)
        add("csv_fifo_nonblocking", "SELECT * FROM read_csv(" + sql_literal(fifo) + ")", error=1210)
        add("jsonl_unknown_option", "SELECT * FROM read_json(" + sql_literal(fixtures / "cross.jsonl")
            + ",header=true)", error=1210)
        add("jsonl_bad_format", "SELECT * FROM read_json(" + sql_literal(fixtures / "cross.jsonl")
            + ",format='array')", error=1235)
        add("jsonl_duplicate_option", "SELECT * FROM read_json(" + sql_literal(fixtures / "cross.jsonl")
            + ",format='newline_delimited',format='newline_delimited')", error=1210)
        for filename in ["orders.parquet", "orders_v2.parquet", "orders_plain.parquet"]:
            source = table(filename)
            tag = filename.removesuffix(".parquet")
            add(tag + "_aggregate", "SELECT count(*),round(sum(total),2),count(channel),sum(paid) FROM "
                + source, "6\t150.74\t5\t3\n")
            add(tag + "_projection", "SELECT channel,order_id,total IS NULL,paid IS NULL FROM "
                + source + " ORDER BY order_id", "web\t101\t0\t0\nshop\t102\t0\t0\nNULL\t103\t0\t0\nweb\t104\t0\t0\nshop\t105\t1\t1\nweb\t106\t0\t0\n")
            add(tag + "_date_time", "SELECT order_date,date_format(stamp,'%Y-%m-%d %H:%i:%s.%f'),ratio"
                " FROM " + source + " WHERE order_id=101", "2026-08-01\t2026-08-01 10:11:12.123456\t1.25\n")
        add("parquet_schema", "DESCRIBE SELECT * FROM " + table("orders.parquet"), "".join(
            f"{name}\t{kind}\tYES\tNULL\tNULL\tNULL\n" for name, kind in [
                ("order_id", "BIGINT"), ("person_id", "BIGINT"), ("total", "DOUBLE"),
                ("channel", "VARCHAR"), ("paid", "BOOLEAN"), ("order_date", "DATE"),
                ("stamp", "DATETIME"), ("ratio", "DOUBLE")]))
        for name, sql, rows in [
            ("parquet_prune_good", "SELECT good FROM ", "1\n2\n3\n"),
            ("parquet_prune_count", "SELECT count(*) FROM ", "3\n")]:
            add(name, sql + table("pruning.parquet"), rows)
        add("parquet_prune_bad", "SELECT bad FROM " + table("pruning.parquet"), error=1210)
        for filename, error in [("orders_zstd.parquet",1235), ("int96.parquet",1235),
                                ("nested.parquet",1235), ("truncated.parquet",1210),
                                ("footer_overflow.parquet",1210)]:
            add(filename.removesuffix(".parquet"), "SELECT count(*) FROM " + table(filename), error=error)
        add("parquet_empty_count", "SELECT count(*) FROM " + table("empty.parquet"), "0\n")
        add("parquet_empty_rows", "SELECT * FROM " + table("empty.parquet"), "")
        retained_capacity = table("parquet_retained_capacity.parquet")
        add("parquet_retained_capacity_count", "SELECT count(*) FROM " + retained_capacity, "22\n")
        add("parquet_retained_capacity_single_column",
            "SELECT length(col_00) FROM " + retained_capacity,
            str(13 * 1024 * 1024) + "\n" + "7\n" * 21)
        add("parquet_retained_capacity_all_columns", "SELECT * FROM " + retained_capacity,
            error=1210, discard_stdout=True)
        add("parquet_millis", "SELECT date_format(stamp,'%Y-%m-%d %H:%i:%s.%f') FROM "
            + table("millis.parquet"), "1970-01-01 00:00:01.234000\n1969-12-31 23:59:59.999000\n")
        for function, filename in [("read_csv","big.csv"),("read_json","big.jsonl"),
                                    ("read_parquet","big.parquet")]:
            source = table(filename, function)
            add(function + "_big_aggregate", "SELECT count(*),sum(id),min(id),max(id),min(length(value)),max(length(value)) FROM "
                + source, "5000\t12502500\t1\t5000\t1111\t1111\n")
            for batch in [0, 1, 16, 31]:
                add(function + "_big_batch_" + str(batch), "SELECT /*+ OPT_PARAM('ROWSETS_MAX_ROWS', '"
                    + str(batch) + "') */ id,substr(value,1,11),length(value) FROM " + source
                    + " WHERE id>4997 ORDER BY value DESC", "5000\tvalue_05000\t1111\n4999\tvalue_04999\t1111\n4998\tvalue_04998\t1111\n")
        people = table("people.csv", "read_csv")
        add("csv_parquet_left_join", "SELECT p.id,p.name,o.order_id FROM " + people
            + " p LEFT JOIN " + table("orders.parquet") + " o ON p.id=o.person_id ORDER BY p.id,o.order_id",
            "1\tAlice\t101\n1\tAlice\t102\n2\tBob\t103\n3\tCy\tNULL\n4\tDi\t104\n4\tDi\t105\n5\tEd\tNULL\n6\tFay\t106\n")
        add("csv_json_left_join", "SELECT p.id,j.score FROM " + people + " p LEFT JOIN "
            + table("cross.jsonl","read_json") + " j ON p.id=j.id ORDER BY p.id",
            "1\t10\n2\tNULL\n3\t30\n4\tNULL\n5\tNULL\n6\tNULL\n")
        for other, function in [("big.jsonl","read_json"),("big.parquet","read_parquet")]:
            add("cross_" + function + "_big_join", "SELECT count(*) FROM " + table("big.csv","read_csv")
                + " c JOIN " + table(other,function) + " j ON c.id=j.id AND c.value=j.value", "5000\n")
        add("json_parquet_join", "SELECT j.id,count(*) FROM " + table("cross.jsonl","read_json")
            + " j JOIN " + table("orders.parquet") + " p ON j.id=p.person_id GROUP BY j.id ORDER BY j.id", "1\t2\n")
        add("cross_correlated_rescan", "SELECT /*+ NO_REWRITE */ c.id,(SELECT /*+ NO_UNNEST */ count(*) FROM "
            + table("cross.jsonl","read_json") + " j WHERE j.id<=c.id) FROM " + people
            + " c WHERE c.id<=3 ORDER BY c.id", "1\t1\n2\t1\n3\t2\n")
        add("cross_explain", "EXPLAIN SELECT c.id FROM " + people + " c JOIN "
            + table("orders.parquet") + " p ON c.id=p.person_id")
        records = []
        for case in cases:
            proc = subprocess.run([str(client), "--host=127.0.0.1", "--port=" + str(args.port),
                                   "--user=root", "--batch", "--raw", "--skip-column-names"],
                                  input="SET enable_file_sql=ON;\n" + case["sql"] + ";\n",
                                  stdout=subprocess.DEVNULL if case.get("discard_stdout") else subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, timeout=45)
            match = re.search(r"ERROR (\d+)", proc.stderr)
            actual_error = int(match.group(1)) if match else None
            passed = actual_error == case["expected_error"]
            if case["expected_error"] is None:
                passed = passed and proc.returncode == 0 and (
                    case["expected_rows"] is None or proc.stdout == case["expected_rows"])
            if case["error_contains"]:
                passed = passed and case["error_contains"] in proc.stderr
            if case["name"] == "cross_explain":
                passed = passed and proc.stdout.count("FILE_SCAN") == 2 and "JOIN" in proc.stdout
            records.append(dict(**case, actual_error=actual_error, stdout=proc.stdout,
                                stderr=proc.stderr, returncode=proc.returncode, passed=passed))
            print(("PASSED " if passed else "FAILED ") + case["name"] + " error=" + str(actual_error), flush=True)
            if actual_error in (2002, 2003, 2006, 2013):
                break
    report = dict(recorded_at=datetime.now(timezone.utc).isoformat(),
                  kind="independent format and cross-format SQL audit", cases=records,
                  passed=sum(x["passed"] for x in records), total=len(cases),
                  official_files_unchanged=before == manifest(suite))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("EXTENDED", report["passed"], "/", report["total"], flush=True)
    return 0 if report["passed"] == report["total"] and report["official_files_unchanged"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
