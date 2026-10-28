# REQUIRES: system-linux
## Static checks only: none of the generated target programs are executed.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 4 > %t.cycles-4.fdata
# RUN: llvm-bolt %t.exe -o %t.affine --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.affine | FileCheck %s --check-prefix=AFFINE
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 1 > %t.cycles-1.fdata
# RUN: llvm-bolt %t.exe -o %t.pointer --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-1.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=POINTER-DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.pointer | FileCheck %s --check-prefix=POINTER
# RUN: llvm-dwarfdump --eh-frame %t.pointer | FileCheck %s --check-prefix=CFI
# RUN: llvm-bolt %t.exe -o %t.guarded --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-1.fdata --lite=0 --loop-prefetch-cache-line-size=64 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=mixed %t.guarded | FileCheck %s --check-prefix=GUARD
# RUN: llvm-bolt %t.exe -o %t.auto --data=%t/profile \
# RUN:   --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=AUTO-DIAG
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=affine,pointer,pointer_copy %t.auto | FileCheck %s --check-prefix=AUTO

# DIAG-DAG: BOLT-PREFETCH: affine {{.*}} inserted base-delta=32
# DIAG-DAG: BOLT-PREFETCH: chain {{.*}} inserted base-delta=32
# DIAG-DAG: BOLT-PREFETCH: partial_write {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: cross_block {{.*}} inserted base-delta=32
# DIAG-DAG: BOLT-PREFETCH: pointer {{.*}} pointer chase requires distance 1
# DIAG-DAG: BOLT-PREFETCH: pointer_modified {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: subtract {{.*}} inserted base-delta=-32
# DIAG-DAG: BOLT-PREFETCH: ambiguous {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: header_value {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: pointer_clobber {{.*}} unproven recurrence

# AFFINE-LABEL: <affine>:
# AFFINE:      add x8, x0, x1, lsl #3
# AFFINE-NEXT: prfm pldl1keep, [x8, #32]
# AFFINE-NEXT: ldr x3, [x8]
# AFFINE-LABEL: <chain>:
# AFFINE:      lsl x8, x1, #3
# AFFINE-NEXT: add x8, x0, x8
# AFFINE-NEXT: add x8, x8, #16
# AFFINE-NEXT: mov x9, x8
# AFFINE-NEXT: prfm pldl1keep, [x9, #32]
# AFFINE-NEXT: ldr x3, [x9]
# AFFINE-LABEL: <partial_write>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <cross_block>:
# AFFINE:      prfm pldl1keep, [x8, #32]
# AFFINE-NEXT: ldr x3, [x8]
# AFFINE-LABEL: <pointer>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <pointer_modified>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <mixed>:
# AFFINE-LABEL: <subtract>:
# AFFINE:      sub x8, x0, x1, lsl #3
# AFFINE-NEXT: prfum pldl1keep, [x8, #-32]
# AFFINE-NEXT: ldr x3, [x8]
# AFFINE-LABEL: <pointer_copy>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <ambiguous>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <header_value>:
# AFFINE-NOT:  prfm
# AFFINE-LABEL: <pointer_clobber>:
# AFFINE-NOT:  prfm

# POINTER-DIAG: BOLT-PREFETCH: pointer {{.*}} inserted {{.*}} placement=after-load
# POINTER-LABEL: <pointer>:
# POINTER:      ldr x0, [x0, #8]
# POINTER-NEXT: prfm pldl1keep, [x0, #8]
# POINTER-NEXT: cbnz x0,
# POINTER-NOT:  ldr
# POINTER-LABEL: <pointer_modified>:
# POINTER-NOT:  prfm
# POINTER-LABEL: <mixed>:
# POINTER:      ldr x0, [x0, #8]
# POINTER-NEXT: prfm pldl1keep, [x0, #8]
# POINTER-NEXT: add x8, x4, x1, lsl #3
# POINTER-NEXT: prfm pldl1keep, [x8, #8]
# POINTER-NEXT: ldr x3, [x8]
# POINTER-LABEL: <pointer_copy>:
# POINTER:      ldr x5, [x0, #8]
# POINTER-NEXT: prfm pldl1keep, [x5, #8]
# POINTER-NEXT: cbz x2,
# POINTER:      ldr x3, [x0, #16]
# POINTER-NEXT: mov x0, x5
# POINTER-NEXT: cbnz x0,
# POINTER-LABEL: <pointer_clobber>:
# POINTER-NOT:  prfm

# CFI:      DW_CFA_offset: W30 -8
# CFI-NEXT: DW_CFA_advance_loc: 8
# CFI-NEXT: DW_CFA_offset: W19 -16

# GUARD-LABEL: <mixed>:
# GUARD:      ldr x0, [x0, #8]
# GUARD-NEXT: prfm pldl1keep, [x0, #8]
# GUARD-NEXT: add x8, x4, x1, lsl #3
# GUARD:      cbnz
# GUARD:      prfm pldl1keep,
# GUARD:      ldr x3, [x8]
# GUARD-NOT:  ldr

# AUTO-DIAG-DAG: BOLT-PREFETCH: affine {{.*}} source=profile distance=1 {{.*}} inserted base-delta=8 {{.*}} cycles-source=profile
# AUTO-DIAG-DAG: BOLT-PREFETCH: pointer {{.*}} source=profile distance=1 {{.*}} placement=after-load {{.*}} cycles-source=profile
# AUTO-DIAG-DAG: BOLT-PREFETCH: pointer_copy {{.*}} source=profile distance=1 {{.*}} placement=after-load {{.*}} cycles-source=profile
# AUTO-LABEL: <affine>:
# AUTO:      prfm pldl1keep, [x8, #8]
# AUTO-NEXT: ldr x3, [x8]
# AUTO-LABEL: <pointer>:
# AUTO:      ldr x0, [x0, #8]
# AUTO-NEXT: prfm pldl1keep, [x0, #8]
# AUTO-NEXT: cbnz x0,
# AUTO-LABEL: <pointer_copy>:
# AUTO:      ldr x5, [x0, #8]
# AUTO-NEXT: prfm pldl1keep, [x5, #8]
# AUTO-NOT:  prfm
# AUTO:      ldr x3, [x0, #16]

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  ret
.size _start, .-_start

.globl affine
.type affine, %function
affine:
  add x8, x0, x1, lsl #3
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne affine
  ret
.size affine, .-affine

.globl chain
.type chain, %function
chain:
  lsl x8, x1, #3
  add x8, x0, x8
  add x8, x8, #16
  mov x9, x8
  ldr x3, [x9]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne chain
  ret
.size chain, .-chain

.globl partial_write
.type partial_write, %function
partial_write:
  add x8, x0, x1, lsl #3
  mov w8, w2
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne partial_write
  ret
.size partial_write, .-partial_write

.globl cross_block
.type cross_block, %function
cross_block:
  add x8, x0, x1, lsl #3
  cbz x2, .Lcross_exit
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne cross_block
.Lcross_exit:
  ret
.size cross_block, .-cross_block

.globl pointer
.type pointer, %function
pointer:
  cbz x0, .Lpointer_exit
.Lpointer_loop:
  ldr x0, [x0, #8]
  cbnz x0, .Lpointer_loop
.Lpointer_exit:
  ret
.size pointer, .-pointer

.globl pointer_modified
.type pointer_modified, %function
pointer_modified:
  ldr x0, [x0, #8]
  add x0, x0, #16
  subs x1, x1, #1
  b.ne pointer_modified
  ret
.size pointer_modified, .-pointer_modified

.globl mixed
.type mixed, %function
mixed:
  .cfi_startproc
  ldr x0, [x0, #8]
  add x8, x4, x1, lsl #3
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne mixed
  ret
  .cfi_endproc
.size mixed, .-mixed

.globl subtract
.type subtract, %function
subtract:
  sub x8, x0, x1, lsl #3
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne subtract
  ret
.size subtract, .-subtract

.globl pointer_copy
.type pointer_copy, %function
pointer_copy:
  .cfi_startproc
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x30, -8
  cbz x0, .Lcopy_exit
.Lcopy_loop:
  ldr x5, [x0, #8]
  .cfi_offset x19, -16
  cbz x2, .Lcopy_exit
  ldr x3, [x0, #16]
  mov x0, x5
  cbnz x0, .Lcopy_loop
.Lcopy_exit:
  ldp x19, x30, [sp], #16
  .cfi_restore x19
  .cfi_restore x30
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc
.size pointer_copy, .-pointer_copy

.globl ambiguous
.type ambiguous, %function
ambiguous:
  cbz x4, .Lother_base
  add x8, x0, x1, lsl #3
  b .Lmerge
.Lother_base:
  add x8, x5, x1, lsl #3
.Lmerge:
  ldr x3, [x8]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne ambiguous
  ret
.size ambiguous, .-ambiguous

.globl header_value
.type header_value, %function
header_value:
  ldr x3, [x8]
  add x8, x0, x1, lsl #3
  add x1, x1, #1
  subs x2, x2, #1
  b.ne header_value
  ret
.size header_value, .-header_value

.globl pointer_clobber
.type pointer_clobber, %function
pointer_clobber:
  ldr x5, [x0, #8]
  mov w5, w4
  mov x0, x5
  cbnz x0, pointer_clobber
  ret
.size pointer_clobber, .-pointer_clobber

#--- profile
bolt-prefetch llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 affine 0 0 1 0 0
0 [unknown] 0 1 pointer 0 0 1 0 0
1 affine 10 1 affine 0 0 15 2000 10
1 pointer 8 1 pointer 4 0 15 2000 10
7 affine 4 8 10
7 pointer 4 8 10
0 [unknown] 0 1 pointer_copy 0 0 1 0 0
1 pointer_copy 18 1 pointer_copy 8 0 15 2000 10
7 pointer_copy 8 8 10
