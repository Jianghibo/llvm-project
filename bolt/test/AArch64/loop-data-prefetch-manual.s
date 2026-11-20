# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 --image-base=0x10000 -Ttext=0x11000 -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --loop-prefetch-diagnostics --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %t.exe && %t.out %}
# RUN: llvm-bolt %t.exe -o %t.hints --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=0 --loop-prefetch-hints=%t/hints --loop-prefetch-diagnostics \
# RUN:   --lite=1 2>&1 | FileCheck %s --check-prefix=HINTS
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=large %t.hints | FileCheck %s --check-prefix=LARGE
# RUN: %if target=aarch64{{.*}} %{ %t.hints %}
# RUN: llvm-bolt %t.exe -o %t.budget --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --loop-prefetch-max-instructions=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.zero --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --loop-prefetch-max-per-loop=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: llvm-bolt %t.exe -o %t.limit --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --loop-prefetch-analysis-limit=0 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ZERO
# RUN: ld.lld -pie --no-dynamic-linker --emit-relocs -e _start %t.o -o %t.pie
# RUN: llvm-bolt %t.pie -o %t.pie.out --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --verify-cfg
# RUN: %if target=aarch64{{.*}} %{ %t.pie && %t.pie.out %}
# RUN: ld.lld -shared -Bsymbolic --emit-relocs %t.o -o %t.so
# RUN: llvm-bolt %t.so -o %t.so.out --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-distance=4 --verify-cfg
# RUN: llvm-objdump -d --no-print-imm-hex %t.so.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %python %t/check-dso.py %t.so.out %}
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-cache-line-size=48 2>&1 | FileCheck %s --check-prefix=BAD-LINE
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-cache-line-size=64 2>&1 | FileCheck %s --check-prefix=LINE-BATCH
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-hints=%t/bad-id 2>&1 | FileCheck %s --check-prefix=BAD-ID
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-hints=%t/bad-pc 2>&1 | FileCheck %s --check-prefix=BAD-PC
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-hint=invalid 2>&1 | FileCheck %s --check-prefix=BAD-OPTION

# DIAG-DAG: BOLT-PREFETCH: with_cfi {{.*}} source=command line distance=4 {{.*}} inserted
# DIAG-DAG: BOLT-PREFETCH: multiblock {{.*}} inserted
# DIAG-DAG: BOLT-PREFETCH: conditional_update {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: pressure {{.*}} insufficient dead registers
# DIAG-DAG: BOLT-PREFETCH: mixed_width {{.*}} unproven recurrence
# DIAG-DAG: BOLT-PREFETCH: opaque_cfi {{.*}} opaque CFI expression
# DIAG-DAG: BOLT-PREFETCH: atomic_loop {{.*}} unsupported loop context
# DIAG-DAG: BOLT-PREFETCH: duplicate {{.*}} duplicate address
# HINTS-DAG: BOLT-PREFETCH: with_cfi pc=0x1110c source=pc hint distance=4 hint=pldl2strm budget=8 inserted
# HINTS-DAG: BOLT-PREFETCH: post_index pc=0x11200 source=function hint distance=0 {{.*}} disabled
# HINTS-DAG: BOLT-PREFETCH: large {{.*}} distance=65536 {{.*}} inserted
# ZERO: number of prefetch instructions inserted: 0
# BAD-ID: prefetch hints build-id does not match input
# BAD-PC: invalid prefetch hints at line 2
# BAD-OPTION: invalid loop prefetch hint, distance, cutoff or register budget

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
# LARGE:      mov x{{[0-9]+}}, #2147483648
# LARGE:      add x{{[0-9]+}}, x0, x{{[0-9]+}}
# LARGE:      prfm pldl3strm

# BAD-LINE: cache line size must be 0 or a power of two
# LINE-BATCH: loop-prefetch-cache-line-size requires --loop-prefetch-batch

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

.org 0x100
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

.org 0x200
.globl post_index
.type post_index, %function
post_index:
  ldr x2, [x0], #8
  subs x1, x1, #1
  b.ne post_index
  mov x0, x2
  ret
.size post_index, .-post_index

.org 0x300
.globl pre_index
.type pre_index, %function
pre_index:
  ldp x2, x3, [x0, #16]!
  subs x1, x1, #1
  b.ne pre_index
  ret
.size pre_index, .-pre_index

.org 0x400
.globl signed_index
.type signed_index, %function
signed_index:
  ldr q2, [x0, w1, sxtw #4]
  sub w1, w1, #2
  cmp w1, w4
  b.ne signed_index
  ret
.size signed_index, .-signed_index

.org 0x500
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

.org 0x600
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

.org 0x700
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

.org 0x800
.globl mixed_width
.type mixed_width, %function
mixed_width:
  ldr x2, [x0, x1, lsl #3]
  add w1, w1, #1
  cmp w1, w3
  b.lo mixed_width
  ret
.size mixed_width, .-mixed_width

.org 0x900
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

.org 0xa00
.globl large
.type large, %function
large:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1, lsl #12
  cmp x1, x3
  b.lo large
  ret
.size large, .-large

.org 0xb00
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

.org 0xc00
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

.org 0xd00
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

.org 0xe00
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

.org 0xf00
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

.org 0x1000
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

.org 0x1100
.type odd_stride, %function
odd_stride:
  ldrb w2, [x0], #3
  subs x1, x1, #1
  b.ne odd_stride
  mov w0, w2
  ret
.size odd_stride, .-odd_stride

.org 0x1200
.type reverse_stride, %function
reverse_stride:
  ldrb w2, [x0], #-3
  subs x1, x1, #1
  b.ne reverse_stride
  mov w0, w2
  ret
.size reverse_stride, .-reverse_stride

.org 0x1300
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

.org 0x1400
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

#--- hints
bolt-prefetch-hints 1 11223344
function 11100 0 pldl1keep 8
pc 1110c LDRXroW 4 pldl2strm 8
function 11200 0 pldl1keep 8
pc 11a00 LDRXroX 65536 pldl3strm 12

#--- bad-id
bolt-prefetch-hints 1 11223345

#--- bad-pc
bolt-prefetch-hints 1 11223344
pc 1110c LDRXroX 4 pldl1keep 8

#--- check-dso.py
import ctypes
import sys

library = ctypes.CDLL(sys.argv[1])
array = (ctypes.c_uint64 * 16)(*range(16))
run = library.with_cfi
run.argtypes = [ctypes.POINTER(ctypes.c_uint64)] + [ctypes.c_uint64] * 3
run.restype = ctypes.c_uint64
assert run(array, 0, 0, 16) == 15
