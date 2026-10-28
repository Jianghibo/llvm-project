#!/usr/bin/env python3
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check synthetic fdata -> BOLT -> emitted prefetches without target execution."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(command, log, success=True):
    result = subprocess.run([str(arg) for arg in command], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_text(result.stdout)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"unexpected exit code {result.returncode}; see {log}")
    return result.stdout


def instructions(text):
    functions = {}
    current = None
    for line in text.splitlines():
        header = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if header:
            current = functions.setdefault(header[2], [])
        item = re.match(r"\s*([0-9a-f]+):\s+(.+)", line)
        if item and current is not None:
            asm = re.sub(r"\s+", " ", item[2].split("//")[0]).strip()
            current.append((int(item[1], 16), asm))
    return functions


def target(branch):
    return int(re.search(r"\b0x([0-9a-f]+)", branch)[1], 16)


def prefetches(code):
    return [(pc, asm) for pc, asm in code
            if asm.startswith(("prfm ", "prfum "))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", required=True, type=Path)
    parser.add_argument("--bolt", type=Path,
                        help="BOLT executable; defaults to --tools/llvm-bolt")
    parser.add_argument("--output-dir", type=Path,
                        help="new artifact directory; defaults to a temporary directory")
    args = parser.parse_args()
    tools = args.tools.resolve()
    bolt = args.bolt.resolve() if args.bolt else tools / "llvm-bolt"
    root = (args.output_dir.resolve() if args.output_dir else
            Path(tempfile.mkdtemp(prefix="bolt-prefetch-profile-")))
    if args.output_dir:
        root.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(Path(__file__).with_name("profile-input.s"), root / "input.s")
    binary = root / "input"
    run([tools / "llvm-mc", "-triple=aarch64", "-filetype=obj", root / "input.s",
         "-o", root / "input.o"], root / "assemble.log")
    run([tools / "ld.lld", "--emit-relocs", "--build-id=sha1", "-e", "_start",
         root / "input.o", "-o", binary], root / "link.log")
    notes = run([tools / "llvm-readelf", "-n", binary], root / "notes.txt")
    build_id = re.search(r"Build ID: ([0-9a-f]+)", notes)[1]

    def disassemble(path, log):
        text = run([tools / "llvm-objdump", "-d", "--no-show-raw-insn",
                    "--no-print-imm-hex", path], log)
        return instructions(text)

    original = disassemble(binary, root / "input.asm")
    assert not any(prefetches(code) for code in original.values())
    header = f"bolt-prefetch llc_miss {build_id} 1024 200000001 0 0\n"
    cases = [
        ("direct", r"ldr x3, \[x0\], #8", 500, 4, "affine", []),
        (
            "gather",
            r"ldr x4, \[x1, w3, uxtw #3\]",
            500,
            4,
            "indirect",
            ["--loop-prefetch-indirect"],
        ),
        (
            "batch",
            r"ldr x4, \[x0\], #8",
            1000,
            2,
            "batch",
            ["--loop-prefetch-batch", "--loop-prefetch-cache-line-size=64"],
        ),
    ]
    report = dict(synthetic_profile=True, target_executed=False,
                  performance_validated=False, build_id=build_id,
                  bolt_sha256=hashlib.sha256(bolt.read_bytes()).hexdigest(),
                  cases=[], negative_checks=[])
    for name, pattern, cycle_total, distance, kind, options in cases:
        folder = root / name
        folder.mkdir()
        code = original[name]
        start = code[0][0]
        load_pc = next(pc for pc, asm in code if re.fullmatch(pattern, asm))
        branches = [(pc, target(asm)) for pc, asm in code if asm.startswith("b.ne ")]
        # Counts/cycles are synthetic. Batch uses parent mean 100 rather
        # than inner mean 2; the other cases use mean 50 cycles.
        rows = [f"0 [unknown] 0 1 {name} 0 0 1 0 0"]
        for index, (pc, destination) in enumerate(branches):
            count, total = (
                (48, 20) if name == "batch" and index == 0 else (15, cycle_total)
            )
            rows.append(
                f"1 {name} {pc-start:x} 1 {name} {destination-start:x} "
                f"0 {count} {total} 10"
            )
        rows.append(f"7 {name} {load_pc-start:x} 8 10")
        if name == "gather":
            index_pc = next(pc for pc, asm in code if asm.startswith("ldr w3,"))
            rows.append(f"7 {name} {index_pc-start:x} 0 10")
        profile = folder / "input.fdata"
        profile.write_text(header + "\n".join(rows) + "\n")
        common = [
            bolt,
            binary,
            f"--data={profile}",
            "--lite=0",
            "--verify-cfg",
            "--loop-prefetch-diagnostics",
            "-v=1",
            *options,
        ]
        baseline = folder / "baseline"
        optimized = folder / "prefetched"
        run([*common, "--enable-loop-data-prefetch=false", "-o", baseline],
            folder / "baseline.log")
        command = [*common, "--enable-loop-data-prefetch", "-o", optimized]
        log = run(command, folder / "prefetch.log")
        before = disassemble(baseline, folder / "baseline.asm")[name]
        after = disassemble(optimized, folder / "prefetched.asm")[name]
        assert not prefetches(before)
        bindings = 2 if name == "gather" else 1
        assert (
            f"cache-miss bindings: {bindings} loads, 0 invalid PCs, 0 non-loads, 0 unmatched"
            in log
        )
        decision = next(
            line
            for line in log.splitlines()
            if line.startswith(f"BOLT-PREFETCH: {name} pc={load_pc:#x} ")
            and " inserted " in line
        )
        assert f"source=profile distance={distance} " in decision
        assert f"kind={kind}" in decision
        requests = prefetches(after)
        assert len(requests) == (2 if name == "batch" else 1)
        loads_before = sum(asm.startswith(("ldr ", "ldrsw ")) for _, asm in before)
        loads_after = sum(asm.startswith(("ldr ", "ldrsw ")) for _, asm in after)
        assert loads_after == loads_before + (name == "gather")
        asm = [item for _, item in after]
        if name == "direct":
            assert requests[0][1] == "prfm pldl1keep, [x0, #32]"
            at = asm.index(requests[0][1])
            assert asm[at + 1] == "ldr x3, [x0], #8"
        elif name == "gather":
            at = next(i for i, item in enumerate(asm)
                      if re.fullmatch(r"ldr w\d+, \[x0, #12\]", item))
            assert asm[at - 1].startswith("tbnz ")
            join = target(asm[at - 1])
            current = next(pc for pc, item in after
                           if item == "ldr x4, [x1, w3, uxtw #3]")
            assert join == current
            assert requests[0] == after[at + 1]
            assert re.fullmatch(r"prfm pldl1keep, \[x1, w\d+, uxtw #3\]", requests[0][1])
        else:
            assert [item for _, item in requests] == [
                "prfm pldl1keep, [x0, #64]", "prfum pldl1keep, [x0, #95]"]
            setup = next(pc for pc, item in after if item == "mov w3, #4")
            inner = next(pc for pc, item in after if item == "ldr x4, [x0], #8")
            assert setup < requests[0][0] < requests[-1][0] < inner
            backedges = [target(item) for _, item in after if item.startswith("b.ne ")]
            assert backedges == [inner, setup]
        report["cases"].append(
            dict(
                function=name,
                load_pc=hex(load_pc),
                load_offset=hex(load_pc - start),
                default_latency_cycles=200,
                distance=distance,
                kind=kind,
                status="passed",
                prefetches=[item for _, item in requests],
                command=[str(arg) for arg in command],
            )
        )
        print(
            f"PASS {name}: profile bound to {load_pc:#x}, D={distance}, "
            f"{len(requests)} prefetch instruction(s)"
        )

    direct = root / "direct" / "input.fdata"
    bad_id = ("0" if build_id[0] != "0" else "1") + build_id[1:]
    wrong = root / "wrong-id.fdata"
    wrong.write_text(direct.read_text().replace(build_id, bad_id, 1))
    log = run(
        [
            bolt,
            binary,
            f"--data={wrong}",
            "-o",
            root / "bad",
            "--enable-loop-data-prefetch",
        ],
        root / "wrong-id.log",
        success=False,
    )
    assert "prefetch profile build-id does not match input" in log
    report["negative_checks"].append("wrong build-id rejected")
    no_cycles = root / "no-cycles.fdata"
    no_cycles.write_text(direct.read_text().replace(" 500 10\n", " 0 0\n"))
    output = root / "no-cycles"
    log = run(
        [
            bolt,
            binary,
            f"--data={no_cycles}",
            "-o",
            output,
            "--enable-loop-data-prefetch",
            "--loop-prefetch-diagnostics",
            "--verify-cfg",
        ],
        root / "no-cycles.log",
    )
    timing = re.search(
        r"distance=(\d+).*iteration-cycle-total=(\d+).*" r"cycles-source=static", log
    )
    assert timing
    distance, cycles = map(int, timing.groups())
    assert distance == (200 + cycles - 1) // cycles
    requests = prefetches(disassemble(output, root / "no-cycles.asm")["direct"])
    assert [item for _, item in requests] == [f"prfm pldl1keep, [x0, #{distance * 8}]"]
    report["fallback_checks"] = ["missing cycles use a static loop estimate"]
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS identity and fallback checks; artifacts: {root}")


if __name__ == "__main__":
    main()
