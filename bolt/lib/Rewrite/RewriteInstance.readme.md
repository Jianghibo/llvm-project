# RewriteInstance.cpp 源码走读（BOLT ELF 重写主引擎）

本文档对应实现文件：`bolt/lib/Rewrite/RewriteInstance.cpp`、接口：`bolt/include/bolt/Rewrite/RewriteInstance.h`。行号以当前树为准（共 7091 行），若上游改动请以源码为准。

**阅读方式**：先读[整体概括](#整体概括)与[主调用树](#主调用树)建立骨架，再按调用关系顺序阅读各函数分析（每个函数均按统一模板展开：函数签名与目的 → 整体结构 → 逐段注释 → 关键数据结构 → 优化意图 → 约束与易错点 → 关键 API / 源码路径）。

**章节组织（按函数调用关系）**：

| 章 | 内容 | 函数数 |
|---|---|---|
| 一 | 构造与初始化 | 5 |
| 二 | `run()` 主流程 | 1 |
| 三 | 存储与 section 发现 | 9 |
| 四 | 符号与函数发现（discoverFileObjects 族） | 3 |
| 五 | 重定位处理 | 12 |
| 六 | PLT 反汇编 | 6 |
| 七 | 函数边界与 fragment | 7 |
| 八 | 插桩运行时钩子 | 4 |
| 九 | Profile 与函数选择 | 8 |
| 十 | 反汇编与 CFG 构建 | 5 |
| 十一 | Metadata 与优化 Pass | 7 |
| 十二 | 发射与链接（emitAndLink） | 10 |
| 十三 | 输出文件重写（rewriteFile） | 20 |
| 十四 | 地址翻译与工具函数 | 7 |

---

## 整体概括

**文件定位**：BOLT 对 ELF 二进制做 post-link 优化的**总驱动**。一个 `RewriteInstance` 对应一个输入 ELF：从读入原始二进制（符号、section、重定位、EH frame、debug info、profile），到反汇编建 CFG、跑 BOLT pass 流水线，再到把优化后的代码发射成内存 `.o`、经 JITLink 分配地址，最后回写成一个**可执行的新 ELF 文件**（PHDR 表、符号表、`.eh_frame`、动态段、RELA/RELR 重定位表、GOT、section header 表全量修补）。

**两阶段视角**：

```text
阶段 A：理解输入（读）
  输入 ELF ──符号/section/重定位/FDE──> BinaryContext (BC)
           ──disassemble──> BinaryFunction (MCInst + annotations)
           ──buildCFG──> MCFG (BinaryBasicBlock + 边 + 跳转表)
           ──profile──> 执行计数（块/边频度）

阶段 B：重写输出（写）
  优化后函数 ──emit──> 内存中 .o (MCStreamer)
           ──JITLink──> 地址分配/重定位消解
           ──rewriteFile──> 输出 ELF（全量 ELF 补丁）
```

**贯穿全文件的四个关键模式**：

1. **两套模式**：relocation mode（输入带 `--emit-relocs`，函数可移动并修补所有引用）与非 relocation mode（函数只能原地覆写，尺寸不得超过原空间）。大量分支以 `BC->HasRelocations` 区分。
2. **模板分发**：ELF32/ELF64 小端各一套实现，`template <typename ELFT>` + 顶层按 `ELF64LEObjectFile`/`ELF32LEObjectFile` 实例化。
3. **旧地址→新地址翻译**：所有"引用修补"最终收敛到 `getNewValueForSymbol` / `getNewFunctionAddress` / `getNewFunctionOrDataAddress` 这组函数。
4. **错误处理**：可恢复的用 `BOLT-WARNING` + 降级（`setSimple(false)`/`setIgnored()`）；不可恢复的直接 `exit(1)`。

---

## 主调用树

`llvm-bolt` driver（`bolt/tools/driver/llvm-bolt.cpp`）先构造再 `run()`：

```text
RewriteInstance::create()                          // L403  工厂（错误传出）
└── RewriteInstance::RewriteInstance()             // L415  构造 BC/MCPlusBuilder/BAT 等
    ├── BinaryContext::createBinaryContext()       // 外部文件
    ├── createMCPlusBuilder()                      // L367  按架构分发 X86/AArch64/RISCV
    └── (driver 随后调用) setProfile()             // L486  按文件魔数选 profile reader

run()                                              // L811  主流水线
├── selectFunctionsToPrint()                       // L3665 装载 -print-only 列表
│    └── populateFunctionNames()                   // L3655 从文件读函数名
├── discoverStorage()                              // L636  规划新代码地址、PHDR 表位置
├── readSpecialSections()                          // L2444 注册全部 section、解析 FDE 索引
│    ├── markGnuRelroSections()                    // L594  标记 GNU_RELRO section
│    │    ├── checkOffsets()                       // L557  文件偏移包含性检查
│    │    └── checkVMA()                           // L579  虚拟地址包含性检查
│    ├── processSectionMetadata()                  // L3926
│    │    └── initializeMetadataManager()          // L3911 注册 6 个 metadata rewriter
│    └── readELFDynamic()                          // L6520 解析 PT_DYNAMIC
├── adjustCommandLineOptions()                     // L2578 选项互斥检查/自动降级
├── discoverFileObjects()                          // L895  ★ 符号→函数发现核心
│    ├── createRISCVIFuncResolverFunctions()       // L534
│    ├── processDynamicRelocations()               // L2950
│    │    ├── readDynamicRelrRelocations()         // L3090 解压 RELR 位图
│    │    │    └── handleRelativeDynamicRelocation()// L3144
│    │    └── readDynamicRelocations()             // L3029 DT_RELA/DT_JMPREL
│    │         └── handleRelativeDynamicRelocation()
│    ├── disassemblePLT()                          // L2116 按架构分发
│    │    ├── disassemblePLTSectionAArch64()       // L1986
│    │    │    ├── disassemblePLTInstruction()     // L1965
│    │    │    └── createPLTBinaryFunction()       // L1891
│    │    ├── disassemblePLTSectionRISCV()         // L2030
│    │    │    └── createPLTBinaryFunction()
│    │    └── disassemblePLTSectionX86()           // L2077
│    │         ├── disassemblePLTInstruction()
│    │         └── createPLTBinaryFunction()
│    ├── adjustFunctionBoundaries()                // L2306 MaxSize/二级入口
│    │    └── isCFIBoundedTailPredecessor()        // L2155
│    ├── splitUnmarkedTailFunctions()              // L2260 AArch64 尾巴切分
│    │    ├── isCFIBoundedTailPredecessor()
│    │    └── measureAArch64UnmarkedTail()         // L2190
│    │         ├── isAArch64TailPaddingInst()      // L2167
│    │         └── isValidAArch64UnmarkedTail()    // L2176
│    ├── processRelocations()                      // L3010 静态重定位
│    │    └── readRelocations()                    // L3189
│    │         └── handleRelocation()              // L3227 ★ 单条重定位分类处理
│    │              ├── analyzeRelocation()        // L2819 提取符号/addend/校验
│    │              │    ├── getRelocationAddend() // L2754
│    │              │    └── getRelocationSymbol() // L2789
│    │              └── printRelocationInfo()      // L3165 调试打印
│    ├── registerFragments()                       // L1736 cold 片段挂回 parent
│    └── discoverBOLTReserved()                    // L1494 BOLT 预留空间
├── [opts::Instrument] discoverRtInitAddress()     // L1521
│                     discoverRtFiniAddress()      // L1580
├── preprocessProfileData()                        // L3880
├── selectFunctionsToProcess()                     // L3669
│    ├── populateFunctionNames()
│    └── getInitFunctionIfStaticBinary()           // L3638 aarch64 静态 glibc workaround
├── readDebugInfo()                                // L3863
├── disassembleFunctions()                         // L4001
│    └── shouldDisassemble()                       // L517
├── processMetadataPreCFG()                        // L3934
│    └── processProfileDataPreCFG()                // L3948
├── buildFunctionsCFG()                            // L4135 并行建 CFG
├── processProfileData()                           // L3959 profile 绑定到 CFG
├── [opts::EnableBAT] BAT->saveMetadata()
├── postProcessFunctions()                         // L4175
├── processMetadataPostCFG()                       // L3942
├── [opts::DiffOnly] return                        // boltdiff 出口
├── [opts::BinaryAnalysisMode] runBinaryAnalyses() // L4224 分析后 return
├── preregisterSections()                          // L4269
├── runOptimizationPasses()                        // L4218
├── finalizeMetadataPreEmit()                      // L4402
├── emitAndLink()                                  // L4289 ★ 发射 + 链接
│    ├── relocateEHFrameSection()                  // L2396 原 .eh_frame 造重定位副本
│    ├── emitBinaryContext()                       // (BinaryEmitter.cpp)
│    ├── JITLinkLinker::loadObject()
│    │    └── [回调] mapFileSections()             // L4423
│    │         ├── mapCodeSections()               // L4590
│    │         │    ├── getCodeSections()          // L4548
│    │         │    │    └── CodeSectionOrder      // L4462 排序器
│    │         │    ├── mapCodeSectionsInPlace()    // L4769 非重定位模式
│    │         │    └── mergeCodeSections()        // L4726
│    │         └── mapAllocatableSections()        // L4843
│    ├── updateOutputValues()                      // L4957
│    └── RuntimeLibrary::link() ──> mapAllocatableSections() [回调]
├── updateMetadata()                               // L4408
│    └── addBoltInfoSection()                      // L5291
├── [opts::Instrument] updateRtInitReloc()         // L1625
│                     updateRtFiniReloc()          // L1691
└── rewriteFile()                                  // L6806 ★ 输出文件全量重写
     ├── rewriteFunctionsInPlace()                 // L6680 非重定位模式覆写
     ├── writeEHFrameHeader()                      // L6930
     ├── updateSegmentInfo()                       // L4965
     ├── patchELFPHDRTable()                       // L5033
     ├── finalizeSectionStringTable()              // L5271
     ├── patchELFSymTabs()                         // L6088
     │    ├── getOutputSections()                  // L5351（预演拿 index 映射）
     │    └── updateELFSymbolTable()               // L5617 ★ 符号表重写
     ├── [opts::EnableBAT] addBATSection()         // L5309
     │                     encodeBATSection()      // L5316
     ├── rewriteNoteSections()                     // L5170 非分配 section 拷贝+补丁
     │    ├── shouldStrip()                        // L5332
     │    ├── appendPadding()                      // L5156
     │    └── willOverwriteSection()               // L7070
     ├── [opts::UseOldText] zeroPaddingForReusedSections() // L6765
     ├── [HasRelocations] patchELFAllocatableRelaSections() // L6270
     │                     patchELFAllocatableRelrSection() // L6178
     │                     patchELFGOT()                      // L6385
     ├── patchELFDynamic()                         // L6420
     └── patchELFSectionHeaderTable()              // L5561
          └── getOutputSections()
```

---

# 一、构造与初始化

## createMCPlusBuilder 函数分析

### 函数签名与目的（L367-388）

```cpp
MCPlusBuilder *createMCPlusBuilder(const Triple::ArchType Arch,
                                    const MCInstrAnalysis *Analysis,
                                    const MCInstrInfo *Info,
                                    const MCRegisterInfo *RegInfo,
                                    const MCSubtargetInfo *STI);
```

**功能**: 按目标架构创建对应的 `MCPlusBuilder`（BOLT 对 MCInst 的注解/元信息扩展层工厂）。

### 整体结构

```text
createMCPlusBuilder(Arch, Analysis, Info, RegInfo, STI)
├── [X86_AVAILABLE] Arch == x86_64    → createX86MCPlusBuilder(...)
├── [AARCH64_AVAILABLE] Arch == aarch64 → createAArch64MCPlusBuilder(...)
├── [RISCV_AVAILABLE] riscv64/riscv32  → createRISCVMCPlusBuilder(...)
└── 其余 → llvm_unreachable
```

### 逐段注释

无复杂分段。三组 `#ifdef` 编译期开关 + `if (Arch == ...)` 运行期判别，将工厂请求转发到 `libTarget` 里各架构的 `MCPlusBuilder` 实现；不支持的架构 `llvm_unreachable`。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MCPlusBuilder` | 注解索引表、架构判定接口 | 给 MCInst 附加 BOLT 私有元数据（跳转表、CFI 状态、局部 label 等）的抽象层，定义于 `bolt/Core/MCPlusBuilder.h` |

### 优化意图

1. **为什么定义在本文件**：源码注释明确说明——该函数的自然位置是 libCore，但 libCore 不能依赖 libTarget（循环依赖）；libRewrite 依赖 libTarget 且是唯一使用者，故落位于此（L363-366）。
2. `#ifdef` 开关使未启用某架构 target 的构建不引入符号依赖，减小链接面。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 架构必须受支持 | 只覆盖 x86_64/aarch64/riscv32/64 | 其余架构命中 `llvm_unreachable`（release 下 UB） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| X86 工厂 | `createX86MCPlusBuilder` | `bolt/lib/Target/X86/X86MCPlusBuilder.cpp` |
| AArch64 工厂 | `createAArch64MCPlusBuilder` | `bolt/lib/Target/AArch64/AArch64MCPlusBuilder.cpp` |
| RISCV 工厂 | `createRISCVMCPlusBuilder` | `bolt/lib/Target/RISCV/RISCVMCPlusBuilder.cpp` |

### 其他补充

调用点唯一：`RewriteInstance` 构造函数 L469-471。

---

## RewriteInstance::create 函数分析

### 函数签名与目的（L403-413）

```cpp
Expected<std::unique_ptr<RewriteInstance>>
RewriteInstance::create(ELFObjectFileBase *File, const int Argc,
                        const char *const *Argv, StringRef ToolPath,
                        raw_ostream &Stdout, raw_ostream &Stderr);
```

**功能**: `RewriteInstance` 的工厂函数，把"构造可能失败"转成 `Expected` 返回值。

### 整体结构

```text
create(...)
├── Err = success
├── RI = make_unique<RewriteInstance>(..., Err)   // 构造失败写 Err
├── Err 非空 → 返回 std::move(Err)
└── 返回 std::move(RI)
```

### 逐段注释

无复杂分段。典型的 LLVM **ErrorAsOutParameter** 惯用法：构造函数通过出参 `Error &Err` 上报错误，这里包装成 `Expected`，保证错误路径不会产生"半构造对象被使用"的问题。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Expected<std::unique_ptr<RewriteInstance>>` | value/error 双态 | LLVM 错误传播标准载体（`llvm/Support/Error.h`） |

### 优化意图

1. 构造函数无法返回错误，出参 + 工厂是 LLVM 处理"failable constructor"的标准方案，调用方（driver）用 `Error` 链统一收尾。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| Err 必须被检查 | 返回 `Expected`，调用方必须消费 | 忽略会触发 assert（Error 未 check） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 错误出参守卫 | `ErrorAsOutParameter` | `llvm/Support/Error.h` |
| 调用方 | `llvm-bolt` driver | `bolt/tools/driver/llvm-bolt.cpp` |

### 其他补充

无。

---

## RewriteInstance::RewriteInstance（构造函数）函数分析

### 函数签名与目的（L415-482）

```cpp
RewriteInstance::RewriteInstance(ELFObjectFileBase *File, const int Argc,
                                  const char *const *Argv, StringRef ToolPath,
                                  raw_ostream &Stdout, raw_ostream &Stderr,
                                  Error &Err)
    : InputFile(File), Argc(Argc), Argv(Argv), ToolPath(ToolPath),
      SHStrTab(StringTableBuilder::ELF);
```

**功能**: 搭建 BOLT 运行所需的全部基础设施：格式校验、PIC 判定、`BinaryContext` 创建、`MCPlusBuilder` 注入、BAT / DWARF rewriter / RuntimeLibrary 的按需构造。

### 整体结构

```text
RewriteInstance(...)
├── 1. ELF 格式校验（仅 32/64 位小端）
├── 2. PIC 判定（e_type != ET_EXEC）
├── 3. 输出流置无缓冲（防 core dump 丢日志）
├── 4. RISC-V: 从输入文件读 subtarget features
├── 5. BinaryContext::createBinaryContext(...)
│      └── DWARFContext::create(..., Ignore, ...)
├── 6. BC->initializeTarget(createMCPlusBuilder(...))
├── 7. BAT = make_unique<BoltAddressTranslation>()
├── 8. [UpdateDebugSections] DebugInfoRewriter = DWARFRewriter
└── 9. [Instrument|Hugify] 设置对应 RuntimeLibrary
```

### 逐段注释

**1. 格式与 PIC 检测 (L421-435)**

```cpp
if (!isa<ELF64LEObjectFile>(InputFile) && !isa<ELF32LEObjectFile>(InputFile)) {
  Err = createStringError(errc::not_supported,
                          "Only 32-bit and 64-bit LE ELF binaries are supported");
  return;
}
bool IsPIC = false;
if (File->getEType() != ELF::ET_EXEC) {
  Stdout << "BOLT-INFO: shared object or position-independent executable detected\n";
  IsPIC = true;
}
```

ELF 重写路径只接受小端 ELF32/ELF64；`ET_DYN`（共享库或 PIE）统一标记 `IsPIC`，后续影响地址假设（`HasFixedLoadAddress`）与重定位处理策略。

**2. 无缓冲输出 (L437-440)**

```cpp
Stdout.SetUnbuffered();
Stderr.SetUnbuffered();
LLVM_DEBUG(dbgs().SetUnbuffered());
```

崩溃（core dump）时 libc 缓冲区里的日志会丢失，BOLT 排障高度依赖这些日志，故全部立刻刷出。

**3. RISC-V features (L443-453)**

```cpp
if (TheTriple.isRISCV()) {
  Expected<SubtargetFeatures> FeaturesOrErr = File->getFeatures();
  ...
  Features.reset(new SubtargetFeatures(*FeaturesOrErr));
}
```

RISC-V 扩展（如 C 压缩指令）直接决定反汇编正确性，必须从 ELF 属性段恢复并传给 `createBinaryContext`。

**4. 创建 BinaryContext (L455-471)**

```cpp
Relocation::Arch = TheTriple.getArch();
auto BCOrErr = BinaryContext::createBinaryContext(
    TheTriple, std::make_shared<orc::SymbolStringPool>(), File->getFileName(),
    Features.get(), IsPIC,
    DWARFContext::create(*File, DWARFContext::ProcessDebugRelocations::Ignore,
                         nullptr, opts::DWPPathName, ...),
    JournalingStreams{Stdout, Stderr});
...
BC = std::move(BCOrErr.get());
BC->initializeTarget(std::unique_ptr<MCPlusBuilder>(
    createMCPlusBuilder(BC->TheTriple->getArch(), BC->MIA.get(), ...)));
```

`Relocation::Arch` 是全局静态锚点，此后 `Relocation::getPC32()` 等按架构取值；DWARFContext 创建时**忽略** debug 重定位（BOLT 自己处理）；`orc::SymbolStringPool` 供后续 JITLink 使用。

**5. 可选组件 (L473-481)**

BAT（新旧地址翻译表）、`DWARFRewriter`（`--update-debug-sections`）、RuntimeLibrary（instrument/hugify 互斥二选一）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryContext` | 函数表/section 表/符号表/MC 层 | BOLT 全局上下文（`bolt/Core/BinaryContext.h`） |
| `BoltAddressTranslation` | 新旧地址映射 | 二次 BOLT / 采样 profile 的翻译层 |
| `RuntimeLibrary` 派生 | runtime start/fini 地址 | 插桩 / hugify 运行库链接与钩子 |

### 优化意图

1. **journaling 流**：`JournalingStreams{Stdout, Stderr}` 让 BC 的日志输出与工具主输出统一，且可被 replay（BOLT 复现问题用）。
2. 组件全部按需构造（`make_unique` 延迟到确定需要时），避免无 profile / 无插桩场景白白占用内存。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅小端 ELF32/64 | 大端 / ELF class 不符直接失败 | 命中报错返回，无风险 |
| Instrument 与 Hugify 互斥 | if/else if 链保证 | 同开时 Hugify 静默失效 |
| 构造失败必须短路返回 | 每个错误分支 `return` 前已设置 Err | 忘记 return 会继续用半初始化状态 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 创建 BC | `BinaryContext::createBinaryContext` | `bolt/lib/Core/BinaryContext.cpp` |
| DWARF 上下文 | `DWARFContext::create` | `llvm/DebugInfo/DWARF/DWARFContext.h` |
| 目标注入 | `BC->initializeTarget` | `bolt/Core/BinaryContext.h` |

### 其他补充

`Argc/Argv/ToolPath` 保存进成员，供 `addBoltInfoSection`（写 bolt info note）与 `emitAndLink`（运行库链接需要工具路径找 runtime 库文件）后续使用。

---

## RewriteInstance::~RewriteInstance 函数分析

### 函数签名与目的（L484）

```cpp
RewriteInstance::~RewriteInstance() {}
```

**功能**: 空默认析构。

### 整体结构

无逻辑。所有资源（`BC`、`BAT`、`Linker`、`Out` 等）均为 `unique_ptr` 成员，自动释放。

### 逐段注释

无。

### 关键数据结构

无本地结构。

### 优化意图

1. 显式写出空析构是为了在头文件/实现分离时保持所有权语义清晰（全部成员 RAII）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 成员析构顺序 | 声明序逆序释放 | `Out`（输出文件）与 `Linker` 的存活期需覆盖 `rewriteFile` 全程，由 `run()` 顺序保证 |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## setProfile 函数分析

### 函数签名与目的（L486-514）

```cpp
Error RewriteInstance::setProfile(StringRef Filename);
```

**功能**: 注册 profile 输入文件，按文件内容魔数选择 `ProfileReader` 实现。

### 整体结构

```text
setProfile(Filename)
├── 1. 文件不存在 → no_such_file_or_directory
├── 2. 已有 reader:
│      同为 DataAggregator → addInputFile 追加（多 perf.data 合并）
│      否则 → "multiple profiles specified" 错误
└── 3. 首次注册，按魔数分发:
       perf.data 魔数 → DataAggregator
       YAML        → YAMLProfileReader
       其他        → DataReader (.fdata 文本)
```

### 逐段注释

**1. 重复注册分支 (L490-503)**

```cpp
if (ProfileReader) {
  if (DataAggregator::checkPerfDataMagic(Filename) &&
      ProfileReader->getReaderName() == StringRef("perf data aggregator")) {
    static_cast<DataAggregator *>(ProfileReader.get())->addInputFile(Filename);
    return Error::success();
  }
  return make_error<StringError>(Twine("multiple profiles specified: ") + ...);
}
```

"Poorman's RTTI"（源码注释）：`ProfileReader` 体系无 LLVM RTTI，用 `getReaderName()` 字符串比较代替 `dyn_cast`。同为 perf 聚合器时追加输入文件（多次采样合并），异类则报"多 profile"错误。

**2. 魔数分发 (L505-511)**

```cpp
if (DataAggregator::checkPerfDataMagic(Filename))
  ProfileReader = std::make_unique<DataAggregator>(Filename);
else if (YAMLProfileReader::isYAML(Filename))
  ProfileReader = std::make_unique<YAMLProfileReader>(Filename);
else
  ProfileReader = std::make_unique<DataReader>(Filename);
```

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ProfileReader`（成员） | `std::unique_ptr<ProfileReader>` | 多态基类（`bolt/Profile/ProfileReader.h`），后续 preprocess/readProfile 全走它 |

### 优化意图

1. 按内容而非扩展名分发：perf2bolt / YAML（BOLT 导出，支持 stale matching）/ `.fdata` 文本三种格式自动识别，降低用户使用门槛。
2. 允许 DataAggregator 聚合多个 perf.data——生产环境多次采样的标准做法。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 非 DataAggregator 只能一个 | 第二个 profile 直接报错 | 用户混用格式时失败，需提示 |
| 魔数识别顺序 | perf 魔数优先于 YAML | perf.data 恰为 YAML 文本的概率为零，顺序安全 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| perf 魔数检测 | `DataAggregator::checkPerfDataMagic` | `bolt/lib/Profile/DataAggregator.cpp` |
| YAML 检测 | `YAMLProfileReader::isYAML` | `bolt/lib/Profile/YAMLProfileReader.cpp` |

### 其他补充

调用方是 driver（`llvm-bolt.cpp`），发生在 `run()` 之前；`ProfileReader` 在 `processProfileData` 结束后 `reset()` 释放。

---

# 二、run() 主流程

## run 函数分析

### 函数签名与目的（L811-893）

```cpp
Error RewriteInstance::run();
```

**功能**: BOLT 主流水线入口：按固定数据依赖顺序调度"发现 → profile → 反汇编 → CFG → 优化 → 发射 → 回写"全部阶段。

### 整体结构

```text
run()
├── 1. 打印目标架构与 BOLT 版本
├── 2. selectFunctionsToPrint()
├── 3. discoverStorage()                     // 新代码地址规划
├── 4. readSpecialSections()                 // section 注册 + FDE 索引
├── 5. adjustCommandLineOptions()            // 知彼后的选项裁决
├── 6. discoverFileObjects()                 // 符号→函数（含重定位/PLT）
├── 7. [Instrument 且非静态] discoverRtInitAddress/FiniAddress
├── 8. preprocessProfileData()
├── 9. selectFunctionsToProcess()            // 处理/跳过名单
├── 10. readDebugInfo()
├── 11. disassembleFunctions()
├── 12. processMetadataPreCFG()              // 内含 processProfileDataPreCFG
├── 13. buildFunctionsCFG()                  // 并行建 CFG
├── 14. processProfileData()                 // profile 绑定 CFG
├── 15. [EnableBAT] BAT->saveMetadata()
├── 16. postProcessFunctions()
├── 17. processMetadataPostCFG()
├── 18. [DiffOnly] return                    // boltdiff 出口
├── 19. [BinaryAnalysisMode] runBinaryAnalyses() 后 return
├── 20. preregisterSections()
├── 21. runOptimizationPasses()
├── 22. finalizeMetadataPreEmit()
├── 23. emitAndLink()
├── 24. updateMetadata()
├── 25. [Instrument 且非静态] updateRtInitReloc/FiniReloc
├── 26. [Output==/dev/null] return（不落盘）
└── 27. rewriteFile()
```

### 逐段注释

**1. 前置输出 (L814-818)**：目标架构名与 `BoltRevision` 首先打印——用户报 bug 时的第一手信息。

**2. 阶段顺序即依赖序 (L820-848)**

```cpp
selectFunctionsToPrint();
if (Error E = discoverStorage()) return E;
if (Error E = readSpecialSections()) return E;
adjustCommandLineOptions();
discoverFileObjects();
...
disassembleFunctions();
processMetadataPreCFG();
buildFunctionsCFG();
processProfileData();
```

顺序不可随意调整：`discoverStorage` 要在 `readSpecialSections` 前（section 注册需要知道布局锚点）；profile 绑定必须在建 CFG 后（计数挂在块/边上）；`processProfileDataPreCFG` 提供函数级入口计数，`buildFunctionsCFG` 之前的 `selectFunctionsToProcess`/lite 阈值决策依赖它（经 `processMetadataPreCFG` 调用）。

**3. 三个提前出口 (L858-865)**

```cpp
if (opts::DiffOnly)
  return Error::success();
if (opts::BinaryAnalysisMode) {
  runBinaryAnalyses();
  return Error::success();
}
```

boltdiff 只需要 CFG 与 profile；binary-analysis（如 PAuth gadget 扫描）不需要重写输出。

**4. /dev/null 出口 (L883-888)**：`-o /dev/null` 表示"只跑到 emit 为止"（性能实验/冒烟测试），跳过落盘；Linux kernel 则打印实验性支持警告。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::DiffOnly/BinaryAnalysisMode/Instrument/EnableBAT` | cl::opt 开关 | 决定流水线形态的模式开关（`bolt/Utils/CommandLineOpts.cpp`） |
| `BC->IsStaticExecutable` | bool | 静态可执行判定，决定插桩钩子是否需要 |

### 优化意图

1. **每阶段一个函数**：`run()` 本体只有调度，逻辑全部下沉，方便逐段打断点/计时（每个子函数自带 `NamedRegionTimer`，`-time-rewrite` 可输出阶段耗时）。
2. 早出口避免无谓计算：diff/analysis 模式不进入 emit/rewrite。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 阶段顺序不可重排 | 见"逐段注释"2 | 乱序会导致空指针（如 BAT 保存早于 profile 绑定） |
| Instrument 且 IsStaticExecutable 时不发现 init/fini | 静态二进制钩子机制不同 | 条件与 updateRt*Reloc 的调用条件严格一致 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 阶段计时 | `NamedRegionTimer` | `llvm/Support/Timer.h` |
| 错误上抛 | `Error`/`report_error` | `llvm/Support/Error.h`、`bolt/Utils/Utils.h` |

### 其他补充

`assert(BC && ...)` 防御性检查在首行；`run()` 返回的 `Error` 由 driver 统一 `reportError` 后退出。

---

# 三、存储与 section 发现

## checkOffsets 函数分析

### 函数签名与目的（L557-575，匿名命名空间，模板）

```cpp
template <class ELFT>
static bool checkOffsets(const typename ELFT::Phdr &Phdr,
                         const typename ELFT::Shdr &Sec, bool &Overlap);
```

**功能**: 判断 section 的**文件镜像区间**（`sh_offset` 起）是否完整落在 segment 的文件区间（`p_offset..p_offset+p_filesz`）内。

### 整体结构

```text
checkOffsets(Phdr, Sec, Overlap)
├── SHT_NOBITS → true（无文件内容）
├── 构造 SectionAddressRange / SegmentAddressRange
├── contains → true
└── intersects → Overlap = true，返回 false
```

### 逐段注释

简单函数，无代码片段。空 section 按 1 字节处理（`SectionSize = Sec.sh_size ? Sec.sh_size : 1`），使"空 section 恰在 segment 末尾"也算包含；不完整包含但相交时把出参 `Overlap` 置真（部分重叠是危险信号，调用方据此放弃并告警）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AddressRange` | start/end | `llvm/ADT/AddressRanges.h`，`contains/intersects` |

### 优化意图

1. 借助 `AddressRange` 工具类把"区间包含/相交"从手写比较变成语义化调用，减少边界错误。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只看文件偏移 | 与 `checkVMA` 的虚拟地址通道互补 | 单独使用会漏掉"文件内对但 VMA 不对"的畸形布局 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 区间运算 | `AddressRange::contains/intersects` | `llvm/ADT/AddressRanges.h` |

### 其他补充

仅被 `markGnuRelroSections` 调用。

---

## checkVMA 函数分析

### 函数签名与目的（L579-592，匿名命名空间，模板）

```cpp
template <class ELFT>
static bool checkVMA(const typename ELFT::Phdr &Phdr,
                     const typename ELFT::Shdr &Sec, bool &Overlap);
```

**功能**: 与 `checkOffsets` 同构，但比较**虚拟地址区间**（`sh_addr` vs `p_vaddr..p_vaddr+p_memsz`）。

### 整体结构

与 `checkOffsets` 完全同构（区别仅在于用 `sh_addr/p_vaddr/p_memsz`，且**不**处理 `SHT_NOBITS` 特例——.bss 类 section 在 VMA 通道同样要校验）。

### 逐段注释

简单函数，无代码片段。同样"空 section 按 1 字节"与 `Overlap` 出参语义。

### 关键数据结构

同 `checkOffsets`。

### 优化意图

1. 文件镜像与虚拟地址双通道校验：加载器以 VMA 为准，`strip/objcopy` 等工具以文件偏移为准，两者必须同时成立才能安全打 RELRO 标记。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| p_memsz 可能大于 p_filesz | .bss 尾部 | `AddressRange` 构造已涵盖 |

### 关键 API / 源码路径

同 `checkOffsets`。

### 其他补充

仅被 `markGnuRelroSections` 调用。

---

## markGnuRelroSections 函数分析

### 函数签名与目的（L594-634，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::markGnuRelroSections(ELFObjectFile<ELFT> *ELFObjFile);
```

**功能**: 对每个 `PT_GNU_RELRO` segment 内的可分配 section 打 `setRelro()` 标记。

### 整体结构

```text
markGnuRelroSections(ELFObjFile)
├── 遍历 program headers，筛 PT_GNU_RELRO
└── 对该 segment 的每个 section 调 handleSection(Phdr, SecRef):
     ├── BinarySection 缺失或非 allocatable → 返回
     ├── checkOffsets → ImageOverlap / ImageContains
     ├── checkVMA     → VMAOverlap / VMAContains
     ├── ImageOverlap → WARNING 后放弃
     ├── VMAOverlap   → WARNING 后放弃
     ├── 双通道都完整包含 → BinarySection->setRelro()
     └── [Verbosity>=1] 打印标记信息
```

### 逐段注释

**1. handleSection lambda（L599-628）**

```cpp
auto handleSection = [&](const typename ELFT::Phdr &Phdr, SectionRef SecRef) {
  BinarySection *BinarySection = BC->getSectionForSectionRef(SecRef);
  if (!BinarySection || !BinarySection->isAllocatable())
    return;
  ...
  bool ImageOverlap{false}, VMAOverlap{false};
  bool ImageContains = checkOffsets<ELFT>(Phdr, *Sec, ImageOverlap);
  bool VMAContains = checkVMA<ELFT>(Phdr, *Sec, VMAOverlap);
  ...
  if (!ImageContains || !VMAContains)
    return;
  BinarySection->setRelro();
};
```

非 allocatable section 无法在运行时被 mprotect，直接跳过；两个通道任一出现**部分重叠**即告警放弃——畸形布局不可信。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinarySection` | `IsRelro` 标志 | `--reorder-data` 等数据重排 pass 消费，避开运行时只读化区间 |

### 优化意图

1. **为什么要标记 RELRO**：RELRO 区间在运行时被动态链接器 `mprotect` 为只读。若 BOLT 把可写数据重排进该区间（或反之），程序启动即崩。预先标记让数据布局决策有据可依。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 双通道完整包含才标记 | 文件偏移与 VMA 同时成立 | 只查单通道会把畸形二进制误标 |
| 部分重叠 → 放弃 + WARNING | 不猜测 | 漏标只是次优，误标是崩溃 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| RELRO 标记 | `BinarySection::setRelro` | `bolt/Core/BinarySection.h` |
| section 查找 | `BC->getSectionForSectionRef` | `bolt/Core/BinaryContext.h` |

### 其他补充

被 `readSpecialSections`（L2482）调用。

---

## discoverStorage 函数分析

### 函数签名与目的（L636-809，模板成员）

```cpp
template <typename ELFT>
Error RewriteInstance::discoverStorage(ELFObjectFile<ELFT> *ELFObjFile);
```

**功能**: 规划"新代码放哪"：计算 `NextAvailableAddress/Offset` 游标、新 PHDR 表位置、记录旧 `.text` 信息——整个输出二进制布局的起点。

### 整体结构

```text
discoverStorage(ELFObjFile)
├── 1. e_entry → BC->StartFunctionAddress
├── 2. 扫描 program headers:
│      PT_LOAD → FirstAllocAddress=min / NextAvailableAddress=max(vaddr+memsz)
│                NextAvailableOffset=max(offset+filesz)
│                BC->SegmentMapInfo[vaddr] = SegmentInfo{...}
│                [x86_64 且 vaddr>=KernelStart] IsLinuxKernel = true
│      PT_INTERP → HasInterpHeader
├── 3. 记录旧 .text 地址/大小/文件偏移
├── 4. 检测"输入已被 BOLT 处理" → 拒绝
├── 5. 无 PT_LOAD → 报错
├── 6. FirstNonAllocatableOffset = NextAvailableOffset
├── 7. [--custom-allocation-vma] 与现有 segment 冲突告警
├── 8. 页对齐两个游标；[Hugify] 左侧多留一页
├── 9. 新 PHDR 表位置计算（黑魔法，见逐段注释）
│      → PHDRTableAddress/Offset，预留 (e_phnum+3[+2]) 项，64B 对齐
├── 10. LayoutStartAddress = NextAvailableAddress
└── 11. 校验旧 .text 起址能映射到合法文件偏移
```

### 逐段注释

**1. PT_LOAD 扫描 (L652-680)**

```cpp
case ELF::PT_LOAD:
  BC->FirstAllocAddress = std::min(BC->FirstAllocAddress,
                                   static_cast<uint64_t>(Phdr.p_vaddr));
  NextAvailableAddress = std::max(NextAvailableAddress,
                                  static_cast<uint64_t>(Phdr.p_vaddr) + Phdr.p_memsz);
  NextAvailableOffset = std::max(NextAvailableOffset,
                                 static_cast<uint64_t>(Phdr.p_offset) + Phdr.p_filesz);
  BC->SegmentMapInfo[Phdr.p_vaddr] =
      SegmentInfo{Phdr.p_vaddr, Phdr.p_memsz, Phdr.p_offset, Phdr.p_filesz,
                  Phdr.p_align, (Phdr.p_flags & ELF::PF_X) != 0,
                  (Phdr.p_flags & ELF::PF_W) != 0};
```

`SegmentMapInfo`（vaddr→SegmentInfo 的 map）是后续 `getFileOffsetForAddress` 做 VMA→文件偏移翻译的数据源；两个 max 游标把新分配空间锚定在所有既有段之后。

**2. BOLT 输出检测 (L702-709)**

```cpp
if (!opts::HeatmapMode &&
    !(opts::AggregateOnly && BAT->enabledFor(InputFile)) &&
    (SectionName.starts_with(getOrgSecPrefix()) ||
     SectionName == getBOLTTextSectionName()))
  return createStringError(errc::function_not_supported,
                           "BOLT-ERROR: input file was processed by BOLT. Cannot re-optimize");
```

输入含 `.bolt.org` 前缀 section（BOLT 输出特征）→ 拒绝二次优化；例外：heatmap 模式与"在 BOLT 输出上聚合 profile"（BAT 使能）。

**3. PHDR 表黑魔法 (L751-797)**

```cpp
if (NextAvailableOffset <= NextAvailableAddress - BC->FirstAllocAddress)
  NextAvailableOffset = NextAvailableAddress - BC->FirstAllocAddress;
else
  NextAvailableAddress = NextAvailableOffset + BC->FirstAllocAddress;
assert(NextAvailableOffset ==
           NextAvailableAddress - BC->FirstAllocAddress &&
       "PHDR table address calculation error");
```

不同 loader 定位 PHDR 表的方式不一致：有的假设 `e_phoff` 相对 ELF 头所在段，有的做正规 vaddr 计算。BOLT 强制满足**文件偏移 == 虚拟地址 − FirstAllocAddress**（新段内 offset 与 vaddr 同相位），两种 loader 策略都能找到表；代价是另一侧要 padding 补齐。随后预留 `Phnum+3`（新 text 段、新 RW 段、`.eh_frame_hdr`）个表项，instrument 再 +2（计数器拆独立 RW 段），最后按 64 字节（cache line）对齐。`-use-gnu-stack` 或 Linux kernel 跳过整套逻辑（前者复用 PT_GNU_STACK，规避 strip/objcopy 兼容问题）。

**4. Hugify 左侧预留 (L744-746)**：ASLR 映射地址只 4KB 对齐时，2MB 大页左边界可能切进新 text，多留一页做保险。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SegmentInfo` | Address/Size/FileOffset/FileSize/Alignment/IsExecutable/IsWritable | PHDR 重写的中间表示（`RewriteInstance.h`） |
| `NextAvailableAddress/Offset`（成员） | uint64 游标 | 所有后续新分配空间的推进基准 |

### 优化意图

1. **一次规划全局受益**：PHDR 表位置在此定死后，`patchELFPHDRTable` 只需照写；"同相位"不变式让 `getFileOffsetForAddress` 对新段只需一次减法。
2. 旧 `.text` 信息（地址/大小/偏移）被 `--use-old-text` 与函数原地覆写复用，避免二次解析。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `offset == vaddr - FirstAllocAddress` 不变式 | assert 校验 | 破坏后部分 loader 找不到 PHDR 表 |
| 预留表项数 = 实际新增 PT_LOAD 数 | +3/+2 规则 | 少预留 → `patchELFPHDRTable` 写越界 |
| 输入必须含 PT_LOAD | 否则报 executable_format_error | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 对齐 | `alignTo` | `llvm/Support/Alignment.h` |
| BOLT 特征 section | `getOrgSecPrefix/getBOLTTextSectionName` | `bolt/Rewrite/RewriteInstance.h` |

### 其他补充

`BC->OldTextSectionOffset` 用"section 内容指针 − 文件数据指针"（L698-699）计算，是通用的文件偏移求法，不依赖 section API。

---

## readSpecialSections 函数分析

### 函数签名与目的（L2444-2576）

```cpp
Error RewriteInstance::readSpecialSections();
```

**功能**: 把输入 ELF 全部 section 注册进 BC（形成 `BinarySection` 全集），解析 `.eh_frame` 的 FDE 索引，并确定运行模式（relocation / stripped 与否）。

### 整体结构

```text
readSpecialSections()
├── 1. section 循环:
│      isDebugSection → HasDebugInfo；压缩 debug + UpdateDebugSections → 报错
│      BC->registerSection(Section)
├── 2. markGnuRelroSections()
├── 3. debug info 将被剥离的 WARNING（非 UpdateDebugSections/AggregateOnly）
├── 4. .ltext 检测 → UseLargeCodeModel + updateLSDAEncoding
├── 5. HasTextRelocations(.rela.text/.crel.text) / HasSymbolTable / EHFrameSection
├── 6. BAT section 检测 → HasBATSection + [非 heatmap] BAT->parse
├── 7. PrintSections 打印
├── 8. relocation mode 判定与强约束:
│      -relocs=1 但无 text 重定位 → exit(1)
│      HasRelocations = HasTextRelocations && !BOU_FALSE
│      Linux kernel / heatmap 独占 → 强制关闭
├── 9. IsStripped 判定 + AllowStripped 检查
├── 10. .eh_frame 只解析 FDE 索引（ParseCFIProgram=false）→ CFIRdWrt
├── 11. processSectionMetadata()
└── 12. return readELFDynamic()
```

### 逐段注释

**1. 注册与压缩 debug 检查 (L2453-2479)**

```cpp
if (isDebugSection(SectionName)) {
  HasDebugInfo = true;
  if (opts::UpdateDebugSections && isCompressedDebugSection(Section)) {
    return createStringError(errc::not_supported,
                             Twine("compressed debug section '") + SectionName +
                                 "' detected. --update-debug-sections "
                                 "requires uncompressed debug info");
  }
}
...
BC->registerSection(Section);
```

`--update-debug-sections` 路径需要重写 debug 内容，压缩格式（`SHF_COMPRESSED`）没有实现，尽早拒绝。

**2. 模式判定 (L2522-2553)**

```cpp
if (opts::RelocationMode == cl::boolOrDefault::BOU_TRUE && !HasTextRelocations) {
  BC->errs() << "BOLT-ERROR: relocations against code are missing ...";
  exit(1);
}
BC->HasRelocations = HasTextRelocations && (opts::RelocationMode != cl::boolOrDefault::BOU_FALSE);
if (BC->IsLinuxKernel && BC->HasRelocations) {
  BC->outs() << "BOLT-INFO: disabling relocation mode for Linux kernel\n";
  BC->HasRelocations = false;
}
```

`-relocs` 三态（自动/强制开/强制关）+ 实际有无 `.rela.text` 共同决定；heatmap 独占模式同样强制关闭。

**3. FDE 索引按需解析 (L2555-2570)**

```cpp
auto EHFrame = std::make_unique<DWARFDebugFrame>(
    BC->DwCtx->getArch(), /*IsEH=*/true, EHFrameSection.Address);
if (Error E = EHFrame->parse(EHFrameData, /*ParseCFIProgram=*/false))
  report_error("expected valid eh_frame section", std::move(E));
CFIRdWrt.reset(new CFIReaderWriter(*BC, std::move(EHFrame), std::move(EHFrameData)));
```

此时只建 FDE 地址→范围索引（函数边界信息）；每条 FDE 的 CFI 指令程序**延迟到该函数反汇编时**才由 `CFIReaderWriter::fillCFIInfoFor` 解析，避免一次性物化全二进制 CFI（内存优化）。刻意不用 `DwCtx->getEHFrame()`，防 DWARFContext 把整表缓存到生命周期结束。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `CFIReaderWriter`（成员 CFIRdWrt） | FDE map + 按需 CFI | 函数边界、unwind 信息源（`bolt/Core/Exceptions.h`） |
| `BC->HasRelocations/IsStripped` | bool | 贯穿后续所有决策的模式位 |

### 优化意图

1. **内存**：FDE 索引 vs 全量 CFI 程序，大二进制上差一个数量级；`disassembleFunctions` 末尾还会 `releaseFrameData()` 彻底释放。
2. `-aggregate-only`（perf2bolt）兼容 BAT：输入是 BOLT 处理过的二进制时不拒绝（L702 的例外在这里闭环）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 压缩 debug + UpdateDebugSections | 不支持 | 尽早报错，避免半途崩溃 |
| stripped 且未 --allow-stripped | exit(1) | 符号缺失使函数发现不可行 |
| `p_memsz == p_filesz`（dynamic） | 后续 readELFDynamic 校验 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| section 注册 | `BC->registerSection` | `bolt/Core/BinaryContext.cpp` |
| EH frame 解析 | `DWARFDebugFrame::parse` | `llvm/DebugInfo/DWARF/DWARFDebugFrame.h` |
| BAT 解析 | `BoltAddressTranslation::parse` | `bolt/lib/Profile/BoltAddressTranslation.cpp` |

### 其他补充

`isDebugSection/isCompressedDebugSection` 是本文件底部的两个静态工具（见第十四章）。

---

## readELFDynamic 函数分析

### 函数签名与目的（L6520-6631，模板成员）

```cpp
template <typename ELFT>
Error RewriteInstance::readELFDynamic(ELFObjectFile<ELFT> *File);
```

**功能**: 解析 `PT_DYNAMIC`，把 BOLT 关心的 `DT_*` 条目镜像到成员变量。

### 整体结构

```text
readELFDynamic(File)
├── 1. 定位 PT_DYNAMIC phdr；无 → IsStaticExecutable = true 返回
├── 2. p_memsz != p_filesz → 报错
├── 3. 遍历 dynamic entries，按 d_tag 采集（见下表）
└── 4. 一致性清理: 地址/大小成对校验，RELRSZ 可整除 RELRENT
```

### 逐段注释

**1. 采集表 (L6552-6604)**

| DT 条目 | 存入成员 | 后续消费者 |
|---|---|---|
| `DT_INIT` / `DT_FINI` | `BC->InitAddress` / `FiniAddress` | 插桩钩子（第八章） |
| `DT_INIT_ARRAY(SZ)` / `DT_FINI_ARRAY(SZ)` | `BC->InitArrayAddress/Size` 等 | 同上 |
| `DT_RELA` / `DT_RELASZ` | `DynamicRelocationsAddress/Size` | `processDynamicRelocations` |
| `DT_JMPREL` / `DT_PLTRELSZ` | `PLTRelocationsAddress/Size` | 同上 |
| `DT_RELACOUNT` | `DynamicRelativeRelocationsCount` | `patchELFDynamic` 回写 |
| `DT_RELR/RELRSZ/RELRENT` | `DynamicRelr*` | RELR 读/写 |

**2. 静态 PIE 判定 (L6554-6560)**

```cpp
case ELF::DT_FLAGS_1: {
  auto Flags = Dyn.getVal();
  if (Flags & ELF::DF_1_PIE && !BC->HasInterpHeader) {
    BC->outs() << "BOLT-INFO: static pie executable detected\n";
    BC->IsStaticExecutable = true;
  }
```

`DF_1_PIE` 且无 `PT_INTERP` → 静态 PIE（无动态链接器但仍是 DYN 形态），影响插桩钩子与 GOT 处理。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ELFObjectFile<ELFT>::Elf_Dyn` | d_tag/d_un | ELF 动态段条目原始结构（`llvm/Object/ELF.h`） |

### 优化意图

1. 一次性镜像 DT 条目：后续 5+ 个阶段（动态重定位、PLT、插桩、RELA/RELR 回写、dynamic 补丁）都直接查成员，不重复解析。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 地址/大小必须成对有效 | 单边为 0 → 整组 reset | 半有效状态会让 section 查找失败 |
| RELRSZ % RELRENT == 0 | 否则 exit(1) | 解码会越界 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 动态条目遍历 | `ELFFile::dynamicEntries` | `llvm/Object/ELF.h` |

### 其他补充

无 `PT_DYNAMIC` 时打印 "static input executable detected" 并置 `IsStaticExecutable`。

---

## initializeMetadataManager 函数分析

### 函数签名与目的（L3911-3924）

```cpp
void RewriteInstance::initializeMetadataManager();
```

**功能**: 按固定顺序注册全部元数据重写器（MetadataRewriter）。

### 整体结构

```text
initializeMetadataManager()
├── [IsLinuxKernel] createLinuxKernelRewriter
├── createBuildIDRewriter
├── createPseudoProbeRewriter
├── createRSeqRewriter
├── createSDTRewriter
└── createGNUPropertyRewriter
```

### 逐段注释

简单注册序列，无代码片段。6 个 rewriter 各自来自 `bolt/lib/Rewrite/` 下独立文件，分别处理 Linux kernel 专有 section/符号、build-id、`.pseudo_probe` 探针描述符、rseq 临界区表、`.sdt` DTrace note、`.note.gnu.property`（BTI/PAC 特性位）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MetadataManager`（成员） | rewriter 列表 | 提供 SectionInitializers / PreCFG / PostCFG / PreEmit / AfterEmit 五个时机钩子（`bolt/Rewrite/MetadataManager.h`） |

### 优化意图

1. **阶段化观察者**：元数据重写需要的信息量随流水线推进而增长（section 原始内容 → CFG → 最终输出地址），五个时机让各 rewriter 在恰好拥有所需信息时被调用。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 注册顺序即执行顺序 | 依赖顺序的 rewriter 靠排列保证 | 调整顺序需论证依赖 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 各 rewriter 工厂 | `create*Rewriter` | `bolt/lib/Rewrite/{LinuxKernelRewriter,BuildIDRewriter,PseudoProbeRewriter,RSeqRewriter,SDTRewriter,GNUPropertyRewriter}.cpp` |

### 其他补充

无。

---

## processSectionMetadata 函数分析

### 函数签名与目的（L3926-3932）

```cpp
void RewriteInstance::processSectionMetadata();
```

**功能**: 初始化 metadata manager 并触发其 section 级初始化钩子。

### 整体结构

```text
processSectionMetadata()
├── initializeMetadataManager()
└── MetadataManager.runSectionInitializers()
```

### 逐段注释

两行调度（带计时器），无代码片段。

### 关键数据结构

同 `initializeMetadataManager`。

### 优化意图

1. 单独成函数是为了挂 `NamedRegionTimer`（`processmetadata-section`），让 `-time-rewrite` 能区分各 metadata 阶段耗时。

### 约束与易错点

无（纯调度）。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| section 级钩子 | `MetadataManager::runSectionInitializers` | `bolt/lib/Rewrite/MetadataManager.cpp` |

### 其他补充

被 `readSpecialSections`（L2572）调用。

---

## adjustCommandLineOptions 函数分析

### 函数签名与目的（L2578-2751）

```cpp
void RewriteInstance::adjustCommandLineOptions();
```

**功能**: 在了解二进制形态后，对命令行选项做一致性裁决：非法组合 `exit(1)`，可用但需降级的组合打 WARNING 并改写选项。

### 整体结构

```text
adjustCommandLineOptions()
├── 1. AArch64 非 relocation → WARNING
├── 2. RV32 非静态非 PIE → exit(1)
├── 3. RuntimeLibrary->adjustCommandLineOptions(BC)    // 二次裁决
├── 4. x86 JCC erratum 缓解 + 非 relocation → exit(1)
├── 5. -split-eh + 非 relocation → 降级关闭
├── 6. AArch64 CDSplit + 非 compact-code-model → exit(1)
├── 7. -strict + 非 relocation → 降级关闭
├── 8. relocation + aggregate-only → 自动开 strict
├── 9. 函数重排 / Safe ICF + 非 relocation → exit(1)
├── 10. instrument 或函数重排 → 自动 HotText = true
├── 11. -use-gnu-stack + instrument → exit(1)
├── 12. instrument + entry_point 钩子 + 无 INTERP → 钩子降级为 init
├── 13. HotText → 默认追加 .stub/.mover/.never_hugify
├── 14. -use-old-text 无 .text / 非 relocation → 降级关闭
├── 15. --merge-text-sections 非 relocation / 非 ELF → exit(1)
├── 16. AlignText 默认 PageAlign，不低于 AlignFunctions；
│       对齐参数批量镜像到 BC->*
├── 17. x86/AArch64 自动开启 -lite（非 strict/old-text）
├── 18. -lite 与 -use-old-text / -strict → 降级 / exit(1)
└── 19. Linux kernel → KeepNops / TerminalHLT / TerminalTrap 默认值
```

### 逐段注释

**1. 对齐参数镜像 (L2709-2720)**

```cpp
BC->AlignText = opts::AlignText;
BC->AlignFunctions = opts::AlignFunctions;
BC->AlignBlocks = opts::AlignBlocks;
...
BC->X86AlignBranchBoundaryHotOnly = opts::X86AlignBranchBoundaryHotOnly;
```

把 `opts::*` 批量搬到 `BC`，让 pass 与 emitter 不再直接摸全局 cl::opt（可测试性 + 避免 include 依赖）。

**2. 自动 lite (L2722-2724)**

```cpp
if ((BC->isX86() || BC->isAArch64()) && opts::Lite.getNumOccurrences() == 0 &&
    !opts::StrictMode && !opts::UseOldText)
  opts::Lite = true;
```

x86/AArch64 默认只处理有 profile 的函数（lite），大幅缩短大二进制处理时间；显式 `-strict`/`-use-old-text` 时退回全量。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::*`（cl::opt） | 各选项 | `bolt/Utils/CommandLineOpts.cpp` 全局定义，本函数做"知彼后"的二次赋值 |

### 优化意图

1. **集中裁决**：BOLT 选项空间巨大且与二进制形态强耦合，把所有依赖二进制信息的决策收拢到一个函数，避免 if 散落在几十个 pass 里。
2. 降级优于失败：能继续跑的（如 `-split-eh` 非 reloc）打 WARNING 关掉，最大化可用性。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 依赖移动函数的特性必须 relocation mode | 重排/ICF-safe/old-text | 非 reloc 下 exit(1) |
| instrument 下不得有 W+X 段且用 gnu-stack PHDR | 安全策略矛盾 | exit(1) |
| Linux kernel trap 可恢复 | KeepNops/TerminalHLT 默认反转 | 忽略会生成错误语义 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 运行库选项裁决 | `RuntimeLibrary::adjustCommandLineOptions` | `bolt/RuntimeLibs/RuntimeLibs.h` |

### 其他补充

本函数在 `discoverFileObjects` 之前调用（`run()` L826），即裁决发生在符号扫描前——后续函数发现会受被改写选项（如 HotText 影响 `__hot_start` 处理）影响。

---

# 四、符号与函数发现（discoverFileObjects 族）

## discoverFileObjects 函数分析

### 函数签名与目的（L895-1492）

```cpp
void RewriteInstance::discoverFileObjects();
```

**功能**: 把输入 ELF 符号表转成 BOLT 世界模型——`BinaryFunction`（代码符号）与 `BinaryData`（数据符号），处理名字歧义、重复符号、FDE 冲突、AArch64/RISC-V mapping symbol、冷片段识别；随后触发动态重定位读取、PLT 反汇编、静态重定位读取。全文件最复杂函数之一（约 600 行）。

### 整体结构

```text
discoverFileObjects()
├── 1. FILE 符号收集 + asan/coverage 拒绝
├── 2. 符号过滤与排序（isSymbolInMemory / checkSymbolInSection / CompareSymbols）
├── 3. [AArch64/RISCV] addExtraDataMarkerPerSymbol + marker 分区
├── 4. 主循环：逐符号建 BinaryFunction / BinaryData
│    ├── 名字消歧（全局查重 / NR.uniquify / PG 前缀 / ANONYMOUS.N）
│    ├── 绝对符号、BOLT 保留符号、section 末尾符号 → 只注册名字
│    ├── 非代码 section → 只注册名字
│    ├── 函数内符号 → 局部 label 或二级入口
│    ├── FDE 尺寸交叉校验
│    └── createBinaryFunction / addAlternativeName / fragment 识别
├── 5. processDynamicRelocations() → disassemblePLT() → FDE 兜底
├── 6. adjustFunctionBoundaries() → splitUnmarkedTailFunctions()
├── 7. marker 标注（markCode/markData/常量岛表）
├── 8. [AArch64] 岛内动态重定位 + veneer 补标
├── 9. processRelocations() → registerFragments() → discoverBOLTReserved()
└── 10. FileSymbols/FileSymRefs/NR 清理
```

### 逐段注释

**1. FILE 符号与黑名单 (L899-921)**

```cpp
if (NameOrError && NameOrError->starts_with("__asan_init")) {
  BC->errs() << "BOLT-ERROR: input file was compiled or linked with sanitizer "
                "support. Cannot optimize.\n";
  exit(1);
}
if (cantFail(Symbol.getType()) == SymbolRef::ST_File)
  FileSymbols.emplace_back(Symbol);
```

asan/coverage 二进制的运行时数据结构与 BOLT 假设冲突，直接拒绝；FILE 符号（ST_File）收集起来供本地符号消歧（`<function>/<file>/<id>` 命名）。

**2. 符号排序 (L925-998)**

```cpp
auto CompareSymbols = [this](const SymbolInfo &A, const SymbolInfo &B) {
  if (A.Address != B.Address)
    return A.Address < B.Address;
  const bool AMarker = BC->isMarker(A.Symbol);
  const bool BMarker = BC->isMarker(B.Symbol);
  if (AMarker || BMarker)
    return AMarker && !BMarker;
  const auto AType = cantFail(A.Symbol.getType());
  const auto BType = cantFail(B.Symbol.getType());
  if (AType == SymbolRef::ST_Function && BType != SymbolRef::ST_Function)
    return true;
  if (BType == SymbolRef::ST_Debug && AType != SymbolRef::ST_Debug)
    return true;
  return false;
};
llvm::stable_sort(SortedSymbols, CompareSymbols);
```

地址升序 + 同地址时 marker 押后、`ST_Function` 优先。`checkSymbolInSection` 过滤"地址不在自己 section 内"的异常符号（AArch64 `$d/$t` marker 错位问题，L943-946 注释）。**性能**：`getAddress()` 记忆化进 `SymbolInfo`（"it has rather high overhead"）。

**3. 数据标记扩充 (L1010-1057)**

```cpp
auto addExtraDataMarkerPerSymbol = [&]() {
  bool IsData = false;
  uint64_t LastAddr = 0;
  for (const auto &SymInfo : SortedSymbols) {
    MarkerSymType MarkerType = BC->getMarkerType(SymInfo.Symbol);
    ...
    if (MarkerType != MarkerSymType::NONE) {
      MarkerSymbols[SymInfo.Address] = MarkerType;
      LastAddr = SymInfo.Address;
      IsData = MarkerType == MarkerSymType::DATA;
      continue;
    }
    if (IsData)
      MarkerSymbols[SymInfo.Address] = MarkerSymType::DATA;
  }
};
```

AArch64 ABI（IHI0056B）的 `$d/$x` mapping symbol 标记 code section 里的内嵌数据；编译器常把多个数据对象合并进一个 `$d..$x` 区间，但反汇编器需要**每个**数据对象前都有 `$d`。该 lambda 扫描排序符号流，"处于数据状态"期间的每个符号地址都补 DATA 标记；函数符号强制视为 CODE（并告警 "lacks code marker"）。随后 `stable_partition` 把 marker 符号挪出主列表（`LastSymbol` 指向最后一个非 marker）。

**4. 名字唯一化 (L1101-1163)**

```cpp
std::string Name = SymName.starts_with(BC->AsmInfo->getInternalSymbolPrefix())
                       ? "PG" + std::string(SymName) : std::string(SymName);
...
if (Name.empty()) {
  UniqueName = "ANONYMOUS." + std::to_string(AnonymousId++);
} else if (SymbolFlags & SymbolRef::SF_Global) {
  if (const BinaryData *BD = BC->getBinaryDataByName(Name)) {
    if (BD->getSize() == ELFSymbolRef(Symbol).getSize() &&
        BD->getAddress() == SymbolAddress)
      continue;              // 重复全局（可能是链接器 bug），忽略
    BC->errs() << "BOLT-ERROR: bad input binary, global symbol \"" << Name
               << "\" is not unique\n";
    exit(1);
  }
  UniqueName = Name;
} else {
  auto SFI = llvm::upper_bound(FileSymbols, ELFSymbolRef(Symbol));
  if (SymbolType == SymbolRef::ST_Function && SFI != FileSymbols.begin()) {
    StringRef FileSymbolName = cantFail(SFI[-1].getName());
    if (!FileSymbolName.empty())
      AlternativeName = NR.uniquify(Name + "/" + FileSymbolName.str());
  }
  UniqueName = NR.uniquify(Name);
}
```

`.L` 前缀（internal symbol）的"全局化 local"加 `PG` 恢复全局作用域；本地符号双命名（`<function>/<id>` 主名 + `<function>/<file>/<id2>` 别名）的原因：perf profile 可能采自剥过 FILE 符号的二进制，两种形态都要能匹配。`NR`（NameResolver）提供对称的 `uniquify/restore`。

**5. 二级入口判定 (L1235-1263)**

```cpp
if (PreviousFunction && PreviousFunction->containsAddress(SymbolAddress) &&
    PreviousFunction->getAddress() != SymbolAddress) {
  if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
    ... // 合法局部符号 → 只注册名字
  } else {
    registerName(0);
    PreviousFunction->addEntryPointAtOffset(SymbolAddress - PreviousFunction->getAddress());
    auto SI = llvm::find_if(llvm::make_range(FileSymRefs.equal_range(SymbolAddress)), ...);
    FileSymRefs.erase(SI);
  }
}
```

函数体中间的符号：`isSymbolValidInScope` 判定为局部标签则忽略；否则登记为**另一个入口**（汇编多入口、lambda invoke 等），并从 `FileSymRefs` 移除防后续重复处理。

**6. FDE 交叉校验 (L1266-1304)**：符号地址落进前一个 FDE 内部 → 冲突 `IsSimple=false`；起址相同但大小不同 → 取 max（宁大勿小，反汇编出垃圾再降级）。

**7. fragment 识别 (L1350-1363)**

```cpp
if (FunctionFragmentTemplate.match(SymName)) {
  ...
  BC->HasSplitFunctions = true;
  BF->IsFragment = true;
}
```

输入自带 `.cold` 后缀函数（上游 FDR 分裂 / 前次 BOLT 输出）→ 标记 `HasSplitFunctions`，供 `registerFragments` 挂接；relocation mode 下支持受限（告警）。

**8. AArch64 收尾 (L1445-1476)**：常量岛内的动态重定位 → `markIslandDynamicRelocationAtAddress`（岛内重定位必须原位保留）；`__AArch64AbsLong_thunk_` veneer（16 字节、偏移 8 处缺 `$d`）人工补数据标记，助反汇编器跳过 veneer 的数据半区。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `FileSymRefs`（成员） | multimap<地址, SymbolRef> | 二级入口发现与 fragment 消歧的查询源（用完即清） |
| `NR`（成员 NameResolver） | uniquify/restore | 本地符号名唯一化与还原 |
| `MarkerSymbols`（局部） | DenseMap<地址, MarkerSymType> | AArch64/RISCV code/data 标记 |
| `BC->AddressToConstantIslandMap` | 地址→函数 | 常量岛归属 |

### 优化意图

1. **排序 + 状态机**：符号排序后"前一函数"判断、数据区间追踪都变成单遍扫描，避免 O(n²) 查询。
2. **双命名策略**：一次注册同时兼容 stripped/非 stripped 两种 profile 来源，免去事后按 profile 重建别名的复杂度。
3. 发现期专用数据（FileSymRefs/FileSymbols/NR）用完即 `clear()`，峰值内存受控。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 全局符号必须唯一 | 同名不同地址 exit(1) | 静默接受会导致重定位错绑 |
| FDE 与 symtab 大小冲突取 max | 两者都可能是错的 | 偏小会被截断指令 |
| registerFragments 需要 FileSymRefs | 故清理必须在其后 | 顺序错误 → 悬空引用 |
| 动态重定位先于静态读取 | processDynamicRelocations 在 processRelocations 前 | 静态处理依赖"地址已被动态占据"信息 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 建函数 | `BC->createBinaryFunction` | `bolt/Core/BinaryContext.cpp` |
| 名字注册 | `BC->registerNameAtAddress` | 同上 |
| marker 判定 | `BC->isMarker/getMarkerType` | `bolt/Core/BinaryContext.h` |
| 入口登记 | `BinaryFunction::addEntryPointAtOffset` | `bolt/Core/BinaryFunction.h` |

### 其他补充

主循环只处理非 marker 符号（`LastSymbol` 之前）；AArch64 的 veneer/常量岛处理是 BOLT 对"链接器产物非规范布局"的典型防御。

---

## createRISCVIFuncResolverFunctions 函数分析

### 函数签名与目的（L534-553，静态）

```cpp
static void createRISCVIFuncResolverFunctions(BinaryContext &BC);
```

**功能**: 为 RISC-V IFUNC 解析器补建 `BinaryFunction`。

### 整体结构

```text
createRISCVIFuncResolverFunctions(BC)
└── 扫全部可分配 section 的动态重定位:
     isIRelative && Addend && 该地址无函数
       → 在 Addend 所在 section 建 "__BOLT_IFUNC_RESOLVERat<hex>" 函数
```

### 逐段注释

简单扫描，无代码片段。背景：LLD 会把唯一一个 RISC-V IFUNC 符号规范到 `.iplt` 入口，解析器本体只能通过 `R_RISCV_IRELATIVE` 的 addend 找到；不提前注册，后续 `disassemblePLT` 无法把 `.iplt` entry 关联到解析器函数。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Relocation` | Type/Addend | `bolt/Core/Relocation.h`，`isIRelative()` 判型 |

### 优化意图

1. 在 PLT 反汇编**之前**注册（`discoverFileObjects` L1381-1382），保证 `.iplt` 处理走常规路径。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `assert(BC.isRISCV())` | 仅 RISC-V 调用 | 误用即 assert |
| Addend 必须有对应 section | `getSectionForAddress` assert 失败 | 畸形输入崩溃（可接受） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 重定位遍历 | `BinarySection::dynamicRelocations` | `bolt/Core/BinarySection.h` |

### 其他补充

无。

---

## discoverBOLTReserved 函数分析

### 函数签名与目的（L1494-1519）

```cpp
void RewriteInstance::discoverBOLTReserved();
```

**功能**: 识别 `__bolt_reserved_start/__bolt_reserved_end` 符号对，启用"BOLT 预留空间"模式（新代码放进链接期预留区，不扩张文件布局）。

### 整体结构

```text
discoverBOLTReserved()
├── 1. 两个符号只出现一个 → exit(1)
├── 2. 都不存在 → 返回（常规模式）
├── 3. start >= end → exit(1)
├── 4. BC->BOLTReserved = AddressRange(start, end)
└── 5. 清零 PHDRTable*/NewTextSegment* 并重置 NextAvailableAddress = 预留区起点
```

### 逐段注释

**1. 布局重置 (L1514-1518)**

```cpp
PHDRTableOffset = 0;
PHDRTableAddress = 0;
NewTextSegmentAddress = 0;
NewTextSegmentOffset = 0;
NextAvailableAddress = BC->BOLTReserved.start();
```

预留空间优先于 `discoverStorage` 的全部规划：PHDR 表原地不动，新代码往预留区放。`getFileOffsetForAddress` 里 `NewTextSegmentAddress==0` 时自动落入旧 segment 查询路径，与该重置配套。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->BOLTReserved` | AddressRange | 预留区边界，`mapFileSections` 末尾做容量校验 |

### 优化意图

1. 链接期 `-Wl,--section-start,.bolt.reserve=...` 预留可让 BOLT 输出不改变文件大小布局——对部署管道（如嵌入系统）友好。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 符号必须成对且 start < end | 否则 exit(1) | 半预留状态无法安全分配 |
| 预留区容量在 mapFileSections 校验 | 超容 exit(1) | 静默溢出会覆盖别的段 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 符号查询 | `BC->getBinaryDataByName` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

# 五、重定位处理

## processDynamicRelocations 函数分析

### 函数签名与目的（L2950-3008）

```cpp
void RewriteInstance::processDynamicRelocations();
```

**功能**: 动态重定位读取总调度：按 DT 条目分派到 RELR / JMPREL / RELA 三条路径。

### 整体结构

```text
processDynamicRelocations()
├── 1. [DynamicRelrSize>0] 校验 section 尺寸 → readDynamicRelrRelocations()
├── 2. [PLTRelocationsSize>0] 校验 → readDynamicRelocations(IsJmpRel=true)
├── 3. 静态可执行兜底: 无 DT_RELA 但有 .rela.dyn section
│      → 就地取地址/大小/RELATIVE 计数
└── 4. [DynamicRelocationsSize>0] RISC-V 尺寸特判 → readDynamicRelocations(false)
```

### 逐段注释

**1. RISC-V 尺寸特判 (L2999-3001)**

```cpp
if (DynamicRelocationsSize == DynamicRelSectionSize + PLTRelocationsSize)
  DynamicRelocationsSize = DynamicRelSectionSize;
```

RISC-V 的 `DT_RELASZ` 可能同时覆盖 `.rela.dyn + .rela.plt`，两者之和相等时裁剪，避免 RELA 路径重复处理 JMPREL 条目。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `DynamicRelr*/PLTRelocations*/DynamicRelocations*`（成员） | DT 镜像 | `readELFDynamic` 采集（第三章） |

### 优化意图

1. 静态可执行兜底：静态二进制无 `PT_DYNAMIC` 但可能有 `.rela.dyn`（如静态 PIE 的 IRELATIVE），单独扫 section 名取参数。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| section 尺寸必须与 DT_*SZ 一致 | 否则 `report_error`（fatal） | 不一致说明输入被破坏 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| fatal 错误 | `report_error` | `bolt/Utils/Utils.h` |

### 其他补充

在**静态重定位之前**读取（`discoverFileObjects` L1375）——静态路径用"该地址是否已有动态重定位"跳过重复处理。

---

## readDynamicRelocations 函数分析

### 函数签名与目的（L3029-3088）

```cpp
void RewriteInstance::readDynamicRelocations(const SectionRef &Section,
                                              bool IsJmpRel);
```

**功能**: 读取常规格式动态重定位（`.rela.dyn` / `.rela.plt`）。

### 整体结构

```text
readDynamicRelocations(Section, IsJmpRel)
└── 逐条 relocation:
     ├── R_NONE 跳过
     ├── 符号 → BinaryData 命中用其 MCSymbol，否则建 undefined global
     ├── [IsJmpRel] IsJmpRelocation[RType] = true
     ├── SymbolIndex[Symbol] = getRelocationSymbol(...)
     ├── [R_*_RELATIVE] handleRelativeDynamicRelocation()
     └── BC->addDynamicRelocation(Offset, Symbol, RType, Addend)
```

### 逐段注释

**1. 符号绑定 (L3049-3057)**

```cpp
if (SymbolIter != InputFile->symbol_end()) {
  SymbolName = cantFail(SymbolIter->getName());
  BinaryData *BD = BC->getBinaryDataByName(SymbolName);
  Symbol = BD ? BD->getSymbol()
              : BC->getOrCreateUndefinedGlobalSymbol(SymbolName);
  ...
}
```

已注册的 BinaryData 复用其 MCSymbol（保持符号同一性）；未定义符号创建占位全局——重定位目标在别的 DSO 里，BOLT 只需名字对应。

**2. RELATIVE 特判 (L3077-3084)**：先 `handleRelativeDynamicRelocation`（函数内部引用登记），再入库。注意校验"RELATIVE 的符号地址必须为 0"（L3078-3082，非零 exit(1)）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `IsJmpRelocation`（成员） | DenseMap<uint32,bool> | 重定位类型→是否属于 JMPREL，`patchELFAllocatableRelaSections` 据此分流回写 |
| `SymbolIndex`（成员） | MCSymbol→dynsym index | 回写时重建 `setSymbolAndType` |

### 优化意图

1. 符号复用避免同名 `MCSymbol` 分裂——后续 `getNewValueForSymbol` 按名字查 JITLink 结果，分裂会导致查不到。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| assert allocatable | 动态重定位必在可分配 section | 误传 note section 会 assert |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 动态重定位入库 | `BC->addDynamicRelocation` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

## readDynamicRelrRelocations 函数分析

### 函数签名与目的（L3090-3142）

```cpp
void RewriteInstance::readDynamicRelrRelocations(BinarySection &Section);
```

**功能**: 解码 RELR 压缩格式的相对重定位（`.relr.dyn`）。

### 整体结构

```text
readDynamicRelrRelocations(Section)
├── 1. 准备: RType = getRelative(), PSize, MaxDelta
├── 2. lambda ExtractAddendValue(Address): 从目标地址内存取 addend
├── 3. lambda AddRelocation(Address):
│      Addend = ExtractAddendValue(Address)
│      handleRelativeDynamicRelocation(Address, Addend)
│      BC->addDynamicRelocation(Address, nullptr, RType, Addend, IsRELR=true)
└── 4. 解码循环（地址 entry / 位图 entry 交替）
```

### 逐段注释

**1. RELR 解码循环 (L3121-3141)**

```cpp
uint64_t Offset = 0, Address = 0;
uint64_t RelrCount = DynamicRelrSize / DynamicRelrEntrySize;
while (RelrCount--) {
  assert(DE.isValidOffset(Offset));
  uint64_t Entry = DE.getUnsigned(&Offset, DynamicRelrEntrySize);
  if ((Entry & 1) == 0) {
    AddRelocation(Entry);
    Address = Entry + PSize;
  } else {
    const uint64_t StartAddress = Address;
    while (Entry >>= 1) {
      if (Entry & 1)
        AddRelocation(Address);
      Address += PSize;
    }
    Address = StartAddress + MaxDelta;
  }
}
```

RELR 两级编码压缩连续 RELATIVE：偶数 entry 是**新基准地址**（每地址一条）；奇数 entry（最低位 1）是**位图**——第 i 位为 1 表示 `Base + i*PSize` 处有一条。位图用尽 `MaxDelta = (位数-1)*PSize` 后换新基准。addend 从**目标地址处的内存**抽取（RELATIVE 的编码值即 addend）。编码的逆操作在 `patchELFAllocatableRelrSection`（第十三章）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Relocation::IsRELR` | bool 标志 | 区分常规 RELATIVE 与 RELR 来源，回写路径不同 |
| `DataExtractor` | 字节流读取 | `llvm/Support/DataExtractor.h` |

### 优化意图

1. RELR 是省空间设计（大二进制上万条 RELATIVE 压缩成位图）；BOLT 必须完整支持才能处理现代发行版二进制。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 地址 entry 必须偶数、位图 entry 最低位 1 | 协议约定 | 位错乱 → 全表解码错误 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 按地址取内存值 | `BC->getSectionForAddress` + `DataExtractor` | — |

### 其他补充

无。

---

## handleRelativeDynamicRelocation 函数分析

### 函数签名与目的（L3144-3163）

```cpp
void RewriteInstance::handleRelativeDynamicRelocation(uint64_t RelOffset,
                                                        uint64_t ReferencedAddress);
```

**功能**: 处理"RELATIVE 动态重定位指向**函数内部**"的情形（indirect goto 跳转表标签、局部 static 指针等）。

### 整体结构

```text
handleRelativeDynamicRelocation(RelOffset, ReferencedAddress)
├── 1. 无包含函数 → 返回
├── 2. 指向常量岛 → exit(1)
└── 3. 偏移非 0 → Func->registerInternalRefDataRelocation(RefOffset, RelOffset)
```

### 逐段注释

无复杂分段。第 2 步的 exit：常量岛（code section 内嵌数据）被外部数据槽引用时，BOLT 无法在移动函数后保持该引用正确，宁可失败。第 3 步断言保证重定位槽本身不在代码内（`Relative relocation to code only from data`）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::Relocations` 之外的内引表 | (函数内偏移 → 数据槽地址) | `registerInternalRefDataRelocation` 登记，函数移动后由 `getNewFunctionOrDataAddress` 消费翻译 |

### 优化意图

1. **提前登记而非事后扫描**：在重定位读取时就把"函数内偏移被谁引用"记全，翻译阶段 O(1) 查表。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 岛内引用直接 exit | 无法保证正确性 | 大多数正常二进制不会命中 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 岛判定 | `BinaryFunction::isInConstantIsland` | `bolt/Core/BinaryFunction.h` |

### 其他补充

被 `readDynamicRelocations` 与 `readDynamicRelrRelocations` 共同调用。

---

## processRelocations 函数分析

### 函数签名与目的（L3010-3027）

```cpp
void RewriteInstance::processRelocations();
```

**功能**: 静态重定位读取调度（仅 relocation mode；Linux kernel 除外）。

### 整体结构

```text
processRelocations()
├── 1. !BC->HasRelocations → 返回
├── 2. 遍历输入 section: 有目标(非 end) 且目标可分配 → readRelocations(Section)
└── 3. NumFailedRelocations 告警汇总
```

### 逐段注释

无复杂分段。注意筛选条件写在调度层：只处理"重定向到**可分配** section"的重定位 section——非分配目标的 `-emit-relocs` 残渣直接跳过。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `NumFailedRelocations`（成员） | 计数 | `analyzeRelocation` 失败累计，此处统一告警 |

### 优化意图

1. 非 relocation 模式完全跳过——静态重定位信息只在函数可移动时才有用。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| Linux kernel 不调用 | `discoverFileObjects` L1478 包裹 | kernel 布局约束不同 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 目标 section 查询 | `Section::getRelocatedSection` | `llvm/Object/ObjectFile.h` |

### 其他补充

无。

---

## readRelocations 函数分析

### 函数签名与目的（L3189-3225）

```cpp
void RewriteInstance::readRelocations(const SectionRef &Section);
```

**功能**: 单个重定位 section 的过滤层 + 逐条分发。

### 整体结构

```text
readRelocations(Section)
├── 1. 自身 allocatable → 忽略（运行时重定位，另有路径）
├── 2. 目标 section 非 allocatable → 忽略
├── 3. 目标 ∈ {.plt, .rela.plt, .got.plt, .eh_frame, .gcc_except_table} → 忽略
└── 4. 逐条 handleRelocation(RelocatedSection, Rel)
```

### 逐段注释

无复杂分段。三级过滤逐级裁剪后，剩下的才是"需要 BOLT 理解并重写的代码/数据引用"。`.eh_frame` 有 `relocateEHFrameSection` 专用路径；`.gcc_except_table` 由发射器重建。

### 关键数据结构

无本地结构。

### 优化意图

1. 白名单式忽略清单集中在 `StringSwitch`（L3212-3216），新增 section 只改一处。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| assert 有目标 section | 重定位 section 必有目标 | 畸形输入 assert |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 字符串多路 | `llvm::StringSwitch` | `llvm/ADT/StringSwitch.h` |

### 其他补充

无。

---

## getRelocationAddend 函数分析

### 函数签名与目的（L2754-2787，匿名命名空间）

```cpp
template <typename ELFT>
int64_t getRelocationAddend(const ELFObjectFile<ELFT> *Obj,
                            const RelocationRef &RelRef);
int64_t getRelocationAddend(const ELFObjectFileBase *Obj,
                            const RelocationRef &Rel);
```

**功能**: 绕过 `RelocationRef` 抽象，直接从 ELF 原始结构读 addend。

### 整体结构

```text
getRelocationAddend(Obj, RelRef)
├── 取 relocation 所在 section 的 sh_type
├── SHT_REL  → 0（无 addend）
├── SHT_RELA → Obj->getRela(Rel).r_addend
├── SHT_CREL → Obj->getCrel(Rel).r_addend
└── 其他 → llvm_unreachable
```

### 逐段注释

无复杂分段。`RelocationRef` 接口不暴露 addend（三种 section 布局语义不同），BOLT 重定位模型需要**精确 addend** 做符号化，故直读原始结构。非模板重载做 ELF32LE/ELF64LE 分发。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ELFObjectFile<ELFT>::Elf_Rela` | r_offset/r_info/r_addend | `llvm/Object/ELF.h` |

### 优化意图

1. `SHT_CREL`（压缩重定位，新格式）分支使 BOLT 与 lld 新输出兼容。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅小端 ELF32/64 | 构造期已保证 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 原始结构访问 | `ELFObjectFile::getRela/getCrel` | `llvm/Object/ELFObjectFile.h` |

### 其他补充

被 `analyzeRelocation` 与 `readDynamicRelocations` 调用。

---

## getRelocationSymbol 函数分析

### 函数签名与目的（L2789-2816，匿名命名空间）

```cpp
template <typename ELFT>
uint32_t getRelocationSymbol(const ELFObjectFile<ELFT> *Obj,
                             const RelocationRef &RelRef);
int64_t / uint32_t getRelocationSymbol(const ELFObjectFileBase *Obj, ...);
```

**功能**: 读重定位的符号表索引（r_info 的 symbol 域）。

### 整体结构

与 `getRelocationAddend` 同构：按 `SHT_REL/SHT_RELA` 分派 `getRel()->getSymbol(...)` / `getRela()->getSymbol(...)`；**无** CREL 分支（`default: llvm_unreachable`）——dynsym 重定位 section 不会用 CREL。

### 逐段注释

无复杂分段。

### 关键数据结构

同 `getRelocationAddend`。

### 优化意图

1. 与 addend 配对提供"原始重定位三元组"（symbol index + addend + 编码值），供 `SymbolIndex` 回写与 dynsym 原位修补。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| CREL 不支持 | unreachable | 若未来 dynsym 用 CREL 需补分支 |

### 关键 API / 源码路径

同 `getRelocationAddend`。

### 其他补充

被 `readDynamicRelocations` 调用（填 `SymbolIndex`）。

---

## analyzeRelocation 函数分析

### 函数签名与目的（L2819-2948）

```cpp
bool RewriteInstance::analyzeRelocation(
    const RelocationRef &Rel, uint32_t &RType, std::string &SymbolName,
    bool &IsSectionRelocation, uint64_t &SymbolAddress, int64_t &Addend,
    uint64_t &ExtractedValue) const;
```

**功能**: 把一条静态重定位解析成 BOLT 语义（符号名/地址、addend、编码值、是否 section 重定位），并做可信度验证。

### 整体结构

```text
analyzeRelocation(Rel, ...)
├── 1. isSupported 检查 → false
├── 2. 从二进制提取编码值 + 读 addend + 计算 PCRelOffset
├── 3. 符号路径:
│      无符号 → 合成 RELSYMat<addr>
│      有符号 → 名字/地址；ST_Other → SkipVerification；ST_Debug → IsSectionRelocation
│               [AArch64/RISCV 未定义符号] 查 @PLT BinaryData
├── 4. SkipVerification 累积（PIE 8 字节 / GOT / TLS）
├── 5. section 重定位转换（非 AArch64）: 名字改 "section X"，addend 折算
├── 6. 无符号地址兜底（truncateToSize / exceptions_pic 特例）
└── 7. verifyExtractedValue lambda → assert 校验
```

### 逐段注释

**1. 编码值提取 (L2837-2847)**

```cpp
const size_t RelSize = Relocation::getSizeForType(RType);
ErrorOr<uint64_t Value = BC->getUnsignedValueAtAddress(Rel.getOffset(), RelSize);
assert(Value && "failed to extract relocated value");
ExtractedValue = Relocation::extractValue(RType, *Value, Rel.getOffset());
Addend = getRelocationAddend(InputFile, Rel);
const bool IsPCRelative = Relocation::isPCRelative(RType);
const uint64_t PCRelOffset = IsPCRelative && !IsAArch64 ? Rel.getOffset() : 0;
```

`ExtractedValue` 是链接后**实际编码**的数值（按重定位宽度抽取）。`PCRelOffset` 的架构差异：x86 的 `R_X86_64_PC32` 基点是重定位偏移**后 4 字节**，AArch64 的 PC 相对寻址以指令自身（`Rel.getOffset()` 即指令地址）为基点。

**2. PLT 解析优先 (L2863-2874)**

```cpp
const bool IsRISCVIFuncPLT =
    IsRISCV && RType == ELF::R_RISCV_CALL_PLT &&
    ELFSymbolRef(Symbol).getELFType() == ELF::STT_GNU_IFUNC;
if ((!SymbolAddress || IsRISCVIFuncPLT) && !IsWeakReference(Symbol) &&
    (IsAArch64 || IsRISCV)) {
  const BinaryData *BD = BC->getPLTBinaryDataByName(SymbolName);
  SymbolAddress = BD ? BD->getAddress() : 0;
}
```

未定义/弱引用符号在 AArch64/RISC-V 上优先解释为 PLT 入口（`disassemblePLT` 已注册 `name@PLT`）。RISC-V 特判：LLD 会把 IFUNC 符号值直接写成 `.iplt` 地址，`R_RISCV_CALL_PLT` 必须走 `@PLT` 查询。

**3. 验证 (L2927-2945)**

```cpp
auto verifyExtractedValue = [&]() {
  if (SkipVerification) return true;
  if (IsAArch64 || IsRISCV) return true;
  if (SymbolName == "__hot_start" || SymbolName == "__hot_end") return true;
  if (RType == ELF::R_X86_64_PLT32) return true;
  return truncateToSize(ExtractedValue, RelSize) ==
         truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize);
};
(void)verifyExtractedValue;
assert(verifyExtractedValue() && "mismatched extracted relocation value");
```

核心不变式：**链接后编码值 == SymbolAddress + Addend − PCRelOffset**。违反即输入异常。`SkipVerification` 场景：PIE/动态库的 8 字节重定位（结果挪进动态重定位 addend）、GOT/TLS（指令可能被链接器改写）、AArch64/RISCV（推断型地址）、`__hot_start`（BOLT 自己维护）、`R_X86_64_PLT32`（PLT 桩语义）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Relocation`（bolt） | 静态 Arch/isSupported/extractValue | `bolt/Core/Relocation.h`，架构相关的类型判定集合 |

### 优化意图

1. **验证即文档**：assert 里的不变式就是"链接器如何计算编码值"的可执行规格，后续修改者能立即发现语义破坏。
2. `IsWeakReference` lambda 只查 flags 两次，无开销。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| assert 仅 debug 生效 | release 下静默通过 | `(void)` 抑制未使用告警是刻意的 |
| PCRelOffset 架构差异 | x86 加、AArch64 不加 | 混淆会验证全挂 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 值抽取 | `Relocation::extractValue` | `bolt/Core/Relocation.cpp` |
| 类型支持判定 | `Relocation::isSupported/getSizeForType` | 同上 |

### 其他补充

"weird case"（L2911-2923）：编码值 0 但 addend 非零且非 PC 相对——`exceptions_pic.test` 中出现过，直接返回 true 交给上层跳过。

---

## printRelocationInfo 函数分析

### 函数签名与目的（L3165-3187）

```cpp
void RewriteInstance::printRelocationInfo(const RelocationRef &Rel,
                                           StringRef SymbolName,
                                           uint64_t SymbolAddress,
                                           uint64_t Addend,
                                           uint64_t ExtractedValue) const;
```

**功能**: `LLVM_DEBUG` 专用：单行打印一条重定位的全部字段（offset/type/value/symbol/addend/所属函数），供 `-debug-only=bolt` 排查。

### 整体结构

```text
printRelocationInfo(...)
├── Rel.getTypeName / 所属 section / 所属函数查找
└── dbgs() << formatv(...) 单行输出
```

### 逐段注释

无复杂分段。注意 `getBinaryFunctionContainingAddress(Offset, false, BC->isAArch64())`——AArch64 传 `UseMaxSize=true`（尾部 padding 也算函数内）。

### 关键数据结构

无。

### 优化意图

1. 重定位问题是 BOLT 排障最高频场景，一行全字段的 debug 打印比断点逐层展开高效得多。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅 LLVM_DEBUG 内调用 | release 零开销 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 格式化 | `llvm::formatv` | `llvm/Support/FormatVariadic.h` |

### 其他补充

无。

---

## handleRelocation 函数分析

### 函数签名与目的（L3227-3636）

```cpp
void RewriteInstance::handleRelocation(const SectionRef &RelocatedSection,
                                        const RelocationRef &Rel);
```

**功能**: 对一条静态重定位做完整分类决策，落到四种归宿：`ContainingBF->addRelocation`（代码内）/ `BC->addRelocation`（数据对代码或强制）/ `BC->addPCRelativeDataRelocation`（PIC 跳转表占位）/ 丢弃。本文件重定位语义核心（约 400 行）。

### 整体结构

```text
handleRelocation(RelocatedSection, Rel)
├── 1. 早期清洗: skipRelocationType / x86 converted bit / TLS 规则 / 动态重叠
├── 2. analyzeRelocation() 失败 → NumFailedRelocations++
├── 3. vtable PC 相对特判 (_ZTV/_ZTCN)
├── 4. ContainingBF 确定（IsFromCode 时）
├── 5. ReferencedSymbol / ReferencedSection / RISCV ABS 符号
├── 6. x86 PC 相对三分支 → return
├── 7. ReferencedBF 查找与引用校验
├── 8. devirtualization bug 规避（Address+1 探测）
├── 9. 引用归一化:
│      ForceRelocation && !ReferencedBF → 建符号
│      ReferencedBF → 入口/岛/local label 细化，偏移折进 addend
│      AArch64 → 合成 "SYMBOLat<hex>"
│      BinaryData 命中 → 基址化
│      匿名本地 → 注册新符号
├── 10. ForceRelocation 升级（reorder-data / force / RISCV）
└── 11. 最终分发: IsFromCode → BF; IsToCode||Force → BC; else 忽略
```

### 逐段注释

**1. 早期清洗 (L3237-3264)**

```cpp
uint32_t RType = Relocation::getType(Rel);
if (Relocation::skipRelocationType(RType))
  return;
if (IsX86 && (RType & ELF::R_X86_64_converted_reloc_bit)) {
  ...
  RType &= ~ELF::R_X86_64_converted_reloc_bit;
}
if (Relocation::isTLS(RType)) {
  if (IsX86)
    return;
  if (!Relocation::isGOT(RType))
    return;
}
if (!IsAArch64 && BC->getDynamicRelocationAt(Rel.getOffset())) {
  ...
  return;
}
```

x86 的 TLS 无需特殊处理（直接返回）；AArch64/RISCV 只保留 GOT 型 TLS。地址已被动态重定位占据时（非 AArch64）跳过静态——两套重定位不重复处理。

**2. x86 数据→代码 PC 相对 (L3367-3396)**

```cpp
if (IsX86 && Relocation::isPCRelative(RType)) {
  if (!IsFromCode && IsToCode) {
    BC->addPCRelativeDataRelocation(Rel.getOffset());
  } else if (ContainingBF && !IsSectionRelocation && ReferencedSymbol) {
    ContainingBF->addRelocation(Rel.getOffset(), ReferencedSymbol, RType,
                                Addend, ExtractedValue);
  }
  return;
}
```

PIC 跳转表的编码值 = 表基址与目标的差，链接后原始信息已丢失，无法静态判断目标属于哪个函数——**只登记地址，语义恢复推迟到 `populateJumpTables`**（结合控制流，`disassembleFunctions` L4063 调用）。这是"信息不足时延迟决策"的典范。

**3. devirtualization bug 规避 (L3444-3468)**

```cpp
if (IsToCode && ContainingBF && !Relocation::isPCRelative(RType) &&
    (!ReferencedBF || (ReferencedBF->getAddress() != Address))) {
  if (const BinaryFunction *RogueBF = BC->getBinaryFunctionAtAddress(Address + 1)) {
    bool Found = llvm::any_of(
        llvm::make_second_range(ContainingBF->Relocations), CheckReloc);
    if (Found) {
      BC->errs() << "BOLT-WARNING: detected possible compiler de-virtualization bug ...";
      return;
    }
  }
}
```

老编译器"成员函数指针 = 真实地址 − 1"的 bug：非 PC 相对重定位指向函数头前一字节，且同函数已有对该函数的正常引用 → 判定命中，保持原样跳过。

**4. 引用点细化 (L3479-3523)**

```cpp
} else if (ReferencedBF) {
  ReferencedSymbol = ReferencedBF->getSymbol();
  if (ReferencedBF->containsAddress(Address, /*UseMaxSize=*/true)) {
    RefFunctionOffset = Address - ReferencedBF->getAddress();
    if (Relocation::isInstructionReference(RType)) {
      ReferencedSymbol = nullptr;
      ExtractedValue = Address;
    } else if (RefFunctionOffset) {
      if (ContainingBF && ContainingBF != ReferencedBF) {
        ReferencedSymbol =
            ReferencedBF->isInConstantIsland(Address)
                ? ReferencedBF->getOrCreateIslandAccess(Address)
                : ReferencedBF->addEntryPointAtOffset(RefFunctionOffset);
      } else {
        ReferencedSymbol = ReferencedBF->getOrCreateLocalLabel(Address);
        if (!ContainingBF && !ReferencedBF->isInConstantIsland(Address))
          ReferencedBF->registerInternalRefDataRelocation(RefFunctionOffset,
                                                          Rel.getOffset());
      }
    }
    SymbolAddress = Address;
    Addend = 0;
  }
}
```

**意图**：BOLT 重定位模型是 `Symbol + Addend`。让 Symbol 精确到"入口/局部 label/常量岛访问点"、Addend 归零，函数重排后只需重定向 Symbol。RISC-V `%pcrel_lo`（`isInstructionReference`）引用的是 `%pcrel_hi` **指令**，指令 label 反汇编时才存在，故先置空符号、用 `ExtractedValue=Address` 保存目标。

**5. BinaryData 基址化 (L3547-3569)**

```cpp
if (BinaryData *BD = BC->getBinaryDataContainingAddress(SymbolAddress)) {
  ...
  ReferencedSymbol = BD->getSymbol();
  Addend += (SymbolAddress - BD->getAddress());
  SymbolAddress = BD->getAddress();
  assert(Address == SymbolAddress + Addend);
}
```

数据引用同理归一到"数据对象头符号 + 偏移"。紧邻的 assert 检查 `BD->nameStartsWith(SymbolName)` 等名字一致性（AArch64/RISCV 因推断型地址而豁免，L3554-3561 注释）。

**6. 强制条件与最终分发 (L3608-3636)**

```cpp
if ((ReferencedSection && refersToReorderedSection(ReferencedSection)) ||
    (opts::ForceToDataRelocations && checkMaxDataRelocations()) ||
    BC->isRISCV())
  ForceRelocation = true;

if (IsFromCode)
  ContainingBF->addRelocation(Rel.getOffset(), ReferencedSymbol, RType,
                              Addend, ExtractedValue);
else if (IsToCode || ForceRelocation)
  BC->addRelocation(Rel.getOffset(), ReferencedSymbol, RType, Addend,
                    ExtractedValue);
else
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocation from data to data\n");
```

`refersToReorderedSection`：目标 section 命中 `--reorder-data` 列表 → 必须保留重定位（数据要搬家）；RISC-V 全量强制（ADD/SUB 数据到数据重定位特性）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::Relocations` | multimap<Offset, Relocation> | 代码内重定位表，指令符号化时消费 |
| `BinarySection::Relocations` | 同构 | 数据 section 级重定位 |
| `BC->forceSymbolRelocations` | 名字集合 | 强制保留的符号引用 |

### 优化意图

1. **归一化优先于分发**：第 9 步先把所有引用形态拉齐到统一模型，第 11 步的分发才能保持极简三分支。
2. 跳转表占位避免在静态阶段做出错误归属——错归属比晚归属危害大得多。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| vtable 指向函数中部 → exit(1) | L3288-3296 | 无法安全重排 |
| 代码 padding 区重定位 → 函数扩到 MaxSize 且 non-simple | L3318-3325 | 保守降级 |
| 岛访问必须用 `getOrCreateIslandAccess` | 岛数据随函数整体移动 | 普通 label 会导致引用错位 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 代码重定位登记 | `BinaryFunction::addRelocation` | `bolt/Core/BinaryFunction.cpp` |
| 数据重定位登记 | `BC->addRelocation` | `bolt/Core/BinaryContext.cpp` |
| 跳转表占位 | `BC->addPCRelativeDataRelocation` | 同上 |
| 局部 label | `BinaryFunction::getOrCreateLocalLabel` | 同上 |

### 其他补充

`checkMaxDataRelocations` lambda 实现 `--max-data-relocations` 调试上限（到达前一条时打印详情）。

---

## refersToReorderedSection 函数分析

### 函数签名与目的（L395-399，匿名命名空间）

```cpp
bool refersToReorderedSection(ErrorOr<BinarySection &> Section);
```

**功能**: 判断 section 是否命中 `--reorder-data` 名单。

### 整体结构

```text
refersToReorderedSection(Section)
└── any_of(opts::ReorderData, 名字相等) 且 Section 有效
```

### 逐段注释

`llvm::any_of` + 名字精确匹配，无复杂逻辑。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::ReorderData` | cl::list<string> | `--reorder-data` 指定的 section 名单 |

### 优化意图

1. 供 `handleRelocation` 决定"数据将搬家，引用必须保留为重定位"。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `ErrorOr` 无效时返回 false | 未注册 section | 安全侧默认 |

### 关键 API / 源码路径

无。

### 其他补充

仅被 `handleRelocation`（L3622）调用。

---

# 六、PLT 反汇编

## disassemblePLT 函数分析

### 函数签名与目的（L2116-2147）

```cpp
void RewriteInstance::disassemblePLT();
```

**功能**: 发现所有 PLT 类 section，按架构分发反汇编 entry，为每个 entry 建 `@PLT` 函数。

### 整体结构

```text
disassemblePLT()
├── 1. analyzeOnePLTSection lambda: 按架构分发
│      AArch64 → disassemblePLTSectionAArch64
│      RISCV   → disassemblePLTSectionRISCV
│      X86     → disassemblePLTSectionX86(EntrySize)
├── 2. 遍历 allocatable section:
│      getPLTSectionInfo(名字) 命中 → analyzeOnePLTSection
└── 3. section 起点无函数 → 建 __BOLT_PSEUDO_<sec> 占位并 setPseudo(true)
```

### 逐段注释

**1. pseudo 占位 (L2134-2145)**

```cpp
BinaryFunction *PltBF;
auto BFIter = BC->getBinaryFunctions().find(Section.getAddress());
if (BFIter != BC->getBinaryFunctions().end()) {
  PltBF = &BFIter->second;
} else {
  PltBF = BC->createBinaryFunction(
      "__BOLT_PSEUDO_" + Section.getName().str(), Section,
      Section.getAddress(), 0, PLTSI->EntrySize, Section.getAlignment());
}
PltBF->setPseudo(true);
```

PLT0（section 首 entry 的 resolver 桩）不是普通函数，但布局阶段仍需它占据地址空间——建 0 尺寸 pseudo 函数兜底。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `PLTSectionInfo` | EntrySize 等元数据 | `RewriteInstance.h` 中 `.plt/.plt.sec/.iplt/.plt.got` 的静态描述表（`getPLTSectionInfo`） |

### 优化意图

1. PLT 函数标 `setPseudo` 后不参与优化，只作为**地址翻译锚点**——调用点经 `@PLT` 名字间接引用目标，目标搬家后重定向 GOT/PLT 即可。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 三架构外 unreachable | `llvm_unreachable("Unmplemented PLT")` | 新架构接入需补实现 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PLT 元数据 | `getPLTSectionInfo` | `bolt/include/bolt/Rewrite/RewriteInstance.h` |

### 其他补充

无。

---

## disassemblePLTInstruction 函数分析

### 函数签名与目的（L1965-1984）

```cpp
void RewriteInstance::disassemblePLTInstruction(const BinarySection &Section,
                                                 uint64_t InstrOffset,
                                                 MCInst &Instruction,
                                                 uint64_t &InstrSize);
```

**功能**: 单条 PLT 指令反汇编公共 helper。

### 整体结构

```text
disassemblePLTInstruction(Section, InstrOffset, Instruction, InstrSize)
├── 从 section 内容 slice 出字节
├── BC->DisAsm->getInstruction(...)
└── 失败 → BOLT-ERROR + exit(1)
```

### 逐段注释

无复杂分段。AArch64/X86 版本共用（RISCV 版内联了自己的 lambda）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MCInst` | 操作数 | 反汇编产物，供 `MIB->analyzePLTEntry` 等消费 |

### 优化意图

1. 抽公共 helper 消除三架构版本里的重复错误处理。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 失败即 exit | PLT 是布局关键路径 | 静默跳过会造成后续地址错乱 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 反汇编 | `MCDisassembler::getInstruction` | `llvm/MC/MCDisassembler/MCDisassembler.h` |

### 其他补充

无。

---

## disassemblePLTSectionAArch64 函数分析

### 函数签名与目的（L1986-2028）

```cpp
void RewriteInstance::disassemblePLTSectionAArch64(BinarySection &Section);
```

**功能**: 反汇编 AArch64 PLT section（entry 无固定大小，动态扫描）。

### 整体结构

```text
disassemblePLTSectionAArch64(Section)
├── 外层 while: 逐 entry 扫描
│    ├── 内层 while: 逐指令累积
│    │    ├── disassemblePLTInstruction
│    │    ├── 非间接分支 → 收集指令继续
│    │    └── isIndirectBranch（br x16/blr）→ entry 结束:
│    │         analyzePLTEntry(...) → GOT 目标地址
│    │         createPLTBinaryFunction(Target, EntryAddr, EntrySize)
│    └── 跳过 entry 间 nop 填充
```

### 逐段注释

无额外代码片段（结构即全部）。AArch64 PLT entry 含 BTI `bti c` 桩时变长，无法用固定 stride——以 `isIndirectBranch` 作为 entry 终止信号，`analyzePLTEntry` 从整个指令序列反推 GOT 槽地址。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `InstructionListType` | MCInst 列表 | entry 指令序列，交 `analyzePLTEntry` 分析 |

### 优化意图

1. 动态扫描兼容 BTI/非 BTI 两种 entry 形态，无需链接器配合。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| entry 必须以间接分支结尾 | 扫描假设 | 异常数据会扫到 section 末尾（安全退出循环） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PLT 分析 | `MCPlusBuilder::analyzePLTEntry` | `bolt/lib/Target/AArch64/AArch64MCPlusBuilder.cpp` |

### 其他补充

无。

---

## disassemblePLTSectionRISCV 函数分析

### 函数签名与目的（L2030-2075）

```cpp
void RewriteInstance::disassemblePLTSectionRISCV(BinarySection &Section);
```

**功能**: 反汇编 RISC-V PLT/IPLT section（entry 固定 16 字节）。

### 整体结构

```text
disassemblePLTSectionRISCV(Section)
├── IsHeaderless = (名字 == ".iplt") → 起始 0；否则跳过前 32 字节 header（PLT0）
├── 内联 disassembleInstruction lambda（含 exit 错误处理）
└── while: 每 16 字节一个 entry
     ├── 累积指令 → analyzePLTEntry → TargetAddress
     └── createPLTBinaryFunction(Target, EntryAddr, EntrySize=16)
```

### 逐段注释

无额外代码片段。`.iplt` 无 header（`IsHeaderless` 判断）是 RISC-V 链接器行为；常规 `.plt` 的 PLT0 是 32 字节 resolver 跳板。

### 关键数据结构

同 AArch64 版。

### 优化意图

1. 固定 entry 尺寸使扫描退化为等步长循环，最简单可靠。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| entry 尺寸硬编码 16 | ABI 约定 | 压缩指令场景由 analyzePLTEntry 容错 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PLT 分析 | `MCPlusBuilder::analyzePLTEntry` | `bolt/lib/Target/RISCV/RISCVMCPlusBuilder.cpp` |

### 其他补充

无。

---

## disassemblePLTSectionX86 函数分析

### 函数签名与目的（L2077-2114）

```cpp
void RewriteInstance::disassemblePLTSectionX86(BinarySection &Section,
                                                uint64_t EntrySize);
```

**功能**: 反汇编 x86 PLT section（`jmp *[rip+X]` 结构规整，直接求值）。

### 整体结构

```text
disassemblePLTSectionX86(Section, EntrySize)
├── for: 每 EntrySize 一个 entry
│    ├── while: 逐指令到 entry 尾
│    │    ├── 首 entry 且 isTerminateBranch 且 EntrySize==8 → EntrySize 调整为 16
│    │    └── isIndirectBranch → break
│    ├── 越界检查
│    └── evaluateMemOperandTarget(Instruction, &TargetAddress, ...)
│         → createPLTBinaryFunction(Target, EntryAddr, EntrySize)
```

### 逐段注释

**1. entry 尺寸自适应 (L2089-2091)**

```cpp
if (EntryOffset == 0 && BC->MIB->isTerminateBranch(Instruction) &&
    EntrySize == 8)
  EntrySize = 16;
```

PLT0 用 16 字节（`push; jmp`），普通 entry 8 字节（`jmp *[rip+X]`）——首 entry 检测 `isTerminateBranch` 自动切换。

### 关键数据结构

同 AArch64 版。

### 优化意图

1. x86 PLT 结构完全规整，`evaluateMemOperandTarget` 直接对 `jmp *[rip+X]` 求值得到 GOT 地址，无需序列分析。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 求值失败 exit(1) | 结构异常 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 内存操作数求值 | `MCPlusBuilder::evaluateMemOperandTarget` | `bolt/lib/Target/X86/X86MCPlusBuilder.cpp` |

### 其他补充

无。

---

## createPLTBinaryFunction 函数分析

### 函数签名与目的（L1891-1963）

```cpp
void RewriteInstance::createPLTBinaryFunction(uint64_t TargetAddress,
                                               uint64_t EntryAddress,
                                               uint64_t EntrySize);
```

**功能**: 为 PLT entry 建函数并挂接 GOT 侧符号。

### 整体结构

```text
createPLTBinaryFunction(TargetAddress, EntryAddress, EntrySize)
├── 1. TargetAddress==0 → 返回（解析失败）
├── 2. 已有函数且 AArch64 → IFUNC trampoline 带 symbol:
│      setPLTSymbol("name@GOT" → GOT 槽地址) 后返回
├── 3. GOT 槽动态重定位取目标:
│      有 symbol → 用之
│      无 symbol 且 isIRelative → addend 是解析器函数，取其 symbol
├── 4. 无函数 → createBinaryFunction("sym@PLT")；有 → addAlternativeName("@PLT")
└── 5. [RISCV + IRELATIVE] 所有 STT_GNU_IFUNC 别名符号都注册 alias@PLT
```

### 逐段注释

**1. RISC-V 别名注册 (L1947-1962)**

```cpp
if (BC->isRISCV() && Rel->isIRelative()) {
  auto ResolverSyms = FileSymRefs.equal_range(Rel->Addend);
  for (const SymbolRef &AliasSymbol : ...) {
    if (ELFSymbolRef(AliasSymbol).getELFType() != ELF::STT_GNU_IFUNC)
      continue;
    ...
    BF->addAlternativeName(PLTName);
    BC->registerNameAtAddress(PLTName, EntryAddress, EntrySize, ...);
    setPLTSymbol(BF, AliasName);
  }
}
```

多个 IFUNC 符号可共享同一 resolver，`R_RISCV_CALL_PLT` 可能用任一别名调用——全部注册到同一 IPLT entry，`getPLTBinaryDataByName` 才能解析所有调用点。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::PLTSymbol` | GOT 槽符号 | `setPLTSymbol` 登记，重定位重写时消费 |

### 优化意图

1. `@PLT`/`@GOT` 命名约定把"桩-槽"配对显式化，后续 `handleRelocation`/`patchELFGOT` 按名字即可找到配对物。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 解析失败的 entry 静默跳过 | 只建能确定目标的 | 漏建会导致调用点引用悬空（有告警兜底） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| GOT 符号注册 | `BC->registerNameAtAddress` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

# 七、函数边界与 fragment

## isCFIBoundedTailPredecessor 函数分析

### 函数签名与目的（L2155-2163，匿名命名空间）

```cpp
bool isCFIBoundedTailPredecessor(const BinaryFunction &BF,
                                 const CFIReaderWriter &CFI);
```

**功能**: 判断前驱函数是否"在 FDE 边界处结束"——其后的尾巴字节才可信是独立代码。

### 整体结构

```text
isCFIBoundedTailPredecessor(BF, CFI)
├── 名字以 __BOLT_FDE_FUNC 开头 → true（FDE 兜底合成函数）
├── 查 FDE：不存在 → false
└── FDE 地址范围 == BF.getSize() → true
```

### 逐段注释

无复杂分段。两类"可信前驱"：BOLT 从 FDE 合成的函数（无 symtab 条目），或 symtab 尺寸与 FDE 范围一致的函数。只有这种前驱的 `[Size, MaxSize)` 区间才可能是"漏标号的独立代码"而非函数体一部分。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `CFIReaderWriter::getFDEs()` | map<地址, FDE*> | FDE 索引（第三章 readSpecialSections 建立） |

### 优化意图

1. 为 `splitUnmarkedTailFunctions` 提供保守前置筛——错把函数体当"尾巴"切出去是正确性灾难。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须精确等于 | 大小一致才可信 | 偏小的 symtab 尺寸不可作为边界依据 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| FDE 查询 | `CFIReaderWriter::getFDEs` | `bolt/Core/Exceptions.h` |

### 其他补充

被 `adjustFunctionBoundaries`（L2352）与 `splitUnmarkedTailFunctions`（L2271）调用。

---

## isAArch64TailPaddingInst 函数分析

### 函数签名与目的（L2167-2171，匿名命名空间）

```cpp
bool isAArch64TailPaddingInst(const BinaryContext &BC, const MCInst &Inst);
```

**功能**: 判定指令是否为 AArch64 padding/filler（nop 或 trap）。

### 整体结构

```text
isAArch64TailPaddingInst(BC, Inst)
└── MIB->isNoop(Inst) || MIB->isTrap(Inst)
```

### 逐段注释

单表达式，无代码片段。

### 关键数据结构

无。

### 优化意图

1. `measureAArch64UnmarkedTail` 用它把 ret 之后的 filler 当 slack 而非可调用代码。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| nop/trap 判定 | `MCPlusBuilder::isNoop/isTrap` | `bolt/Core/MCPlusBuilder.h` |

### 其他补充

无。

---

## isValidAArch64UnmarkedTail 函数分析

### 函数签名与目的（L2176-2183，匿名命名空间）

```cpp
bool isValidAArch64UnmarkedTail(const BinaryContext &BC, ArrayRef<MCInst> Insts);
```

**功能**: 判断解码出的尾巴是否像"真实可调用代码"（末条指令必须是 ret）。

### 整体结构

```text
isValidAArch64UnmarkedTail(BC, Insts)
├── 空 → false
└── MIB->isReturn(Insts.back()) → true
```

### 逐段注释

单表达式。前置条件：trailing filler 已被调用方裁剪，`Insts.back()` 即"逻辑上最后一条"。

### 关键数据结构

无。

### 优化意图

1. 以 `ret` 结尾是"可调用片段"的最低特征——把数据/跳转表误判为代码的风险降到可接受水平。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅启发式 | 非充分条件 | 后续反汇编失败仍会降级 non-simple |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 返回指令判定 | `MCPlusBuilder::isReturn` | `bolt/Core/MCPlusBuilder.h` |

### 其他补充

无。

---

## measureAArch64UnmarkedTail 函数分析

### 函数签名与目的（L2190-2256，匿名命名空间）

```cpp
uint64_t measureAArch64UnmarkedTail(
    BinaryContext &BC, const BinaryFunction &Pred,
    DenseMap<uint64_t, MarkerSymType> &MarkerSyms,
    uint64_t TailStart, uint64_t TrailingExtent);
```

**功能**: 实测"前驱函数之后未标记区域"中可调用代码的长度，0 表示不是合法未标记代码。

### 整体结构

```text
measureAArch64UnmarkedTail(BC, Pred, MarkerSyms, TailStart, TrailingExtent)
├── 1. 区间必须完整落在 Pred 的 origin section 内
├── 2. 逐指令解码 [TailStart, TailStart+TrailingExtent):
│      DATA marker / 常量岛地址 → 返回 0
│      解码失败 → break（CodeLen 停止增长）
├── 3. 尾部 filler（nop/trap）裁剪: TailLen 不含 filler
├── 4. 裁剪后必须非空且以 ret 结尾（isValidAArch64UnmarkedTail）
├── 5. 解码区之后剩余字节必须全零
└── 6. 返回 TailLen
```

### 逐段注释

**1. 双重阻断 (L2215-2219)**

```cpp
while (CodeLen < TrailingExtent) {
  if (hasDataMarkerAt(TailStart + CodeLen))
    return 0;
  if (Pred.isInConstantIsland(TailStart + CodeLen))
    return 0;
  ...
}
```

遇 DATA marker 或常量岛地址立即失败——这段区域含数据，不属于"可切分的纯代码"。

**2. filler 裁剪 (L2236-2242)**

```cpp
uint64_t TailLen = CodeLen;
size_t CallableInsts = Insts.size();
while (CallableInsts > 0 &&
       isAArch64TailPaddingInst(BC, Insts[CallableInsts - 1])) {
  --CallableInsts;
  TailLen -= InstSizes[CallableInsts];
}
```

ret 之后的 nop/trap 计入 slack（与后面的全零 padding 同等待遇），真实二进制的 post-ret 对齐填充不会导致误判失败。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MarkerSyms`（入参） | 地址→DATA/CODE | `discoverFileObjects` 收集的标记表 |
| `SmallVector<MCInst/uint64_t, 4>`（局部） | 指令与尺寸 | 双数组同步累积 |

### 优化意图

1. **宁可漏判不可误判**：五个关卡（marker、岛、ret 结尾、全零尾、section 内）层层过滤，只有"教科书式无名函数"才会被切出，配合 WARNING 提示用户补符号。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 返回 0 = 不处理 | 前驱保留原 MaxSize | 尾巴被当前驱 padding 处理（安全） |
| 解码失败即止 | 部分解码按已解部分判定 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 符号化反汇编器 | `BC.SymbolicDisAsm->getInstruction` | `bolt/Core/BinaryContext.h` |

### 其他补充

无。

---

## splitUnmarkedTailFunctions 函数分析

### 函数签名与目的（L2260-2304）

```cpp
void RewriteInstance::splitUnmarkedTailFunctions(
    DenseMap<uint64_t, MarkerSymType> &MarkerSyms);
```

**功能**: 把 AArch64 上"前驱函数之后的未标记代码"切成独立合成函数 `__BOLT_UNMARKED_TAILat<hex>`。

### 整体结构

```text
splitUnmarkedTailFunctions(MarkerSyms)
├── 1. 非 AArch64 → 返回
├── 2. 收集候选: 非 pseudo/fragment、MaxSize>Size、isCFIBoundedTailPredecessor
├── 3. 逐候选:
│      尾巴起点已有函数 → 跳过
│      measureAArch64UnmarkedTail 实测 → 0 跳过
│      Pred->setMaxSize(Pred->getSize())   // 前驱让位
│      createBinaryFunction("__BOLT_UNMARKED_TAILat<hex>", Section,
│                            TailStart, CodeLen, CodeLen)
│      TailBF->setMaxSize(TrailingExtent)
└── 4. WARNING 提示补符号/FDE
```

### 逐段注释

无额外代码片段（结构即全部）。前驱 `setMaxSize(Size)` 收紧后，其后的新函数拿到 `[Size, TrailingExtent)` 的完整区间——两函数边界自此干净。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::MaxSize` | uint64 | 函数可扩张上限，非 reloc 模式覆写不得越界 |

### 优化意图

1. **正确性兜底**：汇编器/链接器偶发漏符号的真实代码若被并入前驱，BOLT 重排时会当作前驱的 padding 覆写——切成独立函数后该代码被完整保留。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅 AArch64 | 其他架构 marker 体系不同 | — |
| 新函数 MaxSize = TrailingExtent | 覆盖整个未标记区 | 与前驱收紧配套，无重叠 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 建函数 | `BC->createBinaryFunction` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

被 `discoverFileObjects`（L1420）调用，在 `adjustFunctionBoundaries` 之后。

---

## adjustFunctionBoundaries 函数分析

### 函数签名与目的（L2306-2394）

```cpp
void RewriteInstance::adjustFunctionBoundaries(
    DenseMap<uint64_t, MarkerSymType> &MarkerSyms);
```

**功能**: 符号扫描完成后，逐函数计算 **MaxSize**（可扩张上限）并补登二级入口。

### 整体结构

```text
adjustFunctionBoundaries(MarkerSyms)
└── 逐函数（按地址序迭代 BC->getBinaryFunctions()）:
     ├── 1. 从 FileSymRefs.upper_bound(函数头) 升序扫体内符号:
     │      撞到下一函数 / 无效 in-scope 符号 → 停
     │      RISC-V .L 基本块 label → 跳过继续
     │      AArch64 FDE 边界上的符号 → 跳过（留给尾巴切分）
     │      非 DATA marker → addEntryPointAtOffset(偏移)
     ├── 2. NextObjectAddress = min(下一符号, 下一函数, section 末尾)
     ├── 3. MaxSize < Size → 告警 + setSimple(false) + setMaxSize(Size)
     └── 4. Size==0 且 simple → Size = MaxSize（零尺寸汇编函数修正）
```

### 逐段注释

**1. 体内符号扫描 (L2323-2363)**

```cpp
auto NextSymRefI = FileSymRefs.upper_bound(Function.getAddress());
while (NextSymRefI != FileSymRefs.end()) {
  ...
  if (NextFunction && SymbolAddress >= NextFunction->getAddress())
    break;
  if (!Function.isSymbolValidInScope(Symbol, SymbolSize))
    break;
  const auto InternalSymbolPrefix = BC->AsmInfo->getInternalSymbolPrefix();
  if (!InternalSymbolPrefix.empty() &&
      cantFail(Symbol.getName()).starts_with(InternalSymbolPrefix)) {
    ++NextSymRefI;
    continue;
  }
  auto It = MarkerSyms.find(NextSymRefI->first);
  if (It == MarkerSyms.end() || It->second != MarkerSymType::DATA) {
    uint64_t EntryOffset = NextSymRefI->first - Function.getAddress();
    if (BC->isAArch64() && EntryOffset == Function.getSize() &&
        isCFIBoundedTailPredecessor(Function, *CFIRdWrt)) {
      ++NextSymRefI;
      continue;
    }
    Function.addEntryPointAtOffset(EntryOffset);
  }
  ++NextSymRefI;
}
```

RISC-V linker relaxation 给每个分支配重定位+符号，`.L` label 不能当入口（L2335-2343 注释）；AArch64 的 FDE 边界符号留给 `splitUnmarkedTailFunctions`；DATA marker 处是数据。

**2. MaxSize 计算 (L2366-2384)**

```cpp
uint64_t NextObjectAddress = Function.getOriginSection()->getEndAddress();
if (NextSymRefI != FileSymRefs.end())
  NextObjectAddress = std::min(NextSymRefI->first, NextObjectAddress);
if (NextFunction)
  NextObjectAddress = std::min(NextFunction->getAddress(), NextObjectAddress);
const uint64_t MaxSize = NextObjectAddress - Function.getAddress();
```

扫描停在哪个符号，那个符号的地址就是上限——`FileSymRefs` 的升序性保证了这是"最近的下一个对象"。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `FileSymRefs`（成员） | multimap 地址→符号 | 升序扫描基础（`discoverFileObjects` 建立） |

### 优化意图

1. MaxSize 是非 reloc 模式原地覆写的**硬约束**（`rewriteFunctionsInPlace` assert `ImageSize <= MaxSize`），此处一次算清。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| MaxSize < Size 说明符号在函数中间 | setSimple(false) 跳过优化 | 继续优化会覆写别的对象 |
| 零尺寸函数按 MaxSize 补 | 汇编函数常见 | 不补则反汇编空转 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 入口登记 | `BinaryFunction::addEntryPointAtOffset` | `bolt/Core/BinaryFunction.cpp` |
| in-scope 判定 | `BinaryFunction::isSymbolValidInScope` | 同上 |

### 其他补充

迭代时保存 `std::next(BFI)` 作为 NextFunction——`getBinaryFunctions()` 按地址有序（`std::map<地址, BF>`）。

---

## registerFragments 函数分析

### 函数签名与目的（L1736-1889）

```cpp
void RewriteInstance::registerFragments();
```

**功能**: 把输入中已分裂的 cold 片段挂接到 parent 函数（`BC->registerFragment`）。

### 整体结构

```text
registerFragments()
├── 0. 无 HasSplitFunctions / heatmap 独占 → 返回
├── 1. 无歧义匹配轮:
│      NR.restore(名字) → FunctionFragmentTemplate.match → ParentName
│      唯一本地 parent（最常见）→ 直接挂
│      全局 parent（次常见）→ 直接挂
│      其余 → AmbiguousFragments 暂存
├── 2. [有歧义项] 检查 hasSymbolsWithFileName（否则 exit(1) 提示 --keep-file-symbols）
├── 3. getLocalSymEnd: symtab sh_info 指出首个全局符号位置
└── 4. 逐歧义 fragment:
       ├── FileSymRefs 找 fragment 自身符号 → 定位包含 FILE 符号
       ├── FILE 是 BOLT 合成符号 → 按全局 parent 注册（上轮 BOLT 输出）
       ├── 从 fragment 符号向后扫: BOLT 片段符号紧挨 parent 主符号前
       ├── 否则在本 FILE 区间内线性找同名 parent
       └── 找到地址 → getBinaryFunctionAtAddress → registerFragment；失败 exit(1)
```

### 逐段注释

**1. 无歧义匹配 (L1758-1768)**

```cpp
const bool IsGlobal = BaseName == Name;
SmallVector<StringRef> Matches;
if (!FunctionFragmentTemplate.match(BaseName, &Matches))
  continue;
StringRef ParentName = Matches[1];
const BinaryData *BD = BC->getBinaryDataByName(ParentName);
const uint64_t NumPossibleLocalParents =
    NR.getUniquifiedNameCount(ParentName);
if (!BD && NumPossibleLocalParents == 1) {
  BD = BC->getBinaryDataByName(NR.getUniqueName(ParentName, 1));
} else if (BD && (!NumPossibleLocalParents || IsGlobal)) {
} else {
  AmbiguousFragments.emplace_back(ParentName, &Function);
  continue;
}
```

`NR.restore` 去掉 uniquify 后缀还原原名；三类常见情形（唯一本地/全局/全局+全局片段）当场解决，歧义项（多个同名本地候选）进第二轮。

**2. BOLT 合成 FILE 符号捷径 (L1846-1849)**

```cpp
if (cantFail(FSI[-1].getName()) == getBOLTFileSymbolName())
  goto registerParent;
```

上一轮 BOLT 在 split 函数前插入了合成 FILE 符号（`updateELFSymbolTable` 的 `addExtraSymbols` 生成），本轮见到它直接走全局 parent——两次 BOLT 的输出/输入约定闭环。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `NR.getUniquifiedNameCount(Name)` | 同名本地数 | 歧义判定核心 |
| `BC->registerFragment` | fragment↔parent 双向 | parent 的 `Fragments` 与 fragment 的 `ParentFragments` |

### 优化意图

1. 两轮设计：昂贵符号表查找只留给"少数歧义项"（注释：vanishing minority），主流情形零开销。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 配对失败 exit(1) | 宁死不猜 | 错配对 = 错误合并布局 |
| 无 FILE 符号的歧义输入 | exit(1) | 提示用 `--keep-file-symbols` 重新 strip |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| fragment 注册 | `BC->registerFragment` | `bolt/Core/BinaryContext.cpp` |
| 本地符号终点 | symtab `sh_info` | `llvm/Object/ELF.h` |

### 其他补充

多路分裂（一个 parent 多个 `.cold.N`）时 parent 跟在最后一个片段符号之后（L1860-1864 注释）。

---

# 八、插桩运行时钩子

## discoverRtInitAddress 函数分析

### 函数签名与目的（L1521-1578）

```cpp
Error RewriteInstance::discoverRtInitAddress();
```

**功能**: 确定"程序第一个用户初始化函数"地址，插桩运行库需在它之前完成计数器初始化。

### 整体结构

```text
discoverRtInitAddress()
├── 1. 有 INTERP 且钩子=entry_point → 保持 e_entry 不动
├── 2. DT_INIT 存在且钩子级别 <= init → StartFunctionAddress = DT_INIT
├── 3. .init_array[0]:
│      首槽动态重定位 RELATIVE → addend 即地址
│      符号+addend → 函数地址 + addend
│      静态重定位 → Reloc->Value
└── 4. 都没有 → not_supported 错误（附 .init_array 校验）
```

### 逐段注释

**1. 一致性校验 (L1531-1550)**

```cpp
if (!BC->InitArrayAddress || !BC->InitArraySize) {
  return createStringError(std::errc::not_supported,
                           "Instrumentation of shared library needs either "
                           "DT_INIT or DT_INIT_ARRAY");
}
if (*BC->InitArraySize < BC->AsmInfo->getCodePointerSize()) {
  return createStringError(std::errc::not_supported,
                           "Need at least 1 DT_INIT_ARRAY slot");
}
...
if (InitArraySection->getAddress() != *BC->InitArrayAddress) {
  return createStringError(std::errc::not_supported,
                           "Inconsistent address of .init_array section");
}
```

至少一个指针槽、section 地址与 `DT_INIT_ARRAY` 一致——发现期就把畸形输入挡住，回写期（`updateRtInitReloc`）依赖这些不变式。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->StartFunctionAddress` | uint64 | 钩子点锚定（构造期初始化为 e_entry） |
| `opts::RuntimeLibInitHook` | RLIH 三态 | entry_point → init → init_array 回退序 |

### 优化意图

1. 三级回退序（`-runtime-lib-init-hook` 可调）兼容静态/动态/无 INTERP 各种形态；钩子点选择影响的是"运行库何时接管"，不影响语义。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 与 updateRtInitReloc 条件一致 | run() 两处 if 必须同构 | 不一致会改写错误的钩子 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 槽位重定位查询 | `BinarySection::getDynamicRelocationAt/getRelocationAt` | `bolt/Core/BinarySection.h` |

### 其他补充

无。

---

## discoverRtFiniAddress 函数分析

### 函数签名与目的（L1580-1623）

```cpp
Error RewriteInstance::discoverRtFiniAddress();
```

**功能**: fini 侧镜像：`DT_FINI` → `.fini_array[0]` →（`-instrumentation-sleep-time` 场景允许全无）。

### 整体结构

与 `discoverRtInitAddress` 同构，差异：无 entry_point 钩子形态；`FiniArray` 全缺且 sleep-time>0 时返回 success（睡眠模式下进程被外部终止，无需 fini 钩子）。

### 逐段注释

无额外代码片段。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->FiniFunctionAddress` | uint64 | fini 钩子锚定 |

### 优化意图

1. sleep-time 豁免：`-instrumentation-sleep-time` 让程序运行 N 秒后自我终止，不经过正常 fini 流程。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `.fini_array[0]` 只支持 RELATIVE 型 | 无符号分支 | 符号型 fini 槽极少见 |

### 关键 API / 源码路径

同 `discoverRtInitAddress`。

### 其他补充

无。

---

## updateRtInitReloc 函数分析

### 函数签名与目的（L1625-1689）

```cpp
Error RewriteInstance::updateRtInitReloc();
```

**功能**: 发射+链接后，把 init 钩子点改写为运行库入口（`RT->getRuntimeStartAddress()`）。

### 整体结构

```text
updateRtInitReloc()
├── 1. entry_point 钩子 → return（e_entry 由 patchELFSectionHeaderTable 改）
├── 2. DT_INIT 路径 → return（patchELFDynamic 直接改条目）
├── 3. .init_array[0] 动态重定向:
│      takeDynamicRelocationAt(0) 取出原重定位
│      校验 addend == 发现期的 StartFunctionAddress
│      RELATIVE → addend 改为运行库地址后 re-add
│      符号型 → 换成 ABS64 + addend 型
└── 4. addPendingRelocation({Offset:0, nullptr, Abs64, Addend:RT地址})
       → rewriteFile 时 flushPendingRelocations 落盘
```

### 逐段注释

**1. 双重写机制 (L1649-1683)**

```cpp
if (std::optional<Relocation> Reloc =
        InitArraySection->takeDynamicRelocationAt(0)) {
  if (Reloc->isRelative()) {
    if (Reloc->Addend != BC->StartFunctionAddress)
      return createStringError(..., "inconsistent .init_array dynamic relocation");
    Reloc->Addend = RT->getRuntimeStartAddress();
    InitArraySection->addDynamicRelocation(*Reloc);
  } else {
    ...
    InitArraySection->addDynamicRelocation(Relocation{
        /*Offset*/ 0, /*Symbol*/ nullptr, /*Type*/ Relocation::getAbs64(),
        /*Addend*/ RT->getRuntimeStartAddress(), /*Value*/ 0});
  }
}
InitArraySection->addPendingRelocation(Relocation{
    /*Offset*/ 0, /*Symbol*/ nullptr, /*Type*/ Relocation::getAbs64(),
    /*Addend*/ RT->getRuntimeStartAddress(), /*Value*/ 0});
```

**为什么写两份**：`.init_array[0]` 的内存映像由动态重定位描述（RELATIVE addend 供动态链接器回填），但静态文件内容也要正确（供不跑动态链接器的路径/工具读取）。pending relocation 的值按"Symbol + Addend"计算，无符号时 addend 即终值。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinarySection` pending 队列 | 待落盘重定位 | `rewriteFile`→`flushPendingRelocations` 消费 |

### 优化意图

1. 校验发现期快照一致后才改写——中途任何 pass 若动过 init 钩子会在此暴露。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| addend 必须与发现期一致 | 否则报 not_supported | 说明流程被外因扰动 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 取出/回加动态重定位 | `takeDynamicRelocationAt/addDynamicRelocation` | `bolt/Core/BinarySection.h` |

### 其他补充

无。

---

## updateRtFiniReloc 函数分析

### 函数签名与目的（L1691-1734）

```cpp
Error RewriteInstance::updateRtFiniReloc();
```

**功能**: fini 侧镜像：改 `.fini_array[0]` 动态重定位 addend + pending 静态重定位。

### 整体结构

与 `updateRtInitReloc` 同构且更简单：fini 只支持 RELATIVE 型（无符号分支）；DT_FINI 路径同样交由 `patchELFDynamic` 处理。

### 逐段注释

无额外代码片段。

### 关键数据结构

同 `updateRtInitReloc`。

### 优化意图

同 `updateRtInitReloc`。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| sleep-time 豁免与发现期一致 | 两函数条件同构 | — |

### 关键 API / 源码路径

同 `updateRtInitReloc`。

### 其他补充

无。

---

# 九、Profile 与函数选择

## preprocessProfileData 函数分析

### 函数签名与目的（L3880-3909）

```cpp
void RewriteInstance::preprocessProfileData();
```

**功能**: 正式读 profile 前的预处理（对 perf2bolt 是启动 perf script 解析 perf.data）。

### 整体结构

```text
preprocessProfileData()
├── 1. 无 ProfileReader → 返回
├── 2. 打印 reader 名
├── 3. 输入带 BAT → ProfileReader->setBAT(&*BAT)
├── 4. ProfileReader->preprocessProfile(*BC)
└── 5. stripped 一致性检查:
       输入无 FILE 符号 但 profile 有带文件名的本地名 → exit(1)
       （-allow-stripped 放行）
```

### 逐段注释

**1. BAT 注入 (L3890-3894)**

```cpp
if (BAT->enabledFor(InputFile)) {
  BC->outs() << "BOLT-INFO: profile collection done on a binary already "
               "processed by BOLT\n";
  ProfileReader->setBAT(&*BAT);
}
```

profile 采自 BOLT 处理过的二进制，采样地址需经 BAT 反翻译回输入地址才能匹配。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BoltAddressTranslation` | 输出→输入映射 | `setBAT` 注入 reader |

### 优化意图

1. **stripped 检测前置**：名字体系不匹配（输入 strip 过而 profiled 没strip）会让 profile 匹配率崩塌，早失败好过静默产出垃圾优化。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| hasLocalsWithFileName 与输入符号形态 | 交叉校验 | 忽略会导致近乎全 miss |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 预处理 | `ProfileReader::preprocessProfile` | `bolt/Profile/ProfileReader.h` |

### 其他补充

无。

---

## processProfileDataPreCFG 函数分析

### 函数签名与目的（L3948-3957）

```cpp
void RewriteInstance::processProfileDataPreCFG();
```

**功能**: 读取 CFG 构建前就需要的信息（函数级入口计数）。

### 整体结构

```text
processProfileDataPreCFG()
├── 无 ProfileReader → 返回
└── ProfileReader->readProfilePreCFG(*BC)   // 失败 report_error
```

### 逐段注释

无复杂分段。产出（如 `getKnownExecutionCount`）被 `selectFunctionsToProcess` 的 lite 阈值决策消费。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction` 计数字段 | 入口执行计数 | lite 模式的过滤依据 |

### 优化意图

1. 拆两级读取：preCFG 只做轻量的函数级信息，避免为 ignored 函数白做块级绑定。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| preCFG 读取 | `ProfileReader::readProfilePreCFG` | `bolt/Profile/ProfileReader.h` |

### 其他补充

由 `processMetadataPreCFG`（L3939）调用，位于 disassembleFunctions 之后、buildFunctionsCFG 之前。

---

## processProfileData 函数分析

### 函数签名与目的（L3959-3999）

```cpp
void RewriteInstance::processProfileData();
```

**功能**: 把 profile 计数绑定到 CFG（块/边频度），随后可选导出聚合结果。

### 整体结构

```text
processProfileData()
├── 1. ProfileReader->readProfile(*BC)
├── 2. [-print-profile/-print-all] 逐函数打印
├── 3. [-o] 且非 BAT 输入 → YAMLProfileWriter::writeProfile
├── 4. [AggregateOnly 且 YAML 格式且非 BAT] 写 OutputFilename
├── 5. ProfileReader.reset()   // 释放
└── 6. [AggregateOnly] PrintProgramStats + TimerGroup 打印 → exit(0)
```

### 逐段注释

**1. 聚合模式出口 (L3993-3998)**

```cpp
if (opts::AggregateOnly) {
  PrintProgramStats PPS(&*BAT);
  BC->logBOLTErrorsAndQuitOnFatal(PPS.runOnFunctions(*BC));
  TimerGroup::printAll(outs());
  exit(0);
}
```

`-aggregate-only` 是 perf2bolt 的形态：到此结束，不重写二进制。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryBasicBlock::ExecutionCount` / 边计数 | uint64/结构 | 后续所有布局 pass 的输入 |

### 优化意图

1. `ProfileReader.reset()` 在数据全部转移进 BinaryFunction 后立刻释放——reader 持有 perf 解析的中间结构（可达数百 MB）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| BAT 输入不重复导出 YAML | 二次翻译无意义 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| profile 绑定 | `ProfileReader::readProfile` | `bolt/Profile/ProfileReader.h` |
| YAML 导出 | `YAMLProfileWriter::writeProfile` | `bolt/lib/Profile/YAMLProfileWriter.cpp` |

### 其他补充

无。

---

## populateFunctionNames 函数分析

### 函数签名与目的（L3655-3663，静态）

```cpp
static void populateFunctionNames(cl::opt<std::string> &FunctionNamesFile,
                                  cl::list<std::string> &FunctionNames);
```

**功能**: 把 `-funcs-file` 类选项指定的文件逐行读入对应 `cl::list`。

### 整体结构

```text
populateFunctionNames(File, List)
└── File 非空 → std::getline 逐行 push_back
```

### 逐段注释

无复杂分段。用 `std::ifstream` 而非 LLVM `MemoryBuffer`（行数少，开销无所谓）。

### 关键数据结构

无。

### 优化意图

1. 三个文件选项（funcs/skip-funcs/funcs-no-regex）复用同一装载器。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 不检查文件存在性 | ifstream 静默空 | 空名单=不限制，可能非用户预期 |

### 关键 API / 源码路径

无。

### 其他补充

被 `selectFunctionsToPrint` 与 `selectFunctionsToProcess` 调用。

---

## getInitFunctionIfStaticBinary 函数分析

### 函数签名与目的（L3638-3653，静态）

```cpp
static BinaryFunction *getInitFunctionIfStaticBinary(BinaryContext &BC);
```

**功能**: aarch64 静态 glibc 二进制返回 `_init` 函数（供跳过），其余返回 nullptr。

### 整体结构

```text
getInitFunctionIfStaticBinary(BC)
├── 非 IsStaticExecutable → nullptr
├── 无 "_init" BinaryData 或不在 .init → nullptr
└── getBinaryFunctionAtAddress(_init 地址)
```

### 逐段注释

无复杂分段。源码注释引用 issue #100096：aarch64 静态 glibc 的 `.init` 的 `_init` 指针可能与某数组尾指针 alias，GOT 重写会把数据指针错误搬移到新 `_init` 地址，运行时崩溃；跳过 `_init`（不重排）无副作用。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryData` | 地址/section 名 | `_init` 定位 |

### 优化意图

1. **上游 bug 的本地 workaround**：比修 GOT 重写逻辑便宜且风险低。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅静态二进制 | 动态场景无此 alias 问题 | 过度跳过只损失一点优化 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 符号查找 | `BC.getBinaryDataByName` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

消费点：`selectFunctionsToProcess` L3785-3786（`Init->setIgnored()`）。

---

## selectFunctionsToPrint 函数分析

### 函数签名与目的（L3665-3667）

```cpp
void RewriteInstance::selectFunctionsToPrint();
```

**功能**: 装载 `-print-only`（`-print-only-file`）的函数名单。

### 整体结构

一行：`populateFunctionNames(opts::PrintOnlyFile, opts::PrintOnly);`

### 逐段注释

无。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::PrintOnly` | cl::list | `BinaryFunction::print` 的过滤名单 |

### 优化意图

1. 打印过滤与处理过滤分离：调试时想看 ignored 函数的场景很常见。

### 约束与易错点

无。

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## selectFunctionsToProcess 函数分析

### 函数签名与目的（L3669-3861）

```cpp
void RewriteInstance::selectFunctionsToProcess();
```

**功能**: 决定每个函数"处理 or 跳过"（`setIgnored`），实现 `-funcs/-skip-funcs/-lite/-max-funcs/function-order` 组合语义。

### 整体结构

```text
selectFunctionsToProcess()
├── 1. 装载三个文件选项
├── 2. funcs 与 skip-funcs 同用 → exit(1)
├── 3. lite 阈值: LiteThresholdPct 按有 profile 函数的执行计数排序取分位
│                     LiteThresholdCount 绝对下限（两者取 max）
├── 4. [RT_USER] 读 function-order 文件 → UserSet/LTOCommonSet
├── 5. mustSkip / shouldProcess lambda:
│      veneer 总是处理；名单模式只处理名单内；
│      lite: order 集合强制含、无 profile 跳过、计数 < 阈值跳过
├── 6. 一轮遍历: pseudo → IsIgnored + HasExternalRefRelocations
│          fragment 暂缓；!shouldProcess → setIgnored
├── 7. 静态二进制 _init workaround
└── 8. fragment 二轮协调:
       fragment 必 skip → 全部 parent 一起 skip（计数修正）
       任一 parent ignored → fragment ignored
       否则 fragment 随 parent 处理
```

### 逐段注释

**1. lite 阈值分位计算 (L3688-3710)**

```cpp
std::vector<const BinaryFunction *> TopFunctions;
for (auto &BFI : BC->getBinaryFunctions()) {
  const BinaryFunction &Function = BFI.second;
  if (ProfileReader->mayHaveProfileData(Function))
    TopFunctions.push_back(&Function);
}
llvm::sort(TopFunctions, [](const BinaryFunction *A, const BinaryFunction *B) {
  return A->getKnownExecutionCount() < B->getKnownExecutionCount();
});
size_t Index = TopFunctions.size() * opts::LiteThresholdPct / 100;
if (Index)
  --Index;
LiteThresholdExecCount = TopFunctions[Index]->getKnownExecutionCount();
```

`mayHaveProfileData` 先过滤再排序（省排序量）；`-lite-threshold-pct 90` = 只处理 top 10% 热度函数。

**2. fragment 联动 (L3824-3860)**

```cpp
if (mustSkip(Function)) {
  for (BinaryFunction *Parent : Function.ParentFragments) {
    ...
    Parent->setIgnored();
    --NumFunctionsToProcess;
  }
  Function.setIgnored();
  continue;
}
bool IgnoredParent =
    llvm::any_of(Function.ParentFragments, [&](BinaryFunction *Parent) {
      return Parent->isIgnored();
    });
```

"parent 优化而 cold 片段被忽略"会产生悬空引用——双向联动保证一致。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::Lite/LiteThresholdPct/MaxFunctions` | 过滤开关 | 第三章 `adjustCommandLineOptions` 可能自动改写 |
| `ReorderFunctionsUserSet` | StringSet | `-function-order` 强制包含集 |

### 优化意图

1. veneer 强制处理：BOLT 要把 veneer 调用替换为直接调用，veneer 函数本身必须进流水线。
2. `-max-funcs` 到量时打印"processing ending on"——帮助用户确定截断点。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| fragment 与 parent 必须同去留 | 二轮协调 | 悬空 fragment 引用 |
| pseudo 强制 HasExternalRefRelocations | 外部可能引用 PLT 桩 | 漏标导致重定位丢失 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| profile 存在性 | `ProfileReader::mayHaveProfileData` | `bolt/Profile/ProfileReader.h` |
| LTO 公共名 | `getLTOCommonName` | `bolt/Rewrite/RewriteInstance.h` |

### 其他补充

`getLTOCommonName`：LTO 内部名（`.llvm.<hash>` 后缀）归一化，让 order 文件可用"去后缀名"匹配。

---

## opts::shouldDumpDot 函数分析

### 函数签名与目的（L136-156）

```cpp
bool shouldDumpDot(const bolt::BinaryFunction &Function);
```

**功能**: 判断函数是否要 dump CFG dot 图（`-dump-dot-all` / `-dump-dot-func`）。

### 整体结构

```text
shouldDumpDot(Function)
├── DumpDotAll → !Function.isIgnored()
├── DumpDotFunc 为空 → false
├── isIgnored → false
└── 任一名字模式 hasNameRegex 命中 → true
```

### 逐段注释

无复杂分段。`DumpDotFunc` 支持正则（`-dump-dot-func='foo.*,bar'`）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `DumpDotAll/DumpDotFunc`（本文件 cl::opt） | 开关/名单 | `dumpGraphForPass` 前的过滤器 |

### 优化意图

1. 全量 dump 大二进制会产生数万文件，名单模式是调试单个函数的实用开关。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 名字匹配 | `BinaryFunction::hasNameRegex` | `bolt/Core/BinaryFunction.cpp` |

### 其他补充

消费点：`postProcessFunctions`（L4200）等各阶段的 `Function.dumpGraphForPass` 调用前。

---

# 十、反汇编与 CFG 构建

## shouldDisassemble 函数分析

### 函数签名与目的（L517-532，静态）

```cpp
static bool shouldDisassemble(const BinaryFunction &BF);
```

**功能**: 反汇编/建 CFG 循环的统一过滤器。

### 整体结构

```text
shouldDisassemble(BF)
├── BC.usesBTI() && BF.isPLTFunction() → true
├── BF.isPseudo() → false
├── opts::processAllFunctions() → true
└── !BF.isIgnored()
```

### 逐段注释

无复杂分段。BTI 二进制的 PLT 也要反汇编：LongJmp pass 目标 PLT 时需判断是否要加 landing pad（BTI 要求间接跳转目标必须是 `bti` 指令）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction` 的 Ignored/Pseudo 标志 | bool | 过滤依据 |

### 优化意图

1. 一个谓词统一三处循环（disassembleFunctions/buildFunctionsCFG/CFI 轮）的过滤口径，避免口径漂移。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| pseudo 默认不反汇编 | BTI 场景例外 | 例外漏掉会产生非法 BTI 跳转 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| BTI 检测 | `BC.usesBTI` | `bolt/Core/BinaryContext.h` |

### 其他补充

无。

---

## readDebugInfo 函数分析

### 函数签名与目的（L3863-3878）

```cpp
void RewriteInstance::readDebugInfo();
```

**功能**: `--update-debug-sections` 时预处理 DWARF（词法作用域边界收集、DWO 释放）。

### 整体结构

```text
readDebugInfo()
├── 1. 非 UpdateDebugSections → 返回
├── 2. BC->preprocessDebugInfo()
├── 3. [AccurateDebugRanges] BC->collectDebugScopeBoundaries()
└── 4. BC->releaseAllDWOContexts()
```

### 逐段注释

无复杂分段。`-accurate-debug-ranges`（默认开）追踪词法作用域边界，把 scope range 精确翻译到新地址；关闭可换取更低内存/时间（退化为输入相对块偏移，见选项描述 L274-280）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC` 的 DWARF 预处理产物 | scope 边界表 | `DWARFRewriter` 消费 |

### 优化意图

1. DWO（split dwarf）上下文在此释放：后续不再需要 .dwo 内容，及早回收。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在反汇编前调用 | 块偏移翻译依赖 | run() 顺序保证 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| DWARF 预处理 | `BC->preprocessDebugInfo` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

## disassembleFunctions 函数分析

### 函数签名与目的（L4001-4133）

```cpp
void RewriteInstance::disassembleFunctions();
```

**功能**: 三段式反汇编主流程：逐函数反汇编 → 跳转表/引用后处理 → CFI/LSDA 填充。

### 整体结构

```text
disassembleFunctions()
├── 第一轮: 逐函数
│    ├── getData() 失败 → exit(1)
│    ├── size==0 → setSimple(false) 继续
│    ├── setFileOffset(数据指针-文件头)
│    ├── !shouldDisassemble → scanExternalRefs() + setSimple(false)
│    ├── Function.disassemble() 错误处理:
│    │    fatal / processAllFunctions → exit(1)
│    │    否则 scanExternalRefs + setIgnored（降级容忍）
│    └── [-print-disasm] 打印
├── BC->processInterproceduralReferences()
├── BC->populateJumpTables()          // 消化 addPCRelativeDataRelocation 占位
├── 第二轮: validateInternalBranches / postProcessEntryPoints / postProcessJumpTables
├── BC->clearJumpTableTempData()
├── BC->adjustCodePadding()
├── 第三轮: [shouldDisassemble 且 simple]
│    ├── CFI: !fillCFIInfoFor → reloc mode exit(1) / 否则 setSimple(false)
│    ├── containedNegateRAState 检查
│    └── LSDA 解析（非 FragmentsToSkip）
└── CFIRdWrt->releaseFrameData()
```

### 逐段注释

**1. ignored 函数的外部引用扫描 (L4025-4031)**

```cpp
if (!shouldDisassemble(Function)) {
  NamedRegionTimer T("scan", "scan functions", "buildfuncs",
                     "Scan Binary Functions", opts::TimeBuild);
  Function.scanExternalRefs();
  Function.setSimple(false);
  continue;
}
```

不能完整反汇编的函数退化为"只记录外部引用"——其他函数对它符号的引用仍可重定位，这是**降级不降正确性**的关键设计。

**2. 跳转表语义恢复的时机 (L4062-4063)**

```cpp
BC->processInterproceduralReferences();
BC->populateJumpTables();
```

`populateJumpTables` 需要第一轮的指令流（找 jump table 寄存器、扫描表内容），产物又被第二轮 `postProcessJumpTables` 消费——两轮之间是它唯一的正确位置。`handleRelocation` 留下的 `addPCRelativeDataRelocation` 占位在此消化。

**3. CFI 的模式差异 (L4093-4104)**

```cpp
if (!Function.trapsOnEntry() && !CFIRdWrt->fillCFIInfoFor(Function)) {
  if (BC->HasRelocations) {
    BC->errs() << BC->generateBugReportMessage("unable to fill CFI.", Function);
    exit(1);
  } else {
    BC->errs() << "BOLT-WARNING: unable to fill CFI for function " ...;
    Function.setSimple(false);
    continue;
  }
}
```

relocation mode 下 CFI 失败是致命的（函数移动后无 unwind 会破坏异常/栈回溯）；非 reloc 模式可原地不动，降级跳过即可。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `CFIReaderWriter::fillCFIInfoFor` | 按需 CFI 解析 | 第三章 FDE-only 解析的延迟兑现点 |
| `BinaryFunction` 状态机 | Empty→Disassembled→CFG | `trapsOnEntry` 等判定 |

### 优化意图

1. **内存**：`releaseFrameData()` 在三轮结束后释放 FDE 索引与已解析 CFI——unwind 数据使命结束（发射时会重新生成）。
2. `NegateRAState` 检查：AArch64 PAC 的 `.cfi_negate_ra_state` 一旦被 BOLT 移动，必须 `--update-branch-protection` 配套，否则签名状态错乱。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| populateJumpTables 必须在两轮之间 | 数据依赖 | 错位 → 跳转表识别失败 |
| reloc mode CFI 失败即 exit | 正确性优先 | 降级会生成破坏 unwind 的输出 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 反汇编 | `BinaryFunction::disassemble` | `bolt/Core/BinaryFunction.cpp` |
| 跳转表恢复 | `BC->populateJumpTables` | `bolt/Core/BinaryContext.cpp` |
| 外部引用扫描 | `BinaryFunction::scanExternalRefs` | 同上 |

### 其他补充

`generateBugReportMessage` 会附上复现命令与函数名，用户可直接提 issue。

---

## buildFunctionsCFG 函数分析

### 函数签名与目的（L4135-4173）

```cpp
void RewriteInstance::buildFunctionsCFG();
```

**功能**: **并行**为所有 simple 函数构建 CFG。

### 整体结构

```text
buildFunctionsCFG()
├── 1. 预创建注解索引（JTIndexReg/NOP）→ 并行无锁
├── 2. WorkFun: BF.buildCFG(AllocId)，错误 handleAllErrors
├── 3. SkipPredicate: !shouldDisassemble || !isSimple
├── 4. runOnEachFunctionWithUniqueAllocId(SP_INST_LINEAR, ...)
│      ForceSequential = SequentialDisassembly || PrintAll
└── 5. BC->postProcessSymbolTable()
```

### 逐段注释

**1. 注解索引预创建 (L4139-4141)**

```cpp
BC->MIB->getOrCreateAnnotationIndex("JTIndexReg");
BC->MIB->getOrCreateAnnotationIndex("NOP");
```

`getOrCreateAnnotationIndex` 首次调用会写全局索引表——多线程并发首调是数据竞争。主线程预创建后，工作线程的查询变只读。

**2. 并行骨架 (L4143-4170)**

```cpp
ParallelUtilities::WorkFuncWithAllocTy WorkFun =
    [&](BinaryFunction &BF, MCPlusBuilder::AllocatorIdTy AllocId) {
      bool HadErrors{false};
      handleAllErrors(BF.buildCFG(AllocId), [&](const BOLTError &E) {
        if (!E.getMessage().empty())
          E.log(BC->errs());
        if (E.isFatal())
          exit(1);
        HadErrors = true;
      });
      ...
    };
ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
    *BC, ParallelUtilities::SchedulingPolicy::SP_INST_LINEAR, WorkFun,
    SkipPredicate, "disassembleFunctions-buildCFG",
    /*ForceSequential*/ opts::SequentialDisassembly || opts::PrintAll);
```

每线程独立 `AllocatorIdTy`：MCInst 注解内存按分配器隔离，免锁释放。`SP_INST_LINEAR` 按指令数线性调度负载均衡。`-print-all` 强制串行——打印保序。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ParallelUtilities` | 调度策略/分配器 id | `bolt/Core/ParallelUtilities.h` |
| `BOLTError` | 可恢复/fatal 双态 | `bolt/Core/BinaryFunction.h` |

### 优化意图

1. **注解索引预创建 + 独立分配器**：BOLT 并行化的两个标准手法，本函数是教科书示例。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 并发首调注解索引 = 数据竞争 | 预创建必须先行 | 新增注解类型时易忘 |
| PrintAll 串行 | 输出交错不可读 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 并行调度 | `ParallelUtilities::runOnEachFunctionWithUniqueAllocId` | `bolt/lib/Core/ParallelUtilities.cpp` |
| CFG 构建 | `BinaryFunction::buildCFG` | `bolt/lib/Core/BinaryFunction.cpp` |

### 其他补充

无。

---

## postProcessFunctions 函数分析

### 函数签名与目的（L4175-4216）

```cpp
void RewriteInstance::postProcessFunctions();
```

**功能**: CFG 完成后的全局收尾：fragment 标记、岛内重定位降级、postProcessCFG、统计汇总。

### 整体结构

```text
postProcessFunctions()
├── 1. BC->skipMarkedFragments() / clearFragmentsToSkip()
├── 2. TotalScore/SumExecutionCount 清零
├── 3. 逐函数:
│      岛内动态重定位 → setSimple(false)（不支持分裂优化）
│      empty 跳过
│      postProcessCFG()
│      [-print-cfg] 打印 / [shouldDumpDot] dump / [PrintLoopInfo] 循环信息
│      累计 TotalScore/SumExecutionCount
└── 4. [-print-globals] printGlobalSymbols
```

### 逐段注释

**1. fragment 延迟标记 (L4178-4179)**

```cpp
// We mark fragments as non-simple here, not during disassembly,
// So we can build their CFGs.
BC->skipMarkedFragments();
BC->clearFragmentsToSkip();
```

标记为 skip 的 fragment 也要建 CFG（LSDA/边界信息需要），故 non-simple 标记推迟到此刻。

**2. 岛内动态重定位降级 (L4189-4190)**

```cpp
if (Function.hasDynamicRelocationAtIsland())
  Function.setSimple(false);
```

常量岛含动态重定位的函数不能做 hot/cold 分裂（岛必须与代码同址移动），降级为不优化。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->TotalScore/SumExecutionCount` | 汇总 | 覆写覆盖率统计（`rewriteFunctionsInPlace` 输出）与收益报告 |

### 优化意图

1. `postProcessCFG` 统一计算块地址/大小与 final layout，为优化 pass 提供一致的地址视图。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| fragment 的 non-simple 必须在建 CFG 后 | 注释明示 | 提前标记会丢 unwind/边界数据 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| CFG 收尾 | `BinaryFunction::postProcessCFG` | `bolt/lib/Core/BinaryFunction.cpp` |
| 循环信息 | `BinaryFunction::calculateLoopInfo` | 同上 |

### 其他补充

无。

---

# 十一、Metadata 与优化 Pass

## processMetadataPreCFG 函数分析

### 函数签名与目的（L3934-3940）

```cpp
void RewriteInstance::processMetadataPreCFG();
```

**功能**: 触发 metadata rewriter 的 PreCFG 钩子，随后读取 pre-CFG profile。

### 整体结构

```text
processMetadataPreCFG()
├── MetadataManager.runInitializersPreCFG()
└── processProfileDataPreCFG()
```

### 逐段注释

两行调度（带计时器），无代码片段。

### 关键数据结构

同 `initializeMetadataManager`（第三章）。

### 优化意图

1. rewriter 的 PreCFG 钩子在 profile 读取前执行——某些 rewriter（如 Linux kernel）会改写函数属性影响 profile 匹配。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PreCFG 钩子 | `MetadataManager::runInitializersPreCFG` | `bolt/lib/Rewrite/MetadataManager.cpp` |

### 其他补充

无。

---

## processMetadataPostCFG 函数分析

### 函数签名与目的（L3942-3946）

```cpp
void RewriteInstance::processMetadataPostCFG();
```

**功能**: 触发 metadata rewriter 的 PostCFG 钩子（此时全部 simple 函数有 CFG）。

### 整体结构

一行：`MetadataManager.runInitializersPostCFG();`（带计时器）。

### 逐段注释

无。

### 关键数据结构

同上。

### 优化意图

1. 需要"函数内指令级视图"的 rewriter（如 RSeq 找临界区指令序列）在此运行。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PostCFG 钩子 | `MetadataManager::runInitializersPostCFG` | `bolt/lib/Rewrite/MetadataManager.cpp` |

### 其他补充

无。

---

## finalizeMetadataPreEmit 函数分析

### 函数签名与目的（L4402-4406）

```cpp
void RewriteInstance::finalizeMetadataPreEmit();
```

**功能**: 触发 metadata rewriter 的 PreEmit 钩子（优化完成后、发射前）。

### 整体结构

一行：`MetadataManager.runFinalizersPreEmit();`（带计时器）。

### 逐段注释

无。

### 关键数据结构

同上。

### 优化意图

1. 需要最终（优化后）CFG 但不依赖输出地址的元数据变换在此收尾。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PreEmit 钩子 | `MetadataManager::runFinalizersPreEmit` | `bolt/lib/Rewrite/MetadataManager.cpp` |

### 其他补充

无。

---

## updateMetadata 函数分析

### 函数签名与目的（L4408-4421）

```cpp
void RewriteInstance::updateMetadata();
```

**功能**: 发射链接完成后：AfterEmit 钩子、DWARF 更新、bolt info section。

### 整体结构

```text
updateMetadata()
├── 1. MetadataManager.runFinalizersAfterEmit()
├── 2. [UpdateDebugSections] DebugInfoRewriter->updateDebugInfo()
└── 3. [WriteBoltInfoSection] addBoltInfoSection()
```

### 逐段注释

无复杂分段。此时全部输出地址已定（`updateOutputValues` 完成），`DWARFRewriter` 才能翻译 line table / ranges / loclists 里的地址。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `DWARFRewriter`（成员） | — | `bolt/lib/Rewrite/DWARFRewriter.cpp`，DWARF 全量地址重写 |

### 优化意图

1. 四时机设计（Section/PreCFG/PostCFG/PreEmit/AfterEmit）让"信息需求"与"执行时机"精确对齐。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在 emitAndLink 之后 | 依赖输出地址 | run() 顺序保证 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| DWARF 更新 | `DWARFRewriter::updateDebugInfo` | `bolt/lib/Rewrite/DWARFRewriter.cpp` |

### 其他补充

无。

---

## runOptimizationPasses 函数分析

### 函数签名与目的（L4218-4222）

```cpp
void RewriteInstance::runOptimizationPasses();
```

**功能**: BOLT 优化 pass 流水线的驱动入口。

### 整体结构

一行：`BC->logBOLTErrorsAndQuitOnFatal(BinaryFunctionPassManager::runAllPasses(*BC));`

### 逐段注释

无。全部 pass（重排、分裂、ICF、LongJmp、frame 优化等）的注册与执行在 `BinaryPassManager.cpp`。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunctionPassManager` | pass 列表 | `bolt/Rewrite/BinaryPassManager.h` |

### 优化意图

1. 本文件只做驱动：pass 体系独立成文件，便于单独演进。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 全部 pass | `BinaryFunctionPassManager::runAllPasses` | `bolt/lib/Rewrite/BinaryPassManager.cpp` |

### 其他补充

无。

---

## runBinaryAnalyses 函数分析

### 函数签名与目的（L4224-4267）

```cpp
void RewriteInstance::runBinaryAnalyses();
```

**功能**: `-scanners` 分析模式（非优化）的执行与退出：目前实现 AArch64 PAuth gadget 扫描。

### 整体结构

```text
runBinaryAnalyses()
├── 1. 统计无 CFG 函数并 WARNING（ignored/pseudo 除外）
├── 2. GadgetScannersToRun 位掩码聚合（空则默认 all）
├── 3. PtrAuth 位非零 → 注册 PAuthGadgetScanner::Analysis pass
└── 4. Manager.runPasses()
```

### 逐段注释

**1. 精度告警 (L4232-4248)**

```cpp
unsigned NoCFGCount = 0;
for (const auto &BFI : BC->getBinaryFunctions()) {
  const BinaryFunction &BF = BFI.second;
  if (BF.isIgnored() || BF.hasCFG())
    continue;
  ++NoCFGCount;
  ...
}
if (NoCFGCount)
  BC->errs() << "BOLT-WARNING: " << NoCFGCount
             << " function(s) lack CFG; binary-analysis results may be"
                " incomplete. ...";
```

无 CFG 的函数上 gadget 扫描既有假阴性也有假阳性，明确告知用户精度受限。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::GadgetKindBitmask` | 位掩码 | 五类 PAuth gadget + all（L305-322 枚举） |

### 优化意图

1. 分析模式与优化模式共用基础设施（CFG/pass 框架），但输出是报告而非二进制。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| run() 中分析后立即 return | 不进入 emit | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| PAuth 扫描 | `PAuthGadgetScanner::Analysis` | `bolt/lib/Passes/PAuthGadgetScanner.cpp` |

### 其他补充

无。

---

## preregisterSections 函数分析

### 函数签名与目的（L4269-4287）

```cpp
void RewriteInstance::preregisterSections();
```

**功能**: emit 前预注册输出 section，固定其在 `BC->sections()` 中的登记顺序。

### 整体结构

```text
preregisterSections()
├── 新 .eh_frame（NewSecPrefix）+ .relocated.eh_frame（原内容副本）
├── 新 .gcc_except_table
├── 新 .rodata
└── 新 .rodata.cold
```

### 逐段注释

无复杂分段。`.relocated.eh_frame` 用 `BC->registerSection(name, 原section)` 整体拷贝注册——`relocateEHFrameSection` 稍后为它补重定位，使其能随 JITLink 整体搬移。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinarySection` 输出顺序 | 登记序 | 决定最终 section 布局顺序 |

### 优化意图

1. **顺序敏感**：`.eh_frame` 必须先于 `.gcc_except_table`/`.rodata` 登记，输出布局才能保持 unwind 数据紧跟代码的局部性。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 调用时机在 runOptimizationPasses 之后 | 优化可能注册新 section | 早调用顺序被破坏 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| section 注册 | `BC->registerOrUpdateSection` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

# 十二、发射与链接（emitAndLink）

## relocateEHFrameSection 函数分析

### 函数签名与目的（L2396-2442）

```cpp
void RewriteInstance::relocateEHFrameSection();
```

**功能**: 把**原 `.eh_frame`** 变成可随链接重定位的副本 `.relocated.eh_frame`。

### 整体结构

```text
relocateEHFrameSection()
├── 1. assert 预注册检查（EHFrameSection / .relocated 副本）
├── 2. createReloc lambda:
│      DW_EH_PE_omit → 跳过
│      非 pcrel/textrel/funcrel/datarel → 跳过
│      sdata4/udata4 → PC32，Offset -= 4
│      sdata8/udata8 → PC64，Offset -= 8
│      → RelocatedEHFrameSection->addRelocation(Offset, nullptr, RelType, Value)
└── 3. EHFrameParser::parse(DE, 地址, createReloc)
```

### 逐段注释

**1. 对绝对值建重定位 (L2434-2437)**

```cpp
// Create a relocation against an absolute value since the goal is to
// preserve the contents of the section independent of the new values
// of referenced symbols.
RelocatedEHFrameSection->addRelocation(Offset, nullptr, RelType, Value);
```

目标是"内容不变地搬走整个 section"：对 unwind 表内部的每个引用位置建"绝对值重定位"（Symbol 为空，Addend=原值），链接器搬移时按新位置重算 PC 相对编码，但引用的目标地址保持原值——旧 FDE 的语义原样保留。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `EHFrameParser` | unwind 编码解析 | `bolt/Core/Exceptions.h`，识别 DW_EH_PE_* 寻址模式 |

### 优化意图

1. 保留旧 unwind 表而非全部重建：未优化函数的 FDE 无需重生成，直接搬移；新表（优化函数）由发射器另出。两表最终在 `writeEHFrameHeader` 合并。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只支持相对编码 | omit/abs 编码跳过 | 绝对编码的表搬移后失效（极少见） |
| 偏移回退量按宽度 | 4/8 字节 | 错配会把重定位挂错位置 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 重定位登记 | `BinarySection::addRelocation` | `bolt/Core/BinarySection.h` |
| 编码解析 | `EHFrameParser::parse` | `bolt/lib/Core/Exceptions.cpp` |

### 其他补充

`-use-old-text`/`-strict` 模式不走此函数（`emitAndLink` L4302-4305 改为 `clearContents` 从头重建）。

---

## emitAndLink 函数分析

### 函数签名与目的（L4289-4400）

```cpp
void RewriteInstance::emitAndLink();
```

**功能**: 把优化后的全部函数发射成内存 `.o`，经 JITLink 链接得到最终地址。

### 整体结构

```text
emitAndLink()
├── 1. ObjectBuffer + BC->createStreamer(OS)
├── 2. [.eh_frame] UseOldText/Strict → clearContents()
│                 否则 → relocateEHFrameSection()
├── 3. emitBinaryContext(*Streamer, *BC, getOrgSecPrefix())
├── 4. Streamer->finish() + 错误检查
├── 5. [-keep-tmp] 中间 .o 落盘（调试）
├── 6. [reloc mode] 原 .text 改名 .bolt.org.text（让位）
├── 7. MemoryBuffer 包装 + ExecutableFileMemoryManager
├── 8. JITLinkLinker->loadObject(obj, mapFileSections 回调)
│      └── mapFileSections(): 全部输出地址分配在此发生
├── 9. updateOutputValues(Linker)
├── 10. [UpdateDebugSections] updateLineTableOffsets(Assembler)
├── 11. [RuntimeLibrary] link() → 回调 mapAllocatableSections()
└── 12. 收尾: 发射期临时 section 还原 origin 名，
        [-print-cache-metrics] CacheMetrics::printAll
```

### 逐段注释

**1. 发射与流 (L4293-4318)**

```cpp
SmallString<0> ObjectBuffer;
raw_svector_ostream OS(ObjectBuffer);
std::unique_ptr<MCStreamer> Streamer = BC->createStreamer(OS);
...
emitBinaryContext(*Streamer, *BC, getOrgSecPrefix());
Streamer->finish();
if (Streamer->getContext().hadError()) {
  BC->errs() << "BOLT-ERROR: Emission failed.\n";
  exit(1);
}
```

"中间 .o"完全在内存（`SmallString<0>`），不落盘（除非 `-keep-tmp`）。`emitBinaryContext`（`BinaryEmitter.cpp`）逐函数发射指令、注解、CFI 与重定位，产出标准 ELF relocatable object。

**2. JITLink 装载 (L4344-4357)**

```cpp
auto EFMM = std::make_unique<ExecutableFileMemoryManager>(*BC);
EFMM->setNewSecPrefix(getNewSecPrefix());
EFMM->setOrgSecPrefix(getOrgSecPrefix());
Linker = std::make_unique<JITLinkLinker>(*BC, std::move(EFMM));
Linker->loadObject(ObjectMemBuffer->getMemBufferRef(),
                   [this](auto MapSection) { mapFileSections(MapSection); });
updateOutputValues(*Linker);
```

`ExecutableFileMemoryManager` 按前缀过滤：`.bolt.new` 前缀 section 分配新内存、`.bolt.org` 前缀映射回原地址。`loadObject` 过程中 JITLink 为每个 section 请求定址 → `mapFileSections` 回调（第十二章 §mapFileSections）完成 BOLT 自主的布局决策。随后 `updateOutputValues` 把 JITLink 消解出的符号地址回填进每个 `BinaryFunction`。

**3. section 名还原 (L4374-4394)**

```cpp
for (BinaryFunction *Function : BC->getAllBinaryFunctions()) {
  ErrorOr<BinarySection &> Section = Function->getCodeSection();
  if (Section && (Function->getImageAddress() == 0 || Function->getImageSize() == 0))
    continue;
  if (Section)
    BC->deregisterSection(*Section);
  assert(Function->getOriginSectionName() && "expected origin section");
  Function->CodeSectionName = Function->getOriginSectionName()->str();
  ...
  if (Function->getLayout().isSplit())
    Function->setColdCodeSectionName(getBOLTTextSectionName());
}
```

发射期函数住在临时 section（`.text.01.foo` 等），链接完成后 de-register 并把 `CodeSectionName` 还原为 origin——后续所有查询（`getCodeSection()->getIndex()` 等）都基于 origin 语义。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `JITLinkLinker`（成员 Linker） | 符号表/section 定址 | `bolt/lib/Rewrite/JITLinkLinker.cpp` |
| `ExecutableFileMemoryManager` | 前缀过滤分配 | `bolt/lib/Rewrite/ExecutableFileMemoryManager.cpp` |
| `StartLinkingRuntimeLib`（成员） | bool | 运行库链接轮次标志（§mapAllocatableSections 消费） |

### 优化意图

1. **内存 .o + JITLink**：避免调用外部链接器（速度 + 可控布局）；BOLT 对"section 放哪"有全局最优诉求（hugify 对齐、hot text 分段），只有自定义 SectionMapper 能满足。
2. 运行库二次 `link()` 走独立回调：运行库 section 是链接时才注册的，需要第二轮 `mapAllocatableSections`。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| mapFileSections 只在 loadObject 期间有效 | 回调时序由 JITLink 控制 | 回调外调用 MapSection 无效 |
| updateOutputValues 必须先于所有补丁 | 地址翻译依赖它 | run() 顺序保证 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 发射 | `emitBinaryContext` | `bolt/lib/Core/BinaryEmitter.cpp` |
| 流创建 | `BC->createStreamer` | `bolt/lib/Core/BinaryContext.cpp` |
| 链接 | `JITLinkLinker::loadObject` | `bolt/lib/Rewrite/JITLinkLinker.cpp` |

### 其他补充

`-print-cache-metrics` 输出布局后的 cache 指标（`CacheMetrics::printAll`），评估重排效果。

---

## mapFileSections 函数分析

### 函数签名与目的（L4423-4457）

```cpp
void RewriteInstance::mapFileSections(BOLTLinker::SectionMapper MapSection);
```

**功能**: JITLink `loadObject` 的回调入口：完成全部输出地址分配。

### 整体结构

```text
mapFileSections(MapSection)
├── 1. BC->deregisterUnusedSections()
├── 2. 无新 .eh_frame → .relocated.eh_frame 映射到 NextAvailableAddress 并 de-register
├── 3. mapCodeSections(MapSection)
├── 4. mapAllocatableSections(MapSection)
└── 5. [BOLTReserved 非空] 容量校验，超容 exit(1)
```

### 逐段注释

**1. relocated .eh_frame 占位 (L4427-4439)**

```cpp
BinarySection *RelocatedEHFrameSection = getSection(".relocated" + ...);
if (RelocatedEHFrameSection && RelocatedEHFrameSection->hasValidSectionID()) {
  BinarySection *NewEHFrameSection = getSection(getNewSecPrefix() + ...);
  if (!NewEHFrameSection || !NewEHFrameSection->isFinalized()) {
    MapSection(*RelocatedEHFrameSection, NextAvailableAddress);
    BC->deregisterSection(*RelocatedEHFrameSection);
  }
}
```

没有新 `.eh_frame` 写入时（全部函数都不需要新 unwind），relocated 副本仍会被 JITLink 处理重定位——映射到无害地址再 de-register，既不失败也不进输出。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BOLTLinker::SectionMapper` | 函数对象 | `section → 输出地址` 的注册接口 |

### 优化意图

1. 先代码后数据的分配序在两个子函数间闭环（代码地址影响数据段的起始对齐）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 预留区容量校验在最后 | 全部分配完成后 | 超容 exit(1) 而非静默覆盖 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 未用 section 清理 | `BC->deregisterUnusedSections` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

## CodeSectionOrder 函数分析（匿名命名空间类，L4462-4544）

### 函数签名与目的

```cpp
class CodeSectionOrder {
public:
  bool operator()(StringRef AName, StringRef BName) const;
private:
  SectionKind getKind(StringRef Name) const;
  unsigned getRank(SectionKind Kind) const;
};
```

**功能**: 定义输出 `.text` 区域内代码 section 的严格弱序（比较器类）。

### 整体结构

```text
operator()(AName, BName)
├── getKind: Mover(.mover) / Main(.text) / Warm(.text.warm) / Cold(.text.cold*) / Other
├── getRank: 普通序 Mover(0)<Main(1)<Warm(2)<Cold(3)<Other(4)
│            HotFunctionsAtEnd 反转: Other(1)<Cold(2)<Warm(3)<Main(4)
├── rank 不同 → 按 rank
└── rank 相同且都是 Cold → 按名字（长度/字典序，方向随 HotFunctionsAtEnd）
```

### 逐段注释

**1. Cold 内部排序 (L4480-4486)**

```cpp
if (AKind == SectionKind::Cold) {
  if (AName.size() != BName.size())
    return HotFunctionsAtEnd ? AName.size() > BName.size()
                             : AName.size() < BName.size();
  if (AName != BName)
    return HotFunctionsAtEnd ? AName > BName : AName < BName;
}
```

BOLT 的 cold section 名带序号后缀（`.cold.1`/`.cold.2`...）——先按长度（`cold.10` > `cold.9` 的字典序陷阱）再字典序，保证同一函数的多路 cold 片段保持与主函数相邻的相对顺序。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `opts::HotText/HotFunctionsAtEnd` | 布尔配置 | 排序方向控制 |

### 优化意图

1. **热代码在前**（普通序）服务 icache/大页局部性；`--hot-functions-at-end` 反转支持"热代码贴近数据段"的布局实验。
2. Mover 最前：`.mover` 是 hugify 的搬移桩，必须在任何被搬代码之前执行到。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须是严格弱序 | stable_sort 前提 | 违反会触发未定义排序行为 |
| debug 断言联动 | getCodeSections 校验 section 序 == 函数序 | 排序逻辑改动需同步断言 |

### 关键 API / 源码路径

无。

### 其他补充

仅被 `getCodeSections` 使用。

---

## getCodeSections 函数分析

### 函数签名与目的（L4548-4588）

```cpp
std::vector<BinarySection *> RewriteInstance::getCodeSections();
```

**功能**: 收集所有已获 JITLink section ID 的 text section，按 `CodeSectionOrder` 排序返回。

### 整体结构

```text
getCodeSections()
├── 1. 收集 textSections() 中 hasValidSectionID 的
├── 2. CodeSectionOrder 构造 + stable_sort
├── 3. [NDEBUG 关闭] 逐 section setIndex
│      getOutputBinaryFunctions 顺序校验: section index 单调不减
└── 4. 返回
```

### 逐段注释

**1. 一致性断言 (L4565-4585)**

```cpp
#ifndef NDEBUG
uint32_t Index = 1;
for (BinarySection *Sec : CodeSections)
  Sec->setIndex(Index++);
uint32_t LastIndex = 0;
for (const BinaryFunction *BF : BC->getOutputBinaryFunctions()) {
  if (!BF->isEmitted() || BF->isPatch())
    continue;
  ErrorOr<BinarySection &> Sec = BF->getCodeSection();
  if (!Sec)
    continue;
  assert(Sec->getIndex() >= LastIndex &&
         "Section order does not match function order");
  LastIndex = Sec->getIndex();
}
#endif
```

section 排序与输出函数排序互为镜像——两套排序逻辑（此处与函数重排 pass）若不一致，debug 构建立即暴露。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->getOutputBinaryFunctions()` | 有序函数视图 | 输出顺序的权威来源 |

### 优化意图

1. debug 断言把"两个独立排序实现必须一致"变成机器检查的不变式。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 断言仅 debug | release 无保护 | 改排序要跑 debug 版验证 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| text section 迭代 | `BC->textSections` | `bolt/Core/BinaryContext.h` |

### 其他补充

无。

---

## mapCodeSections 函数分析

### 函数签名与目的（L4590-4724）

```cpp
void RewriteInstance::mapCodeSections(BOLTLinker::SectionMapper MapSection);
```

**功能**: 给代码 section 分配输出虚拟地址（relocation mode 主路径）。

### 整体结构

```text
mapCodeSections(MapSection)
├── 0. 非 reloc mode → mapCodeSectionsInPlace(MapSection) 后返回
├── 1. injected 函数: 预定地址直接映射
├── 2. getCodeSections()，剔除已分配（patch section）
├── 3. allocateAt lambda: 顺序分配 + 尾部大页/页对齐 padding
├── 4. allocateBefore lambda: 倒序塞进 [*, Address)
├── 5. [-use-old-text] 尝试原 .text:
│      HotFunctionsAtEnd → allocateBefore(text 末)
│      否则 allocateAt(text 头)
│      装不下 → WARNING + 回退常规
├── 6. 常规: NextAvailableAddress = allocateAt(NextAvailableAddress)
├── 7. 逐 section: MapSection + setOutputFileOffset
└── 8. [--merge-text-sections] mergeCodeSections()
```

### 逐段注释

**1. allocateAt 的 padding 逻辑 (L4625-4654)**

```cpp
auto allocateAt = [&](uint64_t Address) {
  const char *LastNonColdSectionName = BC->HasWarmSection
                                           ? BC->getWarmCodeSectionName()
                                           : BC->getMainCodeSectionName();
  for (BinarySection *Section : CodeSections) {
    Address = alignTo(Address, Section->getAlignment());
    Section->setOutputAddress(Address);
    Address += Section->getOutputSize();
    if (opts::Hugify && !BC->HasFixedLoadAddress &&
        Section->getName() == LastNonColdSectionName)
      Address = alignTo(Address, Section->getAlignment());
  }
  ErrorOr<BinarySection &> TextSection = BC->getUniqueSectionByName(LastNonColdSectionName);
  if (opts::HotText && TextSection && TextSection->hasValidSectionID()) {
    uint64_t HotTextEnd = TextSection->getOutputAddress() + TextSection->getOutputSize();
    HotTextEnd = alignTo(HotTextEnd, BC->PageAlign);
    if (HotTextEnd > Address) {
      PaddingSize = HotTextEnd - Address;
      Address = HotTextEnd;
    }
  }
  return Address;
};
```

两个 padding：hugify 在"最后一个非 cold section"之后再对齐一次（右侧预留，配合 discoverStorage 的左侧预留夹出完整 2MB 大页）；HotText 把主 text 段尾推到页边界（整段落进一个大页范围，供 `madvise(MADV_HUGEPAGE)`/`-hot-text` 生效）。

**2. use-old-text 回退 (L4670-4700)**

```cpp
if (opts::UseOldText) {
  ...
  const uint64_t CodeSize = EndAddress - StartAddress;
  if (CodeSize <= BC->OldTextSectionSize) {
    BC->outs() << "BOLT-INFO: using original .text for new code ...";
    AllocationDone = true;
  } else {
    BC->errs() << "BOLT-WARNING: --use-old-text failed. ...";
    opts::UseOldText = false;
  }
}
if (!AllocationDone)
  NextAvailableAddress = allocateAt(NextAvailableAddress);
```

新代码塞回原 `.text`（省文件膨胀）；装不下自动回退常规分配，`allocateBefore` 支持"从段尾倒塞"（`--hot-functions-at-end` 时热代码贴旧 text 末尾）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->OldTextSectionAddress/Size` | 旧 text 锚 | discoverStorage 记录 |
| `NextAvailableAddress`（成员） | 分配游标 | 本函数是主要推进者 |

### 优化意图

1. **padding 即正确性**：hugify 少一页对齐就 `madvise` 失败，hot text 跨页就白干——分配器必须理解微架构诉求。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| use-old-text 失败必须回退 | 不能半装 | 残留部分分配会双重占用 |
| patch section 预分配剔除 | 已有地址 | 重复映射越界 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 注入函数迭代 | `BC->getInjectedBinaryFunctions` | `bolt/Core/BinaryContext.h` |

### 其他补充

无。

---

## mergeCodeSections 函数分析

### 函数签名与目的（L4726-4767）

```cpp
void RewriteInstance::mergeCodeSections(
    const std::vector<BinarySection *> &CodeSections);
```

**功能**: `--merge-text-sections`：把全部代码 section 合并成单一 `.text`。

### 整体结构

```text
mergeCodeSections(CodeSections)
├── 1. 少于 2 个 → 返回
├── 2. Head = 最低地址 section；End = 最高地址 section 尾
├── 3. 记录 MergedTextMarkers（.bolt.pre_merge<名> + 地址）
├── 4. 其余 section setAnonymous(true) → MergedAwayTextSections
├── 5. Head 改名为主代码 section 名
└── 6. MergedTextSection/MergedTextSize 记录 + 信息打印
```

### 逐段注释

**1. 先记录后改名 (L4743-4747)**

```cpp
for (const BinarySection *Section : CodeSections)
  MergedTextMarkers.push_back(
      {(Twine(".bolt.pre_merge") + Section->getOutputName()).str(),
       Section->getOutputAddress()});
```

改名/匿名化之后原名字就没了——先固化 marker（名字+地址），`updateELFSymbolTable` 稍后把它们变成 0 尺寸本地符号，保留"合并前各 section 起点"的可恢复性（源码 L6059-6063 注释：sized 符号会干扰 STT_FUNC 的 symbolization）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `MergedTextSection/MergedTextSize`（成员） | 合并宿主与总大小 | `getOutputSections` 特判 sh_size |
| `MergedTextMarkers/MergedAwayTextSections`（成员） | marker/被并集合 | 符号表与 SHT 消费 |

### 优化意图

1. 合并减少 section header 数量（某些工具对海量 section 慢）；marker 保底符号化不丢失。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| marker size 必须为 0 | 防 symbolization 竞争 | sized NOTYPE 会抢 STT_FUNC |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## mapCodeSectionsInPlace 函数分析

### 函数签名与目的（L4769-4841）

```cpp
void RewriteInstance::mapCodeSectionsInPlace(
    BOLTLinker::SectionMapper MapSection);
```

**功能**: 非重定位模式的代码映射：函数不搬家，cold fragment 外移到新空间。

### 整体结构

```text
mapCodeSectionsInPlace(MapSection)
├── 1. 逐 emitted 函数:
│      FuncSection → OutputAddress = 原地址，MapSection
│      assert ImageSize <= MaxSize
│      split 函数: cold fragment 16B 对齐排进 NextAvailableAddress
│                  FF.setAddress/Image*/FileOffset，MapSection
├── 2. 注册聚合伪 section .bolt.text 覆盖全部新代码
└── 3. （无 merge）
```

### 逐段注释

**1. 原地不变式 (L4781-4790)**

```cpp
FuncSection->setOutputAddress(Function.getAddress());
MapSection(*FuncSection, Function.getAddress());
Function.setImageAddress(FuncSection->getAllocAddress());
Function.setImageSize(FuncSection->getOutputSize());
assert(Function.getImageSize() <= Function.getMaxSize() &&
       "Unexpected large function");
```

函数的输出地址 == 输入地址（原地）；`MaxSize` 是第七章 `adjustFunctionBoundaries` 算出的硬上限，此处 assert 兜底。

**2. cold 外移 (L4799-4818)**：唯一允许搬家的部分——`NextAvailableAddress`（文件尾新空间）顺序排布，最多两个 fragment（hot/cold，L4795-4797 assert）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `FunctionFragment` | Address/ImageAddress/ImageSize/FileOffset | cold 片段的四元组 |

### 优化意图

1. 非 reloc 模式下 PHDR 表不动（或 GnuStack 替换），新空间只用于 cold 与新数据——布局变化最小化。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 最多 hot/cold 两片 | assert | 三片布局在非 reloc 下无处安放 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 布局访问 | `Function->getLayout().getFragment` | `bolt/Core/FunctionLayout.h` |

### 其他补充

无。

---

## mapAllocatableSections 函数分析

### 函数签名与目的（L4843-4955）

```cpp
void RewriteInstance::mapAllocatableSections(
    BOLTLinker::SectionMapper MapSection);
```

**功能**: 非代码可分配 section（`.eh_frame`/`.rodata`/运行库/数据）的映射。

### 整体结构

```text
mapAllocatableSections(MapSection)
├── 1. [-use-old-text/-strict] tryRewriteSection:
│      新 .eh_frame / .gcc_except_table 装得进旧 section → 原地覆写
├── 2. 两轮循环: ST_READONLY → ST_READWRITE
│      READWRITE 前: 对齐 RegularPageSize，记 NewWritableSegmentAddress
│      逐 section: LinkOnly/无ID/已映射/类型不符 → 跳过
│        有 SectionRef → 原地址原偏移映射
│        新 section → 对齐（运行库首个用 RegularPageSize）→
│                     MapSection + setOutput* → 推进游标
│      记 NewTextSegmentSize / NewWritableSegmentSize
└── 3. 无 RW section 时回退 NextAvailableAddress
```

### 逐段注释

**1. 原地覆写 lambda (L4847-4865)**

```cpp
auto tryRewriteSection = [&](BinarySection &OldSection,
                             BinarySection &NewSection) {
  if (OldSection.getSize() < NewSection.getOutputSize())
    return;
  BC->outs() << "BOLT-INFO: rewriting " << OldSection.getName()
             << " in-place\n";
  NewSection.setOutputAddress(OldSection.getAddress());
  NewSection.setOutputFileOffset(OldSection.getInputFileOffset());
  MapSection(NewSection, OldSection.getAddress());
  NewSection.addPadding(OldSection.getSize() - NewSection.getOutputSize());
  OldSection.setAnonymous(true);
};
```

新内容装得进旧壳 → 原地覆写（尾部补零、旧 section 匿名化不出现在 SHT），文件不膨胀。

**2. RO 先 RW 后 (L4882-4954)**

```cpp
enum : uint8_t { ST_READONLY, ST_READWRITE };
for (uint8_t SType = ST_READONLY; SType <= ST_READWRITE; ++SType) {
  const uint64_t LastNextAvailableAddress = NextAvailableAddress;
  if (SType == ST_READWRITE) {
    NextAvailableAddress = alignTo(NextAvailableAddress, BC->RegularPageSize);
    NewWritableSegmentAddress = NextAvailableAddress;
  }
  ...
}
```

RO 紧贴代码段（可同段省 segment）；RW 必须独立段且页对齐——直接决定 `updateSegmentInfo` 生成的 PT_LOAD 数量。运行库首个 section 用 `RegularPageSize` 对齐（`StartLinkingRuntimeLib` 标志一次性消费），保证计数器段边界干净。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `NewWritableSegmentAddress/Size`（成员） | RW 段锚 | `updateSegmentInfo`/`patchELFPHDRTable` 消费 |

### 优化意图

1. **段拓扑最优化**：RO 并入 text 段省一个 PT_LOAD；RW 独立成段满足权限隔离。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 无 RW 时游标回退 | 避免虚增文件 | — |
| LinkOnly section 跳过 | 只参与链接 | 写出会产生悬空 header |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| padding 登记 | `BinarySection::addPadding` | `bolt/Core/BinarySection.h` |

### 其他补充

被 `mapFileSections` 与运行库 `link()` 回调两处调用。

---

## updateOutputValues 函数分析

### 函数签名与目的（L4957-4963）

```cpp
void RewriteInstance::updateOutputValues(const BOLTLinker &Linker);
```

**功能**: 发射后回填：解析 AddressMap + 每函数更新输出地址。

### 整体结构

```text
updateOutputValues(Linker)
├── 1. AddressMap::parse(*BC) → BC->setIOAddressMap
└── 2. 逐函数 Function->updateOutputValues(Linker)
```

### 逐段注释

无复杂分段。`AddressMap` 解析 emit 阶段产生的 `.bolt.address_map` 临时 section（符号 label→输入/输出地址），供 BAT 编码等消费；`BinaryFunction::updateOutputValues` 向 JITLink 查询函数各符号（入口/局部 label）新地址填进 `OutputAddress`/fragment 字段——**这是后续所有"旧→新"翻译的数据源**。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `AddressMap` | label→IO 地址 | `bolt/Core/AddressMap.h` |

### 优化意图

1. 一次性回填：避免散落在各补丁函数里的逐符号 JITLink 查询（每次查询有锁/哈希开销）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在 Linker 存活期内 | 查询接口依赖 | run() 顺序保证 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| IO 地址表 | `AddressMap::parse` | `bolt/lib/Core/AddressMap.cpp` |
| 函数更新 | `BinaryFunction::updateOutputValues` | `bolt/lib/Core/BinaryFunction.cpp` |

### 其他补充

无。

---

# 十三、输出文件重写（rewriteFile）

## rewriteFile 函数分析

### 函数签名与目的（L6806-6928）

```cpp
void RewriteInstance::rewriteFile();
```

**功能**: 输出总装：创建输出文件，按固定次序完成全部写盘与 ELF 补丁。

### 整体结构

```text
rewriteFile()
├── 1. ToolOutputFile 创建（Out 成员）
├── 2. OS << 输入数据 [0, FirstNonAllocatableOffset)   // 可分配区逐字节拷贝
├── 3. rewriteFunctionsInPlace(OS)                      // 非重定位模式覆写
├── 4. [-trap-old-code] 旧函数体逐个写 trap 指令
├── 5. 逐个写 finalized 且有数据的新 allocatable section
├── 6. 全部 section flushPendingRelocations(getNewValueForSymbol)
├── 7. [.eh_frame] writeEHFrameHeader()
├── 8. [-enable-bat] addBATSection()
├── 9. updateSegmentInfo() + patchELFPHDRTable()        // 非 kernel
├── 10. finalizeSectionStringTable()
├── 11. patchELFSymTabs()
├── 12. [-enable-bat] encodeBATSection()
├── 13. rewriteNoteSections()                           // 非分配 section
├── 14. [-use-old-text] zeroPaddingForReusedSections()
├── 15. [reloc mode] patchELFAllocatableRelaSections()
│                     patchELFAllocatableRelrSection()
│                     patchELFGOT()
├── 16. patchELFDynamic()
├── 17. patchELFSectionHeaderTable()
└── 18. 错误检查 + Out->keep() + 按 umask 设置权限
```

### 逐段注释

**1. 逐字节拷贝起家 (L6814-6815)**

```cpp
// Copy allocatable part of the input.
OS << InputFile->getData().substr(0, FirstNonAllocatableOffset);
```

输出文件 = 输入的浅拷贝 + 定点补丁。`FirstNonAllocatableOffset` 是分界线：之后的内容（debug info、symtab、notes）由 `rewriteNoteSections` 重排写入。这也是 `zeroPaddingForReusedSections` 存在的原因——拷贝来的对齐 padding 可能残留旧数据。

**2. 顺序敏感性**：PHDR 补丁在 section 写完后（新段大小已知）；SHT 补丁在最后（section index 已稳定）；RELA/RELR/GOT 依赖 `getNewFunctionOrDataAddress`（updateOutputValues 已完成）。17 步的次序全部由数据依赖决定，乱序即坏。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Out`（成员） | ToolOutputFile | 输出文件句柄，析构前必须 `keep()` 否则删除 |

### 优化意图

1. **补丁式重写**而非重新链接：保真（未触及部分 bit 级不变）+ 快（只写必须写的）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 调序需论证依赖 | 见逐段注释 2 | 静默产出损坏 ELF |
| OS 错误必须检查 | L6915-6920 | 磁盘满时静默截断 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 输出文件 | `ToolOutputFile` | `llvm/Support/ToolOutputFile.h` |
| 定位写 | `safePWrite` | `bolt/Utils/Utils.h` |

### 其他补充

`Out->keep()` 防止 ToolOutputFile 析构时把临时文件删掉；最终权限按 `~umask` 设为可执行。

---

## rewriteFunctionsInPlace 函数分析

### 函数签名与目的（L6680-6763）

```cpp
void RewriteInstance::rewriteFunctionsInPlace(raw_fd_ostream &OS);
```

**功能**: 非重定位模式核心：把每个已发射函数的镜像 `pwrite` 回其**原文件偏移**。

### 整体结构

```text
rewriteFunctionsInPlace(OS)
├── 1. seek 扩容（防 pwrite 越界失败）
├── 2. 逐 emitted 函数:
│      split 且 fragment 地址不全 → 跳过（assert 全有或全无）
│      safePWrite(OS, ImageAddress, ImageSize, FileOffset)
│      [MaxSize 有限] 尾部补 nop（MAB->writeNopData）
│      split: 逐 fragment safePWrite
└── 3. [非 reloc] 覆写覆盖率统计打印
```

### 逐段注释

**1. 尾部 nop 填充 (L6724-6732)**

```cpp
if (Function->getMaxSize() != std::numeric_limits<uint64_t>::max()) {
  uint64_t Pos = OS.tell();
  OS.seek(Function->getFileOffset() + Function->getImageSize());
  BC->MAB->writeNopData(OS, Function->getMaxSize() - Function->getImageSize(),
                        &*BC->STI);
  OS.seek(Pos);
}
```

新代码比原函数短时，残差区域填 nop——旧指令残留在那里会被 disassembler/verifier 误读（也防执行流滑入垃圾）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::ImageAddress/ImageSize/FileOffset` | 输出镜像三元组 | emitAndLink 阶段填好 |

### 优化意图

1. 覆盖率统计（覆写函数数 / 执行计数占比）是非 reloc 模式的"成绩单"——无法移动函数，只能报告改善了多少热点。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| ImageSize <= MaxSize | 上游 mapCodeSectionsInPlace 已 assert | 双保险 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| nop 生成 | `MCAsmBackend::writeNopData` | `llvm/MC/MCAsmBackend.h` |

### 其他补充

无。

---

## zeroPaddingForReusedSections 函数分析

### 函数签名与目的（L6765-6804）

```cpp
void RewriteInstance::zeroPaddingForReusedSections(raw_fd_ostream &OS);
```

**功能**: `-use-old-text` 专用：把 BOLT 重写 section 与相邻旧 section 之间的对齐 padding 清零。

### 整体结构

```text
zeroPaddingForReusedSections(OS)
├── 1. 收集全部 section 起始文件偏移（SectionStarts，排序）
├── 2. 逐 finalized 可执行 section:
│      SecEnd = 偏移+大小
│      upper_bound 找下一个 section 起点 NextStart
│      NextStart > SecEnd → OS 写零 [SecEnd, NextStart)
└── 3. 恢复 OS 位置
```

### 逐段注释

无额外代码片段。源码注释（L6766-6769）点明动机：输出是输入的字节拷贝，BOLT 写过的 section 后面的 padding 可能残留旧数据——清零等价于"写在新偏移"的语义。

### 关键数据结构

无本地结构（`SmallVector<uint64_t, 16> SectionStarts` 临时表）。

### 优化意图

1. 消灭"幽灵指令"：残留字节对 readelf/objdump/后续 BOLT 都是误导源。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 仅处理 SHF_EXECINSTR section | 数据 padding 无害 | 过度清零会抹掉有效数据 |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## writeEHFrameHeader 函数分析

### 函数签名与目的（L6930-7037）

```cpp
void RewriteInstance::writeEHFrameHeader();
```

**功能**: 生成新 `.eh_frame_hdr`（binary search table，加速运行时 unwind 查找）。

### 整体结构

```text
writeEHFrameHeader()
├── 1. 无新 .eh_frame → 返回
├── 2. 解析新 .eh_frame + .relocated.eh_frame 的输出内容
├── 3. 原 .eh_frame_hdr 装得下 → 原地覆写（尾部补零）
├── 4. 装不下/原本没有 → NextAvailableAddress 新分配:
│      [BOLTReserved] 容量校验
│      注册新 section（NewSecPrefix + .eh_frame_hdr）
├── 5. generateEHFrameHeader(旧表, 新表, 地址) → 写盘
└── 6. 合并: relocated 段范围并入新 .eh_frame 的 size，de-register 前者
```

### 逐段注释

**1. gdb 兼容合并 (L7023-7033)**

```cpp
// Merge new .eh_frame with the relocated original so that gdb can locate all
// FDEs.
if (RelocatedEHFrameSection) {
  const uint64_t NewEHFrameSectionSize =
      RelocatedEHFrameSection->getOutputAddress() +
      RelocatedEHFrameSection->getOutputSize() -
      NewEHFrameSection->getOutputAddress();
  NewEHFrameSection->updateContents(NewEHFrameSection->getOutputData(),
                                    NewEHFrameSectionSize);
  BC->deregisterSection(*RelocatedEHFrameSection);
}
```

gdb 要求所有 FDE 从 `.eh_frame` 起址线性可达——把 relocated 段的输出范围"并入"新段 size（内容本就物理相邻），再 de-register 前者。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `CFIReaderWriter::generateEHFrameHeader` | 表生成 | 新旧 FDE 合并排序的 binary table |

### 优化意图

1. 原地覆写优先：`.eh_frame_hdr` 位置不变则 `PT_GNU_EH_FRAME` 无需改（`patchELFPHDRTable` 仍会同步，双保险）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 新分配需校验预留容量 | exit(1) | 溢出预留区覆盖别的段 |
| 二次解析输出内容 | 以写盘内容为准 | 不解析会漏 FDE |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 表生成 | `CFIReaderWriter::generateEHFrameHeader` | `bolt/lib/Core/Exceptions.cpp` |

### 其他补充

`EHFrameHdrAlign`（成员常量 4）控制新分配时的对齐。

---

## updateSegmentInfo 函数分析

### 函数签名与目的（L4965-5031）

```cpp
void RewriteInstance::updateSegmentInfo();
```

**功能**: 把新分配区域汇总成 `SegmentInfo` 列表（`BC->NewSegments`），供 PHDR 补丁消费。

### 整体结构

```text
updateSegmentInfo()
├── 1. 无 RW 段: .eh_frame_hdr 追加 text 段尾 → NewTextSegmentSize 重算
│   有 RW 段: NewWritableSegmentSize 重算
├── 2. [NewTextSegmentSize] 构造 text SegmentInfo（PageAlign, RX）
│      [非 instrument] 单段 push
│      [instrument] 拆三段:
│        RX 前段 | RW (.bolt.instr.counters) | RX 后段
└── 3. [NewWritableSegmentSize] data SegmentInfo（RegularPageSize, RW）push
```

### 逐段注释

**1. instrument 三段拆分 (L4986-5017)**

```cpp
ErrorOr<BinarySection &> Sec =
    BC->getUniqueSectionByName(".bolt.instr.counters");
assert(Sec && "expected one and only one `.bolt.instr.counters` section");
const uint64_t Addr = Sec->getOutputAddress();
...
uint64_t Delta = Addr - TextSegment.Address;
TextSegment.Size = Delta;
TextSegment.FileSize = Delta;
BC->NewSegments.push_back(TextSegment);

SegmentInfo RWSegment = {Addr, Size, Offset, Size, BC->RegularPageSize,
                         false, true};
BC->NewSegments.push_back(RWSegment);
...
SegmentInfo RXSegment = {AddrRX, SizeRX, OffsetRX, SizeRX,
                         BC->RegularPageSize, true, false};
BC->NewSegments.push_back(RXSegment);
```

`.bolt.instr.counters` 必须落在 RW 段，但其前后是 RX 代码 → text 段拆三（这解释了 `discoverStorage` 预留 +2 PHDR）。三段各自 `RegularPageSize` 对齐，assert 保证计数器段严格内含于原 text 范围。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SegmentInfo` | Address/Size/FileOffset/FileSize/Alignment/IsExecutable/IsWritable | `patchELFPHDRTable` 的直接输入 |

### 优化意图

1. 段信息聚合与 PHDR 写入解耦：本函数只做"汇总与拆分决策"，写入细节全部留在补丁函数。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| `.bolt.instr.counters` 必须唯一 | assert | 多计数器段会拆错 |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## patchELFPHDRTable 函数分析

### 函数签名与目的（L5033-5150，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFPHDRTable(ELFObjectFile<ELFT> *File);
```

**功能**: 改写输出文件的 program header 表：更新既有条目、插入新 PT_LOAD。

### 整体结构

```text
patchELFPHDRTable(File)
├── 1. 无新 segment 且无预留 → 返回
├── 2. UseGnuStack: 删 PT_GNU_STACK 顶位（只允许 1 个新段）
│      否则 Phnum += NewSegments.size()
├── 3. seek(PHDRTableOffset)
├── 4. 逐既有 phdr 改写:
│      PT_LOAD 含 BOLTReserved → 加 PF_X
│      PT_PHDR → 指向新表位置/新 phnum
│      PT_GNU_EH_FRAME → 指向新 .eh_frame_hdr
│      PT_GNU_STACK [UseGnuStack] → 删除
├── 5. 新 PT_LOAD 插入到最后一个既有 PT_LOAD 之后（p_vaddr 升序）
├── 6. 整表写出，seek 回原位置
```

### 逐段注释

**1. 升序插入 (L5137-5144)**

```cpp
auto LastPTLoad = llvm::find_if(
    reverse(Phdrs), [](const PhdrTy &P) { return P.p_type == ELF::PT_LOAD; });
assert(LastPTLoad != Phdrs.rend() && "No existing PT_LOAD found");
auto InsertPos = LastPTLoad.base();
for (const SegmentInfo &SI : BC->NewSegments)
  InsertPos = std::next(Phdrs.insert(InsertPos, createPhdr(SI)));
```

ELF 规范要求 PT_LOAD 按 `p_vaddr` 升序（glibc ld.so 依赖）——新段地址必大于全部旧段（分配在末尾），插在最后一个 PT_LOAD 之后即满足。`createPhdr` lambda 把 `SegmentInfo` 转 `PhdrTy`（flags 按 IsExecutable/IsWritable 组合 `PF_R|PF_X|PF_W`）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ELFObjectFile<ELFT>::Elf_Phdr` | p_type/p_offset/p_vaddr/... | `llvm/Object/ELF.h` |

### 优化意图

1. `SavedPos`/`seek` 保存恢复：PHDR 表位置在文件中段，写完必须回到流末尾继续后续写入。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| PT_LOAD p_vaddr 升序 | 插入策略保证 | 乱序 loader 拒载 |
| UseGnuStack 只允许 1 新段 | L5049-5052 exit | 覆盖 PT_GNU_STACK 只有一个槽位 |
| 找不到 PT_GNU_STACK 且 UseGnuStack | exit(1) | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| phdr 遍历 | `ELFFile::program_headers` | `llvm/Object/ELF.h` |

### 其他补充

Linux kernel 不走此函数（`rewriteFile` L6878 条件包裹）。

---

## appendPadding 函数分析

### 函数签名与目的（L5156-5166，匿名命名空间）

```cpp
uint64_t appendPadding(raw_pwrite_stream &OS, uint64_t Offset,
                        uint64_t Alignment);
```

**功能**: 向 OS 写零直到 Offset 对齐 Alignment，返回新 offset。

### 整体结构

```text
appendPadding(OS, Offset, Alignment)
├── Alignment==0 → Offset
├── PaddingSize = offsetToAlignment(Offset, Align(Alignment))
├── 循环写 0
└── 返回 Offset + PaddingSize
```

### 逐段注释

无复杂分段。

### 关键数据结构

无。

### 优化意图

1. note/SHT 写入复用的公共对齐工具，保证"返回值即当前 offset"的使用约定。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 对齐差计算 | `offsetToAlignment` | `llvm/Support/Alignment.h` |

### 其他补充

被 `rewriteNoteSections`/`writeEHFrameHeader`/`patchELFSectionHeaderTable` 调用。

---

## finalizeSectionStringTable 函数分析

### 函数签名与目的（L5271-5289，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::finalizeSectionStringTable(ELFObjectFile<ELFT> *File);
```

**功能**: 生成 `.shstrtab`（section 名字符串表）并注册为 note section。

### 整体结构

```text
finalizeSectionStringTable(File)
├── 1. 全部非 anonymous section 的输出名 → SHStrTab.add
├── 2. SHStrTab.finalize() + write 到新分配缓冲
└── 3. registerOrUpdateNoteSection(".shstrtab", ..., SHT_STRTAB)
```

### 逐段注释

无复杂分段。`StringTableBuilder::ELF` 自动做后缀共享优化（同名后缀复用尾串）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `SHStrTab`（成员） | StringTableBuilder | `llvm/MC/StringTableBuilder.h` |

### 优化意图

1. anonymous section 不进表——merge/覆写掉的 section 名字不该出现在输出里。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在全部改名之后调用 | mergeCodeSections/tryRewriteSection 已定名 | 早调用名字缺失 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 字符串表 | `StringTableBuilder` | `llvm/MC/StringTableBuilder.h` |

### 其他补充

无。

---

## addBoltInfoSection 函数分析

### 函数签名与目的（L5291-5307）

```cpp
void RewriteInstance::addBoltInfoSection();
```

**功能**: 写 `.note.bolt_info`：BOLT revision + 完整命令行。

### 整体结构

```text
addBoltInfoSection()
├── DescStr = "BOLT revision: X, command line: ..."（Argc/Argv 拼接）
├── encodeELFNote("GNU", DescStr, NT_GNU_GOLD_VERSION)
└── registerOrUpdateNoteSection(".note.bolt_info", ...)
```

### 逐段注释

无复杂分段。伪装成 `NT_GNU_GOLD_VERSION` 的目的是 `readelf -n` 能直接打印内容（通用工具无需理解 BOLT 私有 note 类型）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinarySection::encodeELFNote` | note 封装 | name/desc/type 三元组打包 |

### 优化意图

1. **可复现性**：输出二进制自带完整重跑命令，用户报 bug 时信息自包含。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| note 编码 | `BinarySection::encodeELFNote` | `bolt/Core/BinarySection.h` |

### 其他补充

由 `updateMetadata` 触发（`-bolt-info` 默认开）。

---

## addBATSection 函数分析

### 函数签名与目的（L5309-5314）

```cpp
void RewriteInstance::addBATSection();
```

**功能**: 注册空的 BAT note section 占位。

### 整体结构

一行：`BC->registerOrUpdateNoteSection(BAT::SECTION_NAME, nullptr, 0, ..., SHT_NOTE);`

### 逐段注释

无。先占位（进 section 布局），内容稍后由 `encodeBATSection` 填充。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BoltAddressTranslation::SECTION_NAME` | `.note.bolt_bat` | 约定名，二次 BOLT 靠它发现 |

### 优化意图

1. 两步走：占位参与布局计算，编码在 symtab 之后（需要最终符号地址）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 占位与编码之间不得改布局 | 顺序固定在 rewriteFile | — |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## encodeBATSection 函数分析

### 函数签名与目的（L5316-5330）

```cpp
void RewriteInstance::encodeBATSection();
```

**功能**: 编码 BAT note：新旧地址翻译表写入 section。

### 整体结构

```text
encodeBATSection()
├── 1. BAT->write(*BC, DescOS)          // 表序列化
├── 2. encodeELFNote("BOLT", DescStr, NT_BOLT_BAT)
└── 3. registerOrUpdateNoteSection(...) + 尺寸打印
```

### 逐段注释

无复杂分段。owner 为 `BOLT`、type 为 `NT_BOLT_BAT`（私有 note，readelf 不解析但保留）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BoltAddressTranslation` | 函数/块级映射 | 输出二进制上采样的 profile 需它反查**输入**地址 |

### 优化意图

1. **闭环二次优化**：BOLT 输出 → perf 采样（地址是新布局）→ `preprocessProfileData` 的 `setBAT` 反翻译 → 喂给下一轮 BOLT。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在符号表补丁之后 | 映射含符号地址 | 早编码地址不全 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 表序列化 | `BoltAddressTranslation::write` | `bolt/lib/Profile/BoltAddressTranslation.cpp` |

### 其他补充

无。

---

## shouldStrip 函数分析

### 函数签名与目的（L5332-5349，模板成员）

```cpp
template <typename ELFShdrTy>
bool RewriteInstance::shouldStrip(const ELFShdrTy &Section,
                                  StringRef SectionName);
```

**功能**: 判断非分配 section 是否从输出中剥离。

### 整体结构

```text
shouldStrip(Section, Name)
├── 非分配且 SHT_RELA/SHT_CREL → true
├── debug section 且非 UpdateDebugSections → true
└── RemoveSymtab 且 SHT_SYMTAB → true
```

### 逐段注释

无复杂分段。非分配重定位 section 是 `-emit-relocs` 残渣（BOLT 已消费其语义）；debug 默认剥离（`--update-debug-sections` 保留并重写）。

### 关键数据结构

无。

### 优化意图

1. 输出瘦身：重定位/debug 对运行无用，剥离减小文件与攻击面。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 只作用于非分配 section | 分配区剥离破坏布局 | 调用方已保证 |

### 关键 API / 源码路径

无。

### 其他补充

被 `rewriteNoteSections` 与 `getOutputSections` 调用。

---

## getOutputSections 函数分析

### 函数签名与目的（L5351-5552，模板成员）

```cpp
template <typename ELFT>
std::vector<typename object::ELFObjectFile<ELFT>::Elf_Shdr>
RewriteInstance::getOutputSections(ELFObjectFile<ELFT> *File,
                                    std::vector<uint32_t> &NewSectionIndex);
```

**功能**: 构造输出的 section header 表条目全集与"旧 index→新 index"映射，被 SHT 写入与符号表更新两处消费。

### 整体结构

```text
getOutputSections(File, &NewSectionIndex)
├── 1. 输入 allocatable section: 拷贝 shdr 改 sh_name（anonymous 跳过）
├── 2. BOLT 新 allocatable section: 全新 shdr
│      MergedTextSection 特判: sh_size = MergedTextSize
├── 3. 全部按 sh_offset 稳定排序；修相邻 sh_size 防区间重叠
├── 4. 输入非 allocatable section: 拷贝 shdr 改 offset/size（shouldStrip 跳过）
│      SHT_SYMTAB 的 sh_info = NumLocalSymbols
├── 5. 新非 allocatable section: 全新 shdr
├── 6. .eh_frame_hdr 交换到 .eh_frame 之前
├── 7. section index 分配；merged-away 指向合并宿主
└── 8. NewSectionIndex 映射表 + 返回纯 shdr 数组
```

### 逐段注释

**1. 重叠修正 (L5423-5445)**

```cpp
llvm::stable_sort(OutputSections, [](const auto &A, const auto &B) {
  return A.second.sh_offset < B.second.sh_offset;
});
ELFShdrTy *PrevSection = nullptr;
BinarySection *PrevBinSec = nullptr;
for (auto &SectionKV : OutputSections) {
  ELFShdrTy &Section = SectionKV.second;
  if (Section.sh_type == ELF::SHT_NOBITS)
    continue;
  if (PrevSection &&
      PrevSection->sh_offset + PrevSection->sh_size > Section.sh_offset) {
    if (opts::Verbosity > 1)
      BC->outs() << "BOLT-INFO: adjusting size for section "
                 << PrevBinSec->getOutputName() << '\n';
    PrevSection->sh_size = Section.sh_offset - PrevSection->sh_offset;
  }
  PrevSection = &Section;
  PrevBinSec = SectionKV.first;
}
```

原地覆写的 section（如 `.eh_frame`）与其后 section 之间可能出现"声明区间重叠"——排序后线性扫描，把前者的 sh_size 截到后者的 offset。NOBITS 不占文件空间故跳过。注意注释：地址连续性不保证（跨 segment），只修文件区间。

**2. eh_frame_hdr 顺序 (L5502-5516)**

```cpp
auto EHFrameHdrIt =
    llvm::find_if(OutputSections, HasOutputName(getEHFrameHdrSectionName()));
auto EHFrameIt = llvm::find_if(OutputSections, HasOutputName(".eh_frame"));
if (EHFrameHdrIt != OutputSections.end() &&
    EHFrameIt != OutputSections.end() && EHFrameIt < EHFrameHdrIt)
  std::rotate(EHFrameIt, EHFrameHdrIt, std::next(EHFrameHdrIt));
```

elfutils/libdw 找到 `.eh_frame` 就停止扫描、且只有**先出现**的 `.eh_frame_hdr` 被使用——`std::rotate` 把 hdr 换到 frame 前面。排序（供尺寸计算）与最终顺序（供工具兼容）分离处理。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `NewSectionIndex`（出参） | 旧→新 index | `sh_link`/`sh_info` 重映射与符号表 st_shndx 更新的依据 |

### 优化意图

1. **一处构造两处消费**：`patchELFSymTabs` 先"预演"调用拿映射，`patchELFSectionHeaderTable` 再实调拿表——逻辑只有一份，保证两次结果一致。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 排序必须稳定 | 同 offset section 的相对序 | stable_sort 保证 |
| index 分配与 SHT 序一致 | setIndex 逐个赋 | 消费方 assert |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| section 名偏移 | `SHStrTab.getOffset` | `llvm/MC/StringTableBuilder.h` |

### 其他补充

无。

---

## patchELFSectionHeaderTable 函数分析

### 函数签名与目的（L5561-5615，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFSectionHeaderTable(ELFObjectFile<ELFT> *File);
```

**功能**: 写出最终 SHT 并修补 ELF header。

### 整体结构

```text
patchELFSectionHeaderTable(File)
├── 1. getOutputSections(File, NewSectionIndex)
├── 2. 16B 对齐后逐条写 SHT:
│      sh_link = NewSectionIndex[sh_link]
│      SHT_REL/RELA 的 sh_info 同样重映射
├── 3. ELF header 修补:
│      [reloc mode] e_entry = 新入口 或运行库 start（entry_point 钩子）
│      [PHDRTableOffset] e_phoff/e_phnum
│      e_shoff/e_shnum/e_shstrndx
└── 4. safePWrite 回写 offset 0
```

### 逐段注释

**1. 入口点改写 (L5594-5606)**

```cpp
if (BC->HasRelocations) {
  RuntimeLibrary *RtLibrary = BC->getRuntimeLibrary();
  if (RtLibrary && opts::RuntimeLibInitHook == opts::RLIH_ENTRY_POINT) {
    NewEhdr.e_entry = RtLibrary->getRuntimeStartAddress();
    ...
  } else
    NewEhdr.e_entry = getNewFunctionAddress(NewEhdr.e_entry);
  assert((NewEhdr.e_entry || !Obj.getHeader().e_entry) &&
         "cannot find new address for entry point");
}
```

entry_point 钩子模式下 e_entry 直指运行库（插桩初始化最早执行）；否则翻译原入口。assert 保证"原入口非零则新入口也非零"。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ELFObjectFile<ELFT>::Elf_Ehdr` | e_entry/e_phoff/e_shoff/... | `llvm/Object/ELF.h` |

### 优化意图

1. SHT 放文件末尾：其自身尺寸会影响别的 section 偏移（源码注释 L5554-5556），末尾放置消除循环依赖。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| sh_link 重映射必须完整 | REL/RELA 的 sh_info 也要 | 漏映射 → 悬空引用 |
| e_shstrndx 用新 index | 字符串表被重排 | 旧 index 指错 section |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 定位写 | `safePWrite` | `bolt/Utils/Utils.h` |

### 其他补充

无。

---

## updateELFSymbolTable 函数分析

### 函数签名与目的（L5617-6086，模板成员）

```cpp
template <typename ELFT, typename WriteFuncTy, typename StrTabFuncTy>
void RewriteInstance::updateELFSymbolTable(
    ELFObjectFile<ELFT> *File, bool IsDynSym,
    const typename object::ELFObjectFile<ELFT>::Elf_Shdr &SymTabSection,
    const std::vector<uint32_t> &NewSectionIndex, WriteFuncTy Write,
    StrTabFuncTy AddToStrTab);
```

**功能**: 符号表语义核心：dynsym 原位改写 / symtab 全量重建，处理函数搬迁、ICF 折叠、split 片段、二级入口、数据移动全部情形。

### 整体结构

```text
updateELFSymbolTable(File, IsDynSym, ...)
├── 0. 准备: 字符串表、IslandSizes 缓存、四个 lambda
│      getNewSectionIndex（dynsym 容错版）
│      getSplitSymbolName（warm/cold 命名）
│      addExtraSymbols（ICF/split/岛标记）
│      shouldStrip（本地符号剥离）
├── 1. 主循环逐符号:
│      特殊符号 __hot_*/_end → updateSymbolValue(SHN_ABS)
│      函数符号: folded→parent；emitted→value/size/shndx 更新+addExtraSymbols
│      二级入口: translateInputToOutputAddress + fragment 定位 + size=0
│      数据符号: isMoved→新 section/地址；marker→删除
│      其余: 仅换 st_shndx
│      registerSymbol: dynsym→原位 Write；symtab→Symbols 暂存
├── 2. injected 函数符号（+cold）
├── 3. 合成符号: __bolt_runtime_start/fini、__hot_* 兜底、MergedTextMarkers
├── 4. stable_sort 本地符号前置
└── 5. 逐符号 Write(0, Symbol)
```

### 逐段注释

**1. addExtraSymbols 的四类追加 (L5685-5763)**

```cpp
auto addExtraSymbols = [&](const BinaryFunction &Function,
                           const ELFSymTy &FunctionSymbol) {
  if (Function.isFolded()) {
    const BinaryFunction *ICFParent = Function.getFoldedIntoFunction();
    ELFSymTy ICFSymbol = FunctionSymbol;
    ...
    ICFSymbol.st_name = AddToStrTab(...concat(".icf.0")...);
    ICFSymbol.st_value = ICFParent->getOutputAddress();
    ...
    Symbols.emplace_back(ICFSymbol);
  }
  if (Function.isSplit()) {
    if (!EmittedColdFileSymbol &&
        FunctionSymbol.getBinding() == ELF::STB_GLOBAL) {
      ... // 合成 FILE 符号防本地 cold 名冲突
    }
    for (const FunctionFragment &FF : Function.getLayout().getSplitFragments()) {
      if (FF.getAddress()) {
        ELFSymTy NewColdSym = FunctionSymbol;
        const SmallString<256> SymbolName = getSplitSymbolName(FF, FunctionSymbol);
        ...
        Symbols.emplace_back(NewColdSym);
      }
    }
  }
  ... // constant island: $d/$x 标记对
};
```

ICF 折叠函数补 `.icf.0` 别名指向折叠宿主（symbolization 可达）；split 函数每个 fragment 补 `name.cold.N` 本地符号；首个全局函数前插 BOLT 合成 FILE 符号（`getBOLTFileSymbolName`——正是 `registerFragments` 二次 BOLT 消费的那个约定）；常量岛补 `$d/$x` 对标记（AArch64/RISCV 反汇编器约定）。

**2. 二级入口换算 (L5897-5929)**

```cpp
if (Function && Function->isEmitted()) {
  const uint64_t OutputAddress =
      Function->translateInputToOutputAddress(Symbol.st_value);
  if (!OutputAddress)
    continue;                    // 无法映射（跳转表 data label）→ 删除
  NewSymbol.st_value = OutputAddress;
  NewSymbol.st_size = 0;         // 二级入口强制零尺寸
  FunctionLayout::fragment_const_iterator FF = llvm::find_if(
      Function->getLayout().fragments(), [&](const FunctionFragment &FF) {
        uint64_t Lo = FF.getAddress();
        uint64_t Hi = Lo + FF.getImageSize();
        return Lo <= OutputAddress && OutputAddress < Hi;
      });
  ...
  NewSymbol.st_shndx =
      Function->getCodeSection(FF->getFragmentNum())->getIndex();
}
```

本地 NOTYPE label 与 STT_FUNC 符号指向函数中部时，经 `translateInputToOutputAddress` 换算（内部入口/局部 label 的 JITLink 结果），并定位所属 fragment 换 section index；无法翻译的（BOLT 不跟踪的 data-in-code label）直接删除。

**3. 本地符号前置 (L6077-6082)**

```cpp
llvm::stable_sort(Symbols, [](const ELFSymTy &A, const ELFSymTy &B) {
  if (A.getBinding() == ELF::STB_LOCAL && B.getBinding() != ELF::STB_LOCAL)
    return true;
  return false;
});
```

ELF 要求 symtab 前 `sh_info` 个符号全部 STB_LOCAL——`NumLocalSymbols` 由 Write 回调统计，最终写入 SHT（`getOutputSections` 第 4 步读取）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `ELFObjectFile<ELFT>::Elf_Sym` | st_name/st_value/st_size/st_shndx | `llvm/Object/ELF.h` |
| `WriteFuncTy/StrTabFuncTy`（模板回调） | 写出/字符串注入 | dynsym 与 symtab 两种策略的注入点 |

### 优化意图

1. **一套语义、两种落盘**：dynsym 不能变大小（DT_SYMTSZ 固定）→ 原位 Write；symtab 可以重建 → 暂存排序后统一写出。语义逻辑只有一份。
2. `IslandSizes` map 记忆化 `estimateConstantIslandSize`（逐符号查询防重复估算）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| dynsym 不得增删 | 只改 value/shndx | 大小变化破坏 .dynamic |
| 本地符号必须前置 | stable_sort | sh_info 错 → 链接器解析错 |
| `__hot_start/__hot_end` 成对 | assert 0 或 2 | 单边更新语义破坏 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 地址翻译 | `BinaryFunction::translateInputToOutputAddress` | `bolt/lib/Core/BinaryFunction.cpp` |
| 岛尺寸估算 | `BinaryFunction::estimateConstantIslandSize` | 同上 |

### 其他补充

源码 L5967 的 `registerSymbol:` 标签 + goto：特殊符号跳过函数匹配直接注册。

---

## patchELFSymTabs 函数分析

### 函数签名与目的（L6088-6176，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFSymTabs(ELFObjectFile<ELFT> *File);
```

**功能**: 符号表补丁调度：dynsym 原位改 + symtab 重建。

### 整体结构

```text
patchELFSymTabs(File)
├── 1. getOutputSections(File, NewSectionIndex)  // 预演拿映射
├── 2. 找 SHT_DYNSYM:
│      updateELFSymbolTable(IsDynSym=true,
│        Write = safePWrite 原位覆写,
│        AddToStrTab = 哑函数)
├── 3. [RemoveSymtab] 返回
└── 4. 找 SHT_SYMTAB + 其 strtab:
       updateELFSymbolTable(IsDynSym=false,
         Write = 追加 NewContents + 统计 NumLocalSymbols,
         AddToStrTab = 追加 NewStrTab（NameResolver::restore 还原名）)
       注册新 .symtab/.strtab note section
```

### 逐段注释

**1. 字符串还原 (L6156-6160)**

```cpp
[&](StringRef Str) {
  size_t Idx = NewStrTab.size();
  NewStrTab.append(NameResolver::restore(Str).str());
  NewStrTab.append(1, '\0');
  return Idx;
}
```

发现期 `NR.uniquify` 加的 `/N` 后缀在输出 symtab 里还原成原名——`registerFragments` 的 `NR.restore` 与此呼应。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `NumLocalSymbols`（成员） | 计数 | `getOutputSections` 写入 symtab 的 sh_info |

### 优化意图

1. "预演 + 实调"两用 `getOutputSections`：符号表更新需要新 index 映射，但 SHT 写入发生在更后面——先跑一遍只取映射（结果确定，两遍一致）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| dynsym 无 SHT_DYNSYM 且非静态 | assert | 畸形输入 |
| 无 symtab 仅告警 | stripped 输入 | — |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 符号名还原 | `NameResolver::restore` | `bolt/Core/NameResolver.h` |

### 其他补充

无。

---

## patchELFAllocatableRelrSection 函数分析

### 函数签名与目的（L6178-6268，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFAllocatableRelrSection(ELFObjectFile<ELFT> *File);
```

**功能**: 重建 `.relr.dyn`（RELR 压缩表），与 `readDynamicRelrRelocations` 严格互逆。

### 整体结构

```text
patchELFAllocatableRelrSection(File)
├── 1. 无 DynamicRelrAddress → 返回
├── 2. FixAddend lambda: addend 指向被移动函数 → 原位改槽值
├── 3. 收集全部 isRELR 动态重定位的新偏移 → std::set（有序去重）
├── 4. 重新编码: 地址 entry + 位图 entry（超 MaxDelta 换基准）
└── 5. 剩余空间填 1（空位图）
```

### 逐段注释

**1. 编码循环 (L6244-6263)**

```cpp
for (auto RelIt = RelOffsets.begin(); RelIt != RelOffsets.end();) {
  WriteRelr(*RelIt);
  uint64_t Base = *RelIt++ + PSize;
  while (1) {
    uint64_t Bitmap = 0;
    for (; RelIt != RelOffsets.end(); ++RelIt) {
      const uint64_t Delta = *RelIt - Base;
      if (Delta >= MaxDelta || Delta % PSize)
        break;
      Bitmap |= (1ULL << (Delta / PSize));
    }
    if (!Bitmap)
      break;
    WriteRelr((Bitmap << 1) | 1);
    Base += MaxDelta;
  }
}
```

`std::set` 的有序性使"连续可打包"判定退化为相邻差值检查；`(Bitmap << 1) | 1` 保证位图 entry 最低位为 1（协议约定）。槽位溢出 exit(1)。

### 关键数据结构

同 `readDynamicRelrRelocations`（第五章）。

### 优化意图

1. **读写字节级对称**：任何不对称会在二次 BOLT 时立即暴露（输入解析失败）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 新偏移必须偶数 | assert `Wrong relocation offset` | 奇数地址进不了 RELR |
| 尾部填充 1 | 空位图 | 残留旧值会被解出假重定位 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 新地址翻译 | `getNewFunctionOrDataAddress` | 本文件 L6646 |

### 其他补充

无。

---

## patchELFAllocatableRelaSections 函数分析

### 函数签名与目的（L6270-6383，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFAllocatableRelaSections(ELFObjectFile<ELFT> *File);
```

**功能**: 重建 `.rela.dyn` 与 `.rela.plt` 内容。

### 整体结构

```text
patchELFAllocatableRelaSections(File)
├── 1. 计算两个表的文件偏移区间（.rela.dyn / .rela.plt）
├── 2. writeRelocations(PatchRelative):
│      逐 section 的动态重定位:
│        r_offset = getNewFunctionOrDataAddress(旧) 或 section新址+旧偏移
│        有符号 → getOutputDynamicSymbolIndex(Symbol)
│        无符号 RELATIVE → addend 换新函数地址
│        IsJmpRelocation[RType] 决定写 .rela.plt 还是 .rela.dyn
├── 3. writeRelocations(true) 先跑（RELATIVE 前置），再 false
└── 4. fillNone: 剩余槽位填 R_*_NONE
```

### 逐段注释

**1. RELATIVE 前置 (L6361-6364)**

```cpp
// The dynamic linker expects all R_*_RELATIVE relocations in RELA
// to be emitted first.
writeRelocations(/* PatchRelative */ true);
writeRelocations(/* PatchRelative */ false);
```

glibc 的 RELATIVE 快速路径要求表内相对重定位连续前置——两遍写入保证序。顺带统计 `DynamicRelativeRelocationsCount` 供 `patchELFDynamic` 回填 `DT_RELACOUNT`。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Elf_Rela` | r_offset/sym/type/addend | `setSymbolAndType` 打包 r_info |

### 优化意图

1. 表大小不变（`.dynamic` 的 DT_RELASZ 未更新）：多余槽位填 NONE 而非截断。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 槽位溢出 exit(1) | 新表条数 > 旧表 | 需要用户重链更大的表 |
| 每表偏移有效性检查 | 无对应表 → exit | JMPREL 型重定位必须有 .rela.plt |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| dynsym index | `getOutputDynamicSymbolIndex`（成员） | `RewriteInstance.h` |

### 其他补充

无。

---

## patchELFGOT 函数分析

### 函数签名与目的（L6385-6418，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFGOT(ELFObjectFile<ELFT> *File);
```

**功能**: 遍历 `.got` 每个槽位，值命中旧函数地址则改写为新地址。

### 整体结构

```text
patchELFGOT(File)
├── 1. 找 .got section；无 → 静态二进制仅提示，返回
└── 2. 逐 8 字节槽:
       getNewFunctionAddress(槽值) 非零 → safePWrite 新值
```

### 逐段注释

无复杂分段。GOT 槽的消费者（PLT stub、间接调用、`-fno-plt`）自动指向新函数，无需逐一改调用方。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `.got` 内容 | uint64 数组 | 以 8 字节为步长解释（64 位假设由模板保证） |

### 优化意图

1. **单点收敛**：改 GOT 一个槽等效于改所有经它跳转的调用点——比扫指令流便宜得多。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 槽值恰好等于函数地址才改 | 保守匹配 | 数据巧合命中函数地址的概率可忽略（且有 reloc mode 语义保护） |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 函数地址翻译 | `getNewFunctionAddress` | 本文件 L6633 |

### 其他补充

无。

---

## patchELFDynamic 函数分析

### 函数签名与目的（L6420-6518，模板成员）

```cpp
template <typename ELFT>
void RewriteInstance::patchELFDynamic(ELFObjectFile<ELFT> *File);
```

**功能**: 逐条改写 `.dynamic`：DT_INIT/DT_FINI 地址、DT_RELACOUNT、DT_FLAGS(znow)。

### 整体结构

```text
patchELFDynamic(File)
├── 1. 静态可执行 → 返回
├── 2. 定位 PT_DYNAMIC（p_offset）
├── 3. 逐 dynamic entry:
│      DT_RELACOUNT → 重算计数
│      DT_INIT/DT_FINI:
│        [reloc] getNewFunctionAddress 换新
│        运行库 → 覆盖为 runtime start/fini（带日志）
│      DT_FLAGS/DT_FLAGS_1: RequiresZNow → 置 DF_BIND_NOW/DF_1_NOW
└── 4. RequiresZNow 但无 FLAGS 条目 → exit(1)（要求 -z now 重链）
```

### 逐段注释

**1. znow 强制 (L6493-6517)**

```cpp
case ELF::DT_FLAGS:
  if (BC->RequiresZNow) {
    NewDE.d_un.d_val |= ELF::DF_BIND_NOW;
    ZNowSet = true;
  }
  break;
...
if (BC->RequiresZNow && !ZNowSet) {
  BC->errs()
      << "BOLT-ERROR: output binary requires immediate relocation "
         "processing which depends on DT_FLAGS or DT_FLAGS_1 presence in "
         ".dynamic. Please re-link the binary with -znow.\n";
  exit(1);
}
```

ICF 等变换要求"加载即完成重定位"（否则折叠地址在 lazy bind 下未定）——`RequiresZNow` 时补置 BIND_NOW；两个 FLAGS 条目都没有则无处置位，只能要求用户重链。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `Elf_Dyn` | d_tag/d_un | 动态段条目 |
| `BC->RequiresZNow` | bool | ICF 等 pass 设置 |

### 优化意图

1. DT_INIT/FINI 的运行库覆盖是插桩 init/fini 钩子的第三条路径（另两条：e_entry、.init_array——与第八章三个 update/discover 函数一一对应）。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| p_memsz == p_filesz | assert | 畸形输入 |
| ZNowSet 必须落位 | 否则 exit | 半吊 znow 等于没设 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 动态条目遍历 | `ELFFile::dynamicEntries` | `llvm/Object/ELF.h` |

### 其他补充

无。

---

# 十四、地址翻译与工具函数

## getNewFunctionAddress 函数分析

### 函数签名与目的（L6633-6644）

```cpp
uint64_t RewriteInstance::getNewFunctionAddress(uint64_t OldAddress);
```

**功能**: 旧函数头地址 → 新输出地址（只认精确匹配）。

### 整体结构

```text
getNewFunctionAddress(OldAddress)
├── getBinaryFunctionAtAddress(OldAddress) 无 → 0
├── isFolded → 换 getFoldedIntoFunction()
└── 返回 Function->getOutputAddress()
```

### 逐段注释

无复杂分段。ICF 折叠函数没有输出体（0 地址），必须翻译到折叠宿主——GOT/符号表/动态段的折叠符号全部经此归一。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryFunction::OutputAddress` | uint64 | emit 后由 updateOutputValues 填 |

### 优化意图

1. "折叠即重定向"集中一处，三个补丁函数（GOT/dynamic/symtab）共享。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 不处理函数中部地址 | 返回 0 | 中部需求走 `getNewFunctionOrDataAddress` |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 精确查找 | `BC->getBinaryFunctionAtAddress` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

无。

---

## getNewFunctionOrDataAddress 函数分析

### 函数签名与目的（L6646-6678）

```cpp
uint64_t RewriteInstance::getNewFunctionOrDataAddress(uint64_t OldAddress);
```

**功能**: 升级版翻译：函数头 → 已移动数据 → 函数中部入口（内部引用/多入口逐 BB 匹配）。

### 整体结构

```text
getNewFunctionOrDataAddress(OldAddress)
├── 1. getNewFunctionAddress 命中 → 返回
├── 2. BinaryData isMoved → getOutputAddress
├── 3. 包含函数 isEmitted:
│      hasInternalReferenceAt(偏移) 或 isMultiEntry:
│        逐 BB: (内部引用目标 或 入口块) 且地址匹配 → BB.getOutputStartAddress()
│      都不匹配 → BOLT-ERROR + exit(1)（建议 --skip-funcs）
└── 4. 返回 0
```

### 逐段注释

**1. 失败即死 (L6669-6673)**

```cpp
BC->errs() << "BOLT-ERROR: unable to get new address corresponding to "
              "input address 0x"
           << Twine::utohexstr(OldAddress) << " in function " << *BF
           << ". Consider adding this function to --skip-funcs=...\n";
exit(1);
```

地址落在已发射函数内却翻译不出——说明重定位/入口登记有遗漏，静默写 0 会产出损坏二进制。宁可失败并给出修复建议。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BinaryBasicBlock::OutputAddress` | 块级输出地址 | 入口/内部引用的翻译粒度 |
| `BF->hasInternalReferenceAt` | 偏移集合 | 第五章 `registerInternalRefDataRelocation` 登记 |

### 优化意图

1. 三级回退（函数头/数据/块级）覆盖动态重定位翻译的全部合法形态；`exit` 是**最后防线**的正确性策略。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| exit 语义 | 不可降级 | 频繁命中说明上游 pass 有 bug |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 包含查找 | `BC->getBinaryFunctionContainingAddress` | `bolt/Core/BinaryContext.cpp` |

### 其他补充

被 RELA/RELR 补丁与 `patchELFGOT` 间接消费。

---

## getNewValueForSymbol 函数分析

### 函数签名与目的（L7039-7050）

```cpp
uint64_t RewriteInstance::getNewValueForSymbol(const StringRef Name);
```

**功能**: 符号名 → 最终地址：JITLink 消解结果优先，未发射符号回退原值。

### 整体结构

```text
getNewValueForSymbol(Name)
├── Linker->lookupSymbolInfo(Name) 命中 → Address
├── BC->getBinaryDataByName(Name) → getAddress（原值）
└── 0
```

### 逐段注释

无复杂分段。`flushPendingRelocations` 的 value provider——所有 pending relocation 的落盘值都经它取得（`rewriteFile`/`rewriteNoteSections`/`updateRt*Reloc` 注入回调）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BOLTLinker::lookupSymbolInfo` | 符号→地址/大小 | JITLink 符号表查询 |

### 优化意图

1. 回退原值很重要：未参与重写的符号（数据、外部符号）引用必须保持原地址，而不是变 0。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 必须在 Linker 建立后调用 | run() 顺序保证 | 早调用空指针 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| 符号查询 | `BOLTLinker::lookupSymbolInfo` | `bolt/Rewrite/JITLinkLinker.h` |

### 其他补充

无。

---

## getFileOffsetForAddress 函数分析

### 函数签名与目的（L7052-7068）

```cpp
uint64_t RewriteInstance::getFileOffsetForAddress(uint64_t Address) const;
```

**功能**: VMA → 文件偏移（新段恒等式 / 旧 segment 二分两路查询）。

### 整体结构

```text
getFileOffsetForAddress(Address)
├── 1. Address >= NewTextSegmentAddress:
│      return Address - NewTextSegmentAddress + NewTextSegmentOffset
├── 2. SegmentMapInfo.upper_bound(Address) 的前一个 segment:
│      在 [Address, Address+FileSize) 内 → FileOffset + (Address - SegmentInfo.Address)
└── 3. 返回 0（无效地址）
```

### 逐段注释

无复杂分段。第 1 路依赖 `discoverStorage` 维护的"同相位"不变式（新段内 offset == vaddr − 段基）；第 2 路用 `FileSize`（不是 Size）判定——`.bss` 尾部不在文件里，偏移无意义。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `BC->SegmentMapInfo` | map<vaddr, SegmentInfo> | `upper_bound+prev` 即包含段 |

### 优化意图

1. 全文件最高频的工具之一（每个 section 映射、每个补丁都调），两路查询都是 O(log n) 以内。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 返回 0 表示无效 | 调用方必须检查 | `discoverStorage` 末尾就用它校验旧 .text |
| NewTextSegmentAddress==0 时跳过第 1 路 | BOLTReserved 模式 | 与 discoverBOLTReserved 的清零配套 |

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## willOverwriteSection 函数分析

### 函数签名与目的（L7070-7078）

```cpp
bool RewriteInstance::willOverwriteSection(StringRef SectionName);
```

**功能**: 判断 section 内容是否会被 BOLT 整体重新生成（拷贝阶段跳过）。

### 整体结构

```text
willOverwriteSection(Name)
├── SectionsToOverwrite 命中 → true
├── DebugSectionsToOverwrite 命中 → true
└── 该 section 存在且 allocatable 且 finalized → true
```

### 逐段注释

无复杂分段。`SectionsToOverwrite`（头文件成员，`.eh_frame` 等）与 `DebugSectionsToOverwrite`（本文件 L349 静态数组：`.debug_*`/`.gdb_index`/`.pseudo_probe` 等，FIXME 注释标记待改进的替换机制）。

### 关键数据结构

| 结构 | 字段 | 含义 |
|---|---|---|
| `DebugSectionsToOverwrite`（静态） | 名字数组 | `--update-debug-sections` 时被 DWARFRewriter 重生成 |

### 优化意图

1. "跳过拷贝"与"写新内容"必须严格配对，此函数是配对关系的唯一判定点。

### 约束与易错点

| 约束 | 说明 | 风险 |
|---|---|---|
| 列表外的 debug section 会被拷贝 | 与重写内容并存 | FIXME 注释承认该机制粗糙 |

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| finalized 判定 | `BinarySection::isFinalized` | `bolt/Core/BinarySection.h` |

### 其他补充

被 `rewriteNoteSections` 调用。

---

## isDebugSection 函数分析

### 函数签名与目的（L7080-7087）

```cpp
bool RewriteInstance::isDebugSection(StringRef SectionName);
```

**功能**: 判断名字是否属于 debug 类 section。

### 整体结构

```text
isDebugSection(Name)
└── .debug_*/.zdebug_*/.gdb_index/.stab/.stabstr 前缀或精确匹配
```

### 逐段注释

无复杂分段。`.zdebug_` 是传统压缩 debug 前缀（与 `SHF_COMPRESSED` 并存的历史格式）。

### 关键数据结构

无。

### 优化意图

1. `readSpecialSections`（HasDebugInfo 判定 + 剥离告警）与 `shouldStrip` 共用的分类谓词。

### 约束与易错点

无。

### 关键 API / 源码路径

无。

### 其他补充

无。

---

## isCompressedDebugSection 函数分析

### 函数签名与目的（L7089-7091）

```cpp
bool RewriteInstance::isCompressedDebugSection(const SectionRef &Section);
```

**功能**: 判断 section 是否带 `SHF_COMPRESSED` 标志。

### 整体结构

单表达式：`(ELFSectionRef(Section).getFlags() & ELF::SHF_COMPRESSED) != 0`。

### 逐段注释

无。

### 关键数据结构

无。

### 优化意图

1. 与 `isDebugSection` 组合检测"压缩 debug + `--update-debug-sections`"的不支持组合（`readSpecialSections` 尽早拒绝）。

### 约束与易错点

无。

### 关键 API / 源码路径

| 功能 | API | 位置 |
|---|---|---|
| ELF 标志读取 | `ELFSectionRef::getFlags` | `llvm/Object/ELFObjectFile.h` |

### 其他补充

无。

---

# 十五、总结

## 设计要点回顾

1. **一条主线**：`run()` 的阶段顺序即数据依赖序——符号→FDE→（动态重定位→PLT）→边界→静态重定位→profile→反汇编→CFG→优化→发射→映射→回写。
2. **两种输出形态、一套代码**：`HasRelocations` 分支贯穿始终；非 reloc 模式被限定为"原地覆写 + cold 外移"。
3. **Symbol+Addend 归一化**：静态重定位、动态 RELATIVE、符号表、GOT 的引用全部归一成"锚点符号 + 偏移"，移动后只重定向锚点。
4. **信息不足时延迟决策**：PIC 跳转表（占位→`populateJumpTables`）、RISC-V `%pcrel_lo`（留空符号到反汇编）、CFI 程序（按需解析）。
5. **内存意识**：FileSymRefs/NR/CFI frame data/ProfileReader 使命结束即释放；符号地址 memoize；FDE-only 解析。
6. **健壮性**：可恢复错误降级（setSimple(false)/setIgnored），不可恢复 exit(1) 带上下文；形态黑名单（asan/coverage/stripped/二次 BOLT）早拒绝。

## 全文件易错点速查

| 约束 | 位置 | 违反后果 |
|---|---|---|
| 新 PHDR 表满足 `offset == vaddr - FirstAllocAddress` | discoverStorage | 部分 loader 找不到表 |
| RELATIVE 重定位排在 RELA 表最前 | patchELFAllocatableRelaSections | glibc 启动崩溃 |
| PT_LOAD p_vaddr 升序 | patchELFPHDRTable | 违反 ELF 规范 |
| symtab 本地符号前置（sh_info） | updateELFSymbolTable | 链接器解析错乱 |
| 非重定位模式 ImageSize <= MaxSize | mapCodeSectionsInPlace/rewriteFunctionsInPlace | 覆写越界 |
| `.eh_frame_hdr` 在 `.eh_frame` 前（SHT 序） | getOutputSections | elfutils 找不到 hdr |
| RELR 位图 entry 最低位 1、地址 entry 偶数 | 读写两侧 | 全表解码错误 |
| ICF 折叠符号重定向到 parent | getNewFunctionAddress 等 | 悬空地址 |
| fragment 与 parent 同去留 | selectFunctionsToProcess | 悬空 fragment 引用 |
| BAT 编码在符号表之后 | rewriteFile 步序 | 映射不全 |

## 调试与验证方法

```bash
# 全流水线调试输出
llvm-bolt input.bin -o out.bin -data=perf.fdata -v=1 2>&1 | less

# 逐阶段打印函数
llvm-bolt input.bin -o out.bin -print-cfg -print-disasm -print-all

# CFG dump（graphviz）
llvm-bolt input.bin -o out.bin -dump-dot-all -print-loops
llvm-bolt input.bin -o out.bin -dump-dot-func='foo.*,bar'

# 只跑部分函数（快速迭代）
llvm-bolt input.bin -o out.bin -funcs=foo,bar -lite

# 保留中间 .o 人工检查
llvm-bolt input.bin -o out.bin -keep-tmp

# 阶段耗时
llvm-bolt input.bin -o out.bin -time-rewrite

# 验证输出 ELF
readelf -lWdSW out.bin
llvm-dwarfdump --eh-frame out.bin

# 相关 lit 测试
build/bin/llvm-lit bolt/test/X86/rewrite
```

---

你想深入哪个部分？例如 `handleRelocation` 的某个分支族、`updateELFSymbolTable` 的 addExtraSymbols 细节、`emitBinaryContext`（发射器内部）或 `JITLinkLinker` 的回调时序。
