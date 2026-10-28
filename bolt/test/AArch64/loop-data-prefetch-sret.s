# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple aarch64 %s -o %t.o
# RUN: ld.lld --build-id=0x11223344 --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.baseline --verify-cfg
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 4 > %t.cycles-4.fdata
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-4.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --disassemble-symbols=caller %t.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %t.exe && %t.baseline && %t.out %}

## The loop must preserve X8 for the following indirect-result call, even
## though the caller has no explicit use of X8. A CFG check cannot detect
## clobbering this implicit argument.

# DIAG: BOLT-PREFETCH: caller {{.*}} inserted
# CHECK-LABEL: <caller>:
# CHECK: prfm
# CHECK: bl {{.*}} <return_object>

.text
.globl _start
.type _start, %function
_start:
  adrp x0, array
  add x0, x0, :lo12:array
  mov x1, #0
  mov x2, #4
  adrp x8, result
  add x8, x8, :lo12:result
  bl caller
  adrp x1, result
  add x1, x1, :lo12:result
  ldp x2, x3, [x1]
  ldr x4, [x1, #16]
  add x2, x2, x3
  add x2, x2, x4
  cmp x2, #129
  cset w0, ne
  mov x8, #93
  svc #0
.size _start, .-_start

.globl caller
.type caller, %function
caller:
  .cfi_startproc
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x29, -16
  .cfi_offset x30, -8
.Lloop:
  ldr x9, [x0, x1, lsl #3]
  add x1, x1, #1
  subs x2, x2, #1
  b.ne .Lloop
  bl return_object
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x29
  .cfi_restore x30
  ret
  .cfi_endproc
.size caller, .-caller

.globl return_object
.type return_object, %function
return_object:
  mov x9, #42
  mov x10, #43
  mov x11, #44
  stp x9, x10, [x8]
  str x11, [x8, #16]
  ret
.size return_object, .-return_object

.section .rodata
.p2align 6
array:
  .quad 1, 2, 3, 4, 5, 6, 7, 8
.data
.p2align 3
result:
  .quad 0, 0, 0
.section .note.GNU-stack,"",%progbits
