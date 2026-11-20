//===- LoopDataPrefetch.cpp - Loop data prefetching -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/LoopDataPrefetch.h"
#include "bolt/Passes/LivenessAnalysis.h"
#include "bolt/Passes/DataflowInfoManager.h"
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
    cl::desc("cache line bytes for batch prefetch request accounting"),
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
static cl::opt<unsigned> PrefetchEntryLines(
    "loop-prefetch-entry-lines", cl::init(0),
    cl::desc("prefetch N cache lines at the loop entry for variable-trip-count "
             "loops (0 disables; e.g. 3 covers 192 bytes ahead of the base)"),
    cl::cat(BoltOptCategory));
static cl::opt<double> PrefetchEntryTripThreshold(
    "loop-prefetch-entry-trip-threshold", cl::init(100.0),
    cl::desc("average loop trip count above which the loop keeps the in-loop "
             "rolling strategy instead of entry prefetch (default 100)"),
    cl::cat(BoltOptCategory));
static cl::opt<bool> PrefetchSlotStream(
    "loop-prefetch-slot-stream",
    cl::desc("prefetch data streams whose address register is reloaded each "
             "iteration from a frame-relative stack slot: insert the slot "
             "reload plus a distance-ahead prefetch at the loop head"),
    cl::cat(BoltOptCategory), cl::Hidden);
static cl::opt<unsigned> PrefetchSlotDistance(
    "loop-prefetch-slot-distance", cl::init(128),
    cl::desc("bytes to prefetch ahead of a slot-stream base (default 128)"),
    cl::cat(BoltOptCategory), cl::Hidden);
static cl::opt<unsigned> PrefetchSlotMinMisses(
    "loop-prefetch-slot-min-misses", cl::init(10),
    cl::desc("minimum sampled misses (slot reload plus data loads) for a "
             "slot-stream candidate"),
    cl::cat(BoltOptCategory), cl::Hidden);
static cl::opt<unsigned> PrefetchSlotMaxPerFunction(
    "loop-prefetch-slot-max-per-function", cl::init(4),
    cl::desc("maximum slot-stream prefetches per function"),
    cl::cat(BoltOptCategory), cl::Hidden);
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
  uint64_t BatchIterations = 0;
  const BinaryLoop *Loop = nullptr;
  PrefetchPolicy Policy{};
  uint64_t Misses = 0;
  int64_t BaseDelta = 0, IndexDelta = 0;
  unsigned Instructions = 0;
  bool Manual = false, Selected = false, AfterLoad = false, Indirect = false;
  bool EntryPrefetch = false;
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
    // Only the indirect path uses a guard. No trip-count distribution is
    // available for the remaining-count test.
    PrefetchCount = Count / 2;
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

  const auto FirstRequest = Plan.Code.begin();
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

struct EntryPrefetchPoint {
  BinaryBasicBlock *BB = nullptr;
  unsigned Index = 0;
  MCPhysReg BaseReg = 0;
  int64_t OffsetAdjust = 0; // accumulated writeback to subtract from PRFM offset
};

/// Trace one path backwards from a block boundary, following unique
/// predecessors, MOV assignments, and writeback loads, until reaching
/// a non-trivial definition of the base register.  Handles nested join
/// points by recursively tracing each predecessor path (bounded depth).
static bool traceOnePath(BinaryFunction &BF, BinaryLoop *InnerLoop,
                         MCPhysReg BaseReg, const BinaryBasicBlock *StartBB,
                         unsigned StartIdx, int64_t OffsetAdjust,
                         unsigned Depth,
                         SmallVectorImpl<EntryPrefetchPoint> &Results) {
  if (Depth == 0)
    return false;
  BinaryContext &BC = BF.getBinaryContext();
  const auto &MIB = *BC.MIB;
  BitVector Aliases = MIB.getAliases(BaseReg, true);
  const BinaryBasicBlock *Block = StartBB;
  unsigned Index = StartIdx;
  unsigned Limit = 64;
  bool Found = false;

  while (Limit--) {
    // Scan backwards within the current block.
    while (Index) {
      const MCInst &MI = *(Block->begin() + --Index);
      BitVector Written(Aliases.size());
      MIB.getWrittenRegs(MI, Written);
      if (!Written.anyCommon(Aliases))
        continue;
      if (InnerLoop->contains(const_cast<BinaryBasicBlock *>(Block)))
        continue;

      // MOV penetration
      if (auto A = MIB.analyzeLoopRegisterAssignment(MI, BaseReg)) {
        if (A->Base && !A->Index && !A->Shift && !A->SubtractIndex &&
            A->Base != BaseReg) {
          BaseReg = A->Base;
          Aliases = MIB.getAliases(BaseReg, true);
          continue;
        }
        // Self-referencing ADD/SUB: base = base + imm (like writeback).
        // Treat as offset accumulation and continue tracing.
        if (A->Base == BaseReg && !A->Index && !A->Shift) {
          if (int64_t Step; MIB.getLoopRegisterUpdate(MI, BaseReg, Step)) {
            OffsetAdjust += Step;
            continue;
          }
        }
      }

      // Writeback load penetration
      if (auto A = MIB.analyzeLoopMemory(MI)) {
        if (A->Base == BaseReg && A->HasWriteback) {
          OffsetAdjust += A->Writeback;
          continue;
        }
      }

      // Real definition: return insertion point
      EntryPrefetchPoint P;
      P.BB = const_cast<BinaryBasicBlock *>(Block);
      P.Index = Index + 1;
      P.BaseReg = BaseReg;
      P.OffsetAdjust = OffsetAdjust;
      Results.push_back(P);
      Found = true;
      break;
    }
    if (Found)
      return true;

    // Block exhausted: move to the predecessor.
    if (Block->pred_size() == 1) {
      Block = *Block->pred_begin();
      Index = Block->size();
      continue;
    }

    if (Block->pred_size() > 1) {
      // Loop header: follow the unique entry predecessor.
      BinaryLoop *BlockLoop = BF.getLoopInfo().getLoopFor(
          const_cast<BinaryBasicBlock *>(Block));
      if (BlockLoop && BlockLoop->getHeader() == Block) {
        BinaryBasicBlock *EntryPred = nullptr;
        for (BinaryBasicBlock *Pred :
             const_cast<BinaryBasicBlock *>(Block)->predecessors()) {
          if (!BlockLoop->contains(Pred)) {
            if (EntryPred)
              return Found;
            EntryPred = Pred;
          }
        }
        if (!EntryPred)
          return Found;
        Block = EntryPred;
        Index = Block->size();
        continue;
      }
      // Join point: recursively trace each predecessor path.
      for (BinaryBasicBlock *Pred :
           const_cast<BinaryBasicBlock *>(Block)->predecessors()) {
        if (InnerLoop->contains(Pred))
          continue;
        traceOnePath(BF, InnerLoop, BaseReg, Pred, Pred->size(),
                     OffsetAdjust, Depth - 1, Results);
      }
      return !Results.empty();
    }
    return Found;
  }
  return Found;
}

/// Find entry prefetch insertion points for a variable-trip-count loop.
/// If the backwards trace from the load hits a join point with multiple
/// predecessors, trace EACH predecessor path separately and return one
/// insertion point per path.  Each path gets its own PRFM.
/// If RD (reaching definitions) is provided, hoist the insertion point
/// to the earliest dominator ancestor where the defining instruction's
/// effect still reaches (the register provably holds the same value).
static SmallVector<EntryPrefetchPoint, 4>
findBaseInitialization(BinaryFunction &BF, BinaryLoop *InnerLoop,
                       MCPhysReg BaseReg, const BinaryBasicBlock &LoadBB,
                       unsigned LoadIdx,
                       ReachingDefOrUse</*Def=*/true> *RD = nullptr) {
  SmallVector<EntryPrefetchPoint, 4> Results;

  // First: try the simple single-path trace.
  // Scan backwards from the load within its own block, then through
  // unique predecessors until we find the base definition or a join.
  BinaryContext &BC = BF.getBinaryContext();
  const auto &MIB = *BC.MIB;
  BitVector Aliases = MIB.getAliases(BaseReg, true);
  const BinaryBasicBlock *Block = &LoadBB;
  unsigned Index = LoadIdx;
  unsigned Limit = 64;
  int64_t OffsetAdjust = 0;
  MCPhysReg CurReg = BaseReg;
  // Fallback: remember the position of the first self-ADD or writeback
  // outside the loop.  If deeper tracing fails, use this as insertion point.
  std::optional<EntryPrefetchPoint> Fallback;

  while (Limit--) {
    bool Found = false;
    while (Index) {
      const MCInst &MI = *(Block->begin() + --Index);
      BitVector Written(Aliases.size());
      MIB.getWrittenRegs(MI, Written);
      if (!Written.anyCommon(Aliases))
        continue;
      if (InnerLoop->contains(const_cast<BinaryBasicBlock *>(Block)))
        continue;

      // MOV penetration
      if (auto A = MIB.analyzeLoopRegisterAssignment(MI, CurReg)) {
        if (A->Base && !A->Index && !A->Shift && !A->SubtractIndex &&
            A->Base != CurReg) {
          CurReg = A->Base;
          Aliases = MIB.getAliases(CurReg, true);
          continue;
        }
        // Self-referencing ADD/SUB: base = base + imm (like writeback).
        if (A->Base == CurReg && !A->Index && !A->Shift) {
          if (int64_t Step; MIB.getLoopRegisterUpdate(MI, CurReg, Step)) {
            if (!Fallback) {
              Fallback = EntryPrefetchPoint{};
              Fallback->BB = const_cast<BinaryBasicBlock *>(Block);
              Fallback->Index = Index + 1;
              Fallback->BaseReg = CurReg;
              Fallback->OffsetAdjust = OffsetAdjust;
            }
            OffsetAdjust += Step;
            continue;
          }
        }
      }

      // Writeback load penetration
      if (auto A = MIB.analyzeLoopMemory(MI)) {
        if (A->Base == CurReg && A->HasWriteback) {
          if (!Fallback) {
            Fallback = EntryPrefetchPoint{};
            Fallback->BB = const_cast<BinaryBasicBlock *>(Block);
            Fallback->Index = Index + 1;
            Fallback->BaseReg = CurReg;
            Fallback->OffsetAdjust = OffsetAdjust;
          }
          OffsetAdjust += A->Writeback;
          continue;
        }
      }

      // Real definition found on this path
      EntryPrefetchPoint P;
      P.BB = const_cast<BinaryBasicBlock *>(Block);
      P.Index = Index + 1;
      P.BaseReg = CurReg;
      P.OffsetAdjust = OffsetAdjust;
      Results.push_back(P);
      Found = true;
      break;
    }
    if (Found)
      return Results;

    // Block exhausted: check predecessors.
    if (Block->pred_size() == 1) {
      Block = *Block->pred_begin();
      Index = Block->size();
      continue;
    }

    if (Block->pred_size() > 1) {
      // Check if this block is a loop header (any loop, not just
      // InnerLoop).  If so, follow the unique entry predecessor.
      BinaryLoop *BlockLoop = BF.getLoopInfo().getLoopFor(
          const_cast<BinaryBasicBlock *>(Block));
      if (BlockLoop && BlockLoop->getHeader() == Block) {
        BinaryBasicBlock *EntryPred = nullptr;
        for (BinaryBasicBlock *Pred :
             const_cast<BinaryBasicBlock *>(Block)->predecessors()) {
          if (!BlockLoop->contains(Pred)) {
            if (EntryPred)
              break; // multiple entry predecessors
            EntryPred = Pred;
          }
        }
        if (EntryPred) {
          Block = EntryPred;
          Index = Block->size();
          continue;
        }
      }

      // Not a loop header, or multiple entries: per-path tracing.
      if (!InnerLoop->contains(const_cast<BinaryBasicBlock *>(Block))) {
        for (BinaryBasicBlock *Pred :
             const_cast<BinaryBasicBlock *>(Block)->predecessors()) {
          if (InnerLoop->contains(Pred))
            continue; // skip back-edges of the innermost loop
          traceOnePath(BF, InnerLoop, CurReg, Pred, Pred->size(),
                       OffsetAdjust, /*Depth=*/5, Results);
        }
        break; // fall through to dedup below
      }
    }

    break; // no unique predecessor and not a traceable join
  }

  if (Results.empty() && Fallback)
    Results.push_back(*Fallback);

  // Deduplicate: multiple paths often converge to the same block with the
  // same base register.  Keep one insertion point per (BB, BaseReg) pair,
  // preferring the one with the smallest absolute offset adjustment
  // (covers the earliest data).
  {
    SmallVector<EntryPrefetchPoint, 4> Deduped;
    for (auto &P : Results) {
      auto It = llvm::find_if(Deduped, [&](const EntryPrefetchPoint &D) {
        return D.BB == P.BB && D.BaseReg == P.BaseReg;
      });
      if (It == Deduped.end()) {
        Deduped.push_back(P);
      } else if (std::abs(P.OffsetAdjust) < std::abs(It->OffsetAdjust)) {
        *It = P;
      }
    }
    Results = std::move(Deduped);
  }

  // PlanB: if multiple per-path results share the same base register and
  // one result's block dominates all others, merge them into that single
  // entry (the earliest common point).  This reduces PRFM count from
  // N-paths × lines to 1 × lines for this register.
  if (Results.size() > 1) {
    LLVM_DEBUG(
      for (auto &P : Results)
        dbgs() << "BOLT-PREFETCH-DBG: pre-merge result BB=" << P.BB
               << " Idx=" << P.Index << " Reg=" << P.BaseReg
               << " Off=" << P.OffsetAdjust << "\n";
    );
    // Group by base register.
    DenseMap<MCPhysReg, SmallVector<EntryPrefetchPoint *, 4>> ByReg;
    for (auto &P : Results)
      ByReg[P.BaseReg].push_back(&P);

    SmallVector<EntryPrefetchPoint, 4> Merged;
    for (auto &[Reg, Group] : ByReg) {
      if (Group.size() == 1) {
        Merged.push_back(*Group[0]);
        continue;
      }
      // Find the entry whose block dominates all others in this group.
      EntryPrefetchPoint *Dominant = nullptr;
      for (auto *P : Group) {
        bool DominatesAll = true;
        for (auto *Q : Group) {
          if (P == Q)
            continue;
          if (!BF.getDomTree().dominates(P->BB, Q->BB)) {
            DominatesAll = false;
            break;
          }
        }
        if (DominatesAll) {
          Dominant = P;
          break;
        }
      }
      if (Dominant) {
        // Use the dominant entry with the minimum absolute offset adjust.
        EntryPrefetchPoint M = *Dominant;
        for (auto *Q : Group)
          if (std::abs(Q->OffsetAdjust) < std::abs(M.OffsetAdjust))
            M.OffsetAdjust = Q->OffsetAdjust;
        Merged.push_back(M);
      } else {
        // No dominant entry: try the nearest common dominator block.
        BinaryBasicBlock *Common = Group[0]->BB;
        for (unsigned i = 1; i < Group.size(); ++i)
          Common = BF.getDomTree().findNearestCommonDominator(
              Common, Group[i]->BB);
        if (Common && !InnerLoop->contains(Common)) {
          // Insert at the beginning of the common dominator block.
          // Use the register from the first result (all share the same reg).
          EntryPrefetchPoint M;
          M.BB = Common;
          M.Index = 0;
          M.BaseReg = Reg;
          M.OffsetAdjust = 0;
          for (auto *Q : Group)
            M.OffsetAdjust = std::min(M.OffsetAdjust, Q->OffsetAdjust);
          Merged.push_back(M);
        } else {
          // Cannot merge: keep all per-path entries.
          for (auto *P : Group)
            Merged.push_back(*P);
        }
      }
    }
    Results = std::move(Merged);
  }

  // PlanB-ReachingDefs: use reaching-definitions dataflow to hoist the PRFM
  // to the position of the ORIGINAL base-register definition, bypassing
  // both the self-ADD and the join points that block per-path tracing.
  //
  // The per-path trace often stops at a self-ADD (writeback) that is the
  // last write before the loop starts (the Fallback).  RD can find the
  // definition that reaches BEFORE that self-ADD — the instruction that
  // originally computed the base address (e.g., `sub x0,x3,x19` or
  // `mov x13,x20`).  We then insert the PRFM right after that original
  // definition, giving maximum lead time without tracing through joins.
  if (RD && Results.size() == 1) {
    auto &P = Results[0];
    auto &BCRef = BF.getBinaryContext();

    // Only hoist when the per-path trace stopped at a self-ADD or
    // writeback (the Fallback).  If the trace found a real definition
    // (e.g., SUB/ADD with different source), the position is already
    // optimal and hoisting further could place the PRFM before the
    // register gets the correct value.
    if (P.Index > 0) {
      const MCInst &DefInst = P.BB->getInstructionAtIndex(P.Index - 1);
      bool IsSelfAddOrWriteback = false;
      if (auto A = BCRef.MIB->analyzeLoopMemory(DefInst)) {
        if (A->Base == P.BaseReg && A->HasWriteback)
          IsSelfAddOrWriteback = true;
      }
      if (auto A = BCRef.MIB->analyzeLoopRegisterAssignment(DefInst, P.BaseReg)) {
        if (A->Base == P.BaseReg && !A->Index && !A->Shift) {
          int64_t Step;
          if (BCRef.MIB->getLoopRegisterUpdate(DefInst, P.BaseReg, Step))
            IsSelfAddOrWriteback = true;
        }
      }
      if (!IsSelfAddOrWriteback) {
        return Results;  // Real definition found by trace: don't hoist.
      }
    } else {
      return Results;  // No defining instruction: don't hoist.
    }

    // The per-path trace placed the PRFM after the defining instruction
    // at P.Index-1 (e.g., the self-ADD).  Query RD at the position
    // BEFORE that defining instruction to find the ORIGINAL definition.
    const MCInst *QueryPoint = nullptr;
    BinaryBasicBlock *QueryBB = nullptr;
    unsigned QueryIdx = 0;

    if (P.Index >= 2) {
      QueryPoint = &P.BB->getInstructionAtIndex(P.Index - 2);
      QueryBB = P.BB;
      QueryIdx = P.Index - 2;
    } else if (P.Index == 1) {
      // Definition is at index 0. Query at the previous block's last
      // instruction (the unique predecessor on the entry path).
      auto *Node = BF.getDomTree().getNode(P.BB);
      if (Node && Node->getIDom()) {
        QueryBB = Node->getIDom()->getBlock();
        if (QueryBB && !InnerLoop->contains(QueryBB)) {
          const MCInst *Last = QueryBB->getLastNonPseudoInstr();
          if (Last) {
            QueryPoint = Last;
            QueryIdx = QueryBB->size() - 1;
          }
        }
      }
    }

    if (QueryPoint && QueryBB) {
      // Find the outermost loop to prevent hoisting past it.
      BinaryLoop *OutermostLoop = InnerLoop;
      while (OutermostLoop->getParentLoop())
        OutermostLoop = OutermostLoop->getParentLoop();

      // Multi-level RD hoisting: after finding the initial unique
      // definition, trace through MOV chains to get more lead time.
      // Each level queries RD for the SOURCE register of the MOV,
      // finding its unique definition and hoisting further back.
      unsigned MaxLevels = 5;
      MCPhysReg CurReg = P.BaseReg;
      const MCInst *QueryInstr = QueryPoint;
      BinaryBasicBlock *QueryBlock = QueryBB;

      while (MaxLevels--) {
        // Find unique reaching definition of CurReg at QueryInstr.
        const MCInst *UniqueDef = nullptr;
        BinaryBasicBlock *UniqueDefBB = nullptr;
        unsigned UniqueDefIdx = 0;
        unsigned NumCandidates = 0;

        for (BinaryBasicBlock &B : BF) {
          if (InnerLoop->contains(&B))
            continue;
          // Must dominate the query block to be on all paths.
          if (!BF.getDomTree().dominates(&B, QueryBlock))
            continue;
          for (unsigned I = 0; I < B.size(); ++I) {
            MCInst &Inst = B.getInstructionAtIndex(I);
            if (BCRef.MIB->isPseudo(Inst))
              continue;
            BitVector Written(BCRef.MRI->getNumRegs());
            BCRef.MIB->getWrittenRegs(Inst, Written);
            if (!Written.test(CurReg))
              continue;
            if (RD->count(*QueryInstr, Inst)) {
              ++NumCandidates;
              if (NumCandidates > 1)
                break;
              UniqueDef = &Inst;
              UniqueDefBB = &B;
              UniqueDefIdx = I;
            }
          }
          if (NumCandidates > 1)
            break;
        }

        if (NumCandidates != 1 || !UniqueDef || !UniqueDefBB)
          break;

        // Verify the found definition is still inside the outermost
        // enclosing loop.  Hoisting outside it would make the PRFM
        // execute only once per function call, not per loop iteration.
        if (!OutermostLoop->contains(UniqueDefBB))
          break;

        // Update the insertion point to after this definition.
        Results[0].BB = UniqueDefBB;
        Results[0].Index = UniqueDefIdx + 1;
        Results[0].BaseReg = CurReg;
        Results[0].OffsetAdjust = 0;

        // Check if the defining instruction is a MOV to a different
        // register.  If so, recurse on the source register.
        MCPhysReg SrcReg = 0;
        if (UniqueDefIdx < UniqueDefBB->size()) {
          const MCInst &DefInst = UniqueDefBB->getInstructionAtIndex(UniqueDefIdx);
          if (auto A = BCRef.MIB->analyzeLoopRegisterAssignment(DefInst, CurReg)) {
            if (A->Base && !A->Index && !A->Shift && !A->SubtractIndex &&
                A->Base != CurReg)
              SrcReg = A->Base;
          }
        }
        if (!SrcReg)
          break;  // Not a MOV: cannot trace further.

        // Set up the next query point: before the MOV definition.
        // Also verify the NEW position is still inside the outermost
        // loop so the PRFM executes once per outer-loop iteration.
        CurReg = SrcReg;
        if (!OutermostLoop->contains(UniqueDefBB))
          break;  // Hoisted past the outermost loop: PRFM would
                  // execute only once per function call.
        if (UniqueDefIdx >= 1) {
          QueryInstr = &UniqueDefBB->getInstructionAtIndex(UniqueDefIdx - 1);
          QueryBlock = UniqueDefBB;
        } else {
          // Definition at index 0: query at the dominator's last instruction.
          auto *Node = BF.getDomTree().getNode(UniqueDefBB);
          if (!Node || !Node->getIDom())
            break;
          QueryBlock = Node->getIDom()->getBlock();
          if (!QueryBlock || InnerLoop->contains(QueryBlock) ||
              !OutermostLoop->contains(QueryBlock))
            break;
          QueryInstr = QueryBlock->getLastNonPseudoInstr();
          if (!QueryInstr)
            break;
        }
      }
    }
  }

  return Results;
}

/// Determine whether a loop should use the entry multi-line prefetch
/// strategy: low average trip count and sequential small-stride access.
static bool shouldUseEntryStrategy(const BinaryLoop *Loop,
                                   const MCPlusBuilder::LoopMemoryAccess &Access,
                                   int64_t ByteStep) {
  if (!opts::PrefetchEntryLines)
    return false;
  if (!ByteStep || std::abs(ByteStep) > 64)
    return false;
  if (Access.Index)
    return false;
  if (Loop->EntryCount == 0 ||
      Loop->TotalBackEdgeCount == BinaryBasicBlock::COUNT_NO_PROFILE)
    return false;
  double AvgTrip = (double)Loop->TotalBackEdgeCount / Loop->EntryCount;
  return AvgTrip < opts::PrefetchEntryTripThreshold;
}

/// Slot-stream prefetching.  Loops whose body reloads a pointer from a
/// frame-relative stack slot each iteration and streams data through it
/// (e.g. zstd's literal pointer) get the slot reload plus a distance-ahead
/// prefetch inserted at the loop head.  The head reload warms the stack
/// line for the in-body reload, and the prefetch gains the whole loop body
/// as lead time over the data loads, mirroring source-level prefetch hints
/// placed at the top of the loop.  Only non-innermost loops are considered:
/// innermost loops are handled by the affine machinery above.
static void runSlotStreamPrefetch(BinaryContext &BC, BinaryFunction &BF,
                                  uint64_t Address, RegAnalysis &RA,
                                  unsigned &Inserted, unsigned &Rejected) {
  struct SlotInfo {
    const MCInst *Reload = nullptr;
    BinaryBasicBlock *ReloadBB = nullptr;
    int64_t Offset = 0;
    MCPhysReg Dest = 0;
    uint64_t ReloadMisses = 0;
    uint64_t DataMisses = 0;
    unsigned DataLoads = 0;
  };
  auto Report = [&](StringRef Result, StringRef Extra, const SlotInfo *S,
                    const BinaryLoop *Loop) {
    if (!opts::PrefetchDiagnostics)
      return;
    BC.outs() << "BOLT-PREFETCH: " << BF.getPrintName() << " pc=";
    if (S && S->Reload && BC.MIB->getOffset(*S->Reload))
      BC.outs() << formatv("{0:x}", Address + *BC.MIB->getOffset(*S->Reload));
    else
      BC.outs() << "unknown";
    BC.outs() << " source=profile slot-stream " << Result;
    if (!Extra.empty())
      BC.outs() << " " << Extra;
    if (S)
      BC.outs() << formatv(" slot-offset={0} reload-misses={1} "
                          "data-misses={2} data-loads={3}",
                          S->Offset, S->ReloadMisses, S->DataMisses,
                          S->DataLoads);
    if (Loop)
      BC.outs() << formatv(" loop={0:x}",
                           Address + Loop->getHeader()->getInputOffset());
    BC.outs() << '\n';
  };
  const MCPhysReg FP = BC.MIB->getFramePointer();
  if (!FP) {
    Report("rejected", "no frame pointer", nullptr, nullptr);
    return;
  }
  BitVector Available(BC.MRI->getNumRegs());
  BC.MIB->getGPRegs(Available, false);
  BC.MIB->removeNonScavengeableRegs(Available);
  BitVector CalleeSaved(Available.size());
  BC.MIB->getCalleeSavedRegs(CalleeSaved);
  Available.reset(CalleeSaved);
  if (!excludeCFIRegisters(BF, Available)) {
    Report("rejected", "opaque CFI expression", nullptr, nullptr);
    return;
  }
  const auto SlotAllocator = BC.MIB->initializeNewAnnotationAllocator();
  auto Live = std::make_unique<LivenessAnalysis>(RA, BF, SlotAllocator,
                                                 /*AssumeABI=*/true);
  Live->run();
  unsigned FunctionInsertions = 0;
  for (BinaryLoop *TopLoop : BF.getLoopInfo()) {
    for (BinaryLoop *Loop : TopLoop->getLoopsInPreorder()) {
      if (FunctionInsertions >= opts::PrefetchSlotMaxPerFunction)
        goto Done;
      if (Loop->getSubLoops().empty())
        continue; // Innermost loops are handled by the affine machinery.
      // Calls inside the loop are fine here: they cannot write the caller's
      // frame slots, and the prefetch is architecturally safe.  Only the
      // loop header itself must not be an exception landing pad.
      if (Loop->getHeader()->isLandingPad()) {
        Report("rejected", "landing pad header", nullptr, Loop);
        ++Rejected;
        continue;
      }
      // Collect stack-slot reloads grouped by frame offset.  Only slots the
      // pass can reload itself with a single instruction qualify.
      DenseMap<int64_t, SlotInfo> Slots;
      for (BinaryBasicBlock *BB : Loop->blocks()) {
        for (const MCInst &MI : *BB) {
          auto A = BC.MIB->analyzeLoopMemory(MI);
          if (!A || A->Base != FP || A->Index || A->HasWriteback ||
              A->Elements != 1 || A->Size != 8 || !A->Dest)
            continue;
          const bool Encodable =
              isInt<9>(A->Offset) ||
              (A->Offset >= 0 && (A->Offset & 7) == 0 &&
               isUInt<12>(static_cast<uint64_t>(A->Offset) / 8));
          if (!Encodable)
            continue;
          auto Profile = BC.MIB->tryGetAnnotationAs<CacheMissProfile>(
              MI, "CacheMissProfile");
          const uint64_t Misses = Profile ? Profile->MissCount : 0;
          auto [It, IsNew] = Slots.try_emplace(A->Offset);
          if (IsNew) {
            It->second.Reload = &MI;
            It->second.ReloadBB = BB;
            It->second.Offset = A->Offset;
            It->second.Dest = A->Dest;
            It->second.ReloadMisses = Misses;
          } else if (Misses > It->second.ReloadMisses) {
            It->second.Reload = &MI;
            It->second.ReloadBB = BB;
            It->second.Dest = A->Dest;
            It->second.ReloadMisses = Misses;
          }
        }
      }
      if (Slots.empty()) {
        Report("rejected", "no stack slot reloads", nullptr, Loop);
        continue;
      }
      // Match data loads streaming through each slot's anchor register.
      // A load counts only when the anchor reload is the unique reaching
      // definition of the anchor register at that load: an intervening
      // redefinition (e.g. "add x8, x3, x8, lsl #3") means the streamed
      // address comes from a different value than the slot content.
      // ReachingDefOrUse with TrackingReg=R gives exact per-register kills;
      // plain loads only partially kill defs that clobber wider register
      // sets (e.g. writeback loads), which would over-reject here.
      DenseMap<MCPhysReg, int64_t> DestToOffset;
      for (auto &[Off, S] : Slots)
        if (S.Dest)
          DestToOffset[S.Dest] = Off;
      if (!DestToOffset.empty()) {
        // Defs of each candidate register, using the same clobber-list
        // predicate the TrackingReg dataflow uses to gen expressions.
        DenseMap<MCPhysReg, SmallVector<const MCInst *, 8>> DefsByReg;
        for (BinaryBasicBlock &DefBB : BF) {
          for (MCInst &Def : DefBB) {
            if (BC.MIB->isPseudo(Def))
              continue;
            BitVector Clobbers(BC.MRI->getNumRegs());
            RA.getInstClobberList(Def, Clobbers);
            for (auto &[Reg, Off] : DestToOffset)
              if (Clobbers.test(Reg))
                DefsByReg[Reg].push_back(&Def);
          }
        }
        // RD instances share the annotation name, so run them strictly one
        // at a time: create, query, clean annotations, destroy.
        for (auto &[Reg, Off] : DestToOffset) {
          SlotInfo &S = Slots[Off];
          SmallVector<const MCInst *, 8> &Defs = DefsByReg[Reg];
          if (!S.Reload)
            continue;
          bool DefInDefList = false;
          for (const MCInst *Def : Defs)
            if (Def == S.Reload) {
              DefInDefList = true;
              break;
            }
          if (!DefInDefList)
            continue;
          auto RDReg = std::make_unique<ReachingDefOrUse</*Def=*/true>>(
              RA, BF, Reg, SlotAllocator);
          RDReg->run();
          for (BinaryBasicBlock *BB : Loop->blocks()) {
            for (const MCInst &MI : *BB) {
              auto A = BC.MIB->analyzeLoopMemory(MI);
              if (!A || A->Base != Reg || A->Base == FP || !A->Dest)
                continue;
              bool AnchorReaches = RDReg->count(MI, *S.Reload);
              unsigned NumReaching = 0;
              for (const MCInst *Def : Defs) {
                if (RDReg->count(MI, *Def)) {
                  ++NumReaching;
                  if (NumReaching > 1)
                    break;
                }
              }
              if (!AnchorReaches || NumReaching != 1)
                continue;
              ++S.DataLoads;
              auto Profile = BC.MIB->tryGetAnnotationAs<CacheMissProfile>(
                  MI, "CacheMissProfile");
              S.DataMisses += Profile ? Profile->MissCount : 0;
            }
          }
          RDReg->cleanAnnotations();
          RDReg.reset();
        }
      }
      SlotInfo Best;
      bool HasBest = false;
      for (auto &[Off, S] : Slots) {
        if (!S.DataLoads || !S.DataMisses)
          continue; // Anchor register does not stream missing data.
        if (!HasBest || S.ReloadMisses + S.DataMisses >
                            Best.ReloadMisses + Best.DataMisses) {
          Best = S;
          HasBest = true;
        }
      }
      if (!HasBest) {
        Report("rejected", "no slot data stream", nullptr, Loop);
        continue;
      }
      if (Best.ReloadMisses + Best.DataMisses <
          opts::PrefetchSlotMinMisses) {
        Report("rejected", "insufficient slot misses", nullptr, Loop);
        continue;
      }
      // Insert at the loop head, after any leading CFI pseudo instructions.
      BinaryBasicBlock *Header = Loop->getHeader();
      const MCInst *First = nullptr;
      unsigned InsertIdx = 0;
      for (unsigned I = 0; I < Header->size(); ++I) {
        const MCInst &MI = *(Header->begin() + I);
        if (BC.MIB->isCFI(MI))
          continue;
        First = &MI;
        InsertIdx = I;
        break;
      }
      if (!First) {
        Report("rejected", "empty loop header", nullptr, Loop);
        continue;
      }
      MCPhysReg Scratch = 0;
      {
        const BitVector &LiveAt = *Live->getStateAt(*First);
        for (int Reg : Available.set_bits())
          if (!LiveAt.test(Reg)) {
            Scratch = Reg;
            break;
          }
      }
      if (!Scratch) {
        Report("rejected", "no dead register at loop head", nullptr, Loop);
        ++Rejected;
        continue;
      }
      SmallVector<MCInst, 4> Code;
      MCInst SlotLoad;
      BC.MIB->createRestoreFromStack(SlotLoad, FP, Best.Offset, Scratch, 8);
      MCPlusBuilder::LoopMemoryAccess A;
      A.Base = Scratch;
      A.Offset = opts::PrefetchSlotDistance;
      A.Size = 8;
      A.Elements = 1;
      if (!BC.MIB->createLoopPrefetch(A, 0, 0, {}, opts::PrefetchHint,
                                      Code)) {
        Report("rejected", "unsupported prefetch encoding", nullptr, Loop);
        ++Rejected;
        continue;
      }
      BC.MIB->addAnnotation(Code.front(), "LoopDataPrefetch", 1u);
      SmallVector<MCInst, 8> All;
      All.push_back(std::move(SlotLoad));
      All.append(Code);
      for (MCInst &MI : llvm::reverse(All))
        Header->insertInstruction(Header->begin() + InsertIdx, MI);
      ++Inserted;
      ++FunctionInsertions;
      Report("inserted",
             formatv("slot-distance={0} scratch={1} placement=loop-head "
                     "kind=slot-stream",
                     opts::PrefetchSlotDistance, BC.MRI->getName(Scratch))
                 .str(),
             &Best, Loop);
    }
  }
Done:
  Live->cleanAnnotations();
  Live.reset();
  BC.MIB->freeValuesAllocator(SlotAllocator);
}

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
  if (opts::PrefetchCacheLineSize && !opts::PrefetchBatch)
    return createFatalBOLTError(
        "loop-prefetch-cache-line-size requires --loop-prefetch-batch");
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
    // Reaching definitions for value-preserving PRFM hoisting (PlanB).
    auto RD = std::make_unique<ReachingDefOrUse</*Def=*/true>>(
        RA, BF, std::nullopt, AnalysisAllocator);
    RD->run();
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
            // === Entry multi-line prefetch for variable-trip loops ===
            // Trace backwards from the load past the loop boundary and
            // through join points (per-path), then insert N PRFM lines
            // at each path's base initialization point.
            if (!Batch && shouldUseEntryStrategy(
                              Loop, *Access,
                              BaseStride ? *BaseStride : 0)) {
              // Dedup: one entry plan per base register per loop
              bool AlreadyCovered = false;
              for (const auto &S : Selected) {
                if (S.Access.Base == Access->Base &&
                    Plans[S.PlanIndex].EntryPrefetch) {
                  AlreadyCovered = true;
                  break;
                }
              }
              if (AlreadyCovered) {
                Diagnose(&MI, "duplicate base for entry prefetch", Policy);
                continue;
              }
              auto Points = findBaseInitialization(
                  BF, Loop, Access->Base, *BB, I, RD.get());
              if (Points.empty()) {
                Reject("entry prefetch: cannot trace base init");
                continue;
              }
              bool AllEncoded = true;
              SmallVector<PrefetchPlan, 4> NewPlans;
              for (const auto &P : Points) {
                // Adjust the prefetch address by the accumulated writeback
                // offset so the PRFM targets the pre-writeback base.
                auto PrefetchAddr = *Access;
                PrefetchAddr.Base = P.BaseReg;
                SmallVector<MCInst, 8> Code;
                bool Encoded = true;
                for (unsigned Line = 0; Line < opts::PrefetchEntryLines;
                     ++Line) {
                  int64_t Offset = int64_t(Line) * 64 - P.OffsetAdjust;
                  if (AddOverflow(PrefetchAddr.Offset, Offset, Offset) ||
                      !BC.MIB->createLoopPrefetch(PrefetchAddr, Offset, 0,
                                                  /*Scratch=*/{}, Policy.Hint,
                                                  Code)) {
                    Encoded = false;
                    break;
                  }
                }
                if (!Encoded) {
                  AllEncoded = false;
                  break;
                }
                PrefetchPlan Plan{P.BB, P.Index, std::move(Code), {}};
                Plan.Load = &MI;
                Plan.Requests = 1;
                Plan.Loop = Loop;
                Plan.Policy = Policy;
                Plan.Misses = Profile ? Profile->MissCount : 0;
                Plan.Instructions = Plan.Code.size();
                Plan.EntryPrefetch = true;
                NewPlans.push_back(std::move(Plan));
              }
              if (!AllEncoded) {
                Reject("entry prefetch encoding failed");
                continue;
              }
              // Each path's PRFMs only execute on that path, so check the
              // budget per-path, not as a sum across all paths.
              for (auto &P : NewPlans) {
                if (P.Instructions > Policy.Budget) {
                  AllEncoded = false;
                  break;
                }
              }
              if (!AllEncoded) {
                Reject("entry prefetch exceeds instruction budget");
                continue;
              }
              for (auto &P : NewPlans) {
                Selected.push_back({I, unsigned(Plans.size()), *Access,
                                    0, 0, Policy.Hint, *Policy.Distance});
                Plans.push_back(std::move(P));
              }
              continue;
            }
            int64_t BaseDelta = 0, IndexDelta = 0;
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
            }
            auto PrefetchAccess = Batch ? Batch->Address : *Access;
            if (AfterLoad)
              PrefetchAccess.Base = NextPointer;
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
            if (Indirect && Scratch.size() < 2) {
              Reject("indirect prefetch needs two dead registers");
              continue;
            }
            PrefetchPlan Plan{EmitBB, EmitIndex, {}, {}};
            Plan.Load = &MI;
            if (Indirect) {
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
                                                Policy.Hint, Plan.Code)) {
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
                           Policy.Hint, Plan.Code)) {
              Reject("unsupported encoding or insufficient dead registers");
              continue;
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
                  Plan->EntryPrefetch
                      ? "loop-entry"
                      : (Plan->BatchIterations
                             ? "outer-entry"
                             : (Plan->AfterLoad ? "after-load" : "before-load")),
                  Plan->EntryPrefetch
                      ? "entry-affine"
                      : (Plan->BatchIterations
                             ? "batch"
                             : (Plan->Indirect
                                    ? "indirect"
                                    : (Plan->AfterLoad ? "pointer" : "affine"))))
              .str();
      if (Plan->EntryPrefetch) {
        double AvgTrip =
            Plan->Loop && Plan->Loop->EntryCount
                ? (double)Plan->Loop->TotalBackEdgeCount /
                      Plan->Loop->EntryCount
                : 0.0;
        Detail += formatv(" entry-lines={0} avg-trip={1}",
                          opts::PrefetchEntryLines,
                          llvm::format("%.1f", AvgTrip))
                      .str();
      }
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
    // Unregister the annotations first: freeValuesAllocator only destroys the
    // value pool and would otherwise leave dangling registry entries on the
    // instructions for later analyses with the same annotation index.
    Live->cleanAnnotations();
    RD->cleanAnnotations();
    Live.reset();
    BC.MIB->freeValuesAllocator(AnalysisAllocator);
    bool ChangedCFG = false;
    for (auto &Plan : llvm::reverse(Plans)) {
      if (!Plan.Selected)
        continue;
      if (Plan.EntryPrefetch) {
        // Entry multi-line: insert at the recorded position outside the
        // loop.  No CFG changes needed.
        for (MCInst &MI : llvm::reverse(Plan.Code))
          Plan.BB->insertInstruction(Plan.BB->begin() + Plan.Index, MI);
      } else if (Plan.Continuation) {
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
    if (opts::PrefetchSlotStream)
      runSlotStreamPrefetch(BC, BF, Address, RA, Inserted, Rejected);
  }
  BC.outs() << "BOLT-INFO: number of prefetch instructions inserted: "
            << Inserted << " (rejected candidates: " << Rejected << ")\n";
  BC.outs() << "BOLT-INFO: prefetch plans: " << Planned
            << ", accepted loads: " << Accepted
            << ", added instructions: " << Added << "\n";
  return Error::success();
}
