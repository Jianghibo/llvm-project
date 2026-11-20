#!/usr/bin/env python3
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Reproduce controlled BOLT prefetch experiments and retain their artifacts."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time


def integers(value):
    return list(dict.fromkeys(int(item) for item in value.split(",")))


def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def code_bytes(path):
    # The fixture is an ELF64 little-endian AArch64 executable. Count executable
    # sections, not file padding, symtab, or the command-line note.
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError(f"not a little-endian ELF64 file: {path}")
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count = struct.unpack_from("<HH", data, 58)
    return sum(
        struct.unpack_from("<Q", data, offset + i * size + 32)[0]
        for i in range(count)
        if struct.unpack_from("<Q", data, offset + i * size + 8)[0] & 4
    )


def measured(command, log):
    # wait4 gives this command's peak RSS, rather than the cumulative maximum
    # over unrelated children returned by getrusage(RUSAGE_CHILDREN).
    start = time.monotonic()
    with log.open("wb") as output:
        pid = os.fork()
        if pid == 0:
            try:
                os.dup2(output.fileno(), 1)
                os.dup2(output.fileno(), 2)
                os.execvp(str(command[0]), list(map(str, command)))
            except OSError as error:
                os.write(2, str(error).encode())
                os._exit(127)
        _, status, usage = os.wait4(pid, 0)
    result = {
        "command": list(map(str, command)),
        "seconds": time.monotonic() - start,
        "max_rss_kib": usage.ru_maxrss,
        "exit_code": (
            os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
        ),
        "log": str(log),
    }
    return result


def make_large(path, functions, body):
    # The caller checks both original and rewritten programs. One function
    # exceeds the default analysis limit, exercising the bounded skip path.
    expected = 16 * body
    lines = [
        ".text", ".globl _start", ".type _start,%function", "_start:",
        "adrp x0,array", "add x0,x0,:lo12:array", "mov x1,#16", "bl f0",
        f"movz x3,#{expected & 65535}",
        f"movk x3,#{expected >> 16},lsl #16", "cmp x0,x3", "cset x0,ne",
        "mov x8,#93", "svc #0", ".size _start,.-_start",
    ]
    for i in range(functions + 1):
        count = body if i < functions else 21000
        name = f"f{i}" if i < functions else "oversized"
        lines += [
            f".globl {name}", f".type {name},%function", f"{name}:",
            "mov x2,#0", f".Lloop{i}:", "ldr x3,[x0],#8",
            f".rept {count}", "add x2,x2,#1", ".endr", "subs x1,x1,#1",
            f"b.ne .Lloop{i}", "mov x0,x2", "ret", f".size {name},.-{name}",
        ]
    lines += [".data", ".p2align 6", "array:", ".zero 128"]
    path.write_text("\n".join(lines) + "\n")


def select_candidate_choices(report):
    """Apply single-PC gates to recorded results; this does not validate combinations."""
    if not report["completed"]:
        return []
    config = report["configuration"]
    minimum, noise_limit = config["min_speedup_pct"], config["max_noise_pct"]
    ready = (not report["failures"] and config["samples"] >= 3
             and minimum is not None and noise_limit is not None)
    variants = report["variants"]
    choices = []
    for candidate in report["candidates"]:
        for threads in config["threads"]:
            case = [r for r in report["summary"]
                    if r["mode"] == candidate["function"] and r["threads"] == threads]
            base = next(r for r in case if r["variant"] == "base")
            results = [r for r in case
                       if variants[r["variant"]].get("pc") == candidate["pc"]]
            eligible = [r for r in results
                        if ready and variants[r["variant"]].get("inserted")
                        and r["speedup_pct"] >= minimum
                        and base["median_absolute_deviation_pct"] <= noise_limit
                        and r["median_absolute_deviation_pct"] <= noise_limit]
            best = min(eligible or results, key=lambda r: r["median"])
            info = variants[best["variant"]]
            choices.append(dict(
                **candidate, threads=threads, best=best["variant"],
                distance=info["distance"], predicted_distance=report["predicted_distance"],
                eligible=bool(eligible), hints=info["hints"] if eligible else None,
                validation_scope="single_pc_same_input",
                combination_validated=False, independent_input_validated=False,
            ))
    return choices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--cpus", type=integers)
    parser.add_argument("--threads", type=integers, default=[1, 4])
    parser.add_argument("--distances", type=integers, default=[16, 64])
    parser.add_argument("--lines", type=integers, default=[0])
    parser.add_argument("--per-pc", action="store_true",
                        help="scan one load at a time using build-id-bound hints")
    parser.add_argument("--latency", type=int, default=0)
    parser.add_argument("--iteration-cycles", type=int, default=0)
    parser.add_argument("--modes", default="sequential,strided,records,streams2,streams4")
    parser.add_argument("--mib", type=int, default=256)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--perf-stat", action="store_true")
    parser.add_argument("--events", default="cycles:u,instructions:u,cache-misses:u")
    parser.add_argument("--large-functions", type=int, default=0)
    parser.add_argument("--large-body-instructions", type=int, default=4096)
    parser.add_argument("--min-speedup-pct", type=float)
    parser.add_argument("--max-regression-pct", type=float)
    parser.add_argument("--max-noise-pct", type=float)
    parser.add_argument("--max-code-growth-pct", type=float)
    parser.add_argument("--max-rewrite-seconds", type=float)
    parser.add_argument("--max-rss-mib", type=float)
    args = parser.parse_args()
    host = os.uname()
    if host.sysname != "Linux" or host.machine not in ("aarch64", "arm64"):
        parser.error("requires native AArch64 Linux")
    if args.max_noise_pct is not None and args.samples < 3:
        parser.error("noise acceptance requires at least three samples")
    if args.per_pc and args.large_functions:
        parser.error("per-PC scanning and generated scale checks are separate runs")
    if (not 0 <= args.latency < 2**64 or not 0 <= args.iteration_cycles < 2**64
            or bool(args.latency) != bool(args.iteration_cycles)):
        parser.error("model comparison requires positive latency and iteration cycles")
    prediction = None
    if args.latency:
        prediction = (args.latency + args.iteration_cycles - 1) // args.iteration_cycles
        if prediction > 1048576:
            parser.error("predicted distance exceeds the supported range")
        args.distances = list(dict.fromkeys([*args.distances, prediction]))
    modes = args.modes.split(",")
    if (
        not 1 <= args.mib <= 4096
        or args.iterations < 1
        or args.samples < 1
        or not args.threads
        or any(not 1 <= t <= 64 for t in args.threads)
        or not args.distances
        or any(not 1 <= d <= 1048576 for d in args.distances)
        or not args.lines
        or not set(modes) <= {"sequential", "strided", "records", "streams2", "streams4"}
        or not 0 <= args.large_functions <= 4096
        or not 1 <= args.large_body_instructions <= 65536
    ):
        parser.error("invalid workload, distance, line size, or scale")
    if any(args.lines):
        parser.error("cache-line throttling was removed; --lines must be 0")
    available = os.sched_getaffinity(0)
    cpus = args.cpus if args.cpus is not None else sorted(available)[:4]
    if not cpus or not set(cpus) <= available:
        parser.error("CPUs must be a nonempty subset of the process affinity")
    for limit in (
        args.min_speedup_pct, args.max_regression_pct, args.max_noise_pct,
        args.max_code_growth_pct, args.max_rewrite_seconds, args.max_rss_mib,
    ):
        if limit is not None and (limit < 0 or not math.isfinite(limit)):
            parser.error("acceptance limits must be finite and nonnegative")
    for tool in (args.bolt, args.clang):
        if not tool.is_file():
            parser.error(f"tool not found: {tool}")
    root = args.work_dir or Path(tempfile.mkdtemp(prefix="bolt-prefetch-"))
    root.mkdir(parents=True, exist_ok=True)
    report = {
        "configuration": {
            k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()
        },
        "cpus": cpus, "host": list(host), "commands": [],
        "tools": {}, "binaries": {}, "runs": [], "summary": [], "failures": [],
        "scope": "controlled manual-distance experiments; no SPE or production acceptance",
        "completed": False,
        "predicted_distance": prediction,
        "candidates": [], "candidate_choices": [],
    }

    def run(command, name):
        record = measured(command, root / (name + ".log"))
        report["commands"].append(record)
        if record["exit_code"]:
            raise RuntimeError(
                f"command failed ({record['exit_code']}): {record['log']}"
            )
        return record

    def identify(path):
        return {"path": str(path), "sha256": digest(path), "code_bytes": code_bytes(path)}

    variants = [("base", [])]
    variant_info = {"base": {"function": None}}
    for distance in args.distances:
        for line in args.lines:
            variants.append((f"d{distance}-l{line}", [
                "--enable-loop-data-prefetch", f"--loop-prefetch-distance={distance}",
                f"--loop-prefetch-cache-line-size={line}",
                "--loop-prefetch-max-per-loop=4", "--loop-prefetch-max-instructions=12",
                "--loop-prefetch-diagnostics",
            ]))
            variant_info[f"d{distance}-l{line}"] = {
                "function": None, "distance": distance, "line": line,
            }
    try:
        for name, tool in (("bolt", args.bolt), ("clang", args.clang)):
            report["tools"][name] = {
                "path": str(tool.resolve()), "sha256": digest(tool),
                "version": subprocess.check_output([str(tool), "--version"], text=True),
            }
        for name in ("/proc/cpuinfo", "/proc/sys/kernel/perf_event_paranoid",
                     f"/sys/devices/system/cpu/cpu{cpus[0]}/cache/index0/coherency_line_size"):
            if Path(name).exists():
                report.setdefault("machine", {})[name] = Path(name).read_text()
        for cpu in cpus:
            for item in ("cpufreq/scaling_governor", "cpufreq/scaling_min_freq",
                         "cpufreq/scaling_max_freq", "topology/thread_siblings_list",
                         "topology/physical_package_id"):
                path = Path(f"/sys/devices/system/cpu/cpu{cpu}") / item
                if path.exists():
                    report.setdefault("machine", {})[str(path)] = path.read_text()
        source = root / "benchmark.c"
        for original, saved in ((Path(__file__), root / "benchmark.py"),
                                (Path(__file__).with_suffix(".c"), source)):
            if original.resolve() != saved.resolve():
                shutil.copyfile(original, saved)
        report["source_sha256"] = digest(source)
        report["runner_sha256"] = digest(root / "benchmark.py")
        run([args.clang, source, "-O2", "-fno-vectorize", "-fno-slp-vectorize",
             "-fno-unroll-loops", "-fno-omit-frame-pointer", "-fuse-ld=lld", "-pthread",
             "-Wl,--emit-relocs,--build-id", "-o", root / "input"], "compile")
        if args.per_pc:
            run([args.bolt.parent / "llvm-readelf", "--notes", root / "input"],
                "build-id")
            build_id = re.search(
                r"Build ID: ([0-9a-fA-F]+)", (root / "build-id.log").read_text()
            )
            if not build_id:
                raise RuntimeError("input has no build-id for per-PC hints")
            build_id = build_id.group(1)
            run([args.bolt, root / "input", "-o", root / "probe", "--verify-cfg",
                 "--enable-loop-data-prefetch", "--loop-prefetch-distance=1",
                 "--loop-prefetch-max-per-loop=4", "--loop-prefetch-max-instructions=12",
                 "--loop-prefetch-diagnostics"], "discover")
            for function, pc, opcode in re.findall(
                r"^BOLT-PREFETCH: (\S+) pc=(0x[0-9a-f]+).*? inserted .*?opcode=(\w+)",
                (root / "discover.log").read_text(), re.MULTILINE,
            ):
                if function in modes:
                    report["candidates"].append(dict(function=function, pc=pc, opcode=opcode))
            if set(modes) != {c["function"] for c in report["candidates"]}:
                raise RuntimeError("no legal candidate discovered for a requested mode")
            variants = [("base", [])]
            for candidate in report["candidates"]:
                for distance in args.distances:
                    for line in args.lines:
                        key = f"pc{candidate['pc'][2:]}-d{distance}-l{line}"
                        hints = root / (key + ".hints")
                        hints.write_text(
                            f"bolt-prefetch-hints 1 {build_id}\n"
                            f"pc {candidate['pc']} {candidate['opcode']} "
                            f"{distance} pldl1keep 12\n"
                        )
                        variants.append((key, [
                            "--enable-loop-data-prefetch", "--loop-prefetch-distance=0",
                            f"--loop-prefetch-hints={hints}",
                            f"--loop-prefetch-cache-line-size={line}",
                            "--loop-prefetch-diagnostics",
                        ]))
                        variant_info[key] = dict(
                            **candidate, distance=distance, line=line, hints=str(hints)
                        )
        report["variants"] = {key: variant_info[key] for key, _ in variants}
        inputs = [("", root / "input")]
        if args.large_functions:
            make_large(root / "large.s", args.large_functions, args.large_body_instructions)
            run([args.bolt.parent / "llvm-mc", "-triple=aarch64", "-filetype=obj",
                 root / "large.s", "-o", root / "large.o"], "assemble-large")
            run([args.clang.parent / "ld.lld", "--emit-relocs", "--build-id", "-e", "_start",
                 root / "large.o", "-o", root / "large.input"], "link-large")
            inputs.append(("large-", root / "large.input"))
        for prefix, binary in inputs:
            report["binaries"][prefix + "input"] = identify(binary)
            if prefix:
                run([binary], "check-large-input")
            for variant, options in variants:
                key = prefix + variant
                output = root / key
                rewrite = run([args.bolt, binary, "-o", output, "--verify-cfg",
                               "--thread-count=4", *options], "rewrite-" + key)
                if args.per_pc and variant != "base":
                    info = variant_info[variant]
                    info["inserted"] = bool(re.search(
                        rf"pc={re.escape(info['pc'])} .*? inserted ",
                        Path(rewrite["log"]).read_text(),
                    ))
                report["binaries"][key] = identify(output)
                if prefix:
                    run([output], "check-" + key)
                base_size = report["binaries"][prefix + "base"]["code_bytes"]
                growth = 100 * (code_bytes(output) / base_size - 1)
                report["binaries"][key]["growth_pct"] = growth
                if (
                    args.max_code_growth_pct is not None
                    and growth > args.max_code_growth_pct
                ):
                    report["failures"].append(f"{key}: code growth {growth:.2f}%")
                if (
                    args.max_rss_mib is not None
                    and rewrite["max_rss_kib"] > args.max_rss_mib * 1024
                ):
                    rss = rewrite["max_rss_kib"] / 1024
                    report["failures"].append(f"{key}: peak RSS {rss:.1f} MiB")
                if (
                    args.max_rewrite_seconds is not None
                    and rewrite["seconds"] > args.max_rewrite_seconds
                ):
                    report["failures"].append(
                        f"{key}: rewrite {rewrite['seconds']:.3f}s"
                    )
        jobs = [
            (mode, threads, variant, sample)
            for mode in modes for threads in args.threads
            for variant, _ in variants for sample in range(args.samples)
            if not args.per_pc or variant == "base"
            or variant_info[variant]["function"] == mode
        ]
        random.Random(args.seed).shuffle(jobs)
        checksums = {}
        for index, (mode, threads, variant, sample) in enumerate(jobs):
            key = f"{mode}-t{threads}-{variant}-s{sample}"
            command = ["taskset", "-c", ",".join(map(str, cpus)), root / variant,
                       mode, str(args.mib), str(args.iterations), str(threads)]
            stat = root / (key + ".stat")
            if args.perf_stat:
                command = [
                    "perf", "stat", "-x,", "-e", args.events, "-o", stat, "--",
                    *command,
                ]
            run(command, key)
            fields = next(
                line.split()
                for line in (root / (key + ".log")).read_text().splitlines()
                if line.startswith(mode + " ")
            )
            seconds, checksum = float(fields[3]), fields[4]
            if seconds <= 0:
                raise RuntimeError(f"measurement too short: {key}")
            case = (mode, threads)
            if checksums.setdefault(case, checksum) != checksum:
                raise RuntimeError(f"checksum mismatch: {key}")
            report["runs"].append({"mode": mode, "threads": threads, "variant": variant,
                                   "sample": sample, "seconds": seconds, "checksum": checksum,
                                   "stat": str(stat) if args.perf_stat else None})
            if (index + 1) % 20 == 0:
                print(f"completed {index + 1}/{len(jobs)} runs", flush=True)
        for mode in modes:
            for threads in args.threads:
                medians = {}
                for variant, _ in variants:
                    if (args.per_pc and variant != "base"
                            and variant_info[variant]["function"] != mode):
                        continue
                    values = [
                        r["seconds"] for r in report["runs"]
                        if (r["mode"], r["threads"], r["variant"])
                        == (mode, threads, variant)
                    ]
                    median = medians[variant] = statistics.median(values)
                    regression = 100 * (median / medians["base"] - 1)
                    speedup = 100 * (medians["base"] / median - 1)
                    noise = (
                        100 * statistics.median(abs(v - median) for v in values)
                        / median
                    )
                    report["summary"].append(dict(
                        mode=mode, threads=threads, variant=variant, median=median,
                        minimum=min(values), maximum=max(values),
                        regression_pct=regression, speedup_pct=speedup,
                        median_absolute_deviation_pct=noise,
                    ))
                    case = f"{mode}/t{threads}/{variant}"
                    print(
                        f"{mode:10} t={threads} {variant:12} "
                        f"{median:.6f}s {regression:+.2f}%"
                    )
                    if (
                        not args.per_pc and variant != "base" and args.min_speedup_pct is not None
                        and speedup < args.min_speedup_pct
                    ):
                        report["failures"].append(f"{case}: speedup {speedup:.2f}%")
                    if (not args.per_pc and args.max_noise_pct is not None
                            and noise > args.max_noise_pct):
                        report["failures"].append(f"{case}: noise {noise:.2f}%")
                    if (
                        not args.per_pc and args.max_regression_pct is not None
                        and regression > args.max_regression_pct
                    ):
                        report["failures"].append(
                            f"{case}: regression {regression:.2f}%"
                        )
        report["completed"] = True
        if args.per_pc:
            report["candidate_choices"] = select_candidate_choices(report)
    except (OSError, RuntimeError, subprocess.CalledProcessError, StopIteration) as error:
        report["failures"].append(str(error))
    finally:
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"artifacts: {root}")
    for failure in report["failures"]:
        print(failure, file=sys.stderr)
    return bool(report["failures"])


if __name__ == "__main__":
    sys.exit(main())
