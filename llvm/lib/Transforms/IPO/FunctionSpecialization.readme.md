# FunctionSpecialization Pass 源码分析文档

本文档对应实现文件：`llvm/lib/Transforms/IPO/FunctionSpecialization.cpp`、接口：`llvm/include/llvm/Transforms/IPO/FunctionSpecialization.h`。行号以当前树为准，若上游改动请以源码为准。

## 文档导航

| 章节 | 内容 |
|------|------|
| [整体分析](#整体分析) | 定位、与 IPSCCP 的关系、数据流、阈值与权衡 |
| [`run()` 分阶段走读](#functionspecializerrun-代码分析) | 候选发现 → Top-K → 克隆与 SCCP → 调用点更新 |
| [递归与栈常量提升](#promoteconstantstackvalues-代码分析) | `promoteConstantStackValues` / `getPromotableAlloca` |
| [InstCostVisitor 总览](#instcostvisitor-常量传播估算总览) / [详细](#instcostvisitor-常量传播估算分析) | 收益估算、PHI/死块、`visit*` 族 |
| [SCCPSolver 与特化](#sccpsolver-交互机制) | lattice 初始化、`solveWhileResolvedUndefsIn`、常量返回值 |
| [函数级详解](#findspecializations-函数分析) | `findSpecializations`、`createSpecialization`、`updateCallSites` 等 |

---

## 整体分析

### 1. 基本定位

- **Pass 类型**：IPO 变换逻辑；**并非**独立的 `PassManager` 注册名（`PassRegistry.def` 中无单独 `function-specialization` 条目）。
- **粒度**：Module（通过 `FunctionSpecializer` 遍历模块内函数与调用点）。
- **入口**：`FunctionSpecializer::run()`（`FunctionSpecialization.cpp`）。
- **与 IPSCCP 的关系（关键点）**：特化逻辑嵌在 **IPSCCP**（`llvm/lib/Transforms/IPO/SCCP.cpp` 的 `runIPSCCP`）中：先对全模块做一次 `Solver.solveWhileResolvedUndefsIn(M)`，若启用函数特化（`IPSCCPOptions::AllowFuncSpec`，默认 true），则在**同一 `SCCPSolver` 实例**上循环调用 `Specializer.run()`，最多 `funcspec-max-iters`（默认 10）次，然后再进入 IPSCCP 的替换/清理阶段。因此更准确的表述是「**IPSCCP 求解阶段之后、IPSCCP 指令替换之前，在同一 Pass 内的特化子阶段**」，而不是「整个 IPSCCP Pass 之后再跑一个独立 Pass」。
- **一句话职责**：对带常量实参（或可视为常量）的调用点克隆特化体，把形参钉死在 lattice 上，再跑稀疏条件常量传播，使跨函数边界的常量信息与死代码机会暴露给后续 IPSCCP/内联等优化。

**启用/关闭**：`opt -passes='ipsccp'` 时可用解析选项 `func-spec` / `no-func-spec`（见 `PassRegistry.def` 中 `parseIPSCCPOptions`）控制是否执行特化子阶段。

### 2. 输入、输出与前置条件

- **输入 IR**：含直接调用/invoke、且实参可被抽象为「特化用常量」的 Module；**与 IPSCCP 共用**已建立 PredicateInfo、全局/函数跟踪后的 `SCCPSolver` 状态。
- **输出结果**：`*.specialized.N` 克隆体、`internal` linkage、调用点重定向、`DeadFunctions` 中待删原函数（析构时清理）、PGO 元数据在重定向时的迁移（若启用 profile 修复）。
- **值得特化的形参类型（概要）**：指针；在 `-funcspec-for-literal-constant`（默认开启）等条件下还可包含整型/浮点/结构体等字面量相关情形（详见 `getCandidateConstant` / `isArgumentInteresting` 实现）。
- **分析依赖**：`CodeMetrics`、`TargetTransformInfo`、延迟估算时的 `BlockFrequencyInfo`、`AssumptionCache` 与 `TargetLibraryInfo`（用于 `getInliningBonus` 内联成本）。
- **典型跳过场景**：
  - 声明、无参、`noduplicate`、已位于 `Specializations` 集合、`shouldOptimizeForSize`、入口块不可执行、`alwaysinline`
  - 调用点带 `minsize`、调用所在块不可执行
  - 地址被频繁取用或无法安全重定向全部调用时（由 `canTrackArgumentsInterprocedurally` 等与 IPSCCP 共同约束）

### 3. 关键函数调用链

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

### 4. 整体执行流程

1. **候选函数筛选**：遍历 Module，结合 `CodeMetrics`（体积、递归、`notDuplicatable`）与 `isCandidateFunction`
2. **特化机会发现**：`findSpecializations` 扫描调用点，构建 `SpecSig`，`UniqueSpecs` 去重并合并同一签名下的多个 `CallSites`
3. **收益评估**：`InstCostVisitor`（代码大小 + pending PHI）+ `getInliningBonus` + 惰性 `getLatencySavingsForKnownConstants`
4. **Top-K 选择**：`N = min(NumCandidates × MaxClones, |AllSpecs|)`，堆维护最高分候选
5. **创建与重定向**：`createSpecialization` → 已知调用点立即 `setCalledFunction`，并可选修正 BFI profile 计数
6. **克隆体上 SCCP**：`solveWhileResolvedUndefsIn(Clones)`
7. **`updateCallSites`**：递归/未预先列入的调用点匹配「最具体」特化
8. **常量返回值**：若特化体返回值为 lattice 常量，对调用点 `resetLatticeValueFor` 后再 `solveWhileResolvedUndefs()`
9. **递归栈提升**：对递归原函数再次 `promoteConstantStackValues`，便于**下一轮** IPSCCP/特化迭代（外层还有 `funcspec-max-iters`）

### 4.1 关键设计不变式

- **共享求解器**：特化只扩展/重定向 IR，不新建独立 SCCP；克隆体通过 `setLatticeValueForSpecializationArguments` 把形参钉死为常量，与 IPSCCP 其余阶段共用 lattice。
- **收益必须先于克隆**：`findSpecializations` 内完成盈利性判断，避免无意义克隆。
- **特化函数命名**：`cloneCandidateFunction` 生成 `F.specialized.N`，并 `removeSSACopy` 清理 SCCP 为 PredicateInfo 插入的冗余 bitcast。
- **析构收尾**：`~FunctionSpecializer` 调用 `removeDeadFunctions`（不可达调用换 `poison`）与 `cleanUpSSA`。

### 5. 核心逻辑拆解

#### 5.1 候选函数筛选 (isCandidateFunction, 1040-1067)

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

#### 5.2 特化机会发现与收益评估 (findSpecializations, 919-1035)

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

#### 5.3 常量传播与死代码估算 (getCodeSizeSavingsForUser, 219-256)

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

#### 5.4 特化函数创建 (createSpecialization, 1069-1093)

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

### 6. 关键数据结构与 LLVM API

| 名称 | 作用 | 关键字段/接口 | 位置 |
|---|---|---|---|
| `SpecSig` | 特化签名（参数-常量映射） | `SmallVector<ArgInfo> Args` | 头文件 |
| `Spec` | 特化候选 | `Function *F`, `SpecSig Sig`, `unsigned Score`, `Function *Clone` | 头文件 |
| `InstCostVisitor` | 常量传播估算器 | `KnownConstants`, `PendingPHIs`, `DeadBlocks` | 类成员 |
| `SCCPSolver` | 常量传播求解器 | `getConstantOrNull()`, `setLatticeValueForSpecializationArguments()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| `CodeMetrics` | 函数代码度量 | `NumInsts`, `isRecursive`, `notDuplicatable` | `llvm/Analysis/CodeMetrics.h` |
| `TargetTransformInfo` | 指令成本查询 | `getInstructionCost(I, TCK_CodeSize/Latency)` | `llvm/Analysis/TargetTransformInfo.h` |

### 7. 正确性约束与易错点

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

### 8. 分析依赖与 Pass 交互

**与 IPSCCP 同 Pass 内的分工**：特化子阶段运行在 `runIPSCCP` 首次 `solveWhileResolvedUndefsIn(M)` **之后**、IPSCCP 对指令做常量替换/删块 **之前**；`SCCPSolver` 上的 lattice、可执行块信息在此阶段被读取并更新。

**依赖的 analyses / 设施**（由 `FunctionSpecializer` 构造参数传入）：
- `SCCPSolver`：`isBlockExecutable`、`getConstantOrNull`、特化形参 lattice 写入
- `CodeMetrics`：规模与递归判断
- `TargetTransformInfo`：`TCK_CodeSize` / `TCK_Latency`
- `BlockFrequencyInfo`：延迟节省加权
- `AssumptionCache`、`TargetLibraryInfo`：`getInliningBonus` 中的 `getInlineCost`

**IPSCCP Pass 的 `PreservedAnalyses`**（见 `IPSCCPPass::run`）：特化会改 Module，但 IPSCCP 仍显式 `preserve` 若干分析；**不要**与「若单独写一个 Module pass 则 `PreservedAnalyses::none()`」混淆——本文档描述的特化代码本身不直接返回 `PreservedAnalyses`，它嵌入 IPSCCP。

**为同一 Pass 内后续步骤及其它 Pass 创造的机会**：
- IPSCCP 替换阶段能利用特化体与更新后的调用图做更多常量折叠
- 间接调用直调化后有利于 **Inliner**；常量形参有利于 **DCE**、循环相关优化

### 9. 收益模型 / 编译时权衡

**主要收益**：
1. **代码大小节省**：常量传播后死代码消除
2. **延迟节省**：消除运行时计算，替换为编译时常量
3. **内联收益**：间接调用提升为直接调用，进而内联

**主要 CLI 阈值（`FunctionSpecialization.cpp` / `SCCP.cpp`）**：
- `funcspec-min-function-size`（默认 500）：过小函数跳过，抑制特化爆炸
- `funcspec-max-clones`（默认 3）：单原函数预算内最多保留的特化个数（与 Top-K 一起作用）
- `funcspec-min-codesize-savings` / `funcspec-min-latency-savings`（默认 20%）：相对 `FuncSize` 的节省比例下限
- `funcspec-min-inlining-bonus`（默认 300%）：间接调用直调化收益阈值（相对函数大小）
- `funcspec-max-codesize-growth`（默认 3）：单原函数累计 `FunctionGrowth` 允许的最大相对膨胀
- `funcspec-max-iters`（**SCCP.cpp**，默认 10）：IPSCCP 内 `while` 调用 `Specializer.run()` 的上限
- `funcspec-max-discovery-iterations` / `funcspec-max-incoming-phi-values` / `funcspec-max-block-predecessors`：`InstCostVisitor` 对 PHI 与死块估计的防爆参数

**编译时成本来源**：
- 遍历所有函数和调用点
- `InstCostVisitor` 递归传播常量（复杂度 O(指令数)）
- SCCP Solver 求解
- 函数克隆

**可能的 trade-off**：
- 收益高但编译时也高：需要保守的阈值
- 递归函数可能需要多次迭代：`promoteConstantStackValues()` 支持迭代
- 间接调用提升收益难以精确估计：使用启发式

### 10. 验证与调试方法

**建议看的测试**：
- `llvm/test/Transforms/FunctionSpecialization/` 目录
- 关注递归函数、间接调用、多参数特化的测试用例

**建议的 `opt` 命令**（特化在 **ipsccp** 中触发；`DEBUG_TYPE` 为 `function-specialization`）：
```bash
# 查看特化调试输出
opt -passes=ipsccp -debug-only=function-specialization -disable-output input.ll

# 关闭特化子阶段，仅跑 IPSCCP 常量传播
opt -passes='ipsccp(no-func-spec)' -disable-output input.ll

# 强制特化（忽略部分盈利性检查，慎用）
opt -passes=ipsccp -force-specialization -disable-output input.ll

# 调整特化预算/阈值
opt -passes=ipsccp -funcspec-max-clones=5 -funcspec-min-function-size=100 -disable-output input.ll
```

**应关注的 IR 前后差异**：
- 新增的 `.specialized.*` 函数
- 调用点从原函数重定向到特化函数
- 特化函数中参数被常量替换
- 死代码消除后的简化 IR

### 11. 总结

**核心设计点**：
1. **嵌入 IPSCCP、共享 `SCCPSolver`**：避免重复实现跨过程常量信息；特化是「在已有 lattice 上克隆 + 钉死形参」的增量求解问题。
2. **收益驱动克隆**：`InstCostVisitor` 静态模拟传播 + 多阈值 + `FunctionGrowth`，在克隆前过滤低价值候选。
3. **调用点与签名**：`SpecSig` 去重、多 `CallSites` 合并；`updateCallSites` 处理递归与求解后才匹配的调用。
4. **递归与栈常量**：`promoteConstantStackValues` 把仅用于调用的栈上常量搬到 `internal global`，配合外层 `funcspec-max-iters` 迭代。
5. **间接调用与内联**：`getInliningBonus` 用 `getInlineCost` + 提高的阈值估计直调化后的内联红利。

**可继续深挖**：
- `InstCostVisitor` 对 PHI 环、`visitCallBase`/`visitGEP` 等与真实 SCCP 的一致性边界。
- 与 PGO profile 修正、`-funcspec-on-address` 等扩展语义交互。

**盈利性判定与排序（与 §9 文字一致，便于检索）**：

```cpp
给定原函数 F，规模 FuncSize；候选特化 S：
  InliningBonus = Σ InlineBonus(arg)
  CodeSizeSavings、LatencySavings 来自 InstCostVisitor
  SpecSize ≈ FuncSize - CodeSizeSavings   // 文档用语义化表述；实现以 getCostValue 为准

盈利（ForceSpecialization 时恒 true）：
  (1) InliningBonus > MinInliningBonus% × FuncSize   或
  (2) CodeSize/Latency 双达标 且 累计增长 ≤ MaxCodeSizeGrowth

排序得分：Score = InliningBonus + max(CodeSizeSavings, LatencySavings)
Top-K：N = min(NumCandidates × MaxClones, |AllSpecs|)
```

---

## FunctionSpecializer::run() 代码分析

### 函数签名与目的（676行）

```cpp
bool FunctionSpecializer::run()
```

**上下文**：由 `IPSCCP` 内的 `runIPSCCP` 调用，非独立注册的 Module Pass。
**目的**：在单次 `run()` 内完成候选收集、Top-K、克隆与 SCCP 求解、调用点更新与递归栈提升。

---

### 阶段一：发现特化候选（677-734行）

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
}
```

**关键点**：
- **isCandidateFunction()** (1040-1067)：过滤声明、空参数、NoDuplicate、已特化、size优化、不可执行、AlwaysInline函数
- **promoteConstantStackValues()** (585-613)：将递归归约中的栈常量提升为全局变量，支持多轮特化
- **findSpecializations()** (902-1038)：遍历调用点，收集常量参数，计算收益

**优化意图**：避免对小函数或即将被内联的函数做无意义的特化，控制代码膨胀

---

### 阶段二：选择最优特化（743-767行）

```cpp
// 2.1 定义分数比较函数（收益优先，平局时取索引较大）
auto CompareScore = [&AllSpecs](unsigned I, unsigned J) {
    if (AllSpecs[I].Score != AllSpecs[J].Score)
        return AllSpecs[I].Score > AllSpecs[J].Score;
    return I > J;
};

// 2.2 计算特化数量限制：min(候选数×MaxClones, 总候选数)
const unsigned NSpecs =
    std::min(NumCandidates * MaxClones, unsigned(AllSpecs.size()));

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

**算法复杂度**：O(N log K)，其中 N = `AllSpecs.size()`，K = `NSpecs`
**优化意图**：在预算约束下选择收益最大的特化，避免代码爆炸

---

### 阶段三：创建特化函数（781-825行）

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

### 阶段四：SCCP 求解与传播（827-862行）

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

### 阶段五：递归函数二次优化（864-866行）

```cpp
for (Function *F : OriginalFuncs)
    if (FunctionMetrics[F].isRecursive)
        promoteConstantStackValues(F);
```

**优化意图**：在特化完成后，再次提升常量栈值，为下一轮Pass做准备

---

### 收益计算与 CLI 参数

`IsProfitable` 判定、`Score` 累加与 `funcspec-*` 阈值含义见 **[§9 收益模型](#9-收益模型--编译时权衡)** 与 **[§11 总结](#11-总结)**；`findSpecializations` 内与 `InstCostVisitor` 的协作见 **[InstCostVisitor 常量传播估算分析](#instcostvisitor-常量传播估算分析)**。此处不再重复。

---

### 关键 API 与源码路径

| 功能 | API | 头文件 |
|------|-----|---------|
| 函数克隆 | `CloneFunction()` | `llvm/Transforms/Utils/Cloning.h` |
| SCCP 求解器 | `SCCPSolver` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 代码度量 | `CodeMetrics` | `llvm/Analysis/CodeMetrics.h` |
| 内联成本 | `getInlineCost()` | `llvm/Analysis/InlineCost.h` |
| 块频率 | `BlockFrequencyInfo` | `llvm/Analysis/BlockFrequencyInfo.h` |
| 目标变换信息 | `TargetTransformInfo` | `llvm/Analysis/TargetTransformInfo.h` |

---

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

## InstCostVisitor 常量传播估算（总览）

`InstCostVisitor` 继承 `InstVisitor`，在**不修改 IR** 的前提下沿 use-def 链模拟「若某形参为常量 C」带来的折叠与死区收益：`getCodeSizeSavingsForArg` → `getCodeSizeSavingsForUser` → 各类 `visit*`；`Switch`/`Branch` 走 `estimateSwitchInst` / `estimateBranchInst` 与 `estimateBasicBlocks`（死块 DFS，受 `funcspec-max-block-predecessors` 等约束）；`PHINode` 用 `PendingPHIs` + `discoverTransitivelyIncomingValues` 处理环与延迟信息。`getLatencySavingsForKnownConstants` 用 BFI 对 `TCK_Latency` 加权。

**完整调用栈、三入口函数、`findSpecializations` 中的调用顺序及 `visitPHINode`/`visitCallBase` 等逐函数分析见下文 [InstCostVisitor 常量传播估算分析](#instcostvisitor-常量传播估算分析)（其后为 `getInliningBonus` 详解等章节）。**

---

## SCCPSolver 交互机制

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

**首次求解（827行）：**
```cpp
Solver.solveWhileResolvedUndefsIn(Clones);
```
在克隆函数上运行，传播特化参数到整个函数。

**二次求解（862行）：**
```cpp
Solver.solveWhileResolvedUndefs();
```
在全局范围上运行，通知所有用户修改的调用点。

**重置 lattice 值（856行）：**
```cpp
Solver.resetLatticeValueFor(CS);
```
当特化函数返回值为常量时，重置调用点的lattice值，触发重新求解。

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
| SCCP 求解器 | `SCCPSolver` | `llvm/Transforms/Utils/SCCPSolver.h` |

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

1. **常量传播**：特化函数的参数常量化后，其返回值可能也变为常量。通过重置调用点的晶格值，触发求解器重新计算，将常量返回值传播到调用者。

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

## getInliningBonus 函数详解

### 函数签名与目的（1099-1148行）

```cpp
unsigned FunctionSpecializer::getInliningBonus(Argument *A, Constant *C)
```

**功能**: 计算将参数 `A` 替换为常量函数指针 `C` 后，通过间接调用直调化（indirect call promotion）带来的内联收益。

---

### 整体结构

```
getInliningBonus(A, C)
├── 检查 C 是否为函数指针
├── 遍历参数 A 的所有使用者
│   ├── 筛选 CallInst/InvokeInst
│   ├── 验证调用操作数是否为 A
│   ├── 验证函数类型匹配
│   └── 计算内联成本并累加收益
└── 返回总收益
```

---

### 逐段注释

**1. 参数验证（1100-1102）**

```cpp
Function *CalledFunction = dyn_cast<Function>(C->stripPointerCasts());
if (!CalledFunction)
  return 0;
```

目的：将常量 `C` 剥离指针转换后转换为函数指针，如果不是函数则返回 0。此函数只关注函数指针常量。

**2. 获取目标函数的 TTI（1104-1105）**

```cpp
auto &CalleeTTI = (GetTTI)(*CalledFunction);
```

目的：获取被调用函数的 TargetTransformInfo，用于后续内联成本计算。

**3. 遍历参数使用者并计算收益（1107-1145）**

```cpp
int InliningBonus = 0;
for (User *U : A->users()) {
  if (!isa<CallInst>(U) && !isa<InvokeInst>(U))
    continue;
  auto *CS = cast<CallBase>(U);
  if (CS->getCalledOperand() != A)
    continue;
  if (CS->getFunctionType() != CalledFunction->getFunctionType())
    continue;

  auto Params = getInlineParams();
  Params.DefaultThreshold += InlineConstants::IndirectCallThreshold;
  InlineCost IC =
      getInlineCost(*CS, CalledFunction, Params, CalleeTTI, GetAC, GetTLI);

  if (IC.isAlways())
    InliningBonus += Params.DefaultThreshold;
  else if (IC.isVariable() && IC.getCostDelta() > 0)
    InliningBonus += IC.getCostDelta();
}
```

目的：
- 遍历参数 `A` 的所有使用者，筛选出以 `A` 为被调用操作数的调用指令
- 验证函数类型匹配，确保可以直调化
- 调用 `getInlineCost` 计算将被调用函数内联到此调用点的成本
- 如果内联总是有益（`isAlways()`），增加默认阈值作为收益
- 如果内联成本为正（`isVariable() && getCostDelta() > 0`），增加成本差值作为收益

注释说明：此处的内联成本计算仅为估计，被调用函数后续可能因其他函数被内入而变大，导致实际不再适合内联。

**4. 返回结果（1147）**

```cpp
return InliningBonus > 0 ? static_cast<unsigned>(InliningBonus) : 0;
```

目的：返回正数的内联收益，负数返回 0。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `InlineCost` | `isAlways()`, `isVariable()`, `getCostDelta()` | 内联成本分析结果 |

---

### 优化意图

1. **间接调用直调化收益**：通过函数特化将间接调用转换为直接调用，使编译器能够更精确地分析内联机会
2. **内联决策优化**：为间接调用增加 `IndirectCallThreshold` 阈值奖励，鼓励直调化后的内联
3. **成本收益平衡**：累加所有相关调用点的内联收益，评估特化的整体价值

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 剥离指针转换 | `stripPointerCasts()` | `llvm/IR/Value.h` |
| 获取内联参数 | `getInlineParams()` | `llvm/Analysis/InlineCost.h` |
| 计算内联成本 | `getInlineCost()` | `llvm/Analysis/InlineCost.h` |
| 内联成本结果 | `InlineCost` | `llvm/Analysis/InlineCost.h` |

---

### InlineCost::getCostDelta() 详解

**函数签名**（InlineCost.h:176）：

```cpp
int getCostDelta() const { return Threshold - getCost(); }
```

**含义**：
- **Threshold**：内联阈值（例如 O3 模式下默认 250）
- **Cost**：估算的内联成本（函数体指令数）
- **CostDelta = Threshold - Cost**

**语义**：
- **CostDelta > 0**：内联成本低于阈值，应该内联
- **CostDelta 值**：表示"还有多少余量"，即阈值比成本高多少
- **CostDelta < 0**：内联成本高于阈值，不应该内联

**注意**：不要与 `InstructionCostDetail::getCostDelta()` 混淆，后者用于调试，计算的是 `CostAfter - CostBefore`。

---

### IR 用例说明场景

#### 场景：函数指针参数的间接调用直调化

```llvm
; 原始函数：通过函数指针调用
define i32 @process_data(i32 %val, i32 (i32)* %callback) {
entry:
  %result = call i32 %callback(i32 %val)
  ret i32 %result
}

; 被调用函数
define i32 @square(i32 %x) {
entry:
  %mul = mul i32 %x, %x
  ret i32 %mul
}

; 调用点1：传入常量函数指针 @square
define i32 @caller1() {
entry:
  %res1 = call i32 @process_data(i32 5, i32 (i32)* @square)
  ret i32 %res1
}

; 调用点2：传入另一个函数指针
define i32 @double(i32 %x) {
entry:
  %add = add i32 %x, %x
  ret i32 %add
}

define i32 @caller2() {
entry:
  %res2 = call i32 @process_data(i32 3, i32 (i32)* @double)
  ret i32 %res2
}
```

#### FunctionSpecialization 的优化过程

**步骤 1**：检测到 `@process_data` 的第二个参数在调用点1是常量 `@square`

**步骤 2**：调用 `getInliningBonus(@callback, @square)`
- 遍历 `@callback` 的使用者，找到 `call i32 %callback(i32 %val)`
- 计算将 `@square` 内联到此调用点的成本：
  - 假设 `@square` 有 2 条指令（mul + ret）
  - 内联成本 Cost = 2
  - 默认阈值 Threshold = 250（O3 模式）
  - CostDelta = 250 - 2 = **248**（很大的正值，说明内联非常有利）
- 返回 InliningBonus = **248**

**步骤 3**：创建特化版本

```llvm
; 特化后的函数：callback 参数被替换为直接调用 @square
define internal i32 @process_data.specialized.1(i32 %val) {
entry:
  %mul = mul i32 %val, %val  ; 直接内联的 @square 代码
  ret i32 %mul
}
```

**步骤 4**：更新调用

```llvm
define i32 @caller1() {
entry:
  ; 间接调用变为直接调用特化版本
  %res1 = call i32 @process_data.specialized.1(i32 5)
  ret i32 %res1
}
```

#### 为什么这个优化有价值？

1. **消除间接调用开销**：间接调用需要查表、预测失败率高
2. **启用内联**：特化后 `@square` 可以被内联，消除函数调用开销
3. **常量传播**：参数 `%val` 在特化版本中可能进一步优化

#### CostDelta 在收益计算中的作用

在 `findSpecializations:990` 中：

```cpp
if (Score > MinInliningBonus * FuncSize / 100)
  return true;
```

- `Score` 包含 `getInliningBonus` 返回的 248
- 如果 `MinInliningBonus = 300`（默认），`FuncSize = 500`
- 阈值 = 300 * 500 / 100 = 1500
- 248 < 1500，所以单靠 inlining bonus 不足以通过

但如果还有其他参数的代码大小节省：

```cpp
Score += std::max(CodeSizeSavings, LatencySavings);  // 1018行
```

总 Score 可能超过 1500，使特化变得有利。

---

### Inline Cost 计算的基本概念

#### InlineCost 的三种状态

| 状态 | 含义 | getCostDelta() | 使用场景 |
|------|------|----------------|----------|
| `isAlways()` | 总是应该内联 | 返回 `Threshold` | 函数有 `alwaysinline` 属性，或非常小的函数 |
| `isNever()` | 永远不应该内联 | N/A | 函数有 `noinline` 属性，或函数太大 |
| `isVariable()` | 需要成本分析 | 返回 `Threshold - Cost` | 大多数函数，需要计算内联成本 |

#### 内联成本估算因素

`getInlineCost()` 考虑以下因素计算成本：

1. **函数体大小**：基本块和指令数量
2. **循环复杂度**：嵌套循环增加成本
3. **调用指令**：函数内部的调用指令
4. **alloca 指令**：栈分配的内存大小
5. **属性影响**：
   - `inlinehint`：降低成本
   - `cold`：增加成本
   - `optsize`：使用更小的阈值
6. **调用点上下文**：
   - 参数传递成本
   - 调用频率（profile 数据）

#### 阈值调整

```cpp
auto Params = getInlineParams();
Params.DefaultThreshold += InlineConstants::IndirectCallThreshold;  // 1132行
```

**IndirectCallThreshold = 100**（InlineCost.h:50）

- 间接调用通常不内联，因为无法确定调用目标
- 但在 `getInliningBonus` 中，我们已经知道目标函数（通过特化）
- 因此增加阈值奖励，鼓励这种间接调用直调化后的内联

---

### 其他补充

此函数在 `findSpecializations` 的第 973 行被调用，用于评估每个常量参数特化的内联收益。该收益与代码大小节省和延迟节省一起构成特化的总得分（Score），用于判断特化是否值得进行。

---

## InstCostVisitor 常量传播估算分析

### 三个核心函数的调用栈

```text
findSpecializations()
  -> getCodeSizeSavingsForArg()         // 169-181行
     -> getCodeSizeSavingsForUser()       // 219-256行
        -> visit*()                    // 各类指令的常量折叠
        -> estimateSwitchInst()         // 258-281行
        -> estimateBranchInst()         // 283-297行
           -> estimateBasicBlocks()      // 114-147行
        -> visitPHINode()              // 343-396行
           -> discoverTransitivelyIncomingValues()  // 299-341行
        -> visitFreezeInst()            // 398-404行
        -> visitCallBase()              // 406-428行
        -> visitLoadInst()              // 430-436行
        -> visitGetElementPtrInst()      // 438-452行
        -> visitSelectInst()            // 454-467行
        -> visitCastInst()              // 469-472行
        -> visitCmpInst()               // 474-495行
        -> visitUnaryOperator()         // 497-501行
        -> visitBinaryOperator()        // 503-517行

  -> getCodeSizeSavingsFromPendingPHIs()  // 157-166行
     -> getCodeSizeSavingsForUser()       // 递归处理 PHIs

  -> getLatencySavingsForKnownConstants()  // 195-217行
```

---

### 1. getCodeSizeSavingsForArg 分析（169-181行）

#### 函数签名与目的

```cpp
Cost InstCostVisitor::getCodeSizeSavingsForArg(Argument *A, Constant *C)
```

**功能**：计算将参数 `A` 替换为常量 `C` 后，通过常量传播可消除的代码大小。

#### 实现逻辑

```cpp
Cost CodeSize;
for (auto *U : A->users())
  if (auto *UI = dyn_cast<Instruction>(U))
    if (isBlockExecutable(UI->getParent()))
      CodeSize += getCodeSizeSavingsForUser(UI, A, C);
```

**关键点**：
- 遍历参数 `A` 的所有使用者（指令）
- 只处理可执行基本块中的指令
- 对每个使用者调用 `getCodeSizeSavingsForUser` 递归传播常量

**优化意图**：
- 估算参数常量化后，整个函数体中可消除的代码量
- 考虑基本块可执行性，避免计算死代码

---

### 2. getCodeSizeSavingsFromPendingPHIs 分析（157-166行）

#### 函数签名与目的

```cpp
Cost InstCostVisitor::getCodeSizeSavingsFromPendingPHIs()
```

**功能**：处理首次访问时无法确定的 PHI 节点，在所有常量参数传播完成后重新计算。

#### 实现逻辑

```cpp
Cost CodeSize;
while (!PendingPHIs.empty()) {
  Instruction *Phi = PendingPHIs.pop_back_val();
  // The pending PHIs could have been proven dead by now.
  if (isBlockExecutable(Phi->getParent()))
    CodeSize += getCodeSizeSavingsForUser(Phi);
}
```

**关键点**：
- `PendingPHIs` 是在 `visitPHINode` 中首次访问时加入的（371行）
- 再次调用 `getCodeSizeSavingsForUser` 处理这些 PHI
- 检查 PHI 所在基本块是否可执行（可能已被 SCCP 求记为死）

**优化意图**：
- 处理循环依赖的 PHI 节点
- 在所有常量参数传播后，PHI 的 incoming 值可能都已确定

---

### 3. getLatencySavingsForKnownConstants 分析（195-217行）

#### 函数签名与目的

```cpp
Cost InstCostVisitor::getLatencySavingsForKnownConstants()
```

**功能**：计算所有已知常量指令的延迟节省（考虑执行频率权重）。

#### 实现逻辑

```cpp
auto &BFI = GetBFI(*F);
Cost TotalLatency = 0;

for (auto Pair : KnownConstants) {
  Instruction *I = dyn_cast<Instruction>(Pair.first);
  if (!I)
    continue;

  uint64_t Weight = BFI.getBlockFreq(I->getParent()).getFrequency() /
                    BFI.getEntryFreq().getFrequency();

  Cost Latency =
      Weight * TTI.getInstructionCost(I, TargetTransformInfo::TCK_Latency);

  TotalLatency += Latency;
}
```

**关键点**：
- 遍历 `KnownConstants` 映射（`Value* -> Constant*`）
- 只处理 `Instruction` 类型的常量
- **权重计算**：`BlockFreq / EntryFreq`，考虑基本块执行频率
- **延迟成本**：使用 `TCK_Latency` 类型查询指令延迟

**优化意图**：
- 代码大小节省 ≠ 延迟节省
- 热路径的常量化收益更高，冷路径收益可忽略
- 权重指导特化决策，避免优化冷代码

---

### 在 FunctionSpecialization 中的调用位置

#### 在 findSpecializations 中（970-975行）

```cpp
InstCostVisitor Visitor = getInstCostVisitorFor(F);
for (ArgInfo &A : S.Args) {
  CodeSize += Visitor.getCodeSizeSavingsForArg(A.Formal, A.Actual);
  Score += getInliningBonus(A.Formal, A.Actual);
}
CodeSize += Visitor.getCodeSizeSavingsFromPendingPHIs();
```

**调用顺序**：
1. 先为每个常量参数调用 `getCodeSizeSavingsForArg`
2. 然后调用 `getCodeSizeSavingsFromPendingPHIs` 处理 PHI
3. 最后在 `IsProfitable` lambda 中懒计算 `getLatencySavingsForKnownConstants`（1003-1004行）

**设计理由**：
- `getCodeSizeSavingsForArg` 会填充 `KnownConstants` 和 `PendingPHIs`
- 必须先处理所有参数，才能正确处理 PHI 依赖
- 延迟计算需要 `BFI`，开销较大，因此懒计算

---

### 辅助函数分析

#### findConstantFor（149-155行）

```cpp
Constant *InstCostVisitor::findConstantFor(Value *V) const {
  if (auto *C = dyn_cast<Constant>(V))
    return C;
  if (auto *C = Solver.getConstantOrNull(V))
    return C;
  return KnownConstants.lookup(V);
}
```

**查找优先级**：
1. 直接常量（`dyn_cast<Constant>`）
2. SCCP Solver 已知的常量
3. `KnownConstants` 映射中已传播的常量

#### estimateBasicBlocks（114-147行）

```cpp
Cost InstCostVisitor::estimateBasicBlocks(
                           SmallVectorImpl<BasicBlock *> &WorkList) {
  Cost CodeSize = 0;
  while (!WorkList.empty()) {
    BasicBlock *BB = WorkList.pop_back_val();

    if (!DeadBlocks.insert(BB).second)
      continue;

    for (Instruction &I : *B) {
      if (KnownConstants.contains(&I))
        continue;
      Cost C = TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);
      CodeSize += C;
    }

    for (BasicBlock *SuccBB : successors(B))
      if (isBlockExecutable(SuccBB) && canEliminateSuccessor(B, SuccBB))
        WorkList.push_back(SuccBB);
  }
  return CodeSize;
}
```

**死块递归消除**：
- 使用工作列表 DFS 遍历死块
- `DeadBlocks` 防列防止重复处理
- 累加死后继块（满足唯一前驱条件）
- 累加指令的代码大小成本

---

### 数据流与状态管理

#### 关键成员变量

| 成员 | 类型 | 作用 |
|---|---|---|
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 已知的值-常量映射 |
| `PendingPHIs` | `SmallVector<Instruction *>` | 待处理的 PHI 节点 |
| `VisitedPHIs` | `DenseSet<Instruction *>` | 已访问的 PHI 节点 |
| `DeadBlocks` | `DenseSet<BasicBlock *>` | 可消除的死块集合 |
| `LastVisited` | `DenseMap<Value *, Constant *>::iterator` | 上次访问的迭代器指针 |

#### 状态转换流程

```
初始状态
  ↓
getCodeSizeSavingsForArg(A, C)
  ├─ 填充 KnownConstants[A] = C
  ├─ 调用 getCodeSizeSavingsForUser
  │    ├─ 访问各指令类型
  │    ├─ 常量折叠 → 更新 KnownConstants
  │    ├─ 发现 PHI → 加入 PendingPHIs
  │    └─ 递归传播到使用者
  ↓
getCodeSizeSavingsFromPendingPHIs()
  └─ 处理 PendingPHIs（现在 incoming 值可能已确定）
     └─ 再次调用 getCodeSizeSavingsForUser
  ↓
getLatencySavingsForKnownConstants()
  └─ 遍历 KnownConstants
      └─ 计算 Weight × Latency
```

---

### 优化意图总结

#### 代码大小节省估算

1. **常量传播**：参数常量化后，依赖指令可被常量折叠
2. **死代码消除**：Switch/Branch 的死 case 可消除整个基本块
3. **PHI 处理**：正确处理循环依赖，避免重复计算

#### 延迟节省估算

1. **执行频率加权**：热路径的优化收益更高
2. **延迟成本查询**：使用 TTI 的微架构信息
3. **懒计算**：只在需要时计算（通过 `MinLatencySavings` 检查）

#### 编译时开销控制

1. **缓存机制**：`KnownConstants` 避免重复计算
2. **提前终止**：`DeadBlocks` 防列避免重复处理
3. **阈值限制**：`MaxIncomingPhiValues`、`MaxDiscoveryIterations` 防止复杂度爆炸

---

### 在 findSpecializations 中的收益计算流程（970-1020行）

```cpp
// 阶段1：计算代码大小节省
InstCostVisitor Visitor = getInstCostVisitorFor(F);
for (ArgInfo &A : S.Args) {
  CodeSize += Visitor.getCodeSizeSavingsForArg(A.Formal, A.Actual);
  Score += getInliningBonus(A.Formal, A.Actual);
}
CodeSize += Visitor.getCodeSizeSavingsFromPendingPHIs();

unsigned CodeSizeSavings = getCostValue(CodeSize);
unsigned SpecSize = FuncSize - CodeSizeSavings;

// 阶段2：盈利性检查（懒计算延迟节省）
auto IsProfitable = [&]() -> bool {
  if (ForceSpecialization)
    return true;

  // 检查1：内联奖励 > 阈值
  if (Score > MinInliningBonus * FuncSize / 100)
    return true;

  // 检查2：代码大小节省 > 阈值
  if (CodeSizeSavings < MinCodeSizeSavings * FuncSize / 100)
    return false;

  // 检查3：延迟节省 > 阈值（懒计算）
  unsigned LatencySavings =
      getCostValue(Visitor.getLatencySavingsForKnownConstants());
  if (LatencySavings < MinLatencySavings * FuncSize / 100)
    return false;

  // 检查4：代码增长限制
  if ((FunctionGrowth[F] + SpecSize) / FuncSize > MaxCodeSizeGrowth)
    return false;

  Score += std::max(CodeSizeSavings, LatencySavings);
  return true;
};
```

**设计亮点**：
- **延迟计算**：延迟节省只在通过前两个检查后才计算
- **短路径优化**：如果内联奖励足够高，直接返回
- **综合评分**：最终 Score = InliningBonus + max(CodeSize, Latency)

---

### 总结

InstCostVisitor 通过模拟常量传播过程，精确估算函数特化的收益：

1. **getCodeSizeSavingsForArg**：递归传播常量，估算可消除的代码大小
2. **getCodeSizeSavingsFromPendingPHIs**：处理循环依赖的 PHI 节点
3. **getLatencySavingsForKnownConstants**：加权计算延迟节省，考虑执行频率

这三个函数在 `findSpecializations` 中协同工作，为每个特化候选计算准确的收益评估，确保特化决策的收益大于成本。

---

## getCodeSizeSavingsForUser 函数分析

### 函数签名与目的（219-256行）

```cpp
Cost InstCostVisitor::getCodeSizeSavingsForUser(Instruction *User, Value *Use,
                                                Constant *C)
```

**功能**: 递归估算将 `Use` 替换为常量 `C` 后，从指令 `User` 开始传播可节省的代码大小。

---

### 整体结构

```
getCodeSizeSavingsForUser(User, Use, C)
├── 检查是否已处理过 User（避免重复计算）
├── 缓存 LastVisited 迭代器（供 visit* 函数使用）
├── 根据 User 类型处理
│   ├── SwitchInst → estimateSwitchInst（死代码消除）
│   ├── BranchInst → estimateBranchInst（死代码消除）
│   └── 其他指令 → visit(User)（常量折叠）
├── 标记 User 为已知常量
├── 累加 User 本身的代码大小
└── 递归传播到 User 的使用者
```

---

### 逐段注释

**1. 避免重复计算（221-223行）**

```cpp
if (KnownConstants.contains(User))
  return 0;
```

目的作用：防止循环依赖导致的无限递归，避免重复计算同一指令的收益。

**2. 缓存 LastVisited 迭代器（225-227行）**

```cpp
LastVisited = Use ? KnownConstants.insert({Use, C}).first
                  : KnownConstants.end();
```

目的作用：将 `Use → C` 的映射插入 `KnownConstants`，并缓存迭代器。`visit*` 函数通过 `LastVisited` 快速访问最近插入的常量映射。

**3. 根据指令类型处理（229-238行）**

```cpp
Cost CodeSize = 0;
if (auto *I = dyn_cast<SwitchInst>(User)) {
  CodeSize = estimateSwitchInst(*I);
} else if (auto *I = dyn_cast<BranchInst>(User)) {
  CodeSize = estimateBranchInst(*I);
} else {
  C = visit(*User);
  if (!C)
    return 0;
}
```

目的作用：
- **SwitchInst/BranchInst**：调用专门的死代码消除估算函数
- **其他指令**：调用 `visit` 进行常量折叠，如果无法折叠则返回 0

**4. 标记 User 为已知常量（243行）**

```cpp
KnownConstants.insert({User, C});
```

目的作用：即使 Switch/Branch 不常量折叠，也要标记已处理，防止重复估算。

**5. 累加 User 本身的代码大小（245行）**

```cpp
CodeSize += TTI.getInstructionCost(User, TargetTransformInfo::TCK_CodeSize);
```

目的作用：User 指令本身可以被消除（常量折叠或死代码消除），累加其代码大小。

**6. 递归传播到使用者（250-253行）**

```cpp
for (auto *U : User->users())
  if (auto *UI = dyn_cast<Instruction>(U))
    if (UI != User && isBlockExecutable(UI->getParent()))
      CodeSize += getCodeSizeSavingsForUser(UI, User, C);
```

目的作用：
- 遍历 User 的所有使用者
- 递归估算每个使用者的收益
- 只处理可执行基本块中的指令

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 已知的常量映射，避免重复计算 |
| `LastVisited` | `DenseMap<Value *, Constant *>::iterator` | 缓存最近插入的迭代器，供 `visit*` 使用 |
| `DeadBlocks` | `DenseSet<BasicBlock *>` | 可消除的死块集合（estimateBasicBlocks 使用） |

---

### 优化意图

1. **避免重复计算**：`KnownConstants` 防止同一指令被多次估算
2. **递归传播**：通过使用者链传播常量，估算整个数据流的收益
3. **死代码消除**：Switch/Branch 的死 case 可消除整个基本块
4. **常量折叠**：其他指令可被常量折叠，节省代码大小

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 查询指令代码大小 | `TTI.getInstructionCost` | `llvm/Analysis/TargetTransformInfo.h` |
| 判断基本块可执行性 | `isBlockExecutable` | SCCPSolver 成员函数 |
| 常量折叠 | `visit*` | InstCostVisitor 成员函数 |
| 死代码消除估算 | `estimateSwitchInst` / `estimateBranchInst` | InstCostVisitor 成员函数 |

---

### 调用关系

```
getCodeSizeSavingsForArg(A, C)
  └── getCodeSizeSavingsForUser(UI, A, C)
       ├── estimateSwitchInst / estimateBranchInst
       │    └── estimateBasicBlocks
       └── visit(User)
            └── visitBinaryOperator / visitCmpInst / ...
       └── getCodeSizeSavingsForUser（递归）
```

---

### 其他补充

**参数说明**：
- `User`：当前处理的指令
- `Use`：被替换为常量的值（可能是参数、中间指令等）
- `C`：替换的常量值

**返回值**：可节省的代码大小（`Cost` 类型）

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

## estimateSwitchInst 函数分析

### 函数签名与目的（258-281行）

```cpp
Cost InstCostVisitor::estimateSwitchInst(SwitchInst &I)
```

**功能**: 估算当 switch 指令的条件值被替换为已知常量后，可消除的死代码块的代码大小成本。

---

### 整体结构

```
estimateSwitchInst(SwitchInst &I)
├── 验证 LastVisited 迭代器有效性
├── 检查 switch 条件是否为最近访问的值
├── 提取常量值并验证类型
├── 找到常量对应的目标基本块
├── 收集可消除的死代码块
│   └── 遍历所有 case 分支
│       └── 排除目标块，收集满足条件的块
└── 估算并返回死代码块的总成本
```

---

### 逐段注释

**1. 前置条件检查（259-266行）**

```cpp
assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

if (I.getCondition() != LastVisited->first)
  return 0;

auto *C = dyn_cast<ConstantInt>(LastVisited->second);
if (!C)
  return 0;
```

目的验证 `LastVisited` 迭代器有效，确保正在分析的 switch 条件值是最近被标记为常量的值。如果条件不匹配或值不是整数常量，直接返回 0（无收益）。

**2. 确定存活分支（268行）**

```cpp
BasicBlock *Succ = I.findCaseValue(C)->getCaseSuccessor();
```

根据常量值 `C` 找到 switch 中对应的 case，获取该 case 的目标基本块 `Succ`。这个块在常量传播后会被保留，其他 case 分支将成为死代码。

**3. 收集死代码块（272-278行）**

```cpp
SmallVector<BasicBlock *> WorkList;
for (const auto &Case : I.cases()) {
  BasicBlock *BB = Case.getCaseSuccessor();
  if (BB != Succ && isBlockExecutable(BB) &&
      canEliminateSuccessor(I.getParent(), BB))
    WorkList.push_back(BB);
}
```

遍历所有 case 分支，收集满足以下条件的块：
- 不是目标块 `Succ`（即会被消除的块）
- 当前可执行（`isBlockExecutable`）
- 可被安全消除（`canEliminateSuccessor`，要求块只有唯一前驱）

**4. 估算成本（280行）**

```cpp
return estimateBasicBlocks(WorkList);
```

调用 `estimateBasicBlocks` 递归计算所有死代码块及其后继块的代码大小成本。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LastVisited` | `DenseMap<Value*, Constant*>::iterator` | 指向最近被标记为常量的值对（值→常量映射） |
| `KnownConstants` | `DenseMap<Value*, Constant*>` | 存储所有已知常量映射 |
| `WorkList` | ``SmallVector<BasicBlock*>` | 待估算的死代码块列表 |

---

### 优化意图

1. **常量传播收益评估**: 当函数特化将某个参数替换为常量后，switch 指令的条件值可能变为常量，从而消除大部分 case 分支
2. **死代码消除**: 通过提前估算可消除的代码大小，帮助判断特化是否值得进行
3. **保守估计**: 只消除可安全删除的块（满足 `canEliminateSuccessor` 条件），避免过度乐观的收益估计

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 获取 switch 条件 | `I.getCondition()` | `llvm/IR/Instructions.h` |
| 查找 case 值 | `I.findCaseValue(C)` | `llvm/IR/Instructions.h` |
| 获取 case 目标块 | `Case.getCaseSuccessor()` | `llvm/IR/Instructions.h` |
| 遍历所有 case | `I.cases()` | `llvm/IR/Instructions.h` |
| 检查块可执行性 | `isBlockExecutable(BB)` | SCCPSolver 成员函数 |
| 检查可消除性 | `canEliminateSuccessor(Pred, Succ)` | 99-106行 |
| 估算块成本 | `estimateBasicBlocks(WorkList)` | 114-147行 |

---

### 其他补充

此函数是 `InstCostVisitor` 类的一部分，用于在函数特化过程中评估将参数替换为常量后的收益。它与 `estimateBranchInst`（283-297行）功能类似，都是评估控制流简化带来的代码大小收益。

**调用链**:
```
getCodeSizeSavingsForArg (169-181行)
  └─> getCodeSizeSavingsForUser (219-256行)
       └─> estimateSwitchInst (258-281行)
            └─> estimateBasicBlocks (114-147行)
```

---

---

## estimateBranchInst 函数分析

### 函数签名与目的（283-297行）

```cpp
Cost InstCostVisitor::estimateBranchInst(BranchInst &I)
```

**功能**: 估算当分支指令的条件变为常量后，可消除的死代码（不可达基本块）带来的代码大小节省。

---

### 整体结构

```
estimateBranchInst(BranchInst &I)
├── 验证 LastVisited 有效性
├── 检查分支条件是否是当前分析的常量值
├── 根据条件值确定唯一的可达后继块
├── 将不可达的后继块加入工作列表（如果可消除）
└── 估算这些死基本块的代码大小
```

---

### 逐段注释

**1. 前置条件检查 (284-287)**

```cpp
assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

if (I.getCondition() != LastVisited->first)
  return 0;
```

目的：确保当前分析的分支指令的条件值正是我们正在追踪的常量值。如果不是，说明这个分支指令不受当前常量传播影响，返回 0 收益。

**2. 确定不可达后继块 (289-294)**

```cpp
BasicBlock *Succ = I.getSuccessor(LastVisited->second->isOneValue());
SmallVector<BasicBlock *> WorkList;
if (isBlockExecutable(Succ) && canEliminateSuccessor(I.getParent(), Succ))
  WorkList.push_back(Succ);
```

目的：
- `isOneValue()` 返回 true 表示条件为真，取第 0 个后继（true 分支）；否则取第 1 个后继（false 分支）
- `Succ` 是**不可达**的后继块（因为条件已确定为常量，只会走另一条路）
- 检查该块是否可执行且可消除（只有唯一前驱），如果是则加入工作列表

**3. 估算死代码收益 (296)**

```cpp
return estimateBasicBlocks(WorkList);
```

目的：计算工作列表中所有死基本块的代码大小总和。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LastVisited` | `DenseMap<Value *, Constant *>::iterator` | 指向最近被标记为常量的 Value 及其常量值 |
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 已知常量映射表 |
| `WorkList` | `SmallVector<BasicBlock *>` | 待估算的死基本块列表 |

---

### 优化意图

1. **常量传播后的死代码消除**：当分支条件变为常量后，一个分支路径变得不可达，该路径上的所有代码都可以删除
2. **收益评估**：在函数特化之前，先估算通过常量传播能消除多少代码，判断特化是否值得
3. **保守估计**：只消除那些"只有一个前驱"的块，避免误删可能从其他路径到达的块

---

### BranchInst vs SwitchInst 处理差异

**对比 estimateSwitchInst (258-281):**

```cpp
// SwitchInst 处理逻辑
BasicBlock *Succ = I.findCaseValue(C)->getCaseSuccessor();
SmallVector<BasicBlock *> WorkList;
for (const auto &Case : I.cases()) {
  BasicBlock *BB = Case.getCaseSuccessor();
  if (BB != Succ && isBlockExecutable(BB) &&
      canEliminateSuccessor(I.getParent(), BB))
    WorkList.push_back(BB);
}
```

**差异原因：**

| 维度 | BranchInst | SwitchInst |
|---|---|---|
| **分支数量** | 2 个（true/false） | N 个（多个 case + default） |
| **不可达块数量** | 1 个（另一个分支） | N-1 个（所有不匹配的 case） |
| **处理方式** | 直接计算唯一的不可达后继 | 遍历所有 case，排除匹配的那个 |
| **语义差异** | 二元选择，条件确定后只有一条路 | 多路选择，条件确定后只有一条路 |

**举例说明：**

```llvm
; BranchInst 场景
br i1 %cond, label %if_true, label %if_false
; 如果 %cond = true，则 %if_false 不可达（1 个死块）

; SwitchInst 场景
switch i32 %val, label %default [
  i32 1, label %case1
  i32 2, label %case2
  i32 3, label %case3
]
; 如果 %val = 2，则 %default、%case1、%case3 都不可达（3 个死块）
```

**为什么实现不同：**

1. **BranchInst**: 条件确定后，只有 1 个后继不可达，直接计算 `getSuccessor(!cond)` 即可
2. **SwitchInst**: 条件确定后，可能有多个 case 不可达，需要遍历所有 case，排除匹配的那个，将其他都加入工作列表

这种差异源于两种指令的**语义不同**：Branch 是二元分支，Switch 是多路分支。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 获取分支条件 | `BranchInst::getCondition()` | `llvm/IR/Instructions.h` |
| 获取后继块 | `BranchInst::getSuccessor(unsigned)` | `llvm/IR/Instructions.h` |
| 判断常量是否为真 | `Constant::isOneValue()` | `llvm/IR/Constants.h` |
| 查找匹配的 case | `SwitchInst::findCaseValue()` | `llvm/IR/Instructions.h` |
| 遍历所有 case | `SwitchInst::cases()` | `llvm/IR/Instructions.h` |

---


---

## estimateBasicBlocks 函数分析

### 函数签名与目的（114-147行）

```cpp
Cost InstCostVisitor::estimateBasicBlocks(
                           SmallVectorImpl<BasicBlock *> &WorkList)
```

**功能**: 递归估算所有死基本块的代码大小，通过工作列表进行 DFS 遍历。

---

### 整体结构

```
estimateBasicBlocks(WorkList)
├── 初始化代码大小累加器
├── 循环处理工作列表
│   ├── 弹出死块
│   ├── 检查是否已处理（避免重复）
│   ├── 累加块中所有指令的代码大小
│   └── 递归添加死后继块
└── 返回总代码大小
```

---

### 逐段注释

**1. 初始化 (116-117)**

```cpp
Cost CodeSize = 0;
```

目的：初始化代码大小累加器。

**2. 循环处理死块 (118-146)**

```cpp
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
```

目的：
- 弹出死块：从工作列表中取出待处理的死块
- 去重检查：通过 `DeadBlocks` 集合避免重复处理
（代码大小累加：遍历块中所有指令，累加 `TCK_CodeSize` 类型的指令成本
- 死块传播：将所有满足条件的后继块加入工作列表

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `WorkList` | `SmallVector<BasicBlock *>` | 待处理的死基本块列表（DFS工作列表） |
| `DeadBlocks` | `DenseSet<BasicBlock *>` | 已处理的死块集合（避免重复） |
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 已知常量映射表 |
| `TTI` | `TargetTransformInfo` | 目标变换信息（用于指令成本查询） |

---

### 优化意图

1. **递归死块传播**：通过工作列表实现 DFS 遍历，确保所有可达的死块都被处理
2. **避免重复处理**：使用 `DeadBlocks` 集合标记已处理的块
3. **保守估计**：只累加非已知常量的指令成本，避免高估收益
4. **唯一前驱检查**：`canEliminateSuccessor()` 确保块只能从死路径到达

---

### 算法复杂度

- **时间复杂度**：O(N)，其中 N 是可消除的块总数
- **空间复杂度**：O(N)，`DeadBlocks` 集合的大小
- **正确性保证**：通过 `DeadBlocks` 集合避免无限循环

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 指令成本查询 | `TTI.getInstructionCost()` | `llvm/Analysis/TargetTransformInfo.h` |
| 基本块可执行性 | `isBlockExecutable()` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 唍驱遍历 | `successors()` | `llvm/IR/CFG.h` |
| 唯一前驱检查 | `canEliminateSuccessor()` | 99-106行 |

---

### 其他补充

**调用上下文**：
- 被 `estimateSwitchInst()` (283-281行）和 `estimateBranchInst()` (283-297行）调用
- 用于估算分支/switch 指令常量化后不可达块的代码大小

**与 estimateSwitchInst/estimateBranchInst 的协作**：
- `estimateSwitchInst` 将所有不匹配的 case 对应的块加入工作列表
- `estimateBranchInst` 将不可达的后继块加入工作列表
- 本函数负责递归计算这些块及其后继的代码大小

---

## InstCostVisitor::getLatencySavingsForKnownConstants 函数分析

> 本节由文档维护者根据 `llvm/lib/Transforms/IPO/FunctionSpecialization.cpp`（约 195–217 行）整理，对应头文件 `llvm/include/llvm/Transforms/IPO/FunctionSpecialization.h`（约 187 行）。

### 函数签名与目的

```cpp
Cost InstCostVisitor::getLatencySavingsForKnownConstants();
```

**功能**：在**已经为某次特化候选**做完常量传播式估计之后，遍历 `KnownConstants` 里所有**被当成可替换为常量的 `Instruction`**，用 **块频率（BFI）加权** 的 **TTI 延迟模型**，累加得到「若做这次特化，在运行时可省下的延迟代价」的估计值，供收益模型与 `funcspec-min-latency-savings` 阈值比较。

- `Cost` 即 `InstructionCost`（见头文件 `KnownConstants` / `Cost` 类型别名）。

### 整体结构

```text
getLatencySavingsForKnownConstants()
├── 取当前函数 F 的 BlockFrequencyInfo（GetBFI）
├── TotalLatency = 0
├── 对 KnownConstants 中每个 (Value*, Constant*)：
│   ├── 若 key 不是 Instruction → 跳过
│   ├── Weight = BB 相对入口的频率比（整数除法）
│   ├── Latency = Weight * TTI.getInstructionCost(I, TCK_Latency)
│   └── TotalLatency += Latency
└── 返回 TotalLatency
```

**返回值**：`InstructionCost` 类型的加权延迟估计总和；调用方在 `findSpecializations` 中用 `getCostValue(...)` 转为 `unsigned` 再与阈值比较。

### 逐段注释

**1. 调用时机与前置条件（源码注释 183–194 行 + `findSpecializations` 配合）**

注释要求：必须在

- 对每个常量实参跑完 `getCodeSizeSavingsForArg`，且
- 跑完 `getCodeSizeSavingsFromPendingPHIs`

之后再调用，这样 `KnownConstants` 才包含本次估计过程中所有被访问且已判定可常数化的指令。

在 `findSpecializations` 中，**只有**在 codesize 节省已满足 `MinCodeSizeSavings`（或 `ForceSpecialization` / inlining bonus 已足够）时，才会**惰性**调用本函数，避免每个候选都去算 BFI 加权延迟（源码约 998–1004 行）。

**2. 获取 BFI 并初始化累加器**

- `GetBFI` 为构造 `InstCostVisitor` 时注入的回调，用于拿到被特化函数 `F` 的 `BlockFrequencyInfo`（无 profile 时通常为合成/均匀频率，仍是合法输入）。

**3. 遍历 `KnownConstants` 并只处理 `Instruction`**

- `KnownConstants` 为 `DenseMap<Value *, Constant *>`。
- 映射里除指令外，还可能存在**非 `Instruction` 的 key**（例如 `getCodeSizeSavingsForUser` 里对操作数 `Use` 的 `insert`）。这些**不参与延迟累计**，因为延迟是按「某条 IR 指令的执行代价」建模的。

**4. 块频率权重**

```cpp
uint64_t Weight = BFI.getBlockFreq(I->getParent()).getFrequency() /
                  BFI.getEntryFreq().getFrequency();
```

- **含义**：该指令所在基本块相对函数入口的**相对执行频度**（整数除法，冷块可能为 0）。
- **意图**：同样一条可被常量传播消掉的指令，在**热路径**上比在**几乎不走**的路径上更值钱；与 codesize「删掉多少字节」互补，体现**运行时**收益。

**5. 单条指令的加权延迟与累加**

- `TTI.getInstructionCost(I, TargetTransformInfo::TCK_Latency)`：目标相关的**延迟**估计（非 codesize）。
- 乘以 `Weight` 后累加到 `TotalLatency`；`LLVM_DEBUG` 可逐条打印贡献。

### 关键数据结构

| 结构 | 字段/类型 | 含义 |
|------|-----------|------|
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 估计过程中某 `Value` 在特化后可视为某 `Constant`；本函数只对其中的 `Instruction` 键计延迟 |
| `BlockFrequencyInfo` | `getBlockFreq` / `getEntryFreq` | 基本块与入口的频度，用于热路径加权 |
| `TargetTransformInfo` | `getInstructionCost(I, TCK_Latency)` | 单条指令在目标上的延迟代价模型 |
| `Cost` | `InstructionCost` | 与 TTI 一致的代价类型 |

### 优化意图

1. **与 codesize 解耦**：特化可能主要减少动态执行而非静态体积；用 **TCK_Latency** 把「能消掉的计算」量化成运行时向的 bonus。
2. **与 profile 对齐**：用 **BFI** 加权，使热路径上可被常量传播干掉的指令在分数里权重更大。
3. **惰性计算**：仅在 codesize 已通过一定门槛后再算 BFI+延迟，降低编译时开销。
4. **与 `MinLatencySavings` 联动**：若加权延迟 savings 占原函数 size 的比例仍低于 `funcspec-min-latency-savings`，则拒绝该特化，避免「体积略好但几乎不加速」的 clone。

**模型近似说明**：凡在 `KnownConstants` 中且为 `Instruction` 的项都会计入（包括为去重而插入的 branch/switch 等），是对「特化后可简化控制流/数据流」的整体近似，而非严格的逐条动态计数。

### 关键 API / 源码路径

| 功能 | API / 符号 | 位置 |
|------|------------|------|
| 本函数实现 | `InstCostVisitor::getLatencySavingsForKnownConstants` | `FunctionSpecialization.cpp` 约 195–217 行 |
| 类与 `KnownConstants` | `InstCostVisitor`, `ConstMap` | `FunctionSpecialization.h` 约 103–107、153–187 行 |
| 消费方与惰性调用 | `getCostValue(Visitor.getLatencySavingsForKnownConstants())` | `FunctionSpecialization.cpp` 约 1002–1013 行 |
| 块频率 | `BlockFrequencyInfo::getBlockFreq` / `getEntryFreq` | `llvm/Analysis/BlockFrequencyInfo.h` |
| 延迟代价 | `TargetTransformInfo::getInstructionCost(..., TCK_Latency)` | `llvm/Analysis/TargetTransformInfo.h` |

**与 Score 的关系**：`IsProfitable` 末尾 `Score += std::max(CodeSizeSavings, LatencySavings)`（约 1018 行），最终排序分数将 codesize 与 latency **取较大者**再叠加 inlining bonus，避免双重满额叠加。

### 其他补充

- **无 BFI / 无 profile**：通常仍有默认频率；`Weight` 可能多为 0 或较小整数比，延迟 bonus 会偏保守或离散。
- **可深入方向**：`KnownConstants` 在 `visit*` / `getCodeSizeSavingsForUser` 中的增长方式；`Weight` 整数除法对冷块的影响；`InstructionCost` 与 `getCostValue` 的舍入行为。

---

## `InstCostVisitor::visitPHINode` 函数分析

> 位置：`llvm/lib/Transforms/IPO/FunctionSpecialization.cpp` 约 343–396 行

### 函数签名与目的

```cpp
Constant *InstCostVisitor::visitPHINode(PHINode &I);
```

**功能**：在**不修改 IR** 的前提下，判断 PHI 节点 `I` 在"把特化实参替换为常量"之后，**能否被静态折叠为单一常量**，并返回该常量供上层继续传播与代价估计；若不能则返回 `nullptr`。

---

### 整体结构

```
visitPHINode(PHI I)
├── 入边数 > MaxIncomingPhiValues → nullptr（编译时上限保护）
├── 记录是否首次访问（VisitedPHIs）
├── 遍历每条 incoming(V, PredBlock)：
│   ├── 跳过：自环 / 来自不可执行前驱的边
│   ├── findConstantFor(V) 成功：
│   │   ├── 首次命中 → Const = C
│   │   └── 与已有 Const 不同 → nullptr（各路径常量不一致）
│   ├── 非常量 + 首次见该 PHI → PendingPHIs 延后 + nullptr
│   ├── 非常量 + PHI 类型    → HaveSeenIncomingPHI = true，继续
│   └── 非常量 + 其它类型    → nullptr（无法推理）
├── 未获得任何常量 → nullptr
├── 无"未知 PHI 链"入边 → 直接返回 Const（快速路径）
└── 有"未知 PHI 链"入边 → discoverTransitivelyIncomingValues 验证
    ├── 传递闭包一致 → 返回 Const
    └── 不一致 / 超限 → nullptr
```

---

### 逐段注释

**1. 入边数量上限（344–345）**

```cpp
if (I.getNumIncomingValues() > MaxIncomingPhiValues)
    return nullptr;
```

- `MaxIncomingPhiValues` 默认 **8**（`-funcspec-max-incoming-phi-values`）。
- PHI 入边越多，分析代价越高，此处以常量时间结束，防止编译时膨胀。

**2. 访问状态初始化（347–349）**

```cpp
bool Inserted = VisitedPHIs.insert(&I).second;
Constant *Const = nullptr;
bool HaveSeenIncomingPHI = false;
```

- `Inserted == true`：本轮**第一次**遇到该 PHI。
- `Inserted == false`：本次特化估计中已在某传播路径上处理过它。
- 两者在"入边是非常量"时走**不同的逻辑分支**。

**3. 遍历每条 incoming（351–383）**

- **跳过自环和不可达前驱（354–357）**：`V == &I` 或 `incoming block` 不可执行则忽略。`isBlockExecutable` 综合了 `SCCPSolver` 结果与本次估计暂定的 `DeadBlocks`，等价于 SCCP 的 CFG 剪枝。

- **尝试解析为常量（359–366）**：`findConstantFor(V)` 依次查询：① `V` 本身是 `Constant`；② `SCCPSolver::getConstantOrNull(V)`；③ `KnownConstants.lookup(V)`。**一致性检查**：若两条入边解析出不同常量，立即 `nullptr`。

- **非常量 + 首次见该 PHI（368–373）**：`PendingPHIs.push_back(&I)` 并返回 `nullptr`。时机问题：可能是其他实参还没传播到，等 `getCodeSizeSavingsFromPendingPHIs` 在**所有实参处理完后**重试。

- **非常量 + PHI 类型（375–379）**：在第二次 visit 时，入边若是另一个 PHI（非已知常量），说明可能是**传递 PHI 网络**，设 `HaveSeenIncomingPHI = true`，最后由 `discoverTransitivelyIncomingValues` 做完整验证。

- **不可推理的入边（381–382）**：`load`、`add`、`call` 等在当前模型下无法静态折叠，直接 `nullptr`。

**4. 快速路径（385–389）**

```cpp
if (!Const)
    return nullptr;
if (!HaveSeenIncomingPHI)
    return Const;
```

所有有效 incoming 统一为同一常量，且没有待验证的 PHI 链 → **直接返回 `Const`**。

**5. 传递 PHI 验证（391–395）**

```cpp
DenseSet<PHINode *> TransitivePHIs;
if (!discoverTransitivelyIncomingValues(Const, &I, TransitivePHIs))
    return nullptr;
return Const;
```

从 `I` 出发 BFS 展开整个"只由 PHI 互相连接"的区域（TIV），验证其中每个常量都等于 `Const`，且无非 PHI 的未知值。受 `MaxDiscoveryIterations`（默认 100）和 `MaxIncomingPhiValues` 双重限制。

---

### 关键数据结构

| 结构 | 类型 | 含义 |
|------|------|------|
| `VisitedPHIs` | `DenseSet<Instruction *>` | 记录已访问的 PHI，区分"初次 vs 重试" |
| `PendingPHIs` | `SmallVector<Instruction *>` | 第一次见到"有未解析入边"的 PHI，等待重试 |
| `KnownConstants` | `DenseMap<Value *, Constant *>` | 本次估计积累的"已知可视为常量"映射 |
| `TransitivePHIs` | `DenseSet<PHINode *>` | 传递 PHI 验证的临时已访问集合 |

---

### 优化意图

1. **分两阶段传播（Pending 机制）**：第一轮实参传播完成前，某些 PHI 入边信息不全；`PendingPHIs` 确保**所有实参都传播完**后再做最终判断。
2. **处理 PHI 环（传递 PHI）**：Loop-back PHI 的 incoming 互相引用，`discoverTransitivelyIncomingValues` 专门处理整个 PHI 网络统一来自同一常量的场景。
3. **保守正确性优先**：任何一条有效入边不是同一常量就 `nullptr`，绝不过度估计，避免创建不值得的 clone。
4. **编译时保护**：入边数、迭代数双重上限确保最坏情况有界。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 查询 Value 是否是常量 | `findConstantFor(V)` | 同文件约 149–154 行 |
| 判断基本块可执行性 | `isBlockExecutable(BB)` | 头文件约 178–180 行 |
| 传递 PHI 验证 | `discoverTransitivelyIncomingValues` | 同文件约 299–341 行 |
| Pending 重试驱动 | `getCodeSizeSavingsFromPendingPHIs` | 同文件约 156–164 行 |
| 最大入边数 | `MaxIncomingPhiValues` | 文件顶 cl::opt 约 51–54 行 |
| 最大传递迭代 | `MaxDiscoveryIterations` | 文件顶 cl::opt 约 44–48 行 |

---

### 完整示例

#### C 代码与 IR

```c
int foo(int mode, int x) {
    int v = (mode == 1) ? x : 20;
    return v * 2;
}

int caller() {
    return foo(1, 42);  // mode=1, x=42 均为常量
}
```

对应关键 IR：

```llvm
define i32 @foo(i32 %mode, i32 %x) {
entry:
  %cmp = icmp eq i32 %mode, 1
  br i1 %cmp, label %then, label %else
then:
  br label %merge
else:
  br label %merge
merge:
  %v = phi i32 [ %x, %then ], [ 20, %else ]
  %r = mul i32 %v, 2
  ret i32 %r
}
```

#### 特化触发

`findSpecializations` 检测到调用点 `foo(1, 42)` 中 `%mode → i32 1`、`%x → i32 42`，建立 `InstCostVisitor` 后依次调用：
1. `getCodeSizeSavingsForArg(%mode, i32 1)`
2. `getCodeSizeSavingsForArg(%x, i32 42)`
3. `getCodeSizeSavingsFromPendingPHIs()`

#### 第一步：传播 `%mode = i32 1`

- `icmp eq %mode, 1` 折叠为 `true`
- `br i1 true` 走 `estimateBranchInst`，`%else` 块标记进 `DeadBlocks`

#### 第二步：传播 `%x = i32 42`，首次 visit `%v`

- `KnownConstants[%x] = i32 42`
- `%x` 的使用者 `%v` 触发 `visitPHINode(%v)`：

```
visitPHINode(%v)  ← Inserted = true（首次）

  Idx=0: incoming = %x，来自 %then
    findConstantFor(%x) = i32 42  ← 命中 KnownConstants
    Const = i32 42

  Idx=1: incoming = i32 20，来自 %else
    findConstantFor(20) = i32 20
    C(20) ≠ Const(42)
    → 返回 nullptr ✗（两路常量不同）
```

此时 `%else` 已在 `DeadBlocks`，但**第一次 visit** 不区分死活，直接做一致性检查就失败了。

#### 第三步：重试（Pending 或再次 visit）

`%else` 已是死块，再次触发 `visitPHINode(%v)`：

```
visitPHINode(%v)  ← Inserted = false（非首次）

  Idx=0: incoming = %x，来自 %then
    isBlockExecutable(%then) → true
    findConstantFor(%x) = i32 42
    Const = i32 42

  Idx=1: incoming = i32 20，来自 %else
    isBlockExecutable(%else) → false  ← 死块！跳过

  HaveSeenIncomingPHI = false
  → 快速路径，返回 i32 42  ✓
```

#### 第四步：继续传播到 `%r`

- `KnownConstants[%v] = i32 42`
- `visit(%r)` → `mul i32 42, 2` 折叠为 `i32 84`
- `KnownConstants[%r] = i32 84`

#### 最终效果

特化后的函数等价于：

```llvm
define i32 @foo.specialized.1() {
  ret i32 84   ; 全部被常量折叠
}
```

**关键总结**：`visitPHINode` 第一次因"两路常量不同"失败，等 `%mode=1` 将 `%else` 标记为死块后，第二次 visit 忽略了死前驱，只剩一路 `i32 42`，成功折叠，进而带动 `%r` 也被折叠，最终整个函数体变为一条 `ret`。

---

## `InstCostVisitor::discoverTransitivelyIncomingValues` 函数分析

> 位置：`llvm/lib/Transforms/IPO/FunctionSpecialization.cpp` 约 299–341 行

### 函数签名与目的

```cpp
bool InstCostVisitor::discoverTransitivelyIncomingValues(
    Constant *Const, PHINode *Root, DenseSet<PHINode *> &TransitivePHIs);
```

**功能**：以 `Root` 为起点，BFS 展开整个"只由 PHI 节点互相连接"的传递区域（TIV），验证其中出现的**所有常量**均与 `Const` 相同，且不存在无法推理的非 PHI 值。若整个区域一致则返回 `true`，否则返回 `false`。

---

### 整体结构

```
discoverTransitivelyIncomingValues(Const, Root, TransitivePHIs)
├── 初始化 WorkList = {Root}，Iter = 0
├── BFS 主循环（WorkList 不空）：
│   ├── 取出 PN
│   ├── 迭代数/入边数超限 → return false（编译时保护）
│   ├── PN 已访问（TransitivePHIs 去重）→ continue
│   └── 遍历 PN 的每条 incoming(V, pred)：
│       ├── 跳过：自环 / 来自不可执行前驱的边
│       ├── findConstantFor(V) 成功：
│       │   ├── C == Const → 合法，continue
│       │   └── C != Const → return false（常量不一致）
│       ├── V 是 PHI → WorkList.push_back(V)，continue
│       └── 其它 Value → return false（无法推理）
└── 所有节点验证通过 → return true
```

---

### 逐段注释

**1. 初始化（302–304）**

```cpp
SmallVector<PHINode *, 64> WorkList;
WorkList.push_back(Root);
unsigned Iter = 0;
```

- `WorkList` 预分配 64 个槽，适应中等规模的 PHI 网络，避免频繁扩容。
- `Iter` 是**全局迭代计数器**，记录从 WorkList 中弹出的总次数，跨多个 PHI 节点累计。

**2. BFS 主循环与编译时安全保护（306–311）**

```cpp
while (!WorkList.empty()) {
    PHINode *PN = WorkList.pop_back_val();
    if (++Iter > MaxDiscoveryIterations ||
        PN->getNumIncomingValues() > MaxIncomingPhiValues)
      return false;
```

两重保护：
- **`MaxDiscoveryIterations`**（默认 100，`-funcspec-max-discovery-iterations`）：限制**弹出节点的总次数**，防止巨大 PHI 网络的编译时膨胀。是全局计数，不是单节点计数。
- **`MaxIncomingPhiValues`**（默认 8）：每个 PHI 的入边数上限，防止宽 PHI 节点的组合爆炸。

**3. 去重（313–314）**

```cpp
if (!TransitivePHIs.insert(PN).second)
    continue;
```

`TransitivePHIs` 兼任两个角色：**已访问集合**（防止 BFS 重复处理同一 PHI）和**输出结果**（记录 TIV 区域中所有 PHI 节点）。

**4. 遍历 incoming 并分类处理（316–338）**

- **跳过自环和不可达前驱（320–322）**：与 `visitPHINode` 完全对称，死前驱提供的值不参与运行时，直接忽略。

- **常量一致性检查（324–329）**：遇到常量 incoming 时，立即检查是否等于预期的 `Const`。任何不一致 → `return false`，**快速失败**。利用 LLVM 常量 uniquing 特性，**指针相等即值相等**，无需 `APInt` 级别的数值比较。

- **传递 PHI 扩展（331–334）**：若 incoming 是另一个 PHI，加入 WorkList 继续探索，这正是"传递性"的体现。去重保证同一 PHI 不会被重复 push。

- **无法推理的 incoming（337）**：`add`、`load`、`call`、函数参数等在不运行 SCCP 的情况下无法静态确定，保守返回 `false`。

**5. 全部通过返回 true（340）**

WorkList 清空意味着从 `Root` 出发的整个传递区域全部通过验证。

---

### 关键数据结构

| 结构 | 类型 | 含义 |
|------|------|------|
| `WorkList` | `SmallVector<PHINode *, 64>` | BFS 待处理队列 |
| `TransitivePHIs` | `DenseSet<PHINode *>` | 已访问集合 + 输出：TIV 中所有 PHI 节点 |
| `Const` | `Constant *` | 预期的单一常量（由 `visitPHINode` 传入） |
| `Iter` | `unsigned` | 全局弹出次数，超过 `MaxDiscoveryIterations` 时放弃 |

---

### 与 `visitPHINode` 的协作关系

```
visitPHINode(PHI I)
  ├── 快速路径：所有 incoming 均已是同一常量，无 PHI 链
  │   └── 直接返回 Const（不调用本函数）
  └── 慢速路径：有 incoming 是 PHI 类型（HaveSeenIncomingPHI = true）
      └── discoverTransitivelyIncomingValues(Const, &I, TransitivePHIs)
          ├── BFS 展开整个 PHI 网络
          ├── 验证所有常量均为 Const
          └── 返回 true/false → visitPHINode 据此决定返回 Const 或 nullptr
```

`visitPHINode` 做**单层快速检查**，本函数做**多层传递验证**，二者分工明确：前者尽早走快速路径，只在必要时才下沉到后者。

---

### 优化意图

1. **处理 Loop-carried PHI 环**：最典型场景是 `%phi1 ↔ %phi2` 互相引用，两者均来自同一常量但彼此无法直接解析。BFS 能遍历整个 PHI 环，验证整体一致性。
2. **快速失败优于完整验证**：常量不一致、或遇到无法推理的 Value，立即返回 `false`，不做无效探索。
3. **指针比较等价值比较**：利用 LLVM 常量 uniquing，`C != Const` 即判断"不是同一常量值"，性能更好。
4. **双重编译时保护**：`MaxDiscoveryIterations` 限制总体规模，`MaxIncomingPhiValues` 限制单 PHI 宽度，两者配合防止分析时间无界。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 本函数实现 | `discoverTransitivelyIncomingValues` | 同文件 299–341 行 |
| 唯一调用方 | `visitPHINode` | 同文件 391–393 行 |
| 常量查询 | `findConstantFor(V)` | 同文件 149–154 行 |
| 块可执行性 | `isBlockExecutable(BB)` | 头文件约 178–180 行 |
| 最大迭代数 | `MaxDiscoveryIterations` | 文件顶 cl::opt 约 44–48 行 |
| 最大入边数 | `MaxIncomingPhiValues` | 文件顶 cl::opt 约 51–54 行 |

---

### 完整示例

#### 场景一：Loop-carried PHI 环，均来自同一常量（成功）

```llvm
loop.header:
  %phi1 = phi i32 [ 7,     %entry ],
                   [ %phi2, %loop.latch ]
  %phi2 = phi i32 [ %phi1, %loop.body  ],
                   [ 7,     %entry     ]
```

`visitPHINode(%phi1)` 发现 `%phi2` 是 PHI incoming，设 `HaveSeenIncomingPHI = true`，调用：

```
discoverTransitivelyIncomingValues(i32 7, %phi1, {})

── 迭代1：PN = %phi1 ──────────────────────────────────
  TransitivePHIs.insert(%phi1) → 成功
  Idx=0: incoming = i32 7  → C=7 == Const=7 ✓
  Idx=1: incoming = %phi2  → WorkList.push_back(%phi2)

── 迭代2：PN = %phi2 ──────────────────────────────────
  TransitivePHIs.insert(%phi2) → 成功
  Idx=0: incoming = %phi1  → WorkList.push_back(%phi1)
  Idx=1: incoming = i32 7  → C=7 == Const=7 ✓

── 迭代3：PN = %phi1 ──────────────────────────────────
  TransitivePHIs.insert(%phi1) → 已存在，continue（去重）

WorkList 清空 → return true ✓
```

**结果**：`visitPHINode` 返回 `i32 7`，`%phi1` 写入 `KnownConstants`。

#### 场景二：PHI 环中有不一致常量（快速失败）

```llvm
%phi_a = phi i32 [ 7,       %entry ],
                  [ %phi_b,  %latch ]
%phi_b = phi i32 [ %phi_a,  %body  ],
                  [ 42,      %entry ]  ; ← 不同的常量 42
```

```
discoverTransitivelyIncomingValues(i32 7, %phi_a, {})

── 迭代1：PN = %phi_a ──────────────────────────────────
  Idx=0: incoming = 7    → ✓
  Idx=1: incoming = %phi_b → WorkList.push_back(%phi_b)

── 迭代2：PN = %phi_b ──────────────────────────────────
  Idx=0: incoming = %phi_a → WorkList.push_back(%phi_a)
  Idx=1: incoming = i32 42
    C(42) != Const(7)
    → return false ✗（快速失败）
```

**结果**：`visitPHINode` 返回 `nullptr`，不纳入节省统计。

---

### 其他补充

- **`pop_back_val` 使遍历实际是 DFS 顺序**：`SmallVector` 末尾弹出是 LIFO，严格来说是 DFS，但在正确性上与 BFS 等价——去重保证每个节点只处理一次。
- **`TransitivePHIs` 的潜在扩展价值**：调用方目前只用了返回的 `bool`，但 `TransitivePHIs` 已记录整个 TIV 区域的 PHI 集合，若未来需要批量写入 `KnownConstants` 可直接复用。
- **与 `VisitedPHIs` 的区别**：`VisitedPHIs` 跨多次 `visitPHINode` 调用保持（整个特化估计过程全局），`TransitivePHIs` 是每次调用 `discoverTransitivelyIncomingValues` 时的局部临时集合，两者生命周期不同。

---

## `visitCallBase` 函数分析

### 函数签名与目的（406-428 行）

```cpp
Constant *InstCostVisitor::visitCallBase(CallBase &I);
```

**功能**：作为 `InstCostVisitor`（`InstVisitor` 的子类）的 `visit*` 回调，尝试对一条 Call/Invoke 指令做**编译时常量折叠**。若所有参数都能解析为常量，则调用 `ConstantFoldCall` 求出结果常量；否则返回 `nullptr`（表示"无法折叠，收益不可知"）。

---

### 整体结构

```text
visitCallBase(CallBase &I)
├── 断言 LastVisited 有效（调用路径完整性保证）
├── 获取被调函数，检查是否可常量折叠（canConstantFoldCallTo）
├── 遍历所有参数操作数（除最后一个 callee 操作数）
│   ├── 跳过 MetadataAsValue（无法表示为 Constant）
│   └── 通过 findConstantFor 解析为 Constant，任一失败则提前返回 nullptr
└── 调用 ConstantFoldCall，返回折叠结果（或 nullptr）
```

---

### 逐段注释

**1. 调用路径断言（407 行）**

```cpp
assert(LastVisited != KnownConstants.end() && "Invalid iterator!");
```

`LastVisited` 是 `InstCostVisitor` 的成员，在 `getCodeSizeSavingsForUser` 调用 `visit(*User)` 之前被设置：

```cpp
// getCodeSizeSavingsForUser (第 226 行)
LastVisited = Use ? KnownConstants.insert({Use, C}).first
                  : KnownConstants.end();
```

当 `Use != nullptr`（正常传播路径），`LastVisited` 指向 `KnownConstants` 中刚插入的"已知为常量的操作数"条目。断言确保本函数只通过合法传播路径被调用（不是 PHI 重试路径）。

值得注意的是：`visitCallBase` 的函数体并不像 `visitFreezeInst`/`visitSelectInst` 那样直接读取 `LastVisited->second`，但该断言仍然重要——它隐式保证了触发本次 visit 的那个操作数已经存入 `KnownConstants`，使得后续 `findConstantFor` 能查到它。

---

**2. 函数可折叠性检查（409-411 行）**

```cpp
Function *F = I.getCalledFunction();
if (!F || !canConstantFoldCallTo(&I, F))
  return nullptr;
```

- `getCalledFunction()` 对间接调用（函数指针）返回 `nullptr`，此时无法折叠。
- `canConstantFoldCallTo`（`llvm/Analysis/ConstantFolding.h`）检查函数是否在编译器的常量折叠白名单中，例如：`sin`/`cos`/`sqrt` 等数学函数、`memcpy`/`strlen` 等标准库函数、以及特定 LLVM intrinsics。

---

**3. 构建操作数列表（413-424 行）**

```cpp
SmallVector<Constant *, 8> Operands;
Operands.reserve(I.getNumOperands());

for (unsigned Idx = 0, E = I.getNumOperands() - 1; Idx != E; ++Idx) {
  Value *V = I.getOperand(Idx);
  if (isa<MetadataAsValue>(V))
    return nullptr;
  Constant *C = findConstantFor(V);
  if (!C)
    return nullptr;
  Operands.push_back(C);
}
```

关键细节：

- **循环上界 `I.getNumOperands() - 1`**：`CallBase` 的最后一个 operand 是 callee 函数指针，不属于参数列表，需跳过。真正的参数是 operand `[0, N-2]`。
- **`MetadataAsValue` 检测**：`llvm.dbg.value` 等 debug intrinsic 的参数类型是 `metadata`，无法表示为 `Constant *`，直接放弃折叠。
- **`findConstantFor(V)`** 三路查询（第 149-154 行）：

  ```cpp
  Constant *InstCostVisitor::findConstantFor(Value *V) const {
    if (auto *C = dyn_cast<Constant>(V))       // 1. 本身就是字面常量
      return C;
    if (auto *C = Solver.getConstantOrNull(V)) // 2. IPSCCP solver 已证明为常量
      return C;
    return KnownConstants.lookup(V);           // 3. 本轮传播中已推导为常量
  }
  ```

  **全量要求**：所有参数必须可解析为常量，任何一个失败即返回 `nullptr`——这是调用常量折叠的必要条件。

---

**4. 常量折叠调用（426-427 行）**

```cpp
auto Ops = ArrayRef(Operands.begin(), Operands.end());
return ConstantFoldCall(&I, F, Ops);
```

`ConstantFoldCall`（`llvm/Analysis/ConstantFolding.h`，实现在 `lib/Analysis/ConstantFolding.cpp`）在已知全部参数为常量的情况下对调用求值，返回折叠后的 `Constant *`，若内部折叠仍失败则返回 `nullptr`。

---

### 关键数据结构

| 结构/字段 | 类型 | 含义 |
|---|---|---|
| `KnownConstants` | `ConstMap`（即 `DenseMap<Value*, Constant*>`） | 本轮传播中推导出的"Value → 常量"映射 |
| `LastVisited` | `ConstMap::iterator` | 指向触发当前 visit 的那个已知常量操作数的 map 条目 |
| `Solver` | `const SCCPSolver &` | IPSCCP 求解器，提供跨 Pass 的常量传播结果 |

---

### 优化意图

1. **折叠 call 指令本身**：若一个 call 的返回值在特化后是常量（例如 `strlen("hello")` → `5`），其所有 user 都可进一步被折叠，从而累积更大的收益估算，驱动 FunctionSpecialization 判定该特化值得做。

2. **三路常量来源统一**：`findConstantFor` 统一了字面常量、IPSCCP 已知常量、本轮推导常量，避免在 cost model 阶段重复计算。

3. **保守正确性**：任何参数非常量则立即返回 `nullptr`，不做任何 IR 修改——这里是**纯估算**，不是实际变换，因此绝对安全。

---

### 调用上下文（完整调用链）

```text
findSpecializations()
  -> getCodeSizeSavingsForArg(Argument *A, Constant *C)    // 以常量参数为起点
     -> getCodeSizeSavingsForUser(UI, A, C)                // 遍历参数的每个 User
        -> [LastVisited = KnownConstants.insert({A,C})]    // 缓存已知常量
        -> visit(*User)                                    // 分发 InstVisitor
           -> visitCallBase(CallBase &I)                   // ← 目标函数
              -> findConstantFor(operand)                  // 查三路常量源
              -> ConstantFoldCall(&I, F, Ops)              // 常量折叠求值
        -> [递归] getCodeSizeSavingsForUser(UI, User, C)   // 传播到 call 的 User
```

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 检查函数是否可常量折叠 | `canConstantFoldCallTo(CallBase*, Function*)` | `llvm/Analysis/ConstantFolding.h` |
| 执行常量折叠 | `ConstantFoldCall(CallBase*, Function*, ArrayRef<Constant*>)` | `lib/Analysis/ConstantFolding.cpp` |
| 查找值对应的常量 | `findConstantFor(Value*)` | 本文件第 149 行 |
| InstVisitor 分发机制 | `visit(Instruction&)` | `llvm/IR/InstVisitor.h` |
| IPSCCP 常量查询 | `Solver.getConstantOrNull(Value*)` | `lib/Transforms/Utils/SCCPSolver.cpp` |

---

### 易错点与约束

- **`MetadataAsValue` 必须先拦截**：`dyn_cast<Constant>` 不会把 `MetadataAsValue` 转型为 `Constant`，但若不检查，传给 `ConstantFoldCall` 会出现未定义行为。
- **`getNumOperands() - 1` 的边界**：跳过 callee operand 是正确的，但若误写 `getNumArgOperands()`（C API 遗留）或用错边界，会包含 callee 导致崩溃。
- **此处不修改 IR**：整个 `InstCostVisitor` 框架是**纯估算**——它只写 `KnownConstants`，不调用 `replaceAllUsesWith` 或 `eraseFromParent`。实际的常量传播由 IPSCCP 在 specialization 后完成。

---

## `visitGetElementPtrInst` 函数分析

### 函数签名与目的（438-452 行）

```cpp
Constant *InstCostVisitor::visitGetElementPtrInst(GetElementPtrInst &I);
```

**功能**：尝试对一条 `getelementptr` 指令做编译时常量折叠。若基址指针和所有索引操作数都能解析为常量，则通过 `ConstantFoldInstOperands` 求出折叠后的指针常量；否则返回 `nullptr`。

---

### 整体结构

```text
visitGetElementPtrInst(GetElementPtrInst &I)
├── 构建常量操作数列表（含所有操作数：基址 + 所有索引）
│   └── 任一操作数无法解析为常量则提前返回 nullptr
└── 调用 ConstantFoldInstOperands 进行折叠，返回结果
```

---

### 逐段注释

**1. 构建全量操作数列表（439-448 行）**

```cpp
SmallVector<Constant *, 8> Operands;
Operands.reserve(I.getNumOperands());

for (unsigned Idx = 0, E = I.getNumOperands(); Idx != E; ++Idx) {
  Value *V = I.getOperand(Idx);
  Constant *C = findConstantFor(V);
  if (!C)
    return nullptr;
  Operands.push_back(C);
}
```

GEP 指令的操作数布局为：

```
operand[0]        : 基址指针（base pointer）
operand[1..N-1]   : 各维度索引（indices）
```

与 `visitCallBase` 不同，这里循环上界是 `I.getNumOperands()`（**不减 1**），原因在于 GEP 的所有操作数——包括基址指针——都需要是常量才能折叠，没有像 `CallBase` 那样需要跳过的 callee 操作数。`reserve` 的大小也恰好等于实际收集的操作数数量，没有多余预留。

---

**2. 调用通用指令常量折叠器（450-451 行）**

```cpp
auto Ops = ArrayRef(Operands.begin(), Operands.end());
return ConstantFoldInstOperands(&I, Ops, DL);
```

使用 `ConstantFoldInstOperands`（`llvm/Analysis/ConstantFolding.h`）而非 `ConstantFoldCall`。两者的适用场景不同：

| 函数 | 适用场景 | 额外参数 |
|---|---|---|
| `ConstantFoldInstOperands` | 任意 IR 指令（含 GEP、cast、算术等） | `DataLayout`，可选 `TLI` |
| `ConstantFoldCall` | 专用于 `CallBase` 类型的调用指令 | `Function*`，操作数不含 callee |

对于 GEP，`ConstantFoldInstOperands` 内部会构造 `ConstantExpr::getGetElementPtr`（或在结果可进一步化简时返回更简单的常量），并利用 `DataLayout` 完成指针算术。

---

### 与 `visitCallBase` 的关键差异对比

| 特征 | `visitCallBase` | `visitGetElementPtrInst` |
|---|---|---|
| `assert(LastVisited != end())` | ✓ 有 | ✗ **无** |
| 操作数遍历范围 | `[0, N-2]`（跳过最后的 callee） | `[0, N-1]`（全量，含基址） |
| `MetadataAsValue` 检测 | ✓ 有（call 可携带 debug metadata） | ✗ 不需要（GEP 操作数只有指针和整数） |
| 常量折叠入口 | `ConstantFoldCall` | `ConstantFoldInstOperands` |
| `LastVisited` 使用 | 仅断言，不直接读取 | 完全不使用 |

**缺少断言的深层原因**：大多数 `visit*` 方法（`visitFreezeInst`、`visitLoadInst`、`visitSelectInst` 等）使用 `LastVisited->second` 来直接取出触发 visit 的那个已知常量。`visitGetElementPtrInst` 和 `visitCallBase` 则不同——它们要求**所有操作数**同时为常量，统一通过 `findConstantFor` 查询，不依赖 `LastVisited` 作为单一入口点。`visitCallBase` 保留了断言作为调用路径完整性的防御性检查，`visitGetElementPtrInst` 则省略了它，行为上两者等价（均不会在 `LastVisited == end()` 的路径中被调用），但这构成了代码风格上的不一致。

---

### 优化意图

1. **指针常量化打通传播链**：GEP 结果（一个具体的地址）常量化后，下游的 `load` 指令即可通过 `ConstantFoldLoadFromConstPtr` 进一步折叠为被加载的值，从而在 cost model 中累积更多可消除指令的收益。

2. **全量要求保证正确性**：GEP 的语义依赖基址和所有索引的组合，缺少任何一个都无法求出确定地址，因此"全量或放弃"是唯一安全策略。

3. **`DataLayout` 驱动精确折叠**：指针宽度、结构体对齐、数组元素大小均由目标相关的 `DataLayout` 提供，`ConstantFoldInstOperands` 据此计算出正确的字节偏移。

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 通用指令常量折叠 | `ConstantFoldInstOperands(Instruction*, ArrayRef<Constant*>, DataLayout&)` | `llvm/Analysis/ConstantFolding.h` |
| GEP 常量表达式构造 | `ConstantExpr::getGetElementPtr(...)` | `llvm/IR/Constants.h` |
| 查找值对应的常量 | `findConstantFor(Value*)` | 本文件第 149 行 |
| 目标布局信息 | `DataLayout` | `llvm/IR/DataLayout.h` |

---

### 其他补充

- **`inbounds` 语义的保留**：`ConstantFoldInstOperands` 内部处理 GEP 时会尊重原指令的 `inbounds` 标志。若基址加索引计算出的地址越界（仅在折叠非 `inbounds` GEP 时可能），折叠结果仍合法，只是不带 `inbounds` 语义的 poison 保证。
- **与 `visitLoadInst` 的协作**：`visitGetElementPtrInst` 折叠出的常量指针会被记录进 `KnownConstants`，当后续 `visitLoadInst` 被调用时，它通过 `LastVisited->second`（此时已是折叠后的常量指针）调用 `ConstantFoldLoadFromConstPtr`，两者形成完整的"指针折叠 → 内存读取折叠"传播链。

---

## `visitSelectInst` 函数分析

### 函数签名与目的（454-467 行）

```cpp
Constant *InstCostVisitor::visitSelectInst(SelectInst &I);
```

**功能**：在 cost model 估算阶段尝试对 `select` 指令进行常量折叠。根据 `LastVisited`（触发本次 visit 的已知常量操作数）是**条件**还是**某个数据分支**，走不同的折叠路径。

`select` 指令形如：

```llvm
%result = select i1 %cond, <type> %true_val, <type> %false_val
```

---

### 整体结构

```text
visitSelectInst(SelectInst &I)
├── 断言 LastVisited 有效
├── 路径 A：LastVisited 是条件操作数
│   ├── 条件为 false(0) → 选 FalseValue → findConstantFor 查其常量
│   └── 条件为 true(非0) → 选 TrueValue → findConstantFor 查其常量
└── 路径 B：LastVisited 是数据操作数（true/false 分支之一）
    ├── 尝试查条件是否也为常量（findConstantFor）
    ├── 条件为 true(1) 且 LastVisited 是 TrueValue → 返回 LastVisited->second
    ├── 条件为 false(0) 且 LastVisited 是 FalseValue → 返回 LastVisited->second
    └── 否则返回 nullptr
```

---

### 逐段注释

**1. 路径 A：已知常量是条件操作数（457-460 行）**

```cpp
if (I.getCondition() == LastVisited->first) {
    Value *V = LastVisited->second->isNullValue() ? I.getFalseValue()
                                                  : I.getTrueValue();
    return findConstantFor(V);
}
```

条件已知为常量时，`select` 退化为确定性选择：
- `isNullValue()` 对 `i1 false`（即 0）为 true → 结果是 FalseValue
- 否则（`i1 true`，即 1）→ 结果是 TrueValue

确定分支后，再通过 `findConstantFor` 查被选中的那个分支值是否也为常量，从而决定整个 `select` 能否折叠。注意此处不限于 `i1`——`isNullValue()` 对任意全零常量成立，具有通用性。

**2. 路径 B：已知常量是数据操作数（462-465 行）**

```cpp
if (Constant *Condition = findConstantFor(I.getCondition()))
    if ((I.getTrueValue() == LastVisited->first && Condition->isOneValue()) ||
        (I.getFalseValue() == LastVisited->first && Condition->isNullValue()))
        return LastVisited->second;
```

当触发 visit 的是某个数据分支而非条件时，反向验证逻辑：
1. 先查条件是否恰好也为常量
2. 若条件为 `1`（`isOneValue`）且 `LastVisited` 恰是 TrueValue → `select` 必然选 TrueValue，结果为 `LastVisited->second`
3. 若条件为 `0`（`isNullValue`）且 `LastVisited` 恰是 FalseValue → `select` 必然选 FalseValue，结果为 `LastVisited->second`

此路径之所以需要额外验证"LastVisited 是被选中的那一侧"，是因为即便条件已知，也必须确认已知常量的数据分支正好是被选中的那一侧，才能将整个 `select` 的结果确定为该常量。

---

### IR 示例

**场景 1（路径 A）：条件是特化参数，数据分支之一也是常量**

```llvm
; 特化参数 %flag 已知为 i1 true
define i32 @foo(i1 %flag) {
  %r = select i1 %flag, i32 42, i32 99
  ret i32 %r
}
```

- `LastVisited->first = %flag`，`LastVisited->second = i1 true`
- 走路径 A：`isNullValue()` 为 false → 选 TrueValue（`i32 42`）
- `findConstantFor(i32 42)` 直接返回字面常量 `i32 42`
- `select` 折叠成功，记入 `KnownConstants`，下游 `ret` 可进一步折叠

**场景 2（路径 B）：数据分支是特化参数，条件恰好也能查到常量**

```llvm
; 特化参数 %val 已知为 i32 7
; IPSCCP 已推导 %cond = icmp eq i32 7, 0 → i1 false
define i32 @bar(i32 %val) {
  %cond = icmp eq i32 %val, 0
  %r = select i1 %cond, i32 999, i32 %val   ; FalseValue = %val
  ret i32 %r
}
```

- `LastVisited->first = %val`（FalseValue），`LastVisited->second = i32 7`
- 走路径 B：`findConstantFor(%cond)` 得到 `i1 false`（`isNullValue()` 为 true）
- `%val == getFalseValue()` 且条件为 false → 返回 `i32 7`

---

### 与其他 visitor 的横向对比

与 `visitBinaryOperator`、`visitCmpInst` 同属"**一个操作数已知（LastVisited），尝试查另一个**"的模式，但 `select` 是三元操作，折叠分叉更多：

| visitor | 操作数数量 | 折叠策略 |
|---|---|---|
| `visitUnaryOperator` | 1 | 直接用 `LastVisited->second` 折叠 |
| `visitBinaryOperator` | 2 | 已知一个，查另一个，两个都有则折叠 |
| `visitCmpInst` | 2 | 已知一个，查另一个；另一个未知时还可走 lattice 比较 |
| `visitSelectInst` | 3（条件+两分支） | 已知条件 → 确定性选择；已知数据分支 → 反向验证条件 |

---

### 其他补充

- **`isNullValue` vs `isOneValue`**：两者都是 `Constant` 的成员方法。`isNullValue()` 检查全零（对 `i1` 即 false），`isOneValue()` 检查全一（对 `i1` 即 true）。对于 `i1` 类型这两者互补，但对多位整数含义不同，路径 B 中使用它们来区分条件为 true/false 两种情况是精确的。
- **路径 B 不覆盖"条件未知且另一数据分支也未知"的情形**：若条件无法确定，无论 `LastVisited` 是哪个数据分支，都无法推断结果，直接返回 `nullptr`，保守且正确。

---

## `visitCmpInst` 函数分析

### 函数签名与目的（474-495 行）

```cpp
Constant *InstCostVisitor::visitCmpInst(CmpInst &I);
```

**功能**：对 `icmp` / `fcmp` 指令尝试常量折叠。与其他 visitor 的"单操作数已知"模式一致，但额外引入了一条**格值（lattice）比较路径**：即使另一操作数不是精确常量，只要 IPSCCP Solver 记录了该值的范围信息（`ConstantRange`），也可能推断出比较结果为确定的 `true` / `false`。

---

### 整体结构

```text
visitCmpInst(CmpInst &I)
├── 断言 LastVisited 有效
├── 确定已知常量位于 LHS 还是 RHS（ConstOnRHS）
├── 路径 A（快速路径）：另一操作数也是精确常量
│   ├── 若 ConstOnRHS，交换 LHS/RHS 确保顺序正确
│   └── ConstantFoldCompareInstOperands → 直接返回折叠结果
└── 路径 B（Lattice 路径）：另一操作数非精确常量
    ├── 将已知常量包装为 ValueLatticeElement（get(Const)）
    ├── 查询另一操作数的格值（Solver.getLatticeValueFor）
    ├── 按原始指令的 LHS/RHS 顺序排列两个格值
    └── getCompare(pred, type, other_lattice, DL) → 返回结果或 nullptr
```

---

### 逐段注释

**1. 确定操作数方向（477-480 行）**

```cpp
Constant *Const = LastVisited->second;
bool ConstOnRHS = I.getOperand(1) == LastVisited->first;
Value *V = ConstOnRHS ? I.getOperand(0) : I.getOperand(1);
Constant *Other = findConstantFor(V);
```

`LastVisited->first` 是触发本次 visit 的操作数（已知为常量），它可能在 LHS（operand 0）也可能在 RHS（operand 1）。`ConstOnRHS` 记录这一位置，`V` 则指向**另一个**操作数。后续两条路径都需要维护 LHS/RHS 的正确顺序，这个标志贯穿全函数。

---

**2. 路径 A：双精确常量，直接折叠（482-486 行）**

```cpp
if (Other) {
    if (ConstOnRHS)
        std::swap(Const, Other);
    return ConstantFoldCompareInstOperands(I.getPredicate(), Const, Other, DL);
}
```

`ConstantFoldCompareInstOperands(pred, LHS, RHS, DL)` 要求参数按指令原始顺序传入（LHS 在前）。当 `ConstOnRHS = true` 时：
- 初始状态：`Const` = 已知常量（原 RHS），`Other` = 另一侧（原 LHS）
- `swap` 后：`Const` → 原 LHS，`Other` → 原 RHS
- 调用变为 `ConstantFoldCompareInstOperands(pred, 原LHS, 原RHS, DL)` ✓

若不 swap 而直接传入，参数顺序颠倒，对有方向性的谓词（`slt`、`sgt`、`ule` 等）会产生错误的折叠结果。

---

**3. 路径 B：Lattice 比较（488-494 行）**

```cpp
const ValueLatticeElement &ConstLV = ValueLatticeElement::get(Const);
const ValueLatticeElement &OtherLV = Solver.getLatticeValueFor(V);
auto &V1State = ConstOnRHS ? OtherLV : ConstLV;
auto &V2State = ConstOnRHS ? ConstLV : OtherLV;
return V1State.getCompare(I.getPredicate(), I.getType(), V2State, DL);
```

当另一操作数 `V` 不是精确常量时，IPSCCP Solver 仍可能掌握它的**值域范围**（`ConstantRange`）。此路径将两侧操作数都升格为 `ValueLatticeElement`：

- `ValueLatticeElement::get(Const)`：把精确常量包装成格值（状态为 `constant`，退化的 ConstantRange）
- `Solver.getLatticeValueFor(V)`：从 IPSCCP 求解器取 `V` 的格值，可能是 `overdefined`（未知）、精确常量、或带范围的 `constantrange`

`V1State`/`V2State` 按 LHS/RHS 顺序排列（与路径 A 中的 swap 逻辑对称），再通过 `getCompare` 在格值层面推断比较结果：

```
// ValueLattice.h 注释
// Compares this symbolic value with Other using Pred and returns either
// true, false or undef constants, or nullptr if the comparison cannot be evaluated.
```

---

### IR 示例

**场景 1（路径 A）：两个操作数都是精确常量**

```llvm
; 特化参数 %n 已知为 i32 5
define i1 @foo(i32 %n) {
  %cmp = icmp slt i32 %n, 10
  ret i1 %cmp
}
```

- `LastVisited->first = %n`（LHS），`Const = i32 5`，`ConstOnRHS = false`
- `V = I.getOperand(1) = i32 10`（字面常量），`findConstantFor` 直接返回 `i32 10`
- 走路径 A：无需 swap（ConstOnRHS = false）
- `ConstantFoldCompareInstOperands(slt, i32 5, i32 10, DL)` → `i1 true`

---

**场景 2（路径 B）：另一操作数非精确常量，但 IPSCCP 有范围信息**

```llvm
; 特化参数 %base 已知为 i32 100
; IPSCCP 已推导 %offset ∈ [0, 50]（ConstantRange）
define i1 @bar(i32 %base, i32 %offset) {
  %cmp = icmp sgt i32 %base, %offset   ; 100 > [0,50] → 必然 true
  ret i1 %cmp
}
```

- `LastVisited->first = %base`（LHS），`Const = i32 100`，`ConstOnRHS = false`
- `V = %offset`，`findConstantFor` 返回 `nullptr`（非精确常量）
- 走路径 B：
  - `ConstLV = ValueLatticeElement::get(i32 100)` → 精确常量格值
  - `OtherLV = Solver.getLatticeValueFor(%offset)` → `ConstantRange [0, 50]`
  - `V1State = ConstLV`（LHS），`V2State = OtherLV`（RHS）
  - `getCompare(sgt, i1, [0,50], DL)` → `i32 100 > [0,50]` 恒成立 → `i1 true`

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 双精确常量比较折叠 | `ConstantFoldCompareInstOperands(pred, LHS, RHS, DL)` | `llvm/Analysis/ConstantFolding.h` |
| 常量包装为格值 | `ValueLatticeElement::get(Constant*)` | `llvm/Analysis/ValueLattice.h:201` |
| 查询值的格值 | `Solver.getLatticeValueFor(Value*)` | `llvm/Transforms/Utils/SCCPSolver.h` |
| 格值层面比较推断 | `ValueLatticeElement::getCompare(pred, type, other, DL)` | `llvm/Analysis/ValueLattice.h:471` |

---

### 与其他 visitor 对比：`visitCmpInst` 的独特之处

`visitCmpInst` 是 `InstCostVisitor` 中**唯一同时使用精确常量折叠和格值推断两条路径**的 visitor：

| visitor | 折叠能力 |
|---|---|
| `visitUnaryOperator` | 仅精确常量折叠 |
| `visitBinaryOperator` | 精确常量折叠（双侧或单侧） |
| `visitSelectInst` | 精确常量折叠（条件或数据分支已知） |
| `visitCmpInst` | 精确常量折叠 **+** Lattice 范围比较（更强） |

Lattice 路径的价值在于：IPSCCP 在分析过程中可能已为某个值建立了 `ConstantRange`（例如循环变量、经过 range check 的参数），即便该值最终不能被 SCCP 折叠为单一常量，`visitCmpInst` 也能利用这些范围信息把 cmp 结果推断为 `true`/`false`，进而推动 branch/switch 折叠，累积出更高的特化收益估算。

---

### 其他补充

- **`getCompare` 返回 `nullptr` 的情形**：当格值信息不足以确定比较结果（如 `OtherLV` 为 `overdefined`），`getCompare` 返回 `nullptr`，`visitCmpInst` 随之返回 `nullptr`，表示无法折叠，cost model 不统计该指令的收益，保守且正确。
- **`fcmp` 与 NaN 语义**：`CmpInst` 同时涵盖 `icmp` 和 `fcmp`。`ConstantFoldCompareInstOperands` 对浮点比较会处理 NaN（`ordered` vs `unordered` 谓词），`getCompare` 在浮点 lattice 层面同样遵从 IEEE 语义。

