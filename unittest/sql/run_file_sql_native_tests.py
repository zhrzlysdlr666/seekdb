#!/usr/bin/env python3
"""Compile and run native FILE_AS_SQL gtests using an existing release build.

Run from any directory: python3 unittest/sql/run_file_sql_native_tests.py
Requires build_release/compile_commands.json and its already-built dependency
archives. This compiles individual current production sources; it never invokes
the production build, changes CMake, starts a server, or links stale SQL unity
objects. Use --build-dir to retain the executable, commands, XML, and logs.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(command, directory, log):
    print("RUN", log.stem, flush=True)
    result = subprocess.run(command, cwd=directory, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text(result.stdout)
    if result.returncode:
        print(result.stdout[-24000:], flush=True)
        raise subprocess.CalledProcessError(result.returncode, command)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--gtest-filter", default="*")
    parser.add_argument("--syntax-only", action="store_true", help="Compile-check JSONL only")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / "build_release"
    output = args.build_dir.resolve() if args.build_dir else Path(tempfile.mkdtemp(prefix="file-sql-native-"))
    output.mkdir(parents=True, exist_ok=True)
    print("ARTIFACTS", output, flush=True)
    database = json.loads((build / "compile_commands.json").read_text())
    reference = next(entry for entry in database if entry["file"].endswith("/ob_parser.cpp"))
    original = shlex.split(reference["command"])
    flags = []
    i = 1
    while i < len(original):
        flag = original[i]
        if flag in ("-c", "-o"):
            i += 2
            continue
        if flag.startswith("-O") or flag == "-g":
            i += 1
            continue
        flags.append(flag)
        i += 1
    flags += ["-O0", "-g0", "-fno-color-diagnostics"]
    sources = ["src/sql/engine/basic/ob_jsonl_file_scanner.cpp"]
    if not args.syntax_only:
        sources += ["src/sql/engine/basic/ob_file_scan_utils.cpp",
                    "unittest/sql/engine/basic/test_file_scan_utils.cpp",
                    "unittest/sql/engine/basic/test_jsonl_file_scanner.cpp",
                    "unittest/sql/parser/test_file_sql_parser.cpp",
                    "unittest/sql/file_sql_scalar_test.cpp"]
    commands = []
    objects = []
    for source in sources:
        obj = output / (Path(source).stem + ".o")
        command = [original[0], *flags]
        command += ["-fsyntax-only"] if args.syntax_only else ["-c", "-o", str(obj)]
        command += [str(root / source)]
        commands.append(command)
        run(command, reference["directory"], output / (Path(source).stem + ".compile.log"))
        objects.append(str(obj))
    if args.syntax_only:
        (output / "commands.json").write_text(json.dumps(commands, indent=2))
        print("JSONL_SYNTAX_EXIT 0", flush=True)
        return 0
    observer = build / "src/observer"
    link = shlex.split((observer / "CMakeFiles/seekdb.dir/link.txt").read_text())
    compiler = link[0]
    arguments = []
    excluded = {"liboceanbase_static.a", "libob_sql_static.a", "libob_storage_static.a", "libob_share_static.a"}
    i = 1
    while i < len(link):
        flag = link[i]
        if flag == "-o":
            i += 2
            continue
        if flag.endswith(".o") or Path(flag).name in excluded:
            i += 1
            continue
        arguments.append(flag)
        i += 1
    deps = root / "deps/3rd/usr/local/oceanbase/deps/devel/lib"
    executable = output / "file_sql_native_tests"
    hook = output / "libfile_sql_intermediate_symlink_race.so"
    hook_source = root / "unittest/sql/file_sql_intermediate_symlink_race.c"
    hook_command = ["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-o", str(hook), str(hook_source), "-ldl"]
    commands.append(hook_command)
    run(hook_command, root, output / "race_hook.compile.log")
    command = [compiler, *objects, str(deps / "libgtest_main.a"), str(deps / "libgtest.a"),
               *arguments, "-o", str(executable)]
    commands.append(command)
    (output / "commands.json").write_text(json.dumps(commands, indent=2))
    run(command, observer, output / "link.log")
    command = [str(executable), "--gtest_filter=" + args.gtest_filter,
               "--gtest_output=xml:" + str(output / "results.xml")]
    environment = os.environ.copy()
    existing_preload = environment.get("LD_PRELOAD")
    environment["LD_PRELOAD"] = str(hook) if not existing_preload else str(hook) + ":" + existing_preload
    result = subprocess.run(command, cwd=output, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    (output / "results.log").write_text(result.stdout)
    print(result.stdout[-30000:], flush=True)
    print("NATIVE_TEST_EXIT", result.returncode, flush=True)
    return result.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
