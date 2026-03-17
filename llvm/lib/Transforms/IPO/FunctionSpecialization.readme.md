# FunctionSpecialization Pass

## commit

```
commit: b7e20442d5edc768c986cc1f3363ab76cd9d7281
Title: [MLIR][ODS] Fix AllElementCountsMatch crash on dynamic shaped types (#183948)
```

# FunctionSpecialization Pass 分析报告

## 1. 基本定位
- **Pass 类型**：Transform (IPO)
- **粒度**：Module
- **所在文件**：`llvm/lib/Transforms/IPO/FunctionSpecialization.cpp`
- **入口函数**：`FunctionSpecializer::run()` (676-869)
- **在 pipeline 中的大致位置**：IPSCCP 之后，Inliner 之前
- **一句话职责**：为传入常量参数的函数调用点创建特化版本，通过常量传播和死代码消除提升性能

## 2. 输入、输出与前置条件
- **输入 IR**：包含函数调用、可能带有常量参数的 Module
- **输出结果**：克隆的特化函数、重定向的调用点、可能的死函数删除
- **关键前置条件**：
  - SCCP Solver 已初始化（`SCCPSolver`）
  - 函数代码度量可用（`CodeMetrics`）
  - BlockFrequencyInfo 可用（用于权重计算）
- **不处理的场景**：
  - 声明函数、无参函数
  - 标记 `noduplicate` 的函数
  - 优化 `minsize` 的函数
  - `alwaysinline` 函数（会被内联）
  - 函数地址被取用的场景（部分限制）

## 3. 关键函数调用调用链

```text
run()
  -> isCandidateFunction()           // 筛选候选函数
  -> promoteConstantStackValues()     // 递归函数：栈常量提升到全局
  -> findSpecializations()           // 查找特化机会
     -> isArgumentInteresting()       // 判断参数是否值得特化
     -> getCandidateConstant()       // 获取候选常量
     -> getCodeSizeSavingsForArg()  // 估算代码大小节省
        -> getCodeSizeSavingsForUser() // 递归传播常量估算
           -> visit*()              // 访问各类指令进行常量折叠
     -> getInliningBonus()           // 计算内联收益（间接调用提升）
  -> createSpecialization()         // 创建特化函数
     -> cloneCandidateFunction()     // 克隆函数
     -> Solver.setLatticeValueForSpecializationArguments() // 设置 lattice 值
  -> Solver.solveWhileResolvedUndefsIn() // SCCP 求解
  -> updateCallSites()             // 更新剩余调用点
  -> promoteConstantStackValues()     // 再次提升栈常量
```

**主调用链说明**：
- `run()` 遍历 Module 中所有函数，筛选候选
- `findSpecializations()` 扫描调用点，收集特化候选并评估收益
- `createSpecialization()` 克隆函数并初始化 SCCP lattice
- SCCP Solver 运行常量传播
- `updateCallSites()` 重定向剩余调用点到最佳特化版本

**最关键函数**：
- `findSpecializations()` (902-1038)：候选发现与收益评估核心
- `getCodeSizeSavingsForUser()` (219-256)：常量传播与死代码估算
- `createSpecialization()` (1069-1093)：特化函数创建

## 4. 整体执行流程

1. **候选函数筛选** (681-734)：遍历 Module，过滤不符合条件的函数
2. **特化机会发现** (902-1038)：扫描每个候选函数的调用点，识别常量参数
3. **收益评估** (967-1024)：计算代码大小节省、延迟节省、内联收益
4. **候选排序与选择** (743-767)：按收益排序，选择 Top-N（受 `MaxClones` 限制）
5. **创建特化函数** (784-825)：克隆函数，设置 lattice 值，重定向已知调用点
6. **SCCP 求解** (827)：运行 SCCP 求解器传播常量
7. **更新剩余调用点** (832-835)：匹配剩余调用，重定向到最佳特化版本
8. **递归函数栈常量提升** (864-866)：支持递归函数的迭代特化

## 5. 核心逻辑拆解

### 5.1 候选函数筛选 (isCandidateFunction, 1040-1067)

```cpp
bool FunctionSpecializer::isCandidateFunction(Function *F) {
  if (F->isDeclaration() || F->arg_empty())  // 声明或无参
    return false;
  if (F->hasFnAttribute(Attribute::NoDuplicate))  // 禁止复制
    return false;
  if (Specializations.contains(F))  // 已特化
    return false;
  if (shouldOptimizeForSize(F, ...))  // 优化 size 时跳过
    return false;
  if (!Solver.isBlockExecutable(&F->getEntryBlock()))  // 死函数
    return false;
  if (F->hasFnAttribute(Attribute::AlwaysInline))  // 会被内联
    return false;
  return true;
}
```

**作用**：过滤掉不值得特化的函数，避免浪费编译时间。

### 5.2 特化机会发现与收益评估 (findSpecializations, 919-1035)

```cpp
for (User *U : F->users()) {
  auto &CS = *cast<CallBase>(U);
  if (CS.getCalledFunction() != F) continue;  // 不是调用 F
  if (CS.hasFnAttr(Attribute::MinSize)) continue;  // minsize 跳过
  if (!Solver.isBlockExecutable(CS.getParent())) continue;  // 死代码
  
  // 收集常量参数
  SpecSig S;
  for (Argument *A : Args) {
    Constant *C = getCandidateConstant(CS.getArgOperand(A->getArgNo()));
    if (C) S.Args.push_back({A, C});
  }
  
  // 计算收益
  InstCostVisitor Visitor = getInstCostVisitorFor(F);
  for (ArgInfo &A : S.Args) {
    CodeSize += Visitor.getCodeSizeSavingsForArg(A.Formal, A.Actual);
    Score += getInliningBonus(A.Formal, A.Actual);
  }
  CodeSize += Visitor.getCodeSizeSavingsFromPendingPHIs();
  
  // 检查收益是否足够
  if (IsProfitable()) {
    AllSpecs.emplace_back(F, S, Score, SpecSize);
  }
}
```

**作用**：扫描调用点，识别常量参数组合，估算特化后的收益。

**为什么这样做**：
- 常量参数可以触发大量常量传播和死代码消除
- 间接调用参数可以提升为直接调用，进而内联
- 需要评估收益是否超过代码膨胀成本

### 5.3 常量传播与死代码估算 (getCodeSizeSavingsForUser, 219-256)

```cpp
Cost InstCostVisitor::getCodeSizeSavingsForUser(Instruction *User, Value *Use, Constant *C) {
  if (Known) return 0;  // 已处理
  
  // 记录当前值-常量映射
  LastVisited = KnownConstants.insert({Use, C}).first;
  
  // 根据指令类型估算
  if (auto *I = dyn_cast<SwitchInst>(User))
    CodeSize = estimateSwitchInst(*I);  // Switch 消除 dead case
  else if (auto *I = dyn_cast<BranchInst>(User))
    CodeSize = estimateBranchInst(*I);  // Branch 消除 dead succ
  else {
    C = visit(*User);  // 常量折叠
    if (!C) return 0;
  }
  
  // 递归传播到使用者
  for (auto *U : User->users())
    CodeSize += getCodeSizeSavingsForUser(U, User, C);
  
  return CodeSize;
}
```

**作用**：模拟常量传播，估算可消除的代码量。

**依赖**：
- SCCP Solver 的 lattice 值（已知常量）
- TargetTransformInfo 的指令成本

### 5.4 特化函数创建 (createSpecialization, 1069-1093)

```cpp
Function *FunctionSpecializer::createSpecialization(Function *F, const SpecSig &S) {
  Function *Clone = cloneCandidateFunction(F, Specializations.size() + 1);
  Clone->setLinkage(GlobalValue::InternalLinkage);  // 设为 internal
  
  // 初始化 lattice：标记特化参数为常量
  Solver.setLatticeValueForSpecializationArguments(Clone, S.Args);
  Solver.markBlockExecutable(&Clone->front());
  Solver.addArgumentTrackedFunction(Clone);
  Solver.addTrackedFunction(Clone);
  
  Specializations.insert(Clone);
  return Clone;
}
```

**作用**：克隆函数并初始化 SCCP lattice，使特化参数成为已知常量。

## 6. 关键数据结构与 LLVM API

| 名称 | 作用 | 关键字段/接口 | 位置 |
|`---|---|---|---|
| `SpecSig` | 特化签名（参数-常量映射） | `SmallVector<ArgInfo> Args` | 头文件 |
| `Spec` | 特化候选 | `Function *F`, `SpecSig Sig`, `unsigned Score`, `Function *Clone` | 头文件 |
| `InstCostVisitor` | 常量传播估算器 | `KnownConstants`, `PendingPHIs`, `DeadBlocks` | 类成员 |
| `SCCPSolver` | 常量传播求解器 | `getConstantOrNull()`, `setLatticeValueForSpecializationArguments()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| `CodeMetrics` | 函数代码度量 | `NumInsts`, `isRecursive`, `notDuplicatable` | `llvm/Analysis/CodeMetrics.h` |
| `TargetTransformInfo` | 指令成本查询 | `getInstructionCost(I, TCK_CodeSize/Latency)` | `llvm/Analysis/TargetTransformInfo.h` |

## 7. 正确性约束与易错点

**语义约束**：
- 特化必须保持函数语义不变（只是参数变为常量）
- 递归调用需要特殊处理（避免无限特化）
- 函数地址取用会限制特化（因为调用点可能无法全部重定向）

**IR 合法性约束**：
- 克隆函数必须设为 `internal` linkage
- SCCP lattice 必须正确初始化
- Profile count 需要正确迁移

**可能踩坑**：
1. **递归函数**：需要 `promoteConstantStackValues()` 将栈常量提升到全局，否则无法继续特化
2. **代码膨胀**：受 `MaxCodeSizeGrowth` 和 `MaxClones` 限制，避免过度特化
3. **间接调用提升**：`getInliningBonus()` 估算间接调用提升为直接调用后的内联收益，但这是启发式估计
4. **Profile count**：重定向调用点时需要更新 profile count，避免 PGO 失效

**本 Pass 如何避免**：
- 使用 `FunctionGrowth` 跟踪每个函数的代码增长
- 限制每个函数最多 `MaxClones` 个特化版本
- 要求最小收益阈值（`MinCodeSizeSavings`, `MinLatencySavings`）
- 递归函数通过栈常量提升支持迭代特化

## 8. 分析依赖与与 Pass 交互

**依赖的 analyses**：
- `SCCPSolver`：常量传播求解，提供 lattice 值
- `CodeMetrics`：函数大小和递归性分析
- `TargetTransformInfo`：指令成本估算
- `BlockFrequencyInfo`：权重计算（延迟节省）
- `AssumptionCache`：内联成本估算
- `TargetLibraryInfo`：内联成本估算

**`PreservedAnalyses` / `invalidate()` 行为**：
- 创建新函数 → 模块级分析失效
- 修改调用点 → 函数级分析失效
- 删除死函数 → 调用图分析失效
- 返回 `PreservedAnalyses::none()`（几乎所有分析都失效）

**依赖哪些前置 Pass**：
- `IPSCCP`：提供初始常量信息
- `GlobalOpt`：全局常量传播

**为哪些后续 Pass 创造机会**：
- `Inliner`：间接调用提升为直接调用，更容易内联
- `DCE`：死代码消除
- `ConstantPropagation`：进一步常量传播
- `LoopUnroll`：循环边界变为常量后更容易展开

## 9. 收益模型 / 编译时权衡

**主要收益**：
1. **代码大小节省**：常量传播后死代码消除
2. **延迟节省**：消除运行时计算，替换为编译时常量
3. **内联收益**：间接调用提升为直接调用，进而内联

**启发式或阈值**：
- `MinFunctionSize = 500`：太小函数不值得特化
- `MaxClones = 3`：每个函数最多 3 个特化版本
- `MinCodeSizeSavings = 20%`：代码大小节省至少 20%
- `MinLatencySavings = 20%`：延迟节省至少 20%
- `MinInliningBonus = 300%`：内联收益至少 300%（间接调用提升）
- `MaxCodeSizeGrowth = 3`：代码增长不超过 3 倍

**编译时成本来源**：
- 遍历所有函数和调用点
- `InstCostVisitor` 递归传播常量（复杂度 O(指令数)）
- SCCP Solver 求解
- 函数克隆

**可能的 trade-off**：
- 收益高但编译时也高：需要保守的阈值
- 递归函数可能需要多次迭代：`promoteConstantStackValues()` 支持迭代
- 间接调用提升收益难以精确估计：使用启发式

## 10. 验证与调试方法

**建议看的测试**：
- `llvm/test/Transforms/FunctionSpecialization/` 目录
- 关注递归函数、间接调用、多参数特化的测试用例

**建议的 `opt` 命令**：
```bash
# 查看特化详情
opt -passes='function-specialization' -debug-only=function-specialization -S input.ll

# 强制所有常量参数特化
opt -passes='function-specialization' -force-specialization -S input.ll

# 调整阈值
opt -passes='function-specialization' -funcspec-max-clones=5 -funcspec-min-function-size=100 -S input.ll
```

**应关注的 IR 前后差异**：
- 新增的 `.specialized.*` 函数
- 调用点从原函数重定向到特化函数
- 特化函数中参数被常量替换
- 死代码消除后的简化 IR

## 11. 总结

**这个 Pass 最核心的设计点**：
1. **收益驱动的特化**：不是盲目特化，而是严格评估收益是否超过成本
2. **SCCP 集成**：利用 SCCP Solver 进行常量传播，无需重新实现
3. **递归函数支持**：通过栈常量提升支持迭代特化
4. **间接调用提升**：识别函数指针参数，提升为直接调用后可内联

**最值得继续深挖的 1~2 个问题**：
1. **`InstCostVisitor` 的常量传播估算算法**：如何精确估算死代码消除？如何处理 PHI 和循环？
2. **递归函数的迭代特化机制**：`promoteConstantStackValues()` 如何支持多轮特化？是否存在收敛保证？

---

## 其他补充

### 针对的有效参数类型

1. pointer type
2. 允许字面量常量优化时：integer、float point、struct

### cost model

```cpp
给定一个函数 F，函数规模 FuncSize

对某个候选特化 S：
  InliningBonus = Σ_{arg ∈ S.Args} InlineBonus(arg)
  CodeSizeSavings = CodeSizeSavings(S)        // 基于 TCK_CodeSize
  LatencySavings = LatencySavings(S)          // 基于 TCK_Latency × BFI 权重
  SpecSize = FuncSize - CodeSizeSavings

盈利性判定（ForceSpecialization 为真时直接通过）：
  1) InliningBonus > MinInliningBonus% × FuncSize
     或者
  2) CodeSizeSavings ≥ MinCodeSizeSavings% × FuncSize
     且 LatencySavings ≥ MinLatencySavings% × FuncSize
     且 (FunctionGrowth[F] + SpecSize) / FuncSize ≤ MaxCodeSizeGrowth

最终得分（用于排序）：
  Score = InliningBonus + max(CodeSizeSavings, LatencySavings)

全局预算：
  只取 Top-N，其中 N = min(NumCandidates × MaxClones, |AllSpecs|)
```

---

# FunctionSpecializer::run() 代码分析

## 函数签名与目的（676行）

```cpp
bool FunctionSpecializer::run()
```

**Pass类型**：ModulePassManager中的IPO Pass  
**目的**：通过函数特化使常量能够跨函数边界传播，提升后续优化机会

---

## 阶段一：发现特化候选（677-734行）

```cpp
SpecMap SM;                          // Function -> [Begin, End) 特化范围映射
SmallVector<Spec, 32> AllSpecs;      // 所有特化候选集合
unsigned NumCandidates = 0;

for (Function &F : M) {
    // 1.1 筛选候选函数
    if (!isCandidateFunction(&F))
        continue;

    // 1.2 分析函数代码度量（首次分析）
    auto [It, Inserted] = FunctionMetrics.try_emplace(&F);
    CodeMetrics &Metrics = It->second;
    if (Inserted) {
        SmallPtrSet<const Value *, 32> EphValues;
        CodeMetrics::collectEphemeralValues(&F, &GetAC(F), EphValues);
        for (BasicBlock &BB : F)
            Metrics.analyzeBasicBlock(&BB, GetTTI(F), EphValues);
    }

    // 1.3 检查函数大小和可复制性
    const bool RequireMinSize =
        !ForceSpecialization &&
        (SpecializeLiteralConstant || !F.hasFnAttribute(Attribute::NoInline));

    if (Metrics.notDuplicatable || !Metrics.NumInsts.isValid() ||
        (RequireMinSize && Metrics.NumInsts < MinFunctionSize))
        continue;

    // 1.4 非字面常量特化时，跳过非递归函数（避免重复分析）
    if (!SpecializeLiteralConstant && !Inserted && !Metrics.isRecursive)
        continue;

    unsigned FuncSize = static_cast<unsigned>(Metrics.NumInsts.getValue());

    // 1.5 递归函数：提升常量栈值到全局变量
    if (Inserted && Metrics.isRecursive)
        promoteConstantStackValues(&F);

    // 1.6 查找特化机会
    if (!findSpecializations(&F, FuncSize, AllSpecs, SM))
        continue;

    ++NumCandidates;
;
```

**关键点**：
- **isCandidateFunction()** (1040-1067)：过滤声明、空参数、NoDuplicate、已特化、size优化、不可执行、AlwaysInline函数
- **promoteConstantStackValues()** (585-613)：将递归归约中的栈常量提升为全局变量，支持多轮特化
- **findSpecializations()** (902-1038)：遍历调用点，收集常量参数，计算收益

**优化意图**：避免对小函数或即将被内联的函数做无意义的特化，控制代码膨胀

---

## 阶段二：选择最优特化（743-767行）

```cpp
// 2.1 定义分数比较函数（收益优先，平局时取索引较大）
auto CompareScore = [&AllSpecs](unsigned I, unsigned J) {
    if (AllSpecs[I].Score != AllSpecs[J].Score)
        return AllSpecs[I].Score > AllSpecs[J].Score;
    return I > J;
};

// 2.2 计算特化数量限制：min(候选数×MaxClones, 总候选数)
const unsigned NSpecs =
    std.::min(NumCandidates * MaxClones, unsigned(AllSpecs.size()));

// 2.3 使用堆算法实现Top-K选择
SmallVector<unsigned> BestSpecs(NSpecs + 1);
std::iota(BestSpecs.begin(), BestSpecs.begin() + NSpecs, 0);

if (AllSpecs.size() > NSpecs) {
    std::make_heap(BestSpecs.begin(), BestSpecs.begin() + NSpecs, CompareScore);
    for (unsigned I = NSpecs, N = AllSpecs.size(); I < N; ++I) {
        BestSpecs[NSpecs] = I;
        std::push_heap(BestSpecs.begin(), BestSpecs.end(), CompareScore);
        std::pop_heap(BestSpecs.begin(), BestSpecs.end(), CompareScore);
    }
}
```

**算法复杂度度**：O(N log K)，其中N=AllSpecs.size()，K=NSpecs  
**优化意图**：在预算约束下选择收益最大的特化，避免代码爆炸

---

## 阶段三：创建特化函数（781-825行）

```cpp
SmallPtrSet<Function *, 8> OriginalFuncs;
SmallVector<Function *> Clones;

for (unsigned I = 0; I < NSpecs; ++I) {
    Spec &S = AllSpecs[BestSpecs[I]];

    // 3.1 累计代码增长（用于后续预算检查）
    FunctionGrowth[S.F] += S.CodeSize;

    // 3.2 克隆函数并设置常量参数
    S.Clone = createSpecialization(S.F, S.Sig);

    // 3.3 重定向已知调用点到特化版本
    for (CallBase *Call : S.CallSites) {
        Function *Clone = S.Clone;
        Call->setCalledFunction(S.Clone);

        // 3.4 更新profile计数（PGO感知）
        auto &BFI = GetBFI(*Call->getFunction());
        std::optional<uint64_t> Count = BFI.getBlockProfileCount(Call->getParent());
        if (Count && !ProfcheckDisableMetadataFixes) {
            uint64_t CallCount = *Count + Clone->getEntryCount().getCount();
            Clone->setEntryCount(CallCount);
            S.F->setEntryCount(OriginalCount - CallCount);
        }
    }

    Clones.push_back(S.Clone);
    OriginalFuncs.insert(S.F);
}
```

**createSpecialization()** (1069-1093)：
- 调用`CloneFunction()`克隆函数 → `llvm/Transforms/Utils/Cloning.h`
- 设置为内部链接（允许内联）
- 在SCCP Solver中标记特化参数为常量值 → `llvm/Transforms/Utils/SCCPSolver.h`

**优化意图**：通过profile数据指导后续优化决策

---

## 阶段四：SCCP求解与传播（827-862行）

```cpp
// 4.1 在克隆函数上运行SCCP（稀疏条件常量传播）
Solver.solveWhileResolvedUndefsIn(Clones);

// 4.2 更新剩余调用点（递归调用、新增匹配）
for (Function *F : OriginalFuncs) {
    auto [Begin, End] = SM[F];
    updateCallSites(F, AllSpecs.begin() + Begin, AllSpecs.begin() + End);
}

// 4.3 处理返回值为常量的特化函数
for (Function *F : Clones) {
    if (F->getReturnType()->isVoidTy())
        continue;

    // 检查返回值是否为常量
    bool IsConstant = false;
    if (F->getReturnType()->isStructTy()) {
        IsConstant = Solver.isStructLatticeConstant(F, STy);
    } else {
        auto It = Solver.getTrackedRetVals().find(F);
        IsConstant = !SCCPSolver::isOverdefined(It->second);
    }

    if (IsConstant) {
        // 重置调用点的lattice值，触发重新求解
        for (User *U : F->users()) {
            if (auto *CS = dyn_cast<CallBase>(U)) {
                if (CS->getCalledFunction() != F)
                    continue;
                Solver.resetLatticeValueFor(CS);
            }
        }
    }
}

// 4.4 重新运行求解器传播常量
Solver.solveWhileResolvedUndefs();
```

**关键机制**：
- **solveWhileResolvedUndefsIn()**：在指定函数上迭代求解，直到收敛
- **updateCallSites()** (1211-1260)：匹配调用点到最佳特化，标记完全特化的原函数为死函数

**优化意图**：增量求解，只处理受影响的函数，降低编译时开销

---

## 阶段五：递归函数二次优化（864-866行）

```cpp
for (Function *F : OriginalFuncs)
    if (FunctionMetrics[F].isRecursive)
        promoteConstantStackValues(F);
```

**优化意图**：在特化完成后，再次提升常量栈值，为下一轮Pass做准备

---

## 收益计算模型（968-1020行）

```cpp
auto IsProfitable = [&]() -> bool {
    if (ForceSpecialization)
        return true;

    // 1. 内联收益检查（间接调用直接化）
    if (Score > MinInliningBonus * FuncSize / 100)
        return true;

    // 2. 代码大小节省检查（死代码消除）
    if (CodeSizeSavings < MinCodeSizeSavings * FuncSize / 100)
        return false;

    // 3. 延迟节省检查（懒计算，避免不必要BFI）
    unsigned LatencySavings =
        getCostValue(Visitor.getLatencySavingsForKnownConstants());
    if (LatencySavings < MinLatencySavings * FuncSize / 100)
        return false;

    // 4. 代码增长限制检查
    if ((FunctionGrowth[F] + SpecSize) / FuncSize > MaxCodeSizeGrowth)
        return false;

    Score += std::max(CodeSizeSavings, LatencySavings);
    return true;
};
```

**收益组成**：
- **Inlining Bonus**：间接调用直接化带来的内联机会 → `llvm/Analysis/InlineCost.h`
- **CodeSize Savings**：常量传播后可消除的死代码 → `InstCostVisitor::getCodeSizeSavingsForArg()`
- **Latency Savings**：基于BlockFrequency的执行时间节省 → `llvm/Analysis/BlockFrequencyInfo.h`

---

## 关键API与源码路径

| 功能 | API | 头文件 |
|------|-----|---------|
| 函数克隆 | `CloneFunction()` | `llvm/Transforms/Utils/Cloning.h` |
| SCCP求解器 | `SCCPSolver` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 代码度量 | `CodeMetrics` | `llvm/Analysis/CodeMetrics.h` |
| 内联成本 | `getInlineCost()` | `llvm/Analysis/InlineCost.h` |
| 块频率 | `BlockFrequencyInfo` | `llvm/Analysis/BlockFrequencyInfo.h` |
| 目标变换信息 | `TargetTransformInfo` | `llvm/Analysis/TargetTransformInfo.h` |

---

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-funcspec-max-clones` | 3 | 单函数最大克隆数 |
| `-funcspec-min-function-size` | 500 | 最小函数大小 |
| `-funcspec-max-codesize-growth` | 3 | 最大代码增长倍数 |
| `-funcspec-min-codesize-savings` | 20% | 最小代码节省百分比 |
| `-funcspec-min-latency-savings` | 20% | 最小延迟节省百分比 |
| `-funcspec-min-inlining-bonus` | 300% | 最小内联收益百分比 |

---

##
## promoteConstantStackValues() 代码分析

### 函数签名与目的（585-613行）

```cpp
void FunctionSpecializer::promoteConstantStackValues(Function *F)
```

**目的**：支持递归函数的多轮特化，将栈上的常量值提升为全局变量，使 SCCP Solver 能够在下一轮 Pass 中发现更多特化机会。

---

### 整体流程分析

```cpp
// 阶段1：遍历函数的所有调用点
for (User *U : F->users()) {
    auto *Call = dyn_cast<CallInst>(U);
    if (!Call)
        continue;

    // 1.1 检查调用点所在基本块是否可执行
    if (!Solver.isBlockExecutable(Call->getParent()))
        continue;

    // 阶段2：遍历调用点的每个参数
    for (const Use &U : Call->args()) {
        unsigned Idx = Call->getArgOperandNo(&U);
        Value *ArgOp = Call->getArgOperand(Idx);
        Type *ArgOpType = ArgOp->getType();

        // 2.1 只处理只读内存的指针参数
        if (!Call->onlyReadsMemory(Idx) || !ArgOpType->isPointerTy())
            continue;

        // 2.2 检查参数值是否是可提升的常量栈值
        auto *ConstVal = getConstantStackValue(Call, ArgOp);
        if (!ConstVal)
            continue;

        // 阶段3：创建全局变量并替换参数
        Value *GV = new GlobalVariable(M, ConstVal->getType(), true,
                                      GlobalValue::InternalLinkage, ConstVal,
                                      "specialized.arg." + Twine(++NGlobals));
        Call->setArgOperand(Idx, GV);
    }
}
```

---

### 辅助函数分析

#### getConstantStackValue()（548-560行）

```cpp
Constant *FunctionSpecializer::getConstantStackValue(CallInst *Call,
                                                  Value *Val) {
    if (!Val)
        return nullptr;
    Val = Val->stripPointerCasts();
    auto *Alloca = dyn_cast<AllocaInst>(Val);
    if (!Alloca)
        return nullptr;
    Constant *C = getPromotableAlloca(Alloca, Call);
    if (!C || !C->getType()->isIntegerTy())
        return nullptr;
    return C;
}
```

**功能**：
1. 去除指针转换
2. 检查值是否为 AllocaInst
3. 调用 `getPromotableAlloca()` 验证可提升性
4. 只接受整数类型的常量

---

#### getPromotableAlloca()（519-543行）

```cpp
Constant *FunctionSpecializer::getPromotableAlloca(AllocaInst *Alloca,
                                                   CallInst *Call) {
    Value *StoreValue = nullptr;
    for (auto *User : Alloca->users()) {
        // 跳过调用点本身
        if (User == Call)
            continue;

        // 检查是否有单个 store 指令
        if (auto *Store = dyn_cast<StoreInst>(User)) {
            // 拒绝重复 store 或 volatile store
            if (StoreValue || Store->isVolatile())
                return nullptr;
            StoreValue = Store->getValueOperand();
            continue;
        }
        // 有其他未知使用，无法提升
        return nullptr;
    }

    if (!StoreValue)
        return nullptr;

    return getCandidateConstant(StoreValue);
}
```

**功能**：验证 AllocaInst 是否只被单个常量值存储到，且只被调用点使用。

---

### 优化意图与正确性保证

**优化意图**：
- **递归函数特化**：在多轮 Pass 中，第一轮特化后，递归调用可能使用栈上的常量值，但这些值无法跨函数边界传播
- **跨边界传播**：通过将栈常量提升为全局常量，使 SCCP Solver 能够在下一轮中识别这些常量
- **代码示例**（562-581行注释）：

```llvm
; 特化前：
define internal void @RecursiveFn(i32* arg1) {
  %temp = alloca i32, align 4
  store i32 2, i32* %temp, align 4
  call void @RecursiveFn.1(i32* nonnull %temp)
  ret void
}

; 提升后：
@funcspec.arg = internal constant i32 2

define internal void @someFunc(i32* arg1) {
  call void @otherFunc(i32* nonnull @funcspec.arg)
  ret void
}
```

**正确性保证**：
1. **只读内存检查**：`onlyReadsMemory(Idx)` 确保不会修改内存
2. **指针类型检查**：只处理指针参数
3. **整数类型限制**：`getConstantStackValue()` 只接受整数常量
4. **可执行性检查**：`isBlockExecutable()` 跳过死代码
5. **单一 store 验证**：`getPromotableAlloca()` 确保没有副作用

---

### 与 SCCP Solver 的交互

**问题**：SCCP Solver 可能插入 bitcast 指令用于 PredicateInfo，这些会干扰 `promoteConstantStackValues()` 的优化。

**解决方案**：`removeSSACopy()`（617-627行）在特化函数创建后清理这些冗余 bitcast。

```cpp
static void removeSSACopy(Function &F) {
    for (BasicBlock &BB : F) {
        for (Instruction &Inst : llvm::make_early_inc_range(BB)) {
            auto *BC = dyn_cast<BitCastInst>(&Inst);
            // 只移除类型相同的冗余 bitcast
            if (!BC || BC->getType() != BC->getOperand(0)->getType())
                continue;
            Inst.replaceAllUsesWith(BC->getOperand(0));
            Inst.eraseFromParent();
        }
    }
}
```

---

### 关键 API 与源码路径

| 功能 | API | 头文件 |
|------|-----|---------|
| 全局变量创建 | `new GlobalVariable()` | `llvm/IR/GlobalVariable.h` |
| 参数属性查询 | `onlyReadsMemory()` | `llvm/IR/CallSite.h` |
| 指针转换去除 | `stripPointerCasts()` | `llvm/IR/Value.h` |
| 基本块可执行性 | `isBlockExecutable()` | `llvm/Transforms/Utils/SCCPSolver.h` |

---

### 调用时机

在 `run()` 函数中被调用两次：

1. **首次调用**（723-724行）：在发现特化候选阶段，针对首次分析的递归函数
2. **二次调用**（864-866行）：在所有特化创建完成后，再次提升常量栈值，为下一轮 Pass 准备

这种两阶段调用确保了递归函数在当前 Pass 和后续 Pass 中都能发现特化机会。

---

## InstCostVisitor 常量量传播估算分析

### 核心机制

InstCostVisitor 继承了 `InstVisitor` 接口，通过递归访问模拟常量传播过程，估算特化后的收益。

**关键成员变量**：
- `KnownConstants`：已知的常量映射（Value* -> Constant*）
- `DeadBlocks`：可消除的死块集合
- `PendingPHIs`：待处理的PHI节点（首次访问时可能无法确定）
- `VisitedPHIs`：已访问的PHI节点（避免重复处理）
- `LastVisited`：上次访问的迭代器指针

---

### 核心算法流程

```
getCodeSizeSavingsForArg(A, C) {
    // 遍历参数A的所有用户（指令）
    for (auto *U : A->users())
        if (auto *UI = dyn_cast<Instruction>(U))
            if (isBlockExecutable(UI->getParent()))
                CodeSize += getCodeSizeSavingsForUser(UI, A, C);
}
```

**递归过程**（getCodeSizeSavingsForUser → visit() → 用户链）：
1. **缓存检查**：若 `KnownConstants.contains(User)`，返回0（已处理）
2. **记录常量**：`LastVisited = KnownConstants.insert({Use, C}).first` 或 `KnownConstants.end()`
3. **指令分类处理**：
   - **SwitchInst**：`estimateSwitchInst()` → 死代码消除
   - **BranchInst**：`estimateBranchInst()` → 死代码消除
   - **其他指令**：`C = visit(*User)` → 常量折叠
4. **累加收益**：`CodeSize += TTI.getInstructionCost(User, TCK_CodeSize)`
5. **递归用户链**：遍历 User->users()，继续传播

**常量折叠逻辑**（visit() 分发）：
- **PHINode**：检查所有incoming值是否为同一常量
- **SelectInst**：条件分支常量选择
- **GetElementPtrInst**：指针常量GEP常量
- **CmpInst**：比较指令常量折叠
- **CallBase**：可常量折叠的函数调用
- **LoadInst**：从常量指针加载
- **CastInst**：类型转换常量折叠
- **CmpInst**：二元运算常量折叠
- **FreezeInst**：冻结 poison/undef
```

---

### 死代码消除估算（estimateSwitchInst/estimateBranchInst）

**SwitchInst 估算**（258-281行）：
```cpp
Cost InstCostVisitor::estimateSwitchInst(SwitchInst &I) {
    // 1. 确认是常量条件
    if (I.getCondition() != LastVisited->first)
        return 0;

    // 2. 获取常量值
    auto *C = dyn_cast<ConstantInt>(LastVisited->second);
    if (!C)
        return 0;

    // 3. 确定目标后继块
    BasicBlock *Succ = I.findCaseValue(C)->getCaseSuccessor();

    // 4. 初始化死块工作列表
    SmallVector<BasicBlock *> WorkList;
    for (const auto &Case : I.cases()) {
        BasicBlock *BB = Case.getCaseSuccessor();
        if (BB != Succ && isBlockExecutable(BB) &&
            canEliminateSuccessor(I.getParent(), BB))
            WorkList.push_back(BB);
    }

    // 5. 递归估算死块代码大小
    return estimateBasicBlocks(WorkList);
}
```

**关键点**：
- **条件匹配**：`I.getCondition() == LastVisited->first`
- **死块识别**：非目标case的所有case对应的死块
- **唯一前驱检查**：`canEliminateSuccessor()` 确保块只能从该switch跳转到达

**BranchInst 估算**（283-297行）**：
```cpp
Cost InstCostVisitor::estimateBranchInst(BranchInst &I) {
    // 1. 检查是否是常量条件
    if (I.getCondition() != LastVisited->first)
        return 0;

    // 2. 确定目标后继块
    BasicBlock *Succ = I.getSuccessor(LastVisited->second->isOneValue());

    // 3. 初始化死块工作列表
    SmallVector<BasicBlock *> WorkList;
    if (isBlockExecutable(Succ) && canEliminateSuccessor(I.getParent(), Succ))
        WorkList.push_back(Succ);

    // 4. 递归估算死块代码大小
    return estimateBasicBlocks(WorkList);
}
```

**关键点**：
- **条件匹配**：`I.getCondition() == LastVisited->first`
- **死块识别**：条件为真时的false successor
- **唯一前驱检查**：`canEliminateSuccessor()` 确保块只能从该branch跳转到达

---

### estimateBasicBlocks() 死块递归（114-147行）

```cpp
Cost InstCostEstimator::estimateBasicBlocks(
                           SmallVectorImpl<BasicBlock *> &WorkList) {
    Cost CodeSize = 0;
    while (!WorkList.empty()) {
        BasicBlock *BB = WorkList.pop_back_val();

        // 标记已处理块，避免重复
        if (!DeadBlocks.insert(BB).second)
            continue;

        // 累加该块中所有指令的代码大小
        for (Instruction &I : *BB) {
            if (KnownConstants.contains(&I))
                continue;
            Cost C = TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);
            CodeSize += C;
        }

        // 递归添加死后继块
        for (BasicBlock *SuccBB : successors(BB))
            if (isBlockExecutable(SuccBB) && canEliminateSuccessor(BB, SuccBB))
                WorkList.push_back(SuccBB);
    }
    return CodeSize;
}
```

**算法**：DFS递归 + 唯约性检查
- **唯一前驱条件**：`canEliminateSuccessor(BB, SuccBB)` 确保：
  - 前驱数量 ≤ MaxBlockPredecessors（默认2）
  - 所有前驱都是BB本身、SuccBB或不可执行块
- **复杂度**：O(N) 其中N是可消除块总数

---

### PHINode 处理（343-396行）

**首次访问（Inserted=false）**：
- 将PHI加入`PendingPHIs`，返回nullptr（延迟处理）
- **原因**：可能依赖的incoming值还未确定

**二次访问（Inserted=true）**：
- 检查所有incoming值是否为常量
- **Transitive Phi检测**：如果有incoming PHI，调用`discoverTransitivelyIncomingValues()`
  - **普通Phi**：所有incoming值都是同一常量 → 返回常量
- **Transitive Phi**：incoming PHI链形成循环 → 返回常量

---

## SCCPSolver交互机制

### 特化参数设置（createSpecialization → 1083-1086行）

```cpp
Solver.setLatticeValueForSpecializationArguments(Clone, S.Args);
Solver.markBlockExecutable(&Clone->front());
Solver.addArgumentTrackedFunction(Clone);
Solver.addTrackedFunction(Clone);
```

**关键操作**：
1. **setLatticeValueForSpecializationArguments()**：将特化参数标记为常量值
2. **markBlockExecutable()**：标记入口块可执行
3. **addArgumentTrackedFunction()**：添加到参数跟踪函数列表
4. **addTrackedFunction()**：添加到跟踪函数列表

**Lattice值状态**：
- **Overdefined**：未确定，可传播
- **Constant**：已知常量，不可优化
- **Lattice range**：值域范围信息
- **Struct lattice**：结构体类型的lattice值

### 求解器调用时机

**首次求解**（827行）**：
```cpp
Solver.solveWhileResolvedUndefsIn(Clones);
```
在克隆函数上运行，传播特化参数到整个函数。

**二次求解**（862行）**：
```cpp
Solver.solveWhileResolvedUndefs();
```
在全局范围上运行，通知所有用户修改的调用点。

**重置lattice值**（856行）**：
```cpp
Solver.resetLatticeValueFor(CS);
```
当特化函数返回值为常量时，重置调用点的lattice值，触发重新求解。

---

## 收益计算细节：Inlining Bonus

### 计算逻辑（1099-1148行）

**目的**：估算间接调用直接化带来的内联机会

**步骤1：参数类型检查**
```cpp
Function *CalledFunction = dyn_cast<Function>(C->stripPointerCasts());
if (!CalledFunction)
    return 0;
```
只处理函数指针类型的常量。

**步骤2：遍历参数A的所有使用**
```cpp
for (User *U : A->users()) {
    if (!isa<CallInst>(U) && !isa<InvokeInst>(U))
        continue;
    auto *CS = cast<CallBase>(U);
    if (CS->getCalledOperand() != A)
        continue;
    if (CS->getFunctionType() != CalledFunction->getFunctionType())
        continue;
```
只处理调用A的间接调用。

**步骤3：计算内联成本**
```cpp
auto Params = getInlineParams();
Params.DefaultThreshold += InlineConstants::IndirectCallThreshold;
InlineCost IC =
    getInlineCost(*CS, CalledFunction, Params, CalleeTTI, GetAC, GetTLI);
```

**关键优化**：
- **阈值提升**：`DefaultThreshold += IndirectCallThreshold` 提高间接调用阈值
- **成本估算**：`getInlineCost()` 考虑内联的收益
- **收益截断**：
  - `IC.isAlways()`：直接内联 → `+= DefaultThreshold`
  - `IC.isVariable() && IC.getCostDelta() > 0`：有正收益 → `+= getCostDelta()`

**收益类型**：
- **Always内联**：返回完整阈值
- **有正收益**：返回收益差值
- **无收益**：返回0

**示例场景**：
```cpp
// 特化前：void foo(i32* callback) { callback(callback); }
// 特化后：void foo.1(i32* callback) { foo.1(callback); }
```
特化后：`foo.1()` 可能变为直接调用，内联`foo.1()`变得可能。

---

## 总结

**InstCostVisitor**：通过递归访问估算常量传播后的代码/延迟节省
**SCCPSolver**：提供lattice值系统，使特化参数与常量值系统无缝协作
**Inlining Bonus**：通过间接调用直接化暴露内联机会，是函数特化的重要收益来源之一

---

## findSpecializations 函数分析

### 函数签名与目的（902-1038行）

```cpp
bool FunctionSpecializer::findSpecializations(Function *F, unsigned FuncSize,
                                               SmallVectorImpl<Spec> &AllSpecs,
                                               SpecMap &SM)
```

**功能**: 扫描函数的所有调用点，识别和评估函数特化机会，收集所有可盈利的特化候选。

---

### 整体结构

```
findSpecializations(F, FuncSize, AllSpecs, SM)
├── 初始化 UniqueSpecs 映射表（确保特化唯一性）
├── 收集"有趣"的参数（isArgumentInteresting）
├── 遍历函数的所有调用点
│   ├── 过滤：直接调用 + 非MinSize + 可执行块
│   ├── 构建 构建 SpecSig（常量实参对）
│   ├── 检查是否已存在相同特化
│   │   ├── 存在 → 添加到现有特化的 CallSites（跳过递归调用）
│   │   └── 不存在 → 计算收益并创建新特化
│   │       ├── 代码大小节省
│   │       ├── 内联奖励
│   │       ├── 延迟节省
│   │       └── 盈利性检查
└── 返回是否存在特化
```

---

### 逐段注释

**1. 初始化和参数收集 (902-917)**

```cpp
DenseMap<SpecSig, unsigned> UniqueSpecs;  // 特化签名 → AllSpecs索引
SmallVector<Argument *> Args;
for (Argument &Arg : F->args())
  if (isArgumentInteresting(&Arg))  // 检查参数是否值得特化
    Args.push_back(&Arg);
```

- `UniqueSpecs` 避免重复创建相同签名的特化
- `isArgumentInteresting` 过滤：未使用、非指针/整型/浮点、已常量、byval 等

**2. 遍历调用点 (919-952)**

```cpp
for (User *U : F->users()) {
  auto &CS = *cast<CallBase>(U);
  if (CS.getCalledFunction() != F) continue;      // 必须直接调用
  if (CS.hasFnAttr(Attribute::MinSize)) continue;  // MinSize调用点不特化
  if (!Solver.isBlockExecutable(CS.getParent())) continue;  // 死代码跳过
  
  SpecSig S;
  for (Argument *A : Args) {
    Constant *C = getCandidateConstant(CS.getArgOperand(A->getArgNo()));
    if (C) S.Args.push_back({A, C});  // 收集常量实参
  }
```

- `getCandidateConstant` 获取常量或通过 SCCP 推导的常量
- `SpecSig` 是一组 `(形式参数, 实际常量)` 对

**3. 去重处理 (954-965)**

```cpp
if (auto It = UniqueSpecs.find(S); It != UniqueSpecs.end()) {
  if (CS.getFunction() == F) continue;  // 递归调用暂不重定向
  AllSpecs[It->second].CallSites.push_back(&CS);  // 添加到现有特化
}
```

- 递归调用不直接重定向（因为后续克隆可能需要更好的匹配）
- 非递归调用立即添加到现有特化的调用点列表
- **避免对于不同callsite处存在相同常量实参的场景下多次函数特化（递归场景除外）**

**4. 计算特化收益 (966-1020)**

```cpp
InstCostVisitor Visitor = getInstCostVisitorFor(F);
for (ArgInfo &A : S.Args) {
  CodeSize += Visitor.getCodeSizeSavingsForArg(A.Formal, A.Actual);
  Score += getInliningBonus(A.Formal, A.Actual);
}
CodeSize += Visitor.getCodeSizeSavingsFromPendingPHIs();
```

- `getCodeSizeSavingsForArg`: 估算常量传播后可消除的代码大小
- `getInliningBonus`: 估算间接通过调用变为直接调用后的内联收益
- `getCodeSizeSavingsFromPendingPHIs`: 处理 PHI 节点的额外节省

**5. 盈利性检查 (980-1020)**

```cpp
auto IsProfitable = [&]() -> bool {
  if (ForceSpecialization) return true;
  
  // 检查1: 内联奖励 > 阈值
  if (Score > MinInliningBonus * FuncSize / 100) return true;
  
  // 检查2: 代码大小节省 > 阈值
  if (CodeSizeSavings < MinCodeSizeSavings * FuncSize / 100) return false;
  
  // 检查3: 延迟节省 > 阈值
  unsigned LatencySavings = getCostValue(Visitor.getLatencySavingsForKnownConstants());
  if (LatencySavings < MinLatencySavings * FuncSize / 100) return false;
  
  // 检查4: 代码大小增长 < 阈值
  if ((FunctionGrowth[F] + SpecSize) / FuncSize > MaxCodeSizeGrowth) return false;
  
  return true;
};
```

- 按优先级检查：内联奖励 → 代码大小 → 延迟 → 增长限制
- 阈值默认：`MinInliningBonus=300%`, `MinCodeSizeSavings=20%`, `MinLatencySavings=20%`, `MaxCodeSizeGrowth=3x`

**6. 创建特化条目 (1026-1034)**

```cpp
auto &Spec = AllSpecs.emplace_back(F, S, Score, SpecSize);
if (CS.getFunction() != F)
  Spec.CallSites.push_back(&CS);
const unsigned Index = AllSpecs.size() - 1;
UniqueSpecs[S] = Index;
if (auto [It, Inserted] = SM.try_emplace(F, Index, Index + 1); !Inserted)
  It->second.second = Index + 1;
```

- `SM` (SpecMap) 记录每个函数的特化范围 `[Begin, End)`
- `SpecSize`: 函数code size - 不变参数带来的code size的较少量

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SpecSig` | `Args: SmallVector<ArgInfo>` | 特化签名：`(形式参数, 实际常量)` 对集合 |
| `ArgInfo` | `Formal: Argument*`, `Actual: Constant*` | 形式参数和实际常量 |
| `Spec` | `F: Function*`, `Sig: SpecSig`, `Score: unsigned`, `CodeSize: unsigned`, `Clone: Function*`, `CallSites: SmallVector<CallBase*>` | 完整特化描述 |
| `SpecMap` | `DenseMap<Function*, std::pair<unsigned, unsigned>>` | 函数 → 特化范围 `[Begin, End)` |

---

### 优化意图

1. **常量传播优化**: 通过特化常常量参数，启用函数内部的常量折叠、死代码消除
2. **间接调用优化**: 特化函数指针参数后，间接调用可变为直接调用，进而可内联
3. **收益平衡**: 通过多维度评估（代码大小、延迟、内联）避免代码膨胀
4. **递归支持**: 通过 `promote`ConstantStackValues` 支持递归函数的迭代特化

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 参数有趣性检查 | `isArgumentInteresting()` | 1152行 |
| 常量候选获取 | `getCandidateConstant()` | 1191行 |
| 代码大小节省估算 | `InstCostVisitor::getCodeSizeSavingsForArg()` | 169行 |
| 内联奖励计算 | `getInliningBonus()` | 1099行 |
| SCCP求解器 | `SCCPSolver` | `llvm/Transforms/Utils/SCCPSolver.h` |

---

## createSpecialization 函数分析

### 函数签名与目的（1069-1093行）

```cpp
Function *FunctionSpecializer::createSpecialization(Function *F,
                                                     const SpecSig &S)
```

**功能**: 创建函数的特化版本（克隆），配置其属性并在 SCCP Solver 中初始化常量参数状态。

---

### 整体结构

```
createSpecialization(F, S)
├── 克隆原函数 → cloneCandidateFunction()
├── 设置为内部链接（允许内联）
├── 重置 profile 计数
├── 初始化 SCCP Solver 状态
│   ├── 设置特化参数的 lattice 值为常量
│   ├── 标记入口块可执行
│   ├── 添加到参数跟踪函数列表
│   └── 添加到跟踪函数列表
├── 标记为已特化函数
└── 返回克隆函数
```

---

### 逐段注释

**1. 函数克隆 (1071)**

```cpp
Function *Clone = cloneCandidateFunction(F, Specializations.size() + 1);
```

- 调用 `cloneCandidateFunction()` 创建完整副本
- 使用 `Specializations.size() + 1` 作为序号生成唯一名称
- 内部调用 `CloneFunction()` + `removeSSACopy()` 清理冗余 bitcast

**2. 设置链接类型 (1073-1075)**

```cpp
// The original function does not neccessarily have internal linkage, but the
// clone must.
Clone->setLinkage(GlobalValue::InternalLinkage);
```

- 原函数可能是外部链接（如 public API）
- 特化版本必须是内部链接，因为：
  - 允许编译器更激进地内联
  - 避免符号冲突
  - 特化版本不应被外部直接调用

**3. Profile 计数初始化 (1077-1078)**

```cpp
if (F->getEntryCount() && !ProfcheckDisableMetadataFixes)
  Clone->setEntryCount(0);
```

- 只有原函数有 profile 数据时才处理
- 将克隆函数的入口计数设为 0
- 后续在 `run()` 中根据实际调用点累加 profile 数据（800-820行）

**4. SCCP Solver 状态初始化 (1080-1086)**

```cpp
// Initialize the lattice state of the arguments of the function clone,
// marking the argument on which we specialized the function constant
// with the given value.
Solver.setLatticeValueForSpecializationArguments(Clone, S.Args);
Solver.markBlockExecutable(&Clone->front());
Solver.addArgumentTrackedFunction(Clone);
Solver.addTrackedFunction(Clone);
```

**关键操作**：
- `setLatticeValueForSpecializationArguments()`: 将 `S.Args` 中的参数标记为对应的常量值
  - 例如：原函数 `foo(int x)`，特化 `foo.1` 时 `x=42`，则将 `foo.1` 的参数 `x` 的 lattice 值设为常量 42
- `markBlockExecutable()`: 标记入口块可执行（SCCP 求解起点）
- `addArgumentTrackedFunction()`: 添加到参数跟踪列表（SCCP 会跟踪参数的 lattice 值）
- `addTrackedFunction()`: 添加到全局跟踪列表（SCCP 会跟踪返回值）

**5. 标记与统计 (1088-1090)**

```cpp
Specializations.insert(Clone);
++NumSpecsCreated;
```

- `Specializations` 集合防止重复特化同一克隆
- `NumSpecsCreated` 统计全局特化数量

---

### 优化意图

1. **常量传播基础**: 通过设置 lattice 值，使 SCCP Solver 能将特化参数视为常量，触发函数内部的常量折叠和死代码消除

2. **内联友好设计**: 内部链接允许编译器更激进地内联特化版本，因为：
   - 特化版本通常更小（常量传播后）
   - 特化版本调用点有限（只有特定常量参数的调用）
   - 内联后可进一步优化

3. **Profile 准确性**: 初始设为 0 后根据实际调用点累加，避免继承原函数的 profile 数据导致错误优化决策

4. **Solver 集成**: 通过 lattice 值系统与 SCCP Solver 深度集成，使常量传播能跨函数边界工作

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 函数克隆 | `CloneFunction()` | `llvm/Transforms/Utils/Cloning.h` |
| 链接设置 | `setLinkage()` | `llvm/IR/GlobalValue.h` |
| Profile 计数 | `setEntryCount()` | `llvm/IR/Function.h` |
| Lattice 值设置 | `setLatticeValueForSpecializationArguments()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 块可执行性 | `markBlockExecutable()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 函数跟踪 | `addArgumentTrackedFunction()` / `addTrackedFunction()` | `llvm/Transforms/Utils/SCCPSolver.h` |

---

### 与 `run()` 的交互

**调用时机** (791行):
```cpp
S.Clone = createSpecialization(S.F, S.Sig);
```

**后续操作** (794-821行):
- 重定向调用点到克隆函数
- 更新 profile 计数（累加调用点计数到克隆，从原函数减去）
- 调用 `Solver.solveWhileResolvedUndefsIn(Clones)` 传播常量

---

## cloneCandidateFunction 函数分析

### 函数签名与目的（894-900行）

```cpp
static Function *cloneCandidateFunction(Function *F, unsigned NSpecs)
```

**功能**: 克隆候选函数并清理 SCCP Solver 插入的 ssa_copy 指令，返回克隆后的函数指针。

---

### 整体结构

```
cloneCandidateFunction(F, NSpecs)
├── 创建值映射表 ValueToValueMapTy
├── 调用 CloneFunction 克隆整个函数
├── 设置克隆函数的名称为 "原函数名.specialized.N"
├── 移除克隆函数中的 ssa_copy 指令
└── 返回克隆后的函数指针
```

---

### 逐段注释

**1. 创建值映射并克隆函数（895-896行）**

```cpp
ValueToValueMapTy Mappings;
Function *Clone = CloneFunction(F, Mappings);
```

创建一个空的 `ValueToValueMapTy` 映射表，用于记录原函数到克隆函数的值映射关系。`CloneFunction` 是 LLVM 标准克隆 API（`llvm/Transforms/Utils/Cloning.h`），它会：
- 克隆函数的所有基本块和指令
- 更新映射表记录原值到克隆值的对应关系
- 保留函数属性和元数据

**2. 设置克隆函数名称（897行）**

```cpp
Clone->setName(F->getName() + ".specialized." + Twine(NSpecs));
```

为克隆函数设置独特的名称，格式为 `原函数名.specialized.N`，其中 N 是特化编号。这便于调试和识别特化版本。

**3. 清理 SSA 副本指令（898行）**

```cpp
removeSSACopy(*Clone);
```

调用 `removeSSACopy` 静态函数（617-627行）移除 SCCP Solver 插入的无用 bitcast 指令。这些指令是 SCCP 为了 PredicateInfo 机制插入的，但会干扰后续的 `promoteConstantStackValues` 优化。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ValueToValueMapTy` | - | 类型别名，实际为 `ValueMap<const Value *, WeakTrackingVH>`，记录克隆过程中的值映射 |

---

### 优化意图

1. **函数特化基础**: 为函数特化提供干净的函数副本，允许在克隆版本上应用常量传播优化
2. **消除冗余指令**: 移除 SCCP 插入的临时 bitcast，避免干扰后续优化（特别是 `promoteConstantStackValues`）
3. **保持可调试性**: 通过命名约定区分特化版本，便于在 IR dump 中识别

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 函数克隆 | `CloneFunction` | `llvm/Transforms/Utils/Cloning.h` |
| 值映射表 | `ValueToValueMapTy` | `llvm/Transforms/Utils/ValueMapper.h` |
| 指令替换 | `replaceAllUsesWith` | `llvm/IR/Instruction.h` |

---

### 其他补充

**调用上下文**: 此函数仅在 `createSpecialization`（1069-1093行）中被调用，是函数特化流程的核心步骤。克隆后的函数会被：
- 设置为内部链接（1075行）
- 初始化 SCCP 格状态（1083-1086行）
- 标记为已特化（1089行）

**与 removeSSACopy 的协作**: `removeSSACopy` 专门查找类型相同的 bitcast 指令（`BC->getType() != BC->getOperand(0)->getType()` 为假），这些通常是 SCCP 插入的 ssa_copy 指令，可以安全删除。

---

### removeSSACopy 函数分析（617-627行）

**函数签名**:

```cpp
static void removeSSACopy(Function &F)
```

**功能**: 移除函数中所有类型相同的冗余 bitcast 指令（ssa_copy 指令）。

**实现逻辑**:

```cpp
for (BasicBlock &BB : F) {
    for (Instruction &Inst : llvm::make_early_inc_range(BB)) {
        auto *BC = dyn_cast<BitCastInst>(&Inst);
        // 只移除类型相同的冗余 bitcast
        if (!BC || BC->getType() != BC->getOperand(0)->getType())
            continue;
        Inst.replaceAllUsesWith(BC->getOperand(0));
        Inst.eraseFromParent();
    }
}
```

**关键点**:
- 使用 `make_early_inc_range` 避免迭代器失效（因为会删除指令）
- 只处理类型相同的 bitcast（源类型和目标类型相同），这些是无操作指令
- 先 `replaceAllUsesWith` 再 `eraseFromParent`，保证删除安全性

**优化意图**: SCCP Solver 可能插入 bitcast 指令用于 **PredicateInfo 机制**，这些冗余指令会干扰 `promoteConstantStackValues()` 优化，因此需要清理。

---

## PredicateInfo 机制详解

### 核心概念

**PredicateInfo** 是 LLVM 的一种扩展 SSA 形式分析，用于**稀疏地传播基于条件分支和 `llvm.assume` 的约束信息**。

### 工作原理

**传统问题**：在分支后的代码中，编译器不知道某些值的具体约束。

**PredicateInfo 解决方案**：在分支的 true/false 边插入操作的副本（通常是 bitcast），并附加谓词信息，使优化 Pass 能轻松理解值的约束。

---

### 具体示例

**原始 IR**：

```llvm
%cmp = icmp eq i32 %x, 50
br i1 %cmp, label %true, label %false

true:
  ret i32 %x

false:
  ret i32 1
```

**问题**：在 `true` 块中，编译器不知道 `%x` 一定等于 50。

**应用 PredicateInfo 后**：

```llvm
%cmp = icmp eq i32 %x, 50
br i1 %cmp, label %true, label %false

true:
  %x.0 = bitcast i32 %x to i32    ; PredicateInfo 插入的副本
  ret i32 %x.0

false:
  ret i32 1
```

**关键**：通过 `getPredicateInfoFor(%x.0)` 可以获取：
- 该值被 `%cmp` 比较支配
- 位于 true 边
- 因此 `%x.0` 一定等于 50

---

### PredicateInfo 类型

| 类型 | 枚举值 | 说明 |
|------|---------|------|
| 分支谓词 | `PT_Branch` | 来自 `br` 指令的 true/false 边 |
| 条件 assume | `PT_ConditionAssume` | 来自 `llvm.assume` 指令 |
| Bundle assume | `PT_BundleAssume` | 来自属性 bundle assume |
| Switch 谓词 | `PT_Switch` | 来自 `switch` 指令的 case |

---

### 数据结构

**1. PredicateConstraint（约束表示）**

```cpp
struct PredicateConstraint {
  CmpInst::Predicate Predicate;  // 比较谓词（eq, ne, slt, etc.）
  Value *OtherOp;               // 另一个操作数
};
```

表示形如 `Op Predicate OtherOp` 的约束，例如 `%x == 50`。

**2. PredicateBase（基类）**

```cpp
class PredicateBase {
  PredicateType Type;     // 谓词类型
  Value *OriginalOp;     // 原始操作数
  Value *RenamedOp;      // 重命名后的操作数
  Value *Condition;      // 关联的条件
};
```

**3. PredicateBranch（分支谓词）**

```cpp
class PredicateBranch : public PredicateWithEdge {
  bool TrueEdge;          // true 边还是 false 边
  BasicBlock *From;        // 分支所在块
  BasicBlock *To;          // 目标块
};
```

**4. PredicateSwitch（Switch 谓词）**

```cpp
class PredicateSwitch : public PredicateWithEdge {
  Value *CaseValue;       // case 的值
  SwitchInst *Switch;      // switch 指令
};
```

---

### 优化意图

1. **稀疏传播**：只在需要的地方插入副本，避免代码膨胀
2. **约束传递**：使优化 Pass 能理解分支后的值约束
3. **死代码消除**：基于约束消除不可能的代码路径
4. **常量折叠**：在特定路径上将值替换为常量

---

### 与 FunctionSpecialization 的关系

在 `FunctionSpecialization` 中：

```cpp
static void removeSSACopy(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &Inst : llvm::make_early_inc_range(BB)) {
      auto *BC = dyn_cast<BitCastInst>(&Inst);
      // 只移除类型相同的冗余 bitcast
      if (!BC || BC->getType() != BC->getOperand(0)->getType())
        continue;
      Inst.replaceAllUsesWith(BC->getOperand(0));
      Inst.eraseFromParent();
    }
  }
}
```

**问题**：SCCP Solver 为了与 PredicateInfo 机制协作，会插入类型相同的 bitcast 指令（ssa_copy）。

**干扰**：这些冗余 bitcast 会干扰 `promoteConstantStackValues()` 优化，因为它无法识别这些是临时指令。

**解决**：`removeSSACopy()` 清理这些 PredicateInfo 相关的冗余指令，确保特化流程正常工作。

---

### 使用场景

**1. 值范围优化**

```llvm
%cmp = icmp slt i32 %x, 100
br i1 %cmp, label %lt100, label %ge100

lt100:
  ; PredicateInfo 告诉我们 %x < 100
  ret i32 %x
```

**2. 空指针检查消除**

```llvm
%cmp = icmp ne ptr %p, null
br i1 %cmp, label %nonnull, label %isnull

nonnull:
  ; PredicateInfo 告诉我们 %p != null
  %v = load i32, ptr %p
  ret i32 %v
```

**3. Assume 指令传播**

```llvm
call void @llvm.assume(i1 %cond)
; PredicateInfo 记录 %cond 为真的约束
```

---

### 关键 API

| 功能 | API | 说明 |
|------|-----|------|
| 查询谓词信息 | `getPredicateInfoFor(Value *V)` | 获取值的谓词约束 |
| 获取约束 | `getConstraint()` | 获取 `PredicateConstraint` |
| 验证 | `verifyPredicateInfo()` | 验证谓词信息正确性 |
| 打印 | `print()` | 调试输出 |

---

### 总结

**PredicateInfo** 是 LLVM 中的一种值约束传播机制，通过在分支路径插入操作的副本并附加谓词信息，使优化 Pass 能够：
1. 理解分支后的值约束
2. 基于约束进行优化（死代码消除、常量折叠）
3. 稀疏地传播信息，避免代码膨胀

在 `FunctionSpecialization` 中，需要清理 PredicateInfo 插入的冗余 bitcast，以避免干扰特化优化流程。

---

## SCCPSolver::solveWhileResolvedUndefsIn 函数分析

### 函数签名与目的（1011-1019行）

```cpp
void SCCPInstVisitor::solveWhileResolvedUndefsIn(SmallVectorImpl<Function *> &WorkList)
```

**功能**: 在指定的函数列表上迭代运行 SCCP 求解，直到这些函数中没有 undef 被解析为止。

---

### 整体结构

```
solveWhileResolvedUndefsIn(WorkList)
├── 初始化 Resolvedudnefs = true
├── while (Resolvedudnefs)
│   ├── 调用 solve() 进行一次完整求解
│   ├── 重置 Resolvedudnefs = false
│   ├── 遍历 WorkList 中的每个函数
│   │   └── 检查该函数中是否有 undef 被解析
│   └── 如果任何函数有 undef 被解析，继续迭代
└── 返回
```

---

### 逐段注释

**1. 初始化迭代标志（1012行）**

```cpp
bool Resolvedudnefs = true;
```

初始设置为 `true`，确保至少执行一次迭代。

**2. 迭代求解循环（1013-1018行）**

```cpp
while (Resolvedudnefs) {
  solve();
  Resolvedudnefs = false;
  for (Function *F : WorkList)
    Resolvedudnefs |= resolvedudnefsIn(*F);
}
```

**关键步骤**：
1. **调用 `solve()`**：执行一次完整的 SCCP 求解，包括：
   - 处理基本块工作列表
   - 处理指令工作列表
   - 标记常量、overdefined 等
   - 更新 lattice 值

2. **重置标志**：`Resolvedudnefs = false`，准备检查本轮是否有进展

3. **检查进展**：遍历 `WorkList` 中的每个函数，调用 `resolvedudnefsIn(*F)` 检查该函数中是否有 undef 被解析

4. **继续条件**：如果任何函数有 undef 被解析，`Resolvedudnefs` 为 `true`，继续下一轮迭代

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `WorkList` | `SmallVectorImpl<Function *>` | 需要求解的函数列表 |
| `Resolvedudnefs` | `bool` | 本轮迭代中是否有 undef 被解析 |

---

### 优化意图

1. **增量求解**：只在指定的函数列表中求解，避免全局范围的重复计算
2. **收敛保证**：通过迭代直到收敛，确保所有可能的常量传播都被发现
3. **性能优化**：相比全局求解，减少不必要的函数遍历和计算
4. **特化支持**：在 `FunctionSpecialization` 中用于在克隆函数上传播特化参数

---

### 与其他求解函数的对比

| 函数 | 求解范围 | 使用场景 |
|------|---------|---------|
| `solveWhileResolvedudnefsIn(WorkList)` | 指定函数列表 | 函数特化后传播常量 |
| `solveWhileResolvedudnefs()` | 全局失效值 | 调用点修改后的全局传播 |
| `solveWhileResolvedudnefsIn(Module &M)` | 整个模块 | 全局求解 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单次求解 | `solve()` | 900行 |
| 检查函数 undef 解析 | `resolvedudnefsIn(Function &F)` | 902-904行 |
| 全局求解 | `solveWhileResolvedudnefs()` | 1021-1031行 |

---

### 调用上下文

**在 FunctionSpecialization 中的使用**：

```cpp
// 首次求解（827行）
Solver.solveWhileResolvedudnefsIn(Clones);

// 二次求解（862行）
Solver.solveWhileResolvedudnefs();
```

**调用时机**：
1. **首次调用**：在克隆函数创建后，只在克隆函数上传播特化参数
2. **二次调用**：在调用点重定向后，全局传播常量值

---

### 算法复杂度

**最坏情况**：O(K × N × M)
- K：迭代轮数（通常 < 10）
- N：`WorkList` 中的函数数量
- M：每个函数中的指令数量

**实际性能**：由于 SCCP 的稀疏特性，通常只需要 2-3 轮迭代即可收敛。

---

### 其他补充

**与 `resolvedudnefsIn()` 的协作**：

```cpp
bool resolvedudnefsIn(Function &F) {
  bool Resolvedudnefs = false;
  for (Value *V : Invalidated)
    if (auto *I = dyn_cast<Instruction>(V))
      if (I->getParent()->getParent() == &F)
        Resolvedudnefs |= resolvedudnef(*I);
  return Resolvedudnefs;
}
```

`resolvedudnefsIn()` 检查指定函数中是否有失效的指令被解析为常量或 overdefined。

**收敛保证**：由于每次迭代至少会解析一个 undef（如果有），且指令数量有限，算法保证收敛。

---

## updateCallSites 函数分析

### 函数签名与目的（1211-1260）

```cpp
void FunctionSpecializer::updateCallSites(Function *F, const Spec *Begin,
                                      const Spec *End);
```

**功能**: 更新函数 `F` 的所有调用点，将其重定向到最佳匹配的特化版本；如果原函数的所有调用点都被特化版本替换，则标记原函数为不可达并可删除。

---

### 整体结构

```
updateCallSites(F, Begin, End)
├── 收集需要更新的调用点（遍历 F 的所有 user）
├── 对每个调用点：
│   ├── 查找最佳匹配的特化版本
│   ├── 如果找到匹配，重定向调用点
│   └── 更新剩余调用计数
└── 如果函数完全特化（无剩余调用），标记为不可达
```

---

### 逐段注释

**1. 收集需要更新的调用点（1213-1219）**

```cpp
SmallVector<CallBase *> ToUpdate;
for (User *U : F->users())
  if (auto *CS = dyn_cast<CallBase>(U);
      CS && CS->getCalledFunction() == F &&
      Solver.isBlockExecutable(CS->getParent()))
    ToUpdate.push_back(CS);
```

遍历 `F` 的所有用户，筛选出：
- 是 `CallBase`（`CallInst` 或 `InvokeInst`）
- 调用的目标函数确实是 `F`
- 所在基本块是可执行的（由 SCCP Solver 判断）

**2. 遍历并更新每个调用点（1221-1249）**

```cpp
unsigned NCallsLeft = ToUpdate.size();
for (CallBase *CS : ToUpdate) {
  bool ShouldDecrementCount = CS->getFunction() == F;

  const Spec *BestSpec = nullptr;
  for (const Spec &S : make_range(Begin, End)) {
    if (!S.Clone || (BestSpec && S.Score <= BestSpec->Score))
      continue;

    if (any_of(S.Sig.Args, [CS, this](const ArgInfo &Arg) {
          unsigned ArgNo = Arg.Formal->getArgNo();
          return getCandidateConstant(CS->getArgOperand(ArgNo)) != Arg.Actual;
        }))
      continue;

    BestSpec = &S;
  }

  if (BestSpec) {
    CS->setCalledFunction(BestSpec->Clone);
    ShouldDecrementCount = true;
  }

  if (ShouldDecrementCount)
    --NCallsLeft;
}
```

- **1223**: `ShouldDecrementCount` 标记是否递减剩余调用计数。初始值判断调用点是否在原函数内部（递归调用）。
- **1227-1238**: 遍历所有候选特化版本 `[Begin, End)`，寻找最佳匹配：
  - 跳过没有克隆的特化或分数不高于当前最佳
  - 检查所有特化参数是否匹配（`getCandidateConstant` 获取调用点的实际常量参数，与特化签名中的 `Arg.Actual` 比较）
  - 如果所有参数都匹配，则该特化是候选
- **1240-1245**: 如果找到匹配的特化，将调用点重定向到特化版本 `Clone`，并设置 `ShouldDecrementCount = true`。
- **1247-1248**: 如果应该递减计数，则减少 `NCallsLeft`。

**3. 检查函数是否完全特化（1251-1259）**

```cpp
if (NCallsLeft == 0 && Solver.isArgumentTrackedFunction(F) &&
    !F->hasAddressTaken()) {
  Solver.markFunctionUnreachable(F);
  DeadFunctions.insert(F);
}
```

- 如果剩余调用数为 0，且：
  - 函数的参数被 SCCP Solver 追踪（`isArgumentTrackedFunction`）
  - 函数地址未被取用（`!hasAddressTaken()`）
- 则标记函数为不可达，并加入 `DeadFunctions` 集合供后续删除。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Spec` | `Clone` | 特化后的函数克隆 |
| `Spec` | `Score` | 特化的收益评分（用于选择最佳匹配） |
| `Spec` | `Sig.Args` | 特化签名：包含形式参数和实际常量值 |
| `ArgInfo` | `Formal` | 形式参数（`Argument*`） |
| `ArgInfo` | `Actual` | 实际常量值（`Constant*`） |

---

### 优化意图

1. **最大化常量传播收益**: 将调用点重定向到参数已固化为常量的特化版本，使后续优化（如死代码消除、常量折叠）生效。
2. **支持递归函数特化**: 处理递归调用场景，通过多次迭代逐步将递归调用替换为特化版本。
3. **清理无用函数**: 当原函数的所有调用点都被特化版本替换后，原函数可被删除，减少代码膨胀。
4. **选择最佳匹配**: 当多个 `Spec` 匹配同一调用点时，选择 `Score` 最高的版本（收益最大）。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 遍历用户 | `F->users()` | `llvm/IR/Value.h` |
| 类型转换 | `dyn_cast<CallBase>` | `llvm/IR/Instructions.h` |
| 获取调用目标 | `CS->getCalledFunction()` | `llvm/IR/InstrTypes.h` |
| 重定向调用 | `CS->setCalledFunction()` | `llvm/IR/InstrTypes.h` |
| 检查地址取用 | `F->hasAddressTaken()` | `llvm/IR/Function.h` |
| 获取常量候选 | `getCandidateConstant()` | 本文件 1191 |
| 检查块可执行 | `Solver.isBlockExecutable()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 标记函数不可达 | `Solver.markFunctionUnreachable()` | `llvm/Transforms/Utils/SCCPSolver.h` |

**使用示例**: 在 `FunctionSpecializer::run()` 中调用（832-835 行），用于在创建特化后更新剩余调用点（包括递归调用和动态匹配的调用点）。

---

### 其他补充

**调用时机**:
- 在 `[run()](FunctionSpecialization.cpp:676)` 中，首次创建特化并更新已知调用点后（794-821 行）
- 运行 `Solver.solveWhileResolvedUndefsIn(Clones)` 后（827 行）
- 再次调用 `updateCallSites` 处理：
  - 递归调用（原函数内部的调用）
  - 被丢弃的特化版本的调用点
  - Solver 运行后新匹配的调用点（常量传播后更多调用点可能匹配）

**与递归函数的关系**:
- 递归函数的调用点初始不在 `S.CallSites` 中（962-963 行跳过 `CS.getFunction() == F`）
- `updateCallSites` 负责在后续迭代中处理这些递归调用
- 配合 `promoteConstantStackValues`（585-613 行）支持递归函数的栈变量常量传播

**性能考虑**:
- `getCandidateConstant` 在内层循环中调用（1233 行），可能涉及 Solver 查询
- 特化数量通常较少（受 `MaxClones` 限制，默认 3），因此遍历 `[Begin, End)` 开销可控
---

## 多个特化版本匹配同一调用点的原因

### 核心原因：特化参数数量的差异

在 `FunctionSpecialization` 中，**特化版本是按"常量参数组合"创建的**，不同调用点可能产生**特化参数数量不同**的版本。

---

### 场景示例

**假设原函数**：
```cpp
void foo(int x, int y, int z) {
    // ... 函数体
}
```

**多个调用点**：
```cpp
foo(42, 100, var1);    // 调用点1：前两个参数是常量
foo(42, 100, 200);     // 调用点2：三个参数都是常量
foo(42, 100, 200);     // 调用点3：与调用点2相同
```

**生成的特化版本**：
1. **`foo.1(x=42, y=100)`**：来自调用于点1，只特化了2个参数
2. **`foo.2(x=42, y=100, z=200)`**：来自调用于点2，特化了3个参数

**匹配逻辑**（`updateCallSites` 1227-1238行）：
```cpp
for (const Spec &S : make_range(Begin, End)) {
    // 跳过没有克隆的特化或分数不高于当前最佳
    if (!S.Clone || (BestSpec && S.Score <= BestSpec->Score))
        continue;

    // 检查所有特化参数是否匹配
    if (any_of(S.Sig.Args, [CS, this](const ArgInfo &Arg) {
          unsigned ArgNo = Arg.Formal->getArgNo();
          return getCandidateConstant(CS->getArgOperand(ArgNo)) != Arg.Actual;
        }))
        continue;

    BestSpec = &S;
}
```

**匹配结果**：
- 调用点3 `foo(42, 100, 200)` **同时匹配** `foo.1` 和 `foo.2`
  - `foo.1`：`x=42, y=100` 都匹配 ✓
  - `foo.2`：`x=42, y=100, z=200` 都匹配 ✓
- **最终选择**：`foo.2`（因为 `Score` 更高，特化参数更多 → 收益更大）

---

### 为什么会产生部分特化版本？

**`findSpecializations` 的逻辑**（941-948行）：
```cpp
SpecSig S;
for (Argument *A : Args) {
    Constant *C = getCandidateConstant(CS.getArgOperand(A->getArgNo()));
    if (!C)
        continue;  // 非常量参数跳过
    S.Args.push_back({A, C});  // 只收集常量参数
}
```

- 每个 `SpecSig` **只包含常量参数**
- 不同调用点的常量参数数量可能不同
- 因此会生成**特化程度不同**的多个版本

---

### 其他场景

#### 1. 递归函数的多轮特化

**第一轮**：
```llvm
define void @foo(i32 %x, i32 %y) {
  %cond = icmp eq i32 %x, 42
  br i1 %cond, label %then, label %else

then:
  call void @foo(i32 42, i32 %y)  ; 递归调用
  ret void

else:
  ret void
}
```
- 发现调用点 `foo(42, %y)` → 创建 `foo.1(x=42)`

**第二轮**（`foo.1` 内部）：
```llvm
define void @foo.1(i32 %x, i32 %y) {
  ; %x 已固化为 42
  %cond = icmp eq i32 42, 42  ; 常量折叠后变为 true
  br i1 true, label %then, label %else

then:
  call void @foo(i32 42, i32 %y)  ; 递归调用
  ret void
}
```
- 如果 `%y` 在某些调用点也是常量，可能创建 `foo.2(x=42, y=100)`
- 原调用点 `foo(42, 100)` 同时匹配 `foo.1` 和 `foo.2`

#### 2. PGO/Profile 指导的差异化

不同调用点可能有不同的 profile 计数，导致：
- 热点调用点：创建全参数特化（收益高）
- 冷点调用点：创建部分参数特化（节省代码大小）
- 同一常量组合的调用点可能匹配多个版本

---

### 选择策略：按 Score 排序

**代码逻辑**（1228行）：
```cpp
if (BestSpec && S.Score <= BestSpec->Score)
    continue;  // 跳过分数不高于当前的
```

**Score 组成**（1018行）：
```cpp
Score += std::max(CodeSizeSavings, LatencySavings);
```

- **特化参数越多** → `CodeSizeSavings` 越大 → `Score` 越高
- **执行频率越高** → `LatencySavings` 越大 → `Score` 越高
- **内联机会越多** → `InliningBonus` 越大 → `Score` 越高

**结论**：`updateCallSites` 会选择**收益最高**的特化版本，通常是**特化参数最多**的版本。

---

### 设计权衡

| 策略 | 优点 | 缺点 |
|------|------|------|
| **允许部分特化** | 适应不同调用点的常量组合，避免代码膨胀 | 需要选择逻辑，增加复杂度 |
| **只创建全参数特化** | 简单，无歧义 | 可能错过优化机会，或产生过多版本 |
| **当前实现** | 平衡收益和代码大小，通过 Score 选择最佳 | 需要维护多个版本 |

---

### 总结

多个特化版本匹配同一调用点的根本原因是：
1. **特化参数数量不同**：部分特化 vs 全参数特化
2. **不同调用点产生不同特化**：`findSpecializations` 为每个常量组合创建版本
3. **递归/多轮特化**：后续轮次可能创建更细粒度的版本
4. **选择机制**：通过 `Score` 选择收益最高的版本，通常是最完整的特化

这种设计允许编译器在**代码大小**和**优化收益**之间取得平衡。
---

## 处理常量返回值的特化函数（837-862行）

### 函数签名与目的（837-862行）

位于 `FunctionSpecializer::run()` 中，处理常量返回值的特化函数，重置其调用点的晶格值并重新运行 SCCP 求解器。

---

### 整体结构

```
处理常量返回值的特化函数
├── 遍历所有特化函数（Clones）
│   ├── 跳过 void 返回类型
│   ├── 检查返回值是否为常量
│   │   ├── struct 类型：检查 isStructLatticeConstant
│   │   └── 非struct 类型：检查 tracked ret val 是否 overdefined
│   └── 遍历该函数的调用点
│       └── 重置调用点的晶格值（resetLatticeValueFor）
└── 重新运行求解器（solveWhileResolvedUndefs）
```

---

### 逐段注释

**1. 遍历所有特化函数（837-850行）**

```cpp
for (Function *F : Clones) {
  if (F->getReturnType()->isVoidTy())
    continue;
  if (F->getReturnType()->isStructTy()) {
    auto *STy = cast<StructType>(F->getReturnType());
    if (!Solver.isStructLatticeConstant(F, STy))
      continue;
  } else {
    auto It = Solver.getTrackedRetVals().find(F);
    assert(It != Solver.getTrackedRetVals().end() &&
           "Return value ought to be tracked");
    if (SCCPSolver::isOverdefined(It->second))
      continue;
  }
```

这段代码筛选出返回值是常量的特化函数：
- void 返回类型无法传播常量，跳过
- struct 类型：通过 `isStructLatticeConstant` 检查整个结构体是否为常量
- 非struct 类型：检查返回值是否在 SCCP 求解器的 tracked ret vals 中且不为 overdefined（即可能是常量）

**2. 重置调用点的晶格值（851-859行）**

```cpp
  for (User *U : F->users()) {
    if (auto *CS = dyn_cast<CallBase>(U)) {
      //The user instruction does not call our function.
      if (CS->getCalledFunction() != F)
        continue;
      Solver.resetLatticeValueFor(CS);
    }
  }
}
```

对于返回值是常量的特化函数，遍历其所有用户（调用点），重置这些调用点的晶格值。这会导致 SCCP 求解器重新计算这些调用点的值，从而将常量返回值传播到调用者。

**3. 重新运行求解器（861-862行）**

```cpp
// Rerun the solver to notify the users of the modified callsites.
Solver.solveWhileResolvedUndefs();
```

重新运行 SCCP 求解器，将常量返回值传播到所有受影响的调用点。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Clones` | `SmallVector<Function *>` | 所有创建的特化函数 |
| `Solver` | `SCCPSolver` | 稀疏条件常量传播求解器 |
| `TrackedRetVals` | `DenseMap<Function *, ValueLatticeElement>` | 求解器跟踪的函数返回值晶格状态 |

---

### 优化意图

1. **常量传播**：特化函数的参数常量化后，其返回值可能也变为常量。通过重置调用点的晶格值，触发求解器重新计算，将将常量返回值传播到调用者。

2. **连锁优化**：常量返回值传播到调用者后，调用者内部的指令可能进一步常量化，触发更多优化机会（如死代码消除、分支简化等）。

3. **正确性保证**：`resetLatticeValueFor` 清除调用点的旧晶格值，确保求解器使用最新的函数定义（特化后的版本）重新计算。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 检查结构体返回值是否为常量 | `isStructLatticeConstant` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 获取跟踪的返回值 | `getTrackedRetVals()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 检查晶格值是否 overdefined | `isOverdefined` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 重置调用点的晶格值 | `resetLatticeValueFor` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 运行求解器 | `solveWhileResolvedUndefs` | `llvm/Transforms/Utils/SCCPSolver.h` |

---

### 其他补充

这段代码是函数特化流程中的关键步骤，位于以下操作之后：
- 创建特化函数（791行）
- 更新已知调用点调用特化版本（794-821行）
- 对特化函数运行一次求解器（827行）

其目的是确保特化函数的常量返回值能够传播到调用者，从而最大化常量传播的收益。

---

## Lattice（晶格）的作用

### Lattice 的完整状态

`ValueLatticeElement` 有 **7 种状态**：

| 状态 | 含义 | 例子 | 转换方向 |
|---|---|---|---|
| **unknown** | 值未知，初始状态 | `int x;` | → 任意状态 |
| **undef** | 值是 `undef` | `int x = undef;` | → constant, constantrange_including_undef, overdefined |
| **constant** | 值是特定常量 | `int x = 42;` | → overdefined |
| **notconstant** | 值不是某个特定值 | `x != 5` | → overdefined |
| **constantrange** | 值在某个范围内（整数） | `0 <= x <= 100` | → constantrange（范围扩大）, constantrange_including_undef, overdefined |
| **constantrange_including_undef** | 值在范围内，也可能是 undef | `x ∈ [0,100] ∪ {undef}` | → overdefined |
| **overdefined** | 无法精确建模，停止传播 | `x = phi(1, 2, 3)` | 终态 |

---

### 完整例子展示 Lattice 传播

**特化前的代码**：

```c
int compute(int x) {
  if (x > 10) {
    return x * 2;  // 路径1
  } else {
    return x + 5;  // 路径2
  }
}

int main() {
  int a = compute(20);  // 返回 40
  int b = compute(5);   // 返回 10
  return a + b;
}
```

**特化前的 Lattice 状态**：

```
compute 函数：
  参数 x: overdefined（合并 20 和 5 后）
  返回值: overdefined（两条路径返回不同值）

main 函数：
  %a = compute(20): overdefined
  %b = compute(5):  overdefined
  %result = a + b: overdefined
```

---

**特化后（针对 x=20 创建特化版本）**：

```c
int compute.1(int x) {  // x 常量化为 20
  if (20 > 10) {       // 条件变为 true
    return 40;           // 常量折叠
  } else {
    return 25;           // 死代码
  }
}
```

**第一轮求解器后的 Lattice 状态**：

```
compute.1 函数：
  参数 x: constant(20)      // 特化时设置
  返回值: constant(40)     // 求解器计算得出

main 函数：
  %a = compute(20): overdefined  // 调用点未更新，仍指向原函数
  %b = compute(5):  overdefined
  %result = a + b: overdefined
```

---

**执行 837-862 行：resetLatticeValueFor + 重新求解**

```cpp
// 1. 发现 compute.1 返回值是 constant(40)
if (!SCCPSolver::isOverdefined(It->second))
  continue;  // 不跳过，继续处理

// 2. 找到调用点 %a = compute(20)，重置其 lattice
Solver.resetLatticeValueFor(%a);
// %a 的 lattice: overdefined → unknown

// 3. 重新运行求解器
Solver.solveWhileResolvedUndefs();
```

**第二轮求解器后的 Lattice 状态**：

```
compute.1 函数：
  返回值: constant(40)

main 函数：
  %a = compute.1(20): constant(40)  // 传播成功！
  %b = compute(5):   overdefined
  %result = 40 + b:   constantrange([40+∞, 40+∞])  // 部分常量化
```

---

### 更复杂的例子：展示 constantrange

```c
int clamp(int x) {
  if (x < 0) return 0;
  if (x > 100) return 100;
  return x;
}

int main() {
  int a = clamp(50);  // 返回 50
  int b = clamp(-5); // 返回 0
  return a + b;
}
```

**特化 clamp.1 针对 x=50 后的 Lattice 传播**：

```
clamp.1 函数：
  参数 x: constant(50)
  if (50 < 0): constant(false)  // 条件简化
  if (50 > 100): constant(false) // 条件简化
  返回值: constant(50)

main 函数：
  %a = clamp.1(50): constant(50)
  %b = clamp(-5):  overdefined
  %result = 50 + b: constantrange([50+∞, 50+∞])
```

---

### Lattice 状态转换规则

**mergeIn 操作的转换规则**：

```
unknown ∪ X → X
undef ∪ constant → constant_including_undef
undef ∪ constantrange → constantrange_including_undef
constant(5) ∪ constant(5) → constant(5)
constant(5) ∪ constant(10) → overdefined
constantrange([0,10]) ∪ constantrange([5,15]) → constantrange([0,15])
constantrange([0,10]) ∪ constantrange([20,30]) → overdefined
X ∪ overdefined → overdefined
```

---

### 为什么 resetLatticeValueFor 关键？

**不重置的情况**：
```
%a 的 lattice: overdefined
求解器："已经是 overdefined，无需计算"
→ 常量返回值无法传播
```

**重置后**：
```
%a 的 lattice: unknown
求解器："需要重新计算"
→ 重新评估 %a = compute.1(20)
→ 发现 compute.1 返回 constant(40)
→ %a 提升为 constant(40)
→ 传播到使用者
```

---

### 优化效果

经过 lattice 重置和重新求解后，可触发后续优化：

```c
int main() {
  int a = 40;        // 常量传播成功
  int b = compute(5); // 仍需调用
  return 40 + b;      // 可进一步优化为 b + 40
}
```

如果 `compute(5)` 也被特化并传播常量，最终可优化为：

```c
int main() {
  return 50;  // 完全常量化！
}
```

---

### 总结

Lattice 的 7 种状态实现了**渐进式常量传播**：
- 从 `unknown` 开始
- 随着分析深入，状态从精确（`constant`）到不精确（`constantrange`）再到无法建模（`overdefined`）
- `resetLatticeValueFor` 将调用点重置为 `unknown`，强制求解器重新计算，利用特化函数的精确信息提升 lattice 状态

837-862 行代码的核心作用是**触发 lattice 状态的重新计算和传播**，将特化函数的精确返回值信息传递给调用者。

---
