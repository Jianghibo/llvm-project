# REQUIRES: system-linux
## Static checks only; never execute the generated target ELF.
# RUN: rm -rf %t
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: llvm-nm --print-size %t.exe > %t/symbols
# RUN: %python %t/profile.py %t/symbols > %t/selected-profile
# RUN: llvm-bolt %t.exe -o %t.off --enable-loop-data-prefetch --data=%t/selected-profile --loop-prefetch-max-instructions=12 --lite=0 --verify-cfg 2>&1 | FileCheck %s --check-prefix=OFF
# RUN: llvm-bolt %t.exe -o %t.on --enable-loop-data-prefetch --loop-prefetch-indirect --data=%t/selected-profile --loop-prefetch-max-instructions=12 --lite=0 --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.on | FileCheck %s --check-prefix=CODE
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 1 > %t.mixed-input
# RUN: sed 's/7 gather32 4 0 10/7 gather32 4 21 100/' %t.mixed-input > %t.mixed-profile
# RUN: llvm-bolt %t.exe -o %t.mixed --enable-loop-data-prefetch --loop-prefetch-indirect --data=%t.mixed-profile --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-max-instructions=16 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=gather32 %t.mixed | FileCheck %s --check-prefix=MIXED
# RUN: llvm-bolt %t.exe -o %t.zero --enable-loop-data-prefetch --loop-prefetch-indirect --data=%t/selected-profile --loop-prefetch-max-instructions=12 --lite=0 --loop-prefetch-max-per-loop=0 --verify-cfg 2>&1 | FileCheck %s --check-prefix=OFF
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=gather32 %t.zero | FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.auto --data=%t/profile --enable-loop-data-prefetch --loop-prefetch-indirect --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=AUTO
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=gather32 %t.auto | FileCheck %s --check-prefix=AUTO-CODE

# OFF: number of prefetch instructions inserted: 0
# DIAG-DAG: BOLT-PREFETCH: gather32 {{.*}} inserted {{.*}} kind=indirect
# DIAG-DAG: BOLT-PREFETCH: gather64 {{.*}} inserted {{.*}} kind=indirect
# DIAG-DAG: BOLT-PREFETCH: signed_index {{.*}} inserted {{.*}} kind=indirect
# DIAG-DAG: BOLT-PREFETCH: indexed {{.*}} inserted {{.*}} kind=indirect
# DIAG-DAG: BOLT-PREFETCH: stores {{.*}} unsupported indirect bounds
# DIAG-DAG: BOLT-PREFETCH: early_exit {{.*}} unsupported indirect bounds
# DIAG-DAG: BOLT-PREFETCH: unguarded {{.*}} unsupported indirect bounds
# DIAG-DAG: BOLT-PREFETCH: clobber {{.*}} unsupported indirect bounds
# DIAG: number of prefetch instructions inserted: 4

# CODE-LABEL: <gather32>:
# CODE:      cbz x2,
# CODE:      ldr w3, [x0], #4
# CODE-NEXT: cmp x5, #0
# CODE-NEXT: sub [[GUARD:x[0-9]+]], x2, #5
# CODE-NEXT: bic [[GUARD]], [[GUARD]], x2
# CODE-NEXT: tbnz [[GUARD]], #63, 0x[[JOIN:[0-9a-f]+]]
# CODE-NOT:  ldr
# CODE:      ldr [[INDEX:w[0-9]+]], [x0, #12]
# CODE-NEXT: prfm pldl1keep, [x1, [[INDEX]], sxtw #3]
# CODE:      [[JOIN]]: {{.*}}ldr x4, [x1, w3, sxtw #3]
# CODE-NEXT: csel x6, x6, x4, eq
# CODE-NEXT: subs x2, x2, #1

# CODE-LABEL: <gather64>:
# CODE:      ldr x3, [x0], #8
# CODE-NEXT: sub [[WG:w[0-9]+]], w2, #5
# CODE-NEXT: bic [[WG]], [[WG]], w2
# CODE-NEXT: tbnz [[WG]], #31,
# CODE:      ldr [[XI:x[0-9]+]], [x0, #24]
# CODE-NEXT: add [[EA:x[0-9]+]], x1, [[XI]], lsl #2
# CODE-NEXT: prfm pldl1keep, {{\[}}[[EA]]{{\]}}
# CODE:      ldr w4, [x1, x3, lsl #2]

# CODE-LABEL: <signed_index>:
# CODE:      ldrsw x3, [x0], #4
# CODE:      tbnz
# CODE:      ldrsw [[SI:x[0-9]+]], [x0, #12]
# CODE-NEXT: prfm pldl1keep, [x1, [[SI]], lsl #3]
# CODE:      ldr x4, [x1, x3, lsl #3]

# CODE-LABEL: <indexed>:
# CODE:      ldr w3, [x0, x5, lsl #2]
# CODE:      tbnz
# CODE:      add [[BASE:x[0-9]+]], x0, #16
# CODE-NEXT: add [[BASE]], [[BASE]], x5, lsl #2
# CODE-NEXT: ldr [[WI:w[0-9]+]], {{\[}}[[BASE]]{{\]}}
# CODE-NEXT: prfm pldl1keep, [x1, [[WI]], uxtw #3]
# CODE:      ldr x4, [x1, w3, uxtw #3]
# CODE-LABEL: <stores>:
# CODE-NOT:  prfm
# CODE:      ldr w3, [x0], #4
# CODE-NEXT: ldr x4, [x1, w3, uxtw #3]
# CODE-LABEL: <early_exit>:
# CODE-NOT:  prfm
# CODE-LABEL: <unguarded>:
# CODE-NOT:  prfm
# CODE-LABEL: <clobber>:
# CODE-NOT:  prfm

# MIXED-LABEL: <gather32>:
# MIXED:      cbnz
# MIXED:      prfm
# MIXED:      ldr w3, [x0], #4
# MIXED:      tbnz
# MIXED:      ldr {{w[0-9]+}}, [x0]
# MIXED-NEXT: prfm
# MIXED:      ldr x4, [x1, w3, sxtw #3]

# ZERO-LABEL: <gather32>:
# ZERO:      ldr w3, [x0], #4
# ZERO-NEXT: cmp x5, #0
# ZERO-NEXT: ldr x4, [x1, w3, sxtw #3]
# ZERO-NOT:  ldr

# AUTO: BOLT-PREFETCH: gather32 {{.*}} source=profile distance=4 {{.*}} kind=indirect
# AUTO-CODE-LABEL: <gather32>:
# AUTO-CODE:      tbnz {{x[0-9]+}}, #63,
# AUTO-CODE-NEXT: ldr [[FUTURE:w[0-9]+]], [x0, #12]
# AUTO-CODE-NEXT: prfm pldl1keep, [x1, [[FUTURE]], sxtw #3]
# AUTO-CODE:      ldr x4, [x1, w3, sxtw #3]


#--- profile.py
import sys
from pathlib import Path

symbols = {p[3]: int(p[1], 16)
           for line in Path(sys.argv[1]).read_text().splitlines()
           if len(p := line.split()) == 4}
print("bolt-prefetch llc_miss 11223344 1024 200000001 0 0")
for name, offset in [
    ("gather32", 12), ("gather64", 8), ("signed_index", 8), ("indexed", 16),
    ("stores", 8), ("early_exit", 12), ("unguarded", 4), ("clobber", 12),
]:
    header = {"unguarded": 0, "indexed": 12}.get(name, 4)
    print(f"0 [unknown] 0 1 {name} 0 0 1 0 0")
    print(f"1 {name} {symbols[name] - 8:x} 1 {name} {header:x} 0 15 500 10")
    print(f"7 {name} {offset:x} 8 10")
    print(f"7 {name} {header:x} 0 10")

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  ret
.size _start, .-_start

.globl gather32
.type gather32, %function
gather32:
  cbz x2, .Ldone32
.Lloop32:
  ldr w3, [x0], #4
  cmp x5, #0
  ldr x4, [x1, w3, sxtw #3]
  csel x6, x6, x4, eq
  subs x2, x2, #1
  b.ne .Lloop32
.Ldone32:
  ret
.size gather32, .-gather32

.globl gather64
.type gather64, %function
gather64:
  cbz w2, .Ldone64
.Lloop64:
  ldr x3, [x0], #8
  ldr w4, [x1, x3, lsl #2]
  subs w2, w2, #1
  b.ne .Lloop64
.Ldone64:
  ret
.size gather64, .-gather64

.globl signed_index
.type signed_index, %function
signed_index:
  cbz x2, .Ldone_signed
.Lloop_signed:
  ldrsw x3, [x0], #4
  ldr x4, [x1, x3, lsl #3]
  subs x2, x2, #1
  b.ne .Lloop_signed
.Ldone_signed:
  ret
.size signed_index, .-signed_index

.globl indexed
.type indexed, %function
indexed:
  mov x5, #0
  cbnz x2, .Lloop_indexed
  ret
.Lloop_indexed:
  ldr w3, [x0, x5, lsl #2]
  ldr x4, [x1, w3, uxtw #3]
  add x5, x5, #1
  subs x2, x2, #1
  b.ne .Lloop_indexed
  ret
.size indexed, .-indexed

.globl stores
.type stores, %function
stores:
  cbz x2, .Ldone_stores
.Lloop_stores:
  ldr w3, [x0], #4
  ldr x4, [x1, w3, uxtw #3]
  str x4, [x6]
  subs x2, x2, #1
  b.ne .Lloop_stores
.Ldone_stores:
  ret
.size stores, .-stores

.globl early_exit
.type early_exit, %function
early_exit:
  cbz x2, .Ldone_early
.Lloop_early:
  ldr w3, [x0], #4
  cbz x7, .Ldone_early
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne .Lloop_early
.Ldone_early:
  ret
.size early_exit, .-early_exit

.globl unguarded
.type unguarded, %function
unguarded:
  ldr w3, [x0], #4
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne unguarded
  ret
.size unguarded, .-unguarded

.globl clobber
.type clobber, %function
clobber:
  cbz x2, .Ldone_clobber
.Lloop_clobber:
  ldr w3, [x0], #4
  add w3, w3, #1
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne .Lloop_clobber
.Ldone_clobber:
  ret
.size clobber, .-clobber

#--- profile
bolt-prefetch llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 gather32 0 0 1 0 0
1 gather32 18 1 gather32 4 0 15 500 10
7 gather32 c 8 10
7 gather32 4 0 10
