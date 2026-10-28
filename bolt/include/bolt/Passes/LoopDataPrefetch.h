//===- bolt/Passes/LoopDataPrefetch.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_LOOPDATAPREFETCH_H
#define BOLT_PASSES_LOOPDATAPREFETCH_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

/// Prefetch affine loads and loaded next pointers using dead temporaries.
class LoopDataPrefetch : public BinaryFunctionPass {
public:
  explicit LoopDataPrefetch(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "loop data prefetch"; }

  bool shouldOptimize(const BinaryFunction &BF) const override;

  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
