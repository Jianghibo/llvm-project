#!/usr/bin/env python3
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Combine per-PC hints and optionally check the plan with BOLT, without execution."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def unsigned(value):
    number = int(value)
    if not 0 <= number < 2**32:
        raise ValueError("expected an unsigned 32-bit integer")
    return number


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def combine(paths):
    build_id = None
    records = {}
    for path in paths:
        lines = [line.split() for line in path.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        if (not lines or len(lines[0]) != 3
                or lines[0][:2] != ["bolt-prefetch-hints", "1"]
                or not re.fullmatch(r"(?:[0-9a-fA-F]{2})+", lines[0][2])):
            raise ValueError(f"{path}: invalid hints header")
        identity = lines[0][2].lower()
        if build_id is not None and identity != build_id:
            raise ValueError(f"{path}: incompatible build-id")
        build_id = identity
        for fields in lines[1:]:
            if len(fields) != 6 or fields[0] != "pc":
                raise ValueError(f"{path}: expected a per-PC hints record")
            pc = int(fields[1], 16)
            distance, budget = unsigned(fields[3]), unsigned(fields[5])
            if not 0 <= pc < 2**64 or distance > 1048576:
                raise ValueError(f"{path}: PC or distance out of range")
            record = dict(pc=f"0x{pc:x}", opcode=fields[2], distance=distance,
                          hint=fields[4], budget=budget)
            if pc in records and records[pc] != record:
                raise ValueError(f"{path}: conflicting hints for PC {pc:#x}")
            records[pc] = record
    return build_id, [records[pc] for pc in sorted(records)]


def write_hints(path, build_id, records):
    lines = [f"bolt-prefetch-hints 1 {build_id}"]
    lines.extend(f"pc {r['pc']} {r['opcode']} {r['distance']} {r['hint']} {r['budget']}"
                 for r in records)
    temporary = path.with_suffix(".tmp")
    temporary.write_text("\n".join(lines) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hints", type=Path, nargs="+", required=True)
    parser.add_argument("--output-dir", type=Path,
                        help="new artifact directory (must not already exist)")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--bolt", type=Path)
    parser.add_argument("--indirect", action="store_true",
                        help="enable guarded index-array prefetches in BOLT")
    parser.add_argument("--batch", action="store_true",
                        help="enable constant inner-loop batches in BOLT")
    parser.add_argument("--batch-max-bytes", type=unsigned, default=256)
    parser.add_argument("--cache-line-size", type=unsigned, default=0)
    parser.add_argument("--max-per-loop", type=unsigned, default=2)
    parser.add_argument("--max-per-function", type=unsigned, default=32)
    parser.add_argument("--max-loop-instructions", type=unsigned, default=32)
    parser.add_argument("--max-function-instructions", type=unsigned, default=256)
    args = parser.parse_args()
    if bool(args.binary) != bool(args.bolt):
        parser.error("static checking requires both --binary and --bolt")
    line = args.cache_line_size
    if line and (line < 16 or line > 4096 or line & (line - 1)):
        parser.error("cache line size must be 0 or a power of two from 16 to 4096")
    if line and not args.batch:
        parser.error("--cache-line-size requires --batch")
    try:
        root = args.output_dir or Path(tempfile.mkdtemp(prefix="prefetch-plan-"))
        if args.output_dir:
            root.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(str(error))
    report = {
        "status": "unverified", "performance_validated": False,
        "configuration": {k: str(v) if isinstance(v, Path) else v
                          for k, v in vars(args).items() if k != "hints"},
        "inputs": [dict(path=str(p)) for p in args.hints],
        "candidates": [], "error": None,
    }
    try:
        for record, path in zip(report["inputs"], args.hints):
            record["sha256"] = digest(path)
        build_id, records = combine(args.hints)
        report["build_id"] = build_id
        report["requested_loads"] = len(records)
        report["candidates"] = [dict(**r, status="not_checked") for r in records]
        combined = root / "combined.hints"
        write_hints(combined, build_id, records)
        if args.binary:
            output = root / "rewritten"
            command = [str(args.bolt.resolve()), str(args.binary.resolve()),
                       "-o", str(output.resolve()), "--enable-loop-data-prefetch",
                       "--loop-prefetch-distance=0",
                       f"--loop-prefetch-hints={combined.resolve()}",
                       "--loop-prefetch-diagnostics", "--verify-cfg",
                       f"--loop-prefetch-cache-line-size={line}",
                       f"--loop-prefetch-max-per-loop={args.max_per_loop}",
                       f"--loop-prefetch-max-per-function={args.max_per_function}",
                       f"--loop-prefetch-max-loop-instructions={args.max_loop_instructions}",
                       f"--loop-prefetch-max-function-instructions={args.max_function_instructions}"]
            if args.indirect:
                command.append("--loop-prefetch-indirect")
            if args.batch:
                command.extend(["--loop-prefetch-batch",
                                f"--loop-prefetch-batch-max-bytes={args.batch_max_bytes}"])
            report["binary"] = dict(path=str(args.binary), sha256=digest(args.binary))
            report["bolt"] = dict(path=str(args.bolt), sha256=digest(args.bolt))
            report["command"] = command
            log = root / "rewrite.log"
            with log.open("w") as stream:
                result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
            report["exit_code"] = result.returncode
            if result.returncode:
                raise RuntimeError(f"BOLT static check failed; see {log}")
            diagnostics = log.read_text()
            if "BOLT-INFO: number of prefetch instructions inserted:" not in diagnostics:
                raise RuntimeError("BOLT did not run loop data prefetch; "
                                   "require a supported AArch64 rewrite")
            states = {r["pc"]: r for r in report["candidates"]}
            for state in states.values():
                state["status"] = "not_planned"
            for text in diagnostics.splitlines():
                if not text.startswith("BOLT-PREFETCH:"):
                    continue
                match = re.search(r"\bpc=(0x[0-9a-fA-F]+)\b", text)
                if not match or match[1].lower() not in states:
                    continue
                state = states[match[1].lower()]
                state["diagnostic"] = text
                state["status"] = (
                    "planned" if re.search(r"\bbudget=\d+ inserted\b", text) else "skipped"
                )
            selected = [r for r in report["candidates"] if r["status"] == "planned"]
            report["rewritten_sha256"] = digest(output)
            write_hints(root / "planned.hints", build_id, selected)
            report["planned_loads"] = len(selected)
            report["status"] = "static_checked"
    except (OSError, ValueError, RuntimeError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
    (root / "plan.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['status']}: {root}")
    if report["error"]:
        print(report["error"], file=sys.stderr)
    return bool(report["error"])


if __name__ == "__main__":
    sys.exit(main())
