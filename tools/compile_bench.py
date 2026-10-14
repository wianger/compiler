#!/usr/bin/env python3
"""Alternate two native compilers; record per-process time/RSS and PASS timings."""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def run(compiler, source, mode, stats):
    with tempfile.TemporaryFile() as errors:
        start = time.perf_counter()
        child = subprocess.Popen([str(compiler.resolve()), "-O3", mode, str(source),
                                  "-o", "/dev/null"] + (["--stats"] if stats else []),
                                 stdout=subprocess.DEVNULL, stderr=errors, close_fds=False)
        _, status, usage = os.wait4(child.pid, 0)
        child.returncode = os.waitstatus_to_exitcode(status)
        elapsed = time.perf_counter() - start
        errors.seek(0)
        diagnostics = errors.read().decode()
        if child.returncode:
            raise RuntimeError(diagnostics)
        return {"wall": elapsed, "cpu": usage.ru_utime + usage.ru_stime,
                "stats": json.loads(diagnostics) if stats else None}


def profile(compiler, source, mode, stats):
    # A small exec'd launcher prevents the Python parent's resident memory from
    # becoming the compiler's inherited high-water mark on Linux.
    command = ["/usr/bin/time", "-f", "rss=%M", str(compiler.resolve()), "-O3", mode,
               str(source), "-o", "/dev/null"] + (["--stats"] if stats else [])
    run = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if run.returncode:
        raise RuntimeError(run.stderr)
    lines = run.stderr.splitlines()
    return int(lines[-1].removeprefix("rss=")), json.loads(lines[0]) if stats else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--tests", type=Path, default=Path("test"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--hir", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("build/architecture/compile.json"))
    opts = parser.parse_args()
    if opts.repeats < 1:
        parser.error("--repeats must be positive")
    if hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    results = []
    for source in sorted(opts.tests.rglob("*.sy")):
        samples = [[], []]
        for iteration in range(opts.repeats + 1):
            for v in ([0, 1] if iteration % 2 == 0 else [1, 0]):
                sample = run([opts.baseline, opts.candidate][v], source,
                             "--emit-hir" if opts.hir else "-S", False)
                if iteration:
                    samples[v].append(sample)
        mode = "--emit-hir" if opts.hir else "-S"
        baseline_rss, _ = profile(opts.baseline,source,mode,False)
        candidate_rss, stats = profile(opts.candidate,source,mode,True)
        results.append({"case": str(source), "samples": samples, "stats": stats, "rss_kib": [baseline_rss,candidate_rss],
                        **{name: [statistics.median(s[name] for s in group) for group in samples]
                           for name in ("wall", "cpu")}})
    report = {"baseline": str(opts.baseline), "candidate": str(opts.candidate),
              "mode": "hir" if opts.hir else "assembly", "repeats": opts.repeats,
              "affinity": sorted(os.sched_getaffinity(0)), "results": results}
    opts.output.parent.mkdir(parents=True, exist_ok=True)
    opts.output.write_text(json.dumps(report, indent=2) + "\n")
    for name in ("wall", "cpu", "rss_kib"):
        totals = [sum(r[name][v] for r in results) for v in range(2)]
        ratios = [r[name][0] / r[name][1] for r in results if r[name][0] and r[name][1]]
        print(name, totals, "ratio", totals[0] / totals[1], "geomean",
              math.exp(statistics.mean(math.log(r) for r in ratios)))


if __name__ == "__main__":
    main()
