# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -triple=aarch64 -filetype=obj %t/input.s -o %t.o
# RUN: ld.lld -shared -Bsymbolic --build-id=0x11223344 --emit-relocs --eh-frame-hdr %t.o -o %t.so
# RUN: llvm-bolt %t.so -o %t.base.so --verify-cfg
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 1 > %t.cycles-1.fdata
# RUN: llvm-bolt %t.so -o %t.d1.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-batch --data=%t.cycles-1.fdata --lite=0 \
# RUN:   --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=4 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.d1.so > %t.d1.asm
# RUN: FileCheck %s --check-prefix=CODE --input-file=%t.d1.asm
# RUN: FileCheck %s --check-prefix=COPY --input-file=%t.d1.asm
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 4 > %t.cycles-4.fdata
# RUN: llvm-bolt %t.so -o %t.d4.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-batch --data=%t.cycles-4.fdata --lite=0 \
# RUN:   --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=4 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.d4.so > %t.d4.asm
# RUN: FileCheck %s --check-prefix=CODE --input-file=%t.d4.asm
# RUN: %python %S/../Inputs/loop-prefetch-profile.py %t.so 16 > %t.cycles-16.fdata
# RUN: llvm-bolt %t.so -o %t.d16.so --enable-loop-data-prefetch \
# RUN:   --loop-prefetch-batch --data=%t.cycles-16.fdata --lite=0 \
# RUN:   --loop-prefetch-cache-line-size=64 --loop-prefetch-max-per-loop=4 \
# RUN:   --loop-prefetch-max-instructions=16 --loop-prefetch-diagnostics \
# RUN:   --verify-cfg 2>&1 | FileCheck %s --check-prefix=DIAG
# RUN: llvm-objdump -d --no-print-imm-hex %t.d16.so > %t.d16.asm
# RUN: FileCheck %s --check-prefix=CODE --input-file=%t.d16.asm
# RUN: %if target=aarch64{{.*}} %{ %python %t/check.py %t.so %t.base.so \
# RUN:   %t.d1.so %t.d4.so %t.d16.so %}
# RUN: %if target=aarch64{{.*}} %{ %clang %S/../Inputs/prefetch-unwind.c -O1 -rdynamic \
# RUN:   -fomit-frame-pointer -fasynchronous-unwind-tables -ldl -o %t.unwind %}
# RUN: %if target=aarch64{{.*}} %{ %python %t/check-unwind.py %t.unwind \
# RUN:   %t.d1.so %t.d1.asm %t.d4.so %t.d4.asm %t.d16.so %t.d16.asm %}

# DIAG-DAG: BOLT-PREFETCH: batch_forward {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_reverse {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_pair {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_prefix {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_copy {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_copy_reverse {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_unwind {{.*}} inserted {{.*}} kind=batch
# DIAG-DAG: BOLT-PREFETCH: batch_partial {{.*}} unsupported batch address
# DIAG-DAG: BOLT-PREFETCH: batch_loaded {{.*}} unsupported batch address

# CODE-LABEL: <batch_forward>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_reverse>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_pair>:
# CODE: cbz
# CODE: prf
# CODE: ldp
# CODE-LABEL: <batch_prefix>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_copy>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_copy_reverse>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_unwind>:
# CODE: cbz
# CODE: prf
# CODE: ldr
# CODE-LABEL: <batch_partial>:
# CODE-NOT: prf
# CODE-LABEL: <batch_loaded>:
# CODE-NOT: prf

# COPY-LABEL: <batch_copy>:
# COPY:      mov x6, x0
# COPY-NEXT: mov w3, #4
# COPY-NEXT: prfm pldl1keep, [x6, #64]
# COPY-NEXT: prfum pldl1keep, [x6, #95]
# COPY-NEXT: ldr x4, [x6], #8
# COPY-LABEL: <batch_copy_reverse>:
# COPY:      mov x6, x0
# COPY-NEXT: mov w3, #4
# COPY-NEXT: prfum pldl1keep, [x6, #-88]
# COPY-NEXT: prfum pldl1keep, [x6, #-57]
# COPY-NEXT: ldr x4, [x6], #-8

#--- input.s
.text
.globl batch_entry
.type batch_entry, %function
batch_entry:
  b batch_forward
.size batch_entry, .-batch_entry

.globl batch_forward
.type batch_forward, %function
batch_forward:
  mov x1, #0
  cbz x2, .Ldone_forward
.Louter_forward:
  mov w3, #4
.Linner_forward:
  ldr x4, [x0], #8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_forward
  subs x2, x2, #1
  b.ne .Louter_forward
.Ldone_forward:
  mov x0, x1
  ret
.size batch_forward, .-batch_forward

.globl batch_reverse
.type batch_reverse, %function
batch_reverse:
  mov x1, #0
  cbz x2, .Ldone_reverse
.Louter_reverse:
  mov w3, #4
.Linner_reverse:
  ldr x4, [x0], #-8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_reverse
  subs x2, x2, #1
  b.ne .Louter_reverse
.Ldone_reverse:
  mov x0, x1
  ret
.size batch_reverse, .-batch_reverse

.globl batch_pair
.type batch_pair, %function
batch_pair:
  mov x1, #0
  cbz x2, .Ldone_pair
.Louter_pair:
  mov x3, #4
.Linner_pair:
  ldp x4, x5, [x0], #16
  add x1, x1, x4
  add x1, x1, x5
  subs x3, x3, #1
  b.ne .Linner_pair
  subs x2, x2, #1
  b.ne .Louter_pair
.Ldone_pair:
  mov x0, x1
  ret
.size batch_pair, .-batch_pair

.globl batch_prefix
.type batch_prefix, %function
batch_prefix:
  mov x1, #0
  cbz x2, .Ldone_prefix
.Louter_prefix:
  mov w3, #4
.Linner_prefix:
  add x0, x0, #8
  ldr x4, [x0]
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_prefix
  subs x2, x2, #1
  b.ne .Louter_prefix
.Ldone_prefix:
  mov x0, x1
  ret
.size batch_prefix, .-batch_prefix

.globl batch_copy
.type batch_copy, %function
batch_copy:
  mov x1, #0
  cbz x2, .Ldone_copy
.Louter_copy:
  mov x6, x0
  mov w3, #4
.Linner_copy:
  ldr x4, [x6], #8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_copy
  add x0, x0, #64
  subs x2, x2, #1
  b.ne .Louter_copy
.Ldone_copy:
  mov x0, x1
  ret
.size batch_copy, .-batch_copy

.globl batch_copy_reverse
.type batch_copy_reverse, %function
batch_copy_reverse:
  mov x1, #0
  cbz x2, .Ldone_copy_reverse
.Louter_copy_reverse:
  mov x6, x0
  mov w3, #4
.Linner_copy_reverse:
  ldr x4, [x6], #-8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_copy_reverse
  sub x0, x0, #64
  subs x2, x2, #1
  b.ne .Louter_copy_reverse
.Ldone_copy_reverse:
  mov x0, x1
  ret
.size batch_copy_reverse, .-batch_copy_reverse

## LR is deliberately cleared after saving it. Unwinding at a new prefetch
## PC must use the CFI save rule rather than the current LR or a frame chain.
.globl batch_unwind
.type batch_unwind, %function
batch_unwind:
  .cfi_startproc
  stp x19, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x19, -16
  .cfi_offset x30, -8
  mov x19, x0
  mov x30, #0
  mov x1, #0
  cbz x2, .Ldone_unwind
.Louter_unwind:
  mov w3, #4
.Linner_unwind:
  ldr x4, [x19], #8
  add x1, x1, x4
  subs w3, w3, #1
  b.ne .Linner_unwind
  subs x2, x2, #1
  b.ne .Louter_unwind
.Ldone_unwind:
  mov x0, x1
  ldp x19, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x30
  ret
  .cfi_endproc
.size batch_unwind, .-batch_unwind

.globl batch_partial
.type batch_partial, %function
batch_partial:
  cbz x2, .Ldone_partial
.Louter_partial:
  mov x6, x0
  mov w6, w0
  mov w3, #4
.Linner_partial:
  ldr x4, [x6], #8
  subs w3, w3, #1
  b.ne .Linner_partial
  add x0, x0, #64
  subs x2, x2, #1
  b.ne .Louter_partial
.Ldone_partial:
  ret
.size batch_partial, .-batch_partial

.globl batch_loaded
.type batch_loaded, %function
batch_loaded:
  cbz x2, .Ldone_loaded
.Louter_loaded:
  ldr x6, [x0]
  mov w3, #4
.Linner_loaded:
  ldr x4, [x6], #8
  subs w3, w3, #1
  b.ne .Linner_loaded
  add x0, x0, #64
  subs x2, x2, #1
  b.ne .Louter_loaded
.Ldone_loaded:
  ret
.size batch_loaded, .-batch_loaded

#--- check.py
import ctypes
import mmap
import sys

page = mmap.PAGESIZE
mapping = mmap.mmap(-1, 3 * page)
start = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
base = start + page
libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
for address in [start, start + 2 * page]:
    if libc.mprotect(address, page, 0):
        raise OSError(ctypes.get_errno(), "mprotect failed")
values = (ctypes.c_uint64 * (page // 8)).from_address(base)
values[:] = range(1, len(values) + 1)

for path in sys.argv[1:]:
    library = ctypes.CDLL(path)
    for name in ["batch_forward", "batch_reverse", "batch_pair", "batch_prefix",
                 "batch_copy", "batch_copy_reverse", "batch_unwind"]:
        function = getattr(library, name)
        function.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
        function.restype = ctypes.c_uint64
        for count in [0, 1, 2, 3, 17]:
            width = 8 if name == "batch_pair" else 4
            stride = 8 if "copy" in name else width
            indices = [row * stride + column
                       for row in range(count)
                       for column in range(width)]
            span = max(indices, default=-1) + 1
            for alignment in [0, 8, 24]:
                if "reverse" in name:
                    first = base + alignment
                    address = first + (span - 1) * 8
                else:
                    first = base + page - span * 8 - alignment
                    address = first - (8 if name == "batch_prefix" else 0)
                expected = sum(ctypes.c_uint64.from_address(first + i * 8).value
                               for i in indices)
                result = function(address, 0, count)
                assert result == expected, (path, name, count, alignment)

#--- check-unwind.py
import re
import subprocess
import sys
from pathlib import Path

for library, assembly in zip(sys.argv[2::2], sys.argv[3::2]):
    text = Path(assembly).read_text()
    body = re.search(r"^([0-9a-f]+) <batch_unwind>:\n(.*?)(?=\n\n|\Z)",
                     text, re.M | re.S)
    assert body, assembly
    prefetches = re.findall(r"^\s*([0-9a-f]+):.*\bprf(?:m|um)\b", body[2], re.M)
    assert len(prefetches) == 2, assembly
    for pc in prefetches:
        offset = int(pc, 16) - int(body[1], 16)
        subprocess.run(
            [sys.argv[1], library, "batch_unwind", str(offset), "78"],
            check=True)
