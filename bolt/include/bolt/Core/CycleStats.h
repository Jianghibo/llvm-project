//===- CycleStats.h - Branch interval statistics -----------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_CORE_CYCLE_STATS_H
#define BOLT_CORE_CYCLE_STATS_H

#include "llvm/Support/MathExtras.h"
#include <cstdint>

namespace llvm {
namespace bolt {

/// Repeated-branch intervals. Samples == 0 means unknown; -1 means invalid.
struct CycleStats {
  int64_t Total{0};
  int64_t Samples{0};

  CycleStats() = default;
  CycleStats(int64_t Total, int64_t Samples) : Total(Total), Samples(Samples) {}

  void merge(const CycleStats &Other) {
    int64_t NewTotal, NewSamples;
    if (Samples < 0 || Other.Samples < 0 ||
        AddOverflow(Total, Other.Total, NewTotal) ||
        AddOverflow(Samples, Other.Samples, NewSamples)) {
      Total = 0;
      Samples = -1;
      return;
    }
    Total = NewTotal;
    Samples = NewSamples;
  }
};

} // namespace bolt
} // namespace llvm

#endif
