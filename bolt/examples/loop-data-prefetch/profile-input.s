// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// All three functions sum unsigned 64-bit values. The verification script
// assembles and rewrites this program, but never executes it.
.text
.globl _start
.type _start, %function
_start:
  adrp x0, values
  add x0, x0, :lo12:values
  mov x2, #16
  bl direct
  cmp x0, #136
  b.ne .Lfail
  adrp x0, indices
  add x0, x0, :lo12:indices
  adrp x1, values
  add x1, x1, :lo12:values
  mov x2, #16
  bl gather
  cmp x0, #136
  b.ne .Lfail
  adrp x0, matrix
  add x0, x0, :lo12:matrix
  mov x2, #16
  bl batch
  cmp x0, #2080
  b.ne .Lfail
  mov x0, #0
  b .Lexit
.Lfail:
  mov x0, #1
.Lexit:
  mov x8, #93
  svc #0
.size _start, .-_start

// sum(values[0..count)): one 8-byte step per natural iteration.
.globl direct
.type direct, %function
direct:
  .cfi_startproc
  mov x1, #0
  cbz x2, .Ldirect_exit
.Ldirect_loop:
  ldr x3, [x0], #8
  add x1, x1, x3
  subs x2, x2, #1
  b.ne .Ldirect_loop
.Ldirect_exit:
  mov x0, x1
  ret
  .cfi_endproc
.size direct, .-direct

// sum(values[indices[i]]): ordinary uint32_t indices and uint64_t data.
.globl gather
.type gather, %function
gather:
  .cfi_startproc
  mov x5, #0
  cbz x2, .Lgather_exit
.Lgather_loop:
  ldr w3, [x0], #4
  ldr x4, [x1, w3, uxtw #3]
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lgather_loop
.Lgather_exit:
  mov x0, x5
  ret
  .cfi_endproc
.size gather, .-gather

// sum(matrix[row][column]): four elements per outer iteration.
.globl batch
.type batch, %function
batch:
  .cfi_startproc
  mov x1, #0
  cbz x2, .Lbatch_exit
.Lbatch_outer:
  mov w3, #4
.Lbatch_inner:
  ldr x4, [x0], #8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Lbatch_inner
  subs x2, x2, #1
  b.ne .Lbatch_outer
.Lbatch_exit:
  mov x0, x1
  ret
  .cfi_endproc
.size batch, .-batch

.data
.p2align 6
values:
  .quad 1, 2, 3, 4, 5, 6, 7, 8
  .quad 9, 10, 11, 12, 13, 14, 15, 16
indices:
  .word 15, 14, 13, 12, 11, 10, 9, 8
  .word 7, 6, 5, 4, 3, 2, 1, 0
.p2align 6
matrix:
  .quad 1, 2, 3, 4, 5, 6, 7, 8
  .quad 9, 10, 11, 12, 13, 14, 15, 16
  .quad 17, 18, 19, 20, 21, 22, 23, 24
  .quad 25, 26, 27, 28, 29, 30, 31, 32
  .quad 33, 34, 35, 36, 37, 38, 39, 40
  .quad 41, 42, 43, 44, 45, 46, 47, 48
  .quad 49, 50, 51, 52, 53, 54, 55, 56
  .quad 57, 58, 59, 60, 61, 62, 63, 64
.section .note.GNU-stack,"",%progbits
