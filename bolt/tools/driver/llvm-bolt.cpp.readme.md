# llvm-bolt.cpp Driver 分析

## 文件概述

**文件路径**: `bolt/tools/driver/llvm-bolt.cpp`
**总行数**: 333 行

**功能**: BOLT 工具的单一入口 driver，通过可执行文件名（symlink 方式）分发到三种不同模式：
1. **`perf2bolt`** — perf.data 聚合为 .fdata profile
2. **`llvm-boltdiff`** — 对比两个二进制 + profile 的优化效果
3. **`llvm-bolt`**（默认） — 二进制优化与布局重写

---

## 整体结构

```
main()
├── 初始化阶段
│   ├── PrintStackTraceOnErrorSignal()   // 信号处理
│   ├── llvm_shutdown_obj               // RAII 清理
│   ├── getMainExecutable()             // 获取工具路径
│   └── BOLT_TARGET() 宏展开            // 初始化目标架构（X86/AArch64/RISCV）
│
├── 模式分发（按可执行文件名）
│   ├── starts_with("perf2bolt")     → perf2boltMode()
│   ├── starts_with("llvm-boltdiff") → boltDiffMode()
│   └── 其他                          → boltMode()
│
├── 通用前置
│   ├── 检查输入文件存在性
│   └── 初始化日志流（stdout/stderr 或 -log-file）
│
├── 非 Diff 模式（perf2bolt / bolt）
│   ├── createBinary()                // 加载 ELF 或 Mach-O
│   ├── ELF 分支
│   │   ├── RewriteInstance::create()
│   │   ├── RI.setProfile()           // 加载 perf.data 或 .fdata
│   │   └── RI.run()                  // 执行完整流水线
│   └── Mach-O 分支
│       ├── MachORewriteInstance::create()
│       ├── MachORI.setProfile()
│       └── MachORI.run()
│
└── Diff 模式（llvm-boltdiff）
    ├── 加载两个二进制 + 两份 profile
    ├── 分别创建 RewriteInstance 并 run()
    └── RI1.compare(RI2)              // 输出对比报告
```

---

## 逐段注释

### 1. 头文件与命名空间（行 1-34）

```cpp
#include "bolt/Profile/DataAggregator.h"
#include "bolt/Rewrite/MachORewriteInstance.h"
#include "bolt/Rewrite/RewriteInstance.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/Binary.h"
// ... llvm Support 头文件
```

**目的作用**: 引入 BOLT 核心类（`RewriteInstance`、`MachORewriteInstance`、`DataAggregator`）和 LLVM 基础支撑库。

---

### 2. 命令行选项定义（行 36-84）

```cpp
namespace opts {
  static cl::OptionCategory *BoltCategories[] = {...};       // 5 个选项分类
  static cl::OptionCategory *BoltDiffCategories[] = {...};   // diff 专用
  static cl::OptionCategory *Perf2BoltCategories[] = {...};  // perf2bolt 专用

  static cl::opt<std::string> InputFilename(cl::Positional, ...);  // 位置参数 <executable>
  static cl::opt<std::string> InputDataFilename("data", ...);      // -data=<profile>
  static cl::alias BoltProfile("b", ...);                          // -b 是 -data 的别名
  static cl::opt<std::string> LogFile("log-file", ...);            // 日志重定向
  static cl::opt<std::string> InputDataFilename2("data2", ...);    // diff 用第二份 profile
  static cl::opt<std::string> InputFilename2(cl::Positional, ...); // diff 用第二个二进制
}
```

**目的作用**: 定义全局命令行选项。通过 `cl::OptionCategory` 分组，后续各模式用 `HideUnrelatedOptions()` 隐藏不相关选项，实现单二进制多工具的效果。

---

### 3. 错误处理与版本打印（行 86-103）

```cpp
static void report_error(StringRef Message, std::error_code EC);
static void report_error(StringRef Message, Error E);
static void printBoltRevision(llvm::raw_ostream &OS);
```

**目的作用**: 统一的错误报告（打印错误信息并 `exit(1)`）和版本号打印。

---

### 4. perf2boltMode 函数分析（行 105-133）

```cpp
void perf2boltMode(int argc, char **argv)
```

**功能**: 配置 perf2bolt 模式的命令行选项，将 `perf.data` 聚合为 BOLT 可读的 `.fdata` 格式。

**整体结构**:

```
perf2boltMode(argc, argv)
├── HideUnrelatedOptions(Perf2BoltCategories)   // 只显示 Aggregator + Output 选项
├── AddExtraVersionPrinter(printBoltRevision)
├── ParseCommandLineOptions()
├── 校验: opts::PerfData 非空
├── 校验: -data 选项不能出现（perf2bolt 不用 -data）
├── 校验: perf.data 文件存在
├── 校验: perf.data 魔数正确（DataAggregator::checkPerfDataMagic）
├── 校验: -o 输出文件非空
├── 设置: opts::AggregateOnly = true
└── 设置: opts::ShowDensity = true
```

**关键逻辑**:

| 步骤 | 行号 | 说明 |
|---|---|---|
| 隐藏无关选项 | 106 | 只保留 `AggregatorCategory` 和 `BoltOutputCategory` |
| 校验 perf.data | 120-126 | 检查文件存在 + 魔数验证 |
| 设置聚合标志 | 131 | `opts::AggregateOnly = true` 告诉 `RewriteInstance::run()` 只做聚合不做重写 |

**在 main() 中的行为**（行 247-265）: 调用 `RI.setProfile(opts::PerfData)` 加载 perf.data，然后 `RI.run()` 执行聚合。由于 `AggregateOnly=true`，`RewriteInstance` 会在 profile 预处理阶段后直接返回，不进入反汇编和优化流水线。

---

### 5. boltDiffMode 函数分析（行 135-159）

```cpp
void boltDiffMode(int argc, char **argv)
```

**功能**: 配置 llvm-boltdiff 模式的命令行选项，用于对比两个二进制+profile 的优化效果。

**整体结构**:

```
boltDiffMode(argc, argv)
├── HideUnrelatedOptions(BoltDiffCategories)   // 只显示 diff 专用选项
├── AddExtraVersionPrinter(printBoltRevision)
├── ParseCommandLineOptions()
├── 校验: -data2 非空（第二份 profile）
├── 校验: -data 非空（第一份 profile）
├── 校验: 第二个位置参数非空（第二个二进制）
├── 校验: 第一个位置参数非空（第一个二进制）
└── 设置: opts::DiffOnly = true
```

**关键逻辑**:

| 步骤 | 行号 | 说明 |
|---|---|---|
| 隐藏无关选项 | 136 | 只保留 `BoltDiffCategory` |
| 四参数校验 | 142-157 | 需要两个二进制文件 + 两份 profile |
| 设置 diff 标志 | 158 | `opts::DiffOnly = true` 告诉 main() 走 diff 分支 |

**在 main() 中的行为**（行 287-324）: 分别加载两个二进制，各自创建 `RewriteInstance`、加载 profile、执行 `run()`，最后调用 `RI1.compare(RI2)` 输出对比报告。

---

### 6. boltMode 函数分析（行 161-174）

```cpp
void boltMode(int argc, char **argv)
```

**功能**: 配置默认 llvm-bolt 模式的命令行选项，执行完整的二进制优化流水线。

**整体结构**:

```
boltMode(argc, argv)
├── HideUnrelatedOptions(BoltCategories)     // 显示全部 5 个 BOLT 选项分类
├── AddExtraVersionPrinter(printBoltRevision)
├── AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion)
├── ParseCommandLineOptions()
└── 校验: -o 输出文件非空
```

**关键逻辑**:

| 步骤 | 行号 | 说明 |
|---|---|---|
| 显示全部选项 | 162 | 包含 `BoltCategory`、`BoltOptCategory`、`BoltRelocCategory`、`BoltInstrCategory`、`BoltOutputCategory` |
| 注册目标打印 | 165 | `--version` 时额外打印支持的目标架构（X86/AArch64/RISCV） |
| 输出校验 | 170-173 | 必须指定 `-o` 输出文件 |

**在 main() 中的行为**（行 227-284）: 加载二进制 → 创建 `RewriteInstance` → 可选加载 profile → `RI.run()` 执行完整 9 阶段流水线。

---

### 7. main 函数分析（行 176-333）

```cpp
int main(int argc, char **argv)
```

**功能**: 程序入口，初始化环境、分发到三种模式之一、执行二进制优化/聚合/对比。

**整体结构**:

```
main(argc, argv)
├── 环境初始化（行 177-194）
│   ├── PrintStackTraceOnErrorSignal()        // 崩溃时打印栈追踪
│   ├── PrettyStackTraceProgram               // 记录命令行到栈追踪
│   ├── llvm_shutdown_obj                     // RAII: 退出时调用 llvm_shutdown()
│   ├── getMainExecutable()                   // 获取工具绝对路径
│   └── BOLT_TARGET 宏展开                    // 初始化目标架构
│       ├── LLVMInitialize{Target}TargetInfo()
│       ├── LLVMInitialize{Target}TargetMC()
│       ├── LLVMInitialize{Target}AsmParser()
│       ├── LLVMInitialize{Target}Disassembler()
│       ├── LLVMInitialize{Target}Target()
│       └── LLVMInitialize{Target}AsmPrinter()
│
├── 模式分发（行 196-203）
│   └── 按可执行文件名前缀选择模式
│       ├── "perf2bolt"     → perf2boltMode()
│       ├── "llvm-boltdiff" → boltDiffMode()
│       └── 其他            → boltMode()
│
├── 通用前置（行 205-224）
│   ├── 检查输入文件存在性
│   └── 初始化日志流
│       ├── 默认: BOLTJournalOut = &outs(), BOLTJournalErr = &errs()
│       └── -log-file: 重定向到文件
│
├── 非 Diff 模式（行 227-284）
│   ├── createBinary() 加载二进制
│   ├── ELF 分支（行 234-268）
│   │   ├── RewriteInstance::create()
│   │   ├── 可选: RI.setProfile(perf.data)  // 直接读 perf.data（不推荐）
│   │   ├── 可选: RI.setProfile(.fdata)     // 读预聚合 profile
│   │   └── RI.run()                        // 执行流水线
│   └── Mach-O 分支（行 269-282）
│       ├── MachORewriteInstance::create()
│       ├── 可选: MachORI.setProfile(.fdata)
│       └── MachORI.run()
│
└── Diff 模式（行 287-330）
    ├── 加载两个二进制
    ├── 分别创建 RewriteInstance + 加载 profile + run()
    └── RI1.compare(RI2)
```

---

### 8. 目标初始化宏（行 186-194）

```cpp
#define BOLT_TARGET(target)                                                    \
  LLVMInitialize##target##TargetInfo();                                        \
  LLVMInitialize##target##TargetMC();                                          \
  LLVMInitialize##target##AsmParser();                                         \
  LLVMInitialize##target##Disassembler();                                      \
  LLVMInitialize##target##Target();                                            \
  LLVMInitialize##target##AsmPrinter();

#include "bolt/Core/TargetConfig.def"
```

**目的作用**: 通过 `TargetConfig.def` 中定义的 `BOLT_TARGET(X86)`、`BOLT_TARGET(AArch64)`、`BOLT_TARGET(RISCV)` 等宏调用，初始化所有支持架构的反汇编器、目标信息和汇编打印机。每个架构需要 6 个初始化函数。

---

## 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `cl::OptionCategory` | `BoltCategories[5]` | 5 个选项分类：Bolt、BoltOpt、BoltReloc、BoltInstr、BoltOutput |
| `cl::OptionCategory` | `BoltDiffCategories[1]` | Diff 专用选项分类 |
| `cl::OptionCategory` | `Perf2BoltCategories[2]` | Perf2Bolt 专用：Aggregator + Output |
| `raw_ostream*` | `BOLTJournalOut/Err` | 日志输出流，可重定向到文件 |

---

## 模式对比

| 特性 | perf2bolt | llvm-boltdiff | llvm-bolt（默认） |
|---|---|---|---|
| **可执行文件名** | `perf2bolt` | `llvm-boltdiff` | `llvm-bolt` 或其他 |
| **输入** | 二进制 + perf.data | 2 个二进制 + 2 份 .fdata | 二进制 + 可选 .fdata |
| **输出** | .fdata 文件 | 对比报告（stdout） | 优化后的二进制 |
| **核心动作** | profile 聚合 | 分别分析 + 对比 | 完整优化流水线 |
| **关键标志** | `AggregateOnly=true` | `DiffOnly=true` | 无特殊标志 |
| **使用的类** | `RewriteInstance` | `RewriteInstance` × 2 | `RewriteInstance` 或 `MachORewriteInstance` |
| **支持格式** | 仅 ELF | 仅 ELF | ELF + Mach-O |

---

## 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 模式按文件名分发 | 通过 `starts_with("perf2bolt")` 判断，依赖 symlink | 如果可执行文件被重命名，模式分发会错误 |
| perf.data 直接读取 | 行 248-254 有 WARNING 提示不推荐 | 可能导致 profile 解析不完整 |
| ArmSPE 仅限 AArch64 | 行 241-245 检查 | 在非 AArch64 上使用 -spe 会报错退出 |
| Diff 仅支持 ELF | 行 298-327 只处理 `ELFObjectFileBase` | Mach-O 二进制无法使用 diff 功能 |
| Mach-O 无 journal 流 | 行 270 `MachORewriteInstance::create()` 不传日志流 | Mach-O 模式日志输出可能不同 |

---

## 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 模式分发 | `llvm::sys::path::filename().starts_with()` | 行 198-203 |
| 选项隐藏 | `cl::HideUnrelatedOptions()` | 行 106, 136, 162 |
| 二进制加载 | `llvm::object::createBinary()` | 行 228-229 |
| ELF 重写实例创建 | `RewriteInstance::create()` | 行 235-236 |
| Mach-O 重写实例创建 | `MachORewriteInstance::create()` | 行 270 |
| Profile 加载 | `RewriteInstance::setProfile()` | 行 255, 259 |
| 执行流水线 | `RewriteInstance::run()` | 行 267 |
| 二进制对比 | `RewriteInstance::compare()` | 行 324 |
| 目标初始化 | `BOLT_TARGET` 宏 + `TargetConfig.def` | 行 186-194 |

---

## 其他补充

**Symlink 多工具模式**: BOLT 采用 LLVM 常见的"单一二进制 + 按文件名分发"模式。构建系统会创建 `perf2bolt` 和 `llvm-boltdiff` 作为 `llvm-bolt` 的 symlink（或硬链接），运行时通过检查 `argv[0]` 的文件名前缀决定行为。

**AggregateOnly 的工作机制**: 当 `opts::AggregateOnly = true` 时，`RewriteInstance::run()` 会在完成 profile 聚合后直接返回，跳过反汇编、CFG 构建、优化 Pass 和二进制重写。这是 perf2bolt 模式能只输出 .fdata 文件而不生成新二进制的原因。
