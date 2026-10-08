#!/usr/bin/env python3
"""Times each bench/*.co program against its Go twin in bench/go/.

usage: bench/run.py <coc> [-O<n>] [--runs N]

Go is looked up as $GO, then `go` on PATH; without it only co is timed.
Each program runs N times (default 5) and the fastest time is reported.
Both versions must print the same output.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def best_of(exe, runs):
    best, out = None, None
    for _ in range(runs):
        start = time.perf_counter()
        r = subprocess.run([exe], capture_output=True, text=True, check=True)
        elapsed = time.perf_counter() - start
        best = elapsed if best is None else min(best, elapsed)
        out = r.stdout
    return best, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("coc")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("opt", nargs="?", default="-O2")
    args = ap.parse_args()
    go = os.environ.get("GO") or shutil.which("go")

    names = sorted(f[:-3] for f in os.listdir(HERE) if f.endswith(".co"))
    print("%-14s %10s %10s %8s" % ("bench", "co", "go", "co/go"))
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GOCACHE=os.path.join(tmp, "gocache"), GOFLAGS="-mod=mod")
        for name in names:
            co_exe = os.path.join(tmp, name)
            subprocess.run([args.coc, "build", os.path.join(HERE, name + ".co"), "-o", co_exe, args.opt], check=True)
            co_t, co_out = best_of(co_exe, args.runs)
            go_src = os.path.join(HERE, "go", name + ".go")
            if not go or not os.path.exists(go_src):
                print("%-14s %9.3fs %10s %8s" % (name, co_t, "-", "-"))
                continue
            go_exe = os.path.join(tmp, name + ".go.bin")
            subprocess.run([go, "build", "-o", go_exe, go_src], check=True, cwd=os.path.join(HERE, "go"), env=env)
            go_t, go_out = best_of(go_exe, args.runs)
            if go_out != co_out:
                sys.exit("%s: output differs\n  co: %r\n  go: %r" % (name, co_out, go_out))
            print("%-14s %9.3fs %9.3fs %8.2f" % (name, co_t, go_t, co_t / go_t))


if __name__ == "__main__":
    main()
