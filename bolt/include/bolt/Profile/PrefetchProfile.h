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

/// Persisted event names and the corresponding perf SPE event group.
struct SPECacheEvent {
  StringLiteral Name, Access, Miss;
  char ITrace;
};

inline constexpr SPECacheEvent SPECacheEvents[] = {
    {"tlb_miss", "tlb-access:", "tlb-miss:", 't'},
    {"l1d_miss", "l1d-access:", "l1d-miss:", 'f'},
    {"llc_miss", "llc-access:", "llc-miss:", 'm'}};

/// Identity and sampling attributes needed to interpret and merge SPE counts.
struct PrefetchProfileHeader {
  std::string Event, BuildID;
  uint64_t Period = 0, Config = 0, Config1 = 0, Config2 = 0;

  /// Parse and validate a complete extension header.
  static Expected<PrefetchProfileHeader> parse(StringRef Line);
  /// Check the binary identity and SPE sampling contract.
  Error validate() const;
  /// Write the canonical extension header.
  void print(raw_ostream &OS) const;
};

inline Expected<PrefetchProfileHeader>
PrefetchProfileHeader::parse(StringRef Line) {
  SmallVector<StringRef, 7> Fields;
  Line.split(Fields, ' ');
  if (Fields.size() != 7 || Fields[0] != "bolt-prefetch")
    return createStringError(errc::invalid_argument,
                             "invalid bolt-prefetch header");
  PrefetchProfileHeader Header;
  Header.Event = Fields[1].str();
  if (Fields[3].getAsInteger(10, Header.Period) ||
      Fields[4].getAsInteger(16, Header.Config) ||
      Fields[5].getAsInteger(16, Header.Config1) ||
      Fields[6].getAsInteger(16, Header.Config2))
    return createStringError(
        errc::invalid_argument,
        "invalid prefetch identity or sampling attributes");
  Header.BuildID = Fields[2].lower();
  if (Error E = Header.validate())
    return std::move(E);
  return Header;
}

inline Error PrefetchProfileHeader::validate() const {
  if (llvm::none_of(SPECacheEvents,
                    [&](const SPECacheEvent &E) { return Event == E.Name; }))
    return createStringError(errc::invalid_argument,
                             "unsupported bolt-prefetch cache event");
  if (BuildID.empty() || BuildID.size() % 2 ||
      !llvm::all_of(BuildID, [](char C) { return isHexDigit(C); }) || !Period)
    return createStringError(
        errc::invalid_argument,
        "invalid prefetch identity or sampling attributes");
  // Only unfiltered load sampling has the denominator expected by the pass.
  constexpr uint64_t AllowedConfig = (1ULL << 33) | (1ULL << 16) | 7;
  if (!(Config & (1ULL << 33)) || (Config & ~AllowedConfig) || Config1 ||
      Config2)
    return createStringError(errc::not_supported,
                             "prefetch profiles require unfiltered SPE loads");
  return Error::success();
}

inline void PrefetchProfileHeader::print(raw_ostream &OS) const {
  OS << "bolt-prefetch " << Event << ' ' << BuildID << ' ' << Period << ' '
     << Twine::utohexstr(Config) << ' ' << Twine::utohexstr(Config1) << ' '
     << Twine::utohexstr(Config2) << '\n';
}

} // namespace bolt
} // namespace llvm

#endif
