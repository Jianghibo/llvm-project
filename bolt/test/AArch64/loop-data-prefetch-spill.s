# REQUIRES: system-linux
## Prefer free registers. Borrow live GPRs only in profitable, balanced windows
## with a fixed SP-based CFA; restore values and unwind state at every boundary.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 %t/input.s -o %t.o
# RUN: ld.lld -shared -Bsymbolic --emit-relocs --eh-frame-hdr \
# RUN:   --build-id=0x11223344 %t.o -o %t.so
# RUN: %python %t/check.py %llvm_bolt %t.so %t
# RUN: llvm-objdump -d --no-print-imm-hex %t/high.so > %t/high.asm
# RUN: llvm-dwarfdump --eh-frame %t/high.so | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %python %t/run.py %t.so %t/low.so \
# RUN:   %t/medium.so %t/high.so %}
# RUN: %if target=aarch64{{.*}} %{ %clang %S/../Inputs/prefetch-unwind.c \
# RUN:   -O1 -rdynamic \
# RUN:   -fomit-frame-pointer -fasynchronous-unwind-tables -ldl -o %t.unwind %}
# RUN: %if target=aarch64{{.*}} %{ %python %t/check-unwind.py %t.unwind \
# RUN:   %t/high.so %t/high.asm %}

# CHECK: DW_CFA_def_cfa_offset: +16
# CHECK: DW_CFA_offset: W2 -16
# CHECK: DW_CFA_def_cfa_offset: +0
# CHECK: DW_CFA_def_cfa_offset: +16
# CHECK: DW_CFA_offset: W2 -16
# CHECK: DW_CFA_offset: W3 -8
# CHECK: DW_CFA_def_cfa_offset: +0
# CHECK: DW_CFA_def_cfa_offset: +32
# CHECK: DW_CFA_offset: W2 -32
# CHECK: DW_CFA_offset: W3 -24
# CHECK: DW_CFA_def_cfa_offset: +16

#--- check.py
from pathlib import Path
import subprocess
import sys

bolt, binary, output = sys.argv[1:]
root = Path(output)
names = [
    "free",
    "one",
    "two",
    "stack_frame",
    "frame_pointer",
    "sp_address",
    "no_cfi",
    "multi",
]
for mode, misses, accesses in [
    ("low", 4, 100),
    ("medium", 8, 100),
    ("high", 8, 10),
]:
    rows = ["bolt-prefetch llc_miss 11223344 1024 200000001 0 0"]
    for name in names:
        # The loop header follows register initialization and any stack frame.
        header = 16 * 4 + {"stack_frame": 8, "frame_pointer": 8}.get(name, 0)
        # Count the load, pressure uses and induction updates up to the backedge.
        unused = {"free": 2, "one": 1}.get(name, 0)
        extra = 3 if name == "multi" else 0
        backedge = header + (21 - unused + extra) * 4
        rows += [
            f"0 [unknown] 0 1 {name} 0 0 100 0 0",
            f"1 {name} {backedge:x} " f"1 {name} {header:x} 0 9900 200 1",
            f"7 {name} {header:x} {misses} {accesses}",
        ]
        if name == "multi":
            rows.append(f"7 {name} {header + 8:x} {misses} {accesses}")
    profile = root / (mode + ".fdata")
    profile.write_text("\n".join(rows) + "\n")
    proc = subprocess.run(
        [
            bolt,
            binary,
            "-o",
            str(root / (mode + ".so")),
            "--data=" + str(profile),
            "--enable-loop-data-prefetch",
            "--cache-miss-cutoff=0",
            "--loop-prefetch-diagnostics",
            "--lite=0",
            "--verify-cfg",
            "--reorder-blocks=ext-tsp",
            "--split-functions",
            "--split-all-cold",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    (root / (mode + ".log")).write_text(proc.stdout)
    assert proc.returncode == 0, proc.stdout
    selected = {}
    selected_counts = {}
    for line in proc.stdout.splitlines():
        if not line.startswith("BOLT-PREFETCH:") or " inserted " not in line:
            continue
        fields = dict(
            part.split("=", 1) for part in line.split() if "=" in part
        )
        name = line.split()[1]
        selected[name] = fields
        selected_counts[name] = selected_counts.get(name, 0) + 1
        assert float(fields["estimated-benefit-cycles"]) > float(
            fields["estimated-cost-cycles"]
        ), line
    expected = {"free"}
    if mode != "low":
        expected.add("one")
    if mode == "high":
        expected |= {"two", "stack_frame", "multi"}
    assert set(selected) == expected, proc.stdout
    for name, fields in selected.items():
        assert (
            int(fields["spilled-registers"])
            == {"free": 0, "one": 1, "two": 2, "stack_frame": 2, "multi": 2}[
                name
            ]
        )
    if mode == "high":
        assert selected_counts["multi"] == 2, proc.stdout
    assert "unsupported spill frame" in proc.stdout
    assert "spill would change an SP-based prefetch address" in proc.stdout


#--- run.py
import ctypes
import sys

for path in sys.argv[1:]:
    lib = ctypes.CDLL(path)
    data = (ctypes.c_uint64 * 12)(*range(1, 13))
    for name in [
        "free",
        "one",
        "two",
        "stack_frame",
        "frame_pointer",
        "no_cfi",
        "multi",
    ]:
        run = getattr(lib, name)
        run.argtypes = [
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_uint64,
            ctypes.c_uint64,
        ]
        run.restype = ctypes.c_uint64
        assert run(data, 0, 3) == 5, (path, name)


#--- check-unwind.py
from pathlib import Path
import re
import subprocess
import sys

runner, library, assembly = sys.argv[1:]
text = Path(assembly).read_text()
for name in ["one", "two", "stack_frame", "multi"]:
    body = re.search(
        r"^([0-9a-f]+) <" + name + r">:\n(.*?)(?=\n\n|\Z)", text, re.M | re.S
    )
    assert body, name
    instructions = [
        (int(m[1], 16), m[2])
        for m in re.finditer(
            r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(.*)$", body[2], re.M
        )
    ]
    starts = [
        i
        for i, (_, code) in enumerate(instructions)
        if re.search(r"\[sp, #-16\]!", code)
    ]
    assert len(starts) == (2 if name == "multi" else 1), name
    for start in starts:
        end = next(
            i
            for i in range(start + 1, len(instructions))
            if re.search(r"\[sp\], #16", instructions[i][1])
        )
        for pc, _ in instructions[start : end + 2]:
            subprocess.run(
                [runner, library, name, str(pc - int(body[1], 16)), "5"],
                check=True,
            )


#--- input.s
.text
.globl entry
.type entry, %function
entry:
  b free
.size entry, .-entry

.macro function name, unused=0, frame=0, cfi=1, spbase=0, multi=0
.globl \name
.type \name, %function
\name:
.if \cfi
  .cfi_startproc
.endif
.if \frame == 1
  sub sp, sp, #16
  .cfi_def_cfa_offset 16
  str x30, [sp, #8]
  .cfi_offset 30, -8
.elseif \frame == 2
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset 29, -16
  .cfi_offset 30, -8
  mov x29, sp
  .cfi_def_cfa_register 29
.endif
.irp r,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
  mov x\r, #\r
.endr
  movk x3, #0x123, lsl #48
1:
.if \spbase
  ldr q0, [sp, w1, uxtw #4]
.else
  ldr q0, [x0, w1, uxtw #4]
.endif
.if \multi
  add w1, w1, #1
  ldr q1, [x0, w1, uxtw #4]
  sub w1, w1, #1
.endif
.irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  add xzr, x\r, xzr
.endr
.if \unused < 2
  add xzr, x16, xzr
.endif
.if \unused < 1
  add xzr, x17, xzr
.endif
  add w1, w1, #1
  subs x2, x2, #1
  b.ne 1b
  lsr x2, x3, #48
  cmp x2, #0x123
  b.ne 9f
  movk x3, #0, lsl #48
.irp r,3,4,5,6,7,8,9,10,11,12,13,14,15
  cmp x\r, #\r
  b.ne 9f
.endr
.if \unused < 2
  cmp x16, #16
  b.ne 9f
.endif
.if \unused < 1
  cmp x17, #17
  b.ne 9f
.endif
  fmov x0, d0
  b 8f
9:
  mov x0, #0
8:
.if \frame == 1
  ldr x30, [sp, #8]
  .cfi_restore 30
  add sp, sp, #16
  .cfi_def_cfa_offset 0
.elseif \frame == 2
  ldp x29, x30, [sp], #16
  .cfi_restore 29
  .cfi_restore 30
  .cfi_def_cfa 31, 0
.endif
  ret
.if \cfi
  .cfi_endproc
.endif
.size \name, .-\name
.endm

function free, 2
function one, 1
function two
function stack_frame, 0, 1
function frame_pointer, 0, 2
function sp_address, 0, 0, 1, 1
function no_cfi, 0, 0, 0
function multi, 0, 0, 1, 0, 1
