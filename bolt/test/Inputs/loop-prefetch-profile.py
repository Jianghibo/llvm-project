# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Create synthetic cycle and miss records for AArch64 code generation tests."""

import argparse
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("distance", type=int)
args = parser.parse_args()
notes = subprocess.check_output(["llvm-readelf", "-n", args.binary], text=True)
build_id = re.search(r"Build ID: ([0-9a-f]+)", notes).group(1)
symbols = subprocess.check_output(
    ["llvm-nm", "-n", "--print-size", args.binary], text=True
)
functions = []
for line in symbols.splitlines():
    fields = line.split()
    if len(fields) == 4 and fields[2] in ("T", "t"):
        start, size = (int(field, 16) for field in fields[:2])
        if size:
            # These fixtures have unique local names. BOLT disambiguates each
            # local symbol with a /1 suffix, including when it is unique.
            name = fields[3] + ("/1" if fields[2] == "t" else "")
            functions.append((start, start + size, name))
assembly = subprocess.check_output(
    ["llvm-objdump", "-d", "--no-show-raw-insn", args.binary], text=True
)
print(f"bolt-prefetch llc_miss {build_id} 1024 200000001 0 0")
for start, end, name in functions:
    print(f"0 [unknown] 0 1 {name} 0 0 100 0 0")
    instructions = []
    for line in assembly.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s+(\w[\w.]*)\s+(.*)", line)
        if match and start <= int(match[1], 16) < end:
            instructions.append((int(match[1], 16), match[2], match[3]))
    index_regs = set()
    for _, opcode, operands in instructions:
        if opcode.startswith(("ldr", "ldur")):
            index = re.search(r"\[[^],]+,\s*[wx](\d+)", operands)
            if index:
                index_regs.add(index[1])
    for address, opcode, operands in instructions:
        offset = address - start
        if opcode.startswith(("ldr", "ldur", "ldp", "ldar", "ldax", "ldxr")):
            result = re.match(r"[wx](\d+),", operands)
            # Give index producers a hit-only profile. Their speculative copy
            # must have a known cost, without creating another affine request.
            misses = 0 if result and result[1] in index_regs else 9
            print(f"7 {name} {offset:x} {misses} 10")
        if opcode == "b" or opcode.startswith(("b.", "cbz", "cbnz", "tbz", "tbnz")):
            target = re.search(r"0x([0-9a-f]+)", operands)
            if target and start <= int(target[1], 16) < end:
                to = int(target[1], 16) - start
                # With the pass's 200-cycle latency, this mean predicts D.
                print(
                    f"1 {name} {offset:x} 1 {name} {to:x} " f"0 100 200 {args.distance}"
                )
