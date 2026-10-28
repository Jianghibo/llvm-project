# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64 %t/input.s -o %t.o
# RUN: ld.lld -shared -Bsymbolic --build-id=0x11223344 --emit-relocs %t.o -o %t.so
# RUN: llvm-bolt %t.so -o %t.baseline.so --verify-cfg
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 1 > %t.cycles-1.fdata
# RUN: llvm-bolt %t.so -o %t.d1.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-indirect --data=%t.cycles-1.fdata --lite=0 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s
# RUN: llvm-objdump -d %t.d1.so | FileCheck %s --check-prefix=CODE
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 4 > %t.cycles-4.fdata
# RUN: llvm-bolt %t.so -o %t.d4.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-indirect --data=%t.cycles-4.fdata --lite=0 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s
# RUN: llvm-objdump -d %t.d4.so | FileCheck %s --check-prefix=CODE
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 16 > %t.cycles-16.fdata
# RUN: llvm-bolt %t.so -o %t.d16.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-indirect --data=%t.cycles-16.fdata --lite=0 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s
# RUN: llvm-objdump -d %t.d16.so | FileCheck %s --check-prefix=CODE
# RUN: %if target=aarch64{{.*}} %{ %python %t/check.py %t.so %t.baseline.so \
# RUN:   %t.d1.so %t.d4.so %t.d16.so %}

## Each index buffer borders a protected page. Counts around D exercise the
## guard threshold and the last D iterations. The zero-trip buffer is itself
## inaccessible. Signed indices address both sides of the target base.

# CHECK-DAG: BOLT-PREFETCH: gather_uxtw {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_sxtw {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather64 {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_indexed {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_pre {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_reverse {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_blocks {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: gather_blocks64 {{.*}} inserted {{.*}} kind=indirect
# CHECK-DAG: BOLT-PREFETCH: changed_address {{.*}} unproven recurrence
# CHECK-DAG: BOLT-PREFETCH: extra_update {{.*}} unproven recurrence
# CHECK-DAG: BOLT-PREFETCH: early_exit {{.*}} unproven recurrence

# CODE-LABEL: <gather_uxtw>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_sxtw>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather64>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_indexed>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_pre>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_reverse>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_blocks>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm
# CODE-LABEL: <gather_blocks64>:
# CODE: tb{{n?}}z
# CODE: ld{{u?}}r
# CODE: prfm

#--- input.s
.text
## Keep a text relocation so BOLT can emit the growing functions.
.globl gather_entry
.type gather_entry, %function
gather_entry:
  b gather_uxtw
.size gather_entry, .-gather_entry

.globl gather_uxtw
.type gather_uxtw, %function
gather_uxtw:
  mov x5, #0
  cbz w2, .Ldone_uxtw
.Lloop_uxtw:
  ldr w3, [x0], #4
  cmp x5, #0
  ldr x4, [x1, w3, uxtw #3]
  csel x4, x4, xzr, ge
  add x5, x5, x4
  subs w2, w2, #1
  b.ne .Lloop_uxtw
.Ldone_uxtw:
  mov x0, x5
  ret
.size gather_uxtw, .-gather_uxtw

.globl gather_sxtw
.type gather_sxtw, %function
gather_sxtw:
  mov x5, #0
  cbz x2, .Ldone_sxtw
.Lloop_sxtw:
  ldr w3, [x0], #4
  cmp x5, #0
  ldr x4, [x1, w3, sxtw #3]
  csel x4, x4, xzr, ge
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lloop_sxtw
.Ldone_sxtw:
  mov x0, x5
  ret
.size gather_sxtw, .-gather_sxtw

.globl gather64
.type gather64, %function
gather64:
  mov x5, #0
  cbz x2, .Ldone64
.Lloop64:
  ldr x3, [x0], #8
  cmp x5, #0
  ldr x4, [x1, x3, lsl #3]
  csel x4, x4, xzr, ge
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lloop64
.Ldone64:
  mov x0, x5
  ret
.size gather64, .-gather64

.globl gather_indexed
.type gather_indexed, %function
gather_indexed:
  mov x5, #0
  mov x6, #0
  cbz x2, .Ldone_indexed
.Lloop_indexed:
  ldr w3, [x0, x6, lsl #2]
  cmp x5, #0
  ldr x4, [x1, w3, uxtw #3]
  csel x4, x4, xzr, ge
  add x5, x5, x4
  add x6, x6, #1
  subs x2, x2, #1
  b.ne .Lloop_indexed
.Ldone_indexed:
  mov x0, x5
  ret
.size gather_indexed, .-gather_indexed

.globl gather_pre
.type gather_pre, %function
gather_pre:
  mov x5, #0
  cbz x2, .Ldone_pre
.Lloop_pre:
  ldr w3, [x0, #4]!
  ldr x4, [x1, w3, uxtw #3]
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lloop_pre
.Ldone_pre:
  mov x0, x5
  ret
.size gather_pre, .-gather_pre

.globl gather_reverse
.type gather_reverse, %function
gather_reverse:
  mov x5, #0
  cbz x2, .Ldone_reverse
.Lloop_reverse:
  ldr w3, [x0], #-4
  ldr x4, [x1, w3, uxtw #3]
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lloop_reverse
.Ldone_reverse:
  mov x0, x5
  ret
.size gather_reverse, .-gather_reverse

.globl gather_blocks
.type gather_blocks, %function
gather_blocks:
  mov x5, #0
  cbz x2, .Ldone_blocks
.Lloop_blocks:
  ldr w3, [x0], #4
  b .Lflags_blocks
.Lflags_blocks:
  cmp x5, #0
  b .Lconsume_blocks
.Lconsume_blocks:
  ldr x4, [x1, w3, uxtw #3]
  csel x4, x4, xzr, ge
  add x5, x5, x4
  subs x2, x2, #1
  b.ne .Lloop_blocks
.Ldone_blocks:
  mov x0, x5
  ret
.size gather_blocks, .-gather_blocks

.globl gather_blocks64
.type gather_blocks64, %function
gather_blocks64:
  mov x5, #0
  cbz w2, .Ldone_blocks64
.Lloop_blocks64:
  ldr x3, [x0], #8
  b .Lconsume_blocks64
.Lconsume_blocks64:
  ldr x4, [x1, x3, lsl #3]
  add x5, x5, x4
  subs w2, w2, #1
  b.ne .Lloop_blocks64
.Ldone_blocks64:
  mov x0, x5
  ret
.size gather_blocks64, .-gather_blocks64

## The remaining-count guard cannot justify address changes between blocks.
.globl changed_address
.type changed_address, %function
changed_address:
  cbz x2, .Ldone_changed
.Lloop_changed:
  ldr w3, [x0], #4
  b .Lconsume_changed
.Lconsume_changed:
  add x0, x0, #4
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne .Lloop_changed
.Ldone_changed:
  ret
.size changed_address, .-changed_address

.globl extra_update
.type extra_update, %function
extra_update:
  cbz x2, .Ldone_extra
.Lloop_extra:
  ldr w3, [x0], #4
  add x2, x2, #1
  b .Lconsume_extra
.Lconsume_extra:
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne .Lloop_extra
.Ldone_extra:
  ret
.size extra_update, .-extra_update

.globl early_exit
.type early_exit, %function
early_exit:
  cbz x2, .Ldone_early
.Lloop_early:
  ldr w3, [x0], #4
  cbz x7, .Ldone_early
.Lconsume_early:
  ldr x4, [x1, w3, uxtw #3]
  subs x2, x2, #1
  b.ne .Lloop_early
.Ldone_early:
  ret
.size early_exit, .-early_exit

#--- check.py
import ctypes
import mmap
import sys

page = mmap.PAGESIZE
libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.mprotect.restype = ctypes.c_int


class GuardedBuffer:
    def __init__(self):
        self.mapping = mmap.mmap(-1, 3 * page)
        start = ctypes.addressof(ctypes.c_char.from_buffer(self.mapping))
        self.address = start + page
        for guard in [start, start + 2 * page]:
            if libc.mprotect(guard, page, 0):
                raise OSError(ctypes.get_errno(), "mprotect failed")


source = GuardedBuffer()
target = GuardedBuffer()
values = [11 + 3 * i for i in range(16)]
data = (ctypes.c_uint64 * len(values)).from_address(target.address)
data[:] = values

for path in sys.argv[1:]:
    library = ctypes.CDLL(path)
    for name, element, signed in [
        ("gather_entry", ctypes.c_uint32, False),
        ("gather_sxtw", ctypes.c_int32, True),
        ("gather64", ctypes.c_uint64, False),
        ("gather_indexed", ctypes.c_uint32, False),
        ("gather_pre", ctypes.c_uint32, False),
        ("gather_reverse", ctypes.c_uint32, False),
        ("gather_blocks", ctypes.c_uint32, False),
        ("gather_blocks64", ctypes.c_uint64, False),
    ]:
        function = getattr(library, name)
        function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64]
        function.restype = ctypes.c_uint64
        for count in [0, 1, 2, 3, 4, 5, 15, 16, 17, 33]:
            indices = [i % 16 - (8 if signed else 0) for i in range(count)]
            size = ctypes.sizeof(element)
            if name == "gather_reverse":
                buffer = (element * count).from_address(source.address)
                buffer[:] = indices[::-1]
                address = source.address + (count - 1) * size
            else:
                address = source.address + page - count * size
                (element * count).from_address(address)[:] = indices
                if name == "gather_pre":
                    address -= size
            base = target.address + (64 if signed else 0)
            expected = sum(values[i + (8 if signed else 0)] for i in indices)
            actual = function(address, base, count)
            assert actual == expected, (path, name, count, actual, expected)

    # A one-element index buffer must not be read ahead, even if its next
    # entry would contain an inaccessible data address.
    indices = (ctypes.c_uint32 * 2).from_address(source.address)
    indices[:] = [0, page // 8]
    assert library.gather_entry(source.address, target.address, 1) == values[0]
