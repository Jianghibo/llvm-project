# RegAllocGreedy Pass 关键函数调用栈分析

---

## 1. 整体架构

```text
RAGreedy Pass
├── 入口函数
│   ├── RAGreedyLegacy::runOnMachineFunction()  [Legacy PM]
│   │   └── RAGreedy::run(MF)                   [实际实现]
│   └── RAGreedyPass::run(MF, MFAM)             [New PM]
│       └── RAGreedy::run(MF)
│
├── 继承关系
│   ├── RAGreedy : public RegAllocBase          [继承基类驱动框架]
│   └── RAGreedy : private LiveRangeEdit::Delegate [响应 live range 编辑事件]
│
└── 核心数据结构
    ├── Queue (优先队列)                        [待分配虚拟寄存器]
    ├── ExtraRegInfo                            [Stage + Cascade 状态]
    ├── SpillerInstance                         [InlineSpiller]
    ├── SplitAnalysis (SA) / SplitEditor (SE)   [分裂工具]
    └── EvictAdvisor / PriorityAdvisor          [ML-driven 决策]
```

---

## 2. 关键函数调用栈（完整版）

### 2.1 Pass 入口 → 主循环

```text
run(MF)                                          // RegAllocGreedy.cpp:2931
├── RegAllocBase::init(VRM, LIS, Matrix)         // 初始化基类数据
├── initializeCSRCost()                          // 初始化 CSR 成本
├── createInlineSpiller()                        // 创建 Spiller
├── VRAI->calculateSpillWeightsAndHints()        // 计算 spill 权重和 hint
├── SA/SE 初始化                                 // SplitAnalysis/SplitEditor
│
├── allocatePhysRegs()                           // ← 继承调用：主循环入口
│   │                                            // RegAllocBase.cpp:87
│   ├── seedLiveRegs()                           // 初始化优先队列
│   │   └── for each VirtReg:
│   │       └── enqueue(&LIS->getInterval(Reg))
│   │
│   └── while (VirtReg = dequeue())              // ← 主循环
│       ├── Matrix->invalidateVirtRegs()         // 清除干涉缓存
│       │
│       ├── selectOrSplit(VirtReg, SplitVRegs)   // ← 核心策略函数
│       │   │                                    // RegAllocGreedy.cpp:2322
│       │   ├── selectOrSplitImpl(...)           // 实际实现
│       │   │                                    // 行2644-2744
│       │   │   [详见 2.2]
│       │   │
│       │   └── if Reg == ~0u:                   // 分配失败
│       │       └── emitError(...)               // 发出错误诊断
│       │
│       ├── if AvailablePhysReg:
│       │   └── Matrix->assign(VirtReg, PhysReg) // 分配成功
│       │
│       ├── if ~0u (失败):
│       │   └── getErrorAssignment()             // 强制分配占位寄存器
│       │   └── cleanupFailedVReg()              // 清理失败寄存器
│       │
│       └── for Reg in SplitVRegs:               // 处理分裂产生的新 vregs
│           └── enqueue(&LIS->getInterval(Reg))  // ← 加入队列，继续循环
│
├── tryHintsRecoloring()                         // 分配后修复 broken hints
│   └── for each broken hint:
│       └── tryHintRecoloring(LI)
│
├── postOptimization()                           // 后处理
│   └── spiller().postOptimization()
│   └── 清理 DeadRemats
│
└── reportStats()                                // 报告统计信息
```

---

### 2.2 selectOrSplitImpl：核心策略函数（详细调用栈）

```text
selectOrSplitImpl(VirtReg, NewVRegs, FixedRegisters, RecolorStack, Depth)
│                                               // RegAllocGreedy.cpp:2644
├── AllocationOrder::create()                   // 创建分配顺序
│
├── [策略 1] tryAssign()                         // 尝试直接分配
│   │                                            // 行535-587
│   ├── for PhysReg in Order:
│   │   └── Matrix->checkInterference(VirtReg, PhysReg)
│   │       └── if 无干涉:
│   │           └── return PhysReg              // ← 直接成功
│   │
│   ├── if 有 hint 且 hint 有干涉:
│   │   ├── EvictAdvisor->canEvictHintInterference()
│   │   ├── if 可驱逐:
│   │   │   └── evictInterference(VirtReg, PhysHint, NewVRegs)
│   │   │       │                                // 行620-657
│   │   │       ├── getOrAssignNewCascade(VirtReg)
│   │   │       ├── for each PhysReg unit:
│   │   │       │   └── Matrix->query()
│   │   │       │   └── Q.interferingVRegs()
│   │   │       │
│   │   │       └── for each Intf:
│   │   │           ├── Matrix->unassign(*Intf)
│   │   │           ├── setCascade(Intf, Cascade)
│   │   │           └── NewVRegs.push_back(Intf->reg()) // ← 被驱逐的加入队列
│   │   │       └── return PhysHint
│   │   │
│   │   └── else:
│   │       └── trySplitAroundHintReg()         // 尝试围绕 hint 分裂
│   │       └── SetOfBrokenHints.insert()       // 记录 broken hint
│   │
│   └── if PhysReg 有额外成本:
│       └── tryEvict()                          // ← 交叉调用：寻找更便宜的
│           [详见 2.3]
│
├── [策略 2] tryEvict()                          // 驱逐低权重寄存器
│   │                                            // 行716-729
│   [详见 2.3]
│   └── if 成功:
│       └── SetOfBrokenHints.insert()           // 记录 broken hint（用于后续修复）
│       └── return PhysReg
│
├── [等待第二轮] if Stage < RS_Split:
│   └── setStage(VirtReg, RS_Split)
│   └── NewVRegs.push_back(VirtReg.reg())        // ← 加入队列，等待第二轮
│   └── return MCRegister()                      // 本轮不分配
│
├── [策略 3] trySplit()                          // Live range 分裂
│   │                                            // 行1969-2004
│   [详见 2.4]
│
├── [策略 4] tryLastChanceRecoloring()           // 最后机会重染色
│   │                                            // 行2126-2276
│   [详见 2.5]
│   │                                            // ← 递归调用 selectOrSplitImpl
│
└── [最终] spill()                               // 溢出
    │                                            // 行2723-2743
    ├── LiveRangeEdit LRE(...)
    ├── spiller().spill(LRE, &Order)             // InlineSpiller::spill
    │   │                                        // InlineSpiller.cpp:1330
    │   ├── collectRegsToSpill()
    │   ├── reMaterializeAll()                   // 尝试重物化
    │   └── spillAll()                           // 生成 spill/reload 指令
    │       ├── for each use:
    │       │   └── insert store/load
    │       └── 创建新的 vregs（reload 后）
    │
    ├── ExtraInfo->setStage(NewVRegs, RS_Done)
    └── DebugVars->splitRegister()
    └── return MCRegister()                      // 本轮不分配，新 vregs 继续排队
```

---

### 2.3 tryEvict：驱逐策略（详细调用栈）

```text
tryEvict(VirtReg, Order, NewVRegs, CostPerUseLimit, FixedRegisters)
│                                               // RegAllocGreedy.cpp:716-729
├── EvictAdvisor->tryFindEvictionCandidate()    // ← ML-driven 决策
│   │                                            // 寻找最佳驱逐候选
│   ├── for PhysReg in Order:
│   │   ├── if 成本过高: continue
│   │   ├── Matrix->query(VirtReg, PhysReg)
│   │   ├── Q.interferingVRegs()                // 获取干涉寄存器
│   │   │
│   │   └── for each Intf in interferingVRegs:
│   │       ├── if Intf 已 Fixed:               // 不可驱逐
│   │       │   └── break
│   │       │
│   │       ├── if Intf.Cascade >= Cascade:     // 防止驱逐循环
│   │       │   └── break
│   │       │
│   │       ├── calculateEvictionCost(Intf)     // 计算驱逐成本
│   │       └── 更新 BestPhys 和 BestCost
│   │
│   └── return BestPhys                         // 最佳候选
│
└── if BestPhys:
    └── evictInterference(VirtReg, BestPhys, NewVRegs)
        │                                        // 行620-657
        ├── getOrAssignNewCascade(VirtReg)      // 分配 cascade 编号
        │
        ├── for each PhysReg unit:
        │   └── Matrix->query(VirtReg, Unit)
        │   └── Q.interferingVRegs()            // 收集干涉
        │
        └── for each Intf:
            ├── Matrix->unassign(*Intf)         // ← 解除分配
            ├── setCascade(Intf, Cascade)       // 设置 cascade（防循环）
            ├── ++NumEvicted                     // 统计
            └── NewVRegs.push_back(Intf->reg())  // ← 加入队列重新分配
```

---

### 2.4 trySplit：分裂策略（详细调用栈）

```text
trySplit(VirtReg, Order, NewVRegs, FixedRegisters)
│                                               // RegAllocGreedy.cpp:1969-2004
├── if Stage >= RS_Spill:                       // 已尝试过分裂
│   └── return MCRegister()
│
├── if VirtReg 是局部（在一个 MBB 内）:
│   ├── SA->analyze(&VirtReg)
│   │
│   ├── tryLocalSplit()                         // 行1738
│   │   ├── SE->openIntv()
│   │   ├── SE->useIntv()
│   │   ├── SE->closeIntv()
│   │   └── SE->finish()
│   │
│   └── if tryLocalSplit 失败:
│       └── tryInstructionSplit()              // 行1585
│           └── for each instruction:
│               └── SE->splitAroundInstr()
│
└── else (全局 live range):
    ├── SA->analyze(&VirtReg)
    │
    ├── if Stage < RS_Split2:
    │   └── tryRegionSplit()                    // 行1200-1231
    │       │                                    // 全局区域分裂
    │       ├── calcBlockSplitCost()
    │       ├── calcCompactRegion()             // 检查紧凑区域
    │       │
    │       ├── calculateRegionSplitCost()      // 行1233-1381
    │       │   ├── for PhysReg in Order:
    │       │   │   ├── SpillPlacer->prepare()
    │       │   │   ├── addSplitConstraints()   // 添加分裂约束
    │       │   │   ├── addThroughConstraints() // 添加穿透约束
    │       │   │   ├── growRegion()            // 扩展区域
    │       │   │   └── calcGlobalSplitCost()   // 计算成本
    │       │   │
    │       │   └── return BestCand             // 最佳候选
    │       │
    │       └── doRegionSplit()                 // 行1382-1459
    │           │                                // 执行分裂
    │           ├── SE->reset(LREdit)
    │           ├── for each candidate:
    │           │   └── SE->splitAroundRegion()
    │           └── SE->finish()
    │           └── DebugVars->splitRegister()
    │
    └── if tryRegionSplit 失败:
        └── tryBlockSplit()                     // 行1463-1499
            │                                    // 基本块分裂
            ├── SE->reset(LREdit)
            ├── for each UseBlock:
            │   └── SE->splitSingleBlock(BI)
            ├── SE->finish()
            └── setStage(NewVRegs, RS_Spill)     // 剩余部分标记为 Spill
```

---

### 2.5 tryLastChanceRecoloring：重染色策略（递归调用栈）

```text
tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters, RecolorStack, Depth)
│                                               // RegAllocGreedy.cpp:2126-2276
├── TRI->shouldUseLastChanceRecoloringForVirtReg() // 检查是否允许
├── if Depth >= MaxDepth && !ExhaustiveSearch:  // 深度限制
│   └── return ~0u
│
├── FixedRegisters.insert(VirtReg)              // ← 标记为 Fixed（防递归重染色）
│
├── for PhysReg in Order:
│   ├── Matrix->checkInterference()             // 检查干涉类型
│   │
│   ├── mayRecolorAllInterferences()            // 行2039-2081
│   │   │                                        // 检查所有干涉是否可重染色
│   │   ├── if 干涉数 >= MaxInterf:             // 干涉数量限制
│   │   │   └── return false
│   │   │
│   │   └── for each Intf:
│   │       ├── if Intf.Stage == RS_Done:       // 已完成，不可重染色
│   │       │   └── return false
│   │       ├── if Intf in FixedRegisters:      // 已 Fixed
│   │       │   └── return false
│   │       └── RecoloringCandidates.insert(Intf)
│   │
│   ├── for each Intf in RecoloringCandidates:
│   │   ├── RecolorStack.push_back(Intf, OldPhysReg) // ← 记录原始分配（用于回滚）
│   │   └── Matrix->unassign(*Intf)             // 解除分配
│   │
│   ├── Matrix->assign(VirtReg, PhysReg)        // 临时分配
│   │
│   ├── tryRecoloringCandidates()               // 行2286-2316
│   │   │                                        // ← 递归重染色所有干涉
│   │   ├── while Queue not empty:
│   │   │   ├── dequeue(LI)
│   │   │   │
│   │   │   ├── selectOrSplitImpl(LI, NewVRegs, FixedRegisters, RecolorStack, Depth+1)
│   │   │   │   // ← ← ← ← ← ← ← ← ← ← ← ← ← ← ←
│   │   │   │   // ← 递归调用：回到核心策略函数！
│   │   │   │   // ← 可能再次触发 tryEvict/trySplit/tryLastChanceRecoloring
│   │   │   │   // ← ← ← ← ← ← ← ← ← ← ← ← ← ← ←
│   │   │   │
│   │   │   ├── if PhysReg == ~0u:              // 递归失败
│   │   │   │   └── return false
│   │   │   │
│   │   │   └── Matrix->assign(*LI, PhysReg)    // 分配成功
│   │   │   └── FixedRegisters.insert(LI)       // ← 标记为 Fixed
│   │   │
│   │   └── return true                         // 所有干涉重染色成功
│   │
│   ├── if tryRecoloringCandidates 成功:
│   │   └── Matrix->unassign(VirtReg)           // 解除临时分配
│   │   └── return PhysReg                      // ← 成功返回
│   │
│   └── else (失败):                            // 回滚
│       ├── Matrix->unassign(VirtReg)
│       │
│       ├── for I in RecolorStack (reverse):    // 解除临时分配
│       │   └── Matrix->unassign(*LI)
│       │
│       ├── for I in RecolorStack:              // 恢复原始分配
│       │   └── Matrix->assign(*LI, OldPhysReg)
│       │
│       └── RecolorStack.resize(EntryStackSize) // 清空栈
│       └── continue (尝试下一个 PhysReg)
│
└── return ~0u                                  // 所有 PhysReg 都失败
```

---

## 3. 递归调用分析

### 3.1 selectOrSplitImpl ↔ tryLastChanceRecoloring ↔ tryRecoloringCandidates 递归链

```text
selectOrSplitImpl(VirtReg, Depth=D)                    [入口]
│
├── tryLastChanceRecoloring(VirtReg, Depth=D)
│   │
│   └── tryRecoloringCandidates(Queue, Depth=D)
│       │
│       └── for each Intf in Queue:
│           │
│           └── selectOrSplitImpl(Intf, Depth=D+1)     ← 递归！
│               │                                       [回到入口]
│               ├── tryAssign(Intf)
│               ├── tryEvict(Intf)
│               ├── trySplit(Intf)
│               │
│               └── tryLastChanceRecoloring(Intf, Depth=D+1)
│                   │                                   ← 二次递归！
│                   └── tryRecoloringCandidates(Queue', Depth=D+1)
│                       │
│                       └── selectOrSplitImpl(Intf', Depth=D+2) ← 三次递归！
│                           │                           ...
│                           └── until Depth >= MaxDepth  [终止]
```

**递归终止条件**：
- `Depth >= LastChanceRecoloringMaxDepth`（默认 5）
- 或 `!ExhaustiveSearch`（除非突破限制）

**递归栈最大深度**：默认 5 层（可配置）

---

### 3.2 allocatePhysRegs 主循环的迭代调用

```text
allocatePhysRegs()                                    [主循环]
│
├── seedLiveRegs()                                    [初始化]
│   └── enqueue(v1), enqueue(v2), ..., enqueue(vN)
│
├── dequeue() -> v1
│   └── selectOrSplit(v1)
│       ├── if 成功: Matrix->assign(v1, R1)
│       ├── if 分裂: enqueue(v1a), enqueue(v1b)       ← 新 vregs 加入队列
│       └── if 溢出: enqueue(v1_reload)               ← reload vregs 加入队列
│
├── dequeue() -> v2
│   └── selectOrSplit(v2)
│       ├── ...
│       └── if 驱逐 v1:
│           ├── Matrix->unassign(v1)
│           └── enqueue(v1)                           ← 被驱逐的 v1 重新加入队列！
│
├── dequeue() -> v1                                   ← v1 再次被取出！
│   └── selectOrSplit(v1)
│       └── ...                                       [第二轮分配]
│
└── dequeue() -> v1a                                  [分裂产生的 vreg]
    └── ...
```

**迭代终止条件**：Queue 为空（所有 vregs 分配完成）

---

## 4. 交叉调用分析

### 4.1 tryAssign → tryEvict 交叉调用

```text
tryAssign(VirtReg)
│
├── for PhysReg in Order:
│   └── if 无干涉:
│       └── 记录 PhysReg                             [候选]
│
├── if PhysReg 有额外成本 (Cost > 0):
│   └── tryEvict(VirtReg, CostLimit)                 ← 交叉调用
│       │                                            [寻找更便宜的]
│       ├── EvictAdvisor->tryFindEvictionCandidate()
│       │   └── return CheapReg
│       │
│       └── if CheapReg:
│           └── evictInterference()
│           └── return CheapReg
│
└── return PhysReg 或 CheapReg
```

**目的**：当前可用寄存器有额外成本（如 CSR），尝试驱逐更便宜的寄存器。

---

### 4.2 tryEvict → enqueue (通过 allocatePhysRegs)

```text
tryEvict(VirtReg)
│
└── evictInterference(VirtReg, BestPhys)
    │
    └── for each Intf:
        ├── Matrix->unassign(*Intf)
        └── NewVRegs.push_back(Intf->reg())          ← 被驱逐的加入 NewVRegs
│
└── selectOrSplit 返回 PhysReg
    │
    └── allocatePhysRegs:
        │   for Reg in NewVRegs:
        │       └── enqueue(Reg)                     ← 加入主队列
        │
        └── while loop:
            └── dequeue() -> Intf                    ← 被驱逐的 Intf 被取出
            └── selectOrSplit(Intf)                  ← 重新分配
```

---

### 4.3 spill → enqueue (通过 allocatePhysRegs)

```text
spiller().spill(LRE)
│
├── collectRegsToSpill()
├── reMaterializeAll()
├── spillAll()
│   └── 创建 reload vregs (v_reload1, v_reload2, ...)
│
└── return NewVRegs (reload vregs)
│
└── selectOrSplit 返回 MCRegister() (空)
    │
    └── allocatePhysRegs:
        │   for Reg in NewVRegs:
        │       └── enqueue(Reg)                     ← reload vregs 加入队列
        │
        └── while loop:
            └── dequeue() -> v_reload1
            └── selectOrSplit(v_reload1)              ← 分配 reload vreg
```

---

## 5. 关键数据流

### 5.1 Queue（优先队列）的生命周期

```text
初始化：
  seedLiveRegs()
    └── 所有 VirtRegs 加入 Queue

主循环：
  while (dequeue()):
    ├── 取出优先级最高的 VirtReg
    ├── selectOrSplit(VirtReg)
    │   ├── 成功分配 → 从 Queue 消失
    │   ├── 分裂 → 新 vregs 加入 Queue
    │   ├── 驱逐 → 被驱逐的加入 Queue
    │   └── 溢出 → reload vregs 加入 Queue
    │
    └── 处理 NewVRegs:
        └── enqueue(NewVRegs)                        ← 加入 Queue，继续循环

终止：
  Queue.empty()                                      ← 所有 vregs 分配完成
```

### 5.2 ExtraRegInfo（Stage + Cascade）的状态转换

```text
Stage 状态转换：
  RS_New     → RS_Assign     (enqueue 时设置)
  RS_Assign  → RS_Split      (第一轮失败)
  RS_Split   → RS_Split2     (trySplit 后)
  RS_Split2  → RS_Spill      (分裂失败后)
  RS_Spill   → RS_Done       (spill 后)

Cascade 状态转换：
  无         → Cascade=N     (evictInterference 时设置)
  Cascade=N  → Cascade=M     (被更高 Cascade 驱逐时更新)
  Cascade    → 无            (重新分配成功后清除)

防止驱逐循环：
  if Intf.Cascade >= VirtReg.Cascade:
    └── 不能驱逐 Intf                        [防止循环]
```

### 5.3 FixedRegisters（重染色会话）

```text
重染色会话（tryLastChanceRecoloring 递归链）：

开始：
  tryLastChanceRecoloring(VirtReg)
    └── FixedRegisters.insert(VirtReg)               ← VirtReg 标记为 Fixed

递归：
  tryRecoloringCandidates()
    └── selectOrSplitImpl(Intf)
        └── 成功分配
            └── FixedRegisters.insert(Intf)          ← Intf 也标记为 Fixed

回滚：
  if 重染色失败:
    ├── FixedRegisters = SaveFixedRegisters          ← 恢复保存的状态
    └── 所有临时 Fixed 标记清除

终止：
  重染色会话结束
    └── FixedRegisters 清空                          [下个 VirtReg 开始新会话]
```

---

## 6. 关键函数源码位置索引

| 函数 | 文件 | 行号 | 功能 |
|---|---|---|---|
| **run()** | RegAllocGreedy.cpp | 2931 | Pass 入口 |
| **allocatePhysRegs()** | RegAllocBase.cpp | 87 | 主循环 |
| **selectOrSplit()** | RegAllocGreedy.cpp | 2322 | 基类接口 |
| **selectOrSplitImpl()** | RegAllocGreedy.cpp | 2644 | 核心策略函数 |
| **tryAssign()** | RegAllocGreedy.cpp | 535 | 直接分配策略 |
| **tryEvict()** | RegAllocGreedy.cpp | 716 | 驱逐策略 |
| **evictInterference()** | RegAllocGreedy.cpp | 620 | 执行驱逐 |
| **trySplit()** | RegAllocGreedy.cpp | 1969 | 分裂策略 |
| **tryRegionSplit()** | RegAllocGreedy.cpp | 1200 | 区域分裂 |
| **tryBlockSplit()** | RegAllocGreedy.cpp | 1463 | 基本块分裂 |
| **tryLocalSplit()** | RegAllocGreedy.cpp | 1738 | 局部分裂 |
| **tryInstructionSplit()** | RegAllocGreedy.cpp | 1585 | 指令分裂 |
| **tryLastChanceRecoloring()** | RegAllocGreedy.cpp | 2126 | 重染色策略 |
| **tryRecoloringCandidates()** | RegAllocGreedy.cpp | 2286 | 递归重染色 |
| **mayRecolorAllInterferences()** | RegAllocGreedy.cpp | 2039 | 干涉检查 |
| **spill()** | RegAllocGreedy.cpp | 2723 | 溢出处理 |
| **InlineSpiller::spill()** | InlineSpiller.cpp | 1330 | Spill 实现 |
| **tryHintsRecoloring()** | RegAllocGreedy.cpp | 2632 | Hint 重染色 |
| **tryHintRecoloring()** | RegAllocGreedy.cpp | 2523 | 单个 Hint 重染色 |
| **enqueue()** | RegAllocGreedy.cpp | 423 | 加入队列 |
| **dequeue()** | RegAllocBase.cpp | 虚函数 | 从队列取出 |
| **getErrorAssignment()** | RegAllocBase.cpp | 215 | 强制分配 |
| **cleanupFailedVReg()** | RegAllocBase.cpp | 169 | 清理失败寄存器 |

---

## 7. 调用栈特点总结

### 7.1 递归调用

| 递归链 | 最大深度 | 终止条件 |
|---|---|---|
| **selectOrSplitImpl → tryLastChanceRecoloring → tryRecoloringCandidates → selectOrSplitImpl** | 5（默认） | Depth >= MaxDepth |
| **tryRecoloringCandidates 内部迭代** | N（干涉数量） | Queue.empty() |

### 7.2 迭代调用

| 迭代链 | 终止条件 |
|---|---|
| **allocatePhysRegs 主循环** | Queue.empty() |
| **tryEvict 内部遍历** | Order.end() |
| **tryAssign 内部遍历** | Order.end() |

### 7.3 交叉调用

| 调用链 | 目的 |
|---|---|
| **tryAssign → tryEvict** | 寻找成本更低的寄存器 |
| **tryEvict → evictInterference → NewVRegs → enqueue** | 被驱逐的重新排队 |
| **trySplit → doRegionSplit → SE->finish → NewVRegs → enqueue** | 分裂产生的新 vregs 排队 |
| **spill → InlineSpiller::spill → NewVRegs → enqueue** | Reload vregs 排队 |
| **selectOrSplitImpl → tryLastChanceRecoloring → tryRecoloringCandidates → selectOrSplitImpl** | 递归重染色 |

---

## 8. 核心设计模式

### 8.1 策略模式（Strategy Pattern）

```cpp
selectOrSplitImpl() {
  // 策略链：按优先级尝试不同策略
  tryAssign();      // 策略1：直接分配
  tryEvict();       // 策略2：驱逐
  trySplit();       // 策略3：分裂
  tryLastChanceRecoloring(); // 策略4：重染色
  spill();          // 策略5：溢出
}
```

### 8.2 迭代收敛模式

```cpp
allocatePhysRegs() {
  while (queue not empty) {
    VirtReg = dequeue();
    PhysReg = selectOrSplit(VirtReg);
    if (分配失败) {
      // 分裂/驱逐/溢出产生新 vregs
      enqueue(NewVRegs);  // 加入队列继续迭代
    }
  }
  // 收敛：所有 vregs 分配完成
}
```

### 8.3 回滚模式（Backtracking）

```cpp
tryLastChanceRecoloring() {
  RecolorStack 记录原始分配;
  try {
    递归重染色;
  } catch (失败) {
    回滚：恢复 RecolorStack 中的原始分配;
  }
}
```

---

## 9. 关键观察

1. **主循环驱动**：allocatePhysRegs 是核心驱动，所有策略结果最终回到主循环迭代
2. **优先队列**：按优先级分配，大/全局 live range 优先
3. **Stage 管理**：防止重复尝试同一策略，确保向前推进
4. **Cascade 防循环**：驱逐时设置 cascade 编号，防止驱逐循环
5. **Fixed 防递归重染色**：重染色会话中标记 Fixed，防止递归重染色循环
6. **分裂优先于溢出**：多次尝试分裂，最后才溢出
7. **重染色是最后手段**：Stage >= RS_Done 或不可 Spill 才触发

---

## 10. 简化版调用栈（核心路径）

```text
run(MF)
  → allocatePhysRegs()
    → while dequeue()
      → selectOrSplit(VirtReg)
        → selectOrSplitImpl()
          ├─ tryAssign()         → 成功: assign | 失败: 继续
          ├─ tryEvict()          → 成功: evict & enqueue | 失败: 继续
          ├─ wait (Stage < RS_Split) → enqueue & return
          ├─ trySplit()          → 成功: split & enqueue | 失败: 继续
          ├─ tryLastChanceRecoloring() → 成功: return | 失败: 继续
          │   → tryRecoloringCandidates()
          │     → selectOrSplitImpl(Depth+1) ← 递归！
          └─ spill()             → spill & enqueue
        → 处理 NewVRegs: enqueue
  → tryHintsRecoloring()
  → postOptimization()
```

---

## 11. 图形化调用关系

### 11.1 策略链

```
┌─────────────────────────────────────────────────────────────────┐
│                   selectOrSplitImpl                              │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略1: tryAssign()                                        │   │
│  │   ├─ 无干涉 → 成功                                        │   │
│  │   ├─ 有干涉 → tryEvict (交叉调用)                          │   │
│  │   └─ 有 hint → evictInterference / trySplitAroundHintReg  │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略2: tryEvict()                                         │   │
│  │   ├─ 找到候选 → evictInterference → NewVRegs              │   │
│  │   └─ 没找到 → 继续                                        │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略3: wait (Stage < RS_Split)                            │   │
│  │   └─ enqueue(VirtReg) → return                            │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略4: trySplit()                                         │   │
│  │   ├─ 局部 → tryLocalSplit / tryInstructionSplit           │   │
│  │   ├─ 全局 → tryRegionSplit / tryBlockSplit                │   │
│  │   └─ NewVRegs → enqueue                                   │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略5: tryLastChanceRecoloring()                          │   │
│  │   ├─ 递归重染色 → selectOrSplitImpl(Depth+1)              │   │
│  │   ├─ 成功 → return                                        │   │
│  │   └─ 失败 → 回滚 → continue                               │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 策略6: spill()                                            │   │
│  │   └─ InlineSpiller::spill → NewVRegs → enqueue            │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 11.2 主循环迭代

```
┌─────────────────────────────────────────────────────────────────┐
│                   allocatePhysRegs                               │
│                                                                   │
│   Queue: [v1, v2, v3, ...]                                       │
│                                                                   │
│   ┌─────────────────────────────────────────────────────────┐    │
│   │ dequeue() → v1                                           │    │
│   │   └─ selectOrSplit(v1)                                   │    │
│   │      ├─ 成功: assign(v1, R1)                              │    │
│   │      ├─ 驱逐: enqueue(v1重新)                             │    │
│   │      ├─ 分裂: enqueue(v1a, v1b)                           │    │
│   │      └─ 溢出: enqueue(v1_reload)                          │    │
│   └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│   Queue: [v2, v3, ..., v1重新, v1a, v1b, v1_reload]              │
│                                                                   │
│   ┌─────────────────────────────────────────────────────────┐    │
│   │ dequeue() → v2                                           │    │
│   │   └─ selectOrSplit(v2) ...                               │    │
│   └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│   Queue: [..., v1重新, v1a, v1b, ...]                            │
│                                                                   │
│   ┌─────────────────────────────────────────────────────────┐    │
│   │ dequeue() → v1重新                                       │    │
│   │   └─ selectOrSplit(v1重新) → 第二轮分配                   │    │
│   └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│   ...                                                             │
│                                                                   │
│   Queue: []                                                       │
│                                                                   │
│   └─ 终止                                                         │
└─────────────────────────────────────────────────────────────────┘
```

### 11.3 递归重染色

```
┌─────────────────────────────────────────────────────────────────┐
│ Depth=0: selectOrSplitImpl(VirtReg)                              │
│   └─ tryLastChanceRecoloring(VirtReg, D=0)                       │
│      └─ FixedRegisters.insert(VirtReg)                           │
│      └─ tryRecoloringCandidates(Queue=[Intf1, Intf2])            │
│                                                                   │
│         ┌──────────────────────────────────────────────────┐     │
│         │ Depth=1: selectOrSplitImpl(Intf1)                 │     │
│         │   ├─ tryAssign(Intf1)                              │     │
│         │   ├─ tryEvict(Intf1)                               │     │
│         │   ├─ trySplit(Intf1)                               │     │
│         │   └─ tryLastChanceRecoloring(Intf1, D=1)           │     │
│         │      └─ tryRecoloringCandidates(Queue=[Intf1'])    │     │
│         │                                                    │     │
│         │         ┌────────────────────────────────────┐     │     │
│         │         │ Depth=2: selectOrSplitImpl(Intf1') │     │     │
│         │         │   └─ tryLastChanceRecoloring(D=2)  │     │     │
│         │         │      └─ until D >= 5: return ~0u   │     │     │
│         │         └────────────────────────────────────┘     │     │
│         └──────────────────────────────────────────────────┘     │
│                                                                   │
│         ┌──────────────────────────────────────────────────┐     │
│         │ Depth=1: selectOrSplitImpl(Intf2)                 │     │
│         │   └─ ...                                          │     │
│         └──────────────────────────────────────────────────┘     │
│                                                                   │
│      └─ return (成功 or 失败+回滚)                                │
└─────────────────────────────────────────────────────────────────┘
```

---

## 12. 总结

RegAllocGreedy Pass 的核心是一个**迭代收敛的主循环**（allocatePhysRegs），驱动一个**策略链**（selectOrSplitImpl），策略链中包含**递归重染色**（tryLastChanceRecoloring）。

**关键设计**：
- **迭代收敛**：主循环不断从队列取出 vreg，分配或产生新 vreg，直到队列空
- **策略链**：按优先级尝试不同策略（Assign → Evict → Split → Recoloring → Spill）
- **递归重染色**：最大深度 5 层，防止无限递归
- **防循环机制**：Cascade（驱逐防循环）、Fixed（重染色防循环）
- **Stage 管理**：确保向前推进，避免重复尝试

**核心洞察**：
> "代码质量由 live range splitting 决定，而非最优染色"（RegAllocBase.h）
> 
> Greedy 通过**迭代分裂**替代传统图染色的**全局规划**，实现编译时间可控且代码质量足够好。
---

## 13. RegAllocBase 与 RAGreedy 交互关系分析

### 13.1 继承关系与设计模式

```
RegAllocBase (基类,框架驱动)
    └── RAGreedy (派生类,具体策略)
```

**设计模式**: 模板方法模式
- `RegAllocBase`: 定义寄存器分配的骨架流程和稳定接口
- `RAGreedy`: 实现贪婪分配策略的具体逻辑

---

### 13.2 关键虚函数接口(继承契约)

| 虚函数 | 调用者 | 实现者 | 作用 |
|---|---|---|---|
| `selectOrSplit()` | `RegAllocBase::allocatePhysRegs()` | `RAGreedy` | 核心决策:返回可用物理寄存器或分裂出的新虚拟寄存器 |
| `enqueueImpl()` | `RegAllocBase::enqueue()` | `RAGreedy` | 将待分配的虚拟寄存器加入优先级队列 |
| `dequeue()` | `RegAllocBase::allocatePhysRegs()` | `RAGreedy` | 从优先级队列取出下一个待分配寄存器 |
| `spiller()` | `RegAllocBase::postOptimization()` | `RAGreedy` | 返回溢出器实例,用于溢出优化 |
| `aboutToRemoveInterval()` | `RegAllocBase` | `RAGreedy` | 删除 LiveInterval 前的清理回调 |

---

### 13.3 执行流程(主调用链)

```text
RAGreedy::run(MachineFunction &mf)
  ├── RegAllocBase::init(VirtRegMap, LiveIntervals, LiveRegMatrix)  // 初始化基类字段
  ├── [初始化 RAGreedy 自己的状态: ExtraInfo, Advisors, Spiller, SplitAnalysis]
  ├── allocatePhysRegs()                                             // 基类驱动入口
  │     ├── seedLiveRegs()                                           // 将所有虚拟寄存器入队
  │     │     └── enqueue(LiveInterval)                             // 逐个入队
  │     │           └── enqueueImpl(LI)                             // RAGreedy 实现优先级计算
  │     └── while (dequeue())                                       // 循环分配
  │     │     ├── dequeue()                                         // RAGreedy 实现优先级队列取出
  │     │     ├── selectOrSplit(VirtReg, SplitVRegs)                // RAGreedy 实现核心决策
  │     │     │     └── selectOrSplitImpl(...)                      // RAGreedy 内部实现
  │     │     │           ├── tryAssign()                           // 尝试直接分配
  │     │     │           ├── tryEvict()                            // 尝试驱逐其他寄存器
  │     │     │           ├── trySplit()                            // 尝试分裂 LiveRange
  │     │     │           └── spiller().spill()                     // 最后溢出到栈
  │     │     ├── Matrix->assign(VirtReg, PhysReg)                  // 基类执行分配
  │     │     └── enqueue(SplitVRegs)                               // 将分裂产生的新寄存器入队
  │     └── postOptimization()                                      // 基类收尾
  │           └── spiller().postOptimization()                      // RAGreedy 提供的 Spiller
  ├── tryHintsRecoloring()                                          // RAGreedy 特有的 Hint 重着色
  └── reportStats()                                                 // RAGreedy 统计报告
```

---

### 13.4 RegAllocBase 的职责(驱动框架)

| 函数 | 职责 | 是否调用派生类接口 |
|---|---|---|
| `init()` | 初始化 TRI, MRI, VRM, LIS, Matrix 等公共字段 | ❌ |
| `allocatePhysRegs()` | **主驱动循环**:种子入队 → 循环 dequeue → selectOrSplit → 分配/入队新分裂寄存器 | ✅ dequeue, selectOrSplit, enqueue |
| `seedLiveRegs()` | 遍历所有虚拟寄存器并 `enqueue()` | ✅ enqueue |
| `enqueue()` | 公共入口,检查已分配/过滤 → 调用 `enqueueImpl()` | ✅ enqueueImpl |
| `postOptimization()` | 调用 `spiller().postOptimization()` 并清理 DeadRemats | ✅ spiller |

---

### 13.5 RAGreedy 的职责(贪婪策略实现)

#### 13.5.1 核心决策 `selectOrSplitImpl()` 的执行路径

```text
selectOrSplitImpl(VirtReg, NewVRegs, FixedRegisters, RecolorStack, Depth)
  ├── tryAssign()                         // 1. 尝试直接分配空闲物理寄存器
  │     └── Matrix->checkInterference()   // 检查冲突
  │     └── Matrix->assign()              // 如果无冲突,直接分配
  │
  ├── tryEvict()                          // 2. 尝试驱逐低优先级寄存器
  │     └── EvictAdvisor->tryFindEvictionCandidate() // 使用驱逐决策器
  │     └── evictInterference()           // 驱逐选定的寄存器
  │     └── Matrix->assign()              // 分配当前寄存器到释放出的物理寄存器
  │
  ├── [Stage < RS_Split] → 标记为 RS_Split,重新入队等待第二轮
  │
  ├── trySplit()                          // 3. 尝试分裂 LiveRange
  │     ├── tryRegionSplit()              // 全局分裂
  │     ├── tryBlockSplit()               // 块分裂
  │     ├── tryLocalSplit()               // 局部分裂
  │     └── tryInstructionSplit()         // 指令级分裂
  │     └── SplitEditor::split()          // 执行分裂,产生 NewVRegs
  │
  ├── tryLastChanceRecoloring()           // 4. 最后机会重着色(深度递归)
  │     └── [递归调用 selectOrSplitImpl()]// 尝试重着色冲突寄存器
  │
  └── spiller().spill(LRE)                // 5. 溢出到栈槽
```

#### 13.5.2 优先级队列管理 `enqueueImpl() / dequeue()`

- **优先级计算**: `PriorityAdvisor->getPriority(LI)`
- **优先级组成**:
  - LiveRange 大小
  - 全局/局部属性
  - 寄存器类分配优先级
  - Stage(RS_New, RS_Assign, RS_Split, RS_Spill)
- **队列结构**: `std::priority_queue<pair<unsigned, unsigned>>`

#### 13.5.3 状态管理: `ExtraRegInfo`

| 字段 | 含义 |
|---|---|
| `Stage` | LiveRange 处理阶段 |
| `Cascade` | 驱逐循环防护标记,防止无限驱逐循环 |

---

### 13.6 数据共享(基类 protected 字段)

| 字段 | 类型 | 用途 |
|---|---|---|
| `TRI` | `TargetRegisterInfo*` | 目标寄存器信息 |
| `MRI` | `MachineRegisterInfo*` | 机器寄存器信息 |
| `VRM` | `VirtRegMap*` | 虚拟寄存器映射 |
| `LIS` | `LiveIntervals*` | 活跃区间分析 |
| `Matrix` | `LiveRegMatrix*` | 冲突矩阵(核心数据结构) |
| `RegClassInfo` | `RegisterClassInfo` | 寄存器类信息缓存 |
| `DeadRemats` | `SmallPtrSet<MachineInstr*, 32>` | 待删除的重物质化指令 |

`RAGreedy` 直接访问这些字段,无需自己维护副本。

---

### 13.7 关键交互点详解

#### 13.7.1 分配循环(`allocatePhysRegs`)

```cpp
while (const LiveInterval *VirtReg = dequeue()) {  // RAGreedy 决定下一个
  // ...
  MCRegister PhysReg = selectOrSplit(*VirtReg, SplitVRegs); // RAGreedy 决策
  if (PhysReg)
    Matrix->assign(*VirtReg, PhysReg);              // 基类执行
  for (Register Reg : SplitVRegs)
    enqueue(&LIS->getInterval(Reg));                // 基类管理新分裂寄存器
}
```

- **基类**: 控制循环节奏、执行分配动作、管理队列状态
- **派生类**: 决定谁先分配、能否分配、如何分裂

#### 13.7.2 失败处理(`cleanupFailedVReg`)

```cpp
// selectOrSplit 返回 ~0u(失败)时
AvailablePhysReg = getErrorAssignment(*RC, MI);  // 基类选择一个寄存器作为错误占位
cleanupFailedVReg(FailedReg, PhysReg, SplitRegs);// 基类清理失败寄存器的活跃范围
```

#### 13.7.3 LiveRangeEdit 回调

`RAGreedy` 还实现了 `LiveRangeEdit::Delegate` 接口:

| 回调 | 时机 |
|---|---|
| `LRE_CanEraseVirtReg()` | LiveRange 编辑前询问能否删除 |
| `LRE_WillShrinkVirtReg()` | LiveRange 收缩前重新入队 |
| `LRE_DidCloneVirtReg()` | LiveRange 克隆后同步 ExtraRegInfo |

---

### 13.8 对比其他寄存器分配器

LLVM 中其他分配器(如 `RegAllocBasic`, `RegAllocFast`)也继承 `RegAllocBase`,实现不同的 `selectOrSplit` 策略:

| 分配器 | `selectOrSplit` 策略 |
|---|---|
| `RAGreedy` | 贪婪分配 + 多级分裂 + 驱逐 + 重着色 |
| `RegAllocBasic` | 简单的线性扫描 + 分裂 |
| `RegAllocFast` | 本地快速分配(无全局分裂) |

---

### 13.9 总结

**核心交互模式**: 
- `RegAllocBase` 是稳定的**驱动层**,控制何时做什么
- `RAGreedy` 是灵活的**策略层**,控制如何做决策

**关键文件路径**:
- `RegAllocBase.h`: `llvm/lib/CodeGen/RegAllocBase.h:63-152`
- `RegAllocBase.cpp`: `llvm/lib/CodeGen/RegAllocBase.cpp:87-154` (`allocatePhysRegs`)
- `RegAllocGreedy.h`: `llvm/lib/CodeGen/RegAllocGreedy.h:59-60` (继承声明)
- `RegAllocGreedy.cpp`: `llvm/lib/CodeGen/RegAllocGreedy.cpp:2644-2744` (`selectOrSplitImpl`)
- `RegAllocGreedy.cpp`: `llvm/lib/CodeGen/RegAllocGreedy.cpp:2984` (`allocatePhysRegs` 调用点)

**为什么这样设计**:
- 框架稳定,策略可扩展
- 不同分配器共享驱动逻辑,减少重复代码
- 通过虚函数接口隔离变化点

---

## calculateSpillWeightsAndHints 函数分析

### 函数签名与目的（33行）
```cpp
void VirtRegAuxInfo::calculateSpillWeightsAndHints()
```

**功能**: 遍历**当前 MachineFunction（单个函数）内的所有虚拟寄存器**，为每个寄存器的活跃区间计算溢出权重和分配提示。

---

### 整体结构

```
calculateSpillWeightsAndHints()
├── 打印调试信息（显示当前函数名）
├── 获取当前函数的 MachineRegisterInfo
├── 遍历当前函数内所有虚拟寄存器（for loop）
│   ├── 转换索引到虚拟寄存器编号（函数级作用域）
│   ├── 检查寄存器是否被使用（排除空寄存器）
│   └── 调用 calculateSpillWeightAndHint 计算权重和提示
└── 返回（无返回值，直接修改 LiveInterval）
```

---

### 逐段注释

**1. 调试信息打印 (33-36)**

```cpp
LLVM_DEBUG(dbgs() << "********** Compute Spill Weights **********\n"
                  << "********** Function: " << MF.getName() << '\n');
```

目的：在调试模式下打印函数名，标识当前正在计算溢出权重的函数。  
注释：使用 `DEBUG_TYPE "calcspillweights"`，可通过 `-debug-only=calcspillweights` 查看输出。`MF.getName()` 明确显示当前处理的是哪个函数。

**2. 获取寄存器信息并遍历 (37-44)**

```cpp
MachineRegisterInfo &MRI = MF.getRegInfo();
for (unsigned I = 0, E = MRI.getNumVirtRegs(); I != E; ++I) {
  Register Reg = Register::index2VirtReg(I);
  if (MRI.reg_nodbg_empty(Reg))
    continue;
  calculateSpillWeightAndHint(LIS.getInterval(Reg));
}
```

目的：遍历**当前 MachineFunction 内的所有虚拟寄存器**，为每个活跃区间计算溢出权重。  
注释：
- `MF.getRegInfo()` — 获取**当前函数 MF** 的寄存器信息，明确了作用域是函数级
- `MRI.getNumVirtRegs()` — 返回**该函数内部**的虚拟寄存器总数，不同函数的虚拟寄存器编号独立
- `Register::index2VirtReg(I)` — 将线性索引转换为虚拟寄存器编号（0-based 编号）
- `MRI.reg_nodbg_empty(Reg)` — 过滤掉未被使用（排除调试信息）的寄存器，避免无效计算
- `LIS.getInterval(Reg)` — 获取寄存器的活跃区间对象，传递给计算函数

---

### 关键数据结构

| 结构 | 字段/方法 | 含义 |
|---|---|---|
| `MachineFunction` | `getRegInfo()` | 获取当前函数的寄存器信息（函数级作用域） |
| `MachineRegisterInfo` | `getNumVirtRegs()` | 获取**当前函数内**的虚拟寄存器数量 |
| `Register` | `index2VirtReg()` | 索引到虚拟寄存器编号的转换方法 |
| `LiveInterval` | `weight`, `hints` | 活跃区间，存储溢出权重和分配提示 |

---

### 优化意图

1. **批量处理优化**：使用线性遍历而非复杂的迭代器，提高效率。
2. **过滤优化**：跳过空寄存器（`reg_nodbg_empty`），避免无效计算。
3. **职责分离**：本函数仅负责遍历和调度，具体计算委托给 `calculateSpillWeightAndHint`。
4. **函数级隔离**：每个函数独立处理，避免跨函数干扰，符合寄存器分配的设计（寄存器分配是逐函数进行的）。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| **虚拟寄存器范围** | 仅处理当前 MachineFunction 内的虚拟寄存器 | 误解为全局范围会导致混淆；跨函数的虚拟寄存器编号独立，不在此处理 |
| 只处理被使用的寄存器 | `reg_nodbg_empty(Reg)` 过滤 | 计算空寄存器浪费资源 |
| 调用前提 | `LIS` 必须已构建活跃区间分析 | 若未构建则 `getInterval` 会失败 |
| 修改 LiveInterval | 直接修改 `LiveInterval` 的权重和提示 | 需确保后续寄存器分配器正确使用 |
| 函数级作用域 | 寄存器分配逐函数进行，每个函数有独立的 LiveInterval 分析 | 不同函数的虚拟寄存器互不干扰 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 | 作用域说明 |
|------|-----|------|---|
| 获取当前函数的寄存器信息 | `MF.getRegInfo()` | 行37 | 返回当前 MachineFunction 的 MRI，明确函数级作用域 |
| 获取虚拟寄存器数量 | `MRI.getNumVirtRegs()` | 行38 | **当前函数内**的虚拟寄存器总数 |
| 索引转虚拟寄存器 | `Register::index2VirtReg(I)` | 行39 | 函数内的 0-based 编号转换 |
| 检查寄存器是否被使用 | `MRI.reg_nodbg_empty(Reg)` | 行40 | 函数内该寄存器的使用情况 |
| 获取活跃区间 | `LIS.getInterval(Reg)` | 行42 | 当前函数的 LiveInterval 分析结果 |
| 计算权重和提示 | `calculateSpillWeightAndHint(LI)` | 行42 | 对单个活跃区间进行计算 |

---

### 在 Pass Pipeline 中的位置

- **所属 Pass**: 寄存器分配辅助分析（VirtRegAuxInfo）
- **粒度**: 函数级（Function-level analysis）
- **调用时机**: 在活跃区间分析完成后，寄存器分配前
- **目的**: 为寄存器分配器提供决策依据（溢出权重、分配提示）
- **下游消费者**: 寄存器分配器（如 greedy allocator）使用权重决定溢出优先级

---

### 核心调用栈

```text
calculateSpillWeightsAndHints()              // 遍历当前函数内所有虚拟寄存器
  -> calculateSpillWeightAndHint(LI)         // 计算单个活跃区间
     -> weightCalcHelper(LI)                 // 核心权重计算逻辑
        -> LiveIntervals::getSpillWeight()  // 计算指令权重
        -> copyHint()                        // 提取 COPY 提示
        -> isRematerializable()              // 判断是否可重物化
```

---

### 其他补充

**虚拟寄存器作用域详解**：
- **函数级作用域**：虚拟寄存器在寄存器分配前是 SSA 形式的临时寄存器（如 `%v0`, `%v1`），它们的编号在**每个函数内独立**，不同函数的 `%v0` 是不同的寄存器
- **为什么是函数级**：
  1. 寄存器分配是逐函数进行的（每个函数有独立的寄存器分配策略）
  2. LiveInterval 分析是函数级的（活跃区间只在函数内部有意义）
  3. 不同函数的虚拟寄存器互不干扰（函数调用边界有明确的调用约定）
- **跨函数处理**：如果需要跨函数的寄存器分析（如全局寄存器分配、链接时优化 LTO），那是调用图层面的分析，不在本 Pass 处理

**权重含义**：
- 权重越大 → 溢出代价高 → 寄存器分配器倾向于优先分配该寄存器
- 权重为负（-1.0） → 标记为不可溢出

**核心计算逻辑在 weightCalcHelper**：
- 统计活跃区间内所有指令的权重（读/写、循环嵌套、频率）
- 提取 COPY 指令的分配提示
- 调整权重（可重物化降权重、零长度区间标记不可溢出）

<!-- Group A: Entry & Setup Functions -->

## RAGreedyPass::run 函数分析

### 函数签名与目的（行号）
```cpp
PreservedAnalyses RAGreedyPass::run(MachineFunction &MF,
                                    MachineFunctionAnalysisManager &MFAM)
```

**功能**: New Pass Manager 下贪婪寄存器分配的入口函数，构造 `RAGreedy` 实例并调用其 `run` 执行分配，最后根据是否发生改动返回相应的 `PreservedAnalyses`。

---

### 整体结构

```
RAGreedyPass::run(MF, MFAM)
├── MFPropsModifier _(*this, MF)
├── RAGreedy::RequiredAnalyses Analyses(MF, MFAM)
├── RAGreedy Impl(Analyses, Opts.Filter)
├── bool Changed = Impl.run(MF)
├── if (!Changed) return PreservedAnalyses::all()
├── PA = getMachineFunctionPassPreservedAnalyses()
├── PA.preserveSet<CFGAnalyses>() 与多条 PA.preserve<...>
└── return PA
```

---

### 逐段注释

**1. 设置函数属性并构造分配器实例 (行号 251-256)**

```cpp
MFPropsModifier _(*this, MF);

RAGreedy::RequiredAnalyses Analyses(MF, MFAM);
RAGreedy Impl(Analyses, Opts.Filter);

bool Changed = Impl.run(MF);
```

`MFPropsModifier` 在构造时记录并修改 `MachineFunction` 的属性（如清掉 `IsSSA`、要求 `NoPHIs`），析构时负责恢复，从而保证 Pass 退出后属性一致。随后从 `MFAM` 抓取全部依赖分析结果（封装在 `RequiredAnalyses`），构造 `RAGreedy` 实现对象并调用 `Impl.run(MF)` 执行真正的分配。

**2. 无改动时全量保留 (行号 257-258)**

```cpp
if (!Changed)
  return PreservedAnalyses::all();
```

若 `Impl.run` 返回 `false`（例如没有需要分配的虚拟寄存器，见 `hasVirtRegAlloc`），则所有分析都未失效，返回 `PreservedAnalyses::all()` 以让 pipeline 跳过后续无效化的工作。

**3. 声明保留的分析集合 (行号 259-268)**

```cpp
auto PA = getMachineFunctionPassPreservedAnalyses();
PA.preserveSet<CFGAnalyses>();
PA.preserve<MachineBlockFrequencyAnalysis>();
PA.preserve<LiveIntervalsAnalysis>();
PA.preserve<SlotIndexesAnalysis>();
PA.preserve<LiveDebugVariablesAnalysis>();
PA.preserve<LiveStacksAnalysis>();
PA.preserve<VirtRegMapAnalysis>();
PA.preserve<LiveRegMatrixAnalysis>();
return PA;
```

发生改动时，先取机器函数 Pass 的默认保留集，再显式保留 CFG 相关分析集和本 Pass 仍然保持有效的若干关键分析（LiveIntervals / SlotIndexes / VirtRegMap / LiveRegMatrix 等）。由于寄存器分配不改 CFG，但会重写指令操作数和栈帧，所以必须精确声明这些保留项，避免下游 Pass 重复计算。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedyPass::Opts` | `FilterName` / `Filter` | 寄存器过滤函数，决定哪些虚拟寄存器参与分配 |
| `MFPropsModifier` | （RAII） | 构造时设置/清除 `MachineFunctionProperties`，析构时还原 |
| `PreservedAnalyses` | （位集） | 标记哪些分析结果在 Pass 运行后仍然有效 |

---

### 优化意图

1. 通过 `MFPropsModifier` RAII 保证 Pass 异常或提前返回时 `MachineFunctionProperties` 仍能正确恢复。
2. 区分“无改动”和“有改动”两条返回路径，前者返回 `all()` 让 Pass pipeline 短路掉无效化开销。
3. 精细列出保留分析，避免下游对 `LiveIntervals`、`SlotIndexes` 等昂贵分析的重算。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须先构造 `RequiredAnalyses` 再构造 `RAGreedy` | `RAGreedy` 构造函数直接拷贝指针 | 顺序颠倒会读到空指针 |
| `preserveSet<CFGAnalyses>()` 必须显式调用 | 默认 `getMachineFunctionPassPreservedAnalyses` 不一定包含 CFG 集 | 下游 CFG 分析被误判失效 |
| `Opts.Filter` 透传给 `RAGreedy` | 过滤函数决定分配范围 | 误传空过滤可能导致全部分配或漏分配 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| New PM 入口 | `RAGreedyPass::run` | RegAllocGreedy.cpp:249 |
| 依赖分析封装 | `RAGreedy::RequiredAnalyses::RequiredAnalyses(MF, MFAM)` | RegAllocGreedy.cpp:230 |
| 实现入口 | `RAGreedy::run` | RegAllocGreedy.cpp:2934 |
| 属性 RAII | `MFPropsModifier` | llvm/include/llvm/CodeGen/MachineFunctionPass.h |

---

### 其他补充

`RAGreedyPass` 是 New PM 下的薄壳，真正逻辑在 `RAGreedy` 中；与之对应的 Legacy PM 入口为 `RAGreedyLegacy::runOnMachineFunction`（行 290）。

---

## RAGreedy::run 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::run(MachineFunction &mf)
```

**功能**: 贪婪寄存器分配的主驱动函数。设置上下文、初始化各种代价/Advisor/Spiller，调用 `allocatePhysRegs` 执行分配主体，再做 hint recoloring 与后优化，最后释放内存并返回是否发生改动。

---

### 整体结构

```
RAGreedy::run(mf)
├── 设置 MF/TII，可选 verify
├── RegAllocBase::init(VRM, LIS, Matrix)
├── if (!hasVirtRegAlloc()) return false
├── Indexes->packIndexes()
├── initializeCSRCost()
├── 读取 RegCosts / RegClassPriorityTrumpsGlobalness / ReverseLocalAssignment
├── ExtraInfo.emplace()
├── 获取 EvictAdvisor / PriorityAdvisor
├── 构造 VRAI / SpillerInstance
├── VRAI->calculateSpillWeightsAndHints()
├── 构造 SA / SE（SplitAnalysis / SplitEditor）
├── IntfCache.init / GlobalCand.resize / SetOfBrokenHints.clear
├── allocatePhysRegs()                  // 分配主体
├── tryHintsRecoloring()                // 二次尝试满足 hint
├── 可选 verify + postOptimization()
├── reportStats()
├── releaseMemory()
└── return true
```

---

### 逐段注释

**1. 上下文初始化与校验 (行号 2934-2942)**

```cpp
LLVM_DEBUG(dbgs() << "********** GREEDY REGISTER ALLOCATION **********\n"
                  << "********** Function: " << mf.getName() << '\n');

MF = &mf;
TII = MF->getSubtarget().getInstrInfo();

if (VerifyEnabled)
  MF->verify(LIS, Indexes, "Before greedy register allocator", &errs());
```

保存 `MF` 与目标指令信息 `TII` 指针；若 `VerifyEnabled`（通常由 `-verify-regalloc` 打开）则在分配前对 `MachineFunction` 做一次带 `LIS`/`Indexes` 的一致性校验，便于尽早发现上游 Pass 损坏 IR。

**2. 基类初始化与早退 (行号 2944-2949)**

```cpp
RegAllocBase::init(*this->VRM, *this->LIS, *this->Matrix);

if (!hasVirtRegAlloc())
  return false;
```

`RegAllocBase::init` 把 `VRM/LIS/Matrix` 写入基类成员，供 `allocatePhysRegs` 等基类逻辑使用。`hasVirtRegAlloc()` 扫描所有虚拟寄存器，若没有任何需要分配的 vreg 则直接返回 `false`，调用方据此返回 `PreservedAnalyses::all()`。

**3. 索引打包与 CSR 代价 (行号 2951-2955)**

```cpp
Indexes->packIndexes();

initializeCSRCost();
```

`packIndexes()` 重新压缩 `SlotIndexes`，使后续 `getApproxInstrDistance` 得到稳定一致的指令距离估计（用于区域拆分代价计算）。`initializeCSRCost()` 计算被调用者保存寄存器首次使用代价（见单独分析），影响是否倾向使用 CSR。

**4. 读取目标相关开关与代价 (行号 2957-2965)**

```cpp
RegCosts = TRI->getRegisterCosts(*MF);
RegClassPriorityTrumpsGlobalness =
    GreedyRegClassPriorityTrumpsGlobalness.getNumOccurrences()
        ? GreedyRegClassPriorityTrumpsGlobalness
        : TRI->regClassPriorityTrumpsGlobalness(*MF);

ReverseLocalAssignment = GreedyReverseLocalAssignment.getNumOccurrences()
                              ? GreedyReverseLocalAssignment
                              : TRI->reverseLocalAssignment();
```

从 `TRI` 取每寄存器代价 `RegCosts`；两个启发式开关优先用命令行值，否则回退到目标平台 `TRI` 默认。`RegClassPriorityTrumpsGlobalness` 决定寄存器类 `AllocationPriority` 是否压过“是否全局”这一因素；`ReverseLocalAssignment` 反转局部 vreg 的分配顺序，让较短局部区间先分配以减少碎片。

**5. 构造 Advisor 与 Spiller (行号 2967-2974)**

```cpp
ExtraInfo.emplace();

EvictAdvisor = EvictProvider->getAdvisor(*MF, *this, MBFI, Loops);
PriorityAdvisor = PriorityProvider->getAdvisor(*MF, *this, *Indexes);

VRAI = std::make_unique<VirtRegAuxInfo>(*MF, *LIS, *VRM, *Loops, *MBFI);
SpillerInstance.reset(createInlineSpiller({*LIS, *LSS, *DomTree, *MBFI}, *MF,
                                          *VRM, *VRAI, Matrix));
```

`ExtraInfo` 是可选附加信息结构（用 `optional` 惰性构造）。通过 Provider 取具体 Evict/Priority Advisor（默认或 ML 版）。构造 `VirtRegAuxInfo` 用于计算 spill 权重与 hint，再用工厂 `createInlineSpiller` 创建 `InlineSpiller` 实例。

**6. 计算 spill 权重并构造 SplitKit (行号 2976-2985)**

```cpp
VRAI->calculateSpillWeightsAndHints();

LLVM_DEBUG(LIS->dump());

SA.reset(new SplitAnalysis(*VRM, *LIS, *Loops));
SE.reset(new SplitEditor(*SA, *LIS, *VRM, *DomTree, *MBFI, *VRAI));

IntfCache.init(MF, Matrix->getLiveUnions(), Indexes, LIS, TRI);
GlobalCand.resize(32);
SetOfBrokenHints.clear();
```

`calculateSpillWeightsAndHints` 给每个 vreg 计算 spill 权重和分配 hint，是后续优先级排序的关键输入。`SplitAnalysis` / `SplitEditor` 是区域拆分的核心工具；`IntfCache` 缓存活跃区间并查；`GlobalCand` 预分配 32 项供全局拆分候选使用，`SetOfBrokenHints` 记录 hint 被破坏的区间集合。

**7. 分配主体与后处理 (行号 2987-2996)**

```cpp
allocatePhysRegs();
tryHintsRecoloring();

if (VerifyEnabled)
  MF->verify(LIS, Indexes, "Before post optimization", &errs());
postOptimization();
reportStats();

releaseMemory();
return true;
```

`allocatePhysRegs`（基类提供）是真正的分配循环：按优先级遍历 vreg，调用 `selectOrSplit` 选物理寄存器或拆分/溢出。`tryHintsRecoloring` 再尝试满足先前被破坏的 hint。后处理包括可选 verify、`postOptimization`（如消除冗余 COPY）、统计上报、释放本 Pass 持有内存，最后返回 `true` 表示发生了改动。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedy` | `MF/TII` | 当前函数与目标指令信息 |
| `RAGreedy` | `RegCosts` | 每寄存器代价数组 |
| `RAGreedy` | `EvictAdvisor/PriorityAdvisor` | 驱逐与优先级策略对象 |
| `RAGreedy` | `VRAI/SpillerInstance/SA/SE` | spill 权重、spiller、拆分分析/编辑器 |
| `RAGreedy` | `IntfCache/GlobalCand/SetOfBrokenHints` | 干扰缓存、全局拆分候选、破坏 hint 集合 |
| `RAGreedy` | `ExtraInfo` | 可选附加信息（`optional`） |

---

### 优化意图

1. 早退：无 vreg 时直接返回，避免无谓初始化。
2. `packIndexes` 保证距离估计稳定，提升拆分决策质量。
3. 把目标相关启发式开关集中在开头读取，便于按平台调优。
4. Advisor 与 Spiller 都走工厂/Provider，支持策略替换与 ML 实验。
5. 在分配前后做 `verify`，配合 `VerifyEnabled` 在调试构建中及时发现问题。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须先 `RegAllocBase::init` 再 `allocatePhysRegs` | 基类成员未初始化 | 崩溃 |
| `hasVirtRegAlloc` 早退必须在 `packIndexes` 之前 | 否则做了无用压缩 | 性能浪费 |
| `VRAI->calculateSpillWeightsAndHints` 必须在分配前调用 | 优先级依赖权重 | 分配质量退化 |
| `releaseMemory` 必须在最后调用 | 清空本 Pass 缓存 | 多次 run 内存泄漏 |
| `ExtraInfo.emplace()` 之后才能使用 `ExtraInfo` | 它是 `optional` | 未 emplace 解引用会崩溃 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 分配主循环 | `RegAllocBase::allocatePhysRegs` | llvm/lib/CodeGen/RegAllocBase.cpp |
| spill 权重 | `VirtRegAuxInfo::calculateSpillWeightsAndHints` | llvm/lib/CodeGen/CalcSpillWeights.cpp |
| 拆分工具 | `SplitAnalysis` / `SplitEditor` | llvm/lib/CodeGen/SplitKit.cpp |
| Spiller 工厂 | `createInlineSpiller` | llvm/lib/CodeGen/Spiller.cpp |
| hint 重染色 | `RAGreedy::tryHintsRecoloring` | RegAllocGreedy.cpp（同文件） |

---

### 其他补充

`run` 是 `RAGreedyPass::run` 与 `RAGreedyLegacy::runOnMachineFunction` 的共同核心，二者都只做薄壳包装后调用本函数。

---

## RAGreedy::hasVirtRegAlloc 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::hasVirtRegAlloc()
```

**功能**: 遍历当前函数的全部虚拟寄存器，判断是否存在“非 debug 且未被过滤掉”的 vreg 需要参与分配，作为 `run` 的早退判定条件。

---

### 整体结构

```
hasVirtRegAlloc()
├── for 每个虚拟寄存器 Reg
│   ├── if MRI->reg_nodbg_empty(Reg): continue
│   └── if shouldAllocateRegister(Reg): return true
└── return false
```

---

### 逐段注释

**1. 遍历虚拟寄存器 (行号 2922-2932)**

```cpp
bool RAGreedy::hasVirtRegAlloc() {
  for (unsigned I = 0, E = MRI->getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    if (shouldAllocateRegister(Reg))
      return true;
  }

  return false;
}
```

按索引 `[0, NumVirtRegs)` 逐个还原成 `Register`。先用 `reg_nodbg_empty` 跳过“仅被 debug 指令使用或完全没使用”的 vreg——这些不影响代码生成，不需要分配。再调用基类的 `shouldAllocateRegister(Reg)`（受 `RegAllocFilterFunc` 控制）确认该寄存器是否属于本 Pass 应处理范围。一旦命中即返回 `true`；全部遍历未命中返回 `false`。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MachineRegisterInfo` | `getNumVirtRegs()` | 当前函数虚拟寄存器总数 |
| `Register` | `index2VirtReg(I)` | 把线性索引还原为 vreg 编号 |
| `RegAllocBase` | `shouldAllocateRegister(Reg)` | 过滤函数包装，决定是否参与分配 |

---

### 优化意图

1. 跳过纯 debug 使用，避免为不产生代码的 vreg 启动完整分配 pipeline。
2. 尊重 `RegAllocFilterFunc`，允许多个分配器协同（如只分配某寄存器类）。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须用 `reg_nodbg_empty` 而非 `reg_empty` | debug-only 使用不算活跃 | 误判会导致无谓分配 |
| `shouldAllocateRegister` 依赖 `Filter` | `Filter` 为空时全部分配 | 过滤函数配置错误会漏分配 |
| `MRI` 必须已初始化 | 由基类 `init` 间接设置 | 在 `run` 中调用顺序正确即可 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| vreg 数量 | `MachineRegisterInfo::getNumVirtRegs` | llvm/include/llvm/CodeGen/MachineRegisterInfo.h |
| 过滤判定 | `RegAllocBase::shouldAllocateRegister` | llvm/lib/CodeGen/RegAllocBase.cpp |

---

### 其他补充

返回 `false` 时 `run` 直接 `return false`，进而使 `RAGreedyPass::run` 返回 `PreservedAnalyses::all()`，是常见的快路径。

---

## RAGreedy::initializeCSRCost 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::initializeCSRCost()
```

**功能**: 计算并设置被调用者保存寄存器（CSR）的“首次使用代价”`CSRCost`，用于在分配时权衡是否使用 CSR（使用 CSR 需要在 prologue/epilogue 保存恢复）。支持两种命令行开关的兼容计算，并对块频率做归一化。

---

### 整体结构

```
initializeCSRCost()
├── if 未显式设 CSRCostScale 且 (显式设 CSRFirstTimeCost 或 TRI->getCSRCost() 非零)
│   ├── CSRCost = 显式 ? CSRFirstTimeCost : max(CSRFirstTimeCost, TRI->getCSRCost())
│   ├── if CSRCost == 0: return
│   ├── 取 ActualEntry = MBFI->getEntryFreq()
│   ├── if ActualEntry == 0: CSRCost = 0; return
│   └── 按 ActualEntry vs 1<<14 缩放 CSRCost
└── else (新路径)
    ├── CSRCost = TRI->getCSRFirstUseCost() * EntryFreq
    └── 按 CSRCostScale 相对 100 做 *= 或 /=
```

---

### 逐段注释

**1. 旧开关分支：未显式设 CSRCostScale 时 (行号 2420-2432)**

```cpp
if (!CSRCostScale.getNumOccurrences() &&
    (CSRFirstTimeCost.getNumOccurrences() || TRI->getCSRCost())) {
  CSRCost = BlockFrequency(
      CSRFirstTimeCost.getNumOccurrences()
          ? CSRFirstTimeCost
          : std::max((unsigned)CSRFirstTimeCost, TRI->getCSRCost()));
  if (!CSRCost.getFrequency())
    return;
```

进入条件：用户没有显式设 `-regalloc-csr-cost-scale`，但设了旧开关 `-regalloc-csr-first-time-cost` 或目标 `TRI->getCSRCost()` 返回非零。`CSRCost` 取值优先用显式命令行值，否则取命令行默认（0）与 `TRI` 报告值的较大者。若算出来是 0 则直接返回，表示不考虑 CSR 代价。

**2. 旧路径的块频率归一化 (行号 2433-2449)**

```cpp
uint64_t ActualEntry = MBFI->getEntryFreq().getFrequency();
if (!ActualEntry) {
  CSRCost = BlockFrequency(0);
  return;
}
uint64_t FixedEntry = 1 << 14;
if (ActualEntry < FixedEntry) {
  CSRCost *= BranchProbability(ActualEntry, FixedEntry);
} else if (ActualEntry <= UINT32_MAX) {
  CSRCost /= BranchProbability(FixedEntry, ActualEntry);
} else {
  CSRCost =
      BlockFrequency(CSRCost.getFrequency() * (ActualEntry / FixedEntry));
}
```

`CSRFirstTimeCost` 的原始代价以“Entry == 2^14”为基准，需要按当前函数真实 entry 频率缩放。三种情况：实际 entry 小于基准用乘法概率；不大于 32 位上限用除法（反转分子分母避免溢出）；超过 32 位时 `BranchProbability` 无法表达，直接用整数乘除。这保证 `CSRCost` 与函数热度成正比，热度高的函数使用 CSR 代价更高。

**3. 新路径：基于 TRI 的首次使用代价 (行号 2450-2457)**

```cpp
} else {
  uint64_t EntryFreq = MBFI->getEntryFreq().getFrequency();
  CSRCost = BlockFrequency(TRI->getCSRFirstUseCost() * EntryFreq);
  if (CSRCostScale < 100)
    CSRCost *= BranchProbability(CSRCostScale, 100);
  else
    CSRCost /= BranchProbability(100, CSRCostScale);
}
```

新路径直接用 `TRI->getCSRFirstUseCost()`（目标平台给出的首次使用代价）乘 entry 频率，得到与函数热度成比例的代价。再用 `CSRCostScale`（默认 80）相对 100 做缩放：小于 100 用乘法概率，大于等于 100 用除法（等价于乘 `CSRCostScale/100`）。这样旧命令行开关未被设时走更现代的目标驱动代价模型。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedy` | `CSRCost` | `BlockFrequency` 类型，CSR 首次使用代价 |
| `cl::opt<unsigned>` | `CSRFirstTimeCost` | 旧开关，显式指定 CSR 首次使用代价 |
| `cl::opt<unsigned>` | `CSRCostScale` | 新开关，CSR 代价缩放百分比，默认 80 |
| `TargetRegisterInfo` | `getCSRCost()` / `getCSRFirstUseCost()` | 目标平台报告的 CSR 代价 |
| `MachineBlockFrequencyInfo` | `getEntryFreq()` | 函数 entry 块频率，用于归一化 |

---

### 优化意图

1. 让 CSR 代价与函数热度挂钩：热度高的函数 prologue/epilogue 执行更频繁，使用 CSR 代价更大，分配器更倾向保留 caller-saved 寄存器。
2. 提供新旧两套开关，兼容历史命令行用法同时推荐目标驱动代价。
3. 用 `BranchProbability` 乘除避免 64 位溢出，分三档处理保证数值稳定。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 旧开关 `CSRFirstTimeCost` 即将废弃 | 注释明确标注 | 未来版本可能删除，迁移到 `CSRCostScale` |
| `ActualEntry > UINT32_MAX` 分支必须存在 | `BranchProbability` 只接受 32 位 | 否则大函数会触发 UB |
| `MBFI` 必须有效 | 由构造时注入 | 若为空崩溃 |
| `CSRCostScale == 100` 时走除法分支 | `BranchProbability(100,100)==1` | 结果不变，正确 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 目标 CSR 代价 | `TargetRegisterInfo::getCSRCost` / `getCSRFirstUseCost` | llvm/include/llvm/CodeGen/TargetRegisterInfo.h |
| 块频率 | `MachineBlockFrequencyInfo::getEntryFreq` | llvm/lib/CodeGen/MachineBlockFrequencyInfo.cpp |
| 概率乘除 | `BlockFrequency::operator*=` / `operator/=` with `BranchProbability` | llvm/lib/Support/BlockFrequency.cpp |

---

### 其他补充

`CSRCost` 在 `selectOrSplit` 等路径中作为“是否接受 CSR”的阈值，若区域拆分代价高于 `CSRCost` 则倾向直接使用 CSR（见行 2400-2410 的 `tryRegionSplit` 逻辑）。
<!-- Group B: Queue & Core Strategy Functions -->

## RAGreedy::enqueue 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::enqueue(PQueue &CurQueue, const LiveInterval *LI)
```

**功能**: 将虚拟寄存器对应的 LiveInterval 加入优先级队列，根据 PriorityAdvisor 计算优先级，并将新进入的 vreg 的 Stage 从 `RS_New` 推进到 `RS_Assign`。

---

### 整体结构

```
enqueue(CurQueue, LI)
├── 取出 vreg 编号并断言为虚拟寄存器
├── 读取 Stage，若为 RS_New 则改写为 RS_Assign
├── 调用 PriorityAdvisor->getPriority(*LI) 计算优先级 Ret
└── CurQueue.push({Ret, ~Reg.id()})   // 取反使小编号优先
```

---

### 逐段注释

**1. 参数校验与 Stage 推进 (行 427-434)**

```cpp
const Register Reg = LI->reg();
assert(Reg.isVirtual() && "Can only enqueue virtual registers");

auto Stage = ExtraInfo->getOrInitStage(Reg);
if (Stage == RS_New) {
  Stage = RS_Assign;
  ExtraInfo->setStage(Reg, Stage);
}
```

确保只对虚拟寄存器入队；首次入队的 vreg 通过 `getOrInitStage` 取出（必要时扩容 Info 表），若仍是初始态 `RS_New` 则升级到 `RS_Assign`，使其进入主分配流程。

**2. 计算优先级并压栈 (行 436-440)**

```cpp
unsigned Ret = PriorityAdvisor->getPriority(*LI);

CurQueue.push(std::make_pair(Ret, ~Reg.id()));
```

调用 advisor 获得位编码优先级 `Ret`；优先级队列以 `pair<unsigned,unsigned>` 为元素，默认大顶堆。第二个元素存 `~Reg.id()`，是为了在优先级相等时让 vreg 编号小者优先（取反后大的原始值小）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `PQueue` | `priority_queue<pair<unsigned,unsigned>>` | 优先级队列，first=优先级，second=~vreg id |
| `ExtraRegInfo::RegInfo` | `Stage`, `Cascade` | 每个 vreg 的分配阶段与驱逐环路防护编号 |
| `LiveRangeStage` | `RS_New/RS_Assign/...` | 表示 vreg 在 greedy 流程中的当前阶段 |

---

### 优化意图

1. 通过 `~Reg.id()` 作 tie-breaker，保证相同优先级时小编号先分配，使行为确定化。
2. `RS_New→RS_Assign` 自动跃迁保证入队即进入主分配，无需调用方关心 Stage。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须为虚拟寄存器 | assert 检查 `isVirtual()` | 误传 physreg 会触发断言失败 |
| `ExtraInfo` 必须已初始化 | `getOrInitStage` 会 grow | 若 ExtraInfo 为 nullopt 则 UB |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 优先级计算 | `RegAllocPriorityAdvisor::getPriority` | RegAllocGreedy.cpp:443 |
| Stage 读写 | `ExtraRegInfo::getOrInitStage/setStage` | RegAllocGreedy.h:102/91 |

---

### 其他补充

`enqueueImpl(Queue, LI)` 是基类回调的 thin wrapper（行 422），直接转发给本函数，使用默认队列 `Queue`。

---

## DefaultPriorityAdvisor::getPriority 函数分析

### 函数签名与目的（行号）
```cpp
unsigned DefaultPriorityAdvisor::getPriority(const LiveInterval &LI) const
```

**功能**: 为 LiveInterval 计算一个 32 位整数优先级，编码了 Stage、局部/全局、寄存器类 AllocationPriority、Size/指令距离以及 hint 偏好等多个维度，供 `enqueue` 使用。

---

### 整体结构

```
getPriority(LI)
├── 取 Size、Reg、Stage
├── if Stage == RS_Split:
│   └── Prio = Size                          // 已分裂过未分成功的延后处理
├── else:
│   ├── 计算 ForceGlobal（巨型 range 强制全局）
│   ├── if 局部 + 单 MBB + !ForceGlobal:
│   │   └── Prio = 指令距离（正向或反向）
│   └── else: Prio = Size; GlobalBit = 1
│   ├── Prio = min(Prio, 2^24-1)
│   ├── 按 RegClassPriorityTrumpsGlobalness 设置 GlobalBit/AllocPriority 位
│   ├── Prio |= (1u<<31)                      // 高于 RS_Split
│   └── if hasKnownPreference: Prio |= (1u<<30)
└── return Prio
```

---

### 逐段注释

**1. 分支 RS_Split：已分裂但未分成功的延后 (行 449-452)**

```cpp
if (Stage == RS_Split) {
  Prio = Size;
}
```

`RS_Split` 表示已经尝试过分裂但尚未分配成功，这些 range 直接用 Size 作优先级，且不设置 bit31，因此会被所有 `RS_Assign` 阶段的 range 压在后面，等其它分配完后再处理。

**2. ForceGlobal 判定 (行 456-460)**

```cpp
const TargetRegisterClass &RC = *MRI->getRegClass(Reg);
bool ForceGlobal = RC.GlobalPriority ||
                   (!ReverseLocalAssignment &&
                    (Size / SlotIndex::InstrDist) >
                        (2 * RegClassInfo.getNumAllocatableRegs(&RC)));
```

如果寄存器类显式要求全局优先（`GlobalPriority`），或者 range 长度超过可分配寄存器数的 2 倍（按 InstrDist 归一化），则强制走全局分配策略，避免超大 range 把局部小 range 挤去 spill。

**3. 局部分配优先级（线性指令序） (行 463-475)**

```cpp
if (Stage == RS_Assign && !ForceGlobal && !LI.empty() &&
    LIS->intervalIsInOneMBB(LI)) {
  if (!ReverseLocalAssignment)
    Prio = LI.beginIndex().getApproxInstrDistance(Indexes->getLastIndex());
  else {
    Prio = Indexes->getZeroIndex().getApproxInstrDistance(LI.endIndex());
  }
}
```

仅 `RS_Assign` 阶段、非强制全局、非空、且局限于单个 MBB 的 range 走局部策略。默认正向：beginIndex 离末尾越远优先级越高，即先分配靠前的局部 range，模拟线性扫描最优着色。反向模式则从尾部开始，便于在超大 MBB 中先抢便宜寄存器。

**4. 全局/分裂 range 的长→短策略 (行 476-482)**

```cpp
else {
  Prio = Size;
  GlobalBit = 1;
}
```

全局或已分裂 range 按 Size 大→小排：大 range 先分配，若装不下应尽早 spill/split，避免后期产生大量 interference。

**5. 优先级位编码 (行 496-509)**

```cpp
Prio = std::min(Prio, (unsigned)maxUIntN(24));
assert(isUInt<5>(RC.AllocationPriority) && "allocation priority overflow");

if (RegClassPriorityTrumpsGlobalness)
  Prio |= RC.AllocationPriority << 25 | GlobalBit << 24;
else
  Prio |= GlobalBit << 29 | RC.AllocationPriority << 24;

Prio |= (1u << 31);

if (VRM->hasKnownPreference(Reg))
  Prio |= (1u << 30);
```

位布局：bit31=主优先级（高于 RS_Split），bit30=有 hint 偏好，bit29/24=GlobalBit（按策略位置不同），bit28-25 或 bit28-24=AllocationPriority（5 位），bit0-23=Size 或指令距离。`maxUIntN(24)` 把 Size 截断到 24 位以防溢出。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| 优先级位编码 | bit31/bit30/bit29或24/bit25-28/bit0-23 | Stage/hint/global/allocPrio/size |
| `TargetRegisterClass` | `GlobalPriority`, `AllocationPriority` | 控制全局优先与分配优先级 |
| `SlotIndex::InstrDist` | 静态常量 | 指令距离归一化单位 |

---

### 优化意图

1. **大 range 先处理**：尽早 spill/split 大 range，避免后期堆积 interference。
2. **局部线性序**：单 MBB 的局部 range 按指令序分配，无全局干扰时可达最优着色。
3. **AllocationPriority 位**：让高代价寄存器类的 range 优先抢好寄存器。
4. **hint 位**：有已知偏好（如 copy coalescing 提示）的 range 优先处理。
5. **bit31 区分 Stage**：保证 `RS_Assign` 全部先于 `RS_Split` 处理。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `AllocationPriority` 必须 5 位内 | assert 校验 | 越界则位编码错乱 |
| `Prio` 须 ≤24 位 | 用 `maxUIntN(24)` 截断 | 否则会覆盖高位字段 |
| `RegClassPriorityTrumpsGlobalness` 影响位布局 | 模式选择 | 改模式需同步调整掩码逻辑 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单 MBB 检测 | `LiveIntervals::intervalIsInOneMBB` | LiveIntervals.h |
| 指令距离 | `SlotIndex::getApproxInstrDistance` | SlotIndexes |
| 寄存器类查询 | `MRI->getRegClass` | MachineRegisterInfo |
| hint 检测 | `VirtRegMap::hasKnownPreference` | VirtRegMap.h |

---

### 其他补充

`DummyPriorityAdvisor::getPriority`（行 515）只返回 `~Reg.virtRegIndex()`，是单元测试/兜底实现，不含任何启发式。

---

## RAGreedy::dequeue(PQueue&) 函数分析

### 函数签名与目的（行号）
```cpp
const LiveInterval *RAGreedy::dequeue(PQueue &CurQueue)
```

**功能**: 从优先级队列弹出优先级最高的 LiveInterval，并通过 `~` 反转恢复原始 vreg id 查询 LIS 得到 LiveInterval 指针。

---

### 整体结构

```
dequeue(CurQueue)
├── if CurQueue.empty(): return nullptr
├── LI = &LIS->getInterval(~CurQueue.top().second)
├── CurQueue.pop()
└── return LI
```

---

### 逐段注释

**1. 空队与取顶 (行 524-528)**

```cpp
if (CurQueue.empty())
  return nullptr;
LiveInterval *LI = &LIS->getInterval(~CurQueue.top().second);
CurQueue.pop();
return LI;
```

空队返回 nullptr 表示本轮分配完成。`top().second` 存的是 `~Reg.id()`（见 `enqueue`），用 `~` 还原得到原 vreg id，再通过 `LIS->getInterval` 转换为 LiveInterval 引用。先取后 pop 保证 top 在 pop 前已被使用。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `PQueue::value_type` | `pair<unsigned,unsigned>` | second 存 `~vreg id` |
| `LiveIntervals` | `getInterval(Register)` | 由 vreg 取 LiveInterval |

---

### 优化意图

1. `~` 双向取反保证入队/出队对称，无需额外存储。
2. 仅返回指针/引用，不拷贝 LiveInterval，零开销。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `top()` 必须在 `pop()` 前使用 | 引用语义 | 先 pop 再访问会悬空 |
| vreg id 必须 ≤ `~0U` 取反可逆 | unsigned 范围 | 与 enqueue 必须一致 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 顶层出队 | `RAGreedy::dequeue()` | RegAllocGreedy.cpp:521 |
| 反查 LiveInterval | `LiveIntervals::getInterval` | LiveIntervals.h |

---

### 其他补充

`dequeue()`（行 521）是对默认 `Queue` 的转发包装，是 `RegAllocBase` 调用的入口。

---

## RAGreedy::selectOrSplit 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::selectOrSplit(const LiveInterval &VirtReg,
                                   SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 寄存器分配的对外入口，调用核心策略 `selectOrSplitImpl`，并处理 recoloring cutoff 失败时的错误上报。

---

### 整体结构

```
selectOrSplit(VirtReg, NewVRegs)
├── CutOffInfo = CO_None                      // 复位 cutoff 标记
├── 准备局部 FixedRegisters、RecolorStack
├── Reg = selectOrSplitImpl(VirtReg, NewVRegs, FixedRegisters, RecolorStack)
├── if Reg == ~0U 且 CutOffInfo != CO_None:
│   └── 按 CutOffInfo 位掩码 emitError
└── return Reg
```

---

### 逐段注释

**1. 局部状态初始化 (行 2327-2332)**

```cpp
CutOffInfo = CO_None;
LLVMContext &Ctx = MF->getFunction().getContext();
SmallVirtRegSet FixedRegisters;
RecoloringStack RecolorStack;
MCRegister Reg =
    selectOrSplitImpl(VirtReg, NewVRegs, FixedRegisters, RecolorStack);
```

每次顶层调用前清空 `CutOffInfo`，确保上一次失败状态不污染本次。`FixedRegisters` 与 `RecolorStack` 是本次调用局部容器，分别记录已固定物理寄存器集合和 recolor 回滚栈。

**2. Cutoff 错误上报 (行 2333-2346)**

```cpp
if (Reg == ~0U && (CutOffInfo != CO_None)) {
  uint8_t CutOffEncountered = CutOffInfo & (CO_Depth | CO_Interf);
  if (CutOffEncountered == CO_Depth)
    Ctx.emitError("register allocation failed: maximum depth for recoloring "
                  "reached. Use -fexhaustive-register-search to skip "
                  "cutoffs");
  else if (CutOffEncountered == CO_Interf)
    Ctx.emitError("register allocation failed: maximum interference for "
                  "recoloring reached. Use -fexhaustive-register-search "
                  "to skip cutoffs");
  else if (CutOffEncountered == (CO_Depth | CO_Interf))
    Ctx.emitError("register allocation failed: maximum interference and "
                  "depth for recoloring reached. Use "
                  "-fexhaustive-register-search to skip cutoffs");
}
```

`~0U` 是 `tryLastChanceRecoloring` 在 cutoff 时返回的特殊值。`CutOffInfo` 是位掩码，可能同时包含 depth 与 interf 两种原因。`emitError` 直接报编译错误（非 fatal），提示用户加 `-fexhaustive-register-search` 跳过 cutoff。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `CutOffStage` | `CO_None/CO_Depth/CO_Interf` | bitmask，标记 recoloring 失败原因 |
| `SmallVirtRegSet` | - | `SmallPtrSet<Register,16>` 限定本轮固定 vreg |
| `RecoloringStack` | `SmallVector<pair<const LiveInterval*, MCRegister>,8>` | recolor 回滚栈 |

---

### 优化意图

1. 集中处理 cutoff 错误，让 `selectOrSplitImpl` 不必关心诊断逻辑。
2. 局部容器避免跨调用残留状态。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `~0U` 仅在 cutoff 场景返回 | 否则会被当作合法 physreg | `tryLastChanceRecoloring` 必须正确返回 |
| `CutOffInfo` 是 bitmask | 用 `&` 测试 | 不能用 `==` 直接判断混合情况 |
| `emitError` 不终止编译 | 后续仍会返回 Reg | 调用方需容错 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 核心策略 | `RAGreedy::selectOrSplitImpl` | RegAllocGreedy.cpp:2647 |
| 错误上报 | `LLVMContext::emitError` | llvm/IR/LLVMContext.h |

---

### 其他补充

`-fexhaustive-register-search` 是前端 flag，最终控制 `lcr-max-depth`/`lcr-max-interf` 限制是否生效。

---

## RAGreedy::selectOrSplitImpl 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::selectOrSplitImpl(const LiveInterval &VirtReg,
                                       SmallVectorImpl<Register> &NewVRegs,
                                       SmallVirtRegSet &FixedRegisters,
                                       RecoloringStack &RecolorStack,
                                       unsigned Depth)
```

**功能**: greedy 寄存器分配的核心策略函数，按 `tryAssign → tryEvict → wait → trySplit → tryLastChanceRecoloring → spill` 的顺序尝试为 VirtReg 找到物理寄存器或产生分裂/spill 子任务。

---

### 整体结构

```
selectOrSplitImpl(VirtReg, NewVRegs, FixedRegisters, RecolorStack, Depth)
├── 构造 AllocationOrder，CostPerUseLimit = ~0
├── [策略1 tryAssign]
│   ├── if tryAssign 成功:
│   │   ├── if 是 unused CSR 且 NewVRegs 空:
│   │   │   └── tryAssignCSRFirstTime → 可能 spill/split
│   │   └── else return PhysReg
│   └── if NewVRegs 非空: return MCRegister()         // 已 split
├── [策略2 tryEvict]   if Stage != RS_Split:
│   ├── if 成功 → 记录 broken hint → return PhysReg
├── [策略3 wait]   if Stage < RS_Split:
│   ├── setStage(RS_Split)，push VirtReg 到 NewVRegs
│   └── return MCRegister()                            // 等下一轮
├── [策略4 trySplit]   if Stage < RS_Spill && !empty:
│   └── if 成功 or 新增 vreg → return PhysReg
├── [策略5 last chance recolor]   if Stage >= RS_Done || !spillable:
│   └── return tryLastChanceRecoloring(...)
└── [策略6 spill]
    ├── LiveRangeEdit + spiller().spill
    ├── setStage(NewVRegs, RS_Done)
    ├── 更新 LiveDebugVariables
    └── return MCRegister()
```

---

### 逐段注释

**1. 构造分配序号与尝试直接分配 (行 2652-2674)**

```cpp
uint8_t CostPerUseLimit = uint8_t(~0u);
auto Order =
    AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);
if (MCRegister PhysReg =
        tryAssign(VirtReg, Order, NewVRegs, FixedRegisters)) {
  if (CSRCost.getFrequency() &&
      EvictAdvisor->isUnusedCalleeSavedReg(PhysReg) && NewVRegs.empty()) {
    MCRegister CSRReg = tryAssignCSRFirstTime(VirtReg, Order, PhysReg,
                                              CostPerUseLimit, NewVRegs);
    if (CSRReg || !NewVRegs.empty())
      return CSRReg;
  } else
    return PhysReg;
}
if (!NewVRegs.empty())
  return MCRegister();
```

`CostPerUseLimit` 初始为最大值表示无限制。`AllocationOrder::create` 按寄存器类和 hint 生成物理寄存器迭代序。`tryAssign` 若返回非空表示找到空闲寄存器；若该寄存器是首次使用的 CSR，则交由 `tryAssignCSRFirstTime` 评估是否值得 spill/pre-split 而不引入 prologue/epilogue 开销。若 NewVRegs 已被填充（如 hint split）则直接返回，让外层把新 vreg 重新入队。

**2. 驱逐低优先级 interference (行 2676-2697)**

```cpp
LiveRangeStage Stage = ExtraInfo->getStage(VirtReg);
LLVM_DEBUG(dbgs() << StageName[Stage] << " Cascade "
                  << ExtraInfo->getCascade(VirtReg.reg()) << '\n');

if (Stage != RS_Split) {
  if (MCRegister PhysReg =
          tryEvict(VirtReg, Order, NewVRegs, CostPerUseLimit,
                   FixedRegisters)) {
    Register Hint = MRI->getSimpleHint(VirtReg.reg());
    if (Hint && Hint != PhysReg)
      SetOfBrokenHints.insert(&VirtReg);
    return PhysReg;
  }
}
```

`RS_Split` 阶段跳过 evict，因为已经试过且失败，再试可能形成环路。驱逐成功后若 hint 与实际分配不一致，则记入 `SetOfBrokenHints`，留待 `tryHintRecoloring` 后处理。

**3. 等待第二轮 (行 2699-2709)**

```cpp
assert((NewVRegs.empty() || Depth) && "Cannot append to existing NewVRegs");

if (Stage < RS_Split) {
  ExtraInfo->setStage(VirtReg, RS_Split);
  LLVM_DEBUG(dbgs() << "wait for second round\n");
  NewVRegs.push_back(VirtReg.reg());
  return MCRegister();
}
```

首次到达此处的 range 标为 `RS_Split` 并把自己 push 回 NewVRegs，让外层把它重新入队。等所有更小 range 都被处理后再回来，此时 interference 图景更完整，split 决策更优。assert 防止在非递归调用中向已有 NewVRegs 追加。

**4. 分裂 (行 2711-2717)**

```cpp
if (Stage < RS_Spill && !VirtReg.empty()) {
  unsigned NewVRegSizeBefore = NewVRegs.size();
  MCRegister PhysReg = trySplit(VirtReg, Order, NewVRegs, FixedRegisters);
  if (PhysReg || (NewVRegs.size() - NewVRegSizeBefore))
    return PhysReg;
}
```

`RS_Split` 阶段尝试 region split / block split / live range splitting。若 `trySplit` 返回非空 PhysReg 表示成功；若 NewVRegs 增加表示已生成新子 range，外层会重新入队。两者皆无则继续走 spill。

**5. 最后机会重染色 (行 2721-2724)**

```cpp
if (Stage >= RS_Done || !VirtReg.isSpillable()) {
  return tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters,
                                 RecolorStack, Depth);
}
```

已经过 spill 阶段（`RS_Done`）或不可 spill（如 tied-to-reg/inline asm 操作数）的 range，交给 `tryLastChanceRecoloring` 尝试递归重染色已分配的 range。该函数可能返回 `~0U` 触发 cutoff 错误。

**6. Spill (行 2727-2746)**

```cpp
NamedRegionTimer T("spill", "Spiller", TimerGroupName,
                   TimerGroupDescription, TimePassesIsEnabled);
LiveRangeEdit LRE(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
spiller().spill(LRE, &Order);
ExtraInfo->setStage(NewVRegs.begin(), NewVRegs.end(), RS_Done);

for (Register r : spiller().getSpilledRegs())
  DebugVars->splitRegister(r, LRE.regs(), *LIS);
for (Register r : spiller().getReplacedRegs())
  DebugVars->splitRegister(r, LRE.regs(), *LIS);

if (VerifyEnabled)
  MF->verify(LIS, Indexes, "After spilling", &errs());

return MCRegister();
```

构造 `LiveRangeEdit` 让 spiller 把 VirtReg 拆成若干子 range（部分 spill 到栈），新 vreg 标为 `RS_Done` 表示本轮不再处理。同步更新 `LiveDebugVariables` 中 debug 信息的寄存器映射，让 spilled/replaced 寄存器的 dbg 值能在后续 LDV 重写阶段正确分发到新 range。返回空表示本轮不分配，外层会把 NewVRegs 重新入队。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AllocationOrder` | 迭代序 | 按 hint/RC/RegClassInfo 排序的物理寄存器候选 |
| `LiveRangeEdit` | `regs()` | 记录 spill 产生的新 vreg 列表 |
| `SmallVirtRegSet` | - | 本轮 recolor 上下文中已固定的 vreg 集合 |
| `RecoloringStack` | - | recolor 尝试回滚栈 |
| `LiveRangeStage` | `RS_Assign/Split/Split2/Spill/Done` | 分配状态机 |

---

### 优化意图

1. **优先低成本策略**：assign → evict → split → spill，从无副作用到有副作用逐级升级。
2. **wait 机制**：让小 range 先分配，使 split/spill 决策基于更完整的 interference 信息。
3. **CSR first-time 评估**：权衡 prologue 开销与 spill/split 成本。
4. **`SetOfBrokenHints`**：延迟到 `tryHintRecoloring` 修复 broken hint，提升 copy coalescing 命中率。
5. **`RS_Split` 跳过 evict**：防止与已分裂 range 间形成驱逐环路。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| Stage 状态机严格单调 | RS_Assign→Split→Spill→Done | 跳跃会丢失优化机会 |
| `NewVRegs.empty() || Depth` | assert | 顶层不能向已非空 NewVRegs 追加 |
| `tryLastChanceRecoloring` 返回 `~0U` 仅表 cutoff | 与合法 physreg 区分 | selectOrSplit 依赖此约定 |
| spill 后必须更新 LDV | 否则 debug 信息错乱 | - |
| `RS_Split` 阶段不可再 evict | 防止环路 | - |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 直接分配 | `RAGreedy::tryAssign` | RegAllocGreedy.cpp:536 |
| 驱逐 | `RAGreedy::tryEvict` | RegAllocGreedy.cpp |
| CSR 评估 | `RAGreedy::tryAssignCSRFirstTime` | RegAllocGreedy.cpp:2381 |
| 分裂 | `RAGreedy::trySplit` | RegAllocGreedy.cpp |
| 重染色 | `RAGreedy::tryLastChanceRecoloring` | RegAllocGreedy.cpp |
| Spill | `Spiller::spill(LiveRangeEdit&, ...)` | RegAllocBase.h |

---

### 其他补充

`Depth` 参数用于递归 recolor 时的深度计数，配合 `lcr-max-depth` cutoff。`CutOffInfo` 在子函数中通过 `|=` 累积，最终由 `selectOrSplit` 统一上报。

---

## RAGreedy::tryAssign 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::tryAssign(const LiveInterval &VirtReg,
                               AllocationOrder &Order,
                               SmallVectorImpl<Register> &NewVRegs,
                               const SmallVirtRegSet &FixedRegisters)
```

**功能**: 遍历 AllocationOrder 寻找无干涉的物理寄存器，若发现 hint 偏好则优先返回；否则尝试驱逐 hint 干涉或更便宜的替代寄存器，是 greedy 分配的第一策略。

---

### 整体结构

```
tryAssign(VirtReg, Order, NewVRegs, FixedRegisters)
├── 遍历 Order 找无干涉寄存器
│   ├── isHint 命中 → 立即返回
│   └── 否则记录第一个可用 PhysReg
├── if 无可用 → return invalid
├── [missed hint 处理]
│   ├── if 有 simple hint 且在 Order 内:
│   │   ├── if canEvictHintInterference: evict + return PhysHint
│   │   ├── if trySplitAroundHintReg: return MCRegister()  // 已 split
│   │   └── 否则记入 SetOfBrokenHints
├── [更便宜替代]
│   ├── Cost = RegCosts[PhysReg]
│   ├── if !Cost: return PhysReg            // 0 成本，直接用
│   └── CheapReg = tryEvict(... Cost ...)
└── return CheapReg ? CheapReg : PhysReg
```

---

### 逐段注释

**1. 遍历 AllocationOrder 寻可用寄存器 (行 540-551)**

```cpp
MCRegister PhysReg;
for (auto I = Order.begin(), E = Order.end(); I != E && !PhysReg; ++I) {
  assert(*I);
  if (!Matrix->checkInterference(VirtReg, *I)) {
    if (I.isHint())
      return *I;
    else
      PhysReg = *I;
  }
}
if (!PhysReg.isValid())
  return PhysReg;
```

`Matrix->checkInterference` 查询 LiveIntervalUnion 中该 physreg 与 VirtReg 是否冲突。若命中 hint 寄存器（Order 中标记为 hint 的位置，通常来自 copy coalescing）则立即返回，无需考虑后续。否则记录第一个非 hint 可用寄存器，循环结束。若全空则返回 invalid。

**2. 错过的 hint 处理 (行 557-575)**

```cpp
if (Register Hint = MRI->getSimpleHint(VirtReg.reg()))
  if (Order.isHint(Hint)) {
    MCRegister PhysHint = Hint.asMCReg();
    LLVM_DEBUG(dbgs() << "missed hint " << printReg(PhysHint, TRI) << '\n');

    if (EvictAdvisor->canEvictHintInterference(VirtReg, PhysHint,
                                               FixedRegisters)) {
      evictInterference(VirtReg, PhysHint, NewVRegs);
      return PhysHint;
    }

    if (trySplitAroundHintReg(PhysHint, VirtReg, NewVRegs, Order))
      return MCRegister();

    SetOfBrokenHints.insert(&VirtReg);
  }
```

如果存在 simple hint（来自 `MRI`，一般是 copy 关系推导）但被其它 range 占用：
1. 先问 `EvictAdvisor` 能否驱逐 hint 寄存器上的 interference，能则驱逐并返回。
2. 否则尝试在 cold block 周围绕 hint 寄存器分裂 VirtReg，成功则返回空（NewVRegs 已填充，外层重新入队）。
3. 都失败则记入 `SetOfBrokenHints`，等 `tryHintRecoloring` 后处理。

**3. 更便宜的替代寄存器 (行 577-587)**

```cpp
uint8_t Cost = RegCosts[PhysReg.id()];

if (!Cost)
  return PhysReg;

LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI) << " is available at cost "
                  << (unsigned)Cost << '\n');
MCRegister CheapReg = tryEvict(VirtReg, Order, NewVRegs, Cost, FixedRegisters);
return CheapReg ? CheapReg : PhysReg;
```

`RegCosts` 编码每个 physreg 的额外使用成本（如 CSR 首次使用、调用约定需要保存等）。0 表示无额外成本，直接用。否则以当前 Cost 为上限调用 `tryEvict` 找更便宜的替代；找不到则退回原 PhysReg。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AllocationOrder` | 迭代器 + `isHint()` | 候选物理寄存器序，标记 hint 位置 |
| `LiveRegMatrix` | `checkInterference` | 查询 vreg 与 physreg 的干涉 |
| `RegCosts` | `SmallVector<uint8_t>` | 每个 physreg 的额外使用成本 |
| `SetOfBrokenHints` | `SmallSetVector<const LiveInterval*,4>` | hint 失败的 vreg 集合，供后处理 |

---

### 优化意图

1. **hint 优先于 size**：一旦遇到 hint 立即返回，最大化 copy coalescing 机会。
2. **驱逐 hint interference 优于 fallback**：相比使用非 hint 寄存器，驱逐可保留 hint。
3. **冷块绕 hint 分裂**：避免驱逐代价高时通过 split 在热块仍用 hint。
4. **成本感知替代**：用 `RegCosts` 防止选到昂贵 CSR。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `Order` 必须有效 | 来自 `AllocationOrder::create` | 失效会导致迭代 UB |
| `PhysReg` 命中 hint 后立即返回 | 不再考虑后续 | 设计如此，非 bug |
| `trySplitAroundHintReg` 成功后返回空 | NewVRegs 已填充 | 调用方需检查 NewVRegs |
| `RegCosts` 索引按 physreg id | 必须 size 充足 | 越界 UB |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 干涉查询 | `LiveRegMatrix::checkInterference` | LiveRegMatrix.h |
| Hint 查询 | `MachineRegisterInfo::getSimpleHint` | MachineRegisterInfo.h |
| Hint 驱逐可行性 | `RegAllocEvictionAdvisor::canEvictHintInterference` | RegAllocEvictionAdvisor.h |
| 替代寄存器驱逐 | `RAGreedy::tryEvict` | RegAllocGreedy.cpp |
| 围绕 hint 分裂 | `RAGreedy::trySplitAroundHintReg` | RegAllocGreedy.cpp |

---

### 其他补充

`tryAssign` 是 `selectOrSplitImpl` 第一步，无副作用（除 evict/split 副路径外），失败时返回 invalid 让外层进入 `tryEvict`/`trySplit`/spill 流程。`FixedRegisters` 参数仅在 hint 驱逐子路径中传给 advisor，避免驱逐已被本轮固定的 vreg。
<!-- Group C: Eviction & Hint Split Functions -->

## RegAllocEvictionAdvisor::canReassign 函数分析

### 函数签名与目的（行号）
```cpp
bool RegAllocEvictionAdvisor::canReassign(const LiveInterval &VirtReg,
                                          MCRegister FromReg) const
```

**功能**: 检查虚拟寄存器 VirtReg 是否可以从当前物理寄存器 FromReg 重新分配到 AllocationOrder 中的另一个无冲突物理寄存器，用于在 eviction 决策时寻找替代方案。

---

### 整体结构

```
canReassign(VirtReg, FromReg)
├── 定义 HasRegUnitInterference lambda：检查某 regunit 是否存在干扰
├── 遍历 AllocationOrder 中所有候选物理寄存器
│   ├── 跳过 FromReg
│   └── 若所有 regunits 均无干扰则返回 true
└── return false
```

---

### 逐段注释

**1. 定义 regunit 干扰查询 lambda (行 596-601)**

```cpp
auto HasRegUnitInterference = [&](MCRegUnit Unit) {
  LiveIntervalUnion::Query SubQ(
      VirtReg, Matrix->getLiveUnions()[static_cast<unsigned>(Unit)]);
  return SubQ.checkInterference();
};
```

为每个 register unit 构造一个临时 `LiveIntervalUnion::Query`（subquery，不复用全局 Queries 数组），检测 VirtReg 与该 unit 上已分配区间是否存在干扰。

**2. 遍历候选寄存器寻找无干扰目标 (行 603-614)**

```cpp
for (MCRegister Reg :
     AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix)) {
  if (Reg == FromReg)
    continue;
  if (none_of(TRI->regunits(Reg), HasRegUnitInterference)) {
    LLVM_DEBUG(dbgs() << "can reassign: " << VirtReg << " from "
                      << printReg(FromReg, TRI) << " to "
                      << printReg(Reg, TRI) << '\n');
    return true;
  }
}
return false;
```

按 AllocationOrder 顺序遍历候选物理寄存器，跳过原寄存器 FromReg。使用 `none_of` 对该寄存器的所有 regunits 调用 lambda：若全部无干扰则可重分配，立即返回 true；否则继续。遍历结束未找到则返回 false。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveIntervalUnion::Query` | VirtReg, LiveUnion | 单个虚拟寄存器与某个 register unit 上所有已分配区间的干扰查询器 |
| `AllocationOrder` | - | 为某 VirtReg 生成的物理寄存器分配顺序迭代器（含 hint/avoid 等过滤） |
| `MCRegUnit` | - | register unit 标识符（物理寄存器可能由多个 unit 组成，如 alias 重叠） |

---

### 优化意图

1. 优先尝试重分配而非直接 evict：在 eviction 候选评估时，如果被 evict 的 VirtReg 还能挪到别的空闲/无冲突寄存器，则不需要真正 evict，降低 cascade 深度。
2. 使用 subquery 而非全局 Queries 数组：避免污染主查询缓存，因为这是临时性、针对单一 regunit 的检查。
3. 按 AllocationOrder 顺序遍历：尊重 hint 与优先级，保证重分配结果与首选分配方向一致。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 跳过 FromReg | 必须避免重新分配回原寄存器 | 若漏跳过会误判为可重分配 |
| regunit 粒度检查 | 通过 `TRI->regunits(Reg)` 检查所有 unit | subreg/overlap 处理依赖 regunit 正确性，物理寄存器与 alias 共享 unit |
| 不修改状态 | 函数为 const，仅查询 | subquery 不应写入 LiveIntervalUnion，否则会破坏全局缓存一致性 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 创建分配顺序 | `AllocationOrder::create` | llvm/lib/CodeGen/AllocationOrder.cpp |
| regunit 迭代 | `TargetRegisterInfo::regunits` | llvm/include/llvm/CodeGen/TargetRegisterInfo.h |
| 干扰查询 | `LiveIntervalUnion::Query::checkInterference` | llvm/lib/CodeGen/LiveIntervalUnion.cpp |

---

### 其他补充

该函数是 `RegAllocEvictionAdvisor` 的 const 成员，仅做查询不改状态，通常被 `tryFindEvictionCandidate` 在评估某 VirtReg 是否可被 evict 时调用，以判断其是否有"逃生路径"。

---

## RAGreedy::evictInterference 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::evictInterference(const LiveInterval &VirtReg,
                                 MCRegister PhysReg,
                                 SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 收集并驱逐所有阻碍 VirtReg 分配到 PhysReg 的干扰虚拟寄存器。调用前假定 `canEvictInterference` 已返回 true。为每个被驱逐者打上 cascade 编号以防止无限循环。

---

### 整体结构

```
evictInterference(VirtReg, PhysReg, NewVRegs)
├── 获取/分配 VirtReg 的 cascade 编号
├── 阶段1：收集所有干扰 VirtReg（遍历 PhysReg 的 regunits）
└── 阶段2：逐个 unassign 并设置 cascade，加入 NewVRegs
```

---

### 逐段注释

**1. 获取 cascade 编号 (行 627-630)**

```cpp
unsigned Cascade = ExtraInfo->getOrAssignNewCascade(VirtReg.reg());

LLVM_DEBUG(dbgs() << "evicting " << printReg(PhysReg, TRI)
                  << " interference: Cascade " << Cascade << '\n');
```

为发起方 VirtReg 获取或新建一个 cascade 编号。该编号会赋给所有被驱逐的寄存器，确保它们只能被更新一代的 cascade 驱逐，从而打破驱逐循环。

**2. 收集所有干扰虚拟寄存器 (行 633-642)**

```cpp
SmallVector<const LiveInterval *, 8> Intfs;
for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
  LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
  ArrayRef<const LiveInterval *> IVR = Q.interferingVRegs();
  Intfs.append(IVR.begin(), IVR.end());
}
```

遍历 PhysReg 的所有 register units，通过 `Matrix->query` 获取每个 unit 上与 VirtReg 干扰的虚拟寄存器列表，追加到 `Intfs`。先收集后驱逐是为了避免在驱逐过程中失效 query 缓存。

**3. 执行驱逐 (行 645-659)**

```cpp
for (const LiveInterval *Intf : Intfs) {
  if (!VRM->hasPhys(Intf->reg()))
    continue;

  Matrix->unassign(*Intf);
  assert((ExtraInfo->getCascade(Intf->reg()) < Cascade ||
          (Cascade < ExtraInfo->getCascade(Intf->reg()) &&
           EvictAdvisor->isUrgentEviction(VirtReg, *Intf)) ||
          VirtReg.isSpillable() < Intf->isSpillable()) &&
         "Cannot decrease cascade number, illegal eviction");
  ExtraInfo->setCascade(Intf->reg(), Cascade);
  ++NumEvicted;
  NewVRegs.push_back(Intf->reg());
}
```

逐个驱逐：跳过已被前一个 unit 处理掉的重复项（`hasPhys` 为 false 表示已 unassign）；调用 `Matrix->unassign` 解除分配；断言驱逐合法性（被驱逐者的 cascade 必须 < 当前 Cascade，除非是 urgent eviction 或可溢出性更差）；设置新 cascade，统计计数，并加入 NewVRegs 队列等待重新分配。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SmallVector<const LiveInterval *, 8>` | Intfs | 收集到的所有干扰虚拟寄存器区间 |
| `ExtraInfo` | Cascade | 驱逐代际编号，单调递增，防止驱逐环 |
| `LiveIntervalUnion::Query` | interferingVRegs | 缓存的干扰虚拟寄存器列表 |
| `SmallVectorImpl<Register>` | NewVRegs | 被驱逐后需重新分配的虚拟寄存器输出列表 |

---

### 优化意图

1. 两阶段（先收集后驱逐）：避免在 `Matrix->unassign` 后失效 query 缓存导致遍历错误，保证收集阶段使用缓存数据。
2. Cascade 机制：被驱逐者继承驱逐者的 cascade 编号，后续只能被更高 cascade 驱逐，从根本上防止驱逐风暴无限循环。
3. 跳过已 unassign 的重复项：同一 VirtReg 可能出现在多个 regunit 的干扰列表中，避免重复处理。
4. 断言保护：强制 cascade 单调性 / urgent / spillable 三种合法驱逐情形，防止非法驱逐破坏算法不变量。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 前置条件 canEvictInterference==true | 调用方必须先验证可驱逐 | 否则可能驱逐不该驱逐的寄存器，违反断言 |
| cascade 单调性 | 被驱逐者 cascade 必须 < 驱逐者 | 违反会导致无限循环或断言失败 |
| query 缓存失效 | unassign 会使 query 失效 | 必须在收集完成后才 unassign，否则需重新查询 |
| 重复项处理 | 同一 VirtReg 跨多 unit 重复出现 | 用 `hasPhys` 跳过已处理项 |
| NewVRegs 顺序 | 输出顺序影响后续重分配优先级 | 被驱逐者将回到主循环重新分配 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 查询干扰 | `LiveIntervalsMatrix::query` | llvm/lib/CodeGen/LiveIntervalsMatrix.cpp |
| 解除分配 | `LiveIntervalsMatrix::unassign` | llvm/lib/CodeGen/LiveIntervalsMatrix.cpp |
| cascade 管理 | `RAGreedy::ExtraInfo::getOrAssignNewCascade/setCascade` | llvm/lib/CodeGen/RegAllocGreedy.cpp |
| 紧急驱逐判定 | `RegAllocEvictionAdvisor::isUrgentEviction` | llvm/lib/CodeGen/RegAllocEvictionAdvisor.cpp |

---

### 其他补充

`NumEvicted` 是调试统计量。`isUrgentEviction` 允许在 cascade 较低但属于紧急情况（如 fixed register 冲突）时破例驱逐，是 cascade 单调性的合法例外。

---

## RegAllocEvictionAdvisor::isUnusedCalleeSavedReg 函数分析

### 函数签名与目的（行号）
```cpp
bool RegAllocEvictionAdvisor::isUnusedCalleeSavedReg(MCRegister PhysReg) const
```

**功能**: 判断 PhysReg 是否是一个尚未被使用的 callee-saved 寄存器（CSR）。用于在低 cost limit 场景下避免首次启用 CSR（首次使用会产生 save/restore 开销）。

---

### 整体结构

```
isUnusedCalleeSavedReg(PhysReg)
├── 查询 PhysReg 的 LastCalleeSavedAlias
├── 若无 CSR alias → return false
└── return !Matrix->isPhysRegUsed(PhysReg)
```

---

### 逐段注释

**1. 查询 CSR alias (行 665-667)**

```cpp
MCRegister CSR = RegClassInfo.getLastCalleeSavedAlias(PhysReg);
if (!CSR)
  return false;
```

通过 `RegClassInfo` 获取 PhysReg 对应的最后一个 callee-saved alias。若 PhysReg 本身不是任何 CSR 的 alias（CSR==0），则直接返回 false，表示它不是 CSR。

**2. 检查是否已被使用 (行 669)**

```cpp
return !Matrix->isPhysRegUsed(PhysReg);
```

若该 CSR 尚未被任何虚拟寄存器分配使用，则返回 true（是"未使用的 CSR"）。一旦被使用过，后续再分配该 CSR 不再产生额外首次开销，故返回 false。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RegClassInfo` | LastCalleeSavedAlias | 给定物理寄存器在该函数中对应的 CSR alias（含别名链） |
| `Matrix` | isPhysRegUsed | 查询某物理寄存器是否已被分配给某 VirtReg |

---

### 优化意图

1. 首次使用 CSR 惩罚：函数中第一次使用 CSR 需要在 prologue/epilogue 插入 save/restore，开销显著。本函数识别此情形以在 cost limit=1 时拒绝启用新 CSR。
2. 重用已启用 CSR：一旦 CSR 已被使用（save/restore 已存在），再分配给它无额外开销，故不再阻止。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 依赖 RegClassInfo 初始化 | LastCalleeSavedAlias 需正确反映函数 CSR 集合 | 初始化错误会误判 |
| isPhysRegUsed 语义 | 仅检测当前已分配状态 | 若 CSR 被释放后再分配，会被误判为"首次使用" |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| CSR alias 查询 | `RegisterClassInfo::getLastCalleeSavedAlias` | llvm/lib/CodeGen/RegisterClassInfo.cpp |
| 物理寄存器使用查询 | `LiveIntervalsMatrix::isPhysRegUsed` | llvm/lib/CodeGen/LiveIntervalsMatrix.cpp |

---

### 其他补充

该函数被 `canAllocatePhysReg` 在 `CostPerUseLimit==1` 时调用，是 CSR 首次启用保护的核心判断。

---

## RegAllocEvictionAdvisor::getOrderLimit 函数分析

### 函数签名与目的（行号）
```cpp
std::optional<unsigned>
RegAllocEvictionAdvisor::getOrderLimit(const LiveInterval &VirtReg,
                                       const AllocationOrder &Order,
                                       unsigned CostPerUseLimit) const
```

**功能**: 在给定 CostPerUseLimit 下，计算 AllocationOrder 中最多需要尝试的候选寄存器数量上限 OrderLimit。若该寄存器类的最小代价已超限则返回 nullopt（无可行寄存器），否则返回裁剪后的上限，避免遍历昂贵寄存器。

---

### 整体结构

```
getOrderLimit(VirtReg, Order, CostPerUseLimit)
├── OrderLimit = Order.getOrder().size()  (默认全部)
├── if CostPerUseLimit < 255:
│   ├── 取 RC 最小代价 MinCost
│   ├── if MinCost >= CostPerUseLimit → return nullopt
│   └── if 末尾寄存器代价超限 → OrderLimit = getLastCostChange(RC)
└── return OrderLimit
```

---

### 逐段注释

**1. 默认上限为全量 (行 676)**

```cpp
unsigned OrderLimit = Order.getOrder().size();
```

默认情况下尝试 AllocationOrder 中的所有候选寄存器。

**2. 仅在有限 cost limit 下裁剪 (行 678-686)**

```cpp
if (CostPerUseLimit < uint8_t(~0u)) {
  const TargetRegisterClass *RC = MRI->getRegClass(VirtReg.reg());
  uint8_t MinCost = RegClassInfo.getMinCost(RC);
  if (MinCost >= CostPerUseLimit) {
    LLVM_DEBUG(dbgs() << TRI->getRegClassName(RC) << " minimum cost = "
                      << MinCost << ", no cheaper registers to be found.\n");
    return std::nullopt;
  }
```

`uint8_t(~0u)` 即 255，表示无限制。当有限制时，取 VirtReg 所在寄存器类的最小代价 MinCost；若最小代价已 >= 限制，则该类中无任何寄存器可行，返回 nullopt 通知调用方放弃。

**3. 裁剪尾部昂贵寄存器 (行 690-694)**

```cpp
if (RegCosts[Order.getOrder().back()] >= CostPerUseLimit) {
  OrderLimit = RegClassInfo.getLastCostChange(RC);
  LLVM_DEBUG(dbgs() << "Only trying the first " << OrderLimit
                    << " regs.\n");
}
```

若 AllocationOrder 末尾的寄存器代价超限（说明尾部存在更贵的寄存器组），则把上限设为 `getLastCostChange(RC)`——即寄存器类中代价发生变化的最后一个分界点，跳过昂贵尾部。寄存器类常具有"长尾同代价"特性，此裁剪减少无效尝试。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AllocationOrder::OrderType` | getOrder() | 候选物理寄存器数组 |
| `RegClassInfo` | getMinCost / getLastCostChange | 寄存器类最小代价、代价变化分界索引 |
| `RegCosts` | [PhysReg] | 每个物理寄存器的 per-use 代价（含 CSR load/store 等） |
| `std::optional<unsigned>` | - | 返回值，nullopt 表示无可行寄存器 |

---

### 优化意图

1. 早退省时：若整类最小代价都超限，直接返回 nullopt，避免后续遍历。
2. 长尾裁剪：寄存器类常有一大批同代价寄存器，尾部更贵时用 `getLastCostChange` 跳过，减少候选数量。
3. 无限制时全量：CostPerUseLimit==255 表示不限代价，无需裁剪，保留全量以最大化分配成功率。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| RegCosts 已初始化 | 需在 Advisor 构造时填充 | 未初始化会读到默认 0，误判所有寄存器便宜 |
| AllocationOrder 顺序 | 末尾应存放更贵寄存器 | 若顺序非按代价排序，裁剪逻辑会误删可行候选 |
| getLastCostChange 语义 | 返回代价变化的最后索引 | 依赖 RegClassInfo 正确排序，否则裁剪位置错误 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 寄存器类最小代价 | `RegisterClassInfo::getMinCost` | llvm/lib/CodeGen/RegisterClassInfo.cpp |
| 代价变化分界 | `RegisterClassInfo::getLastCostChange` | llvm/lib/CodeGen/RegisterClassInfo.cpp |
| 寄存器类查询 | `MachineRegisterInfo::getRegClass` | llvm/include/llvm/CodeGen/MachineRegisterInfo.h |

---

### 其他补充

`uint8_t(~0u)` 用作"无限"哨兵值。`CostPerUseLimit` 来自上游 `selectOrSplit` 的策略，通常基于 spill weight 与寄存器类启发式计算。

---

## RegAllocEvictionAdvisor::canAllocatePhysReg 函数分析

### 函数签名与目的（行号）
```cpp
bool RegAllocEvictionAdvisor::canAllocatePhysReg(unsigned CostPerUseLimit,
                                                 MCRegister PhysReg) const
```

**功能**: 判断在给定 CostPerUseLimit 下，PhysReg 是否可直接分配给 VirtReg。检查两点：单寄存器代价是否超限；当 limit=1 时是否为尚未使用的 CSR（避免首次启用 CSR）。

---

### 整体结构

```
canAllocatePhysReg(CostPerUseLimit, PhysReg)
├── if RegCosts[PhysReg] >= CostPerUseLimit → return false
├── if CostPerUseLimit==1 && isUnusedCalleeSavedReg(PhysReg) → return false
└── return true
```

---

### 逐段注释

**1. 代价超限检查 (行 701-702)**

```cpp
if (RegCosts[PhysReg.id()] >= CostPerUseLimit)
  return false;
```

从 `RegCosts` 表查 PhysReg 的 per-use 代价，若 >= 限制则不可分配。这是基本的代价门槛。

**2. CSR 首次启用保护 (行 705-711)**

```cpp
if (CostPerUseLimit == 1 && isUnusedCalleeSavedReg(PhysReg)) {
  LLVM_DEBUG(
      dbgs() << printReg(PhysReg, TRI) << " would clobber CSR "
             << printReg(RegClassInfo.getLastCalleeSavedAlias(PhysReg), TRI)
             << '\n');
  return false;
}
```

当 limit 极低（==1，表示几乎不允许额外开销）且 PhysReg 是未使用的 CSR 时拒绝分配。首次使用 CSR 会触发 prologue/epilogue 插入 save/restore，代价远超 1，故阻止。调试输出提示会 clobber 的 CSR alias。

**3. 默认允许 (行 712)**

```cpp
return true;
```

通过以上两项检查则可分配。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RegCosts` | [PhysReg.id()] | 每个物理寄存器的 per-use 代价字节数组 |
| `RegClassInfo` | getLastCalleeSavedAlias | 用于调试输出 CSR alias |

---

### 优化意图

1. 代价门槛过滤：避免选择代价过高的寄存器（如需要额外指令编码的寄存器）。
2. CSR 首次启用惩罚：在低 limit 下保护 CSR，避免为单次分配引入函数级 save/restore 开销；高 limit 时则放行（因为开销可接受）。
3. 与 getOrderLimit 配合：`getOrderLimit` 裁剪候选范围，本函数对范围内每个候选做最终把关。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| CostPerUseLimit==1 特判 | 仅在 limit=1 时保护 CSR | 若误用其他低值（如 2）则保护失效 |
| RegCosts 索引 | 用 PhysReg.id() 直接索引 | 需保证 RegCosts 数组覆盖所有物理寄存器编号 |
| 与 isUnusedCalleeSavedReg 协同 | 二者使用同一 RegClassInfo | 初始化时机必须一致 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| CSR 未使用判定 | `RegAllocEvictionAdvisor::isUnusedCalleeSavedReg` | 本文件行 664 |
| CSR alias 查询 | `RegisterClassInfo::getLastCalleeSavedAlias` | llvm/lib/CodeGen/RegisterClassInfo.cpp |

---

### 其他补充

本函数是 eviction 候选评估的最后一道关卡，在 `tryFindEvictionCandidate` 中对每个候选 PhysReg 调用，确保选出的驱逐目标在代价上可行。

---

## RAGreedy::tryEvict 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::tryEvict(const LiveInterval &VirtReg,
                              AllocationOrder &Order,
                              SmallVectorImpl<Register> &NewVRegs,
                              uint8_t CostPerUseLimit,
                              const SmallVirtRegSet &FixedRegisters)
```

**功能**: Strategy 2 的入口：尝试通过驱逐已分配的干扰寄存器，为 VirtReg 腾出一个物理寄存器。委托给 EvictAdvisor 寻找最佳驱逐候选，再调用 evictInterference 执行驱逐。

---

### 整体结构

```
tryEvict(VirtReg, Order, NewVRegs, CostPerUseLimit, FixedRegisters)
├── 启动 NamedRegionTimer 计时
├── BestPhys = EvictAdvisor->tryFindEvictionCandidate(...)
├── if BestPhys.isValid():
│   └── evictInterference(VirtReg, BestPhys, NewVRegs)
└── return BestPhys
```

---

### 逐段注释

**1. 计时与性能追踪 (行 724-725)**

```cpp
NamedRegionTimer T("evict", "Evict", TimerGroupName, TimerGroupDescription,
                   TimePassesIsEnabled);
```

构造一个 `NamedRegionTimer`，在 `TimePassesIsEnabled` 时统计本函数耗时，归入 RA 的 TimerGroup。析构时自动记录。

**2. 委托 Advisor 查找候选 (行 727-728)**

```cpp
MCRegister BestPhys = EvictAdvisor->tryFindEvictionCandidate(
    VirtReg, Order, CostPerUseLimit, FixedRegisters);
```

将候选查找完全委托给 `EvictAdvisor`（默认或 ML advisor）。Advisor 会综合考虑代价、cascade、urgent、FixedRegisters 等因素，返回最优驱逐目标 PhysReg（可能为 invalid 表示无解）。

**3. 执行驱逐并返回 (行 729-731)**

```cpp
if (BestPhys.isValid())
  evictInterference(VirtReg, BestPhys, NewVRegs);
return BestPhys;
```

若找到有效候选，调用 `evictInterference` 真正驱逐 PhysReg 上的所有干扰 VirtReg，将其加入 NewVRegs 重新排队。返回 BestPhys 给上层 `selectOrSplit` 用于 final assignment。若无候选则返回 invalid（0），上层转而尝试 split/spill。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `EvictAdvisor` | - | 驱逐策略对象（Default/ML），封装候选查找逻辑 |
| `SmallVirtRegSet` | FixedRegisters | 不可被驱逐的固定分配寄存器集合（如全局点） |
| `NamedRegionTimer` | T | RA 阶段计时器 |

---

### 优化意图

1. 策略分离：`tryEvict` 仅做编排（计时→查候选→执行驱逐），具体策略交给 Advisor，便于替换为 ML 模型而不改本函数。
2. 一次只驱逐一个 PhysReg：选最优目标后批量驱逐其所有干扰，避免反复失效缓存。
3. 返回 invalid 触发后续策略：若驱逐不可行，上层会尝试 split（Strategy 3+），形成多级 fallback。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| FixedRegisters 不可驱逐 | Advisor 必须跳过固定寄存器 | 若误驱逐会导致正确性问题 |
| BestPhys 有效性检查 | 必须先 isValid 再 evict | 否则 evictInterference 收到 invalid 寄存器会崩溃 |
| 计时器开销 | NamedRegionTimer 即使未启用也有构造开销 | 但已通过 TimePassesIsEnabled 守卫 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 候选查找 | `RegAllocEvictionAdvisor::tryFindEvictionCandidate` | llvm/lib/CodeGen/RegAllocEvictionAdvisor.cpp |
| 执行驱逐 | `RAGreedy::evictInterference` | 本文件行 621 |
| 计时 | `NamedRegionTimer` | llvm/include/llvm/Support/Timer.h |

---

### 其他补充

本函数是 `selectOrSplit` 中 Strategy 2 的实现，位于"已有空闲寄存器但需付代价 → 驱逐 → split → spill"的 fallback 链中。在 `selectOrSplit` 中，只有当 `tryAllocate` 返回的寄存器有非零代价时才会调用本函数（见行 586）。

---

## RAGreedy::trySplitAroundHintReg 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::trySplitAroundHintReg(MCRegister Hint,
                                     const LiveInterval &VirtReg,
                                     SmallVectorImpl<Register> &NewVRegs,
                                     AllocationOrder &Order)
```

**功能**: 当 VirtReg 拥有物理寄存器 Hint 时，尝试将 VirtReg 围绕 Hint 进行区域分裂：在冷块中插入 COPY，使得热点块中 VirtReg 可绑定到 Hint（消除该 COPY），从而在保持 Hint 分配的同时降低总开销。

---

### 整体结构

```
trySplitAroundHintReg(Hint, VirtReg, NewVRegs, Order)
├── 守卫1: hasOptSize → return false
├── 守卫2: stage >= RS_Split2 → return false
├── 计算不绑定 Hint 的代价 Cost（遍历 COPY 指令）
├── Cost *= SplitThresholdForRegWithHint/100
├── if Cost==0 → return false
├── SA->analyze(&VirtReg)
├── calculateRegionSplitCostAroundReg(Hint, Order, Cost, NumCands, BestCand)
├── if BestCand==NoCand → return false
├── doRegionSplit(VirtReg, BestCand, false, NewVRegs)
└── return true
```

---

### 逐段注释

**1. 大小优化守卫 (行 1380-1381)**

```cpp
if (MF->getFunction().hasOptSize())
  return false;
```

分裂会在多个冷块插入 COPY，增加代码体积。若函数以 optsize 为目标（`-Os`/`-Oz`），则放弃此优化。

**2. 防止重复分裂守卫 (行 1384-1385)**

```cpp
if (ExtraInfo->getStage(VirtReg) >= RS_Split2)
  return false;
```

`RS_Split2` 表示已经分裂过两轮。再分裂可能陷入循环，作为安全保护直接拒绝。

**3. 初始化代价与寄存器 (行 1387-1388)**

```cpp
BlockFrequency Cost = BlockFrequency(0);
Register Reg = VirtReg.reg();
```

代价初始化为 0，用块频率衡量。`Reg` 是当前虚拟寄存器编号，用于遍历其所有操作数。

**4. 遍历 COPY 计算不绑定 Hint 的代价 (行 1397-1440)**

```cpp
for (const MachineOperand &Opnd : MRI->reg_nodbg_operands(Reg)) {
  const MachineInstr &Instr = *Opnd.getParent();
  if (!Instr.isCopy() || Opnd.isImplicit())
    continue;

  const bool IsDef = Opnd.isDef();
  const MachineOperand &OtherOpnd = Instr.getOperand(IsDef);
  Register OtherReg = OtherOpnd.getReg();
  assert(Reg == Opnd.getReg());
  if (OtherReg == Reg)
    continue;

  unsigned SubReg = Opnd.getSubReg();
  unsigned OtherSubReg = OtherOpnd.getSubReg();
  if (SubReg && OtherSubReg && SubReg != OtherSubReg)
    continue;
```

遍历 Reg 的所有非调试操作数。只处理非 implicit 的 COPY 指令。找到 COPY 的另一端 `OtherReg`（def↔use 对称位置）。跳过自拷贝（OtherReg==Reg）和子寄存器索引不一致的拷贝（避免错误匹配 subreg 拷贝）。

```cpp
  if (Opnd.readsReg()) {
    SlotIndex Index = LIS->getInstructionIndex(Instr).getRegSlot();

    if (SubReg) {
      LaneBitmask Mask = TRI->getSubRegIndexLaneMask(SubReg);
      if (IsDef)
        Mask = ~Mask;

      if (any_of(VirtReg.subranges(), [=](const LiveInterval::SubRange &S) {
            return (S.LaneMask & Mask).any() && S.liveAt(Index);
          })) {
        continue;
      }
    } else {
      if (VirtReg.liveAt(Index))
        continue;
    }
  }
```

仅在读操作数时检查：若该 COPY 位置 VirtReg 已经 live（即 COPY 被包含在 VirtReg 的 live range 内），则该 COPY 无法通过分裂消除（分裂后仍需保留），跳过。子寄存器场景用 lane mask 精确判断：def 时取反 mask（其他 lane），检查 subrange 是否在该 lane 与 index 处存活。

```cpp
  MCRegister OtherPhysReg =
      OtherReg.isPhysical() ? OtherReg.asMCReg() : VRM->getPhys(OtherReg);
  MCRegister ThisHint = SubReg ? TRI->getSubReg(Hint, SubReg) : Hint;
  if (OtherPhysReg == ThisHint)
    Cost += MBFI->getBlockFreq(Instr.getParent());
}
```

计算 COPY 另一端实际分配的物理寄存器（物理则直接取，虚拟则查 VRM）。`ThisHint` 是 Hint 对应的子寄存器（若涉及 subreg）。若另一端正好是 Hint（即这条 COPY 在分裂后可被消除），则累加该块频率到 Cost——这代表"绑定 Hint 可省下的 COPY 开销"。

**5. 代价缩放与零检查 (行 1443-1446)**

```cpp
BranchProbability Threshold(SplitThresholdForRegWithHint, 100);
Cost *= Threshold;
if (Cost == BlockFrequency(0))
  return false;
```

用 `SplitThresholdForRegWithHint/100` 概率缩放代价，相当于把"可省 COPY 开销"折算为"允许的分裂开销预算"。若预算为 0 说明无收益，直接放弃。

**6. 分析与区域分裂代价计算 (行 1448-1453)**

```cpp
unsigned NumCands = 0;
unsigned BestCand = NoCand;
SA->analyze(&VirtReg);
calculateRegionSplitCostAroundReg(Hint, Order, Cost, NumCands, BestCand);
if (BestCand == NoCand)
  return false;
```

`SA->analyze` 对 VirtReg 做分裂分析（计算 live-in/live-out 块集合等）。`calculateRegionSplitCostAroundReg` 在给定 Cost 预算下评估各候选分裂方案，选出最优 `BestCand`。若无候选则放弃。

**7. 执行分裂 (行 1455-1456)**

```cpp
doRegionSplit(VirtReg, BestCand, false/*HasCompact*/, NewVRegs);
return true;
```

用最优候选执行区域分裂，`HasCompact=false` 表示不使用 compact 形式。新生成的子区间加入 NewVRegs 重新分配。返回 true 表示已处理。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BlockFrequency` | Cost | 块频率度量，表示分裂可省的 COPY 总开销 |
| `BranchProbability` | Threshold | 分裂阈值概率，缩放 Cost 预算 |
| `SlotIndex` | Index | 指令在 SlotIndexes 中的位置，用于 liveAt 查询 |
| `LaneBitmask` | Mask | 子寄存器 lane 掩码，判断 subrange 存活 |
| `LiveInterval::SubRange` | LaneMask, liveAt | 子区间 lane 与存活判断 |
| `SplitAnalysis` | SA | 分裂分析器，提供块集合与可行性 |
| `ExtraInfo` | getStage | 分裂阶段编号（RS_Split/Split2...） |

---

### 优化意图

1. 利用 hint 消除 COPY：VirtReg 有 Hint 时，若不分裂则可能因干扰无法分配到 Hint，导致 COPY 残留；分裂后热点块绑定 Hint，冷块插入 COPY，净收益来自消除热块 COPY。
2. 冷块放置 COPY：分裂把 COPY 限制在低频率块，使总开销低于省下的热 COPY 开销。
3. 预算化决策：Cost = 可省 COPY 开销 × Threshold，作为分裂预算传给 `calculateRegionSplitCostAroundReg`，保证只在有正收益时分裂。
4. 子寄存器精确处理：用 lane mask 判断 subrange 存活，避免误把"在 VirtReg 子 lane 中存活的 COPY"计入可省项。
5. 防止无限分裂：stage 守卫与 optsize 守卫控制分裂次数与体积。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| hasOptSize 守卫 | 体积优化时禁止分裂 | 漏检会导致 optsize 函数体积膨胀 |
| stage < RS_Split2 | 防止重复分裂 | 漏检可能陷入分裂-重分配-再分裂循环 |
| SubReg/OtherSubReg 一致性 | 双方都有 subreg 时必须相等 | 否则误匹配跨 subreg COPY |
| liveAt 检查 | VirtReg 在 COPY 处存活则该 COPY 不可消除 | 漏检会高估收益，导致无收益分裂 |
| VRM->getPhys 可能返回 0 | OtherReg 未分配时返回 invalid | 比较会自然不匹配，但需注意不误用 |
| FIXME: subreg 与 copy bundle | 当前未识别 SplitKit 形成的 copy bundle | 代价估算可能偏差，注释已标注 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 非调试操作数遍历 | `MachineRegisterInfo::reg_nodbg_operands` | llvm/include/llvm/CodeGen/MachineRegisterInfo.h |
| 指令 SlotIndex | `LiveIntervals::getInstructionIndex` | llvm/lib/CodeGen/LiveIntervals.cpp |
| 子寄存器 lane mask | `TargetRegisterInfo::getSubRegIndexLaneMask` | llvm/include/llvm/CodeGen/TargetRegisterInfo.h |
| 物理分配查询 | `VirtRegMap::getPhys` | llvm/lib/CodeGen/VirtRegMap.cpp |
| 块频率 | `MachineBlockFrequencyInfo::getBlockFreq` | llvm/lib/CodeGen/MachineBlockFrequencyInfo.cpp |
| 分裂分析 | `SplitAnalysis::analyze` | llvm/lib/CodeGen/SplitKit.cpp |
| 区域分裂代价 | `RAGreedy::calculateRegionSplitCostAroundReg` | 本文件 |
| 执行分裂 | `RAGreedy::doRegionSplit` | 本文件 |

---

### 其他补充

FIXME（行 1394-1396）指出当前对 subreg 与 SplitKit copy bundle 的代价估算存在偏差，未来应识别同块内的 copy bundle 而非仅直接 COPY 指令。`SplitThresholdForRegWithHint` 是可调参数（默认见 RegAllocGreedy.h / 命令行）。该函数是 hint-driven 分裂的核心，与 `tryRegionSplit`（无 hint 的通用区域分裂）形成互补。
<!-- Group D: Region Split Functions -->

## calculateRegionSplitCostAroundReg 函数分析

### 函数签名与目的（行号）
```cpp
unsigned RAGreedy::calculateRegionSplitCostAroundReg(MCRegister PhysReg,
                                                      AllocationOrder &Order,
                                                      BlockFrequency &BestCost,
                                                      unsigned &NumCands,
                                                      unsigned &BestCand)
```

**功能**: 针对指定物理寄存器 `PhysReg` 构造一个全局分裂候选，运行约束添加、区域增长与代价计算，比较 `BestCost` 决定是否更新当前最佳候选。

---

### 整体结构

```
calculateRegionSplitCostAroundReg(PhysReg, Order, BestCost, NumCands, BestCand)
├── 候选数已达 IntfCache 最大 cursor 数时淘汰最差候选
├── 扩容 GlobalCand 并初始化新候选 Cand
├── SpillPlacer->prepare + addSplitConstraints 得到静态 Cost
├── 若 Cost >= BestCost 直接返回；否则 growRegion 增长区域
├── SpillPlacer->finish()，若 LiveBundles 为空返回
├── Cost += calcGlobalSplitCost，若更优则更新 BestCand/BestCost 并 ++NumCands
└── return BestCand
```

---

### 逐段注释

**1. 候选淘汰（行 1243-1259）**

```cpp
if (NumCands == IntfCache.getMaxCursors()) {
  unsigned WorstCount = ~0u;
  unsigned Worst = 0;
  for (unsigned CandIndex = 0; CandIndex != NumCands; ++CandIndex) {
    if (CandIndex == BestCand || !GlobalCand[CandIndex].PhysReg)
      continue;
    unsigned Count = GlobalCand[CandIndex].LiveBundles.count();
    if (Count < WorstCount) {
      Worst = CandIndex;
      WorstCount = Count;
    }
  }
  --NumCands;
  GlobalCand[Worst] = GlobalCand[NumCands];
  if (BestCand == NumCands)
    BestCand = Worst;
}
```

当候选数达到 InterferenceCache cursor 上限时（多寄存器类场景 >32），通过 `LiveBundles.count()` 找到活跃 bundle 数最少（即最不优）的候选淘汰，将末尾候选搬至其位置。若被淘汰的恰是 `BestCand`，则将 `BestCand` 重定向到搬运后的新位置。

**2. 候选初始化与约束添加（行 1261-1283）**

```cpp
if (GlobalCand.size() <= NumCands)
  GlobalCand.resize(NumCands+1);
GlobalSplitCandidate &Cand = GlobalCand[NumCands];
Cand.reset(IntfCache, PhysReg);

SpillPlacer->prepare(Cand.LiveBundles);
BlockFrequency Cost;
if (!addSplitConstraints(Cand.Intf, Cost)) {
  LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI) << "\tno positive bundles\n");
  return BestCand;
}
LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI)
                  << "\tstatic = " << printBlockFreq(*MBFI, Cost));
if (Cost >= BestCost) {
  LLVM_DEBUG({
    if (BestCand == NoCand)
      dbgs() << " worse than no bundles\n";
    else
      dbgs() << " worse than "
             << printReg(GlobalCand[BestCand].PhysReg, TRI) << '\n';
  });
  return BestCand;
}
```

扩容候选数组并 `reset` 初始化新候选；调用 `addSplitConstraints` 获取插入 spill 的静态代价 `Cost`。如果约束阶段返回 false（无可正偏置 bundle）或 Cost 不优于已知 `BestCost`，提前返回旧 `BestCand`。

**3. 区域增长与最终代价（行 1284-1310）**

```cpp
if (!growRegion(Cand)) {
  LLVM_DEBUG(dbgs() << ", cannot spill all interferences.\n");
  return BestCand;
}

SpillPlacer->finish();

if (!Cand.LiveBundles.any()) {
  LLVM_DEBUG(dbgs() << " no bundles.\n");
  return BestCand;
}

Cost += calcGlobalSplitCost(Cand, Order);
LLVM_DEBUG({
  dbgs() << ", total = " << printBlockFreq(*MBFI, Cost) << " with bundles";
  for (int I : Cand.LiveBundles.set_bits())
    dbgs() << " EB#" << I;
  dbgs() << ".\n";
});
if (Cost < BestCost) {
  BestCand = NumCands;
  BestCost = Cost;
}
++NumCands;

return BestCand;
```

`growRegion` 将候选区域扩展到尽量多的 through-block，若失败则放弃。`finish()` 后若 `LiveBundles` 为空，则交由 `splitSingleBlocks` 处理，不在此更新候选。否则叠加全局代价 `calcGlobalSplitCost`，若严格更优则更新 `BestCand`，并 `++NumCands` 占用槽位。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `GlobalSplitCandidate` | `PhysReg` | 候选目标物理寄存器（0 表示 compact 区域） |
| `GlobalSplitCandidate` | `LiveBundles` | 应保持活跃的 edge bundle 位图 |
| `GlobalSplitCandidate` | `Intf` | InterferenceCache::Cursor，按 block 查询冲突 |
| `GlobalCand` | (SmallVector) | 所有候选集合，按 NumCands 动态扩展 |
| `BestCand` / `BestCost` | (out 参数) | 当前最佳候选索引与其代价 |

---

### 优化意图

1. 为每个可用 PhysReg 估计区域分裂代价，挑选代价最低者进行分裂。
2. 用 cursor 上限触发淘汰，避免 InterferenceCache 资源耗尽。
3. 静态代价先行裁剪，跳过明显劣于当前最佳的候选以节省后续 `growRegion` 开销。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `BestCand == NoCand` 判定 | 返回前必须正确处理“无候选胜出”情形 | 否则会错误地保留无效索引 |
| `NumCands` 维护 | 仅当成功完整评估一候选后才 `++NumCands` | 提前 return 时不应自增，否则后续淘汰逻辑会越界 |
| 淘汰后 `BestCand` 修正 | 被淘汰槽若曾是 BestCand，需指向搬运后的新位置 | 漏掉则后续 `GlobalCand[BestCand]` 指向错误候选 |
| `Cand.LiveBundles.any()` 检查 | 空集合说明无 bundle 需要分裂 | 否则会创建无意义区间 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 初始化候选 | `GlobalSplitCandidate::reset` | RegAllocGreedy.h:241 |
| 添加 use-block 约束 | `RAGreedy::addSplitConstraints` | RegAllocGreedy.cpp:743 |
| 增长区域 | `RAGreedy::growRegion` | RegAllocGreedy.cpp:869 |
| 计算全局代价 | `RAGreedy::calcGlobalSplitCost` | RegAllocGreedy.cpp:1010 |
| SpillPlacer 准备/结束 | `SpillPlacement::prepare/finish` | llvm/lib/CodeGen/SpillPlacement.cpp |

---

### 其他补充

`BestCost` 是 in/out 参数，调用方（`calculateRegionSplitCost`）会传入初始上界（如 `NoCand` 时的极大值），多次调用此函数逐个尝试候选并收敛 `BestCost`。

---

## addSplitConstraints 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::addSplitConstraints(InterferenceCache::Cursor Intf,
                                    BlockFrequency &Cost)
```

**功能**: 遍历当前 VirtReg 的所有 use-block，根据 live-in/live-out 与冲突情况，为 SpillPlacer 构造 `BlockConstraint` 列表并累计静态 spill 代价 `Cost`。

---

### 整体结构

```
addSplitConstraints(Intf, Cost)
├── 取 UseBlocks，resize SplitConstraints
├── for 每个 use-block:
│   ├── 设置 BC.Entry/Exit/ChangesValue
│   ├── 检查 live-in 冲突 → 改 Entry 为 MustSpill/PrefSpill 或只计 Ins
│   ├── 检查 live-out 冲突 → 改 Exit 为 MustSpill/PrefSpill 或只计 Ins
│   ├── 若需在 MBB start 前 spill 且 FirstSplitPoint 不可用 → return false
│   └── 累加 StaticCost += BlockFreq * Ins
├── Cost = StaticCost
├── SpillPlacer->addConstraints(SplitConstraints)
└── return SpillPlacer->scanActiveBundles()
```

---

### 逐段注释

**1. 基础约束初始化（行 745-764）**

```cpp
ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();

SplitConstraints.resize(UseBlocks.size());
BlockFrequency StaticCost = BlockFrequency(0);
for (unsigned I = 0; I != UseBlocks.size(); ++I) {
  const SplitAnalysis::BlockInfo &BI = UseBlocks[I];
  SpillPlacement::BlockConstraint &BC = SplitConstraints[I];

  BC.Number = BI.MBB->getNumber();
  Intf.moveToBlock(BC.Number);
  BC.Entry = BI.LiveIn ? SpillPlacement::PrefReg : SpillPlacement::DontCare;
  BC.Exit = (BI.LiveOut &&
             !LIS->getInstructionFromIndex(BI.LastInstr)->isImplicitDef())
                ? SpillPlacement::PrefReg
                : SpillPlacement::DontCare;
  BC.ChangesValue = BI.FirstDef.isValid();

  if (!Intf.hasInterference())
    continue;
```

设置每个 use-block 的初始偏好：若值 live-in 则偏好入口处置于寄存器（PrefReg），live-out（且末指令非 implicit-def）则偏好出口处也置于寄存器。`ChangesValue` 标记 block 内有定义。无冲突则直接跳过后续 spill 调整。

**2. live-in 冲突处理（行 766-787）**

```cpp
unsigned Ins = 0;

if (BI.LiveIn) {
  if (Intf.first() <= Indexes->getMBBStartIdx(BC.Number)) {
    BC.Entry = SpillPlacement::MustSpill;
    ++Ins;
  } else if (Intf.first() < BI.FirstInstr) {
    BC.Entry = SpillPlacement::PrefSpill;
    ++Ins;
  } else if (Intf.first() < BI.LastInstr) {
    ++Ins;
  }

  if (((BC.Entry == SpillPlacement::MustSpill) ||
       (BC.Entry == SpillPlacement::PrefSpill)) &&
      SlotIndex::isEarlierInstr(BI.FirstInstr,
                                SA->getFirstSplitPoint(BC.Number)))
    return false;
}
```

按冲突起始位置 `Intf.first()` 与 block 边界、首末使用指令比较，决定 `Entry` 改为 `MustSpill`（冲突覆盖入口）/`PrefSpill`（冲突在首指令前）或仅累计 `Ins`。`Ins` 表示需要插入的 spill 指令数。若需要在 block 起点前 spill 但首条可分裂点早于 `FirstInstr`，无法放置 spill，返回 false 终止整个候选。

**3. live-out 冲突处理与代价累计（行 789-811）**

```cpp
if (BI.LiveOut) {
  if (Intf.last() >= SA->getLastSplitPoint(BC.Number)) {
    BC.Exit = SpillPlacement::MustSpill;
    ++Ins;
  } else if (Intf.last() > BI.LastInstr) {
    BC.Exit = SpillPlacement::PrefSpill;
    ++Ins;
  } else if (Intf.last() > BI.FirstInstr) {
    ++Ins;
  }
}

while (Ins--)
  StaticCost += SpillPlacer->getBlockFrequency(BC.Number);
}
Cost = StaticCost;

SpillPlacer->addConstraints(SplitConstraints);
return SpillPlacer->scanActiveBundles();
```

对 live-out 做对称处理：冲突末点越过最后分裂点则 `MustSpill`，越过末使用指令则 `PrefSpill`。累计 `Ins × BlockFreq` 到 `StaticCost`，赋给 out 参数 `Cost`。最后把约束提交给 `SpillPlacer` 并扫描得到正偏置 bundle，返回是否至少存在一个正 bundle。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SplitAnalysis::BlockInfo` | `LiveIn/LiveOut` | 该 use-block 入口/出口是否活跃 |
| `BlockInfo` | `FirstInstr/LastInstr/FirstDef` | block 内首末使用/首定义的 SlotIndex |
| `SpillPlacement::BlockConstraint` | `Entry/Exit` | 入口/出口偏好（PrefReg/PrefSpill/MustSpill/DontCare） |
| `SplitConstraints` | (SmallVector) | 与 UseBlocks 一一对应的约束列表 |

---

### 优化意图

1. 在 use-block 边界上确定 spill 位置，最小化 spill 指令的执行频率。
2. `PrefReg` 偏置使 SpillPlacement 倾向于把值留在寄存器，避免冗余 reload。
3. 提前 return false 让上层 `calculateRegionSplitCostAroundReg` 放弃无法放置 spill 的候选。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `BI.LastInstr` 的 implicit-def 检查 | implicit-def 不是真实使用，不能据此设 PrefReg | 否则错误地保留寄存器 |
| `FirstSplitPoint` 检查 | 必须保证 spill 可在 block 起点前安全插入 | 否则分裂失败导致 miscompile |
| `Intf` cursor 状态 | 必须先 `moveToBlock` 再读 `first()/last()` | 否则读到错误 block 的冲突信息 |
| `scanActiveBundles` 返回值 | false 表示无正偏置 bundle，候选无效 | 上层会据此丢弃该 PhysReg |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 取 use-block 信息 | `SplitAnalysis::getUseBlocks` | llvm/lib/CodeGen/SplitKit.cpp |
| 移动冲突游标 | `InterferenceCache::Cursor::moveToBlock` | llvm/lib/CodeGen/InterferenceCache.cpp |
| 取分裂点 | `SplitAnalysis::getFirstSplitPoint/getLastSplitPoint` | llvm/lib/CodeGen/SplitKit.cpp |
| 添加约束 | `SpillPlacement::addConstraints` | llvm/lib/CodeGen/SpillPlacement.cpp |

---

### 其他补充

此函数是区域分裂代价评估的"静态部分"，与 `calcGlobalSplitCost`（动态部分）配合：前者仅考虑 use-block 的强制 spill；后者还会考虑 through-block 与 PrefReg 偏好不一致带来的额外 spill。

---

## growRegion 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::growRegion(GlobalSplitCandidate &Cand)
```

**功能**: 以 use-block 周边的 through-block 为起点，迭代扩展 SpillPlacer 的活跃 bundle 区域，直到收敛或预算耗尽。

---

### 整体结构

```
growRegion(Cand)
├── Todo = SA->getThroughBlocks()，初始化 Budget
├── while true:
│   ├── 取 SpillPlacer->getRecentPositive() 得新正 bundle
│   ├── for 每个 NewBundle:
│   │   ├── 取 Bundles->getBlocks(Bundle)，扣减 Budget
│   │   └── 把仍在 Todo 中的 block 推入 ActiveBlocks
│   ├── 若无新增 → break
│   ├── 对新增 ActiveBlocks 调 addThroughConstraints（有 PhysReg）
│   │   或 addPrefSpill（无 PhysReg，含 loop IV 特殊处理）
│   ├── 更新 AddedTo
│   └── SpillPlacer->iterate()
└── return true
```

---

### 逐段注释

**1. 初始化与预算（行 871-878）**

```cpp
BitVector Todo = SA->getThroughBlocks();
SmallVectorImpl<unsigned> &ActiveBlocks = Cand.ActiveBlocks;
unsigned AddedTo = 0;
#ifndef NDEBUG
unsigned Visited = 0;
#endif

unsigned long Budget = GrowRegionComplexityBudget;
```

`Todo` 标记尚未纳入 SpillPlacer 的 through-block；`AddedTo` 记录已处理的位置；`Budget` 限制遍历的 block 总数防止编译时间爆炸。

**2. 收集新周边 through-block（行 879-899）**

```cpp
while (true) {
  ArrayRef<unsigned> NewBundles = SpillPlacer->getRecentPositive();
  for (unsigned Bundle : NewBundles) {
    ArrayRef<unsigned> Blocks = Bundles->getBlocks(Bundle);
    if (Blocks.size() >= Budget)
      return false;
    Budget -= Blocks.size();
    for (unsigned Block : Blocks) {
      if (!Todo.test(Block))
        continue;
      Todo.reset(Block);
      ActiveBlocks.push_back(Block);
#ifndef NDEBUG
      ++Visited;
#endif
    }
  }
```

每轮取 SpillPlacer 最近一轮变为正偏置的 bundle，看其涉及的 block 中是否有未处理的 through-block，加入 `ActiveBlocks`。若某个 bundle 的 block 数超过剩余 Budget，直接放弃整个候选（返回 false）。

**3. 终止与约束添加（行 900-935）**

```cpp
  if (ActiveBlocks.size() == AddedTo)
    break;

  auto NewBlocks = ArrayRef(ActiveBlocks).slice(AddedTo);
  if (Cand.PhysReg) {
    if (!addThroughConstraints(Cand.Intf, NewBlocks))
      return false;
  } else {
    bool PrefSpill = true;
    if (SA->looksLikeLoopIV() && NewBlocks.size() >= 2) {
      MachineLoop *L = Loops->getLoopFor(MF->getBlockNumbered(NewBlocks[0]));
      if (L && L->getHeader()->getNumber() == (int)NewBlocks[0] &&
          all_of(NewBlocks.drop_front(), [&](unsigned Block) {
            return L == Loops->getLoopFor(MF->getBlockNumbered(Block));
          }))
        PrefSpill = false;
    }
    if (PrefSpill)
      SpillPlacer->addPrefSpill(NewBlocks, /* Strong= */ true);
  }
  AddedTo = ActiveBlocks.size();

  SpillPlacer->iterate();
}
```

若无新增 block，跳出循环。否则对新 block 子集调用 `addThroughConstraints`（有 PhysReg 时基于真实冲突）或 `addPrefSpill`（无 PhysReg 即 compact 模式时强制偏好 spill）。compact 模式下特殊处理 loop IV：若 NewBlocks 是同一循环的 header + 内部块，则不强制 PrefSpill，让值在 Header↔Latch 间保持寄存器活跃。最后 `SpillPlacer->iterate()` 让偏好传播到下一轮，可能产生新的正 bundle。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `GlobalSplitCandidate` | `ActiveBlocks` | 已纳入区域的 through-block 列表 |
| `BitVector Todo` | - | 剩余未处理的 through-block |
| `GrowRegionComplexityBudget` | (配置) | block 遍历预算上限 |

---

### 优化意图

1. 通过迭代传播让区域尽量覆盖 use-block 周边以减少跨区域 spill。
2. compact 模式下避开 loop IV 的强制 spill，防止把循环不变量推到栈上。
3. 预算机制避免在巨型 CFG 上指数级膨胀编译时间。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `Budget` 检查 | 必须在 `Blocks.size() >= Budget` 时立即返回 false | 否则无符号下溢导致后续逻辑错误 |
| `AddedTo` 维护 | 必须只在成功添加约束后才更新 | 否则会重复处理或漏处理 |
| loop IV 判定 | 严格要求 NewBlocks[0] 是 header 且其余块属同循环 | 否则错误地跳过 PrefSpill |
| `getRecentPositive` 语义 | 仅返回上一轮 iterate 后变正的 bundle | 与 iterate 顺序强耦合 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 取新正 bundle | `SpillPlacement::getRecentPositive` | llvm/lib/CodeGen/SpillPlacement.cpp |
| bundle→blocks | `EdgeBundles::getBlocks` | llvm/lib/CodeGen/EdgeBundles.cpp |
| 添加 through 约束 | `RAGreedy::addThroughConstraints` | RegAllocGreedy.cpp:816 |
| 偏好传播 | `SpillPlacement::iterate` | llvm/lib/CodeGen/SpillPlacement.cpp |

---

### 其他补充

`SA->looksLikeLoopIV()` 的判定让紧凑区域分裂避免把循环计数变量 spill 出去，否则会让 loop backedge 上携带活跃的栈值，代价很高。

---

## addThroughConstraints 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::addThroughConstraints(InterferenceCache::Cursor Intf,
                                      ArrayRef<unsigned> Blocks)
```

**功能**: 对一组 live-through block 添加 SpillPlacement 约束：无冲突的加入 link，有冲突的构造 `BlockConstraint` 并检查 spill 可放置性。

---

### 整体结构

```
addThroughConstraints(Intf, Blocks)
├── GroupSize=8 缓冲区 BCS[]/TBS[]
├── for 每个 block Number in Blocks:
│   ├── Intf.moveToBlock(Number)
│   ├── 无冲突 → 放入 TBS，满 8 flush 为 addLinks
│   └── 有冲突:
│       ├── 检查 FirstNonDebugInstr 早于 FirstSplitPoint → return false
│       ├── 设 Entry=MustSpill/PrefSpill（按 Intf.first() vs MBBStart）
│       ├── 设 Exit=MustSpill/PrefSpill（按 Intf.last() vs LastSplitPoint）
│       └── 放入 BCS，满 8 flush 为 addConstraints
├── flush 剩余 BCS 与 TBS
└── return true
```

---

### 逐段注释

**1. 无冲突块加入 link 缓冲（行 818-834）**

```cpp
const unsigned GroupSize = 8;
SpillPlacement::BlockConstraint BCS[GroupSize];
unsigned TBS[GroupSize];
unsigned B = 0, T = 0;

for (unsigned Number : Blocks) {
  Intf.moveToBlock(Number);

  if (!Intf.hasInterference()) {
    assert(T < GroupSize && "Array overflow");
    TBS[T] = Number;
    if (++T == GroupSize) {
      SpillPlacer->addLinks(ArrayRef(TBS, T));
      T = 0;
    }
    continue;
  }
```

无冲突的 through-block 不需要约束，只需在 SpillPlacer 中加入"链接"——表示该 block 两侧 bundle 应保持同样的 reg/spill 状态。每 8 个一批 flush 减少 API 调用次数。

**2. 有冲突块设置 Entry/Exit（行 836-862）**

```cpp
  assert(B < GroupSize && "Array overflow");
  BCS[B].Number = Number;

  MachineBasicBlock *MBB = MF->getBlockNumbered(Number);
  auto FirstNonDebugInstr = MBB->getFirstNonDebugInstr();
  if (FirstNonDebugInstr != MBB->end() &&
      SlotIndex::isEarlierInstr(LIS->getInstructionIndex(*FirstNonDebugInstr),
                                SA->getFirstSplitPoint(Number)))
    return false;
  if (Intf.first() <= Indexes->getMBBStartIdx(Number))
    BCS[B].Entry = SpillPlacement::MustSpill;
  else
    BCS[B].Entry = SpillPlacement::PrefSpill;

  if (Intf.last() >= SA->getLastSplitPoint(Number))
    BCS[B].Exit = SpillPlacement::MustSpill;
  else
    BCS[B].Exit = SpillPlacement::PrefSpill;

  if (++B == GroupSize) {
    SpillPlacer->addConstraints(ArrayRef(BCS, B));
    B = 0;
  }
}
```

有冲突的 through-block：先检查首条非调试指令是否早于 `FirstSplitPoint`，若是则无法在 block 起点前插入 spill，整候选失败。否则按冲突起止位置决定 `Entry/Exit`：冲突覆盖入口/出口则 `MustSpill`，否则 `PrefSpill`（through-block 没有"中间档"，因为值不在此 block 使用）。每 8 个约束一批提交。

**3. 尾部 flush（行 864-866）**

```cpp
SpillPlacer->addConstraints(ArrayRef(BCS, B));
SpillPlacer->addLinks(ArrayRef(TBS, T));
return true;
```

循环结束后将剩余不足 8 个的 BCS（约束）与 TBS（链接）一并提交。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SpillPlacement::BlockConstraint` | `Number/Entry/Exit` | block 编号与入口/出口偏好 |
| `BCS[GroupSize]` | - | 冲突块约束批缓冲 |
| `TBS[GroupSize]` | - | 无冲突块编号批缓冲 |

---

### 优化意图

1. 区分冲突与无冲突 through-block：前者强制 spill 偏好，后者仅保持一致。
2. 批量提交降低 SpillPlacer API 开销。
3. 提前 return false 让上层放弃无法分裂的候选。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `B < GroupSize` / `T < GroupSize` | assert 防止批缓冲溢出 | 若 GroupSize 改小但未同步处理会越界 |
| `FirstSplitPoint` 检查 | 必须保证 spill 可在 block 入口前插入 | 否则 miscompile |
| 末尾 flush 顺序 | 先 addConstraints 再 addLinks | 顺序无强约束但保持稳定行为 |
| 返回值 | 失败时 false 会让 `growRegion` 立即放弃整个候选 | 单个 block 失败影响整个 PhysReg 候选 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 添加约束 | `SpillPlacement::addConstraints` | llvm/lib/CodeGen/SpillPlacement.cpp |
| 添加链接 | `SpillPlacement::addLinks` | llvm/lib/CodeGen/SpillPlacement.cpp |
| 取 block 首非调试指令 | `MachineBasicBlock::getFirstNonDebugInstr` | llvm/include/llvm/CodeGen/MachineBasicBlock.h |

---

### 其他补充

`GroupSize=8` 是一个经验性批大小，平衡栈空间与 API 调用次数；该函数被 `growRegion` 在每轮新增 through-block 上调用，调用频率较高，批量化对编译时间有意义。

---

## calcGlobalSplitCost 函数分析

### 函数签名与目的（行号）
```cpp
BlockFrequency RAGreedy::calcGlobalSplitCost(GlobalSplitCandidate &Cand,
                                              const AllocationOrder &Order)
```

**功能**: 在 SpillPlacer 收敛后，根据 `Cand.LiveBundles` 与各 block 的 PrefReg 偏好一致性，估算区域分裂的"动态 spill 代价"。

---

### 整体结构

```
calcGlobalSplitCost(Cand, Order)
├── GlobalCost=0
├── for 每个 use-block BI:
│   ├── RegIn = LiveBundles[入 bundle], RegOut = LiveBundles[出 bundle]
│   ├── Intf.moveToBlock
│   ├── 若 LiveIn 且 RegIn != (Entry==PrefReg) → Ins++
│   ├── 若 LiveOut 且 RegOut != (Exit==PrefReg) → Ins++
│   └── GlobalCost += BlockFreq * Ins
├── for 每个 through-block in ActiveBlocks:
│   ├── RegIn/RegOut 同上
│   ├── 都 false → skip
│   ├── 都 true 且有冲突 → +2×BlockFreq
│   └── 仅一边 true → +1×BlockFreq
└── return GlobalCost
```

---

### 逐段注释

**1. use-block 代价（行 1012-1030）**

```cpp
BlockFrequency GlobalCost = BlockFrequency(0);
const BitVector &LiveBundles = Cand.LiveBundles;
ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
for (unsigned I = 0; I != UseBlocks.size(); ++I) {
  const SplitAnalysis::BlockInfo &BI = UseBlocks[I];
  SpillPlacement::BlockConstraint &BC = SplitConstraints[I];
  bool RegIn  = LiveBundles[Bundles->getBundle(BC.Number, false)];
  bool RegOut = LiveBundles[Bundles->getBundle(BC.Number, true)];
  unsigned Ins = 0;

  Cand.Intf.moveToBlock(BC.Number);

  if (BI.LiveIn)
    Ins += RegIn != (BC.Entry == SpillPlacement::PrefReg);
  if (BI.LiveOut)
    Ins += RegOut != (BC.Exit == SpillPlacement::PrefReg);
  while (Ins--)
    GlobalCost += SpillPlacer->getBlockFrequency(BC.Number);
}
```

对每个 use-block，检查入口/出口 bundle 是否在 `LiveBundles` 中（即被分配到该候选 PhysReg）与初始 `PrefReg` 偏好是否一致：若值 live-in 但 bundle 状态与偏好相反（应 spill 却 reg，或应 reg 却 spill），需要插入一条 spill/reload，代价 = `BlockFreq`。注意此处仅比较与 `PrefReg`，`MustSpill`/`PrefSpill` 的强制 spill 已在静态代价 `addSplitConstraints` 计入，不重复。

**2. through-block 代价（行 1032-1048）**

```cpp
for (unsigned Number : Cand.ActiveBlocks) {
  bool RegIn  = LiveBundles[Bundles->getBundle(Number, false)];
  bool RegOut = LiveBundles[Bundles->getBundle(Number, true)];
  if (!RegIn && !RegOut)
    continue;
  if (RegIn && RegOut) {
    Cand.Intf.moveToBlock(Number);
    if (Cand.Intf.hasInterference()) {
      GlobalCost += SpillPlacer->getBlockFrequency(Number);
      GlobalCost += SpillPlacer->getBlockFrequency(Number);
    }
    continue;
  }
  GlobalCost += SpillPlacer->getBlockFrequency(Number);
}
return GlobalCost;
```

对每个 through-block：两侧 bundle 都不在 LiveBundles 则无 spill（值完全在栈上跨过 block，无需 reload）；两侧都在 LiveBundles 但 block 内有冲突，需在入口和出口各插一条 spill（双倍代价）；仅一侧在 LiveBundles 则入口或出口单边 spill（单倍代价）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Cand.LiveBundles` | (BitVector) | 该候选应活跃的 bundle 集合 |
| `SplitConstraints` | - | use-block 的 Entry/Exit 偏好，与 UseBlocks 一一对应 |
| `Cand.ActiveBlocks` | - | 已纳入区域的 through-block 列表 |

---

### 优化意图

1. 用 `BlockFrequency` 加权 spill 数量，使分裂代价与运行时执行频率成正比。
2. through-block 双侧 reg 且有冲突需双 spill，惩罚跨越冲突区域的"假贯通"。
3. 与 `addSplitConstraints` 静态代价互补：静态算强制 spill，本函数算偏好不一致带来的 spill。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `BC.Entry == PrefReg` 比较 | 仅检查与 PrefReg 不一致 | `MustSpill` 情形已在静态代价处理，勿重复计 |
| `Cand.Intf.moveToBlock` | 必须在取 hasInterference 前移动游标 | 否则读到上一个 block 的冲突信息 |
| `getBundle(Number, false/true)` | false=入边 bundle, true=出边 bundle | 弄反会让 RegIn/RegOut 错位 |
| through-block 双 reg+冲突 | 必须 +2 而非 +1 | 否则低估代价 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 取 block 入/出 bundle | `EdgeBundles::getBundle` | llvm/lib/CodeGen/EdgeBundles.cpp |
| 取 block 频率 | `SpillPlacement::getBlockFrequency` | llvm/lib/CodeGen/SpillPlacement.cpp |
| 移动冲突游标 | `InterferenceCache::Cursor::moveToBlock` | llvm/lib/CodeGen/InterferenceCache.cpp |

---

### 其他补充

`Order` 参数在本函数体内未直接使用，保留是为了未来扩展（如根据 AllocationOrder 调整代价）；当前实现下可忽略。

---

## doRegionSplit 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::doRegionSplit(const LiveInterval &VirtReg,
                                    unsigned BestCand, bool HasCompact,
                                    SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 在 `calculateRegionSplitCost` 选定最佳候选后，构造 `LiveRangeEdit` 与 `SplitEditor`，把最佳候选（及可选 compact 区域）的 bundle 分配到对应区间，再调用 `splitAroundRegion` 完成实际分裂。

---

### 整体结构

```
doRegionSplit(VirtReg, BestCand, HasCompact, NewVRegs)
├── 构造 LiveRangeEdit LREdit，SE->reset
├── BundleCand.assign(NoCand)
├── 若 BestCand != NoCand:
│   ├── Cand.getBundles(BundleCand, BestCand) 收集 bundle
│   ├── UsedCands.push_back(BestCand)
│   └── Cand.IntvIdx = SE->openIntv()
├── 若 HasCompact:
│   ├── GlobalCand.front().getBundles(BundleCand, 0)
│   ├── UsedCands.push_back(0)
│   └── Cand.IntvIdx = SE->openIntv()
├── splitAroundRegion(LREdit, UsedCands)
└── return MCRegister()
```

---

### 逐段注释

**1. 编辑器初始化与 BundleCand 默认值（行 1334-1340）**

```cpp
SmallVector<unsigned, 8> UsedCands;
LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
SE->reset(LREdit, SplitSpillMode);

BundleCand.assign(Bundles->getNumBundles(), NoCand);
```

`LiveRangeEdit` 负责管理新创建的子区间与死消 remat 跟踪；`SE->reset` 把 SplitEditor 绑定到本次编辑并设置 spill 模式（`SplitSpillMode`，由上层根据 CSR 情况决定）。`BundleCand` 数组按 bundle 数量分配，全部初始化为 `NoCand`（表示该 bundle 走栈区间）。

**2. 最佳候选 bundle 分配（行 1343-1352）**

```cpp
if (BestCand != NoCand) {
  GlobalSplitCandidate &Cand = GlobalCand[BestCand];
  if (unsigned B = Cand.getBundles(BundleCand, BestCand)) {
    UsedCands.push_back(BestCand);
    Cand.IntvIdx = SE->openIntv();
    LLVM_DEBUG(dbgs() << "Split for " << printReg(Cand.PhysReg, TRI) << " in "
                      << B << " bundles, intv " << Cand.IntvIdx << ".\n");
    (void)B;
  }
}
```

调用 `Cand.getBundles` 把所有属于该候选的 LiveBundles 在 `BundleCand` 中标记为 `BestCand`，并返回被标记的 bundle 数 `B`。若至少有一个 bundle 被标记，则把候选加入 `UsedCands` 并通过 `SE->openIntv()` 在 SplitEditor 中开一个新区间，记下 `IntvIdx` 供后续 `splitAroundRegion` 引用。`(void)B` 抑制未使用警告（仅在 debug 中用到）。

**3. compact 区域 bundle 分配与最终分裂（行 1355-1368）**

```cpp
if (HasCompact) {
  GlobalSplitCandidate &Cand = GlobalCand.front();
  assert(!Cand.PhysReg && "Compact region has no physreg");
  if (unsigned B = Cand.getBundles(BundleCand, 0)) {
    UsedCands.push_back(0);
    Cand.IntvIdx = SE->openIntv();
    LLVM_DEBUG(dbgs() << "Split for compact region in " << B
                      << " bundles, intv " << Cand.IntvIdx << ".\n");
    (void)B;
  }
}

splitAroundRegion(LREdit, UsedCands);
return MCRegister();
```

compact 区域使用 `GlobalCand.front()`（约定其 `PhysReg==0`，assert 验证），同样标记 bundle 并开区间。注意 `getBundles` 第二参数传 0 而非 `BestCand`，因为 compact 候选固定索引为 0。`getBundles` 内部只覆盖仍是 `NoCand` 的 bundle，所以 compact 与 BestCand 不会冲突（compact 是 fallback）。最后调用 `splitAroundRegion` 完成实际指令级分裂。返回空 `MCRegister()` 表示本次未直接分配物理寄存器，新 VReg 会重新进入队列。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveRangeEdit` | - | 管理分裂产生的新区间集合 |
| `BundleCand` | (SmallVector) | 每个 bundle 对应的候选索引，或 `NoCand` |
| `GlobalSplitCandidate::IntvIdx` | - | 该候选在 SplitEditor 中对应的区间编号 |
| `UsedCands` | (SmallVector) | 实际被使用的候选索引列表 |

---

### 优化意图

1. 把代价评估阶段得到的"哪些 bundle 属于哪个候选"信息转换为 SplitEditor 的开区间操作。
2. compact 区域作为 BestCand 之外的补充，覆盖没有物理寄存器候选的剩余 bundle。
3. 返回空寄存器让新 VReg 走重新分配，迭代收敛。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `BundleCand.assign(NoCand)` | 必须先全部置 NoCand，否则 `getBundles` 会错误覆盖 | `getBundles` 仅覆盖 NoCand 项 |
| compact 候选索引固定为 0 | `GlobalCand.front()` 必须是 compact，`PhysReg==0` | assert 验证，违反则架构错误 |
| `IntvIdx` 必须先 openIntv 再用 | `splitAroundRegion` 依赖 `IntvIdx` 引用区间 | 未 open 会使用 0 默认值导致错乱 |
| `B` 未使用 | `(void)B` 抑制警告 | debug 才用到，删除 `(void)` 会编译警告 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 开新区间 | `SplitEditor::openIntv` | llvm/lib/CodeGen/SplitKit.cpp |
| 标记 bundle 归属 | `GlobalSplitCandidate::getBundles` | RegAllocGreedy.h:250 |
| 实际分裂 | `RAGreedy::splitAroundRegion` | RegAllocGreedy.cpp:1064 |
| 编辑器重置 | `SplitEditor::reset` | llvm/lib/CodeGen/SplitKit.cpp |

---

### 其他补充

返回 `MCRegister()` 是区域分裂的关键约定：分裂后原 VirtReg 被拆为多个子区间，调用方 `tryRegionSplit` 应返回 0 让外层把新 VReg 重新入队，而不是直接宣称分配完成。

---

## splitAroundRegion 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::splitAroundRegion(LiveRangeEdit &LREdit,
                                  ArrayRef<unsigned> UsedCands)
```

**功能**: 在 SplitEditor 中已开好若干全局区间后，遍历 use-block 与 through-block，根据 `BundleCand` 把每个 block 的入口/出口接入对应全局区间；最后 `finish` 并按区间类别设置新 VReg 的 stage。

---

### 整体结构

```
splitAroundRegion(LREdit, UsedCands)
├── 取 NumGlobalIntvs，断言非零
├── 计算 SingleInstrs（proper subclass 时隔离单条指令）
├── 处理 use-block:
│   ├── for BI in UseBlocks:
│   │   ├── 取入/出 bundle 的 CandIn/CandOut
│   │   ├── 取对应 IntvIdx 与 IntfIn/IntfOut
│   │   ├── 都无 → isolated，可选 splitSingleBlock
│   │   ├── 都有 → splitLiveThroughBlock
│   │   ├── 仅 IntvIn → splitRegInBlock
│   │   └── 仅 IntvOut → splitRegOutBlock
├── 处理 through-block:
│   ├── Todo = SA->getThroughBlocks()
│   ├── for UsedCand, for Block in ActiveBlocks:
│   │   ├── 去重（Todo）
│   │   ├── 取 CandIn/CandOut 与 IntfIn/IntfOut
│   │   └── 调 splitLiveThroughBlock
├── SE->finish(&IntvMap), DebugVars->splitRegister
└── for 新区间: 按 IntvMap 设 stage (RS_Spill / RS_Split2 / 默认)
```

---

### 逐段注释

**1. 准备与 use-block 入口/出口候选确定（行 1066-1102）**

```cpp
const unsigned NumGlobalIntvs = LREdit.size();
LLVM_DEBUG(dbgs() << "splitAroundRegion with " << NumGlobalIntvs
                  << " globals.\n");
assert(NumGlobalIntvs && "No global intervals configured");

Register Reg = SA->getParent().reg();
bool SingleInstrs = RegClassInfo.isProperSubClass(MRI->getRegClass(Reg));

ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
  unsigned Number = BI.MBB->getNumber();
  unsigned IntvIn = 0, IntvOut = 0;
  SlotIndex IntfIn, IntfOut;
  if (BI.LiveIn) {
    unsigned CandIn = BundleCand[Bundles->getBundle(Number, false)];
    if (CandIn != NoCand) {
      GlobalSplitCandidate &Cand = GlobalCand[CandIn];
      IntvIn = Cand.IntvIdx;
      Cand.Intf.moveToBlock(Number);
      IntfIn = Cand.Intf.first();
    }
  }
  if (BI.LiveOut) {
    unsigned CandOut = BundleCand[Bundles->getBundle(Number, true)];
    if (CandOut != NoCand) {
      GlobalSplitCandidate &Cand = GlobalCand[CandOut];
      IntvOut = Cand.IntvIdx;
      Cand.Intf.moveToBlock(Number);
      IntfOut = Cand.Intf.last();
    }
  }
```

`NumGlobalIntvs` 即 `doRegionSplit` 中 `openIntv` 次数（用于后面区分全局 vs 本地区间）。`SingleInstrs` 决定是否把多用途 block 进一步切到单指令级——当原寄存器类是某寄存器类的真子类时，栈区间需要全是 COPY 才能被强制 inflate 到更宽的类。

对每个 use-block：若 live-in，查入边 bundle 对应候选，得到 `IntvIn`（区间号）与 `IntfIn`（冲突起点，用于决定接入位置）；live-out 对称。

**2. use-block 分发分裂（行 1104-1118）**

```cpp
  if (!IntvIn && !IntvOut) {
    LLVM_DEBUG(dbgs() << printMBBReference(*BI.MBB) << " isolated.\n");
    if (SA->shouldSplitSingleBlock(BI, SingleInstrs))
      SE->splitSingleBlock(BI);
    continue;
  }

  if (IntvIn && IntvOut)
    SE->splitLiveThroughBlock(Number, IntvIn, IntfIn, IntvOut, IntfOut);
  else if (IntvIn)
    SE->splitRegInBlock(BI, IntvIn, IntfIn);
  else
    SE->splitRegOutBlock(BI, IntvOut, IntfOut);
}
```

四种情况：两侧均无候选 → 该 block 是孤立块，按 `shouldSplitSingleBlock` 决定是否单块分裂（依据 `SingleInstrs` 与 block 内使用情况）；两侧都有候选 → 走"贯通分裂"在入口与出口分别接入不同/相同区间；仅入口或仅出口有候选 → 走对应的单边分裂。SplitEditor 内部会自动插入 COPY 与 spill 指令。

**3. through-block 处理（行 1120-1153）**

```cpp
BitVector Todo = SA->getThroughBlocks();
for (unsigned UsedCand : UsedCands) {
  ArrayRef<unsigned> Blocks = GlobalCand[UsedCand].ActiveBlocks;
  for (unsigned Number : Blocks) {
    if (!Todo.test(Number))
      continue;
    Todo.reset(Number);

    unsigned IntvIn = 0, IntvOut = 0;
    SlotIndex IntfIn, IntfOut;

    unsigned CandIn = BundleCand[Bundles->getBundle(Number, false)];
    if (CandIn != NoCand) {
      GlobalSplitCandidate &Cand = GlobalCand[CandIn];
      IntvIn = Cand.IntvIdx;
      Cand.Intf.moveToBlock(Number);
      IntfIn = Cand.Intf.first();
    }

    unsigned CandOut = BundleCand[Bundles->getBundle(Number, true)];
    if (CandOut != NoCand) {
      GlobalSplitCandidate &Cand = GlobalCand[CandOut];
      IntvOut = Cand.IntvIdx;
      Cand.Intf.moveToBlock(Number);
      IntfOut = Cand.Intf.last();
    }
    if (!IntvIn && !IntvOut)
      continue;
    SE->splitLiveThroughBlock(Number, IntvIn, IntfIn, IntvOut, IntfOut);
  }
}
```

`Todo` 用于跨候选去重——一个 through-block 可能出现在多个候选的 `ActiveBlocks` 中（因为它周边有多个正 bundle），但只能被处理一次。对每个 block 同样查入/出 bundle 的候选并取 `IntvIdx` 与冲突边界，调 `splitLiveThroughBlock`（through-block 没有使用，所以总是贯通式接入）。两侧都无候选则跳过（值在栈上完整跨过此 block）。

**4. 收尾与新区间分类（行 1155-1200）**

```cpp
++NumGlobalSplits;

SmallVector<unsigned, 8> IntvMap;
SE->finish(&IntvMap);
DebugVars->splitRegister(Reg, LREdit.regs(), *LIS);

unsigned OrigBlocks = SA->getNumLiveBlocks();

for (unsigned I = 0, E = LREdit.size(); I != E; ++I) {
  const LiveInterval &Reg = LIS->getInterval(LREdit.get(I));

  if (ExtraInfo->getOrInitStage(Reg.reg()) != RS_New)
    continue;

  if (IntvMap[I] == 0) {
    ExtraInfo->setStage(Reg, RS_Spill);
    continue;
  }

  if (IntvMap[I] < NumGlobalIntvs) {
    if (SA->countLiveBlocks(&Reg) >= OrigBlocks) {
      LLVM_DEBUG(dbgs() << "Main interval covers the same " << OrigBlocks
                        << " blocks as original.\n");
      ExtraInfo->setStage(Reg, RS_Split2);
    }
    continue;
  }
}

if (VerifyEnabled)
  MF->verify(LIS, Indexes, "After splitting live range around region",
              &errs());
```

`SE->finish` 生成最终区间映射 `IntvMap`：`IntvMap[I]=0` 表示该新区间是 remainder（栈区间），`< NumGlobalIntvs` 表示是某个全局候选区间，`>= NumGlobalIntvs` 是 block-local 区间（如 splitSingleBlock 产生）。`DebugVars->splitRegister` 把原寄存器的 DBG_VALUE 也复制到新区间。

stage 设置：
- `RS_New` 以外的 stage 跳过（DCE 残留等不重设）；
- remainder → `RS_Spill`（不再尝试分裂，分配不上就 spill）；
- 全局候选区间若覆盖 block 数 ≥ 原 → `RS_Split2`（防止分裂无进展导致死循环），否则保持 `RS_New` 允许继续分裂；
- 本地区间保持 `RS_New` 重新入队。

最后可选 `MF->verify` 校验分裂后 CFG/LIS 一致性。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveRangeEdit` | `regs()` | 分裂产生的所有新 VReg 列表 |
| `BundleCand` | (SmallVector) | 每个 bundle→候选索引映射 |
| `IntvMap` | (SmallVector) | SplitEditor 输出：每个新区间→splitkit 内部 interval 编号 |
| `ExtraInfo` stage | `RS_New/RS_Spill/RS_Split2` | 新区间的下一阶段分配策略 |

---

### 优化意图

1. 把"哪些 bundle 归哪个候选"的拓扑信息翻译成 SplitEditor 的入口/出口接入指令，实现真正的指令级分裂。
2. `SingleInstrs` 模式让 proper subclass 的栈区间全是 COPY，强制 inflate 寄存器类。
3. 通过 `IntvMap` 与 `countLiveBlocks` 防止反复分裂无进展陷入死循环。
4. remainder 标 `RS_Spill` 避免对栈主导区间再次尝试高成本分裂。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `NumGlobalIntvs` 非零断言 | `doRegionSplit` 必须 open 至少一个区间 | 否则后续逻辑无意义 |
| `Todo` 去重 | through-block 可能属多候选，只能分裂一次 | 重复分裂会生成重复 COPY |
| `IntvMap[I] < NumGlobalIntvs` 判定 | 全局候选区间编号 < openIntv 次数 | 弄反会把本地区间误判为全局 |
| `countLiveBlocks >= OrigBlocks` 判定 | 必须严格无进展才设 RS_Split2 | 否则阻止合法的进一步分裂 |
| `SingleInstrs` 仅对 proper subclass | 栈区间需全 COPY 才能 inflate | 误用会让非 subclass 区间被过度切分 |
| `DebugVars->splitRegister` | 必须更新调试变量信息 | 否则调试信息丢失 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单块分裂 | `SplitEditor::splitSingleBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| 贯通分裂 | `SplitEditor::splitLiveThroughBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| 入口接入 | `SplitEditor::splitRegInBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| 出口接入 | `SplitEditor::splitRegOutBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| 完成 | `SplitEditor::finish` | llvm/lib/CodeGen/SplitKit.cpp |
| 调试变量分裂 | `LiveDebugVariables::splitRegister` | llvm/lib/CodeGen/LiveDebugVariables.cpp |

---

### 其他补充

`NumGlobalSplits` 是统计计数器，用于评估分裂启发式的频率。`VerifyEnabled` 由命令行 `-verify-regalloc` 控制，开启时每次分裂后做 MachineFunction 完整 verify，主要用于调试。
<!-- Group E: Local/Instruction/Block Split Functions -->

## trySplit 函数分析

### 函数签名与目的（行号 1972-2007）
```cpp
MCRegister RAGreedy::trySplit(const LiveInterval &VirtReg,
                              AllocationOrder &Order,
                              SmallVectorImpl<Register> &NewVRegs,
                              const SmallVirtRegSet &FixedRegisters)
```

**功能**: 作为寄存器分配主分裂调度入口，根据 VirtReg 所处 stage 与是否局部于单个 MBB，路由到 local/instruction/region/block 四种分裂策略，使 VirtReg 可被分配。

---

### 整体结构

```
trySplit(VirtReg, Order, NewVRegs, FixedRegisters)
├── Stage guard: >= RS_Spill 直接返回
├── 若 intervalIsInOneMBB -> tryLocalSplit -> 失败再 tryInstructionSplit
├── 否则 SA->analyze
│   ├── stage < RS_Split2 -> tryRegionSplit
│   └── 失败 -> tryBlockSplit
└── return PhysReg / MCRegister()
```

---

### 逐段注释

**1. Stage 守门 (行 1977-1978)**

```cpp
if (ExtraInfo->getStage(VirtReg) >= RS_Spill)
  return MCRegister();
```

处于 RS_Spill 及以后 stage 的区间不再尝试分裂，直接返回，避免在已决定要溢出的区间上浪费分裂开销。

**2. 局部分支 (行 1981-1989)**

```cpp
if (LIS->intervalIsInOneMBB(VirtReg)) {
  NamedRegionTimer T("local_split", "Local Splitting", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);
  SA->analyze(&VirtReg);
  MCRegister PhysReg = tryLocalSplit(VirtReg, Order, NewVRegs);
  if (PhysReg || !NewVRegs.empty())
    return PhysReg;
  return tryInstructionSplit(VirtReg, Order, NewVRegs);
}
```

单基本块内的区间走独立计时通道：先 tryLocalSplit（按 gap 拆分），不成功则退化到 tryInstructionSplit（围绕单条指令分裂以放松寄存器类约束）。两条路径互斥地返回。

**3. 全局分支 (行 1991-2003)**

```cpp
NamedRegionTimer T("global_split", "Global Splitting", TimerGroupName,
                   TimerGroupDescription, TimePassesIsEnabled);
SA->analyze(&VirtReg);
if (ExtraInfo->getStage(VirtReg) < RS_Split2) {
  MCRegister PhysReg = tryRegionSplit(VirtReg, Order, NewVRegs);
  if (PhysReg || !NewVRegs.empty())
    return PhysReg;
}
```

跨块区间先调用 SplitAnalysis 分析使用块；stage < RS_Split2 时尝试 region 分裂（按多块区域划分）。RS_Split2 表示 region 分裂已尝试但失败，跳过避免重复无进展分裂。

**4. 兜底 block 分裂 (行 2005-2006)**

```cpp
return tryBlockSplit(VirtReg, Order, NewVRegs);
```

region 分裂失败后，把每个使用块都隔离开来，生成若干局部区间交由后续回合再处理。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ExtraInfo` (RAGreedyExtraInfo) | stage table | 记录每个虚拟寄存器当前所处分裂/溢出阶段 |
| `SplitAnalysis *SA` | UseBlocks/UseSlots | VirtReg 使用信息分析结果 |
| `SmallVirtRegSet FixedRegisters` | - | 当前固定分配的寄存器集合，作上下文约束 |

---

### 优化意图

1. 通过 stage 机制实现"分层递进分裂 + 兜底溢出"，避免在同一策略上无限循环。
2. 区分 local/global 计时通道，便于性能剖析。
3. Region -> Block -> Local -> Instruction 的递进粒度，使大区间逐步收敛到可分配状态。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| stage 上限 | RS_Spill 及以上不分裂 | 否则会与 spiller 反复拉锯 |
| 单块判定 | `intervalIsInOneMBB` 包括 live-in/live-out 单块 | phi-def 单块循环会被误判，但下游 tryLocalSplit 做了兼容处理 |
| 返回语义 | 返回 PhysReg 表示可分配；NewVRegs 非空表示产生新区间 | 调用方需同时检查二者 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单块判定 | `LiveIntervals::intervalIsInOneMBB` | llvm/lib/CodeGen/LiveIntervals.cpp |
| 使用分析 | `SplitAnalysis::analyze` | llvm/lib/CodeGen/SplitKit.cpp |
| 局部分裂 | `RAGreedy::tryLocalSplit` | RegAllocGreedy.cpp:1741 |
| 区域分裂 | `RAGreedy::tryRegionSplit` | RegAllocGreedy.cpp |

---

### 其他补充

`trySplit` 是 RAGreedy 主循环（`selectOrSplit`）每轮都会触发的入口；`FixedRegisters` 参数在此函数内未直接使用，但语义上为下游策略提供"哪些物理寄存器不可动"的上下文。

---

## tryLocalSplit 函数分析

### 函数签名与目的（行号 1741-1963）
```cpp
MCRegister RAGreedy::tryLocalSplit(const LiveInterval &VirtReg,
                                   AllocationOrder &Order,
                                   SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 在 VirtReg 所在的唯一 MBB 内，按 use slot 之间的 gap 寻找最优分裂点，将区间分成 live-through 外段 + 局部分配中段，最大化中段可分配性。

---

### 整体结构

```
tryLocalSplit(VirtReg, Order, NewVRegs)
├── 前置检查: 单 UseBlock 且 Uses>2
├── 计算 RegMaskGaps
├── 设 ProgressRequired = stage>=RS_Split2
├── for each PhysReg in Order:
│   ├── calcGapWeights(PhysReg)
│   ├── 叠加 regmask gap = HUGE
│   └── 双指针 [SplitBefore, SplitAfter] 扫描 gap 找最优 Diff
├── 无候选 -> 返回
├── 执行 SE->openIntv/enter/leave/useIntv/finish
└── 按 NewGaps vs NumGaps 标记 RS_Split2 或保持 RS_New
```

---

### 逐段注释

**1. 前置过滤 (行 1746-1761)**

```cpp
if (SA->getUseBlocks().size() != 1)
  return MCRegister();
const SplitAnalysis::BlockInfo &BI = SA->getUseBlocks().front();
ArrayRef<SlotIndex> Uses = SA->getUseSlots();
if (Uses.size() <= 2)
  return MCRegister();
const unsigned NumGaps = Uses.size()-1;
```

仅处理单 UseBlock 场景；使用点 <=2 时只有 1 个 gap，无法做有意义切分，直接放弃。

**2. RegMask gap 收集 (行 1772-1799)**

```cpp
SmallVector<unsigned, 8> RegMaskGaps;
if (Matrix->checkRegMaskInterference(VirtReg)) {
  ArrayRef<SlotIndex> RMS = LIS->getRegMaskSlotsInBlock(BI.MBB->getNumber());
  unsigned RI = llvm::lower_bound(RMS, Uses.front().getRegSlot()) - RMS.begin();
  unsigned RE = RMS.size();
  for (unsigned I = 0; I != NumGaps && RI != RE; ++I) {
    assert(!SlotIndex::isEarlierInstr(RMS[RI], Uses[I]));
    if (SlotIndex::isEarlierInstr(Uses[I + 1], RMS[RI]))
      continue;
    if (SlotIndex::isSameInstr(Uses[I + 1], RMS[RI]) && I + 1 == NumGaps)
      break;
    RegMaskGaps.push_back(I);
    while (RI != RE && SlotIndex::isEarlierInstr(RMS[RI], Uses[I + 1]))
      ++RI;
  }
}
```

仅当 VirtReg 存在 regmask 干扰时收集：把块内所有 regmask slot 用 lower_bound 对齐到 `[Uses[I], Uses[I+1]]` 区间，记录哪些 gap 含 regmask。这些 gap 在后续按 PhysReg 处理时会被强制设为 HUGE_VALF，排除被选中为中段的可能。注意同一 regmask 落在使用点上会算入相邻两个 gap。

**3. Progress 规则与初始 best (行 1819-1829)**

```cpp
bool ProgressRequired = ExtraInfo->getStage(VirtReg) >= RS_Split2;
unsigned BestBefore = NumGaps;
unsigned BestAfter = 0;
float BestDiff = 0;
const float blockFreq =
    SpillPlacer->getBlockFrequency(BI.MBB->getNumber()).getFrequency() *
    (1.0f / MBFI->getEntryFreq().getFrequency());
SmallVector<float, 8> GapWeight;
```

收敛策略：stage < RS_Split2 允许任意非空切分；stage == RS_Split2 强制 NewGaps < NumGaps。`BestBefore=NumGaps` 作哨兵表示"未找到候选"。`blockFreq` 是归一化块频率，用于估算新区间权重。

**4. 主循环：每个 PhysReg 计算 GapWeight 并叠加 regmask (行 1831-1840)**

```cpp
for (MCRegister PhysReg : Order) {
  assert(PhysReg);
  calcGapWeights(PhysReg, GapWeight);
  if (Matrix->checkRegMaskInterference(VirtReg, PhysReg))
    for (unsigned Gap : RegMaskGaps)
      GapWeight[Gap] = huge_valf;
  unsigned SplitBefore = 0, SplitAfter = 1;
  float MaxGap = GapWeight[0];
```

每个 PhysReg 重新算一次 gap weight（每 gap 需要驱逐的最大 spill weight）。若 VirtReg 与 PhysReg 在 regmask 上冲突，对应 gap 置 HUGE。

**5. 双指针扫描窗口 (行 1852-1923)**

```cpp
while (true) {
  const bool LiveBefore = SplitBefore != 0 || BI.LiveIn;
  const bool LiveAfter = SplitAfter != NumGaps || BI.LiveOut;
  if (!LiveBefore && !LiveAfter) break;
  bool Shrink = true;
  unsigned NewGaps = LiveBefore + SplitAfter - SplitBefore + LiveAfter;
  bool Legal = !ProgressRequired || NewGaps < NumGaps;
  if (Legal && MaxGap < huge_valf) {
    const float EstWeight = normalizeSpillWeight(
        blockFreq * (NewGaps + 1),
        Uses[SplitBefore].distance(Uses[SplitAfter]) +
            (LiveBefore + LiveAfter) * SlotIndex::InstrDist,
        1);
    if (EstWeight * Hysteresis >= MaxGap) {
      Shrink = false;
      float Diff = EstWeight - MaxGap;
      if (Diff > BestDiff) {
        BestDiff = Hysteresis * Diff;
        BestBefore = SplitBefore;
        BestAfter = SplitAfter;
      }
    }
  }
  if (Shrink) {
    if (++SplitBefore < SplitAfter) {
      if (GapWeight[SplitBefore - 1] >= MaxGap) {
        MaxGap = GapWeight[SplitBefore];
        for (unsigned I = SplitBefore + 1; I != SplitAfter; ++I)
          MaxGap = std::max(MaxGap, GapWeight[I]);
      }
      continue;
    }
    MaxGap = 0;
  }
  if (SplitAfter >= NumGaps) break;
  MaxGap = std::max(MaxGap, GapWeight[SplitAfter++]);
}
```

窗口 `[SplitBefore, SplitAfter]` 表示中段覆盖的 gap 集合，`MaxGap` 是窗口内最大干扰权重。`NewGaps` 把窗口两端的 live-before/after COPY 段也计入，反映新分裂区间真正占用的"槽数"。

- **合法性**: ProgressRequired 时必须 NewGaps < NumGaps；同时 MaxGap 不能为 HUGE（regmask/fix 冲突）。
- **可分配性**: `EstWeight * Hysteresis >= MaxGap` —— 估算的新区间 spill weight 乘以迟滞系数必须大于要驱逐的 max weight，才值得扩展。
- **Diff 越大越优**: `BestDiff` 记录全局最佳，乘 Hysteresis 抑制频繁切换。
- **Shrink 优先**: 一旦当前窗口不可分配，先增大 SplitBefore 缩小窗口；Shrink 路径里维护 MaxGap 增量更新：仅当被移出窗口的 gap >= 当前 MaxGap 时才重扫。
- **Extend**: SplitBefore 追上 SplitAfter 时，扩展右端 SplitAfter 并更新 MaxGap。
- 终止：窗口覆盖全部 gap 且 LiveBefore/After 都 false，或 SplitAfter 到达 NumGaps。

**6. 候选不存在 (行 1927-1928)**

```cpp
if (BestBefore == NumGaps)
  return MCRegister();
```

哨兵未变表示没有任何合法可分配切分，返回交由上层走下一条策略。

**7. 执行分裂 (行 1934-1943)**

```cpp
LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
SE->reset(LREdit);
SE->openIntv();
SlotIndex SegStart = SE->enterIntvBefore(Uses[BestBefore]);
SlotIndex SegStop  = SE->leaveIntvAfter(Uses[BestAfter]);
SE->useIntv(SegStart, SegStop);
SmallVector<unsigned, 8> IntvMap;
SE->finish(&IntvMap);
DebugVars->splitRegister(VirtReg.reg(), LREdit.regs(), *LIS);
```

通过 SplitEditor 创建一个新 interval，覆盖 `[Uses[BestBefore], Uses[BestAfter]]`；其余部分成为 remainder（标 0）。IntvMap[i]==1 表示该新 vreg 属于本次切出的中段。DebugVars 同步更新调试变量范围。

**8. Stage 标注 (行 1947-1959)**

```cpp
bool LiveBefore = BestBefore != 0 || BI.LiveIn;
bool LiveAfter = BestAfter != NumGaps || BI.LiveOut;
unsigned NewGaps = LiveBefore + BestAfter - BestBefore + LiveAfter;
if (NewGaps >= NumGaps) {
  LLVM_DEBUG(dbgs() << "Tagging non-progress ranges:");
  assert(!ProgressRequired && "Didn't make progress when it was required.");
  for (unsigned I = 0, E = IntvMap.size(); I != E; ++I)
    if (IntvMap[I] == 1) {
      ExtraInfo->setStage(LIS->getInterval(LREdit.get(I)), RS_Split2);
    }
}
++NumLocalSplits;
```

如果新中段 gap 数 >= 原 NumGaps（即未真正缩短），把它标 RS_Split2，使下一轮强制要求进展，避免无限 2+3 / 3+3 反复切。assert 保证 ProgressRequired 路径不会落入此分支。remainder（IntvMap==0）保持默认 stage，下游会作为溢出处理。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SplitAnalysis::BlockInfo` | LiveIn/LiveOut/FirstInstr/LastInstr/MBB | 单块使用信息 |
| `SmallVector<unsigned> RegMaskGaps` | gap index | 含 regmask 的 gap 下标集合 |
| `SmallVector<float> GapWeight` | per gap | 每 gap 内 PhysReg 最大干扰 spill weight |
| `LiveRangeEdit LREdit` | - | 分裂产物 vreg 容器 |
| `IntvMap[i]` | interval id | 0=remainder, 1=本次切出的中段 |

---

### 优化意图

1. 在单块内寻找"中段可分配 + 两端被驱逐代价低"的最佳切点，使中段拿到物理寄存器、remainder 走溢出。
2. 用 Hysteresis（迟滞系数）抑制同一段被反复切来切去。
3. Progress 规则 + RS_Split2 标注保证收敛：每个 vreg 最多做一次"无进展"切分。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 单块假设 | `getUseBlocks().size()==1` 但区间可能 live-in/out | 注释说明仍按 FirstInstr-LastInstr 连续处理，需保证不破坏语义 |
| RegMask gap 双计 | regmask 在使用点上算入两个相邻 gap | 漏算会导致选中不可分配 gap |
| MaxGap 增量更新 | 只有移出 gap >= MaxGap 时才重扫 | 写错会得到偏小的 MaxGap，误判可分配 |
| NewGaps 计算 | LiveBefore/LiveAfter 各加 1 | 漏加会绕过 Progress 检查导致死循环 |
| EstWeight 模型 | 假设每条指令独立读或写 | read-modify-write 会被高估 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| gap 权重 | `RAGreedy::calcGapWeights` | RegAllocGreedy.cpp:1663 |
| regmask 干扰 | `LiveRegMatrix::checkRegMaskInterference` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| 块频率 | `SpillPlacement::getBlockFrequency` | llvm/lib/CodeGen/SpillPlacement.cpp |
| 分裂执行 | `SplitEditor::openIntv/enterIntvBefore/leaveIntvAfter/useIntv/finish` | llvm/lib/CodeGen/SplitKit.cpp |
| Stage 设置 | `RAGreedyExtraInfo::setStage` | RegAllocGreedy.cpp |

---

### 其他补充

`Hysteresis` 是 RAGreedy 的静态常量（典型 0.95 或类似），起"宁选当前最优"的稳定作用。`NumLocalSplits` 是统计计数器，可在 `-debug-only=regalloc` 输出里看到累计值。

---

## calcGapWeights 函数分析

### 函数签名与目的（行号 1663-1736）
```cpp
void RAGreedy::calcGapWeights(MCRegister PhysReg,
                              SmallVectorImpl<float> &GapWeight)
```

**功能**: 计算在 VirtReg 各 use slot 之间的 gap 上，为使 PhysReg 可用所必须驱逐的最大 spill weight；包含普通虚拟寄存器干扰和固定寄存器干扰两部分。

---

### 整体结构

```
calcGapWeights(PhysReg, GapWeight)
├── 取单块 BI、Uses，算 StartIdx/StopIdx
├── GapWeight.assign(NumGaps, 0)
├── for each MCRegUnit of PhysReg:
│   ├── 查 VirtReg 与该 unit 的干扰 LiveIntervalUnion
│   └── 双指针遍历 segment vs gap，更新 GapWeight = max(weight)
└── for each MCRegUnit of PhysReg:
    └── 遍历 RegUnit LR，把覆盖的 gap 标 HUGE_VALF
```

---

### 逐段注释

**1. 边界与初始化 (行 1665-1676)**

```cpp
assert(SA->getUseBlocks().size() == 1 && "Not a local interval");
const SplitAnalysis::BlockInfo &BI = SA->getUseBlocks().front();
ArrayRef<SlotIndex> Uses = SA->getUseSlots();
const unsigned NumGaps = Uses.size()-1;
SlotIndex StartIdx =
  BI.LiveIn ? BI.FirstInstr.getBaseIndex() : BI.FirstInstr;
SlotIndex StopIdx =
  BI.LiveOut ? BI.LastInstr.getBoundaryIndex() : BI.LastInstr;
GapWeight.assign(NumGaps, 0.0f);
```

StartIdx/StopIdx 区分 live-in/out：live-in 时从 FirstInstr 的 baseIndex（指令前 slot）起，live-out 时到 LastInstr 的 boundaryIndex（指令后 slot）止。这定义了干扰扫描的窗口范围。

**2. 虚拟寄存器干扰 (行 1679-1711)**

```cpp
for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
  if (!Matrix->query(const_cast<LiveInterval &>(SA->getParent()), Unit)
           .checkInterference())
    continue;
  LiveIntervalUnion::SegmentIter IntI =
      Matrix->getLiveUnions()[static_cast<unsigned>(Unit)].find(StartIdx);
  for (unsigned Gap = 0; IntI.valid() && IntI.start() < StopIdx; ++IntI) {
    while (Uses[Gap+1].getBoundaryIndex() < IntI.start())
      if (++Gap == NumGaps)
        break;
    if (Gap == NumGaps)
      break;
    const float weight = IntI.value()->weight();
    for (; Gap != NumGaps; ++Gap) {
      GapWeight[Gap] = std::max(GapWeight[Gap], weight);
      if (Uses[Gap+1].getBaseIndex() >= IntI.stop())
        break;
    }
    if (Gap == NumGaps)
      break;
  }
}
```

对每个 regunit：先快速查询是否与 VirtReg 干扰，无则跳过。否则从 StartIdx 起迭代 LiveIntervalUnion 的 segment。外层推进 segment，内层 `while` 跳过被 segment 起点之前的 gap，内层 `for` 把当前 segment 覆盖的所有 gap 的权重取 max。一个 segment 跨多条指令时会同时影响两侧 gap（对应注释里"interference that overlaps an instruction is counted in both gaps"）。

**3. 固定寄存器干扰 (行 1714-1734)**

```cpp
for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
  const LiveRange &LR = LIS->getRegUnit(Unit);
  LiveRange::const_iterator I = LR.find(StartIdx);
  LiveRange::const_iterator E = LR.end();
  for (unsigned Gap = 0; I != E && I->start < StopIdx; ++I) {
    while (Uses[Gap+1].getBoundaryIndex() < I->start)
      if (++Gap == NumGaps)
        break;
    if (Gap == NumGaps)
      break;
    for (; Gap != NumGaps; ++Gap) {
      GapWeight[Gap] = huge_valf;
      if (Uses[Gap+1].getBaseIndex() >= I->end)
        break;
    }
    if (Gap == NumGaps)
      break;
  }
}
```

结构与上一段相同，但来源是 `LIS->getRegUnit(Unit)` 的固定 LiveRange，被覆盖的 gap 直接置 `huge_valf`（不可驱逐），意味着该 gap 上 PhysReg 不可用。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SplitAnalysis::BlockInfo` | LiveIn/FirstInstr/LastInstr/LiveOut | 决定扫描窗口起止 |
| `LiveIntervalUnion::SegmentIter` | value()->weight() | 干扰区间的 spill weight |
| `LiveRange` (RegUnit) | start/end | 物理寄存器（含 callee-saved 等）的固定占用 |

---

### 优化意图

1. 把"驱逐代价"集中到 gap 维度，让 tryLocalSplit 的窗口扫描只需查一个数组。
2. 用 huge_valf 哨兵统一表达"不可用"，使上层无需分支区分普通/固定干扰。
3. `Matrix->query().checkInterference()` 提前剪枝，避免对无干扰 regunit 走迭代器。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 单块前提 | assert `getUseBlocks().size()==1` | 跨块调用会得到错误窗口 |
| 双计语义 | 一个 segment 覆盖 use slot 时算入两个 gap | 与 tryLocalSplit 的 cost 模型一致才能正确 |
| `getBaseIndex`/`getBoundaryIndex` | gap 边界是 use slot 的前/后边界 | 写反会漏掉紧邻 use 的干扰 |
| Gap 索引终止 | 多处 `if (Gap == NumGaps) break` | 漏一处会越界 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| regunit 迭代 | `TargetRegisterInfo::regunits` | llvm/lib/CodeGen/TargetRegisterInfo.cpp |
| 干扰查询 | `LiveRegMatrix::query` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| RegUnit 范围 | `LiveIntervals::getRegUnit` | llvm/lib/CodeGen/LiveIntervals.cpp |
| LIU segment 迭代 | `LiveIntervalUnion::SegmentIter` | llvm/lib/CodeGen/LiveIntervalUnion.cpp |

---

### 其他补充

`huge_valf` 是 LLVM 内部用的"无穷大" float 哨兵；上层通过 `MaxGap < huge_valf` 判定该 PhysReg 在当前窗口内无固定/不可驱逐冲突。

---

## tryInstructionSplit 函数分析

### 函数签名与目的（行号 1588-1652）
```cpp
MCRegister RAGreedy::tryInstructionSplit(const LiveInterval &VirtReg,
                                         AllocationOrder &Order,
                                         SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 围绕单条指令对 VirtReg 做切分，主要用于把受寄存器类约束的 live range 片段挪到更大的 super-class 中，相当于"溢出到更大的寄存器类"。

---

### 整体结构

```
tryInstructionSplit(VirtReg, Order, NewVRegs)
├── 取 CurRC，判断是否 ProperSubClass；若非且无 subrange -> 返回
├── 建 LREdit + SE->reset(SM_Size)
├── Uses = SA->getUseSlots(); 若 <=1 返回
├── 算 SuperRC 与其 allocatable 数
├── for each Use:
│   ├── 全 copy / 不放松约束 / 不读子 lane -> skip
│   └── 否则 SE openIntv + enter/leave/useIntv 围住该指令
├── LREdit.empty() -> 返回
├── SE->finish
├── DebugVars->splitRegister
└── 全部新 vreg 标 RS_Spill
```

---

### 逐段注释

**1. 子类判定与早退 (行 1591-1599)**

```cpp
const TargetRegisterClass *CurRC = MRI->getRegClass(VirtReg.reg());
bool SplitSubClass = true;
if (!RegClassInfo.isProperSubClass(CurRC)) {
  if (!VirtReg.hasSubRanges())
    return MCRegister();
  SplitSubClass = false;
}
```

只有当 CurRC 是某个更大类的真子类时，"放松到 super class"才有意义；否则需要 VirtReg 有 subranges（lane 维度）才值得切。两者都不满足直接返回。

**2. 编辑器与使用点 (行 1603-1608)**

```cpp
LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
SE->reset(LREdit, SplitEditor::SM_Size);
ArrayRef<SlotIndex> Uses = SA->getUseSlots();
if (Uses.size() <= 1)
  return MCRegister();
```

`SM_Size` 模式提示 SplitEditor 优先选择更小的切分。使用点 <=1 时无切分空间。

**3. SuperRC 信息 (行 1613-1616)**

```cpp
const TargetRegisterClass *SuperRC =
    TRI->getLargestLegalSuperClass(CurRC, *MF);
unsigned SuperRCNumAllocatableRegs =
    RegClassInfo.getNumAllocatableRegs(SuperRC);
```

获取最大合法 super class 与其可分配寄存器数，作为"切分后能否放松约束"的基准。

**4. 逐使用点筛选与切分 (行 1621-1639)**

```cpp
for (const SlotIndex Use : Uses) {
  if (const MachineInstr *MI = Indexes->getInstructionFromIndex(Use)) {
    if (TII->isFullCopyInstr(*MI) ||
        (SplitSubClass &&
         SuperRCNumAllocatableRegs ==
             getNumAllocatableRegsForConstraints(MI, VirtReg.reg(), SuperRC,
                                                 TII, TRI, RegClassInfo)) ||
        (!SplitSubClass && VirtReg.hasSubRanges() &&
         !readsLaneSubset(*MRI, MI, VirtReg, TRI, Use, TII))) {
      continue;
    }
  }
  SE->openIntv();
  SlotIndex SegStart = SE->enterIntvBefore(Use);
  SlotIndex SegStop = SE->leaveIntvAfter(Use);
  SE->useIntv(SegStart, SegStop);
}
```

跳过三类使用点：
- full copy：复制本身没有寄存器类限制价值。
- SplitSubClass 且约束后 SuperRC 可分配数不变：切了也不会放松约束。
- !SplitSubClass 且 `readsLaneSubset` 返回 false：指令读取的 lane 覆盖了所有存活 lane，无法借助 subrange 切分放松。

其余使用点用 `openIntv/enterIntvBefore/leaveIntvAfter/useIntv` 围出一个独立 interval 覆盖该指令。

**5. 收尾 (行 1641-1651)**

```cpp
if (LREdit.empty()) {
  return MCRegister();
}
SmallVector<unsigned, 8> IntvMap;
SE->finish(&IntvMap);
DebugVars->splitRegister(VirtReg.reg(), LREdit.regs(), *LIS);
ExtraInfo->setStage(LREdit.begin(), LREdit.end(), RS_Spill);
return MCRegister();
```

全部使用点都被跳过则放弃。否则 finish 出新 vreg，并把所有新 vreg 标 RS_Spill：这是最后一次机会，下轮走溢出。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `TargetRegisterClass *CurRC/SuperRC` | - | 当前类与最大合法 super 类 |
| `RegClassInfo` | isProperSubClass/getNumAllocatableRegs | 寄存器类信息缓存 |
| `LiveRangeEdit LREdit` | - | 切分产物 |
| `SplitEditor::SM_Size` | - | 切分策略提示 |

---

### 优化意图

1. 在 spiller 介入前，借助"复制 + 切分"把片段移到更宽寄存器类，逃避类约束瓶颈。
2. 通过三重 skip 规则避免插入无收益的不可合并 copy。
3. 标 RS_Spill 防止下一轮再次进入此路径形成无限循环。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| ProperSubClass 检查 | 决定走子类放松还是 lane 子集放松 | 错判会插入无意义 copy |
| bundle 处理 | readsLaneSubset 内对 semi-formed bundle 有特殊判断 | 切分期间 bundle 状态半形成 |
| SM_Size 模式 | 与 SM_Legacy 行为不同 | 切点选择更偏小，可能不够覆盖 |
| 末段 stage | 全标 RS_Spill | 即使中段可分配，remainder 也走溢出 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 子类判定 | `RegisterClassInfo::isProperSubClass` | llvm/lib/CodeGen/RegisterClassInfo.cpp |
| 约束 RC | `MachineInstr::getRegClassConstraintEffectForVReg` | llvm/lib/CodeGen/MachineInstr.cpp |
| lane 读取分析 | `getInstReadLaneMask` | RegAllocGreedy.cpp:1524 |
| copy 识别 | `TargetInstrInfo::isCopyInstr` | llvm/lib/CodeGen/TargetInstrInfo.cpp |
| 切分编辑 | `SplitEditor::openIntv/enterIntvBefore/leaveIntvAfter/useIntv/finish` | llvm/lib/CodeGen/SplitKit.cpp |

---

### 其他补充

此函数仅在 tryLocalSplit 失败后由 trySplit 单块路径调用，作为局部切分的兜底；调用频率远低于 tryLocalSplit。

---

## getNumAllocatableRegsForConstraints 函数分析

### 函数签名与目的（行号 1510-1522）
```cpp
static unsigned getNumAllocatableRegsForConstraints(
    const MachineInstr *MI, Register Reg, const TargetRegisterClass *SuperRC,
    const TargetInstrInfo *TII, const TargetRegisterInfo *TRI,
    const RegisterClassInfo &RCI)
```

**功能**: 给定指令 MI 和寄存器 Reg，在 SuperRC 上应用指令的寄存器类约束（含 bundle），返回约束后剩余可分配寄存器数量。

---

### 整体结构

```
getNumAllocatableRegsForConstraints(MI, Reg, SuperRC, TII, TRI, RCI)
├── assert SuperRC 非空
├── ConstrainedRC = MI->getRegClassConstraintEffectForVReg(Reg, SuperRC, TII, TRI, ExploreBundle=true)
├── ConstrainedRC 为空 -> 返回 0
└── return RCI.getNumAllocatableRegs(ConstrainedRC)
```

---

### 逐段注释

**1. 求约束后 RC (行 1516-1520)**

```cpp
const TargetRegisterClass *ConstrainedRC =
    MI->getRegClassConstraintEffectForVReg(Reg, SuperRC, TII, TRI,
                                           /* ExploreBundle */ true);
if (!ConstrainedRC)
  return 0;
```

把 SuperRC 与 MI 上对 Reg 的所有约束（包括 tied operand、读写约束、bundle 内约束）做交集，得到 ConstrainedRC。若交集为空（约束使 SuperRC 完全不可用）返回 0。

**2. 返回可分配数 (行 1521)**

```cpp
return RCI.getNumAllocatableRegs(ConstrainedRC);
```

用 RegisterClassInfo 查询考虑当前 reserved/used 后该 RC 的可分配数。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `TargetRegisterClass *SuperRC` | - | 输入候选 super class |
| `TargetRegisterClass *ConstrainedRC` | - | 约束后交集 |
| `RegisterClassInfo &RCI` | getNumAllocatableRegs | 考虑 reserved 后的可分配数 |

---

### 优化意图

1. 把"切分后某指令处能否受益于 super class"问题归约为一个整数比较（与 SuperRCNumAllocatableRegs 比较）。
2. ExploreBundle=true 让约束计算覆盖整个 bundle，避免漏算 bundle 内约束。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| SuperRC 非空 | assert | 调用方需保证 |
| 返回 0 语义 | 表示约束使 SuperRC 完全不可用 | 调用方比较时 0 < SuperRCNumAllocatableRegs，会触发"放松"误判 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 约束 RC 计算 | `MachineInstr::getRegClassConstraintEffectForVReg` | llvm/lib/CodeGen/MachineInstr.cpp |
| 可分配数 | `RegisterClassInfo::getNumAllocatableRegs` | llvm/lib/CodeGen/RegisterClassInfo.cpp |

---

### 其他补充

该函数是 tryInstructionSplit 的辅助；调用点用"约束后可分配数 == SuperRCNumAllocatableRegs"判断切分无收益。

---

## getInstReadLaneMask 函数分析

### 函数签名与目的（行号 1524-1551）
```cpp
static LaneBitmask getInstReadLaneMask(const MachineRegisterInfo &MRI,
                                       const TargetRegisterInfo &TRI,
                                       const MachineInstr &FirstMI,
                                       Register Reg)
```

**功能**: 计算 FirstMI（含 bundle）对 Reg 实际读取的 lane mask，用于判断指令是否只读子 lane，从而支持基于 subrange 的切分决策。

---

### 整体结构

```
getInstReadLaneMask(MRI, TRI, FirstMI, Reg)
├── AnalyzeVirtRegInBundle 收集所有相关 operand (MI, OpIdx)
├── for each (MI, OpIdx):
│   ├── 无 SubReg 且 use 且非 undef -> 返回 MaxLaneMask（整寄存器读）
│   ├── SubRegIndexMask = TRI.getSubRegIndexLaneMask(SubReg)
│   ├── def 且非 undef -> Mask |= ~SubRegMask（def 的 lane 视为"不读"）
│   └── use -> Mask |= SubRegMask
└── return Mask
```

---

### 逐段注释

**1. 收集 bundle 内 operand (行 1528-1530)**

```cpp
LaneBitmask Mask;
SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;
(void)AnalyzeVirtRegInBundle(const_cast<MachineInstr &>(FirstMI), Reg, &Ops);
```

`AnalyzeVirtRegInBundle` 跨 bundle 内所有指令返回 Reg 出现的 (MI, OpIdx) 列表。

**2. 遍历 operand 累积 mask (行 1532-1548)**

```cpp
for (auto [MI, OpIdx] : Ops) {
  const MachineOperand &MO = MI->getOperand(OpIdx);
  assert(MO.isReg() && MO.getReg() == Reg);
  unsigned SubReg = MO.getSubReg();
  if (SubReg == 0 && MO.isUse()) {
    if (MO.isUndef())
      continue;
    return MRI.getMaxLaneMaskForVReg(Reg);
  }
  LaneBitmask SubRegMask = TRI.getSubRegIndexLaneMask(SubReg);
  if (MO.isDef()) {
    if (!MO.isUndef())
      Mask |= ~SubRegMask;
  } else
    Mask |= SubRegMask;
}
return Mask;
```

- **无 SubReg 的 use**：读整个寄存器，直接返回 `getMaxLaneMaskForVReg`（最保守也最快），undef 跳过。
- **def 非 undef**：把该 SubReg lane 视为"被定义而不被读"，因此累加其补集 `~SubRegMask` 到读 mask 中（因为同一指令里其他 lane 可能被读，且 def 段不算"读取"）。
- **use**：累加 SubRegMask。
- **undef**：在 def/use 两路都跳过累加，因为 undef 不真正访问寄存器内容。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LaneBitmask` | - | lane 位掩码 |
| `MachineOperand` | SubReg/isDef/isUse/isUndef | operand 属性 |
| `getMaxLaneMaskForVReg` | - | 该 vreg 所有 lane 的并集 |

---

### 优化意图

1. bundle 内多 operand 合并求真实读 mask，避免单 operand 误判。
2. def 路径用补集累加，反映"def 不算读，但同指令其他 lane 仍可能被读"的语义。
3. 整寄存器 use 提前返回，规避逐 lane 累加开销。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| def 补集语义 | def 非 undef 才累加补集 | 错把 undef 累加会引入伪读 lane |
| 无 SubReg use 提前返回 | 必须排除 undef | undef 不应触发整寄存器读 |
| bundle 半形成 | SplitKit 切分过程中 bundle 标志可能孤立 | 上游 readsLaneSubset 已做 isBundled 检查 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| bundle operand 分析 | `AnalyzeVirtRegInBundle` | llvm/lib/CodeGen/MachineInstrBundle.cpp |
| SubReg lane mask | `TargetRegisterInfo::getSubRegIndexLaneMask` | llvm/lib/CodeGen/TargetRegisterInfo.cpp |
| 最大 lane mask | `MachineRegisterInfo::getMaxLaneMaskForVReg` | llvm/lib/CodeGen/MachineRegisterInfo.cpp |

---

### 其他补充

`readsLaneSubset` 调用此函数得到 ReadMask，再与 VirtReg subrange 在 Use 处存活 lane 求交，判断指令是否只读存活 lane 的子集；若是，则切分可放松 lane 约束。

---

## readsLaneSubset 函数分析

### 函数签名与目的（行号 1555-1579）
```cpp
static bool readsLaneSubset(const MachineRegisterInfo &MRI,
                            const MachineInstr *MI, const LiveInterval &VirtReg,
                            const TargetRegisterInfo *TRI, SlotIndex Use,
                            const TargetInstrInfo *TII)
```

**功能**: 判断 MI 在 Use 处读取的 lane 是否是 VirtReg 当时存活 lane 的真子集；若是，则基于 subrange 切分有放松 lane 约束的收益。

---

### 整体结构

```
readsLaneSubset(MRI, MI, VirtReg, TRI, Use, TII)
├── 若是同 SubReg 的非 bundle copy -> 返回 false（无法放松）
├── ReadMask = getInstReadLaneMask(...)
├── LiveAtMask = VirtReg 各 subrange 在 Use 存活 lane 的并集
└── return (ReadMask & ~(LiveAtMask & CoveringLanes)).any()
```

---

### 逐段注释

**1. copy 早退 (行 1562-1565)**

```cpp
auto DestSrc = TII->isCopyInstr(*MI);
if (DestSrc && !MI->isBundled() &&
    DestSrc->Destination->getSubReg() == DestSrc->Source->getSubReg())
  return false;
```

普通非 bundle copy 且 dst/src SubReg 相同时，copy 读写相同 lane 集合，切分不会改变 lane 可用性，提前返回 false。

**2. 读 mask 与存活 mask (行 1568-1574)**

```cpp
LaneBitmask ReadMask = getInstReadLaneMask(MRI, *TRI, *MI, VirtReg.reg());
LaneBitmask LiveAtMask;
for (const LiveInterval::SubRange &S : VirtReg.subranges()) {
  if (S.liveAt(Use))
    LiveAtMask |= S.LaneMask;
}
```

ReadMask 是指令真正读取的 lane；LiveAtMask 是 VirtReg 所有 subrange 在 Use 这一 slot 上存活的 lane 并集。

**3. 子集判定 (行 1578)**

```cpp
return (ReadMask & ~(LiveAtMask & TRI->getCoveringLanes())).any();
```

`LiveAtMask & CoveringLanes` 得到真正"被覆盖、存活"的 lane，取反 `~` 后与 ReadMask 相与：若结果非零，说明 ReadMask 里有 lane 不在存活集合中，即指令读了"非存活 lane"——这等价于 ReadMask 不是 LiveAtMask 的子集，反过来说存活 lane 不是指令读取 lane 的子集……（按代码注释原意：若存活 lane 与指令读取 lane 没有差异，则不放松；返回 true 表示"有差异、可放松"）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveInterval::SubRange` | LaneMask/liveAt() | 每 lane 段 |
| `getCoveringLanes` | - | 覆盖所有 lane 的 mask（用于过滤未占满的复合 lane） |
| `DestSrc` (CopyInstr) | Destination/Source | copy 的 dst/src operand |

---

### 优化意图

1. 用位运算把"是否值得 subrange 切分"压缩成一次 mask 比较。
2. 早退 copy 情形避免无谓分析。
3. 与 `getNumAllocatableRegsForConstraints` 形成"子类放松 / lane 放松"双重 skip 守门。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 半形成 bundle | SplitKit 切分中 bundle 标志孤立 | 用 `!MI->isBundled()` 过滤 |
| CoveringLanes 语义 | 过滤掉"伪 lane" | 不与会导致误判存活集合 |
| 返回语义 | true 表示"可放松" | 调用方 tryInstructionSplit 用 `!readsLaneSubset` 作 skip 条件 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 读 mask | `getInstReadLaneMask` | RegAllocGreedy.cpp:1524 |
| copy 识别 | `TargetInstrInfo::isCopyInstr` | llvm/lib/CodeGen/TargetInstrInfo.cpp |
| subrange 存活 | `LiveInterval::SubRange::liveAt` | llvm/lib/CodeGen/LiveInterval.cpp |
| 覆盖 lane | `TargetRegisterInfo::getCoveringLanes` | llvm/lib/CodeGen/TargetRegisterInfo.cpp |

---

### 其他补充

FIXME 注释提到当前只考虑 use，未来应也考虑 def；这是已知的保守点，可能导致漏掉部分可放松场景。

---

## tryBlockSplit 函数分析

### 函数签名与目的（行号 1466-1502）
```cpp
MCRegister RAGreedy::tryBlockSplit(const LiveInterval &VirtReg,
                                   AllocationOrder &Order,
                                   SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 把全局 VirtReg 按使用块隔离，每个使用块（必要时单指令）切出独立 interval，产生大量局部 live range 供后续回合再分配；remainder 走溢出。

---

### 整体结构

```
tryBlockSplit(VirtReg, Order, NewVRegs)
├── assert SA->getParent == VirtReg
├── SingleInstrs = isProperSubClass
├── 建 LREdit + SE->reset(SplitSpillMode)
├── for BI in UseBlocks:
│   └── shouldSplitSingleBlock(BI, SingleInstrs) -> SE->splitSingleBlock(BI)
├── LREdit.empty() -> 返回
├── SE->finish(&IntvMap)
├── DebugVars->splitRegister
└── for new vreg: 若 stage==RS_New 且 IntvMap==0 -> setStage RS_Spill
```

---

### 逐段注释

**1. 准备 (行 1469-1474)**

```cpp
assert(&SA->getParent() == &VirtReg && "Live range wasn't analyzed");
Register Reg = VirtReg.reg();
bool SingleInstrs = RegClassInfo.isProperSubClass(MRI->getRegClass(Reg));
LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
SE->reset(LREdit, SplitSpillMode);
ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
```

assert 要求 SA 已对 VirtReg 做过分析。SingleInstrs 表示 CurRC 是某 super class 真子类时，每个块进一步按单指令切，便于挪到更宽类。`SE->reset` 用 SplitSpillMode，意味着 remainder 默认走溢出语义。

**2. 遍历使用块切分 (行 1475-1478)**

```cpp
for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
  if (SA->shouldSplitSingleBlock(BI, SingleInstrs))
    SE->splitSingleBlock(BI);
}
```

`shouldSplitSingleBlock` 决定该块是否要被独立切开（基于 SingleInstrs 与块内指令数等）。`splitSingleBlock` 把整块作为一个 interval 切出。

**3. 空切分早退 (行 1480-1481)**

```cpp
if (LREdit.empty())
  return MCRegister();
```

所有块都不值得切时直接返回，下游走溢出。

**4. 收尾与 stage 标注 (行 1484-1501)**

```cpp
SmallVector<unsigned, 8> IntvMap;
SE->finish(&IntvMap);
DebugVars->splitRegister(Reg, LREdit.regs(), *LIS);
for (unsigned I = 0, E = LREdit.size(); I != E; ++I) {
  const LiveInterval &LI = LIS->getInterval(LREdit.get(I));
  if (ExtraInfo->getOrInitStage(LI.reg()) == RS_New && IntvMap[I] == 0)
    ExtraInfo->setStage(LI, RS_Spill);
}
if (VerifyEnabled)
  MF->verify(LIS, Indexes, "After splitting live range around basic blocks",
             &errs());
return MCRegister();
```

`IntvMap[I]==0` 表示该 vreg 属于 remainder（未被任何块 interval 覆盖的部分），且 stage 仍是 RS_New 时直接标 RS_Spill。新切出的块 interval 保持 RS_New，下轮作为局部区间走 tryLocalSplit/tryInstructionSplit。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SplitAnalysis::BlockInfo` | MBB/FirstInstr/LastInstr | 单块使用信息 |
| `LiveRangeEdit LREdit` | - | 切分产物 |
| `IntvMap[i]` | interval id | 0=remainder, >0=被某块 interval 覆盖 |
| `SplitSpillMode` | - | remainder 走溢出语义 |

---

### 优化意图

1. 全局区间无法整体分配时，按块切分让局部段获得再分配机会。
2. SingleInstrs 模式下进一步细化到单指令，配合寄存器类放松。
3. remainder 直接标 RS_Spill 避免再走分裂路径浪费。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| SA 必须已分析 | assert 检查 | 未分析直接调用会崩溃 |
| IntvMap==0 判定 | 仅对 RS_New remainder 标 Spill | 若 remainder 已是更高 stage 会被跳过 |
| SingleInstrs 阈值 | shouldSplitSingleBlock 内部判定 | 块过小时切分无收益 |
| VerifyEnabled | 调用 MF->verify | 仅 debug 模式开销 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单块切分 | `SplitEditor::splitSingleBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| 是否单块切 | `SplitAnalysis::shouldSplitSingleBlock` | llvm/lib/CodeGen/SplitKit.cpp |
| finish | `SplitEditor::finish` | llvm/lib/CodeGen/SplitKit.cpp |
| DebugVars | `LiveDebugVariables::splitRegister` | llvm/lib/CodeGen/LiveDebugVariables.cpp |
| verify | `MachineFunction::verify` | llvm/lib/CodeGen/MachineVerifier.cpp |

---

### 其他补充

`tryBlockSplit` 总是返回 `MCRegister()`（不直接分配），通过 NewVRegs 把新局部区间交回主循环；这是与 tryRegionSplit/tryLocalSplit 的关键区别——它只是"细分"而非"分配"。
<!-- Group F: Region Split Entry & Last Chance Recoloring -->

## tryRegionSplit 函数分析

### 函数签名与目的（行号 1203-1234）
```cpp
MCRegister RAGreedy::tryRegionSplit(const LiveInterval &VirtReg,
                                    AllocationOrder &Order,
                                    SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 区域分裂的入口函数。先尝试紧凑区域分裂，再遍历物理寄存器寻找最优分裂候选；若均失败则回退到单块分裂。返回所选物理寄存器或 `NoRegister`。

---

### 整体结构

```
tryRegionSplit(VirtReg, Order, NewVRegs)
├── 1. 通过 shouldRegionSplitForVirtReg 守门检查是否允许区域分裂
├── 2. calcBlockSplitCost 计算按块分裂基线代价 SpillCost
├── 3. calcCompactRegion 尝试构建紧凑区域候选 GlobalCand[0]
│   ├── 有紧凑区域: NumCands=1, BestCost=max（待 calculateRegionSplitCost 提升）
│   └── 无紧凑区域: BestCost=SpillCost（回退基线）
├── 4. calculateRegionSplitCost 遍历 Order 找最优候选 BestCand
├── 5. 若无紧凑区域且 BestCand==NoCand → 返回 NoRegister
└── doRegionSplit(VirtReg, BestCand, HasCompact, NewVRegs)
```

---

### 逐段注释

**1. 守门与基线代价 (行号 1206-1209)**

```cpp
if (!TRI->shouldRegionSplitForVirtReg(*MF, VirtReg))
  return MCRegister::NoRegister;
unsigned NumCands = 0;
BlockFrequency SpillCost = calcBlockSplitCost();
BlockFrequency BestCost;
```

由 TargetRegisterInfo 决定是否对当前虚拟寄存器启用区域分裂；不启用则直接返回。同时计算按块分裂代价作为兜底比较基准。

**2. 紧凑区域候选 (行号 1213-1224)**

```cpp
bool HasCompact = calcCompactRegion(GlobalCand.front());
if (HasCompact) {
  NumCands = 1;
  BestCost = BlockFrequency::max();
} else {
  BestCost = SpillCost;
  LLVM_DEBUG(dbgs() << "Cost of isolating all blocks = "
                    << printBlockFreq(*MBFI, BestCost) << '\n');
}
```

紧凑区域不依赖具体物理寄存器，是删除 through-blocks 后形成的天然区域。若存在紧凑区域，把它保留为 `GlobalCand[0]`，并把 `BestCost` 设为最大值，迫使后续 `calculateRegionSplitCost` 给出比紧凑区域更好的方案才更新；否则把 `BestCost` 设为按块分裂代价，要求后续必须严格优于全块 spill 才采纳。

**3. 计算最优候选与回退 (行号 1226-1234)**

```cpp
unsigned BestCand = calculateRegionSplitCost(VirtReg, Order, BestCost,
                                             NumCands, false /*IgnoreCSR*/);
if (!HasCompact && BestCand == NoCand)
  return MCRegister::NoRegister;
return doRegionSplit(VirtReg, BestCand, HasCompact, NewVRegs);
```

`IgnoreCSR=false` 表示候选物理寄存器中包含被使用过的 callee-saved 寄存器。如果既没有紧凑区域、`calculateRegionSplitCost` 也没找到比 `SpillCost` 更优的候选，则放弃区域分裂交还上层（最终会落到 block split 或 spill）。否则调用 `doRegionSplit` 真正执行分裂。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `GlobalCand` (SmallVector<GlobalSplitCandidate>) | `front()` | 紧凑区域候选槽位，PhysReg 为 NoRegister |
| `BlockFrequency` | BestCost / SpillCost | 当前最优代价与按块分裂基线代价 |
| `AllocationOrder` | Order | 物理寄存器分配顺序迭代器 |

---

### 优化意图

1. 优先尝试不依赖具体物理寄存器的紧凑区域，避免无谓的 interference 查询。
2. 用按块分裂代价作为门槛，保证区域分裂严格优于全块 spill 才采纳，防止劣化。
3. 紧凑区域与最优物理寄存器候选可以共存（`doRegionSplit` 同时使用两者）。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| HasCompact 与 BestCand 互斥语义 | 无紧凑区域且 BestCand==NoCand 才回退 | 若逻辑颠倒会错误放弃有效候选 |
| IgnoreCSR=false | 入口路径不跳过 CSR | 与某些 secondary 路径语义不同，注意调用方期望 |
| GlobalCand 复用 | `front()` 是全局共享槽 | 多次调用前必须由 calcCompactRegion 重置 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 守门 | `TRI->shouldRegionSplitForVirtReg` | TargetRegisterInfo |
| 紧凑区域 | `RAGreedy::calcCompactRegion` | RegAllocGreedy.cpp:948 |
| 按块代价 | `RAGreedy::calcBlockSplitCost` | RegAllocGreedy.cpp:991 |
| 最优候选 | `RAGreedy::calculateRegionSplitCost` | RegAllocGreedy.cpp:1313 |
| 执行分裂 | `RAGreedy::doRegionSplit` | RegAllocGreedy.cpp:1331 |

---

### 其他补充

`tryRegionSplit` 是 `selectOrSplit` 主路径在普通分裂前的尝试，与 `tryBlockSplit`、`tryLocalSplit` 形成分裂层次。`HasCompact` 为真时即使 `BestCand==NoCand` 也会进入 `doRegionSplit`（紧凑区域单独成立），这是回退条件 `!HasCompact && BestCand==NoCand` 的关键。

---

## calcBlockSplitCost 函数分析

### 函数签名与目的（行号 991-1004）
```cpp
BlockFrequency RAGreedy::calcBlockSplitCost()
```

**功能**: 计算把当前 live range 按"每个使用块单独分裂"方式处理的 spill 代价，作为区域分裂方案的下界基线。

---

### 整体结构

```
calcBlockSplitCost()
├── Cost = 0
├── for each UseBlock BI:
│   ├── Cost += getBlockFrequency(Number)        // 一条 load 或 store
│   └── if LiveIn && LiveOut && FirstDef:
│       └── Cost += getBlockFrequency(Number)    // 额外 spill
└── return Cost
```

---

### 逐段注释

**1. 遍历使用块累计代价 (行号 992-1002)**

```cpp
BlockFrequency Cost = BlockFrequency(0);
ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
  unsigned Number = BI.MBB->getNumber();
  Cost += SpillPlacer->getBlockFrequency(Number);
  if (BI.LiveIn && BI.LiveOut && BI.LiveOut && BI.FirstDef)
    Cost += SpillPlacer->getBlockFrequency(Number);
}
return Cost;
```

对每个使用块默认计一条 spill 指令代价（进入或离开块各一）。若块同时 LiveIn、LiveOut 且 `FirstDef`（块内首次定义），说明值在块内被重定义，必须先 store 再 load，因此再加一条。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SplitAnalysis::BlockInfo` | MBB/LiveIn/LiveOut/FirstDef | 单个使用块的活跃与定义信息 |
| `BlockFrequency` | Cost | 累计的块频次代价 |

---

### 优化意图

1. 用最朴素的按块分裂代价作为区域分裂的"必须严格优于"基线，避免区域分裂反而劣化。
2. 简化模型：只算 spill 指令数，不计 COPY 与 remat，得到保守上界。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| FirstDef 三条件并存 | 必须同时 LiveIn+LiveOut+FirstDef 才加额外代价 | 漏判 FirstDef 会低估 |
| 依赖 SA 当前状态 | SA 必须已对当前 VirtReg 分析完成 | 调用前需保证 SA 已 prepare |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 使用块列表 | `SA->getUseBlocks` | SplitAnalysis |
| 块频次 | `SpillPlacer->getBlockFrequency` | SpillPlacement |

---

### 其他补充

`SpillPlacer->getBlockFrequency` 返回的是该块归一化的执行频次，单位与 `BlockFrequency` 一致，便于和 `calcGlobalSplitCost`、`calcCompactRegion` 的代价直接比较。

---

## calcCompactRegion 函数分析

### 函数签名与目的（行号 948-987）
```cpp
bool RAGreedy::calcCompactRegion(GlobalSplitCandidate &Cand)
```

**功能**: 在不参考具体物理寄存器干扰的前提下，把当前 live range 中所有 through-blocks（仅 live-through、无使用/定义的块）剔除，计算由剩余 edge bundle 组成的"紧凑区域"。返回是否得到非空紧凑区域。

---

### 整体结构

```
calcCompactRegion(Cand)
├── 1. 无 through-blocks → 已紧凑，返回 false
├── 2. Cand.reset(NoRegister)；SpillPlacer->prepare
├── 3. addSplitConstraints → 若无可行约束返回 false
├── 4. growRegion → 若无法 spill 所有干扰返回 false
├── 5. SpillPlacer->finish
├── 6. Cand.LiveBundles.any() 为空 → 返回 false
└── 返回 true
```

---

### 逐段注释

**1. through-blocks 守门 (行号 950-951)**

```cpp
if (!SA->getNumThroughBlocks())
  return false;
```

没有 through-blocks 说明 live range 本身已紧凑，无需也无法再压缩，直接返回 false 表示"无紧凑区域可生成"。

**2. 候选初始化与 spill placer 准备 (行号 954-960)**

```cpp
Cand.reset(IntfCache, MCRegister::NoRegister);
LLVM_DEBUG(dbgs() << "Compact region bundles");
SpillPlacer->prepare(Cand.LiveBundles);
```

`Cand.PhysReg=NoRegister` 是关键标志：`growRegion` 会据此把所有 through-blocks 当作有干扰处理，从而把它们排除在 LiveBundles 之外，形成紧凑区域。

**3. 约束与生长 (行号 963-972)**

```cpp
BlockFrequency Cost;
if (!addSplitConstraints(Cand.Intf, Cost)) {
  LLVM_DEBUG(dbgs() << ", none.\n");
  return false;
}
if (!growRegion(Cand)) {
  LLVM_DEBUG(dbgs() << ", cannot spill all interferences.\n");
  return false;
}
SpillPlacer->finish();
```

`addSplitConstraints` 用 spill placer 求解哪些 bundle 必须放置 spill 代码；`growRegion` 在该约束上扩展 LiveBundles，使剩余 bundle 都满足约束。任一步失败都说明紧凑区域不可行。

**4. 非空校验与返回 (行号 976-986)**

```cpp
if (!Cand.LiveBundles.any()) {
  LLVM_DEBUG(dbgs() << ", none.\n");
  return false;
}
return true;
```

最终若没有任何 bundle 被选中，紧凑区域退化为空，等价于无紧凑区域。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `GlobalSplitCandidate` | PhysReg/LiveBundles/Intf | 紧凑区域候选，PhysReg=NoRegister 是信号 |
| `BitVector` | LiveBundles | 选中的 edge bundle 集合 |
| `InterferenceCache` | Cand.Intf | 干扰查询游标，紧凑模式下视为全干扰 |

---

### 优化意图

1. 不查询具体物理寄存器干扰即可得到一个通用紧凑区域，作为区域分裂的兜底候选。
2. 利用 `PhysReg=NoRegister` 让 `growRegion` 把 through-blocks 全部当作干扰，从而自动剔除它们。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| PhysReg 必须为 NoRegister | 是 growRegion 的特殊语义信号 | 误设物理寄存器会破坏紧凑语义 |
| 返回 false 含义双重 | 既可能"已紧凑"也可能"无法构造" | 调用方需结合 NumThroughBlocks 区分 |
| IntfCache 共享 | Cand.Intf 使用 IntfCache 游标 | 候选数受 IntfCache 容量限制 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| through 数 | `SA->getNumThroughBlocks` | SplitAnalysis |
| 约束求解 | `RAGreedy::addSplitConstraints` | RegAllocGreedy.cpp |
| 区域生长 | `RAGreedy::growRegion` | RegAllocGreedy.cpp |
| spill placer | `SpillPlacer->prepare/finish` | SpillPlacement |

---

### 其他补充

紧凑区域与具体物理寄存器候选在 `doRegionSplit` 中可同时启用：物理寄存器候选覆盖的 bundle 用该寄存器分配，紧凑区域覆盖的 bundle 由后续 selectOrSplit 处理（可能落到不同物理寄存器或 spill）。

---

## calculateRegionSplitCost 函数分析

### 函数签名与目的（行号 1313-1329）
```cpp
unsigned RAGreedy::calculateRegionSplitCost(const LiveInterval &VirtReg,
                                            AllocationOrder &Order,
                                            BlockFrequency &BestCost,
                                            unsigned &NumCands,
                                            bool IgnoreCSR)
```

**功能**: 遍历 `Order` 中的每个物理寄存器，调用 `calculateRegionSplitCostAroundReg` 评估其作为区域分裂候选的代价，并维护最优候选下标 `BestCand`。返回最优候选在 `GlobalCand` 中的下标（或 `NoCand`）。

---

### 整体结构

```
calculateRegionSplitCost(VirtReg, Order, BestCost, NumCands, IgnoreCSR)
├── BestCand = NoCand
├── for PhysReg in Order:
│   ├── if IgnoreCSR && isUnusedCalleeSavedReg(PhysReg): continue
│   └── calculateRegionSplitCostAroundReg(PhysReg, Order, BestCost, NumCands, BestCand)
└── return BestCand
```

---

### 逐段注释

**1. 遍历物理寄存器 (行号 1318-1326)**

```cpp
unsigned BestCand = NoCand;
for (MCRegister PhysReg : Order) {
  assert(PhysReg);
  if (IgnoreCSR && EvictAdvisor->isUnusedCalleeSavedReg(PhysReg))
    continue;
  calculateRegionSplitCostAroundReg(PhysReg, Order, BestCost, NumCands,
                                    BestCand);
}
return BestCand;
```

`BestCand` 在外层初始化为 `NoCand`，由 `calculateRegionSplitCostAroundReg` 通过引用参数更新。`IgnoreCSR` 控制是否跳过"未被使用过的 callee-saved 寄存器"——`tryRegionSplit` 入口传 false（允许 CSR），而某些 secondary 路径传 true 以避免污染 CSR。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AllocationOrder` | Order | 物理寄存器候选顺序 |
| `BlockFrequency &` | BestCost | 当前最优代价（引用更新） |
| `unsigned &` | NumCands / BestCand | 候选数与最优下标（引用更新） |

---

### 优化意图

1. 把"遍历 Order"与"评估单个 PhysReg"解耦，便于不同入口复用单候选评估逻辑。
2. 通过 `IgnoreCSR` 参数支持不同路径对 CSR 的策略差异。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| BestCost/NumCands/BestCand 均为引用 | 调用前后状态语义连续 | 调用方需正确初始化 |
| PhysReg 非空断言 | Order 不应返回 NoRegister | 自定义 AllocationOrder 需保证 |
| IgnoreCSR 语义 | 仅跳过"未使用"的 CSR | 已使用 CSR 仍会评估 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单候选评估 | `RAGreedy::calculateRegionSplitCostAroundReg` | RegAllocGreedy.cpp:1236 |
| CSR 判定 | `EvictAdvisor->isUnusedCalleeSavedReg` | RegAllocEvictAdvisor |

---

### 其他补充

`BestCost` 由调用方预设为基线（紧凑区域时为 max，否则为 SpillCost），`calculateRegionSplitCostAroundReg` 内部会与 `BestCost` 比较并在更优时更新 `BestCand` 与 `BestCost`，因此该函数本身不直接决定采纳与否。

---

## tryLastChanceRecoloring 函数分析

### 函数签名与目的（行号 2129-2279）
```cpp
MCRegister RAGreedy::tryLastChanceRecoloring(
    const LiveInterval &VirtReg, AllocationOrder &Order,
    SmallVectorImpl<Register> &NewVRegs, SmallVirtRegSet &FixedRegisters,
    RecoloringStack &RecolorStack, unsigned Depth)
```

**功能**: 最后手段重着色。为无法直接分配的 `VirtReg` 选一个物理寄存器，把占用该寄存器（及其 alias）的所有干扰虚拟寄存器递归重着色；失败则回滚所有改动。返回成功分配的物理寄存器或 `~0u`。

---

### 整体结构

```
tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters, RecolorStack, Depth)
├── 1. shouldUseLastChanceRecoloringForVirtReg 守门
├── 2. 记录 EntryStackSize = RecolorStack.size()
├── 3. 递归深度检查 (Depth >= LastChanceRecoloringMaxDepth)
├── 4. 把 VirtReg 标记为 Fixed
├── 5. for PhysReg in Order:
│   ├── 5a. checkInterference > IK_VirtReg → 跳过（含固定干扰）
│   ├── 5b. mayRecolorAllInterferences 检查所有干扰可重着色
│   ├── 5c. 构造 RecoloringQueue，记录原分配到 RecolorStack，Matrix->unassign
│   ├── 5d. Matrix->assign(VirtReg, PhysReg)
│   ├── 5e. 保存 SaveFixedRegisters
│   ├── 5f. tryRecoloringCandidates
│   │   ├── 成功 → append NewVRegs，unassign VirtReg，返回 PhysReg
│   │   └── VirtReg 被删除 → 返回 MCRegister()
│   └── 5g. 失败回滚: 恢复 FixedRegisters、unassign VirtReg、还原 CurrentNewVRegs、还原 RecolorStack
└── 返回 ~0u
```

---

### 逐段注释

**1. 守门与调试输出 (行号 2133-2136)**

```cpp
if (!TRI->shouldUseLastChanceRecoloringForVirtReg(*MF, VirtReg))
  return ~0u;
LLVM_DEBUG(dbgs() << "Try last chance recoloring for " << VirtReg << '\n');
```

由 target 决定是否对当前 VirtReg 启用最后手段重着色。`~0u` 是失败哨兵值。

**2. 入口栈深度与递归深度检查 (行号 2138-2151)**

```cpp
const ssize_t EntryStackSize = RecolorStack.size();
assert((ExtraInfo->getStage(VirtReg) >= RS_Done || !VirtReg.isSpillable()) &&
       "Last chance recoloring should really be last chance");
if (Depth >= LastChanceRecoloringMaxDepth && !ExhaustiveSearch) {
  LLVM_DEBUG(dbgs() << "Abort because max depth has been reached.\n");
  CutOffInfo |= CO_Depth;
  return ~0u;
}
```

`EntryStackSize` 是后续回滚的基准点。断言确保 VirtReg 已处于 RS_Done 阶段或不可 spill——最后手段确为"最后"。深度超过 `LastChanceRecoloringMaxDepth`（非穷举模式）立即终止并设置 `CO_Depth` 截断标志。

**3. 标记 VirtReg 为 Fixed 并准备候选集合 (行号 2154-2160)**

```cpp
SmallLISet RecoloringCandidates;
assert(!FixedRegisters.count(VirtReg.reg()));
FixedRegisters.insert(VirtReg.reg());
SmallVector<Register, 4> CurrentNewVRegs;
```

把 VirtReg 加入 FixedRegisters，使其在本次"session"内不会被递归重着色——避免 vC→vA→vC 死循环。`RecoloringCandidates` 收集需要重着色的干扰区间，`CurrentNewVRegs` 收集本次尝试产生的新虚拟寄存器。

**4. 遍历物理寄存器候选 (行号 2162-2184)**

```cpp
for (MCRegister PhysReg : Order) {
  assert(PhysReg.isValid());
  LLVM_DEBUG(dbgs() << "Try to assign: " << VirtReg << " to "
                    << printReg(PhysReg, TRI) << '\n');
  RecoloringCandidates.clear();
  CurrentNewVRegs.clear();
  if (Matrix->checkInterference(VirtReg, PhysReg) >
      LiveRegMatrix::IK_VirtReg) {
    LLVM_DEBUG(
        dbgs() << "Some interferences are not with virtual registers.\n");
    continue;
  }
  if (!mayRecolorAllInterferences(PhysReg, VirtReg, RecoloringCandidates,
                                  FixedRegisters)) {
    LLVM_DEBUG(dbgs() << "Some interferences cannot be recolored.\n");
    continue;
  }
```

每个 PhysReg 重新清空候选集合。先做快速筛选：若干扰级别高于 `IK_VirtReg`（即存在固定/物理寄存器干扰），无法通过重着色虚拟寄存器解决，跳过。然后 `mayRecolorAllInterferences` 详细检查所有虚拟干扰是否都可能被重着色，并把它们填入 `RecoloringCandidates`。

**5. 重着色队列构造与原分配记录 (行号 2189-2206)**

```cpp
PQueue RecoloringQueue;
for (const LiveInterval *RC : RecoloringCandidates) {
  Register ItVirtReg = RC->reg();
  enqueue(RecoloringQueue, RC);
  assert(VRM->hasPhys(ItVirtReg) &&
         "Interferences are supposed to be with allocated variables");
  RecolorStack.push_back(std::make_pair(RC, VRM->getPhys(ItVirtReg)));
  Matrix->unassign(*RC);
}
Matrix->assign(VirtReg, PhysReg);
```

对每个干扰区间：入队待重着色、把原 `(LiveInterval, PhysReg)` 压入 `RecolorStack` 供回滚、`Matrix->unassign` 释放其占用。然后立即把 VirtReg 分配到 PhysReg，使后续递归重着色能看到正确的可用寄存器状态。

**6. 执行递归重着色与成功路径 (行号 2209-2231)**

```cpp
Register ThisVirtReg = VirtReg.reg();
SmallVirtRegSet SaveFixedRegisters(FixedRegisters);
if (tryRecoloringCandidates(RecoloringQueue, CurrentNewVRegs,
                            FixedRegisters, RecolorStack, Depth)) {
  llvm::append_range(NewVRegs, CurrentNewVRegs);
  if (VRM->hasPhys(ThisVirtReg)) {
    Matrix->unassign(VirtReg);
    return PhysReg;
  }
  LLVM_DEBUG(dbgs() << "tryRecoloringCandidates deleted a fixed register "
                    << printReg(ThisVirtReg) << '\n');
  FixedRegisters.erase(ThisVirtReg);
  return MCRegister();
}
```

先保存 `ThisVirtReg`（区间可能在递归中被删除）、保存 FixedRegisters 副本。`tryRecoloringCandidates` 成功则把新 vreg 合并入 `NewVRegs`。此时若 VirtReg 仍存在物理寄存器（即递归过程中未被删除），先 `unassign` 它再返回 PhysReg——交回主分配流程重新正式分配。若 VirtReg 已被删除（被分裂/替换），从 FixedRegisters 移除并返回空 `MCRegister()` 表示"无需再分配"。

**7. 失败回滚 (行号 2233-2275)**

```cpp
LLVM_DEBUG(dbgs() << "Fail to assign: " << VirtReg << " to "
                  << printReg(PhysReg, TRI) << '\n');
FixedRegisters = SaveFixedRegisters;
Matrix->unassign(VirtReg);
for (Register R : CurrentNewVRegs) {
  if (RecoloringCandidates.count(&LIS->getInterval(R)))
    continue;
  NewVRegs.push_back(R);
}
for (ssize_t I = RecolorStack.size() - 1; I >= EntryStackSize; --I) {
  const LiveInterval *LI;
  MCRegister PhysReg;
  std::tie(LI, PhysReg) = RecolorStack[I];
  if (VRM->hasPhys(LI->reg()))
    Matrix->unassign(*LI);
}
for (size_t I = EntryStackSize; I != RecolorStack.size(); ++I) {
  const LiveInterval *LI;
  MCRegister PhysReg;
  std::tie(LI, PhysReg) = RecolorStack[I];
  if (!LI->empty() && !MRI->reg_nodbg_empty(LI->reg()))
    Matrix->assign(*LI, PhysReg);
}
RecolorStack.resize(EntryStackSize);
```

回滚分四步：
1. 恢复 FixedRegisters 到本 PhysReg 尝试前的状态；
2. `unassign` VirtReg 当前 PhysReg；
3. 处理 `CurrentNewVRegs`：属于 `RecoloringCandidates` 的新 vreg（即被分裂自候选区间）会被随原区间一起还原，不加入 `NewVRegs`；其余新 vreg 是 `selectOrSplit` 产生的，需交给主流程，加入 `NewVRegs`；
4. 还原 `RecolorStack` 中本次压入的所有区间：先全部 unassign，再全部按原 PhysReg 重新 assign。两阶段是为了避免子重着色引入的冲突与即将还原的分配相互冲突。最后 `resize` 截断栈到入口深度。

**8. 全部失败 (行号 2277-2278)**

```cpp
return ~0u;
```

所有 PhysReg 都尝试失败，返回 `~0u` 哨兵交回上层处理（通常落到 spill）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RecoloringStack` | `vector<pair<const LiveInterval*, MCRegister>>` | 成功/失败回滚所需的原分配记录 |
| `SmallLISet` | RecoloringCandidates | 需要重着色的干扰区间集合 |
| `SmallVirtRegSet` | FixedRegisters | 本 session 内不可再重着色的 vreg 集合 |
| `PQueue` | RecoloringQueue | 待重着色区间的工作队列 |
| `unsigned` | Depth | 递归深度，受 LastChanceRecoloringMaxDepth 限制 |

---

### 优化意图

1. 通过 FixedRegisters 打破递归重着色环，使 vC→vA→vB→vC 这类循环依赖可解。
2. RecolorStack + EntryStackSize 提供精确的回滚点，使失败尝试不污染全局分配状态。
3. 两阶段 unassign/assign 回滚避免子重着色与还原分配间的瞬时冲突。
4. `CutOffInfo` 记录深度/干扰截断原因，供外层诊断与策略调整。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| VirtReg 必须 RS_Done 或不可 spill | 断言强制"最后手段"语义 | 提前调用会断言失败 |
| FixedRegisters 必须包含 VirtReg | 防止递归重着色自身 | 漏插入会死循环 |
| 回滚两阶段顺序 | 先全部 unassign 再 assign | 顺序错会触发 alias 冲突 |
| CurrentNewVRegs 中候选分裂产物 | 不加入 NewVRegs | 误加会导致重复分配 |
| VirtReg 可能被删除 | 用 ThisVirtReg 缓存 reg() | 直接使用引用会 UAF |
| ~0u 与 MCRegister() 语义不同 | ~0u=失败，空=已处理 | 调用方需区分 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| target 守门 | `TRI->shouldUseLastChanceRecoloringForVirtReg` | TargetRegisterInfo |
| 干扰检查 | `Matrix->checkInterference` | LiveRegMatrix |
| 可重着色检查 | `RAGreedy::mayRecolorAllInterferences` | RegAllocGreedy.cpp:2042 |
| 递归重着色 | `RAGreedy::tryRecoloringCandidates` | RegAllocGreedy.cpp:2281+ |
| 分配/释放 | `Matrix->assign / Matrix->unassign` | LiveRegMatrix |
| 入队 | `RAGreedy::enqueue` | RegAllocGreedy.cpp |

---

### 其他补充

`LastChanceRecoloringMaxDepth` 与 `LastChanceRecoloringMaxInterference` 是控制搜索空间的两道闸门，分别由 `CO_Depth`、`CO_Interf` 在 `CutOffInfo` 中记录截断原因。`ExhaustiveSearch` 模式下两道闸门都放开，用于调试或小函数强制求解。成功路径返回前会 `Matrix->unassign(VirtReg)`，目的是让主分配流程重新走 `selectOrSplit` 正式分配，从而正确触发 hint、split 等后续逻辑。

---

## mayRecolorAllInterferences 函数分析

### 函数签名与目的（行号 2042-2084）
```cpp
bool RAGreedy::mayRecolorAllInterferences(
    MCRegister PhysReg, const LiveInterval &VirtReg,
    SmallLISet &RecoloringCandidates, const SmallVirtRegSet &FixedRegisters)
```

**功能**: 检查 `VirtReg` 在 `PhysReg`（及其 alias）上的所有虚拟寄存器干扰是否都"可能被重着色"。若全部可能，把候选加入 `RecoloringCandidates` 并返回 true；只要有一个不可重着色就提前返回 false。

---

### 整体结构

```
mayRecolorAllInterferences(PhysReg, VirtReg, RecoloringCandidates, FixedRegisters)
├── CurRC = MRI->getRegClass(VirtReg.reg())
├── for Unit in TRI->regunits(PhysReg):
│   ├── Q = Matrix->query(VirtReg, Unit)
│   ├── if interferingVRegs(MaxInt).size() >= MaxInt && !ExhaustiveSearch:
│   │   └── CutOffInfo |= CO_Interf; return false
│   └── for Intf in reverse(Q.interferingVRegs()):
│       ├── 不可重着色判定 → return false
│       └── RecoloringCandidates.insert(Intf)
└── return true
```

---

### 逐段注释

**1. 干扰数量早退 (行号 2045-2057)**

```cpp
const TargetRegisterClass *CurRC = MRI->getRegClass(VirtReg.reg());
for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
  LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
  if (Q.interferingVRegs(LastChanceRecoloringMaxInterference).size() >=
          LastChanceRecoloringMaxInterference &&
      !ExhaustiveSearch) {
    LLVM_DEBUG(dbgs() << "Early abort: too many interferences.\n");
    CutOffInfo |= CO_Interf;
    return false;
  }
```

对 PhysReg 的每个 regunit 查询干扰。`interferingVRegs(N)` 最多返回 N 个，若达到上限说明干扰过多，逐一重着色代价太高，直接截断并设置 `CO_Interf`。

**2. 逐干扰可重着色判定 (行号 2058-2081)**

```cpp
  for (const LiveInterval *Intf : reverse(Q.interferingVRegs())) {
    if (((ExtraInfo->getStage(*Intf) == RS_Done &&
          MRI->getRegClass(Intf->reg()) == CurRC &&
          !assignedRegPartiallyOverlaps(*TRI, *VRM, PhysReg, *Intf)) &&
         !(hasTiedDef(MRI, VirtReg.reg()) &&
           !hasTiedDef(MRI, Intf->reg()))) ||
        FixedRegisters.count(Intf->reg())) {
      LLVM_DEBUG(
          dbgs() << "Early abort: the interference is not recolorable.\n");
      return false;
    }
    RecoloringCandidates.insert(Intf);
  }
}
return true;
```

`reverse` 遍历是为了优先处理较晚分配的干扰（更可能被重着色）。判定逻辑：
- **不可重着色**：Intf 已 RS_Done 且寄存器类相同（与 VirtReg 处于同等状态，重着色结果不会更好）且当前分配不与 PhysReg 部分重叠（无法靠 tuple 换位重着色），并且不满足"VirtReg 有 tied def 而 Intf 没有"的特例；
- 或 Intf 已在 FixedRegisters 中（本 session 内锁定）。
- 满足任一条件即不可重着色，立即返回 false。
- 否则把 Intf 加入 `RecoloringCandidates`。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SmallLISet &` | RecoloringCandidates | 输出：可重着色干扰集合 |
| `SmallVirtRegSet` | FixedRegisters | 本 session 锁定的 vreg 集合 |
| `LiveIntervalUnion::Query` | Q | 单 regunit 上的干扰查询 |

---

### 优化意图

1. 用 `LastChanceRecoloringMaxInterference` 提前剪枝，避免在干扰密集的寄存器上浪费递归开销。
2. 通过"同寄存器类 + RS_Done"判定识别"重着色无意义"的干扰，减少无效递归。
3. 两个特例（tied def、部分重叠）保留少量可能的重着色机会，提升求解率。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| RecoloringCandidates 跨 Unit 累积 | 同一 Intf 可能在多 Unit 出现 | SmallLISet 去重保证唯一 |
| reverse 遍历顺序 | 影响后续重着色队列顺序 | 改动会影响求解结果与性能 |
| CutOffInfo 副作用 | 设置 CO_Interf 后会影响外层策略 | 非穷举模式才设置 |
| 同寄存器类判定 | 用 CurRC 比较 | 子寄存器类场景需注意 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| regunit 迭代 | `TRI->regunits` | TargetRegisterInfo |
| 干扰查询 | `Matrix->query` | LiveRegMatrix |
| 部分重叠 | `assignedRegPartiallyOverlaps` (static) | RegAllocGreedy.cpp:2024 |
| tied def | `hasTiedDef` (static) | RegAllocGreedy.cpp:2014 |
| 阶段查询 | `ExtraInfo->getStage` | RegAllocGreedy |

---

### 其他补充

`interferingVRegs(N)` 的 N 参数是软上限，返回的 vector 大小可能小于 N，但达到 N 时 `.size() >= N` 即触发早退。`ExhaustiveSearch` 模式跳过该早退，用于强制求解小规模困难实例。

---

## hasTiedDef 函数分析

### 函数签名与目的（行号 2014-2020）
```cpp
static bool hasTiedDef(MachineRegisterInfo *MRI, Register reg)
```

**功能**: 静态工具函数。判断虚拟寄存器 `reg` 是否存在任一 tied def 操作数（即定义与某个 use 操作数绑定的定义）。

---

### 整体结构

```
hasTiedDef(MRI, reg)
├── for MO in MRI->def_operands(reg):
│   └── if MO.isTied(): return true
└── return false
```

---

### 逐段注释

**1. 遍历定义操作数 (行号 2015-2019)**

```cpp
for (const MachineOperand &MO : MRI->def_operands(reg))
  if (MO.isTied())
    return true;
return false;
```

`def_operands(reg)` 返回 `reg` 的所有 def 操作数（包括 implicit）。`isTied()` 判断该操作数是否通过 `tieOperands` 与另一操作数绑定——tied def 通常对应指令约束（如 X86 的 `mul` 输出固定寄存器、内联 asm 的 tied input/output）。存在 tied def 意味着重着色时必须保证物理寄存器约束仍可满足，难度更高，因此 `mayRecolorAllInterferences` 用"VirtReg 有 tied def 而 Intf 没有"作为"Intf 更可能被重着色"的信号。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MachineRegisterInfo *` | MRI | 虚拟寄存器操作数索引 |
| `MachineOperand` | MO | 单个操作数，isTied() 判定绑定 |

---

### 优化意图

1. 作为"重着色难度"的轻量代理指标：有 tied def 的 vreg 重着色更难，相对地没有 tied def 的干扰更值得尝试重着色。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只看 def | 不检查 tied use | tied use 必然伴随 tied def，足够 |
| 静态函数 | 不访问 RAGreedy 状态 | 易于单测 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| def 迭代 | `MRI->def_operands` | MachineRegisterInfo |
| 绑定判定 | `MachineOperand::isTied` | MachineOperand |

---

### 其他补充

tied 操作数约束来自指令 TableGen 描述中的 `tied-to`，常见于内联汇编、固定寄存器指令（如 `MUL`/`DIV`）以及部分调用约定下的参数传递。

---

## assignedRegPartiallyOverlaps 函数分析

### 函数签名与目的（行号 2024-2032）
```cpp
static bool assignedRegPartiallyOverlaps(const TargetRegisterInfo &TRI,
                                         const VirtRegMap &VRM,
                                         MCRegister PhysReg,
                                         const LiveInterval &Intf)
```

**功能**: 静态工具函数。判断干扰区间 `Intf` 当前已分配的物理寄存器与目标 `PhysReg` 是否"部分重叠"（重叠但非同一寄存器）。用于识别 tuple 寄存器场景下可能通过换位重着色的机会。

---

### 整体结构

```
assignedRegPartiallyOverlaps(TRI, VRM, PhysReg, Intf)
├── AssignedReg = VRM.getPhys(Intf.reg())
├── if PhysReg == AssignedReg: return false
└── return TRI.regsOverlap(PhysReg, AssignedReg)
```

---

### 逐段注释

**1. 取已分配寄存器并比较 (行号 2028-2031)**

```cpp
MCRegister AssignedReg = VRM.getPhys(Intf.reg());
if (PhysReg == AssignedReg)
  return false;
return TRI.regsOverlap(PhysReg, AssignedReg);
```

若 Intf 已分配的就是 PhysReg 本身，不算"部分重叠"（这种情况下 Intf 直接占用目标，无重着色可能）。否则用 `TRI.regsOverlap` 判断两者是否共享任一 sub/register unit——典型场景是 ARM/AArch64 的 tuple 寄存器对（如 `D0` 与 `Q0` 共享 subreg）。部分重叠意味着 Intf 可能通过换一个不重叠的 tuple 成员重着色，从而腾出 PhysReg。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `VirtRegMap &` | VRM | 虚拟→物理寄存器映射 |
| `MCRegister` | PhysReg / AssignedReg | 目标寄存器与 Intf 当前寄存器 |

---

### 优化意图

1. 在 tuple/子寄存器寄存器类中保留重着色机会：即便 Intf 与 VirtReg 同寄存器类、同 RS_Done，只要当前分配与目标部分重叠，仍可能通过换 tuple 成员解决。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| VRM 必须有 phys | 调用前 Intf 应已分配 | 未分配会返回 NoRegister，regsOverlap 行为需注意 |
| regsOverlap 语义 | 包含 alias 但不含自身 | 与"完全相同"分支配合即可 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 物理寄存器查询 | `VRM.getPhys` | VirtRegMap |
| 重叠判定 | `TRI.regsOverlap` | TargetRegisterInfo |

---

### 其他补充

在 `mayRecolorAllInterferences` 的判定逻辑中，`!assignedRegPartiallyOverlaps(...)` 是"Intf 不可重着色"条件之一：若部分重叠，则可能换 tuple 重着色，故不视为不可重着色。该函数对非 tuple 架构（如纯标量 RISC）几乎总返回 false（除非 alias 关系），不会引入误判。
<!-- Group G: Recoloring Candidates, CSR, Callbacks, Post-Alloc & Stats -->

## tryRecoloringCandidates 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::tryRecoloringCandidates(PQueue &RecoloringQueue,
                                       SmallVectorImpl<Register> &NewVRegs,
                                       SmallVirtRegSet &FixedRegisters,
                                       RecoloringStack &RecolorStack,
                                       unsigned Depth)
```

**功能**: 在 `tryLastChanceRecoloring` 触发后，递归地对队列中所有受干扰的虚拟寄存器调用 `selectOrSplitImpl` 进行再着色（recoloring），成功则固定到 `FixedRegisters`，失败则向上回传 `false`。

---

### 整体结构

```
tryRecoloringCandidates(RecoloringQueue, NewVRegs, FixedRegisters, RecolorStack, Depth)
├── while queue 非空: dequeue 一个 LI
│   ├── selectOrSplitImpl(LI, Depth+1) → 递归尝试分配
│   ├── 失败（~0u 截断或非空却 0）→ return false
│   ├── 空 LI 得到 0 → continue
│   └── 成功 → Matrix->assign + FixedRegisters.insert
└── return true
```

---

### 逐段注释

**1. 主循环取出候选 (行 2294-2298)**

```cpp
while (!RecoloringQueue.empty()) {
  const LiveInterval *LI = dequeue(RecoloringQueue);
  LLVM_DEBUG(dbgs() << "Try to recolor: " << *LI << '\n');
  MCRegister PhysReg = selectOrSplitImpl(*LI, NewVRegs, FixedRegisters,
                                         RecolorStack, Depth + 1);
```

从 `RecoloringQueue` 中按优先级出队一个 `LiveInterval`，调用 `selectOrSplitImpl` 以 `Depth+1` 递归进入分配逻辑——这是 recoloring 递归的核心入口，Depth 用于截断判断。

**2. 截断与失败判定 (行 2303-2304)**

```cpp
if (PhysReg == ~0u || (!PhysReg && !LI->empty()))
  return false;
```

`~0u` 表示触发了 cutoff（深度或干扰超限）；非空 LI 却返回 0 表示真正分配失败。这两种情况都让整条 recoloring 链失败回滚。

**3. 空 LI 容忍 (行 2306-2311)**

```cpp
if (!PhysReg) {
  assert(LI->empty() && "Only empty live-range do not require a register");
  LLVM_DEBUG(dbgs() << "Recoloring of " << *LI
                    << " Succeeded. Empty LI.\n");
  continue;
}
```

分裂后 LI 可能被掏空——这种"无需着色"是合法的成功情况，跳过 assign 继续。

**4. 成功分配并固定 (行 2312-2316)**

```cpp
LLVM_DEBUG(dbgs() << "Recoloring of " << *LI
                  << " succeeded with: " << printReg(PhysReg, TRI) << '\n');
Matrix->assign(*LI, PhysReg);
FixedRegisters.insert(LI->reg());
```

成功拿到物理寄存器后，调用 `Matrix->assign` 正式登记，并加入 `FixedRegisters`——后续 evict 时这些寄存器不可再被驱逐，保证本次 recoloring 决策稳定。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `PQueue` | - | 优先队列，存储待 recolor 的 LI 指针 |
| `SmallVirtRegSet` | FixedRegisters | 本轮 recoloring 已确认的寄存器集合，防止被 evict |
| `RecoloringStack` | RecolorStack | 记录 recoloring 路径，用于回滚 |
| `MCRegister` | PhysReg | 返回值；`~0u` = cutoff，`0` = 失败/空 LI，正值 = 成功 |

---

### 优化意图

1. 通过递归 `selectOrSplitImpl` 把一个 LI 的再着色扩散到所有被挤出的干扰者，形成"链式重排"，避免逐个 evict 反复触发同样的冲突。
2. 用 `FixedRegisters` 锁定已分配项，避免在递归过程中被再次驱逐形成死循环。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 深度截断 | `Depth` 递增，由 `selectOrSplitImpl` 内部 cutoff 检查 | 深度过大会触发 `CO_Depth`，最终 `selectOrSplit` 会 emitError |
| `~0u` vs `0` 语义 | 两者都"无寄存器"但含义不同 | 误判会把 cutoff 当成普通失败或反之 |
| 空 LI 才允许 0 | `assert(LI->empty())` | 若分裂逻辑产生非空但未分配的 LI，assert 会触发 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 递归分配 | `RAGreedy::selectOrSplitImpl` | RegAllocGreedy.cpp:2647 |
| 固定分配 | `LiveRegMatrix::assign` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| 调用方 | `RAGreedy::tryLastChanceRecoloring` | RegAllocGreedy.cpp（上游） |

---

### 其他补充

`FixedRegisters` 通过引用传入递归调用，保证全链条共享同一份"已锁"集合，是 recoloring 一致性的关键。

---

## tryAssignCSRFirstTime 函数分析

### 函数签名与目的（行号）
```cpp
MCRegister RAGreedy::tryAssignCSRFirstTime(
    const LiveInterval &VirtReg, AllocationOrder &Order, MCRegister PhysReg,
    uint8_t &CostPerUseLimit, SmallVectorImpl<Register> &NewVRegs)
```

**功能**: 第一次使用某个 callee-saved register (CSR) 时权衡：直接用 CSR / spill 冷段 / 预分裂冷段。目标是避免为一个低权重 live range 强行启用 CSR 而引入 prologue/epilogue 的 push|pop 开销。

---

### 整体结构

```
tryAssignCSRFirstTime(VirtReg, Order, PhysReg, CostPerUseLimit, NewVRegs)
├── 若 stage==RS_Spill 且可 spill
│   ├── calcSpillCost >= CSRCost → 用 CSR (return PhysReg)
│   └── 否则 CostPerUseLimit=1, return MCRegister() (走 spill 路径)
├── 若 stage < RS_Split
│   ├── calculateRegionSplitCost(IgnoreCSR) → BestCand
│   ├── BestCand==NoCand → 用 CSR
│   └── 否则 doRegionSplit, return MCRegister()
└── 默认 return PhysReg (用 CSR)
```

---

### 逐段注释

**1. 已进入 Spill 阶段：权衡 spill vs CSR (行 2384-2395)**

```cpp
if (ExtraInfo->getStage(VirtReg) == RS_Spill && VirtReg.isSpillable()) {
  SA->analyze(&VirtReg);
  if (calcSpillCost(VirtReg) >= CSRCost)
    return PhysReg;
  CostPerUseLimit = 1;
  return MCRegister();
}
```

进入 RS_Spill 阶段时，若 spill 整个 live range 的代价（reads+writes 加权块频率）低于启用 CSR 的代价 `CSRCost`，则选择 spill。`CostPerUseLimit=1` 是关键副作用——禁止 `tryEvict` 后续再去驱逐别的虚拟寄存器来给本 LI 腾 CSR，避免得不偿失。

**2. 早期阶段：权衡 pre-split vs CSR (行 2396-2411)**

```cpp
if (ExtraInfo->getStage(VirtReg) < RS_Split) {
  SA->analyze(&VirtReg);
  unsigned NumCands = 0;
  BlockFrequency BestCost = CSRCost;
  unsigned BestCand = calculateRegionSplitCost(VirtReg, Order, BestCost,
                                               NumCands, true /*IgnoreCSR*/);
  if (BestCand == NoCand)
    return PhysReg;
  doRegionSplit(VirtReg, BestCand, false/*HasCompact*/, NewVRegs);
  return MCRegister();
}
```

尚未到 Split 阶段时，先用 `calculateRegionSplitCost` 以 `IgnoreCSR=true` 找一个比 CSR 更便宜的 region split 候选；找不到 (`NoCand`) 才回退用 CSR；找到就执行 `doRegionSplit` 把冷段切出去，主循环会重新排队 NewVRegs 中产生的新 vreg。

**3. 默认分支 (行 2412)**

```cpp
return PhysReg;
```

已是 RS_Split 之后阶段（即已尝试过分裂），不再绕路，直接用 CSR。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveRangeStage` | RS_Spill / RS_Split | LI 当前所处的分配阶段，决定走哪条分支 |
| `BlockFrequency` | CSRCost | 启用 CSR 的代价（push/pop），由 `initializeCSRCost` 计算 |
| `uint8_t&` | CostPerUseLimit | 输出：限制 tryEvict 时可驱逐的代价上限 |
| `SmallVectorImpl<Register>&` | NewVRegs | 输出：分裂产生的新 vreg，主循环重新排队 |

---

### 优化意图

1. 把 CSR 启用代价 `CSRCost` 当作"门槛"，与 spill 代价 / region-split 代价比较，挑选整体最便宜方案。
2. `CostPerUseLimit=1` 显式抑制后续 evict 决策再为这个 LI 付出 CSR 代价。
3. `IgnoreCSR=true` 让 `calculateRegionSplitCost` 不把 CSR 列入候选物理寄存器，纯按 spill/bundle 代价评估。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `CSRCost` 必须已初始化 | `initializeCSRCost()` 在 `run()` 中先调用 | 若为 0，所有比较失效，永远走 spill |
| `SA->analyze` 必须先调用 | 后续 `calculateRegionSplitCost` 依赖其结果 | 跳过 analyze 会读到陈旧分裂分析数据 |
| `BestCost` 不修改 `CSRCost` | 注释强调"Don't modify CSRCost" | 误改会让后续所有 LI 的门槛被污染 |
| `CostPerUseLimit` 是引用输出 | 调用方 `selectOrSplitImpl` 据此驱动 tryEvict | 不设的话 evict 可能反复驱逐 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| spill 代价 | `RAGreedy::calcSpillCost` | RegAllocGreedy.cpp:2353 |
| 分裂代价 | `RAGreedy::calculateRegionSplitCost` | RegAllocGreedy.cpp:1313 |
| 执行分裂 | `RAGreedy::doRegionSplit` | RegAllocGreedy.cpp:1331 |
| CSR 代价 | `RAGreedy::initializeCSRCost` | RegAllocGreedy.cpp:2420 |

---

### 其他补充

调用点在 `selectOrSplitImpl` 第 2661-2668 行：仅当 `CSRCost != 0`、`PhysReg` 是未用过的 CSR、且 `NewVRegs` 为空（即没有发生 evict）时才进入，否则直接返回 PhysReg。

---

## calcSpillCost 函数分析

### 函数签名与目的（行号）
```cpp
BlockFrequency RAGreedy::calcSpillCost(const LiveInterval &LI)
```

**功能**: 估算把 `LI` 整个 spill 到栈上的代价：遍历所有非 debug 使用指令，对每条指令累加 `(读次数 + 写次数) × 所在 MBB 的频率`，去重后返回。

---

### 整体结构

```
calcSpillCost(LI)
├── SpillCost=0, Visited={}
├── for each non-debug 指令 I 使用 LI.reg()
│   ├── 跳过 meta 指令
│   ├── 去重 Visited.insert(I)
│   ├── (Reads, Writes) = I->readsWritesVirtualRegister(LI.reg())
│   └── SpillCost += (Reads + Writes) * MBBFreq
└── return BlockFrequency(SpillCost)
```

---

### 逐段注释

**1. 遍历所有非 debug 使用 (行 2357-2365)**

```cpp
for (MachineRegisterInfo::reg_instr_nodbg_iterator
         I = MRI->reg_instr_nodbg_begin(LI.reg()),
         E = MRI->reg_instr_nodbg_end();
     I != E;) {
  MachineInstr *MI = &*(I++);
  if (MI->isMetaInstruction())
    continue;
  if (!Visited.insert(MI).second)
    continue;
```

`reg_instr_nodbg_begin` 跳过纯 debug 使用；meta 指令（如伪指令）也不计；`Visited` 防止同一条指令被多个 operand 重复计数（一条指令可能同时读写该寄存器）。

**2. 累加读写代价 (行 2367-2372)**

```cpp
auto [Reads, Writes] = MI->readsWritesVirtualRegister(LI.reg());
auto MBBFreq = SpillPlacer->getBlockFrequency(MI->getParent()->getNumber());
SpillCost += (Reads + Writes) * MBBFreq.getFrequency();
```

`readsWritesVirtualRegister` 返回该指令对 `LI.reg()` 的读、写次数（一条指令可能多次访问同一寄存器，如 inline asm）。频率来自 `SpillPlacer`（与 spill 决策使用同一来源，保证一致性）。spill 后每条读变 reload、每条写变 store，所以代价正比于 (R+W)×freq。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SmallPtrSet<MachineInstr*, 8>` | Visited | 单条指令去重 |
| `std::pair<unsigned, unsigned>` | (Reads, Writes) | 该指令对 LI.reg() 的读写次数 |
| `BlockFrequency` | SpillCost | 累加结果，按入口频率归一化 |

---

### 优化意图

1. 用块频率加权，让热块中的访问代价更高，符合 spiller 的目标函数。
2. 用 `SpillPlacer->getBlockFrequency` 而非 `MBFI->getBlockFreq`，保证与 region split / spill placement 模型同一频率尺度，避免单位不一致。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 不区分 subreg | `readsWritesVirtualRegister` 处理 subreg | 多 subreg 写入可能被低估 |
| meta 指令跳过 | 不计入代价 | 可能让某些纯标记指令的"占位"消失 |
| 频率可能溢出 | uint64_t 累加 | 超大函数可能溢出，但 BlockFrequency 容忍 |
| `SpillPlacer` 非空 | 必须在 `run()` 中已初始化 | 否则 segfault |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 非debug迭代器 | `MachineRegisterInfo::reg_instr_nodbg_begin` | llvm/include/llvm/CodeGen/MachineRegisterInfo.h |
| 读写计数 | `MachineInstr::readsWritesVirtualRegister` | llvm/lib/CodeGen/MachineInstr.cpp |
| 块频率 | `SpillPlacement::getBlockFrequency` | llvm/lib/CodeGen/SpillPlacement.cpp |

---

### 其他补充

返回 `BlockFrequency(SpillCost)` 而非 `uint64_t`，方便与 `CSRCost`、`BestCost` 等同为 `BlockFrequency` 的门槛直接比较。

---

## LRE_CanEraseVirtReg 函数分析

### 函数签名与目的（行号）
```cpp
bool RAGreedy::LRE_CanEraseVirtReg(Register VirtReg)
```

**功能**: `LiveRangeEdit` delegate 回调。当 LiveRangeEdit 想删除一个 vreg 时询问 RA：如果该寄存器已分配物理寄存器，先解除 `Matrix` 分配并清理 broken-hints 集合，返回 `true` 允许删除；未分配则清空 LI 返回 `false`（让 RegAllocBase 在 dequeue 后再删除）。

---

### 整体结构

```
LRE_CanEraseVirtReg(VirtReg)
├── LI = LIS->getInterval(VirtReg)
├── if VRM->hasPhys(VirtReg)
│   ├── Matrix->unassign(LI)
│   ├── aboutToRemoveInterval(LI)  // 清理 SetOfBrokenHints
│   └── return true
├── LI.clear()  // 未分配：清空 LI 便于 debug dump
└── return false
```

---

### 逐段注释

**1. 已分配：解除分配并清理 (行 375-380)**

```cpp
LiveInterval &LI = LIS->getInterval(VirtReg);
if (VRM->hasPhys(VirtReg)) {
  Matrix->unassign(LI);
  aboutToRemoveInterval(LI);
  return true;
}
```

`Matrix->unassign` 把寄存器从 interference matrix 摘掉，释放物理寄存器；`aboutToRemoveInterval` 在第 2415 行定义，仅做 `SetOfBrokenHints.remove(&LI)`——避免后续 `tryHintsRecoloring` 拿到悬空指针。

**2. 未分配：清空 LI (行 381-386)**

```cpp
LI.clear();
return false;
```

未分配意味着该 vreg 仍可能在优先队列中等待处理。LiveRangeEdit 不能直接删除它（会留下队列中的悬空指针），故返回 `false`，由 `RegAllocBase` 在 dequeue 时再 erase。`LI.clear()` 仅是让 debug dump 显示一致状态。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `LiveIntervals` | LIS | 提供 vreg → LiveInterval 映射 |
| `VirtRegMap` | VRM | 提供 vreg → physreg 映射 |
| `LiveRegMatrix` | Matrix | interference 跟踪矩阵 |
| `SetOfBrokenHints` | - | 记录有未满足 hint 的 LI，需同步清理 |

---

### 优化意图

1. 保证 LiveRangeEdit 删除 vreg 时 RA 内部状态（Matrix、SetOfBrokenHints）保持一致，无悬空引用。
2. 区分"已分配 vs 队列中"两种 vreg，避免错误删除队列项。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 不可在未分配时返回 true | 队列仍持有 LI 指针 | 会导致 dequeue 时 use-after-free |
| `aboutToRemoveInterval` 必须调用 | 否则 SetOfBrokenHints 残留 | `tryHintsRecoloring` 后续会访问已删除 LI |
| `LIS->getInterval(VirtReg)` 必须存在 | 调用方保证 | 不存在会 assert |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 解除分配 | `LiveRegMatrix::unassign` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| 清理回调 | `RAGreedy::aboutToRemoveInterval` | RegAllocGreedy.cpp:2415 |
| Delegate 接口 | `LiveRangeEdit::Delegate::LRE_CanEraseVirtReg` | llvm/include/llvm/CodeGen/LiveRangeEdit.h |

---

### 其他补充

`LiveRangeEdit` 在执行 shrink / split 时会调用此回调询问能否删除原 vreg，是 RA 与 spiller 解耦的关键接口。

---

## LRE_WillShrinkVirtReg 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::LRE_WillShrinkVirtReg(Register VirtReg)
```

**功能**: `LiveRangeEdit` delegate 回调。当某个已分配的 vreg 即将被收缩（shrink）时，解除其物理寄存器分配并重新入队，让 RA 在收缩后的更小范围内重新做决策。

---

### 整体结构

```
LRE_WillShrinkVirtReg(VirtReg)
├── if !VRM->hasPhys(VirtReg) return   // 未分配无需处理
├── LI = LIS->getInterval(VirtReg)
├── Matrix->unassign(LI)
└── RegAllocBase::enqueue(&LI)
```

---

### 逐段注释

**1. 未分配直接返回 (行 390-391)**

```cpp
if (!VRM->hasPhys(VirtReg))
  return;
```

未分配的 vreg 还在队列里，shrink 不影响分配状态，直接返回避免重复入队。

**2. 解除分配并重新入队 (行 394-396)**

```cpp
LiveInterval &LI = LIS->getInterval(VirtReg);
Matrix->unassign(LI);
RegAllocBase::enqueue(&LI);
```

`Matrix->unassign` 释放原来的物理寄存器——收缩后的 LI 范围变小，原来的分配可能不再最优，应让 priority queue 重新调度。`enqueue` 调用基类版本，会触发 `enqueueImpl → enqueue(Queue, LI)`，按大小/级联重新插入。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `VirtRegMap` | VRM | 检查是否已分配 |
| `LiveInterval` | LI | 即将收缩的区间 |
| `PQueue` | Queue | 重新入队的目标 |

---

### 优化意图

1. 收缩是 spiller 的常见操作（如把死子区间剥离），收缩后范围更小、干扰更少，重新分配可能拿到更好的寄存器或 hint。
2. 主动 unassign 而非保留旧分配，避免 RA 状态与新 LI 不一致。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须先 unassign 再 enqueue | 否则队列中 LI 仍带旧 physreg | Matrix 状态不一致 |
| 不可对已分配且未入队的 vreg 漏处理 | 会导致 shrink 后 LI 仍占旧寄存器 | 浪费寄存器资源 |
| enqueue 会重置 cascade | 在 RegAllocBase::enqueue 中处理 | 不影响正确性 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 重新入队 | `RegAllocBase::enqueue` | llvm/lib/CodeGen/RegAllocBase.cpp |
| 解除分配 | `LiveRegMatrix::unassign` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| Delegate 接口 | `LiveRangeEdit::Delegate::LRE_WillShrinkVirtReg` | llvm/include/llvm/CodeGen/LiveRangeEdit.h |

---

### 其他补充

与 `LRE_CanEraseVirtReg` 互补：erase 是删除整个 vreg，shrink 是保留 vreg 但缩小范围——前者返回 false 让基类处理，后者直接重新入队。

---

## ExtraRegInfo::LRE_DidCloneVirtReg 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::ExtraRegInfo::LRE_DidCloneVirtReg(Register New, Register Old)
```

**功能**: `LiveRangeEdit` delegate 回调（由 `RAGreedy::LRE_DidCloneVirtReg` 转发）。当 spiller 因死代码消除把一个 vreg 分裂成多个连通分量时，把 `Old` 的 `ExtraInfo`（stage 等）克隆给 `New`，并显式把 `Old` 的 stage 重置为 `RS_Assign`——因为分量变小后应重新尝试分配。

---

### 整体结构

```
LRE_DidCloneVirtReg(New, Old)
├── if !Info.inBounds(Old) return    // Old 未登记，忽略
├── Info[Old].Stage = RS_Assign      // Old 重新走分配流程
├── Info.grow(New.id())              // 扩容以容纳 New
└── Info[New] = Info[Old]            // 复制 stage/cascade 等到 New
```

---

### 逐段注释

**1. 未登记则忽略 (行 405-406)**

```cpp
if (!Info.inBounds(Old))
  return;
```

`Info` 是 `SparseBitVector`-indexed 数组，若 `Old` 从未被 RA 处理过（比如 spiller 内部新建后立刻克隆），无需也无需复制 stage。

**2. Old 重置为 RS_Assign 并克隆给 New (行 412-414)**

```cpp
Info[Old].Stage = RS_Assign;
Info.grow(New.id());
Info[New] = Info[Old];
```

死代码消除后分裂出的连通分量比原 LI 小很多，原来可能已经走到 RS_Split/RS_Spill 阶段了，但分裂后应"重新开始"——故把 `Old.Stage` 强制设回 `RS_Assign`（最早期阶段），然后把整个 `RegInfo` 结构（stage、cascade、retryCnt 等）复制给 `New`。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SparseBitVectorIndexedArray<RegInfo>` | Info | 按 vreg id 索引的稀疏数组，存 stage/cascade |
| `RegInfo` | Stage | 当前 LiveRangeStage（RS_Assign/RS_Split/...） |
| `RegInfo` | Cascade | evict 级联号，防环路 |

---

### 优化意图

1. 把"分裂后分量变小"这一语义编码为 stage 重置，让新分量获得完整的分配尝试机会，而不是直接进入晚期 spill 阶段。
2. 同时复制 cascade 信息，保持 evict 决策的连续性。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须 `Info.grow(New.id())` | 否则 `Info[New]` 越界 | 稀疏数组需要先扩容 |
| 必须先设 Old.Stage 再复制 | 复制顺序错会让 New 拿到旧 stage | New 会跳过应有的分裂尝试 |
| `inBounds` 检查 | Old 可能未被 RA 见过 | 访问未登记项会读默认值 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 转发入口 | `RAGreedy::LRE_DidCloneVirtReg` | RegAllocGreedy.cpp:399 |
| 稀疏数组扩容 | `SparseBitVectorIndexedArray::grow` | llvm/include/llvm/CodeGen/RegAllocCommon.h |
| Delegate 接口 | `LiveRangeEdit::Delegate::LRE_DidCloneVirtReg` | llvm/include/llvm/CodeGen/LiveRangeEdit.h |

---

### 其他补充

`RAGreedy::LRE_DidCloneVirtReg`（行 399-401）只是单行转发到 `ExtraInfo->LRE_DidCloneVirtReg`，因为 stage 信息存在 `ExtraRegInfo` 中而非 RAGreedy 主类。

---

## tryHintsRecoloring 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::tryHintsRecoloring()
```

**功能**: 后分配阶段（post-allocation）入口：遍历所有"hint 被破坏"的 LI 集合 `SetOfBrokenHints`，跳过已无物理寄存器的死定义，对每个调用 `tryHintRecoloring` 尝试通过传播再着色修复 hint。

---

### 整体结构

```
tryHintsRecoloring()
└── for LI in SetOfBrokenHints
    ├── assert LI->reg().isVirtual()
    ├── if !VRM->hasPhys(LI->reg()) continue   // 死定义跳过
    └── tryHintRecoloring(*LI)
```

---

### 逐段注释

**1. 遍历 broken-hint 集合 (行 2636-2644)**

```cpp
for (const LiveInterval *LI : SetOfBrokenHints) {
  assert(LI->reg().isVirtual() &&
         "Recoloring is possible only for virtual registers");
  if (!VRM->hasPhys(LI->reg()))
    continue;
  tryHintRecoloring(*LI);
}
```

`SetOfBrokenHints` 在 `selectOrSplitImpl` 中检测到 hint != 实际分配的 PhysReg 时插入（行 2693-2694）。此处只对仍存活且有物理寄存器的 LI 尝试修复——某些 LI 可能因后续 spill 被移除物理寄存器，跳过避免无效工作。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SetOfBrokenHints` | - | `SmallPtrSet<const LiveInterval*, 4>`，记录 hint 被破坏的 LI |

---

### 优化意图

1. 主分配阶段以"能分到寄存器"为优先，可能牺牲 hint；分配完成后做一次扫尾修复，把因 evict 释放出的寄存器用于消除 copy。
2. 集中在 post-allocation 调用，避免主循环中反复尝试增加复杂度。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只能处理 vreg | assert 物理寄存器不可 recolor | 误插入 physreg 会 assert |
| 必须跳过无 phys 的 LI | 死定义/已 spill | 调用 `tryHintRecoloring` 会读到 0 physreg 触发 assert |
| `aboutToRemoveInterval` 同步清理 | 否则集合残留悬空指针 | `tryHintRecoloring` 内部访问已删除 LI |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单 LI 修复 | `RAGreedy::tryHintRecoloring` | RegAllocGreedy.cpp:2526 |
| 调用点 | `RAGreedy::run` | RegAllocGreedy.cpp:2988 |

---

### 其他补充

调用时机：`run()` 在 `allocatePhysRegs()` 完成后立即调用 `tryHintsRecoloring()`，再做 `postOptimization()` 与 `reportStats()`。

---

## tryHintRecoloring 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::tryHintRecoloring(const LiveInterval &VirtReg)
```

**功能**: 以 `VirtReg` 当前分配的物理寄存器 `PhysReg` 为目标，通过工作列表传播再着色所有 copy 相关的 LI：对每个候选检查可用性与收益性（旧/新 broken-hint 频率和），若不增代价则 `Matrix->unassign + assign` 完成再着色，并把其 copy 邻居加入候选继续传播。

---

### 整体结构

```
tryHintRecoloring(VirtReg)
├── PhysReg = VRM->getPhys(VirtReg.reg)
├── Visited = {VirtReg.reg}, Candidates = {VirtReg.reg}
├── do
│   ├── Reg = Candidates.pop_back()
│   ├── CurrPhys = VRM->getPhys(Reg)
│   ├── if !CurrPhys continue   // 跳过未分配
│   ├── LI = LIS->getInterval(Reg)
│   ├── if CurrPhys != PhysReg 且 (regclass 不含 PhysReg 或有干扰) continue
│   ├── collectHintInfo(Reg, Info)
│   ├── if CurrPhys != PhysReg
│   │   ├── OldCost = getBrokenHintFreq(Info, CurrPhys)
│   │   ├── NewCost = getBrokenHintFreq(Info, PhysReg)
│   │   └── if OldCost < NewCost continue   // 不划算
│   ├── if CurrPhys != PhysReg: Matrix->unassign + assign(PhysReg)
│   └── for HI in Info: if HI.Reg.isVirtual() && Visited.insert: Candidates.push
└── while !Candidates.empty()
```

---

### 逐段注释

**1. 初始化：从 VirtReg 开始 (行 2530-2536)**

```cpp
HintsInfo Info;
Register Reg = VirtReg.reg();
MCRegister PhysReg = VRM->getPhys(Reg);
SmallSet<Register, 4> Visited = {Reg};
SmallVector<Register, 2> RecoloringCandidates = {Reg};
```

`PhysReg` 是传播的"目标颜色"——所有与 VirtReg copy 相关的 LI 都希望被染成这个色。`Visited` 防止环路重复访问。

**2. 工作列表主循环 (行 2541-2551)**

```cpp
do {
  Reg = RecoloringCandidates.pop_back_val();
  MCRegister CurrPhys = VRM->getPhys(Reg);
  if (!CurrPhys) {
    assert(!shouldAllocateRegister(Reg) &&
           "We have an unallocated variable which should have been handled");
    continue;
  }
```

DFS 弹出候选；若该 vreg 无物理寄存器，assert 它本就不该被分配（如 reserved register），跳过。

**3. 可用性检查 (行 2555-2560)**

```cpp
LiveInterval &LI = LIS->getInterval(Reg);
if (CurrPhys != PhysReg && (!MRI->getRegClass(Reg)->contains(PhysReg) ||
                            Matrix->checkInterference(LI, PhysReg)))
  continue;
```

若已与目标同色则无需再着色；否则要求：`PhysReg` 在该 vreg 的 register class 内，且与 LI 当前不干扰。

**4. 收益性检查 (行 2566-2580)**

```cpp
Info.clear();
collectHintInfo(Reg, Info);
if (CurrPhys != PhysReg) {
  BlockFrequency OldCopiesCost = getBrokenHintFreq(Info, CurrPhys);
  BlockFrequency NewCopiesCost = getBrokenHintFreq(Info, PhysReg);
  if (OldCopiesCost < NewCopiesCost)
    continue;
}
```

`Info` 收集 Reg 周围所有 COPY 的对端寄存器与频率。`getBrokenHintFreq(Info, X)` = 把 Reg 染成 X 时，所有对端不是 X 的 COPY 频率和（即会变成真实 copy 的代价）。只有新代价 ≤ 旧代价才继续——注释强调相等也算"盈利"，因为可能暴露更多传播机会。

**5. 执行再着色 (行 2586-2588)**

```cpp
Matrix->unassign(LI);
Matrix->assign(LI, PhysReg);
```

原子地切换 LI 的颜色到 PhysReg。

**6. 传播到 copy 邻居 (行 2591-2594)**

```cpp
for (const HintInfo &HI : Info) {
  if (HI.Reg.isVirtual() && Visited.insert(HI.Reg).second)
    RecoloringCandidates.push_back(HI.Reg);
}
```

把所有虚拟寄存器对端加入候选，继续传播——物理寄存器对端无法 recolor，跳过。`Visited.insert` 返回 second=true 表示首次访问。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `HintsInfo` (= `SmallVector<HintInfo,4>`) | - | Reg 周围 COPY 对端列表 |
| `HintInfo` | Freq / Reg / PhysReg | 一条 COPY 的频率、对端寄存器、对端物理寄存器 |
| `SmallSet<Register, 4>` | Visited | 已处理过的 vreg，防环路 |
| `SmallVector<Register, 2>` | RecoloringCandidates | 工作列表 |

---

### 优化意图

1. 传播式 recolor：以一个 broken-hint LI 为种子，把它的 copy 链整体染成同一色，可消除多条 copy（如 b→c→d 同色则无 copy）。
2. 用"非身份 copy 频率和"作为代价函数，保证每次再着色单调不增代价，最终收敛。
3. 相等代价也接受，扩大传播范围以期后续更优。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `PhysReg` 必须非 0 | 调用方 `tryHintsRecoloring` 已过滤 | 仍需防 CurrPhys=0 |
| 不能 recolor 物理寄存器 | `HI.Reg.isVirtual()` 检查 | 误 recolor physreg 会破坏固定分配 |
| `checkInterference` 必须查 | 否则可能覆盖现有分配 | 引入正确性问题 |
| 代价相等也接受 | 注释明示 | 可能产生无收益的 recolor 但不增加代价 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 收集 COPY 信息 | `RAGreedy::collectHintInfo` | RegAllocGreedy.cpp:2463 |
| 代价计算 | `RAGreedy::getBrokenHintFreq` | RegAllocGreedy.cpp:2508 |
| 干扰检查 | `LiveRegMatrix::checkInterference` | llvm/lib/CodeGen/LiveRegMatrix.cpp |
| 重分配 | `LiveRegMatrix::unassign/assign` | 同上 |

---

### 其他补充

工作列表用 `SmallVector` + `pop_back_val` 实现 DFS（而非 BFS），传播路径短而深，倾向于沿单条 copy 链一路染下去。

---

## collectHintInfo 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::collectHintInfo(Register Reg, HintsInfo &Out)
```

**功能**: 遍历 `Reg` 的所有非 debug 操作数，对每条 COPY 找到对端寄存器 `OtherReg` 并解析出其当前对应的物理寄存器 `OtherPhysReg`（处理 subreg 与 vreg→phys 映射），构造 `HintInfo(Freq, OtherReg, OtherPhysReg)` 追加到 `Out`。`Out` 不被清空。

---

### 整体结构

```
collectHintInfo(Reg, Out)
├── RC = MRI->getRegClass(Reg)
├── for Opnd in MRI->reg_nodbg_operands(Reg)
│   ├── Instr = *Opnd.getParent()
│   ├── if !Instr.isCopy() || Opnd.isImplicit() continue
│   ├── OtherOpnd = Instr.getOperand(Opnd.isDef())
│   ├── OtherReg = OtherOpnd.getReg(); if ==Reg continue
│   ├── OtherSubReg, SubReg = ...
│   ├── if OtherReg.isPhysical
│   │   ├── 其他端有 subreg → getMatchingSuperReg(OtherReg, OtherSubReg, RC)
│   │   ├── 本端有 subreg → getMatchingSuperReg(OtherReg, SubReg, RC)
│   │   └── else → OtherPhysReg = OtherReg
│   ├── else (vreg)
│   │   ├── OtherPhysReg = VRM->getPhys(OtherReg)
│   │   └── if SubReg && OtherSubReg && SubReg!=OtherSubReg continue
│   └── if OtherPhysReg: Out.push_back(HintInfo(MBFI->getBlockFreq(Instr.getParent()), OtherReg, OtherPhysReg))
```

---

### 逐段注释

**1. 遍历操作数找 COPY (行 2464-2469)**

```cpp
const TargetRegisterClass *RC = MRI->getRegClass(Reg);
for (const MachineOperand &Opnd : MRI->reg_nodbg_operands(Reg)) {
  const MachineInstr &Instr = *Opnd.getParent();
  if (!Instr.isCopy() || Opnd.isImplicit())
    continue;
```

只关心真实 COPY 指令；隐式操作数（如内联 call 的 tied 物理寄存器）不算 hint 来源。`RC` 用于后续 subreg 匹配。

**2. 解析对端 (行 2472-2477)**

```cpp
const MachineOperand &OtherOpnd = Instr.getOperand(Opnd.isDef());
Register OtherReg = OtherOpnd.getReg();
if (OtherReg == Reg)
  continue;
unsigned OtherSubReg = OtherOpnd.getSubReg();
unsigned SubReg = Opnd.getSubReg();
```

`Opnd.isDef()` 为 true 表示当前端是 def，则对端是 use（index 0）；反之对端是 def（index 1）。COPY `dst = src`，找另一端。若两端是同一寄存器（self-copy）跳过。同时取两端 subreg 索引。

**3. 物理对端处理 (行 2480-2495)**

```cpp
MCRegister OtherPhysReg;
if (OtherReg.isPhysical()) {
  if (OtherSubReg)
    OtherPhysReg = TRI->getMatchingSuperReg(OtherReg, OtherSubReg, RC);
  else if (SubReg)
    OtherPhysReg = TRI->getMatchingSuperReg(OtherReg, SubReg, RC);
  else
    OtherPhysReg = OtherReg;
} else {
  OtherPhysReg = VRM->getPhys(OtherReg);
  if (SubReg && OtherSubReg && SubReg != OtherSubReg)
    continue;
}
```

物理对端：若对端有 subreg，按对端 subreg 取 super-reg；否则若本端有 subreg，按本端 subreg 取——总之要把对端"提升"到与本端 `Reg` 的 register class 对齐的 super-reg。虚拟对端：直接查 `VRM->getPhys`；若两端 subreg 都非 0 且不同，无法对齐则跳过（注释提到"应该找 matching superreg 但会回归"，留作 TODO）。

**4. 构造 HintInfo (行 2498-2501)**

```cpp
if (OtherPhysReg) {
  Out.push_back(HintInfo(MBFI->getBlockFreq(Instr.getParent()), OtherReg,
                         OtherPhysReg));
}
```

用 COPY 所在 MBB 的频率作为该 hint 的权重——热块中的 COPY 优先被修复。`OtherPhysReg` 为 0（对端 vreg 未分配）时不入列。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `HintsInfo` | Out | 输出列表，`SmallVector<HintInfo, 4>` |
| `HintInfo` | Freq/Reg/PhysReg | 一条 COPY 的频率、对端 vreg 或 physreg、对端物理寄存器 |
| `TargetRegisterClass` | RC | 用于 subreg→superreg 匹配 |

---

### 优化意图

1. 把所有 COPY 对端归一为"物理寄存器"维度，便于后续 `getBrokenHintFreq` 直接比较 `Info.PhysReg == PhysReg`。
2. 用块频率作为权重，让修复决策与热点对齐。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `Out` 不清空 | 调用方负责 | 多次调用会累加，需调用方 `Info.clear()` |
| subreg 不匹配则跳过 | vreg 双 subreg 不一致 | 会漏掉部分 hint，但避免错误对齐 |
| `getMatchingSuperReg` 用 RC 校验 | 必须传 RC | 不传可能拿到不属于该 class 的 superreg |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 非debug操作数迭代 | `MachineRegisterInfo::reg_nodbg_operands` | llvm/include/llvm/CodeGen/MachineRegisterInfo.h |
| subreg→superreg | `TargetRegisterInfo::getMatchingSuperReg` | llvm/include/llvm/CodeGen/TargetRegisterInfo.h |
| 块频率 | `MachineBlockFrequencyInfo::getBlockFreq` | llvm/include/llvm/CodeGen/MachineBlockFrequencyInfo.h |

---

### 其他补充

调用方 `tryHintRecoloring` 在每次循环开始处 `Info.clear()`，因此 `Out` 不清空的约定是安全的。

---

## getBrokenHintFreq 函数分析

### 函数签名与目的（行号）
```cpp
BlockFrequency RAGreedy::getBrokenHintFreq(const HintsInfo &List,
                                           MCRegister PhysReg)
```

**功能**: 给定一个 LI 的所有 COPY hint 列表 `List` 与一个候选物理寄存器 `PhysReg`，返回若把该 LI 染成 `PhysReg` 会"破坏"的 hint 频率和——即所有对端物理寄存器 ≠ PhysReg 的 COPY 频率之和。

---

### 整体结构

```
getBrokenHintFreq(List, PhysReg)
├── Cost = 0
├── for Info in List
│   └── if Info.PhysReg != PhysReg: Cost += Info.Freq
└── return Cost
```

---

### 逐段注释

**1. 累加不匹配的 hint (行 2510-2515)**

```cpp
BlockFrequency Cost = BlockFrequency(0);
for (const HintInfo &Info : List) {
  if (Info.PhysReg != PhysReg)
    Cost += Info.Freq;
}
return Cost;
```

逻辑直白：每个 COPY 对端的 `PhysReg` 与候选 `PhysReg` 不一致就意味着这条 COPY 无法被消除（即"broken"），把它的频率累加进代价。相等则该 COPY 可被消除，代价 0。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `HintsInfo` | List | COPY 对端列表 |
| `HintInfo` | PhysReg / Freq | 对端物理寄存器 / 该 COPY 频率 |
| `BlockFrequency` | Cost | 累加结果 |

---

### 优化意图

1. 提供一个简单的代价函数：broken-hint 频率和。`tryHintRecoloring` 用它比较旧色 vs 新色的代价，决定是否再着色。
2. 用 `BlockFrequency` 而非整数，与 `CSRCost` 等尺度一致。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `Info.PhysReg` 可能为 0 | 对端 vreg 未分配 | 0 != PhysReg 总是计入代价，可能高估 |
| 不区分 subreg | 已在 collectHintInfo 阶段对齐 | 残留不一致会误判 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 调用方 | `RAGreedy::tryHintRecoloring` | RegAllocGreedy.cpp:2572-2573 |

---

### 其他补充

复杂度 O(|List|)，且 `tryHintRecoloring` 每次循环调用两次（OldCost、NewCost），列表通常很小（≤4），开销可忽略。

---

## reportStats() 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::reportStats()
```

**功能**: 顶层统计入口。先递归处理所有顶层 loop 收集 stats，再扫所有非 loop MBB 累加 stats，最后通过 ORE 发一条 `SpillReloadCopies` 函数级 optimization remark。

---

### 整体结构

```
reportStats()
├── if !ORE->allowExtraAnalysis(DEBUG_TYPE) return
├── Stats = {}
├── for L in *Loops: Stats.add(reportStats(L))    // 递归 loop
├── for MBB in *MF: if !Loops->getLoopFor(&MBB): Stats.add(computeStats(MBB))   // 非 loop
├── if !Stats.isEmpty()
│   └── ORE->emit(... SpillReloadCopies remark, Stats.report, "generated in function")
└── return
```

---

### 逐段注释

**1. 顶层开关与循环统计 (行 2897-2901)**

```cpp
if (!ORE->allowExtraAnalysis(DEBUG_TYPE))
  return;
RAGreedyStats Stats;
for (MachineLoop *L : *Loops)
  Stats.add(reportStats(L));
```

`allowExtraAnalysis` 控制是否启用额外诊断——未启用则直接返回避免无意义计算。对每个顶层 loop 调用 `reportStats(L)` 递归（其内部会自下而上累加子循环 + 自己 MBB，并各自发一条 loop 级 remark）。

**2. 非 loop 块统计 (行 2903-2905)**

```cpp
for (MachineBasicBlock &MBB : *MF)
  if (!Loops->getLoopFor(&MBB))
    Stats.add(computeStats(MBB));
```

`getLoopFor` 返回 null 表示该 MBB 不属于任何 loop。这些块只能单独统计（不会出现在 `reportStats(L)` 中）。

**3. 函数级 remark (行 2906-2919)**

```cpp
if (!Stats.isEmpty()) {
  using namespace ore;
  ORE->emit([&]() {
    DebugLoc Loc;
    if (auto *SP = MF->getFunction().getSubprogram())
      Loc = DILocation::get(SP->getContext(), SP->getLine(), 1, SP);
    MachineOptimizationRemarkMissed R(DEBUG_TYPE, "SpillReloadCopies", Loc,
                                      &MF->front());
    Stats.report(R);
    R << "generated in function";
    return R;
  });
}
```

仅当有非零统计时发 remark。`Loc` 取函数所在 subprogram 的首行——属于"函数级"位置而非具体指令。`MachineOptimizationRemarkMissed` 表示"错失优化"类 remark（spill/reload 都是不期望的）。`Stats.report(R)` 把各项统计写入 remark 的命名变量，再追加"generated in function"标识层级。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedyStats` | Stats | 累加结果 |
| `MachineOptimizationRemarkMissed` | R | ORE remark 载体 |
| `MachineLoopInfo` | Loops | 提供 getLoopFor |

---

### 优化意图

1. 双层级 remark：每个 loop 一条（在 `reportStats(L)` 中），整个函数一条（在此处），让用户能在合适粒度上看到 spill/copy 热点。
2. `allowExtraAnalysis` 提前返回避免无 ORE 消费者时白白扫描所有 MBB。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `*Loops` 顶层迭代 | 只含顶层 loop | 子 loop 由 `reportStats(L)` 递归处理 |
| `getLoopFor` 区分非 loop 块 | 否则非 loop 块不会被统计 | 漏算 |
| Loc 可能为空 | 无 subprogram 时 | ORE 接受空 Loc |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 循环级递归 | `RAGreedy::reportStats(MachineLoop*)` | RegAllocGreedy.cpp:2870 |
| 单块统计 | `RAGreedy::computeStats` | RegAllocGreedy.cpp:2778 |
| ORE 发射 | `OptimizationRemarkEmitter::emit` | llvm/include/llvm/IR/OptimizationRemarkEmitter.h |
| 调用点 | `RAGreedy::run` | RegAllocGreedy.cpp:2993 |

---

### 其他补充

`run()` 中 `reportStats()` 是最后一步（在 `postOptimization()` 之后），确保统计的是最终代码形态。

---

## reportStats(MachineLoop*) 函数分析

### 函数签名与目的（行号）
```cpp
RAGreedy::RAGreedyStats RAGreedy::reportStats(MachineLoop *L)
```

**功能**: 递归统计一个 loop：先累加所有子循环的 stats（递归调用），再统计直接属于本 loop（非子 loop）的 MBB，若有非零统计则通过 ORE 发一条 `LoopSpillReloadCopies` remark。

---

### 整体结构

```
reportStats(L)
├── Stats = {}
├── for SubLoop in *L: Stats.add(reportStats(SubLoop))    // 递归子循环
├── for MBB in L->getBlocks()
│   └── if Loops->getLoopFor(MBB) == L: Stats.add(computeStats(*MBB))   // 仅本层块
├── if !Stats.isEmpty()
│   └── ORE->emit(... LoopSpillReloadCopies remark @L->getStartLoc()/L->getHeader(), Stats.report, "generated in loop")
└── return Stats
```

---

### 逐段注释

**1. 递归子循环 (行 2874-2875)**

```cpp
for (MachineLoop *SubLoop : *L)
  Stats.add(reportStats(SubLoop));
```

`*L` 迭代直接子循环。深度优先：子循环先于本层统计，确保 remark 输出顺序自底向上。

**2. 本层 MBB 统计 (行 2877-2880)**

```cpp
for (MachineBasicBlock *MBB : L->getBlocks())
  if (Loops->getLoopFor(MBB) == L)
    Stats.add(computeStats(*MBB));
```

`L->getBlocks()` 含本 loop 所有块（包括子 loop 内的）；`getLoopFor(MBB) == L` 过滤出"直接属于本 loop"的块——子 loop 的块由各自 `reportStats` 处理，避免重复。

**3. Loop 级 remark (行 2882-2892)**

```cpp
if (!Stats.isEmpty()) {
  using namespace ore;
  ORE->emit([&]() {
    MachineOptimizationRemarkMissed R(DEBUG_TYPE, "LoopSpillReloadCopies",
                                      L->getStartLoc(), L->getHeader());
    Stats.report(R);
    R << "generated in loop";
    return R;
  });
}
```

`L->getStartLoc()` 给出 loop 入口的 DebugLoc，`L->getHeader()` 是 MBB 锚点。Remark 名为 `LoopSpillReloadCopies`（区别于函数级 `SpillReloadCopies`）。`Stats.report(R)` 填充各项 NV 变量，再追加 "generated in loop"。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedyStats` | Stats | 本 loop + 所有子 loop 累加 |
| `MachineLoop` | L | 当前循环 |
| `MachineOptimizationRemarkMissed` | R | remark 载体 |

---

### 优化意图

1. 自底向上累加：父 loop 的 Stats 自然包含子 loop，反映"该 loop 总开销"。
2. 每层都发独立 remark，用户可按 loop 粒度定位热点而非只看函数总和。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `getLoopFor(MBB) == L` 必须过滤 | 否则子 loop 块被重复统计 | 统计翻倍 |
| 递归深度 = loop 嵌套深度 | 一般很浅 | 极端深嵌套可能栈紧张 |
| `isEmpty` 才发 remark | 避免无意义噪声 | 空 loop 不输出 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 单块统计 | `RAGreedy::computeStats` | RegAllocGreedy.cpp:2778 |
| Stats 格式化 | `RAGreedyStats::report` | RegAllocGreedy.cpp:2749 |
| 调用方 | `RAGreedy::reportStats()` | RegAllocGreedy.cpp:2896 |

---

### 其他补充

返回 `RAGreedyStats` 而非 `void`，让父循环可直接 `add` 累加，避免父循环再次扫描子循环的 MBB。

---

## computeStats 函数分析

### 函数签名与目的（行号）
```cpp
RAGreedy::RAGreedyStats RAGreedy::computeStats(MachineBasicBlock &MBB)
```

**功能**: 单 MBB 内统计 spill/reload/copy/folded 等计数。遍历每条指令：识别 COPY（含 vreg 的计 copy）、纯 spill/reload 指令、folded load/store（含 statepoint 的 zero-cost 区分），最后用块相对入口频率加权得到 cost 字段。

---

### 整体结构

```
computeStats(MBB)
├── Stats = {}, MFI = MF->getFrameInfo()
├── isSpillSlotAccess(MMO) / isPatchpointInstr(MI) lambda
├── for MI in MBB
│   ├── DestSrc = TII->isCopyInstr(MI)
│   │   ├── if copy: 解析 Src/Dest，vreg→phys，subreg→subreg，若 Src!=Dest: ++Copies
│   ├── elif isLoadFromStackSlot(spill slot): ++Reloads
│   ├── elif isStoreToStackSlot(spill slot): ++Spills
│   ├── elif hasLoadFromStackSlot(任一 spill slot):
│   │   ├── if !patchpoint: FoldedReloads += Accesses.size()
│   │   └── else: 区分 NonZeroCostRange 内/外 → FoldedReloads / ZeroCostFoldedReloads
│   └── elif hasStoreToStackSlot(spill slot): FoldedSpills += Accesses.size()
├── RelFreq = MBFI->getBlockFreqRelativeToEntryBlock(&MBB)
├── 各 Cost = RelFreq * 计数
└── return Stats
```

---

### 逐段注释

**1. Lambda 工具 (行 2783-2791)**

```cpp
auto isSpillSlotAccess = [&MFI](const MachineMemOperand *A) {
  return MFI.isSpillSlotObjectIndex(cast<FixedStackPseudoSourceValue>(
      A->getPseudoValue())->getFrameIndex());
};
auto isPatchpointInstr = [](const MachineInstr &MI) {
  return MI.getOpcode() == TargetOpcode::PATCHPOINT ||
         MI.getOpcode() == TargetOpcode::STACKMAP ||
         MI.getOpcode() == TargetOpcode::STATEPOINT;
};
```

`isSpillSlotAccess` 判断一个 MMO 是否指向 spill 槽（而非一般栈对象）；`isPatchpointInstr` 区分 patchpoint/stackmap/statepoint——这些指令可折叠大量 reload，但其中部分是"零代价"（运行时不会真正执行）。

**2. COPY 统计 (行 2793-2815)**

```cpp
auto DestSrc = TII->isCopyInstr(MI);
if (DestSrc) {
  const MachineOperand &Dest = *DestSrc->Destination;
  const MachineOperand &Src = *DestSrc->Source;
  Register SrcReg = Src.getReg();
  Register DestReg = Dest.getReg();
  if (SrcReg.isVirtual() || DestReg.isVirtual()) {
    if (SrcReg.isVirtual()) {
      SrcReg = VRM->getPhys(SrcReg);
      if (SrcReg && Src.getSubReg())
        SrcReg = TRI->getSubReg(SrcReg, Src.getSubReg());
    }
    if (DestReg.isVirtual()) {
      DestReg = VRM->getPhys(DestReg);
      if (DestReg && DestReg.getSubReg())
        DestReg = TRI->getSubReg(DestReg, Dest.getSubReg());
    }
    if (SrcReg != DestReg)
      ++Stats.Copies;
  }
  continue;
}
```

`isCopyInstr` 让 target 自定义何为 COPY（不限于 `COPY` opcode）。只统计至少一端是 vreg 的 copy——纯 phys→phys 的 copy 与 RA 无关。把 vreg 经 VRM 映射到 phys 后比较：相同则该 copy 被 coalesce 消除，不同则计为真实 copy。

**3. 普通 spill/reload (行 2818-2825)**

```cpp
if (TII->isLoadFromStackSlot(MI, FI) && MFI.isSpillSlotObjectIndex(FI)) {
  ++Stats.Reloads;
  continue;
}
if (TII->isStoreToStackSlot(MI, FI) && MFI.isSpillSlotObjectIndex(FI)) {
  ++Stats.Spills;
  continue;
}
```

纯 spill 槽 load/store 指令（如 `LDR` / `STR` 单一操作数指向 spill 槽），计数后跳过后续 folded 检测。

**4. Folded reload（含 patchpoint 区分）(行 2826-2852)**

```cpp
if (TII->hasLoadFromStackSlot(MI, Accesses) &&
    llvm::any_of(Accesses, isSpillSlotAccess)) {
  if (!isPatchpointInstr(MI)) {
    Stats.FoldedReloads += Accesses.size();
    continue;
  }
  std::pair<unsigned, unsigned> NonZeroCostRange =
      TII->getPatchpointUnfoldableRange(MI);
  SmallSet<unsigned, 16> FoldedReloads;
  SmallSet<unsigned, 16> ZeroCostFoldedReloads;
  for (unsigned Idx = 0, E = MI.getNumOperands(); Idx < E; ++Idx) {
    MachineOperand &MO = MI.getOperand(Idx);
    if (!MO.isFI() || !MFI.isSpillSlotObjectIndex(MO.getIndex()))
      continue;
    if (Idx >= NonZeroCostRange.first && Idx < NonZeroCostRange.second)
      FoldedReloads.insert(MO.getIndex());
    else
      ZeroCostFoldedReloads.insert(MO.getIndex());
  }
  for (unsigned Slot : FoldedReloads)
    ZeroCostFoldedReloads.erase(Slot);
  Stats.FoldedReloads += FoldedReloads.size();
  Stats.ZeroCostFoldedReloads += ZeroCostFoldedReloads.size();
  continue;
}
```

`hasLoadFromStackSlot` 检测指令是否含栈 load 的 operand（folded）。普通指令全部计入 `FoldedReloads`；patchpoint 类指令按 `getPatchpointUnfoldableRange` 区分：落在 NonZeroCostRange 内的 FI 是真实会执行的 reload（计 FoldedReloads），范围外的是 GC 等不会真正执行的"零代价" reload。最后兜底：若某 slot 同时出现在两个集合，按非零代价处理（`erase`）。

**5. Folded store (行 2853-2857)**

```cpp
Accesses.clear();
if (TII->hasStoreToStackSlot(MI, Accesses) &&
    llvm::any_of(Accesses, isSpillSlotAccess)) {
  Stats.FoldedSpills += Accesses.size();
}
```

folded store 同理（patchpoint 一般不折叠 store，故未特殊处理）。

**6. 代价加权 (行 2861-2867)**

```cpp
float RelFreq = MBFI->getBlockFreqRelativeToEntryBlock(&MBB);
Stats.ReloadsCost = RelFreq * Stats.Reloads;
Stats.FoldedReloadsCost = RelFreq * Stats.FoldedReloads;
Stats.SpillsCost = RelFreq * Stats.Spills;
Stats.FoldedSpillsCost = RelFreq * Stats.FoldedSpills;
Stats.CopiesCost = RelFreq * Stats.Copies;
return Stats;
```

`RelFreq` = 本块频率 / 入口频率（float，便于跨函数比较）。Cost 是计数 × RelFreq——近似反映"在程序运行中发生的次数"。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `RAGreedyStats` | Reloads/Spills/Copies/FoldedReloads/FoldedSpills/ZeroCostFoldedReloads | 各类计数 |
| `RAGreedyStats` | *Cost (float) | 计数 × 相对频率 |
| `SmallVector<const MachineMemOperand*, 2>` | Accesses | 一条指令的所有栈访问 MMO |
| `SmallSet<unsigned, 16>` | FoldedReloads/ZeroCostFoldedReloads | patchpoint FI 索引去重集合 |

---

### 优化意图

1. 用 `isCopyInstr` / `hasLoadFromStackSlot` 等 target 抽象，避免硬编码 opcode，跨架构通用。
2. 区分 folded vs 非 folded：folded reload/store 嵌入到正常指令中代价较低，单独统计便于优化决策。
3. patchpoint 的 zero-cost reload 单列，反映 statepoint GC 重定位的真实运行时开销。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `isCopyInstr` 先于 spill 检测 | `continue` 跳过 | 否则 COPY 会被误当成 load/store |
| `Accesses.clear()` 必须在 hasStore 前 | 上次 hasLoad 留有残留 | 否则 folded store 会重复计数 |
| subreg 处理 | vreg→phys 后取 subreg | 漏取会误判 Src==Dest |
| patchpoint NonZeroCostRange 半开区间 | `[first, second)` | 边界写错会归类错误 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| Copy 识别 | `TargetInstrInfo::isCopyInstr` | llvm/include/llvm/CodeGen/TargetInstrInfo.h |
| 栈访问检测 | `TargetInstrInfo::isLoadFromStackSlot / hasLoadFromStackSlot` | 同上 |
| Patchpoint 范围 | `TargetInstrInfo::getPatchpointUnfoldableRange` | 同上 |
| 相对频率 | `MachineBlockFrequencyInfo::getBlockFreqRelativeToEntryBlock` | llvm/include/llvm/CodeGen/MachineBlockFrequencyInfo.h |

---

### 其他补充

`ZeroCostFoldedReloads` 没有对应的 Cost 字段——因为它运行时不会真正执行，加权无意义，只计数即可。

---

## RAGreedyStats::report 函数分析

### 函数签名与目的（行号）
```cpp
void RAGreedy::RAGreedyStats::report(MachineOptimizationRemarkMissed &R)
```

**功能**: 把 `RAGreedyStats` 各项计数与 cost 格式化为带名变量（`ore::NV`）的字符串，流式追加到 ORE remark `R`。每类非零统计追加计数 + 总 cost 两段。

---

### 整体结构

```
report(R)
├── if Spills: R << NV("NumSpills", Spills) << " spills " << NV("TotalSpillsCost", SpillsCost) << " total spills cost "
├── if FoldedSpills: 同上
├── if Reloads: 同上
├── if FoldedReloads: 同上
├── if ZeroCostFoldedReloads: R << NV(...) << " zero cost folded reloads "
├── if Copies: 同上
└── return (隐式)
```

---

### 逐段注释

**1. Spills 段 (行 2751-2754)**

```cpp
if (Spills) {
  R << NV("NumSpills", Spills) << " spills ";
  R << NV("TotalSpillsCost", SpillsCost) << " total spills cost ";
}
```

只有非零才输出。`NV("NumSpills", Spills)` 创建一个命名值，remark 消费者（如 `-Rpass-missed=regalloc` 或工具）可按名引用而非解析字符串。文本部分 "spills " / "total spills cost " 是给人类可读的后缀。

**2. FoldedSpills / Reloads / FoldedReloads 段 (行 2755-2768)**

```cpp
if (FoldedSpills) {
  R << NV("NumFoldedSpills", FoldedSpills) << " folded spills ";
  R << NV("TotalFoldedSpillsCost", FoldedSpillsCost)
    << " total folded spills cost ";
}
if (Reloads) {
  R << NV("NumReloads", Reloads) << " reloads ";
  R << NV("TotalReloadsCost", ReloadsCost) << " total reloads cost ";
}
if (FoldedReloads) {
  R << NV("NumFoldedReloads", FoldedReloads) << " folded reloads ";
  R << NV("TotalFoldedReloadsCost", FoldedReloadsCost)
    << " total folded reloads cost ";
}
```

模式一致：计数 NV + 文本 + cost NV + 文本。命名变量区分 Num* 与 Total*Cost。

**3. ZeroCostFoldedReloads 段 (行 2769-2771)**

```cpp
if (ZeroCostFoldedReloads)
  R << NV("NumZeroCostFoldedReloads", ZeroCostFoldedReloads)
    << " zero cost folded reloads ";
```

只有计数无 cost（原因见 `computeStats` 注释）。

**4. Copies 段 (行 2772-2775)**

```cpp
if (Copies) {
  R << NV("NumVRCopies", Copies) << " virtual registers copies ";
  R << NV("TotalCopiesCost", CopiesCost) << " total copies cost ";
}
```

变量名为 `NumVRCopies`（强调是 vreg-related copy 而非所有 copy）。

---

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MachineOptimizationRemarkMissed` | R | ORE remark，支持 `<< NV(...)` |
| `ore::NV` | - | 命名值包装器，让消费端可按名引用 |

---

### 优化意图

1. 同时输出"计数"与"cost"两维度，让人类读文本、工具读 NV 名，二者各取所需。
2. 零计数跳过，减少 remark 噪声。

---

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须用 `NV` 而非裸值 | 工具按名提取 | 裸值只能字符串解析 |
| NV 名必须稳定 | 外部工具可能按名引用 | 改名会破坏下游脚本 |
| `R << ... ` 顺序即输出顺序 | 调用方需排序 | 顺序错乱影响可读性 |

---

### 关键 API / 源码路径

| 功能 | API | 位置 |
|------|-----|------|
| 命名值 | `llvm::ore::NV` | llvm/include/llvm/IR/OptimizationRemarkStreamer.h |
| Remark 流 | `DiagnosticInfoOptimizationBase::operator<<` | llvm/lib/IR/DiagnosticInfo.cpp |
| 调用方 | `RAGreedy::reportStats()` / `reportStats(MachineLoop*)` | RegAllocGreedy.cpp:2888, 2915 |

---

### 其他补充

`report` 只格式化数据，不决定是否 emit；emit 由调用方 `ORE->emit([&]{ ... Stats.report(R); ... })` 控制，并附加 "generated in loop" / "generated in function" 区分层级。
