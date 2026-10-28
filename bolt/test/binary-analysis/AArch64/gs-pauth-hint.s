// RUN: llvm-mc -triple=aarch64 -filetype=obj %s -o %t.o
// RUN: ld.lld -e _start %t.o -o %t.exe
// RUN: llvm-bolt-binary-analysis --scanners=ptrauth-all %t.exe

// HINT has mayLoad in its descriptor, but its immediate operand must not be
// interpreted as a base register by the pointer authentication analyses.
        .text
        .globl _start
        .type _start, %function
_start:
        hint #1
        ret
        .size _start, .-_start
