# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64 -mattr=+sve,+sme,+pauth %s -o %t.o
# RUN: ld.lld --build-id=0x11223344 --emit-relocs -e _start %t.o -o %t.exe
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.exe 1 > %t.cycles-1.fdata
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-1.fdata --lite=0 --loop-prefetch-indirect \
# RUN:   --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s
# RUN: llvm-objdump -d --mattr=+sve,+sme,+pauth %t.out | FileCheck %s --check-prefix=ASM

# CHECK-DAG: sve_load pc=unknown {{.*}}unsupported loop context
# CHECK-DAG: sve_store pc=unknown {{.*}}unsupported loop context
# CHECK-DAG: sve_arithmetic pc=unknown {{.*}}unsupported loop context
# CHECK-DAG: sme_matrix pc=unknown {{.*}}unsupported loop context
# CHECK-DAG: authenticated_load pc=unknown {{.*}}unsupported loop context
# CHECK: number of prefetch instructions inserted: 1
# ASM-LABEL: <simd_loop>:
# ASM: prfm
# ASM-LABEL: <sve_load>:
# ASM-NOT: prfm
# ASM-NOT: prfum
# ASM-LABEL: <sve_store>:
# ASM-NOT: prfm
# ASM-NOT: prfum
# ASM-LABEL: <sve_arithmetic>:
# ASM-NOT: prfm
# ASM-LABEL: <sme_matrix>:
# ASM-NOT: prfm
# ASM-LABEL: <authenticated_load>:
# ASM-NOT: prfm

.text
# Retain a code relocation so BOLT can emit the expanded SIMD loop out of line.
.globl _start
.type _start, %function
_start:
  bl simd_loop
  ret
.size _start, .-_start

.macro begin name
.globl \name
.type \name, %function
\name:
  cbz x2, 2f
1:
.endm
.macro end name
  subs x2, x2, #1
  b.ne 1b
2:
  ret
.size \name, .-\name
.endm

begin simd_loop
  ldr q0, [x0], #16
end simd_loop

begin sve_load
  ldr z0, [x3]
  ldr x8, [x0], #8
end sve_load

# This otherwise matches the guarded indirect shape. The store must prevent
# the pass from adding an ordinary speculative index load.
begin sve_store
  str z0, [x3]
  ldr w8, [x1], #4
  ldr x9, [x0, w8, uxtw #3]
end sve_store

begin sve_arithmetic
  add z0.d, z0.d, z1.d
  ldr x8, [x0], #8
end sve_arithmetic

begin sme_matrix
  zero {za}
  ldr x8, [x0], #8
end sme_matrix

begin authenticated_load
  ldraa x8, [x3]
  ldr x9, [x0], #8
end authenticated_load
