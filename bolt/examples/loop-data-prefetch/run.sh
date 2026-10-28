#!/usr/bin/env bash
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Statically check instruction insertion; never execute the generated ELF.
set -euo pipefail

tools_dir=${1:?Usage: bash run.sh /path/to/build/bin [output-dir]}
output_dir=${2:-$(mktemp -d /tmp/bolt-prefetch-example.XXXXXX)}
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output_dir"

"$tools_dir/llvm-mc" -triple=aarch64 -filetype=obj \
  "$source_dir/input.s" -o "$output_dir/input.o"
"$tools_dir/ld.lld" --emit-relocs --build-id=0x11223344 -e _start \
  "$output_dir/input.o" -o "$output_dir/input"

"$tools_dir/llvm-bolt" "$output_dir/input" -o "$output_dir/baseline" \
  --enable-loop-data-prefetch=false --verify-cfg \
  > "$output_dir/baseline.log" 2>&1
"$tools_dir/llvm-bolt" "$output_dir/input" -o "$output_dir/prefetched" \
  --enable-loop-data-prefetch --data="$source_dir/input.fdata" \
  --loop-prefetch-hint=pldl1keep --loop-prefetch-diagnostics --verify-cfg \
  > "$output_dir/prefetch.log" 2>&1

for variant in input baseline prefetched; do
  "$tools_dir/llvm-objdump" -d --no-print-imm-hex \
    --disassemble-symbols=sum_u64 "$output_dir/$variant" \
    > "$output_dir/$variant.asm"
done

if grep -Eq '\b(prfm|prfum)[[:space:]]' \
    "$output_dir/input.asm" "$output_dir/baseline.asm"; then
  echo "FAIL: unexpected prefetch in the original or baseline" >&2
  exit 1
fi
if ! grep -Eq 'prfm[[:space:]]+pldl1keep, \[x0, #64\]' \
    "$output_dir/prefetched.asm"; then
  echo "FAIL: expected prefetch missing; see $output_dir/prefetch.log" >&2
  exit 1
fi

grep -E '^BOLT-PREFETCH: sum_u64 |number of prefetch instructions inserted:' \
  "$output_dir/prefetch.log"
cat "$output_dir/prefetched.asm"
printf '\nPASS: prefetch inserted for data[i+8] (64 bytes ahead).\nArtifacts: %s\n' \
  "$output_dir"
