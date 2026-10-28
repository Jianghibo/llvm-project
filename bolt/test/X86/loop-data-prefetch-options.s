# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch 2>&1 | FileCheck %s

# CHECK-NOT: number of prefetch instructions inserted

  .text
  .globl _start
  .type _start, @function
_start:
  retq
  .size _start, .-_start
