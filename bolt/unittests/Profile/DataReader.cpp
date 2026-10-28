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
  EXPECT_EQ(First->Cycles.average(), 0);
  auto Second = Reader.parseBranchInfo();
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->Branches, 5);
  EXPECT_EQ(Second->Cycles.average(), 0);
}

TEST(DataReaderTest, BranchWithCycles) {
  TestDataReader Reader;
  Reader.setParsingBuffer("1 loop 10 1 loop 0 2 10 107  \n");
  auto Branch = Reader.parseBranchInfo();
  ASSERT_TRUE(Branch);
  EXPECT_EQ(Branch->Branches, 10);
  EXPECT_EQ(Branch->Mispreds, 2);
  EXPECT_EQ(Branch->Cycles.average(), 107);
}

TEST(DataReaderTest, InvalidCycleField) {
  TestDataReader Reader;
  for (StringRef Input :
       {"1 loop 10 1 loop 0 2 10 bad\n", "1 loop 10 1 loop 0 2 10 107 88\n",
        "1 loop 10 1 loop 0 2 10 107"}) {
    SCOPED_TRACE(Input.str());
    Reader.setParsingBuffer(Input);
    EXPECT_FALSE(Reader.parseBranchInfo());
  }
}
