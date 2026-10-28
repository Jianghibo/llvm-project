//===- bolt/unittests/Profile/DataAggregator.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Profile/DataAggregator.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;

namespace opts {
extern cl::opt<bool> ReadPreAggregated;
extern cl::opt<bool> IgnoreBuildID;
} // namespace opts

namespace llvm {
namespace bolt {

/// Test helper with friend access to DataAggregator internals.
/// Used for both parseHexField tests (no BC needed) and pre-aggregated
/// parsing tests (BC needed, X86-only).
struct PreAggregatedTestHelper : public testing::Test {
  void SetUp() override { opts::ReadPreAggregated = true; }

protected:
  using Trace = DataAggregator::Trace;
  using TakenBranchInfo = DataAggregator::TakenBranchInfo;

  /// Parse a hex field from input string.
  ErrorOr<uint64_t> parseHex(StringRef Input) {
    DataAggregator DA("<pseudo input>");
    DA.setParsingBuffer(Input);
    return DA.parseHexField(' ', /*EndNl=*/true);
  }

  /// Parse pre-aggregated input and return collected Traces.
  /// Requires BC to be initialized (X86-only tests).
  void parseAndCollectTraces(
      StringRef Input, std::vector<std::pair<Trace, TakenBranchInfo>> &Result) {
    DataAggregator DA("<pseudo input>");
    DA.BC = BC.get();
    DA.setParsingBuffer(Input);
    std::error_code EC = DA.parsePreAggregatedLBRSamples();
    ASSERT_FALSE(EC);
    Result = std::move(DA.Traces);
  }

  /// Initialize target and BinaryContext for pre-aggregated tests.
  void initializeBOLTForX86() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllDisassemblers();
    llvm::InitializeAllTargets();
    llvm::InitializeAllAsmPrinters();

    memcpy(ElfBuf, "\177ELF", 4);
    ELF64LE::Ehdr *EHdr = reinterpret_cast<typename ELF64LE::Ehdr *>(ElfBuf);
    EHdr->e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    EHdr->e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    EHdr->e_machine = llvm::ELF::EM_X86_64;
    MemoryBufferRef Source(StringRef(ElfBuf, sizeof(ElfBuf)), "ELF");
    ObjFile = cantFail(ObjectFile::createObjectFile(Source));

    BC = cantFail(BinaryContext::createBinaryContext(
        ObjFile->makeTriple(), std::make_shared<orc::SymbolStringPool>(),
        ObjFile->getFileName(), nullptr, /*IsPIC*/ false,
        DWARFContext::create(*ObjFile), {llvm::outs(), llvm::errs()}));
    ASSERT_FALSE(!BC);
  }

  char ElfBuf[sizeof(typename ELF64LE::Ehdr)] = {};
  std::unique_ptr<object::ObjectFile> ObjFile;
  std::unique_ptr<BinaryContext> BC;
};

} // namespace bolt
} // namespace llvm

TEST(DataAggregatorTest, buildID) {
  opts::ReadPreAggregated = true;

  DataAggregator DA("<pseudo input>");
  std::optional<StringRef> FileName;

  DA.setParsingBuffer("");
  ASSERT_FALSE(DA.hasAllBuildIDs());
  FileName = DA.getFileNameForBuildID("1234");
  ASSERT_FALSE(FileName);

  StringRef PartialValidBuildIDs = "     File0\n"
                                   "1111 File1\n"
                                   "     File2\n";
  DA.setParsingBuffer(PartialValidBuildIDs);
  ASSERT_FALSE(DA.hasAllBuildIDs());
  FileName = DA.getFileNameForBuildID("0000");
  ASSERT_FALSE(FileName);
  FileName = DA.getFileNameForBuildID("1111");
  ASSERT_EQ(*FileName, "File1");

  StringRef AllValidBuildIDs = "0000 File0\n"
                               "1111 File1\n"
                               "2222 File2\n"
                               "333  File3\n";
  DA.setParsingBuffer(AllValidBuildIDs);
  ASSERT_TRUE(DA.hasAllBuildIDs());
  FileName = DA.getFileNameForBuildID("1234");
  ASSERT_FALSE(FileName);
  FileName = DA.getFileNameForBuildID("2222");
  ASSERT_EQ(*FileName, "File2");
  FileName = DA.getFileNameForBuildID("333");
  ASSERT_EQ(*FileName, "File3");

  DA.setParsingBuffer("11112222 File1\n111100000000 File2\n");
  FileName = DA.getFileNameForBuildID("1111");
  ASSERT_TRUE(FileName);
  EXPECT_EQ(*FileName, "File2");
}

namespace llvm {
namespace bolt {

class DataAggregatorTester : public testing::Test {
protected:
  void SetUp() override {
    // Avoid spawning perf processes.
    opts::ReadPreAggregated = true;
    DA = std::make_unique<DataAggregator>("<pseudo input>");
  }

  using LBREntry = DataAggregator::LBREntry;

  ErrorOr<LBREntry> parseEntry() { return DA->parseLBREntry(); }

  void prepareContext() {
    DA->NamesToCacheMissEvents.clear();
    DA->MemSamples.clear();
    InitializeNativeTarget();
    InitializeNativeTargetDisassembler();
    BC = cantFail(BinaryContext::createBinaryContext(
        Triple(LLVM_DEFAULT_TARGET_TRIPLE),
        std::make_shared<orc::SymbolStringPool>(), "<pseudo input>", nullptr,
        false, nullptr, {outs(), errs()}));
    BC->FirstAllocAddress = 0x1000;
    BC->LayoutStartAddress = 0x2000;
    BinarySection &Section = BC->registerOrUpdateSection(
        ".text", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    BC->createBinaryFunction("loop", Section, 0x1000, 0x100)->setMaxSize(0x100);
    DA->BC = BC.get();
    DA->BinaryMMapInfo[1] = {};
  }

  void aggregate(ArrayRef<LBREntry> Entries) {
    prepareContext();
    DataAggregator::PerfBranchSample Sample;
    Sample.LBR.append(Entries.begin(), Entries.end());
    DA->parseLBRSample(Sample, /*NeedsSkylakeFix=*/false);
  }

  CycleStats getCycles(uint64_t From, uint64_t To) const {
    CycleStats Cycles;
    for (const auto &[Trace, Info] : DA->TraceMap)
      if (Trace.Branch == From && Trace.From == To)
        Cycles.merge(Info.Cycles);
    return Cycles;
  }

  std::error_code parseMemory(StringRef Input) {
    DA->setParsingBuffer(Input);
    return DA->parseMemEvents();
  }

  Error aggregateMemory() { return DA->processCacheMissEvents(); }

  std::pair<uint64_t, uint64_t> cacheCounts(uint64_t Offset) {
    auto &Profile = DA->NamesToCacheMissEvents.at("loop");
    const auto &Counts = Profile.Data[Profile.Index.at(Offset)];
    return {Counts.MissCount, Counts.AccessCount};
  }

  unsigned legacyMemorySamples() {
    return llvm::count_if(
        DA->MemSamples, [](const auto &Sample) { return !Sample.IsCacheMiss; });
  }

  Error samplingAttrs(StringRef Input) {
    DA->CacheMissHeader = {};
    DA->CacheMissHeader.BuildID = "11223344";
    DA->CacheMissHeader.Event = "llc_miss";
    return DA->parseCacheMissAttrs(Input);
  }

  Error mergeSamplingAttrs(uint64_t Period) {
    DataAggregator Other("<pseudo input>");
    Other.CacheMissHeader = DA->CacheMissHeader;
    Other.CacheMissHeader.Period = Period;
    return DA->mergeFrom(Other);
  }

  std::pair<uint64_t, uint64_t> relocatedMemory() {
    prepareContext();
    DA->BinaryMMapInfo[1].BaseAddress = 0x100000;
    DA->BinaryMMapInfo[1].MMapAddress = 0x100000;
    DA->BinaryMMapInfo[1].Size = 0x2000;
    BC->HasFixedLoadAddress = false;
    EXPECT_FALSE(parseMemory("1 llc-access: 800000 101010\n"));
    return {DA->MemSamples.front().PC, DA->MemSamples.front().Addr};
  }

  std::error_code parseBranches(StringRef Input) {
    DA->setParsingBuffer(Input);
    return DA->parseBranchEvents();
  }

  void TearDown() override {
    cl::getRegisteredOptions()["cache-miss"]->reset();
  }

  std::unique_ptr<BinaryContext> BC;
  std::unique_ptr<DataAggregator> DA;
};

TEST_F(DataAggregatorTester, LegacyLBREntries) {
  DA->setParsingBuffer("0x1010/0x1000/P/-/-/12 0x1020/0x1000/M/-/-/9\n");
  auto First = parseEntry();
  ASSERT_TRUE(First);
  EXPECT_EQ(First->From, 0x1010u);
  EXPECT_EQ(First->To, 0x1000u);
  EXPECT_EQ(First->Cycles, 12);
  EXPECT_FALSE(First->Mispred);
  auto Second = parseEntry();
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->From, 0x1020u);
  EXPECT_EQ(Second->Cycles, 9);
  EXPECT_TRUE(Second->Mispred);
}

TEST_F(DataAggregatorTester, ExtendedLBREntries) {
  DA->setParsingBuffer("0x1010/0x1000/P/-/-/12/COND/-   "
                       "0x1020/0x1000/M/-/-/9/UNCOND/-  \n");
  auto First = parseEntry();
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Cycles, 12);
  auto Second = parseEntry();
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->From, 0x1020u);
  EXPECT_EQ(Second->Cycles, 9);
}

TEST_F(DataAggregatorTester, InvalidLBREntries) {
  for (StringRef Input :
       {"0x1010/0x1000/P/-/-\n", "bad/0x1000/P/-/-/12\n",
        "0x1010/bad/P/-/-/12\n", "0x1010/0x1000/?/-/-/12\n",
        "0x1010/0x1000/P/-/-/bad\n", "0x1010/0x1000/P/-/-/-1\n",
        "0x1010/0x1000/P/-/-/12"}) {
    SCOPED_TRACE(Input.str());
    DA->setParsingBuffer(Input);
    EXPECT_FALSE(parseEntry());
  }
}

TEST_F(DataAggregatorTester, TruncatedEntryDoesNotConsumeNextEntry) {
  DA->setParsingBuffer("0x1010/0x1000/P/-/- 0x1020/0x1000/P/-/-/9\n");
  EXPECT_FALSE(parseEntry());
  auto Second = parseEntry();
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->From, 0x1020u);
  EXPECT_EQ(Second->Cycles, 9);
}

TEST_F(DataAggregatorTester, SingleBranchHasNoInterval) {
  aggregate({{0x1010, 0x1000, false, false, 12}});
  EXPECT_EQ(getCycles(0x1010, 0x1000).Total, 0);
  EXPECT_EQ(getCycles(0x1010, 0x1000).Samples, 0);
}

TEST_F(DataAggregatorTester, RepeatedBranchInterval) {
  aggregate({{0x1010, 0x1000, false, false, 100},
             {0x1020, 0x1000, false, false, 7},
             {0x1010, 0x1000, false, false, 20}});
  EXPECT_EQ(getCycles(0x1010, 0x1000).Total, 107);
  EXPECT_EQ(getCycles(0x1010, 0x1000).Samples, 1);
  EXPECT_EQ(getCycles(0x1020, 0x1000).Total, 0);
  EXPECT_EQ(getCycles(0x1020, 0x1000).Samples, 0);
}

TEST_F(DataAggregatorTester, MultipleBranchIntervals) {
  aggregate({{0x1010, 0x1000, false, false, 100},
             {0x1020, 0x1000, false, false, 7},
             {0x1010, 0x1000, false, false, 20},
             {0x1020, 0x1000, false, false, 3},
             {0x1010, 0x1000, false, false, 40}});
  EXPECT_EQ(getCycles(0x1010, 0x1000).Total, 130);
  EXPECT_EQ(getCycles(0x1010, 0x1000).Samples, 2);
}

TEST_F(DataAggregatorTester, IntervalIncludesUnmappedBranches) {
  aggregate({{0x1010, 0x1000, false, false, 100},
             {0x3000, 0x3010, false, false, 7},
             {0x1010, 0x1000, false, false, 20}});
  EXPECT_EQ(getCycles(0x1010, 0x1000).Total, 107);
  EXPECT_EQ(getCycles(0x1010, 0x1000).Samples, 1);
}

TEST_F(DataAggregatorTester, CacheMissCountsAndEventFiltering) {
  auto *Option = cl::getRegisteredOptions()["cache-miss"];
  ASSERT_FALSE(Option->addOccurrence(0, "cache-miss", "llc_miss"));
  for (StringRef Input : {"1 llc-access: a000 1010\n1 llc-miss: a000 1010\n"
                          "1 llc-access: a000 1020\n1 llc-miss: a000 1020\n"
                          "1 llc-access: a000 1010\n1 l1d-miss: a000 1010\n"
                          "1 mem-loads: a000 1010\n",
                          "1 mem-loads: a000 1010\n1 llc-miss: a000 1020\n"
                          "1 llc-access: a000 1010\n1 l1d-miss: a000 1010\n"
                          "1 llc-miss: a000 1010\n1 llc-access: a000 1020\n"
                          "1 llc-access: a000 1010\n"}) {
    prepareContext();
    ASSERT_FALSE(parseMemory(Input));
    EXPECT_EQ(legacyMemorySamples(), 1u);
    ASSERT_FALSE(bool(aggregateMemory()));
    EXPECT_EQ(cacheCounts(0x10), std::make_pair(uint64_t(1), uint64_t(2)));
    EXPECT_EQ(cacheCounts(0x20), std::make_pair(uint64_t(1), uint64_t(1)));
  }
  Option->reset();
  prepareContext();
  ASSERT_FALSE(parseMemory("1 llc-access: a000 1010\n"
                           "1 mem-loads: a000 1010\n"));
  EXPECT_EQ(legacyMemorySamples(), 1u);
  ASSERT_FALSE(bool(aggregateMemory()));
}

} // namespace bolt
} // namespace llvm

TEST_F(PreAggregatedTestHelper, parseHexField) {
  auto Res = parseHex("4b196f\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, 0x4b196fULL);

  Res = parseHex("ffffffffffffffff\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, Trace::BR_ONLY);

  // -1 → UINT64_MAX (BR_ONLY / FT_ONLY).
  Res = parseHex("-1\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, Trace::BR_ONLY);

  // -2 → UINT64_MAX - 1 (FT_EXTERNAL_ORIGIN).
  Res = parseHex("-2\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, Trace::FT_EXTERNAL_ORIGIN);

  // -3 → UINT64_MAX - 2 (FT_EXTERNAL_RETURN).
  Res = parseHex("-3\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, Trace::FT_EXTERNAL_RETURN);

  Res = parseHex("0\n");
  ASSERT_TRUE(!!Res);
  EXPECT_EQ(*Res, 0ULL);
}

#ifdef X86_AVAILABLE

namespace llvm {
namespace bolt {

struct MMapEventsTestHelper : PreAggregatedTestHelper {
  void SetUp() override {
    PreAggregatedTestHelper::SetUp();
    initializeBOLTForX86();
    BC->setFilename("prog");
    BC->setFileBuildID(BuildID);
    BC->SegmentMapInfo[0x401000] =
        SegmentInfo{0x401000, 0x1000, 0x1000, 0x1000, 0x1000, true, false};
    opts::IgnoreBuildID = false;
  }

  static constexpr StringRef BuildID =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  DenseSet<uint64_t> parseMMapPIDs(StringRef Input) {
    DataAggregator DA("<pseudo input>");
    DA.BC = BC.get();
    DA.setParsingBuffer(Input);
    EXPECT_FALSE(DA.parseMMapEvents());
    DenseSet<uint64_t> PIDs;
    for (const auto &KV : DA.BinaryMMapInfo)
      PIDs.insert(KV.first);
    return PIDs;
  }
};

} // namespace bolt
} // namespace llvm

TEST_F(MMapEventsTestHelper, MatchWithouBuildId) {
  // Profiles recorded without build-ids match mappings by file name.
  StringRef Input =
      "prog 100 160514.108604: PERF_RECORD_MMAP2 100/100: [0x401000(0x1000) @ "
      "0x1000 00:00 0 0]: r-xp /pkg/mmapptest/1/prog\n"
      "prog 200 160514.108605: PERF_RECORD_MMAP2 200/200: [0x401000(0x1000) @ "
      "0x1000 00:00 0 0]: r-xp /pkg/mmapptest/2/prog\n";
  testing::internal::CaptureStderr();
  DenseSet<uint64_t> PIDs = parseMMapPIDs(Input);
  std::string CapturedStderr = testing::internal::GetCapturedStderr();
  EXPECT_EQ(PIDs.size(), 2u);
  EXPECT_TRUE(PIDs.count(100));
  EXPECT_TRUE(PIDs.count(200));
}

TEST_F(MMapEventsTestHelper, MatchWithBuildId) {
  BC->setFilename("prog2");
  StringRef Input =
      "prog 100 160514.108604: PERF_RECORD_MMAP2 100/100: [0x401000(0x1000) @ "
      "0x1000 <aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa>]: r-xp "
      "/pkg/mmapptest/1/prog\n"
      "prog 200 160514.108605: PERF_RECORD_MMAP2 200/200: [0x401000(0x1000) @ "
      "0x1000 <bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb>]: r-xp "
      "/pkg/mmapptest/2/prog\n";
  DenseSet<uint64_t> PIDs = parseMMapPIDs(Input);
  EXPECT_EQ(PIDs.size(), 1u);
  EXPECT_TRUE(PIDs.count(100));
  EXPECT_FALSE(PIDs.count(200));
}

TEST_F(MMapEventsTestHelper, MatchIgnoreBuildId) {
  StringRef Input =
      "prog 100 160514.108604: PERF_RECORD_MMAP2 100/100: [0x401000(0x1000) @ "
      "0x1000 <aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa>]: r-xp "
      "/pkg/mmapptest/1/prog\n"
      "prog 200 160514.108605: PERF_RECORD_MMAP2 200/200: [0x401000(0x1000) @ "
      "0x1000 <bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb>]: r-xp "
      "/pkg/mmapptest/2/prog\n";
  opts::IgnoreBuildID = true;
  DenseSet<uint64_t> PIDs = parseMMapPIDs(Input);
  opts::IgnoreBuildID = false;
  EXPECT_EQ(PIDs.size(), 2u);
  EXPECT_TRUE(PIDs.count(100));
  EXPECT_TRUE(PIDs.count(200));
}

namespace llvm {
namespace bolt {

/// Fixture that adds X86 BinaryContext initialization on top of
/// PreAggregatedTestHelper.
struct PreAggregatedX86TestHelper : PreAggregatedTestHelper {
  void SetUp() override {
    PreAggregatedTestHelper::SetUp();
    initializeBOLTForX86();
  }
};

} // namespace bolt
} // namespace llvm

TEST_F(PreAggregatedX86TestHelper, BranchEntry) {
  // B <from> <to> <count> <mispred>
  // Trace: {from, to, BR_ONLY}
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("B 4b196f 4b19e0 2 3\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.From, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
  EXPECT_EQ(Traces[0].second.TakenCount, 2u);
  EXPECT_EQ(Traces[0].second.MispredCount, 3u);
}

TEST_F(PreAggregatedX86TestHelper, FallThrough) {
  // F <from> <to> <count>
  // Trace: {FT_ONLY, from, to}
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("F 4b196f 4b19e0 5\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, Trace::FT_ONLY);
  EXPECT_EQ(Traces[0].first.From, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.To, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].second.TakenCount, 5u);
}

TEST_F(PreAggregatedX86TestHelper, FallThroughExternalOrigin) {
  // f <from> <to> <count>
  // Trace: {FT_EXTERNAL_ORIGIN, from, to}
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("f 4b196f 4b19e0 3\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, Trace::FT_EXTERNAL_ORIGIN);
  EXPECT_EQ(Traces[0].first.From, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.To, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].second.TakenCount, 3u);
}

TEST_F(PreAggregatedX86TestHelper, FallThroughExternalReturn) {
  // r <from> <to> <count>
  // Trace: {FT_EXTERNAL_RETURN, from, to}
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("r 4b196f 4b19e0 7\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, Trace::FT_EXTERNAL_RETURN);
  EXPECT_EQ(Traces[0].first.From, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.To, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].second.TakenCount, 7u);
}

TEST_F(PreAggregatedX86TestHelper, TraceEntry) {
  // T <branch> <from> <to> <count>
  // Trace: {branch, from, to}
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("T 4b196f 4b19e0 4b19ef 2\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.From, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].first.To, 0x4b19efULL);
  EXPECT_EQ(Traces[0].second.TakenCount, 2u);
}

TEST_F(PreAggregatedX86TestHelper, ReturnEntry) {
  // R <branch> <from> <to> <count>
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("R 4b196f 4b19e0 4b19ef 4\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.From, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].first.To, 0x4b19efULL);
  EXPECT_EQ(Traces[0].second.TakenCount, 4u);
}

TEST_F(PreAggregatedX86TestHelper, OptionalMispredField) {
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("B 100 200 1\n"
                        "T 300 400 500 3 4\n"
                        "R 600 700 800 5 6\n",
                        Traces);
  ASSERT_EQ(Traces.size(), 3u);

  EXPECT_EQ(Traces[0].first.Branch, 0x100ULL);
  EXPECT_EQ(Traces[0].first.From, 0x200ULL);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
  EXPECT_EQ(Traces[0].second.TakenCount, 1u);
  EXPECT_EQ(Traces[0].second.MispredCount, 0u);

  EXPECT_EQ(Traces[1].first.Branch, 0x300ULL);
  EXPECT_EQ(Traces[1].first.From, 0x400ULL);
  EXPECT_EQ(Traces[1].first.To, 0x500ULL);
  EXPECT_EQ(Traces[1].second.TakenCount, 3u);
  EXPECT_EQ(Traces[1].second.MispredCount, 4u);

  EXPECT_EQ(Traces[2].first.Branch, 0x600ULL);
  EXPECT_EQ(Traces[2].first.From, 0x700ULL);
  EXPECT_EQ(Traces[2].first.To, 0x800ULL);
  EXPECT_EQ(Traces[2].second.TakenCount, 5u);
  EXPECT_EQ(Traces[2].second.MispredCount, 6u);
}

TEST_F(PreAggregatedX86TestHelper, TraceWithNeg1AsBROnly) {
  // T entry with -1 as fall-through target: parsed as BR_ONLY.
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("T 4b196f 4b19e0 -1 2\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.Branch, 0x4b196fULL);
  EXPECT_EQ(Traces[0].first.From, 0x4b19e0ULL);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
  EXPECT_EQ(Traces[0].second.TakenCount, 2u);
}

TEST_F(PreAggregatedX86TestHelper, TraceWithFFFAsBROnly) {
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("T 4b196f 4b19e0 ffffffffffffffff 2\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
}

TEST_F(PreAggregatedX86TestHelper, TraceWithBuildIdNeg1) {
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("T 4b196f 4b19e0 deadbeef:-1 2\n", Traces);
  ASSERT_EQ(Traces.size(), 1u);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
}

TEST_F(PreAggregatedX86TestHelper, MultipleEntries) {
  std::vector<std::pair<Trace, TakenBranchInfo>> Traces;
  parseAndCollectTraces("B 100 200 1 0\n"
                        "F 300 400 2\n"
                        "f 500 600 3\n"
                        "r 700 800 4\n"
                        "T 900 a00 b00 5\n",
                        Traces);
  ASSERT_EQ(Traces.size(), 5u);
  EXPECT_EQ(Traces[0].first.To, Trace::BR_ONLY);
  EXPECT_EQ(Traces[0].second.TakenCount, 1u);
  EXPECT_EQ(Traces[1].first.Branch, Trace::FT_ONLY);
  EXPECT_EQ(Traces[1].second.TakenCount, 2u);
  EXPECT_EQ(Traces[2].first.Branch, Trace::FT_EXTERNAL_ORIGIN);
  EXPECT_EQ(Traces[2].second.TakenCount, 3u);
  EXPECT_EQ(Traces[3].first.Branch, Trace::FT_EXTERNAL_RETURN);
  EXPECT_EQ(Traces[3].second.TakenCount, 4u);
  EXPECT_EQ(Traces[4].first.Branch, 0x900ULL);
  EXPECT_EQ(Traces[4].first.From, 0xa00ULL);
  EXPECT_EQ(Traces[4].first.To, 0xb00ULL);
  EXPECT_EQ(Traces[4].second.TakenCount, 5u);
}

#endif
namespace llvm {
namespace bolt {

TEST_F(DataAggregatorTester, BranchSamplesAndParseErrors) {
  prepareContext();
  ASSERT_FALSE(parseBranches("1\n"
                             "1 0x1010/0x1000/P/-/-/12/COND/- \n"));
  EXPECT_TRUE(parseBranches("1 0x1010/0x1000/P/-/-\n"));
}

TEST_F(DataAggregatorTester, PrefetchSamplingAttributes) {
  StringRef Valid =
      "arm_spe_0/load_filter=1/: type: 25, config: 0x200000001, "
      "{ sample_period, sample_freq }: 1024, sample_type: IP|TID\n";
  EXPECT_FALSE(bool(samplingAttrs(Valid)));
  for (StringRef Invalid :
       {"cycles: config: 0, { sample_period, sample_freq }: 1024\n",
        "arm_spe_0//: config: 0x200000001, "
        "{ sample_period, sample_freq }: 1024, config2: 1\n",
        "arm_spe_0//: config: 0x200000001, "
        "{ sample_period, sample_freq }: 1024, freq: 1\n"}) {
    SCOPED_TRACE(Invalid.str());
    auto E = samplingAttrs(Invalid);
    EXPECT_TRUE(bool(E));
    consumeError(std::move(E));
  }
  // Relocate the code PC, while retaining the data VA independently.
  auto *Option = cl::getRegisteredOptions()["cache-miss"];
  ASSERT_FALSE(Option->addOccurrence(0, "cache-miss", "llc_miss"));
  ASSERT_FALSE(bool(samplingAttrs(Valid)));
  EXPECT_FALSE(bool(mergeSamplingAttrs(1024)));
  auto Mismatch = mergeSamplingAttrs(2048);
  EXPECT_TRUE(bool(Mismatch));
  consumeError(std::move(Mismatch));
  EXPECT_EQ(relocatedMemory(),
            std::make_pair(uint64_t(0x1010), uint64_t(0x800000)));
  EXPECT_TRUE(parseMemory("PERF_RECORD_LOST 10\n"));
  EXPECT_TRUE(parseMemory("instruction trace error: lost data\n"));
  Option->reset();
}

} // namespace bolt
} // namespace llvm
