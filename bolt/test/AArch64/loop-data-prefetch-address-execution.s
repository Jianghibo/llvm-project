# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -triple=aarch64 -filetype=obj %t/input.s -o %t.o
# RUN: ld.lld -shared -Bsymbolic --build-id=0x11223344 --emit-relocs --eh-frame-hdr %t.o -o %t.so
# RUN: llvm-bolt %t.so -o %t.base.so --verify-cfg
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 1 > %t.cycles-1.fdata
# RUN: llvm-bolt %t.so -o %t.d1.so --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-1.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.d1.so | FileCheck %s
# RUN: llvm-bolt %t.so -o %t.throttle.so --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-1.fdata --lite=0 --loop-prefetch-cache-line-size=64 \
# RUN:   --loop-prefetch-max-instructions=16 --verify-cfg
# RUN: llvm-objdump -d %t.throttle.so | FileCheck %s --check-prefix=THROTTLE
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 16 > %t.cycles-16.fdata
# RUN: llvm-bolt %t.so -o %t.d16.so --enable-loop-data-prefetch \
# RUN:   --data=%t.cycles-16.fdata --lite=0 --loop-prefetch-diagnostics --verify-cfg \
# RUN:   2>&1 | FileCheck %s --check-prefix=FAR
# RUN: %if target=aarch64{{.*}} %{ %python %t/check.py %t.so %t.base.so \
# RUN:   %t.d1.so %t.throttle.so %t.d16.so %}

# DIAG-DAG: BOLT-PREFETCH: slice {{.*}} inserted {{.*}} kind=affine
# DIAG-DAG: BOLT-PREFETCH: slice_blocks {{.*}} inserted {{.*}} kind=affine
# DIAG-DAG: BOLT-PREFETCH: pointer {{.*}} inserted {{.*}} kind=pointer
# DIAG-DAG: BOLT-PREFETCH: pointer_move {{.*}} inserted {{.*}} kind=pointer
# FAR-DAG: BOLT-PREFETCH: pointer {{.*}} pointer chase requires distance 1
# FAR-DAG: BOLT-PREFETCH: pointer_move {{.*}} pointer chase requires distance 1

# CHECK-LABEL: <slice>:
# CHECK: prfm
# CHECK: ldr x4, [x7]
# CHECK-LABEL: <slice_blocks>:
# CHECK: prfm
# CHECK: ldr x4, [x8]
# CHECK-LABEL: <pointer>:
# CHECK:      ldr x0, [x0, #8]
# CHECK-NEXT: prfm pldl1keep, [x0, #8]
# CHECK-LABEL: <pointer_move>:
# CHECK:      ldr x3, [x19, #8]
# CHECK-NEXT: prfm pldl1keep, [x3, #8]

# THROTTLE-LABEL: <slice>:
# THROTTLE: cbnz
# THROTTLE: prfm
# THROTTLE: ldr
# THROTTLE-LABEL: <slice_blocks>:
# THROTTLE: cbnz
# THROTTLE: prfm
# THROTTLE: ldr

#--- input.s
.text
.globl slice_entry
.type slice_entry, %function
slice_entry:
  b slice
.size slice_entry, .-slice_entry

.globl slice
.type slice, %function
slice:
  mov x1, #0
  mov x3, #0
  cbz x2, .Ldone_slice
.Lloop_slice:
  add x7, x0, x3, lsl #3
  cmp x1, #0
  ldr x4, [x7]
  csel x4, x4, xzr, ge
  add x1, x1, x4
  add x3, x3, #1
  subs x2, x2, #1
  b.ne .Lloop_slice
.Ldone_slice:
  mov x0, x1
  ret
.size slice, .-slice

.globl slice_blocks
.type slice_blocks, %function
slice_blocks:
  mov x1, #0
  mov x3, #0
  cbz x2, .Ldone_blocks
.Lloop_blocks:
  lsl x6, x3, #3
  b .Ladd_blocks
.Ladd_blocks:
  add x7, x0, x6
  b .Lload_blocks
.Lload_blocks:
  mov x8, x7
  ldr x4, [x8]
  add x1, x1, x4
  add x3, x3, #1
  subs x2, x2, #1
  b.ne .Lloop_blocks
.Ldone_blocks:
  mov x0, x1
  ret
.size slice_blocks, .-slice_blocks

.globl pointer
.type pointer, %function
pointer:
  mov x1, #0
  cbz x0, .Ldone_pointer
.Lloop_pointer:
  ldr x0, [x0, #8]
  add x1, x1, #1
  cbnz x0, .Lloop_pointer
.Ldone_pointer:
  mov x0, x1
  ret
.size pointer, .-pointer

.globl pointer_move
.type pointer_move, %function
pointer_move:
  .cfi_startproc
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  mov x19, x0
  mov x1, #0
  cbz x19, .Ldone_move
.Lloop_move:
  ldr x3, [x19, #8]
  add x1, x1, #1
  b .Lupdate_move
.Lupdate_move:
  mov x19, x3
  cbnz x19, .Lloop_move
.Ldone_move:
  mov x0, x1
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
  .cfi_endproc
.size pointer_move, .-pointer_move

#--- check.py
import ctypes
import mmap
import sys

page = mmap.PAGESIZE
mapping = mmap.mmap(-1, 2 * page)
base = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
if libc.mprotect(base + page, page, 0):
    raise OSError(ctypes.get_errno(), "mprotect failed")


class Node(ctypes.Structure):
    _fields_ = [("value", ctypes.c_uint64), ("next", ctypes.c_void_p)]


for path in sys.argv[1:]:
    library = ctypes.CDLL(path)
    for name in ["slice", "slice_blocks"]:
        function = getattr(library, name)
        function.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
        function.restype = ctypes.c_uint64
        for count in [0, 1, 2, 5, 33]:
            for alignment in [0, 8, 24]:
                address = base + page - count * 8 - alignment
                data = (ctypes.c_uint64 * count).from_address(address)
                data[:] = range(1, count + 1)
                assert function(address, 0, count) == count * (count + 1) // 2
    for name in ["pointer", "pointer_move"]:
        function = getattr(library, name)
        function.argtypes = [ctypes.c_void_p]
        function.restype = ctypes.c_uint64
        for count in [0, 1, 2, 5, 33]:
            address = base + page - count * ctypes.sizeof(Node)
            nodes = (Node * count).from_address(address)
            for order in [list(range(count)), list(reversed(range(count)))]:
                for i, index in enumerate(order):
                    nodes[index].next = (ctypes.addressof(nodes[order[i + 1]])
                                         if i + 1 < count else None)
                root = ctypes.addressof(nodes[order[0]]) if count else None
                assert function(root) == count, (path, name, count, order)
