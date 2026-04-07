# BOLT 整体架构与处理流程

BOLT（Binary Optimization and Layout Tool）是一个**链接后优化器（post-link optimizer）**，直接操作已编译链接的 ELF 二进制文件，基于采样 profile（如 Linux perf）重新组织代码布局。

---

## 整体流水线概览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    llvm-bolt 主流程 (RewriteInstance::run())             │
│                                                                          │
│  ┌─ 阶段 1: 二进制读取与发现 ──────────────────────────────────────┐    │
│  │  discoverFileObjects()        // 解析 ELF 符号表、段、重定位     │    │
│  │  readSpecialSections()        // 读取 .eh_frame 等特殊段        │    │
│  │  readDebugInfo()              // 解析 DWARF 调试信息            │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 2: Profile 预处理 ───────────────────────────────────────┐    │
│  │  preprocessProfileData()      // 加载 profile（反汇编前）       │    │
│  │    ├─ DataAggregator          // perf.data → LBR 聚合          │    │
│  │    ├─ DataReader              // .fdata 格式读取               │    │
│  │    └─ YAMLProfileReader       // YAML 格式（支持 stale profile）│    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 3: 反汇编 ───────────────────────────────────────────────┐    │
│  │  disassembleFunctions()       // MCDisassembler 逐函数反汇编    │    │
│  │    → 函数状态: Empty → Disassembled                             │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 4: CFG 构建 ─────────────────────────────────────────────┐    │
│  │  buildFunctionsCFG()          // 并行构建所有函数的 CFG          │    │
│  │    → 划分 BasicBlock、建立控制流边、分析跳转表                  │    │
│  │    → 函数状态: Disassembled → CFG                               │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 5: Profile 关联到 CFG ───────────────────────────────────┐    │
│  │  processProfileData()         // 把分支计数/入口计数绑定到 CFG  │    │
│  │    ├─ 边计数（from→to, count, mispreds）                       │    │
│  │    └─ StaleProfileMatching  // 旧版 profile 的块哈希+流推断    │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 6: 优化 Pass 流水线 ────────────────────────────────────┐    │
│  │  runOptimizationPasses()      // ~50+ 个 BinaryFunctionPass    │    │
│  │    （详见下方优化 Pass 列表）                                    │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 7: 发射与链接 ──────────────────────────────────────────┐    │
│  │  emitAndLink()                                                │    │
│  │    ├─ BinaryEmitter         // 通过 MCStreamer 发射为内存对象  │    │
│  │    └─ JITLinkLinker         // 解析重定位、分配最终地址         │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 8: 元数据更新 ──────────────────────────────────────────┐    │
│  │  updateMetadata()                                             │    │
│  │    ├─ DWARFRewriter         // 更新调试段（如启用）            │    │
│  │    ├─ MetadataManager       // 添加 .note.bolt_info 等         │    │
│  │    └─ BAT                   // BOLT Address Translation 段    │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─ 阶段 9: 重写输出二进制 ──────────────────────────────────────┐    │
│  │  rewriteFile()                                                │    │
│  │    ├─ 复制原始二进制的可分配部分                                │    │
│  │    ├─ 写入优化后的 .text / .text.cold / .rodata 等             │    │
│  │    ├─ 修补 ELF 头（程序头、段头、符号表、GOT）                  │    │
│  │    └─ 填充旧代码区域（可选 -trap-old-code）                    │    │
│  └────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 阶段 1：二进制读取与发现

**入口**：`RewriteInstance::discoverFileObjects()`

| 步骤 | 做什么 | 关键类/文件 |
|---|---|---|
| 解析 ELF 头 | 读取段头表、程序头表、符号表、动态段 | `BinaryContext` |
| 注册段 | 创建 `BinarySection` 对象，标记 allocatable/code/data | `lib/Core/BinaryContext.cpp` |
| 处理重定位 | 建立函数间/函数内引用关系 | `BinarySection::getRelocations()` |
| 创建函数桩 | 为每个函数符号创建 `BinaryFunction` stub | `lib/Core/BinaryFunction.cpp` |

---

## 阶段 2：Profile 预处理

**入口**：`RewriteInstance::preprocessProfileData()`

BOLT 支持三种 profile 格式：

| 格式 | 来源 | 读取器 | 特点 |
|---|---|---|---|
| **`.fdata`** | `perf2bolt` 或 BOLT instrumentation | `DataReader` | 预聚合的分支数据，格式 `(from_off, to_off, count, mispreds)` |
| **YAML** | BOLT YAML writer | `YAMLProfileReader` | **支持 stale profile**（旧版二进制） |
| **`perf.data`** | `perf record` | `DataAggregator` | 原始采样，内部调用 `perf script` 提取 LBR |

**Stale Profile 匹配**（`StaleProfileMatching.cpp`）：当 profile 来自旧版二进制时，通过块哈希（strict/loose/call-based/pseudo-probe）+ 流图推断（profi 库）重建可用 profile。

---

## 阶段 3：反汇编

**入口**：`RewriteInstance::disassembleFunctions()`

- 使用 LLVM `MCDisassembler` 逐函数反汇编原始字节
- 检测并分析跳转表（jump tables）
- 解析函数间引用（inter-procedural references）
- 函数状态迁移：`Empty → Disassembled`

---

## 阶段 4：CFG 构建

**入口**：`RewriteInstance::buildFunctionsCFG()`（并行执行）

- `BinaryFunction::buildCFG()` 将指令流划分为 `BinaryBasicBlock`
- 基于分支指令建立控制流边
- 分析间接分支和跳转表确定可能目标
- 函数状态迁移：`Disassembled → CFG`

---

## 阶段 5：Profile 关联到 CFG

**入口**：`RewriteInstance::processProfileData()`

- 把预加载的 profile 数据（分支计数、入口计数、误预测计数）绑定到 CFG 的边和基本块上
- 此时 profile 数据与实际的 `BinaryBasicBlock` 和 CFG edge 建立映射关系

---

## 阶段 6：优化 Pass 流水线（核心）

**入口**：`BinaryFunctionPassManager::runAllPasses()` → `bolt/lib/Rewrite/BinaryPassManager.cpp`

按执行顺序列出关键 Pass：

| 阶段 | Pass | 作用 |
|---|---|---|
| **准备** | `NormalizeCFG` | 清理空基本块、合并重复边 |
| | `RemoveNops` | 删除 NOP 指令 |
| | `ShortenInstructions` | 压缩指令操作数宽度 |
| **代码变换** | `IndirectCallPromotion` | **基于 profile 将间接调用提升为直接调用 + 守卫检查** |
| | `InlineMemcpy` | 内联优化版 memcpy |
| | `SpecializeMemcpy1` | 特化 size=1 的 memcpy |
| | `Inliner` | **Profile 引导的函数内联（热函数优先）** |
| | `IdenticalCodeFolding` | ICF：折叠相同函数节省空间（运行两次） |
| | `SimplifyRODataLoads` | 用立即数替换只读内存加载 |
| **布局优化** | **`ReorderBasicBlocks`** | **核心：用 Ext-TSP/HFSort 等算法重排基本块，最大化 fall-through** |
| | `EliminateUnreachableBlocks` | 删除不可达代码 |
| | **`SplitFunctions`** | **核心：热/冷代码分离，冷代码放入 .text.cold**（运行两次） |
| | `TailDuplication` | 尾部复制改善 fall-through |
| | `LoopInversionPass` | 循环反转改善布局 |
| | `CMOVConversion` | jcc+mov 折叠为 cmov |
| | `Peepholes` | 窥孔优化（双重跳转、无用分支） |
| | `AlignerPass` | 基本块对齐（cache/分支预测友好） |
| **函数级布局** | **`ReorderFunctions`** | **核心：用 HFSort/CDSort 重排函数顺序，改善 I-cache 局部性** |
| | `PopulateOutputFunctions` | 构建输出函数列表 |
| **数据布局** | `ReorderData` | 基于 memory profile 重排数据段 |
| **后处理** | `FrameOptimizerPass` | 优化栈帧设置/销毁 |
| | `AllocCombinerPass` | 合并栈分配 |
| | `FinalizeFunctions` | 最终化 CFG 和 CFI 状态 |
| | `AssignSections` | 为函数分配输出段 |
| | `LowerAnnotations` | 移除 BOLT 注解 |

### 三大核心优化

1. **`ReorderBasicBlocks`** (`lib/Passes/ReorderAlgorithm.cpp`)：最大化 fall-through 执行、减少分支误预测。算法：`ext-tsp`、`pettis-hansen`、`cache`、`cache-plus`
2. **`ReorderFunctions`** (`lib/Passes/ReorderFunctions.cpp`)：改善指令缓存局部性。算法：`hfsort`、`hfsort+`、`cdsort`、`exec-count`
3. **`SplitFunctions`** (`lib/Passes/SplitFunctions.cpp`)：热/温/冷代码分离，减少 I-cache 压力

---

## 阶段 7：发射与链接

**入口**：`RewriteInstance::emitAndLink()`

1. **BinaryEmitter** (`lib/Core/BinaryEmitter.cpp`)：通过 LLVM `MCObjectStreamer` 将优化后的函数发射为内存中的 ELF 对象文件
2. **JITLinkLinker** (`lib/Rewrite/JITLinkLinker.cpp`)：加载对象文件，解析所有重定位，为所有新段分配最终虚拟地址
3. 函数获得最终 `ImageAddress` 和 `ImageSize`

---

## 阶段 8：元数据更新

**入口**：`RewriteInstance::updateMetadata()`

| 组件 | 作用 |
|---|---|
| `DWARFRewriter` | 更新 DWARF 调试段（需 `-update-debug-sections`） |
| `MetadataManager` | 添加 `.note.bolt_info`（BOLT 版本、命令行） |
| **BAT** | **BOLT Address Translation** 段：记录旧地址→新地址映射，使输出二进制可被重新 profile |
| `.eh_frame` | 更新异常处理信息 |

---

## 阶段 9：重写输出二进制

**入口**：`RewriteInstance::rewriteFile()`

```
输出二进制结构：
├── 原始二进制的可分配部分（被复制）
├── .text          ← 热代码（重排后的函数和基本块）
├── .text.cold     ← 冷代码（异常处理、错误路径等罕见执行块）
├── .text.warm     ← 温代码（中等频率块）
├── .bolt.text     ← BOLT 额外生成的代码
├── .rodata        ← 重排的只读数据
├── .rodata.cold   ← 冷数据
├── .eh_frame      ← 更新后的异常处理信息
├── .note.bolt_info ← BOLT 版本和命令行
├── BAT 段         ← 地址翻译映射
└── 修补的 ELF 头
    ├── 程序头（Program Headers）
    ├── 段头表（Section Headers）
    ├── 符号表（Symbol Table）
    ├── 动态段（Dynamic Section）
    └── GOT 更新
```

### 两种输出模式

| 模式 | 条件 | 行为 |
|---|---|---|
| **Relocation Mode**（默认） | 链接时用了 `--emit-relocs` | 通过 JITLink 分配新地址，函数可自由增长 |
| **Non-Relocation Mode** | 无 `--emit-relocs` | **原地覆写**，函数增长超出原始空间时会被拒绝 |

---

## 关键源码文件索引

| 文件 | 职责 |
|---|---|
| `bolt/tools/driver/llvm-bolt.cpp` | 主入口 |
| `bolt/lib/Rewrite/RewriteInstance.cpp` | **整个流水线的编排者** |
| `bolt/lib/Core/BinaryContext.cpp` | 全局上下文（段、函数、数据对象） |
| `bolt/lib/Core/BinaryFunction.cpp` | 函数表示（状态机：Empty→Disassembled→CFG→Emitted） |
| `bolt/lib/Rewrite/BinaryPassManager.cpp` | Pass 流水线注册与执行 |
| `bolt/lib/Profile/DataReader.cpp` | .fdata profile 读取 |
| `bolt/lib/Profile/DataAggregator.cpp` | perf.data 聚合 |
| `bolt/lib/Profile/StaleProfileMatching.cpp` | 旧版 profile 匹配 |
| `bolt/lib/Core/BinaryEmitter.cpp` | 代码发射 |
| `bolt/lib/Rewrite/JITLinkLinker.cpp` | JITLink 链接 |
| `bolt/lib/Passes/ReorderAlgorithm.cpp` | 基本块重排算法 |
| `bolt/lib/Passes/ReorderFunctions.cpp` | 函数重排算法 |
| `bolt/lib/Passes/SplitFunctions.cpp` | 热冷代码分离 |
| `bolt/lib/Passes/IndirectCallPromotion.cpp` | 间接调用提升 |
| `bolt/lib/Passes/Inliner.cpp` | Profile 引导内联 |

## 其他注意事项
 * BOLT会将旧代码信息保留到`.bolt.org.text`
