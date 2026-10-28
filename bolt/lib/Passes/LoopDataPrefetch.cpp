//===- LoopDataPrefetch.cpp - Loop data prefetching -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/LoopDataPrefetch.h"
#include "bolt/Passes/LivenessAnalysis.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#define DEBUG_TYPE "loop-data-prefetch"

using namespace llvm;
using namespace llvm::bolt;

namespace opts {
extern cl::OptionCategory BoltOptCategory;
static cl::opt<uint64_t> PrefetchLatency(
    "loop-prefetch-latency", cl::init(0),
    cl::desc(
        "calibrated latency to hide in CPU cycles (0 disables prediction)"),
    cl::cat(BoltOptCategory));
static cl::opt<uint64_t> PrefetchIterationCycles(
    "loop-prefetch-iteration-cycles", cl::init(0),
    cl::desc("calibrated cycles per loop iteration (0 uses profile intervals)"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxDistance(
    "loop-prefetch-max-distance", cl::init(1u << 20),
    cl::desc("maximum predicted distance; larger predictions are skipped"),
    cl::cat(BoltOptCategory));
static cl::opt<uint64_t> PrefetchMinMisses(
    "loop-prefetch-min-misses", cl::init(0),
    cl::desc("minimum sampled misses for an automatic candidate"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned>
    CacheMissCutOff("cache-miss-cutoff", cl::Hidden, cl::init(2000),
                    cl::desc("minimum cache miss rate in units of 1/10000"),
                    cl::cat(BoltOptCategory));
static cl::opt<std::string> PrefetchHint("loop-prefetch-hint",
                                         cl::init("pldl1keep"),
                                         cl::desc("AArch64 prefetch hint"),
                                         cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxInstructions(
    "loop-prefetch-max-instructions", cl::init(8),
    cl::desc("maximum added instructions per candidate"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned>
    PrefetchMaxPerLoop("loop-prefetch-max-per-loop", cl::init(2),
                       cl::desc("maximum prefetch requests per loop iteration"),
                       cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxPerFunction(
    "loop-prefetch-max-per-function", cl::init(32),
    cl::desc("maximum static prefetch requests per function"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxLoopInstructions(
    "loop-prefetch-max-loop-instructions", cl::init(32),
    cl::desc("maximum added instructions per loop"), cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxFunctionInstructions(
    "loop-prefetch-max-function-instructions", cl::init(256),
    cl::desc("maximum added instructions per function"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchMaxRegisters(
    "loop-prefetch-max-registers", cl::init(2),
    cl::desc("maximum dead scratch registers per candidate (0 to 2)"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchCacheLineSize(
    "loop-prefetch-cache-line-size", cl::init(0),
    cl::desc("cache line bytes for request throttling (0 disables)"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchAnalysisLimit(
    "loop-prefetch-analysis-limit", cl::init(20000),
    cl::desc("maximum original instructions analyzed per function"),
    cl::cat(BoltOptCategory));
static cl::opt<bool> PrefetchDiagnostics(
    "loop-prefetch-diagnostics",
    cl::desc("report loop prefetch decisions by original PC"),
    cl::cat(BoltOptCategory));
static cl::opt<bool> PrefetchIndirect(
    "loop-prefetch-indirect",
    cl::desc("enable guarded index-array prefetches for ordinary memory"),
    cl::cat(BoltOptCategory));
static cl::opt<bool> PrefetchBatch(
    "loop-prefetch-batch",
    cl::desc("prefetch constant inner-loop ranges at the outer-loop entry"),
    cl::cat(BoltOptCategory));
static cl::opt<unsigned> PrefetchBatchMaxBytes(
    "loop-prefetch-batch-max-bytes", cl::init(256),
    cl::desc("maximum requested cache-line bytes per outer-loop iteration"),
    cl::cat(BoltOptCategory));
} // namespace opts

namespace {
struct PrefetchPolicy {
  std::optional<unsigned> Distance;
  StringRef Hint = opts::PrefetchHint;
  unsigned Budget = opts::PrefetchMaxInstructions;
  StringRef Source = "profile";
  uint64_t IterationTotal = 0, IterationSamples = 0;
};

struct PrefetchHints {
  std::unique_ptr<MemoryBuffer> Buffer;
  DenseMap<uint64_t, PrefetchPolicy> Functions, PCs;
};

Expected<PrefetchHints> readHints(BinaryContext &BC) {
  PrefetchHints Hints;
  if (opts::LoopPrefetchHints.empty())
    return Hints;
  auto Buffer = MemoryBuffer::getFile(opts::LoopPrefetchHints);
  if (!Buffer)
    return createFatalBOLTError(Twine("cannot read prefetch hints: ") +
                                Buffer.getError().message());
  Hints.Buffer = std::move(*Buffer);
  line_iterator Line(*Hints.Buffer, true, '#');
  auto ErrorAtLine = [&]() {
    return createFatalBOLTError(Twine("invalid prefetch hints at line ") +
                                Twine(Line.line_number()));
  };
  if (Line.is_at_eof())
    return ErrorAtLine();
  SmallVector<StringRef, 8> Fields;
  SplitString(*Line, Fields);
  if (Fields.size() != 3 || Fields[0] != "bolt-prefetch-hints" ||
      Fields[1] != "1")
    return ErrorAtLine();
  auto BuildID = BC.getFileBuildID();
  if (!BuildID || !Fields[2].equals_insensitive(*BuildID))
    return createFatalBOLTError("prefetch hints build-id does not match input");
  for (++Line; !Line.is_at_eof(); ++Line) {
    Fields.clear();
    SplitString(*Line, Fields);
    const bool IsPC = !Fields.empty() && Fields[0] == "pc";
    if (Fields.size() != (IsPC ? 6u : 5u) || (!IsPC && Fields[0] != "function"))
      return ErrorAtLine();
    uint64_t Address;
    unsigned Distance, Budget;
    StringRef AddressText = Fields[1];
    AddressText.consume_front("0x");
    const unsigned D = IsPC ? 3 : 2;
    if (AddressText.getAsInteger(16, Address) ||
        Fields[D].getAsInteger(10, Distance) || Distance > (1u << 20) ||
        Fields[D + 2].getAsInteger(10, Budget) ||
        !BC.MIB->isPrefetchHint(Fields[D + 1]))
      return ErrorAtLine();
    BinaryFunction *BF = IsPC ? BC.getBinaryFunctionContainingAddress(Address)
                              : BC.getBinaryFunctionAtAddress(Address);
    if (!BF || (IsPC && !BF->hasCFG()))
      return ErrorAtLine();
    if (IsPC) {
      const uint64_t Offset = Address - BF->getAddress();
      const MCInst *Load = BF->getInstructionAtOffset(Offset);
      if (!Load || BC.MIB->getOffset(*Load) != Offset ||
          !BC.MIB->analyzeLoopMemory(*Load) ||
          BC.MII->getName(Load->getOpcode()) != Fields[2])
        return ErrorAtLine();
    }
    PrefetchPolicy Policy;
    Policy.Distance = Distance;
    Policy.Hint = Fields[D + 1];
    Policy.Budget = Budget;
    Policy.Source = IsPC ? "pc hint" : "function hint";
    if (!(IsPC ? Hints.PCs : Hints.Functions)
             .try_emplace(Address, Policy)
             .second)
      return ErrorAtLine();
  }
  return Hints;
}

/// Registers used by any CFI rule must retain their values at every new PC.
/// Escaped expressions are opaque, so skip that function rather than decode
/// another DWARF expression language in this pass.
bool excludeCFIRegisters(BinaryFunction &BF, BitVector &Available) {
  auto &BC = BF.getBinaryContext();
  auto Exclude = [&](unsigned DwarfReg) {
    if (auto Reg = BC.MRI->getLLVMRegNum(DwarfReg, false))
      Available.reset(BC.MIB->getAliases(*Reg));
  };
  auto Check = [&](const MCCFIInstruction &CFI) {
    switch (CFI.getOperation()) {
    case MCCFIInstruction::OpEscape:
      return false;
    case MCCFIInstruction::OpRegister:
      Exclude(CFI.getRegister2());
      [[fallthrough]];
    case MCCFIInstruction::OpDefCfa:
    case MCCFIInstruction::OpLLVMDefAspaceCfa:
    case MCCFIInstruction::OpDefCfaRegister:
    case MCCFIInstruction::OpOffset:
    case MCCFIInstruction::OpRelOffset:
    case MCCFIInstruction::OpRestore:
    case MCCFIInstruction::OpUndefined:
    case MCCFIInstruction::OpSameValue:
      Exclude(CFI.getRegister());
      break;
    default:
      break;
    }
    return true;
  };
  return llvm::all_of(make_range(BF.cie_begin(), BF.cie_end()), Check) &&
         llvm::all_of(BF.getFDEProgram(), Check);
}

/// Removing backedges must leave an acyclic iteration. This also excludes
/// irreducible inner cycles that natural-loop discovery cannot represent.
bool hasAcyclicIteration(BinaryLoop &Loop) {
  DenseMap<BinaryBasicBlock *, unsigned> InDegree;
  for (BinaryBasicBlock *BB : Loop.blocks())
    InDegree[BB] = 0;
  for (BinaryBasicBlock *BB : Loop.blocks())
    for (BinaryBasicBlock *Succ : BB->successors())
      if (Succ != Loop.getHeader() && Loop.contains(Succ))
        ++InDegree[Succ];
  SmallVector<BinaryBasicBlock *, 16> Ready;
  for (auto [BB, Degree] : InDegree)
    if (!Degree)
      Ready.push_back(BB);
  unsigned Count = 0;
  while (!Ready.empty()) {
    BinaryBasicBlock *BB = Ready.pop_back_val();
    ++Count;
    for (BinaryBasicBlock *Succ : BB->successors())
      if (Succ != Loop.getHeader() && Loop.contains(Succ) && !--InDegree[Succ])
        Ready.push_back(Succ);
  }
  return Count == Loop.getNumBlocks();
}

std::optional<int64_t> getStride(BinaryFunction &BF, BinaryLoop &Loop,
                                 ArrayRef<BinaryBasicBlock *> Latches,
                                 MCPhysReg Reg) {
  if (!Reg)
    return 0;
  const auto &MIB = *BF.getBinaryContext().MIB;
  int64_t Stride = 0;
  for (BinaryBasicBlock *BB : Loop.blocks()) {
    for (const MCInst &MI : *BB) {
      BitVector Clobbered(BF.getBinaryContext().MRI->getNumRegs());
      MIB.getWrittenRegs(MI, Clobbered);
      if (!Clobbered.anyCommon(MIB.getAliases(Reg, true)))
        continue;
      int64_t Step;
      LLVM_DEBUG(dbgs() << "Checking recurrence "
                        << BF.getBinaryContext().MRI->getName(Reg) << " at "
                        << BF.getBinaryContext().MII->getName(MI.getOpcode())
                        << '\n');
      if (!MIB.getLoopRegisterUpdate(MI, Reg, Step) ||
          !llvm::all_of(Latches,
                        [&](BinaryBasicBlock *Latch) {
                          return BF.getDomTree().dominates(BB, Latch);
                        }) ||
          AddOverflow(Stride, Step, Stride))
        return std::nullopt;
    }
  }
  LLVM_DEBUG(dbgs() << "Stride " << BF.getBinaryContext().MRI->getName(Reg)
                    << " = " << Stride << '\n');
  return Stride;
}

/// Follow address definitions through a bounded chain of unique predecessors.
/// Query sources before each assignment; stop at joins and the loop header.
std::optional<int64_t>
getAddressStride(const BinaryBasicBlock &BB, unsigned Index, MCPhysReg Reg,
                 const BinaryLoop &Loop,
                 function_ref<std::optional<int64_t>(MCPhysReg)> GetStride,
                 unsigned Depth = 4) {
  if (auto Stride = GetStride(Reg))
    return Stride;
  if (!Depth)
    return std::nullopt;
  const auto &MIB = *BB.getFunction()->getBinaryContext().MIB;
  const BitVector &Aliases = MIB.getAliases(Reg, true);
  const BinaryBasicBlock *Block = &BB;
  for (unsigned Limit = 32; Limit; --Limit) {
    if (!Index) {
      if (Block == Loop.getHeader() || Block->pred_size() != 1)
        return std::nullopt;
      Block = *Block->pred_begin();
      if (!Loop.contains(Block))
        return std::nullopt;
      Index = Block->size();
      continue;
    }
    const MCInst &MI = *(Block->begin() + --Index);
    BitVector Written(Aliases.size());
    MIB.getWrittenRegs(MI, Written);
    if (!Written.anyCommon(Aliases))
      continue;
    auto A = MIB.analyzeLoopRegisterAssignment(MI, Reg);
    if (!A)
      return std::nullopt;
    auto Base =
        getAddressStride(*Block, Index, A->Base, Loop, GetStride, Depth - 1);
    auto Offset =
        getAddressStride(*Block, Index, A->Index, Loop, GetStride, Depth - 1);
    int64_t Scaled, Stride;
    if (!Base || !Offset || A->Shift >= 63 ||
        MulOverflow(*Offset, int64_t(1) << A->Shift, Scaled))
      return std::nullopt;
    if (A->SubtractIndex ? SubOverflow(*Base, Scaled, Stride)
                         : AddOverflow(*Base, Scaled, Stride))
      return std::nullopt;
    return Stride;
  }
  return std::nullopt;
}

/// The loaded pointer itself is the address for the next iteration. Reuse it
/// after the load, without speculating an additional dereference.
std::optional<MCPhysReg>
getNextPointer(BinaryFunction &BF, BinaryLoop &Loop,
               ArrayRef<BinaryBasicBlock *> Latches, const BinaryBasicBlock &BB,
               const MCInst &Load,
               const MCPlusBuilder::LoopMemoryAccess &Access) {
  if (!Access.Dest || Access.Index || Access.HasWriteback || Access.Size != 8 ||
      Access.Elements != 1)
    return std::nullopt;
  const auto &MIB = *BF.getBinaryContext().MIB;
  const BitVector &Aliases = MIB.getAliases(Access.Base, true);
  BitVector Written(Aliases.size());
  const BitVector &DestAliases = MIB.getAliases(Access.Dest, true);
  if (!llvm::all_of(Latches, [&](BinaryBasicBlock *Latch) {
        return BF.getDomTree().dominates(&BB, Latch);
      }))
    return std::nullopt;
  bool Updated = Access.Dest == Access.Base;
  for (const BinaryBasicBlock *Block : Loop.blocks()) {
    for (const MCInst &MI : *Block) {
      if (&MI == &Load)
        continue;
      Written.reset();
      MIB.getWrittenRegs(MI, Written);
      if (Written.anyCommon(DestAliases))
        return std::nullopt;
      if (!Written.anyCommon(Aliases))
        continue;
      MCPhysReg From, To;
      if (Updated || !MIB.isRegToRegMove(MI, From, To) || From != Access.Dest ||
          To != Access.Base || !BF.getDomTree().dominates(&BB, Block) ||
          (Block == &BB && &MI < &Load) ||
          !llvm::all_of(Latches, [&](BinaryBasicBlock *Latch) {
            return BF.getDomTree().dominates(Block, Latch);
          }))
        return std::nullopt;
      Updated = true;
    }
  }
  return Updated ? std::optional<MCPhysReg>(Access.Dest) : std::nullopt;
}

struct IndirectLoopInfo {
  MCPhysReg Count;
  unsigned UpdateIndex;
  SmallVector<BinaryBasicBlock *, 4> Blocks;
};

MCInst *getConditionalTerminator(BinaryBasicBlock &BB) {
  const auto &MIB = *BB.getFunction()->getBinaryContext().MIB;
  const MCSymbol *Taken = nullptr, *Fallthrough = nullptr;
  MCInst *Cond = nullptr, *Uncond = nullptr;
  if (!MIB.analyzeBranch(BB.begin(), BB.end(), Taken, Fallthrough, Cond,
                         Uncond))
    return nullptr;
  return Cond;
}

std::optional<unsigned> getCountDownUpdate(const BinaryBasicBlock &BB,
                                           const MCInst &Branch) {
  const auto &MIB = *BB.getFunction()->getBinaryContext().MIB;
  unsigned Index = &Branch - &BB.front();
  while (Index && (MIB.isCFI(*(BB.begin() + Index - 1)) ||
                   MIB.isNoop(*(BB.begin() + Index - 1))))
    --Index;
  return Index ? std::optional<unsigned>(Index - 1) : std::nullopt;
}

/// Follow a straight countdown iteration with no early exit. Reject stores
/// rather than build alias analysis for the additional index load.
std::optional<IndirectLoopInfo> analyzeIndirectLoop(BinaryLoop &Loop) {
  BinaryBasicBlock *Header = Loop.getHeader();
  if (Header->pred_size() != 2)
    return std::nullopt;
  SmallVector<BinaryBasicBlock *, 4> Blocks;
  BinaryBasicBlock *BB = Header;
  while (Blocks.size() < Loop.getNumBlocks()) {
    Blocks.push_back(BB);
    if (BB->succ_size() != 1)
      break;
    BB = *BB->succ_begin();
    if (BB == Header || !Loop.contains(BB) || BB->pred_size() != 1)
      return std::nullopt;
  }
  if (Blocks.size() != Loop.getNumBlocks() || BB->succ_size() != 2 ||
      !BB->isSuccessor(Header))
    return std::nullopt;
  for (BinaryBasicBlock *Succ : BB->successors())
    if (Succ != Header && Loop.contains(Succ))
      return std::nullopt;
  const auto &MIB = *BB->getFunction()->getBinaryContext().MIB;
  MCInst *Backedge = getConditionalTerminator(*BB);
  if (!Backedge || MIB.getTargetSymbol(*Backedge) != Header->getLabel())
    return std::nullopt;
  auto Update = getCountDownUpdate(*BB, *Backedge);
  if (!Update)
    return std::nullopt;
  BinaryBasicBlock *Entry = *Header->pred_begin();
  if (Loop.contains(Entry))
    Entry = *std::next(Header->pred_begin());
  if (Loop.contains(Entry) || Entry->succ_size() != 2 ||
      !BB->getFunction()->getDomTree().dominates(Entry, Header))
    return std::nullopt;
  MCInst *EntryBranch = getConditionalTerminator(*Entry);
  if (!EntryBranch)
    return std::nullopt;
  bool EntryOnZero;
  MCPhysReg Count = MIB.getLoopCountDownReg(*(BB->begin() + *Update), *Backedge,
                                            *EntryBranch, EntryOnZero);
  if (!Count ||
      EntryOnZero == (MIB.getTargetSymbol(*EntryBranch) == Header->getLabel()))
    return std::nullopt;
  const BitVector &Aliases = MIB.getAliases(Count, true);
  const BitVector &Stack = MIB.getAliases(MIB.getStackPointer(), true);
  const MCInst *CounterUpdate = &*(BB->begin() + *Update);
  for (BinaryBasicBlock *Block : Blocks)
    for (const MCInst &MI : *Block) {
      BitVector Written(Aliases.size());
      MIB.getWrittenRegs(MI, Written);
      if (MIB.mayStore(MI) || Written.anyCommon(Stack) ||
          (&MI != CounterUpdate && Written.anyCommon(Aliases)))
        return std::nullopt;
    }
  return IndirectLoopInfo{Count, *Update, std::move(Blocks)};
}

struct BatchLoopInfo {
  BinaryLoop *Outer;
  BinaryBasicBlock *Entry, *Latch;
  unsigned EntryIndex;
  uint64_t Iterations;
};

/// Only a straight entry -> constant single-block inner -> outer latch is
/// admitted. Every inner access then executes exactly Iterations times.
std::optional<BatchLoopInfo> analyzeBatchLoop(BinaryLoop &Inner) {
  BinaryLoop *Outer = Inner.getParentLoop();
  if (!Outer || Inner.getNumBlocks() != 1 || Outer->getNumBlocks() != 3 ||
      Outer->getSubLoops().size() != 1)
    return std::nullopt;
  BinaryBasicBlock *Entry = Outer->getHeader(), *Body = Inner.getHeader();
  SmallVector<BinaryBasicBlock *, 2> Latches;
  Outer->getLoopLatches(Latches);
  if (Latches.size() != 1)
    return std::nullopt;
  BinaryBasicBlock *Latch = Latches.front();
  if (Entry == Body || Latch == Entry || Latch == Body ||
      Entry->succ_size() != 1 || !Entry->isSuccessor(Body) ||
      Body->pred_size() != 2 || Body->succ_size() != 2 ||
      !Body->isSuccessor(Body) || !Body->isSuccessor(Latch) ||
      Latch->succ_size() != 2 || !Latch->isSuccessor(Entry))
    return std::nullopt;
  for (BinaryBasicBlock *Succ : Latch->successors())
    if (Succ != Entry && Outer->contains(Succ))
      return std::nullopt;
  const auto &MIB = *Body->getFunction()->getBinaryContext().MIB;
  MCInst *Backedge = getConditionalTerminator(*Body);
  if (!Backedge || MIB.getTargetSymbol(*Backedge) != Body->getLabel())
    return std::nullopt;
  auto Update = getCountDownUpdate(*Body, *Backedge);
  if (!Update)
    return std::nullopt;
  unsigned Anchor = 0;
  while (Anchor < Entry->size() &&
         !MIB.isTerminator(*(Entry->begin() + Anchor)))
    ++Anchor;
  for (unsigned I = 0; I < Anchor; ++I) {
    const MCInst &Init = *(Entry->begin() + I);
    auto Count =
        MIB.getLoopConstantCount(Init, *(Body->begin() + *Update), *Backedge);
    if (!Count)
      continue;
    const BitVector &Aliases = MIB.getAliases(Count->Reg, true);
    for (BinaryBasicBlock *BB : {Entry, Body}) {
      for (const MCInst &MI : *BB) {
        if (&MI == &Init || &MI == &*(Body->begin() + *Update))
          continue;
        BitVector Written(Aliases.size());
        MIB.getWrittenRegs(MI, Written);
        if (Written.anyCommon(Aliases))
          return std::nullopt;
      }
    }
    return BatchLoopInfo{Outer, Entry, Latch, Anchor, Count->Count};
  }
  return std::nullopt;
}

std::optional<int64_t> getBatchStride(const BinaryLoop &Inner,
                                      const BatchLoopInfo &Batch,
                                      MCPhysReg Reg) {
  const auto &MIB = *Batch.Entry->getFunction()->getBinaryContext().MIB;
  const BitVector &Aliases = MIB.getAliases(Reg, true);
  int64_t Stride = 0;
  for (BinaryBasicBlock *BB : Batch.Outer->blocks()) {
    for (const MCInst &MI : *BB) {
      BitVector Written(Aliases.size());
      MIB.getWrittenRegs(MI, Written);
      if (!Written.anyCommon(Aliases))
        continue;
      int64_t Step;
      if (!MIB.getLoopRegisterUpdate(MI, Reg, Step) ||
          (BB == Inner.getHeader() &&
           MulOverflow(Step, int64_t(Batch.Iterations), Step)) ||
          AddOverflow(Stride, Step, Stride))
        return std::nullopt;
    }
  }
  return Stride;
}

struct BatchAccess {
  MCPlusBuilder::LoopMemoryAccess Address;
  int64_t Delta;
  uint64_t Span;
  unsigned Requests;
};

std::optional<BatchAccess>
getBatchAccess(const BinaryBasicBlock &BB, unsigned Index,
               const MCPlusBuilder::LoopMemoryAccess &Access,
               const BatchLoopInfo &Loop, unsigned Distance,
               function_ref<std::optional<int64_t>(MCPhysReg)> GetInnerStride,
               function_ref<std::optional<int64_t>(MCPhysReg)> GetOuterStride) {
  if (Access.Index)
    return std::nullopt;
  const uint64_t Width = uint64_t(Access.Size) * Access.Elements;
  auto Step = GetInnerStride(Access.Base);
  if (!Step || (*Step < 0 ? -uint64_t(*Step) : uint64_t(*Step)) != Width ||
      Loop.Iterations > opts::PrefetchBatchMaxBytes / Width)
    return std::nullopt;
  const uint64_t Span = Loop.Iterations * Width;
  const uint64_t Line = opts::PrefetchCacheLineSize;
  // With unknown alignment, include both ends of the contiguous range.
  const uint64_t Requests = divideCeil(Span - 1, Line) + 1;
  if (Requests * Line > opts::PrefetchBatchMaxBytes)
    return std::nullopt;
  const auto &MIB = *BB.getFunction()->getBinaryContext().MIB;
  const BitVector &Aliases = MIB.getAliases(Access.Base, true);
  auto OuterStep = GetOuterStride(Access.Base);
  if (!OuterStep) {
    // A copied inner pointer advances with its source between parent
    // iterations. At the insertion point the copy has already executed.
    MCPhysReg Row = 0;
    for (BinaryBasicBlock *Block : Loop.Outer->blocks()) {
      if (Block == &BB)
        continue;
      for (const MCInst &MI : *Block) {
        BitVector Written(Aliases.size());
        MIB.getWrittenRegs(MI, Written);
        if (!Written.anyCommon(Aliases))
          continue;
        MCPhysReg From, To;
        if (Row || Block != Loop.Entry || !MIB.isRegToRegMove(MI, From, To) ||
            To != Access.Base || MIB.getAliasSized(From, 8) != From)
          return std::nullopt;
        Row = From;
      }
    }
    if (Row)
      OuterStep = GetOuterStride(Row);
  }
  int64_t Delta;
  if (!OuterStep || !*OuterStep ||
      MulOverflow(*OuterStep, int64_t(Distance), Delta))
    return std::nullopt;
  int64_t Prefix = 0;
  for (unsigned I = 0; I < Index; ++I) {
    const MCInst &MI = *(BB.begin() + I);
    BitVector Written(Aliases.size());
    MIB.getWrittenRegs(MI, Written);
    if (!Written.anyCommon(Aliases))
      continue;
    int64_t Update;
    if (!MIB.getLoopRegisterUpdate(MI, Access.Base, Update) ||
        AddOverflow(Prefix, Update, Prefix))
      return std::nullopt;
  }
  auto Address = Access;
  if (AddOverflow(Address.Offset, Prefix, Address.Offset) ||
      (*Step < 0 &&
       AddOverflow(Address.Offset, int64_t(Loop.Iterations - 1) * *Step,
                   Address.Offset)))
    return std::nullopt;
  Address.Size = Address.Elements = 1;
  Address.HasWriteback = false;
  Address.Writeback = 0;
  return BatchAccess{Address, Delta, Span, unsigned(Requests)};
}

std::optional<MCPlusBuilder::LoopIndirectAccess>
getIndirectAccess(const BinaryBasicBlock &BB, unsigned Index,
                  const MCPlusBuilder::LoopMemoryAccess &Target,
                  const IndirectLoopInfo &Loop, unsigned Distance,
                  function_ref<std::optional<int64_t>(MCPhysReg)> GetStride) {
  if (!Target.Index || Target.HasWriteback || Target.Elements != 1 ||
      (&BB == Loop.Blocks.back() && Index >= Loop.UpdateIndex))
    return std::nullopt;
  auto BaseStride = GetStride(Target.Base);
  if (!BaseStride || *BaseStride)
    return std::nullopt;
  const auto &MIB = *BB.getFunction()->getBinaryContext().MIB;
  const BitVector &Aliases = MIB.getAliases(Target.Index, true);
  auto Block = llvm::find(Loop.Blocks, &BB);
  BitVector Changed(Aliases.size());
  for (unsigned Limit = 32; Limit; --Limit) {
    if (!Index) {
      if (Block == Loop.Blocks.begin())
        return std::nullopt;
      --Block;
      Index = (*Block)->size();
      continue;
    }
    const MCInst &MI = *((*Block)->begin() + --Index);
    BitVector Written(Aliases.size());
    MIB.getWrittenRegs(MI, Written);
    if (!Written.anyCommon(Aliases)) {
      Changed |= Written;
      continue;
    }
    auto Source = MIB.analyzeLoopMemory(MI);
    if (!Source || !Source->Dest || Source->Elements != 1 ||
        !MIB.getAliases(Source->Dest, true).anyCommon(Aliases) ||
        (Source->Size != 4 && Source->Size != 8))
      return std::nullopt;
    // Query the source address at the consumer. Only its own writeback may
    // have changed the address since the index load.
    BitVector Inputs = MIB.getAliases(Source->Base, true);
    if (Source->Index)
      Inputs |= MIB.getAliases(Source->Index, true);
    if (Changed.anyCommon(Inputs))
      return std::nullopt;
    auto Base = GetStride(Source->Base), Offset = GetStride(Source->Index);
    MCPlusBuilder::LoopIndirectAccess Access;
    if (!Base || !Offset || (!*Base && !*Offset) ||
        MulOverflow(*Base, int64_t(Distance), Access.BaseDelta) ||
        MulOverflow(*Offset, int64_t(Distance), Access.IndexDelta))
      return std::nullopt;
    Access.IndexLoad = &MI;
    Access.Count = Loop.Count;
    Access.Address = *Source;
    Access.Address.Offset -= Source->Writeback;
    Access.Address.HasWriteback = false;
    Access.Address.Writeback = 0;
    return Access;
  }
  return std::nullopt;
}

struct PrefetchPlan {
  BinaryBasicBlock *BB;
  unsigned Index;
  SmallVector<MCInst, 8> Code;
  SmallVector<MCInst, 4> Guard;
  const MCInst *Load = nullptr;
  MCSymbol *Continuation = nullptr;
  unsigned Requests = 0;
  unsigned GuardedStart = 0;
  uint64_t StepMagnitude = 0;
  uint64_t BatchIterations = 0;
  const BinaryLoop *Loop = nullptr;
  PrefetchPolicy Policy{};
  uint64_t Misses = 0;
  int64_t BaseDelta = 0, IndexDelta = 0;
  unsigned Instructions = 0;
  bool Manual = false, Selected = false, AfterLoad = false, Indirect = false;
};

/// Split at the original load. Existing edge metadata follows the original
/// terminators; synthetic guard edges have estimates and no measured cycles.
void insertGuardedPrefetch(BinaryFunction &BF, PrefetchPlan &Plan) {
  BinaryContext &BC = BF.getBinaryContext();
  BinaryBasicBlock &BB = *Plan.BB;
  auto Prefetch = BF.createBasicBlock();
  auto Join = BF.createBasicBlock(Plan.Continuation);
  const int32_t CFIState = BB.getCFIStateAtInstr(&*(BB.begin() + Plan.Index));
  const uint64_t Count = BB.getExecutionCount();
  uint64_t PrefetchCount = Count, SkipCount = Count;
  if (BB.hasProfile()) {
    if (Plan.Indirect) {
      // No trip-count distribution is available for the remaining-count test.
      PrefetchCount = Count / 2;
    } else {
      const uint64_t LineSize = opts::PrefetchCacheLineSize;
      PrefetchCount =
          Count / LineSize * Plan.StepMagnitude +
          divideCeil(Count % LineSize * Plan.StepMagnitude, LineSize);
    }
    SkipCount = Count - PrefetchCount;
  }
  for (BinaryBasicBlock *New : {Prefetch.get(), Join.get()}) {
    New->setOffset(BB.getInputOffset());
    New->setFragmentNum(BB.getFragmentNum());
    New->setCFIState(CFIState);
  }
  Prefetch->setExecutionCount(PrefetchCount);
  Join->setExecutionCount(Count);
  MCInst Load = *(BB.begin() + Plan.Index);
  auto Tail = BB.splitInstructions(&*(BB.begin() + Plan.Index));
  // The label belongs to the moved load, rather than a deleted instruction.
  BC.MIB->removeAnnotation(*(BB.begin() + Plan.Index),
                           MCPlus::MCAnnotation::kLabel);
  BB.eraseInstruction(BB.begin() + Plan.Index);
  Join->addInstruction(Load);
  Join->addInstructions(Tail.begin(), Tail.end());
  BB.moveAllSuccessorsTo(Join.get());
  BB.addSuccessor(Join.get(), SkipCount, 0);
  BB.addSuccessor(Prefetch.get(), PrefetchCount, 0);
  Prefetch->addSuccessor(Join.get(), PrefetchCount, 0);

  const auto FirstRequest = Plan.Code.begin() + Plan.GuardedStart;
  BB.addInstructions(Plan.Code.begin(), FirstRequest);
  BB.addInstructions(Plan.Guard.begin(), Plan.Guard.end());
  MCInst Jump;
  BC.MIB->createUncondBranch(Jump, Prefetch->getLabel(), BC.Ctx.get());
  BB.addInstruction(Jump);
  Prefetch->addInstructions(FirstRequest, Plan.Code.end());
  BC.MIB->createUncondBranch(Jump, Join->getLabel(), BC.Ctx.get());
  Prefetch->addInstruction(Jump);
  BB.shrinkInstructions();
  std::vector<std::unique_ptr<BinaryBasicBlock>> NewBlocks;
  NewBlocks.push_back(std::move(Prefetch));
  NewBlocks.push_back(std::move(Join));
  BF.insertBasicBlocks(&BB, std::move(NewBlocks), /*UpdateLayout=*/true,
                       /*UpdateCFIState=*/false,
                       /*RecomputeLandingPads=*/false);
}
} // namespace

bool LoopDataPrefetch::shouldOptimize(const BinaryFunction &BF) const {
  const bool Manual = opts::LoopPrefetchDistance.getNumOccurrences() ||
                      !opts::LoopPrefetchHints.empty();
  return BF.isSimple() && !BF.isIgnored() && !BF.isMultiEntry() &&
         !BF.hasUnknownControlFlow() &&
         (Manual || (BF.hasCacheMissProfile() && BF.hasProfile() &&
                     (BF.getProfileFlags() & BinaryFunction::PF_BRANCH)));
}

Error LoopDataPrefetch::runOnFunctions(BinaryContext &BC) {
  if (!BC.MIB->isPrefetchHint(opts::PrefetchHint) ||
      opts::LoopPrefetchDistance > (1u << 20) ||
      opts::CacheMissCutOff > 10000 || opts::PrefetchMaxRegisters > 2)
    return createFatalBOLTError(
        "invalid loop prefetch hint, distance, cutoff or register budget");
  if (!opts::PrefetchMaxDistance || opts::PrefetchMaxDistance > (1u << 20))
    return createFatalBOLTError("invalid maximum predicted prefetch distance");
  if (opts::PrefetchCacheLineSize &&
      (!isPowerOf2_32(opts::PrefetchCacheLineSize) ||
       opts::PrefetchCacheLineSize < 16 || opts::PrefetchCacheLineSize > 4096))
    return createFatalBOLTError("loop prefetch cache line size must be 0 or a "
                                "power of two between 16 and 4096");
  if (opts::PrefetchBatch && !opts::PrefetchCacheLineSize)
    return createFatalBOLTError(
        "batch prefetch requires an explicit cache line size");
  auto Hints = readHints(BC);
  if (!Hints)
    return Hints.takeError();
  RegAnalysis RA(BC, nullptr, nullptr);
  const auto AnalysisAllocator = BC.MIB->initializeNewAnnotationAllocator();
  unsigned Inserted = 0, Rejected = 0;
  uint64_t Planned = 0, Accepted = 0, Added = 0;
  for (auto &Entry : BC.getBinaryFunctions()) {
    const uint64_t Address = Entry.first;
    BinaryFunction &BF = Entry.second;
    if (!shouldOptimize(BF))
      continue;
    PrefetchPolicy FunctionPolicy;
    if (opts::LoopPrefetchDistance.getNumOccurrences()) {
      FunctionPolicy.Distance = opts::LoopPrefetchDistance;
      FunctionPolicy.Source = "command line";
    }
    if (auto It = Hints->Functions.find(Address); It != Hints->Functions.end())
      FunctionPolicy = It->second;
    const BinaryLoop *CurrentLoop = nullptr;
    auto Diagnose = [&](const MCInst *MI, StringRef Reason,
                        const PrefetchPolicy &Policy) {
      if (!opts::PrefetchDiagnostics)
        return;
      BC.outs() << "BOLT-PREFETCH: " << BF.getPrintName() << " pc=";
      if (MI && BC.MIB->getOffset(*MI))
        BC.outs() << formatv("{0:x}", Address + *BC.MIB->getOffset(*MI));
      else
        BC.outs() << "unknown";
      BC.outs() << " source=" << Policy.Source << " distance=";
      if (Policy.Distance)
        BC.outs() << *Policy.Distance;
      else
        BC.outs() << "auto";
      BC.outs() << " hint=" << Policy.Hint << " budget=" << Policy.Budget << " "
                << Reason;
      if (Policy.IterationSamples)
        BC.outs() << " latency-cycles=" << opts::PrefetchLatency
                  << " iteration-cycle-total=" << Policy.IterationTotal
                  << " iteration-cycle-samples=" << Policy.IterationSamples
                  << " cycles-source="
                  << (opts::PrefetchIterationCycles ? "calibrated" : "profile");
      if (CurrentLoop)
        BC.outs() << formatv(" loop={0:x}",
                             Address +
                                 CurrentLoop->getHeader()->getInputOffset());
      if (MI) {
        BC.outs() << " opcode=" << BC.MII->getName(MI->getOpcode());
        if (auto Profile = BC.MIB->tryGetAnnotationAs<CacheMissProfile>(
                *MI, "CacheMissProfile"))
          BC.outs() << " misses=" << Profile->MissCount
                    << " accesses=" << Profile->AccessCount;
        if (auto A = BC.MIB->analyzeLoopMemory(*MI))
          BC.outs() << " base=" << BC.MRI->getName(A->Base) << " index="
                    << (A->Index ? BC.MRI->getName(A->Index) : "none")
                    << " index-bits=" << A->IndexBits << " shift=" << A->Shift
                    << " extend="
                    << (A->IndexBits == 32 ? (A->SignExtend ? "sxtw" : "uxtw")
                                           : (A->SignExtend ? "sxtx" : "lsl"))
                    << " offset=" << A->Offset << " writeback=" << A->Writeback
                    << " width=" << A->Size << " elements=" << A->Elements;
      }
      BC.outs() << '\n';
    };
    unsigned Instructions = 0;
    for (const BinaryBasicBlock &BB : BF)
      Instructions += BB.size();
    if (Instructions > opts::PrefetchAnalysisLimit) {
      Diagnose(nullptr, "analysis limit", FunctionPolicy);
      continue;
    }
    BF.calculateLoopInfo();
    if (BF.getLoopInfo().empty())
      continue;
    BitVector Available(BC.MRI->getNumRegs());
    BC.MIB->getGPRegs(Available, false);
    BC.MIB->removeNonScavengeableRegs(Available);
    BitVector CalleeSaved(Available.size());
    BC.MIB->getCalleeSavedRegs(CalleeSaved);
    Available.reset(CalleeSaved);
    if (!excludeCFIRegisters(BF, Available)) {
      Diagnose(nullptr, "opaque CFI expression", FunctionPolicy);
      continue;
    }
    // This pass targets AAPCS64 user ELF. Restrict scratch to caller-saved
    // registers; use conservative call information from the existing analysis.
    auto Live = std::make_unique<LivenessAnalysis>(RA, BF, AnalysisAllocator,
                                                   /*AssumeABI=*/true);
    Live->run();
    SmallVector<PrefetchPlan, 8> Plans;
    unsigned FunctionRequests = 0, FunctionInstructions = 0;
    for (BinaryLoop *TopLoop : BF.getLoopInfo()) {
      for (BinaryLoop *Loop : TopLoop->getLoopsInPreorder()) {
        CurrentLoop = Loop;
        if (!Loop->getSubLoops().empty() || !hasAcyclicIteration(*Loop)) {
          Diagnose(nullptr, "non-innermost or irreducible loop",
                   FunctionPolicy);
          continue;
        }
        const bool WantsBatch = opts::PrefetchBatch && Loop->getParentLoop();
        std::optional<BatchLoopInfo> BatchLoop;
        if (WantsBatch) {
          BatchLoop = analyzeBatchLoop(*Loop);
          if (!BatchLoop) {
            Diagnose(nullptr, "unsupported constant inner-loop batch shape",
                     FunctionPolicy);
            continue;
          }
          CurrentLoop = BatchLoop->Outer;
        }
        SmallVector<BinaryBasicBlock *, 2> Latches;
        Loop->getLoopLatches(Latches);
        bool Unsafe = Latches.empty();
        for (BinaryBasicBlock *BB :
             (BatchLoop ? BatchLoop->Outer : Loop)->blocks()) {
          Unsafe |= BB->isLandingPad();
          for (const MCInst &MI : *BB) {
            const bool Barrier = BC.MIB->isLoopPrefetchBarrier(MI);
            LLVM_DEBUG(if (Barrier) BC.printInstruction(dbgs(), MI));
            Unsafe |= Barrier || BC.MIB->getAnnotationWithDefault(
                                     MI, "LoopDataPrefetch", 0u);
          }
        }
        if (Unsafe) {
          Diagnose(nullptr, "unsupported loop context", FunctionPolicy);
          continue;
        }
        CycleStats Cycles;
        SmallVector<BinaryBasicBlock *, 2> TimingLatches = Latches;
        if (BatchLoop)
          TimingLatches = {BatchLoop->Latch};
        for (BinaryBasicBlock *Latch : TimingLatches) {
          const auto &Edge = Latch->getBranchInfo(*CurrentLoop->getHeader());
          if (!Edge.Count || Edge.Count == BinaryBasicBlock::COUNT_NO_PROFILE ||
              Edge.Cycles.Samples <= 0 || Edge.Cycles.Total <= 0) {
            Cycles = {};
            break;
          }
          Cycles.merge(Edge.Cycles);
        }
        // Cache each register recurrence once per loop, including failures.
        DenseMap<MCPhysReg, std::optional<int64_t>> Strides;
        auto Stride = [&](MCPhysReg Reg) {
          auto It = Strides.find(Reg);
          if (It != Strides.end())
            return It->second;
          return Strides.try_emplace(Reg, getStride(BF, *Loop, Latches, Reg))
              .first->second;
        };
        DenseMap<MCPhysReg, std::optional<int64_t>> OuterStrides;
        auto OuterStride = [&](MCPhysReg Reg) {
          auto It = OuterStrides.find(Reg);
          if (It != OuterStrides.end())
            return It->second;
          return OuterStrides
              .try_emplace(Reg, getBatchStride(*Loop, *BatchLoop, Reg))
              .first->second;
        };
        std::optional<IndirectLoopInfo> IndirectLoop;
        if (opts::PrefetchIndirect)
          IndirectLoop = analyzeIndirectLoop(*Loop);
        for (BinaryBasicBlock *BB : Loop->blocks()) {
          // A path-specific load could miss every boundary iteration. Keep
          // its original cadence unless it executes on all backedges.
          const bool EveryIteration =
              opts::PrefetchCacheLineSize &&
              llvm::all_of(Latches, [&](BinaryBasicBlock *Latch) {
                return BF.getDomTree().dominates(BB, Latch);
              });
          struct SelectedAccess {
            unsigned Index;
            unsigned PlanIndex;
            MCPlusBuilder::LoopMemoryAccess Access;
            int64_t BaseDelta, IndexDelta;
            StringRef Hint;
            unsigned Distance;
          };
          SmallVector<SelectedAccess, 2> Selected;
          for (unsigned I = 0; I < BB->size(); ++I) {
            MCInst &MI = *(BB->begin() + I);
            auto Access = BC.MIB->analyzeLoopMemory(MI);
            if (!Access) {
              if (BC.MIB->mayLoad(MI) &&
                  (FunctionPolicy.Distance.value_or(0) ||
                   BC.MIB->hasAnnotation(MI, "CacheMissProfile"))) {
                ++Rejected;
                Diagnose(&MI, "unsupported address", FunctionPolicy);
              }
              continue;
            }
            PrefetchPolicy Policy = FunctionPolicy;
            if (auto Offset = BC.MIB->getOffset(MI))
              if (auto It = Hints->PCs.find(Address + *Offset);
                  It != Hints->PCs.end())
                Policy = It->second;
            if (Policy.Distance && !*Policy.Distance) {
              Diagnose(&MI, "disabled", Policy);
              continue;
            }
            auto Reject = [&](StringRef Reason) {
              ++Rejected;
              Diagnose(&MI, Reason, Policy);
            };
            const bool Manual = Policy.Distance.has_value();
            auto Profile = BC.MIB->tryGetAnnotationAs<CacheMissProfile>(
                MI, "CacheMissProfile");
            if (!Manual) {
              if (!Profile)
                continue;
              if (Profile->AccessCount < 5) {
                Reject("insufficient access samples");
                continue;
              }
              if (Profile->MissCount > Profile->AccessCount) {
                Reject("invalid miss/access counts");
                continue;
              }
              if (double(Profile->MissCount) / Profile->AccessCount <=
                  double(opts::CacheMissCutOff) / 10000) {
                Diagnose(&MI, "miss rate below cutoff", Policy);
                continue;
              }
              if (Profile->MissCount < opts::PrefetchMinMisses) {
                Reject("insufficient sampled misses");
                continue;
              }
              if (!opts::PrefetchLatency) {
                Reject("missing calibrated latency");
                continue;
              }
              if (opts::PrefetchIterationCycles) {
                Policy.IterationTotal = opts::PrefetchIterationCycles;
                Policy.IterationSamples = 1;
              } else if (TimingLatches.size() != 1) {
                Reject("multiple latches need calibrated cycles");
                continue;
              } else if (Cycles.Samples > 0 && Cycles.Total > 0) {
                Policy.IterationTotal = Cycles.Total;
                Policy.IterationSamples = Cycles.Samples;
              } else {
                Reject("missing cycles");
                continue;
              }
              // Preserve fractional means and avoid overflowing L * samples.
              APInt Numerator = APInt(128, opts::PrefetchLatency) *
                                APInt(128, Policy.IterationSamples);
              APInt Quotient, Remainder;
              APInt::udivrem(Numerator, APInt(128, Policy.IterationTotal),
                             Quotient, Remainder);
              if (!Remainder.isZero())
                ++Quotient;
              if (Quotient.ugt(opts::PrefetchMaxDistance)) {
                Reject("automatic distance exceeds limit");
                continue;
              }
              Policy.Distance = Quotient.getZExtValue();
            }
            std::optional<BatchAccess> Batch;
            if (BatchLoop) {
              Batch = getBatchAccess(*BB, I, *Access, *BatchLoop,
                                     *Policy.Distance, Stride, OuterStride);
              if (!Batch) {
                Reject("unsupported batch address or footprint");
                continue;
              }
            }
            const auto BaseStride =
                getAddressStride(*BB, I, Access->Base, *Loop, Stride);
            const auto IndexStride =
                getAddressStride(*BB, I, Access->Index, *Loop, Stride);
            int64_t BaseDelta = 0, IndexDelta = 0, ByteStep = 0;
            MCPhysReg NextPointer = 0;
            bool AfterLoad = false;
            std::optional<MCPlusBuilder::LoopIndirectAccess> Indirect;
            if (Batch) {
              BaseDelta = Batch->Delta;
            } else if (!BaseStride || !IndexStride ||
                       (!*BaseStride && !*IndexStride) ||
                       MulOverflow(*BaseStride, int64_t(*Policy.Distance),
                                   BaseDelta) ||
                       MulOverflow(*IndexStride, int64_t(*Policy.Distance),
                                   IndexDelta)) {
              if (IndirectLoop)
                Indirect = getIndirectAccess(*BB, I, *Access, *IndirectLoop,
                                             *Policy.Distance, Stride);
              if (!Indirect) {
                auto Next =
                    getNextPointer(BF, *Loop, Latches, *BB, MI, *Access);
                if (!Next) {
                  Reject(
                      opts::PrefetchIndirect
                          ? "unproven recurrence or unsupported indirect bounds"
                          : "unproven recurrence");
                  continue;
                }
                if (*Policy.Distance != 1) {
                  Reject("pointer chase requires distance 1");
                  continue;
                }
                NextPointer = *Next;
                AfterLoad = true;
              }
              BaseDelta = IndexDelta = 0;
            } else {
              ByteStep = uint64_t(*BaseStride) +
                         (uint64_t(*IndexStride) << Access->Shift);
            }
            const uint64_t Magnitude =
                ByteStep < 0 ? -uint64_t(ByteStep) : uint64_t(ByteStep);
            const bool Throttle = opts::PrefetchCacheLineSize && Magnitude &&
                                  Magnitude < opts::PrefetchCacheLineSize &&
                                  EveryIteration;
            // A throttled contiguous access needs one request per line. Use
            // its last byte so unaligned scalar/pair accesses follow the same
            // boundary rule; an initial partial line can remain uncovered.
            auto PrefetchAccess = Batch ? Batch->Address : *Access;
            if (AfterLoad)
              PrefetchAccess.Base = NextPointer;
            if (Throttle) {
              PrefetchAccess.Offset += Access->Size * Access->Elements - 1;
              PrefetchAccess.Size = PrefetchAccess.Elements = 1;
            }
            const unsigned Requests =
                Batch ? Batch->Requests : PrefetchAccess.Elements;
            if (Batch && (Requests > Policy.Budget ||
                          Requests > opts::PrefetchMaxPerLoop ||
                          Requests > opts::PrefetchMaxPerFunction ||
                          Requests > opts::PrefetchMaxLoopInstructions ||
                          Requests > opts::PrefetchMaxFunctionInstructions)) {
              Reject("batch request/instruction budget");
              continue;
            }
            // Equal effective addresses imply the same cache line. Do not
            // merge different offsets without a proof of base alignment.
            auto Duplicate =
                llvm::find_if(Selected, [&](const SelectedAccess &S) {
                  const auto &A = S.Access;
                  if (A.Base != PrefetchAccess.Base ||
                      A.Index != PrefetchAccess.Index ||
                      A.IndexBits != PrefetchAccess.IndexBits ||
                      A.Shift != PrefetchAccess.Shift ||
                      A.SignExtend != PrefetchAccess.SignExtend ||
                      A.Offset != PrefetchAccess.Offset ||
                      A.Size != PrefetchAccess.Size ||
                      A.Elements != PrefetchAccess.Elements ||
                      S.BaseDelta != BaseDelta || S.IndexDelta != IndexDelta ||
                      S.Hint != Policy.Hint || S.Distance != *Policy.Distance)
                    return false;
                  BitVector Changed(Available.size());
                  for (unsigned J = S.Index; J < I; ++J)
                    BC.MIB->getWrittenRegs(*(BB->begin() + J), Changed);
                  return !Changed.anyCommon(
                             BC.MIB->getAliases(Access->Base, true)) &&
                         (!Access->Index ||
                          !Changed.anyCommon(
                              BC.MIB->getAliases(Access->Index, true)));
                });
            if (Duplicate != Selected.end()) {
              auto &Earlier = Plans[Duplicate->PlanIndex];
              Earlier.Misses =
                  std::max(Earlier.Misses, Profile ? Profile->MissCount : 0);
              Earlier.Manual |= Manual && Earlier.Instructions <= Policy.Budget;
              Diagnose(&MI, "duplicate address", Policy);
              continue;
            }
            SmallVector<MCPhysReg, 2> Scratch;
            BinaryBasicBlock *EmitBB = Batch ? BatchLoop->Entry : BB;
            const unsigned EmitIndex = Batch ? BatchLoop->EntryIndex : I;
            const auto LiveAt =
                Batch ? (EmitIndex < EmitBB->size()
                             ? Live->getStateAt(*(EmitBB->begin() + EmitIndex))
                             : Live->getStateAt(*EmitBB))
                      : (AfterLoad ? Live->getStateBefore(MI)
                                   : Live->getStateAt(MI));
            for (int Reg : Available.set_bits()) {
              if (Scratch.size() == opts::PrefetchMaxRegisters)
                break;
              // Alias-mask intersections can include a common tuple even
              // for disjoint registers. Query the physical register itself.
              if (!LiveAt->test(Reg))
                Scratch.push_back(Reg);
            }
            if ((Throttle || Indirect) && Scratch.size() < 2) {
              Reject(Indirect ? "indirect prefetch needs two dead registers"
                              : "throttling needs two dead registers");
              continue;
            }
            PrefetchPlan Plan{EmitBB, EmitIndex, {}, {}};
            Plan.Load = &MI;
            if (Indirect || Throttle) {
              auto Lock = BC.scopeLock();
              Plan.Continuation = BC.Ctx->createNamedTempSymbol("PrefetchJoin");
            }
            if (Batch) {
              bool Encoded = true;
              for (unsigned R = 0; R < Requests; ++R) {
                auto Point = PrefetchAccess;
                const uint64_t Offset = std::min(
                    uint64_t(R) * unsigned(opts::PrefetchCacheLineSize),
                    Batch->Span - 1);
                if (AddOverflow(Point.Offset, int64_t(Offset), Point.Offset) ||
                    !BC.MIB->createLoopPrefetch(Point, BaseDelta, 0, Scratch,
                                                Policy.Hint, Plan.Code,
                                                /*MaterializeAddress=*/false)) {
                  Encoded = false;
                  break;
                }
              }
              if (!Encoded) {
                Reject("unsupported batch encoding or insufficient registers");
                continue;
              }
              Plan.BatchIterations = BatchLoop->Iterations;
            } else if (Indirect) {
              if (!BC.MIB->createLoopIndirectPrefetch(
                      *Indirect, PrefetchAccess, *Policy.Distance, Scratch,
                      Policy.Hint, Plan.Continuation, BC.Ctx.get(), Plan.Code,
                      Plan.Guard)) {
                Reject("unsupported indirect index load or encoding");
                continue;
              }
              Plan.Indirect = true;
            } else if (!BC.MIB->createLoopPrefetch(
                           PrefetchAccess, BaseDelta, IndexDelta, Scratch,
                           Policy.Hint, Plan.Code,
                           /*MaterializeAddress=*/Throttle)) {
              Reject("unsupported encoding or insufficient dead registers");
              continue;
            }
            if (Throttle) {
              if (!BC.MIB->createLoopPrefetchGuard(
                      Scratch[0], Scratch[1], ByteStep,
                      opts::PrefetchCacheLineSize, Plan.Continuation,
                      BC.Ctx.get(), Plan.Guard)) {
                Reject("unsupported throttle guard");
                continue;
              }
              Plan.StepMagnitude = Magnitude;
              Plan.GuardedStart = Plan.Code.size() - Requests;
            }
            // Charge both possible CFG transfers even if FixupBranches later
            // removes a jump to the next block in the layout.
            const unsigned AddedInstructions = Plan.Code.size() +
                                               Plan.Guard.size() +
                                               (Plan.Continuation ? 2 : 0);
            if (AddedInstructions > Policy.Budget) {
              Reject("instruction budget");
              continue;
            }
            Selected.push_back({I, unsigned(Plans.size()), PrefetchAccess,
                                BaseDelta, IndexDelta, Policy.Hint,
                                *Policy.Distance});
            Plan.Requests = Requests;
            Plan.Loop = Batch ? BatchLoop->Outer : Loop;
            Plan.Policy = Policy;
            Plan.Misses = Profile ? Profile->MissCount : 0;
            Plan.Manual = Manual;
            Plan.AfterLoad = AfterLoad;
            Plan.BaseDelta = BaseDelta;
            Plan.IndexDelta = IndexDelta;
            Plan.Instructions = AddedInstructions;
            Plans.push_back(std::move(Plan));
          }
        }
      }
    }
    // Rank only complete legal plans. All counts came from the same sampling
    // protocol; misses are a priority proxy, not an estimate of saved stalls.
    SmallVector<PrefetchPlan *, 8> Ranked;
    for (auto &Plan : Plans)
      Ranked.push_back(&Plan);
    llvm::stable_sort(Ranked, [](const PrefetchPlan *A, const PrefetchPlan *B) {
      if (A->Manual != B->Manual)
        return A->Manual;
      if (A->Misses != B->Misses)
        return A->Misses > B->Misses;
      return A->Instructions < B->Instructions;
    });
    DenseMap<const BinaryLoop *, std::pair<unsigned, unsigned>> LoopBudgets;
    Planned += Plans.size();
    for (PrefetchPlan *Plan : Ranked) {
      CurrentLoop = Plan->Loop;
      const MCInst &MI = *Plan->Load;
      auto &[LoopRequests, LoopInstructions] = LoopBudgets[Plan->Loop];
      StringRef Reason;
      if (uint64_t(LoopRequests) + Plan->Requests > opts::PrefetchMaxPerLoop ||
          uint64_t(FunctionRequests) + Plan->Requests >
              opts::PrefetchMaxPerFunction)
        Reason = "request budget";
      else if (Plan->BatchIterations &&
               (uint64_t(LoopRequests) + Plan->Requests) *
                       unsigned(opts::PrefetchCacheLineSize) >
                   opts::PrefetchBatchMaxBytes)
        Reason = "batch footprint budget";
      else if (uint64_t(LoopInstructions) + Plan->Instructions >
                   opts::PrefetchMaxLoopInstructions ||
               uint64_t(FunctionInstructions) + Plan->Instructions >
                   opts::PrefetchMaxFunctionInstructions)
        Reason = "instruction budget";
      if (!Reason.empty()) {
        ++Rejected;
        Diagnose(&MI, Reason, Plan->Policy);
        continue;
      }
      Plan->Selected = true;
      LoopRequests += Plan->Requests;
      FunctionRequests += Plan->Requests;
      LoopInstructions += Plan->Instructions;
      FunctionInstructions += Plan->Instructions;
      ++Accepted;
      Added += Plan->Instructions;
      BC.MIB->addAnnotation(Plan->Code.front(), "LoopDataPrefetch", 1u);
      std::string Detail =
          formatv("inserted base-delta={0} index-delta={1} instructions={2} "
                  "cache-line={3} placement={4} kind={5}",
                  Plan->BaseDelta, Plan->IndexDelta, Plan->Instructions,
                  Plan->Continuation && !Plan->Indirect
                      ? unsigned(opts::PrefetchCacheLineSize)
                      : 0,
                  Plan->BatchIterations
                      ? "outer-entry"
                      : (Plan->AfterLoad ? "after-load" : "before-load"),
                  Plan->BatchIterations
                      ? "batch"
                      : (Plan->Indirect
                             ? "indirect"
                             : (Plan->AfterLoad ? "pointer" : "affine")))
              .str();
      if (Plan->BatchIterations)
        Detail +=
            formatv(" inner-iterations={0} requests={1} footprint-bytes={2} "
                    "distance-unit=outer batch-line-size={3}",
                    Plan->BatchIterations, Plan->Requests,
                    uint64_t(Plan->Requests) *
                        unsigned(opts::PrefetchCacheLineSize),
                    unsigned(opts::PrefetchCacheLineSize))
                .str();
      Diagnose(&MI, Detail, Plan->Policy);
    }
    // Release per-function analysis values before moving instructions. Using
    // the default arena would retain every function's live sets until exit.
    Live.reset();
    BC.MIB->freeValuesAllocator(AnalysisAllocator);
    bool ChangedCFG = false;
    for (auto &Plan : llvm::reverse(Plans)) {
      if (!Plan.Selected)
        continue;
      if (Plan.Continuation) {
        insertGuardedPrefetch(BF, Plan);
        ChangedCFG = true;
      } else {
        unsigned Index = Plan.Index + Plan.AfterLoad;
        // Rules at the load's outgoing PC must also cover the new prefetch.
        while (Plan.AfterLoad && Index < Plan.BB->size() &&
               BC.MIB->isCFI(*(Plan.BB->begin() + Index)))
          ++Index;
        for (MCInst &MI : llvm::reverse(Plan.Code))
          Plan.BB->insertInstruction(Plan.BB->begin() + Index, MI);
      }
    }
    if (ChangedCFG) {
      BF.constructDomTree();
      BF.calculateLoopInfo();
    }
    Inserted += FunctionRequests;
  }
  BC.outs() << "BOLT-INFO: number of prefetch instructions inserted: "
            << Inserted << " (rejected candidates: " << Rejected << ")\n";
  BC.outs() << "BOLT-INFO: prefetch plans: " << Planned
            << ", accepted loads: " << Accepted
            << ", added instructions: " << Added << "\n";
  return Error::success();
}
