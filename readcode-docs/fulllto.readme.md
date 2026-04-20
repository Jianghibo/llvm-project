# LLVM Full LTO 机制完整分析

## 1. 整体架构概览

Full LTO（全程序链接时优化）是一种将整个程序的 IR 在链接阶段合并后进行跨模块优化的技术。与 ThinLTO 不同，Full LTO 将所有 bitcode 合并为一个大模块后统一优化。

```
编译流程：
┌─────────────────────────────────────────────────────────────────────────────┐
│ Full LTO 编译流程                                                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐                              │
│  │ Source.c │ -> │  Clang   │ -> │ a.o      │  (bitcode，带 module summary) │
│  └──────────┘    │ Frontend │    │ (IR obj) │                              │
│                  └──────────┘    └──────────┘                              │
│                        │                                                      │
│                        ▼                                                      │
│                  ┌──────────┐                                                │
│                  │ CodeGen  │  生成 LLVM IR + ModuleSummaryIndex            │
│                  │ Action   │  (若 -flto)                                    │
│                  └──────────┘                                                │
│                                                                             │
│  ══════════════════════════════════════════════════════════════════════════ │
│                          链接阶段                                            │
│                                                                             │
│  ┌──────────┐    ┌──────────┐    ┌──────────────────────────────┐          │
│  │ LLD/Gold │ -> │ LTO API  │ -> │ Backend (opt + codegen)      │          │
│  │ Linker   │    │ (LTO.cpp)│    │ (LTOBackend.cpp)             │          │
│  └──────────┘    └──────────┘    └──────────────────────────────┘          │
│       │               │                       │                             │
│       │               │                       │                             │
│       ▼               ▼                       ▼                             │
│  符号解析        Module 合并            LTO Pipeline                         │
│  (Resolution)    IR Linking              + Target CodeGen                   │
│                                                                             │
│                  ┌──────────┐                                               │
│                  │ Native   │  最终 ELF/COFF 目标文件                       │
│                  │ .o/.obj  │                                               │
│                  └──────────┘                                               │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 命令执行总览

以命令 `clang -O2 -flto -fuse-ld=lld test.cpp -o test` 为例，完整流程分为两个阶段：

- **编译期**：Clang Driver → clang -cc1 → 生成 bitcode IR object
- **链接期**：LLD → LTO API → Module 合并 + 优化 + 代码生成

---

## 3. 第一阶段：编译期（生成 LTO bitcode）

### 3.1 Clang Driver 入口

```
clang/tools/driver/driver.cpp:242
clang_main(int Argc, char **Argv, ToolContext)
  │
  ├── 解析命令行参数，确定 LTO 模式
  │   └─ Driver.cpp:860: Driver::setLTOMode(Args)
  │       └─ parseLTOMode() → LTOK_Full / LTOK_Thin
  │
  ├── 创建 Driver 对象
  │   └─ driver.cpp:360: Driver TheDriver(Path, Triple, Diags)
  │
  ├── 构建编译任务 Compilation
  │   └─ driver.cpp:388: Driver::BuildCompilation(Args)
  │       │
  │       ├── Driver.cpp:4423: BuildActions()
  │       │   │   构建编译管线: Preprocess → Compile → Backend → Assemble → Link
  │       │   │   对于 -flto，输出类型为 IR object (bitcode)
  │       │   │
  │       │   └─ ConstructPhaseAction()
  │       │       └─ 根据类型和阶段创建具体 Action
  │       │
  │       └─ BuildJobs()  // 从 Actions 构建 Job 命令
  │           │
  │           └─ 选择工具链: ToolChain::getTool()
  │               └─ Clang::ConstructJob()  // 构建 -cc1 命令
  │                   │   [clang/lib/Driver/ToolChains/Clang.cpp]
  │                   │   添加 -flto=full -flto-unit 等参数
  │                   │   Driver.cpp:5283: "-flto=full"
  │                   │
  │                   └─ 创建 Command 对象
  │                       └─ CC1Command 或 Command
  │
  └─ 执行编译
      └─ driver.cpp:419: Driver::ExecuteCompilation(*C, FailingCommands)
          │
          └─ Driver.cpp:2327: C.ExecuteJobs(C.getJobs(), FailingCommands)
              │
              └─ Compilation.cpp:235: ExecuteJobs()
                  │
                  └─ Compilation.cpp:160: ExecuteCommand(Job)
                      │
                      └─ Job.cpp:325: Command::Execute()
                          │
                          ├─ 若是 CC1Command (in-process):
                          │   └─ CC1Command::Execute() [Job.cpp:408]
                          │       └─ 直接调用 ExecuteCC1Tool()
                          │
                          └─ 若是普通 Command:
                              └─ llvm::sys::ExecuteAndWait()
                                  └─ fork+exec 启动 clang -cc1 子进程
```

### 3.2 clang -cc1 前端编译

```
clang/tools/driver/driver.cpp:228
ExecuteCC1Tool(Args) -> cc1_main()

clang/tools/driver/cc1_main.cpp
  │
  ├─ 创建 CompilerInstance
  │   └─ CompilerInstance CI
  │
  ├─ 创建 CompilerInvocation (解析 -cc1 参数)
  │   └─ CompilerInvocation::CreateFromArgs()
  │       └─ 解析 -flto、优化级别等
  │
  ├─ 创建 CompilerInstance 并初始化
  │
  └─ 执行编译动作
      └─ clang/lib/Frontend/FrontendAction.cpp:ExecuteAction()
          │
          └─ clang/lib/CodeGen/CodeGenAction.cpp
              │
              ├─ CodeGenAction::BeginSourceFileAction()
              │   └─ 创建 CodeGenerator
              │       └─ BackendConsumer
              │
              ├─ ASTConsumer::HandleTranslationUnit()
              │   │
              │   └─ BackendConsumer::HandleTranslationUnit() [CodeGenAction.cpp:231]
              │       │
              │       ├─ CodeGenModule()  // AST -> LLVM IR
              │       │
              │       └─ emitBackendOutput() [BackendUtil.cpp:1423]
              │           │
              │           │ 若 PrepareForLTO=true:
              │           ├─ EmitAssemblyHelper::emitAssemblyForLTO()
              │           │   │
              │           │   ├─ BackendUtil.cpp:197: shouldEmitRegularLTOSummary()
              │           │   │   └─ 生成 ModuleSummaryIndex
              │           │   │
              │           │   └─ BackendUtil.cpp:1000-1060: RunOptimizationPipeline()
              │           │       │   对于 LTO，执行简化优化
              │           │       │   不执行跨模块优化（留给链接时）
              │           │       │
              │           │       └─ PassBuilder::buildLTOPreLinkPipeline()
              │           │           │   [PassBuilderPipelines.cpp]
              │           │           └─ 前期优化 + Summary 生成
              │           │
              │           └─ WriteBitcodeToFile()
              │               │   [llvm/Bitcode/BitcodeWriter.cpp]
              │               │
              │               └─ 输出包含 IR + ModuleSummaryIndex 的 bitcode
              │                   输出文件: test.o (实际是 bitcode)
              │
              └─ 返回 bitcode object file
```

**编译期输出**：`test.o` 实际是包含 LLVM IR + ModuleSummaryIndex 的 bitcode 文件

### 3.3 Module Summary 生成

在 LTO 编译时，前端会生成 **ModuleSummaryIndex**（模块摘要索引），用于后续链接时的符号解析和 dead code elimination：

```cpp
// clang/lib/CodeGen/BackendUtil.cpp:197-200
bool shouldEmitRegularLTOSummary() const {
  return CodeGenOpts.PrepareForLTO && !CodeGenOpts.DisableLLVMPasses &&
         TargetTriple.getVendor() != llvm::Triple::Apple;
}
```

关键位置：
- **`llvm/Bitcode/BitcodeWriter.cpp`**: `WriteBitcodeToFile()` 函数将 Module 和 Summary 写入 bitcode
- **`llvm/Analysis/ModuleSummaryAnalysis.cpp`**: `buildModuleSummaryIndex()` 分析模块生成 summary

---

## 4. 第二阶段：链接期（LTO 优化与代码生成）

### 4.1 Clang Driver 构建链接命令

```
Driver::BuildActions() [Driver.cpp:4423]
  │
  └─ 输入文件为 .o (实际是 bitcode)
      └─ Phase = Link
      └─ ConstructPhaseAction(Link)
          └─ 创建 LinkJobAction
          │
          └─ BuildJobs()
              │
              └─ 选择 Linker 工具
              │   └─ ToolChain::SelectTool()
              │       └─ 若 -fuse-ld=lld: 选择 lld
              │
              └─ Gnu.cpp:271: gnutools::Linker::ConstructJob()
                  │   构建 lld 命令行
                  │
                  ├─ Gnu.cpp:442-444: addLTOOptions()
                  │   │   [CommonArgs.cpp:998]
                  │   │
                  │   ├─ 判断链接器类型
                  │   │   └─ 若是 LLD: 不需要 -plugin
                  │   │   └─ 若是 GNU ld/gold: 添加 -plugin LLVMgold.so
                  │   │
                  │   ├─ 添加 LTO 相关参数
                  │   │   ├─ -plugin-opt=O2  (优化级别)
                  │   │   ├─ -plugin-opt=mcpu=xxx
                  │   │   ├─ -plugin-opt=thinlto (若 ThinLTO)
                  │   │   └─ ...
                  │   │
                  │   └─ CommonArgs.cpp:1109-1113: 传递优化级别
                  │
                  └─ Gnu.cpp:584-587: 创建 Command
                      │   Exec = lld (或 ld.lld)
                      │   CmdArgs = [--sysroot, -m, -o test, -plugin-opt=O2, 
                      │              test.o, -lc, ...]
                      │
                      └─ C.addCommand(std::make_unique<Command>(...))
```

### 4.2 执行链接器 LLD

```
Driver::ExecuteCompilation() [Driver.cpp:2297]
  │
  └─ Compilation::ExecuteJobs() [Compilation.cpp:235]
      │
      └─ ExecuteCommand(LinkCommand)
          │
          └─ Command::Execute() [Job.cpp:325]
              │
              └─ llvm::sys::ExecuteAndWait("ld.lld", args...)
                  │   启动 LLD 进程
                  │
                  └─ lld/ELF/Driver.cpp:118
                      lld::elf::link(args, stdoutOS, stderrOS)
                        │
                        ├─ 创建 Ctx (Linker Context)
                        │   └─ Ctx ctx
                        │
                        └─ Driver.cpp:140: ctx.driver.linkerMain(args)
                            │   [Driver.cpp:652-740]
                            │
                            ├─ 解析命令行参数
                            │   └─ ELFOptTable parser.parse(args)
                            │   └─ readConfigs(ctx, args)
                            │
                            ├─ 创建输入文件
                            │   └─ Driver.cpp:721: createFiles(args)
                            │       │
                            │       └─ addFile(path)
                            │           │
                            │           └─ InputFiles.cpp:863
                            │               lto::InputFile::create(mbref)
                            │               └─ 创建 BitcodeFile 对象
                            │                   │   [InputFiles.cpp:863]
                            │                   │   解析 bitcode 符号表
                            │                   │
                            │                   └─ obj = check(lto::InputFile::create(mbref))
                            │                       │   [llvm/lib/LTO/LTO.cpp:624]
                            │                       │
                            │                       └─ InputFile::create(MemoryBufferRef)
                            │                           ├─ readIRSymtab() 读取 IR symbol table
                            │                           └─ 提取符号列表
                            │
                            ├─ 推断机器类型
                            │   └─ inferMachineType()
                            │
                            └─ Driver.cpp:731: invokeELFT(link, args)
                                │   调用模板化 link<ELFT>()
                                │   [Driver.cpp:3146-3324]
                                │
                                ├─ Driver.cpp:3161: parseFiles()
                                │   └─ 解析所有输入文件
                                │
                                ├─ Driver.cpp:3215-3218: 处理 libcall 符号
                                │   └─ 若有 bitcode 文件
                                │       └─ handleLibcall() 确保 runtime libcall 符号
                                │
                                └─ Driver.cpp:3303: compileBitcodeFiles()
                                    │   *** 核心 LTO 执行点 ***
                                    │   [Driver.cpp:2704-2744]
                                    │
                                    └─ 见下方详细分析
```

### 4.3 LTO 核心执行：BitcodeCompiler

```
lld/ELF/Driver.cpp:2704
LinkerDriver::compileBitcodeFiles<ELFT>(skipLinkedOutput)
  │
  ├─ Driver.cpp:2707: 创建 BitcodeCompiler
  │   │
  │   └─ lto.reset(new BitcodeCompiler(ctx))
  │       │   [lld/ELF/LTO.cpp:169-223]
  │       │
  │       ├─ LTO.cpp:46-167: createConfig(ctx)
  │       │   │   配置 LTO 参数:
  │       │   │   - OptLevel, CGOptLevel
  │       │   │   - FunctionSections, DataSections
  │       │   │   - HasWholeProgramVisibility
  │       │   │   - SampleProfile, PGO 等
  │       │   │
  │       │   └─ 返回 lto::Config
  │       │
  │       ├─ LTO.cpp:175-197: 创建 ThinBackend (若 ThinLTO)
  │       │   └─ createInProcessThinBackend() 或其他
  │       │
  │       └─ LTO.cpp:204-211: 创建 LTO 对象
  │           │
  │           └─ ltoObj = std::make_unique<lto::LTO>(createConfig(ctx), backend, ...)
  │               │   [llvm/lib/LTO/LTO.cpp:684-696]
  │               │
  │               ├─ 初始化 RegularLTOState
  │               │   └─ CombinedModule = "ld-temp.o"
  │               │   └─ Mover = IRMover(*CombinedModule)
  │               │
  │               └─ 初始化 ThinLTOState
  │                   └─ CombinedIndex = ModuleSummaryIndex
  │
  ├─ Driver.cpp:2708-2709: 添加所有 BitcodeFile
  │   │
  │   └─ for (BitcodeFile *file : ctx.bitcodeFiles)
  │       │   lto->add(*file)
  │       │   [lld/ELF/LTO.cpp:227-289]
  │       │
  │       ├─ LTO.cpp:234-236: 收集符号分辨率
  │       │   │   ArrayRef<Symbol *> syms = f.getSymbols()
  │       │   │   ArrayRef<lto::InputFile::Symbol> objSyms = obj->symbols()
  │       │   │   std::vector<lto::SymbolResolution> resols
  │       │   │
  │       │   └─ for each symbol:
  │       │       ├─ r.Prevailing = 是否为选定定义
  │       │       ├─ r.VisibleToRegularObj = 是否对普通 obj 可见
  │       │       ├─ r.FinalDefinitionInLinkageUnit = 是否本地定义
  │       │       └─ r.LinkerRedefined = 是否被 linker 重定义
  │       │
  │       └─ LTO.cpp:288: ltoObj->add(std::move(f.obj), resols)
  │           │   [llvm/lib/LTO/LTO.cpp:816-845]
  │           │
  │           └─ LTO::add(std::unique_ptr<InputFile> Input, Resolutions)
  │               │
  │               ├─ LTO.cpp:838-840: addModule()
  │               │   │   对每个 bitcode module 执行
  │               │   │
  │               │   ├─ LTO.cpp:851-863: 检查 LTOInfo
  │               │   │   └─ BitcodeModule.getLTOInfo()
  │               │   │   └─ IsThinLTO / HasSummary
  │               │   │
  │               │   ├─ LTO.cpp:883-885: addModuleToGlobalRes()
  │               │   │   └─ 记录全局符号分辨率
  │               │   │
  │               │   ├─ 若是 Regular LTO:
  │               │   │   └─ LTO.cpp:891-908: addRegularLTO()
  │               │   │       │   [LTO.cpp:944-1120]
  │               │   │       │
  │               │   │       ├─ LTO.cpp:951-957: BM.getLazyModule()
  │               │   │       │   └─ 懒加载 Module
  │               │   │       │
  │               │   │       ├─ LTO.cpp:1024-1095: 处理符号分辨率
  │               │   │       │   ├─ Prevailing: Keep.push_back(GV)
  │               │   │       │   ├─ Non-prevailing ODR: setLinkage(AvailableExternally)
  │               │   │       │   └─ 处理 Commons
  │               │   │       │
  │               │   │       └─ 若无 Summary:
  │               │   │           └─ LTO.cpp:897-900: linkRegularLTO()
  │               │   │               │   [LTO.cpp:1122-1157]
  │               │   │               │
  │               │   │               └─ IRMover::move(Mod.M, Keep, ...)
  │               │   │                   │   [llvm/lib/Linker/IRMover.cpp]
  │               │   │                   └─ 将 Module 链接到 CombinedModule
  │               │   │
  │               │   └─ 若是 ThinLTO:
  │               │       └─ LTO.cpp:1160-1236: addThinLTO()
  │               │           └─ BM.readSummary(ThinLTO.CombinedIndex)
  │               │           └─ 添加到 ThinLTO.ModuleMap
  │               │
  │               └─ 所有模块添加完成
  │
  └─ Driver.cpp:2714: lto->compile()
      │   [lld/ELF/LTO.cpp:320-430]
      │
      └─ BitcodeCompiler::compile()
          │
          ├─ LTO.cpp:321: ltoObj->getMaxTasks()
          │   └─ 返回需要生成的 object 文件数量
          │       Regular LTO: 1 (CombinedModule)
          │       ThinLTO: N (每个 module 一个)
          │
          ├─ LTO.cpp:339-346: ltoObj->run(AddStream, Cache)
          │   │   *** LTO 核心执行 ***
          │   │   [llvm/lib/LTO/LTO.cpp:1289-1353]
          │   │
          │   └─ LTO::run(AddStreamFn, FileCache)
          │       │
          │       ├─ LTO.cpp:1295-1324: 计算 dead symbols
          │       │   └─ computeDeadSymbolsWithConstProp()
          │       │   └─ 基于 GlobalResolutions 确定存活符号
          │       │
          │       ├─ LTO.cpp:1332-1333: setupOptimizationRemarks()
          │       │   └─ 配置优化 remarks 输出
          │       │
          │       ├─ LTO.cpp:1343: runRegularLTO(AddStream)
          │       │   │   [LTO.cpp:1355-1476]
          │       │   │
          │       │   ├─ LTO.cpp:1363-1366: linkRegularLTO()
          │       │   │   └─ 链接 ModsWithSummaries
          │       │   │   └─ IRMover::move()
          │       │   │
          │       │   ├─ LTO.cpp:1377-1402: 处理 Commons
          │       │   │   └─ 创建/更新 common 变量
          │       │   │
          │       │   ├─ LTO.cpp:1420-1425: updateVCallVisibilityInModule()
          │       │   │   └─ 更新 vcall 可见性 (WPD 准备)
          │       │   │
          │       │   ├─ LTO.cpp:1431-1464: 内部化符号
          │       │   │   └─ GV->setLinkage(InternalLinkage)
          │       │   │   └─ 非导出符号变为 internal
          │       │   │
          │       │   └─ LTO.cpp:1472-1475: backend()
          │       │       │   [llvm/lib/LTO/LTOBackend.cpp:575-600]
          │       │       │
          │       │       └─ lto::backend(Config, AddStream, ..., CombinedModule, Index)
          │       │           │
          │       │           ├─ LTOBackend.cpp:579-583: initAndLookupTarget()
          │       │           │   └─ TargetRegistry::lookupTarget()
          │       │           │
          │       │           ├─ LTOBackend.cpp:583: createTargetMachine()
          │       │           │   └─ Target::createTargetMachine()
          │       │           │
          │       │           ├─ LTOBackend.cpp:587-590: opt()
          │       │           │   │   [LTOBackend.cpp:381-414]
          │       │           │   │   运行 LTO 优化 pipeline
          │       │           │   │
          │       │           │   └─ opt(Config, TM, Task, Module, IsThinLTO, ...)
          │       │           │       │
          │       │           │       └─ LTOBackend.cpp:408-411: runNewPMPasses()
          │       │           │           │   [LTOBackend.cpp:256-371]
          │       │           │           │
          │       │           │           ├─ 创建 PassBuilder
          │       │           │           │   └─ PassBuilder(TM, PTO, PGOOpt, &PIC)
          │       │           │           │
          │       │           │           ├─ 注册分析
          │       │           │           │   └─ PB.registerModuleAnalyses(MAM)
          │       │           │           │   └─ PB.crossRegisterProxies()
          │       │           │           │
          │       │           │           └─ LTOBackend.cpp:351-355: 构建优化 pipeline
          │       │           │               │
          │       │           │               ├─ 若是 Full LTO:
          │       │           │               │   └─ MPM.addPass(PB.buildLTODefaultPipeline(OL, ExportSummary))
          │       │           │               │       │   [PassBuilderPipelines.cpp:1954-2200]
          │       │           │               │       │
          │       │           │               │       └─ Full LTO Pipeline:
          │       │           │               │           ├─ CrossDSOCFIPass
          │       │           │               │           ├─ WholeProgramDevirtPass (WPD)
          │       │           │               │           ├─ LowerTypeTestsPass
          │       │           │               │           ├─ GlobalDCEPass (dead code elimination)
          │       │           │               │           ├─ InferFunctionAttrsPass
          │       │           │               │           ├─ ArgumentPromotionPass
          │       │           │               │           ├─ IPSCCPPass (跨程序常量传播)
          │       │           │               │           ├─ GlobalSplitPass
          │       │           │               │           ├─ GlobalOptPass
          │       │           │               │           ├─ ModuleInlinerPass (关键!跨模块内联)
          │       │           │               │           ├─ MemProfContextDisambiguation
          │       │           │               │           ├─ GlobalDCEPass (再次)
          │       │           │               │           ├─ PromotePass
          │       │           │               │           ├─ InstCombinePass
          │       │           │               │           ├─ ...
          │       │           │               │           ├─ 最终优化: DCE, CFG simplification
          │       │           │               │           └─ LowerTypeTestsPass (最终)
          │       │           │               │
          │       │           │               └─ MPM.run(Mod, MAM)
          │       │           │                   └─ 执行所有优化 Pass
          │       │           │
          │       │           └─ LTOBackend.cpp:593-598: codegen()
          │       │               │   [LTOBackend.cpp:416-493]
          │       │               │   代码生成
          │       │               │
          │       │               └─ codegen(Config, TM, AddStream, Task, Module, Index)
          │       │                   │
          │       │                   ├─ LTOBackend.cpp:423-427: 嵌入 bitcode (可选)
          │       │                   │   └─ embedBitcodeInModule()
          │       │                   │
          │       │                   └─ LTOBackend.cpp:462-489: 代码生成 pipeline
          │       │                       │
          │       │                       └─ legacy::PassManager CodeGenPasses
          │       │                           │
          │       │                           ├─ LTOBackend.cpp:464-469: 添加辅助 Pass
          │       │                           │   ├─ TargetLibraryInfoWrapperPass
          │       │                           │   └─ RuntimeLibraryInfoWrapper
          │       │                           │   └─ ImmutableModuleSummaryIndexWrapperPass
          │       │                           │
          │       │                           └─ LTOBackend.cpp:481-485: TM->addPassesToEmitFile()
          │       │                               │   [llvm/lib/CodeGen/TargetPassConfig.cpp]
          │       │                               │
          │       │                               └─ 后端 pipeline:
          │       │                                   ├─ ISel (指令选择)
          │       │                                   ├─ MachineScheduler
          │       │                                   ├─ RegisterAllocator
          │       │                                   ├─ PrologEpilogInserter
          │       │                                   ├─ BranchFolding
          │       │                                   ├─ BlockPlacement
          │       │                                   └─ MC 层: 编码生成
          │       │                                   └─ 输出 ELF object
          │       │
          │       │                           └─ CodeGenPasses.run(Module)
          │       │                               └─ 执行代码生成
          │       │
          │       │                           └─ Stream->commit()
          │       │                               └─ 写入 native object file
          │       │
          │       ├─ LTO.cpp:1344-1347: runThinLTO() (若有 ThinLTO 模块)
          │       │   └─ 并行执行 ThinLTO backend
          │       │
          │       └─ 返回 native object files 列表
          │           └─ 每个 Task 对应一个 object
          │
          └─ LTO.cpp:382-427: 处理输出文件
              │
              ├─ 若有 ThinLTO cache: 从缓存获取
              │   └─ files[task] = cached object
              │
              ├─ 否则: 使用 buf[task]
              │   └─ buf[task].second = 编译后的 object
              │
              └─ for each task:
                  └─ createObjFile(ctx, MemoryBufferRef(objBuf, ltoObjName))
                      │   [lld/ELF/InputFiles.cpp]
                      │
                      └─ 返回 ObjFile 对象
                          └─ 添加到 ctx.objectFiles
```

### 4.4 后续链接流程

```
lld/ELF/Driver.cpp:3319-3324
  │
  ├─ 解析 LTO 生成的 object files
  │   └─ parallelForEach(newObjectFiles, initSectionsAndLocalSyms)
  │   └─ parallelForEach(newObjectFiles, postParseObjectFile)
  │
  ├─ Driver.cpp:3340-3345: 符号解析完成后的处理
  │   └─ 处理 duplicate symbols
  │   └─ 处理 undefined symbols
  │
  ├─ Driver.cpp:3350-3365: 执行链接
  │   │
  │   ├─ MarkLive()  ── 标记存活 sections (garbage collection)
  │   │   [lld/ELF/MarkLive.cpp]
  │   │
  │   ├─ ICF()  ── Identical Code Folding
  │   │   [lld/ELF/ICF.cpp]
  │   │
  │   ├─ assignAddresses()  ── 分配虚拟地址
  │   │
  │   ├─ finalizeSections()  ── 完成段布局
  │   │
  │   └─ Writer::run()  ── 写入最终 executable
  │       │   [lld/ELF/Writer.cpp]
  │       │
  │       ├─ 创建 ELF header
  │       ├─ 创建 Program headers
  │       ├─ 写入 section contents
  │       ├─ 创建 symbol table
  │       ├─ 创建 string tables
  │       └─ 写入最终文件: test
  │
  └─ 返回链接结果
      └─ errCount(ctx) == 0 ? success : failure
```

---

## 5. 关键数据结构与信息传递

### 5.1 编译期到链接期的信息传递

| 数据结构 | 编译期生成 | 链接期消费 | 位置 |
|---------|-----------|----------|------|
| `ModuleSummaryIndex` | `buildModuleSummaryIndex()` | `LTO::add()` 中读取 | `ModuleSummaryAnalysis.cpp` |
| `BitcodeModule` | `WriteBitcodeToFile()` | `InputFile::create()` 解析 | `BitcodeWriter.cpp` |
| `IRSymbolTable` | 嵌入 bitcode | `readIRSymtab()` | `IRObjectFile.cpp` |
| `GlobalValueSummary` | 每个 GV 的 summary | `CombinedIndex` 中合并 | `ModuleSummaryIndex.h` |

### 5.2 链接期核心数据流

```
BitcodeFile.obj (lto::InputFile)
    │   └─ symbols() 返回符号列表
    │   └─ 每个符号带 linkage/visibility 信息
    │
    └─> LTO::add(InputFile, SymbolResolution)
        │   SymbolResolution 决定每个符号的命运:
        │   ├─ Prevailing: 选定定义
        │   ├─ VisibleToRegularObj: 保留
        │   ├─ FinalDefinitionInLinkageUnit: 可 internalize
        │   └─ LinkerRedefined: 禁止 IPO
        │
        └─> GlobalResolutions (全局决议表)
            │   └─ 记录每个符号的最终状态
            │   └─ 用于后续 internalization
            │
            └─> CombinedModule (合并模块)
                │   Regular LTO: 所有 Module 链接到这里
                │   └─ IRMover::move() 合并
                │
                └─> backend() 执行优化+代码生成
                    └─> Native Object Files
                        └─> 继续正常链接流程
```

### 5.3 关键类定义

**lto::LTO 类** (`llvm/include/llvm/LTO/LTO.h:415-667`):

```cpp
class LTO {
  struct RegularLTOState {
    std::unique_ptr<Module> CombinedModule;     // 合并后的模块
    std::unique_ptr<IRMover> Mover;             // IR 模块链接器
  } RegularLTO;
  
  struct ThinLTOState {
    ModuleSummaryIndex CombinedIndex;           // 合并的 summary index
    ModuleMapType ModuleMap;                    // bitcode 模块映射
  } ThinLTO;
};
```

**SymbolResolution 结构** (`llvm/include/llvm/LTO/LTO.h:671`):

| 字段 | 含义 |
|------|------|
| `Prevailing` | 是否为选定的定义（链接器决议） |
| `FinalDefinitionInLinkageUnit` | 定义在本链接单元内不可抢占 |
| `VisibleToRegularObj` | 是否对普通 object 可见 |
| `ExportDynamic` | 是否动态导出 |
| `LinkerRedefined` | 是否被 linker 重定义（如 --wrap） |

---

## 6. 关键函数调用栈汇总

### 6.1 编译期完整调用栈

```text
clang_main()                            [driver.cpp:242]
  └─ Driver::BuildCompilation()         [Driver.cpp:1850]
      └─ Driver::BuildActions()         [Driver.cpp:4423]
          └─ ConstructPhaseAction()
              └─ Clang::ConstructJob()   [Clang.cpp:nnn]
                  └─ CC1Command 创建
      └─ Driver::ExecuteCompilation()   [Driver.cpp:2297]
          └─ Compilation::ExecuteJobs() [Compilation.cpp:235]
              └─ ExecuteCommand()
                  └─ Command::Execute()  [Job.cpp:325]
                      └─ llvm::sys::ExecuteAndWait("clang -cc1")
                          └─ cc1_main() [cc1_main.cpp:nnn]
                              └─ CompilerInstance::ExecuteAction()
                                  └─ BackendConsumer::HandleTranslationUnit()
                                      └─ emitBackendOutput()          [BackendUtil.cpp:1423]
                                          └─ EmitAssemblyHelper::RunOptimizationPipeline()
                                              └─ PassBuilder::buildLTOPreLinkPipeline()
                                                  └─ ModuleSummaryAnalysis
                                                  └─ WriteBitcodeToFile()    [BitcodeWriter.cpp]
                                                      └─ 输出 test.o (bitcode)
```

### 6.2 链接期完整调用栈

```text
Command::Execute("ld.lld", args...)     [Job.cpp:385]
  └─ llvm::sys::ExecuteAndWait()
      └─ lld::elf::link()                [Driver.cpp:118]
          └─ LinkerDriver::linkerMain()  [Driver.cpp:652]
              └─ createFiles()
                  └─ addFile("test.o")
                      └─ BitcodeFile::BitcodeFile()     [InputFiles.cpp:863]
                          └─ lto::InputFile::create()   [LTO.cpp:624]
                              └─ readIRSymtab()
              └─ link<ELFT>()             [Driver.cpp:3146]
                  └─ compileBitcodeFiles()             [Driver.cpp:2704]
                      └─ BitcodeCompiler::BitcodeCompiler()  [LTO.cpp:169]
                          └─ createConfig()
                          └─ lto::LTO::LTO()           [LTO.cpp:684]
                              └─ RegularLTOState 初始化
                              └─ ThinLTOState 初始化
                      └─ for each BitcodeFile:
                          └─ BitcodeCompiler::add()    [LTO.cpp:227]
                              └─ lto::LTO::add()       [LTO.cpp:816]
                                  └─ addModule()
                                      └─ addModuleToGlobalRes()
                                      └─ addRegularLTO() / addThinLTO()
                                          └─ IRMover::move()    [IRMover.cpp]
                      └─ BitcodeCompiler::compile()   [LTO.cpp:320]
                          └─ lto::LTO::getMaxTasks()
                          └─ lto::LTO::run()          [LTO.cpp:1289]
                              └─ computeDeadSymbolsWithConstProp()
                              └─ runRegularLTO()       [LTO.cpp:1355]
                                  └─ linkRegularLTO()
                                      └─ IRMover::move()
                                  └─ internalization
                                  └─ backend()         [LTOBackend.cpp:575]
                                      └─ opt()         [LTOBackend.cpp:381]
                                          └─ runNewPMPasses()
                                              └─ PassBuilder::buildLTODefaultPipeline()
                                                  └─ WPD, GlobalDCE, IPSCCP, Inliner, ...
                                              └─ MPM.run(Module, MAM)
                                      └─ codegen()     [LTOBackend.cpp:416]
                                          └─ TM->addPassesToEmitFile()
                                              └─ ISel, RegAlloc, ...
                                          └─ CodeGenPasses.run(Module)
                              └─ runThinLTO() (若有)
                          └─ createObjFile()  ── Native objects
                  └─ MarkLive()            [MarkLive.cpp]
                  └─ ICF()                 [ICF.cpp]
                  └─ Writer::run()         [Writer.cpp]
                      └─ 写入最终 executable: test
```

---

## 7. 编译期与链接期的串联关键点

| 连接点 | 编译期 | 链接期 | 关键机制 |
|-------|-------|-------|---------|
| **LTO 标记传递** | `-flto` → `-flto=full` | `-plugin-opt=O2` | `addLTOOptions()` [CommonArgs.cpp:998] |
| **Bitcode 格式** | `WriteBitcodeToFile()` | `InputFile::create()` | 嵌入 IRSymtab |
| **Module Summary** | `buildModuleSummaryIndex()` | `BM.readSummary()` | GV 摘要信息 |
| **符号决议** | 生成符号表 | `SymbolResolution` | Linker 提供 prevailing/visibility |
| **模块合并** | - | `IRMover::move()` | IR 级链接 |
| **跨模块优化** | 单模块优化 | `buildLTODefaultPipeline()` | 全程序视野 |
| **代码生成** | 前端跳过 | `codegen()` + `addPassesToEmitFile()` | 后端 pipeline |

---

## 8. 时间线总结

```
时间点 T1: clang 执行
  └─ Driver 解析 -flto，确定输出 bitcode
  └─ 构建 -cc1 命令
  └─ 执行 clang -cc1

时间点 T2: clang -cc1 执行 (前端编译)
  └─ AST → LLVM IR
  └─ 执行 LTOPreLink 优化 (简化优化，不做 IPO)
  └─ 生成 ModuleSummaryIndex
  └─ 输出 bitcode: test.o

时间点 T3: clang Driver 执行链接命令
  └─ 构建 ld.lld 命令
  └─ 添加 -plugin-opt= 参数
  └─ 执行 ld.lld

时间点 T4: LLD 执行 (LTO 核心)
  └─ 解析 test.o，识别为 bitcode
  └─ 创建 lto::LTO 对象
  └─ 添加 bitcode + 符号分辨率
  └─ IRMover 合并所有 Module 到 CombinedModule
  └─ 内部化非导出符号
  └─ 运行 buildLTODefaultPipeline (全程序优化)
      └─ 关键: 跨模块内联、常量传播、dead code elimination
  └─ 执行代码生成: ISel → RegAlloc → Emit
  └─ 输出 native object: test.lto.o (临时)

时间点 T5: LLD 完成链接
  └─ 解析 test.lto.o
  └─ MarkLive (GC)
  └─ ICF (代码折叠)
  └─ 分配地址
  └─ 写入最终 executable: test
```

---

## 9. Full LTO 优化 Pipeline 详细分析

### 9.1 buildLTODefaultPipeline() 位置

**`llvm/lib/Passes/PassBuilderPipelines.cpp:1954-2200`**

### 9.2 Full LTO Pipeline Pass 列表

```text
PassBuilder::buildLTODefaultPipeline(OptimizationLevel, ExportSummary)
  │
  ├─ FullLinkTimeOptimizationEarlyEP callbacks
  │
  ├─ MemProfRemoveInfo()              // 若不支持 hot/cold new
  │
  ├─ CrossDSOCFIPass()                // Cross-DSO CFI 检查
  │
  ├─ WholeProgramDevirtPass()         // 全程序虚函数去虚拟化
  │   └─ 分析所有 virtual call，确定可能的 callees
  │   └─ 若唯一 callee: 转换为 direct call
  │
  ├─ LowerTypeTestsPass()             // 降低 type.test intrinsics
  │   └─ CFI 实现: bitset check 转换为 runtime check
  │
  ├─ [若 O0]: 跳过大部分优化，仅执行必要的 lowering
  │
  ├─ [若 O1+]:
  │   ├─ SampleProfileLoaderPass()   // 若有 sample profile
  │   ├─ OpenMPOptPass()              // OpenMP 优化
  │   ├─ GlobalDCEPass(InLTOPostLink=true)  // 全局 DCE
  │   │   └─ 移除未使用的全局变量和函数
  │   │
  │   ├─ InferFunctionAttrsPass()     // 推断函数属性
  │   │   └─ 从已知库函数推断 readonly/nounwind 等
  │   │
  │   ├─ [若 O2+]:
  │   │   ├─ CallSiteSplittingPass()  // 调用点分裂
  │   │   ├─ PGOIndirectCallPromotion()  // 间接调用提升
  │   │   │   └─ 将 profile 指导的热点 indirect call 转为 direct
  │   │   │
  │   │   ├─ ArgumentPromotionPass()  // 参数提升
  │   │   │   └─ 将 by-ref 参数转为 by-value
  │   │   │
  │   │   ├─ SROAPass()               // Scalar Replacement of Aggregates
  │   │   │   └─ 消除 alloca，展开聚合类型
  │   │   │
  │   │   ├─ IPSCCPPass()             // Interprocedural SCCP
  │   │   │   └─ 跨程序常量传播和 dead argument elimination
  │   │   │   └─ 关键: 确定函数参数的常量值
  │   │   │
  │   │   └─ CalledValuePropagationPass()
  │   │
  │   ├─ ReversePostOrderFunctionAttrsPass()
  │   │   └─ RPO 属性推断
  │   │
  │   ├─ GlobalSplitPass()            // 全局变量分裂
  │   │   └─ 将大型全局变量按使用分裂
  │   │
  │   ├─ WholeProgramDevirtPass()     // 再次 WPD
  │   │   └─ 经过 IPSCCP 后可能有新的 devirtualization 机会
  │   │
  │   ├─ NoRecurseLTOInferencePass()
  │   │
  │   ├─ [若 O1]: 结束
  │   │
  │   ├─ [若 O2+]:
  │   │   ├─ CoroEarlyPass()          // Coroutine early pass
  │   │   │
  │   │   ├─ GlobalOptPass()          // 全局优化
  │   │   │   └─ 移除未使用的全局变量
  │   │   │   └─ 将全局变量转为常量
  │   │   │   └─ 合并重复的字符串常量
  │   │   │
  │   │   ├─ PromotePass()            // 提升 localized globals
  │   │   │   └─ 将 internal globals 提升为 SSA values
  │   │   │
  │   │   ├─ ConstantMergePass()      // 常量合并
  │   │   │
  │   │   ├─ DeadArgumentEliminationPass()
  │   │   │   └─ 移除未使用的函数参数
  │   │   │
  │   │   ├─ InstCombinePass()        // 指令合并
  │   │   │   └─ peephole 优化
  │   │   │
  │   │   ├─ AggressiveInstCombinePass()  [若 O2+]
  │   │   │
  │   │   ├─ ExpandVariadicsPass()    // 可变参数函数展开
  │   │   │
  │   │   ├─ ModuleInlinerPass() / ModuleInlinerWrapperPass()
  │   │   │   └─ *** 关键: 跨模块内联 ***
  │   │   │   └─ 将所有函数放入统一调用图
  │   │   │   └─ 基于 cost model 决定 inline
  │   │   │   └─ Full LTO 优势: 可内联任意跨模块调用
  │   │   │
  │   │   ├─ MemProfContextDisambiguation()  // Memory profile context 分离
  │   │   │   └─ 区分不同 allocation context
  │   │   │
  │   │   ├─ GlobalOptPass()          // 再次全局优化
  │   │   │   └─ inline 后有新的优化机会
  │   │   │
  │   │   ├─ OpenMPOptPass()          // 再次 OpenMP 优化
  │   │   │
  │   │   ├─ GlobalDCEPass()          // 再次全局 DCE
  │   │   │   └─ inline 后移除 dead code
  │   │   │
  │   │   ├─ [后续 scalar optimizations...]
  │   │   │   ├─ SCCPPass
  │   │   │   ├─ BDCEPass
  │   │   │   ├─ InstCombinePass
  │   │   │   ├─ JumpThreadingPass
  │   │   │   ├─ CorrelatedValuePropagationPass
  │   │   │   ├─ DSEPass
  │   │   │   ├─ LoopPass pipeline:
  │   │   │   │   ├─ LoopSimplifyPass
  │   │   │   │   ├─ LCSSAPass
  │   │   │   │   ├─ LICMPass
  │   │   │   │   ├─ LoopInstSimplifyPass
  │   │   │   │   ├─ LoopSimplifyCFGPass
  │   │   │   │   ├─ RotatelPass
  │   │   │   │   ├─ LoopDeletionPass
  │   │   │   │   ├─ LoopUnrollPass
  │   │   │   │   └─ SROAPass
  │   │   │   ├─ GVNPass
  │   │   │   ├─ NewGVNPass
  │   │   │   ├─ MemCpyOptPass
  │   │   │   ├─ DSEPass
  │   │   │   ├─ MoveHeaderPass
  │   │   │   ├─ InstCombinePass
  │   │   │   ├─ SimplifyCFGPass
  │   │   │   └─ ...
  │   │   │
  │   │   ├─ LowerTypeTestsPass()     // 最终 type test 降低
  │   │   │   └─ 将所有 type metadata 转为具体实现
  │   │   │
  │   │   ├─ CoroCleanupPass()        // Coroutine 清理
  │   │   │
  │   │   └─ AllocTokenPass()         // 分配 token pass
  │   │
  │   └─ FullLinkTimeOptimizationLastEP callbacks
  │   │
  │   └─ AnnotationRemarksPass()
  │
  └─ 返回 MPM
```

---

## 10. 核心优化差异：编译期 vs 链接期

| Pass 类型 | 编译期 (LTOPreLink) | 链接期 (LTO PostLink) |
|-----------|-------------------|---------------------|
| **跨模块内联** | ❌ 不执行 | ✅ ModuleInlinerPass |
| **IPSCCP** | ❌ 不执行 | ✅ 跨程序常量传播 |
| **GlobalDCE** | ❌ 单模块受限 | ✅ 全程序 dead elimination |
| **WPD** | ❌ 不执行 | ✅ WholeProgramDevirtPass |
| **Internalization** | ❌ 不执行 | ✅ 链接后执行 |
| **跨模块常量折叠** | ❌ 不执行 | ✅ GlobalOpt + InstCombine |

---

## 11. IRMover：模块链接核心

**`llvm/lib/Linker/IRMover.cpp`**

IRMover 负责 LTO 模块合并，处理符号链接、类型合并、全局值链接等：

```cpp
// LTO.cpp:1122-1157
Error LTO::linkRegularLTO(RegularLTOState::AddedModule Mod, bool LivenessFromIndex) {
  std::vector<GlobalValue *> Keep;
  // 筛选需要保留的 GlobalValue
  for (GlobalValue *GV : Mod.Keep) {
    if (LivenessFromIndex && !ThinLTO.CombinedIndex.isGUIDLive(GV->getGUID()))
      continue;  // Dead symbol 跳过
    Keep.push_back(GV);
  }
  return RegularLTO.Mover->move(std::move(Mod.M), Keep, nullptr, false);
}
```

---

## 12. Full LTO vs ThinLTO 关键区别

| 特性 | Full LTO | ThinLTO |
|------|----------|---------|
| 模块处理 | 合并为单一 Module | 保持独立模块 |
| 优化范围 | 全程序统一优化 | 模块 + 跨模块导入 |
| 内存占用 | 高（单 Module） | 低（并行处理） |
| 编译时间 | 长 | 短（并行） |
| 优化质量 | 最高 | 略低但接近 |
| Internalization | 全局符号内部化 | 模块级内部化 |
| 内联视野 | 跨所有模块 | 跨导入的模块 |

---

## 13. 调试与验证方法

### 13.1 编译期调试

```bash
# 查看 LTO 编译时的参数传递
clang -flto -O2 -v test.cpp -o test

# 查看 -cc1 命令
clang -flto -O2 -### test.cpp -o test

# 查看生成的 bitcode
llvm-dis test.o -o test.ll

# 查看 ModuleSummaryIndex
llvm-bcanalyzer -dump test.o
```

### 13.2 链接期调试

```bash
# 使用 -save-temps 查看各阶段 IR
clang -flto -O2 -Wl,-plugin-opt=save-temps test.cpp -o test

# 产生的文件：
#   test.0.preopt.bc      - 合并后优化前
#   test.1.promote.bc     - ThinLTO promote 阶段（若有）
#   test.2.internalize.bc - 内部化后
#   test.4.opt.bc         - 优化后
#   test.5.precodegen.bc  - 代码生成前

# 查看 LTO pipeline
clang -flto -O2 -mllvm -print-pipeline-passes test.cpp

# 查看优化 remarks
clang -flto -O2 -fsave-optimization-record test.cpp

# 使用 LLD 直接链接查看详情
lld -flto test.o -o test --lto-debug-pass-manager
```

### 13.3 LTO 特定调试选项

```bash
# 禁用 LTO internalization
clang -flto -O2 -mllvm -enable-lto-internalization=false test.cpp

# 查看 dead symbols
clang -flto -O2 -Wl,-plugin-opt=-debug-only=lto test.cpp

# 查看模块合并过程
clang -flto -O2 -mllvm -debug-only=irmover test.cpp

# 并行代码生成（Full LTO 也支持）
clang -flto -O2 -flto-partitions=4 test.cpp -o test
```

---

## 14. 关键源码文件索引

| 功能 | 文件 | 关键函数 |
|------|------|---------|
| Driver LTO 解析 | `clang/lib/Driver/Driver.cpp:838-877` | `parseLTOMode()`, `setLTOMode()` |
| LTO 参数传递 | `clang/lib/Driver/ToolChains/CommonArgs.cpp:998-1194` | `addLTOOptions()` |
| 前端 bitcode 生成 | `clang/lib/CodeGen/BackendUtil.cpp:1423-1474` | `emitBackendOutput()` |
| LTO PreLink pipeline | `llvm/lib/Passes/PassBuilderPipelines.cpp` | `buildLTOPreLinkPipeline()` |
| LTO 入口 | `lld/ELF/LTO.cpp:169-430` | `BitcodeCompiler` |
| LTO 核心控制 | `llvm/lib/LTO/LTO.cpp:684-1476` | `LTO::LTO()`, `LTO::add()`, `LTO::run()` |
| 模块链接 | `llvm/lib/LTO/LTO.cpp:944-1157` | `addRegularLTO()`, `linkRegularLTO()` |
| IR 合并 | `llvm/lib/Linker/IRMover.cpp` | `IRMover::move()` |
| LTO backend | `llvm/lib/LTO/LTOBackend.cpp:575-600` | `backend()` |
| LTO 优化 | `llvm/lib/LTO/LTOBackend.cpp:256-371` | `runNewPMPasses()` |
| LTO pipeline | `llvm/lib/Passes/PassBuilderPipelines.cpp:1954-2200` | `buildLTODefaultPipeline()` |
| 代码生成 | `llvm/lib/LTO/LTOBackend.cpp:416-493` | `codegen()` |
| LLD 链接入口 | `lld/ELF/Driver.cpp:652-740` | `linkerMain()` |
| LTO 执行点 | `lld/ELF/Driver.cpp:2704-2744` | `compileBitcodeFiles()` |

---

## 15. 总结

### 15.1 Full LTO 核心价值

Full LTO 的核心价值在于：

1. **全程序视野优化**：将所有模块合并后，优化器拥有完整的程序信息
2. **跨模块内联**：可内联任意跨模块函数调用，消除函数调用开销
3. **精确 dead code elimination**：基于全局符号决议，精确移除未使用代码
4. **Whole Program Devirtualization**：若 virtual call 的所有可能 callee 都在 LTO 范围内，可转为 direct call
5. **跨模块常量传播**：IPSCCP 可跨模块传播常量，优化函数参数

### 15.2 关键执行路径回顾

```
完整 Full LTO 执行链路：

1. Clang Frontend (clang/lib/CodeGen/)
   BackendConsumer::HandleTranslationUnit()
   -> emitBackendOutput() [BackendUtil.cpp:1423]
   -> WriteBitcodeToFile() + ModuleSummaryIndex

2. LLD Linker (lld/ELF/LTO.cpp)
   BitcodeCompiler::add()
   -> lto::LTO::add() [LTO.cpp:816]
   -> SymbolResolution + addModuleToGlobalRes()
   -> addRegularLTO() -> IRMover::move()

3. LTO Core (llvm/lib/LTO/LTO.cpp)
   LTO::run() [LTO.cpp:1289]
   -> computeDeadSymbolsWithConstProp()
   -> runRegularLTO() [LTO.cpp:1355]
   -> linkRegularLTO() -> 内部化符号
   -> backend() 调用

4. LTO Backend (llvm/lib/LTO/LTOBackend.cpp)
   lto::backend() [LTOBackend.cpp:575]
   -> opt() -> runNewPMPasses()
   -> PassBuilder::buildLTODefaultPipeline() [PassBuilderPipelines.cpp:1954]
   -> codegen() -> TargetMachine::addPassesToEmitFile()
```

### 15.3 待深入问题

- IRMover 的类型合并机制如何处理同名不同类型的全局值？
- WPD (WholeProgramDevirt) 的实现细节和 type metadata 结构？
- LTO internalization 的精确条件与 visibility 的交互？
- ModuleSummaryIndex 的计算算法和 liveness 分析？
- 并行 LTO 代码生成的实现机制 (`splitCodeGen`)？

---

## 16. FullLTO 符号可见性处理机制

本节详细分析 FullLTO 如何处理符号可见性问题，以及 IR 层面的符号可见性类型。

---

### 16.1 IR 符号可见性类型

LLVM IR 中符号可见性由四个属性共同决定：

#### 16.1.1 LinkageTypes（链接类型）

定义位置：`llvm/include/llvm/IR/GlobalValue.h:52-64`

| 类型 | 语义 | 链接器行为 | LTO 可优化程度 |
|---|---|---|---|
| `ExternalLinkage` | 强定义，外部可见 | 最终选择此定义 | 可 internalized |
| `AvailableExternallyLinkage` | 仅用于 inline，不发代码 | inline 后丢弃 | 最大优化空间 |
| `LinkOnceAnyLinkage` | 保留一份，可被任意替换 | 可能被不同定义替换 | 阻止激进 IPO |
| `LinkOnceODRLinkage` | 保留一份，只被等价定义替换 | ODR 保证等价 | 允许 IPO |
| `WeakAnyLinkage` | 弱定义，可被任意替换 | 可能被强定义覆盖 | 阻止激进 IPO |
| `WeakODRLinkage` | 弱定义，只被等价替换 | ODR 保证等价 | 允许 IPO |
| `AppendingLinkage` | 仅用于全局数组追加 | 特殊处理 | 不处理 |
| `InternalLinkage` | 内部链接 | 仅当前 TU 可见 | 已最大化优化 |
| `PrivateLinkage` | 私有链接，不进符号表 | 仅当前 TU 可见 | 已最大化优化 |
| `ExternalWeakLinkage` | 外部弱引用 | 未定义时允许 | 声明，不处理 |
| `CommonLinkage` | 惰性定义 | 多定义时选最大 | 转强定义后处理 |

**关键分类函数**：

```cpp
// 可替换（阻止激进 IPO）
static bool isInterposableLinkage(LinkageTypes L) {
  return L == WeakAnyLinkage || L == LinkOnceAnyLinkage || 
         L == CommonLinkage || L == ExternalWeakLinkage;
}

// ODR（允许 IPO，因为替换定义等价）
static bool isODRLinkage(LinkageTypes L) {
  return L == LinkOnceODRLinkage || L == WeakODRLinkage;
}

// Local（已最大化优化）
static bool isLocalLinkage(LinkageTypes L) {
  return L == InternalLinkage || L == PrivateLinkage;
}
```

#### 16.1.2 VisibilityTypes（符号可见性）

定义位置：`llvm/include/llvm/IR/GlobalValue.h:67-71`

| 类型 | ELF 对应 | 语义 | 对 DSOLocal 影响 |
|---|---|---|---|
| `DefaultVisibility` | `STV_DEFAULT` | 公开可见，可被外部 DSO 访问 | 不隐式 DSOLocal |
| `HiddenVisibility` | `STV_HIDDEN` | 仅当前 DSO 内可见，不导出 | **隐式 DSOLocal** |
| `ProtectedVisibility` | `STV_PROTECTED` | 可见但不可被抢占 | **隐式 DSOLocal** |

#### 16.1.3 DLLStorageClassTypes（DLL 存储类）

| 类型 | 语义 | 约束 |
|---|---|---|
| `DefaultStorageClass` | 普通 | 无特殊约束 |
| `DLLImportStorageClass` | 从 DLL 导入 | **禁止 DSOLocal**，禁止 internalization |
| `DLLExportStorageClass` | 导出到 DLL | 必须保持 external，禁止 internalization |

#### 16.1.4 DSOLocal（DSO 本地性）

语义：符号定义在当前链接单元内，运行时不会被动态链接器抢占。

**隐式 DSOLocal 条件** (`GlobalValue.h:300-303`):

```cpp
bool isImplicitDSOLocal() const {
  return hasLocalLinkage() ||
         (!hasDefaultVisibility() && !hasExternalWeakLinkage());
}
```

---

### 16.2 FullLTO 处理符号可见性的机制

FullLTO 通过链接器符号决议 → 全局决议表构建 → Internalization 三阶段处理。

#### 16.2.1 Phase 1：链接器符号决议

LLD 为每个符号构建 `SymbolResolution` 结构，告知 LTO 符号的可见性边界。

**定义位置**：`llvm/include/llvm/LTO/LTO.h:671-693`

**字段含义**：

| 字段 | LLD 判断逻辑 (`lld/ELF/LTO.cpp:238-287`) | 对 LTO 影响 |
|---|---|---|
| `Prevailing` | `!objSym.isUndefined() && sym->file == &f` | 标记为保留定义，非 prevailing 转 `AvailableExternallyLinkage` |
| `FinalDefinitionInLinkageUnit` | `(isExec || sym->visibility() != STV_DEFAULT) && dr` | 允许设置 `DSOLocal`，允许 internalization |
| `VisibleToRegularObj` | `relocatable || isUsedInRegularObj || referencedAfterWrap || (Prevailing && isExported)` | **禁止 internalization** |
| `ExportDynamic` | `computeBinding() != STB_LOCAL && (exportDynamic || isExported)` | 标记动态导出 |
| `LinkerRedefined` | `scriptDefined` (--wrap/--defsym) | **禁止 IPO** |

#### 16.2.2 Phase 2：全局决议表构建

LTO 将所有符号决议汇总到 `GlobalResolution` 结构。

**核心逻辑**：`LTO::addModuleToGlobalRes()` (`llvm/lib/LTO/LTO.cpp:709-782`)

**关键决策：Partition 判断**：

```cpp
if (Res.LinkerRedefined || Res.VisibleToRegularObj || Sym.isUsed() ||
    IsLibcall || GlobalRes.Partition != Partition) {
  GlobalRes.Partition = GlobalResolution::External;  // 不可 internalized
} else {
  GlobalRes.Partition = Partition;  // RegularLTO = 0
}
```

**Partition 含义**：
- `Partition == 0`：RegularLTO 分区，可 internalized
- `Partition == External`：对外部可见，禁止 internalization

#### 16.2.3 Phase 3：符号 Internalization

`LTO::runRegularLTO()` (`llvm/lib/LTO/LTO.cpp:1431-1464`) 执行核心变换。

**执行逻辑**：

```cpp
for (const auto &R : *GlobalResolutions) {
  GlobalValue *GV = CombinedModule->getNamedValue(R.second.IRName);
  
  // 跳过非 prevailing、非 RegularLTO 分区、已 local、声明符号
  if (!R.second.isPrevailingIRSymbol()) continue;
  if (R.second.Partition != 0) continue;
  if (!GV || GV->hasLocalLinkage() || GV->isDeclaration()) continue;
  
  // DLLImport/Export、AvailableExternally、Appending 不处理
  if (GV->getDLLStorageClass() != DefaultStorageClass ||
      GV->hasAvailableExternallyLinkage() || GV->hasAppendingLinkage())
    continue;
  
  // 核心：转为 InternalLinkage
  if (EnableLTOInternalization)
    GV->setLinkage(GlobalValue::InternalLinkage);
}
```

**Internalization 效果**：

| 变化 | 原因 |
|---|---|
| `Linkage` → `InternalLinkage` | 解除跨模块限制，最大化 IPO |
| `Visibility` → `DefaultVisibility` | Local linkage 要求 |
| `DLLStorageClass` → `DefaultStorageClass` | Local linkage 要求 |
| `DSOLocal` → `true` | `isImplicitDSOLocal()` 自动触发 |

#### 16.2.4 Phase 4：非 Prevailing 符号处理

`LTO::addRegularLTO()` (`llvm/lib/LTO/LTO.cpp:1049-1063`)

**处理逻辑**：

```cpp
if (!R.Prevailing && isa<GlobalObject>(GV) &&
    (GV->hasLinkOnceODRLinkage() || GV->hasWeakODRLinkage() ||
     GV->hasAvailableExternallyLinkage()) && !AliasedGlobals.count(GV)) {
  GV->setLinkage(GlobalValue::AvailableExternallyLinkage);
  GV->setComdat(nullptr);
}
```

**AvailableExternallyLinkage 用途**：
- 允许 inline 到调用点
- inline 后定义丢弃，不发到 .o 文件
- 实现"链接后死代码消除"

#### 16.2.5 Phase 5：DSOLocal 设置时机

**两个时机**：

1. **LTO 符号决议时** (`LTO.cpp:1067`):

```cpp
if (R.FinalDefinitionInLinkageUnit)
  GV->setDSOLocal(true);
```

2. **Internalization 后隐式生效** (`GlobalValue.h:545-546`):

```cpp
void setLinkage(LinkageTypes LT) {
  if (isLocalLinkage(LT)) {
    Visibility = DefaultVisibility;
    DllStorageClass = DefaultStorageClass;
  }
  if (isImplicitDSOLocal())
    setDSOLocal(true);  // Internal/Private 自动触发
}
```

---

### 16.3 关键约束

| 约束 | 位置 | 违反后果 |
|---|---|---|
| `LocalLinkage` 必须配合 `DefaultVisibility` | `GlobalValue.h:541-543` | IR 验证失败 |
| `LocalLinkage` 必须配合 `DefaultStorageClass` | `GlobalValue.h:542` | IR 验证失败 |
| `DLLImport` 不可 `DSOLocal` | `Verifier.cpp:822` | IR 验证失败 |
| `VisibleToRegularObj` 符号不可 internalized | `LTO.cpp:766-770` | 链接时 undefined reference |

---

### 16.4 Internalization 禁止条件

**完整条件列表** (`LTO.cpp:1431-1464`):

| 条件 | 原因 |
|---|---|
| `!EnableLTOInternalization` | 用户禁用 |
| `Partition != 0` | 对外部可见 |
| `!Prevailing` | 非保留定义 |
| `GV->isDeclaration()` | 未定义符号 |
| `GV->hasLocalLinkage()` | 已是 local |
| `DLLStorageClass != Default` | DLLImport/Export |
| `AvailableExternallyLinkage` | inline 专用 |
| `AppendingLinkage` | 特殊用途 |
| `LinkerRedefined` | --wrap/--defsym |
| `VisibleToRegularObj` | 对普通 object 可见 |

---

### 16.5 代码定位

| 功能 | 文件 | 函数/行号 |
|---|---|---|
| Linkage/Visibility 定义 | `llvm/include/llvm/IR/GlobalValue.h` | 行 52-71 |
| SymbolResolution 结构 | `llvm/include/llvm/LTO/LTO.h` | 行 671-693 |
| LLD 构建决议 | `lld/ELF/LTO.cpp` | 行 238-287 |
| GlobalResolution 构建 | `llvm/lib/LTO/LTO.cpp` | `addModuleToGlobalRes()` 行 709-782 |
| Internalization 执行 | `llvm/lib/LTO/LTO.cpp` | `runRegularLTO()` 行 1431-1464 |
| DSOLocal 设置 | `llvm/lib/LTO/LTO.cpp` | 行 1067 |
| Internalize Pass | `llvm/lib/Transforms/IPO/Internalize.cpp` | `maybeInternalize()` 行 132-167 |

---

### 16.6 总结

**FullLTO 符号可见性处理核心思想**：

1. 链接器通过 `SymbolResolution` 定义可见性边界
2. LTO 通过 `GlobalResolution` 汇总所有模块状态
3. Internalization 将非导出符号转为 `InternalLinkage`，解除跨模块优化限制
4. `DSOLocal` 保证运行时本地性，使优化假设成立

**关键设计权衡**：
- 最大化 IPO 空间（internalization）
- 保证链接正确性（保留必要 external 符号）
- 支持 DLL/动态链接场景
- 支持 --wrap/--defsym 等链接器特性

---

## 17. 同名符号处理机制完整分析

本节详细分析不同场景下同名 InternalLinkage/PrivateLinkage 符号的处理机制。

---

### 17.1 IR 符号可见性类型

LLVM IR 中符号可见性由四个属性共同决定，其中 **LinkageTypes** 是最关键的属性。

#### 17.1.1 LinkageTypes（链接类型）

定义位置：`llvm/include/llvm/IR/GlobalValue.h:52-64`

| 类型 | 语义 | 符号表可见性 | 同名冲突处理 |
|---|---|---|---|
| `ExternalLinkage` | 强定义，外部可见 | 在符号表中，参与决议 | 链接器选择一个定义 |
| `InternalLinkage` | 内部链接，仅当前 TU 可见 | 在符号表中，STB_LOCAL | 各 .o 独立保留，不冲突 |
| `PrivateLinkage` | 私有链接，不进符号表 | **不在符号表中** | 不检查冲突，直接共存 |
| `LinkOnceAnyLinkage` | 保留一份，可被任意替换 | 在符号表中 | 链接器选择一个定义 |
| `LinkOnceODRLinkage` | 保留一份，只被等价替换 | 在符号表中 | 链接器选择，ODR 保证等价 |
| `WeakAnyLinkage` | 弱定义，可被任意替换 | 在符号表中 | 链接器选择强定义 |
| `WeakODRLinkage` | 弱定义，只被等价替换 | 在符号表中 | 链接器选择，ODR 保证等价 |
| `AvailableExternallyLinkage` | 仅用于 inline | 在符号表中 | inline 后丢弃 |

**关键分类函数**：

```cpp
// Local linkage（不参与全局符号决议）
static bool isLocalLinkage(LinkageTypes L) {
  return L == InternalLinkage || L == PrivateLinkage;
}

// 可替换（阻止激进 IPO）
static bool isInterposableLinkage(LinkageTypes L) {
  return L == WeakAnyLinkage || L == LinkOnceAnyLinkage || 
         L == CommonLinkage || L == ExternalWeakLinkage;
}
```

#### 17.1.2 PrivateLinkage vs InternalLinkage

| 属性 | InternalLinkage | PrivateLinkage |
|---|---|---|
| 符号表 | 在符号表中 | **不在符号表中** |
| 名字前缀 | 无特殊要求 | 通常 `.Lxxx`（汇编临时符号） |
| 非 LTO | STB_LOCAL，各 .o 保留 | 不进符号表，不保留 |
| Full LTO | ValueSymbolTable 自动重命名 | 不进符号表，不检查冲突，直接共存 |

---

### 17.2 非 LTO 场景完整处理链路

非 LTO 场景下，同名 InternalLinkage 符号**在各编译单元独立处理后，全部保留到最终二进制中，互不冲突**。

#### 17.2.1 编译阶段：IR Linkage → ELF Binding

**InternalLinkage 不设置全局属性**：

位置：`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp:758-760`

```cpp
case GlobalValue::PrivateLinkage:
case GlobalValue::InternalLinkage:
  return;  // 不调用 emitSymbolAttribute，不设置 MCSA_Global/Weak
```

**默认 Binding 规则**：

位置：`llvm/lib/MC/MCSymbolELF.cpp:83-84`

```cpp
unsigned MCSymbolELF::getBinding() const {
  if (isBindingSet()) { ... }
  if (isDefined())
    return ELF::STB_LOCAL;  // 已定义且未显式设置 → STB_LOCAL
}
```

**结果**：`InternalLinkage` → `STB_LOCAL`

#### 17.2.2 ELF Object 文件结构

位置：`llvm/lib/MC/ELFObjectWriter.cpp`

```cpp
// 行 515-516: 分离 local 和 global 符号
std::vector<ELFSymbolData> LocalSymbolData;
std::vector<ELFSymbolData> ExternalSymbolData;

// 行 538: 判断 binding
bool Local = Symbol.getBinding() == ELF::STB_LOCAL;

// 行 584-587: 分类存放
if (Local)
  LocalSymbolData.push_back(MSD);
else
  ExternalSymbolData.push_back(MSD);

// 行 632: 记录 local/global 边界
LastLocalSymbolIndex = Index;

// 行 892: sh_info 指示第一个 non-local 符号索引
sh_info = LastLocalSymbolIndex;
```

**单个 .o 文件的符号表结构**：

```
.symtab:
┌────────────────────────────────────────────────┐
│ Index 0: NULL                                  │
├────────────────────────────────────────────────┤
│ Index 1..N: STB_LOCAL symbols    ← sh_info=N+1 │
│   - static 函数                                │
│   - static 变量                                │
│   - STT_FILE                                   │
├────────────────────────────────────────────────┤
│ Index N+1..M: STB_GLOBAL/STB_WEAK              │
│   - external 函数                              │
│   - weak 符号                                  │
└────────────────────────────────────────────────┘
```

#### 17.2.3 链接阶段：LLD 解析输入

位置：`lld/ELF/InputFiles.cpp:1268-1302`

```cpp
// 解析 local 符号（索引 0 到 firstGlobal-1）
for (size_t i = 0, end = firstGlobal; i != end; ++i) {
  const Elf_Sym &eSym = eSyms[i];
  
  // 验证必须是 STB_LOCAL
  if (LLVM_UNLIKELY(eSym.getBinding() != STB_LOCAL))
    Err(ctx) << "non-local symbol at index < sh_info";
  
  // 直接创建 Defined，存入 ObjFile.locals[] 数组
  // 不加入全局符号表 ctx.symtab
  symbols[i] = reinterpret_cast<Symbol *>(locals + i);
  new (symbols[i]) Defined(ctx, this, name, STB_LOCAL, ...);
}

// 行 1310: 只有 global 符号加入全局符号表
for (size_t i = firstGlobal; i != eSyms.size(); ++i) {
  Symbol *s = ctx.symtab->addSymbol(...);  // 参与 name resolution
}
```

**核心机制**：

| 符号类型 | 存储位置 | 符号决议 |
|---|---|---|
| `STB_LOCAL` | `ObjFile.locals[]`（每个文件独立） | 不决议，直接保留 |
| `STB_GLOBAL` | `ctx.symtab`（全局符号表） | 参与 name resolution，选择一个定义 |

#### 17.2.4 链接阶段：构建输出符号表

位置：`lld/ELF/Writer.cpp:493-510`

```cpp
static void demoteAndCopyLocalSymbols(Ctx &ctx) {
  for (ELFFileBase *file : ctx.objectFiles) {
    for (Symbol *b : file->getLocalSymbols()) {
      // 符号所属 section 未被 GC → 添加到输出 symTab
      if (ctx.in.symTab && includeInSymtab(ctx, *b) &&
          shouldKeepInSymtab(ctx, *dr))
        ctx.in.symTab->addSymbol(b);  // 同名符号全部添加
    }
  }
}
```

位置：`lld/ELF/SyntheticSections.cpp:2221`

```cpp
void SymbolTableBaseSection::addSymbol(Symbol *b) {
  symbols.push_back({b, strTabSec.addString(b->getName(), false)});
  //                                   hashIt=false ↑
  // local 符号的字符串不唯一化，同名字符串重复添加
}
```

位置：`lld/ELF/SyntheticSections.cpp:1282-1294`

```cpp
unsigned StringTableSection::addString(StringRef s, bool hashIt) {
  if (hashIt) {  // global 符号使用 hash，已存在则返回已有偏移
    auto r = stringMap.try_emplace(CachedHashStringRef(s), size);
    if (!r.second)
      return r.first->second;
  }
  // hashIt=false（local 符号）：直接添加，每次返回新偏移
  unsigned ret = this->size;
  this->size = this->size + s.size() + 1;
  strings.push_back(s);
  return ret;
}
```

**字符串表处理关键**：
- `hashIt=false` → 同名字符串不唯一化
- 每次 `addString("helper", false)` 都返回不同偏移
- 例如：第一次返回 5，第二次返回 12

#### 17.2.5 符号表排序

位置：`lld/ELF/SyntheticSections.cpp:2195-2216`

```cpp
void SymbolTableBaseSection::sortSymTabSymbols() {
  // 将 local 符号移到前半部分
  auto e = std::stable_partition(
      symbols.begin(), symbols.end(),
      [](const SymbolTableEntry &s) { return s.sym->isLocal(); });
  size_t numLocals = e - symbols.begin();
  getParent()->info = numLocals + 1;  // sh_info
  
  // 按文件分组排列 local 符号
  MapVector<InputFile *, SmallVector<SymbolTableEntry, 0>> arr;
  for (auto &s : llvm::make_range(symbols.begin(), e))
    arr[s.sym->file].push_back(s);
  
  auto i = symbols.begin();
  for (auto &p : arr)
    for (auto &entry : p.second)
      *i++ = entry;
}
```

#### 17.2.6 最终二进制结构示例

**输入**：

```
a.o:  "helper" (STB_LOCAL, section=.text.helper, value=0)
b.o:  "helper" (STB_LOCAL, section=.text.helper, value=0)
```

**链接后 executable**：

```
.strtab:
  Offset 0:  (null)
  Offset 1:  "a.c"     (STT_FILE)
  Offset 5:  "helper"  (来自 a.o)
  Offset 12: "b.c"     (STT_FILE)
  Offset 16: "helper"  (来自 b.o，同名不同偏移)
  Offset 23: "main"    (global)

.symtab:
  Index 0: NULL
  ──────────────────── sh_info = 5
  Index 1: "a.c",   STT_FILE,  STB_LOCAL, st_shndx=ABS
  Index 2: "helper", STT_FUNC, STB_LOCAL, st_name=5,  st_value=0x1000
  Index 3: "b.c",   STT_FILE,  STB_LOCAL, st_shndx=ABS
  Index 4: "helper", STT_FUNC, STB_LOCAL, st_name=16, st_value=0x2000
  ────────────────────
  Index 5: "main",  STT_FUNC, STB_GLOBAL, ...

Sections:
  .text:      包含来自 a.o 的 helper 函数代码，地址 0x1000
  .text:      包含来自 b.o 的 helper 函数代码，地址 0x2000
```

**同名共存机制**：

| 字段 | a.o 的 helper | b.o 的 helper |
|---|---|---|
| 符号索引 | Index 2 | Index 4 |
| `st_name` | 5（指向 "helper"） | 16（指向 "helper"） |
| `st_value` | 0x1000 | 0x2000 |

---

### 17.3 Full LTO 场景处理

Full LTO 将所有 IR 合并为单一 CombinedModule，同名 Internal 符号由 **ValueSymbolTable 自动重命名**。

#### 17.3.1 IRMover 处理 Local Linkage

位置：`llvm/lib/Linker/IRMover.cpp`

```cpp
// 行 365-366: Local 符号不参与符号匹配
GlobalValue *getLinkedToGlobal(const GlobalValue *SrcGV) {
  if (!SrcGV->hasName() || SrcGV->hasLocalLinkage())
    return nullptr;  // 不查找目标模块同名符号
}

// 行 897: Local 符号总是需要复制
bool shouldLink(GlobalValue *DGV, GlobalValue &SGV) {
  if (SGV.hasLocalLinkage())
    return true;  // 强制复制
}
```

**核心**：Internal/Private 符号直接复制到 CombinedModule，不检查是否与目标模块同名。

#### 17.3.2 ValueSymbolTable 自动重命名

位置：`llvm/lib/IR/ValueSymbolTable.cpp:42-78`

```cpp
ValueName *ValueSymbolTable::makeUniqueName(Value *V, StringRef &UniqueName) {
  unsigned BaseSize = UniqueName.size();
  bool AppendDot = isa<GlobalValue>(V);  // GlobalValue 使用 "." 分隔
  
  while (true) {
    UniqueName.resize(BaseSize);
    raw_svector_ostream S(UniqueName);
    if (AppendDot) S << ".";
    S << ++LastUnique;  // 添加递增数字
    
    auto IterBool = vmap.insert(std::make_pair(UniqueName.str(), V));
    if (IterBool.second)  // 插入成功
      return &*IterBool.first;
    // 冲突则继续尝试下一个数字
  }
}

// 行 112-127: createValueName
ValueName *ValueSymbolTable::createValueName(StringRef Name, Value *V) {
  auto IterBool = vmap.insert(std::make_pair(Name, V));
  if (IterBool.second)
    return &*IterBool.first;  // 无冲突
  
  // 有冲突 → 调用 makeUniqueName
  SmallString<256> UniqueName(Name);
  return makeUniqueName(V, UniqueName);
}
```

**重命名规则**：

| 原名 | 第一次冲突 | 第二次冲突 | 第三次冲突 |
|---|---|---|---|
| `helper` | `helper.1` | `helper.2` | `helper.3` |

#### 17.3.3 Full LTO 合并示例

**输入**：

```cpp
// a.c
static int helper = 1;

// b.c
static int helper = 2;

// c.c  
static int helper = 3;
```

**合并后 CombinedModule**：

```llvm
@helper = internal global i32 1      ; 原始名
@helper.1 = internal global i32 2    ; 自动重命名
@helper.2 = internal global i32 3    ; 自动重命名
```

---

### 17.4 Thin LTO 场景处理

Thin LTO 保持模块分离，处理方式与非 LTO 类似。

| 特性 | 处理方式 |
|---|---|
| 模块合并 | 不合并，各 backend 独立 |
| 同名 Internal | 各模块独立处理，不重命名 |
| 同名 Private | 不进符号表，不检查 |
| 符号导入 | 通过 `FunctionImporter` 按需导入函数 |

---

### 17.5 完整对比表

| 维度 | 非 LTO | Full LTO | Thin LTO |
|---|---|---|---|
| **处理时机** | 链接阶段 | IR 合并阶段 | 各 backend 独立 |
| **InternalLinkage 同名** | 各 .o 独立保留，最终全部共存 | 自动重命名为 `name.N` | 各模块独立保留 |
| **PrivateLinkage 同名** | 不进符号表，不保留 | 不进符号表，共存 | 不进符号表 |
| **符号决议** | STB_LOCAL 不参与决议 | IR 层合并后重命名 | 类似非 LTO |
| **字符串表** | 同名重复存储（hashIt=false） | Module 内唯一化 | 各模块独立 |
| **最终二进制** | 同名符号全部保留 | 重命名后符号共存 | 各 .o 独立输出 |

---

### 17.6 验证方法

```bash
# 非 LTO 场景
clang -c a.c b.c
ld.lld a.o b.o -o test
readelf -s test  # 查看同名 helper 符号

# Full LTO 场景
clang -flto -c a.c b.c
clang -flto a.o b.o -o test
llvm-dis test.0.preopt.bc -o combined.ll  # 查看重命名后的符号
grep helper combined.ll

# Private 符号验证
clang -S -emit-llvm a.c -o a.ll  # 查看 PrivateLinkage 符号
grep "private" a.ll
```

---

### 17.7 代码定位索引

| 功能 | 文件 | 关键行号 |
|---|---|---|
| **非 LTO** | | |
| Internal 不设置 global | `AsmPrinter.cpp` | 758-760 |
| STB_LOCAL 默认规则 | `MCSymbolELF.cpp` | 83-84 |
| ELF 符号表布局 | `ELFObjectWriter.cpp` | 538, 584-587, 632, 892 |
| LLD 解析 local 符号 | `InputFiles.cpp` | 1278-1302 |
| LLD 添加 local 到输出 | `Writer.cpp` | 493-510 |
| 字符串表添加 | `SyntheticSections.cpp` | 1282-1294, 2221 |
| 符号表排序 | `SyntheticSections.cpp` | 2195-2216 |
| **Full LTO** | | |
| Local 符号跳过决议 | `IRMover.cpp` | 365-366 |
| Local 符号强制复制 | `IRMover.cpp` | 897 |
| 符号表自动重命名 | `ValueSymbolTable.cpp` | 42-78, 112-127 |

---

### 17.8 总结

**核心设计思想**：

| 场景 | 核心机制 | 设计目的 |
|---|---|---|
| **非 LTO** | `InternalLinkage` → `STB_LOCAL` → 不参与全局决议 → 各 .o 独立保留 → 最终全部共存 | 符号设计为"文件私有"，链接器通过索引而非名字引用 |
| **Full LTO** | IRMover 复制 → ValueSymbolTable 冲突检测 → 自动添加 `.N` 后缀 | 所有 IR 合并到单一 Module，符号表需唯一化 |
| **Thin LTO** | 模块不合并 → 各 backend 独立 → 类似非 LTO | 保持模块分离，支持并行编译 |

**关键差异**：
- 非 LTO：**链接层**分离保留，通过不同 `st_name`、`st_value` 区分同名符号
- Full LTO：**IR 层**合并重命名，保证 Module 内符号表唯一性

---

## 18. Local 符号重定位机制完整分析

本节详细分析 Local 符号（STB_LOCAL/InternalLinkage）的重定位处理机制。

---

### 18.1 核心结论

**Local 符号重定位遵循以下核心原则：**

1. **编译时优化**：Local 符号引用转换为 **STT_SECTION 符号 + addend**，避免为每个 local 符号创建符号表条目
2. **链接时简化**：通过 **符号索引** 直接定位，不需要名字查找和全局符号决议
3. **地址确定性**：Local 符号地址在链接时就完全确定，运行时无需动态解析

> **总结性表述**：Local 符号的重定位设计体现了"最小化开销"原则：编译时减少符号表大小，链接时跳过符号决议，运行时无额外处理。整个过程是确定性的、静态的，不涉及任何动态链接机制。

---

### 18.2 编译阶段行为总结

#### 18.2.1 Local 符号 → STT_SECTION 转换机制

**位置**：`llvm/lib/MC/ELFObjectWriter.cpp:1358-1377`

```cpp
// 行 1358-1364: 判断是否转换为 STT_SECTION
bool UseSectionSym = SymA && SymA->getBinding() == ELF::STB_LOCAL &&
                     !SymA->isUndefined() &&
                     !mc::isRelocRelocation(Fixup.getKind());

// 行 1372-1374: 执行转换
if (UseSectionSym && useSectionSymbol(Target, SymA, Addend, Type)) {
  Addend += Asm->getSymbolOffset(*SymA);  // 符号偏移加到 addend
  SymA = static_cast<const MCSymbolELF *>(SecA->getBeginSymbol());  // 使用 section 符号
}
```

**行为总结**：

| 原始状态 | 转换后状态 | 设计目的 |
|---|---|---|
| 重定位引用 local 符号 `helper` | 重定位引用 section 符号 `.text` | 减少 `.symtab` 条目 |
| `r_addend = 0` | `r_addend = helper_offset` | 通过 addend 携带符号偏移 |
| 符号表包含 `helper` | 符号表可能不含 `helper` | 节省符号表空间 |

**不能转换的情况**：
- 符号类型为 `STT_GNU_IFUNC`（需要 `IRELATIVE` 动态重定位）
- `SHF_MERGE` section 且 offset ≠ 0（merge section 的特殊性）

---

### 18.3 链接阶段行为总结

#### 18.3.1 重定位扫描阶段

**位置**：`lld/ELF/Relocations.cpp:1333-1382`

**行为总结**：

| 符号类型 | 扫描行为 | 是否需要 GOT/PLT |
|---|---|---|
| `STB_LOCAL` | 直接标记引用 | **否**（地址已知） |
| `STB_GLOBAL` | 检查 preemptibility | 可能需要 |
| `STB_WEAK` | 检查 preemptibility | 可能需要 |

> **关键行为**：Local 符号在扫描阶段被识别为"非 preemptible"，不需要任何 GOT/PLT/动态重定位。链接器直接使用符号的最终地址。

#### 18.3.2 重定位应用阶段

**位置**：`lld/ELF/Arch/X86_64.cpp:1246-1259`

```cpp
void X86_64::relocateAlloc(InputSection &sec, uint8_t *buf) const {
  uint64_t secAddr = sec.getOutputSection()->addr + sec.outSecOff;
  
  for (const Relocation &rel : sec.relocs()) {
    uint8_t *loc = buf + rel.offset;
    const uint64_t val = sec.getRelocTargetVA(ctx, rel, secAddr + rel.offset);
    relocate(loc, rel, val);  // 直接写入最终地址
  }
}
```

> **关键行为**：重定位应用阶段，链接器**直接将最终地址写入代码段**。对于 local 符号，这个地址是完全确定的，不需要预留 GOT 条目或 PLT 入口。

#### 18.3.3 地址计算行为

**位置**：`lld/ELF/Symbols.cpp:66-134` 和 `InputSection.cpp:231-234`

**地址计算公式**：

```
symbol VA = OutputSection.addr + InputSection.outSecOff + symbol.value

分解：
  OutputSection.addr     → 链接器分配的虚拟地址
  InputSection.outSecOff → 输入 section 在输出 section 内的偏移
  symbol.value           → 符号在 section 内的偏移（来自 .o 的 st_value）
```

> **关键行为**：地址计算是**纯加法操作**，三个值都是链接时确定的静态值。Local 符号的地址计算不涉及任何动态查找或解析。

---

### 18.4 STT_SECTION 符号行为总结

#### 18.4.1 编译时的转换行为

```
示例：static int helper; 位于 .data section 偏移 0x10 处

原始重定位记录：
  r_offset  = 0x20（代码中的引用位置）
  r_symidx  = helper 的符号索引
  r_type    = R_X86_64_32
  r_addend  = 0

转换后重定位记录：
  r_offset  = 0x20
  r_symidx  = .data section 符号索引（STT_SECTION）
  r_type    = R_X86_64_32
  r_addend  = 0x10（helper 的偏移）

行为：把"引用 helper"改为"引用 .data + 0x10"
```

#### 18.4.2 链接时的还原行为

**位置**：`lld/ELF/InputSection.cpp:480-527`

```cpp
if (sym.type == STT_SECTION) {
  // STT_SECTION 符号：地址 = section VA + addend
  int64_t addend = rel.addend;
  uint64_t va = section->getVA(addend);  // 直接计算
}
```

> **关键行为**：STT_SECTION 重定位在链接时被还原为"section 起始地址 + addend"。addend 中携带的符号偏移被直接加到 section 地址上，得到符号最终地址。

---

### 18.5 符号索引定位行为

**位置**：`lld/ELF/InputFiles.h:110-113`

```cpp
template <typename RelT> Symbol &getRelocTargetSym(const RelT &rel) const {
  uint32_t symIndex = rel.getSymbol(ctx.arg.isMips64EL);
  return getSymbol(symIndex);  // 直接通过索引获取符号
}
```

**行为总结**：

| 操作 | Local 符号 | Global 符号 |
|---|---|---|
| 符号查找 | 索引 → `ObjFile.locals[]` | 索引 → `ctx.symtab` |
| 是否决议 | **否**（直接返回） | **是**（可能被替换） |
| 名字作用 | **无**（索引定位） | 用于 name resolution |

> **关键行为**：Local 符号通过符号索引直接定位，名字只用于调试，不参与任何链接决策。

---

### 18.6 Full LTO 场景行为总结

#### 18.6.1 IR 层行为

> **关键行为**：IR 层使用 `GlobalValue*` 指针直接引用符号，不生成重定位记录。同名 Internal 符号被 ValueSymbolTable 重命名后，各自独立存在。IR 合并阶段完全避免了传统重定位机制。

#### 18.6.2 代码生成行为

> **关键行为**：IR → native object 过程中，生成标准 ELF 重定位记录。此时 Internal 符号已被重命名为 `symbol.N`，但仍然保持 `InternalLinkage`。代码生成后的 `.o` 文件与非 LTO 场景一致。

---

### 18.7 完整流程行为对比

#### 18.7.1 非 LTO 场景

```text
编译阶段行为：
  1. 检测到 InternalLinkage 符号引用
  2. 判断是否可转换为 STT_SECTION（大部分情况可以）
  3. 转换：符号索引改为 section，偏移加到 addend
  4. 结果：重定位记录精简，符号表可选精简

链接阶段行为：
  1. 解析 .o 文件：local 符号存入 ObjFile.locals[]
  2. 扫描重定位：local 符号标记为"不需要 GOT/PLT"
  3. 分配地址：OutputSection.addr + outSecOff 确定
  4. 应用重定位：直接写入最终地址到代码段
  
运行时行为：
  1. 无动态重定位需要解析
  2. 无 GOT/PLT 条目占用
  3. 地址直接可用
```

#### 18.7.2 Full LTO 场景

```text
IR 合并阶段行为：
  1. IRMover 复制 Internal 符号到 CombinedModule
  2. ValueSymbolTable 检测同名冲突，自动添加 .N 后缀
  3. IR 内符号引用使用 GlobalValue*，无重定位
  
代码生成阶段行为：
  1. CombinedModule → native object
  2. 生成标准 ELF 重定位（行为与非 LTO 一致）
  3. Internal 符号保持 InternalLinkage
  
后续链接行为：
  1. 与非 LTO 完全一致
```

---

### 18.8 设计思想总结

| 设计决策 | 行为体现 | 性质 |
|---|---|---|
| **静态确定性** | 地址在链接时完全确定 | 无运行时解析 |
| **最小符号表** | Local → STT_SECTION 转换 | 减少 .symtab 大小 |
| **索引定位** | 通过索引而非名字查找 | 跳过 name resolution |
| **无动态开销** | 不需要 GOT/PLT/动态重定位 | 运行时零成本 |
| **文件隔离** | 各 .o 文件的 local 符号独立 | 无跨文件决议 |

---

### 18.9 与非 Local 符号的对比

| 行为维度 | Local 符号 | Global 符号 |
|---|---|---|
| **符号表位置** | `.symtab` 前半部分（sh_info 边界） | `.symtab` 后半部分 |
| **重定位生成** | 可能转换为 STT_SECTION | 保持符号引用 |
| **链接扫描** | 不需要 GOT/PLT | 可能需要 |
| **地址计算** | 链接时确定 | 可能需要运行时解析 |
| **动态重定位** | **无** | 可能有（`R_*_RELATIVE` 等） |
| **符号决议** | **无**（索引定位） | 有（name resolution） |
| **运行时处理** | 无 | 可能需要动态链接器 |

---

### 18.10 代码定位索引

| 功能 | 文件 | 关键行号 |
|---|---|---|
| **编译阶段** | | |
| Local → STT_SECTION 判断 | `ELFObjectWriter.cpp` | 1358-1364 |
| 转换执行 | `ELFObjectWriter.cpp` | 1372-1374 |
| useSectionSymbol 条件 | `ELFObjectWriter.cpp` | 1247-1274 |
| **链接阶段** | | |
| 重定位扫描入口 | `Relocations.cpp` | 1333 |
| 重定位应用 | `X86_64.cpp` | 1246-1259 |
| 目标地址计算 | `InputSection.cpp` | 803-1003 |
| 符号地址计算 | `Symbols.cpp` | 66-134, 149 |
| Section 地址 | `InputSection.cpp` | 231-234 |
| STT_SECTION 处理 | `InputSection.cpp` | 480-527 |
| 符号索引定位 | `InputFiles.h` | 110-113 |

---

### 18.11 验证方法

```bash
# 编译阶段验证：查看重定位是否使用 STT_SECTION
clang -c test.c
readelf -r test.o
# 观察：r_symidx 对应的符号是否为 section 符号

# 链接阶段验证：查看是否无动态重定位
ld.lld test.o -o test
readelf -d test  # 查看 REL/RELA 条目（应无 local 符号相关）
readelf -S test  # 查看 GOT/PLT 大小（local 符号不占用）

# 符号表验证：查看 local 符号位置
readelf -s test
# 观察：local 符号在前半部分，sh_info 指示边界

# 地址验证：计算符号地址
readelf -S test  # 获取 OutputSection.addr
readelf -s test  # 获取 symbol.value
# 验证：addr + outSecOff + value = 最终地址
```

---

### 18.12 总结

**核心设计思想**：

| 设计决策 | 行为体现 | 性质 |
|---|---|---|
| **静态确定性** | 地址在链接时完全确定 | 无运行时解析 |
| **最小符号表** | Local → STT_SECTION 转换 | 减少 .symtab 大小 |
| **索引定位** | 通过索引而非名字查找 | 跳过 name resolution |
| **无动态开销** | 不需要 GOT/PLT/动态重定位 | 运行时零成本 |

**关键行为总结**：
- 编译时：Local 符号重定位转换为 STT_SECTION + addend，减少符号表大小
- 链接时：通过符号索引直接定位，跳过全局符号决议，直接写入最终地址
- 运行时：无动态重定位解析，无 GOT/PLT 占用，地址直接可用