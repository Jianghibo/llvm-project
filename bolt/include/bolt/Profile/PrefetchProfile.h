//===- PrefetchProfile.h - SPE profile header -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PROFILE_PREFETCH_PROFILE_H
#define BOLT_PROFILE_PREFETCH_PROFILE_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace llvm {
namespace bolt {

/// Identity and sampling attributes needed to interpret and merge SPE counts.
/// Version 1 lacks these attributes and cannot authorize loop prefetching.
struct PrefetchProfileHeader {
  unsigned Version = 2;
  std::string Event, BuildID;
  uint64_t Period = 0, Config = 0, Config1 = 0, Config2 = 0;

  static Expected<PrefetchProfileHeader> parse(StringRef Line);
  void print(raw_ostream &OS) const;
};

inline Expected<PrefetchProfileHeader>
PrefetchProfileHeader::parse(StringRef Line) {
  SmallVector<StringRef, 8> Fields;
  Line.split(Fields, ' ');
  if (Fields.size() < 3 || Fields[0] != "bolt-prefetch" ||
      (Fields[1] != "1" && Fields[1] != "2") ||
      (Fields[2] != "tlb_miss" && Fields[2] != "l1d_miss" &&
       Fields[2] != "llc_miss"))
    return createStringError(
        errc::invalid_argument,
        "unsupported bolt-prefetch version or cache event");
  PrefetchProfileHeader Header;
  Header.Version = Fields[1] == "1" ? 1 : 2;
  Header.Event = Fields[2].str();
  if (Header.Version == 1 && Fields.size() == 3)
    return Header;
  if (Header.Version != 2 || Fields.size() != 8 || Fields[3].empty() ||
      Fields[3].size() % 2 ||
      !llvm::all_of(Fields[3], [](char C) { return isHexDigit(C); }) ||
      Fields[4].getAsInteger(10, Header.Period) || !Header.Period ||
      Fields[5].getAsInteger(16, Header.Config) ||
      Fields[6].getAsInteger(16, Header.Config1) ||
      Fields[7].getAsInteger(16, Header.Config2))
    return createStringError(
        errc::invalid_argument,
        "invalid prefetch identity or sampling attributes");
  // Only unfiltered load sampling has the denominator expected by the pass.
  constexpr uint64_t AllowedConfig = (1ULL << 33) | (1ULL << 16) | 7;
  if (!(Header.Config & (1ULL << 33)) || (Header.Config & ~AllowedConfig) ||
      Header.Config1 || Header.Config2)
    return createStringError(errc::not_supported,
                             "prefetch profiles require unfiltered SPE loads");
  Header.BuildID = Fields[3].lower();
  return Header;
}

inline void PrefetchProfileHeader::print(raw_ostream &OS) const {
  OS << "bolt-prefetch " << Version << ' ' << Event;
  if (Version == 2)
    OS << ' ' << BuildID << ' ' << Period << ' ' << Twine::utohexstr(Config)
       << ' ' << Twine::utohexstr(Config1) << ' ' << Twine::utohexstr(Config2);
  OS << '\n';
}

} // namespace bolt
} // namespace llvm

#endif
