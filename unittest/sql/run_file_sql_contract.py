#!/usr/bin/env python3
"""Run focused Lab2 checks on an owned local instance, then stop that instance.

Requires an already built seekdb and a dedicated --work-dir (data persists there).
Never attaches to or changes an already running server.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--port", type=int, default=10006)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--race-library", type=Path)
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--extended-fixtures", type=Path)
    parser.add_argument("--official", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    base = args.work_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    secure_root = Path(os.path.commonpath([str(base), str(repo)]))
    if secure_root == Path("/"):
        parser.error("place --work-dir alongside the repository so /etc remains outside the test root")
    client = str(repo / "deps/3rd/u01/obclient/bin/obclient")
    client_args = [client, "--host=127.0.0.1", "--port=" + str(args.port),
                   "--user=root", "--batch", "--raw"]
    with socket.socket() as check_port:
        if check_port.connect_ex(("127.0.0.1", args.port)) == 0:
            parser.error("port is occupied; will not change that server")
    env = os.environ.copy()
    race_log = base / "copy-link-race.log"
    if args.race_library:
        if not args.race_library.resolve().is_file():
            parser.error("race library does not exist")
        env["LD_PRELOAD"] = str(args.race_library.resolve())
        env["FILE_SQL_COPY_RACE_LOG"] = str(race_log)
        race_log.unlink(missing_ok=True)
    with (base / "console.log").open("w") as log:
        server = subprocess.Popen([
            str(repo / "build_release/src/observer/seekdb"), "--nodaemon", "--port", str(args.port),
            "--base-dir", str(base), "--data-dir", str(base / "store"),
            "--parameter", "cpu_count=2", "--parameter", "memory_limit=3G",
            "--parameter", "datafile_size=256M", "--parameter", "datafile_maxsize=1G",
            "--parameter", "log_disk_size=2G"], cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(90):
                if server.poll() is not None:
                    raise RuntimeError("server exited: " + str(server.returncode))
                check = subprocess.run(client_args + ["--execute", "SELECT 1"],
                                       capture_output=True, timeout=5)
                if check.returncode == 0:
                    break
                time.sleep(1)
            else:
                raise RuntimeError("server startup timed out")
            # The server only allows changing secure_file_priv through its Unix socket.
            setup = subprocess.run([
                client, "--socket=" + str(base / "run/sql.sock"), "--user=root", "--execute",
                "SET GLOBAL secure_file_priv='" + str(secure_root).replace("'", "''") + "'"],
                capture_output=True, text=True, timeout=10)
            if setup.returncode != 0:
                raise RuntimeError("could not configure dedicated test root: " + setup.stderr)
            command = ["python3", "unittest/sql/file_sql_contract_audit.py", "--port", str(args.port),
                       "--work-dir", str(base), "--restricted-root", "--report", str(args.report)]
            for case in args.case:
                command += ["--case", case]
            if args.race_library:
                command += ["--copy-race-log", str(race_log)]
            audit = subprocess.run(command, cwd=repo)
            extended_code = 0
            if args.extended_fixtures:
                extended_report = args.report.with_name(args.report.stem + "-extended.json")
                extended = subprocess.run([
                    "python3", "unittest/sql/file_sql_extended_audit.py", "--port", str(args.port),
                    "--fixture-dir", str(args.extended_fixtures.resolve()),
                    "--report", str(extended_report)], cwd=repo)
                extended_code = extended.returncode
            official_code = 0
            if args.official:
                official_report = args.report.with_name(args.report.stem + "-official.json")
                official = subprocess.run([
                    "python3", "unittest/sql/run_file_sql_mysqltest.py", "--port", str(args.port),
                    "--base-dir", str(base), "--report", str(official_report)], cwd=repo)
                official_code = official.returncode
            if args.demo:
                demo_sql = (repo / "docs/lab2/demo/sql-interface.sql").read_text().replace(
                    "__COPY_DEMO_PATH__", str(base / ("demo-export-" + str(time.time_ns()) + ".csv")))
                (repo / "docs/lab2/demo/actual-sql.sql").write_text(demo_sql)
                demo = subprocess.run(client_args, input=demo_sql,
                                      capture_output=True, text=True, timeout=30)
                (repo / "docs/lab2/demo/actual-output.txt").write_text(demo.stdout + demo.stderr)
                print("DEMO_EXIT", demo.returncode, flush=True)
                if demo.returncode != 0:
                    raise RuntimeError(demo.stderr)
            print("AUDIT_EXIT", audit.returncode, "SERVER_ALIVE", server.poll() is None, flush=True)
            return audit.returncode or extended_code or official_code
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == "__main__":
    raise SystemExit(main())
