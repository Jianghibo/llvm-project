# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.default --print-loop-data-prefetch 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DISABLED
# RUN: llvm-bolt %t.exe -o %t.enabled --enable-loop-data-prefetch 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ENABLED
# RUN: llvm-bolt %t.exe -o %t.disabled --enable-loop-data-prefetch=false 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DISABLED

# RUN: not llvm-bolt %t.exe -o %t.nl --enable-loop-data-prefetch -nl 2>&1 | \
# RUN:   FileCheck %s --check-prefix=NO-BASIC
# RUN: not llvm-bolt %t.exe -o %t.ba --enable-loop-data-prefetch --ba 2>&1 | \
# RUN:   FileCheck %s --check-prefix=NO-BASIC

# NO-BASIC: loop data prefetch does not support -nl
# DISABLED-NOT: number of prefetch instructions inserted
# ENABLED: BOLT-INFO: number of prefetch instructions inserted:

  .text
  .globl _start
  .type _start, %function
_start:
  ret
  .size _start, .-_start
