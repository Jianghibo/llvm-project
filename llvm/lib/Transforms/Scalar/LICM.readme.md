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

