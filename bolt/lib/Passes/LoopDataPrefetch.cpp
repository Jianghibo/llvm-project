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
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"

#define DEBUG_TYPE "loop-data-prefetch"

using namespace llvm;
using namespace llvm::bolt;

namespace opts {
extern cl::OptionCategory BoltOptCategory;
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
// Generic AArch64 estimate until target-specific miss-latency models are
// available. BOLT currently creates a generic MC subtarget, not a named CPU.
constexpr uint64_t DefaultPrefetchLatency = 200;
constexpr unsigned MaxLoopInstructions = 32;
constexpr unsigned MaxFunctionInstructions = 256;

struct PrefetchPolicy {
  std::optional<unsigned> Distance;
  uint64_t IterationTotal = 0, IterationSamples = 0;
  bool UsesEstimatedCycles = false;
};

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

/// Return a topological order after removing backedges and an optional
/// collapsed self-loop. Return an empty order if any other cycle remains.
SmallVector<BinaryBasicBlock *, 16>
getIterationOrder(const BinaryLoop &Loop,
                  BinaryBasicBlock *Collapsed = nullptr) {
  DenseMap<BinaryBasicBlock *, unsigned> InDegree;
  for (BinaryBasicBlock *BB : Loop.blocks())
    InDegree[BB] = 0;
  for (BinaryBasicBlock *BB : Loop.blocks())
    for (BinaryBasicBlock *Succ : BB->successors())
      if (Succ != Loop.getHeader() && Loop.contains(Succ) &&
          !(BB == Collapsed && Succ == BB))
        ++InDegree[Succ];
  SmallVector<BinaryBasicBlock *, 16> Ready;
  for (auto [BB, Degree] : InDegree)
    if (!Degree)
      Ready.push_back(BB);
  SmallVector<BinaryBasicBlock *, 16> Order;
  while (!Ready.empty()) {
    BinaryBasicBlock *BB = Ready.pop_back_val();
    Order.push_back(BB);
    for (BinaryBasicBlock *Succ : BB->successors())
      if (Succ != Loop.getHeader() && Loop.contains(Succ) &&
          !(BB == Collapsed && Succ == BB) && !--InDegree[Succ])
        Ready.push_back(Succ);
  }
  if (Order.size() != Loop.getNumBlocks())
    return {};
  return Order;
}

bool writesAnyOf(const MCPlusBuilder &MIB, const MCInst &MI,
                 const BitVector &Aliases) {
  BitVector Written(Aliases.size());
  MIB.getWrittenRegs(MI, Written);
  return Written.anyCommon(Aliases);
}

bool dominatesAll(BinaryFunction &BF, const BinaryBasicBlock *BB,
                  ArrayRef<BinaryBasicBlock *> Latches) {
  return llvm::all_of(Latches, [&](BinaryBasicBlock *Latch) {
    return BF.getDomTree().dominates(BB, Latch);
  });
}

template <typename ComputeT>
std::optional<int64_t>
cachedStride(DenseMap<MCPhysReg, std::optional<int64_t>> &Cache, MCPhysReg Reg,
             ComputeT Compute) {
  auto It = Cache.find(Reg);
  if (It != Cache.end())
    return It->second;
  return Cache.try_emplace(Reg, Compute()).first->second;
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
      if (!writesAnyOf(MIB, MI, MIB.getAliases(Reg, true)))
        continue;
      int64_t Step;
      LLVM_DEBUG(dbgs() << "Checking recurrence "
                        << BF.getBinaryContext().MRI->getName(Reg) << " at "
                        << BF.getBinaryContext().MII->getName(MI.getOpcode())
                        << '\n');
      if (!MIB.getLoopRegisterUpdate(MI, Reg, Step) ||
          !dominatesAll(BF, BB, Latches) || AddOverflow(Stride, Step, Stride))
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
    const MCInst &MI = Block->getInstructionAtIndex(--Index);
    if (!writesAnyOf(MIB, MI, Aliases))
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
  if (!dominatesAll(BF, &BB, Latches))
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
          (Block == &BB && &MI < &Load) || !dominatesAll(BF, Block, Latches))
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
  while (Index && (MIB.isCFI(BB.getInstructionAtIndex(Index - 1)) ||
                   MIB.isNoop(BB.getInstructionAtIndex(Index - 1))))
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
  MCPhysReg Count = MIB.getLoopCountDownReg(
      BB->getInstructionAtIndex(*Update), *Backedge, *EntryBranch, EntryOnZero);
  if (!Count ||
      EntryOnZero == (MIB.getTargetSymbol(*EntryBranch) == Header->getLabel()))
    return std::nullopt;
  const BitVector &Aliases = MIB.getAliases(Count, true);
  const BitVector &Stack = MIB.getAliases(MIB.getStackPointer(), true);
  const MCInst *CounterUpdate = &BB->getInstructionAtIndex(*Update);
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
         !MIB.isTerminator(Entry->getInstructionAtIndex(Anchor)))
    ++Anchor;
  for (unsigned I = 0; I < Anchor; ++I) {
    const MCInst &Init = Entry->getInstructionAtIndex(I);
    auto Count = MIB.getLoopConstantCount(
        Init, Body->getInstructionAtIndex(*Update), *Backedge);
    if (!Count)
      continue;
    const BitVector &Aliases = MIB.getAliases(Count->Reg, true);
    for (BinaryBasicBlock *BB : {Entry, Body}) {
      for (const MCInst &MI : *BB) {
        if (&MI == &Init || &MI == &Body->getInstructionAtIndex(*Update))
          continue;
        if (writesAnyOf(MIB, MI, Aliases))
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
      if (!writesAnyOf(MIB, MI, Aliases))
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
  if (!Step || AbsoluteValue(*Step) != Width ||
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
        if (!writesAnyOf(MIB, MI, Aliases))
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
    const MCInst &MI = BB.getInstructionAtIndex(I);
    if (!writesAnyOf(MIB, MI, Aliases))
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
    const MCInst &MI = (*Block)->getInstructionAtIndex(--Index);
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

enum class PrefetchKind { Affine, Pointer, Indirect, Batch };

struct PrefetchCost {
  double Benefit = 0, Overhead = 0, Priority = 0, SpillOverhead = 0;
  unsigned ScratchRegisters = 0, SpilledRegisters = 0;
  bool HasEstimate = false;
};

struct PrefetchSpill {
  SmallVector<MCPhysReg, 2> Registers;
  MCInst Save, Restore;
  int64_t CFAOffset = 0;
  int32_t CFIState = 0;
};

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
  CacheMissProfile Profile{};
  std::optional<CacheMissProfile> IndexProfile{};
  PrefetchCost Cost{};
  std::optional<PrefetchSpill> Spill{};
  int64_t BaseDelta = 0, IndexDelta = 0;
  unsigned Instructions = 0;
  bool Selected = false;
  PrefetchKind Kind = PrefetchKind::Affine;

  StringRef getKindName() const {
    switch (Kind) {
    case PrefetchKind::Affine:
      return "affine";
    case PrefetchKind::Pointer:
      return "pointer";
    case PrefetchKind::Indirect:
      return "indirect";
    case PrefetchKind::Batch:
      return "batch";
    }
    llvm_unreachable("invalid prefetch kind");
  }

  StringRef getPlacement() const {
    if (Kind == PrefetchKind::Batch)
      return "outer-entry";
    return Kind == PrefetchKind::Pointer ? "after-load" : "before-load";
  }
};

/// Keep the temporary frame and its unwind rules within a single block.
void insertSpilledPrefetch(BinaryFunction &BF, PrefetchPlan &Plan,
                           unsigned Index) {
  BinaryBasicBlock &BB = *Plan.BB;
  const PrefetchSpill &Spill = *Plan.Spill;
  const MCRegisterInfo &MRI = *BF.getBinaryContext().MRI;
  auto Pos = BB.begin() + Index;
  auto Emit = [&](MCInst &MI) {
    Pos = std::next(BB.insertInstruction(Pos, MI));
  };
  auto CFI = [&](MCCFIInstruction Inst) {
    Pos = std::next(BF.addCFIInstruction(&BB, Pos, std::move(Inst)));
  };
  MCInst Save = Spill.Save, Restore = Spill.Restore;
  Emit(Save);
  CFI(MCCFIInstruction::cfiDefCfaOffset(nullptr, Spill.CFAOffset + 16));
  for (unsigned I = 0; I < Spill.Registers.size(); ++I)
    CFI(MCCFIInstruction::createOffset(
        nullptr, MRI.getDwarfRegNum(Spill.Registers[I], false),
        -Spill.CFAOffset - 16 + I * 8));
  for (MCInst &MI : Plan.Code)
    Emit(MI);
  Emit(Restore);
  for (MCPhysReg Reg : Spill.Registers)
    CFI(MCCFIInstruction::createRestore(nullptr,
                                        MRI.getDwarfRegNum(Reg, false)));
  CFI(MCCFIInstruction::cfiDefCfaOffset(nullptr, Spill.CFAOffset));
  // Re-anchor the unchanged logical state for CFI reconstruction after layout.
  // State zero is allowed only for a function with no original FDE program.
  if (Spill.CFIState)
    BF.addCFIPseudo(&BB, Pos, Spill.CFIState - 1);
}

/// Split at the original load. Existing edge metadata follows the original
/// terminators; synthetic guard edges have estimates and no measured cycles.
void insertGuardedPrefetch(BinaryFunction &BF, PrefetchPlan &Plan) {
  BinaryContext &BC = BF.getBinaryContext();
  BinaryBasicBlock &BB = *Plan.BB;
  auto Prefetch = BF.createBasicBlock();
  auto Join = BF.createBasicBlock(Plan.Continuation);
  const int32_t CFIState =
      BB.getCFIStateAtInstr(&BB.getInstructionAtIndex(Plan.Index));
  const uint64_t Count = BB.getExecutionCount();
  uint64_t PrefetchCount = Count, SkipCount = Count;
  if (BB.hasProfile()) {
    if (Plan.Kind == PrefetchKind::Indirect) {
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
  MCInst Load = BB.getInstructionAtIndex(Plan.Index);
  auto Tail = BB.splitInstructions(&BB.getInstructionAtIndex(Plan.Index));
  // The label belongs to the moved load, rather than a deleted instruction.
  BC.MIB->removeAnnotation(BB.getInstructionAtIndex(Plan.Index),
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

/// Estimate dependency latency and throughput from the MC schedule.
/// This is a local cost model, not a simulation of cache misses or speculation.
template <typename Instructions>
std::optional<uint64_t> estimateCycles(const BinaryContext &BC,
                                       const Instructions &Code,
                                       bool Generated = false) {
  const MCSubtargetInfo &STI = *BC.STI;
  const MCSchedModel &Model = STI.getSchedModel();
  if (!Model.hasInstrSchedModel() || !Model.IssueWidth)
    return std::nullopt;
  SmallVector<uint64_t, 0> Ready(BC.MRI->getNumRegs(), 0);
  BitVector Used(Ready.size()), Written(Ready.size());
  DenseMap<unsigned, uint64_t> Resources;
  uint64_t IssueCycle = 0, Cycles = 0, LastMemory = 0, LastStore = 0;
  unsigned IssueSlots = 0;
  for (const MCInst &MI : Code) {
    if (BC.MIB->isCFI(MI))
      continue;
    // A call's cost includes its callee. Do not model it as a single issue.
    if (BC.MIB->isCall(MI))
      return std::nullopt;
    const MCInstrDesc &Desc = BC.MII->get(MI.getOpcode());
    const MCSchedClassDesc *SC = nullptr;
    unsigned MicroOps = 1, Latency = 1;
    const bool Noop = BC.MIB->isNoop(MI);
    if (!Noop) {
      // Generated hints carry the side-effect flag but have scheduling data.
      // Their semantics were checked by the plan builder; unknown original
      // instructions must still make a static iteration estimate unavailable.
      if (!Generated && Desc.hasUnmodeledSideEffects())
        return std::nullopt;
      unsigned Class = Desc.getSchedClass();
      for (unsigned Limit = 8; Limit; --Limit) {
        if (!Class || Class >= Model.NumSchedClasses)
          return std::nullopt;
        SC = Model.getSchedClassDesc(Class);
        if (!SC->isValid())
          return std::nullopt;
        if (!SC->isVariant())
          break;
        Class = STI.resolveVariantSchedClass(Class, &MI, BC.MII.get(),
                                             Model.getProcessorID());
      }
      if (!SC || SC->isVariant())
        return std::nullopt;
      const int Delay = MCSchedModel::computeInstrLatency(STI, *SC);
      if (Delay < 0)
        return std::nullopt;
      Latency = std::max(1, Delay);
      MicroOps = SC->NumMicroOps;
      for (const MCWriteProcResEntry &R : make_range(
               STI.getWriteProcResBegin(SC), STI.getWriteProcResEnd(SC))) {
        if (!R.ProcResourceIdx ||
            R.ProcResourceIdx >= Model.NumProcResourceKinds ||
            R.ReleaseAtCycle < R.AcquireAtCycle)
          return std::nullopt;
        Resources[R.ProcResourceIdx] += R.ReleaseAtCycle - R.AcquireAtCycle;
      }
    }
    Used.reset();
    Written.reset();
    BC.MIB->getSrcRegs(MI, Used);
    BC.MIB->getWrittenRegs(MI, Written);
    uint64_t Start = IssueCycle;
    for (int Reg : Used.set_bits())
      if (!BC.MRI->isConstant(Reg))
        Start = std::max(Start, Ready[Reg]);
    if (!Noop && (Desc.mayLoad() || Desc.mayStore()))
      Start = std::max(Start, Desc.mayStore() ? LastMemory : LastStore);
    const uint64_t Finish = Start + Latency;
    for (int Reg : Written.set_bits())
      if (!BC.MRI->isConstant(Reg))
        Ready[Reg] = Finish;
    if (!Noop && (Desc.mayLoad() || Desc.mayStore())) {
      LastMemory = std::max(LastMemory, Finish);
      if (Desc.mayStore())
        LastStore = Finish;
    }
    Cycles = std::max(Cycles, Finish);
    if (!Model.isOutOfOrder() && Start > IssueCycle) {
      IssueCycle = Start;
      IssueSlots = 0;
    }
    IssueSlots += MicroOps;
    IssueCycle += IssueSlots / Model.IssueWidth;
    IssueSlots %= Model.IssueWidth;
  }
  Cycles = std::max(Cycles, IssueCycle + unsigned(IssueSlots != 0));
  for (auto [Resource, Usage] : Resources) {
    const unsigned Units = Model.getProcResource(Resource)->NumUnits;
    if (!Units)
      return std::nullopt;
    Cycles = std::max(Cycles, divideCeil(Usage, uint64_t(Units)));
  }
  return Cycles;
}

/// Cost completed header-to-backedge paths, excluding paths that only exit.
/// Use conditional edge weights when all surviving edges have counts; otherwise
/// take their maximum. For batch mode, collapse the constant inner self-loop.
std::optional<int64_t> estimateLoopCycles(const BinaryLoop &Loop,
                                          BinaryBasicBlock *InnerHeader,
                                          uint64_t InnerIterations) {
  const auto Order = getIterationOrder(Loop, InnerHeader);
  if (Order.empty())
    return std::nullopt;
  DenseMap<BinaryBasicBlock *, uint64_t> Costs;
  for (BinaryBasicBlock *BB : llvm::reverse(Order)) {
    // Keep products and sums of 64-bit edge counts and costs exact.
    APInt Weighted(192, 0), Weights(192, 0);
    uint64_t Longest = 0;
    bool Completes = false, KnownWeights = true;
    for (BinaryBasicBlock *Succ : BB->successors()) {
      if (BB == InnerHeader && Succ == BB)
        continue;
      auto It = Costs.find(Succ);
      if (Succ != Loop.getHeader() && It == Costs.end())
        continue;
      const uint64_t Cost = Succ == Loop.getHeader() ? 0 : It->second;
      Completes = true;
      Longest = std::max(Longest, Cost);
      const auto &Edge = BB->getBranchInfo(*Succ);
      if (!Edge.Count || Edge.Count == BinaryBasicBlock::COUNT_NO_PROFILE ||
          Edge.MispredictedCount == BinaryBasicBlock::COUNT_INFERRED) {
        KnownWeights = false;
        continue;
      }
      Weights += APInt(192, Edge.Count);
      Weighted += APInt(192, Edge.Count) * APInt(192, Cost);
    }
    if (!Completes)
      continue;
    auto Local = estimateCycles(BB->getFunction()->getBinaryContext(), *BB);
    if (!Local)
      return std::nullopt;
    if (BB == InnerHeader) {
      if (*Local && InnerIterations > uint64_t(INT64_MAX) / *Local)
        return std::nullopt;
      *Local *= InnerIterations;
    }
    const uint64_t Tail =
        KnownWeights ? ((Weighted + Weights - 1).udiv(Weights)).getZExtValue()
                     : Longest;
    if (*Local > uint64_t(INT64_MAX) - Tail)
      return std::nullopt;
    Costs[BB] = *Local + Tail;
  }
  auto It = Costs.find(Loop.getHeader());
  if (It == Costs.end() || !It->second)
    return std::nullopt;
  return It->second;
}

StringRef computeAutoDistance(PrefetchPolicy &Policy, const CycleStats &Cycles,
                              bool Estimated) {
  if (Cycles.Samples <= 0 || Cycles.Total <= 0)
    return "unavailable static iteration cost";
  Policy.IterationTotal = Cycles.Total;
  Policy.IterationSamples = Cycles.Samples;
  Policy.UsesEstimatedCycles = Estimated;
  // Preserve fractional means and avoid overflowing L * samples.
  APInt Numerator =
      APInt(128, DefaultPrefetchLatency) * APInt(128, Policy.IterationSamples);
  APInt Quotient, Remainder;
  APInt::udivrem(Numerator, APInt(128, Policy.IterationTotal), Quotient,
                 Remainder);
  if (!Remainder.isZero())
    ++Quotient;
  if (Quotient.ugt(MCPlusBuilder::MaxLoopPrefetchDistance))
    return "automatic distance exceeds limit";
  Policy.Distance = Quotient.getZExtValue();
  return {};
}

Error validateOptions(const BinaryContext &BC) {
  if (!BC.MIB->isPrefetchHint(opts::PrefetchHint) ||
      opts::CacheMissCutOff > 10000)
    return createFatalBOLTError("invalid loop prefetch hint or cutoff");
  if (opts::PrefetchCacheLineSize &&
      (!isPowerOf2_32(opts::PrefetchCacheLineSize) ||
       opts::PrefetchCacheLineSize < 16 || opts::PrefetchCacheLineSize > 4096))
    return createFatalBOLTError("loop prefetch cache line size must be 0 or a "
                                "power of two between 16 and 4096");
  if (opts::PrefetchBatch && !opts::PrefetchCacheLineSize)
    return createFatalBOLTError(
        "batch prefetch requires an explicit cache line size");
  return Error::success();
}

struct LoopAnalysis {
  BinaryFunction &BF;
  BinaryLoop &Loop;
  ArrayRef<BinaryBasicBlock *> Latches;
  CycleStats Cycles;
  bool EstimatedCycles;
  const std::optional<BatchLoopInfo> &BatchLoop;
  const std::optional<IndirectLoopInfo> &IndirectLoop;
  DenseMap<MCPhysReg, std::optional<int64_t>> Strides{}, OuterStrides{};

  std::optional<int64_t> stride(MCPhysReg Reg) {
    return cachedStride(Strides, Reg,
                        [&] { return getStride(BF, Loop, Latches, Reg); });
  }

  std::optional<int64_t> outerStride(MCPhysReg Reg) {
    assert(BatchLoop && "outer stride requires a batch loop");
    return cachedStride(OuterStrides, Reg,
                        [&] { return getBatchStride(Loop, *BatchLoop, Reg); });
  }
};

struct SelectedAccess {
  unsigned Index, PlanIndex;
  MCPlusBuilder::LoopMemoryAccess Access;
  int64_t BaseDelta, IndexDelta;
  unsigned Distance;
};

struct PrefetchStats {
  unsigned Inserted = 0, Rejected = 0;
  uint64_t Planned = 0, Accepted = 0, Added = 0;
};

/// Plan and budget a whole function before changing its instructions or CFG.
class FunctionPrefetchPlanner {
  BinaryFunction &BF;
  BinaryContext &BC;
  const uint64_t Address;
  RegAnalysis &RA;
  const MCPlusBuilder::AllocatorIdTy AnalysisAllocator;
  PrefetchStats &Stats;
  const BinaryLoop *CurrentLoop = nullptr;
  BitVector Available;
  std::unique_ptr<LivenessAnalysis> Live;
  SmallVector<PrefetchPlan, 8> Plans;
  unsigned FunctionRequests = 0, FunctionInstructions = 0;

  void diagnose(const MCInst *MI, StringRef Reason,
                const PrefetchPolicy &Policy,
                const PrefetchCost *Cost = nullptr) {
    if (!opts::PrefetchDiagnostics)
      return;
    BC.outs() << "BOLT-PREFETCH: " << BF.getPrintName() << " pc=";
    if (MI && BC.MIB->getOffset(*MI))
      BC.outs() << formatv("{0:x}", Address + *BC.MIB->getOffset(*MI));
    else
      BC.outs() << "unknown";
    BC.outs() << " source=profile distance=";
    if (Policy.Distance)
      BC.outs() << *Policy.Distance;
    else
      BC.outs() << "auto";
    BC.outs() << " hint=" << opts::PrefetchHint
              << " budget=" << opts::PrefetchMaxInstructions << " " << Reason;
    if (Policy.IterationSamples)
      BC.outs() << " latency-cycles=" << DefaultPrefetchLatency
                << " iteration-cycle-total=" << Policy.IterationTotal
                << " iteration-cycle-samples=" << Policy.IterationSamples
                << " latency-source=default cycles-source="
                << (Policy.UsesEstimatedCycles ? "static" : "profile");
    if (Policy.UsesEstimatedCycles)
      BC.outs() << " timing-cpu=" << BC.STI->getCPU();
    if (Cost && Cost->HasEstimate)
      BC.outs() << formatv(" estimated-benefit-cycles={0:F3} "
                           "estimated-cost-cycles={1:F3} scratch-registers={2} "
                           "spilled-registers={3} spill-cost-cycles={4:F3}",
                           Cost->Benefit, Cost->Overhead,
                           Cost->ScratchRegisters, Cost->SpilledRegisters,
                           Cost->SpillOverhead);
    if (CurrentLoop)
      BC.outs() << formatv(
          " loop={0:x}", Address + CurrentLoop->getHeader()->getInputOffset());
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
  }

  /// Compare cycles per iteration (per outer iteration for batch plans). This
  /// estimates potential savings, not measured stalls: overlap and contention
  /// cannot be reconstructed from miss counts alone.
  StringRef evaluateCost(PrefetchPlan &Plan) {
    Plan.Cost.HasEstimate = false;
    SmallVector<MCInst, 16> Always(Plan.Code.begin(),
                                   Plan.Code.begin() + Plan.GuardedStart);
    Always.append(Plan.Guard.begin(), Plan.Guard.end());
    SmallVector<MCInst, 16> Requests(Plan.Code.begin() + Plan.GuardedStart,
                                     Plan.Code.end());
    if (Plan.Continuation) {
      MCInst Jump;
      BC.MIB->createUncondBranch(Jump, Plan.Continuation, BC.Ctx.get());
      // Charge both transfers even if branch fixups later eliminate one.
      Requests.push_back(Jump);
      Requests.push_back(Jump);
    }
    auto AlwaysCycles = estimateCycles(BC, Always, /*Generated=*/true);
    auto RequestCycles = estimateCycles(BC, Requests, /*Generated=*/true);
    if (!AlwaysCycles || !RequestCycles)
      return "unavailable prefetch instruction cost";

    Plan.Cost.SpillOverhead = 0;
    if (Plan.Spill) {
      const MCInst Transfers[] = {Plan.Spill->Save, Plan.Spill->Restore};
      auto SpillCycles = estimateCycles(BC, Transfers, /*Generated=*/true);
      if (!SpillCycles)
        return "unavailable spill instruction cost";
      // Conservatively serialize the saved 64-bit values as load-latency-sized
      // stack transfers, even when pair instructions have better throughput.
      const uint64_t MemoryCycles =
          2 * Plan.Spill->Registers.size() *
          uint64_t(std::max(1u, BC.STI->getSchedModel().LoadLatency));
      const uint64_t Calls = BF.getKnownExecutionCount();
      const uint64_t Executions = Plan.BB->getKnownExecutionCount();
      if (!Calls || !Executions)
        return "unavailable spill execution frequency";
      // A fixed CFA makes this slot reusable within one function invocation.
      // Charge a cold miss per saved register per invocation, capped at one per
      // actual execution of the window. Do not assume a cache-line size.
      const double ColdRate = std::min(1.0, double(Calls) / Executions);
      Plan.Cost.SpillOverhead =
          std::max(*SpillCycles, MemoryCycles) +
          ColdRate * Plan.Spill->Registers.size() * DefaultPrefetchLatency;
    }
    double IndexPenalty = 0;
    if (Plan.Kind == PrefetchKind::Indirect) {
      if (!Plan.IndexProfile || !Plan.IndexProfile->AccessCount ||
          Plan.IndexProfile->MissCount > Plan.IndexProfile->AccessCount)
        return "unavailable index-load miss cost";
      // The sequence already includes the index load's hit latency. Charge
      // only the additional delay of a miss, using its original PC's profile.
      bool Found = false;
      for (const MCInst &MI : Plan.Code) {
        const MCInstrDesc &Desc = BC.MII->get(MI.getOpcode());
        if (!Desc.mayLoad() || !Desc.getNumDefs())
          continue;
        auto Hit = estimateCycles(BC, ArrayRef<MCInst>(MI), /*Generated=*/true);
        if (!Hit)
          return "unavailable index-load instruction cost";
        Found = true;
        IndexPenalty =
            double(Plan.IndexProfile->MissCount) /
            Plan.IndexProfile->AccessCount *
            (DefaultPrefetchLatency - std::min(DefaultPrefetchLatency, *Hit));
        break;
      }
      if (!Found)
        return "unavailable index-load instruction cost";
    }
    // Unknown indirect-guard frequency is charged at its full execution rate.
    // A fixed-stride cache-line guard has an analytically known request rate.
    const double Rate =
        Plan.StepMagnitude
            ? double(Plan.StepMagnitude) / unsigned(opts::PrefetchCacheLineSize)
            : 1;
    Plan.Cost.Overhead = *AlwaysCycles +
                         Rate * (*RequestCycles + IndexPenalty) +
                         Plan.Cost.SpillOverhead;
    const double Iterations = Plan.BatchIterations ? Plan.BatchIterations : 1;
    const double IterationCycles =
        double(Plan.Policy.IterationTotal) / Plan.Policy.IterationSamples;
    const double Lead = *Plan.Policy.Distance * IterationCycles -
                        (*AlwaysCycles + *RequestCycles + IndexPenalty +
                         Plan.Cost.SpillOverhead);
    const double Hidden = std::clamp(Lead, 0.0, double(DefaultPrefetchLatency));
    double Misses =
        double(Plan.Profile.MissCount) / Plan.Profile.AccessCount * Iterations;
    if (Plan.StepMagnitude)
      Misses = std::min(Misses, Rate);
    if (Plan.BatchIterations)
      Misses = std::min(Misses, double(Plan.Requests));
    Plan.Cost.Benefit = Misses * Hidden;
    // Measured iteration time already includes overlapped memory stalls. Do
    // not claim savings beyond that time. Static time excludes cache misses.
    if (!Plan.Policy.UsesEstimatedCycles)
      Plan.Cost.Benefit = std::min(Plan.Cost.Benefit, IterationCycles);
    Plan.Cost.HasEstimate = true;
    if (Plan.Cost.Benefit <= Plan.Cost.Overhead)
      return "prefetch cost exceeds estimated benefit";
    Plan.Cost.Priority = (Plan.Cost.Benefit - Plan.Cost.Overhead) *
                         Plan.Profile.AccessCount / Iterations;
    return {};
  }

  /// A spill window stays inside one block and returns to its original CFI
  /// state. Restrict it to a fixed SP-based CFA and idempotent state anchors.
  std::optional<PrefetchSpill> getSpillFrame(BinaryBasicBlock &BB,
                                             unsigned Index) {
    PrefetchSpill Spill;
    Spill.CFIState = BB.getCFIStateAtInstr(
        Index < BB.size() ? &BB.getInstructionAtIndex(Index) : nullptr);
    const auto &FDE = BF.getFDEProgram();
    if (!Spill.CFIState && !FDE.empty())
      return std::nullopt;
    unsigned CFAReg = UINT_MAX;
    int64_t Offset = -1;
    auto Update = [&](const MCCFIInstruction &CFI) {
      switch (CFI.getOperation()) {
      case MCCFIInstruction::OpDefCfa:
        CFAReg = CFI.getRegister();
        Offset = CFI.getOffset();
        break;
      case MCCFIInstruction::OpDefCfaRegister:
        CFAReg = CFI.getRegister();
        break;
      case MCCFIInstruction::OpDefCfaOffset:
        Offset = CFI.getOffset();
        break;
      case MCCFIInstruction::OpAdjustCfaOffset:
        if (Offset < 0 || AddOverflow(Offset, CFI.getOffset(), Offset))
          return false;
        break;
      case MCCFIInstruction::OpOffset:
      case MCCFIInstruction::OpRelOffset:
      case MCCFIInstruction::OpRestore:
      case MCCFIInstruction::OpSameValue:
      case MCCFIInstruction::OpUndefined:
      case MCCFIInstruction::OpRegister:
      case MCCFIInstruction::OpGnuArgsSize:
        break;
      default:
        return false;
      }
      return true;
    };
    for (const MCCFIInstruction &CFI : BF.cie())
      if (!Update(CFI))
        return std::nullopt;
    for (int32_t I = 0; I < Spill.CFIState; ++I)
      if (!Update(FDE[I]))
        return std::nullopt;
    const unsigned SP =
        BC.MRI->getDwarfRegNum(BC.MIB->getStackPointer(), false);
    if (CFAReg != SP || Offset < 0 || Offset % 16 || Offset > INT32_MAX - 16)
      return std::nullopt;
    if (Spill.CFIState) {
      const auto Op = FDE[Spill.CFIState - 1].getOperation();
      // Relative updates cannot re-anchor a state; args-size is not a state.
      if (Op == MCCFIInstruction::OpAdjustCfaOffset ||
          Op == MCCFIInstruction::OpGnuArgsSize)
        return std::nullopt;
    }
    Spill.CFAOffset = Offset;
    return Spill;
  }

  void planAccess(LoopAnalysis &Analysis, BinaryBasicBlock *BB, unsigned I,
                  bool EveryIteration,
                  SmallVectorImpl<SelectedAccess> &Selected) {
    BinaryLoop *Loop = &Analysis.Loop;
    auto &Latches = Analysis.Latches;
    auto &BatchLoop = Analysis.BatchLoop;
    auto &IndirectLoop = Analysis.IndirectLoop;
    const auto &Cycles = Analysis.Cycles;
    const unsigned LineSize = opts::PrefetchCacheLineSize;
    auto Stride = [&](MCPhysReg Reg) { return Analysis.stride(Reg); };
    auto OuterStride = [&](MCPhysReg Reg) { return Analysis.outerStride(Reg); };
    MCInst &MI = BB->getInstructionAtIndex(I);
    auto Access = BC.MIB->analyzeLoopMemory(MI);
    if (!Access) {
      if (BC.MIB->mayLoad(MI) &&
          BC.MIB->hasAnnotation(MI, "CacheMissProfile")) {
        ++Stats.Rejected;
        diagnose(&MI, "unsupported address", {});
      }
      return;
    }
    PrefetchPolicy Policy;
    auto Reject = [&](StringRef Reason) {
      ++Stats.Rejected;
      diagnose(&MI, Reason, Policy);
    };
    auto Profile =
        BC.MIB->tryGetAnnotationAs<CacheMissProfile>(MI, "CacheMissProfile");
    if (!Profile)
      return;
    if (!Profile->AccessCount || Profile->MissCount > Profile->AccessCount) {
      Reject("invalid miss/access counts");
      return;
    }
    if (double(Profile->MissCount) / Profile->AccessCount <=
        double(opts::CacheMissCutOff) / 10000) {
      diagnose(&MI, "miss rate below cutoff", Policy);
      return;
    }
    if (StringRef Reason =
            computeAutoDistance(Policy, Cycles, Analysis.EstimatedCycles);
        !Reason.empty()) {
      Reject(Reason);
      return;
    }
    std::optional<BatchAccess> Batch;
    if (BatchLoop) {
      Batch = getBatchAccess(*BB, I, *Access, *BatchLoop, *Policy.Distance,
                             Stride, OuterStride);
      if (!Batch) {
        Reject("unsupported batch address or footprint");
        return;
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
    } else if (!BaseStride || !IndexStride || (!*BaseStride && !*IndexStride) ||
               MulOverflow(*BaseStride, int64_t(*Policy.Distance), BaseDelta) ||
               MulOverflow(*IndexStride, int64_t(*Policy.Distance),
                           IndexDelta)) {
      if (IndirectLoop)
        Indirect = getIndirectAccess(*BB, I, *Access, *IndirectLoop,
                                     *Policy.Distance, Stride);
      if (!Indirect) {
        auto Next = getNextPointer(BF, *Loop, Latches, *BB, MI, *Access);
        if (!Next) {
          Reject(opts::PrefetchIndirect
                     ? "unproven recurrence or unsupported indirect bounds"
                     : "unproven recurrence");
          return;
        }
        if (*Policy.Distance != 1) {
          Reject("pointer chase requires distance 1");
          return;
        }
        NextPointer = *Next;
        AfterLoad = true;
      }
      BaseDelta = IndexDelta = 0;
    } else {
      ByteStep =
          uint64_t(*BaseStride) + (uint64_t(*IndexStride) << Access->Shift);
    }
    const uint64_t Magnitude = AbsoluteValue(ByteStep);
    const bool Throttle =
        LineSize && Magnitude && Magnitude < LineSize && EveryIteration;
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
    const unsigned Requests = Batch ? Batch->Requests : PrefetchAccess.Elements;
    if (Batch && (Requests > opts::PrefetchMaxInstructions ||
                  Requests > opts::PrefetchMaxPerLoop ||
                  Requests > opts::PrefetchMaxPerFunction ||
                  Requests > MaxLoopInstructions ||
                  Requests > MaxFunctionInstructions)) {
      Reject("batch request/instruction budget");
      return;
    }
    // Equal effective addresses imply the same cache line. Do not
    // merge different offsets without a proof of base alignment.
    auto Duplicate = llvm::find_if(Selected, [&](const SelectedAccess &S) {
      if (!S.Access.hasSameAddress(PrefetchAccess) ||
          S.BaseDelta != BaseDelta || S.IndexDelta != IndexDelta ||
          S.Distance != *Policy.Distance)
        return false;
      BitVector Changed(Available.size());
      for (unsigned J = S.Index; J < I; ++J)
        BC.MIB->getWrittenRegs(BB->getInstructionAtIndex(J), Changed);
      return !Changed.anyCommon(BC.MIB->getAliases(Access->Base, true)) &&
             (!Access->Index ||
              !Changed.anyCommon(BC.MIB->getAliases(Access->Index, true)));
    });
    if (Duplicate != Selected.end()) {
      auto &Earlier = Plans[Duplicate->PlanIndex];
      // Compare complete observations; do not sum duplicate sample counts.
      PrefetchPlan Alternative = Earlier;
      Alternative.Profile = *Profile;
      if (evaluateCost(Alternative).empty() &&
          Alternative.Cost.Priority > Earlier.Cost.Priority) {
        Earlier.Load = &MI;
        Earlier.Profile = Alternative.Profile;
        Earlier.Cost = Alternative.Cost;
      }
      diagnose(&MI, "duplicate address", Policy);
      return;
    }
    SmallVector<MCPhysReg, 2> Scratch;
    BinaryBasicBlock *EmitBB = Batch ? BatchLoop->Entry : BB;
    const unsigned EmitIndex = Batch ? BatchLoop->EntryIndex : I;
    const auto LiveAt =
        Batch
            ? (EmitIndex < EmitBB->size()
                   ? Live->getStateAt(EmitBB->getInstructionAtIndex(EmitIndex))
                   : Live->getStateAt(*EmitBB))
            : (AfterLoad ? Live->getStateBefore(MI) : Live->getStateAt(MI));
    for (int Reg : Available.set_bits()) {
      if (Scratch.size() == 2)
        break;
      // Alias-mask intersections can include a common tuple even
      // for disjoint registers. Query the physical register itself.
      if (!LiveAt->test(Reg))
        Scratch.push_back(Reg);
    }
    if ((Throttle || Indirect) && Scratch.size() < 2) {
      Reject(Indirect ? "indirect prefetch needs two dead registers"
                      : "throttling needs two dead registers");
      return;
    }
    PrefetchPlan Plan{EmitBB, EmitIndex, {}, {}};
    Plan.Load = &MI;
    if (Indirect || Throttle)
      Plan.Continuation = BC.Ctx->createNamedTempSymbol("PrefetchJoin");
    auto Encode = [&]() {
      Plan.Code.clear();
      Plan.Guard.clear();
      if (Batch) {
        for (unsigned R = 0; R < Requests; ++R) {
          auto Point = PrefetchAccess;
          const uint64_t Offset =
              std::min(uint64_t(R) * unsigned(LineSize), Batch->Span - 1);
          if (AddOverflow(Point.Offset, int64_t(Offset), Point.Offset) ||
              !BC.MIB->createLoopPrefetch(Point, BaseDelta, 0, Scratch,
                                          opts::PrefetchHint, Plan.Code,
                                          /*MaterializeAddress=*/false))
            return false;
        }
        Plan.Kind = PrefetchKind::Batch;
        Plan.BatchIterations = BatchLoop->Iterations;
        return true;
      }
      if (Indirect) {
        Plan.Kind = PrefetchKind::Indirect;
        return BC.MIB->createLoopIndirectPrefetch(
            *Indirect, PrefetchAccess, *Policy.Distance, Scratch,
            opts::PrefetchHint, Plan.Continuation, BC.Ctx.get(), Plan.Code,
            Plan.Guard);
      }
      return BC.MIB->createLoopPrefetch(PrefetchAccess, BaseDelta, IndexDelta,
                                        Scratch, opts::PrefetchHint, Plan.Code,
                                        /*MaterializeAddress=*/Throttle);
    };
    bool Encoded = Encode();
    if (!Encoded && !Plan.Continuation && Scratch.size() < 2) {
      unsigned InsertIndex = EmitIndex + unsigned(AfterLoad);
      while (AfterLoad && InsertIndex < EmitBB->size() &&
             BC.MIB->isCFI(EmitBB->getInstructionAtIndex(InsertIndex)))
        ++InsertIndex;
      Plan.Spill = getSpillFrame(*EmitBB, InsertIndex);
      if (!Plan.Spill) {
        Reject("insufficient dead registers or unsupported spill frame");
        return;
      }
      BitVector Inputs(Available.size());
      for (MCPhysReg Reg : {PrefetchAccess.Base, PrefetchAccess.Index})
        if (Reg)
          Inputs |= BC.MIB->getAliases(Reg, true);
      for (int Reg : Available.set_bits()) {
        if (Scratch.size() == 2 || Encoded)
          break;
        if (!LiveAt->test(Reg) ||
            Inputs.anyCommon(BC.MIB->getAliases(Reg, true)))
          continue;
        Scratch.push_back(Reg);
        Plan.Spill->Registers.push_back(Reg);
        Encoded = Encode();
      }
    }
    if (!Encoded) {
      Reject("unsupported encoding or insufficient dead registers");
      return;
    }
    BitVector Written(Available.size()), Used(Available.size());
    for (const MCInst &MI : Plan.Code) {
      BC.MIB->getWrittenRegs(MI, Written);
      BC.MIB->getSrcRegs(MI, Used);
    }
    if (Plan.Spill) {
      if (Used.anyCommon(BC.MIB->getAliases(BC.MIB->getStackPointer(), true))) {
        Reject("spill would change an SP-based prefetch address");
        return;
      }
      llvm::erase_if(Plan.Spill->Registers, [&](MCPhysReg Reg) {
        return !Written.anyCommon(BC.MIB->getAliases(Reg, true));
      });
      if (Plan.Spill->Registers.empty()) {
        Plan.Spill.reset();
      } else if (!BC.MIB->createLoopPrefetchSpill(Plan.Spill->Registers,
                                                  Plan.Spill->Save,
                                                  Plan.Spill->Restore)) {
        Reject("unsupported spill save/restore sequence");
        return;
      }
    }
    if (Throttle) {
      if (!BC.MIB->createLoopPrefetchGuard(Scratch[0], Scratch[1], ByteStep,
                                           LineSize, Plan.Continuation,
                                           BC.Ctx.get(), Plan.Guard)) {
        Reject("unsupported throttle guard");
        return;
      }
      Plan.StepMagnitude = Magnitude;
      Plan.GuardedStart = Plan.Code.size() - Requests;
    }
    // Charge both possible CFG transfers even if FixupBranches later
    // removes a jump to the next block in the layout.
    const unsigned AddedInstructions = Plan.Code.size() + Plan.Guard.size() +
                                       (Plan.Continuation ? 2 : 0) +
                                       (Plan.Spill ? 2 : 0);
    if (AddedInstructions > opts::PrefetchMaxInstructions) {
      Reject("instruction budget");
      return;
    }
    Plan.Requests = Requests;
    Plan.Loop = Batch ? BatchLoop->Outer : Loop;
    Plan.Policy = Policy;
    Plan.Profile = *Profile;
    if (AfterLoad)
      Plan.Kind = PrefetchKind::Pointer;
    Plan.BaseDelta = BaseDelta;
    Plan.IndexDelta = IndexDelta;
    Plan.Instructions = AddedInstructions;
    if (Indirect)
      if (auto Profile = BC.MIB->tryGetAnnotationAs<CacheMissProfile>(
              *Indirect->IndexLoad, "CacheMissProfile"))
        Plan.IndexProfile = *Profile;
    for (const MCInst &MI : Plan.Guard)
      BC.MIB->getWrittenRegs(MI, Written);
    for (MCPhysReg Reg : Scratch)
      Plan.Cost.ScratchRegisters +=
          Written.anyCommon(BC.MIB->getAliases(Reg, true));
    Plan.Cost.SpilledRegisters = Plan.Spill ? Plan.Spill->Registers.size() : 0;
    ++Stats.Planned;
    if (StringRef Reason = evaluateCost(Plan); !Reason.empty()) {
      diagnose(&MI, Reason, Policy, &Plan.Cost);
      ++Stats.Rejected;
      return;
    }
    Selected.push_back({I, unsigned(Plans.size()), PrefetchAccess, BaseDelta,
                        IndexDelta, *Policy.Distance});
    Plans.push_back(std::move(Plan));
  }

  void planLoop(BinaryLoop *Loop) {
    CurrentLoop = Loop;
    if (!Loop->getSubLoops().empty() || getIterationOrder(*Loop).empty()) {
      diagnose(nullptr, "non-innermost or irreducible loop", {});
      return;
    }
    const bool WantsBatch = opts::PrefetchBatch && Loop->getParentLoop();
    std::optional<BatchLoopInfo> BatchLoop;
    if (WantsBatch) {
      BatchLoop = analyzeBatchLoop(*Loop);
      if (!BatchLoop) {
        diagnose(nullptr, "unsupported constant inner-loop batch shape", {});
        return;
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
        Unsafe |= Barrier ||
                  BC.MIB->getAnnotationWithDefault(MI, "LoopDataPrefetch", 0u);
      }
    }
    if (Unsafe) {
      diagnose(nullptr, "unsupported loop context", {});
      return;
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

    const bool EstimatedCycles =
        TimingLatches.size() != 1 || Cycles.Total <= 0 || Cycles.Samples <= 0;
    if (EstimatedCycles) {
      auto Estimate = estimateLoopCycles(
          *CurrentLoop, BatchLoop ? Loop->getHeader() : nullptr,
          BatchLoop ? BatchLoop->Iterations : 1);
      Cycles = Estimate ? CycleStats{*Estimate, 1} : CycleStats{};
    }

    std::optional<IndirectLoopInfo> IndirectLoop;
    if (opts::PrefetchIndirect)
      IndirectLoop = analyzeIndirectLoop(*Loop);
    LoopAnalysis Analysis{
        BF, *Loop, Latches, Cycles, EstimatedCycles, BatchLoop, IndirectLoop};
    for (BinaryBasicBlock *BB : Loop->blocks()) {
      // Throttle only loads that execute on every backedge.
      const bool EveryIteration =
          opts::PrefetchCacheLineSize && dominatesAll(BF, BB, Latches);
      SmallVector<SelectedAccess, 2> Selected;
      for (unsigned I = 0; I < BB->size(); ++I)
        planAccess(Analysis, BB, I, EveryIteration, Selected);
    }
  }

  void selectPlans() {
    // Rank profitable plans by estimated aggregate gain within one sampling
    // protocol. Free scratch registers carry no independent penalty.
    SmallVector<PrefetchPlan *, 8> Ranked;
    for (auto &Plan : Plans)
      Ranked.push_back(&Plan);
    llvm::stable_sort(Ranked, [](const PrefetchPlan *A, const PrefetchPlan *B) {
      if (A->Cost.Priority != B->Cost.Priority)
        return A->Cost.Priority > B->Cost.Priority;
      if (A->Profile.MissCount != B->Profile.MissCount)
        return A->Profile.MissCount > B->Profile.MissCount;
      return A->Instructions < B->Instructions;
    });
    DenseMap<const BinaryLoop *, std::pair<unsigned, unsigned>> LoopBudgets;
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
                   MaxLoopInstructions ||
               uint64_t(FunctionInstructions) + Plan->Instructions >
                   MaxFunctionInstructions)
        Reason = "instruction budget";
      if (!Reason.empty()) {
        ++Stats.Rejected;
        diagnose(&MI, Reason, Plan->Policy, &Plan->Cost);
        continue;
      }
      Plan->Selected = true;
      LoopRequests += Plan->Requests;
      FunctionRequests += Plan->Requests;
      LoopInstructions += Plan->Instructions;
      FunctionInstructions += Plan->Instructions;
      ++Stats.Accepted;
      Stats.Added += Plan->Instructions;
      BC.MIB->addAnnotation(Plan->Code.front(), "LoopDataPrefetch", 1u);
      std::string Detail =
          formatv("inserted base-delta={0} index-delta={1} instructions={2} "
                  "cache-line={3} placement={4} kind={5}",
                  Plan->BaseDelta, Plan->IndexDelta, Plan->Instructions,
                  Plan->Continuation && Plan->Kind != PrefetchKind::Indirect
                      ? unsigned(opts::PrefetchCacheLineSize)
                      : 0,
                  Plan->getPlacement(), Plan->getKindName())
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
      diagnose(&MI, Detail, Plan->Policy, &Plan->Cost);
    }
  }

  void commitPlans() {
    bool ChangedCFG = false;
    for (auto &Plan : llvm::reverse(Plans)) {
      if (!Plan.Selected)
        continue;
      if (Plan.Continuation) {
        insertGuardedPrefetch(BF, Plan);
        ChangedCFG = true;
      } else {
        const bool AfterLoad = Plan.Kind == PrefetchKind::Pointer;
        unsigned Index = Plan.Index + AfterLoad;
        // Rules at the load's outgoing PC must also cover the new prefetch.
        while (AfterLoad && Index < Plan.BB->size() &&
               BC.MIB->isCFI(Plan.BB->getInstructionAtIndex(Index)))
          ++Index;
        if (Plan.Spill) {
          insertSpilledPrefetch(BF, Plan, Index);
        } else {
          for (MCInst &MI : llvm::reverse(Plan.Code))
            Plan.BB->insertInstruction(Plan.BB->begin() + Index, MI);
        }
      }
    }
    if (ChangedCFG) {
      BF.constructDomTree();
      BF.calculateLoopInfo();
    }
  }

public:
  FunctionPrefetchPlanner(BinaryFunction &BF, RegAnalysis &RA,
                          MCPlusBuilder::AllocatorIdTy AnalysisAllocator,
                          PrefetchStats &Stats)
      : BF(BF), BC(BF.getBinaryContext()), Address(BF.getAddress()), RA(RA),
        AnalysisAllocator(AnalysisAllocator), Stats(Stats),
        Available(BC.MRI->getNumRegs()) {}

  void run() {
    unsigned Instructions = 0;
    for (const BinaryBasicBlock &BB : BF)
      Instructions += BB.size();
    if (Instructions > opts::PrefetchAnalysisLimit) {
      diagnose(nullptr, "analysis limit", {});
      return;
    }
    BF.calculateLoopInfo();
    if (BF.getLoopInfo().empty())
      return;
    BC.MIB->getGPRegs(Available, false);
    BC.MIB->removeNonScavengeableRegs(Available);
    BitVector CalleeSaved(Available.size());
    BC.MIB->getCalleeSavedRegs(CalleeSaved);
    Available.reset(CalleeSaved);
    if (!excludeCFIRegisters(BF, Available)) {
      diagnose(nullptr, "opaque CFI expression", {});
      return;
    }
    // This pass targets AAPCS64 user ELF. Restrict scratch to caller-saved
    // registers; use conservative call information from the existing analysis.
    Live = std::make_unique<LivenessAnalysis>(RA, BF, AnalysisAllocator,
                                              /*AssumeABI=*/true);
    Live->run();
    for (BinaryLoop *TopLoop : BF.getLoopInfo())
      for (BinaryLoop *Loop : TopLoop->getLoopsInPreorder())
        planLoop(Loop);
    selectPlans();
    // Release analysis annotations before moving instructions.
    Live.reset();
    BC.MIB->freeValuesAllocator(AnalysisAllocator);
    commitPlans();
    Stats.Inserted += FunctionRequests;
  }
};

} // namespace

bool LoopDataPrefetch::shouldOptimize(const BinaryFunction &BF) const {
  return BinaryFunctionPass::shouldOptimize(BF) && !BF.isMultiEntry() &&
         !BF.hasUnknownControlFlow() && BF.hasCacheMissProfile() &&
         BF.hasProfile() && (BF.getProfileFlags() & BinaryFunction::PF_BRANCH);
}

Error LoopDataPrefetch::runOnFunctions(BinaryContext &BC) {
  if (Error E = validateOptions(BC))
    return E;
  RegAnalysis RA(BC, nullptr, nullptr);
  const auto AnalysisAllocator = BC.MIB->initializeNewAnnotationAllocator();
  PrefetchStats Stats;
  for (auto &[Address, BF] : BC.getBinaryFunctions()) {
    if (!shouldOptimize(BF))
      continue;
    FunctionPrefetchPlanner(BF, RA, AnalysisAllocator, Stats).run();
  }
  BC.outs() << "BOLT-INFO: number of prefetch instructions inserted: "
            << Stats.Inserted << " (rejected candidates: " << Stats.Rejected
            << ")\n";
  BC.outs() << "BOLT-INFO: prefetch plans: " << Stats.Planned
            << ", accepted loads: " << Stats.Accepted
            << ", added instructions: " << Stats.Added << "\n";
  return Error::success();
}
