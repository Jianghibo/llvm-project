# LLVM Greedy 寄存器分配中的重染色（Recoloring）算法详解

---

## 0. 传统图染色算法理论基础

### 0.1 寄存器分配与图染色的关系

寄存器分配问题可以**建模为图染色问题**：

```text
程序中的虚拟寄存器 → 图的节点
寄存器间的干涉关系 → 图的边
物理寄存器 → 颜色
分配目标 → 为图中所有节点染色，相邻节点颜色不同
```

**干涉（Interference）定义**：
- 两个虚拟寄存器在**同一时刻都存活**，则它们干涉
- 干涉的寄存器**不能分配同一物理寄存器**

**示例**：
```
v1: [0, 10]  live range
v2: [5, 15]  live range
v3: [0, 5]   live range

干涉图：
  v1 ── v2  （在 [5, 10] 期间同时存活）
  v1 ── v3  （在 [0, 5] 期间同时存活）
  v2 不干涉 v3 （存活时间不重叠）

染色（假设 2 个物理寄存器 R1, R2）：
  v1 → R1
  v2 → R2
  v3 → R1  （与 v2 不干涉，可用 R1）
```

### 0.2 图染色问题的复杂度

**关键事实**：
- **K-染色问题是 NP-complete**
- 寄存器分配问题是图染色问题的实例，因此也是 NP-complete

**NP-complete 意味着**：
1. **不存在多项式时间算法**（除非 P = NP）
2. **最优解难以在有限时间内找到**
3. **实际工程必须采用启发式或近似算法**

### 0.3 传统图染色算法

#### 0.3.1 简化式染色（Simplification-based Coloring）

**Chaitin 算法（1981）**：经典的图染色寄存器分配算法

**核心思想**：
1. **简化（Simplify）**：移除度 < K 的节点（一定可染色）
2. **溢出（Spill）**：无法简化时，选择节点溢出
3. **选择（Select）**：逆序重新分配颜色

**算法流程**：
```
Input: 干涉图 G, 寄存器数量 K
Output: 分配方案或溢出决策

Stack = []
WorkList = G.nodes

while WorkList 不空:
  // 简化阶段
  if 存在节点 n 且 degree(n) < K:
    Stack.push(n)
    G.remove(n)  // 移除节点和边
    WorkList.remove(n)
  else:
    // 溢出阶段：选择节点标记为潜在溢出
    n = chooseSpillCandidate()  // 启发式选择
    Stack.push(n)  // 标记为潜在溢出
    G.remove(n)

// 选择阶段：逆序分配
while Stack 不空:
  n = Stack.pop()
  if n 可染色（邻居颜色数 < K）:
    为 n 分配颜色
  else:
    实际溢出 n（生成 spill code）
```

**关键启发式**：
- **Spill Cost**：溢出成本 = load/store 频率
- **Spill Benefit**：degree / spill_cost
- 选择 **benefit 最高**的节点溢出

#### 0.3.2 其他经典算法

| 算法 | 作者 | 核心改进 |
|---|---|---|
| **Briggs** | Briggs (1994) | 潜在溢出节点重新检查，可能不需实际溢出 |
| **Iterated Coalescing** | George & Appel (1996) | 合并 COPY 节点，减少寄存器压力 |
| **Linear Scan** | Traub (1998) | 线性扫描 live ranges，快速但不最优 |

#### 0.3.3 图染色算法的局限

**问题 1：编译时间**
```
对于 N 个虚拟寄存器，干涉图构建 + 染色：
- 干涉图构建：O(N²)（需要检查所有 pair）
- 染色启发式：O(N × K)
- 对于大型函数，编译时间不可接受
```

**问题 2：溢出决策不精确**
```
传统算法在图构建阶段决定溢出
- 无法利用分裂（splitting）减少溢出
- 无法动态调整溢出决策
```

**问题 3：无法处理复杂约束**
```
- 寄存器别名（如 RAX vs EAX vs AX）
- 寄存器类（如 integer vs float）
- 目标特定约束（如 two-address instructions）
```

### 0.4 传统图染色 vs LLVM Greedy

| 维度 | 传统图染色 | LLVM Greedy |
|---|---|---|
| **理论基础** | 图染色理论 | Live Interval + Splitting |
| **分配时机** | 全局一次性分配 | 增量式迭代分配 |
| **溢出策略** | 全局溢出 | 局部分裂 + 按需溢出 |
| **复杂度控制** | 依赖启发式质量 | 深度限制 + 阶段管理 |
| **实际应用** | 学术研究主导 | 工程实践主导 |

**LLVM Greedy 的改进**：

1. **Live Interval Model**：
   - 不构建完整干涉图
   - 使用 LiveIntervalUnion 做 on-the-fly 干涉检查
   - 编译时间可控

2. **Incremental Splitting**：
   - 分配失败时分裂 live range
   - 分裂后的片段重新加入队列
   - 逐步收敛，减少全局溢出

3. **Eviction + Recoloring**：
   - 驱逐低权重寄存器（类似图染色的"移除节点"）
   - 重染色是对"移除节点"的**递归扩展**
   - 工程（而非理论最优）但实用

**关键洞察（RegAllocBase.h 注释）**：
```cpp
// Register allocation complexity, and generated code performance is
// determined by the effectiveness of live range splitting rather than optimal
// coloring.
```

**翻译**：
> 寄存器分配的复杂度和代码性能由 live range splitting 的效率决定，而非最优染色。

### 0.5 传统图染色在 LLVM 中的残留

LLVM **不使用传统图染色作为主算法**，但保留了核心概念：

| 传统概念 | LLVM Greedy 对应 |
|---|---|
| **干涉图** | LiveRegMatrix（动态干涉查询） |
| **节点移除（degree < K）** | Eviction（驱逐低权重寄存器） |
| **节点溢出** | Spilling（最终溢出） |
| **潜在溢出重试** | Last Chance Recoloring（重染色） |
| **Coalescing** | Register Coalescer Pass（独立 Pass） |

**本质区别**：
- **传统图染色**：全局规划 → 一次性执行
- **LLVM Greedy**：局部决策 → 迭代收敛

---

## 1. 重染色算法概述

### 1.1 什么是重染色（Recoloring）

重染色是一种**寄存器分配优化技术**：当无法为当前虚拟寄存器找到可用物理寄存器时，尝试**重新分配已分配的寄存器**，腾出空间给当前寄存器。

**与传统图染色的关系**：
- **传统图染色**：在"选择阶段"逆序分配时，若节点无法染色则溢出
- **Greedy 重染色**：在"分配失败时"尝试重新分配已分配寄存器，推迟溢出

**类比**：
```text
传统图染色：
  Simplify → 移除节点（保证可染色）
  Spill    → 无法移除，标记溢出
  Select   → 逆序染色，溢出节点最终不染色

Greedy 重染色：
  Assign   → 尝试直接分配
  Evict    → 驱逐低权重（类似移除节点）
  Recolor  → 对驱逐后的节点递归分配（类似 Select 的逆序染色）
  Spill    → 最终仍无法分配，溢出
```

**核心思想**：
```
当前虚拟寄存器 V 无法分配
    ↓
检查已分配寄存器 R 的占用者
    ↓
尝试为占用者分配其他寄存器（递归）
    ↓
成功 → R 可用于 V
失败 → 回滚，尝试下一个 R
    ↓
全部失败 → 溢出 V
```

### 1.2 为什么 Greedy 使用重染色而非传统图染色

**原因 1：编译时间可控**
```
传统图染色：构建完整干涉图 O(N²)，大型函数不可接受
Greedy 重染色：on-the-fly 干涉检查，深度限制 5 层
```

**原因 2：与 Splitting 协同**
```
传统图染色：全局溢出决策，无法精细控制
Greedy：失败时先分裂，重染色作为最后手段
```

**原因 3：工程实用性**
```
传统图染色：理论最优但编译时间不可控
Greedy 重染色：工程启发式，编译时间可控，代码质量足够好
```

### 1.3 重染色与传统图染色的对比

| 特性 | 传统图染色 | Greedy 重染色 |
|---|---|---|
| **时机** | 分配开始时全局规划 | 分配失败时局部补救 |
| **策略** | 寻找最优解（理论） | 贪婪搜索可行解（工程） |
| **复杂度** | NP-complete（不可控） | 深度限制 5（可控） |
| **溢出决策** | 全局溢出 | 局部分裂 + 最后溢出 |
| **适用场景** | 学术研究、小型函数 | 工程实践、大型函数 |

---

## 2. Greedy 中的两种重染色机制

LLVM Greedy 分配器实现了**两种重染色**：

| 类型 | 函数 | 触发时机 | 目的 |
|---|---|---|---|
| **Last Chance Recoloring** | `tryLastChanceRecoloring()` | 所有策略失败后（最后手段） | 避免最终 spill |
| **Hint Recoloring** | `tryHintRecoloring()` / `tryHintsRecoloring()` | eviction 成功后 | 修复被破坏的寄存器 hint，减少 COPY |

### 2.1 在分配流程中的位置

```text
selectOrSplitImpl()
├── tryAssign()                    // 直接分配（无干涉）
├── tryEvict()                     // 驱逐低权重寄存器
│   └── SetOfBrokenHints.insert() // ← 记录 broken hint（用于 Hint Recoloring）
├── trySplit()                     // Live range 分裂
├── tryLastChanceRecoloring()      // ← Last Chance Recoloring（最后机会）
└── spill()                        // 最终溢出
```

**与传统图染色流程对比**：
```text
传统图染色（Chaitin）：
  Simplify → 移除 degree < K 的节点（保证可染色）
  Spill    → 无法移除，标记溢出候选
  Select   → 逆序染色，候选可能仍可染色或实际溢出

Greedy 分配流程：
  tryAssign      → 直接染色（类似 Simplify 成功）
  tryEvict       → 驱逐（类似移除节点，但可递归）
  trySplit       → 分裂（传统算法无此步骤）
  tryRecolor     → 重染色（类似 Select 逆序染色）
  spill          → 最终溢出（类似 Select 失败）
```

---

## 3. Last Chance Recoloring 详细分析

### 3.1 核心函数签名

```cpp
// RegAllocGreedy.cpp 行2126-2276
MCRegister RAGreedy::tryLastChanceRecoloring(
    const LiveInterval &VirtReg,       // 待分配的虚拟寄存器
    AllocationOrder &Order,            // 候选物理寄存器顺序
    SmallVectorImpl<Register> &NewVRegs, // 新创建的虚拟寄存器（split/spill 结果）
    SmallVirtRegSet &FixedRegisters,   // 已固定不可重染色的寄存器
    RecoloringStack &RecolorStack,     // 重染色历史栈（用于回滚）
    unsigned Depth);                   // 递归深度
```

### 3.2 算法流程

```text
tryLastChanceRecoloring(VirtReg, Order, ...)
├── 检查是否允许重染色（TRI->shouldUseLastChanceRecoloringForVirtReg）
├── 检查深度限制（Depth >= LastChanceRecoloringMaxDepth）
├── 标记 VirtReg 为 Fixed（防止递归重染色）
│
├── for PhysReg in Order:
│   ├── 检查干涉类型（仅处理虚拟寄存器干涉）
│   ├── mayRecolorAllInterferences()
│   │   ├── 检查干涉数量（<= LastChanceRecoloringMaxInterference）
│   │   ├── 检查每个干涉是否可重染色
│   │   └── 收集 RecoloringCandidates
│   │
│   ├── 解除所有干涉寄存器的当前分配
│   │   └── RecolorStack.push(LI, OldPhysReg) // 记录原始分配
│   │
│   ├── 临时分配 VirtReg 到 PhysReg
│   │
│   ├── tryRecoloringCandidates()
│   │   └── for LI in RecoloringQueue:
│   │       ├── selectOrSplitImpl(LI, Depth+1) // 递归分配
│   │       ├── 成功 → Matrix->assign(LI, NewPhysReg)
│   │       └── 失败 → return false
│   │
│   ├── 成功 → return PhysReg
│   │
│   └── 失败 → 回滚（恢复所有原始分配）
│       ├── 解除所有临时分配
│       └── 恢复 RecolorStack 中的原始分配
│
└── return ~0u（失败）
```

**与传统图染色 Select 阶段的对比**：
```
传统 Select：
  Stack.pop() → n
  if n 的邻居颜色数 < K:
    为 n 分配可用颜色
  else:
    溢出 n

Greedy tryRecoloringCandidates：
  dequeue() → LI
  selectOrSplitImpl(LI, Depth+1)  // 递归尝试分配
  if 成功:
    assign(LI, PhysReg)
    FixedRegisters.insert(LI)  // 标记为 Fixed
  else:
    return false（触发回滚）
```

**本质区别**：
- **传统 Select**：单次尝试，失败即溢出
- **Greedy Recolor**：递归尝试，失败可回滚并尝试其他候选

### 3.3 关键约束

#### 3.3.1 深度限制

```cpp
// 行94-96
static cl::opt<unsigned> LastChanceRecoloringMaxDepth("lcr-max-depth", 
    cl::Hidden, cl::desc("Last chance recoloring max depth"),
    cl::init(5));
```

**目的**：
- 防止无限递归（类似传统算法的"停止条件"）
- 控制编译时间复杂度
- 深度 >= 5 时直接放弃

**对比传统图染色**：
```
传统算法：整个图一次性处理，无递归深度概念
Greedy：递归重染色，深度限制保证编译时间可控
```

#### 3.3.2 干涉数量限制

```cpp
// 行98-102
static cl::opt<unsigned> LastChanceRecoloringMaxInterference(
    "lcr-max-interf", cl::Hidden,
    cl::desc("Last chance recoloring maximum number of considered interference"),
    cl::init(8));
```

**目的**：
- 干涉太多时，至少一个不可重染色
- 提前放弃，避免无谓搜索

**对比传统图染色**：
```
传统算法：degree >= K 时标记溢出候选
Greedy：干涉数 >= 8 时放弃重染色尝试
```

#### 3.3.3 Fixed Register 机制

```cpp
// 行2156
FixedRegisters.insert(VirtReg.reg());
```

**规则**：
- 一旦寄存器被 Last Chance Recoloring 分配，标记为 **Fixed**
- Fixed 寄存器在当前"重染色会话"中不可再被重染色
- 防止循环重染色（A → B → A）

**对比传统图染色**：
```
传统算法：节点染色后固定，不再改变
Greedy：Fixed 机制类似，但只对当前重染色会话生效
```

#### 3.3.4 Exhaustive Search

```cpp
// 行104-108
static cl::opt<bool> ExhaustiveSearch(
    "exhaustive-register-search", cl::NotHidden,
    cl::desc("Exhaustive Search for registers bypassing the depth "
             "and interference cutoffs of last chance recoloring"));
```

**效果**：
- 绕过深度和干涉数量限制
- 用于极端情况（不惜编译时间换取分配成功）

**对比传统图染色**：
```
传统算法：无法突破 NP-complete 限制
Greedy Exhaustive：突破深度限制，但仍不是最优解
```

### 3.4 干涉检查逻辑

```cpp
// 行2039-2081: mayRecolorAllInterferences
bool RAGreedy::mayRecolorAllInterferences(
    MCRegister PhysReg, const LiveInterval &VirtReg,
    SmallLISet &RecoloringCandidates, const SmallVirtRegSet &FixedRegisters) {
  
  // 1. 检查干涉数量
  if (Q.interferingVRegs(LastChanceRecoloringMaxInterference).size() >= 
      LastChanceRecoloringMaxInterference && !ExhaustiveSearch)
    return false;  // 干涉太多，放弃
  
  // 2. 检查每个干涉是否可重染色
  for (const LiveInterval *Intf : Q.interferingVRegs()) {
    // 不可重染色的条件：
    // - Intf 已完成（RS_Done）且与 VirtReg 同寄存器类
    // - Intf 已被标记为 Fixed
    if ((ExtraInfo->getStage(*Intf) == RS_Done && 
         MRI->getRegClass(Intf->reg()) == CurRC) ||
        FixedRegisters.count(Intf->reg()))
      return false;
    
    RecoloringCandidates.insert(Intf);
  }
  return true;
}
```

**对比传统图染色 Simplify 阶段**：
```
传统 Simplify：
  if degree(n) < K:
    移除 n（保证可染色）

Greedy mayRecolorAllInterferences：
  if 干涉数 < MaxInterference 且所有干涉可重染色:
    继续尝试重染色
```

### 3.5 递归重染色实现

```cpp
// 行2286-2316: tryRecoloringCandidates
bool RAGreedy::tryRecoloringCandidates(
    PQueue &RecoloringQueue,
    SmallVectorImpl<Register> &NewVRegs,
    SmallVirtRegSet &FixedRegisters,
    RecoloringStack &RecolorStack,
    unsigned Depth) {
  
  while (!RecoloringQueue.empty()) {
    const LiveInterval *LI = dequeue(RecoloringQueue);
    
    // 递归调用 selectOrSplitImpl（可能再次触发重染色）
    MCRegister PhysReg = selectOrSplitImpl(*LI, NewVRegs, FixedRegisters,
                                           RecolorStack, Depth + 1);
    
    if (PhysReg == ~0u)  // 分配失败
      return false;
    
    if (!PhysReg)  // Live range 已空（被 split 消除）
      continue;
    
    // 成功分配
    Matrix->assign(*LI, PhysReg);
    FixedRegisters.insert(LI->reg());  // 标记为 Fixed
  }
  return true;
}
```

**关键差异**：
- **传统算法**：Select 阶段单次染色，失败即溢出
- **Greedy**：递归调用 `selectOrSplitImpl`，可能再次触发 Eviction/Split/Recolor

### 3.6 回滚机制

```cpp
// 行2247-2271: 失败时回滚
// 1. 解除所有临时分配
for (ssize_t I = RecolorStack.size() - 1; I >= EntryStackSize; --I) {
  const LiveInterval *LI;
  MCRegister PhysReg;
  std::tie(LI, PhysReg) = RecolorStack[I];
  
  if (VRM->hasPhys(LI->reg()))
    Matrix->unassign(*LI);
}

// 2. 恢复原始分配
for (size_t I = EntryStackSize; I != RecolorStack.size(); ++I) {
  std::tie(LI, PhysReg) = RecolorStack[I];
  if (!LI->empty() && !MRI->reg_nodbg_empty(LI->reg()))
    Matrix->assign(*LI, PhysReg);  // 恢复原分配
}

// 3. 清空重染色栈
RecolorStack.resize(EntryStackSize);
```

**传统算法无回滚**：
- 传统图染色一旦溢出节点，无法回滚
- Greedy 重染色失败可回滚，尝试其他物理寄存器

### 3.7 示例场景

**源码注释中的例子（行2091-2112）**：

```
场景：
  vA 可用 {R1, R2}
  vB 可用 {R2, R3}
  vC 可用 {R1}
  三者互相干涉，且无法 split

初始分配：
  vA → R1
  vB → R2
  vC 无法分配（R1 被 vA 占用，vA 已完成）

Last Chance Recoloring：
  1. vC 尝试 R1（假设 vA 被驱逐）
  2. vC 标记为 Fixed
  3. vA 需要重新分配
     - R1 不可用（vC Fixed）
     - 尝试 R2（假设 vB 被驱逐）
  4. vA 标记为 Fixed
  5. vB 需要重新分配
     - R3 可用
  6. 成功！

最终分配：
  vC → R1
  vA → R2
  vB → R3
```

**传统图染色如何处理**：
```
干涉图：
  vA ── vB ── vC（三角形，无法用 3 颜色）

传统 Simplify：
  degree(vA) = 2 < K=3 → 移除 vA
  degree(vB) = 1 < K=3 → 移除 vB
  degree(vC) = 0 < K=3 → 移除 vC

传统 Select：
  vC → R1
  vB → R2（与 vC 不干涉）
  vA → ?（与 vB 和 vC 都干涉，无颜色可用）
  → 溢出 vA

结果：
  传统：溢出 vA
  Greedy：通过重染色成功分配三者
```

---

## 4. Hint Recoloring 详细分析

### 4.1 目的

修复被破坏的**寄存器 hint**，减少不必要的 COPY 指令。

**Hint 来源**：
- COPY 指令：`COPY %vreg1, %vreg2` → 希望 vreg1 和 vreg2 使用同一物理寄存器
- 目标特定 hint：如 AMDGPU 的 AGPR → VGPR hint

**与传统 Coalescing 的关系**：
- **传统 Coalescing**：在图染色前合并 COPY 节点
- **Greedy Hint Recoloring**：在分配后修复被破坏的 hint

### 4.2 触发时机

```cpp
// 行2684-2692: 在 tryEvict 成功后记录 broken hint
if (MCRegister PhysReg = tryEvict(...)) {
  Register Hint = MRI->getSimpleHint(VirtReg.reg());
  if (Hint && Hint != PhysReg)  // hint 被破坏
    SetOfBrokenHints.insert(&VirtReg);  // 记录为待修复
  return PhysReg;
}
```

### 4.3 执行时机

```cpp
// 行2632-2642: tryHintsRecoloring
void RAGreedy::tryHintsRecoloring() {
  for (const LiveInterval *LI : SetOfBrokenHints) {
    if (!VRM->hasPhys(LI->reg()))
      continue;  // 死寄存器跳过
    tryHintRecoloring(*LI);
  }
}
```

**调用位置**：在 `selectOrSplit` 返回前（行2345附近）。

### 4.4 算法流程

```cpp
// 行2523-2593: tryHintRecoloring
void RAGreedy::tryHintRecoloring(const LiveInterval &VirtReg) {
  MCRegister PhysReg = VRM->getPhys(VirtReg.reg());  // 目标寄存器
  SmallVector<Register, 2> RecoloringCandidates = {VirtReg.reg()};
  
  do {
    Register Reg = RecoloringCandidates.pop_back_val();
    MCRegister CurrPhys = VRM->getPhys(Reg);
    LiveInterval &LI = LIS->getInterval(Reg);
    
    // 1. 检查是否可重染色到 PhysReg
    if (CurrPhys != PhysReg) {
      if (!MRI->getRegClass(Reg)->contains(PhysReg) ||
          Matrix->checkInterference(LI, PhysReg))
        continue;  // 不满足约束或有干涉
      
      // 2. 检查收益
      BlockFrequency OldCopiesCost = getBrokenHintFreq(Info, CurrPhys);
      BlockFrequency NewCopiesCost = getBrokenHintFreq(Info, PhysReg);
      
      if (OldCopiesCost < NewCopiesCost)
        continue;  // 不划算
      
      // 3. 执行重染色
      Matrix->unassign(LI);
      Matrix->assign(LI, PhysReg);
    }
    
    // 4. 传播到 copy-related 寄存器
    collectHintInfo(Reg, Info);
    for (const HintInfo &HI : Info) {
      if (HI.Reg.isVirtual() && Visited.insert(HI.Reg).second)
        RecoloringCandidates.push_back(HI.Reg);
    }
  } while (!RecoloringCandidates.empty());
}
```

### 4.5 收益计算

```cpp
// 行2569-2577
BlockFrequency OldCopiesCost = getBrokenHintFreq(Info, CurrPhys);
BlockFrequency NewCopiesCost = getBrokenHintFreq(Info, PhysReg);

if (OldCopiesCost < NewCopiesCost)
  continue;  // 重染色后 COPY 成本更高，放弃
```

**决策原则**：
- 成本更低或相等时执行
- 相等时也执行（可能暴露更多重染色机会）

### 4.6 传播机制

```cpp
// 行2588-2592
for (const HintInfo &HI : Info) {
  if (HI.Reg.isVirtual() && Visited.insert(HI.Reg).second)
    RecoloringCandidates.push_back(HI.Reg);  // 加入待处理队列
}
```

**效果**：重染色传播到整个 copy-chain，最大化消除 COPY。

### 4.7 示例场景

**源码注释中的例子（行2599-2631）**：

```
场景：
  BB1:
    a = ...
    b = ...
  BB2:
    c = b      // COPY
    ...
    d = c      // COPY
    = d
    = a

假设 b 被 split：
  BB1:
    a = ...
    b = ...
  BB2:
    c = b      // COPY
    ...
    d = c      // COPY
    = d

问题：b, c, d 可能被分配不同寄存器（如 R1, R2, R3）
结果：两条 COPY 指令

Hint Recoloring：
  1. 检测到 b-c-d 的 copy-chain
  2. 假设 a 被 evict 后腾出 R4
  3. 尝试将 b, c, d 都重染色到 R4
  4. 成功 → 消除两条 COPY

最终：
  b, c, d 都使用 R4 → 无需 COPY
```

**传统 Iterated Coalescing 如何处理**：
```
传统算法：
  在 Simplify 前尝试合并 COPY 节点
  合并成功 → 减少节点，降低寄存器压力
  合并失败 → 保留 COPY，由染色后处理

Greedy Hint Recoloring：
  分配后修复 broken hint
  更灵活，可利用 eviction 后释放的寄存器
```

---

## 5. 重染色控制参数

### 5.1 参数列表

| 参数 | 默认值 | 作用 |
|---|---|---|
| `lcr-max-depth` | 5 | Last Chance 最大递归深度 |
| `lcr-max-interf` | 8 | 最大干涉数量 |
| `exhaustive-register-search` | false | 绕过深度和干涉限制 |
| `regalloc-csr-first-time-cost` | 0 | CSR 首次使用成本 |
| `regalloc-csr-cost-scale` | 80 | CSR 成本缩放比例 |

### 5.2 使用示例

```bash
# 默认重染色（深度限制 5）
clang -O2 source.c

# 突破限制（可能导致编译时间大幅增加）
clang -O2 -mllvm -exhaustive-register-search source.c

# 增加深度（编译时间增加，可能找到更多分配方案）
clang -O2 -mllvm -lcr-max-depth=10 source.c

# 减少干涉限制（更激进）
clang -O2 -mllvm -lcr-max-interf=4 source.c
```

---

## 6. 重染色失败处理

### 6.1 CutOffInfo 机制

```cpp
// 行198-210: CutOffStage 枚举
enum CutOffStage {
  CO_None = 0,      // 无 cutoff
  CO_Depth = 1,     // 达到深度限制
  CO_Interf = 2     // 达到干涉数量限制
};
```

### 6.2 错误报告

```cpp
// 行2330-2344
if (Reg == ~0U && (CutOffInfo != CO_None)) {
  if (CutOffEncountered == CO_Depth)
    Ctx.emitError("register allocation failed: maximum depth for recoloring "
                  "reached. Use -fexhaustive-register-search to skip cutoffs");
  else if (CutOffEncountered == CO_Interf)
    Ctx.emitError("register allocation failed: maximum interference for "
                  "recoloring reached...");
}
```

---

## 7. 性能影响分析

### 7.1 编译时间

| 场景 | 编译时间影响 |
|---|---|---|
| 无重染色 | 最快（但更多 spill） |
| 默认重染色（depth=5） | 中等增加 |
| Exhaustive Search | 可能显著增加 |

### 7.2 代码质量

| 场景 | Spill 数量 | COPY 数量 |
|---|---|---|
| 无重染色 | 较多 | 较多 |
| Last Chance Recoloring | 减少 | 不变 |
| Hint Recoloring | 不变 | 减少 |

### 7.3 适用场景

| 场景 | 推荐设置 |
|---|---|---|
| 常规编译 | 默认（depth=5, interf=8） |
| 寄存器压力极高 | `-exhaustive-register-search`（谨慎使用） |
| 关注 COPY 消除 | Hint Recoloring 自动启用 |

---

## 8. 关键源码位置

| 功能 | 文件 | 行号 |
|---|---|---|
| Last Chance Recoloring | `RegAllocGreedy.cpp` | 2007-2276 |
| `tryLastChanceRecoloring()` | `RegAllocGreedy.cpp` | 2126-2276 |
| `mayRecolorAllInterferences()` | `RegAllocGreedy.cpp` | 2039-2081 |
| `tryRecoloringCandidates()` | `RegAllocGreedy.cpp` | 2286-2316 |
| Hint Recoloring | `RegAllocGreedy.cpp` | 2515-2642 |
| `tryHintRecoloring()` | `RegAllocGreedy.cpp` | 2523-2593 |
| `tryHintsRecoloring()` | `RegAllocGreedy.cpp` | 2632-2642 |
| CutOffInfo 机制 | `RegAllocGreedy.cpp` | 198-210, 2330-2344 |
| 参数定义 | `RegAllocGreedy.cpp` | 94-122 |
| 设计原则注释 | `RegAllocBase.h` | 9-33 |

---

## 9. 总结

### 9.1 设计原则

1. **Last Chance**：作为最后手段，所有其他策略失败后触发
2. **深度限制**：控制编译时间复杂度（默认 5 层）
3. **Fixed 机制**：防止循环重染色
4. **回滚能力**：失败时恢复原始分配
5. **收益驱动**：Hint Recoloring 只在收益时执行

### 9.2 核心创新

| 机制 | 创新点 | 与传统图染色的关系 |
|---|---|---|---|
| **Last Chance** | 递归重染色 + Fixed 标记 + 回滚栈 | 类似 Select 阶段逆序染色，但可递归和回滚 |
| **Hint Recoloring** | Copy-chain 传播 + 收益计算 | 类似 Iterated Coalescing，但分配后执行 |

### 9.3 为什么 LLVM 选择重染色而非传统图染色

**关键洞察**（RegAllocBase.h 行18-20）：
```cpp
// Register allocation complexity, and generated code performance is
// determined by the effectiveness of live range splitting rather than optimal
// coloring.
```

**原因总结**：
1. **编译时间可控**：深度限制替代 NP-complete
2. **与 Splitting 协同**：分裂替代全局溢出
3. **工程实用性**：启发式替代最优解

### 9.4 适用性

- **Last Chance**：适合寄存器压力极高的场景（如 inline asm、复杂函数）
- **Hint Recoloring**：适合有大量 COPY 的场景（自动启用）

### 9.5 注意事项

- `-exhaustive-register-search` 可能大幅增加编译时间
- 深度限制过低可能导致分配失败
- Hint Recoloring 是后处理，不影响分配成功率

---

## 10. 参考文献与延伸阅读

### 10.1 传统图染色算法

| 论文 | 作者 | 年份 | 核心贡献 |
|---|---|---|---|
| **Register Allocation via Coloring** | G. Chaitin | 1981 | 首次将图染色应用于寄存器分配 |
| **Improvements to Graph Coloring Register Allocation** | P. Briggs | 1994 | 潜在溢出重试，减少实际溢出 |
| **Iterated Register Coalescing** | L. George, A. Appel | 1996 | 合并 COPY，减少寄存器压力 |
| **Linear Scan Register Allocation** | M. Traub | 1998 | 线性扫描，快速分配 |

### 10.2 LLVM Greedy 设计

| 文档 | 位置 |
|---|---|---|
| **RegAllocBase.h 注释** | `llvm/lib/CodeGen/RegAllocBase.h:9-33` |
| **Greedy 算法设计** | `llvm/lib/CodeGen/RegAllocGreedy.cpp:1-11` |
| **LLVM Programmer's Manual** | https://llvm.org/docs/ProgrammersManual.html |

### 10.3 关键论文对比

| 维度 | Chaitin (1981) | Briggs (1994) | LLVM Greedy |
|---|---|---|---|
| **溢出决策时机** | Simplify 阶段 | Select 阶段重试 | 分配失败时 |
| **分裂能力** | 无 | 无 | 有（核心机制） |
| **回滚能力** | 无 | 部分重试 | 完整回滚栈 |
| **复杂度控制** | 启发式质量 | 启发式质量 | 深度限制 |
| **实际应用** | 学术原型 | 改进原型 | 工程实践 |