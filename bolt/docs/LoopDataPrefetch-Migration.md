# Loop Data Prefetch 迁移记录（2026-10-28）

本次将 `gitee-llvm-jhb/llvm-project` 的合并提交 `80d4bd0643d1` 迁移到 `github-llvm-jhb/llvm-project` 的 `feature-bolt-loop-data-prefetch` 分支，目标基线为 `a938d863c4f2`（LLVM 24.0.0git）。原仓基于 LLVM 19.1.7；原有评估及实验文档保留为历史记录，不能视为目标版本已完成相同硬件或性能验收。

## 复用与清理

| 范围 | 迁移处理 |
| --- | --- |
| AArch64 寄存器分析 | 复用目标仓的 SP/FP/NZCV 查询、GP 枚举、callee-saved/default-live-out 集合、`isCleanReg` 和不可分配寄存器排除；不覆盖为旧分支的另一套实现。预取 Pass 仍额外排除 CFI 引用寄存器 |
| AArch64 指令辅助 | 复用已有 `isADD`、`isLDRD`、`isLDRQ`，去掉重复定义；移除没有消费者的 `isCMP` 公共接口、X86 的配套修改及旧的 `createPrefetchMui`/`createADD64rs` 接口 |
| 循环遍历 | 使用已有循环接口，移除旧分支增加但当前变换不需要的 `BinaryLoop` GraphTraits 和注释掉的构造函数声明 |
| 指令原始 offset | 保留目标仓的 DWARF scope 边界和控制流指令处理，额外在启用预取时保留 load offset；不恢复旧版 `BinaryContext::keepOffsetForInstruction` 路径 |
| miss 绑定 | 统一使用已有 MCInst 注解设施中的 `CacheMissProfile`；移除未参与有效绑定的基本块 miss 映射，`--print-cache-miss` 也读取实际注解 |
| 分支与 SPE 解析 | 保留目标仓已有的 `pid,brstack` 布局、SPE `PN/MN` 标记、返回分支识别、内核中断过滤及解析框架，只增加周期字段读取和重复分支区间统计；不引入旧的 event-PC 参数和双地址聚合表 |
| profile 聚合 | 周期附加在目标仓的 `Trace{Branch, From, To}` 统计上，复用多输入及三地址 trace 处理；不恢复旧版聚合器的进程启动、清理、预聚合解析及函数计数逻辑 |
| 入口计数与基本样本 | 复用 `FuncBranchData::setEntryCounts`、`BasicSampleInfo` 和已有 symbols 模式，去掉旧分支重复保存的函数入口计数字段 |
| profile 合并 | 保留新版 `merge-fdata` 的流式 legacy 解析、branch/memory 分开归约、no_lbr 事件名及损坏记录处理；增加 预取扩展入口和格式混合拒绝 |
| pipeline | 保留目标仓的 PointerAuthCFIAnalyzer 等既有 Pass 顺序，将预取 Pass 接入已有循环变换位置 |

迁移保留直接访存、基础地址切片、一步指针追逐、显式启用的带 guard 间接访存及父循环批量预取，支持矩阵见[使用文档](LoopDataPrefetch.md)。这些能力仍默认关闭。

目标仓增加了 `--basic-events`/`--ba` 并保留已废弃的 `-nl` 参数。本特性对这些基本采样模式均拒绝；普通 BOLT 的原有基本采样能力保持不变。SPE cache-miss 转换仍要求原始二进制身份、合法采样属性及分支记录；仅有 memory 信息的输入不在支持范围内。

多输入 SPE 转换要求所有输入解析成功，且事件、build-id、采样周期和 config 属性一致；不采用普通聚合器对部分坏输入的宽松继续策略。循环周期没有可靠输入时仍跳过自动距离，不将 SPE 采样周期当作延迟。

## 验证范围

验证使用目标仓代码构建，限定为宿主工具、Core/Profile 单元测试及静态 ELF 重写。两个既有预取 lit 中的目标执行命令在 `/tmp` 副本中去掉，仓库测试文件中的运行覆盖仍保留。真实 perf/SPE 采集、目标程序执行和 benchmark 均不在本次执行范围。

迁移和验证产物保存在 `/tmp/bolt-prefetch-migrate/`。本次未推送远端；原仓提交和目标仓既有的无关未跟踪文件保留。

## 验证结果

目标仓现有 `build/` 使用 RelWithDebInfo、断言开启配置，BOLT 包含 AArch64/X86/RISCV 目标。`llvm-bolt`、`merge-fdata`、`merge-fdata-for-test`、`CoreTests` 和 `ProfileTests` 均构建通过；最终增量构建没有迁移代码警告。

| 检查 | 结果 |
| --- | --- |
| Core 单元测试 | 71 通过，74 按架构条件跳过 |
| Profile 单元测试 | 53 全部通过，包含目标仓原有 SPE、预聚合和 perf-script 解析覆盖 |
| 特性静态 lit | 7 全部通过，覆盖基本/间接/批量、开关、离线规划和 profile 协议 |
| 两个既有用例的静态副本 | 2 全部通过，含非 PIE/PIE/DSO 静态重写及模拟 perf 输出到 fdata 的转换 |
| 目标仓原有 profile/merge-fdata lit | 20 全部通过，含多输入聚合、基本采样、BAT 及 legacy 格式 |
| 合成 profile 到最终机器码 | 直接 D=4/1 条预取，间接 D=4/1 条预取，父循环批量 D=2/2 条预取，均通过 |
| 合成反例 | build-id 不匹配拒绝；缺周期时不插入自动预取 |
| miss 打印 | `--print-cache-miss` 从绑定的 MCInst 注解打印 8 次 miss、10 次 access |

为适配新版测试工具，新增 `%llvm_bolt` 原始路径替换，避免脚本的 `--bolt` 参数被 lit 自动追加运行库选项；固定地址示例显式设置 LLD image-base；mock perf 的 mmap 对应实际可执行段，子进程 PATH 由 fixture 明确设置。临时寄存器的检查使用 FileCheck 捕获其关系，保留新版 ABI 活跃性分析的分配结果，没有改回旧版寄存器集合。

日志为 `build-verified.log`、`build-complete.log`、`core.log`、`profile.log`、`feature-lit.log`、`static-lit.log`、`upstream-lit.log` 和 `print-cache-miss.log`。最终合成 profile 示例产物在 `/tmp/bolt-prefetch-migrate/profile-e2e-final/`，`report.json` 保存原始 PC、距离、最终指令、完整 BOLT 命令及工具 SHA-256；`migration-files.json` 列出迁移涉及的 54 个文件。

可在目标仓根目录独立复现三组静态示例，输出目录应不存在：

```sh
python3 bolt/examples/loop-data-prefetch/run-profile.py \
  --tools build/bin --output-dir /tmp/loop-prefetch-llvm24-demo
```

本次迁移作为目标分支上的一个独立提交保存。上述结果证明在目标代码版本上的构建和静态功能通路通过；真实 SPE 数据质量、目标程序运行正确性和性能收益仍待目标环境验收。
