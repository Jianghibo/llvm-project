#!/usr/bin/env python3
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Run profile-guided BOLT experiments on a prebuilt benchmark."""

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", required=True, type=Path)
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        help="binary built from loop-data-prefetch-bench.c",
    )
    parser.add_argument(
        "--profile",
        required=True,
        type=Path,
        help="extended fdata collected for --input",
    )
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--cpus", type=integers)
    parser.add_argument("--threads", type=integers, default=[1, 4])
    parser.add_argument("--lines", type=integers, default=[0, 64])
    parser.add_argument("--modes", default="sequential,strided,records,streams2,streams4")
    parser.add_argument("--mib", type=int, default=256)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--perf-stat", action="store_true")
    parser.add_argument("--events", default="cycles:u,instructions:u,cache-misses:u")
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
    modes = args.modes.split(",")
    if (
        not 1 <= args.mib <= 4096
        or args.iterations < 1
        or args.samples < 1
        or not args.threads
        or any(not 1 <= t <= 64 for t in args.threads)
        or not args.lines
        or any(n and (n < 16 or n > 4096 or n & (n - 1)) for n in args.lines)
        or not set(modes) <= {"sequential", "strided", "records", "streams2", "streams4"}
    ):
        parser.error("invalid workload or line size")
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
    for tool in (args.bolt, args.input, args.profile):
        if not tool.is_file():
            parser.error(f"input or tool not found: {tool}")
    root = args.work_dir or Path(tempfile.mkdtemp(prefix="bolt-prefetch-"))
    root.mkdir(parents=True, exist_ok=True)
    report = {
        "configuration": {
            k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()
        },
        "cpus": cpus, "host": list(host), "commands": [],
        "tools": {}, "binaries": {}, "runs": [], "summary": [], "failures": [],
        "scope": "profile-guided controlled experiments; no SPE collection or production acceptance",
        "completed": False,
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

    variants = [("base", [f"--data={args.profile.resolve()}"])]
    variant_info = {"base": {}}
    for line in args.lines:
        name = f"auto-l{line}"
        variants.append(
            (
                name,
                [
                    "--enable-loop-data-prefetch",
                    f"--data={args.profile.resolve()}",
                    f"--loop-prefetch-cache-line-size={line}",
                    "--loop-prefetch-max-per-loop=4",
                    "--loop-prefetch-max-instructions=12",
                    "--loop-prefetch-diagnostics",
                ],
            )
        )
        variant_info[name] = {"line": line}
    try:
        report["tools"]["bolt"] = {
            "path": str(args.bolt.resolve()),
            "sha256": digest(args.bolt),
            "version": subprocess.check_output(
                [str(args.bolt), "--version"], text=True
            ),
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
        if args.profile.resolve() != (root / "input.fdata").resolve():
            shutil.copyfile(args.profile, root / "input.fdata")
        report["profile"] = {
            "path": str(args.profile.resolve()),
            "sha256": digest(args.profile),
        }
        report["variants"] = {key: variant_info[key] for key, _ in variants}
        binary = root / "input"
        if args.input.resolve() != binary.resolve():
            shutil.copy2(args.input, binary)
        report["binaries"]["input"] = identify(binary)
        for variant, options in variants:
            output = root / variant
            rewrite = run(
                [
                    args.bolt,
                    binary,
                    "-o",
                    output,
                    "--verify-cfg",
                    "--thread-count=4",
                    *options,
                ],
                "rewrite-" + variant,
            )
            if variant != "base":
                log = (root / ("rewrite-" + variant + ".log")).read_text()
                if not re.search(
                    r"number of prefetch instructions inserted: [1-9]", log
                ):
                    raise RuntimeError(f"no prefetches inserted for {variant}")
            report["binaries"][variant] = identify(output)
            base_size = report["binaries"]["base"]["code_bytes"]
            growth = 100 * (code_bytes(output) / base_size - 1)
            report["binaries"][variant]["growth_pct"] = growth
            if (
                args.max_code_growth_pct is not None
                and growth > args.max_code_growth_pct
            ):
                report["failures"].append(f"{variant}: code growth {growth:.2f}%")
            if (
                args.max_rss_mib is not None
                and rewrite["max_rss_kib"] > args.max_rss_mib * 1024
            ):
                rss = rewrite["max_rss_kib"] / 1024
                report["failures"].append(f"{variant}: peak RSS {rss:.1f} MiB")
            if (
                args.max_rewrite_seconds is not None
                and rewrite["seconds"] > args.max_rewrite_seconds
            ):
                report["failures"].append(
                    f"{variant}: rewrite {rewrite['seconds']:.3f}s"
                )
        jobs = [
            (mode, threads, variant, sample)
            for mode in modes for threads in args.threads
            for variant, _ in variants for sample in range(args.samples)
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
                        variant != "base"
                        and args.min_speedup_pct is not None
                        and speedup < args.min_speedup_pct
                    ):
                        report["failures"].append(f"{case}: speedup {speedup:.2f}%")
                    if args.max_noise_pct is not None and noise > args.max_noise_pct:
                        report["failures"].append(f"{case}: noise {noise:.2f}%")
                    if (
                        args.max_regression_pct is not None
                        and regression > args.max_regression_pct
                    ):
                        report["failures"].append(
                            f"{case}: regression {regression:.2f}%"
                        )
        report["completed"] = True
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
