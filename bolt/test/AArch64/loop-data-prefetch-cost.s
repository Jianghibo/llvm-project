# REQUIRES: system-linux
## Cost decisions use the generated sequence, including dependent address work
## and speculative index loads. Free registers have no separate penalty.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: llvm-nm --print-size %t.exe > %t/symbols
# RUN: %python %t/check.py %llvm_bolt %t.exe %t/symbols %t

#--- check.py
from pathlib import Path
import subprocess
import sys

bolt, binary, symbols, output = sys.argv[1:]
sizes = {
    p[3]: int(p[1], 16)
    for line in Path(symbols).read_text().splitlines()
    if len(p := line.split()) == 4
}
for mode in ["low", "high", "hot-index", "unknown-index"]:
    rows = ["bolt-prefetch llc_miss 11223344 1024 200000001 0 0"]
    for name in ["cheap", "address", "indirect", "compete"]:
        header = 4 if name == "indirect" else 0
        rows += [
            f"0 [unknown] 0 1 {name} 0 0 100 0 0",
            f"1 {name} {sizes[name] - 8:x} 1 {name} {header:x} 0 100 200 1",
        ]
    misses, accesses = (8, 10) if mode == "high" else (2, 100)
    rows += [
        f"7 cheap 0 {misses} {accesses}",
        f"7 address 0 {misses} {accesses}",
        "7 indirect 8 8 10",
        "7 compete 0 30 100",
        "7 compete 4 31 1000",
    ]
    if mode != "unknown-index":
        rows.append(f"7 indirect 4 {10 if mode == 'hot-index' else 0} 10")
    profile = Path(output) / (mode + ".fdata")
    profile.write_text("\n".join(rows) + "\n")
    proc = subprocess.run(
        [
            bolt,
            binary,
            "-o",
            str(Path(output) / (mode + ".bolt")),
            "--data=" + str(profile),
            "--enable-loop-data-prefetch",
            "--loop-prefetch-indirect",
            "--cache-miss-cutoff=0",
            "--loop-prefetch-max-instructions=16",
            "--loop-prefetch-max-per-loop=1",
            "--loop-prefetch-diagnostics",
            "--lite=0",
            "--verify-cfg",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    (Path(output) / (mode + ".log")).write_text(proc.stdout)
    assert proc.returncode == 0, proc.stdout
    decisions = {}
    for line in proc.stdout.splitlines():
        if not line.startswith("BOLT-PREFETCH:"):
            continue
        fields = dict(
            part.split("=", 1) for part in line.split() if "=" in part
        )
        name = line.split()[1]
        if " inserted " in line:
            assert float(fields["estimated-benefit-cycles"]) > float(
                fields["estimated-cost-cycles"]
            ), line
            decisions[name] = fields
    assert "cheap" in decisions, proc.stdout
    assert ("address" in decisions) == (mode == "high"), proc.stdout
    if mode == "high":
        assert decisions["address"]["scratch-registers"] == "2"
    else:
        assert "prefetch cost exceeds estimated benefit" in proc.stdout
    assert (decisions.get("indirect", {}).get("kind") == "indirect") == (
        mode in ["low", "high"]
    ), proc.stdout
    target = next(
        line
        for line in proc.stdout.splitlines()
        if line.startswith("BOLT-PREFETCH: indirect ")
        and "opcode=LDRXroW" in line
    )
    if mode == "unknown-index":
        assert "unavailable index-load miss cost" in target
    if mode == "hot-index":
        assert "prefetch cost exceeds estimated benefit" in target
    # A higher miss count does not outweigh its larger dynamic execution cost.
    assert decisions["compete"]["opcode"] == "LDRXpost", proc.stdout

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  bl cheap
  ret
.size _start, .-_start

.macro begin name
.globl \name
.type \name, %function
\name:
.endm
.macro end name
  subs x2, x2, #1
  b.ne \name
  ret
.size \name, .-\name
.endm

begin cheap
  ldr x3, [x0], #8
end cheap

begin address
  ldr x3, [x0, w1, uxtw #3]
  add w1, w1, #1
end address

begin indirect
  cbz x2, 2f
1:
  ldr w3, [x0], #4
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne 1b
2:
  ret
.size indirect, .-indirect

begin compete
  ldr x3, [x0], #8
  ldr x4, [x1, w5, uxtw #3]
  add w5, w5, #1
end compete
