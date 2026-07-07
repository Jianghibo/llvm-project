# Pass 分析报告：LLVM 后端指令调度

LLVM 后端指令调度**不是一个独立 Pass**，而是分布在两层的多个调度器：

| 层 | 时机 | 形态 | 默认调度器 | 入口 |
|---|---|---|---|---|
| SelectionDAG | 指令选择阶段内嵌（pre-RA） | 由 `SelectionDAGISel` 直接调用 | `createSourceListDAGScheduler` | `SelectionDAGISel::CodeGenAndEmitDAG` |
| MachineInstr (Pre-RA) | 独立 `MachineFunctionPass` | `MachineScheduler` | `GenericScheduler` + `ScheduleDAGMILive` | `MachineSchedulerLegacy::runOnMachineFunction` |
| MachineInstr (Post-RA) | 独立 `MachineFunctionPass` | `PostMachineScheduler` | `PostGenericScheduler` + `ScheduleDAGMI` | `PostMachineSchedulerLegacy::runOnMachineFunction` |

下面分两层给出分析。源码行号基于当前仓库 `llvm-project` 主分支。

---

## 一、SelectionDAG 层调度（pre-regalloc）

### 1. 基本定位

- **Pass 类型**：Transform（重排 SDNode 顺序、发射 MachineInstr）
- **粒度**：`MachineBasicBlock`，每个 BB 一次
- **所在文件**：`llvm/lib/CodeGen/SelectionDAG/SelectionDAGISel.cpp`、`llvm/lib/CodeGen/SelectionDAG/ScheduleDAGSDNodes.{h,cpp}`、`llvm/lib/CodeGen/SelectionDAG/ScheduleDAGRRList.cpp`、`ScheduleDAGFast.cpp`、`ScheduleDAGVLIW.cpp`
- **入口函数**：`SelectionDAGISel::CodeGenAndEmitDAG()` (`SelectionDAGISel.cpp:948`)
- **在 pipeline 中的位置**：位于指令选择 DAG 完整构建并经过 `LegalizeTypes`、`LegalizeDAG`、`DAGCombine` 之后，`EmitSchedule` 把 SUnit 序列落成 `MachineInstr` 之前。每个 BB 调用一次。
- **职责**：在 SDNode 层面对 BB 内的 DAG 做列表调度，目标是减少寄存器压力 / 利用 ILP / 满足 hazard 约束，把调度结果以 `MachineInstr` 顺序写入 MBB。

### 2. 输入、输出与前置条件

- **输入**：当前 BB 的 `SelectionDAG`（已经过 Legalize/Combine/ISel），即 `CurDAG`
- **输出**：MBB 内 `MachineInstr` 的最终顺序（通过 `EmitSchedule` 写回）
- **关键前置条件**：DAG 已经 legal；`TargetLowering::getSchedulingPreference` 决定节点倾向 source/order
- **不处理的场景**：跨 BB 调度、虚拟寄存器跨 BB 生命周期（这些在 MISched 层处理）

### 3. 关键函数调用栈

```text
SelectionDAGISel::CodeGenAndEmitDAG()                  // SelectionDAGISel.cpp:948
  -> (DAG Combine1 -> LegalizeTypes -> Combine-LT -> Legalize -> Combine2 -> ISel)
  -> CreateScheduler()                                 // :1165
     -> createDefaultScheduler(IS, OptLevel)            // :301
        -> ST.getDAGScheduler(OptLevel)                 // subtarget 可覆盖
        -> 按 Sched::Pref 选 source-list/burr/hybrid/vliw/fast
  -> Scheduler->Run(CurDAG, FuncInfo->MBB)              // :1169  ScheduleDAGSDNodes::Run
     -> BuildSchedGraph()                               // ScheduleDAGSDNodes.cpp:536
        -> BuildSchedUnits()                            // :324   分配 SUnit、合并 glue 节点
        -> AddSchedEdges()                              // :441   构造 SDep
        -> ClusterNeighboringLoads / ClusterNodes       // 加 weak edge
     -> Schedule()  [子类实现]                          // 纯虚，由 RRList/Fast/VLIW 实现
  -> Scheduler->EmitSchedule(FuncInfo->InsertPt)        // :1184  生成 MachineInstr
```

### 4. 整体执行流程（以默认 `ScheduleDAGRRList` 为例）

1. `BuildSchedGraph`：从 DAG 根深度遍历，为每个非 passive 节点分配 `SUnit`；glue 链上的节点合并为一个 SUnit；TokenFactor 标记 `isScheduleLow`；调用 `computeLatency` 设置 `SUnit::Latency`。
2. `AvailableQueue->initNodes(SUnits)`：优先级队列初始化（不同工厂函数创建不同 PQ）。
3. `ListScheduleBottomUp`：核心列表调度循环。
4. `EmitSchedule`：按 `Sequence` 顺序调用 `EmitNode` 生成 `MachineInstr`，处理跨 BB 的 glue 序列和物理寄存器 copy。

### 5. 核心逻辑拆解

#### 5.1 `ScheduleDAGRRList::Schedule()` (`ScheduleDAGRRList.cpp:356`)

```cpp
CurCycle = 0; IssueCount = 0;
LiveRegDefs.reset(new SUnit*[TRI->getNumRegs() + 1]());
BuildSchedGraph();          // 构造 SUnit DAG
Topo.MarkDirty();
AvailableQueue->initNodes(SUnits);
HazardRec->Reset();
ListScheduleBottomUp();     // 真正的调度循环
AvailableQueue->releaseState();
```

- **作用**：建立调度状态机：当前 cycle、issue 计数、物理寄存器活定义表（用于检测 WAR/WAW hazard）。
- **为什么 bottom-up**：BURR/SourceList 默认自底向上，因为目标是减少寄存器压力——先调度使用节点，再调度定义节点，能在节点变成 ready 时立即消费物理寄存器，缩短物理寄存器生命周期。

#### 5.2 `ListScheduleBottomUp` 与 `PickNodeToScheduleBottomUp` (`ScheduleDAGRRList.cpp:1449`)

主循环结构（伪代码）：

```cpp
while (true) {
  SU = PickNodeToScheduleBottomUp();   // 从 AvailableQueue 取最高优先级
  if (!SU) { AdvanceToCycle(MinAvailableCycle); continue; }
  if (!HazardRec->atIssueTime(SU))    { AdvancePastStalls(SU); continue; }
  ScheduleNodeBottomUp(SU);           // 把 SU 放入 Sequence，更新 LiveRegDefs
  ReleasePredecessors(SU);            // 释放前驱，更新 NumSuccsLeft/Height
}
```

- **`ReleasePred`** (`ScheduleDAGRRList.cpp:400`)：递减前驱 `NumSuccsLeft`；为 0 时根据 latency 更新 `Height`，并放入 Available 或 Pending 队列。Height 反映"从该节点到 DAG 叶的最长延迟路径"。
- **`AvailableQueue` 是策略可插拔点**：`BURegReductionPriorityQueue`（BURR，按 reg-reduction 启发式排序）、`SrcRegReductionPriorityQueue`（SourceList，倾向源顺序，code-size 友好）、`HybridBURRPriorityQueue`（带 latency 感知）、`ILPBURRPriorityQueue`（最大化 ILP）。

#### 5.3 SDAG 调度器的策略差异（工厂函数）

| 工厂函数 | PQ 类 | `NeedLatency` | 适用场景 |
|---|---|---|---|
| `createSourceListDAGScheduler` (`:3134`) | `SrcRegReductionPriorityQueue` | false | 默认；倾向保留源序，code size 友好 |
| `createBURRListDAGScheduler` (`:3121`) | `BURegReductionPriorityQueue` | false | BURR，按物理寄存器压力启发式 |
| `createHybridListDAGScheduler` (`:3148`) | `HybridBURRPriorityQueue` | true | 在 BURR 基础上加 latency 感知 |
| `createILPListDAGScheduler` (`:3164`) | `ILPBURRPriorityQueue` | true | 优先调度长依赖链，最大化 ILP |
| `createFastDAGScheduler` (`ScheduleDAGFast.cpp:773`) | - | - | 无调度（线性化） |
| `createVLIWDAGScheduler` (`ScheduleDAGVLIW.cpp:264`) | - | - | VLIW bundle 调度 |

调度器选择在 `createDefaultScheduler` (`SelectionDAGISel.cpp:301`)：subtarget 可通过 `getDAGScheduler(OptLevel)` 覆盖；否则根据 `Sched::Source`/`Sched::Resource`/`Sched::ILP`/`Sched::VLIW` 偏好选。当 subtarget `enableMachineScheduler()` 为 true 时（多数现代后端），会走 `createSourceListDAGScheduler`，把真正调度工作下放给 MISched。

### 6. 关键数据结构

| 结构 | 关键字段 | 含义 | 位置 |
|---|---|---|---|
| `SUnit` | `NodeNum`, `Preds`, `Succs`, `Latency`, `Height`, `Depth`, `NumSuccsLeft`, `isAvailable`, `isPending`, `SchedulingPref` | 调度单元 + 状态 | `ScheduleDAG.h` |
| `SDep` | `Kind` (Data/Anti/Output/Order), `Latency`, `Reg`, `OrdKind` (Barrier/MayAliasMem/Weak/Cluster) | 依赖边 | `ScheduleDAG.h:51` |
| `ScheduleDAGSDNodes::Sequence` | `std::vector<SUnit*>` | 调度结果（bottom-up 顺序） | `ScheduleDAGSDNodes.h:53` |
| `ScheduleDAGTopologicalSort Topo` | `IsReachable`, `WillCreateCycle` | 快速环检测，支持动态加边 | `ScheduleDAG.h:731` |
| `SchedulingPriorityQueue` | `push/pop/getName`, `isReady` | 抽象优先级队列 | `SchedulerRegistry.h` |

### 7. 正确性约束与易错点

- **Glue 合并**：`BuildSchedUnits` 把 glue 链合并为一个 SUnit，否则会破坏 `TokenFactor`/`CopyToReg`/`CALLSEQ` 语义。开发时新增 SDNode 误用 glue 会被这里强制不可分离。
- **物理寄存器 hazard**：`LiveRegDefs` 数组维护每个物理寄存器的当前定义者，调度器据此避免 WAR/WAW；不维护会导致 miscompile。
- **Anti/Output 边**：`ScheduleDAGSDNodes` 文档明确"不使用 Anti/Output 边"，物理寄存器依赖由调度器自己处理（与 MISched 不同）。
- **`isScheduleLow` / `isScheduleHigh`**：`TokenFactor` 被标记为 low，调度器推迟调度它，避免给祖先节点造成假 stall。
- **动态加边必须查环**：`AddPred` 必须经 `Topo.AddPred` 检测环。

### 8. 分析依赖与 Pass 交互

- **依赖**：`TargetLowering`（`getSchedulingPreference`）、`TargetInstrInfo::CreateTargetHazardRecognizer`、`InstrItineraryData`、`TargetRegisterInfo`
- **无 `PreservedAnalyses`**：本层不是 NewPM Pass，由 `SelectionDAGISel` 直接驱动
- **前置 Pass**：DAG Combine、Legalize 全套
- **后续 Pass**：`EmitSchedule` 写入 MIR 后，进入 MISched（如果 subtarget 启用）做更精细的 pre-RA 调度

### 9. 收益模型 / 编译时权衡

- **收益**：减少跨 call 的物理寄存器拷贝、减少寄存器压力、利用 ILP、满足 hazard 约束
- **启发式**：源顺序优先（code size）、register reduction（spill 减少）、ILP（延迟隐藏）——四选一，由 subtarget 通过 `Sched::Pref` 或 `getDAGScheduler` 决定
- **编译时**：DAG 大小决定 cost；`BuildSchedGraph` 是 O(N+E)，调度循环本身 O(N log N)（PQ）

### 10. 验证与调试方法

```bash
# 1. 查看 SDAG 调度输入/输出
llc -debug-only=isel -o - input.ll 2>&1 | less
# 2. 可视化调度 DAG
llc -view-sched-dags -view-sunit-dags input.ll
# 3. 切换调度器对比
llc -pre-RA-sched=source/burr/hybrid/ilp/fast input.ll
# 4. 在特定 BB 强制开启
llc -filter-print-funcs=foo -debug-only=isel input.ll
```

---

## 二、MachineInstr 层调度（Pre-RA `MachineScheduler` + Post-RA `PostMachineScheduler`）

### 1. 基本定位

- **Pass 类型**：Transform（重排 MBB 内 MachineInstr）
- **粒度**：`MachineFunction`，遍历每个 MBB；调度本身按 region 切分
- **所在文件**：`llvm/lib/CodeGen/MachineScheduler.cpp`、`llvm/include/llvm/CodeGen/MachineScheduler.h`
- **入口函数**：
  - Legacy PM：`MachineSchedulerLegacy::runOnMachineFunction` (`MachineScheduler.cpp:645`)
  - New PM：`MachineSchedulerPass::run(MachineFunction&, MFAM)` (`MachineScheduler.cpp:681`)
  - Post-RA Legacy：`PostMachineSchedulerLegacy::runOnMachineFunction` (`:710`)
  - Post-RA New PM：`PostMachineSchedulerPass::run` (`:730`)
- **在 pipeline 中的位置**：
  - Pre-RA MISched：寄存器粗化分配（`VirtRegMap` 之前）和寄存器合并之间；`TargetPassConfig::addMachineSSAOptimization` 阶段
  - Post-RA：寄存器分配完成后、`PrologEpilogInserter` 之前
- **职责**：在 MIR 层重排指令，平衡 ILP 与寄存器压力（pre-RA）或满足 hazard / bundle 约束（post-RA）

### 2. 输入、输出与前置条件

- **输入**：`MachineFunction`、`LiveIntervals`、`MachineDominatorTree`、`MachineLoopInfo`、`AAResults`、`MachineBlockFrequencyInfo`、`RegisterClassInfo`、`TargetSchedModel`
- **输出**：重排后的 MBB（指令顺序变化）；`LiveIntervals` 被增量更新
- **关键前置条件**：subtarget `enableMachineScheduler()` 返回 true；Pre-RA 必须有 `LiveIntervals`
- **不处理的场景**：跨 MBB 调度、跨 region 调度

### 3. 关键函数调用栈

```text
MachineSchedulerLegacy::runOnMachineFunction(MF)        // MachineScheduler.cpp:645
  -> Impl.run(MF, TM, {MLI, MDT, AA, LIS, MBFI})        // :665
     -> scheduleRegions(*Scheduler, FixKillFlags=false) // :572
        -> for each MBB:
           Scheduler.startBlock(MBB)
           getSchedRegions(MBB, MBBRegions, TopDown?)    // 把 MBB 切成 region
           for each SchedRegion R:
              Scheduler.enterRegion(MBB, Begin, End, N)  // ScheduleDAGMI:1003
                 -> SchedImpl->initPolicy(Begin, End, N) // GenericScheduler::initPolicy:3674
              Scheduler.schedule()                       // ScheduleDAGMI:1058
                 -> buildSchedGraph(AA)
                 -> postProcessDAG()                     // 跑 DAG mutations
                 -> findRootsAndBiasEdges(TopRoots, BotRoots)
                 -> SchedImpl->initialize(this)          // GenericScheduler::initialize:3641
                 -> initQueues(TopRoots, BotRoots)
                 -> while (SU = SchedImpl->pickNode(IsTopNode)):
                    moveInstruction(MI, CurrentTop/Bottom)
                    SchedImpl->schedNode(SU, IsTopNode)
                    updateQueues(SU, IsTopNode)
              Scheduler.exitRegion()
           Scheduler.finishBlock()
        -> Scheduler.finalizeSchedule()
```

post-RA 路径同构，只是 `ScheduleDAGInstrs` 实现是 `ScheduleDAGMI`（不带 live interval），策略是 `PostGenericScheduler`，`FixKillFlags=true`。

### 4. 整体执行流程

1. **MBB 切 region**：`getSchedRegions` 把一个 MBB 切成若干 `[RegionBegin, RegionEnd)` 区间，边界是调度屏障（call、terminator、无法重排的指令）。`doMBBSchedRegionsTopDown()` 决定遍历方向。
2. **每个 region 进入**：`enterRegion` 通知策略 `initPolicy`，决定本 region 是 TopDown/BottomUp/Bidirectional、是否跟踪压力。
3. **构造 MBB DAG**：`buildSchedGraph(AA)` 建立 `SUnit` 图，依赖来自 `LiveIntervals`（数据/抗/输出依赖）、AA（内存别名）、物理寄存器 hazard。
4. **DAG mutations**：`postProcessDAG` 跑 `createLoadClusterDAGMutation` / `createStoreClusterDAGMutation` / `createCopyConstrainDAGMutation`，添加 weak/cluster 边辅助启发式。
5. **列表调度循环**：`pickNode` 选节点 → `moveInstruction` 真的 splice MBB → `schedNode` 更新 SchedBoundary cycle / 资源计数 → `updateQueues` 释放后继/前驱到 ready 队列。
6. **收尾**：`placeDebugValues` 把 `DBG_VALUE` 放回原定义附近；`fixupKills`（仅 post-RA）。

### 5. 核心逻辑拆解

#### 5.1 `ScheduleDAGMI::schedule()` (`MachineScheduler.cpp:1058`)

```cpp
buildSchedGraph(AA);
postProcessDAG();
findRootsAndBiasEdges(TopRoots, BotRoots);
SchedImpl->initialize(this);
initQueues(TopRoots, BotRoots);

bool IsTopNode = false;
while (true) {
  if (!checkSchedLimit()) break;
  SUnit *SU = SchedImpl->pickNode(IsTopNode);
  if (!SU) break;
  MachineInstr *MI = SU->getInstr();
  if (IsTopNode) moveInstruction(MI, CurrentTop);
  else           moveInstruction(MI, CurrentBottom);
  SchedImpl->schedNode(SU, IsTopNode);   // 更新 cycle / hazard
  updateQueues(SU, IsTopNode);            // 释放后继/前驱
}
placeDebugValues();
```

- **关键不变量**：`CurrentTop` 与 `CurrentBottom` 之间是"未调度区"，循环结束时二者必须重合。
- **`moveInstruction`** (`:1025`)：用 `MBB->splice` 移动指令，调用 `LIS->handleMove` 增量更新 LiveIntervals。

#### 5.2 `GenericScheduler::pickNode` (`MachineScheduler.cpp:4166`)

```cpp
if (DAG->top() == DAG->bottom()) return nullptr;
if (RegionPolicy.OnlyTopDown)         { 从 Top queue 选; }
else if (RegionPolicy.OnlyBottomUp)   { 从 Bot queue 选; }
else                                  { pickNodeBidirectional(IsTopNode); }
Top.removeReady(SU); Bot.removeReady(SU);  // 双向队列都要清理
```

- **`pickNodeBidirectional`** (`:4088`)：先看 `pickOnlyChoice()`（只有 1 个候选时直接选，避免启发式开销）；否则分别从 Bot/Top 队列各选一个候选，用 `tryCandidate` 比较择优。
- **为什么双向**：pre-RA MISched 默认 bidirectional，能同时利用 top-down 的 ILP 与 bottom-up 的寄存器压力减少。

#### 5.3 `GenericScheduler::tryCandidate` (`MachineScheduler.cpp:3951`) —— 启发式优先级链

按以下顺序短路返回（任一胜出即返回）：

```text
1. FirstValid        首次初始化候选
2. PhysReg bias      物理寄存器 def/copy 倾向其 use
3. RegExcess         避免超出 target pressure 限制
4. RegCritical       避免增加 critical pressure set
5. (SameBoundary 才比较)
   - Stall           减少 latency stall cycles
6. Cluster           保持 clustered 节点相邻
7. (SameBoundary) Weak 优先 weak-edge 邻接
8. RegMax            避免增加 region 最大压力
9. (SameBoundary)
   - ResourceReduce  减少关键资源消耗
   - ResourceDemand  增加"外部需要"的资源
   - Latency         减少依赖链延迟 (acyclic latency limited 时)
10. NodeOrder        兜底：源顺序
```

每条规则对应 `CandReason` 枚举值，`-debug-only=misched` 会 trace 每次选择原因。

#### 5.4 `SchedBoundary` 维护的状态

每个边界（Top/Bot）维护：
- `Available` / `Pending` 两个 ReadyQueue（Pending 是 ready 但被 hazard 卡住的）
- `CurrCycle`, `CurrMOps`, `RetiredMOps`
- `ExecutedResCounts[]`（按 ProcResource 维度统计已消耗资源）
- `ReservedResourceSegments`（基于区间的资源占用，用于精确 hazard 检查，替代旧版 `ReservedCycles`）
- `HazardRec`（target 提供的 `ScheduleHazardRecognizer`）
- `ExpectedLatency`, `DependentLatency`（关键路径长度追踪）

`releaseNode` 在节点 ready 时根据 hazard 状态决定放 Available 还是 Pending；`bumpCycle` 在无法发射时推进 cycle；`countResource` 按 `MCSchedClassDesc` 累加资源消耗。

### 6. 关键数据结构与 LLVM API

| 结构 | 关键字段/接口 | 含义 | 位置 |
|---|---|---|---|
| `ScheduleDAGMI` | `SchedImpl`, `CurrentTop`, `CurrentBottom`, `Mutations` | MI 调度器主体 | `MachineScheduler.h:314` |
| `ScheduleDAGMILive` | `RPTracker`, `TopRPTracker`, `BotRPTracker`, `RegionCriticalPSets`, `SUPressureDiffs`, `DFSResult` | 加上 register pressure 的 pre-RA 版本 | `MachineScheduler.h:429` |
| `MachineSchedStrategy` | `pickNode`, `schedNode`, `releaseTopNode`, `releaseBottomNode`, `initialize`, `initPolicy` | 策略接口 | `MachineScheduler.h:248` |
| `GenericSchedulerBase::SchedCandidate` | `SU`, `Reason`, `AtTop`, `RPDelta`, `ResDelta`, `Policy` | 候选节点 + 决策理由 | `MachineScheduler.h:1164` |
| `SchedBoundary` | `Available`, `Pending`, `CurrCycle`, `HazardRec`, `ReservedResourceSegments` | 单边界状态机 | `MachineScheduler.h:863` |
| `ScheduleDAGMutation` | `apply(ScheduleDAGInstrs*)` | DAG 后处理钩子（cluster/copy constrain） | `ScheduleDAGMutation.h` |
| `TargetPassConfig::createMachineScheduler` / `createPostMachineScheduler` | - | target 替换默认策略的扩展点 | `TargetPassConfig.h` |

### 7. 正确性约束与易错点

- **数据依赖必须保持**：`buildSchedGraph` 通过 `LiveIntervals` 计算数据/抗/输出边，重排后必须保证 `LiveIntervals` 仍然有效——这是 `moveInstruction` 必须调 `LIS->handleMove` 的原因。
- **物理寄存器 hazard**：pre-RA 阶段 physreg 已经分配，必须经 `LiveRegDefs` 风格的检查避免 WAR/WAW；MISched 通过 `biasPhysReg` 启发式把 physreg def 倾向 use。
- **MBB 边界不可跨越**：region 切分保证调度只能在 `[RegionBegin, RegionEnd)` 内重排；terminator、call 通常是边界。
- **`UpdateKillFlags` (post-RA)**：post-RA 调度会改 kill 标志语义，必须 `fixupKills` 修复。
- **`PlaceDebugValues`**：调度后 `DBG_VALUE` 必须重新定位到其定义附近，否则调试信息错误。
- **集群 mutation 不会成环**：`createLoadClusterDAGMutation` 添加 weak edge，由 `Topo` 保证不形成强依赖环。
- **Bidirectional 调度中的"巧合 ready"**：见 `pickNode` 注释 (`:4200`)——节点可能恰好在另一边界也 ready，必须从两个 queue 都移除。

### 8. 分析依赖与 Pass 交互

- **依赖的 analyses**：`MachineDominatorTree`、`MachineLoopInfo`、`AAResultsWrapperPass`、`SlotIndexes`、`LiveIntervals`、`MachineBlockFrequencyInfo`、`TargetSchedModel`、`RegisterClassInfo`
- **`PreservedAnalyses`**（New PM，`:704`）：
  - `getMachineFunctionPassPreservedAnalyses()`
  - `.preserveSet<CFGAnalyses>()`
  - `.preserve<SlotIndexesAnalysis>()`
  - `.preserve<LiveIntervalsAnalysis>()`
  - 即：CFG 不变；LiveIntervals 和 SlotIndexes 增量更新后保留
- **前置 Pass**：指令选择 + SDAG 调度、`LiveIntervals` 计算（`calculateSpillWeights` 之后）
- **后续 Pass**：寄存器合并（`RegisterCoalescer`）、`VirtRegMap`、greedy 分配
- **target 自定义入口**：
  - `TargetPassConfig::createMachineScheduler` 替换整个 DAG 构造 + 策略
  - `Subtarget::overrideSchedPolicy` 调整 region 级 policy（方向、是否跟踪压力）
  - `ScheduleDAGMI::addMutation` 注入自定义 DAG 后处理

### 9. 收益模型 / 编译时权衡

- **收益**：pre-RA 平衡 ILP 与寄存器压力，减少 spill；post-RA 满足 hazard、bundling、cluster 优化
- **启发式**：
  - 是否跟踪压力：`NumRegionInstrs > NIntRegs / 2`（`initPolicy:3690`），小 region 关闭以省编译时
  - Cluster：load/store cluster 鼓励下游 peephole 合并
  - Latency vs Resource：`IsResourceLimited` 切换两种倾向
  - `DisableLatencyHeuristic`、`BiasPRegsExtra` 等 policy flag
- **编译时成本**：每个 region O(N log N) + DAG mutations；`MISchedCutoff` 可强制截断
- **trade-off**：越激进的策略（Hybrid/ILP）越能压 ILP 但编译时和 register pressure 风险更高；SourceList/NoOpt 反之

### 10. 验证与调试方法

```bash
# 1. 看每个 region 的 DAG 和调度结果
llc -debug-only=misched input.ll 2>&1 | less
# 2. 可视化调度 DAG
llc -view-misched-dags input.ll
# 3. 在 misched 前后 dump MIR 对比
llc -print-before=machine-scheduler -print-after=machine-scheduler input.ll
# 4. 切换调度器
llc -misched=generic|ilp|ilpmax|source input.ll
# 5. 强制单向调度
llc -pre-RA-sched=top-down|bottom-up input.ll
# 6. 打印关键路径长度
llc -misched-dump-critical-path-length input.ll
# 7. VerifyScheduling
llc -misched-verify-sched input.ll
# 8. post-RA 调度调试
llc -debug-only=postmisched input.ll
```

### 11. 总结

**两层调度的分工**：

| 维度 | SDAG 层（SelectionDAGISel 内嵌） | MI 层（MachineScheduler pass） |
|---|---|---|
| 节点单位 | SDNode（合并 glue） | MachineInstr |
| 依赖来源 | DAG 边 + 物理寄存器表 | LiveIntervals + AA + physreg |
| 默认方向 | bottom-up | bidirectional |
| 主要目标 | 寄存器压力 / code size / hazard | ILP / 寄存器压力 / cluster |
| 压力跟踪 | 无（粗粒度启发式） | 有（`RegPressureTracker`） |
| target 扩展点 | `getDAGScheduler` / `Sched::Pref` | `createMachineScheduler` / `overrideSchedPolicy` / `addMutation` |

**最核心的设计点**：

- SDAG 层是"快路径 + 粗启发式"，由 subtarget 用 `Sched::Pref` 选 4 种 PQ 之一；多数现代后端把工作下放给 MISched。
- MISched 层是"region 切分 + bidirectional list scheduling + 可插拔 MachineSchedStrategy"，`GenericScheduler` 用 10+ 级启发式链 `tryCandidate` 平衡 ILP、寄存器压力、资源、cluster，所有状态集中在 `SchedBoundary`。
- target 通过三个层次介入：替换 strategy、注入 DAG mutation、override region policy。

**最值得继续深挖的 1~2 个问题**：

1. `tryCandidate` 的启发式顺序对调度质量的影响——每条规则的相对权重和短路行为如何决定 ILP vs RegPressure 的实际平衡？能否通过 alive2 / 仿真量化每条规则的贡献？
2. `ScheduleDAGMILive` 的 register pressure tracking 三 tracker（`RPTracker`/`TopRPTracker`/`BotRPTracker`）是如何在 bidirectional 调度中协同更新 `SUPressureDiffs` 的？这是 pre-RA MISched 区别于 SDAG 调度的本质能力。
