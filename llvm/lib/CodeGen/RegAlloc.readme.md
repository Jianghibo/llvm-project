# LLVM 寄存器分配实现分析报告

## 1. 框架概览

LLVM 寄存器分配采用 **RegAllocBase + 多种算法实现** 的架构，核心设计思想：

| 模块 | 位置 | 核心职责 |
|---|---|---|
| **RegAllocBase** | `llvm/lib/CodeGen/RegAllocBase.h/cpp` | 提供通用驱动框架：初始化、优先队列管理、主循环、失败处理 |
| **算法子类** | RegAllocFast / Basic / Greedy / PBQP | 实现 `selectOrSplit()` 核心策略 |
| **VirtRegRewriter** | `llvm/lib/CodeGen/VirtRegMap.cpp` | 将虚拟寄存器替换为物理寄存器，最终出口 |
| **分析依赖** | LiveIntervals / LiveRegMatrix / VirtRegMap | 提供生命周期分析、干涉查询、映射表 |

**设计原则**（RegAllocBase.h 注释）：
- 用 **LiveIntervalUnion** 实现 on-the-fly 干涉检查
- 寄存器分配质量主要取决于 **live range splitting** 而非最优染色
- 通过 **incremental splitting driver** 权衡编译时与代码质量

---

## 2. 寄存器分配在 CodeGen Pipeline 中的位置

### 2.1 入口位置（TargetPassConfig.cpp）

```cpp
// TargetPassConfig::addOptimizedRegAlloc() (行1485-1541)
addPass(&DetectDeadLanesID);
addPass(&InitUndefID);
addPass(&ProcessImplicitDefsID);
addPass(&UnreachableMachineBlockElimID);
addPass(&LiveVariablesID);
addPass(&MachineLoopInfoID);
addPass(&PHIEliminationID);                // ← SSA → 非 SSA
addPass(&TwoAddressInstructionPassID);     // ← 二地址指令转换
addPass(&RegisterCoalescerID);             // ← 寄存器合并
addPass(&RenameIndependentSubregsID);
addPass(&MachineSchedulerID);              // ← Pre-RA 调度
addRegAssignAndRewriteOptimized();         // ← 寄存器分配入口
```

**关键前置 Pass**：
- **PHIElimination**: 消除 PHI 指令，SSA → 非 SSA
- **TwoAddressInstruction**: 转换为二地址形式（如 `ADD %0, %1` → `ADD %0, %0, %1`）
- **RegisterCoalescer**: 合并可合并的 COPY 指令，减少寄存器压力
- **MachineScheduler (Pre-RA)**: 调度指令以提高 ILP，但受寄存器压力约束

### 2.2 寄存器分配流程

```cpp
// TargetPassConfig::addRegAssignAndRewriteOptimized() (行1451-1465)
addPass(createRegAllocPass(true));         // ← 具体分配算法（Greedy 等）
addPreRewrite();                           // ← 目标特定钩子
addPass(&VirtRegRewriterID);               // ← 虚拟寄存器替换（出口）
addPass(createRegAllocScoringPass());      // ← ML-driven 评分
```

### 2.3 出口位置（VirtRegRewriter）

**VirtRegRewriter** 是寄存器分配的最后一步：

```cpp
// VirtRegMap.cpp (行193-196)
// The VirtRegRewriter is the last of the register allocator passes.
// It rewrites virtual registers to physical registers as specified in
// the VirtRegMap analysis. It also updates live-in information on basic
// blocks according to LiveIntervals.
```

**核心功能**：
- 遍历所有指令，将虚拟寄存器替换为分配的物理寄存器
- 添加 spill/reload 指令（根据 VirtRegMap 映射）
- 更新基本块的 live-in 信息
- 清理冗余 COPY 指令

---

## 3. 与指令调度 Pass 的关系

### 3.1 Pre-RA Scheduling（寄存器分配之前）

**位置**：`MachineSchedulerID`，在寄存器合并之后、寄存器分配之前

**设计目标**（MachineScheduler.cpp 注释）：
```cpp
// MachineScheduler schedules machine instructions after phi elimination.
// It preserves LiveIntervals so it can be invoked before register allocation.
```

**关键特性**：
- **保留 LiveIntervals**：调度后生命周期分析仍然有效
- **寄存器压力感知**：调度时考虑寄存器压力，避免过度增加压力
- **优化 ILP**：通过指令重排提升指令级并行性
- **为寄存器分配做准备**：减少寄存器压力峰值，改善分配成功率

### 3.2 Post-RA Scheduling（寄存器分配之后）

**位置**：`PostRASchedulerID` 或 `PostMachineSchedulerID`，在寄存器分配之后

**设计目标**：
- **寄存器已固定**：不需要考虑寄存器压力
- **优化延迟**：减少关键路径延迟
- **处理硬件约束**：如流水线 hazard、资源冲突

**Pipeline**（TargetPassConfig.cpp 行1209-1215）：
```cpp
if (getOptLevel() != CodeGenOptLevel::None &&
    !TM->targetSchedulesPostRAScheduling()) {
  if (MISchedPostRA)
    addPass(&PostMachineSchedulerID);
  else
    addPass(&PostRASchedulerID);
}
```

### 3.3 两次调度对比

| 特性 | Pre-RA Scheduling | Post-RA Scheduling |
|---|---|---|
| **输入状态** | 虚拟寄存器 | 物理寄存器（已固定） |
| **主要约束** | 寄存器压力 + ILP | 硬件 hazard + 延迟 |
| **是否保留 LiveIntervals** | 是 | 否（已废弃） |
| **优化机会** | 减少寄存器压力峰值 | 优化流水线、减少 stall |
| **典型优化** | 指令聚类、减少 live range | 延迟优化、指令组合 |

---

## 4. 不同寄存器分配算法的使用与关系

### 4.1 四种算法对比

| 算法 | 注册名称 | 适用场景 | 核心策略 | 复杂度 |
|---|---|---|---|---|
| **RegAllocFast** | `fast` | `-O0`、调试构建 | 基本块内线性扫描，立即溢出 | 低（~1900 行） |
| **RegAllocBasic** | `basic` | 教学/基准测试 | 优先队列（按 spill weight） + 简单溢出 | 中等（~260 行） |
| **RegAllocGreedy** | `greedy` | **默认优化编译** `-O2/-O3` | Split/Eviction/Recoloring 等复杂策略 | 高（~3000 行） |
| **RegAllocPBQP** | `pbqp` | 特定场景（理论最优） | Partitioned Boolean Quadratic Programming | 中等 |

### 4.2 选择机制（TargetPassConfig.cpp）

```cpp
// 行1371-1379: 决定是否使用优化分配器
bool TargetPassConfig::getOptimizeRegAlloc() const {
  switch (OptimizeRegAlloc) {
  case cl::BOU_UNSET:
    return getOptLevel() != CodeGenOptLevel::None;  // ← 默认行为
  case cl::BOU_TRUE:  return true;
  case cl::BOU_FALSE: return false;
  }
}

// 行1403-1408: 创建目标默认分配器
FunctionPass *TargetPassConfig::createTargetRegisterAllocator(bool Optimized) {
  if (Optimized)
    return createGreedyRegisterAllocator();  // ← 优化编译用 Greedy
  else
    return createFastRegisterAllocator();    // ← 非优化用 Fast
}
```

**命令行覆盖**：
```bash
-regalloc=basic   # 强制使用 Basic
-regalloc=greedy  # 强制使用 Greedy（默认）
-regalloc=pbqp    # 使用 PBQP
-regalloc=fast    # 使用 Fast
```

### 4.3 各算法核心策略详解

#### RegAllocFast（快速分配器）

**位置**：`llvm/lib/CodeGen/RegAllocFast.cpp`

**特点**：
- **基本块级别分配**：逐 BB 处理，不跨 BB 考虑
- **线性扫描**：按指令顺序分配
- **立即溢出**：寄存器不足时直接 spill 到栈
- **无 live range splitting**：不分裂生命周期
- **编译速度快**：适合调试构建（-O0）

**适用场景**：
- `-O0` 编译（默认路径）
- 快速编译需求
- 调试场景（寄存器分配结果可预测）

#### RegAllocBasic（基础分配器）

**位置**：`llvm/lib/CodeGen/RegAllocBasic.cpp`

**特点**：
- **继承 RegAllocBase**：使用标准驱动框架
- **优先队列**：按 `spill weight` 排序（权重低优先分配）
- **简单策略**：无法分配时直接溢出
- **无 eviction**：不驱逐已分配寄存器
- **基准测试用途**：评估其他分配器的改进

**核心函数**（RegAllocBasic.cpp 行83-84）：
```cpp
MCRegister selectOrSplit(const LiveInterval &VirtReg,
                         SmallVectorImpl<Register> &SplitVRegs) override;
```

#### RegAllocGreedy（贪婪分配器 - 主力算法）

**位置**：`llvm/lib/CodeGen/RegAllocGreedy.cpp` (~3000 行)

**核心策略栈**：

```text
selectOrSplit()
├── tryAssign()              // 尝试直接分配（无干涉）
├── tryEvict()               // 驱逐低权重寄存器（若收益更高）
├── tryRegionSplit()         // 全局区域分裂（SplitKit）
├── tryBlockSplit()          // 基本块级别分裂
├── tryInstructionSplit()    // 单指令级分裂
├── tryLocalSplit()          // 本地分裂
├── tryLastChanceRecoloring() // 最后机会重染色（深度递归）
└── spill                    // 最终溢出
```

**关键创新**：

1. **Eviction（驱逐）**（RegAllocGreedy.cpp 行324-325）：
   - 计算驱逐已分配寄存器的收益
   - 使用 `RegAllocEvictionAdvisor`（ML-driven）决策
   - 防止驱逐循环（Cascade 编号）

2. **Live Range Splitting**（SplitKit）：
   - **Region Split**: 全局分裂，将 live range 分成多个片段
   - **Block Split**: 基本块级别分裂
   - **Instruction Split**: 指令级分裂
   - 使用 `SpillPlacement` 分析最佳分裂点

3. **Recoloring**（重染色）：
   - 当直接分配失败时，尝试重新分配已分配寄存器
   - `LastChanceRecoloring`：深度递归搜索（受 `-lcr-max-depth` 限制）
   - 防止无限递归（Cascade 防循环）

4. **Hint 处理**：
   - 优先考虑寄存器 hint（如 COPY hint）
   - `trySplitAroundHintReg`: 尝试围绕 hint 寄存器分裂

**Advisor 系统**（行186-194）：
```cpp
std::unique_ptr<RegAllocEvictionAdvisor> EvictAdvisor;  // 决定驱逐策略
std::unique_ptr<RegAllocPriorityAdvisor> PriorityAdvisor; // 决定分配优先级
```

**ML-driven 优化**：
- `MLRegAllocEvictAdvisor`: 使用机器学习模型决定驱逐
- `MLRegAllocPriorityAdvisor`: 使用 ML 决定分配优先级

#### RegAllocPBQP（PBQP 分配器）

**位置**：`llvm/lib/CodeGen/RegAllocPBQP.cpp`

**理论基础**：
- **Partitioned Boolean Quadratic Programming**
- 理论上可找到最优解（但编译时昂贵）
- 适合特定场景（如寄存器压力极高）

**特点**：
- 构建 PBQP 图：节点=虚拟寄存器，边=干涉
- 使用 PBQP solver 求解
- 不常用（编译时开销大）

---

## 5. RegAllocBase 框架详解

### 5.1 核心驱动流程（RegAllocBase.cpp 行87-154）

```cpp
void RegAllocBase::allocatePhysRegs() {
  seedLiveRegs();                          // 初始化优先队列

  while (const LiveInterval *VirtReg = dequeue()) {
    // 获取下一个待分配寄存器

    Matrix->invalidateVirtRegs();          // 清除干涉缓存

    VirtRegVec SplitVRegs;
    MCRegister PhysReg = selectOrSplit(*VirtReg, SplitVRegs);  // ← 子类核心逻辑

    if (PhysReg == ~0u) {
      // 分配失败（通常因 inline asm）
      PhysReg = getErrorAssignment(*RC, MI);
      cleanupFailedVReg(VirtReg->reg(), PhysReg, SplitVRegs);
    } else if (PhysReg) {
      Matrix->assign(*VirtReg, PhysReg);   // 分配到物理寄存器
    }

    // 将分裂产生的新虚拟寄存器加入队列
    for (Register Reg : SplitVRegs)
      enqueue(&LIS->getInterval(Reg));
  }
}
```

### 5.2 子类必须实现的接口

```cpp
// RegAllocBase.h 行117-131
virtual void enqueueImpl(const LiveInterval *LI) = 0;  // 加入优先队列
virtual const LiveInterval *dequeue() = 0;             // 从队列取出
virtual MCRegister selectOrSplit(const LiveInterval &VirtReg,
                                 SmallVectorImpl<Register> &splitLVRs) = 0; // 核心策略
virtual Spiller &spiller() = 0;                        // 溢出器
```

### 5.3 关键数据结构

| 结构 | 作用 | 位置 |
|---|---|---|
| **LiveIntervals** | 记录每个虚拟寄存器的生命周期 | `llvm/CodeGen/LiveIntervals.h` |
| **LiveRegMatrix** | 物理寄存器生命周期矩阵，用于干涉查询 | `llvm/CodeGen/LiveRegMatrix.h` |
| **VirtRegMap** | 虚拟寄存器 → 物理寄存器 / 栈槽映射 | `llvm/CodeGen/VirtRegMap.h` |
| **Spiller** | 溢出实现（InlineSpiller / StandardSpiller） | `llvm/CodeGen/Spiller.h` |

---

## 6. 关键辅助 Pass

### 6.1 LiveIntervals 计算

**位置**：`llvm/lib/CodeGen/LiveIntervals.cpp`

**作用**：计算每个虚拟寄存器的生命周期区间，寄存器分配的基础数据。

**计算方法**：
- 基于 `SlotIndexes`（指令编号）
- 遍历指令，标记 def/use 点
- 计算 live range segments

### 6.2 Register Coalescer（寄存器合并）

**位置**：`llvm/lib/CodeGen/RegisterCoalescer.cpp`

**作用**：合并 COPY 指令，减少寄存器压力。

**关键优化**：
- 合并 `COPY %vreg1, %vreg2` → 使用同一寄存器
- 必须保证合并安全（无干涉、无副作用）

### 6.3 SpillPlacement

**位置**：`llvm/lib/CodeGen/SpillPlacement.cpp`

**作用**：决定 live range splitting 的最佳分裂点。

**算法**：
- 基于 `EdgeBundles`（控制流边聚类）
- 计算每个 bundle 的 spill 成本
- 使用贪心算法选择最佳位置

---

## 7. 关键函数调用栈（RegAllocGreedy）

```text
run()
  -> init(VRM, LIS, Matrix)
  -> allocatePhysRegs()
     -> seedLiveRegs()
     -> while dequeue()
        -> selectOrSplit()
           -> tryAssign()
           -> tryEvict()
              -> EvictAdvisor->tryFindEvictionCandidate()
              -> evictInterference()
           -> tryRegionSplit()
              -> calcRegionSplitCost()
              -> doRegionSplit()
                 -> SplitEditor->split()
           -> tryLastChanceRecoloring()
              -> mayRecolorAllInterferences()
              -> tryRecoloringCandidates()
           -> spillVReg()
  -> postOptimization()
```

---

## 8. 总结与关键设计点

### 核心设计理念

1. **分离驱动与策略**：RegAllocBase 提供框架，子类实现 `selectOrSplit()`
2. **Live Range Splitting 优先**：而非最优图染色
3. **Incremental Splitting**：每次分裂产生新 interval，继续迭代
4. **Advisor 系统**：ML-driven 决策（驱逐/优先级）

### 关键权衡

| 因素 | Fast | Basic | Greedy | PBQP |
|---|---|---|---|---|
| **编译时** | 极快 | 中等 | 较慢 | 很慢 |
| **代码质量** | 低（多 spill） | 中等 | **高** | 理论最优 |
| **适用场景** | -O0 | 基准测试 | **-O2/-O3（默认）** | 特殊场景 |

### 关键改进点（Greedy vs Basic）

- **Eviction**：驱逐低权重寄存器而非立即 spill
- **Splitting**：多策略分裂而非整段 spill
- **Recoloring**：尝试重新分配而非放弃
- **ML Advisor**：智能决策而非硬编码启发式

---

## 9. 可继续深挖的问题

1. **SplitKit 机制**：如何实现高效的 live range splitting？
2. **EvictionAdvisor ML 模型**：如何训练、如何决策？
3. **InterferenceCache**：干涉查询的缓存机制？
4. **LiveRegMatrix**：如何快速检测寄存器干涉？
5. **Post-RA scheduling** 与 Pre-RA scheduling 的具体策略差异？
6. **如何调试寄存器分配失败**：`-verify-regalloc`、`opt-bisect`？

---

## 10. 建议的下一步行动

- 阅读 SplitKit 源码：`llvm/lib/CodeGen/SplitKit.cpp`
- 阅读 Advisor 实现：`llvm/lib/CodeGen/RegAllocEvictionAdvisor.cpp`
- 使用 `-debug-only=regalloc` 调试具体分配过程
- 测试不同算法：`clang -mllvm -regalloc=basic vs greedy`

---

## 11. 关键源码路径索引

| 模块 | 路径 |
|---|---|
| **RegAllocBase** | `llvm/lib/CodeGen/RegAllocBase.h`, `RegAllocBase.cpp` |
| **RegAllocFast** | `llvm/lib/CodeGen/RegAllocFast.cpp` |
| **RegAllocBasic** | `llvm/lib/CodeGen/RegAllocBasic.h`, `RegAllocBasic.cpp` |
| **RegAllocGreedy** | `llvm/lib/CodeGen/RegAllocGreedy.h`, `RegAllocGreedy.cpp` |
| **RegAllocPBQP** | `llvm/lib/CodeGen/RegAllocPBQP.cpp` |
| **VirtRegRewriter** | `llvm/lib/CodeGen/VirtRegMap.cpp:190` |
| **VirtRegMap** | `llvm/include/llvm/CodeGen/VirtRegMap.h` |
| **LiveIntervals** | `llvm/lib/CodeGen/LiveIntervals.cpp` |
| **LiveRegMatrix** | `llvm/lib/CodeGen/LiveRegMatrix.cpp` |
| **SplitKit** | `llvm/lib/CodeGen/SplitKit.h`, `SplitKit.cpp` |
| **SpillPlacement** | `llvm/lib/CodeGen/SpillPlacement.cpp` |
| **RegisterCoalescer** | `llvm/lib/CodeGen/RegisterCoalescer.cpp` |
| **MachineScheduler** | `llvm/lib/CodeGen/MachineScheduler.cpp` |
| **TargetPassConfig** | `llvm/lib/CodeGen/TargetPassConfig.cpp:1485` |
| **EvictionAdvisor** | `llvm/lib/CodeGen/RegAllocEvictionAdvisor.cpp` |
| **MLRegAllocEvictAdvisor** | `llvm/lib/CodeGen/MLRegAllocEvictAdvisor.cpp` |