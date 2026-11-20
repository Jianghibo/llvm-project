# REQUIRES: system-linux
## Static checks only: none of the generated target programs are executed.
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.out --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-slot-stream --data=%t/profile \
# RUN:   --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=slot_stream,slot_nodata %t.out | FileCheck %s --check-prefix=ASM

# Slot-stream prefetch: a non-innermost loop reloads a pointer from a
# frame-relative stack slot each iteration and streams data through it.
# The pass must insert the slot reload plus a distance-ahead prefetch at
# the loop head, and must not touch a loop whose reloaded register never
# feeds a missing data load.

# DIAG: BOLT-PREFETCH: slot_stream pc={{0x[0-9a-f]+}} source=profile slot-stream inserted slot-distance=128 scratch=X0 placement=loop-head kind=slot-stream slot-offset=-32 reload-misses=20 data-misses=30 data-loads=2 loop={{0x[0-9a-f]+}}
# DIAG: BOLT-PREFETCH: slot_nodata pc=unknown source=profile slot-stream rejected no slot data stream loop={{0x[0-9a-f]+}}
# DIAG: BOLT-INFO: number of prefetch instructions inserted: 1

# ASM:      <slot_stream>:
# ASM:      ldur [[REG:x[0-9]+]], [x29, #-32]
# ASM-NEXT: prfm pldl1keep, [[[REG]], #128]
# ASM:      ldur x11, [x29, #-32]
# ASM-NEXT: ldr x0, [x11, #16]
# ASM:      <slot_nodata>:
# ASM-NOT:  prfm

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  bl slot_stream
  bl slot_nodata
  ret
.size _start, .-_start

.globl slot_stream
.type slot_stream, %function
slot_stream:
  stp x29, x30, [sp, #-48]!
  mov x29, sp
  stur x1, [x29, #-32]
  mov x4, #0
.Lslot_outer:
  add x10, x4, x4
  ldur x11, [x29, #-32]
  ldr x0, [x11, #16]
  mov x2, #0
.Lslot_inner:
  ldr x3, [x11, x2]
  add x2, x2, #8
  cmp x2, #64
  b.ne .Lslot_inner
  add x11, x11, #64
  stur x11, [x29, #-32]
  add x4, x4, #1
  cmp x4, #100
  b.ne .Lslot_outer
  ldp x29, x30, [sp], #48
  ret
.size slot_stream, .-slot_stream

.globl slot_nodata
.type slot_nodata, %function
slot_nodata:
  stp x29, x30, [sp, #-48]!
  mov x29, sp
  stur x1, [x29, #-32]
  mov x4, #0
.Lnodata_outer:
  add x10, x4, x4
  ldur x11, [x29, #-32]
  ldr x0, [x10]
  mov x2, #0
.Lnodata_inner:
  ldr x3, [x10, x2]
  add x2, x2, #8
  cmp x2, #64
  b.ne .Lnodata_inner
  add x11, x11, #64
  stur x11, [x29, #-32]
  add x4, x4, #1
  cmp x4, #100
  b.ne .Lnodata_outer
  ldp x29, x30, [sp], #48
  ret
.size slot_nodata, .-slot_nodata

#--- profile
bolt-prefetch 2 l1d_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 slot_stream 0 0 1 0 0
1 slot_stream 40 1 slot_stream 10 0 100 20000 1000
1 slot_stream 2c 1 slot_stream 20 0 2000 40000 2000
7 slot_stream 14 20 500
7 slot_stream 18 30 600
0 [unknown] 0 1 slot_nodata 0 0 1 0 0
1 slot_nodata 40 1 slot_nodata 10 0 100 20000 1000
1 slot_nodata 2c 1 slot_nodata 20 0 2000 40000 2000
7 slot_nodata 14 20 500
