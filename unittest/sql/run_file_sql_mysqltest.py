#!/usr/bin/env python3
"""Run the unchanged official file_sql_basic test against an owned local server.

Its fixture includes use SSH/SCP. A temporary key enables only the duration of
this run; wrappers add that key's SSH configuration without changing commands.
The expected result is copied byte-for-byte into the run directory, so mysqltest
cannot create reject/log files inside the official suite.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone

from file_sql_contract_audit import manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--base-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    suite = repo / "tools/deploy/mysql_test/test_suite/file_sql"
    before = manifest(suite)
    base = args.base_dir.resolve()
    if not base.is_dir() or base == Path("/"):
        parser.error("--base-dir must be the dedicated test instance directory")
    run_dir = base / ("mysqltest-" + str(time.time_ns()))
    run_dir.mkdir()
    client = repo / "deps/3rd/u01/obclient/bin/obclient"
    client_args = [str(client), "--host=127.0.0.1", "--port=" + str(args.port), "--user=root"]
    setup = subprocess.run(client_args + ["--execute", "CREATE USER admin IDENTIFIED BY ''"],
                           capture_output=True, text=True, timeout=10)
    created_admin = setup.returncode == 0
    if not created_admin:
        raise RuntimeError("official test needs a fresh admin user; refusing to change an existing user: " + setup.stderr)
    ssh_dir = Path.home() / ".ssh"
    ssh_dir.mkdir(mode=0o700, exist_ok=True)
    authorized = ssh_dir / "authorized_keys"
    original = authorized.read_bytes() if authorized.exists() else None
    public_line = None
    result = None
    try:
        subprocess.run(client_args + ["--execute", "GRANT ALL ON *.* TO admin"],
                       check=True, capture_output=True, timeout=10)
        with tempfile.TemporaryDirectory(prefix="official-ssh-", dir=base) as ssh_temp:
            ssh_temp = Path(ssh_temp)
            key = ssh_temp / "id_ed25519"
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C",
                            "seekdb-file-sql-temporary-test", "-f", str(key)], check=True)
            public_line = key.with_suffix(".pub").read_bytes().strip() + b"\n"
            separator = b"\n" if original and not original.endswith(b"\n") else b""
            with authorized.open("ab") as output:
                output.write(separator + public_line)
            authorized.chmod(0o600)
            config = ssh_temp / "config"
            config.write_text("Host *\n  IdentityFile " + str(key) + "\n  IdentitiesOnly yes\n"
                              "  BatchMode yes\n  StrictHostKeyChecking accept-new\n"
                              "  UserKnownHostsFile " + str(ssh_temp / "known_hosts") + "\n")
            commands = ssh_temp / "bin"
            commands.mkdir()
            for command in ["ssh", "scp"]:
                wrapper = commands / command
                executable = shutil.which(command)
                if not executable:
                    raise RuntimeError(command + " is required by the official fixture includes")
                wrapper.write_text("#!/bin/sh\nexec " + shlex.quote(executable) + " -F "
                                   + shlex.quote(str(config)) + ' "$@"\n')
                wrapper.chmod(0o700)
            env = os.environ.copy()
            env["PATH"] = str(commands) + os.pathsep + env.get("PATH", "")
            check = subprocess.run([str(commands / "ssh"), "127.0.0.1", "true"],
                                   capture_output=True, text=True, timeout=10)
            if check.returncode:
                raise RuntimeError("local SSH required by official test failed: " + check.stderr)
            test = run_dir / "file_sql_basic.test"
            test.write_text("--disable_query_log\n"
                            "let $OBMYSQL_MS0 = 127.0.0.1;\n"
                            "let $OBMYSQL_PWD = ;\n"
                            "let $OBMYSQL_PORT = " + str(args.port) + ";\n"
                            "let $OBSERVER_DIR = " + str(base) + ";\n"
                            "let $PWD = " + str(repo / "tools/deploy") + ";\n"
                            "--source mysql_test/test_suite/file_sql/t/file_sql_basic.test\n")
            expected = run_dir / "file_sql_basic.result"
            shutil.copyfile(suite / "r/mysql/file_sql_basic.result", expected)
            command = [str(repo / "deps/3rd/u01/obclient/bin/mysqltest"), "--no-defaults",
                       "--host=127.0.0.1", "--port=" + str(args.port), "--user=root", "--database=test",
                       "--test-file=" + str(test), "--result-file=" + str(expected),
                       "--logdir=" + str(run_dir), "--basedir=" + str(repo / "tools/deploy")]
            result = subprocess.run(command, cwd=repo / "tools/deploy", env=env,
                                    capture_output=True, text=True, timeout=120)
            (run_dir / "stdout.txt").write_text(result.stdout)
            (run_dir / "stderr.txt").write_text(result.stderr)
    finally:
        if public_line is not None and authorized.exists():
            current = authorized.read_bytes()
            appended = (original or b"") + (b"\n" if original and not original.endswith(b"\n") else b"") + public_line
            if current == appended:
                if original is None:
                    authorized.unlink()
                else:
                    authorized.write_bytes(original)
            else:
                authorized.write_bytes(b"".join(line for line in current.splitlines(keepends=True)
                                                if line != public_line))
        subprocess.run(client_args + ["--execute", "DROP USER admin"], capture_output=True, timeout=10)
    report = dict(recorded_at=datetime.now(timezone.utc).isoformat(),
                  kind="unchanged official mysqltest file_sql_basic", returncode=result.returncode,
                  stdout=result.stdout, stderr=result.stderr, run_directory=str(run_dir),
                  official_files_unchanged=before == manifest(suite), official_file_sha256=before,
                  temporary_ssh_authorization_removed=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("OFFICIAL_MYSQLTEST_EXIT", result.returncode, flush=True)
    if result.returncode:
        print(result.stdout + result.stderr, flush=True)
    return result.returncode or (0 if report["official_files_unchanged"] else 1)


if __name__ == "__main__":
    raise SystemExit(main())
