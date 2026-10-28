// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// uint64_t sum_u64(const uint64_t *data, uint64_t count)
// Each iteration advances data by 8 bytes. D=8 should insert a prefetch
// for [x0, #64] immediately before the original post-indexed load.
  .text
  .p2align 2
  .globl sum_u64
  .type sum_u64, %function
sum_u64:
  mov x2, #0
  cbz x1, .Ldone
.Lloop:
  ldr x3, [x0], #8
  add x2, x2, x3
  subs x1, x1, #1
  b.ne .Lloop
.Ldone:
  mov x0, x2
  ret
  .size sum_u64, .-sum_u64

// Linux entry point: sum 1..16 and return 0 if the result is 136.
  .globl _start
  .type _start, %function
_start:
  adrp x0, data
  add x0, x0, :lo12:data
  mov x1, #16
  bl sum_u64
  cmp x0, #136
  cset w0, ne
  mov x8, #93
  svc #0
  .size _start, .-_start

  .data
  .p2align 6
data:
  .quad 1, 2, 3, 4, 5, 6, 7, 8
  .quad 9, 10, 11, 12, 13, 14, 15, 16

  .section .note.GNU-stack,"",%progbits
