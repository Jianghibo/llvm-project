# REQUIRES: system-linux
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux-gnu %t/input.s -o %t.o
# RUN: ld.lld --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.out --data %t/profile --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --verify-cfg 2>&1 | FileCheck %s --check-prefix=COUNT
# RUN: llvm-objdump -d --no-print-imm-hex --disassemble-symbols=forward,negative,immediate,indirect,no_cycles,pair %t.out | FileCheck %s
# RUN: %if target=aarch64{{.*}} %{ %t.exe && %t.out %}
# RUN: llvm-bolt %t.exe -o %t.layout --data %t/profile --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 \
# RUN:   --reorder-blocks=ext-tsp --split-functions --split-all-cold --verify-cfg
# RUN: %if target=aarch64{{.*}} %{ %t.layout %}
# RUN: not llvm-bolt %t.exe -o %t.bad --data %t/no_lbr \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 2>&1 | FileCheck %s --check-prefix=NO-LBR
# RUN: not llvm-bolt %t.exe -o %t.bad --enable-loop-data-prefetch --loop-prefetch-latency=200 -nl 2>&1 | \
# RUN:   FileCheck %s --check-prefix=NO-LBR

# RUN: llvm-nm %t.exe > %t/symbols
# RUN: chmod +x %t/perf
# RUN: %python %t/convert.py perf2bolt %t.exe -p %t/perf.data -o %t/generated --cache-miss=llc_miss
# RUN: FileCheck %s --input-file=%t/generated --check-prefix=HEADER
# RUN: llvm-bolt %t.exe --data %t/generated -o %t.generated.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-diagnostics --verify-cfg -v=1 2>&1 | \
# RUN:   FileCheck %s --check-prefix=E2E
# RUN: %if target=aarch64{{.*}} %{ %t.generated.out %}
# RUN: sed 's/11223344/55667788/' %t/generated > %t/wrong-id
# RUN: not llvm-bolt %t.exe --data %t/wrong-id -o %t.bad 2>&1 | FileCheck %s --check-prefix=IDENTITY
# RUN: sed 's/1024/2048/' %t/generated > %t/other-period
# RUN: not merge-fdata %t/generated %t/other-period 2>&1 | FileCheck %s --check-prefix=PROTOCOL
# RUN: merge-fdata %t/generated %t/generated > %t/doubled
# RUN: llvm-bolt %t.exe --data %t/doubled -o %t.doubled.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-diagnostics --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DOUBLED
# RUN: ld.lld -pie --no-dynamic-linker --emit-relocs --build-id=0x11223344 -e _start %t.o -o %t.pie
# RUN: llvm-nm %t.pie > %t/symbols
# RUN: env BOLT_TEST_BINARY=%t.pie %python %t/convert.py perf2bolt %t.pie -p %t/perf.data -o %t/pie-profile --cache-miss=llc_miss
# RUN: llvm-bolt %t.pie --data %t/pie-profile -o %t.pie.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-diagnostics --verify-cfg -v=1 2>&1 | \
# RUN:   FileCheck %s --check-prefix=E2E
# RUN: %if target=aarch64{{.*}} %{ %t.pie.out %}
# RUN: ld.lld -shared -Bsymbolic --emit-relocs --build-id=0x11223344 %t.o -o %t.so
# RUN: llvm-nm %t.so > %t/symbols
# RUN: env BOLT_TEST_BINARY=%t.so %python %t/convert.py perf2bolt %t.so -p %t/perf.data -o %t/so-profile --cache-miss=llc_miss
# RUN: llvm-bolt %t.so --data %t/so-profile -o %t.so.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-diagnostics --verify-cfg -v=1 2>&1 | \
# RUN:   FileCheck %s --check-prefix=E2E
# RUN: llvm-nm %t.exe > %t/symbols
# RUN: not env BOLT_TEST_NO_BRANCH=1 %python %t/convert.py perf2bolt %t.exe -p %t/perf.data \
# RUN:   -o %t.bad --cache-miss=llc_miss 2>&1 | FileCheck %s --check-prefix=NO-BRANCH
# RUN: env BOLT_TEST_ORPHAN_MEM=1 %python %t/convert.py perf2bolt %t.exe -p %t/perf.data \
# RUN:   -o %t.orphan-generated --cache-miss=llc_miss 2>&1 | FileCheck %s --check-prefix=ORPHAN-GEN
# RUN: llvm-bolt %t.exe --data %t.orphan-generated -o %t.orphan-generated.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ORPHAN-USE
# RUN: llvm-bolt %t.exe --data %t/orphan -o %t.orphan-reader.out --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ORPHAN-READER
# RUN: not llvm-bolt %t.exe --data %t/legacy -o %t.bad --enable-loop-data-prefetch --loop-prefetch-latency=200 2>&1 | \
# RUN:   FileCheck %s --check-prefix=LEGACY

# RUN: llvm-bolt %t.exe --data %t/generated -o %t.no-model --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-diagnostics 2>&1 | FileCheck %s --check-prefix=NO-MODEL
# RUN: sed 's/2 10 1/2 11 2/' %t/generated > %t/fractional
# RUN: llvm-bolt %t.exe --data %t/fractional -o %t.fractional --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=11 --loop-prefetch-diagnostics 2>&1 | \
# RUN:   FileCheck %s --check-prefix=FRACTIONAL
# RUN: llvm-bolt %t.exe --data %t/generated -o %t.override --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=11 --loop-prefetch-iteration-cycles=4 \
# RUN:   --loop-prefetch-diagnostics 2>&1 | FileCheck %s --check-prefix=CALIBRATED
# RUN: llvm-bolt %t.exe --data %t/generated -o %t.limit --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=18446744073709551615 \
# RUN:   --loop-prefetch-diagnostics 2>&1 | FileCheck %s --check-prefix=DISTANCE-LIMIT
# RUN: llvm-bolt %t.exe --data %t/ranking -o %t.ranking --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-max-per-loop=1 \
# RUN:   --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=RANKING
# RUN: llvm-bolt %t.exe --data %t/ranking -o %t.miss-limit --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-min-misses=100 \
# RUN:   --loop-prefetch-diagnostics 2>&1 | FileCheck %s --check-prefix=MISS-LIMIT

# RUN: %python %t/prefer-first.py %t/symbols > %t/prefer-first
# RUN: llvm-bolt %t.exe --data %t/ranking -o %t.preferred --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=200 --loop-prefetch-hints=%t/prefer-first \
# RUN:   --loop-prefetch-max-per-function=1 --loop-prefetch-diagnostics --verify-cfg 2>&1 | \
# RUN:   FileCheck %s --check-prefix=PREFERRED

# PREFERRED: compete {{.*}}source=pc hint distance=4 {{.*}}inserted{{.*}}opcode=LDRXroX
# PREFERRED: compete {{.*}}request budget{{.*}}opcode=LDRQroX
# RUN: llvm-bolt %t.exe --data %t/two-latches -o %t.ambiguous --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=11 --loop-prefetch-diagnostics 2>&1 | \
# RUN:   FileCheck %s --check-prefix=AMBIGUOUS
# RUN: llvm-bolt %t.exe --data %t/two-latches -o %t.calibrated --lite=0 \
# RUN:   --enable-loop-data-prefetch --loop-prefetch-latency=11 --loop-prefetch-iteration-cycles=4 \
# RUN:   --loop-prefetch-diagnostics --verify-cfg 2>&1 | FileCheck %s --check-prefix=CALIBRATED

# AMBIGUOUS: multiple latches need calibrated cycles
# AMBIGUOUS: number of prefetch instructions inserted: 0
# NO-MODEL: missing calibrated latency
# NO-MODEL: number of prefetch instructions inserted: 0
# FRACTIONAL: source=profile distance=2 {{.*}}inserted{{.*}}iteration-cycle-total=11 iteration-cycle-samples=2
# CALIBRATED: source=profile distance=3 {{.*}}inserted{{.*}}cycles-source=calibrated
# DISTANCE-LIMIT: automatic distance exceeds limit
# DISTANCE-LIMIT: number of prefetch instructions inserted: 0
# RANKING: compete {{.*}}inserted{{.*}}opcode=LDRQroX misses=80
# RANKING: compete {{.*}}request budget{{.*}}opcode=LDRXroX misses=40
# RANKING: number of prefetch instructions inserted: 1
# MISS-LIMIT: insufficient sampled misses
# MISS-LIMIT: number of prefetch instructions inserted: 0

# HEADER: bolt-prefetch 2 llc_miss 11223344 1024 200000001 0 0
# HEADER: 7 forward 0 6 8
# E2E: cache-miss bindings: 1 loads, 1 invalid PCs, 1 non-loads, 0 unmatched function records
# E2E: source=profile distance=20 {{.*}}inserted{{.*}}misses=6 accesses=8
# E2E: number of prefetch instructions inserted: 1
# DOUBLED: source=profile distance=20 {{.*}}inserted{{.*}}misses=12 accesses=16
# IDENTITY: prefetch profile build-id does not match input
# PROTOCOL: cannot merge incompatible identities, sampling attributes
# NO-BRANCH: require branch records from a supported non--nl source
# ORPHAN-GEN: BOLT-WARNING: ignoring 1 cache-miss functions without branch records
# ORPHAN-GEN: PERF2BOLT: wrote {{.*}} cache miss objects
# ORPHAN-USE: number of prefetch instructions inserted: 1
# ORPHAN-READER: BOLT-WARNING: ignoring 1 cache-miss functions without branch records
# ORPHAN-READER: number of prefetch instructions inserted: 1
# LEGACY: prefetch profile has no binary identity

# COUNT: number of prefetch instructions inserted: 5
# NO-LBR: loop data prefetch does not support
# CHECK-LABEL: <forward>:
# CHECK:      add x2, x0, x1, lsl #3
# CHECK-NEXT: prfm pldl1keep, [x2, #160]
# CHECK-NEXT: ldr x2, [x0, x1, lsl #3]
# CHECK-LABEL: <negative>:
# CHECK:      sub [[NEG_ADDR:x[0-9]+]], x0, #320
# CHECK-NEXT: add [[NEG_ADDR]], [[NEG_ADDR]], x1, lsl #4
# CHECK-NEXT: prfm pldl1keep, {{\[}}[[NEG_ADDR]]{{\]}}
# CHECK-NEXT: ldr q2, [x0, x1, lsl #4]
# CHECK-LABEL: <immediate>:
# CHECK-NOT: prfm
# CHECK-LABEL: <indirect>:
# CHECK-NOT: prfm
# CHECK-LABEL: <no_cycles>:
# CHECK-NOT: prfm
# CHECK-LABEL: <pair>:
# CHECK:      prfm pldl1keep, [x0, #320]
# CHECK-NEXT: prfm pldl1keep, [x0, #328]
# CHECK-NEXT: ldp x2, x4, [x0]

#--- input.s
.text
.globl _start
.type _start, %function
_start:
  adrp x0, array
  add x0, x0, :lo12:array
  mov x5, x0
  mov x1, #0
  mov x3, #16
  bl forward
  cmp x0, x5
  b.ne fail
  cmp x1, #16
  b.ne fail
  cmp x2, #15
  b.ne fail
  mov x0, #0
  b exit
fail:
  mov x0, #1
exit:
  mov x8, #93
  svc #0
.size _start, .-_start

.globl forward
.type forward, %function
forward:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo forward
  ret
.size forward, .-forward

.globl negative
.type negative, %function
negative:
  ldr q2, [x0, x1, lsl #4]
  sub x1, x1, #1
  cmp x1, x3
  b.hi negative
  ret
.size negative, .-negative

.globl immediate
.type immediate, %function
immediate:
  ldr x2, [x0, #16]
  add x1, x1, #1
  cmp x1, x3
  b.lo immediate
  ret
.size immediate, .-immediate

.globl indirect
.type indirect, %function
indirect:
  ldr x1, [x4, x3, lsl #3]
  ldr x2, [x0, x1, lsl #3]
  add x3, x3, #1
  cmp x3, x5
  b.lo indirect
  ret
.size indirect, .-indirect

.globl no_cycles
.type no_cycles, %function
no_cycles:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo no_cycles
  ret
.size no_cycles, .-no_cycles

.globl with_cfi
.type with_cfi, %function
with_cfi:
  .cfi_startproc
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  cmp x1, x3
  b.lo with_cfi
  ret
  .cfi_endproc
.size with_cfi, .-with_cfi

.globl pair
.type pair, %function
pair:
  ldp x2, x4, [x0]
  add x0, x0, #16
  subs x1, x1, #1
  b.ne pair
  ret
.size pair, .-pair

.globl compete
.type compete, %function
compete:
  ldr x2, [x0, x1, lsl #3]
  ldr q4, [x0, x1, lsl #4]
  add x1, x1, #1
  cmp x1, x3
  b.lo compete
  ret
.size compete, .-compete

.globl two_latches
.type two_latches, %function
two_latches:
  ldr x2, [x0, x1, lsl #3]
  add x1, x1, #1
  tbz x1, #0, .Leven
  cmp x1, x3
  b.lo two_latches
  ret
.Leven:
  cmp x1, x3
  b.lo two_latches
  ret
.size two_latches, .-two_latches

.data
.p2align 4
array:
  .quad 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

#--- profile
bolt-prefetch 2 llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 forward 0 0 1 0 0
0 [unknown] 0 1 pair 0 0 1 0 0
0 [unknown] 0 1 negative 0 0 1 0 0
0 [unknown] 0 1 immediate 0 0 1 0 0
0 [unknown] 0 1 indirect 0 0 1 0 0
0 [unknown] 0 1 no_cycles 0 0 1 0 0
0 [unknown] 0 1 with_cfi 0 0 1 0 0
1 forward c 1 forward 0 0 15 100 10
1 pair c 1 pair 0 0 15 100 10
1 negative c 1 negative 0 0 15 100 10
1 immediate c 1 immediate 0 0 15 100 10
1 indirect 10 1 indirect 0 0 15 100 10
1 no_cycles c 1 no_cycles 0 0 15 0 0
1 with_cfi c 1 with_cfi 0 0 15 100 10
7 forward 0 8 10
7 pair 0 8 10
7 negative 0 8 10
7 immediate 0 8 10
7 indirect 4 8 10
7 no_cycles 0 8 10
7 with_cfi 0 8 10

#--- orphan
bolt-prefetch 2 llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 forward 0 0 1 0 0
1 forward c 1 forward 0 0 15 100 10
7 forward 0 8 10
7 pair 0 8 10

#--- no_lbr
no_lbr cycles
1 forward 0 10

#--- legacy
bolt-prefetch 1 llc_miss
1 forward c 1 forward 0 0 10 100 10
7 forward 0 6 8

#--- perf.data
PERFILE2

#--- convert.py
# Set PATH explicitly: the internal lit shell does not expand $PATH in env.
import os
from pathlib import Path
import subprocess
import sys

environment = os.environ.copy()
environment["PATH"] = str(Path(__file__).parent) + os.pathsep + environment["PATH"]
sys.exit(subprocess.call(sys.argv[1:], env=environment))

#--- perf
#!/usr/bin/env python3
# Fixed perf output exercises the conversion pipeline without SPE hardware.
import os
from pathlib import Path
import struct
import sys

root = Path(__file__).parent
binary = Path(os.environ.get("BOLT_TEST_BINARY", str(root) + ".exe"))
data = binary.read_bytes()
bias = 0x4000000000 if struct.unpack_from("<H", data, 16)[0] == 3 else 0
symbols = {}
for line in (root / "symbols").read_text().splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
pc = symbols["forward"] + bias
args = sys.argv[1:]
if args[0] == "buildid-list":
    print("11223344".ljust(40, "0"), binary)
elif args[0] == "evlist":
    print("arm_spe_0/load_filter=1/u: type: 25, config: 0x200000001, "
          "{ sample_period, sample_freq }: 1024, sample_type: IP|TID")
elif "--show-mmap-events" in args:
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    segment = next(offset + i * size for i in range(count)
                   if struct.unpack_from("<I", data, offset + i * size)[0] == 1
                   and struct.unpack_from("<I", data, offset + i * size + 4)[0] & 1)
    base = (struct.unpack_from("<Q", data, segment + 16)[0] & ~0xfff) + bias
    file_offset = struct.unpack_from("<Q", data, segment + 8)[0] & ~0xfff
    print(f"PERF_RECORD_MMAP2 1/1: [{base:#x}(0x100000) @ {file_offset:#x} "
          f"00:00 0 0]: r-xp {binary}")
elif "--show-task-events" in args:
    pass
elif "pid,event,addr,ip" in args:
    assert "--itrace=me" in args
    for _ in range(8):
        print(f"1 llc-access: 800000 {pc:x}")
    for _ in range(6):
        print(f"1 llc-miss: 800000 {pc:x}")
    print(f"1 llc-access: 800000 {pc + 1:x}")
    print(f"1 llc-access: 800000 {pc + 4:x}")
    if os.environ.get("BOLT_TEST_ORPHAN_MEM"):
        orphan = symbols["pair"] + bias
        for _ in range(8):
            print(f"1 llc-access: 800000 {orphan:x}")
        for _ in range(6):
            print(f"1 llc-miss: 800000 {orphan:x}")
elif not os.environ.get("BOLT_TEST_NO_BRANCH"):
    entry = f"{pc + 12:#x}/{pc:#x}/P/-/-/10/COND/-"
    assert "pid,brstack" in args
    print(f"1 {entry} {entry}")

#--- ranking
bolt-prefetch 2 llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 compete 0 0 1 0 0
1 compete 10 1 compete 0 0 100 1000 100
7 compete 0 40 100
7 compete 4 80 100

#--- prefer-first.py
from pathlib import Path
import sys

address = next(line.split()[0] for line in Path(sys.argv[1]).read_text().splitlines()
               if line.split()[-1] == "compete")
print("bolt-prefetch-hints 1 11223344")
print(f"pc {address} LDRXroX 4 pldl1keep 8")

#--- two-latches
bolt-prefetch 2 llc_miss 11223344 1024 200000001 0 0
0 [unknown] 0 1 two_latches 0 0 1 0 0
1 two_latches 8 1 two_latches 18 0 8 0 0
1 two_latches 10 1 two_latches 0 0 7 70 7
1 two_latches 1c 1 two_latches 0 0 7 70 7
7 two_latches 0 8 10
