#!/usr/bin/env python3
"""Runs the co test suite.

Each tests/*.co file is a program; so is each tests/<dir>/ with a main.co,
built as a whole directory (its packages live in subdirectories). A test's
.co file (main.co for directories) declares its expectations in comments:
  // expect: <line>      expected stdout line (in order)
  // error: <substring>  expected compile error (the file must fail to compile)
  // exit: <code>        expected exit code (default 0)
  // stderr: <substring> expected text in the program's stderr
"""
import os
import subprocess
import sys
import tempfile


def parse(path):
    exp = {"expect": [], "error": [], "exit": 0, "stderr": []}
    with open(path) as f:
        for line in f:
            s = line.strip()
            for key in ("expect", "error", "exit", "stderr"):
                tag = "// " + key + ":"
                if s.startswith(tag):
                    val = s[len(tag):]
                    val = val[1:] if val.startswith(" ") else val
                    if key == "exit":
                        exp["exit"] = int(val)
                    else:
                        exp[key].append(val)
    return exp


def run_one(coc, path, tmp):
    is_dir = os.path.isdir(path)
    exp = parse(os.path.join(path, "main.co") if is_dir else path)
    out = os.path.join(tmp, os.path.basename(path).removesuffix(".co"))
    build = subprocess.run([coc, "build", path, "-o", out], capture_output=True, text=True)
    if exp["error"]:
        if build.returncode == 0:
            return "expected a compile error, but it compiled"
        missing = [e for e in exp["error"] if e not in build.stderr]
        if missing:
            return "missing expected error(s) %r in:\n%s" % (missing, build.stderr)
        return None
    if build.returncode != 0:
        return "compile failed:\n" + build.stderr
    env = dict(os.environ, CO_DEBUG_ALLOC="1")
    run = subprocess.run([out], capture_output=True, text=True, timeout=30, env=env)
    if run.returncode != exp["exit"]:
        return "exit code %d, expected %d\nstderr: %s" % (run.returncode, exp["exit"], run.stderr)
    got = run.stdout.splitlines()
    if got != exp["expect"]:
        return "stdout mismatch\n  expected: %r\n  got:      %r" % (exp["expect"], got)
    if exp["exit"] == 0 and "live allocations at exit: 0\n" not in run.stderr:
        return "memory leak or double free: " + run.stderr.strip()
    for s in exp["stderr"]:
        if s not in run.stderr:
            return "stderr missing %r, got %r" % (s, run.stderr)
    return None


def main():
    coc, test_dir = sys.argv[1], sys.argv[2]
    files = sorted(f for f in os.listdir(test_dir)
                   if f.endswith(".co") or os.path.exists(os.path.join(test_dir, f, "main.co")))
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in files:
            err = run_one(coc, os.path.join(test_dir, name), tmp)
            if err:
                failed += 1
                print("FAIL %s: %s" % (name, err))
            else:
                print("ok   %s" % name)
    print("\n%d passed, %d failed" % (len(files) - failed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
