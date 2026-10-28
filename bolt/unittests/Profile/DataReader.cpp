//===- bolt/unittests/Profile/DataReader.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Profile/DataReader.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;

namespace {
struct TestDataReader : DataReader {
  TestDataReader() : DataReader("<pseudo input>") {}
  using DataReader::parseBranchInfo;
  void enablePrefetchProfile() { PrefetchProfile.emplace(); }
};
} // namespace

TEST(DataReaderTest, BranchWithoutCycles) {
  TestDataReader Reader;
  Reader.setParsingBuffer("1 loop 10 1 loop 0 2 10\n"
                          "1 loop 20 1 loop 0 1 5   \n");
  auto First = Reader.parseBranchInfo();
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Branches, 10);
  EXPECT_EQ(First->Mispreds, 2);
  EXPECT_EQ(First->Cycles.Total, 0);
  EXPECT_EQ(First->Cycles.Samples, 0);
  auto Second = Reader.parseBranchInfo();
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->Branches, 5);
  EXPECT_EQ(Second->Cycles.Total, 0);
  EXPECT_EQ(Second->Cycles.Samples, 0);
}

TEST(DataReaderTest, BranchWithCycles) {
  TestDataReader Reader;
  Reader.enablePrefetchProfile();
  Reader.setParsingBuffer("1 loop 10 1 loop 0 2 10 107 2  \n");
  auto Branch = Reader.parseBranchInfo();
  ASSERT_TRUE(Branch);
  EXPECT_EQ(Branch->Branches, 10);
  EXPECT_EQ(Branch->Mispreds, 2);
  EXPECT_EQ(Branch->Cycles.Total, 107);
  EXPECT_EQ(Branch->Cycles.Samples, 2);
}

TEST(DataReaderTest, InvalidCycleField) {
  TestDataReader Reader;
  Reader.enablePrefetchProfile();
  for (StringRef Input :
       {"1 loop 10 1 loop 0 2 10 bad 1\n", "1 loop 10 1 loop 0 2 10\n",
        "1 loop 10 1 loop 0 2 10 107\n", "1 loop 10 1 loop 0 2 10 107 0\n",
        "1 loop 10 1 loop 0 2 10 -1 1\n", "1 loop 10 1 loop 0 2 10 0 -2\n",
        "1 loop 10 1 loop 0 2 10 107 2 extra\n",
        "1 loop 10 1 loop 0 2 10 107 2"}) {
    SCOPED_TRACE(Input.str());
    Reader.setParsingBuffer(Input);
    EXPECT_FALSE(Reader.parseBranchInfo());
  }
}

TEST(DataReaderTest, ExtraBranchFields) {
  TestDataReader Reader;
  for (StringRef Input :
       {"1 loop 10 1 loop 0 2 10 107\n", "1 loop 10 1 loop 0 2 10 107 2\n"}) {
    SCOPED_TRACE(Input.str());
    Reader.setParsingBuffer(Input);
    EXPECT_FALSE(Reader.parseBranchInfo());
  }
}

TEST(PrefetchProfileTest, Header) {
  auto Header = PrefetchProfileHeader::parse(
      "bolt-prefetch llc_miss AABB 1024 200000001 0 0");
  ASSERT_TRUE(bool(Header)) << toString(Header.takeError());
  EXPECT_EQ(Header->Event, "llc_miss");
  EXPECT_EQ(Header->BuildID, "aabb");
  EXPECT_EQ(Header->Period, 1024u);
  EXPECT_EQ(Header->Config, 0x200000001ULL);
  EXPECT_EQ(Header->Config1, 0u);
  EXPECT_EQ(Header->Config2, 0u);
  std::string Text;
  raw_string_ostream OS(Text);
  Header->print(OS);
  EXPECT_EQ(Text, "bolt-prefetch llc_miss aabb 1024 200000001 0 0\n");
}

TEST(PrefetchProfileTest, InvalidHeader) {
  for (StringRef Input :
       {"bolt-prefetch llc_miss",
        "bolt-prefetch llc_miss aabb 1024 200000001 0",
        "bolt-prefetch llc_miss aabb 1024 200000001 0 0 extra",
        "bolt-prefetch unknown aabb 1024 200000001 0 0",
        "bolt-prefetch llc_miss aab 1024 200000001 0 0",
        "bolt-prefetch llc_miss aazz 1024 200000001 0 0",
        "bolt-prefetch llc_miss aabb 0 200000001 0 0",
        "bolt-prefetch llc_miss aabb 1024 1 0 0",
        "bolt-prefetch llc_miss aabb 1024 600000001 0 0",
        "bolt-prefetch llc_miss aabb 1024 200000001 1 0",
        "bolt-prefetch llc_miss aabb 1024 200000001 0 1"}) {
    SCOPED_TRACE(Input.str());
    auto Header = PrefetchProfileHeader::parse(Input);
    EXPECT_FALSE(bool(Header));
    if (!Header)
      consumeError(Header.takeError());
  }
}
