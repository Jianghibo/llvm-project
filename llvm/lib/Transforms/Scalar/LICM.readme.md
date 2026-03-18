# Pass 分析报告：LICM（Loop Invariant Code Motion）

## 1. 基本定位

- **Pass 类型**：Transform Pass
- **粒度**：Loop（New PM: `LICMPass::run(Loop &, LoopAnalysisManager &, LoopStandardAnalysisResults &, LPMUpdater &)`）
- **所在文件**：`llvm/lib/Transforms/Scalar/LICM.cpp` / `llvm/include/llvm/Transforms/Scalar/LICM.h`
- **`run()` 入口**：`LICMPass::run()` (New PM) / `LegacyLICMPass::runOnLoop()` (Legacy PM)，均委托给 `LoopInvariantCodeMotion::runOnLoop()`
- **pipeline 位置**：`-O2/-O3` 中在循环简化（LoopSimplify）和 LCSSA 化之后、循环展开/向量化之前；典型出现多次（early LICM、late LICM）
- **一句话职责**：将循环内的不变量计算提升到 preheader 或下沉到 exit block，并将循环内的 must-alias 内存访问提升为寄存器标量，减少循环内冗余计算和内存访问。

还有一个 `LNICMPass`（Loop Nest LICM），用于 Loop Nest 粒度，在最外层循环上跑、透过内层循环提升不变量。

---

## 2. 输入、输出与前置条件

| 项目 | 说明 |
|---|---|
| **输入** | Loop IR（需要 preheader、LCSSA 形式、dedicated exits） |
| **关键分析依赖** | MemorySSA（**必须**，缺失会 fatal）、AliasAnalysis、DominatorTree、LoopInfo、ScalarEvolution（可选）、AssumptionCache、TLI、TTI、LazyBFI |
| **输出** | 修改后的 Loop IR：指令被 hoist/sink、内存访问被提升为标量 |
| **前置条件** | `L->isLCSSAForm(*DT)`（有断言检查）；需要 loop preheader；`!hasDisableLICMTransformsHint(L)` |
| **不处理的场景** | 含 `coro.suspend` 的 coroutine 循环（不做 sink）；volatile/有序 atomic load/store；can-throw 且指针可逃逸的 store promotion；内存访问数量超过 `licm-mssa-max-acc-promotion`（250） |

---

## 3. 关键函数调用栈

**主执行链**（三阶段串行）：

```text
LICMPass::run()
  -> LoopInvariantCodeMotion::runOnLoop()     // 核心入口，三阶段编排

  ── 阶段一：Sink ──
  -> sinkRegion()                             // 反向 DFS 遍历 DomTree，先访问 use 后访问 def
     -> canSinkOrHoistInst()                  // 合法性判断：内存语义、别名、side effect
        -> pointerInvalidatedByLoop()         // 基于 MemorySSA 判断 load 是否被 loop 中 store clobber
        -> noConflictingReadWrites()          // 判断 store/写-only call 是否安全下沉
        -> getClobberingMemoryAccess()        // MSSA walker，受 licm-mssa-optimization-cap 限制
     -> isNotUsedOrFoldableInLoop()           // 所有 user 都在循环外 → 可以 sink
     -> sink()                               // 实际下沉：clone 到 exit block，RAUW LCSSA phi
        -> sinkThroughTriviallyReplaceablePHI()
        -> cloneInstructionInExitBlock()
        -> splitPredecessorsOfLoopExit()      // 若 PHI 非 trivially replaceable，拆分前驱

  ── 阶段二：Hoist ──
  -> hoistRegion()                            // 正向 RPO 遍历，先访问 def 后访问 use
     -> ControlFlowHoister (CFH)              // 辅助类：处理控制流 hoist 和 PHI hoist
     -> CurLoop->hasLoopInvariantOperands()   // 所有操作数不变 → 候选
     -> canSinkOrHoistInst()                  // 同上，别名与合法性
     -> isSafeToExecuteUnconditionally()      // 是否可无条件执行（speculation 或 guaranteed）
        -> isSafeToSpeculativelyExecute()     // 可推测执行（无 side effect、无 UB 路径）
        -> SafetyInfo->isGuaranteedToExecute()// 必然执行（支配所有出口）
     -> hoistArithmetics()                    // 算术重关联提升：min/max、GEP、add/sub、FP/Int
        -> hoistMinMax()
        -> hoistGEP()
        -> hoistAdd() / hoistSub()
        -> hoistFPAssociation() / hoistBOAssociation()
     -> hoist()                              // 实际提升：moveBefore preheader，更新 MSSA/SE

  ── 阶段三：Memory Promotion ──
  -> collectPromotionCandidates()             // 用 AliasSetTracker 找 must-alias mod 集合
     -> foreachMemoryAccess()                 // 遍历 Loop 所有 MemoryAccess
     -> AliasSetTracker::add()               // 建立别名集
  -> promoteLoopAccessesToScalars()           // 对每个候选集做标量提升
     -> LoopPromoter (继承 LoadAndStorePromoter)
        -> insertStoresInLoopExitBlocks()     // 在 exit block 插入 store
     -> SSAUpdater                           // 维护提升后值的 SSA 形式
     -> formLCSSARecursively()               // 提升后重建 LCSSA
```

---

## 4. 整体执行流程

### Phase 0：初始化与安全检查

检查 LICM hints metadata、coroutine suspend、初始化 `ICFLoopSafetyInfo`（计算每个 BB 是否必然执行、是否可能抛异常）、初始化 `SinkAndHoistLICMFlags`（统计 loop 中 MemoryAccess 数量，超过 cap 则降级）。

### Phase 1：Sink

```cpp
// 反向 DFS：先处理 use，再处理 def → 单趟完成 sink，无需迭代
SmallVector<BasicBlock *, 16> Worklist = collectChildrenInLoop(DT, N, CurLoop);
for (BasicBlock *BB : reverse(Worklist)) {
    for (Instruction &I : reverse(*BB)) {
        if (isInstructionTriviallyDead(&I)) { eraseInstruction(I); continue; }
        if (!I.mayHaveSideEffects()
            && isNotUsedOrFoldableInLoop(...)
            && canSinkOrHoistInst(...))
            sink(I, ...);
    }
}
```

### Phase 2：Hoist

```cpp
// 正向 RPO：先处理 def，再处理 use → 单趟完成 hoist
LoopBlocksRPO Worklist(CurLoop);
for (BasicBlock *BB : Worklist) {
    for (Instruction &I : BB) {
        // 优先级依次尝试：
        // 1. 全操作数不变 + 可安全执行 → hoist 到 preheader
        // 2. FDiv + allowReciprocal → 转为倒数乘法再 hoist
        // 3. invariant.start / guard + 必然执行 → hoist
        // 4. PHI + 可 hoist 的控制流 → hoist（ControlFlowHoisting 开关控制）
        // 5. 算术重关联 → hoistArithmetics
        // 6. 记录可提升 branch → CFH.registerPossiblyHoistableBranch
    }
}
// 修正：已 hoist 的指令可能不支配全部 use → rehoist 到 idom
```

### Phase 3：Memory Promotion（标量化）

前提：有 preheader、有 dedicated exits、访问数量 ≤ cap、无 coroutine suspend。

用 `AliasSetTracker` 找 must-alias、包含 mod 的别名集 → 对每组指针集调用 `promoteLoopAccessesToScalars()`，在 preheader 插入 load、用 `SSAUpdater` 替换循环内所有 load/store、在 exit block 插入 store，最后 `formLCSSARecursively()` 修复 LCSSA。

---

## 5. 核心逻辑拆解

### 5.1 `canSinkOrHoistInst()` — 核心合法性判断（行 1165-1284）

这是 LICM 中最关键的决策函数，按指令类型分派：

```cpp
bool llvm::canSinkOrHoistInst(Instruction &I, AAResults *AA, DominatorTree *DT,
                               Loop *CurLoop, MemorySSAUpdater &MSSAU,
                               bool TargetExecutesOncePerLoop,
                               SinkAndHoistLICMFlags &Flags,
                               OptimizationRemarkEmitter *ORE) {
```

- **Load**：必须 unordered；常量内存直接返回 true；有 `invariant_load` metadata 直接 true；调用 `isLoadInvariantInLoop()`（invariant.start 支配路径）；最后用 `pointerInvalidatedByLoop()` + MSSA walker 判断是否被 loop 内 store clobber。
- **Call**：不可 throw，不可 convergent，不可 presplit coroutine；`doesNotAccessMemory` → true；`onlyReadsMemory` → 同 load 路径；`onlyWritesMemory` → `noConflictingReadWrites()`
- **Store**：必须 unordered；`isOnlyMemoryAccess()`（loop 中唯一内存访问）→ true；否则走 `noConflictingReadWrites()`
- **Fence**：只有它是 loop 中唯一内存访问时才允许

**为什么这样设计**：LICM 要求移动后程序语义不变。Load 的关键风险是被 loop 内写操作修改值（用 MSSA 精确追踪）；Store 的关键风险是引入新的副作用路径或与其他读写冲突。

### 5.2 `isSafeToExecuteUnconditionally()` — 安全性判断（行 1729-1753）

```cpp
// 两条路：speculation（可推测执行）或 guaranteed（必然执行）
if (AllowSpeculation && isSafeToSpeculativelyExecute(&Inst, CtxI, AC, DT, TLI))
    return true;
return SafetyInfo->isGuaranteedToExecute(Inst, DT, CurLoop);
```

- **Speculation 路径**：指令无 side effect、无法 trap（如整数加法、比较、无越界的 GEP），可安全提升到 preheader，即使原来是条件执行的。
- **Guaranteed 路径**：指令在每次循环执行时必然被执行（支配所有 back-edge 和 exits），可以提升即使可能 trap（如 load）。

`AllowSpeculation=false` 时（通过 `no-allowspeculation` 参数控制），只允许 guaranteed 路径，更保守，适用于 sanitizers 等场景。

### 5.3 `sinkRegion()` vs `hoistRegion()` — 遍历顺序的精妙设计（行 559-1060）

- **Sink 用反向 DFS**（`reverse(Worklist)`）：优先处理 DomTree 叶节点。因为循环内 use 在 def 之后，反向遍历保证先看到 use、再看到 def，在单趟中就能判断一条指令的所有 user 是否都在循环外，无需多轮。
- **Hoist 用正向 RPO**（`LoopBlocksRPO`）：优先处理支配者。因为要提升的指令依赖操作数的不变性，正向遍历保证先看到操作数的 def（可能已被提升），再看到使用者，可以级联提升。

### 5.4 `promoteLoopAccessesToScalars()` — 内存标量化（行 1911-2223）

**核心思路**：将循环内对同一内存位置的反复 load/store，替换为：
1. preheader 中一次 load（`.promoted`）
2. 循环内操作寄存器变量
3. exit block 中一次 store

**安全性分三层**：

| 安全性要求 | 验证方式 |
|---|---|
| p1. preheader 处可解引用（load 安全） | `isSafeToExecuteUnconditionally()` 或有 guaranteed store/load |
| p2. exit block 处 store 合法（不引入新写路径） | 三种方式之一：① store 支配所有 exit block ② 对象线程本地 ③ 对象 unwind 不可见 |
| 访问类型一致 | `AccessTy == getLoadStoreType(UI)`，否则不提升 |

**特殊处理**：
- `HasReadsOutsideSet`：集合外有读操作时，只能提升 load（只读提升），不能提升 store（store 会改变语义）
- unordered atomic 与 non-atomic 混合访问：直接放弃（atomic model 要求一致性）
- `SSAUpdater` 处理循环内 PHI 节点和多个 store 版本的 SSA 重建

### 5.5 `hoistArithmetics()` — 重关联提升（行 2435-2882）

几种特殊模式：

- **`hoistMinMax()`**：`(A < C1) && (A < C2)` → `A < min(C1, C2)`，将两个不变量合并为一个 min/max，后移出循环
- **`hoistGEP()`**：`gep(gep(ptr, idx1_variant), idx2_invariant)` → `gep(gep(ptr, idx2_invariant), idx1_variant)`，把不变的索引层移到外层，允许外层 GEP 被提升
- **`hoistAdd()/hoistSub()`**：`(LV + C1) < C2` → `LV < C2 - C1`，将不变量的加减移到比较的另一侧
- **`hoistBOAssociation()`**：对 FP（`AllowFPReassoc`）和 Int 的通用二元运算重关联，上限分别由 `licm-max-num-fp-reassociations`（5）和 `licm-max-num-int-reassociations`（5）控制

这些是标准 LICM 之外的"模式识别 hoist"，适用于操作数本身不是不变量、但经过重组后不变部分可以提取出循环的情况。

---

## 6. 关键数据结构与 LLVM API

| 名称 | 作用 | 关键字段/接口 | 位置 |
|---|---|---|---|
| `LoopInvariantCodeMotion` | 核心实现类，封装配置参数 | `runOnLoop()`, `LicmMssaOptCap`, `LicmAllowSpeculation` | `LICM.cpp:226` |
| `ICFLoopSafetyInfo` | 计算每个 BB 是否必然执行、是否可能抛异常 | `computeLoopSafetyInfo()`, `isGuaranteedToExecute()`, `anyBlockMayThrow()`, `doesNotWriteMemoryBefore()` | `llvm/Analysis/MustExecute.h` |
| `SinkAndHoistLICMFlags` | 编译时限流配置，防止 MSSA walker 调用次数过多 | `tooManyClobberingCalls()`, `tooManyMemoryAccesses()`, `NoOfMemAccTooLarge`, `LicmMssaOptCap` | `LICM.cpp:388` |
| `MemorySSAUpdater` | 在修改 IR 时同步更新 MemorySSA | `removeMemoryAccess()`, `createMemoryAccessInBB()`, `insertDef()`, `insertUse()`, `moveToPlace()` | `llvm/Analysis/MemorySSAUpdater.h` |
| `ControlFlowHoister` | 处理条件分支和 PHI 的提升，维护 BB → 目标 BB 映射 | `registerPossiblyHoistableBranch()`, `canHoistPHI()`, `getOrCreateHoistedBlock()` | `LICM.cpp:654` |
| `LoopPromoter` | 继承 `LoadAndStorePromoter`，实现内存提升的具体 IR 改写 | `insertStoresInLoopExitBlocks()`, `instructionDeleted()`, `shouldDelete()` | `LICM.cpp:1756` |
| `AliasSetTracker` | 快速分组 must-alias 集合 | `add()`, `isMustAlias()`, `isMod()` | `llvm/Analysis/AliasSetTracker.h` |
| `SSAUpdater` | 多 def 场景下的 SSA 重建 | `AddAvailableValue()`, `GetValueInMiddleOfBlock()` | `llvm/Transforms/Utils/SSAUpdater.h` |
| `LoopBlocksRPO` | Loop 内 BB 的反后序遍历 | `perform(LI)` | `llvm/Analysis/LoopIterator.h` |

---

## 7. 正确性约束与易错点

### 7.1 LCSSA 形式保证

LICM 始终要求循环处于 LCSSA 形式，开头和结尾都有 `assert(L->isLCSSAForm(*DT))`。Sink 操作通过 exit block 的 LCSSA PHI node（trivially replaceable PHI）实现：指令被克隆到 exit block，原 PHI 被 RAUW 替换。若 PHI 不是 trivially replaceable（多个前驱对应不同值），则先调用 `splitPredecessorsOfLoopExit()` 拆分前驱。

### 7.2 UB/Poison 语义下的 metadata 处理

```cpp
// hoist() 内，非 guaranteed-to-execute 的指令提升前需要 drop UB-implying 属性
if ((I.hasMetadataOtherThanDebugLoc() || isa<CallInst>(I)) &&
    !SafetyInfo->isGuaranteedToExecute(I, DT, CurLoop))
    I.dropUBImplyingAttrsAndMetadata();
```

将条件执行的指令提升到 preheader 后，该指令在原来不执行的路径上也会执行，此时 `nonnull`、`noundef`、`range` 等依赖条件成立的属性必须丢弃，否则会引入 UB。

### 7.3 min/max 重关联的 poison 防护

```cpp
// hoistMinMax() 中，对于 SelectInst 形式的 logical-and/or，
// 需要对 RHS2 插入 freeze，防止原先 short-circuit 不求值的 poison 扩散
if (isa<SelectInst>(I))
    RHS2 = Builder.CreateFreeze(RHS2, RHS2->getName() + ".fr");
```

`select(cond, a, poison)` 中 poison 只要不被选中就安全，但提升后 `min(a, poison)` 中 poison 会传播，必须用 `freeze` 冻结。

### 7.4 Coroutine 不做 sink

```cpp
bool HasCoroSuspendInst = llvm::any_of(L->getBlocks(), [](BasicBlock *BB) {
    return any_of(..., match_fn(m_Intrinsic<Intrinsic::coro_suspend>()));
});
```

coroutine suspend 的 switch 默认分支指向 coroutine 帧可能已被销毁的路径，将 store sink 到那里会导致 use-after-free。

### 7.5 MemorySSA Cap 机制（编译时保护）

`SinkAndHoistLICMFlags` 追踪 `getClobberingMemoryAccess()` 调用次数（默认上限 100）和 loop 内 MemoryAccess 总数（默认上限 250）。超过后：
- 超过 clobbering cap → 回退到 `getDefiningAccess()`，结果保守但仍正确（可能错过优化机会）
- 超过 promotion cap → 完全跳过 memory promotion

---

## 8. 分析依赖与 Pass 交互

**依赖的 Analyses**：

| Analysis | 获取方式 | 用途 |
|---|---|---|
| `MemorySSA` | `AR.MSSA`（必须） | 别名驱动的 hoist/sink 合法性，内存标量化 |
| `AliasAnalysis` | `AR.AA` | `canSinkOrHoistInst()`、`collectPromotionCandidates()` 中的别名查询 |
| `DominatorTree` | `AR.DT` | 支配关系、guaranteed-to-execute 判断 |
| `LoopInfo` | `AR.LI` | 子循环判断、loop 成员判断 |
| `ScalarEvolution` | `AR.SE`（可选） | hoist 后 `SE->forgetLoopDispositions()`，避免缓存失效 |
| `AssumptionCache` | `AR.AC` | `isSafeToSpeculativelyExecute()` 中推断安全条件 |
| `TLI` | `AR.TLI` | `isInstructionTriviallyDead()` 等 |
| `TTI` | `AR.TTI` | `isFoldableInLoop()` 中 GEP cost 查询 |
| `LazyBFI` | 懒加载 | 仅 Legacy PM 中 preserved（新 PM 中按需） |

**`PreservedAnalyses`（New PM）**：

```cpp
// Changed 时：
auto PA = getLoopPassPreservedAnalyses();  // 保留 LoopAnalysis、DominatorTree 等基础
PA.preserve<MemorySSAAnalysis>();          // MSSAU 同步更新，MemorySSA 得以保留
// NOT preserved: ScalarEvolution（forgetLoopDispositions 已通知，但不保证完整）

// LNICMPass 还额外保留：
PA.preserve<DominatorTreeAnalysis>();
PA.preserve<LoopAnalysis>();
```

**为哪些后续 Pass 创造机会**：
- 将循环不变量提升到 preheader → 暴露给 GVN/CSE 做跨循环公共子表达式消除
- 内存标量化后 → Mem2Reg / SSA-based 优化可处理提升后的寄存器变量
- sink 到 exit block → 代码仅在实际使用时执行，降低热路径代价

**依赖哪些前置 Pass**：
- `LoopSimplify`：保证 preheader 存在、dedicated exits
- `LCSSA`：保证 exit block 有 phi 节点，LICM 可以安全 sink
- 内层循环先处理（`LPM` 的 bottom-up 顺序）：内层不变量已提升到外层循环体，LICM 再处理外层时可继续提升

---

## 9. 收益模型 / 编译时权衡

| 变换类型 | 主要收益 | 编译时成本 |
|---|---|---|
| Hoist（计算型） | 减少循环内计算，O(N) → O(1)，降低动态指令数 | DomTree 遍历 O(\|BB\|)，每条指令合法性检查 |
| Sink | 将只有 exit 路径使用的计算推迟，降低 live range 压力 | 需克隆指令、拆分 BB（split predecessors） |
| Memory Promotion | 消除循环内 load/store，将内存访问 amortize 到循环外 | AliasSetTracker 构建，SSAUpdater 重建 |
| hoistArithmetics | 暴露更多不变量，使后续 hoist 机会增多 | PatternMatch 遍历，上限由 FP/Int cap 控制 |

**关键 heuristics**：
- `MaxNumUsesTraversed=8`：`isLoadInvariantInLoop()` 中限制 use 遍历量，防止高 fanout 指针导致编译时爆炸
- `SetLicmMssaOptCap=100`：MSSA walker 调用上限，pathological 情况下保守降级
- `SetLicmMssaNoAccForPromotionCap=250`：内存访问数量超过后跳过 promotion，防止大循环的 O(N²) 开销

---

## 10. 验证与调试方法

```bash
# 查看 hoist/sink 发生了什么
opt -passes="licm" -debug-only=licm -S input.ll 2>&1 | head -50

# 查看 optimization remarks（hoist/sink/promote 统计）
opt -passes="licm" -pass-remarks=licm -pass-remarks-missed=licm \
    -S input.ll -o output.ll 2>&1

# 验证 MemorySSA 一致性（需 -DLLVM_ENABLE_ASSERTIONS）
opt -passes="licm" -verify-memoryssa -S input.ll

# 前后 IR 对比
opt -passes="print<memoryssa>,licm,print<memoryssa>" -S input.ll 2>&1

# LICM 统计项
opt -passes="licm" -stats -S input.ll 2>&1 | grep licm
# 输出：NumHoisted, NumSunk, NumMovedLoads, NumLoadStorePromoted 等

# 关键测试位置
# llvm/test/Transforms/LICM/
```

**典型 IR 变化模式**：

```llvm
; === Hoist 前 ===
loop:
  %invariant = add i32 %x, %y       ; x, y 均不变
  %use = mul i32 %invariant, %i
  br loop

; === Hoist 后 ===
preheader:
  %invariant = add i32 %x, %y       ; 提升到 preheader
loop:
  %use = mul i32 %invariant, %i
  br loop

; === Memory Promotion 前 ===
loop:
  %v = load i32, ptr %p             ; 每次迭代 load
  %nv = add i32 %v, 1
  store i32 %nv, ptr %p             ; 每次迭代 store

; === Memory Promotion 后 ===
preheader:
  %p.promoted = load i32, ptr %p    ; 循环前一次 load
loop:
  %v = phi i32 [%p.promoted, %pre], [%nv, %loop]
  %nv = add i32 %v, 1
exit:
  store i32 %nv.lcssa, ptr %p       ; 循环后一次 store
```

---

## 11. 总结

**最核心的三个设计点**：

1. **MemorySSA 驱动的精确别名判断**：LICM 全面依赖 MemorySSA 而非传统 AliasSetTracker（AliasSetTracker 仅用于 promotion 候选收集）。`getClobberingMemoryAccess()` 通过 MSSA walker 精确追踪"谁在 loop 内 clobber 了这个 load"，大幅减少假阳性（保守拒绝），同时用 cap 机制控制 pathological 情况下的编译时开销。

2. **三阶段编排保证完整性**：Sink（反向 DFS）→ Hoist（正向 RPO）→ Promote（fixpoint 迭代）。Sink 先行可清理死代码减小 Hoist 工作量；Promote 之所以是 fixpoint 循环（`do { } while (LocalPromoted)`），是因为提升一组访问后，其指针可能变为 loop invariant，解锁新一轮提升机会。

3. **`ICFLoopSafetyInfo` + `AllowSpeculation` 的双轨安全判断**：Speculation 路径适用于无副作用的纯计算（可安全在任意路径执行）；Guaranteed 路径适用于有副作用的访存（必须证明必然执行）。两者分工清晰，`no-allowspeculation` 模式下完全禁用推测，适配 sanitizer/调试场景。

**最值得继续深挖的 1-2 个问题**：

1. **ControlFlowHoisting（PHI/分支提升）**：通过 `ControlFlowHoister` 类实现，默认关闭（`-licm-control-flow-hoisting=false`）。其核心思路是识别条件不变的分支，在 preheader 外复制该分支的控制流结构，从而允许条件执行的不变量（和 PHI）被提升。这是一个较复杂且风险较高的扩展，值得深入研究其 convergence 判断和 rehoist 修正逻辑（行 1023-1045）。

2. **`hoistBOAssociation()`（整数/FP 重关联）**：通过重写操作数的结合方式将部分不变子表达式分离，适用于类似 `(a + LV) * C`（其中 `a, C` 不变）这类模式。其正确性依赖 `nsw/nuw/reassoc` flags 的精确判断，是 LICM 中正确性最微妙的部分之一（行 2700-2882）。

---

## 函数分析：`LoopInvariantCodeMotion::runOnLoop`（行 415–552）

### 函数签名与目的

```cpp
bool LoopInvariantCodeMotion::runOnLoop(Loop *L, AAResults *AA, LoopInfo *LI,
                                        DominatorTree *DT, AssumptionCache *AC,
                                        TargetLibraryInfo *TLI,
                                        TargetTransformInfo *TTI,
                                        ScalarEvolution *SE, MemorySSA *MSSA,
                                        OptimizationRemarkEmitter *ORE,
                                        bool LoopNestMode)
```

**功能**：LICM Pass 的单循环处理入口，统筹协调三大子变换——**Sink（下沉）**、**Hoist（提升）**、**Promote（提升到标量寄存器）**，将循环不变量从循环体中移出以减少循环内的计算/访存开销。

---

### 整体结构

```
runOnLoop(L, ...)
├── 前置检查与早退
│   ├── LCSSA 形式断言
│   ├── hasDisableLICMTransformsHint → 元数据禁用检查
│   └── HasCoroSuspendInst → 协程 suspend 检测
├── 初始化
│   ├── MemorySSAUpdater MSSAU
│   ├── SinkAndHoistLICMFlags Flags
│   ├── Preheader 获取
│   └── ICFLoopSafetyInfo 计算
├── Phase 1：Sink（下沉循环内 store）
│   └── sinkRegion / sinkRegionForLoopNest
├── Phase 2：Hoist（提升循环不变计算）
│   └── hoistRegion
├── Phase 3：Promote（内存访问提升为标量）
│   ├── collectPromotionCandidates → 收集候选访存集合
│   ├── promoteLoopAccessesToScalars（迭代执行）
│   └── formLCSSARecursively（LCSSA 修复）
└── 后置检查与清理
    ├── LCSSA 形式断言（二次验证）
    ├── MemorySSA 验证（可选）
    └── SE->forgetLoopDispositions（SE 失效）
```

---

### 逐段注释

**1. 前置检查与早退（行 424–445）**

```cpp
assert(L->isLCSSAForm(*DT) && "Loop is not in LCSSA form.");
if (hasDisableLICMTransformsHint(L)) { return false; }
bool HasCoroSuspendInst = llvm::any_of(L->getBlocks(), [](BasicBlock *BB) {
    return any_of(make_pointer_range(*BB),
                  match_fn(m_Intrinsic<Intrinsic::coro_suspend>()));
});
```

- **LCSSA 断言**：LICM 必须在 LCSSA 形式下运行，因为被提升的定义需要通过 phi 节点才能被循环外使用者访问。
- **元数据禁用**：循环可带 `llvm.loop.licm.disable` 元数据，允许用户精细控制。
- **协程 suspend 检测**：协程的 switch-resume 默认目标 BB 可能在 frame 析构后执行，任何 sink 到该 BB 的 store 都会产生 use-after-free。`HasCoroSuspendInst` 后续会阻断 Promote 阶段（但不阻断 Hoist）。注释中的 FIXME 指出这是"宁可错杀"的保守策略。

**2. 初始化（行 447–456）**

```cpp
MemorySSAUpdater MSSAU(MSSA);
SinkAndHoistLICMFlags Flags(LicmMssaOptCap, LicmMssaNoAccForPromotionCap,
                            /*IsSink=*/true, *L, *MSSA);
BasicBlock *Preheader = L->getLoopPreheader();
ICFLoopSafetyInfo SafetyInfo;
SafetyInfo.computeLoopSafetyInfo(L);
```

- **`MemorySSAUpdater`**：LICM 在移动指令时需要同步更新 MemorySSA，`MSSAU` 封装增量修改，避免每次移动后完整重建。
- **`SinkAndHoistLICMFlags`**：持有两个关键阈值：
  - `LicmMssaOptCap`：限制 MemorySSA 优化操作数量（编译时开销 budget）。
  - `LicmMssaNoAccForPromotionCap`：Promote 候选数量上限（`tooManyMemoryAccesses()` 的判断依据）。
  - 初始 `IsSink=true`，Phase 2 前调用 `setIsSink(false)` 切换语义。
- **`ICFLoopSafetyInfo`**：比基础 `LoopSafetyInfo` 更精细，能感知隐式控制流（如可能 throw 的 call），用于判断指令在循环某路径上是否"保证执行"，是 Hoist 合法性的关键。

**3. Phase 1：Sink 下沉（行 467–474）**

```cpp
if (L->hasDedicatedExits())
    Changed |= LoopNestMode
        ? sinkRegionForLoopNest(DT->getNode(L->getHeader()), ...)
        : sinkRegion(DT->getNode(L->getHeader()), ...);
Flags.setIsSink(false);
```

- **为什么先 Sink**：Sink 把"只在退出路径才有效"的 store 下沉到出口 BB，减少循环内的写操作；之后 Hoist 阶段面对更干净的 IR。
- **`hasDedicatedExits()`**：出口 BB 必须只有循环内的前驱，才能安全插入指令。
- **`LoopNestMode`**：Loop Nest 模式下调用专门版本，一次性处理整个嵌套循环的公共外壳，避免对子循环重复扫描外层不变量。
- **`Flags.setIsSink(false)`**：切换 Flags 状态，后续 Hoist 阶段使用不同的 MemorySSA 查询策略。

**4. Phase 2：Hoist 提升（行 475–478）**

```cpp
if (Preheader)
    Changed |= hoistRegion(DT->getNode(L->getHeader()), AA, LI, DT, AC, TLI, L,
                           MSSAU, SE, &SafetyInfo, Flags, ORE, LoopNestMode,
                           LicmAllowSpeculation);
```

- **必须有 Preheader**：被提升的指令要插入 Preheader（循环的唯一前驱），没有则无法保证支配关系。
- **`LicmAllowSpeculation`**：允许 Hoist 推测执行——即使指令不在所有路径上必然执行。这是 tradeoff：激进时可能引入新的异常，保守时错过部分提升机会。
- Hoist 依赖 DFS 支配树序（保证先见到 def 再见到 use），可在一遍扫描内完成不动点。

**5. Phase 3：Promote 内存提升（行 487–536）**

```cpp
if (!DisablePromotion && Preheader && L->hasDedicatedExits() &&
    !Flags.tooManyMemoryAccesses() && !HasCoroSuspendInst) {
    // ... 收集出口 BB 插入点 ...
    bool Promoted = false, LocalPromoted;
    do {
        LocalPromoted = false;
        for (auto [PointerMustAliases, HasReadsOutsideSet] :
                 collectPromotionCandidates(MSSA, AA, L)) {
            LocalPromoted |= promoteLoopAccessesToScalars(...);
        }
        Promoted |= LocalPromoted;
    } while (LocalPromoted);

    if (Promoted)
        formLCSSARecursively(*L, *DT, LI, SE);
}
```

- **多重 guard 条件**：
  - `!DisablePromotion`：命令行 flag 禁用。
  - `!Flags.tooManyMemoryAccesses()`：防止 MemorySSA 操作数量爆炸。
  - `!HasCoroSuspendInst`：协程保护。
  - `!HasCatchSwitch`（内层判断）：`catchswitch` 终结符的 BB 不允许插入普通指令（EH 语义约束）。
- **Promote 的核心思路**：把循环内对同一内存地址的反复 load/store 替换为对 SSA value 的操作，只在循环入口 load 一次、在出口 BB 插入点 store 回去，将内存访问频率从 O(N) 降为 O(1)。
- **迭代执行原因**：Promote 一个地址后可能使另一个地址的指针变为循环不变（依赖关系消失），需要迭代至不动点（行 511 注释明确说明）。
- **`formLCSSARecursively`**：Promote 引入跨越循环边界的 SSA def-use，必须在之后重建 LCSSA。行 529 的 FIXME 指出这很"heavy handed"，理想方案是在 Promote 过程中用 LCSSA-aware 的 SSAUpdater 增量维护。

**6. 后置检查与清理（行 539–551）**

```cpp
assert(L->isLCSSAForm(*DT) && ...);
assert((L->isOutermost() || L->getParentLoop()->isLCSSAForm(*DT)) && ...);
if (VerifyMemorySSA) MSSA->verifyMemorySSA();
if (Changed && SE) SE->forgetLoopDispositions();
```

- 两个 LCSSA 断言（当前循环 + 父循环）体现了 LICM 对循环边界操作的高度敏感性。
- `SE->forgetLoopDispositions()`：通知 SCEV 丢弃与该循环相关的所有 disposition 缓存（AddRecExpr 的值域信息、步长信息等），指令移动后归纳变量结构可能已改变。

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `SinkAndHoistLICMFlags` | 持有 MemorySSA 操作 budget 阈值及 Sink/Hoist 模式切换状态 |
| `ICFLoopSafetyInfo` | 隐式控制流感知的循环安全信息，判断指令在哪些路径必然执行 |
| `MemorySSAUpdater` | 封装对 MemorySSA 图的增量更新，避免移动指令后重建全图 |
| `PredIteratorCache` | 缓存 BasicBlock 前驱遍历结果，Promote 阶段频繁查询时提升效率 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 循环出口检测 | `L->hasDedicatedExits()` | `llvm/Analysis/LoopInfo.h` |
| 协程 intrinsic 匹配 | `m_Intrinsic<Intrinsic::coro_suspend>()` | `llvm/IR/PatternMatch.h` |
| 安全信息计算 | `ICFLoopSafetyInfo::computeLoopSafetyInfo()` | `llvm/Analysis/MustExecute.h` |
| MemorySSA 增量更新 | `MemorySSAUpdater` | `llvm/Analysis/MemorySSAUpdater.h` |
| Promote 候选收集 | `collectPromotionCandidates()` | `LICM.cpp` 内部 |
| SCEV 失效通知 | `SE->forgetLoopDispositions()` | `llvm/Analysis/ScalarEvolution.h` |
| LCSSA 修复 | `formLCSSARecursively()` | `llvm/Transforms/Utils/LCSSA.h` |

---

### 优化意图

1. **三阶段串行而非合并**：Sink 先于 Hoist，是因为 Sink 可消除循环内的死 store，使 Hoist 面对更干净的 IR；Promote 放在最后，依赖 Hoist 已将地址计算提升到 Preheader（使"循环不变指针"条件成立）。
2. **MemorySSA budget 机制**：`LicmMssaOptCap` 防止在超大循环体中 MemorySSA clobber 查询复杂度爆炸，是编译时开销和优化质量之间的核心 tradeoff。
3. **协程保守处理**：有 `coro_suspend` 时完全跳过 Promote，而非精细分析哪些 store 安全——是在正确性和实现复杂度之间的工程取舍（FIXME 标注了这一欠债）。
4. **`LicmAllowSpeculation`**：允许在非必然执行路径上提升无副作用指令，以换取更多 Hoist 机会；`no-allowspeculation` 模式适配 sanitizer/调试场景。
