#!/usr/bin/env python3
"""Run generated RV64 assembly under QEMU; no external language compiler."""
import argparse
import concurrent.futures
import json
import math
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sysy-toolchain/root"


def tool(name, override):
    candidate = override or shutil.which(name)
    if not candidate and (CACHE / "usr/bin" / name).exists():
        candidate = str(CACHE / "usr/bin" / name)
    if not candidate:
        flag = "qemu" if name == "qemu-riscv64" else name.split("-")[-1]
        raise RuntimeError(f"Missing {name}; supply its path with --{flag}")
    return candidate


def compare(actual, expected):
    a, b = actual.split(), expected.split()
    if len(a) != len(b):
        return f"output token count: got {len(a)}, expected {len(b)}"
    for n, (x, y) in enumerate(zip(a, b)):
        if x == y:
            continue
        # Integers and punctuation compare exactly. Float output accepts the
        # usual relative/absolute tolerance, in hexadecimal or decimal form.
        if re.search(r"[.pPeE]", y):
            try:
                xf = float.fromhex(x) if "0x" in x.lower() else float(x)
                yf = float.fromhex(y) if "0x" in y.lower() else float(y)
                if math.isclose(xf, yf, rel_tol=1e-6, abs_tol=1e-6) or math.isnan(xf) and math.isnan(yf):
                    continue
            except ValueError:
                pass
        return f"token {n}: got {x!r}, expected {y!r}"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, default=[ROOT / "test"])
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/compiler")
    parser.add_argument("--as", dest="assembler")
    parser.add_argument("--ld", dest="linker")
    parser.add_argument("--qemu")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--runtime", type=Path, default=ROOT / "libs/libsysy_riscv.a")
    parser.add_argument("--output", type=Path, default=ROOT / "build/test-results")
    parser.add_argument("--opt", type=int, choices=range(4), default=2)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--exclude-tensor", action="store_true")
    opts = parser.parse_args()
    if opts.jobs < 1 or opts.timeout <= 0:
        parser.error("--jobs and --timeout must be positive")
    _, stack_limit = resource.getrlimit(resource.RLIMIT_STACK)
    resource.setrlimit(resource.RLIMIT_STACK, (stack_limit, stack_limit))
    assembler = tool("riscv64-linux-gnu-as", opts.assembler)
    linker = tool("riscv64-linux-gnu-ld", opts.linker)
    qemu = tool("qemu-riscv64", opts.qemu)
    sysroot = opts.sysroot or (CACHE / "usr/riscv64-linux-gnu" if CACHE.exists() else Path("/usr/riscv64-linux-gnu"))
    lib = sysroot / "lib"
    for path in [opts.compiler, opts.runtime, lib / "crt1.o", lib / "libc.so.6"]:
        if not path.exists():
            raise RuntimeError(f"Missing {path}")
    env = os.environ.copy()
    hostlibs = CACHE / "usr/lib/x86_64-linux-gnu"
    if hostlibs.exists():
        env["LD_LIBRARY_PATH"] = str(hostlibs) + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    cases = sorted({p.resolve() for path in opts.paths for p in (path.rglob("*.sy") if path.is_dir() else [path])})
    skipped = []
    if opts.exclude_tensor:
        skipped = [p for p in cases if re.search(r"\btensor\s+(?:int|float)\b", p.read_text())]
        cases = [p for p in cases if p not in skipped]
    opts.output.mkdir(parents=True, exist_ok=True)

    def run(case):
        name = f"{case.parent.name}/{case.stem}"
        directory = opts.output / case.parent.name / case.stem
        directory.mkdir(parents=True, exist_ok=True)
        assembly, obj, executable = directory / "program.s", directory / "program.o", directory / "program"
        started = time.monotonic()
        try:
            commands = [
                [str(opts.compiler.resolve()), f"-O{opts.opt}", str(case), "-o", str(assembly)],
                [assembler, "-march=rv64gc", "-mabi=lp64d", str(assembly), "-o", str(obj)],
                [linker, "-o", str(executable), "-dynamic-linker", "/lib/ld-linux-riscv64-lp64d.so.1",
                 "-rpath-link", str(lib), str(lib / "crt1.o"), str(lib / "crti.o"), str(obj),
                 str(opts.runtime.resolve()), str(lib / "libc.so.6"), str(lib / "crtn.o")],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, env=env, timeout=opts.timeout)
                if result.returncode:
                    return {"case": name, "ok": False, "error": result.stderr.decode(errors="replace")}
            input_file = case.with_suffix(".in")
            result = subprocess.run([qemu, "-L", str(sysroot), str(executable)],
                                    input=input_file.read_bytes() if input_file.exists() else b"",
                                    capture_output=True, env=env, timeout=opts.timeout)
            (directory / "stdout").write_bytes(result.stdout)
            (directory / "stderr").write_bytes(result.stderr)
            if result.returncode < 0:
                return {"case": name, "ok": False, "error": f"signal {-result.returncode}"}
            actual = result.stdout.decode(errors="replace") + "\n" + str(result.returncode) + "\n"
            (directory / "actual.out").write_text(actual)
            error = compare(actual, case.with_suffix(".out").read_text())
            return {"case": name, "ok": error is None, "error": error, "seconds": round(time.monotonic() - started, 3)}
        except subprocess.TimeoutExpired:
            return {"case": name, "ok": False, "error": f"timeout after {opts.timeout}s"}
        except Exception as error:
            return {"case": name, "ok": False, "error": str(error)}

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=opts.jobs) as pool:
        for result in pool.map(run, cases):
            results.append(result)
            print(f"{'PASS' if result['ok'] else 'FAIL'} {result['case']}" +
                  (f": {result['error']}" if result.get("error") else ""), flush=True)
    failures = sum(not r["ok"] for r in results)
    summary = {"optimization": opts.opt, "passed": len(results) - failures, "failed": failures,
               "skipped": [str(p.relative_to(ROOT)) for p in skipped], "results": results}
    (opts.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{len(results) - failures}/{len(results)} passed; {len(skipped)} tensor cases excluded")
    return int(failures != 0 or not cases)


if __name__ == "__main__":
    raise SystemExit(main())
