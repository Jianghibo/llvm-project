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

**为什么这样设计**：LICM 要求移动后程序语义不变。Load 的关键风险是被 loop 内写操作修改值（用 MSSA. 精确追踪）；Store 的关键风险是引入新的副作用路径或与其他读写冲突。

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

- **Sink 用反向 DFS**（`reverse(®orklist)`）：：优先处理 DomTree 叶节点。因为循环内 use 在 def 之后，反向遍历保证先看到 use、再看到 def，在单趟中就能判断一条指令的所有 user 是否都在循环外，无需多轮。
- **Hoist 用正向 RPO**（`LoopBlocksRPO`）：：优先处理支配者。因为要提升的指令依赖操作数的不变性，正向遍历保证先看到操作数的 def（可能已被提升），再看到使用者，可以级联提升。

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
| `PredIteratorCache` | 缓存 exit block 的前驱列表，避免 promotion 过程中重复扫描 CFG 前驱关系 | `size()`, `get()`, `clear()` | `llvm/include/llvm/IR/PredIteratorCache.h` |
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
  %nv = add i i32 %v, 1
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

---

## 函数分析：`sinkRegionForLoopNest`（行 626-644）

### 函数签名与目的（行 626-632）

```cpp
bool llvm::sinkRegionForLoopNest(DomTreeNode *N, AAResults *AA, LoopInfo *LI,
                                 DominatorTree *DT, TargetLibraryInfo *TLI,
                                 TargetTransformInfo *TTI, Loop *CurLoop,
                                 MemorySSAUpdater &MSSAU,
                                 ICFLoopSafetyInfo *SafetyInfo,
                                 SinkAndHoistLICMFlags &Flags,
                                 OptimizationRemarkEmitter *ORE)
```

**功能**：对循环嵌套中的所有循环执行代码下沉，将循环内只使用一次的指令下沉到循环退出块。

---

### 整体结构

```
sinkRegionForLoopNest(N, AA, LI, DT, TLI, TTI, CurLoop, MSSAU, SafetyInfo, Flags, ORE)
├── 初始化工作列表
│   ├── 创建 SmallPriorityWorklist
│   ├── 插入当前循环 CurLoop
│   └── 添加所有嵌套循环到工作列表
└── 遍历处理循环
    ├── 从工作列表取出循环 L
    ├── 调用 sinkRegion() 对 L 执行代码下沉
    └── 累计 Changed 状态
```

---

### 逐段注释

**1. 初始化工作列表 (行 634-637)**

```cpp
bool Changed = false;
SmallPriorityWorklist<Loop *, 4> Worklist;
Worklist.insert(CurLoop);
appendLoopsToWorklist(*CurLoop, Worklist);
```

目的作用：创建优先级工作列表并收集所有需要处理的循环。
注释说明：
- `SmallPriorityWorklist<Loop *, 4>` 是一个优先级工作列表，初始容量为 4
- `Worklist.insert(CurLoop)` 将当前循环（最外层循环）插入工作列表
- `appendLoopsToWorklist(*CurLoop, Worklist)` 将当前循环的所有嵌套循环添加到工作列表

**2. 遍历处理循环 (行 638-642)**

```cpp
while (!Worklist.empty()) {
  Loop *L = Worklist.pop_back_val();
  Changed |= sinkRegion(DT->getNode(L->getHeader()), AA, LI, DT, TTI, TTI, L,
                        MSSAU, SafetyInfo, Flags, ORE, CurLoop);
}
return Changed;
```

目的作用：对工作列表中的每个循环执行代码下沉。
注释说明：
- `while (!Worklist.empty())` 循环直到工作列表为空
- `Worklist.pop_back_val()` 从工作列表取出一个循环
- `sinkRegion()` 对该循环执行代码下沉，传入 `CurLoop` 作为最外层循环参数（用于 LoopNestMode）
- `Changed |=` 累计是否发生了代码下沉变换

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SmallPriorityWorklist<Loop *, 4>` | `insert()`, `pop_back_val()`, `empty()` | 优先级工作列表，用于按特定顺序处理循环 |
| `sinkRegion()` | - | 对单个循环执行代码下沉的核心函数 |

---

### 优化意图

1. **处理循环嵌套**：在循环嵌套模式下，需要对所有嵌套循环执行代码下沉，而不仅仅是当前循环
2. **优先级处理**：使用 `SmallPriorityWorklist` 确保循环按正确的优先级顺序处理
3. **统一接口**：通过调用 `sinkRegion()` 复用单循环下沉的逻辑，避免代码重复

对于重要部分，要解释其为什么这么优化：
- 使用优先级工作列表可以确保内层循环先于外层循环处理，这对于正确的代码下沉很重要
- 传入 `CurLoop` 作为最外层循环参数，使得 `sinkRegion()` 可以在 LoopNestMode 下正确判断指令的使用范围

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 创建优先级工作列表 | `SmallPriorityWorklist<Loop *, 4>` | `llvm/ADT/PriorityWorklist.h` |
| 添加嵌套循环到工作列表 | `appendLoopsToWorklist()` | `llvm/Analysis/LoopIterator.h` |
| 执行代码下沉 | `sinkRegion()` | `llvm/lib/Transforms/Scalar/LICM.cpp:559` |

**使用示例**：参考 `LoopInvariantCodeMotion::runOnLoop()` 中的调用方式（行 470-471）。

---

### 其他补充

**与 sinkRegion() 的关系**：
- `sinkRegionForLoopNest()` 是循环嵌套模式下的下沉入口
- `sinkRegion()` 是单循环下沉的核心实现
- 两者通过 `LoopNestMode` 参数区分，后者在 LoopNestMode 下会将 `CurLoop` 作为最外层循环传递

**调用时机**：
- 在 `LoopInvariantCodeMotion::runOnLoop()` 中，当 ``LoopNestMode` 为 true 时调用（行 470-471）
- 在 `LNICMPass::run()` 中启用循环嵌套模式（行 346-351）

---

## 函数分析：`sinkRegion`（行 559-624）

### 函数签名与目的（行 559-564）

```cpp
bool llvm::sinkRegion(DomTreeNode *N, AAResults *AA, LoopInfo *LI,
                      DominatorTree *DT, TargetLibraryInfo *TLI,
                      TargetTransformInfo *TTI, Loop *CurLoop,
                      MemorySSAUpdater &MSSAU, ICFLoopSafetyInfo *SafetyInfo,
                      SinkAndHoistLICMFlags &Flags,
                      OptimizationRemarkEmitter *ORE, Loop *OutermostLoop)
```

**功能**：将循环内只使用一次的指令下沉到循环退出块，减少循环内的冗余计算。

---

### 整体结构

```
sinkRegion(N, AA, LI, DT, TLI, TTI, CurLoop, MSSAU, SafetyInfo, Flags, ORE, OutermostLoop)
├── 参数验证
├── 收集循环内基本块
│   └── collectChildrenInLoop(DT, N, CurLoop)
├── 反向遍历基本块
│   ├── 跳过子循环
│   └── 反向遍历指令
│       ├── 删除死指令
│       │   ├── isInstructionTriviallyDead()
│       │   ├── salvageKnowledge()
│       │   ├── salvageDebugInfo()
│       │   └── eraseInstruction()
│       └── 尝试下沉指令
│           ├── !I.mayHaveSideEffects()
│           ├── isNotUsedOrFoldableInLoop()
│           ├── canSinkOrHoistInst()
│           └── sink()
└── MemorySSA 验证（可选）
```

---

### 逐段注释

**1. 参数验证（行 567-569）**

```cpp
assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
       CurLoop != nullptr && SafetyInfo != nullptr &&
       "Unexpected input to sinkRegion.");
```

目的作用：验证所有必要参数非空，确保函数调用正确。
注释说明：断言检查，防止空指针导致的崩溃。

**2. 收集循环内基本块（行 574-575）**

```cpp
SmallVector<BasicBlock *, 16> Worklist =
    collectChildrenInLoop(DT, N, CurLoop);
```

目的作用：收集循环内所有基本块，按支配树顺序排列。
注释说明：`collectChildrenInLoop` 按支配树顺序收集基本块，为后续反向遍历做准备。

**3. 反向遍历基本块（行 577-579）**

```cpp
bool Changed = false;
for (BasicBlock *BB : reverse(Worklist)) {
    if (inSubLoop(BB, CurLoop, LI))
      continue;
```

目的作用：按反向顺序遍历基本块，跳过子循环。
注释说明：
- `reverse(Worklist)` 反向遍历，保证先访问 use 后访问 def
- `inSubLoop()` 跳过子循环，因为子循环已单独处理

**4. 反向遍历指令（行 583-584）**

```cpp
for (BasicBlock::iterator II = BB->end(); II != BB->begin();) {
    Instruction &I = *--II;
```

目的作用：按反向顺序遍历指令，保证先访问 use 后访问 def。
注释说明：
- 从 BB 结尾开始向前遍历
- `*--II` 先递减迭代器再解引用

**5. 删除死指令（行 588-596）**

```cpp
if (isInstructionTriviallyDead(&I, TLI)) {
    LLVM_DEBUG(dbgs() << "LICM deleting dead inst: " << I << '\n');
    salvageKnowledge(&I);
    salvageDebugInfo(I);
    ++II;
    eraseInstruction(I, *SafetyInfo, MSSAU);
    Changed = true;
    continue;
}
```

目的作用：删除循环内的死指令，清理无用代码。
注释说明：
- `isInstructionTriviallyDead()` 判断指令是否死代码
- `salvageKnowledge()` 保存指令中的知识（如条件信息）
- `salvageDebugInfo()` 保存调试信息
- `eraseInstruction()` 删除指令并更新 MemorySSA
- `++II` 因为删除指令后迭代器需要前进

**6. 尝试下沉指令（行 603-618）**

```cpp
bool FoldableInLoop = false;
bool LoopNestMode = OutermostLoop != nullptr;
if (!I.mayHaveSideEffects() &&
    isNotUsedOrFoldableInLoop(I, LoopNestMode ? OutermostLoop : CurLoop,
                                  SafetyInfo, TTI, FoldableInLoop,
                                  LoopNestMode) &&
    canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, true, Flags, ORE)) {
    if (sink(I, LI, DT, CurLoop, SafetyInfo, MSSAU, ORE)) {
        if (!FoldableInLoop) {
            ++II;
            salvageDebugInfo(I);
            eraseInstruction(I, *SafetyInfo, MSSAU);
        }
        Changed = true;
    }
}
```

目的作用：检查指令是否可以下沉，如果可以则执行下沉。
注释说明：
- `!I.mayHaveSideEffects()` 指令无副作用
- `isNotUsedOrFoldableInLoop()` 指令在循环内不使用或可折叠
- `canSinkOrHoistInst()` 检查下沉的合法性（别名、内存语义等）
- `sink()` 实际执行下沉操作
- 如果下沉成功且指令不可折叠，则删除原指令
- `FoldableInLoop` 标记指令是否可在循环内折叠

**7. MemorySSA 验证（行 621-622）**

```cpp
if (VerifyMemorySSA)
    MSSAU.getMemorySSA()->verifyMemorySSA();
```

目的作用：验证 MemorySSA 的一致性（仅在调试模式下启用）。
注释说明：确保下沉操作后 MemorySSA 仍然有效。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SmallVector<BasicBlock *, 16>` | `reverse()` | 存储循环内基本块，用于反向遍历 |
| `FoldableInLoop` | `bool` | 标记指令是否可在循环内折叠 |
| `LoopNestMode` | `bool` | 标记是否为循环嵌套模式 |

---

### 优化意图

1. **反向遍历**：先访问 use 后访问 def，可以在单趟中判断指令的所有 user 是否都在循环外，无需多轮迭代
2. **死代码删除**：在下沉前先删除死指令，减少后续处理的工作量
3. **FoldableInLoop**：如果指令可在循环内折叠，则不删除原指令，让后续优化处理
4. **LoopNestMode**：在循环嵌套模式下，使用 `OutermostLoop` 作为判断指令使用范围的依据

对于重要部分，要解释其为什么这么优化：
- 反向遍历保证单趟完成下沉，无需迭代，提高效率
- 死代码删除减少后续处理的工作量，避免处理无用指令
- FoldableInLoop 标记避免删除可在循环内折叠的指令，保留优化机会

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 收集循环内基本块 | `collectChildrenInLoop()` | `llvm/Analysis/LoopIterator.h` |
| 判断是否为子循环 | `inSubLoop()` | `LICM.cpp` 内部 |
| 判断指令是否死代码 | `isInstructionTriviallyDead()` | `llvm/Analysis/Loads.h` |
| 保存指令知识 | `salvageKnowledge()` | `llvm/Transforms/Utils/Local.h` |
| 保存调试信息 | `salvageDebugInfo()` | `llvm/IR/Instructions.h` |
| 删除指令 | `eraseInstruction()` | `LICM.cpp` 内部 |
| 判断指令无副作用 | `mayHaveSideEffects()` | `llvm/IR/Instruction.h` |
| 判断指令不使用或可内折叠 | `isNotUsedOrFoldableInLoop()` | `LICM.cpp` 内部 |
| 判断下沉合法性 | `canSinkOrHoistInst()` | `LICM.cpp` 内部 |
| 执行下沉 | `sink()` | `LICM.cpp` 内部 |
| 验证 MemorySSA | `verifyMemorySSA()` | `llvm/Analysis/MemorySSA.h` |

**使用示例**：参考 `LoopInvariantCodeMotion::runOnLoop()` 中的调用方式（行 472-473）。

---

### 其他补充

**与 hoistRegion() 的关系**：
- `sinkRegion()` 是下沉阶段的核心函数
- `hoistRegion()` 是提升阶段的核心函数
- 两者遍历顺序相反：sinkRegion 用反向遍历，hoistRegion 用正向 RPO 遍历

**调用时机**：
- 在 `LoopInvariantCodeMotion::runOnLoop()` 中，Phase 1 调用（行 472-473）
- 在 `sinkRegionForLoopNest()` 中对每个循环调用（行 640-641）

**正确性保证**：
- 通过 `isNotUsedOrFoldableInLoop()` 确保指令只在循环外使用
- 通过 `canSinkOrHoistInst()` 确保下沉不会改变程序语义
- 通过 MemorySSA 验证确保内存访问的一致性

---

## 函数分析：`isInstructionTriviallyDead`（行 406-411）

### 函数签名与目的

```cpp
bool llvm::isInstructionTriviallyDead(Instruction *I,
                                      const TargetLibraryInfo *TLI) {
  if (!I->use_empty())
    return false;
  return wouldInstructionBeTriviallyDead(I, TLI);
}
```

**功能**: 判断指令是否可以安全删除。返回 true 当且仅当指令的结果未被使用且指令没有副作用。

---

### 整体结构

```
isInstructionTriviallyDead(I, TLI)
├── 检查指令是否有使用者
│   └── 如果有使用者 → 返回 false
└── 委托给 wouldInstructionBeTriviallyDead 检查副作用
    └── 返回结果
```

---

### 逐段注释

**1. 快速路径：检查使用者 (行 408-409)**

```cpp
if (!I->use_empty())
  return false;
```

目的作用：如果指令的结果被其他指令使用，则不能删除。这是最快速的第一道检查，避免不必要的副作用分析。

**2. 委托副作用检查 (行 410)**

```cpp
return wouldInstructionBeTriviallyDead(I, TLI);
```

目的作用：委托给 `wouldInstructionBeTriviallyDead` 函数检查指令是否有副作用。这个函数包含详细的副作用判断逻辑。

---

### wouldInstructionBeTriviallyDead 函数分析 (行 425-532)

#### 整体结构

```
wouldInstructionBeTriviallyDead(I, TLI)
├── 检查终止符指令 → 不能删除
├── 检查异常处理指令 → 不能删除
├── 检查调试标签指令 → 特殊处理
├── 检查可移除的内存分配 → 可以删除
├── 检查不返回值的指令
│   └── 处理特殊 intrinsic (guard, wasm_trunc, ptrauth)
├── 检查无副作用 → 可以删除
├── 检查特殊 intrinsic
│   ├── stacksave / launder_invariant_group → 可以删除
│   ├── allow_runtime_check / allow_ubsan_check → 可以删除
│   ├── lifetime start/end → 检查 alloca 使用情况
│   ├── assume → 检查条件是否为真
│   └── constrained FP intrinsic → 检查异常行为
├── 检查调用指令
│   ├── free(nullptr/undef) → 可以删除
│   └── 数学库 noop → 可以删除
└── 检查常量全局变量的非易失性 load → 可以删除
```

#### 逐段注释

**1. 终止符指令检查 (行 427-428)**

```cpp
if (I->isTerminator())
  return false;
```

目的作用：终止符指令（br, ret, switch, unreachable 等）控制控制流，不能删除。

**2. 异常处理指令检查 (行 432-433)**

```cpp
if (I->isEHPad())
  return false;
```

目的作用：异常处理 pad（landingpad, catchpad, cleanuppad）不能删除，因为它们影响异常处理语义。

**3. 调试标签指令 (行 435-439)**

```cpp
if (const DbgLabelInst *DLI = dyn_cast<DbgLabelInst>(I)) {
  if (DLI->getLabel())
    return false;
  return true;
}
```

目的作用：有标签的调试指令不能删除（可能影响调试器），无标签的可以删除。

**4. 可移除的内存分配 (行 441-443)**

```cpp
if (auto *CB = dyn_cast<CallBase>(I))
  if (isRemovableAlloc(CB, TLI))
    return true;
```

目的作用：通过 `isRemovableAlloc` 检查是否是可移除的内存分配（如未使用的 malloc/new）。

**5. 不返回值的指令处理 (行 445-469)**

```cpp
if (!I->willReturn()) {
  auto *II = dyn_cast<IntrinsicInst>(I);
  if (!II)
    return false;

  switch (II->getIntrinsicID()) {
  case Intrinsic::experimental_guard: {
    auto *Cond = dyn_cast<ConstantInt>(II->getArgOperand(0));
    return Cond && Cond->isOne();
  }
  case Intrinsic::wasm_trunc_signed:
  case Intrinsic::wasm_trunc_unsigned:
  case Intrinsic::ptrauth_auth:
  case Intrinsic::ptrauth_resign:
  case Intrinsic::ptrauth_resign_load_relative:
    return true;
  default:
    return false;
  }
}
```

目的作用：
- `experimental_guard`: 只有条件为 true 时才是 no-op，可以删除
- `wasm_trunc_*`: WebAssembly 截断 intrinsic 可以删除
- `ptrauth_*`: 指针认证 intrinsic 可以删除（注释提到可能移除 well-defined trap）

**6. 无副作用检查 (行 471-472)**

```cpp
if (!I->mayHaveSideEffects())
  return true;
```

目的作用：如果没有副作用，可以直接删除。这是最常见的情况（如算术指令、bitcast 等）。

**7. 特殊 intrinsic 处理 (行 476-514)**

```cpp
if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
  // Safe to delete llvm.stacksave and launder.invariant.group if dead.
  if (II->getIntrinsicID() == Intrinsic::stacksave ||
      II->getIntrinsicID() == Intrinsic::launder_invariant_group)
    return true;

  // Intrinsics declare sideeffects to prevent them from moving, but they are
  // nops without users.
  if (II->getIntrinsicID() == Intrinsic::allow_runtime_check ||
      II->getIntrinsicID() == Intrinsic::allow_ubsan_check)
    return true;

  if (II->isLifetimeStartOrEnd()) {
    auto *Arg = II->getArgOperand(0);
    if (isa<PoisonValue>(Arg))
      return true;

    // If the only uses of the alloca are lifetime intrinsics, then the
    // intrinsics are dead.
    return llvm::all_of(Arg->uses(), [](Use &Use) {
      return isa<LifetimeIntrinsic>(Use.getUser());
    });
  }

  // Assumptions are dead if their condition is trivially true.
  if (II->getIntrinsicID() == Intrinsic::assume &&
      isAssumeWithEmptyBundle(cast<AssumeInst>(*II))) {
    if (ConstantInt *Cond = dyn_cast<ConstantInt>(II->getArgOperand(0)))
      return !Cond->isZero();

    return false;
  }

  if (auto *FPI = dyn_cast<ConstrainedFPIntrinsic>(I)) {
    std::optional<fp::ExceptionBehavior> ExBehavior =
        FPI->getExceptionBehavior();
    return *ExBehavior != fp::ebStrict;
  }
}
```

目的作用：
- `stacksave` / `launder_invariant_group`: 可以删除
- `allow_runtime_check` / `allow_ubsan_check`: 用于防止指令移动的 intrinsic，无使用者时可删除
- `lifetime.start` / `lifetime.end`: 如果参数是 poison 或 alloca 只被 lifetime intrinsic 使用，则可删除
- `assume`: 只有条件为常量 true 时可删除
- `constrained FP intrinsic`: 非严格异常行为时可删除

**8. 调用指令特殊处理 (行 516-522)**

```cpp
if (auto *Call = dyn_cast<CallBase>(I)) {
  if (Value *FreedOp = getFreedOperand(Call, TLI))
    if (Constant *C = dyn_cast<Constant>(FreedOp))
      return C->isNullValue() || isa<UndefValue>(C);
  if (isMathLibCallNoop(Call, TLI))
    return true;
}
```

目的作用：
- `free(nullptr)` 或 `free(undef)` 可以删除
- 数学库 noop（如 `sqrt(NaN)` 保持 NaN）可以删除

**9. 常量全局变量 load (行 525-529)**

```cpp
if (auto *LI = dyn_cast<LoadInst>(I))
  if (auto *GV = dyn_cast<GlobalVariable>(
          LI->getPointerOperand()->stripPointerCasts()))
    if (!LI->isVolatile() && GV->isConstant())
      return true;
```

目的作用：非易失性加载常量全局变量可以删除（值可以在编译时确定）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Instruction` | `use_empty()` | 检查是否有使用者 |
| `TargetLibraryInfo` | - | 提供目标库信息（如 free, malloc 行为） |
| `IntrinsicInst` | `getIntrinsicID()` | 获取 intrinsic ID |
| `CallBase` | - | 调用指令基类（call, invoke） |
| `LoadInst` | `isVolatile()` | 检查是否易失性 |

---

### 优化意图

1. **快速路径优化**: 先检查 `use_empty()`，避免避免昂贵的副作用分析
2. **分层检查**: 从简单的从终止符/异常处理检查到复杂的 intrinsic 特殊处理
3. **保守但精确**: 对于有副作用的指令，精确识别哪些特殊情况下可以删除
4. **支持特殊 intrinsic**: 处理生命周期、假设、约束浮点等特殊 intrinsic

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 检查使用者 | `I->use_empty()` | `llvm/IR/Instruction.h` |
| 检查终止符 | `I->isTerminator()` | `llvm/IR/Instruction.h` |
| 检查异常处理 | `I->isEHPad()` | `llvm/IR/Instruction.h` |
| 检查副作用 | `I->mayHaveSideEffects()` | `llvm/IR/Instruction.h` |
| 检查可移除分配 | `isRemovableAlloc(CB, TLI)` | `llvm/Analysis/MemoryBuiltins.h` |
| 获取释放操作数 | `getFreedOperand(Call, TLI)` | `llvm/Analysis/MemoryBuiltins.h` |
| 检查数学库 noop | `isMathLibCallNoop(Call, TLI)` | `llvm/Analysis/MemoryBuiltins.h` |

---

### 其他补充

**相关函数**:
- `wouldInstructionBeTriviallyDeadOnUnusedPaths` (行 413-423): 类似函数，但用于未使用路径上的检查，额外排除某些"标记"intrinsic（stacksave, launder_invariant_group, lifetime）

**使用场景**:
- DCE (Dead Code Elimination) Pass
- `RecursivelyDeleteTriviallyDeadInstructions` (行 538-613): 递归删除死指令
- `LICM` 中的 `sinkRegion()` (行 589-596): 删除循环内的死指令

**设计权衡**:
- 两层函数分离：`isInstructionTriviallyDead` 提供快速检查，`wouldInstructionBeTriviallyDead` 提供详细分析
- 保守策略：宁可保留可能影响语义的指令，也不要误删


---

## 函数分析：`salvageKnowledge`（行 292-306）

### 函数签名与目的

```cpp
bool llvm::salvageKnowledge(Instruction *I, AssumptionCache *AC,
                            DominatorTree *DT) {
  if (!EnableKnowledgeRetention || I->isTerminator())
    return false;
  bool Changed = false;
  AssumeBuilderState Builder(I->getModule(), I, AC, DT);
  Builder.addInstruction(I);
  if (auto *Intr = Builder.build()) {
    Intr->insertBefore(I->getIterator());
    Changed = true;
    if (AC)
      AC->registerAssumption(Intr);
  }
  return Changed;
}
```

**功能**: 尝试从指令中提取知识（如 nonnull、alignment、dereferenceable 等属性），并构建 `llvm.assume` intrinsic 来保存这些知识，然后将 assume 插入到指令之前，以防止后续优化丢失这些信息。

---

### 整体结构

```
salvageKnowledge(I, AC, DT)
├── 前置检查
│   ├── EnableKnowledgeRetention 全局开关
│   └── 终止符指令跳过
├── 初始化 AssumeBuilderState
├── 从指令中提取知识
│   └── addInstruction() → 根据指令类型提取属性
├── 构建 assume intrinsic
│   └── build() → 将知识打包为 operand bundle
├── 插入 assume 到指令之前
│   └── insertBefore()
└── 注册到 AssumptionCache
    └── registerAssumption()
```

---

### 逐段注释

**1. 前置检查（行 294-295）**

```cpp
if (!EnableKnowledgeRetention || I->isTerminator())
  return false;
```

目的作用：
- `EnableKnowledgeRetention` 是全局开关（默认 false），需要通过 `-enable-knowledge-retention` 命令行参数启用
- 终止符指令（br, ret, switch 等）不包含可提取的知识，直接跳过

**2. 初始化 AssumeBuilderState（行 297）**

```cpp
AssumeBuilderState Builder(I->getModule(), I, AC, DT);
```

目的作用：创建 `AssumeBuilderState` 对象，用于收集和管理知识。参数包括：
- `I->getModule()`: 模块指针，用于创建 assume intrinsic 声明
- `I`: 当前正在修改的指令，用于判断上下文有效性
- `AC`: AssumptionCache，用于查询已有的知识
- `DT`: DominatorTree，用于支配关系判断

**3. 从指令中提取知识（行 298）**

```cpp
Builder.addInstruction(I);
```

目的作用：根据指令类型提取知识。`addInstruction` 内部逻辑（行 267-279）：

```cpp
void addInstruction(Instruction *I) {
  if (auto *Call = dyn_cast<CallBase>(I))
    return addCall(Call);
  if (auto *Load = dyn_cast<LoadInst>(I))
    return addAccessedPtr(I, Load->getPointerOperand(), Load->getType(),
                          Load->getAlign());
  if (auto *Store = dyn_cast<StoreInst>(I))
    return addAccessedPtr(I, Store->getPointerOperand(),
                          Store->getValueOperand()->getType(),
                          Store->getAlign());
  // TODO: Add support for other Instructions.
}
```

- **CallBase**: 提取函数参数和函数本身的属性
- **LoadInst**: 提取指针的 `nonnull`、`dereferenceable`、`alignment` 知识
- **StoreInst**: 同样提取指针的相关知识

**4. 构建 assume intrinsic（行 299）**

```cpp
if (auto *Intr = Builder.build()) {
```

目的作用：`build()` 方法（行 222-249）将收集的知识打包为 `llvm.assume` intrinsic：

```cpp
AssumeInst *build() {
  if (AssumedKnowledgeMap.empty())
    return nullptr;
  if (!DebugCounter::shouldExecute(BuildAssumeCounter))
    return nullptr;
  Function *FnAssume =
      Intrinsic::getOrInsertDeclaration(M, Intrinsic::assume);
  LLVMContext &C = M->getContext();
  SmallVector<OperandBundleDef, 8> OpBundle;
  for (auto &MapElem : AssumedKnowledgeMap) {
    SmallVector<Value *, 2> Args;
    if (MapElem.first.first)
      Args.push_back(MapElem.first.first);
    if (MapElem.second)
      Args.push_back(ConstantInt::get(Type::getInt64Ty(M->getContext()),
                                            MapElem.second));
    OpBundle.push_back(OperandBundleDefT<Value *>(
        std::string(Attribute::getNameFromAttrKind(MapElem.first.second)),
        Args));
    NumBundlesInAssumes++;
  }
  NumAssumeBuilt++;
  return cast<AssumeInst>(CallInst::Create(
      FnAssume, ArrayRef<Value *>({ConstantInt::getTrue(C)}), OpBundle));
}
```

- 如果没有收集到知识，返回 nullptr
- 创建 `llvm.assume` 函数声明
- 将知识打包为 operand bundle（每个 bundle 包含属性名、操作数、参数值）
- 创建 assume 调用，第一个参数是 `true`（条件），后续参数是知识 bundle

**5. 插入 assume 到指令之前（行 300-301）**

```cpp
Intr->insertBefore(I->getIterator());
Changed = true;
```

目的作用：将构建好的 assume intrinsic 插入到指令 I 之前，确保 assume 在指令删除前已经记录了相关知识。

**6. 注册到 AssumptionCache（行 302-304）**

```cpp
if (AC)
  AC->registerAssumption(Intr);
```

目的作用：将新创建的 assume 注册到 AssumptionCache，使得后续 Pass 可以查询这些知识，避免重复或冗余。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AssumeBuilderState` | `AssumedKnowledgeMap` | 存储收集的知识，键是 `(Value*, Attribute::AttrKind)`，值是属性参数 |
| `AssumeBuilderState` | `InstBeingModified` | 当前正在修改的指令，用于上下文判断 |
| `RetainedKnowledge` | `AsOn`, `AttrKind`, `ArgValue` | 知识的表示：作用于哪个值、什么属性、参数值 |
| `AssumptionCache` | - | 全局知识缓存，用于查询已有的 assume |

---

### 优化意图

1. **知识保留机制**: 当指令被删除或移动时，其携带的属性（如 nonnull、alignment）可能丢失。通过在删除前插入 `llvm.assume` 保存这些知识，确保后续优化仍然可以利用这些信息。
2. **按需构建**: 只有当确实收集到知识时才创建 assume，避免无意义的 assume。
3. **上下文感知**: `AssumeBuilderState` 使用 `InstBeingModified` 和 `DT` 来判断知识在当前上下文下是否有效，避免错误的假设。
4. **全局开关**: 默认关闭，需要显式启用，因为知识保留可能增加代码大小和编译时开销。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 创建 assume 声明 | `Intrinsic::getOrInsertDeclaration(M, Intrinsic::assume)` | `llvm/IR/Intrinsics.h` |
| 创建带 bundle 的调用 | `CallInst::Create(Fn, Args, OpBundle)` | `llvm/IR/Instructions.h` |
| 插入指令 | `insertBefore(Iterator)` | `llvm/IR/Instruction.h` |
| 注册 assume | `AC->registerAssumption(Intr)` | `llvm/Analysis/AssumptionCache.h` |
| 判断判断指令类型 | `dyn_cast<LoadInst>(I)` | `llvm/IR/Instructions.h` |
| 获取指针操作数 | `getPointerOperand()` | `llvm/IR/Instructions.h` |

---

### 其他相关

**相关知识提取函数**:

- `addCall()` (行 205-220): 提取调用指令的参数属性和函数属性
- `addAccessedPtr()` (行 251-265): 提取访存指令的指针知识（nonnull、dereferenceable、alignment）
- `canonicalizedKnowledge()` (行 71-100): 将知识规范化（如处理 GEP 的 alignment、dereferenceable 的 offset）

**使用场景**:
- 在删除指令前调用（如 LICM 的 `sinkRegion` 中）
- 在指令移动或变换前调用，防止丢失属性信息
- `AssumeBuilderPass` 遍历函数中的所有指令，尝试保留知识

**与 simplifyAssumes 的关系**:
- `salvageKnowledge` 负责创建 assume 来保存知识
- `simplifyAssumes` (行 544-560) 负责清理和合并冗余的 assume
- 两者配合工作：先创建，后简化，避免 assume 过多

**设计权衡**:
- 默认关闭：知识保留可能增加代码大小，需要显式启用
- 支配关系检查：确保 assume 在正确的位置，避免错误的优化
- 缓存机制：通过 AssumptionCache 避免重复知识

---

## 函数分析：`salvageDebugInfo`（行 2014-2018）

### 函数签名与目的（行 2014-2018）

```cpp
/// Where possible to salvage debug information for \p I do so.
/// If not possible mark undef.
void llvm::salvageDebugInfo(Instruction &I) {
  SmallVector<DbgVariableRecord *, 1> DPUsers;
  findDbgUsers(&I, DPUsers);
  salvageDebugInfoForDbgValues(I, DPUsers);
}
```

**功能**: 尝试挽救（salvage）即将被删除的指令 `I` 的调试信息。如果无法挽救，则将调试信息标记为 undefined。

---

### 整体结构

```
salvageDebugInfo(Instruction &I)
├── 查找使用 I 的 DbgVariableRecord
└── 尝试挽救这些调试记录
```

---

### 逐段注释

**1. 查找调试信息用户（行 2015-2016）**

```cpp
SmallVector<DbgVariableRecord *, 1> DPUsers;
findDbgUsers(&I, DPUsers);
```

目的作用：查找所有使用指令 `I` 作为位置操作数的调试变量记录（`DbgVariableRecord`）。这些记录描述了变量在程序中的位置信息，用于调试器跟踪变量值。

---

**2. 尝试挽救调试信息（行 2017）**

```cpp
salvageDebugInfoForDbgValues(I, DPUsers);
```

目的作用：调用核心挽救逻辑，尝试为每个调试记录找到替代的变量位置表示。如果无法找到有效的替代，则将调试记录标记为（kill location），表示该变量值在此时变为 undefined。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `DbgVariableRecord` | - | 调试变量记录，表示变量的位置信息（如 dbg.value、dbg.declare、dbg.assign） |
| `SmallVector<DbgVariableRecord *, 1>` | DPUsers | 存储使用指令 I 的调试记录的容器 |

---

### 优化意图

1. **保持调试信息可用性**：当优化器删除指令时，尝试保留变量值的调试信息，避免调试器中变量变为"optimized out"
2. **使用 DIExpression 表示**：如果无法直接使用指令 I 的结果，尝试用 DWARF 表达式（`DIExpression`）表示计算过程
3. **降级处理**：当无法挽救时，优雅地标记变量为 undefined，而不是让调试器崩溃

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 查找调试用户 | `findDbgUsers` | `llvm/IR/DebugInfo.h` |
| 挽救调试值 | `salvageDebugInfoForDbgValues` | Local.cpp:2055 |

---

## 函数分析：`MemorySSAUpdater::removeMemoryAccess`（行 1286–1351）

### 函数签名与目的

```cpp
void MemorySSAUpdater::removeMemoryAccess(MemoryAccess *MA, bool OptimizePhis)
```

**功能**：从 MemorySSA 中删除一个 MemoryAccess 节点，重定向该节点的所有使用者，并可选择优化平凡的 MemoryPhi 节点。

---

### 整体结构

```
removeMemoryAccess(MA, OptimizePhis)
├── 检查：不能删除 LiveOnEntryDef
├── 确定新的定义目标
│   ├── 如果是 MemoryPhi：检查是否可删除（所有 incoming 值相同或无使用者）
│   └── 如果是 MemoryUseOrDef：获取其 defining access
├── 重定向 MA 的所有使用者到 NewDefTarget
├── 从 MSSA 的 lookup 和 lists 中删除 MA
└── 如果 OptimizePhis 为 true，尝试删除平凡的 phi 节点
```

---

### 逐段注释

**1. 检查 LiveOnEntryDef（行 1287-1288）**

```cpp
assert(!MSSA->isLiveOnEntryDef(MA) &&
       "Trying to remove of live on entry def");
```

目的作用：LiveOnEntryDef 是 MemorySSA 中的特殊节点，表示函数入口处的内存状态，不能删除。
注释说明：断言检查，防止删除根节点。

**2. 确定新的定义目标（行 1291-1303）**

```cpp
MemoryAccess *NewDefTarget = nullptr;
if (MemoryPhi *MP = dyn_cast<MemoryPhi>(MA)) {
  NewDefTarget = onlySingleValue(MP);
  assert((NewDefTarget || MP->use_empty()) &&
         "We can't delete this memory phi");
} else {
  NewDefTarget = cast<MemoryUseOrDef>(MA)->getDefiningAccess();
}
```

目的作用：确定删除 MA 后，其使用者应该指向哪个新的定义。
注释说明：
- 如果是 MemoryPhi：检查所有 incoming 值是否相同（通过 `onlySingleValue`），如果是则用该值替换 phi
- 如果是 MemoryUseOrDef：获取其 defining access，即该访问使用的内存定义

**3. 重定向所有使用者（行 1307-1331）**

```cpp
if (!isa<MemoryUse>(MA) && !MA->use_empty()) {
  if (MA->hasValueHandle())
    ValueHandleBase::ValueIsRAUWd(MA, NewDefTarget);
  
  assert(NewDefTarget != MA && "Going into an infinite loop");
  
  while (!MA->use_empty()) {
    Use &U = *MA->use_begin();
    if (auto *MUD = dyn_cast<MemoryUseOrDef>(U.getUser()))
      MUD->resetOptimized();
    if (OptimizePhis)
      if (MemoryPhi *MP = dyn_cast<MemoryPhi>(U.getUser()))
        PhisToCheck.insert(MP);
    U.set(NewDefTarget);
  }
}
```

目的作用：将所有使用 MA 的地方改为使用 NewDefTarget。
注释说明：
- 只处理 MemoryDef 和 MemoryPhi（MemoryUse 不需要重定向）
- 重置被修改的 MemoryUseOrDef 的 optimized 标志
- 如果 OptimizePhis 为 true，收集受影响的 MemoryPhi 节点用于后续优化
- 使用 while 循环逐个重定向使用者，避免迭代器失效

**4. 从 MSSA 中删除 MA（行 1336-1337）**

```cpp
MSSA->removeFromLookups(MA);
MSSA->removeFromLists(MA);
```

目的作用：从 MemorySSA 的内部数据结构中删除节点。
注释说明：`removeFromLookups` 从查找表中删除，`removeFromLists` 从访问列表中删除。

**5. 优化平凡的 Phi 节点（行 1340-1350）**

```cpp
if (!PhisToCheck.empty()) {
  SmallVector<WeakVH, 16> PhisToOptimize{PhisToCheck.begin(),
                                         PhisToCheck.end()};
  PhisToCheck.clear();

  unsigned PhisSize = PhisToOptimize.size();
  while (PhisSize-- > 0)
    if (MemoryPhi *MP =
            cast_or_null<MemoryPhi>(PhisToOptimize.pop_back_val()))
      tryRemoveTrivialPhi(MP);
}
```

目的作用：递归尝试删除平凡的 MemoryPhi 节点（例如所有 incoming 值相同的 phi）。
注释说明：
- 使用 WeakVH 避免递归优化期间的悬空指针问题
- 反向遍历避免迭代器失效
- `tryRemoveTrivialPhi` 会递归删除所有可简化的 phi

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MemoryAccess` | - | MemorySSA 中所有内存访问节点的基类 |
| `MemoryPhi` | operands | MemorySSA PHI 节点，合并来自多个前驱的内存状态 |
| `MemoryUseOrDef` | - | MemoryUse 或 MemoryDef 的基类 |
| `MemorySSA` | - | Memory SSA 分析结果 |
| `WeakVH` | - | 弱值句柄，避免悬空指针问题 |

---

### 优化意图

1. **维护 SSA 形式**：删除节点时必须重定向所有使用者，避免悬空引用
2. **递归 phi 优化**：删除一个节点可能使其他 phi 变得平凡，需要递归处理
3. **避免冗余工作**：通过 OptimizePhis 参数控制是否优化 phi，避免不必要的开销
4. **使用 WeakVH**：递归优化期间 phi 可能被删除，需要使用弱引用

对于重要部分，要解释其为什么这么优化：
- 使用 `while (!MA->use_empty())` 而不是 `for (Use &U : MA->uses())`，是因为在循环内部会修改使用者列表，可能导致迭代器失效
- 使用 WeakVH 存储待优化的 phi，因为在递归优化过程中，某些 phi 可能已经被删除，弱引用可以安全地检测这种情况
- 只重定向 MemoryDef 和 MemoryPhi 的使用者，因为 MemoryUse 不定义内存状态，不需要重定向

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 获取 phi 的唯一值 | `onlySingleValue` | `MemorySSAUpdater.cpp:547` |
| 删除平凡 phi | `tryRemoveTrivialPhi` | `MemorySSAUpdater.cpp:202` |
| 从查找表删除 | `MemorySSA::removeFromLookups` | `llvm/Analysis/MemorySSA.h` |
| 从列表删除 | `MemorySSA::removeFromLists` | `llvm/Analysis/MemorySSA.h` |
| 重值句柄通知 | `ValueHandleBase::ValueIsRAUWd` | `llvm/IR/ValueHandle.h` |

**使用示例**：参考 `LICM.cpp::eraseInstruction` 中的调用方式（行 412-413）。

---

### 调用时机

该函数在以下场景中被调用：

1. **LICM.cpp::eraseInstruction**：删除指令时清理 MemorySSA
2. **tryRemoveTrivialPhi**：递归调用，删除平凡的 phi 节点时
3. **各种 CFG 更新场景**：边删除、块合并等操作后清理无效的 MemoryAccess

---

### 其他补充

**正确性保证**：
- LiveOnEntryDef 不能删除：这是 MemorySSA 的根节点
- MemoryUse 不需要重定向：因为 use 不定义内存状态
- 递归优化顺序：反向遍历避免迭代器失效
- WeakVH 使用：递归优化期间 phi 可能被删除，需要使用弱引用

**与 SSA 构造算法的关系**：
- `removeMemoryAccess` 是 SSA 构造算法的逆操作
- SSA 构造时插入 phi 节点，删除时需要清理可能变得平凡的 phi
- 维护 SSA 形式是 MemorySSA 正确性的核心保证

---

## `isNotUsedOrFoldableInLoop` 函数分析

> 源文件：`llvm/lib/Transforms/Scalar/LICM.cpp`，第 1330–1370 行（辅助函数 `isFoldableInLoop`：第 1300–1322 行）

### 函数签名与目的

```cpp
static bool isNotUsedOrFoldableInLoop(const Instruction &I, const Loop *CurLoop,
                                      const LoopSafetyInfo *SafetyInfo,
                                      TargetTransformInfo *TTI,
                                      bool &FoldableInLoop, bool LoopNestMode)
```

**功能**：判断指令 `I` 是否可以被 sink 出 `CurLoop`——要么所有 use 都在循环外（直接可 sink），要么循环内的 use 可被折叠（例如 GEP 被吸收进 load/store 的寻址模式）。是 LICM sink 阶段的核心 legality check 之一。

---

### 整体结构

```
isNotUsedOrFoldableInLoop(I, CurLoop, SafetyInfo, TTI, FoldableInLoop, LoopNestMode)
├── 获取 BlockColors（Windows EH funclet 着色信息）
├── 查询 I 是否可折叠（isFoldableInLoop）
└── 遍历 I 的所有 users
    ├── 若 user 是 PHINode
    │   ├── PHI 所在 BB 有 CatchSwitchInst → 直接 return false
    │   ├── I 是 CallInst 且 BB 颜色不唯一 → return false
    │   └── LoopNestMode：沿单 use PHI 链向下穿透，找到真正 user
    └── 若 user 在循环内
        ├── IsFoldable → 设置 FoldableInLoop = true，continue
        └── 否则 → return false（有不可处理的循环内 use）
返回 true（所有 user 均通过检查）
```

---

### 逐段注释

**1. 前置信息准备（第 1334–1335 行）**

```cpp
const auto &BlockColors = SafetyInfo->getBlockColors();
bool IsFoldable = isFoldableInLoop(I, CurLoop, TTI);
```

- `BlockColors`：Windows SEH/EH 的 funclet 着色表，记录每个 BB 属于哪个 funclet（catch/finally 块）。在非 EH 路径上为空 map。
- `IsFoldable`：当前仅对 GEP 有效——若 GEP 的 TTI cost 为 `TCC_Free` 且所有循环内 user 都是同 BB 的 load/store，则认为该 GEP 会被折叠进寻址模式，loop 内的 use 可以被「接受」而不阻止 sink。

**2. PHINode user 的特殊处理（第 1338–1358 行）**

```cpp
if (const PHINode *PN = dyn_cast<PHINode>(UI)) {
  const BasicBlock *BB = PN->getParent();
  // ① 不能 sink 进 catchswitch
  if (isa<CatchSwitchInst>(BB->getTerminator()))
    return false;
  // ② CallInst sink 到 funclet 需要唯一着色
  if (isa<CallInst>(I))
    if (!BlockColors.empty() &&
        BlockColors.find(const_cast<BasicBlock *>(BB))->second.size() != 1)
      return false;
  // ③ LoopNestMode：穿透单 use PHI 链
  if (LoopNestMode) {
    while (isa<PHINode>(UI) && UI->hasOneUser() &&
           UI->getNumOperands() == 1) {
      if (!CurLoop->contains(UI))
        break;
      UI = cast<Instruction>(UI->user_back());
    }
  }
}
```

三个子检查：

- **① CatchSwitch 屏障**：`CatchSwitchInst` 终结的 BB 是 EH dispatch 点，指令不能被 sink 至其中，否则破坏异常处理语义。
- **② CallInst funclet 唯一性**：将 call sink 到某 funclet 中时，必须能唯一确定目标 funclet（`CV.size() == 1`）。若一个 BB 同时属于多个 funclet（着色不唯一），sink 会生成语义错误的 funclet bundle。
- **③ LoopNestMode PHI 穿透**：在循环嵌套模式下，内层循环的 LCSSA PHI 往往是「只有一个操作数、只有一个 user」的透传节点。这里沿链向下找到真正消费该值的指令，避免因中间 LCSSA PHI 导致误判「loop 内有 use」而放弃 sink。

**3. 循环内 user 的处置（第 1361–1367 行）**

```cpp
if (CurLoop->contains(UI)) {
  if (IsFoldable) {
    FoldableInLoop = true;
    continue;
  }
  return false;
}
```

- 若 `UI` 在循环内且不可折叠 → 无法 sink，立即返回 `false`。
- 若可折叠（`IsFoldable`）→ 设置输出参数 `FoldableInLoop = true` 并继续遍历其余 user。该 flag 会被上层调用方（`canSinkOrHoistInst` → `sinkInstruction`）用于决定是否保留原指令的副本（因为 loop 内仍有寻址模式使用）。

---

### 关键数据结构

| 结构 | 关键接口 | 含义 |
|---|---|---|
| `LoopSafetyInfo::BlockColors` | `getBlockColors()` → `DenseMap<BB*, ColorVector>` | Windows EH funclet 着色，每个 BB 映射到其所属的 funclet BB 列表 |
| `ColorVector` | `size()` / `front()` | 存储 BB 所属 funclet 集合，`size() == 1` 表示唯一 funclet |
| `TargetTransformInfo` | `getInstructionCost(I, TCK_SizeAndLatency)` | 查询指令在目标机器上的代价，`TCC_Free` 表示无代价（将折叠） |

---

### `isFoldableInLoop` 辅助函数（第 1300–1322 行）

```cpp
static bool isFoldableInLoop(const Instruction &I, const Loop *CurLoop,
                              const TargetTransformInfo *TTI) {
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    if (TTI->getInstructionCost(&I, ...) != TCC_Free) return false;
    // 若 loop 内有非同 BB 或非 load/store 的 user → 不可折叠
    for (const User *U : GEP->users()) {
      const Instruction *UI = cast<Instruction>(U);
      if (CurLoop->contains(UI) &&
          (BB != UI->getParent() || (!isa<StoreInst>(UI) && !isa<LoadInst>(UI))))
        return false;
    }
    return true;
  }
  return false;  // 当前仅 GEP 可折叠
}
```

目前只处理 GEP：当 GEP 被用作同 BB load/store 的 base 地址，且目标机器认为该 GEP 代价为零（将被 ISel 折进寻址模式）时，允许 sink，同时保留 loop 内的原 GEP（`FoldableInLoop = true`，上层不删除原指令）。

---

### 优化意图

1. **精确区分「可 sink」与「不可 sink」**：函数名 `isNotUsedOrFoldableInLoop` 精确表达了判断逻辑——不是「所有 use 在循环外」（过于保守），而是「所有 use 在循环外 **OR** 循环内的 use 可被折叠」。这使得带 GEP 地址计算的 load/store 也能被 sink。

2. **`FoldableInLoop` 输出参数的设计**：通过输出参数而非简单的 bool 返回值，向上层传递「循环内是否存在可折叠的 use」。上层据此决定是否在 sink 后保留原指令的原地副本（避免破坏 loop 内 load/store 的寻址模式）。

3. **LoopNestMode PHI 穿透的必要性**：在循环嵌套场景下，`LoopSimplify` 和 `LCSSA` 会在内层循环出口插入大量单操作数 PHI。若不穿透这些 PHI，sink 判断会因为「PHI 在 loop 内」而错误地放弃优化。穿透后找到真正 user 再判断，提升了嵌套循环的 sink 机会。

4. **EH 路径保守处理**：`CatchSwitch` 和多色 funclet 的检查代价极低，但一旦遗漏会导致 EH 语义破坏（程序 crash 而非产生错误输出），因此在此处提前拦截，而不依赖上层调用链的其他检查。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 获取 EH funclet 着色 | `SafetyInfo->getBlockColors()` | `llvm/Analysis/MustExecute.h` |
| 指令代价查询 | `TTI->getInstructionCost(I, TCK_SizeAndLatency)` | `llvm/Analysis/TargetTransformInfo.h` |
| 判断 BB 是否在循环内 | `CurLoop->contains(UI)` | `llvm/Analysis/LoopInfo.h` |
| EH pad 类型判断 | `BB->getTerminator()` / `isa<CatchSwitchInst>` | `llvm/IR/Instructions.h` |
| 辅助：可折叠判断 | `isFoldableInLoop()` | `LICM.cpp` 第 1300 行 |

---

### 调用上下文

该函数在 `canSinkOrHoistInst()` 中被调用，作为 sink 决策的 use 合法性检查环节：

```text
sinkInstruction()
  -> canSinkOrHoistInst()          // sink 总入口
     -> isNotUsedOrFoldableInLoop() // use 合法性
     -> isSafeToExecuteUnconditionally() // 指令是否可在出口安全执行
```

`FoldableInLoop` 返回 `true` 时，`sinkInstruction` 会选择**克隆**而非移动该指令，原指令留在循环内继续充当 load/store 的寻址 GEP。

---

## `isFoldableInLoop` 函数分析

> 源文件：`llvm/lib/Transforms/Scalar/LICM.cpp`，第 1299–1322 行

### 函数签名与目的（行号 1299-1322）

```cpp
/// Return true if the instruction is foldable in the loop.
static bool isFoldableInLoop(const Instruction &I, const Loop *CurLoop,
                             const TargetTransformInfo *TTI)
```

**功能**：判断指令 `I` 是否可以被"折叠"进循环内的寻址模式中——即该指令虽然在循环内仍有使用者，但不需要单独生成，可以被后端合并消除，因此不阻止 sink 操作。

---

### 整体结构

```
isFoldableInLoop(I, CurLoop, TTI)
├── 1. 类型检查：I 是否为 GEP 指令
│   ├── 否：直接 return false（当前只支持 GEP）
│   └── 是：进入 GEP 专属分析
│       ├── 2. TTI 代价查询：GEP 代价是否为 TCC_Free
│       │   └── 非 Free → return false
│       └── 3. In-loop 用户验证：所有循环内用户是否满足严格条件
│           ├── 用户在循环内 && (不同 BB 或非 Load/Store) → return false
│           └── 全部通过 → return true
└── 返回值：true=可折叠，false=不可折叠
```

---

### 逐段注释

**1. 类型筛选：仅处理 GEP（行 1302）**

```cpp
if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
```

函数当前只对 `GetElementPtrInst` 实施折叠判断。对其他所有指令类型，直接返回 `false`（行 1321）。
`GetElementPtrInst` 是 LLVM 中计算指针地址偏移的专属指令，在后端往往可以被合并进 `load`/`store` 的寻址模式（如 `[base + offset]`），从而不需要额外的机器指令。

---

**2. TTI 代价查询：GEP 是否为"零代价"（行 1303-1306）**

```cpp
InstructionCost CostI =
    TTI->getInstructionCost(&I, TargetTransformInfo::TCK_SizeAndLatency);
if (CostI != TargetTransformInfo::TCC_Free)
  return false;
```

通过 `TargetTransformInfo::getInstructionCost()` 查询该 GEP 指令在目标平台上的代价。
- `TCK_SizeAndLatency`：综合代码大小与延迟的代价模型。
- `TCC_Free`：特殊常量，表示该指令"免费"——后端可以将其折叠进寻址模式，不产生额外指令。

若代价不为 `TCC_Free`，说明这个 GEP 在后端会生成独立指令，无法折叠，因此返回 `false`。

---

**3. 保守验证：GEP 在循环内的用户必须满足严格约束（行 1307-1318）**

```cpp
const BasicBlock *BB = GEP->getParent();
for (const User *U : GEP->users()) {
  const Instruction *UI = cast<Instruction>(U);
  if (CurLoop->contains(UI) &&
      (BB != UI->getParent() ||
       (!isa<StoreInst>(UI) && !isa<LoadInst>(UI))))
    return false;
}
return true;
```

这是整个函数的核心约束，分两层判断：

- **外层条件**：`CurLoop->contains(UI)` —— 只检查仍在循环内的使用者（不在循环内的不影响 foldability）。
- **内层"否决"条件**（满足其一则 return false）：
  - `BB != UI->getParent()`：用户不在与 GEP 相同的基本块。不同 BB 的 load/store 无法将 GEP 折叠进其寻址模式，因为 GEP 和用户在不同 BB 内无法合并。
  - `!isa<StoreInst>(UI) && !isa<LoadInst>(UI)`：用户不是 `load` 或 `store`。GEP 折叠只能发生于 `load`/`store` 的寻址模式中，如果是其他类型的指令（例如 `icmp`、另一个 GEP），则 GEP 的结果不能被折叠。

**为什么需要这个保守检查？**
代码注释（行 1307-1309）明确说明：TTI 的 `getInstructionCost()` 当前对 GEP 是**过于乐观的**——它会假设 GEP 一定能折叠进寻址模式，而不管其实际用户情况。这层检查修正了 TTI 的过度乐观估计，确保只有满足条件的 GEP 才被认为可折叠。

---

### 关键数据结构

| 结构/类型 | 关键字段/接口 | 含义 |
|---|---|---|
| `GetElementPtrInst` | `getParent()`, `users()` | GEP 指令，计算指针偏移；`getParent()` 返回所在基本块 |
| `InstructionCost` | 比较运算符 `!=` | 代表指令代价，可以与 `TCC_Free` 等常量比较 |
| `TargetTransformInfo` | `getInstructionCost(I, TCK)` | 目标相关的代价模型接口 |
| `TargetTransformInfo::TCC_Free` | 枚举常量 | 表示指令可被后端合并消除，代价为零 |
| `Loop` | `contains(BB/I)` | 判断基本块或指令是否属于当前循环 |

---

### 优化意图

1. **允许 GEP sink 而不引入新指令**：将循环不变的 GEP（循环不变指针计算）sink 出循环，本意是在出口 block 执行而非每次循环迭代执行。但如果 GEP 在循环内被 `load`/`store` 使用且能折叠，则 sink 后可在出口 block 克隆一份，循环内的那份与 load/store 合并，净效果是消除了 GEP 作为独立指令的存在。

2. **弥补 TTI 乐观估计**：`getInstructionCost` 对 GEP 报告 `TCC_Free` 基于最理想假设，实际能否折叠还取决于用户类型和基本块关系。本函数通过显式验证"同 BB 且用户为 load/store"来补足这一保守检查，避免错误地认为可以折叠。

3. **为 `isNotUsedOrFoldableInLoop` 提供 foldability 判断**：调用者（行 1335）用本函数的返回值来判断循环内用户是否属于"可接受的残留"，从而决定是否可以 sink 指令（克隆到出口 block 而非移动）。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 查询指令代价 | `TTI->getInstructionCost(I, TCK_SizeAndLatency)` | `llvm/Analysis/TargetTransformInfo.h` |
| 零代价常量 | `TargetTransformInfo::TCC_Free` | `llvm/Analysis/TargetTransformInfo.h` |
| 判断指令在循环内 | `CurLoop->contains(UI)` | `llvm/Analysis/LoopInfo.h` |
| 类型匹配 | `dyn_cast<GetElementPtrInst>(&I)` | `llvm/IR/Instructions.h` |
| 遍历使用者 | `GEP->users()` | `llvm/IR/Value.h` |
| 调用点 | `isFoldableInLoop(I, CurLoop, TTI)` | `LICM.cpp:1335`（由 `isNotUsedOrFoldableInLoop` 调用） |

---

### 其他补充

`isFoldableInLoop` 是 `isNotUsedOrFoldableInLoop` 的关键子判断，后者决定是否可以 sink 指令（包括 clone 到出口 block 的场景）。两者的关系是：

- `isFoldableInLoop` 为 `true`：循环内有用户，但用户可以把 GEP 吸收进自身的寻址模式，因此 sink 时需要 clone 指令到出口 block，同时设置 `FoldableInLoop = true`（提示调用者需 clone 而非 move）。
- `isFoldableInLoop` 为 `false`：循环内有真实用户且不可折叠，则不能 sink。


## 函数分析：`canSinkOrHoistInst`（行 1165–1284）

### 函数签名与目的（行 1165-1169）

```cpp
bool llvm::canSinkOrHoistInst(Instruction &I, AAResults *AA, DominatorTree *DT,
                               Loop *CurLoop, MemorySSAUpdater &MSSAU,
                               bool TargetExecutesOncePerLoop,
                               SinkAndHoistLICMFlags &Flags,
                               OptimizationRemarkEmitter *ORE)
```

**功能**：判断指令是否可以安全地提升（hoist）或下沉（sink）出循环，这是 LICM Pass 的核心合法性判断函数。

---

### 整体结构

```
canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, TargetExecutesOncePerLoop, Flags, ORE)
├── 检查指令类型是否支持 hoist/sink
│   └── isHoistableAndSinkableInst()
├── LoadInst 处理
│   ├── 检查是否为 unordered
│   ├── 检查常量内存
│   ├── 检查 invariant_load metadata
│   ├── 检查 atomic load
│   ├── 检查 invariant.start 支配
│   └── 检查是否被 loop 内 store clobber
├── CallInst 处理
│   ├── 检查是否可抛异常
│   ├── 检查是否为 convergent
│   ├── 检查是否为 coroutine presplit
│   ├── 检查 assume intrinsic
│   ├── 检查内存访问行为
│   │   ├── doesNotAccessMemory → true
│   │   ├── onlyReadsMemory → 检查 pointerInvalidatedByLoop
│   │   └── onlyWritesMemory → 检查 noConflictingReadWrites
│   └── 其他情况 → false
├── FenceInst 处理
│   └── 检查是否为 loop 中唯一内存访问
├── StoreInst 处理
│   ├── 检查是否为 unordered
│   ├── 检查是否为 loop 中唯一内存访问
│   └── 检查是否有冲突的读写
└── 其他指令
    └── 断言不涉及内存访问
```

---

### 逐段注释

**1. 指令类型检查（行 1170-1172）**

```cpp
if (!isHoistableAndSinkableInst(I))
  return false;
```

目的作用：快速过滤不支持的指令类型。
注释说明：只有特定类型的指令（Load、Store、Call、Fence、Cast、UnaryOperator、BinaryOperator、Select、GEP、Cmp、向量操作、ExtractValue、InsertValue、Freeze）可以被 hoist/sink。

**2. LoadInst 处理（行 1174-1210）**

```cpp
if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
    if (!LI->isUnordered())
      return false;
    if (!isModSet(AA->getModRefInfoMask(LI->getOperand(0))))
      return true;
    if (LI->hasMetadata(LLVMContext::MD_invariant_load))
      return true;
    if (LI->isAtomic() && !TargetExecutesOncePerLoop)
      return false;
    if (isLoadInvariantInLoop(LI, DT, CurLoop))
      return true;
    auto MU = cast<MemoryUse>(MSSA->getMemoryAccess(LI));
    bool InvariantGroup = LI->hasMetadata(LLVMContext::MD_invariant_group);
    bool Invalidated = pointerInvalidatedByLoop(
        MSSA, MU, CurLoop, I, Flags, InvariantGroup);
    if (ORE && Invalidated && CurLoop->isLoopInvariant(LI->getPointerOperand()))
      ORE->emit([&]() {
        return OptimizationRemarkMissed(
                     DEBUG_TYPE, "LoadWithLoopInvariantAddressInvalidated", LI)
                << "failed to move load with loop-invariant address "
                   "because loop may invalidate its value";
      });
    return !Invalidated;
  }
```

目的作用：判断 Load 指令是否可以安全移动。
注释说明：
- `isUnordered()`：volatile 和有序 atomic load 不能移动，因为它们有可见的副作用
- `isModSet()`：从常量内存（只读）加载总是安全的，即使别名集中有修改
- `MD_invariant_load`：用户标记为 invariant 的 load 可以直接移动
- `isAtomic() && !TargetExecutesOncePerLoop`：atomic load 在循环内多次执行时不能移动，因为可能被其他线程修改
- `isLoadInvariantInLoop()`：检查是否有 `invariant.start` intrinsic 支配 load，表示该内存位置在循环内不变
- `pointerInvalidatedByLoop()`：使用 MemorySSA walker 检查 load 是否被 loop 内的 store clobber
- `ORE->emit()`：发出优化 remark，帮助用户理解为什么 load 不能移动

**3. CallInst 处理（行 1211-1260）**

```cpp
} else if (CallInst *CI = dyn_cast<CallInst>(&I)) {
    // Don't sink calls which can throw.
    if (CI->mayThrow())
      return false;
    if (CI->isConvergent())
      return false;
    if (CI->getFunction()->isPresplitCoroutine())
      return false;
    using namespace PatternMatch;
    if (match(I, m_Intrinsic<Intrinsic::assume>()))
      return true;
    MemoryEffects Behavior = AA->getMemoryEffects(CI);
    if (Behavior.doesNotAccessMemory())
      return true;
    if (Behavior.onlyReadsMemory()) {
      auto *MU = dyn_cast<MemoryUse>(MSSA->getMemoryAccess(CI));
      if (!MU)
        return false;
      return !pointerInvalidatedByLoop(
          MSSA, MU, CurLoop, I, Flags, /*InvariantGroup=*/false);
    }
    if (Behavior.onlyWritesMemory()) {
      return noConflictingReadWrites(CI, MSSA, AA, CurLoop, Flags);
    }
    return false;
  }
```

目的作用：判断 Call 指令是否可以安全移动。
注释说明：
- `mayThrow()`：可抛异常的 call 不能移动，因为可能改变异常处理路径
- `isConvergent()`：convergent 操作（如某些 GPU 操作）隐式依赖控制流，不能移动
- `isPresplitCoroutine()`：coroutine presplit 函数不能移动（FIXME 指出这是保守策略）
- `assume` intrinsic：不访问内存且不抛异常，可以安全移动
- `doesNotAccessMemory()`：不访问内存的 call（如纯计算）可以安全移动
- `onlyReadsMemory()`：只读内存的 call，检查是否被 loop 内的 store clobber
- `onlyWritesMemory()`：只写内存的 call，检查是否有冲突的读写

**4. FenceInst 处理（行 1261-1264）**

```cpp
} else if (auto *FI = dyn_cast<FenceInst>(&I)) {
    return isOnlyMemoryAccess(FI, CurLoop, MSSAU);
  }
```

目的作用：判断 Fence 指令是否可以安全移动。
注释说明：fence 提供内存序，只有当它是 loop 中唯一的内存操作时才能移动，否则会破坏序语义。

**5. StoreInst 处理（行 1265-1267）**

```cpp
} else if (auto *SI = dyn_cast<StoreInst>(&I)) {
    if (!SI->isUnordered())
      return false;

    if (isOnlyMemoryAccess(SI, CurLoop, MSSAU))
      return true;
    return noConflictingReadWrites(SI, MSSA, AA, CurLoop, Flags);
  }
```

目的作用：判断 Store 指令是否可以安全移动。
注释说明：
- `isUnordered()`：volatile 和有序 atomic store 不能移动
- `isOnlyMemoryAccess()`：如果 store 是 loop 中唯一的内存访问，可以安全移动
- `noConflictingReadWrites()`：否则检查是否有冲突的读写

**6. 其他指令断言（行 1279-1283）**

```cpp
assert(!I.mayReadOrWriteMemory() && "unhandled aliasing");

return true;
```

目的作用：处理其他不涉及内存访问的指令。
注释说明：对于不涉及内存访问的指令（如算术、比较等），已经确认可以机械地移动，由调用者检查故障安全性。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LoadInst` | `isUnordered()`, `isAtomic()`, `hasMetadata()` | Load 指令属性检查 |
| `CallInst` | `mayThrow()`, `isConvergent()`, `getFunction()` | Call 指令属性检查 |
| `MemoryEffects` | `doesNotAccessMemory()`, `onlyReadsMemory()`, `onlyWritesMemory()` | 内存访问行为描述 |
| `MemoryUse` | - | MemorySSA 中的内存使用节点 |
| `SinkAndHoistLICMFlags` | `tooManyClobberingCalls()` | 编译时开销控制 |

---

### 优化意图

1. **分层检查**：从简单的指令类型检查到复杂的别名分析，逐步深入
2. **Load 特殊处理**：常量内存、invariant_load、invariant.start 等多种快速路径
3. **Call 安全性**：严格检查异常、convergent、coroutine 等边界情况
4. **MemorySSA 精确追踪**：通过 `pointerInvalidatedByLoop()` 和 `noConflictingReadWrites()` 精确判断内存冲突
5. **编译时保护**：通过 `Flags.tooManyClobberingCalls()` 限制 MemorySSA walker 调用次数

对于重要部分，要解释其为什么这么优化：
- Load 的多种快速路径（常量内存、invariant_load、invariant.start）避免昂贵的 MemorySSA 查询
- Call 的 convergent 检查防止跨线程通信操作被错误移动
- Fence 的唯一内存访问检查确保内存序不被破坏
- Store 的 `isOnlyMemoryAccess()` 优化处理简单情况，避免复杂的别名分析

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 检查指令类型 | `isHoistableAndSinkableInst()` | `LICM.cpp:1123` |
| 检查 Load 属性 | `isUnordered()`, `isAtomic()`, `hasMetadata()` | `llvm/IR/Instructions.h` |
| 检查 Call 属性 | `mayThrow()`, `isConvergent()`, `getFunction()` | `llvm/IR/Instructions.h` |
| 获取内存访问行为 | `AA->getMemoryEffects()` | `llvm/Analysis/AliasAnalysis.h` |
| 获取 MemoryAccess | `MSSA->getMemoryAccess()` | `llvm/Analysis/MemorySSA.h` |
| 检查是否被 clobber | `pointerInvalidatedByLoop()` | `LICM.cpp:196` |
| 检查冲突读写 | `noConflictingReadWrites()` | `LICM.cpp:193` |
| 检查唯一内存访问 | `isOnlyMemoryAccess()` | `LICM.cpp:1135` |
| 检查 Load 不变性 | `isLoadInvariantInLoop()` | `LICM.cpp:1066` |
| 检查 ModRef | `AA->getModRefInfoMask()` | `llvm/Analysis/AliasAnalysis.h` |

**使用示例**：参考 `sinkRegion()` 和 `hoistRegion()` 中的调用方式（行 609、929）。

---

### 其他补充

**与 MemorySSA 的关系**：
- `pointerInvalidatedByLoop()` 使用 MemorySSA walker 精确追踪 load 是否被 loop 内的 store clobber
- `noC onflictingReadWrites()` 使用 MemorySSA 判断 store/call 是否与 loop 内的其他内存操作冲突
- `getClobberingMemoryAccess()` 受 `Flags.tooManyClobberingCalls()` 限制，防止编译时爆炸

**调用时机**：
- 在 `sinkRegion()` 中，尝试下沉指令前调用（行 609）
- 在 `hoistRegion()` 中，尝试提升指令前调用（行 929）

**正确性保证**：
- Load：通过 MemorySSA 精确追踪确保移动后读取的值不变
- Call：检查异常、convergent、coroutine 等边界情况
- Store：确保不会引入新的写入路径或与现有读写冲突
- Fence：只有在唯一内存访问时才能移动，保证内存序

**性能优化**：
- 常量内存、invariant_load、invariant.start 等快速路径避免昂贵的 MemorySSA 查询
- `Flags.tooManyClobberingCalls()` 限制 MemorySSA walker 调用次数，防止 pathological 情况下的编译时爆炸

---

## 函数分析：`isLoadInvariantInLoop`（行 1066-1118）

### 函数签名与目的（行 1066-1118）

```cpp
static bool isLoadInvariantInLoop(LoadInst *LI, DominatorTree *DT,
                                   Loop *CurLoop)
```

**功能**: 判断一个 load 指令在循环中是否是不变的。判断依据是：循环外是否存在一个 `invariant.start` intrinsic，该 intrinsic 支配整个循环，且覆盖的内存位置和大小包含 load 的访问范围。

---

### 整体结构

```
isLoadInvariantInLoop(LoadInst *LI, DominatorTree *DT, Loop *CurLoop)
├── 获取 load 的地址和类型大小
├── 检查是否为可变大小类型 → 否则返回 false
├── 检查地址是否为常量 → 是则返回 false
├── 遍历地址的所有使用者
│   ├── 限制遍历数量（MaxNumUsesTraversed = 8）
│   ├── 检查查找到 invariant.start intrinsic
│   │   ├── 验证 intrinsic 无使用者
│   │   ├── 验证 intrinsic 大小参数非负
│   │   ├── 验证 intrinsic 覆盖 load 的大小
│   │   └── 验证 intrinsic 支配循环 header
│   └── 满足所有条件则返回 true
└── 未找到匹配的 invariant.start，返回 false
```

---

### 逐段注释

**1. 提取 load 的地址和大小信息（行 1068-1070）**

```cpp
Value *Addr = LI->getPointerOperand();
const DataLayout &DL = LI->getDataLayout();
const TypeSize LocSizeInBits = DL.getTypeSizeInBits(LI->getType());
```

目的作用：获取 load 指令加载的内存地址和加载的数据类型大小（以 bit 为单位），后续用于与 `invariant.start` 的覆盖范围进行比较。

---

**2. 拒绝可变大小类型（行 1072-1082）**

```cpp
if (LocSizeInBits.isScalable())
  return false;
```

目的作用：可变大小类型（如 SVE 的 scalable vector）不支持 `invariant.start`，因为：
- Clang 目前不会为这类类型生成 `invariant.start`
- `invariant.start` 对可变大小对象使用 -1 作为大小参数，无法精确验证覆盖范围
- 例如 `<vscale x 32 x i8>` 和 `<vscale x 16 x i8>` 都会是 -1，但前者是后者的两倍大小

---

**3. 拒绝常量地址（行 1084-1087）**

```cpp
if (isa<Constant>(Addr))
  return false;
```

目的作用：如果地址是全局变量或常量，则不需要通过 `invariant.start` 来判断不变性，且循环 pass 不应该遍历全局/常量的 uselist。

---

**4. 遍历地址的使用者，查找匹配的 invariant.start（行 1089-1115）**

```cpp
unsigned UsesVisited = 0;
for (auto *U : Addr->users()) {
  if (++UsesVisited > MaxNumUsesTraversed)
    return false;
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(U);
  if (!II || II->getIntrinsicID() != Intrinsic::invariant_start ||
      !II->use_empty())
    continue;
  ConstantInt *InvariantSize = cast<ConstantInt>(II->getArgOperand(0));
  if (InvariantSize->isNegative())
    continue;
  uint64_t InvariantSizeInBits = InvariantSize->getSExtValue() * 8;
  if (LocSizeInBits.getFixedValue() <= InvariantSizeInBits &&
      DT->properlyDominates(II->getParent(), CurLoop->getHeader()))
    return true;
}
```

目的作用：
- 遍历地址的所有使用者，限制遍历数量（`MaxNumUsesTraversed` 默认为 8），避免编译时开销过大
- 查找 `invariant.start` intrinsic，该 intrinsic 标记了一段内存区域在当前路径上不会被修改
- 验证条件：
  1. `invariant.start` 无使用者（否则可能存在转义，无法保证不变性）
  2. `invariant.start` 的大小参数非负（-1 表示可变大小对象，已排除）
  3. `invariant.start` 覆盖 load 的大小（`LocSizeInBits <= InvariantSizeInBits`）
  4. `invariant.start` 支配循环 header（确保 invariant.start 在循环外，且对整个循环有效）

---

**5. 未找到匹配的 invariant.start（行 1117）**

```cpp
return false;
```

目的作用：遍历完所有使用者后，未找到满足条件的 `invariant.start`，则认为 load 不是循环不变的。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LoadInst` | `getPointerOperand()` | 获取 load 指令加载的内存地址 |
| `TypeSize` | `isScalable()` / `getFixedValue()` | 表示类型大小，可能是固定大小或可变大小 |
| `IntrinsicInst` | `getIntrinsicID()` / `getArgOperand(0)` | `invariant.start` intrinsic，参数 0 为覆盖的字节数 |
| `DominatorTree` | `properlyDominates(BB1, BB2)` | 判断 BB1 是否严格支配 BB2 |

---

### 优化意图

1. **利用 `invariant.start` intrinsic 识别循环不变的 load**：`invariantser.start` 由前端（如 Clang）插入，用于标记在当前执行路径上不会被修改的内存区域。LICM 利用这个信息安全的将 load 提升到循环外。
2. **限制遍历数量控制编译时开销**：通过 `MaxNumUsesTraversed`（默认 8）限制遍历地址使用者的数量，避免在地址使用者过多时产生过大的编译时开销。
3. **保守的可变大小类型处理**：当前不支持可变大小类型的 `invariant.start`，直接返回 false，避免错误判断。
4. **支配关系保证安全性**：要求 `invariant.start` 支配循环 header，确保 intrinsic 在循环外，且对整个循环有效。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 获取 load 地址 | `LoadInst::getPointerOperand()` | `llvm/IR/Instructions.h` |
| 获取类型大小 | `DataLayout::getTypeSizeInBits()` | `llvm/IR/DataLayout.h` |
| 判断可变大小 | `TypeSize::isScalable()` | `llvm/Support/TypeSize.h` |
| 遍历使用者 | `Value::users()` | `llvm/IR/User.h` |
| 判断 intrinsic 类型 | `IntrinsicInst::getIntrinsicID()` | `llvm/IR/IntrinsicInst.h` |
| 支配关系判断 | `DominatorTree::properlyDominates()` | `llvm/Analysis/Dominators.h` |
| 命令行选项 | `MaxNumUsesTraversed`` | `llvm/lib/Transforms/Scalar/LICM.cpp:132-135` |

---

### 其他补充

**调用上下文**：
- 该函数在 LICM 中用于判断 load 指令是否可以安全地提升到循环外
- 通常与 `invariant.start` intrinsic 配合使用，该 intrinsic 由前端在已知内存不会修改的路径上插入

**与 MemorySSA 的关系**：
- LICM 主要依赖 MemorySSA 进行内存别名分析，但 `isLoadInvariantInLoop` 提供了一种基于 `invariant.start` 的补充判断方式
- 当 MemorySSA 无法精确判断时，`invariant.start` 可以提供额外的安全性保证

**限制**：
- 不支持可变大小类型（scalable vector）
- 限制遍历地址使用者的数量，可能在地址使用者过多时错过有效的 `invariant.start`
- 依赖前端正确插入 `invariant.start` intrinsic


---

## 函数分析：`pointerInvalidatedByLoop`（行 2375-2424）

### 函数签名与目的（行 2375-2424）

```cpp
static bool pointerInvalidatedByLoop(MemorySSA *MSSA, MemoryUse *MU,
                                 Loop *CurLoop, Instruction &I,
                                 SinkAndHoistLICMFlags &Flags,
                                 bool InvariantGroup)
```

**功能**: 判断某个内存访问指令（load/store）所访问的指针在循环内是否会被无效化（即被修改），从而决定该指令是否可以安全地提升或（下沉）出循环。

---

### 整体结构

```
pointerInvalidatedByLoop()
├── 检查是提升还是下沉场景
│   ├── 提升场景（hoisting）
│   │   ├── 使用 MemorySSA walker 查找 clobbering access
│   │   ├── 判断 clobbering access 是否在循环内
│   │   └── 特殊处理 invariant group 场景
│   └── 下沉场景（sinking）
│       ├── 遍历循环内所有基本块
│       │   └── 调用 pointerInvalidatedByBlock 检查
│       └── 检查源块是否也无效化指针
└── 返回是否被无效化
```

---

### 逐段注释

**1. 提升场景的判断逻辑（行 2378-2395）**

```cpp
if (!Flags.getIsSink()) {
    // 如果是提升场景，我们只需要检查在循环开始到 load 之间
    // 是否有 store 修改了被加载的指针（因为所有值必须相同）
    
    // 这可以通过两个条件检查：
    // 1) 如果 memory access 在循环外
    // 2) 最早的 access 在循环 header，
    //    如果加载的内存是 phi 节点
    
    BatchAAResults BAA(MSSA->getAA());
    MemoryAccess *Source = getClobberingMemoryAccess(*MSSA, BAA, Flags, MU);
    return !MSSA->isLiveOnEntryDef(Source) &&
           CurLoop->contains(Source->getBlock()) &&
           !(InvariantGroup && Source->getBlock() == CurLoop->getHeader() && 
             isa<MemoryPhi>(Source));
}
```

目的作用：在提升场景下，使用 MemorySSA 的 walker 来确定安全性。如果存在一个在循环内且会 clobber 该访问的内存操作，则不能提升。

注释说明：
- `getClobberingMemoryAccess` 查找会修改或影响该内存位置的最新访问
- `isLiveOnEntryDef` 检查该访问是否是循环入口处的定义
- `InvariantGroup` 特殊处理：如果是 invariant group，且 clobbering access 是循环 header 的 MemoryPhi，则认为安全

**2. 下沉场景的判断逻辑（行 2397-2423）**

```cpp
// 对于下沉场景，我们需要检查该 use 下方的所有 Def。
// getClobbering 调用会在循环的 backedge 上查找，但会检查与
// 前一次迭代中指令的 aliasing。
// 例如：
// for (i ... )
//   load a[i] ( Use (LoE)
//   store a[i] ( 1 = Def (2), with 2 = Phi for 循环
//   i++;
// load 在循环内看不到 clobbering，因为 backedge alias check
// 会做 phi 翻译，并检查与 store a[i-1] 的 aliasing。
// 然而将 load 下沉到循环外，store 下方是不正确的。

// 目前，只有当循环内没有 Def，且现有的 Def 在 use 之前
// 且在同一块中时才下沉。
// FIXME: 提高精度：如果 Use 后支配 Def 则安全下沉；
// 需要 PostDominatorTreeAnalysis。
// FIXME: 更精确：没有 alias 该 Use 的 Def。
if (Flags.tooManyMemoryAccesses())
  return true;
for (auto *BB : CurLoop->getBlocks())
  if (pointerInvalidatedByBlock(*BB, *MSSA, *MU))
    return true;
// 当下沉时，源块可能不是循环的一部分，所以也要检查它。
if (!CurLoop->contains(&I))
  return pointerInvalidatedByBlock(*I.getParent(), *MSSA, *MU);

return false;
```

目的作用：在下沉场景下，遍历循环内所有基本块，检查是否有任何块会无效化该指针。如果源块不在循环内，也要检查源块。

注释说明：
- 下沉场景更保守，因为需要考虑后向依赖
- 当前实现只检查同一块内的 Def，FIXME 注释表明可以用后支配树分析提高精度
- `tooManyMemoryAccesses` 是编译时优化，避免在复杂循环中做昂贵的遍历

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MemorySSA` | - | Memory SSA 分析结果，用于追踪内存访问间的依赖关系 |
| `MemoryUse` | - | 表示一个内存使用（load/store）节点 |
| `MemoryAccess` | - | MemoryUse 或 MemoryDef 的基类 |
| `SinkAndHoistLICMFlags` | `IsSink`, `NoOfMemAccTooLarge` | 控制提升/下沉行为和编译时优化标志 |
| `BatchAAResults` | - | 批量别名分析结果，用于高效的 alias 查询 |

---

### 优化意图

1. **区分提升和下沉场景**：提升只需检查前向依赖（循环开始到指令之间），下沉需检查后向依赖（指令到循环结束）
2. **使用 MemorySSA 提高精度**：通过 MemorySSA 的 walker 机制，精确追踪内存访问间的 clobbering 关系
3. **编译时优化**：通过 `tooManyMemoryAccesses` 标志避免在复杂循环中做昂贵的遍历
4. **特殊处理 invariant group**：对于标记为 invariant group 的内存访问，放宽某些检查条件
5. **保守的下沉（sink）策略**：当前下沉实现较保守，FIXME 注释表明可以通过后支配树分析提高精度

对于重要部分，要解释其为什么这么优化：
- MemorySSA 的使用避免了传统的保守别名分析，提供了更精确的内存依赖信息
- 区分提升和下沉场景是因为它们的语义不同：提升是将指令移到循环前（只需保证循环内无修改），下沉是将指令移到循环后（需保证循环内无后续修改）

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 获取 clobbering 内存访问 | `getClobberingMemoryAccess` | LICM.cpp:1151-1163 |
| 检查是否是入口定义 | `isLiveOnEntryDef` | MemorySSA.h |
| 检查块内是否无效化 | `pointerInvalidatedByBlock` | LICM.cpp:2426-2433 |
| 批量别名分析 | `BatchAAResults` | llvm/Analysis/BasicAliasAnalysis.h |

**使用示例**：
```cpp
// 在 canSinkOrHoistInst 中使用（行 1198-1210）
auto MU = cast<MemoryUse>(MSSA->getMemoryAccess(LI));
bool Invalidated = pointerInvalidatedByLoop(
    MSSA, MU, CurLoop, I, Flags, InvariantGroup);
return !Invalidated;
```

---

### 其他补充

**调用上下文**：
- 主要在 `canSinkOrHoistInst` 函数中被调用（行 1198-1210），用于判断 load 指令是否可以安全提升或下沉
- 对于只读内存的 call 指令也会调用此函数（行 1250-1251）

**正确性保证**：
- 对于提升场景，通过检查 clobbering access 是否在循环内来保证提升后的 load 仍然读取正确值
- 对于下沉场景，通过遍历所有块来保证没有后续的 store 会修改该内存位置

**性能权衡**：
- 使用 `tooManyMemoryAccesses` 避免在内存访问过多的循环中做昂贵的遍历
- MemorySSA walker 有 cap 限制（`SetLicmMssaOptCap`），超过后使用不精确的 `getDefiningAccess`

**待改进点**（根据代码中的 FIXME 注释）：
1. 下沉场景可以使用 PostDominatorTreeAnalysis 提高精度（行 2411）
2. 可以更精确地只检查 alias 该 Use 的 Def（行 2413）
3. 可以禁用提升 past 潜在干扰的 loads（行 2342）


---

## `noConflictingReadWrites` 函数分析

> 源文件：`llvm/lib/Transforms/Scalar/LICM.cpp`，第 2307–2373 行

### 函数签名与目的（行号 2307-2373）

```cpp
static bool noConflictingReadWrites(Instruction *I, MemorySSA *MSSA,
                                    AAResults *AA, Loop *CurLoop,
                                    SinkAndHoistLICMFlags &Flags)
```

**功能**：针对 `StoreInst` 或只写内存的 `CallInst`，判断在 `CurLoop` 范围内是否存在与指令 `I` 的内存操作**冲突的读或写**，若不存在冲突则返回 `true`，允许 LICM 对 `I` 执行 hoist 或 sink。

**调用者**（`canSinkOrHoistInst` 中）：

```text
canSinkOrHoistInst()
  ├── CallInst 且 onlyWritesMemory() → noConflictingReadWrites(CI, ...)
  └── StoreInst 且非 isOnlyMemoryAccess → noConflictingReadWrites(SI, ...)
```

---

### 整体结构

```
noConflictingReadWrites(I, MSSA, AA, CurLoop, Flags)
├── 1. 访问数量守卫：tooManyMemoryAccesses → return false
├── 2. 找 I 自身的 clobbering access
│   └── clobber 在循环内 → return false（I 本身被覆盖，不能移出）
└── 3. 遍历循环内所有 BB 的每条 MemoryAccess
    ├── MemoryUse（普通 load）
    │   ├── 其 clobber 在循环内 → return false（循环内写干扰该 load）
    │   └── 仅 hoist 模式：IMD 不支配 MU → return false（可能被该 load 干扰）
    └── MemoryDef（非普通 store 的 Def）
        ├── 是有序 load（ordered load 存为 Def）→ return false
        └── 是 CallInst → 检查 Call 与 I 的 ModRef
            ├── I 是 StoreInst：BAA.getModRefInfo(CI, MemLoc(SI))
            └── I 是 CallInst：BAA.getModRefInfo(CI, SCI)，跳过 CI == SCI
            └── isModOrRefSet → return false
└── return true（无冲突）
```

---

### 逐段注释

**1. 访问数量守卫（第 2310–2314 行）**

```cpp
assert(isa<CallInst>(*I) || isa<StoreInst>(*I));
if (Flags.tooManyMemoryAccesses())
  return false;
```

`SinkAndHoistLICMFlags::NoOfMemAccTooLarge` 在构造时根据循环内 MemoryAccess 数量与阈值设置。超过上限时，跳过整个分析，保守返回 `false`，避免在大循环中引发编译时爆炸。

**2. 检查 I 自身是否在循环内被 clobber（第 2316–2321 行）**

```cpp
auto *IMD = MSSA->getMemoryAccess(I);
BatchAAResults BAA(*AA);
auto *Source = getClobberingMemoryAccess(*MSSA, BAA, Flags, IMD);
if (!MSSA->isLiveOnEntryDef(Source) && CurLoop->contains(Source->getBlock()))
  return false;
```

- `IMD`：`I` 对应的 `MemoryAccess`（对 store/写 call 通常是 `MemoryDef`）。
- `getClobberingMemoryAccess` 找到真正 clobber `IMD` 的最近访问：若该 clobber **不是** `LiveOnEntry` 且位于循环内，说明循环内存在写覆盖/干扰，不能 hoist/sink。

**3. 遍历循环内所有 MemoryAccess（第 2328–2371 行）**

```cpp
for (auto *BB : CurLoop->getBlocks()) {
  auto *Accesses = MSSA->getBlockAccesses(BB);
  if (!Accesses)
    continue;
  for (const auto &MA : *Accesses)
    ...
}
```

按基本块遍历 `MemoryUse` 与 `MemoryDef`，逐类检查可能的冲突点。

**3a. `MemoryUse`：循环内读的 clobber 与 hoist 限制（第 2333–2343 行）**

```cpp
if (const auto *MU = dyn_cast<MemoryUse>(&MA)) {
  auto *MD = getClobberingMemoryAccess(*MSSA, BAA, Flags,
                                       const_cast<MemoryUse *>(MU));
  if (!MSSA->isLiveOnEntryDef(MD) && CurLoop->contains(MD->getBlock()))
    return false;
  if (!Flags.getIsSink() && !MSSA->dominates(IMD, MU))
    return false;
}
```

- 若该 `MemoryUse` 的 clobbering Def 在循环内，则循环内存在写-读链，会与移动 `I` 产生干扰风险，返回 `false`。
- 仅 hoist（`!Flags.getIsSink()`）时额外要求 `IMD` 支配 `MU`：否则 hoist 可能改变该 load 在循环内观察到的写入效果。该检查目前较保守（源码 `FIXME` 指出更精确做法应仅针对别名 `I` 的 Uses）。

**3b. `MemoryDef`：ordered load 与 call 的 ModRef 冲突（第 2344–2369 行）**

```cpp
} else if (const auto *MD = dyn_cast<MemoryDef>(&MA)) {
  if (auto *LI = dyn_cast<LoadInst>(MD->getMemoryInst())) {
    assert(!LI->isUnordered() && "Expected unordered load");
    return false;
  }
  if (auto *CI = dyn_cast<CallInst>(MD->getMemoryInst())) {
    if (auto *SI = dyn_cast<StoreInst>(I)) {
      ModRefInfo MRI = BAA.getModRefInfo(CI, MemoryLocation::get(SI));
      if (isModOrRefSet(MRI))
        return false;
    } else {
      auto *SCI = cast<CallInst>(I);
      if (SCI == CI)
        continue;
      ModRefInfo MRI = BAA.getModRefInfo(CI, SCI);
      if (isModOrRefSet(MRI))
        return false;
    }
  }
}
```

- ordered atomic load 在 MemorySSA 中可能被建模为 Def（带顺序语义），出现即放弃移动（保守且正确）。
- 对循环内 `CallInst`（作为 Def）用 `BatchAAResults` 做 ModRef 检查：若 call 可能读/写 `I` 相关位置（store-location 或 call-call），则存在潜在冲突，返回 `false`。

---

### 关键数据结构

| 类型 | 关键接口 | 含义 |
|---|---|---|
| `MemorySSA` | `getMemoryAccess`, `getBlockAccesses`, `isLiveOnEntryDef`, `dominates` | 内存 SSA 图与 clobber/支配查询 |
| `MemoryUse` | - | 读内存访问节点 |
| `MemoryDef` | `getMemoryInst()` | 写内存/有序 load/call 等访问节点 |
| `BatchAAResults` | `getModRefInfo` | 批量缓存别名与 ModRef 查询，降低重复 AA 成本 |
| `SinkAndHoistLICMFlags` | `tooManyMemoryAccesses`, `getIsSink` | 分析预算与 hoist/sink 模式控制 |

---

### 优化意图

1. **用 MemorySSA 提升精度**：通过 clobbering access 判断真实内存依赖，而非完全依赖传统 AliasSet 的保守结论。
2. **区分 hoist 与 sink 的约束**：hoist 额外限制 `dominates(IMD, MU)`，避免提升穿越潜在干扰的 loads（当前实现偏保守）。
3. **编译时开销保护**：`tooManyMemoryAccesses()` 提供预算上限，避免在高密度内存访问循环中做昂贵遍历。
4. **原子/顺序语义守卫**：遇到 ordered atomic load 直接放弃移动，防止破坏内存顺序语义。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 获取指令 MemoryAccess | `MSSA->getMemoryAccess(I)` | `llvm/Analysis/MemorySSA.h` |
| 获取块内访问序列 | `MSSA->getBlockAccesses(BB)` | `llvm/Analysis/MemorySSA.h` |
| clobber 查询 | `getClobberingMemoryAccess(...)` | `LICM.cpp`（内部 helper） |
| LiveOnEntry 判定 | `MSSA->isLiveOnEntryDef(...)` | `llvm/Analysis/MemorySSA.h` |
| 支配关系 | `MSSA->dominates(IMD, MU)` | `llvm/Analysis/MemorySSA.h` |
| ModRef 查询 | `BatchAAResults::getModRefInfo(...)` | `llvm/Analysis/AliasAnalysis.h` |
| 模式与预算 | `SinkAndHoistLICMFlags` | `llvm/Transforms/Utils/LoopUtils.h` |

---

### 其他补充

- **保守点**：hoist 场景的 `dominates(IMD, MU)` 检查未做 alias 过滤，可能过度拒绝；源码明确标注 `FIXME`，未来可按“仅对 alias I 的 Uses”收紧。
- **适用范围**：函数入口 `assert` 限定仅用于 store 或 call（写内存）这两类需要读写冲突检查的指令。

---

## 函数分析：`hoistRegion`（行 889-1060）

### 函数签名与目的（行 889-896）

```cpp
bool llvm::hoistRegion(DomTreeNode *N, AAResults *AA, LoopInfo *LI,
                       DominatorTree *DT, AssumptionCache *AC,
                       TargetLibraryInfo *TLI, Loop *CurLoop,
                       MemorySSAUpdater &MSSAU, ScalarEvolution *SE,
                       ICFLoopSafetyInfo *SafetyInfo,
                       SinkAndHoistLICMFlags &Flags,
                       OptimizationRemarkEmitter *ORE, bool LoopNestMode,
                       bool AllowSpeculation)
```

**功能**: 遍历循环内支配树区域（深度优先），将循环不变量从循环体提升到预置头块（preheader）。核心是**定义先于使用**的遍历顺序，使得 hoist 能一次完成无需迭代。

---

### 整体结构

```
hoistRegion()
├── 输入验证（assert 参数非空）
├── 初始化
│   ├── 创建 ControlFlowHoister（处理控制流提升）
│   └── 创建 HoistedInstructions 记录（后续可能需 re-hoist）
├── 工作列表构建（LoopBlocksRPO）
├── 主循环：遍历基本块（逆后序）
│   └── 遍历块内指令
│       ├── 死指令删除
│       ├── 普通 hoisting（4 个条件检查）
│       ├── FP 除转乘（Reciprocal Optimization）
│       ├── invariant.start / guard 提升
│       ├── PHI 节点提升（ControlFlowHoister）
│       └── 算术重结合提升（hoistArithmetics）
│   └── 收集可能提升的分支
├── 控制流 re-hoisting（修复 dominance）
├── 验证（MemorySSA + DT/LI）
└── 返回 Changed
```

---

### 逐段注释

**1. 输入验证与初始化（897-906 行）**

```cpp
assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
       CurLoop != nullptr && SafetyInfo != nullptr &&
       "Unexpected input to hoistRegion.");

ControlFlowHoister CFH(LI, DT, CurLoop, MSSAU);
SmallVector<Instruction *, 16> HoistedInstructions;
```

- 断言所有关键指针参数有效
- 创建 `ControlFlowHoister` 辅助对象，用于处理条件分支和 PHI 的复杂提升
- `HoistedInstructions` 记录所有提升的指令，用于后续检查是否 dominate 所有 use

---

**2. 工作列表构建（908-913 行）**

```cpp
LoopBlocksRPO Worklist(CurLoop);
Worklist.perform(LI);
bool Changed = false;
BasicBlock *Preheader = CurLoop->getLoopPreheader();
```

- 使用 `LoopBlocksRPO` 生成循环块的**逆后序**（RPO）遍历序列
- **关键点**: 逆后序保证在处理块前，其支配祖先已被处理，这对后续 re-hoisting 时的 dominance 修复至关重要

---

**3. 块内指令遍历主循环（915-1014 行）**

```cpp
for (BasicBlock *BB : Worklist) {
  if (!LoopNestMode && inSubLoop(BB, CurLoop, LI))
    continue;

  for (Instruction &I : llvm::make_early_inc_range(*BB)) {
    // ... 一系列尝试提升 I
  }
}
```

- `inSubLoop`: 跳过子循环体（已由子循环 LICM 处理）
- `make_early_inc_range`: 允许在循环中安全删除/移动指令
- 按**顺序**尝试多种提升策略，任一成功则 `continue` 跳过后续检查

---

**4. 普通 hoisting（928-938 行）**

```cpp
if (CurLoop->hasLoopInvariantOperands(&I) &&
    canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, true, Flags, ORE) &&
    isSafeToExecuteUnconditionally(I, DT, TLI, CurLoop, SafetyInfo, ORE,
                                   Preheader->getTerminator(), AC,
                                   AllowSpeculation)) {
  hoist(I, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB), SafetyInfo,
        MSSAU, SE, ORE);
  HoistedInstructions.push_back(&I);
  Changed = true;
  continue;
}
```

**三重条件**：
1. 所有操作数循环不变 (`hasLoopInvariantOperands`)
2. 可移动（无别名冲突、memory access 安全）
3. 无条件执行安全（无 side effect 或 speculation 允许）

成功则调用 `hoist()` 移动到目标块（可能是 preheader 或复制的控制流块）

---

**5. FP 除法优化（940-966 行）**

```cpp
if (I.getOpcode() == Instruction::FDiv && I.hasAllowReciprocal() &&
    CurLoop->isLoopInvariant(I.getOperand(1))) {
  // 创建: %reciprocal = fdiv 1.0, %divisor
  // 创建: %product = fmul %numerator, %reciprocal
  // 替换所有使用，删除原 FDiv
  hoist(*ReciprocalDivisor, ...);
}
```

- 仅当除数循环不变且允许 `allow_reciprocal`（fast-math 标志）
- 利用 FP 等价性：`a / b = a * (1/b)`，将除法移出循环
- 注意：需同时 hoist 新创建的 reciprocal 指令

---

**6. invariant.start / guard 提升（968-985 行）**

```cpp
if ((IsInvariantStart(I) || isGuard(&I)) &&
    CurLoop->hasLoopInvariantOperands(&I) &&
    MustExecuteWithoutWritesBefore(I)) {
  hoist(I, ...);
}
```

- `invariant.start` 是 LLVM 的指针不变性 intrinsic（用于 alias analysis 优化）
- `guard` 是保护性分支（如 `llvm.experimental.guard`）
- 需满足：循环前必定执行、循环内不写内存

---

**7. PHI 节点提升（987-1000 行）**

```cpp
if (PHINode *PN = dyn_cast<PHINode>(&I)) {
  if (CFH.canHoistPHI(PN)) {
    // 重定向所有 incoming block 到 hoisted 版本
    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
      PN->setIncomingBlock(i, CFH.getOrCreateHoistedBlock(PN->getIncomingBlock(i)));
    hoist(*PN, ...);
    assert(DT->dominates(PN, BB) && "Conditional PHIs not expected");
    Changed = true;
    continue;
  }
}
```

- `ControlFlowHoister::canHoistPHI`: 检查所有 predecessor 是否被 hoistable 分支覆盖
- 先将 incoming block 重定向到复制的块，再 hoist PHI
- 最终 PHI 应在 preheader 或复制控制流后的块中

---

**8. 算术重结合提升（1002-1007 行）**

```cpp
if (hoistArithmetics(I, *CurLoop, *SafetyInfo, MSSAU, AC, DT)) {
  Changed = true;
  continue;
}
```

- 在 `hoistArithmetics()` 中尝试将加减/FP 运算的子表达式提前计算
- 见 STATISTIC: `NumAddSubHoisted`, `NumFPAssociationsHoisted`, `NumBOAssociationsHoisted`

---

**9. 分支收集（1009-1012 行）**

```cpp
if (BranchInst *BI = dyn_cast<BranchInst>(&I))
  CFH.registerPossiblyHoistableBranch(BI);
```

- 暂存可能提升的条件分支，后续用于 PHI 提升判断
- **不是立即提升**，因为可能需复制控制流

---

**10. Control Flow Re-hoisting（1016-1045 行）**

```cpp
Instruction *HoistPoint = nullptr;
if (ControlFlowHoisting) {
  for (Instruction *I : reverse(HoistedInstructions)) {
    if (!llvm::all_of(I->uses(),
                      [&](Use &U) { return DT->dominates(I, U); })) {
      BasicBlock *Dominator = DT->getNode(I->getParent())->getIDom()->getBlock();
      if (!HoistPoint || !DT->dominates(HoistPoint->getParent(), Dominator)) {
        HoistPoint = Dominator->getTerminator();
      }
      moveInstructionBefore(*I, HoistPoint->getIterator(), ...);
      HoistPoint = I;
      Changed = true;
    }
  }
}
```

- **目的**: 修复因提升到条件块导致的 dominance 不完整问题
- 逆序遍历（use 在 def 前），逐步找到最近公共支配者（immediate dominator）
- 将指令移动到该支配点，确保 dominate 所有 use

---

**11. 验证与返回（1046-1060 行）**

```cpp
if (VerifyMemorySSA)
  MSSAU.getMemorySSA()->verifyMemorySSA();

#ifdef EXPENSIVE_CHECKS
if (Changed) {
  assert(DT->verify(DominatorTree::VerificationLevel::Fast) && ...);
  LI->verify(*DT);
}
#endif

return Changed;
```

- MemorySSA 验证（仅 debug 模式）
- 在 `EXPENSIVE_CHECKS` 下验证 DT 和 LI 一致性

---

### 关键数据结构

| 结构 | 作用 | 关键字段/方法 |
|---|---|---|
| `LoopBlocksRPO` | 循环块的逆后序列表 | `perform(LI)` 计算 RPO |
| `ControlFlowHoister` | 管理条件分支复制与块映射 | `HoistDestinationMap`, `HoistableBranches` |
| `SinkAndHoistLICMFlags` | 传递优化 cap 参数 | `tooManyMemoryAccesses()` |
| `ICFLoopSafetyInfo` | 循环安全信息（speculation 边界） | `computeLoopSafetyInfo()` |
| `HoistedInstructions` | 记录已提升指令（用于 re-hoisting） | `vector<Instruction*>` |

---

### 优化意图

1. **单遍 hoisting**: 通过 RPO 遍历（定义先于使用），确保 sink 一次完成；hoist 也只需一次遍历
2. **控制流提升**: 通过复制分支和基本块，将循环内条件指令提升到对应路径，避免全部提升到 preheader 导致过度 speculation
3. **PHI 提升**: 将循环头 PHI 提前，减少循环内 PHI 数量，简化后续分析
4. **FP 优化**: 将 invariant 除法转为乘法，利用 FP 松弛特性
5. **重结合**: 将部分算术运算提前计算，减少循环内计算量
6. **MemorySSA 增量更新**: 所有 IR 修改通过 `MSSAU` 维护一致性

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| LCSSA 形式 | `assert(L->isLCSSAForm(*DT))` (行 424) | LICM 修改 loop boundary 可能破坏 LCSSA |
| 支配关系 | hoist 后必须 dominate 所有 use | 否则需 re-hoisting (行 1024-1044) |
| 内存别名 | 通过 `canSinkOrHoistInst` + `noConflictingReadWrites` 检查 | 别名冲突可能导致错误提升 |
| speculation 安全 | `isSafeToExecuteUnconditionally` 判断 | 可能导致运行时异常（如除零） |
| 子循环跳过 | `inSubLoop` 避免重复处理 | 否则重复提升，代码膨胀 |
| preheader 存在 | `Preheader && L->hasDedicatedExits()` | 否则无法插入 hoisted 代码 |
| MemorySSA 一致性 | 所有修改通过 `MSSAU` | 直接操作 IR 而不更新 MSSA 会导致后续分析错误 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 循环块 RPO 计算 | `LoopBlocksRPO::perform(LoopInfo)` | llvm/lib/Transforms/Utils/LoopUtils.h |
| 子循环判断 | `inSubLoop(BB, CurLoop, LI)` | 行 176（静态函数） |
| 指令是否死代码 | `isInstructionTriviallyDead(I, TLI)` | llvm/lib/Transforms/Utils/Local.h |
| 可移动性检查 | `canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, ...)` | llvm/lib/Transforms/Utils/LoopUtils.h |
| 无条件执行安全 | `isSafeToExecuteUnconditionally(...)` | 行 188-192（声明） |
| 实际提升 | `hoist(I, DT, CurLoop, Dest, SafetyInfo, MSSAU, SE, ORE)` | 行 181-184 声明，需查看其定义 |
| 算术重结合 | `hoistArithmetics(I, L, SafetyInfo, MSSAU, AC, DT)` | 行 203-206 声明 |
| ControlFlowHoister | `getOrCreateHoistedBlock(BB)` | 行 769-880 |
| PHI 可提升性 | `ControlFlowHoister::canHoistPHI(PN)` | 行 732-767 |
| MemorySSA 验证 | `MSSA->verifyMemorySSA()` | llvm/lib/Analysis/MemorySSA.cpp |

**hoist() 函数位置**: 未在当前文件显示，应在同一文件的下文部分（使用 offset 继续读取）

---

### 其他补充

- **统计项**: `NumHoisted`, `NumCreatedBlocks`, `NumClonedBranches` 等记录优化效果
- **命令行选项**: 
  - `-licm-control-flow-hoisting` (默认 false) 控制是否复制控制流
  - `-licm-mssa-optimization-cap` (默认 100) 控制 MemorySSA 查询精度/性能权衡
  - `-licm-mssa-max-acc-promotion` (默认 250) 控制内存提升的门限
- **与 sinkRegion 关系**: LICM 先 `sinkRegion` 后 `hoistRegion`，两者使用相同遍历模式但方向相反（sink 用 reverse RPO，hoist 用 RPO）

---

## 函数分析：`isSafeToExecuteUnconditionally()`（行 1729-1753）

### 函数签名与目的（行 1729-1753）

```cpp
static bool isSafeToExecuteUnconditionally(
    Instruction &Inst, const DominatorTree *DT, const TargetLibraryInfo *TLI,
    const Loop *CurLoop, const LoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE, const Instruction *CtxI,
    AssumptionCache *AC, bool AllowSpeculation)
```

**功能**: 判断是否安全地将指令无条件执行（即在循环外/预头块中执行），主要用于 LICM 的 hoisting 和 sinking 决策。

---

### 整体结构

```text
isSafeToExecuteUnconditionally(Inst, DT, TLI, CurLoop, SafetyInfo, ORE, CtxI, AC, AllowSpeculation)
├── Step 1: Speculative Execution Check
│   └── 如果允许推测执行且安全 → 返回 true
│       调用：isSafeToSpeculativelyExecute
├── Step 2: Guaranteed-to-Execute Check
│   ├── 检查指令是否在循环内 guaranteed to execute
│   │   调用：SafetyInfo->isGuaranteedToExecute
│   └── 如果不是 guaranteed to execute
│       ├── 如果是 LoadInst 且地址 loop invariant
│       │   └── 发出 OptimizationRemarkMissed
│       └── 返回 false
└── Step 3: 返回结果
    └── GuaranteedToExecute 的值
```

---

### 逐段注释

#### 1. 推测执行检查 (行 1734-1736)

```cpp
if (AllowSpeculation &&
    isSafeToSpeculativelyExecute(&Inst, CtxI, AC, DT, TLI))
  return true;
```

**目的作用**: 
- 首先检查是否允许推测执行 (`AllowSpeculation` flag)
- 如果允许，调用 `isSafeToSpeculativelyExecute` 验证指令是否可以安全地推测执行
- 如果可以推测执行，直接返回 true，无需后续检查

**关键点**:
- 这是一个快速路径优化，避免进入更复杂的检查
- 推测执行通常针对可能 trap 的指令（如 load、div）
- 需要 AC(假设缓存)、DT(支配树)、TLI(目标库信息) 来验证安全性

---

#### 2. 确认执行安全检查 (行 1738-1750)

**代码片段**:
```cpp
bool GuaranteedToExecute =
    SafetyInfo->isGuaranteedToExecute(Inst, DT, CurLoop);

if (!GuaranteedToExecute) {
  auto *LI = dyn_cast<LoadInst>(&Inst);
  if (LI && CurLoop->isLoopInvariant(LI->getPointerOperand()))
    ORE->emit([&]() {
      return OptimizationRemarkMissed(
                 DEBUG_TYPE, "LoadWithLoopInvariantAddressCondExecuted", LI)
             << "failed to hoist load with loop-invariant address "
                "because load is conditionally executed";
    });
}
```

**目的作用**:
- **核心检查**: 通过 `SafetyInfo->isGuaranteedToExecute` 判断指令是否在循环的每个可执行路径上都会被执行
- `SafetyInfo` 是 LICM 在 `runOnLoop` 中预先计算的循环安全信息
- `isGuaranteedToExecute` 基于支配树分析确定指令是否被条件分支保护

**非 guaranteed 时的特殊处理**:
- 如果指令不是 guaranteed to execute，特别检查是否为 `LoadInst`
- 如果 load 的地址是 loop invariant（但执行本身有條件），发出 remark 记录优化未生效的原因
- Remark ID: `"LoadWithLoopInvariantAddressCondExecuted"`

**调试意义**:
- 这条信息帮助开发者理解为什么某些循环不变 load 没有被 hoist
- 常见于条件执行的路径上的内存访问

---

#### 3. 返回最终结果 (行 1752)

**代码片段**:
```cpp
return GuaranteedToExecute;
```

**目的作用**:
- 直接返回步骤 2 中计算的结果
- 只有当指令 guaranteed to execute 时才返回 true

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|------|------|------|
| `LoopSafetyInfo` | - | 存储循环安全信息，包括哪些指令 guaranteed to execute |
| `DominatorTree` | - | 用于支配关系分析和执行路径判断 |
| `Instruction` | - | 待检查的指令对象 |
| `AssumptionCache` | - | 包含假设信息，辅助推断指针有效性等 |

---

### 优化意图

**设计动机**:

1. **防止 Trap 异常**: 确保在循环外执行指令不会引入新的 trap（例如访问无效指针）
   - 推测执行允许对无副作用的指令做乐观推理
   - 保证执行则要求严格的路径可达性分析

2. **保持语义等价性**: LICM 的核心原则是不改变程序语义
   - 移动到预头块意味着该指令会在循环前始终执行一次
   - 对于原循环内可能不执行的指令，必须谨慎对待

3. **保留编译器提示能力**: 通过 ORE 输出 missed optimization 信息
   - 即使无法 hoist，也能告知开发者失败原因
   - 便于源码级调整（如添加 restrict、重构控制流）

**权衡考虑**:

- **保守 vs 激进**: `AllowSpeculation` 参数控制策略激进程度
  - `true`: 允许更多优化，但依赖假设缓存的准确性
  - `false`: 仅移动 guaranteed to execute 的指令

- **精度 vs 开销**: `SafetyInfo->isGuaranteedToExecute` 的计算成本较高
  - 在 `runOnLoop` 中一次性计算供多次使用
  - 避免每次 hoist 时重复分析

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|------|------|------|
| `AllowSpeculation` 为 false | 此时跳过推测执行检查，强制要求 guaranteed to execute | 错过一些 hoisting 机会 |
| `SafetyInfo` 已失效 | LICM 修改 IR 后需重新计算安全信息 | 可能导致错误的 hoisting 决策 |
| Load 条件执行 | 即使地址 invariant，若执行有条件也不能 hoist | 正确性保护机制，不可绕过 |
| Context 指令 CtxI | 通常是 Preheader 的 terminator，作为 hoisting 的目标上下文 | 需提供有效的上下界 |

**关键前提条件**:

```cpp
// 调用链位置参考 (hoistRegion 行 930):
if (CurLoop->hasLoopInvariantOperands(&I) &&
    canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, true, Flags, ORE) &&
    isSafeToExecuteUnconditionally(...)) {
  // 三个检查缺一不可:
  // 1. 操作数 loop invariant
  // 2. alias 安全 + 机械能力满足
  // 3. 执行无条件安全 ← 本函数负责
}
```

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 推测执行检查 | `isSafeToSpeculativelyExecute` | `llvm/include/llvm/Transforms/Utils/Local.h` |
| 保证执行检查 | `SafetyInfo->isGuaranteedToExecute` | `llvm/lib/Transforms/Scalar/LICM.cpp:2128+` (LICMSafetyInfo) |
| 循环不变性判断 | `CurLoop->isLoopInvariant` | `llvm/include/llvm/Analysis/LoopInfo.h` |
| Optimization 记录 | `ORE->emit` | `llvm/include/llvm/Analysis/OptimizationRemarkEmitter.h` |
| 指令类型转换 | `dyn_cast<LoadInst>` | `llvm/include/llvm/IR/Instructions.h` |

**使用示例**:
```cpp
// 在 LICM::runOnLoop 中，先计算 SafetyInfo:
ICFLoopSafetyInfo SafetyInfo;
SafetyInfo.computeLoopSafetyInfo(L);

// 在 hoistRegion 中对每个候选指令检查:
if (isSafeToExecuteUnconditionally(I, DT, TLI, CurLoop, 
                                    &SafetyInfo, ORE, 
                                    Preheader->getTerminator(), AC,
                                    LicmAllowSpeculation)) {
  // 安全 hoist
}
```

---

### 关联函数

| 函数 | 关系 |
|------|------|
| `canSinkOrHoistInst` (行 1165+) | 检查指令是否有能力 hoist/sink，但不验证执行安全性 |
| `isSafeToSpeculativelyExecute` | 实际实现推测执行的安全检查逻辑 |
| `hoist` (行 1682+) | 通过本函数确认安全后才调用 |
| `sink` (行 1576+) | 同样需要先通过本函数验证 |

**典型调用链**:
```text
hoistRegion
  → 遍历每个指令
    → canSinkOrHoistInst (别名分析 + 机械能力)
    → isSafeToExecuteUnconditionally (执行安全性) ← 当前函数
      → isSafeToSpeculativelyExecute (推测路径)
      → SafetyInfo->isGuaranteedToExecute (保证执行路径)
    → hoist (实际移动)
```

---

### 其他补充

**版本差异**:
- NewPM(LLVM 17+) 通过 `Opts.AllowSpeculation` 传递此标志
- Legacy PM 在 `LegacyLICMPass` 构造函数中默认初始化为 `true`

**性能考量**:
- `isGuaranteedToExecute` 的计算在 `runOnLoop` 开始时完成
- 整个 pass 中对每个指令只调用一次，复杂度 O(N)

**调试建议**:
```bash
opt -load-pass-plugin=libclicm.so -passes="licm" \
    -debug-only=licm -S input.ll 2>&1 | grep -A2 -B2 "conditional"
```

---


## 函数分析：`hoist`（行 1682-1724）

### 函数签名与目的（行 1682-1685）

```cpp
static void hoist(Instruction &I, const DominatorTree *DT, const Loop *CurLoop,
                   BasicBlock *Dest, ICFLoopSafetyInfo *SafetyInfo,
                   MemorySSAUpdater &MSSAU, ScalarEvolution *SE,
                   OptimizationRemarkEmitter *ORE);
```

**功能**: 将指令 I 从循环体提升到目标块 Dest（通常是循环前驱块），执行必要的清理和更新工作。

---

### 整体结构

```
hoist(Instruction &I, ...)
├── 发出优化备注
├── 处理元数据和调用属性
│   ├── 如果指令有元数据且非必然执行，删除 UB 属性和元数据
└── 移动指令到目标位置
    ├── 如果是 PHI 节点，移到目标块 PHI 列表末尾
    └── 否则，移到目标块终止符之前
└── 更新调试信息位置
└── 更新统计信息
```

---

### 逐段注释

**1. 发出优化备注（行 1686-1691）**

```cpp
LLVM_DEBUG(dbgs() << "LICM hoisting to " << Dest->getNameOrAsOperand() << ": "
                  << I << "\n");
ORE->emit([&]() {
  return OptimizationRemark(DEBUG_TYPE, "Hoisted", &I) << "hoisting "
                                                         << ore::NV("Inst", &I);
});
```

目的作用：
- 记录提升操作的调试信息
- 发出优化备注，供用户查看哪些指令被提升

**2. 处理元数据和调用属性（行 169.3-1707）**

```cpp
if ((I.hasMetadataOtherThanDebugLoc() || isa<CallInst>(I)) &&
      !SafetyInfo->isGuaranteedToExecute(I, DT, CurLoop)) {
  I.dropUBImplyingAttrsAndMetadata();
}
```

目的作用：
- 当指令有元数据或为调用指令，且在循环中非必然执行时
- 删除可能导致可能导致未定义行为的属性和元数据
- 原因：提升到循环前驱块后，这些元数据可能不再有效

**3. 移动指令到目标位置（行 1709-1715）**

```cpp
if (isa<PHINode>(I))
  moveInstructionBefore(I, Dest->getFirstNonPHIIt(), *SafetyInfo, MSSAU, SE);
else
  moveInstructionBefore(I, Dest->getTerminator()->getIterator(), *SafetyInfo,
                        MSSAU, SE);
```

目的作用：
- 将指令 I 移动到目标块 Dest
- PHI 节点移到目标块的 PHI 列表末尾（在非 PHI 指令之前）
- 其他指令移到目标块终止符之前

**4. 更新调试信息（行 1717）**

```cpp
I.updateLocationAfterHoist();
```

目的作用：
- 更新指令的调试信息位置，反映其新的位置

**5. 更新统计信息（行 1719-1723）**

```cpp
if (isa<LoadInst>(I))
  ++NumMovedLoads;
else if (isa<CallInst>(I))
  ++NumMovedCalls;
++NumHoisted;
```

目的作用：
- 更新 LICM 统计信息，记录提升的指令数量

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| Instruction | - | 被提升的指令 |
| DominatorTree | - | 支配树，用于判断指令是否必然执行 |
| Loop | - | 当前循环 |
| BasicBlock | - | 目标块（通常是循环前驱块） |
| ICFLoopSafetyInfo | - | 循环安全信息，用于判断指令是否必然执行 |
| MemorySSAUpdater | - | MemorySSA 更新器，用于更新内存 SSA |
| ScalarEvolution | - | 标量演化，用于遗忘循环信息 |
| OptimizationRemarkEmitter | - | 优化备注发射器，用于发出优化备注 |

---

### 优化意图

1. 减少循环体中的计算量，提高性能
2. 将循环不变计算移出循环，避免重复计算
3. 为后续优化创造机会（如常量传播、死代码消除）

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 指令必须安全提升 | 指令不能有副作用，提升后不会改变程序语义 | 错误提升可能导致程序行为改变 |
| 元数据可能失效 | 提升后某些元数据可能不再有效 | 需要删除可能导致 UB 的元数据 |
| PHI 节点位置特殊 | PHI 节点必须放在目标块 PHI 列表末尾 | 错误放置可能导致 IR 不合法 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 判断指令是否必然执行 | `isGuaranteedToExecute` | `llvm/Analysis/MustExecute.h` |
| 移动指令 | `moveInstructionBefore` | `llvm/lib/Transforms/Scalar/LICM.cpp:1456` |
| 更新调试信息 | `updateLocationAfterHoist` | `llvm/IR/Instructions.h` |
| 删除 UB 属性和元数据 | `dropUBImplyingAttrsAndMetadata` | `llvm/IR/Instructions.h` |

**使用示例**：
- 在 `hoistRegion` 函数中调用 `hoist` 提升指令（行 933）
- 在处理 FDiv 时调用 `hoist` 提升倒数除数（行 961）
- 在处理 invariant.start 或 guard 时调用 `hoist`（行 980）

---

### 其他补充

1. `hoist` 函数是 LICM Pass 的核心操作之一，负责将指令从循环体提升到循环前驱
2. 函数内部调用了 `moveInstructionBefore` 来实际移动指令，并更新相关分析结果
3. 函数会处理元数据和调用属性，确保提升后的指令不会导致未定义行为
4. 函数会更新统计信息，用于性能分析和调试

---

## 函数分析：`hoistArithmetics`（行 203-206）

### 函数签名与目的（行 203-206）

```cpp
static bool hoistArithmetics(Instruction &I, Loop &L,
                              ICFLoopSafetyInfo &SafetyInfo,
                              MemorySSAUpdater &MSSAU, AssumptionCache *AC,
                              DominatorTree *DT);
```

**功能**: 尝试通过重关联表达式将部分不变计算提取出循环，提升到循环前驱块。支持 min/max、GEP、add/sub、浮点数和整数运算等多种模式。

---

### 整体结构

```
hoistArithmetics(I, L, SafetyInfo, MSSAU, AC, DT)
├── 尝试 hoistMinMax（选择指令）
│   └── 尝试 hoistGEP（GEP 重关联）
│   └── 尝试 hoistAdd / hoistSub（加减重关联）
│   └── 尝试 hoistFPAssociation（浮点数重关联）
│   └── 尝试 hoistBOAssociation（整数/浮点数通用重关联）
└── 返回是否发生变换
```

---

### 逐段注释

**1. hoistMinMax（选择指令）**

```cpp
if (hoistMinMax(I, L, SafetyInfo, MSSAU, AC, DT))
  return true;
```

目的作用：
- 尝试将 `select` 指令形式的 min/max 表达式提升到循环外
- 例如：`(A < C1) && (A < C2)` → `A < min(C1, C2)`

**2. hoistGEP（GEP 重关联）**

```cpp
if (hoistGEP(I, L, SafetyInfo, MSSAU, AC, DT))
  return true;
```

目的作用：
- 尝试将 GEP（GetElementPtr）指令重关联，把不变的索引层移到外层，允许外层 GEP 被提升
- 例如：`gep(gep(ptr, idx1_variant), idx2_invariant)` → `gep(gep(ptr, idx2_invariant), idx1_variant)`

**3. hoistAdd / hoistSub（加减重关联）**

```cpp
if (hoistAdd(Pred, LHS, RHS, cast<ICmpInst>(I), L, SafetyInfo, MSSAU, AC, DT))
  return true;
if (hoistSub(Pred, LHS, RHS, cast<ICmpInst>(I), L, SafetyInfo, MSSAU, AC, DT))
  return true;
```

目的作用：
- 尝试将加减运算重关联，将不变量的加减移到比较的另一侧
- 例如：`(LV + C1) < C2` → `LV < C2 - C1`

**4. hoistFPAssociation（浮点数重关联）**

```cpp
if (hoistFPAssociation(I, L, SafetyInfo, MSSAU, AC, DT))
  return true;
```

目的作用：
- 尝试浮点数重关联，受 `AllowFPReassoc` 标志控制
- 有 `FPAssociationUpperLimit` 限制单轮变换次数（默认 5）

**5. hoistBOAssociation（整数/浮点数通用重关联）**

```cpp
if (hoistBOAssociation(I, L, SafetyInfo, MSSAU, AC, DT))
  return true;
```

目的作用：
- 尝试整数和浮点数通用重关联，受 `IntAssociationUpperLimit` 限制单轮变换次数（默认 5）

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| Instruction | - | 被试重关联的指令 |
| Loop | - | 当前循环 |
| ICFLoopSafetyInfo | - | 循环安全信息 |
| MemorySSAUpdater | - | MemorySSA 更新器 |
| AssumptionCache | - | 假设缓存 |
| DominatorTree | - | 支配树 |

---

### 优化意图

1. **重关联提升不变量**：通过重新组织表达式的结合方式，将部分不变的计算提取到循环外
2. **减少循环内计算量**：不变量提升后，循环内只需执行一次计算
3. **为后续优化创造机会**：提升后的不变量可能被常量传播、死代码消除等优化利用

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 结合律结合律不满足 | 重关联可能改变计算结果，需要确保 flags 正确 | 计算错误 |
| 浮点溢出 | 浮点运算可能溢出，需要检查 nsw/nuw flags | 溢出风险 |
| Poison 传播 | 重关联可能引入 poison，需要正确处理 | 语义风险 |
| 浮序依赖 | 重关联改变操作数顺序，可能改变程序行为 | 正确性风险 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 判断指令类型 | `isa<SelectInst>()`, `isa<ICmpInst>()` 等 | `llvm/IR/Instructions.h` |
| 检查操作数不变性 | `L->isLoopInvariant()` | `llvm/Analysis/LoopInfo.h` |
| 创建新指令 | `BinaryOperator::Create*()` | `llvm/IR/Instructions.h` |
| 更新指令 | `I.replaceAllUsesWith()` | `llvm/IR/Instruction.h` |
| 删除旧指令 | `eraseInstruction()` | `LICM.cpp` 内部 |
| 判断是否安全执行 | `isSafeToExecuteUnconditionally()` | `LICM.cpp` 内部 |
| 更新指令位置 | `moveInstructionBefore()` | `LICM.cpp` 内部 |

**使用示例**：
- 在 `hoistRegion` 函数中调用 `hoistArithmetics(I, ...)`（行 1004）
- 在 `hoistRegion` 函数中对每个指令尝试重关联提升（行 1002-1007）

---

### 其他补充

1. **变换次数限制**：`FPAssociationUpperLimit` 和 `IntAssociationUpperLimit` 限制单轮变换次数，防止编译时爆炸
2. **递归调用**：每个 helper 函数可能递归调用自身，形成级联提升（如 `hoistMinMax` → `hoistGEP` → `hoistAdd` → `hoistFPAssociation`）
3. **变换成功返回**：只要任一 helper 成功即返回 true，允许后续继续尝试其他模式

---

## 补充分析：`PredIteratorCache` 在 LICM Promotion 中的作用与效率价值

`PredIteratorCache` 是一个非常轻量的 **BasicBlock 前驱列表缓存**，定义在 `llvm/include/llvm/IR/PredIteratorCache.h:24`。它内部用 `DenseMap<BasicBlock *, ArrayRef<BasicBlock *>>` 记录某个 block 的前驱列表，并在第一次查询时把 `predecessors(BB)` 的结果物化到 `BumpPtrAllocator` 中保存；后续对同一个 `BB` 的查询直接复用缓存数组，而不是重新沿 CFG 的 use-list 遍历。

**它解决的问题**：LICM 的 memory promotion 会对同一批 loop exit blocks 反复插入 LCSSA PHI 和 store。如果每次都直接调用 `predecessors(BB)`，那么同一个 exit block 的前驱列表会被重复扫描多次；`PredIteratorCache` 把这部分重复工作折叠为“第一次收集，后续复用”。

### 在 LICM 中的调用链

```cpp
LoopInvariantCodeMotion::runOnLoop()              // LICM.cpp:447-535
  -> PredIteratorCache PIC;                       // LICM.cpp:508
  -> promoteLoopAccessesToScalars(..., PIC, ...) // LICM.cpp:518-521
     -> LoopPromoter Promoter(..., PIC, ...)     // LICM.cpp:2182-2186
        -> maybeInsertLCSSAPHI()                 // LICM.cpp:1775-1787
```

### 它具体用在什么场景

`LoopPromoter::insertStoresInLoopExitBlocks()` 会在每个 exit block 中插入写回 store。如果要写回的值或指针定义在 loop 内部，那么直接在 exit block 使用它会破坏 LCSSA，此时 `maybeInsertLCSSAPHI()` 会先在 exit block 头部插入一个 PHI：

```cpp
PHINode *PN = PHINode::Create(I->getType(), PredCache.size(BB),
                              I->getName() + ".lcssa");
for (BasicBlock *Pred : PredCache.get(BB))
  PN->addIncoming(I, Pred);
```

这里会同时用到：
- `PredCache.size(BB)`：提前给 PHI 预留 incoming 个数
- `PredCache.get(BB)`：遍历 exit block 的所有前驱并填充 incoming

也就是说，`PredIteratorCache` 在 LICM 中不是用于 hoist/sink 合法性判断，而是用于 **promotion 阶段构造 LCSSA PHI**。

### 为什么它比直接用 `predecessors(BB)` 更高效

- 它**不是**让“单次前驱查询”更快；第一次查询同样需要扫描前驱。
- 它的收益来自“**同一个 BasicBlock 被多次查询前驱**”的场景。
- 如果直接写成：

```cpp
unsigned NumPreds = std::distance(pred_begin(ExitBB), pred_end(ExitBB));
PHINode *PN = PHINode::Create(Ty, NumPreds, Name);
for (BasicBlock *Pred : predecessors(ExitBB))
  PN->addIncoming(V, Pred);
```

那么每造一个 PHI，通常至少要对 `ExitBB` 的前驱做两次遍历：一次数个数，一次数列表。对于 promotion 来说，同一轮 `do { ... } while (LocalPromoted)` 里会反复处理多个 must-alias 候选集合（`LICM.cpp:514-524`），这些候选往往共享同一批 exit blocks，因此不加缓存会反复重扫相同的前驱关系。

### 一个具体例子

假设 loop 有一个共享的 `exit` block，循环内部有三条不同路径都能跳到它：

```llvm
for.body:
  br i1 %c1, label %exit, label %cont1
cont1:
  br i1 %c2, label %exit, label %cont2
cont2:
  br i1 %c3, label %exit, label %latch
exit:
  ...
```

这时 `exit` 的前驱是 `for.body`、`cont1`、`cont2`。如果 LICM promotion 发现 5 组可提升的 must-alias 内存访问，并且其中多组都需要在 `exit` 中插入 LCSSA PHI：

- **直接用 `predecessors(exit)`**：每次插一个 PHI，都要重新数一遍前驱、再遍历一遍前驱
- **用 `PredIteratorCache`**：第一次 `PIC.get(exit)` 时把 `[for.body, cont1, cont2]` 缓存下来，后续 `PIC.size(exit)` / `PIC.get(exit)` 直接复用这份 `ArrayRef`

因此，`PredIteratorCache` 优化掉的是“对同一个 exit block 的重复 CFG 前驱扫描”，而不是 PHI 构造本身。

### 怎么使用以及边界条件

```cpp
PredIteratorCache PIC;

PHINode *PN = PHINode::Create(Ty, PIC.size(ExitBB), Name);
for (BasicBlock *Pred : PIC.get(ExitBB))
  PN->addIncoming(V, Pred);
```

- 适合在一个局部变换中多次查询同一批 blocks 的前驱列表
- 如果 CFG 边发生变化（例如 split block、改 branch successor、增删前驱），缓存内容可能过期，需要调用 `PIC.clear()` 失效重建；`PredIteratorCache.h:48-52`
- LICM 的 promotion 这段逻辑主要插入 `load/store/phi`，不改 exit block 的前驱关系，所以可以安全地在整轮 promotion 中复用同一个 `PIC`

---

## moveInstructionBefore 函数分析

### 函数签名与目的（行号 1456-1459）

```cpp
static void moveInstructionBefore(Instruction &I, BasicBlock::iterator Dest,
                                  ICFLoopSafetyInfo &SafetyInfo,
                                  MemorySSAUpdater &MSSAU,
                                  ScalarEvolution *SE)
```

**功能**: 将指令 `I` 移动到目标位置 `Dest`，同时更新 SafetyInfo、MemorySSA 和 SCEV 分析信息，保证 LICM 变换后分析结果的正确性。

---

### 整体结构

```
moveInstructionBefore(I, Dest, SafetyInfo, MSSAU, SE)
├── 从 SafetyInfo 移除指令 I
├── 将指令 I 插入到目标 BB 的 SafetyInfo
├── 执行 IR 层面的指令移动
├── 更新 MemorySSA 访问位置
└── 清除 SCEV 的块/循环 disposition 缓存
```

---

### 逐段注释

**1. SafetyInfo 状态转移 (行 1460-1461)**

```cpp
SafetyInfo.removeInstruction(&I);
SafetyInfo.insertInstructionTo(&I, Dest->getParent());
I.moveBefore(*Dest->getParent(), Dest);
```

- **目的**: 先从原位置移除指令的安全信息，再将指令标记为属于目标基本块
- **为何先移后插**: `ICFLoopSafetyInfo` 维护每个 BB 的指令集合，移动前后需要更新所属关系
- **关键点**: 必须在 IR 移动前更新 SafetyInfo，否则 `I` 的父 BB 信息可能不一致

**2. IR 层面指令移动 (行 1462)**

```cpp
I.moveBefore(*Dest->getParent(), Dest);
```

- **目的**: 将指令 `I` 从原位置移动到 `Dest` 指定的位置
- **API 说明**: `moveBefore(BasicBlock &BB, BasicBlock::iterator I)` 将指令插入到 BB 的 I 位置之前

**3. MemorySSA 更新 (行 1463-1466)**

```cpp
if (MemoryUseOrDef *OldMemAcc = cast_or_null<MemoryUseOrDef>(
        MSSAU.getMemorySSA()->getMemoryAccess(&I)))
  MSSAU.moveToPlace(OldMemAcc, Dest->getParent(),
                    MemorySSA::BeforeTerminator);
```

- **目的**: 如果指令涉及内存访问，更新其在 MemorySSA 中的位置
- **条件检查**: `cast_or_null` 返回 null 表示非内存指令，跳过 MSSA 更新
- **目标位置**: `BeforeTerminator` 表示插入到目标块终结指令之前

**4. SCEV 缓存清理 (行 1467-1468)**

```cpp
if (SE)
  SE->forgetBlockAndLoopDispositions(&I);
```

- **目的**: 使 SCEV 中与该指令相关的块/循环 disposition 信息失效
- **原因**: 指令位置变化可能影响循环分析结果（如 `isLoopInvariant` 判断）
- **可选性**: SE 参数可能为空，需判空

---

### 关键数据结构

| 结构 | 字段/接口 | 含义 |
|---|---|---|
| `ICFLoopSafetyInfo` | `removeInstruction()` | 从当前 BB 的指令集合中移除 |
| `ICFLoopSafetyInfo` | `insertInstructionTo()` | 将指令登记到目标 BB |
| `MemorySSAUpdater` | `moveToPlace()` | 移动 MemoryUse/Def 到新位置 |
| `ScalarEvolution` | `forgetBlockAndLoopDispositions()` | 清除缓存的分析结果 |

---

### 优化意图

1. **维护分析一致性**: LICM 将指令从循环内提升到循环外，必须同步更新 SafetyInfo、MemorySSA、SCEV 三类分析
2. **减少重建开销**: 通过增量更新而非全量重建，降低编译时成本
3. **支持后续变换**: 更新后的分析结果供后续 hoist/sink 决策使用

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| SafetyInfo 必须先更新 | 在 `I.moveBefore` 前调用 `removeInstruction`/`insertInstructionTo` | IR 移动后父 BB 信息可能不一致 |
| MSSA 更新仅限内存指令 | 非内存指令无 MemoryAccess，`getMemoryAccess` 返回 null | 无需处理 |
| SCEV 可为空 | 调用方可能不传入 SE | 需判空 |
| 目标位置必须在有效 BB | `Dest->getParent()` 必须有效 | 否则 UB |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 指令移动 | `Instruction::moveBefore()` | `llvm/IR/Instruction.h` |
| MemorySSA 更新 | `MemorySSAUpdater::moveToPlace()` | `llvm/Analysis/MemorySSAUpdater.h` |
| SCEV 缓存清理 | `ScalarEvolution::forgetBlockAndLoopDispositions()` | `llvm/Analysis/ScalarEvolution.h` |
| SafetyInfo 维护 | `ICFLoopSafetyInfo::removeInstruction()` | 本文件行号 214 声明 |

---

### 调用场景

该函数在 LICM 中被调用三次：

1. **行 1039**: Hoist（提升）阶段，将循环不变指令提升到循环前置块
2. **行 1711**: Sink（下沉）阶段，将指令下沉到出口块的首个非 PHI 位置
3. **行 1714**: Sink 阶段，将指令下沉到出口块的终结指令之前

---

## hoistMinMax 函数分析

### 函数签名与目的（行号 2438-2439）

```cpp
static bool hoistMinMax(Instruction &I, Loop &L, ICFLoopSafetyInfo &SafetyInfo,
                        MemorySSAUpdater &MSSAU)
```

**功能**: 尝试将形如 `(A < INV_1 AND A < INV_2)` 的条件简化为 `(A < min(INV_1, INV_2))`，其中 `INV_1` 和 `INV_2` 是循环不变量，`A` 是循环变量。通过在循环外计算 min/max，减少循环内的比较次数。

---

### 整体结构

```
hoistMinMax(I, L, SafetyInfo, MSSAU)
├── 步骤1: 匹配逻辑与/或指令，确定是否需要反转谓词
├── 步骤2: 提取两个 icmp 并验证模式（一个循环变量 vs 一个不变量）
├── 步骤3: 检查两个 icmp 是否可合并（相同 LHS，匹配的谓词）
├── 步骤4: 在循环前置块创建 min/max intrinsic
├── 步骤5: 替换原指令并清理
└── 返回 true/false
```

---

### 逐段注释

**1. 匹配逻辑与/或指令 (行 2440-2448)**

```cpp
bool Inverse = false;
using namespace PatternMatch;
Value *Cond1, *Cond2;
if (match(&I, m_LogicalOr(m_Value(Cond1), m_Value(Cond2)))) {
  Inverse = true;
} else if (match(&I, m_LogicalAnd(m_Value(Cond1), m_Value(Cond2)))) {
  // Do nothing
} else
  return false;
```

- 目的：识别输入指令是否为逻辑与（AND）或逻辑或（OR）
- 对于 `OR`，后续需要对谓词取反
- `m_LogicalAnd` / `m_LogicalOr` 同时匹配 `and i1` 和 `select i1` 形式

**2. 提取并验证 icmp 模式 (行 2450-2472)**

```cpp
auto MatchICmpAgainstInvariant = [&](Value *C, CmpPredicate &P, Value *&LHS,
                                     Value *&RHS) {
  if (!match(C, m_OneUse(m_ICmp(P, m_Value(LHS), m_Value(RHS)))))
    return false;
  if (!LHS->getType()->isIntegerTy())
    return false;
  if (!ICmpInst::isRelational(P))
    return false;
  if (L.isLoopInvariant(LHS)) {
    std::swap(LHS, RHS);
    P = ICmpInst::getSwappedPredicate(P);
  }
  if (L.isLoopInvariant(LHS) || !L.isLoopInvariant(RHS))
    return false;
  if (Inverse)
    P = ICmpInst::getInversePredicate(P);
  return true;
};
CmpPredicate P1, P2;
Value *LHS1, *LHS2, *RHS1, *RHS2;
if (!MatchICmpAgainstInvariant(Cond1, P1, LHS1, RHS1) ||
    !MatchICmpAgainstInvariant(Cond2, P2, LHS2, RHS2))
  return false;
```

- 目的：验证每个条件都是 `icmp`，且形式为"循环变量 op 循环不变量"
- 关键约束：
  - 必须是整数类型的 icmp
  - 必须是关系谓词（`<`, `<=`, `>`, `>=`），不支持 `==`/`!=`
  - 必须满足"一个操作数循环不变，另一个循环变化"
  - `m_OneUse` 限制确保 icmp 只有这一个使用者，变换后可安全删除

**3. 检查可合并性 (行 2473-2475)**

```cpp
auto MatchingPred = CmpPredicate::getMatching(P1, P2);
if (!MatchingPred || LHS1 != LHS2)
  return false;
```

- 目的：确保两个 icmp 可合并
- 条件：
  - `LHS1 == LHS2`：两个比较必须是同一个循环变量
  - `getMatching(P1, P2)` 返回有效谓词：例如 `<` 和 `<` 匹配，`<` 和 `>` 不匹配

**4. 创建 min/max intrinsic (行 2478-2504)**

```cpp
bool UseMin = ICmpInst::isLT(*MatchingPred) || ICmpInst::isLE(*MatchingPred);
Intrinsic::ID id = ICmpInst::isSigned(*MatchingPred)
                       ? (UseMin ? Intrinsic::smin : Intrinsic::smax)
                       : (UseMin ? Intrinsic::umin : Intrinsic::umax);
auto *Preheader = L.getLoopPreheader();
IRBuilder<> Builder(Preheader->getTerminator());
if (isa<SelectInst>(I))
  RHS2 = Builder.CreateFreeze(RHS2, RHS2->getName() + ".fr");
Value *NewRHS = Builder.CreateBinaryIntrinsic(
    id, RHS1, RHS2, nullptr,
    StringRef("invariant.") +
        (ICmpInst::isSigned(*MatchingPred) ? "s" : "u") +
        (UseMin ? "min" : "max"));
Builder.SetInsertPoint(&I);
ICmpInst::Predicate P = *MatchingPred;
if (Inverse)
  P = ICmpInst::getInversePredicate(P);
Value *NewCond = Builder.CreateICmp(P, LHS1, NewRHS);
```

- 目的：在循环前置块计算 min/max，循环内只做一次比较
- 谓词映射：
  - `<` / `<=` → 使用 `min`
  - `>` / `>=` → 使用 `max`
  - 有符号 → `smin`/`smax`，无符号 → `umin`/`umax`
- **Freeze 处理**（行 2493-2494）：如果是 `select` 形式的逻辑操作，`RHS2` 可能是 poison（短路求值时未使用的分支），需要 freeze

**5. 替换与清理 (行 2505-2514)**

```cpp
NewCond->takeName(&I);
I.replaceAllUsesWith(NewCond);
eraseInstruction(I, SafetyInfo, MSSAU);
Instruction &CondI1 = *cast<Instruction>(Cond1);
Instruction &CondI2 = *cast<Instruction>(Cond2);
salvageDebugInfo(CondI1);
salvageDebugInfo(CondI2);
eraseInstruction(CondI1, SafetyInfo, MSSAU);
eraseInstruction(CondI2, SafetyInfo, MSSAU);
return true;
```

- 目的：替换原指令并清理死代码
- 保留 debug 信息后删除原逻辑指令和两个 icmp

---

### 关键数据结构

| 结构 | 字段/接口 | 含义 |
|---|---|---|
| `PatternMatch::m_LogicalAnd` | 匹配器 | 匹配 `and i1` 或 `select i1, i1 true, i1 false` 形式 |
| `PatternMatch::m_LogicalOr` | 匹配器 | 匹配 `or i1` 或 `select i1, i1 false, i1 true` 形式 |
| `CmpPredicate` | `getMatching()` | 判断两个谓词是否可合并为同一方向 |
| `ICmpInst` | `isRelational()` | 判断是否为 `<`, `<=`, `>`, `>=` |
| `Intrinsic` | `smin/smax/umin/umax` | LLVM 内联函数 ID |

---

### 优化意图

1. **减少循环内比较次数**：将两次 icmp + 一次逻辑运算 → 一次 icmp
2. **提升不变计算**：min/max 计算提到循环外，每轮循环复用结果
3. **支持短路求值形式**：通过 `m_LogicalAnd/m_LogicalOr` 兼容 `select` 实现的短路求值

**为什么这样优化**：
- 循环内条件判断越少，分支预测压力越低
- min/max 通常可映射到单条指令（如 x86 的 `pminsd`）
- 对于 N 次循环，从 2N 次 icmp → N 次 icmp + 1 次 min/max

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| icmp 必须是 one-use | 确保 icmp 没有其他使用者 | 删除后导致 use-after-free |
| LHS 必须相同 | 两个比较必须是同一个变量 | 否则无法合并 |
| 谓词方向必须一致 | `<` 和 `>` 不能合并 | 无意义变换 |
| select 形式的 poison | 短路求值中未使用的分支可能是 poison | 需要 freeze |
| 整数类型限制 | 不支持浮点 | 浮点 min/max 语义不同 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 模式匹配 | `PatternMatch::match()` | `llvm/IR/PatternMatch.h` |
| 逻辑操作匹配 | `m_LogicalAnd`, `m_LogicalOr` | `llvm/IR/PatternMatch.h` |
| icmp 匹配 | `m_ICmp()` | `llvm/IR/PatternMatch.h` |
| 谓词操作 | `ICmpInst::getSwappedPredicate()` | `llvm/IR/InstrTypes.h` |
| 创建 intrinsic | `IRBuilder::CreateBinaryIntrinsic()` | `llvm/IR/IRBuilder.h` |
| freeze 创建 | `IRBuilder::CreateFreeze()` | `llvm/IR/IRBuilder.h` |

---

### 示例场景

#### C 语言示例

```c
// 场景：循环内的边界检查优化
// 原始代码：检查 i 是否在某个动态上界内
int clamp(int *arr, int n, int lower, int upper) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        // 两个上界比较，可合并为 i < min(lower, upper)
        if (i < lower && i < upper) {
            sum += arr[i];
        }
    }
    return sum;
}
```

#### 变换前的 IR

```llvm
define i32 @clamp(ptr %arr, i32 %n, i32 %lower, i32 %upper) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop.end ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop.end ]
  ; 两次 icmp
  %cmp1 = icmp slt i32 %i, %lower    ; i < lower
  %cmp2 = icmp slt i32 %i, %upper    ; i < upper
  ; 逻辑与
  %and = and i1 %cmp1, %cmp2         ; 两个条件都满足
  br i1 %and, label %body, label %loop.end
body:
  %gep = getelementptr i32, ptr %arr, i32 %i
  %val = load i32, ptr %gep
  %sum.next = add i32 %sum, %val
  br label %loop.end
loop.end:
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %sum
}
```

#### 变换后的 IR

```llvm
define i32 @clamp(ptr %arr, i32 %n, i32 %lower, i32 %upper) {
entry:
  ; min/max 计算提到循环外
  %invariant.smin = call i32 @llvm.smin.i32(i32 %lower, i32 %upper)
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop.end ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop.end ]
  ; 只需一次 icmp
  %cmp = icmp slt i32 %i, %invariant.smin  ; i < min(lower, upper)
  br i1 %cmp, label %body, label %loop.end
body:
  %gep = getelementptr i32, ptr %arr, i32 %i
  %val = load i32, ptr %gep
  %sum.next = add i32 %sum, %val
  br label %loop.end
loop.end:
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %sum
}

; LLVM intrinsic 声明
declare i32 @llvm.smin.i32(i32, i32)
```

#### 变换效果

| 指标 | 变换前 | 变换后 |
|---|---|---|
| 循环内 icmp | 2 次 | 1 次 |
| 循环内逻辑运算 | 1 次 and | 0 次 |
| 循环外计算 | 无 | 1 次 smin |
| N 次循环总 icmp | 2N | N + 1 |

---

## 函数分析：`hoistGEP`（行 2519-2573）

### 函数签名与目的（行 2519-2524）

```cpp
static bool hoistGEP(Instruction &I, Loop &L, ICFLoopSafetyInfo &SafetyInfo,
                      MemorySSAUpdater &MSSAU, AssumptionCache *AC,
                      DominatorTree *DT)
```

**功能**：通过重新关联嵌套 GEP 的索引层，将不变量的索引层移到外层，从而允许外层 GEP 被提升到循环外。核心变换是 `gep(gep(ptr, idx1_variant), idx2_invariant)` → `gep(gep(ptr, idx2_invariant), idx1_variant)`，使得外层 GEP（使用不变索引）可以 hoist 到 preheader。

---

### 整体结构

```
hoistGEP(I, L, SafetyInfo, MSSAU, AC, DT)
├── 1. 类型检查：I 是否为 GEP
├── 2. 常量索引检查：排除全常量 GEP
├── 3. 嵌套 GEP 检查：指针操作数必须是循环内的单用 GEP
├── 4. 不变性验证：基指针不变 + 外层 GEP 的所有索引不变
├── 5. 早退检查：内层 GEP 索引不能全不变（否则标准 LICM 已处理）
├── 6. inbounds 条件计算：两个 GEP 均 inbounds + 所有偏移非负
├── 7. 创建重关联 GEP 链
│   ├── 在 preheader 创建 NewSrc = gep(SrcPtr, GEP.indices)   // 不变部分
│   ├── 在原位置创建 NewGEP = gep(NewSrc, Src.indices)         // 变部分
│   ├── GEP.replaceAllUsesWith(NewGEP)
│   └── 删除原 GEP 和 Src GEP
└── 返回 true
```

---

### 逐段注释

**1. 类型检查与常量索引排除（行 2522-2530）**

```cpp
auto *GEP = dyn_cast<GetElementPtrInst>(&I);
if (!GEP)
  return false;

// Do not try to hoist a constant GEP out of the loop via reassociation.
// Constant GEPs can often be folded into addressing modes, and reassociating
// them may inhibit CSE of a common base.
if (GEP->hasAllConstantIndices())
  return false;
```

- `dyn_cast<GetElementPtrInst>`：只处理 GEP 指令，其他指令直接返回 false
- `hasAllConstantIndices()`：全常量索引的 GEP 通常能被后端折叠进寻址模式（如 `[base + offset]`），重关联反而可能抑制公共基址的 CSE

**2. 嵌套 GEP 与单用检查（行 2532-2534）**

```cpp
auto *Src = dyn_cast<GetElementPtrInst>(GEP->getPointerOperand());
if (!Src || !Src->hasOneUse() || !L.contains(Src))
  return false;
```

三个条件缺一不可：
- 指针操作数必须是另一个 GEP（嵌套 GEP 模式）
- `Src->hasOneUse()`：内层 GEP 只能被当前 GEP 使用，确保重关联后不会遗漏其他用户
- `L.contains(Src)`：内层 GEP 必须在循环内（否则不需要重关联）

**3. 不变性验证（行 2536-2539）**

```cpp
Value *SrcPtr = Src->getPointerOperand();
auto LoopInvariant = [&](Value *V) { return L.isLoopInvariant(V); };
if (!L.isLoopInvariant(SrcPtr) || !all_of(GEP->indices(), LoopInvariant))
  return false;
```

变换的前提条件：
- 最外层基指针 `SrcPtr` 必须循环不变
- 外层 GEP 的所有索引必须循环不变（这些是要提升到 preheader 的部分）

**4. 早退：内层 GEP 索引全不变（行 2541-2546）**

```cpp
// This can only happen if !AllowSpeculation, otherwise this would already be
// handled.
// FIXME: Should we respect AllowSpeculation in these reassociation folds?
// The flag exists to prevent metadata dropping, which is not relevant here.
if (all_of(Src->indices(), LoopInvariant))
  return false;
```

如果内层 GEP 的所有索引也不变，那么 `Src` 本身就是标准 LICM 的 hoist 候选，不需要重关联。到达这里说明 `Src` 至少有一个变索引。FIXME 指出当前未考虑 `AllowSpeculation` 标志，但注释认为这里不涉及 metadata dropping，可能无需处理。

**5. inbounds 条件计算（行 2548-2557）**

```cpp
const DataLayout &DL = GEP->getDataLayout();
auto NonNegative = [&](Value *V) {
  return isKnownNonNegative(V, SimplifyQuery(DL, DT, AC, GEP));
};
bool IsInBounds = Src->isInBounds() && GEP->isInBounds() &&
                  all_of(Src->indices(), NonNegative) &&
                  all_of(GEP->indices(), NonNegative);
```

交换 GEP 嵌套顺序后保持 `inbounds` 的条件：
- 两个原始 GEP 都必须是 `inbounds`
- 两个 GEP 的所有偏移量都必须非负

原因：`inbounds` 要求指针不越界，交换索引层后只有当所有偏移都是非负时才能保证不改变越界语义。使用 `isKnownNonNegative` + `SimplifyQuery`（依赖 DT、AC）做精确推断。

**6. 创建重关联后的 GEP 链（行 2559-2572）**

```cpp
BasicBlock *Preheader = L.getLoopPreheader();
IRBuilder<> Builder(Preheader->getTerminator());
Value *NewSrc = Builder.CreateGEP(GEP->getSourceElementType(), SrcPtr,
                                  SmallVector<Value *>(GEP->indices()),
                                  "invariant.gep", IsInBounds);
Builder.SetInsertPoint(GEP);
Value *NewGEP = Builder.CreateGEP(Src->getSourceElementType(), NewSrc,
                                  SmallVector<Value *>(Src->indices()), "gep",
                                  IsInBounds);
GEP->replaceAllUsesWith(NewGEP);
eraseInstruction(*GEP, SafetyInfo, MSSAU);
salvageDebugInfo(*Src);
eraseInstruction(*Src, SafetyInfo, MSSAU);
return true;
```

变换步骤：
1. 在 preheader terminator 前创建 `NewSrc = gep(SrcPtr, GEP.indices)` —— 这是不变的部分，可以被后续标准 LICM hoist
2. 将 Builder 插入点移回原 GEP 位置
3. 创建 `NewGEP = gep(NewSrc, Src.indices)` —— 这是变的部分，留在循环内
4. `GEP->replaceAllUsesWith(NewGEP)`：保持语义等价
5. 删除原 GEP 和内层 Src GEP（原 GEP 先 RAUW 再 erase；Src 先 salvage debug 再 erase）

---

### 变换示例

**变换前**（`Src` 有变索引 `i`，`GEP` 有不变索引 `offset`）：

```llvm
loop:
  %i = phi i64 [0, %pre], [%i.next, %loop]
  %src.gep = getelementptr inbounds i32, ptr %base, i64 %i    ; Src（变索引）
  %outer.gep = getelementptr inbounds i32, ptr %src.gep, i64 4  ; GEP（不变索引）
  %val = load i32, ptr %outer.gep
  ...
```

**变换后**（不变索引 `4` 被提到外层 GEP，该 GEP 可被后续 LICM hoist）：

```llvm
preheader:
  %invariant.gep = getelementptr inbounds i32, ptr %base, i64 4  ; NewSrc（不变，可 hoist）
loop:
  %i = phi i64 [0, %pre], [%i.next, %loop]
  %gep = getelementptr inbounds i32, ptr %invariant.gep, i64 %i  ; NewGEP（变，留循环内）
  %val = load i32, ptr %gep
  ...
```

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `GetElementPtrInst` | `getPointerOperand()`, `indices()`, `isInBounds()`, `hasAllConstantIndices()` | GEP 指令，提供指针、索引、inbounds 属性 |
| `IRBuilder<>` | `CreateGEP()`, `SetInsertPoint()` | IR 构建器，在指定位置创建指令 |
| `SimplifyQuery` | 构造函数 `(DL, DT, AC, CtxI)` | 简化查询上下文，为 `isKnownNonNegative` 提供分析信息 |

---

### 优化意图

1. **暴露更多 hoist 机会**：嵌套 GEP 中，如果外层索引不变但内层索引变，标准 LICM 无法提升外层 GEP（因为它的指针操作数是循环内的）。通过交换索引层，不变部分被移到外层 GEP，使其成为标准 LICM 的候选。

2. **常量 GEP 排除**：全常量 GEP 通常被后端折叠进寻址模式（如 x86 的 `[base + disp32]`），重关联不仅无益，还可能抑制公共基址的 CSE。

3. **单用约束**：`Src->hasOneUse()` 确保内层 GEP 只被当前外层 GEP 使用。如果 Src 有多个用户，重关联会改变其他用户的计算结果，不安全。

4. **inbounds 保守处理**：只有当两个 GEP 都 inbounds 且所有偏移非负时才保留 inbounds。否则新 GEP 不带 inbounds，保守但正确。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 嵌套 GEP 模式 | 仅处理 `gep(gep(...))` 两层嵌套 | 更深层嵌套不被处理 |
| 内层 GEP 单用 | `Src->hasOneUse()` | 多用户时重关联会改变其他用户的语义 |
| inbounds 保留条件 | 两个 GEP 均 inbounds + 所有偏移非负 | 条件不满足时新 GEP 丢失 inbounds，可能影响后续优化 |
| 内层 GEP 至少一个变索引 | `all_of(Src->indices(), LoopInvariant)` 为 false 时才继续 | 全不变时标准 LICM 已处理，重关联是冗余工作 |
| 调试信息保留 | `salvageDebugInfo(*Src)` 在删除 Src 前调用 | 不调用会导致调试信息丢失 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 类型转换 | `dyn_cast<GetElementPtrInst>(&I)` | `llvm/IR/Instructions.h` |
| 获取 GEP 基指针 | `GEP->getPointerOperand()` | `llvm/IR/Instructions.h` |
| 遍历 GEP 索引 | `GEP->indices()` | `llvm/IR/Instructions.h` |
| 检查常量索引 | `GEP->hasAllConstantIndices()` | `llvm/IR/Instructions.h` |
| 检查 inbounds | `GEP->isInBounds()` | `llvm/IR/Instructions.h` |
| 判断循环不变 | `L.isLoopInvariant(V)` | `llvm/Analysis/LoopInfo.h` |
| 判断非负 | `isKnownNonNegative(V, SimplifyQuery)` | `llvm/Analysis/ValueTracking.h` |
| 创建 GEP | `Builder.CreateGEP()` | `llvm/IR/IRBuilder.h` |
| 替换使用 | `GEP->replaceAllUsesWith(NewGEP)` | `llvm/IR/Value.h` |
| 删除指令 | `eraseInstruction()` | `LICM.cpp:211` |
| 保存调试信息 | `salvageDebugInfo()` | `llvm/Transforms/Utils/Local.h` |

---

### 调用上下文

```text
hoistRegion()                              // LICM.cpp:889
  -> 遍历循环内每条指令
     -> hoistArithmetics(I, ...)            // LICM.cpp:1004
        -> hoistGEP(I, ...)                 // LICM.cpp:2519（本函数）
```

`hoistGEP` 是 `hoistArithmetics` 中尝试的多种重关联策略之一，在 `hoistMinMax` 之后、`hoistAdd`/`hoistSub` 之前被调用。返回 `true` 时，`hoistArithmetics` 立即返回，不再尝试后续策略。

---

### 统计项

变换成功时递增 `NumGEPsHoisted`（`LICM.cpp:107-108`）：

```cpp
STATISTIC(NumGEPsHoisted,
          "Number of geps reassociated and hoisted out of the loop");
```

---

### 与其他 hoistArithmetics helper 的对比

| Helper | 处理的 IR 模式 | 变换效果 |
|---|---|---|
| `hoistMinMax` | `select/and/or(icmp(A, INV1), icmp(A, INV2))` | 合并为 `icmp(A, min/max(INV1, INV2))` |
| `hoistGEP` | `gep(gep(ptr, variant), invariant)` | 交换索引层：`gep(gep(ptr, invariant), variant)` |
| `hoistAdd` | `icmp(LV + INV1, INV2)` | 移项：`icmp(LV, INV2 - INV1)` |
| `hoistSub` | `icmp(LV - INV1, INV2)` 或 `icmp(INV1 - LV, INV2)` | 移项并可能翻转谓词 |
| `hoistFPAssociation` | FP 二元运算链 | 重结合以提取不变子表达式 |
| `hoistBOAssociation` | 整数/FP 二元运算链 | 通用的重结合变换 |

---

## 函数分析：`hoistAdd`（行 2575-2630）

### 函数签名与目的（行 2575-2580）

```cpp
static bool hoistAdd(ICmpInst::Predicate Pred, Value *VariantLHS,
                     Value *InvariantRHS, ICmpInst &ICmp, Loop &L,
                     ICFLoopSafetyInfo &SafetyInfo, MemorySSAUpdater &MSSAU,
                     AssumptionCache *AC, DominatorTree *DT)
```

**功能**：将形如 `(LV + C1) < C2` 的比较重关联为 `LV < (C2 - C1)`，其中 `LV` 是循环变量（loop-variant），`C1` 和 `C2` 是循环不变量（loop-invariant）。通过在 preheader 中预先计算 `C2 - C1`，循环内只需做一次比较。

---

### 整体结构

```
hoistAdd(Pred, VariantLHS, InvariantRHS, ICmp, L, ...)
├── 1. 前置断言：VariantLHS 变，InvariantRHS 不变
├── 2. 确定符号性（signed vs unsigned）
├── 3. PatternMatch：从 VariantLHS 中提取 (VariantOp + InvariantOp)
│   ├── Signed → m_NSWAddLike
│   └── Unsigned → m_NUWAddLike
├── 4. 确保 VariantOp 是变、InvariantOp 是不变
├── 5. 溢出检查：C2 - C1 永不溢出
├── 6. 在 preheader 创建 NewCmpOp = C2 - C1
├── 7. 修改 icmp：LV < NewCmpOp
├── 8. 删除原加法指令
└── 返回 true
```

---

### 逐段注释

**1. 前置断言与符号性判断（行 2581-2584）**

```cpp
assert(!L.isLoopInvariant(VariantLHS) && "Precondition.");
assert(L.isLoopInvariant(InvariantRHS) && "Precondition.");
bool IsSigned = ICmpInst::isSigned(Pred);
```

调用者已确保：比较的 LHS 是循环变量，RHS 是循环不变量。`isSigned` 判断比较谓词是否为有符号（`slt`/`sle`/`sgt`/`sge`）。

**2. 匹配加法模式（行 2586-2594）**

```cpp
using namespace PatternMatch;
Value *VariantOp, *InvariantOp;
if (IsSigned && !match(VariantLHS, m_NSWAddLike(m_Value(VariantOp),
                                                m_Value(InvariantOp))))
  return false;
if (!IsSigned && !match(VariantLHS, m_NUWAddLike(m_Value(VariantOp),
                                                 m_Value(InvariantOp))))
  return false;
```

- 有符号比较 → 匹配 `nsw add`（`m_NSWAddLike` 同时匹配 `add nsw` 和 `or` 形式的加法）
- 无符号比较 → 匹配 `nuw add`（`m_NUWAddLike`）

**为什么需要 nsw/nuw？** 移项 `LV + C1 < C2` → `LV < C2 - C1` 等价于线性代数中的移项，但整数运算中溢出会打破等价性。`nsw`/`nuw` flag 保证加法不溢出，是变换合法性的基础。

**3. 确保操作数角色正确（行 2596-2601）**

```cpp
if (L.isLoopInvariant(VariantOp))
  std::swap(VariantOp, InvariantOp);
if (L.isLoopInvariant(VariantOp) || !L.isLoopInvariant(InvariantOp))
  return false;
```

`PatternMatch` 不保证操作数顺序，这里确保 `VariantOp` 是循环变量、`InvariantOp` 是循环不变量。如果两个都是不变或两个都是变，则无法变换。

**4. 溢出检查（行 2603-2615）**

```cpp
auto &DL = L.getHeader()->getDataLayout();
SimplifyQuery SQ(DL, DT, AC, &ICmp);
if (IsSigned && computeOverflowForSignedSub(InvariantRHS, InvariantOp, SQ) !=
                    llvm::OverflowResult::NeverOverflows)
  return false;
if (!IsSigned &&
    computeOverflowForUnsignedSub(InvariantRHS, InvariantOp, SQ) !=
        llvm::OverflowResult::NeverOverflows)
  return false;
```

核心正确性约束：`C2 - C1` 必须永不溢出。使用 `computeOverflowForSignedSub`/`computeOverflowForUnsignedSub` + `SimplifyQuery`（利用 DT、AC 做值域推断）精确判断。

**5. 创建新比较操作数并修改 icmp（行 2616-2624）**

```cpp
auto *Preheader = L.getLoopPreheader();
IRBuilder<> Builder(Preheader->getTerminator());
Value *NewCmpOp =
    Builder.CreateSub(InvariantRHS, InvariantOp, "invariant.op",
                      /*HasNUW*/ !IsSigned, /*HasNSW*/ IsSigned);
ICmp.setPredicate(Pred);
ICmp.setOperand(0, VariantOp);
ICmp.setOperand(1, NewCmpOp);
```

- 在 preheader 中创建 `C2 - C1`（带 nsw/nuw flag）
- 直接修改原 `icmp` 指令：LHS 改为 `LV`，RHS 改为 `NewCmpOp`，谓词不变

**6. 清理（行 2626-2629）**

```cpp
Instruction &DeadI = cast<Instruction>(*VariantLHS);
salvageDebugInfo(DeadI);
eraseInstruction(DeadI, SafetyInfo, MSSAU);
return true;
```

原加法指令不再需要，先保存调试信息再删除。

---

### 变换示例

**C 语言场景**：

```c
// 循环变量 i 与不变量 offset、limit 的比较
for (int i = 0; i < n; i++) {
    if (i + offset < limit) {  // offset, limit 不变
        arr[i] = val;
    }
}
```

**变换前 IR**：

```llvm
loop:
  %i = phi i32 [0, %pre], [%i.next, %loop]
  %add = add nsw i32 %i, %offset      ; 循环内计算
  %cmp = icmp slt i32 %add, %limit     ; (i + offset) < limit
  br i1 %cmp, label %body, label %loop.end
```

**变换后 IR**：

```llvm
preheader:
  %invariant.op = sub nsw i32 %limit, %offset  ; limit - offset（循环外）
loop:
  %i = phi i32 [0, %pre], [%i.next, %loop]
  %cmp = icmp slt i32 %i, %invariant.op         ; i < (limit - offset)
  br i1 %cmp, label %body, label %loop.end
```

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `PatternMatch` | `m_NSWAddLike`, `m_NUWAddLike` | 匹配带溢出标志的加法 |
| `SimplifyQuery` | 构造函数 `(DL, DT, AC, CtxI)` | 为溢出检查提供分析上下文 |
| `OverflowResult` | `NeverOverflows` | 溢出检查结果 |
| `ICmpInst` | `setPredicate()`, `setOperand()` | 直接修改比较指令 |

---

### 优化意图

1. **消除循环内加法**：将 `LV + C1` 中的不变量 `C1` 移到比较的另一侧，循环内只需做比较，加法被 `C2 - C1` 替代并提升到 preheader
2. **利用 nsw/nuw 保证等价性**：变换依赖加法/减法的无溢出保证，否则移项在溢出情况下不等价
3. **减少循环内指令数**：对于 N 次循环，从 N 次加法 + N 次比较 → N 次比较 + 1 次减法

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须有 nsw/nuw flag | 无溢出标志的 add 不能安全移项 | 溢出时移项不等价 |
| C2 - C1 永不溢出 | 通过 `computeOverflowFor*Sub` 检查 | 否则新减法可能产生 poison |
| 操作数角色必须一変一不变 | 两个都变或两个都不变无法变换 | 提前返回 false |
| VariantLHS 必须有一个使用 | 由调用者 `hoistAddSub` 保证 `hasOneUse()` | 否则删除后影响其他用户 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 匹配加法 | `match(V, m_NSWAddLike(...))` | `llvm/IR/PatternMatch.h` |
| 判断有符号 | `ICmpInst::isSigned(Pred)` | `llvm/IR/InstrTypes.h` |
| 溢出检查 | `computeOverflowForSignedSub()` | `llvm/Analysis/ValueTracking.h` |
| 创建减法 | `Builder.CreateSub()` | `llvm/IR/IRBuilder.h` |
| 修改 icmp | `ICmp.setPredicate()`, `setOperand()` | `llvm/IR/Instructions.h` |
| 删除指令 | `eraseInstruction()` | `LICM.cpp:211` |

---

### 调用上下文

```text
hoistRegion()
  -> hoistArithmetics(I, ...)
     -> hoistAddSub(I, ...)
        -> hoistAdd(Pred, LHS, RHS, ...)  // 本函数
```

---

## 函数分析：`hoistSub`（行 2632-2711）

### 函数签名与目的（行 2632-2638）

```cpp
static bool hoistSub(ICmpInst::Predicate Pred, Value *VariantLHS,
                     Value *InvariantRHS, ICmpInst &ICmp, Loop &L,
                     ICFLoopSafetyInfo &SafetyInfo, MemorySSAUpdater &MSSAU,
                     AssumptionCache *AC, DominatorTree *DT)
```

**功能**：处理两种减法模式的比较重关联：
- **模式 A**：`LV - C1 < C2` → `LV < C1 + C2`
- **模式 B**：`C1 - LV < C2` → `LV > C1 - C2`（变量在减号右侧，需翻转谓词）

---

### 整体结构

```
hoistSub(Pred, VariantLHS, InvariantRHS, ICmp, L, ...)
├── 1. 前置断言：VariantLHS 变，InvariantRHS 不变
├── 2. 确定符号性
├── 3. PatternMatch：从 VariantLHS 中提取减法
│   ├── Signed → m_NSWSub
│   └── Unsigned → m_NUWSub
├── 4. 判断变量在减号的哪一侧
│   ├── VariantOp 不变 → 变量在右侧（模式 B）
│   │   ├── 交换操作数角色
│   │   └── 翻转谓词
│   └── 否则 → 变量在左侧（模式 A）
├── 5. 溢出检查
│   ├── 模式 B：C1 - C2 永不溢出
│   └── 模式 A：C1 + C2 永不溢出
├── 6. 在 preheader 创建 NewCmpOp
│   ├── 模式 B → C1 - C2
│   └── 模式 A → C1 + C2
├── 7. 修改 icmp
├── 8. 删除原减法指令
└── 返回 true
```

---

### 逐段注释

**1. 匹配减法模式（行 2644-2652）**

```cpp
using namespace PatternMatch;
Value *VariantOp, *InvariantOp;
if (IsSigned &&
    !match(VariantLHS, m_NSWSub(m_Value(VariantOp), m_Value(InvariantOp))))
  return false;
if (!IsSigned &&
    !match(VariantLHS, m_NUWSub(m_Value(VariantOp), m_Value(InvariantOp))))
  return false;
```

匹配 `sub nsw` 或 `sub nuw`。`m_NSWSub` 匹配 `sub nsw A, B`，捕获两个操作数。

**2. 判断变量位置（行 2654-2664）**

```cpp
bool VariantSubtracted = false;
if (L.isLoopInvariant(VariantOp)) {
  std::swap(VariantOp, InvariantOp);
  VariantSubtracted = true;
  Pred = ICmpInst::getSwappedPredicate(Pred);
}
if (L.isLoopInvariant(VariantOp) || !L.isLoopInvariant(InvariantOp))
  return false;
```

`m_NSWSub(VariantOp, InvariantOp)` 匹配的是 `VariantOp - InvariantOp`，但 `PatternMatch` 按书写顺序捕获，不区分哪个是变量。如果 `VariantOp` 实际是不变量，说明变量在减号右侧（`C1 - LV`），此时：
- 交换操作数：`VariantOp` 变为循环变量
- `VariantSubtracted = true`：标记变量被减
- 翻转谓词：`<` → `>`（因为 `C1 - LV < C2` 等价于 `LV > C1 - C2`）

**3. 溢出检查（行 2666-2693）**

四种情况，分别检查：

| 模式 | 变换 | 需检查 |
|---|---|---|
| A (Signed) | `LV - C1 < C2` → `LV < C1 + C2` | `C1 + C2` 不溢出 |
| A (Unsigned) | 同上 | `C1 + C2` 不溢出 |
| B (Signed) | `C1 - LV < C2` → `LV > C1 - C2` | `C1 - C2` 不溢出 |
| B (Unsigned) | 同上 | `C1 - C2` 不溢出 |

**4. 创建新操作数并修改 icmp（行 2694-2705）**

```cpp
IRBuilder<> Builder(Preheader->getTerminator());
Value *NewCmpOp =
    VariantSubtracted
        ? Builder.CreateSub(InvariantOp, InvariantRHS, "invariant.op",
                            /*HasNUW*/ !IsSigned, /*HasNSW*/ IsSigned)
        : Builder.CreateAdd(InvariantOp, InvariantRHS, "invariant.op",
                            /*HasNUW*/ !IsSigned, /*HasNSW*/ IsSigned);
ICmp.setPredicate(Pred);
ICmp.setOperand(0, VariantOp);
ICmp.setOperand(1, NewCmpOp);
```

- 模式 A（变量在左侧）→ 创建 `C1 + C2`
- 模式 B（变量在右侧）→ 创建 `C1 - C2`

---

### 变换示例

**模式 A：`LV - C1 < C2` → `LV < C1 + C2`**

```c
// C 语言
for (int i = 0; i < n; i++) {
    if (i - header_size < body_limit) {  // header_size, body_limit 不变
        process(i);
    }
}
```

```llvm
; 变换前
%sub = sub nsw i32 %i, %header_size
%cmp = icmp slt i32 %sub, %body_limit    ; (i - header_size) < body_limit

; 变换后（preheader 中）
%invariant.op = add nsw i32 %header_size, %body_limit  ; header_size + body_limit
; 循环内
%cmp = icmp slt i32 %i, %invariant.op                  ; i < (header_size + body_limit)
```

**模式 B：`C1 - LV < C2` → `LV > C1 - C2`**

```c
// C 语言：剩余空间检查
for (int i = 0; i < n; i++) {
    if (total_budget - cost[i] < min_reserve) {  // 剩余 < 最小保留
        break;
    }
}
```

```llvm
; 变换前
%sub = sub nsw i32 %total_budget, %cost      ; total_budget - cost[i]
%cmp = icmp slt i32 %sub, %min_reserve        ; (total - cost) < min_reserve

; 变换后（preheader 中）
%invariant.op = sub nsw i32 %total_budget, %min_reserve  ; total - min_reserve
; 循环内
%cmp = icmp sgt i32 %cost, %invariant.op                   ; cost > (total - min_reserve)
```

注意：谓词从 `slt` 翻转为 `sgt`。

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `PatternMatch` | `m_NSWSub`, `m_NUWSub` | 匹配带溢出标志的减法 |
| `ICmpInst` | `getSwappedPredicate(Pred)` | 翻转比较方向（`<` → `>`） |
| `OverflowResult` | `NeverOverflows` | 溢出检查结果 |

---

### 优化意图

1. **消除循环内减法**：与 `hoistAdd` 类似，将不变量部分移到比较的另一侧
2. **处理变量在减号右侧的情况**：`C1 - LV` 模式需要翻转谓词，这是 `hoistSub` 相比 `hoistAdd` 的额外复杂度
3. **统一的不变量提升**：变换后比较的一个操作数是完全不变的，为后续标准 LICM hoist 创造条件

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须有 nsw/nuw flag | 减法同样需要无溢出保证 | 溢出时移项不等价 |
| 模式 B 需翻转谓词 | `C1 - LV < C2` 不等价于 `LV < C1 - C2` | 忘记翻转谓词会导致语义错误 |
| 模式 B 的减法溢出检查 | 检查的是 `C1 - C2` 而非 `C1 + C2` | 方向错误会导致漏检溢出 |
| 变量必须恰好有一个使用 | 由调用者保证 | 否则删除减法后影响其他用户 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 匹配减法 | `match(V, m_NSWSub(...))` | `llvm/IR/PatternMatch.h` |
| 翻转谓词 | `ICmpInst::getSwappedPredicate(Pred)` | `llvm/IR/InstrTypes.h` |
| 溢出检查 | `computeOverflowForSignedAdd/Sub()` | `llvm/Analysis/ValueTracking.h` |
| 创建加法 | `Builder.CreateAdd()` | `llvm/IR/IRBuilder.h` |
| 创建减法 | `Builder.CreateSub()` | `llvm/IR/IRBuilder.h` |

---

### 调用上下文

```text
hoistRegion()
  -> hoistArithmetics(I, ...)
     -> hoistAddSub(I, ...)
        -> hoistSub(Pred, LHS, RHS, ...)  // 本函数
```

---

## 函数分析：`hoistAddSub`（行 2713-2742）

### 函数签名与目的（行 2713-2716）

```cpp
static bool hoistAddSub(Instruction &I, Loop &L, ICFLoopSafetyInfo &SafetyInfo,
                        MemorySSAUpdater &MSSAU, AssumptionCache *AC,
                        DominatorTree *DT)
```

**功能**：`hoistAdd` 和 `hoistSub` 的统一入口。识别 `icmp` 指令中是否包含可重关联的加法或减法模式，并分派给对应的 helper 函数执行变换。

---

### 整体结构

```
hoistAddSub(I, L, SafetyInfo, MSSAU, AC, DT)
├── 1. 匹配 icmp 指令
├── 2. 确保 LHS 是变量、RHS 是不变量
│   └── 若 LHS 不变 → 交换 LHS/RHS 并翻转谓词
├── 3. 前置检查
│   ├── LHS 必须是变量
│   ├── RHS 必须是不变量
│   └── LHS 必须只有一个使用（变换后可安全删除）
├── 4. 尝试 hoistAdd
├── 5. 若失败，尝试 hoistSub
└── 返回结果
```

---

### 逐段注释

**1. 匹配 icmp（行 2717-2721）**

```cpp
using namespace PatternMatch;
CmpPredicate Pred;
Value *LHS, *RHS;
if (!match(&I, m_ICmp(Pred, m_Value(LHS), m_Value(RHS))))
  return false;
```

只处理整数比较指令。

**2. 规范化操作数位置（行 2723-2727）**

```cpp
// Put variant operand to LHS position.
if (L.isLoopInvariant(LHS)) {
  std::swap(LHS, RHS);
  Pred = ICmpInst::getSwappedPredicate(Pred);
}
```

如果 LHS 是不变量而 RHS 是变量，交换两者并翻转谓词，确保后续 `hoistAdd`/`hoistSub` 的 `VariantLHS` 参数确实是循环变量。

**3. 前置检查（行 2728-2731）**

```cpp
// We want to delete the initial operation after reassociation, so only do it
// if it has no other uses.
if (L.isLoopInvariant(LHS) || !L.isLoopInvariant(RHS) || !LHS->hasOneUse())
  return false;
```

三个条件缺一不可：
- `LHS` 必须是变量（否则不需要重关联）
- `RHS` 必须是不变量（否则无法将不变量移到比较的另一侧）
- `LHS` 必须只有一个使用（变换后原加法/减法指令需要被删除）

**4. 分派给 hoistAdd / hoistSub（行 2733-2741）**

```cpp
// TODO: We could go with smarter context, taking common dominator of all I's
// users instead of I itself.
if (hoistAdd(Pred, LHS, RHS, cast<ICmpInst>(I), L, SafetyInfo, MSSAU, AC, DT))
  return true;

if (hoistSub(Pred, LHS, RHS, cast<ICmpInst>(I), L, SafetyInfo, MSSAU, AC, DT))
  return true;

return false;
```

先尝试 `hoistAdd`（匹配 `LV + C1` 模式），失败后再尝试 `hoistSub`（匹配 `LV - C1` 或 `C1 - LV` 模式）。任一成功即返回 `true`。

TODO 注释指出：当前以 `I` 本身为上下文做溢出检查，未来可以考虑取 `I` 的所有用户的共同支配点，可能获得更精确的值域信息。

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `PatternMatch` | `m_ICmp(Pred, LHS, RHS)` | 匹配 icmp 指令并捕获谓词和操作数 |
| `Value` | `hasOneUse()` | 检查单一使用 |

---

### 优化意图

1. **统一入口**：`hoistArithmetics` 只需调用一个函数即可尝试加法和减法两种重关联
2. **操作数规范化**：在入口处统一将变量放在 LHS，简化后续 helper 的逻辑
3. **短路尝试**：`hoistAdd` 成功则不再尝试 `hoistSub`，避免重复工作

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只处理 icmp | 不处理 fcmp（浮点比较有不同语义） | 浮点由 `hoistFPAssociation` 处理 |
| LHS 必须单用 | 变换后原加法/减法被删除 | 多用户时删除会破坏其他使用 |
| 谓词翻转的正确性 | 交换操作数时必须同时翻转谓词 | 否则比较语义错误 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 匹配 icmp | `match(&I, m_ICmp(Pred, LHS, RHS))` | `llvm/IR/PatternMatch.h` |
| 翻转谓词 | `ICmpInst::getSwappedPredicate(Pred)` | `llvm/IR/InstrTypes.h` |
| 单一使用检查 | `LHS->hasOneUse()` | `llvm/IR/Value.h` |
| 分派加法 | `hoistAdd(...)` | `LICM.cpp:2575` |
| 分派减法 | `hoistSub(...)` | `LICM.cpp:2632` |

---

### 调用上下文

```text
hoistRegion()                              // LICM.cpp:889
  -> 遍历循环内每条指令
     -> hoistArithmetics(I, ...)            // LICM.cpp:1004
        -> hoistAddSub(I, ...)              // LICM.cpp:2714（本函数）
           -> hoistAdd(...)                 // LICM.cpp:2575
           -> hoistSub(...)                 // LICM.cpp:2632
```

---

### 统计项

变换成功时递增 `NumAddSubHoisted`（`LICM.cpp:109-110`）：

```cpp
STATISTIC(NumAddSubHoisted, "Number of add/subtract expressions reassociated "
                            "and hoisted out of the loop");
```

---

### 三个函数的关系总结

```
hoistAddSub (统一入口，行 2713)
├── 规范化：确保 icmp 的 LHS 是变量、RHS 是不变量
├── 检查：LHS 单用、LHS 变、RHS 不变
│
├── hoistAdd (行 2575)
│   ├── 匹配：LV + C1 < C2
│   ├── 变换：LV < C2 - C1
│   └── 溢出检查：C2 - C1 永不溢出
│
└── hoistSub (行 2632)
    ├── 模式 A：LV - C1 < C2 → LV < C1 + C2
    │   └── 溢出检查：C1 + C2 永不溢出
    │
    └── 模式 B：C1 - LV < C2 → LV > C1 - C2
        ├── 翻转谓词
        └── 溢出检查：C1 - C2 永不溢出
```

**共同点**：
- 都依赖 nsw/nuw flag 保证移项等价
- 都将不变量计算提升到 preheader
- 都修改原 icmp 指令而非创建新 icmp
- 都删除原加法/减法指令

**区别**：
- `hoistAdd` 只处理加法，只有一种变换方向
- `hoistSub` 处理减法，有两种模式（变量在左侧/右侧），模式 B 需要翻转谓词
- `hoistAddSub` 是调度器，负责规范化操作数位置和分派

---

## 函数分析：`hoistMulAddAssociation`（行 2759-2850）

### 函数签名与目的（行 2759-2762）

```cpp
static bool hoistMulAddAssociation(Instruction &I, Loop &L,
                                   ICFLoopSafetyInfo &SafetyInfo,
                                   MemorySSAUpdater &MSSAU, AssumptionCache *AC,
                                   DominatorTree *DT)
```

**功能**：将形如 `((A1 * B1) + (A2 * B2) + ...) * C` 的表达式（其中 `A1, A2, ...` 和 `C` 是循环不变量）重关联为 `((A1 * C * B1) + (A2 * C * B2) + ...)`，并将 `A1 * C`、`A2 * C` 等不变子表达式提升到 preheader。适用于整数乘法/加法链和浮点 FMul/FAdd 链。

---

### 整体结构

```
hoistMulAddAssociation(I, L, ...)
├── 1. 检查 I 是否为可重关联的 Mul/FMul
├── 2. 确定 VariantOp 和 InvariantOp（Factor）
├── 3. 遍历表达式树，收集可变换点
│   ├── Worklist: 从 VariantOp 开始 BFS
│   ├── 遇到 Add/FAdd → 递归进入两个操作数
│   ├── 遇到 Mul/FMul → 检查是否有一个不变操作数
│   │   └── 记录 Changes（待修改的 Use）
│   └── 任一节点多用户 / 无不变操作数 / 超限 → 返回 false
├── 4. 若 Changes 为空 → 返回 false
├── 5. 整数类型：清除所有经过的 Add 的 poison flags
├── 6. 执行变换
│   ├── 对每个 Change：
│   │   ├── 在 preheader 创建 Mul = InvariantOperand * Factor
│   │   ├── 重写原 Mul 指令：用 Mul 替换不变操作数
│   │   ├── RAUW 旧 Mul → 新 Mul
│   │   └── 删除旧 Mul
│   └── 替换 I 为 VariantOp，删除 I
└── 返回 true
```

---

### 逐段注释

**1. 入口检查与操作数分类（行 2763-2771）**

```cpp
if (!isReassociableOp(&I, Instruction::Mul, Instruction::FMul))
  return false;
Value *VariantOp = I.getOperand(0);
Value *InvariantOp = I.getOperand(1);
if (L.isLoopInvariant(VariantOp))
  std::swap(VariantOp, InvariantOp);
if (L.isLoopInvariant(VariantOp) || !L.isLoopInvariant(InvariantOp))
  return false;
Value *Factor = InvariantOp;
```

- `isReassociableOp`：整数要求 `Mul`；浮点要求 `FMul` + `reassoc` + `nsz`（no-signed-zeros）
- 确保一个操作数变、一个不变；不变的那个称为 `Factor`，将乘到每个不变子表达式上

**2. 遍历表达式树，收集 Changes（行 2773-2809）**

```cpp
SmallVector<Use *> Changes;
SmallVector<BinaryOperator *> Adds;
SmallVector<BinaryOperator *> Worklist;
if (BinaryOperator *VariantBinOp = dyn_cast<BinaryOperator>(VariantOp))
  Worklist.push_back(VariantBinOp);
while (!Worklist.empty()) {
  BinaryOperator *BO = Worklist.pop_back_val();
  if (!BO->hasOneUse())
    return false;
  if (isReassociableOp(BO, Instruction::Add, Instruction::FAdd) &&
      isa<BinaryOperator>(BO->getOperand(0)) &&
      isa<BinaryOperator>(BO->getOperand(1))) {
    Worklist.push_back(cast<BinaryOperator>(BO->getOperand(0)));
    Worklist.push_back(cast<BinaryOperator>(BO->getOperand(1)));
    Adds.push_back(BO);
    continue;
  }
  if (!isReassociableOp(BO, Instruction::Mul, Instruction::FMul) ||
      L.isLoopInvariant(BO))
    return false;
  Use &U0 = BO->getOperandUse(0);
  Use &U1 = BO->getOperandUse(1);
  if (L.isLoopInvariant(U0))
    Changes.push_back(&U0);
  else if (L.isLoopInvariant(U1))
    Changes.push_back(&U1);
  else
    return false;
  unsigned Limit = I.getType()->isIntOrIntVectorTy()
                       ? IntAssociationUpperLimit
                       : FPAssociationUpperLimit;
  if (Changes.size() > Limit)
    return false;
}
```

BFS 遍历表达式树：
- **Add/FAdd 节点**：递归进入两个操作数（要求两个操作数也都是 BinaryOperator），记录到 `Adds` 列表
- **Mul/FMul 节点**：必须恰好有一个不变操作数，记录到 `Changes`；如果两个都变或整个 Mul 不变 → 失败
- **单用约束**：每个节点必须 `hasOneUse()`，否则变换后影响其他用户
- **上限控制**：`IntAssociationUpperLimit` / `FPAssociationUpperLimit`（默认 5），防止编译时爆炸

**3. 清除 poison flags（行 2811-2815）**

```cpp
if (I.getType()->isIntOrIntVectorTy()) {
  for (auto *Add : Adds)
    Add->dropPoisonGeneratingFlags();
}
```

整数加法链经过重关联后，原有的 `nsw`/`nuw` 标志可能不再成立，需要清除。

**4. 执行变换（行 2817-2849）**

```cpp
auto *Preheader = L.getLoopPreheader();
IRBuilder<> Builder(Preheader->getTerminator());
for (auto *U : Changes) {
  assert(L.isLoopInvariant(U->get()));
  auto *Ins = cast<BinaryOperator>(U->getUser());
  Value *Mul;
  if (I.getType()->isIntOrIntVectorTy()) {
    Mul = Builder.CreateMul(U->get(), Factor, "factor.op.mul");
    Ins->dropPoisonGeneratingFlags();
  } else
    Mul = Builder.CreateFMulFMF(U->get(), Factor, Ins, "factor.op.fmul");

  unsigned OpIdx = U->getOperandNo();
  auto *LHS = OpIdx == 0 ? Mul : Ins->getOperand(0);
  auto *RHS = OpIdx == 1 ? Mul : Ins->getOperand(1);
  auto *NewBO = BinaryOperator::Create(Ins->getOpcode(), LHS, RHS,
                                       Ins->getName() + ".reass", Ins->getIterator());
  NewBO->setDebugLoc(DebugLoc::getDropped());
  NewBO->copyIRFlags(Ins);
  if (VariantOp == Ins)
    VariantOp = NewBO;
  Ins->replaceAllUsesWith(NewBO);
  eraseInstruction(*Ins, SafetyInfo, MSSAU);
}
I.replaceAllUsesWith(VariantOp);
eraseInstruction(I, SafetyInfo, MSSAU);
return true;
```

对每个 `Change`（即每个 Mul 中的不变操作数）：
1. 在 preheader 创建 `Mul = InvariantOperand * Factor`
2. 在原位置创建新的 Mul 指令，用 `Mul` 替换原来的不变操作数
3. RAUW 旧 Mul → 新 Mul，删除旧 Mul
4. 最后替换最外层的 `I`

---

### 变换示例

**C 语言场景**：

```c
// 循环内：(a[i] * scale + b[i] * scale) 其中 scale 是不变量
for (int i = 0; i < n; i++) {
    result[i] = a[i] * scale + b[i] * scale;
}
```

**变换前 IR**：

```llvm
preheader:
  ; scale 是循环不变
loop:
  %a_val = load i32, ptr %a_ptr
  %b_val = load i32, ptr %b_ptr
  %mul1 = mul i32 %a_val, %scale       ; a[i] * scale
  %mul2 = mul i32 %b_val, %scale       ; b[i] * scale
  %add = add i32 %mul1, %mul2          ; (a[i]*scale) + (b[i]*scale)
  %result = mul i32 %add, %scale2      ; 假设外层再乘一个不变量
  ...
```

更典型的模式是 `((A1 * B1) + (A2 * B2)) * C`：

```llvm
; 假设 I = ((%a_inv * %lv) + (%b_inv * %lv2)) * %c_inv
; 其中 %a_inv, %b_inv, %c_inv 不变，%lv, %lv2 变
```

**变换后 IR**：

```llvm
preheader:
  %factor.op.mul1 = mul i32 %a_inv, %c_inv   ; 不变部分提升到 preheader
  %factor.op.mul2 = mul i32 %b_inv, %c_inv   ; 不变部分提升到 preheader
loop:
  %mul1.reass = mul i32 %factor.op.mul1, %lv   ; (a_inv * c_inv) * lv
  %mul2.reass = mul i32 %factor.op.mul2, %lv2  ; (b_inv * c_inv) * lv2
  %add.reass = add i32 %mul1.reass, %mul2.reass
  ...
```

每个不变乘法 `A * C` 被提升到 preheader，循环内只执行 `A*C*LV` 中的变部分。

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `SmallVector<Use *>` | `Changes` | 记录待修改的 Use（每个 Mul 中的不变操作数） |
| `SmallVector<BinaryOperator *>` | `Adds` | 记录遍历过的 Add/FAdd 节点（用于清除 poison flags） |
| `SmallVector<BinaryOperator *>` | `Worklist` | BFS 工作列表 |
| `isReassociableOp` | 辅助函数 | 判断指令是否为可重关联的 Mul/FMul 或 Add/FAdd |

---

### 优化意图

1. **分配律应用**：利用 `(A * B) + (A * C) = A * (B + C)` 的逆过程，将公共不变因子分配到每个项中，使更多不变计算可以提升到循环外
2. **减少循环内乘法**：对于 N 次循环和 K 个不变因子，从 N * K 次不变乘法 → K 次不变乘法（在 preheader）
3. **同时支持整数和浮点**：整数用 `Mul/Add`，浮点用 `FMul/FAdd`（需 `reassoc` + `nsz` flags）

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 每个节点必须单用 | `BO->hasOneUse()` | 多用户时变换会破坏其他使用 |
| 每个 Mul 必须恰好一个不变操作数 | 两个都变或都不变无法提取 | 返回 false |
| 变换次数上限 | `IntAssociationUpperLimit` / `FPAssociationUpperLimit`（默认 5） | 防止编译时爆炸 |
| 整数 Add 的 poison flags | 重关联后 nsw/nuw 可能不成立 | 必须 `dropPoisonGeneratingFlags()` |
| 浮点需 reassoc + nsz | 浮点重关联改变计算顺序 | 无 flags 时不能变换 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 可重关联判断 | `isReassociableOp()` | `LICM.cpp:2744` |
| 创建整数乘法 | `Builder.CreateMul()` | `llvm/IR/IRBuilder.h` |
| 创建浮点乘法 | `Builder.CreateFMulFMF()` | `llvm/IR/IRBuilder.h` |
| 清除 poison flags | `Ins->dropPoisonGeneratingFlags()` | `llvm/IR/Instruction.h` |
| 复制 IR flags | `NewBO->copyIRFlags(Ins)` | `llvm/IR/Instruction.h` |
| 替换使用 | `Ins->replaceAllUsesWith(NewBO)` | `llvm/IR/Value.h` |

---

### 调用上下文

```text
hoistRegion()
  -> hoistArithmetics(I, ...)
     -> hoistMulAddAssociation(I, ...)  // 本函数（行 2957）
```

---

### 统计项

变换成功时递增 `NumIntAssociationsHoisted`（整数）或 `NumFPAssociationsHoisted`（浮点）：

```cpp
if (IsInt)
  ++NumIntAssociationsHoisted;
else
  ++NumFPAssociationsHoisted;
```

---

## 函数分析：`hoistBOAssociation`（行 2862-2927）

### 函数签名与目的（行 2862-2865）

```cpp
static bool hoistBOAssociation(Instruction &I, Loop &L,
                               ICFLoopSafetyInfo &SafetyInfo,
                               MemorySSAUpdater &MSSAU, AssumptionCache *AC,
                               DominatorTree *DT)
```

**功能**：处理两层嵌套的关联二元运算，将不变量合并后提升到 preheader。支持四种模式：
1. `(LV op C1) op C2` → `LV op (C1 op C2)`
2. `(C1 op LV) op C2` → `LV op (C1 op C2)`
3. `C2 op (C1 op LV)` → `LV op (C1 op C2)`
4. `C2 op (LV op C1)` → `LV op (C1 op C2)`

其中 `op` 是关联运算（`add`/`mul`/`fadd`/`fmul`/`and`/`or`/`xor`），`LV` 是循环变量，`C1`/`C2` 是循环不变量。

---

### 整体结构

```
hoistBOAssociation(I, L, ...)
├── 1. 检查 I 是否为关联 BinaryOperator
├── 2. 确定 LV 在内层还是外层
├── 3. 提取内层 BinaryOperator BO0
├── 4. 从 BO0 中识别 LV、C1，从外层识别 C2
├── 5. 验证：LV 变、C1 不变、C2 不变
├── 6. 在 preheader 创建 Inv = C1 op C2
├── 7. 在原位置创建 NewBO = LV op Inv
├── 8. 传播 flags（FMF 或 OverflowTracking）
├── 9. RAUW I → NewBO，删除 I
├── 10. 若 BO0 无用户，删除 BO0
└── 返回 true
```

---

### 逐段注释

**1. 入口检查（行 2866-2874）**

```cpp
auto *BO = dyn_cast<BinaryOperator>(&I);
if (!BO || !BO->isAssociative())
  return false;

Instruction::BinaryOps Opcode = BO->getOpcode();
bool LVInRHS = L.isLoopInvariant(BO->getOperand(0));
auto *BO0 = dyn_cast<BinaryOperator>(BO->getOperand(LVInRHS));
if (!BO0 || BO0->getOpcode() != Opcode || !BO0->isAssociative() ||
    BO0->hasNUsesOrMore(BO0->getType()->isIntegerTy() ? 2 : 3))
  return false;
```

- 外层 `I` 必须是关联运算
- `LVInRHS`：如果 LHS 不变，说明 LV 在 RHS，内层运算在 LHS
- 内层 `BO0` 必须是相同 opcode 的关联运算
- `hasNUsesOrMore` 检查：整数类型最多 1 个用户，浮点最多 2 个（因为 FMF flags 需要额外考虑）

**2. 提取 LV、C1、C2（行 2877-2886）**

```cpp
Value *LV = BO0->getOperand(0);
Value *C1 = BO0->getOperand(1);
Value *C2 = BO->getOperand(!LVInRHS);

assert(BO->isCommutative() && BO0->isCommutative() &&
       "Associativity implies commutativity");
if (L.isLoopInvariant(LV) && !L.isLoopInvariant(C1))
  std::swap(LV, C1);
if (L.isLoopInvariant(LV) || !L.isLoopInvariant(C1) || !L.isLoopInvariant(C2))
  return false;
```

- 关联运算隐含交换律，所以 `BO0` 的操作数顺序不重要
- 如果 `LV` 实际是不变而 `C1` 是变，交换两者
- 最终确保：`LV` 变、`C1` 不变、`C2` 不变

**3. 创建不变运算和新二元运算（行 2888-2896）**

```cpp
auto *Preheader = L.getLoopPreheader();
IRBuilder<> Builder(Preheader->getTerminator());
auto *Inv = Builder.CreateBinOp(Opcode, C1, C2, "invariant.op");

auto *NewBO = BinaryOperator::Create(
    Opcode, LV, Inv, BO->getName() + ".reass", BO->getIterator());
NewBO->setDebugLoc(DebugLoc::getDropped());
```

- 在 preheader 创建 `Inv = C1 op C2`（不变部分）
- 在原位置创建 `NewBO = LV op Inv`

**4. 传播 flags（行 2898-2914）**

```cpp
if (Opcode == Instruction::FAdd || Opcode == Instruction::FMul) {
  // Intersect FMF flags for FADD and FMUL.
  FastMathFlags Intersect = BO->getFastMathFlags() & BO0->getFastMathFlags();
  if (auto *I = dyn_cast<Instruction>(Inv))
    I->setFastMathFlags(Intersect);
  NewBO->setFastMathFlags(Intersect);
} else {
  OverflowTracking Flags;
  Flags.AllKnownNonNegative = false;
  Flags.AllKnownNonZero = false;
  Flags.mergeFlags(*BO);
  Flags.mergeFlags(*BO0);
  if (auto *I = dyn_cast<Instruction>(Inv))
    Flags.applyFlags(*I);
  Flags.applyFlags(*NewBO);
}
```

- **浮点**：取两个原始指令的 FMF（FastMathFlags）交集，保守处理
- **整数**：用 `OverflowTracking` 合并两个指令的溢出标志（`nsw`/`nuw` 等），只保留两者都有的标志

**5. 清理（行 2916-2926）**

```cpp
BO->replaceAllUsesWith(NewBO);
eraseInstruction(*BO, SafetyInfo, MSSAU);

if (BO0->use_empty()) {
  salvageDebugInfo(*BO0);
  eraseInstruction(*BO0, SafetyInfo, MSSAU);
}
return true;
```

- 替换 `I` → `NewBO`，删除 `I`
- 如果内层 `BO0` 没有其他用户（即只有 `I` 用了它），也删除

---

### 变换示例

**模式 1：`(LV + C1) + C2` → `LV + (C1 + C2)`**

```c
// C 语言
for (int i = 0; i < n; i++) {
    int x = (i + offset1) + offset2;  // offset1, offset2 不变
    arr[i] = x;
}
```

```llvm
; 变换前
loop:
  %i = phi i32 [0, %pre], [%i.next, %loop]
  %add1 = add nsw i32 %i, %offset1     ; LV + C1
  %add2 = add nsw i32 %add1, %offset2  ; (LV + C1) + C2

; 变换后
preheader:
  %invariant.op = add nsw i32 %offset1, %offset2  ; C1 + C2
loop:
  %i = phi i32 [0, %pre], [%i.next, %loop]
  %add2.reass = add nsw i32 %i, %invariant.op     ; LV + (C1 + C2)
```

**模式 2：`(C1 * LV) * C2` → `LV * (C1 * C2)`**

```c
// C 语言：缩放因子合并
for (int i = 0; i < n; i++) {
    result[i] = (scale1 * data[i]) * scale2;  // scale1, scale2 不变
}
```

```llvm
; 变换前
loop:
  %data = load i32, ptr %data_ptr
  %mul1 = mul i32 %scale1, %data       ; C1 * LV
  %mul2 = mul i32 %mul1, %scale2       ; (C1 * LV) * C2

; 变换后
preheader:
  %invariant.op = mul i32 %scale1, %scale2  ; C1 * C2
loop:
  %data = load i32, ptr %data_ptr
  %mul2.reass = mul i32 %data, %invariant.op  ; LV * (C1 * C2)
```

**模式 3：`C2 & (C1 & LV)` → `LV & (C1 & C2)`**（按位与）

```c
// C 语言：掩码合并
for (int i = 0; i < n; i++) {
    flags[i] = (MASK1 & flags[i]) & MASK2;  // MASK1, MASK2 不变
}
```

```llvm
; 变换前
loop:
  %flags = load i32, ptr %flags_ptr
  %and1 = and i32 %MASK1, %flags       ; C1 & LV
  %and2 = and i32 %and1, %MASK2        ; (C1 & LV) & C2

; 变换后
preheader:
  %invariant.op = and i32 %MASK1, %MASK2  ; C1 & C2
loop:
  %flags = load i32, ptr %flags_ptr
  %and2.reass = and i32 %flags, %invariant.op  ; LV & (C1 & C2)
```

---

### 关键数据结构

| 结构 | 关键字段/接口 | 含义 |
|---|---|---|
| `BinaryOperator` | `isAssociative()`, `isCommutative()` | 判断运算是否关联/交换 |
| `FastMathFlags` | `operator&`（交集） | 浮点快速数学标志的交集运算 |
| `OverflowTracking` | `mergeFlags()`, `applyFlags()` | 整数溢出标志的合并与应用 |

---

### 优化意图

1. **不变量合并**：将两层嵌套运算中的两个不变量 `C1` 和 `C2` 合并为一个运算，提升到 preheader
2. **减少循环内运算**：从循环内 2 次运算 → 循环内 1 次运算 + preheader 1 次运算
3. **通用性**：支持所有关联运算（`add`/`mul`/`fadd`/`fmul`/`and`/`or`/`xor`），不限于特定 opcode
4. **保守的 flags 处理**：浮点取 FMF 交集，整数用 `OverflowTracking` 合并标志，确保变换后 flags 正确

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须是关联运算 | `BO->isAssociative()` | 非关联运算（如 `sub`/`div`/`fdiv`）不能变换 |
| 内层必须同 opcode | `BO0->getOpcode() == Opcode` | 不同运算不能合并 |
| 内层用户数限制 | 整数 ≤ 1，浮点 ≤ 2 | 多用户时不能安全删除 |
| FMF flags 交集 | 浮点取两个指令的 FMF 交集 | 交集可能丢失优化标志 |
| OverflowTracking 初始化 | `AllKnownNonNegative/NonZero` 设为 false | 保守处理，可能丢失 nsw/nuw |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 关联性判断 | `BO->isAssociative()` | `llvm/IR/Instruction.h` |
| 创建二元运算 | `Builder.CreateBinOp()` | `llvm/IR/IRBuilder.h` |
| FMF 交集 | `FMF1 & FMF2` | `llvm/IR/Operator.h` |
| 溢出标志合并 | `OverflowTracking::mergeFlags()` | `llvm/IR/Instruction.h` |
| 创建新指令 | `BinaryOperator::Create()` | `llvm/IR/Instructions.h` |

---

### 调用上下文

```text
hoistRegion()
  -> hoistArithmetics(I, ...)
     -> hoistBOAssociation(I, ...)  // 本函数（行 2966）
```

---

### 统计项

变换成功时递增 `NumBOAssociationsHoisted`（`LICM.cpp:116-117`）：

```cpp
STATISTIC(NumBOAssociationsHoisted, "Number of invariant BinaryOp expressions "
                                    "reassociated and hoisted out of the loop");
```

---

### `hoistMulAddAssociation` 与 `hoistBOAssociation` 的对比

| 特性 | `hoistMulAddAssociation` | `hoistBOAssociation` |
|---|---|---|
| **处理的模式** | `((A1*B1)+(A2*B2)+...) * C` | `(LV op C1) op C2` 等四种两层嵌套 |
| **变换本质** | 分配律：将公共因子分配到每个项 | 结合律：重新分组不变量 |
| **遍历深度** | 任意深度（BFS 遍历整棵树） | 固定两层 |
| **支持运算** | Mul/Add 或 FMul/FAdd | 所有关联运算（add/mul/and/or/xor/fadd/fmul） |
| **变换数量上限** | `IntAssociationUpperLimit` / `FPAssociationUpperLimit`（默认 5） | 无上限（单次只处理一个模式） |
| **Poison flags** | 清除所有经过的 Add 的 poison flags | 通过 `OverflowTracking` 保守合并 |
| **FMF flags** | `CreateFMulFMF` 继承源指令 flags | 取两个指令的 FMF 交集 |
| **复杂度** | O(K)，K 为表达式树节点数 | O(1)，固定两层 |
| **适用场景** | 多项式乘以不变量 | 简单的两层嵌套不变量合并 |

---

## 函数分析：`ControlFlowHoister::registerPossiblyHoistableBranch`（行 675-730）

### 函数签名与目的

```cpp
void registerPossiblyHoistableBranch(BranchInst *BI)
```

**功能**：识别并注册循环内可被提升的条件分支。如果分支条件是不变量，且控制流结构允许复制（收敛到一个公共后继），则将其记录到 `HoistableBranches` 映射中，为后续提升条件执行的指令和 PHI 节点做准备。

---

### 整体结构

```
registerPossiblyHoistableBranch(BI)
├── 1. 基本检查：开关、条件分支、操作数不变
├── 2. 目标块检查：都在循环内、不重合
├── 3. 寻找公共后继 (CommonSucc)
│   ├── 三角形模式：TrueDest 是 FalseDest 的后继（或反之）
│   └── 菱形模式：两个目标块有且仅有一个公共后继
└── 4. 支配性检查：BI 支配 CommonSucc → 注册到 HoistableBranches
```

---

### 逐段注释

**1. 基本检查（行 677-679）**

```cpp
if (!ControlFlowHoisting || !BI->isConditional() ||
    !CurLoop->hasLoopInvariantOperands(BI))
  return;
```

- 必须开启 `-licm-control-flow-hoisting`。
- 必须是条件分支。
- 分支条件必须是循环不变量（这是提升的前提）。

**2. 目标块检查（行 684-688）**

```cpp
BasicBlock *TrueDest = BI->getSuccessor(0);
BasicBlock *FalseDest = BI->getSuccessor(1);
if (!CurLoop->contains(TrueDest) || !CurLoop->contains(FalseDest) ||
    TrueDest == FalseDest)
  return;
```

- 两个目标块都必须在循环内。
- 目标块不能相同（否则等价于无条件分支，无需复制）。

**3. 寻找公共后继（行 695-718）**

- **三角形模式**：如果 `TrueDest` 是 `FalseDest` 的后继（或反之），则公共后继就是那个被指向的块。
- **菱形模式**：计算两个目标块后继集合的交集。
  - 如果交集为空，无法提升。
  - 如果有一个公共后继，直接使用。
  - 如果有多个公共后继，选择函数中顺序靠前的那个（`llvm::find_if`），保证确定性。

**4. 支配性检查与注册（行 728-729）**

```cpp
if (CommonSucc && DT->dominates(BI, CommonSucc))
  HoistableBranches[BI] = CommonSucc;
```

- 分支指令必须支配公共后继。
- **为什么？** 确保复制分支后，控制流语义正确。如果 BI 不支配 CommonSucc，说明存在其他路径到达 CommonSucc 而不经过 BI，复制分支会导致语义错误。同时，这也避免了将回边（back-edge）错误地包含在提升的控制流中。

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `HoistableBranches` | `DenseMap<BranchInst *, BasicBlock *>`，记录可提升分支及其收敛点 |

---

### 优化意图

1. **识别可复制结构**：只有三角形或菱形结构才能安全复制，避免复杂 CFG 带来的正确性问题。
2. **保证支配关系**：通过支配性检查确保提升后的控制流与原逻辑等价。

---

## 函数分析：`ControlFlowHoister::canHoistPHI`（行 732-767）

### 函数签名与目的

```cpp
bool canHoistPHI(PHINode *PN)
```

**功能**：判断 PHI 节点是否可以随控制流一起被提升。如果 PHI 的所有前驱都被可提升分支覆盖，且 PHI 操作数不变，则可以提升。

---

### 整体结构

```
canHoistPHI(PN)
├── 1. 检查 PHI 操作数是否循环不变
├── 2. 收集 PHI 所在块的所有前驱
├── 3. 检查前驱是否被 HoistableBranches 覆盖
│   ├── 遍历已注册的可提升分支
│   ├── 若分支目标是 PHI 所在块，根据模式移除对应前驱
│   └── 若所有前驱被移除 → 返回 true
└── 返回 false
```

---

### 逐段注释

**1. 不变性检查（行 734-735）**

```cpp
if (!ControlFlowHoisting || !CurLoop->hasLoopInvariantOperands(PN))
  return false;
```

- PHI 的所有 incoming value 必须循环不变。

**2. 前驱覆盖检查（行 738-766）**

```cpp
BasicBlock *BB = PN->getParent();
SmallPtrSet<BasicBlock *, 8> PredecessorBlocks(llvm::from_range, predecessors(BB));
...
for (auto &Pair : HoistableBranches) {
  if (Pair.second == BB) {
    // 根据分支模式移除前驱
    ...
  }
}
return PredecessorBlocks.empty();
```

- 收集 PHI 所在块 `BB` 的所有前驱。
- 遍历 `HoistableBranches`，如果某个分支的收敛点是 `BB`，说明该分支控制着进入 `BB` 的路径。
- **去重逻辑**：
  - **三角形模式**：分支的一个目标是 `BB`，另一个目标是 `BB` 的前驱。这两个前驱实际上来自同一个分支决策。需要从集合中移除这两个块。
  - **菱形模式**：分支的两个目标都是 `BB` 的前驱。移除这两个块。
- 如果最终集合为空，说明所有进入 `BB` 的路径都被可提升分支覆盖，PHI 可以安全提升。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 前驱必须完全覆盖 | 只要有一个前驱未被覆盖，PHI 就不能提升 | 否则提升后 PHI 的某些 incoming value 无法确定 |
| 分支模式匹配 | 必须正确识别三角形/菱形模式 | 错误识别会导致前驱移除错误，破坏 SSA |

---

## 函数分析：`ControlFlowHoister::getOrCreateHoistedBlock`（行 769-880）

### 函数签名与目的

```cpp
BasicBlock *getOrCreateHoistedBlock(BasicBlock *BB)
```

**功能**：懒加载机制，获取基本块 `BB` 在循环外（Preheader 区域）对应的影子块。如果不存在，则创建并链接，同时更新 DominatorTree、LoopInfo 和 MemorySSA。

---

### 整体结构

```
getOrCreateHoistedBlock(BB)
├── 1. 缓存检查：若已存在，直接返回
├── 2. 关联分支：查找 BB 是哪个可提升分支的目标
├── 3. 若无关，返回 Preheader
├── 4. 递归创建影子块
│   ├── CreateHoistedBlock: 创建 BB，更新 DT/LI
│   └── 递归创建 TrueDest, FalseDest, CommonSucc 的影子块
├── 5. 链接影子 CFG
│   ├── moveBefore, BranchInst::Create
├── 6. 更新 Preheader（若替换）
│   ├── 更新 PHI 使用、DT、MSSA
├── 7. 克隆分支指令
│   ├── 创建新分支，复制 md_prof，设置 DebugLoc
└── 返回影子块
```

---

### 逐段注释

**1. 缓存与开关（行 770-774）**

```cpp
if (!ControlFlowHoisting) return CurLoop->getLoopPreheader();
if (auto It = HoistDestinationMap.find(BB); It != HoistDestinationMap.end())
  return It->second;
```

- 功能关闭时回退到 Preheader。
- 命中缓存直接返回。

**2. 关联分支（行 777-782）**

```cpp
auto HasBBAsSuccessor = [&](...) { ... };
auto It = llvm::find_if(HoistableBranches, HasBBAsSuccessor);
```

- 查找 `BB` 是哪个已注册分支的目标。如果没找到，说明 `BB` 不参与可提升控制流，直接返回 Preheader。

**3. 递归创建（行 799-824）**

```cpp
BasicBlock *HoistTrueDest = CreateHoistedBlock(TrueDest);
BasicBlock *HoistFalseDest = CreateHoistedBlock(FalseDest);
BasicBlock *HoistCommonSucc = CreateHoistedBlock(CommonSucc);
```

- `CreateHoistedBlock` Lambda：
  - 创建新块 `OrigName.licm`。
  - `DT->addNewBlock(New, HoistTarget)`：更新支配树。
  - `CurLoop->getParentLoop()->addBasicBlockToLoop`：加入外层循环。
  - 递增 `NumCreatedBlocks`。
- 递归调用确保依赖的影子块先被创建。

**4. 链接影子 CFG（行 827-842）**

- 将新块插入到正确位置 (`moveBefore`)。
- 创建分支指令连接它们，形成完整的影子控制流。

**5. Preheader 更新（行 846-859）**

```cpp
if (HoistTarget == InitialPreheader) {
  InitialPreheader->replaceSuccessorsPhiUsesWith(HoistCommonSucc);
  MSSAU.wireOldPredecessorsToNewImmediatePredecessor(...);
  DT->changeImmediateDominator(HeaderNode, PreheaderNode);
  ...
}
```

- 如果影子 CommonSucc 替换了原来的 Preheader：
  - 更新循环头 PHI 的使用，指向新 Preheader。
  - 更新 MemorySSA 的前驱关系。
  - 更新 DominatorTree，新 Preheader 支配循环头。
  - 更新 `HoistDestinationMap` 中其他映射到新 Preheader 的条目。

**6. 克隆分支（行 863-873）**

```cpp
auto *NewBI = BranchInst::Create(HoistTrueDest, HoistFalseDest, BI->getCondition(), ...);
HoistTarget->getTerminator()->eraseFromParent();
NewBI->copyMetadata(*BI, {LLVMContext::MD_prof});
NewBI->setDebugLoc(...);
```

- 在影子 Preheader 创建新的分支指令。
- 删除旧的 terminator。
- 复制 `md_prof` 元数据（分支概率），因为条件不变，概率不变。
- 设置 DebugLoc。
- 递增 `NumClonedBranches`。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 创建基本块 | `BasicBlock::Create()` | `LICM.cpp:811` |
| 更新支配树 | `DT->addNewBlock()`, `changeImmediateDominator()` | `LICM.cpp:813, 854` |
| 更新 LoopInfo | `addBasicBlockToLoop()` | `LICM.cpp:815` |
| 更新 MemorySSA | `MSSAU.wireOldPredecessorsToNewImmediatePredecessor()` | `LICM.cpp:849` |
| 克隆分支指令 | `BranchInst::Create()`, `copyMetadata()` | `LICM.cpp:864, 870` |
| 统计项 | `NumCreatedBlocks`, `NumClonedBranches` | `LICM.cpp:96, 97` |

---

### 约束与局限性

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅支持简单模式 | 仅处理三角形/菱形收敛 | 复杂 CFG 无法提升 |
| 代码膨胀 | 复制基本块和分支指令 | 增加代码体积，可能影响 I-Cache |
| 分析同步 | 需手动更新 DT、LI、MSSA | 实现复杂，易引入 IR 不一致 Bug |

---

### 总结

`ControlFlowHoister` 通过**惰性 CFG 复制**（Lazy CFG Duplication），在 Preheader 区域构建循环不变分支的"影子图"，使条件执行的不变量得以安全提升。其核心在于：
1. **`registerPossiblyHoistableBranch`**：识别可复制的控制流模式。
2. **`canHoistPHI`**：验证 PHI 节点是否可随控制流提升。
3. **`getOrCreateHoistedBlock`**：懒加载创建影子块并同步所有分析结构（DT, LI, MSSA）。

该机制显著增强了 LICM 的能力，但默认关闭（`-licm-control-flow-hoisting=false`），因为涉及复杂的 CFG 变换和分析同步，存在较高的实现复杂度和潜在风险。

---

## 类分析：`ControlFlowHoister`（行 654-881）

### 1. 类概述与作用

**定位**：`ControlFlowHoister` 是 LICM `hoistRegion` 阶段的辅助类，用于突破传统 LICM 只能将指令提升到循环 Preheader 的限制。

**核心问题**：标准 LICM 要求被提升的指令在所有循环迭代路径上都必然执行（`isGuaranteedToExecute`）。如果指令位于循环不变条件分支的某一侧（如 `if (inv) { x = a + b; }`），标准 LICM 无法提升，因为提升到 Preheader 会改变语义（原条件为假时不执行，提升后无条件执行）。

**解决方案**：通过在 Preheader 区域**复制循环内的不变控制流结构**，为条件执行的指令创建对应的"影子基本块"。指令被提升到影子块中，从而保持原有的条件语义，同时移出循环体。

**开关控制**：由命令行参数 `-licm-control-flow-hoisting` 控制（默认 `false`）。

---

### 2. 核心数据结构

| 成员 | 类型 | 作用 |
|---|---|---|
| `HoistDestinationMap` | `DenseMap<BasicBlock *, BasicBlock *>` | 缓存原循环块到对应影子块的映射。若块已创建影子块，直接返回；否则按需创建。 |
| `HoistableBranches` | `DenseMap<BranchInst *, BasicBlock *>` | 记录可提升的条件分支及其收敛点（Common Successor）。键为循环内的分支指令，值为两个分支路径汇合的块。 |
| `LI`, `DT`, `CurLoop`, `MSSAU` | 指针/引用 | 维护 LoopInfo、DominatorTree、当前循环、MemorySSA 更新器，用于 CFG 变换时的分析结构同步。 |

---

### 3. 关键方法分析

#### 3.1 `registerPossiblyHoistableBranch(BI)`（行 675-730）
**作用**：在遍历循环块时，识别并注册可提升的条件分支。

**合法性检查**：
1. 分支必须是条件分支且操作数循环不变。
2. 两个目标块都必须在循环内，且不能相同。
3. **收敛点查找**：两个目标块必须共享一个公共后继 `CommonSucc`。支持三种模式：
   - 三角形：`TrueDest` 直接跳到 `FalseDest`（或反之）。
   - 菱形：两个目标块都跳到同一个 `CommonSucc`。
4. **支配关系**：分支指令必须支配 `CommonSucc`，确保控制流复制后不会引入错误路径。

**设计意图**：仅处理简单收敛模式，避免复杂 CFG 复制带来的正确性风险和编译时开销。

#### 3.2 `canHoistPHI(PN)`（行 732-767）
**作用**：判断 PHI 节点是否可以随控制流一起提升。

**检查逻辑**：
1. PHI 的所有操作数必须循环不变。
2. PHI 所在块的所有前驱必须都被 `HoistableBranches` 覆盖。
3. 处理三角形/菱形结构的前驱去重（同一块可能通过不同边到达）。

**意义**：允许提升由不变分支控制的 PHI 节点，进一步暴露提升机会。

#### 3.3 `getOrCreateHoistedBlock(BB)`（行 769-880）
**作用**：懒加载创建影子控制流结构，返回 `BB` 对应的提升目标块。

**执行流程**：
1. **缓存命中**：若 `BB` 已映射，直接返回。
2. **非分支块**：若 `BB` 不参与可提升分支，直接返回 `Preheader`。
3. **触发 CFG 复制**：
   - 递归创建分支父块、TrueDest、FalseDest、CommonSucc 的影子块。
   - 新建块命名规则：`OrigName.licm`。
   - 链接影子块：`HoistTarget` → 克隆分支 → `TrueDest.licm`/`FalseDest.licm` → `CommonSucc.licm`。
4. **更新分析结构**：
   - `DT->addNewBlock()`：维护支配树。
   - `CurLoop->getParentLoop()->addBasicBlockToLoop()`：将影子块加入外层循环。
   - `MSSAU.wireOldPredecessorsToNewImmediatePredecessor()`：同步 MemorySSA。
   - 若替换了 Preheader，更新循环头的前驱 PHI 和支配关系。
5. **克隆分支**：复制原分支指令到影子 Preheader，保留 `md_prof` 元数据。

---

### 4. 工作流程与 IR 变换示例

**调用时机**：在 `hoistRegion` 的 RPO 遍历中：
- 提升指令时：`hoist(I, ..., CFH.getOrCreateHoistedBlock(BB), ...)`
- 检查 PHI 时：`CFH.canHoistPHI(PN)`
- 遍历末尾：`CFH.registerPossiblyHoistableBranch(BI)`

**变换示例**：

```llvm
; === 变换前 ===
loop.preheader:
  br label %loop.header
loop.header:
  br i1 %inv_cond, label %loop.true, label %loop.false
loop.true:
  %val = add i32 %invariant_a, %invariant_b  ; 条件执行，标准 LICM 无法提升
  br label %loop.merge
loop.false:
  br label %loop.merge
loop.merge:
  %res = phi i32 [%val, %loop.true], [0, %loop.false]
  br label %loop.header

; === 变换后 (licm-control-flow-hoisting=true) ===
loop.preheader:
  ; 复制不变分支到循环外
  br i1 %inv_cond, label %loop.true.licm, label %loop.false.licm
loop.true.licm:
  %val.hoisted = add i32 %invariant_a, %invariant_b  ; 成功提升
  br label %loop.merge.licm
loop.false.licm:
  br label %loop.merge.licm
loop.merge.licm:
  br label %loop.header
loop.header:
  br i1 %inv_cond, label %loop.true, label %loop.false  ; 原分支保留（若未被 DCE 清除）
...
```

---

### 5. 约束与局限性

| 约束 | 说明 | 风险/影响 |
|---|---|---|
| **默认关闭** | `-licm-control-flow-hoisting=false` | 生产环境默认不启用，需显式开启 |
| **模式限制** | 仅支持三角形/菱形收敛，要求分支支配汇合点 | 复杂控制流（如多分支汇合、交叉边）无法处理 |
| **代码膨胀** | 复制基本块和分支指令 | 增加代码体积，可能影响 I-Cache |
| **分析同步** | 需手动更新 DT、LI、MSSA、PHI | 实现复杂，易引入 IR 不一致 Bug |
| **Rehoist 修正** | 提升后可能不支配所有 Use，需 `moveInstructionBefore` 修正 | 增加 Pass 复杂度 |

---

### 6. 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 分支收敛检查 | `DT->dominates(BI, CommonSucc)` | `LICM.cpp:728` |
| 创建新基本块 | `BasicBlock::Create()` | `LICM.cpp:811` |
| 更新支配树 | `DT->addNewBlock()`, `changeImmediateDominator()` | `LICM.cpp:813, 854` |
| 更新 LoopInfo | `addBasicBlockToLoop()` | `LICM.cpp:815` |
| 更新 MemorySSA | `MSSAU.wireOldPredecessorsToNewImmediatePredecessor()` | `LICM.cpp:849` |
| 克隆分支指令 | `BranchInst::Create()`, `copyMetadata()` | `LICM.cpp:864, 870` |
| 统计项 | `NumCreatedBlocks`, `NumClonedBranches` | `LICM.cpp:96, 97` |

---

### 7. 总结

**设计精髓**：通过**惰性 CFG 复制**（Lazy CFG Duplication），在 Preheader 区域构建循环不变分支的"影子图"，使条件执行的不变量得以安全提升。

**最值得深挖的点**：
1. **Rehoist 修正逻辑**（行 1023-1045）：提升到影子块后，若指令不支配循环内某些 Use，需将其重新移动到支配者块。该逻辑与 `ControlFlowHoisting` 紧密耦合，是保证正确性的关键。
2. **与 InstCombine/GVN 的交互**：提升后的影子块可能产生冗余计算，依赖后续 Pass 清理；若影子分支最终被证明总是走同一路径，可能引入死代码。

---

## 函数分析：`collectPromotionCandidates`（行 2237-2302）

### 函数签名与目的

```cpp
static SmallVector<PointersAndHasReadsOutsideSet, 0>
collectPromotionCandidates(MemorySSA *MSSA, AliasAnalysis *AA, Loop *L)
```

**功能**：收集循环内可被提升为标量的内存访问候选集。通过 `AliasSetTracker` 将循环内的 load/store 按别名关系分组，筛选出 must-alias 且包含写操作（mod）的指针集合，供后续 `promoteLoopAccessesToScalars` 处理。

---

### 整体结构

```
collectPromotionCandidates(MSSA, AA, L)
├── 1. 初始化 AliasSetTracker (AST)
├── 2. 遍历循环内所有内存访问
│   └── IsPotentiallyPromotable 检查：
│       ├── 必须是 Load 或 Store
│       ├── 指针操作数不能是常量数据
│       └── 指针必须是循环不变量
├── 3. 筛选感兴趣的 AliasSet
│   └── 条件：非 forwarding + isMod() + isMustAlias()
├── 4. 排除有冲突外部访问的集合
│   └── 遍历非提升访问，检查是否与候选集冲突：
│       ├── isModSet(MR) → 直接丢弃（外部写破坏 must-alias 假设）
│       ├── isRefSet(MR) → 标记 HasReadsOutsideSet=true
│       └── 若集合仅有 mod 无 ref 且有外部读 → 丢弃
└── 5. 构建结果：(PointerMustAliases, HasReadsOutsideSet)
```

---

### 逐段注释

**1. 初始化与遍历（行 2239-2261）**

```cpp
BatchAAResults BatchAA(*AA);
AliasSetTracker AST(BatchAA);

auto IsPotentiallyPromotable = [L](const Instruction *I) {
  if (const auto *SI = dyn_cast<StoreInst>(I)) {
    const Value *PtrOp = SI->getPointerOperand();
    return !isa<ConstantData>(PtrOp) && L->isLoopInvariant(PtrOp);
  }
  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    const Value *PtrOp = LI->getPointerOperand();
    return !isa<ConstantData>(PtrOp) && L->isLoopInvariant(PtrOp);
  }
  return false;
};

SmallPtrSet<Value *, 16> AttemptingPromotion;
foreachMemoryAccess(MSSA, L, [&](Instruction *I) {
  if (IsPotentiallyPromotable(I)) {
    AttemptingPromotion.insert(I);
    AST.add(I);
  }
});
```

- `BatchAAResults`：批量缓存别名分析结果，减少重复查询。
- `IsPotentiallyPromotable`：检查指令是否为"可能可提升"的 load/store。要求指针操作数不是常量数据（如 `ConstantData`），且指针本身是循环不变量。
- `AttemptingPromotion`：记录哪些指令参与了提升候选，用于后续冲突检测时排除自身。
- `foreachMemoryAccess`：遍历循环内所有 MemorySSA 访问，提取对应的指令。

**2. 筛选 must-alias + mod 集合（行 2264-2267）**

```cpp
SmallVector<PointerIntPair<const AliasSet *, 1, bool>, 8> Sets;
for (AliasSet &AS : AST)
  if (!AS.isForwardingAliasSet() && AS.isMod() && AS.isMustAlias())
    Sets.push_back({&AS, false});
```

- 只关注 **must-alias** 集合（所有指针指向同一内存位置）。
- 必须包含 **mod**（写操作），因为只有读写混合的集合才有提升价值（纯读集合不需要 store 到 exit block）。
- `isForwardingAliasSet()` 为 true 表示该集合已合并到其他集合，跳过。
- `PointerIntPair` 的 `bool` 位记录 `HasReadsOutsideSet`。

**3. 排除有冲突外部访问的集合（行 2273-2291）**

```cpp
foreachMemoryAccess(MSSA, L, [&](Instruction *I) {
  if (AttemptingPromotion.contains(I))
    return;

  llvm::erase_if(Sets, [&](auto &Pair) {
    ModRefInfo MR = Pair.getPointer()->aliasesUnknownInst(I, BatchAA);
    if (isModSet(MR))
      return true;  // 外部写 → 丢弃
    if (isRefSet(MR)) {
      Pair.setInt(true);  // 标记有外部读
      return !Pair.getPointer()->isRef();  // 集合仅有 mod 且有外部读 → 丢弃
    }
    return false;
  });
});
```

- 遍历循环内**未参与提升**的指令（如 call、volatile load 等）。
- `aliasesUnknownInst`：检查该指令是否与集合中的指针有别名冲突。
- **外部写（isModSet）**：直接丢弃集合。因为循环内存在无法追踪的写操作，must-alias 假设被破坏。
- **外部读（isRefSet）**：
  - 标记 `HasReadsOutsideSet = true`。
  - 如果集合只有 mod（无 ref）且有外部读，说明集合内的 store 值可能被外部读观察到，提升 store 会改变语义 → 丢弃。
  - 如果集合既有 mod 又有 ref（即有 load），则只提升 load（只读提升），不提升 store。

**4. 构建结果（行 2293-2301）**

```cpp
SmallVector<std::pair<SmallSetVector<Value *, 8>, bool>, 0> Result;
for (auto [Set, HasReadsOutsideSet] : Sets) {
  SmallSetVector<Value *, 8> PointerMustAliases;
  for (const auto &MemLoc : *Set)
    PointerMustAliases.insert(const_cast<Value *>(MemLoc.Ptr));
  Result.emplace_back(std::move(PointerMustAliases), HasReadsOutsideSet);
}
return Result;
```

- 将每个 `AliasSet` 转换为 `PointerMustAliases`（指针集合）和 `HasReadsOutsideSet`（是否有外部读）。
- 返回结果供 `promoteLoopAccessesToScalars` 使用。

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `AliasSetTracker` | 别名集合追踪器，将内存访问按别名关系分组 |
| `AliasSet` | 别名集合，包含一组 must/may/no-alias 的指针 |
| `BatchAAResults` | 批量缓存别名分析结果 |
| `PointerIntPair<const AliasSet *, 1, bool>` | 存储 AliasSet 指针 + HasReadsOutsideSet 标志 |

---

### 优化意图

1. **must-alias 过滤**：只有 must-alias 集合才能安全提升，因为提升后所有访问都指向同一个标量变量。may-alias 集合无法保证这一点。
2. **外部访问检测**：循环内可能有无法追踪的内存访问（如 call 指令），需要检查它们是否与候选集冲突。
3. **只读提升支持**：当集合有外部读时，仍然可以只提升 load（`HasReadsOutsideSet = true`），但不提升 store。

---

## 函数分析：`promoteLoopAccessesToScalars`（行 1911-2224）

### 函数签名与目的

```cpp
bool llvm::promoteLoopAccessesToScalars(
    const SmallSetVector<Value *, 8> &PointerMustAliases,
    SmallVectorImpl<BasicBlock *> &ExitBlocks,
    SmallVectorImpl<BasicBlock::iterator> &InsertPts,
    SmallVectorImpl<MemoryAccess *> &MSSAInsertPts, PredIteratorCache &PIC,
    LoopInfo *LI, DominatorTree *DT, AssumptionCache *AC,
    const TargetLibraryInfo *TLI, TargetTransformInfo *TTI, Loop *CurLoop,
    MemorySSAUpdater &MSSAU, ICFLoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE, bool AllowSpeculation,
    bool HasReadsOutsideSet)
```

**功能**：将循环内对同一内存位置（must-alias 指针集合）的反复 load/store 提升为寄存器标量操作。核心变换：
1. 在 preheader 插入一次 load（`.promoted`）
2. 循环内所有 load 替换为 SSA 值
3. 循环内所有 store 删除，在 exit block 插入 store
4. 使用 `SSAUpdater` 维护 SSA 形式

---

### 整体结构

```
promoteLoopAccessesToScalars(...)
├── 1. 安全性检查（dereferenceable + store safety）
│   ├── p1: preheader 处可解引用
│   │   ├── 有 guaranteed store → 满足
│   │   ├── 有 guaranteed load → 满足
│   │   └── isSafeToExecuteUnconditionally → 满足
│   └── p2: exit block 处 store 合法
│       ├── 有 guaranteed store → 满足
│       ├── 线程本地对象 → 满足
│       └── store 支配所有 exit block → 满足
├── 2. 收集循环内所有 load/store
│   ├── 检查类型一致性
│   ├── 收集对齐和 AA 信息
│   └── 检查原子性一致性
├── 3. 最终安全检查
│   ├── 混合原子/非原子 → 返回 false
│   ├── 无法证明 dereferenceable → 返回 false
│   └── StoreSafety 仍为 Unknown → 检查线程本地
├── 4. 执行提升
│   ├── 创建 LoopPromoter（继承 LoadAndStorePromoter）
│   ├── 在 preheader 创建 promoted load
│   ├── SSA.AddAvailableValue(Preheader, PreheaderLoad)
│   ├── Promoter.run(LoopUses) → 重写 load、删除 store
│   └── 清理未使用的 preheader load
└── 返回 true
```

---

### 逐段注释

**1. 安全性检查框架（行 1936-1983）**

```cpp
// p1: preheader 处可解引用（load 安全）
// p2: exit block 处 store 合法（不引入新写路径）
//
// 如果至少有一个 store guaranteed to execute，两个条件都满足。
// 否则需要分别证明。

bool DereferenceableInPH = false;
bool StoreIsGuaranteedToExecute = false;
bool LoadIsGuaranteedToExecute = false;
bool FoundLoadToPromote = false;

enum { StoreSafe, StoreUnsafe, StoreSafetyUnknown } StoreSafety = StoreSafetyUnknown;
```

- **p1（dereferenceable）**：preheader 中插入的 load 必须安全。如果循环内某个 store guaranteed to execute，说明指针在循环内可解引用，preheader 也可解引用。
- **p2（store safety）**：exit block 中插入的 store 不能引入新的写路径。有三种方式证明：
  - 有 guaranteed store
  - 对象是线程本地的（memory model 不适用）
  - store 支配所有 exit block

**2. 异常路径检查（行 2002-2011）**

```cpp
if (StoreSafety == StoreSafetyUnknown && SafetyInfo->anyBlockMayThrow()) {
  Value *Object = getUnderlyingObject(SomePtr);
  if (!isNotVisibleOnUnwindInLoop(Object, CurLoop, DT))
    StoreSafety = StoreUnsafe;
}
```

- 如果循环可能抛出异常，需要证明调用者在 unwind 后无法访问该对象（否则插入的 store 可能被观察到）。
- `isNotVisibleOnUnwindInLoop`：检查对象是否在 unwind 时对外不可见。

**3. 收集循环内访问（行 2016-2115）**

```cpp
for (Value *ASIV : PointerMustAliases) {
  for (Use &U : ASIV->uses()) {
    Instruction *UI = dyn_cast<Instruction>(U.getUser());
    if (!UI || !CurLoop->contains(UI))
      continue;

    if (LoadInst *Load = dyn_cast<LoadInst>(UI)) {
      if (!Load->isUnordered()) return false;  // volatile/ordered 不能提升
      ...
      // 检查 dereferenceable
      if (isSafeToExecuteUnconditionally(*Load, ...)) {
        DereferenceableInPH = true;
        Alignment = std::max(Alignment, InstAlignment);
      }
    } else if (const StoreInst *Store = dyn_cast<StoreInst>(UI)) {
      ...
      // 检查 store safety
      if (GuaranteedToExecute) {
        DereferenceableInPH = true;
        StoreSafety = StoreSafe;
      }
      if (StoreSafety == StoreSafetyUnknown &&
          llvm::all_of(ExitBlocks, [&](BasicBlock *Exit) {
            return DT->dominates(Store->getParent(), Exit);
          }))
        StoreSafety = StoreSafe;
    }
    ...
    LoopUses.push_back(UI);
  }
}
```

- 遍历 must-alias 集合中每个指针的所有使用。
- 跳过循环外的指令。
- **Load**：检查是否 unordered，收集对齐信息，检查是否 guaranteed to execute 或 safe to speculate。
- **Store**：检查是否 unordered，收集对齐信息，检查是否 guaranteed to execute 或支配所有 exit block。
- **类型一致性**：所有 load/store 必须使用相同的访问类型（`AccessTy`），否则返回 false。
- **AA 标签合并**：合并所有 load/store 的 AA metadata。

**4. 原子性一致性检查（行 2117-2128）**

```cpp
if (SawUnorderedAtomic && SawNotAtomic)
  return false;  // 混合原子/非原子 → 不能提升

if (SawUnorderedAtomic && Alignment < MDL.getTypeStoreSize(AccessTy))
  return false;  // 原子操作需要自然对齐
```

- 不能混合提升 atomic 和 non-atomic 访问：无法确定应该使用哪种原子序。
- 原子操作要求自然对齐，否则无法 lower。

**5. 线程本地对象检查（行 2140-2148）**

```cpp
if (StoreSafety == StoreSafetyUnknown) {
  Value *Object = getUnderlyingObject(SomePtr);
  bool ExplicitlyDereferenceableOnly;
  if (isWritableObject(Object, ExplicitlyDereferenceableOnly) &&
      (!ExplicitlyDereferenceableOnly || isDereferenceablePointer(SomePtr, AccessTy, MDL)) &&
      isThreadLocalObject(Object, CurLoop, DT, TTI))
    StoreSafety = StoreSafe;
}
```

- 如果之前的检查都未能证明 store safety，尝试证明对象是线程本地且可写的。
- `isThreadLocalObject`：检查对象是否是函数局部且未在循环内被捕获，或者目标是单线程。

**6. 只读提升回退（行 2150-2154）**

```cpp
if (StoreSafety != StoreSafe && !FoundLoadToPromote)
  return false;  // 既不能提升 store 也没有 load → 放弃
```

- 如果 store 不安全但找到了 load，仍然可以只提升 load（只读提升）。
- 如果连 load 都没有，直接返回 false。

**7. 执行提升（行 2156-2223）**

```cpp
// 创建 SSAUpdater 和 LoopPromoter
SSAUpdater SSA(&NewPHIs);
LoopPromoter Promoter(SomePtr, LoopUses, SSA, ExitBlocks, InsertPts,
                      MSSAInsertPts, PIC, MSSAU, *LI, DL, Alignment,
                      SawUnorderedAtomic,
                      StoreIsGuaranteedToExecute ? AATags : AAMDNodes(),
                      *SafetyInfo, StoreSafety == StoreSafe);

// 在 preheader 创建 promoted load
if (FoundLoadToPromote || !StoreIsGuanteedToExecute) {
  PreheaderLoad = new LoadInst(AccessTy, SomePtr, ..., Preheader->getTerminator()->getIterator());
  ...
  SSA.AddAvailableValue(Preheader, PreheaderLoad);
} else {
  SSA.AddAvailableValue(Preheader, PoisonValue::get(AccessTy));
}

// 重写循环内所有 load/store
Promoter.run(LoopUses);

// 清理未使用的 preheader load
if (PreheaderLoad && PreheaderLoad->use_empty())
  eraseInstruction(*PreheaderLoad, *SafetyInfo, MSSAU);
```

- `LoopPromoter` 继承自 `LoadAndStorePromoter`，是 `SSAUpdater` 的定制回调。
- 在 preheader 创建 promoted load（如果 store guaranteed to execute 则不需要，因为值来自 store）。
- `Promoter.run()` 遍历所有 loop uses：
  - 对 load：用 `SSA.GetValueInMiddleOfBlock` 替换为 SSA 值。
  - 对 store：调用 `SSA.AddAvailableValue` 注册定义，然后删除 store。
- `doExtraRewritesBeforeFinalDeletion()`：在 exit block 插入 store（如果 `CanInsertStoresInExitBlocks` 为 true）。

---

### `LoopPromoter` 类（行 1756-1870）

`LoopPromoter` 是 `LoadAndStorePromoter` 的子类，提供定制化的 promotion 行为：

| 方法 | 作用 |
|---|---|
| `maybeInsertLCSSAPHI` | 如果值定义在循环内但需要在循环外使用，创建 LCSSA PHI |
| `insertStoresInLoopExitBlocks` | 在每个 exit block 插入 store，将最终值写回内存 |
| `doExtraRewritesBeforeFinalDeletion` | 在最终删除前调用，触发 exit block store 插入 |
| `instructionDeleted` | 指令删除时更新 SafetyInfo 和 MemorySSA |
| `shouldDelete` | 判断指令是否应被删除：store 仅在 `CanInsertStoresInExitBlocks` 为 true 时删除 |

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `LoopPromoter` | 继承 `LoadAndStorePromoter`，定制 promotion 行为 |
| `SSAUpdater` | 维护 SSA 形式，自动插入 PHI 节点 |
| `LoadAndStorePromoter` | 基类，提供 load/store 重写的通用逻辑 |

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 类型一致性 | 所有 load/store 必须使用相同类型 | 不同类型无法共享同一个标量变量 |
| 原子性一致性 | 不能混合 atomic 和 non-atomic | 无法确定正确的原子序 |
| 异常路径 | 循环可能 throw 时需要证明对象 unwind 不可见 | 否则插入的 store 可能被异常处理器观察到 |
| 外部写 | 循环内存在无法追踪的写操作时不能提升 | must-alias 假设被破坏 |
| 外部读 | 有外部读时只能提升 load，不能提升 store | store 会改变外部读观察到的值 |

---

### 调用关系

```
runOnLoop()
├── collectPromotionCandidates() → 返回 [(PointerMustAliases, HasReadsOutsideSet), ...]
└── for each candidate:
    └── promoteLoopAccessesToScalars()
        ├── LoopPromoter (SSAUpdater 回调)
        │   ├── insertStoresInLoopExitBlocks()
        │   ├── doExtraRewritesBeforeFinalDeletion()
        │   ├── instructionDeleted()
        │   └── shouldDelete()
        └── formLCSSARecursively() (在 runOnLoop 中调用)
```

---

### 统计项

| 统计项 | 含义 |
|---|---|
| `NumPromotionCandidates` | 尝试提升的候选集数量 |
| `NumLoadPromoted` | 只读提升（load only）的次数 |
| `NumLoadStorePromoted` | 完整提升（load + store）的次数 |

---

## 函数分析：`LoadAndStorePromoter::run`（`SSAUpdater.cpp` 行 369-515）

### 函数签名与目的

```cpp
void LoadAndStorePromoter::run(const SmallVectorImpl<Instruction *> &Insts)
```

**功能**：`LoadAndStorePromoter` 是 `SSAUpdater` 的定制化子类，专门用于将一组 load/store 指令提升为 SSA 形式的标量值。它首先处理块内的定义/使用顺序，确定每个块的"可用值"（Available Value）和需要 PHI 节点的"活入加载"（Live-in Loads），然后调用 `SSAUpdater` 插入 PHI 节点并重写所有使用，最后删除原始的 load/store 指令。

---

### 整体结构

```
LoadAndStorePromoter::run(Insts)
├── 1. 按基本块分组指令 (UsesByBlock)
├── 2. 遍历每个块，处理块内依赖
│   ├── 单指令块：直接注册 AvailableValue 或加入 LiveInLoads
│   ├── 纯 Load 块：全部加入 LiveInLoads
│   └── 混合 Load/Store 块：
│       ├── 按程序顺序排序
│       ├── 遍历：Load 在 Store 前 → LiveInLoads；Load 在 Store 后 → 替换为 StoredValue
│       └── 最后一个 Store 的值注册为 AvailableValue
├── 3. 重写所有 LiveInLoads
│   └── 调用 SSA.GetValueInMiddleOfBlock() 插入 PHI 并重写
├── 4. 调用 doExtraRewritesBeforeFinalDeletion() (钩子函数)
└── 5. 删除原始指令
    ├── 检查 shouldDelete()
    ├── 处理仍有使用的 Load (通过 ReplacedLoads 链式查找最终值)
    ├── 调用 instructionDeleted() (钩子函数)
    └── eraseFromParent()
```

---

### 逐段注释

**1. 按基本块分组（行 373-376）**

```cpp
DenseMap<BasicBlock *, TinyPtrVector<Instruction *>> UsesByBlock;
for (Instruction *User : Insts)
  UsesByBlock[User->getParent()].push_back(User);
```

- `SSAUpdater` 本身只处理跨块的数据流（通过 PHI 节点），块内的 def-use 顺序必须由调用者自行处理。
- 将传入的指令列表按所属基本块分组，便于后续按块处理。

**2. 遍历块处理块内依赖（行 381-468）**

```cpp
SmallVector<LoadInst *, 32> LiveInLoads;
DenseMap<Value *, Value *> ReplacedLoads;

for (Instruction *User : Insts) {
  BasicBlock *BB = User->getParent();
  TinyPtrVector<Instruction *> &BlockUses = UsesByBlock[BB];
  if (BlockUses.empty()) continue; // 已处理过该块
  ...
```

- `LiveInLoads`：收集那些需要使用"从块外流入的值"的 load 指令。
- `ReplacedLoads`：记录被替换的 load 及其新值，用于后续处理链式依赖（如 load 后 store 同一地址）。

**2a. 单指令块处理（行 393-407）**

```cpp
if (BlockUses.size() == 1) {
  if (StoreInst *SI = dyn_cast<StoreInst>(User)) {
    SSA.AddAvailableValue(BB, SI->getOperand(0));
  } else if (auto *AI = dyn_cast<AllocaInst>(User)) {
    SSA.AddAvailableValue(BB, getValueToUseForAlloca(AI));
  } else {
    LiveInLoads.push_back(cast<LoadInst>(User));
  }
  BlockUses.clear();
  continue;
}
```

- **Store**：该块定义了值，将 store 的源操作数注册为该块的 AvailableValue。
- **Alloca**：视为初始定义（通常用于 mem2reg）。
- **Load**：该块只读取值，加入 `LiveInLoads` 等待 PHI 插入。

**2b. 纯 Load 块处理（行 409-424）**

```cpp
bool HasStore = false;
for (Instruction *I : BlockUses) {
  if (isa<StoreInst>(I) || isa<AllocaInst>(I)) { HasStore = true; break; }
}
if (!HasStore) {
  for (Instruction *I : BlockUses)
    LiveInLoads.push_back(cast<LoadInst>(I));
  BlockUses.clear();
  continue;
}
```

- 如果块内全是 load，没有 store，说明这些 load 都依赖外部流入的值，全部加入 `LiveInLoads`。

**2c. 混合 Load/Store 块处理（行 426-467）**

```cpp
llvm::sort(BlockUses.begin(), BlockUses.end(),
           [](Instruction *A, Instruction *B) { return A->comesBefore(B); });

Value *StoredValue = nullptr;
for (Instruction *I : BlockUses) {
  if (LoadInst *L = dyn_cast<LoadInst>(I)) {
    if (StoredValue) {
      replaceLoadWithValue(L, StoredValue);
      L->replaceAllUsesWith(StoredValue);
      ReplacedLoads[L] = StoredValue;
    } else {
      LiveInLoads.push_back(L);
    }
    continue;
  }
  if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    StoredValue = SI->getOperand(0);
  }
  ...
}
SSA.AddAvailableValue(BB, StoredValue);
```

- **排序**：按指令在块中的物理顺序排序。
- **遍历**：
  - 遇到 Load：如果前面有 Store，说明它读的是块内定义的值，直接替换为 `StoredValue`；否则是活入值，加入 `LiveInLoads`。
  - 遇到 Store：更新 `StoredValue`，最后的 Store 决定了该块流出到后继块的值。
- **注册 AvailableValue**：将块内最后一个 Store 的值注册为该块的 AvailableValue。

**3. 重写 LiveInLoads（行 472-480）**

```cpp
for (LoadInst *ALoad : LiveInLoads) {
  Value *NewVal = SSA.GetValueInMiddleOfBlock(ALoad->getParent());
  replaceLoadWithValue(ALoad, NewVal);
  if (NewVal == ALoad) NewVal = PoisonValue::get(NewVal->getType());
  ALoad->replaceAllUsesWith(NewVal);
  ReplacedLoads[ALoad] = NewVal;
}
```

- 调用 `SSA.GetValueInMiddleOfBlock()`。该方法会检查前驱块的 AvailableValue，如果值不统一，则自动在块开头插入 PHI 节点。
- 用计算出的新值（可能是 PHI，也可能是单一前驱的值）替换 Load 的所有使用。

**4. 钩子函数与删除指令（行 483-514）**

```cpp
doExtraRewritesBeforeFinalDeletion();

for (Instruction *User : Insts) {
  if (!shouldDelete(User)) continue;
  if (!User->use_empty()) {
    Value *NewVal = ReplacedLoads[User];
    // 链式查找最终值
    DenseMap<Value*, Value*>::iterator RLI = ReplacedLoads.find(NewVal);
    while (RLI != ReplacedLoads.end()) {
      NewVal = RLI->second;
      RLI = ReplacedLoads.find(NewVal);
    }
    replaceLoadWithValue(cast<LoadInst>(User), NewVal);
    User->replaceAllUsesWith(NewVal);
  }
  instructionDeleted(User);
  User->eraseFromParent();
}
```

- `doExtraRewritesBeforeFinalDeletion()`：子类钩子。在 LICM 中，`LoopPromoter` 重写此方法以在 exit block 插入 store。
- **删除指令**：
  - 检查 `shouldDelete()`：在 LICM 中，如果 store 不能被 sink 到 exit block，则不删除。
  - **链式替换**：如果一个 Load 被替换后仍有使用（例如它被后续的 Store 读取，形成了 load-store 链），需要通过 `ReplacedLoads` 映射表递归查找最终的替换值。
  - 调用 `instructionDeleted()` 通知子类更新分析结构（如 LICM 更新 MemorySSA 和 SafetyInfo）。
  - `eraseFromParent()` 物理删除指令。

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `UsesByBlock` | `DenseMap<BB, TinyPtrVector<Inst *>>`，按块分组待处理的 load/store |
| `LiveInLoads` | 收集需要从块外（前驱）流入值的 load 指令 |
| `ReplacedLoads` | `DenseMap<Value *, Value *>`，记录被替换的 load 及其新值，用于处理链式依赖 |
| `SSAUpdater` | 核心 SSA 重建工具，负责插入 PHI 节点和查询 AvailableValue |

---

### 优化意图

1. **块内顺序敏感**：SSAUpdater 假设每个块最多只有一个定义。对于有多个 load/store 的块，必须先在块内解析 def-use 链，将块内能解决的 load 直接替换为 store 的值，剩下的 load 才交给 SSAUpdater 处理。
2. **链式依赖处理**：在内存提升场景中，经常出现 `load x; store y, x` 的模式。删除 load 时，如果它还有使用（被 store 读取），必须将其替换为 store 的值（或最终值），否则会产生悬空引用。
3. **钩子机制**：通过虚函数 `doExtraRewritesBeforeFinalDeletion`、`shouldDelete`、`instructionDeleted`，允许调用者（如 LICM）在标准 SSA 更新前后插入自定义逻辑（如插入 exit store、更新 MemorySSA）。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 块内顺序 | 必须按程序顺序处理块内的 load/store | 顺序错误会导致 load 读到旧值 |
| 链式替换 | 删除 Load 前必须检查 `use_empty()` 并递归替换 | 否则会导致 IR 中出现悬空指针，触发 verifier 失败 |
| 不可达代码 | `GetValueInMiddleOfBlock` 在不可达块可能返回原 Load | 需检查 `NewVal == ALoad` 并替换为 `PoisonValue` |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 注册块定义值 | `SSA.AddAvailableValue(BB, Val)` | `SSAUpdater.cpp:69` |
| 查询块中间值 | `SSA.GetValueInMiddleOfBlock(BB)` | `SSAUpdater.cpp:97` |
| 插入 PHI | `PHINode::Create()` (内部调用) | `SSAUpdater.cpp:158` |
| 替换 Load 值 | `replaceLoadWithValue()` (虚函数) | `SSAUpdater.h` |
| 删除前钩子 | `doExtraRewritesBeforeFinalDeletion()` (虚函数) | `SSAUpdater.h` |
| 删除后钩子 | `instructionDeleted()` (虚函数) | `SSAUpdater.h` |

---

### 与 LICM 的交互

在 LICM 中，`LoopPromoter` 继承自 `LoadAndStorePromoter` 并重写了以下虚函数：
- `insertStoresInLoopExitBlocks()`：在 `doExtraRewritesBeforeFinalDeletion()` 中调用，负责将提升后的最终值存回内存。
- `instructionDeleted()`：调用 `MSSAU.removeMemoryAccess()` 和 `SafetyInfo.removeInstruction()` 同步分析结构。
- `shouldDelete()`：仅当 `CanInsertStoresInExitBlocks` 为 true 时才删除 Store，否则保留 Store（只读提升场景）。

---

## 函数分析：`LoopPromoter::insertStoresInLoopExitBlocks`（行 1805-1853）

### 函数签名与目的

```cpp
void LoopPromoter::insertStoresInLoopExitBlocks()
```

**功能**：在内存提升（Promotion）的最后阶段，将提升后的标量值写回内存。由于循环可能有多个出口，该函数遍历所有出口块（Exit Blocks），并在每个出口块中插入 Store 指令，将 SSAUpdater 计算出的最终值存回原始内存地址。

---

### 整体结构

```
insertStoresInLoopExitBlocks()
├── 1. 遍历所有循环出口块 (LoopExitBlocks)
├── 2. 获取当前出口块的活跃值 (LiveInValue)
│   └── 调用 SSA.GetValueInMiddleOfBlock()
├── 3. 维护 LCSSA 形式
│   ├── maybeInsertLCSSAPHI(LiveInValue)
│   └── maybeInsertLCSSAPHI(Ptr)
├── 4. 创建并配置 Store 指令
│   ├── new StoreInst(...)
│   ├── 设置原子序、对齐、调试位置、AA 元数据
│   └── 合并 DIAssignID 元数据
└── 5. 更新 MemorySSA
    ├── 创建新的 MemoryDef
    └── 更新 MSSAInsertPts 以维持正确的内存依赖链
```

---

### 逐段注释

**1. 遍历出口块与获取值（行 1811-1815）**

```cpp
for (unsigned i = 0, e = LoopExitBlocks.size(); i != e; ++i) {
  BasicBlock *ExitBlock = LoopExitBlocks[i];
  Value *LiveInValue = SSA.GetValueInMiddleOfBlock(ExitBlock);
```

- `SSA.GetValueInMiddleOfBlock(ExitBlock)`：这是核心调用。它查询 SSAUpdater 获取该出口块处的标量值。
- **为什么是 Middle？** 因为 Store 插入在块的开头（`InsertPts[i]`），此时块内的 PHI 节点已经处理完毕，但非 PHI 指令尚未执行。该方法能正确处理出口块作为多个前驱汇合点的情况（可能需要插入 PHI）。

**2. 维护 LCSSA 形式（行 1816-1817）**

```cpp
LiveInValue = maybeInsertLCSSAPHI(LiveInValue, ExitBlock);
Value *Ptr = maybeInsertLCSSAPHI(SomePtr, ExitBlock);
```

- `maybeInsertLCSSAPHI`：检查值是否定义在循环内。如果是，且当前块在循环外，则必须插入 LCSSA PHI 节点，否则 SSA 形式会被破坏（值从循环内直接跳到循环外，没有经过 PHI）。
- 对 `SomePtr`（被提升的指针）也做同样处理，防止指针本身也是循环内的定义。

**3. 创建 Store 指令（行 1818-1838）**

```cpp
BasicBlock::iterator InsertPos = LoopInsertPts[i];
StoreInst *NewSI = new StoreInst(LiveInValue, Ptr, InsertPos);
if (UnorderedAtomic)
  NewSI->setOrdering(AtomicOrdering::Unordered);
NewSI->setAlignment(Alignment);
NewSI->setDebugLoc(DL);
// ... 处理 DIAssignID 和 AA 元数据 ...
```

- 在预定的插入位置创建 Store。
- **元数据继承**：
  - **原子序**：如果原始访问包含无序原子操作，新 Store 也设为 `Unordered`。
  - **对齐**：使用收集到的最大对齐值。
  - **DIAssignID**：合并原始指令的调试赋值 ID，确保调试信息连续性。
  - **AA 元数据**：继承别名分析元数据（如 TBAA）。

**4. 更新 MemorySSA（行 1840-1851）**

```cpp
MemoryAccess *MSSAInsertPoint = MSSAInsertPts[i];
MemoryAccess *NewMemAcc;
if (!MSSAInsertPoint) {
  NewMemAcc = MSSAU.createMemoryAccessInBB(
      NewSI, nullptr, NewSI->getParent(), MemorySSA::Beginning);
} else {
  NewMemAcc = MSSAU.createMemoryAccessAfter(NewSI, nullptr, MSSAInsertPoint);
}
MSSAInsertPts[i] = NewMemAcc;
MSSAU.insertDef(cast<MemoryDef>(NewMemAcc), true);
```

- **MemorySSA 链式更新**：
  - 如果是第一个出口块（`MSSAInsertPoint` 为空），在块开头创建 MemoryAccess。
  - 否则，在前一个出口块插入的 Store 之后创建（`createMemoryAccessAfter`）。这确保了不同出口块之间的 Store 在 MemorySSA 中有正确的顺序关系（尽管控制流上它们是互斥的，但在 MemorySSA 中通常表现为汇聚到同一个 Phi 或 Def 链）。
  - `MSSAInsertPts[i] = NewMemAcc`：更新插入点，为下一次迭代（如果有）或后续操作提供正确的锚点。
  - `MSSAU.insertDef`：将新的 MemoryDef 正式插入 MemorySSA 图，触发必要的重命名（Rename）。

---

### 关键数据结构

| 结构 | 含义 |
|---|---|
| `SSAUpdater` | 负责计算出口块处的标量值，必要时插入 PHI |
| `LoopExitBlocks` | 循环的所有出口块列表 |
| `LoopInsertPts` | 每个出口块中插入 Store 的位置（通常是第一个非 PHI 指令前） |
| `MSSAInsertPts` | 每个出口块中插入 MemorySSA 节点的锚点 |

---

### 优化意图

1. **完成内存提升闭环**：LICM 将循环内的 Load/Store 替换为寄存器操作，但必须在循环退出时将结果写回内存，以保证循环外代码的正确性。
2. **多出口处理**：循环可能有多个出口（如 `break`、异常、正常结束）。该函数确保无论通过哪个路径退出，内存都能被正确更新。
3. **保持 IR 规范性**：通过插入 LCSSA PHI 和维护 MemorySSA，确保变换后的 IR 仍然符合 LLVM 的标准形式，便于后续 Pass 处理。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| LCSSA 维护 | 必须为跨边界的值插入 PHI | 否则 verifier 会报错，后续 Pass 可能崩溃 |
| MemorySSA 顺序 | 多个出口的 Store 需正确链接 | 错误的 MemorySSA 会导致别名分析错误 |
| 元数据一致性 | 对齐、原子序必须匹配原访问 | 不匹配可能导致代码生成错误或语义改变 |

---

---

## 工具函数分析：`llvm::make_early_inc_range`

### 函数签名与目的（STLExtras.h:632-639）

```cpp
template <typename RangeT>
iterator_range<early_inc_iterator_impl<detail::IterOfRange<RangeT>>>
make_early_inc_range(RangeT &&Range) {
  using EarlyIncIteratorT =
      early_inc_iterator_impl<detail::IterOfRange<RangeT>>;
  return make_range(EarlyIncIteratorT(adl_begin(Range)),
                    EarlyIncIteratorT(adl_end(Range)));
}
```

**功能**: 创建一个"提前递增"（early increment）的范围适配器，使得在遍历容器（如 `BasicBlock` 中的指令列表）时可以**安全地删除或移动当前元素**，而不会破坏迭代器的有效性或导致遍历遗漏。

---

### 为什么需要这个函数

在 LICM 的 `hoistRegion()` 和 `sinkRegion()` 中，遍历循环体内的指令时，需要根据条件**移动或删除**当前指令：

```cpp
// hoistRegion 行 921
for (Instruction &I : llvm::make_early_inc_range(*BB)) {
  if (/* 满足提升条件 */) {
    hoist(I, ...);  // 将 I 移动到 preheader
    // I 被移动后，迭代器必须仍然有效，且指向下一条指令
  }
}

// sinkRegion 行 583
for (BasicBlock::iterator II = BB->end(); II != BB->begin();) {
  Instruction &I = *--II;
  if (isInstructionTriviallyDead(&I, TLI)) {
    ++II;  // 手动先递增
    eraseInstruction(I, ...);  // 再删除
    continue;
  }
}
```

如果使用普通的 range-based for 循环，当循环体内部删除/移动 `I` 后，底层迭代器会失效，导致未定义行为。

---

### 核心机制：`early_inc_iterator_impl`（STLExtras.h:577-618）

```cpp
template <typename WrappedIteratorT>
class early_inc_iterator_impl
    : public iterator_adaptor_base<early_inc_iterator_impl<WrappedIteratorT>,
                                   WrappedIteratorT, std::input_iterator_tag> {
  using BaseT = typename early_inc_iterator_impl::iterator_adaptor_base;
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  bool IsEarlyIncremented = false;
#endif

public:
  early_inc_iterator_impl(WrappedIteratorT I) : BaseT(I) {}

  // 关键：解引用时返回当前元素，同时递增底层迭代器
  decltype(*std::declval<WrappedIteratorT>()) operator*() {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(!IsEarlyIncremented && "Cannot dereference twice!");
    IsEarlyIncremented = true;
#endif
    return *(this->I)++;  // ← 核心：先解引用，再递增底层迭代器
  }

  // 递增操作（实际为空，因为 operator* 已递增）
  early_inc_iterator_impl &operator++() {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(IsEarlyIncremented && "Cannot increment before dereferencing!");
    IsEarlyIncremented = false;
#endif
    return *this;
  }

  // 比较操作
  friend bool operator==(const early_inc_iterator_impl &LHS,
                         const early_inc_iterator_impl &RHS) {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(!LHS.IsEarlyIncremented && "Cannot compare after dereferencing!");
#endif
    return (const BaseT &)LHS == (const BaseT &)RHS;
  }
};
```

---

### 工作流程与状态机

该迭代器实现了一个简单的两阶段状态机：

#### 状态转换

```
┌─────────────┐          operator*()           ┌──────────────────┐
│  初始状态    │ ────────────────────────────→ │  已解引用状态     │
│ (可解引用、  │  1. 返回当前元素引用           │ (可递增、不可再   │
│   可比较)    │  2. 底层迭代器指向下一个元素   │   解引用、不可比较)│
└─────────────┘                               └───────────────────┘
       ↑                                              │
       │                                              │ operator++()
       └───────────────────────←──────────────────────┘
```

#### Range-based for 循环展开

```cpp
for (Instruction &I : llvm::make_early_inc_range(*BB)) {
  // 循环体
}
```

等价于：

```cpp
auto __range = llvm::make_early_inc_range(*BB);
auto __it = __range.begin();    // early_inc_iterator，底层指向 BB 第一条指令
auto __end = __range.end();

while (__it != __end) {          // Step 1: 比较（迭代器在初始状态）
  Instruction &I = *__it;        // Step 2: 解引用（触发 operator*）
                                 //   - 返回当前指令 I
                                 //   - 底层迭代器自动 ++ → 指向下一条指令
  // 循环体开始 -------------------┐
  hoist(I, ...);                 // 可以安全删除/移动 I
                                 // 此时底层迭代器已指向下一条，不受影响
  // 循环体结束 -------------------┘
  ++__it;                        // Step 3: 递增（实际是空操作，底层已递增）
                                   // 将状态重置回初始状态，准备下一轮比较
}
```

**关键时序**：

```
迭代器底层位置： [A] → [B] → [C] → [D] → end
                 ↑    ↑    ↑    ↑
循环体执行：      │    │    │    │
                 ├─删除 A    │    │
                 │    ├─删除 B    │
                 │    │    ├─删除 C
                 │    │    │    └─删除 D
```

---

### 为什么可以安全删除当前元素

**核心原理**：在用户代码访问当前元素（`*__it`）的**同一时刻**，底层迭代器已经移动到下一个位置。

```cpp
// operator* 的实现
return *(this->I)++;  // 顺序：1. 解引用 *I；2. 递增 I
```

**普通迭代器的问题**：

```cpp
// 普通迭代器
for (auto it = list.begin(); it != list.end(); ++it) {
  auto &elem = *it;      // it 仍指向 elem
  list.erase(it);        // it 失效，后续遍历崩溃
}
```

**early_inc_iterator 的安全保障**：

```cpp
// early_inc_iterator
for (auto &elem : make_early_inc_range(list)) {
  // 此时底层迭代器已经指向下一个元素
  list.erase(&elem);     // 删除当前元素，但迭代器已经跳过它
  // 下一轮 ++it 是空操作，状态重置即可
}
```

**内存安全保证**：只要删除操作**不释放内存**（或释放发生在迭代器不访问该内存之后），就是安全的。在 LICM 中：
- `hoist()` 使用 `moveBefore()` 将指令移动到其他块，原位置从容器移除但指令对象本身不销毁
- `eraseInstruction()` 确实会销毁指令，但此时迭代器早已指向下一条，且 `eraseInstruction` 在 LICM 中调用时：
  - 对于 sink 场景，迭代器在调用 `eraseInstruction` **之前**已经 `++II`（手动递增）
  - 对于 hoist 场景，`hoist()` 后通过 `continue` 跳过后续代码，`make_early_inc_range` 的迭代器在下次循环开始时仍处于已递增状态

---

### 使用模式约束

该迭代器**仅适用于特定的使用模式**，违反模式会导致未定义行为：

| 操作 | 允许时机 | 禁止时机 | 后果 |
|---|---|---|---|
| `*it` (解引用) | 初始状态 ✓ | 已解引用状态 ✗ | 断言失败（debug）或未定义行为 |
| `++it` (递增) | 已解引用状态 ✓ | 初始状态 ✗ | 断言失败（debug）或未定义行为 |
| `it == end` (比较) | 初始状态 ✓ | 已解引用状态 ✗ | 断言失败（debug）或未定义行为 |
| 多次解引用 | 任何状态 ✗ | - | 第二次会触发断言 |

**合法循环模式**：

```cpp
// 模式 1: range-based for (标准用法)
for (auto &elem : make_early_inc_range(container)) {
  // 循环体内可以安全删除 elem
  // 每次循环：compare → deref(自动递增) → body → increment(空操作)
}

// 模式 2: 手动循环（等价展开）
auto it = begin, end;
while (it != end) {      // 比较（初始状态）
  auto &elem = *it;      // 解引用 → 底层迭代器自动++
  // 循环体（可删除 elem）
  ++it;                  // 递增（重置状态）
}
```

**非法模式示例**：

```cpp
// ❌ 错误：未解引用就递增
auto it = make_early_inc_range(container).begin();
++it;  // 断言失败：IsEarlyIncremented 为 false

// ❌ 错误：解引用两次
for (auto &elem : make_early_inc_range(container)) {
  use(elem);     // 第一次解引用（底层 ++）
  use(elem);     // 第二次解引用 → 断言失败
}

// ❌ 错误：解引用后继续比较（range-based for 不会，但手动循环可能）
auto &elem = *it;  // 底层 ++
if (&elem == something) { ... }  // 此时 it 已非初始状态，比较可能有问题
```

---

### 与手动递增模式的对比

**LICM 中的两种遍历模式**：

#### 1. `make_early_inc_range`（正向 hoist）

```cpp
// hoistRegion 行 921
for (Instruction &I : llvm::make_early_inc_range(*BB)) {
  if (canHoist(I)) {
    hoist(I, ...);    // 移动 I 到 preheader
    continue;         // 迭代器已指向下一条，安全
  }
}
```

#### 2. 手动反向遍历 + 显式 `++II`（sink 场景）

```cpp
// sinkRegion 行 583
for (BasicBlock::iterator II = BB->end(); II != BB->begin();) {
  Instruction &I = *--II;     // 手动递减获取元素
  if (isDead(I)) {
    ++II;                     // 先递增迭代器
    eraseInstruction(I, ...); // 再删除（此时迭代器已指向下一条）
    continue;
  }
}
```

**为什么 sinkRegion 不用 `make_early_inc_range`**？

因为 sinkRegion 采用**反向遍历**（从 BB 末尾向前），而 `make_early_inc_range` 是正向递增适配器。反向遍历需要：
- 从 `BB->end()` 开始
- 每次 `--II` 获取前一条指令
- 删除前必须 `++II` 将迭代器"回退"到当前位置的下一条（逻辑上的前一条）

这种模式与 `early_inc` 的"解引用即前进"语义不匹配，所以 sinkRegion 采用传统的反向迭代器模式，手动控制递增点。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 提前递增范围创建 | `llvm::make_early_inc_range(Range)` | `llvm/ADT/STLExtras.h:634` |
| 提前递增迭代器 | `early_inc_iterator_impl` | `llvm/ADT/STLExtras.h:578` |
| 迭代器适配基类 | `iterator_adaptor_base` | `llvm/ADT/iterator.h` |
| Debug 检查开关 | `LLVM_ENABLE_ABI_BREAKING_CHECKS` | 编译时配置 |

---

### 设计权衡与适用场景

**适用场景**：
- 遍历时删除当前元素（但释放操作需谨慎）
- 遍历时移动当前元素到其他容器（指令被 hoist/sink）
- 容器结构在遍历过程中被修改，但不影响"下一个"迭代器

**不适用场景**：
- 需要访问刚删除的元素（已销毁）
- 需要在删除后仍能访问迭代器（迭代器已移动）
- 多线程并发修改同一容器（无同步）
- 需要双向遍历或随机访问

**性能考量**：
- `early_inc_iterator_impl` 是零开销抽象：`operator*` 内联后与普通迭代器差异仅在于多一次 `++` 操作
- Debug 模式下有 `IsEarlyIncremented` 状态检查（`LLVM_ENABLE_ABI_BREAKING_CHECKS`），Release 模式下无额外开销
- 相比手动管理迭代器递增，代码可读性显著提升

---

### 总结

`make_early_inc_range` 通过**"解引用即递增"**的迭代器设计，巧妙解决了"遍历中修改容器"的迭代器失效问题。其核心是：
1. 将"获取当前元素"和"移动到下一个"合并为一步
2. 用户代码在访问元素后，迭代器已自动指向下一条，删除当前元素不会影响后续遍历
3. 严格的状态机约束防止误用（debug 模式）
4. 专为 range-based for 设计，要求"恰好一次解引用 + 恰好一次递增"的精确模式

在 LICM 中，这允许 `hoistRegion()` 在单次正向遍历中安全地提升多条指令，无需像 `sinkRegion()` 那样手动管理迭代器位置，代码更清晰且不易出错。

---

## ControlFlowHoisting：支配不完整与 rehoist 修复场景分析（LICM.cpp:1024–1045）

### 触发条件

当 `-licm-control-flow-hoisting` 启用时（默认开启），LICM 会将循环内不变量计算提升到**条件分支块**而非 preheader，而非仅提升到 preheader。这种"条件提升"可能导致：**已提升的指令不支配其未被提升的使用点**，需要后续修复（rehoist）。

---

### 源码调用路径：为什么 `%brmerge` 会被提升到条件块？

| 行号 | 说明 |
|------|------|
| **LICM.cpp:933** | `hoist(I, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB), ...)` - 将指令 `I` 提升到 `BB` 对应的目标块 |
| **LICM.cpp:776-782** | `getOrCreateHoistedBlock(BB)` 检查 `BB` 是否为条件分支的后继：`HasBBAsSuccessor` |
| **LICM.cpp:794-803** | 若 `BB` 是某个可提升分支（`branchA`、`branchB`）的后继，则创建条件块（如 `else2.licm`） |

**测试用例 IR**（来自 `hoist-phi.ll:862`）：

```llvm
; @rehoist 示例
define void @rehoist(ptr %this, i32 %x, i1 %arg) {
entry:
  %sub = add nsw i32 %x, -1
  br label %loop

loop:
  br i1 %arg, label %if1, label %else1      ; 分支 A (branchA)

if1:
  call void %this(ptr %this)                 ; 有副作用
  br label %then1

else1:
  br label %then1

then1:
  %cmp = icmp eq i32 0, %sub                 ; 循环不变，可提升
  br i1 %cmp, label %end, label %else2       ; 分支 B (branchB)

else2:
  %brmerge = or i1 %cmp, true                ; 循环不变，使用 %cmp，可提升
  br i1 %brmerge, label %if3, label %end

if3:
  br label %end

end:
  br label %loop
}
```

---

### Hoist 阶段的执行流程（RPO 顺序）

| 步骤 | 处理的块 | 处理内容 | HoistableBranches 状态 | getOrCreateHoistedBlock(else2) 结果 |
|------|---------|----------|------------------------|------------------------------------|
| 1 | `loop` | `br i1 %arg, if1, else1` (branchA) | `branchA → then1` | - |
| 2 | `then1` | `br i1 %cmp, end, else2` (branchB) | `branchB → end` | - |
| 3 | `else2` | `%brmerge = or i1 %cmp, true` | - | **检测到 `else2` 是 branchB 的后继 → 创建 `else2.licm`** |

**关键判断逻辑**（LICM.cpp:777-782）：
```cpp
auto HasBBAsSuccessor = [&](DenseMap<BranchInst *, BasicBlock *>::value_type &Pair) {
  return BB != Pair.second && 
         (Pair.first->getSuccessor(0) == BB ||   // TrueDest == else2?
          Pair.first->getSuccessor(1) == BB);    // FalseDest == else2?
};
auto It = llvm::find_if(HoistableBranches, HasBBAsSuccessor);
// 对于 else2，branchB 的 FalseDest 正是 else2 → It != end()
```

因此，`%brmerge` 被提升到新创建的 `else2.licm` 块（条件块）中。

---

### 问题根源：支配关系被破坏

**原始控制流**：
```
        loop
       /    \
     if1    else1
       \    /
       then1
       /    \
    end     else2 ← %brmerge 在这里
```

**提升后的控制流（问题状态）**：
```
        entry
       /    \
  if1.licm  else1.licm    ← 条件块，相互独立
      \    /
    then1.licm
           \
       else2.licm ← %brmerge 被提升到这里！
```

**核心问题**：
| 指令 | 提升位置 | 支配关系 | 问题 |
|------|---------|---------|------|
| `%cmp` | `entry` | ✓ `entry` 支配所有块 | 正常 |
| `%brmerge` | `if1.licm.else2.licm` | ✗ `if1.licm.else2.licm` **不支配** `else1.licm` | 如果 `%brmerge` 有使用者在其他分支，会违反支配关系 |

条件块之间互不支配——`if1.licm` 和 `else1.licm` 是兄弟关系，它们各自的子块也无法互相支配。如果某个指令被提升到一个条件块中，而该指令的使用者在另一个不相交的条件块中，那么该指令就不再支配其使用点，违反了 SSA 和 Dominator Tree 的基本约束。

---

### Rehoist 修复逻辑（LICM.cpp:1016-1045）

```cpp
// If we hoisted instructions to a conditional block they may not dominate
// their uses that weren't hoisted (such as phis where some operands are not
// loop invariant). If so make them unconditional by moving them to their
// immediate dominator.
Instruction *HoistPoint = nullptr;
if (ControlFlowHoisting) {
  for (Instruction *I : reverse(HoistedInstructions)) {
    // 检查：已提升的指令是否支配所有使用点
    if (!llvm::all_of(I->uses(),
                      [&](Use &U) { return DT->dominates(I, U); })) {
      // 不支配 → 重新提升到直接支配块
      BasicBlock *Dominator =
          DT->getNode(I->getParent())->getIDom()->getBlock();
      if (!HoistPoint || !DT->dominates(HoistPoint->getParent(), Dominator)) {
        if (HoistPoint)
          assert(DT->dominates(Dominator, HoistPoint->getParent()) &&
                 "New hoist point expected to dominate old hoist point");
        HoistPoint = Dominator->getTerminator();
      }
      LLVM_DEBUG(dbgs() << "LICM rehoisting to "
                        << HoistPoint->getParent()->getNameOrAsOperand()
                        << ": " << *I << "\n");
      moveInstructionBefore(*I, HoistPoint->getIterator(), ...)  // 行 1039
      HoistPoint = I;                                         // 行 1041
      Changed = true;
    }
  }
}
```

**修复步骤**：
1. **反向遍历**：按 `reverse(HoistedInstructions)` 顺序，确保先修复使用者，再修复被使用者（避免重复修复）
2. **支配检查**：`DT->dominates(I, U)` 验证每个使用点是否都被定义点支配
3. **定位支配块**：`getIDom()->getBlock()` 获取当前块的直接支配者
4. **更新 HoistPoint**：将 `HoistPoint` 移动到新的支配块终止符处
5. **MoveBefore**：行 1039 将指令移动到 `HoistPoint` 之前，使其在新的位置上支配所有使用点

---

### 具体场景解析：为什么 `%brmerge` 需要 rehoist？

假设 `%brmerge` 在使用链中有某个使用者不在 `else2.licm` 分支下：

**Rehoist 前**：
```
entry:
  %cmp = icmp eq i32 0, %sub             ; 在 entry
  br i1 %arg, label %if1.licm, label %else1.licm

if1.licm:
  br label %then1.licm

else1.licm:
  br label %then1.licm

else2.licm:                             ; 属于 if1.licm 的子树
  %brmerge = or i1 %cmp, true           ; 在 else2.licm
  br i1 %brmerge, label %if3.licm, label %end.licm
```

此时，如果有一个 PHI 节点在 `then1.licm` 中使用 `%brmerge`：
- `%brmerge` 的定义块是 `else2.licm`
- `else2.licm` 的被支配者是 `if1.licm`
- 但 `then1.licm` 的前驱包括 `else1.licm`，它**不被** `else2.licm` 支配
- **结论**：`else2.licm` 不支配 `then1.licm`，违反支配关系

**Rehoist 后**：
```
entry:
  %cmp = icmp eq i32 0, %sub             ; 在 entry
  %brmerge = or i1 %cmp, true           ; 被 rehoist 到 entry!
  br i1 %arg, label %if1.licm, label %else1.licm
```

现在 `%brmerge` 在 `entry` 块，`entry` 支配所有块，满足支配关系。

---

### 总结：触发 LICM.cpp:1039 行的场景

| 条件 | 说明 | 源码行号 |
|------|------|----------|
| `ControlFlowHoisting` 启用 | `-licm-control-flow-hoisting=1`（默认开启） | LICM.cpp:668 |
| 存在可提升的条件分支 | 循环内有循环不变的条件分支，且分支后继有共同后继 | LICM.cpp:675-729 |
| 指令被提升到条件块 | 指令所在块是某个条件分支的后继，触发 `getOrCreateHoistedBlock(BB)` 创建条件块 | LICM.cpp:933, 769-879 |
| 使用点未被提升 | 指令的使用者在另一条不相交的条件分支中 | - |
| 支配检查失败 | `DT->dominates(I, U)` 返回 false | LICM.cpp:1026-1027 |
| **执行 Rehoist** | 移动到直接支配块，恢复支配完整性 | **LICM.cpp:1039** |

**调试命令**：
```bash
opt -passes="licm" -debug-only=licm input.ll 2>&1 | grep -i rehoist
# 输出：LICM rehoisting to entry: %brmerge = or i1 %cmp, true
```

**相关测试**：
- `llvm/test/Transforms/LICM/hoist-phi.ll:@rehoist`（行 862）
- `llvm/test/Transforms/LICM/hoist-phi.ll:@phi_conditional_use`（行 1260）
- `llvm/test/Transforms/LICM/hoist-phi.ll:@rehoist_wrong_order_*`（行 1381+）

---

## MemorySSA 更新逻辑深度解析

### MemorySSA 的本质：内存操作的 Use-Def 链

MemorySSA 是一个描述**内存操作依赖关系**的 SSA 形式，核心是回答"这个 load/store 可能看到/覆盖哪些内存状态"。

```
MemoryUse(Load A)  ─── defining access ───> MemoryDef(Store B)
                                    "Load A 看到的内存状态可能被 Store B 定义"
```

#### 三种 MemoryAccess 类型

| 类型 | 代表 | 关键字段 | 语义 |
|---|---|---|---|
| `MemoryUse` | `load`, `readonly call` | `definingAccess` | 这个 load 可能看到 defining access 定义的值 |
| `MemoryDef` | `store`, `write call` | `definingAccess` | 这个 store 覆盖了 defining access 的状态，产生新状态 |
| `MemoryPhi` | CFG merge point | `incoming values` (多个前驱) | 在控制流汇合处合并内存状态 |

**核心约束**：`definingAccess` 必须支配当前的 MemoryAccess。

---

### 为什么 LICM 需要更新 MemorySSA

LICM 移动指令后，**内存操作的执行顺序和支配关系发生了变化**，MemorySSA 必须同步更新，否则：

1. **MemoryUse 的 defining access 可能不再支配它** → MemorySSA 验证失败
2. **依赖关系语义错误** → 后续 Pass 做出错误的别名判断
3. **MemoryPhi 的 incoming values 错误** → 反映错误的控制流

---

### 具体场景分析

#### 场景 1：提升 load 到 preheader

**原始 IR + MemorySSA**：

```
preheader:
  ...
  br loop.header

loop.header:
  ; MemoryPhi %phi = [liveOnEntry, preheader], [def.store, loop.body]
  
loop.body:
  store X, ptr    ; MemoryDef %def.store -> %phi
  load ptr        ; MemoryUse %use.load -> %phi (假设无 clobber)
  
exit:
```

**问题**：`use.load` 的 defining access 是 `%phi`（在 loop.header），现在 load 移到 preheader：

```
preheader:
  load ptr        ; 移到这里！
  ...
  br loop.header
```

**问题分析**：
- `%phi` 在 loop.header，**不支配** preheader 中的 load
- MemorySSA 约束被破坏：defining access 必须支配 use
- 需要更新 `use.load` 的 defining access 为 `liveOnEntry` 或 preheader 之前的某个 `MemoryDef`

**更新逻辑**（`moveInstructionBefore()` 中）：

```cpp
// LICM.cpp:1463-1466
if (MemoryUseOrDef *OldMemAcc = cast_or_null<MemoryUseOrDef>(
        MSSAU.getMemorySSA()->getMemoryAccess(&I)))
  MSSAU.moveToPlace(OldMemAcc, Dest->getParent(),
                    MemorySSA::BeforeTerminator);
```

`moveToPlace` 内部做了什么：

1. 从原位置**移除** MemoryAccess（从原 BB 的 access list 中删除）
2. **插入**到新 BB 的指定位置
3. **重新计算 defining access**：调用 `getClobberingMemoryAccess` 查找新位置最近的 clobber
4. **更新依赖链**：如果定义为 `MemoryDef`，还需更新后续依赖它的 MemoryUse

---

#### 场景 2：删除 store（MemoryDef）

**原始**：

```
loop.body:
  store X, ptr    ; MemoryDef %def1 -> %phi
  load ptr        ; MemoryUse %use -> %def1 (依赖 store)
  store Y, ptr    ; MemoryDef %def2 -> %def1
```

**删除 `store X` 后**：

```
loop.body:
  load ptr        ; 原来的 MemoryUse，现在 defining access 错误！
  store Y, ptr    ; MemoryDef %def2 -> 原来指向 %def1，现在 %def1 被删除了
```

**问题分析**：
- `MemoryDef %def1` 被删除后，依赖它的 `MemoryUse %use` 和 `MemoryDef %def2` 的 defining access 变成 dangling
- 需要**修复依赖链**：让后续的 MemoryUse/Def 指向 `%def1` 的 defining access（即 `%phi`）

**更新逻辑**（`eraseInstruction()` 中）：

```cpp
// LICM.cpp:1449-1454
static void eraseInstruction(Instruction &I, ICFLoopSafetyInfo &SafetyInfo,
                             MemorySSAUpdater &MSSAU) {
  MSSAU.removeMemoryAccess(&I);   // 先移除 MemoryAccess
  SafetyInfo.removeInstruction(&I);
  I.eraseFromParent();            // 再删除 IR 指令
}
```

**要点**：
- **必须先** `removeMemoryAccess` 再 `eraseFromParent`
- `removeMemoryAccess` 会清理 MemoryUse/Def 及相关 Phi 节点

---

#### 场景 3：克隆 load 到出口块

**原始**（sink 场景）：

```
loop.header:
  ; MemoryPhi
  
loop.body:
  load ptr        ; MemoryUse -> phi
  
exit:
  phi [load_result, loop.body]  ; LCSSA phi
```

**克隆 load 到 exit**：

```
exit:
  load ptr        ; 新克隆的 load
  phi ...
```

**问题分析**：
- 新 load 需要创建新的 MemoryUse
- 新 MemoryUse 的 defining access 是什么？
  - 应该是 exit 块之前（来自 loop.body 的边）的 MemoryDef
  - 或者如果无 clobber，是 `liveOnEntry`

**更新逻辑**（`cloneInstructionInExitBlock()` 中）：

```cpp
// LICM.cpp:1411-1426
if (MSSAU.getMemorySSA()->getMemoryAccess(&I)) {
  MemoryAccess *NewMemAcc = MSSAU.createMemoryAccessInBB(
      New, nullptr, New->getParent(), MemorySSA::Beginning,
      /*CreationMustSucceed=*/false);
  if (NewMemAcc) {
    if (auto *MemDef = dyn_cast<MemoryDef>(NewMemAcc))
      MSSAU.insertDef(MemDef, /*RenameUses=*/true);
    else {
      auto *MemUse = cast<MemoryUse>(NewMemAcc);
      MSSAU.insertUse(MemUse, /*RenameUses=*/true);
    }
  }
}
```

**要点**：
- `CreationMustSucceed=false`：允许失败（指令可能已变成非内存操作）
- `RenameUses=true`：插入后自动执行 use renaming，确保 defining access 正确

---

### getClobberingMemoryAccess：核心查询

这是 MemorySSA 更新的关键函数，**本质是别名分析 + dominance walk**：

```cpp
// LICM.cpp:1151-1163
static MemoryAccess *getClobberingMemoryAccess(MemorySSA &MSSA,
                                                BatchAAResults &BAA,
                                                SinkAndHoistLICMFlags &Flags,
                                                MemoryUseOrDef *MA) {
  if (Flags.tooManyClobberingCalls())
    return MA->getDefiningAccess();  // fallback：放弃精确性，用保守值
  
  MemoryAccess *Source =
      MSSA.getSkipSelfWalker()->getClobberingMemoryAccess(MA, BAA);
  Flags.incrementClobberingCalls();
  return Source;
}
```

**语义**：给定一个 `MemoryUse/Def`，找到"最近的可能 clobber 它的 MemoryDef"。

**算法（简化）**：

```text
getClobberingMemoryAccess(MA):
  1. 从 MA 的 definingAccess 开始
  2. 向上 walk（沿 use-def 链）
  3. 对遇到的每个 MemoryDef：
     - 检查别名：MA 指令访问的地址 是否与 MemoryDef 指令访问的地址别名？
     - 如果别名：返回该 MemoryDef（这就是 clobber）
     - 如果不别名：继续向上 walk
  4. 最终到达 liveOnEntry：返回 liveOnEntry
```

**LICM 中的使用**：
- 在 `pointerInvalidatedByLoop`（行 2375）：检查 load 是否被 loop 内的 store invalidate
- 在 `noConflictingReadWrites`（行 2307）：检查 store 是否有冲突的读写
- 在 MemorySSAUpdater 的更新操作中：计算新的 defining access

---

### MemorySSAUpdater 的核心方法内部逻辑

#### `moveToPlace` 的完整逻辑

```cpp
// llvm/Analysis/MemorySSAUpdater.cpp (伪代码，展示核心逻辑)
void MemorySSAUpdater::moveToPlace(MemoryUseOrDef *MA, BasicBlock *BB,
                                   MemorySSA::InsertionPlace Where) {
  // Step 1: 从原位置移除
  removeFromLists(MA);
  
  // Step 2: 插入到新位置
  insertIntoLists(MA, BB, Where);
  
  // Step 3: 重新计算 defining access（关键！）
  if (auto *MU = dyn_cast<MemoryUse>(MA)) {
    MemoryAccess *NewDef = Walker->getClobberingMemoryAccess(MU);
    MU->setDefiningAccess(NewDef);
  } else if (auto *MD = dyn_cast<MemoryDef>(MA)) {
    MemoryAccess *NewDef = Walker->getClobberingMemoryAccess(MD);
    MD->setDefiningAccess(NewDef);
    updateDefiningAccesses(MD);  // 修复依赖 MD 的后续 MemoryAccess
  }
  
  // Step 4: 可能需要插入新的 MemoryPhi
  fixupPhis();
}
```

**关键点**：
- `MemoryDef` 移动比 `MemoryUse` 复杂：Def 移走后，依赖它的 Use/Def 可能需要重新指向
- `getClobberingMemoryAccess` 是更新 defining access 的核心

---

#### `removeMemoryAccess` 的完整逻辑

```cpp
void MemorySSAUpdater::removeMemoryAccess(Instruction *I) {
  MemoryAccess *MA = MSSA->getMemoryAccess(I);
  if (!MA) return;
  
  if (auto *MD = dyn_cast<MemoryDef>(MA)) {
    // Step 1: 找 MD 的 defining access
    MemoryAccess *DefOfMD = MD->getDefiningAccess();
    
    // Step 2: 找所有依赖 MD 的 MemoryUse/Def
    SmallVector<MemoryAccess *> Users;
    for (MemoryAccess *User : MSSA->allMemoryAccesses()) {
      if (User->getDefiningAccess() == MD)
        Users.push_back(User);
    }
    
    // Step 3: 重定向依赖
    for (MemoryAccess *User : Users) {
      User->setDefiningAccess(DefOfMD);
    }
    
    // Step 4: 删除 MD 本身
    removeFromLists(MD);
    
    // Step 5: 可能简化 MemoryPhi
    tryRemoveTrivialPhis();
  } else if (auto *MU = dyn_cast<MemoryUse>(MA)) {
    removeFromLists(MU);
  }
}
```

**关键点**：
- 删除 `MemoryDef` 时必须**修复依赖链**（redirect uses）
- 删除 `MemoryUse` 只需从 list 移除

---

#### `wireOldPredecessorsToNewImmediatePredecessor` 的逻辑

```cpp
void MemorySSAUpdater::wireOldPredecessorsToNewImmediatePredecessor(
    BasicBlock *OldBB, BasicBlock *NewBB, ArrayRef<BasicBlock*> NewPreds) {
  // 场景：
  //   原来：OldBB 有多个前驱 Pred1, Pred2, ...
  //   现在：NewBB 是新块，NewPreds 是 OldBB 的部分旧前驱
  //         NewBB -> OldBB
  //         其他前驱 -> OldBB
  
  if (MemoryPhi *OldPhi = MSSA->getBlockPhi(OldBB)) {
    MemoryPhi *NewPhi = MSSA->createMemoryPhi(NewBB);
    
    for (BasicBlock *Pred : NewPreds) {
      MemoryAccess *Incoming = OldPhi->getIncomingValue(Pred);
      NewPhi->addIncoming(Incoming, Pred);
      OldPhi->removeIncoming(Pred);
    }
    
    OldPhi->addIncoming(NewPhi, NewBB);
  }
}
```

**关键点**：
- CFG 变化时，MemoryPhi 的 incoming values 必须同步变化
- 本质是"拆分"一个 MemoryPhi 到两个

---

### LICM 中 MemorySSA 更新的根本动机

归纳起来，LICM 更新 MemorySSA 的根本原因是：

| 操作 | MemorySSA 变化 | 必须更新 |
|---|---|---|
| **指令移动** | MemoryAccess 的 BB 改变 → defining access 可能不再支配它 | 重新计算 defining access，修复 dominance |
| **MemoryDef 删除** | 依赖链断裂 → 后续 MemoryUse/Def 的 defining access dangling | 重定向到被删除 Def 的 defining access |
| **新指令插入** | 需要新的 MemoryUse/Def → defining access 未设置 | 创建并计算 defining access |
| **CFG 变化** | MemoryPhi 的 incoming 错误 → 反映错误的控制流 | 拆分/合并 MemoryPhi |

---

### 一个完整例子：load 提升的 MemorySSA 变化

**原始 IR**：

```llvm
define void @foo(ptr %p) {
entry:
  br label %loop.header

loop.header:
  %i = phi [0, entry], [%i.next, %loop.body]
  br label %loop.body

loop.body:
  store i32 1, ptr %q   ; 写另一个地址
  %v = load i32, ptr %p ; 读 %p，%p loop invariant
  %i.next = add %i, 1
  %cond = icmp %i.next, 100
  br i1 %cond, label %loop.header, label %exit

exit:
  ret void
}
```

**原始 MemorySSA**（简化）：

```
entry:      MemoryDef(liveOnEntry) [隐式]
            
loop.header: MemoryPhi %phi1 = [liveOnEntry, entry], [%def1, loop.body]

loop.body:  
            MemoryDef %def1 -> %phi1   ; store q
            MemoryUse %use1 -> %phi1   ; load p（getClobbering 发现 %phi1 是 LoE，无 clobber）
            
exit:       (无 MemoryAccess)
```

**LICM 提升 load 后的 IR**：

```llvm
entry:
  br label %loop.header

loop.header:
  %v = load i32, ptr %p  ; 提升到这里！
  %i = phi ...
  br label %loop.body

loop.body:
  store i32 1, ptr %q
  %i.next = add %i, 1
  ...
```

**更新后的 MemorySSA**：

```
entry:      MemoryDef(liveOnEntry)
            
loop.header: 
            MemoryUse %use1' -> liveOnEntry  ; load p，现在 defining access 是 LoE
            MemoryPhi %phi1 = [liveOnEntry, entry], [%def1, loop.body]
            
loop.body:  
            MemoryDef %def1 -> %phi1
            ; 原 %use1 被删除
```

**关键变化**：

1. `MemoryUse %use1` 从 `loop.body` 移到 `loop.header`
2. `use1` 的 defining access 从 `%phi1` 变为 `liveOnEntry`
   - 为什么？`getClobberingMemoryAccess` 在新位置（loop.header，在 phi 之前）查询，发现无 clobber
   - `%phi1` 不支配 `loop.header` 的 load（load 在 phi 之前），所以不能用 `%phi1`
3. 原 `loop.body` 的 `%use1` 被移除

---

### LICM 中 MemorySSA API 使用场景汇总

| API | 使用位置 | 场景 |
|---|---|---|
| `MSSAU.removeMemoryAccess(I)` | `eraseInstruction()` :1451 | 删除指令时移除其 MemoryAccess |
| `MSSAU.moveToPlace(OldMemAcc, BB, Where)` | `moveInstructionBefore()` :1465 | 移动指令时更新 MemoryAccess 位置 |
| `MSSAU.createMemoryAccessInBB()` | `cloneInstructionInExitBlock()` :1415, `LoopPromoter` :1843, `promoteLoopAccessesToScalars()` :2202 | 在指定块创建新的 MemoryAccess |
| `MSSAU.createMemoryAccessAfter()` | `LoopPromoter` :1847 | 在指定 MemoryAccess 之后创建 |
| `MSSAU.insertDef()` / `MSSAU.insertUse()` | `cloneInstructionInExitBlock()` :1420/1423, `LoopPromoter` :1850, `promoteLoopAccessesToScalars()` :2205 | 插入 MemoryDef/Use 并执行重命名 |
| `MSSAU.wireOldPredecessorsToNewImmediatePredecessor()` | `ControlFlowHoister` :849 | CFG 变化后连接新前驱到新块 |

---

### 正确性约束

| 操作 | 约束 | 违反后果 |
|---|---|---|
| 删除指令 | 先 `removeMemoryAccess` 后 `eraseFromParent` | MSSA dangling reference crash |
| 移动指令 | `moveToPlace` 必须在 IR 移动后调用 | MemoryAccess 与 IR 指令位置不一致 |
| 克隆指令 | `RenameUses=true` 确保 defining access 正确 | MemoryUse 指向错误的 MemoryDef |
| CFG 变化 | `wireOldPredecessorsToNewImmediatePredecessor` 更新 Phi | MemoryPhi incoming values 错误 |

---

### 验证机制

```cpp
if (VerifyMemorySSA)
  MSSA->verifyMemorySSA();  // 多处调用：行 547, 621, 1046, 2211, 2218
```

---

### PreservedAnalyses 处理

```cpp
auto PA = getLoopPassPreservedAnalyses();
PA.preserve<MemorySSAAnalysis>();  // 行 320, 360
```

**要点**：LICM 明式 preserve MemorySSA，后续 Pass 可复用更新后的结果。

---

### 深入点建议

如果你想继续深入 MemorySSA 的实现细节：

1. **`getClobberingMemoryAccess` 的实现**（`MemorySSA.cpp`）：核心是 alias walk + phi translation
2. **`MemorySSAUpdater::insertDef` 的 renaming 逻辑**：如何批量更新后续 MemoryUse
3. **`optimizeUses`**：MemorySSA 的优化 pass，会简化 defining access

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 创建 MemorySSAUpdater | `MemorySSAUpdater(MSSA)` | `llvm/Analysis/MemorySSAUpdater.h` |
| 移动 MemoryAccess | `moveToPlace()` | `MemorySSAUpdater.cpp` |
| 移除 MemoryAccess | `removeMemoryAccess()` | `MemorySSAUpdater.cpp` |
| 创建 MemoryAccess | `createMemoryAccessInBB()` | `MemorySSAUpdater.cpp` |
| 插入 MemoryDef/Use | `insertDef()`, `insertUse()` | `MemorySSAUpdater.cpp` |
| CFG 更新 | `wireOldPredecessorsToNewImmediatePredecessor()` | `MemorySSAUpdater.cpp` |
| Clobber 查询 | `getClobberingMemoryAccess()` | `MemorySSA.cpp` |
| 验证 MemorySSA | `verifyMemorySSA()` | `MemorySSA.cpp` |

---

## ICFLoopSafetyInfo 深度解析

### 核心作用

`ICFLoopSafetyInfo` 是 LICM 用来回答**"这个指令在循环内是否必然执行"**这一关键安全性问题的数据结构。它继承自 `LoopSafetyInfo`，但比简单的 `SimpleLoopSafetyInfo` 提供更精确的答案。

---

### 三大核心功能

| 功能 | API | 在 LICM 中的用途 |
|---|---|---|
| **判断指令是否必然执行** | `isGuaranteedToExecute(Inst, DT, CurLoop)` | 决定是否能将**有副作用**的指令（如 load、call）提升到 preheader |
| **判断是否之前无内存写** | `doesNotWriteMemoryBefore(Inst, CurLoop)` | 决定是否能提升 `invariant.start` 或 `guard` intrinsic |
| **判断块是否可能抛异常** | `blockMayThrow(BB)` / `anyBlockMayThrow()` | 影响"guaranteed to execute"的判断逻辑 |

---

### 关键数据结构（MustExecute.h:133-140）

```cpp
class ICFLoopSafetyInfo : public LoopSafetyInfo {
  bool MayThrow = false;                           // 循环是否可能抛异常
  mutable ImplicitControlFlowTracking ICF;         // 隐式控制流追踪
  mutable MemoryWriteTracking MW;                 // 内存写追踪
  DenseMap<BasicBlock *, ColorVector> BlockColors; // Windows EH funclet 着色
};
```

**两个关键辅助对象**：

- **`ImplicitControlFlowTracking` (ICF)**：追踪哪些块包含"隐式控制流"——即可能不将执行传递到后继的指令（如 `call that may throw`、`volatile load/store`）
- **`MemoryWriteTracking` (MW)**：追踪哪些块包含"可能写内存的指令"——用于 `doesNotWriteMemoryBefore` 的判断

---

### 核心算法：`isGuaranteedToExecute`

```cpp
// MustExecute.cpp:285-290
bool ICFLoopSafetyInfo::isGuaranteedToExecute(const Instruction &Inst,
                                               const DominatorTree *DT,
                                               const Loop *CurLoop) const {
  return !ICF.isDominatedByICFIFromSameBlock(&Inst) &&
         allLoopPathsLeadToBlock(CurLoop, Inst.getParent(), DT);
}
```

**两个条件**：

1. **`!ICF.isDominatedByICFIFromSameBlock(&Inst)`**：在该指令所在的块内，没有隐式控制流点支配它
   - 即：如果该块前面有 `call that may throw`，且该 call 在 Inst 之前，则 Inst **不保证执行**
2. **`allLoopPathsLeadToBlock(CurLoop, Inst.getParent(), DT)`**：从循环 header 到 Inst 所在块的所有路径，都必须经过该块（无旁路）

---

### ICF 和 MW 的内部机制（简化）

```cpp
// InstructionPrecedenceTracking.h 中的机制（简化描述）
class ImplicitControlFlowTracking {
  DenseMap<BasicBlock *, const Instruction*> FirstICF;  // 每个块第一个 ICF 点
  // isDominatedByICFIFromSameBlock(Inst):
  //   如果 FirstICF[Inst->getParent()] 存在且支配 Inst → 返回 true
};

class MemoryWriteTracking {
  DenseMap<BasicBlock *, const Instruction*> FirstMW;   // 每个块第一个内存写点
  // isDominatedByMemoryWriteFromSameBlock(Inst):
  //   如果 FirstMW[Inst->getParent()] 存在且支配 Inst → 返回 true
};
```

**关键设计**：每个块只记录**第一个** ICF/MW 点，而不是所有。因为只需回答"是否有 ICF/MW 在 Inst 之前"，第一个就足以判断。

---

### 更新策略：维护缓存的正确性

`ICFLoopSafetyInfo` 内部的 `ICF` 和 `MW` 是**缓存对象**，当 LICM 修改 IR 时，必须同步更新。

#### 三种更新时机

| API | 调用时机 | LICM 中的位置 | 内部逻辑 |
|---|---|---|---|
| **`computeLoopSafetyInfo(L)`** | LICM 初始化时 | `runOnLoop` :455-456 | 清空 ICF/MW 缓存，遍历循环所有块，记录每个块的 ICF 和 MW 信息 |
| **`insertInstructionTo(Inst, BB)`** | 向块插入新指令时 | `moveInstructionBefore` :1461, FDiv 变换 :948/955 | 更新 ICF 和 MW：如果新指令有隐式控制流或写内存，将其加入缓存 |
| **`removeInstruction(Inst)`** | 从块删除指令时 | `eraseInstruction` :1452, promotion :1861 | 更新 ICF 和 MW：如果被删指令有隐式控制流或写内存，从缓存移除 |

---

### LICM 中的具体使用场景

#### 1. 初始化（runOnLoop :455-456）

```cpp
ICFLoopSafetyInfo SafetyInfo;
SafetyInfo.computeLoopSafetyInfo(L);
```

一次性计算整个循环的安全性信息，后续所有查询都基于这个缓存。

---

#### 2. 判断能否提升有副作用的指令（isSafeToExecuteUnconditionally :1729-1753）

```cpp
bool GuaranteedToExecute = SafetyInfo->isGuaranteedToExecute(Inst, DT, CurLoop);
```

- 如果 `GuaranteedToExecute == true`：即使指令可能 trap，也可以提升到 preheader
- 如果 `GuaranteedToExecute == false`：需要检查 `AllowSpeculation` 是否允许推测执行

---

#### 3. 提升 `invariant.start` / `guard`（hoistRegion :973-985）

```cpp
auto MustExecuteWithoutWritesBefore = [&](Instruction &I) {
  return SafetyInfo->isGuaranteedToExecute(I, DT, CurLoop) &&
         SafetyInfo->doesNotWriteMemoryBefore(I, CurLoop);
};
if ((IsInvariantStart(I) || isGuard(&I)) &&
    CurLoop->hasLoopInvariantOperands(&I) &&
    MustExecuteWithoutWritesBefore(I)) {
  hoist(I, ...);
}
```

**为什么要 `doesNotWriteMemoryBefore`？**

- `invariant.start` 和 `guard` intrinsic 的语义依赖位置
- 如果在该 intrinsic 之前有内存写，提升后语义就变了

---

#### 4. 删除指令时更新（eraseInstruction :1449-1454）

```cpp
static void eraseInstruction(Instruction &I, ICFLoopSafetyInfo &SafetyInfo,
                             MemorySSAUpdater &MSSAU) {
  MSSAU.removeMemoryAccess(&I);
  SafetyInfo.removeInstruction(&I);     // ← 更新 SafetyInfo 缓存
  I.eraseFromParent();
}
```

**顺序**：先更新 SafetyInfo，再删除 IR。因为 `removeInstruction` 需要访问 `I`。

---

#### 5. 移动指令时更新（moveInstructionBefore :1456-1469）

```cpp
static void moveInstructionBefore(Instruction &I, BasicBlock::iterator Dest,
                                   ICFLoopSafetyInfo &SafetyInfo,
                                   MemorySSAUpdater &MSSAU, ScalarEvolution *SE) {
  SafetyInfo.removeInstruction(&I);              // ← 从原块移除记录
  SafetyInfo.insertInstructionTo(&I, Dest->getParent());  // ← 加入新块记录
  I.moveBefore(*Dest->getParent(), Dest);
  // ...
}
```

**两步更新**：先 `removeInstruction`，再 `insertInstructionTo`。

---

#### 6. FDiv → Reciprocal 变换时更新（hoistRegion :948-956）

```cpp
auto ReciprocalDivisor = BinaryOperator::CreateFDiv(One, Divisor);
SafetyInfo->insertInstructionTo(ReciprocalDivisor, I.getParent());  // ← 新指令加入缓存
ReciprocalDivisor->insertBefore(I.getIterator());

auto Product = BinaryOperator::CreateFMul(...);
SafetyInfo->insertInstructionTo(Product, I.getParent());           // ← 新指令加入缓存
Product->insertAfter(I.getIterator());
```

变换时创建了新指令，必须告知 SafetyInfo。

---

#### 7. 内存提升时更新（LoopPromoter::instructionDeleted :1861）

```cpp
void instructionDeleted(Instruction *I) const override {
  SafetyInfo.removeInstruction(I);
  MSSAU.removeMemoryAccess(I);
}
```

当 `LoadAndStorePromoter` 删除 load/store 时，同步更新 SafetyInfo 缓存。

---

### 与 SimpleLoopSafetyInfo 的对比

| 特性 | SimpleLoopSafetyInfo | ICFLoopSafetyInfo |
|---|---|---|
| **blockMayThrow 判断** | 只看整个块是否可能抛（粗粒度） | 精确追踪块内哪个指令是 ICF 点 |
| **isGuaranteedToExecute 精度** | 只检查指令是否是块第一个指令（保守） | 精确检查是否有 ICF 点支配它 |
| **缓存结构** | 只记录 `MayThrow` 和 `HeaderMayThrow` | 维护每个块的 FirstICF 和 FirstMW |
| **更新机制** | 无增量更新，需重新 compute | 提供 insert/remove 的增量更新 |
| **性能开销** | 低（只遍历一次） | 较高（维护缓存，但更精确） |

LICM 选择 `ICFLoopSafetyInfo` 是因为需要**精确判断**，以最大化提升机会。

---

### 更新策略核心原则

| 原则 | 说明 |
|---|---|
| **先更新再删除** | `removeInstruction` 需要访问指令来判断是否是 ICF/MW 点 |
| **移动是两步** | `removeInstruction(old)` + `insertInstructionTo(new)` |
| **新指令要告知** | 变换创建新指令时调用 `insertInstructionTo` |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 计算循环安全信息 | `computeLoopSafetyInfo()` | `MustExecute.cpp:77` |
| 判断必然执行 | `isGuaranteedToExecute()` | `MustExecute.cpp:285` |
| 判断无内存写 | `doesNotWriteMemoryBefore()` | `MustExecute.cpp:292` |
| 判断块可能抛异常 | `blockMayThrow()` | `MustExecute.cpp:69` |
| 插入指令通知 | `insertInstructionTo()` | `MustExecute.cpp:91` |
| 删除指令通知 | `removeInstruction()` | `MustExecute.cpp:97` |
| ICF/MW 追踪 | `ImplicitControlFlowTracking`, `MemoryWriteTracking` | `InstructionPrecedenceTracking.h` |
| 类定义 | `ICFLoopSafetyInfo` | `MustExecute.h:133` |

