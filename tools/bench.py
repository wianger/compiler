#!/usr/bin/env python3
"""Benchmark already validated QEMU executables, excluding compile/link time."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import resource
import statistics
import struct
import subprocess
import time

from test import CACHE, ROOT, tool

TIMER = re.compile(rb"Timer@\d+-\d+: (\d+)H-(\d+)M-(\d+)S-(\d+)us")


def text_size(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError("Expected little-endian ELF64 executable")
    offset = struct.unpack_from("<Q", data, 40)[0]
    stride, count, names = struct.unpack_from("<HHH", data, 58)
    table = struct.unpack_from("<Q", data, offset + names*stride + 24)[0]
    for n in range(count):
        section = offset + n*stride
        name = table + struct.unpack_from("<I", data, section)[0]
        if data[name:data.index(b"\0", name)] == b".text":
            return struct.unpack_from("<Q", data, section + 32)[0]
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path, nargs="?")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--filter", default="")
    parser.add_argument("--output", type=Path, default=ROOT / "build/bench/report.json")
    opts = parser.parse_args()
    if opts.repeats < 1 or opts.warmups < 0:
        parser.error("--repeats must be positive and --warmups nonnegative")
    qemu = tool("qemu-riscv64", None)
    sysroot = CACHE / "usr/riscv64-linux-gnu" if CACHE.exists() else Path("/usr/riscv64-linux-gnu")
    _, limit = resource.getrlimit(resource.RLIMIT_STACK)
    resource.setrlimit(resource.RLIMIT_STACK, (limit, limit))
    # Serial execution on one available CPU reduces scheduling variance.
    if hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    results = []
    for source in sorted((ROOT / "test/performance").glob("*.sy")):
        if opts.filter not in source.stem:
            continue
        versions = [opts.baseline] + ([opts.candidate] if opts.candidate else [])
        executables = [v / "performance" / source.stem / "program" for v in versions]
        if not all(p.exists() for p in executables):
            raise RuntimeError(f"Missing executables for {source.name}; run tools/test.py first")
        data = source.with_suffix(".in").read_bytes() if source.with_suffix(".in").exists() else b""
        samples = [[] for _ in versions]
        timers = [[] for _ in versions]
        for iteration in range(opts.warmups + opts.repeats):
            # Alternate the order of versions to avoid consistently favoring one.
            for v in range(len(versions)) if iteration % 2 == 0 else reversed(range(len(versions))):
                start = time.perf_counter()
                run = subprocess.run([qemu, "-L", str(sysroot), str(executables[v])], input=data,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=240)
                elapsed = time.perf_counter() - start
                if run.returncode:
                    raise RuntimeError(f"{source.name}: exit {run.returncode}")
                if iteration >= opts.warmups:
                    samples[v].append(elapsed)
                    values = TIMER.findall(run.stderr)
                    timers[v].append(sum(int(h) * 3600 + int(m) * 60 + int(s) + int(us) / 1e6 for h, m, s, us in values))
        record = {"case": source.stem, "wall_samples": samples, "timer_samples": timers,
                  "wall": [statistics.median(s) for s in samples], "timer": [statistics.median(s) for s in timers],
                  "text_bytes": [text_size(p) for p in executables]}
        if len(versions) == 2:
            record["wall_speedup"] = record["wall"][0] / record["wall"][1]
            if record["timer"][0] > 0 and record["timer"][1] > 0:
                record["timer_speedup"] = record["timer"][0] / record["timer"][1]
        results.append(record)
        print(source.stem, "wall", " / ".join(f"{v:.6f}s" for v in record["wall"]),
              "timer", " / ".join(f"{v:.6f}s" for v in record["timer"]), flush=True)
        opts.output.parent.mkdir(parents=True, exist_ok=True)
        metadata = {"baseline": str(opts.baseline), "candidate": str(opts.candidate) if opts.candidate else None,
                    "repeats": opts.repeats, "warmups": opts.warmups, "host": os.uname().machine,
                    "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                    "results": results}
        opts.output.write_text(json.dumps(metadata, indent=2) + "\n")
    if opts.candidate and results:
        significant = [r for r in results if r["timer"][0] >= .01 and "timer_speedup" in r]
        if significant:
            geometric = math.exp(statistics.mean(math.log(r["timer_speedup"]) for r in significant))
            print(f"Timed-region geometric mean: {geometric:.3f}x ({len(significant)} cases >= 10ms baseline)")


if __name__ == "__main__":
    main()
