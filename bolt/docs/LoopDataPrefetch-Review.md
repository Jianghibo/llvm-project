# Loop Data Prefetch 集成审阅与交付准备

当前代码已迁移到 LLVM 24 目标仓，复用、去重及验证边界见[迁移记录](LoopDataPrefetch-Migration.md)。下文原仓评估、提交及实验记录保留其历史含义。

记录 `feature-bolt-data-prefetch` 上自基线 `ee19000986fd`（LLVM 19.1.7）起的集成修复。本次审阅基于当时的未提交工作区，现已整理为下述提交；功能与支持矩阵见 [使用文档](LoopDataPrefetch.md)，阶段状态见 [TODO](LoopDataPrefetch-TODO.md)。

工程实现已具备继续代码评审的条件。真实 SPE 数据闭环、周期/延迟校准、候选组合收益和独立生产输入验收仍未完成，本次审阅不改变这些任务的状态。`no_lbr/-nl` 明确不支持。

## 本次修复

| 问题与触发条件 | 修复及关键回归 |
| --- | --- |
| store 写回递推直接读取偏移立即数，没有拒绝符号表达式 | 沿用 load 的拒绝规则，先检查 `isImm()`；扩展已有 `LoopPrefetchEncoding`，同时覆盖普通立即数和符号偏移 |
| SPE 属性解析 lambda 捕获结构化绑定，依赖 C++20 扩展 | 改用普通 `StringRef` 和已有 `std::tie` 写法；以当前 C++17 构建核对，不增加单测 |

## 跨模块核对

| 范围 | 核对结果与边界 |
| --- | --- |
| profile 输入与合并 | 扩展头中的 build-id、事件、period/config 由生成、读取、合并共用协议；身份及统计不兼容时拒绝。计数合并已有有符号范围限制，不另加饱和逻辑；直接 perf.data、YAML、BAT 和 `-nl` 不属于预取入口 |
| 参数与函数选择 | 统一使用 profile 驱动的自动距离，固定上限 1000，超过则跳过；沿用 Lite 热度筛选及显式函数选择。每轮周期优先使用 profile，缺失或有歧义时按 CFG 和 MC 调度模型静态估算 |
| 预算与提交顺序 | 先生成合法计划，估算潜在收益及执行成本，再按净收益稳定排序分配内部循环/函数预算；失败候选不占成功计划预算。按原始位置逆序提交，避免插入改变后续位置 |
| 寄存器与地址 | 在声明的 AAPCS64 范围内使用空闲 caller-saved，排除 CFI 引用寄存器；W/X 回绕、索引扩展、writeback 和 pair 复用目标接口。空闲不足时限定直线序列可按成本模型保存/恢复活跃 GPR，维护栈与 CFI；不支持的帧、guard spill 和访问跳过 |
| CFG/CFI 与生命周期 | 可选节流拆块保留标签、CFI、fragment 和原有后继统计，新增边使用估计频率、未知周期；变换后重建循环/支配分析。逐函数释放活跃性注解；拆块后的容量收缩避免累计保留源块存储 |

此表记录已核对的实现关系，不表示未列出的输入模式已经支持。多 latch 周期按逐循环静态成本估算；复杂地址切片和间接访存继续沿用现有拒绝边界；不为拆分补丁增加过渡 API 或新的分析框架。

## 审阅补丁分组

审阅时的补丁快照位于 `/tmp/bolt-prefetch-review/patches/`，相对上述基线生成，包含当时 tracked 和 untracked 的 39 个功能文件；不包含之后新增的最小示例和提交整理时的文档调整。`series` 指定应用顺序，`manifest.json` 保存文件归属和 SHA-256，`README.md` 保存依赖和应用核对结果。

| 顺序 | 补丁文件 | 审阅重点 |
| --- | --- | --- |
| 01 | `01-profile.patch` | 计数/周期元数据、SPE 协议、读取/聚合/持久化、merge-fdata 和共享开关 |
| 02 | `02-aarch64.patch` | MCPlusBuilder 目标接口、AArch64 地址及指令生成、活跃性存储与原始 offset 保留 |
| 03 | `03-prefetch.patch` | 直接变换、自动距离与排序、CFG 提交及 pipeline 接入 |
| 04 | `04-tests.patch` | Core/Profile 单测、AArch64 正确性用例及静态开关/profile 用例 |
| 05 | `05-tools.patch` | profile 驱动的实验与结果归档 |
| 06 | `06-docs.patch` | 支持范围、使用方法、阶段清单及本审阅记录 |

这些是按文件划分的审阅分组，不能直接视为六个独立可构建的提交。01 修改周期元数据接口，03 的消费者同步迁移；02 提供 03 所需的目标接口。01～03 必须一起应用和构建。04 依赖 01～03，05 的静态用例复用 04 的汇编，06 描述完整结果。

实际提交将 01～03 和 04 合并为一个实现提交，随后提交工具及其回归、最小示例，最后提交文档。没有按审阅文件分组拆断共享接口，也没有为拆分增加过渡 API。

## 提交记录（2026-10-26）

| 提交 | 内容 |
| --- | --- |
| `d63bd1b4a516` | 核心实现及关键回归，32 个文件；SPE 协议/统计、AArch64 目标接口、循环变换、全局参数、自动距离及 pipeline 同步提交 |
| `bf00a6eec526` | 逐 PC 调优与离线规划的历史记录；当前已移除该功能 |
| `1bf7e9d1cf69` | 最小指令插入示例，3 个文件；原始/普通 BOLT 无预取，手工 D=8 后实际发射一条 `prfm pldl1keep, [x0, #64]` |
| 随本记录提交 | 使用文档、阶段清单和集成审阅记录，3 个文件 |

本次只创建本地提交，没有推送远端。提交整理保留已有实现和验证结论，最小示例的复现命令及反汇编见 [示例记录](../examples/loop-data-prefetch/README.md)。

## 本次验证

本轮只执行宿主工具、编译、单元测试和静态 ELF 重写，未执行生成的目标程序、benchmark、perf 或 SPE 采集。

| 检查 | Assertions ON | Assertions OFF |
| --- | --- | --- |
| `bolt-test-depends CoreTests ProfileTests` 构建 | 通过 | 通过 |
| Core 单元测试 | 17 通过，8 按架构跳过 | 17 通过，8 按架构跳过 |
| Profile 单元测试 | 15 通过 | 15 通过 |
| 四项静态 lit | 4 通过 | 4 通过 |

静态 lit 为 `AArch64/loop-data-prefetch-plan.test`、`AArch64/loop-data-prefetch-options.s`、`X86/loop-data-prefetch-options.s` 和 `X86/prefetch-profile.test`。宿主 Core 单测覆盖符号写回偏移的拒绝路径；规划用例包含不运行 Pass 的失败路径及固定合成噪声数据，不把合成数据当作测量结果。

两组沿用 `/tmp/bolt-prefetch-build`、`/tmp/bolt-prefetch-noassert` 的 Release 配置和当前源码工具；OFF 组的 Clang/LLD 来自 ON 构建。日志在 `/tmp/bolt-prefetch-review/` 的 `{assert,noassert}-{build,core,profile,lit}.log`。C++20 捕获警告已消除；现有 X86 头文件及 OFF 链接器参数警告未纳入本功能修复。

补丁按顺序应用到仅含 HEAD 原始文件的隔离目录，并逐文件比较内容及哈希；结果记录在补丁包中。此检查证明补丁完整性，不证明中间分组可独立构建。Python 语法、Markdown 本地链接及 diff 空白检查与补丁核对一并归档。

未重跑会执行目标程序的完整 BOLT 套件；历史完整回归及四项基线失败见 TODO 8.8。本轮没有补充真实数据或收益证据，后续仍需目标硬件及生产程序才能完成相应验收。
