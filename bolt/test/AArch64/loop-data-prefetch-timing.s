# REQUIRES: system-linux
## Static timing must depend on CFG paths, dependencies and inner trip counts.
## This test rewrites the ELF without executing its synthetic pointer chains.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: llvm-nm --print-size %t.exe > %t/symbols
# RUN: %python %t/check.py %llvm_bolt %t.exe %t/symbols %t

#--- check.py
from pathlib import Path
import re
import subprocess
import sys

bolt, binary, symbols, output = sys.argv[1:]
root = Path(output)
sizes = {
    p[3]: int(p[1], 16)
    for line in Path(symbols).read_text().splitlines()
    if len(p := line.split()) == 4
}
names = [
    "tiny",
    "wide",
    "dependent",
    "independent",
    "diamond",
    "called",
    "batch4",
    "batch8",
]
results = {}
for mode in ["fast", "slow", "unknown", "measured"]:
    rows = ["bolt-prefetch llc_miss 11223344 1024 200000001 0 0"]
    for name in names:
        batch = name.startswith("batch")
        header, load = (4, 8) if batch else (0, 0)
        total, samples = (1230, 10) if mode == "measured" else (0, 0)
        rows += [
            f"0 [unknown] 0 1 {name} 0 0 1 0 0",
            f"1 {name} {sizes[name] - 8:x} 1 {name} {header:x} "
            f"0 100 {total} {samples}",
            f"7 {name} {load:x} 8 10",
        ]
        if batch:
            rows.append(f"1 {name} 10 1 {name} 8 0 500 0 0")
        if name == "diamond" and mode != "unknown":
            fast, slow = (10, 90) if mode == "slow" else (90, 10)
            rows += [
                f"1 diamond 4 1 diamond 8 0 {fast} 0 0",
                f"1 diamond 4 1 diamond 10 0 {slow} 0 0",
                f"1 diamond c 1 diamond 70 0 {fast} 0 0",
            ]
    profile = root / (mode + ".fdata")
    profile.write_text("\n".join(rows) + "\n")
    command = [
        bolt,
        binary,
        "-o",
        str(root / (mode + ".bolt")),
        "--data=" + str(profile),
        "--enable-loop-data-prefetch",
        "--loop-prefetch-batch",
        "--loop-prefetch-cache-line-size=64",
        "--loop-prefetch-diagnostics",
        "--lite=0",
        "--verify-cfg",
    ]
    proc = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    (root / (mode + ".log")).write_text(proc.stdout)
    assert proc.returncode == 0, proc.stdout
    costs = {}
    for line in proc.stdout.splitlines():
        if not line.startswith("BOLT-PREFETCH:") or " inserted " not in line:
            continue
        fields = dict(part.split("=", 1) for part in line.split() if "=" in part)
        name = line.split()[1]
        total = int(fields["iteration-cycle-total"])
        samples = int(fields["iteration-cycle-samples"])
        distance = int(fields["distance"])
        assert distance == (200 * samples + total - 1) // total
        if mode == "measured":
            assert (total, samples, fields["cycles-source"]) == (1230, 10, "profile")
        else:
            assert total > 0 and samples == 1 and fields["cycles-source"] == "static"
            assert fields["timing-cpu"] == "generic"
        assert name not in costs, line
        costs[name] = total
    assert set(costs) == set(names) - {"called"}, proc.stdout
    assert re.search(r"BOLT-PREFETCH: called .*unsupported loop context", proc.stdout)
    results[mode] = costs

costs = results["fast"]
assert costs["wide"] > costs["tiny"], costs
assert costs["dependent"] > costs["independent"], costs
assert costs["batch8"] > costs["batch4"], costs
assert results["fast"]["diamond"] < results["slow"]["diamond"], results
assert results["unknown"]["diamond"] >= results["slow"]["diamond"], results

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  bl tiny
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

begin tiny
  ldr x3, [x0], #8
end tiny

begin wide
  ldr x3, [x0], #8
  .rept 32
  add x6, x6, #1
  .endr
end wide

begin dependent
  ldr x3, [x0], #8
  ldr x4, [x1]
  ldr x5, [x4]
  ldr x6, [x5]
end dependent

begin independent
  ldr x3, [x0], #8
  ldr x4, [x1]
  ldr x5, [x1, #8]
  ldr x6, [x1, #16]
end independent

begin diamond
  ldr x3, [x0], #8
  cbz x4, .Lslow
  add x6, x6, #1
  b .Ljoin
.Lslow:
  .rept 24
  add x6, x6, #1
  .endr
.Ljoin:
end diamond

begin called
  ldr x3, [x0], #8
  bl callee
end called

begin callee
  ret
.size callee, .-callee

.macro batch name, count
begin \name
  cbz x2, 2f
1:
  mov w3, #\count
3:
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne 3b
  subs x2, x2, #1
  b.ne 1b
2:
  ret
.size \name, .-\name
.endm
batch batch4, 4
batch batch8, 8
