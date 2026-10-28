# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --build-id=0x11223344 --emit-relocs -e _start %t.o -o %t.exe
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 4 > %t.cycles-4.fdata
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %t.exe && %t.out %}
# RUN: llvm-bolt %t.exe -o %t.large --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-hint=pldl3strm \
# RUN:   --loop-prefetch-max-instructions=12 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=large %t.large | FileCheck %s --check-prefix=LARGE
# RUN: %if target=aarch64{{.*}} %{ %t.large %}
# RUN: llvm-bolt %t.exe -o %t.disabled --enable-loop-data-prefetch \
# RUN:   --lite=0 2>&1 | FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.budget --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-max-instructions=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.zero --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-max-per-loop=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.limit --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-analysis-limit=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: ld.lld -pie --no-dynamic-linker --build-id=0x11223344 --emit-relocs -e _start %t.o -o %t.pie
# RUN: llvm-bolt %t.pie -o %t.pie.out --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --verify-cfg
# RUN: %if target=aarch64{{.*}} %{ %t.pie && %t.pie.out %}
# RUN: ld.lld -shared -Bsymbolic --build-id=0x11223344 --emit-relocs %t.o -o %t.so
# RUN: llvm-bolt %t.so -o %t.so.out --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex %t.so.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %python %t/check-dso.py %t.so.out %}
# RUN: llvm-bolt %t.exe -o %t.throttled --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-cache-line-size=64 \
# RUN:   --loop-prefetch-diagnostics --enable-bat --verify-cfg 2>&1 | FileCheck %s --check-prefix=THROTTLE
# RUN: llvm-objdump -d --no-print-imm-hex %t.throttled | FileCheck %s --check-prefix=GUARD
# RUN: %if target=aarch64{{.*}} %{ %t.throttled %}
# RUN: llvm-bolt %t.pie -o %t.pie.throttled --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-cache-line-size=64 --verify-cfg
# RUN: %if target=aarch64{{.*}} %{ %t.pie.throttled %}
# RUN: llvm-bolt %t.so -o %t.so.throttled --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-cache-line-size=64 --verify-cfg
# RUN: %if target=aarch64{{.*}} %{ %python %t/check-dso.py %t.so.throttled %}
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-cache-line-size=48 2>&1 | FileCheck %s --check-prefix=BAD-LINE
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-hint=invalid 2>&1 | FileCheck %s --check-prefix=BAD-OPTION

# DIAG-DAG: BOLT-PREFETCH: with_cfi {{.*}} source=profile distance=4 {{.*}} inserted
# DIAG-DAG: BOLT-PREFETCH: multiblock {{.*}} inserted
# DIAG-DAG: BOLT-PREFETCH: conditional_update {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: pressure {{.*}} insufficient dead registers
# DIAG-DAG: BOLT-PREFETCH: mixed_width {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: opaque_cfi {{.*}} opaque CFI expression
# DIAG-DAG: BOLT-PREFETCH: atomic_loop {{.*}} unsupported loop context
# DIAG-DAG: BOLT-PREFETCH: duplicate {{.*}} duplicate address
# ZERO: number of prefetch instructions inserted: 0
# BAD-OPTION: invalid loop prefetch hint or cutoff

# CHECK-LABEL: <with_cfi>:
# CHECK:      add w2, w1, #{{(4|0x4)}}
# CHECK-NEXT: add x0, x19, w2, uxtw #3
# CHECK-NEXT: prfm pldl1keep, [x0]
# CHECK-NEXT: ldr x2, [x19, w1, uxtw #3]
# CHECK-LABEL: <post_index>:
# CHECK:      prfm pldl1keep, [x0, #{{(32|0x20)}}]
# CHECK-NEXT: ldr x2, [x0], #8
# CHECK-LABEL: <pre_index>:
# CHECK:      prfm pldl1keep, [x0, #{{(80|0x50)}}]
# CHECK-NEXT: prfm pldl1keep, [x0, #{{(88|0x58)}}]
# CHECK-NEXT: ldp x2, x3, [x0, #{{(16|0x10)}}]!
# CHECK-LABEL: <signed_index>:
# CHECK:      sub [[SIGNED_IDX:w[0-9]+]], w1, #8
# CHECK:      add [[SIGNED_ADDR:x[0-9]+]], x0, [[SIGNED_IDX]], sxtw #4
# CHECK-NEXT: prfm pldl1keep, {{\[}}[[SIGNED_ADDR]]{{\]}}
# CHECK-NEXT: ldr q2, [x0, w1, sxtw #4]
# CHECK-LABEL: <multiblock>:
# CHECK:      prfm
# CHECK-LABEL: <conditional_update>:
# CHECK-NOT:  prfm
# CHECK-LABEL: <pressure>:
# CHECK-NOT:  prfm
# CHECK-LABEL: <mixed_width>:
# CHECK-NOT:  prfm
# CHECK-LABEL: <unrolled>:
# CHECK:      add x{{[0-9]+}}, x0, x1, lsl #3
# CHECK:      prfm pldl1keep, [x{{[0-9]+}}, #64]
# CHECK:      add x{{[0-9]+}}, x0, x1, lsl #3
# CHECK:      prfm pldl1keep, [x{{[0-9]+}}, #64]
# CHECK-LABEL: <large>:
# CHECK:      add x{{[0-9]+}}, x0, #32, lsl #12
# CHECK:      prfm
# CHECK-LABEL: <opaque_cfi>:
# CHECK-NOT:  prfm
# CHECK-LABEL: <atomic_loop>:
# CHECK-NOT:  prfm
# CHECK-LABEL: <zero_guard>:
# CHECK:      prfm pldl1keep, [x0, #32]
# CHECK-LABEL: <flags_live>:
# CHECK:      cmp x1, x3
# CHECK-NEXT: add x2, x0, x1, lsl #3
# CHECK-NEXT: prfm pldl1keep, [x2, #32]
# CHECK-NEXT: ldr x2, [x0, x1, lsl #3]
# CHECK-NEXT: b.hs
# CHECK-LABEL: <duplicate>:
# CHECK:      prfm
# CHECK-NEXT: ldr x2, [x0, x1, lsl #3]
# CHECK-NOT:  prfm
# CHECK:      ldr x4, [x0, x1, lsl #3]
# LARGE-LABEL: <large>:
# LARGE:      add x{{[0-9]+}}, x0, #32, lsl #12
# LARGE:      prfm pldl3strm

# THROTTLE-DAG: BOLT-PREFETCH: with_cfi {{.*}} cache-line=64
# THROTTLE-DAG: BOLT-PREFETCH: pre_index {{.*}} cache-line=64
# BAD-LINE: cache line size must be 0 or a power of two
# GUARD-LABEL: <with_cfi>:
# GUARD:      and x2, x0, #0x38
# GUARD-NEXT: cbnz x2,
# GUARD:      prfm pldl1keep, [x0]
# GUARD-LABEL: <pre_index>:
# GUARD:      and x3, x2, #0x30
# GUARD:      prfm pldl1keep, [x2]
# GUARD-NOT:  prfm
# GUARD:      ldp x2, x3,
# GUARD-LABEL: <odd_stride>:
# GUARD:      sub [[ODD_TMP:x[0-9]+]], [[ODD_ADDR:x[0-9]+]], #3
# GUARD-NEXT: eor [[ODD_TMP]], [[ODD_TMP]], [[ODD_ADDR]]
# GUARD-NEXT: lsr [[ODD_TMP]], [[ODD_TMP]], #6
# GUARD-NEXT: cbz [[ODD_TMP]],
# GUARD:      prfm pldl1keep, {{\[}}[[ODD_ADDR]]{{\]}}
# GUARD-LABEL: <reverse_stride>:
# GUARD:      add [[REV_TMP:x[0-9]+]], [[REV_ADDR:x[0-9]+]], #3
# GUARD-NEXT: eor [[REV_TMP]], [[REV_TMP]], [[REV_ADDR]]
# GUARD-NEXT: lsr [[REV_TMP]], [[REV_TMP]], #6
# GUARD-NEXT: cbz [[REV_TMP]],
# GUARD-LABEL: <conditional_load>:
# GUARD-NOT:  and
# GUARD:      prfm pldl1keep, [x2, #32]
# GUARD-NEXT: ldr x2, [x0, x1, lsl #3]

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  adrp x0, array
  add x0, x0, :lo12:array
  mov x3, #16
  bl with_cfi
  cmp x0, #15
  b.ne fail
  adrp x0, array
  add x0, x0, :lo12:array
  mov x1, #16
  bl post_index
  cmp x0, #15
  b.ne fail
  adrp x0, array
  add x0, x0, :lo12:array
  mov x1, #0
  mov x3, #16
  mov x4, #1
  bl multiblock
  cmp x2, #15
  b.ne fail
  adrp x0, array
  add x0, x0, :lo12:array
  mov x1, #1
  bl post_index
  cmp x0, #0
  b.ne fail
  mov x0, #0
  mov x1, #0
  bl zero_guard
  cmp x0, #0
  b.ne fail
  adrp x0, array
  add x0, x0, :lo12:array
  mov x1, #0
  mov x3, #15
  bl flags_live
  cmp x0, #15
  b.ne fail
  bl check_strides
  cbnz x0, fail
  bl guard_prefetch
  cbnz x0, fail
  mov x0, #0
  b exit
fail:
  mov x0, #1
exit:
  mov x8, #93
  svc #0
.size _start, .-_start

.globl with_cfi
.type with_cfi, %function
with_cfi:
  .cfi_startproc
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  mov x19, x0
  mov w1, #0
.Lcfi:
  ldr x2, [x19, w1, uxtw #3]
  add w1, w1, #1
  cmp w1, w3
  b.lo .Lcfi
  mov x0, x2
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
  .cfi_endproc
.size with_cfi, .-with_cfi

.globl post_index
.type post_index, %function
post_index:
  ldr x2, [x0], #8
  subs x1, x1, #1
  b.ne post_index
  mov x0, x2
  ret
.size post_index, .-post_index

.globl pre_index
.type pre_index, %function
pre_index:
  ldp x2, x3, [x0, #16]!
  subs x1, x1, #1
  b.ne pre_index
  ret
.size pre_index, .-pre_index

.globl signed_index
.type signed_index, %function
signed_index:
  ldr q2, [x0, w1, sxtw #4]
  sub w1, w1, #2
  cmp w1, w4
  b.ne signed_index
  ret
.size signed_index, .-signed_index

.globl multiblock
.type multiblock, %function
multiblock:
  cbz x4, .Lload
  nop
  b .Lload
.Lload:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo multiblock
  ret
.size multiblock, .-multiblock

.globl conditional_update
.type conditional_update, %function
conditional_update:
  ldr x2, [x0, x1, lsl #3]
  cbz x4, .Lnext
  add x1, x1, #1
.Lnext:
  subs x3, x3, #1
  b.ne conditional_update
  ret
.size conditional_update, .-conditional_update

.globl pressure
.type pressure, %function
pressure:
  ldr q2, [x0, x1, lsl #4]
  .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
    add xzr, x\r, xzr
  .endr
  add x1, x1, #1
  cmp x1, x3
  b.lo pressure
  ret
.size pressure, .-pressure

.globl mixed_width
.type mixed_width, %function
mixed_width:
  ldr x2, [x0, x1, lsl #3]
  add w1, w1, #1
  cmp w1, w3
  b.lo mixed_width
  ret
.size mixed_width, .-mixed_width

.globl unrolled
.type unrolled, %function
unrolled:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  ldr x4, [x0, x1, lsl #3]
  add x1, x1, #1
  subs x3, x3, #1
  b.ne unrolled
  ret
.size unrolled, .-unrolled

.globl large
.type large, %function
large:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1, lsl #12
  cmp x1, x3
  b.lo large
  ret
.size large, .-large

.globl opaque_cfi
.type opaque_cfi, %function
opaque_cfi:
  .cfi_startproc
  .cfi_escape 0x0f, 0x02, 0x77, 0x00
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo opaque_cfi
  ret
  .cfi_endproc
.size opaque_cfi, .-opaque_cfi

.globl atomic_loop
.type atomic_loop, %function
atomic_loop:
  ldar x4, [x0]
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo atomic_loop
  ret
.size atomic_loop, .-atomic_loop

.globl zero_guard
.type zero_guard, %function
zero_guard:
  cbz x1, .Lzero_exit
.Lzero_body:
  ldr x2, [x0], #8
  subs x1, x1, #1
  b.ne .Lzero_body
  mov x0, x2
.Lzero_exit:
  ret
.size zero_guard, .-zero_guard

.globl flags_live
.type flags_live, %function
flags_live:
  cmp x1, x3
  ldr x2, [x0, x1, lsl #3]
  b.hs .Lflags_exit
  add x1, x1, #1
  b flags_live
.Lflags_exit:
  mov x0, x2
  ret
.size flags_live, .-flags_live

.globl duplicate
.type duplicate, %function
duplicate:
  ldr x2, [x0, x1, lsl #3]
  ldr x4, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo duplicate
  ret
.size duplicate, .-duplicate

.type guard_prefetch, %function
guard_prefetch:
  stp x19, x30, [sp, #-16]!
  mov x0, #0
  mov x1, #8192
  mov x2, #3
  mov x3, #0x22
  mov x4, #-1
  mov x5, #0
  mov x8, #222 // mmap two pages
  svc #0
  tbnz x0, #63, .Lguard_fail
  mov x19, x0
  add x0, x0, #1, lsl #12
  mov x1, #4096
  mov x2, #0
  mov x8, #226 // mprotect second page with PROT_NONE
  svc #0
  cbnz x0, .Lguard_fail
  add x0, x19, #4088
  mov x1, #1
  bl post_index // D=4 prefetches into the protected page
  b .Lguard_return
.Lguard_fail:
  mov x0, #1
.Lguard_return:
  ldp x19, x30, [sp], #16
  ret
.size guard_prefetch, .-guard_prefetch

.type odd_stride, %function
odd_stride:
  ldrb w2, [x0], #3
  subs x1, x1, #1
  b.ne odd_stride
  mov w0, w2
  ret
.size odd_stride, .-odd_stride

.type reverse_stride, %function
reverse_stride:
  ldrb w2, [x0], #-3
  subs x1, x1, #1
  b.ne reverse_stride
  mov w0, w2
  ret
.size reverse_stride, .-reverse_stride

.type check_strides, %function
check_strides:
  stp x19, x20, [sp, #-32]!
  stp x21, x30, [sp, #16]
  adrp x20, bytes
  add x20, x20, :lo12:bytes
  mov x19, #0
.Lalignment:
  add x0, x20, x19
  ldrb w21, [x0, #45]
  mov x1, #16
  bl odd_stride
  cmp w0, w21
  b.ne .Lstride_fail
  add x0, x20, x19
  add x0, x0, #128
  ldurb w21, [x0, #-45]
  mov x1, #16
  bl reverse_stride
  cmp w0, w21
  b.ne .Lstride_fail
  add x19, x19, #1
  cmp x19, #64
  b.ne .Lalignment
  mov x0, #0
  b .Lstride_exit
.Lstride_fail:
  mov x0, #1
.Lstride_exit:
  ldp x21, x30, [sp, #16]
  ldp x19, x20, [sp], #32
  ret
.size check_strides, .-check_strides

.type conditional_load, %function
conditional_load:
  tbz x5, #0, .Lconditional_skip
  ldr x2, [x0, x1, lsl #3]
.Lconditional_skip:
  add x1, x1, #1
  cmp x1, x3
  b.lo conditional_load
  mov x0, x2
  ret
.size conditional_load, .-conditional_load

.data
.p2align 4
array:
  .quad 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

bytes:
.set byte_index, 0
.rept 512
  .byte byte_index % 251
  .set byte_index, byte_index + 1
.endr

#--- check-dso.py
import ctypes
import sys

library = ctypes.CDLL(sys.argv[1])
array = (ctypes.c_uint64 * 16)(*range(16))
run = library.with_cfi
run.argtypes = [ctypes.POINTER(ctypes.c_uint64)] + [ctypes.c_uint64] * 3
run.restype = ctypes.c_uint64
assert run(array, 0, 0, 16) == 15
