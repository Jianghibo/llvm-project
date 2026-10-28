# REQUIRES: system-linux
## Static checks only; never execute the generated target ELF.
# RUN: rm -rf %t
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 2 > %t.cycles-2.fdata
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 2>&1 | FileCheck %s --check-prefix=LINE
# RUN: llvm-bolt %t.exe -o %t.on --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=3 --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.on | FileCheck %s --check-prefix=CODE
# RUN: llvm-bolt %t.exe -o %t.small --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-batch-max-bytes=64 --verify-cfg 2>&1 | FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.budget --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-max-instructions=1 --verify-cfg 2>&1 | FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.shared --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=4 --loop-prefetch-batch-max-bytes=128 --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=SHARED
# RUN: llvm-bolt %t.exe -o %t.streams --enable-loop-data-prefetch --loop-prefetch-batch --data=%t.cycles-2.fdata --lite=0 --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=4 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=streams %t.streams | FileCheck %s --check-prefix=STREAMS
# RUN: llvm-bolt %t.exe -o %t.auto --data=%t/profile --enable-loop-data-prefetch --loop-prefetch-batch --loop-prefetch-cache-line-size=64 --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=AUTO
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=forward %t.auto | FileCheck %s --check-prefix=FORWARD
# LINE: batch prefetch requires an explicit cache line size
# ZERO: number of prefetch instructions inserted: 0
# DIAG-DAG: BOLT-PREFETCH: forward {{.*}} distance=2 {{.*}} base-delta=64 {{.*}} placement=outer-entry kind=batch inner-iterations=4 requests=2 footprint-bytes=128
# DIAG-DAG: BOLT-PREFETCH: gap {{.*}} base-delta=256 {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: reverse {{.*}} base-delta=-64 {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: prefix {{.*}} base-delta=64 {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: pair {{.*}} base-delta=128 {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: wide {{.*}} kind=batch inner-iterations=16 requests=3 footprint-bytes=192
# DIAG-DAG: BOLT-PREFETCH: variable {{.*}} unsupported constant inner-loop batch shape
# DIAG-DAG: BOLT-PREFETCH: early {{.*}} unsupported constant inner-loop batch shape
# DIAG-DAG: BOLT-PREFETCH: empty {{.*}} unsupported constant inner-loop batch shape
# DIAG-DAG: BOLT-PREFETCH: streams {{.*}} request budget
# DIAG: number of prefetch instructions inserted: 15
# SHARED: BOLT-PREFETCH: streams {{.*}} batch footprint budget
# AUTO: BOLT-PREFETCH: forward {{.*}} source=profile distance=2 {{.*}} kind=batch {{.*}} iteration-cycle-total=1000 iteration-cycle-samples=10 latency-source=default cycles-source=profile

# CODE-LABEL: <forward>:
# CODE:      cbz x2,
# CODE:      [[OUTER:[0-9a-f]+]]: {{.*}}mov w3, #4
# CODE-NEXT: prfm pldl1keep, [x0, #64]
# CODE-NEXT: prfum pldl1keep, [x0, #95]
# CODE-NEXT: [[INNER:[0-9a-f]+]]: {{.*}}ldr x4, [x0], #8
# CODE-NEXT: subs w3, w3, #1
# CODE-NEXT: b.ne 0x[[INNER]]
# CODE-NEXT: subs x2, x2, #1
# CODE-NEXT: b.ne 0x[[OUTER]]
# CODE-LABEL: <gap>:
# CODE:      mov w3, #4
# CODE-NEXT: prfm pldl1keep, [x0, #256]
# CODE-NEXT: add [[TEMP:x([5-9]|1[0-7])]], x0, #287
# CODE-NEXT: prfm pldl1keep, {{\[}}[[TEMP]]{{\]}}
# CODE-NEXT: add x1, x1, x4
# CODE-NEXT: ldr x4, [x0], #8
# CODE-LABEL: <reverse>:
# CODE:      mov w3, #4
# CODE-NEXT: prfum pldl1keep, [x0, #-88]
# CODE-NEXT: prfum pldl1keep, [x0, #-57]
# CODE-NEXT: ldr x4, [x0], #-8
# CODE-LABEL: <prefix>:
# CODE:      mov w3, #4
# CODE-NEXT: prfm pldl1keep, [x0, #72]
# CODE-NEXT: prfum pldl1keep, [x0, #103]
# CODE-NEXT: add x0, x0, #8
# CODE-NEXT: ldr x4, [x0]
# CODE-LABEL: <pair>:
# CODE:      mov x3, #4
# CODE-NEXT: prfm pldl1keep, [x0, #128]
# CODE-NEXT: prfum pldl1keep, [x0, #191]
# CODE-NEXT: ldp x4, x5, [x0], #16
# CODE-LABEL: <wide>:
# CODE:      mov w3, #16
# CODE-NEXT: prfm pldl1keep, [x0, #256]
# CODE-NEXT: prfm pldl1keep, [x0, #320]
# CODE-NEXT: add [[LAST:x[0-9]+]], x0, #383
# CODE-NEXT: prfm pldl1keep, {{\[}}[[LAST]]{{\]}}
# CODE-NEXT: ldr x4, [x0], #8
# CODE-LABEL: <variable>:
# CODE-NOT:  {{prfm|prfum}}
# CODE-LABEL: <early>:
# CODE-NOT:  {{prfm|prfum}}
# CODE-LABEL: <empty>:
# CODE-NOT:  {{prfm|prfum}}
# CODE-LABEL: <streams>:
# CODE:      mov w3, #4
# CODE-NEXT: prfm pldl1keep, [x0, #64]
# CODE-NEXT: prfum pldl1keep, [x0, #95]
# CODE-NEXT: ldr x4, [x0], #8
# CODE-NEXT: ldr x5, [x1], #8
# CODE-NOT:  {{prfm|prfum}}

# FORWARD-LABEL: <forward>:
# FORWARD:      mov w3, #4
# FORWARD-NEXT: prfm pldl1keep, [x0, #64]
# FORWARD-NEXT: prfum pldl1keep, [x0, #95]
# FORWARD-NEXT: ldr x4, [x0], #8

# STREAMS-LABEL: <streams>:
# STREAMS:      mov w3, #4
# STREAMS-NEXT: prfm pldl1keep, [x0, #64]
# STREAMS-NEXT: prfum pldl1keep, [x0, #95]
# STREAMS-NEXT: prfm pldl1keep, [x1, #64]
# STREAMS-NEXT: prfum pldl1keep, [x1, #95]
# STREAMS-NEXT: ldr x4, [x0], #8

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  bl forward
  ret
.size _start, .-_start

.globl forward
.type forward, %function
forward:
  .cfi_startproc
  cbz x2, .Ldone_forward
.Louter_forward:
  mov w3, #4
.Linner_forward:
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_forward
  subs x2, x2, #1
  b.ne .Louter_forward
.Ldone_forward:
  ret
  .cfi_endproc
.size forward, .-forward

.globl gap
.type gap, %function
gap:
  cbz x2, .Ldone_gap
.Louter_gap:
  mov w3, #4
.Linner_gap:
  add x1, x1, x4
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_gap
  add x0, x0, #96
  subs x2, x2, #1
  b.ne .Louter_gap
.Ldone_gap:
  ret
.size gap, .-gap

.globl reverse
.type reverse, %function
reverse:
  cbz x2, .Ldone_reverse
.Louter_reverse:
  mov w3, #4
.Linner_reverse:
  ldr x4, [x0], #-8
  subs w3, w3, #1
  b.ne .Linner_reverse
  subs x2, x2, #1
  b.ne .Louter_reverse
.Ldone_reverse:
  ret
.size reverse, .-reverse

.globl prefix
.type prefix, %function
prefix:
  cbz x2, .Ldone_prefix
.Louter_prefix:
  mov w3, #4
.Linner_prefix:
  add x0, x0, #8
  ldr x4, [x0]
  subs w3, w3, #1
  b.ne .Linner_prefix
  subs x2, x2, #1
  b.ne .Louter_prefix
.Ldone_prefix:
  ret
.size prefix, .-prefix

.globl pair
.type pair, %function
pair:
  cbz x2, .Ldone_pair
.Louter_pair:
  mov x3, #4
.Linner_pair:
  ldp x4, x5, [x0], #16
  subs x3, x3, #1
  b.ne .Linner_pair
  subs x2, x2, #1
  b.ne .Louter_pair
.Ldone_pair:
  ret
.size pair, .-pair

.globl wide
.type wide, %function
wide:
  cbz x2, .Ldone_wide
.Louter_wide:
  mov w3, #16
.Linner_wide:
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_wide
  subs x2, x2, #1
  b.ne .Louter_wide
.Ldone_wide:
  ret
.size wide, .-wide

.globl variable
.type variable, %function
variable:
  cbz x2, .Ldone_variable
.Louter_variable:
  mov w3, w1
.Linner_variable:
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_variable
  subs x2, x2, #1
  b.ne .Louter_variable
.Ldone_variable:
  ret
.size variable, .-variable

.globl early
.type early, %function
early:
  cbz x2, .Ldone_early
.Louter_early:
  mov w3, #4
.Linner_early:
  cbz x1, .Llatch_early
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_early
.Llatch_early:
  subs x2, x2, #1
  b.ne .Louter_early
.Ldone_early:
  ret
.size early, .-early

.globl empty
.type empty, %function
empty:
  cbz x2, .Ldone_empty
.Louter_empty:
  mov w3, #0
  cbz w3, .Llatch_empty
.Linner_empty:
  ldr x4, [x0], #8
  subs w3, w3, #1
  b.ne .Linner_empty
.Llatch_empty:
  subs x2, x2, #1
  b.ne .Louter_empty
.Ldone_empty:
  ret
.size empty, .-empty

.globl streams
.type streams, %function
streams:
  cbz x2, .Ldone_streams
.Louter_streams:
  mov w3, #4
.Linner_streams:
  ldr x4, [x0], #8
  ldr x5, [x1], #8
  subs w3, w3, #1
  b.ne .Linner_streams
  subs x2, x2, #1
  b.ne .Louter_streams
.Ldone_streams:
  ret
.size streams, .-streams

#--- profile
bolt-prefetch llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 forward 0 0 1 0 0
1 forward 10 1 forward 8 0 48 20 10
1 forward 10 1 forward 14 0 16 0 0
1 forward 18 1 forward 4 0 15 1000 10
7 forward 8 8 10
