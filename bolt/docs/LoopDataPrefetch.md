# Arm SPE 引导的 Loop Data Prefetch：使用与支持范围

当前代码已迁移到 LLVM 24 目标仓，复用、去重及验证边界见[迁移记录](LoopDataPrefetch-Migration.md)。下文原仓评估、提交及实验记录保留其历史含义。

本分支为 AArch64 BOLT 提供直接循环预取变换、一步指针预取、索引数组间接预取及常量内层循环的外层批量预取。功能默认关闭，消费非 `-nl` 的 `perf2bolt` 生成的扩展 fdata，按 profile 周期或逐循环静态成本自动计算距离。

当前已实现 P1 的直接变换、配置、请求节流和实验归档能力；LLVM 24 补充修复隐式 X8 参数的活跃性遗漏，并增加间接预取的 guard page 运行回归，验证范围见 [TODO 8.16](LoopDataPrefetch-TODO.md#816-llvm-24-正确性修复与运行回归2026-11-03)。旧版本的受控扫描和处理开销记录保留其历史含义。生产负载尚未提供，P1 的生产性能验收保持待完成；P2 的身份、协议、计数、绑定及合并代码和固定输入回归已完成，真实硬件闭环验证按用户要求暂缓；P3 已提供内部模型驱动的距离预测、预算内候选排序。完整 SPE 商用交付仍依赖真实输入校准、单点/组合和独立生产负载的收益验收。

批量边界/CFI、地址切片和一步指针的运行验证，以及批量行指针复制、直线多块间接访问的限定扩展，见 [TODO 8.17](LoopDataPrefetch-TODO.md#817-批量与地址访问运行验证及限定扩展2026-11-03)。

## 支持矩阵

| 项目 | 支持范围 |
| --- | --- |
| 平台 | AArch64 Linux 用户态 ELF，遵守 AAPCS64；静态非 PIE、PIE、DSO 沿用普通 BOLT 的符号与重定位要求 |
| CPU 指令 | 基础 AArch64 PRFM/PRFUM 及标准 L1/L2/L3 keep/strm hint；不生成依赖可选 CPU 扩展的预取指令 |
| load | 普通整数、符号扩展和标量/SIMD 的 LDR/LDUR；W/X/S/D/Q 及 LDPSW 的 LDP |
| 寻址 | base+立即数、X 索引、W 索引 UXTW/SXTW、按访问宽度缩放、pre/post-index；符号重定位位移受控拒绝 |
| 递推 | 地址 base 和 index 分别为循环不变量，或由同宽度 ADD/SUB 立即数自更新、普通 load/store writeback 更新；支持 ADDS/SUBS、立即数 `lsl #12`、正负步长及展开后的多次更新 |
| 地址表达式 | 64 位 MOV、ADD/SUB 立即数或带 LSL 的寄存器运算、LSL；最多跟随 4 层赋值，每层最多 32 个指令/块切换步骤，可沿循环内唯一前驱跨块，遇合流或 loop header 边界停止；叶子仍须满足上述递推证明 |
| 一步指针追逐 | 普通 64 位单元素 load，结果直接写回 base 或经一次 64 位 MOV 更新 base；base 每轮只有一次更新，next 寄存器只由该 load 定义；load 和更新均支配全部 latch；仅 D=1，在原 load 后预取 next 节点的同一字段，不新增解引用 |
| 索引数组间接预取 | `--loop-prefetch-indirect` 单独启用；单块或无分叉的直线多块、非零入口检查、32/64 位倒计数循环，无 store/调用/提前退出/SP 更新；32/64 位索引 load 驱动稳定 base 的寄存器索引 load，所有新增普通 load 受剩余迭代数 guard 保护 |
| 外层批量预取 | `--loop-prefetch-batch` 单独启用并显式指定缓存行大小；父循环仅含入口、单块常量内层及 latch，内层 32/64 位正常量倒计数；连续 base-only 标量/pair 访问，可在父入口通过一次 64 位 MOV 从行基址设置内层指针；进入内层之前预取 D 次父循环之后的整段范围，仅生成地址运算及提示 |
| W/X 语义 | W 索引先按 32 位回绕，再执行 UXTW/SXTW；X 地址按 64 位机器算术处理；混合宽度定义不能证明时拒绝 |
| 循环 | 单块和常见跨块的最内层可归约循环；移除回边后单次迭代无环；每次递推更新必须支配所有 latch，可有条件 load 和提前退出 |
| 寄存器与 CFI | 优先使用空闲 caller-saved GPR，限定直线路径可保存/恢复最多两个活跃 GPR；允许 callee-saved base；保持原地址输入、LR、NZCV 及原有 CFI 引用寄存器，临时栈空间保持 16 字节对齐并提供 CFI |
| 输入 profile | 启用预取要求 扩展 fdata，入口与限制见 [TODO 的数据通路约定](LoopDataPrefetch-TODO.md#13-profile-输入边界与通路状态) |

不支持 `no_lbr/-nl`。普通 BOLT 对不支持架构的行为保持不变；本 Pass 只在 AArch64、非 instrumentation 流水线中运行。

以下情况明确跳过：未知控制流、多入口函数、循环内 call/return/间接跳转、landing pad、独占/原子/acquire-release/屏障上下文、含 opaque CFI escape 的函数、SVE/SME 及未列入解码矩阵的地址。包含新增普通 load 的依赖链仅限下述受 guard 保护的索引数组模式。唯一前驱链之外的合流定义、跨迭代定义、W/X 混合定义和一般算术无法证明时仍拒绝。

普通 volatile load/store 和 relaxed atomic 在二进制中未必有独立标记。直接和一步指针路径不新增普通 load/store；间接路径会新增一次受 guard 保护的索引读取，因此只应对普通、非 volatile、非 atomic 的索引缓冲区启用，并要求缓冲区在循环期间保持有效、不并发改变映射。支持范围面向普通内存，不承诺优化设备内存或内存映射 I/O。

建议输入在链接时保留 `--emit-relocs`。非重定位模式仍受普通 BOLT 的原函数空间限制，不能容纳增长的函数可能保留原实现；应核对最终输出及 BOLT 的发射诊断。

完整候选计划在修改前构建；失败不会留下部分序列。直接地址路径在原 load 前插入算术及预取，一步指针路径在原 load 后插入。启用直接地址的请求节流时，在该位置拆出预取块及合流块，原 load 及其后续指令位于合流块。原分支的计数和周期随 terminator 移动，新分支仅有估计计数，周期保持未知。新块继承该位置的 CFI、fragment 和地址映射；保留移动指令的标签，更新布局并重建支配树和循环信息。提交前清除临时活跃性 annotation，并按函数释放其分配空间，拆块后释放原块多余的指令容量。原 CFI 规则及其依赖寄存器在序列中保持有效。

## SPE profile 转换与诊断

沿现有 `perf2bolt → fdata → llvm-bolt` 通路使用。采集原始二进制应带 build-id 并保留重定位；转换和重写都使用同一份原始文件。

合格的 perf.data 必须包含可供非 `-nl` 通路读取的分支记录，以及单个 Arm SPE load 采样事件。分支与 SPE 各有独立来源；默认分支解码采用 `--no-itrace`，避免把 SPE 合成的 branch-miss 事件误当成 brstack。需要其他 trace 解码时，仅在已验证的来源上显式设置 `perf2bolt --itrace=...`。分支记录缺失时拒绝转换；有分支但没有有效周期时，自动距离按 CFG 和指令成本静态估算，并标记来源。

SPE 协议要求固定采样周期、`load_filter=1`、关闭 store/branch 及 event/min-latency 过滤；允许时间戳、地址/上下文包及 jitter 的相关配置位。`perf2bolt` 从 `perf evlist -v` 获取实际参数，拒绝多个 SPE 事件、频率模式、缺失字段和未支持配置。普通 `perf record` 事件若与 SPE 一并记录，应按目标 perf 要求保留 ADDR 字段（通常使用 `-d`），否则 memory 解码可能报缺少 ADDR；不能忽略这一失败。

本机已能执行以下 SPE 单独采集命令，它用于核对访存事件，**不能单独作为符合非 `-nl` 要求的完整输入**：

```sh
perf record -o spe.data --no-buildid-cache \
  -e arm_spe_0/load_filter=1/ -c 1024 -- taskset -c 24 ./bench strided 64 100
perf evlist -v -i spe.data > spe-attributes.txt
perf script -i spe.data --itrace=fe -F pid,event,addr,ip \
  > spe-l1.txt 2> spe-decode.log
perf buildid-list -i spe.data > spe-buildids.txt
```

按目标 CPU 调整 PMU 和 CPU 绑定。完整采集命令须在具备分支数据来源的目标环境核实；当前机器的硬件 `-b` 不受支持，SPE 上下文包也遇到权限限制，尚未完成该验收。不要用 `-nl`、空 brstack 或手工伪造周期绕过。

对已有的合格输入进行转换和重写。每轮周期从有效的单 latch profile 间隔计算，缺失或有歧义时使用内部默认值：

```sh
perf2bolt app -p combined.perf.data --cache-miss=llc_miss -o app.fdata
llvm-bolt app --data=app.fdata -o app.prefetch --enable-loop-data-prefetch \
  --loop-prefetch-diagnostics --verify-cfg -v=1
```

`--cache-miss` 支持 `tlb_miss/l1d_miss/llc_miss`，分别显式选择 `--itrace=te/fe/me` 的完整 access/miss 事件组及解码错误输出，并请求丢样记录。计数只使用所选层级：miss 不额外加入 access 分母，其他层级不混入。SPE 解码 stderr 会转发；格式/计数错误、缺失事件或分支记录使转换失败。上下文匹配等警告仍需结合原始数据复核，命令成功不等于硬件采集已验收。

预取扩展头部示例：

```text
bolt-prefetch llc_miss 11223344 1024 200000001 0 0
```

标记之后的字段依次为事件、原始 build-id、十进制采样周期及三个十六进制 config 值；period 是 SPE 采样间隔，不是 CPU 周期或 load 延迟；完整记录格式见 [TODO 1.5](LoopDataPrefetch-TODO.md#15-p2-的身份与采样协议)。采集时的身份校验不能用 `--ignore-build-id` 绕过；读取 fdata 时再次比较输入 build-id。不能手工补头宣称已经验证采集参数。`--pa`、YAML、BAT 和 `-nl` 均不属于该转换入口。

```sh
merge-fdata run1.fdata run2.fdata > merged.fdata
```

仅合并 build-id、事件、period/config/config1/config2 一致的文件；不兼容或溢出返回失败。miss/access 按计数求和，周期保留总和和有效区间数，不平均文件均值，也不折算不同采样周期。

读取时输出 load 绑定数、非法指令 PC、非 load 和未匹配函数记录数；`-v=1` 给出失败位置及原因。启用预取而所有访存记录均无法绑定时返回失败。Pass 诊断附带原始 PC、miss/access 计数、实际距离，以及计数无效、低于 miss 比例阈值或超出预算等原因，并区分 profile 周期和静态估算周期。数据 VA 保持独立于代码 PC 的重定位，不将堆地址按代码 DSO 的 load bias 解释。

随采集结果保留原始二进制及其身份、perf.data、CPU/SPE 能力、缓存行、内核/perf 版本、完整命令、负载/输入、采集窗口、解码诊断、重复采集分布和采集开销。环境信息使用随附记录，fdata 只保存身份及实际影响计数解释和合并的参数。固定输入回归覆盖解码文本到最终插入及非 PIE/PIE/DSO 映射；真实硬件闭环验收暂缓，不能由该回归替代。

## 启用预取

可先运行[最小指令插入示例](../examples/loop-data-prefetch/README.md)，无需 SPE 数据即可检查原始、普通 BOLT 和启用预取后的机器码。

从原始输入二进制重新运行 BOLT：

```sh
llvm-bolt app -o app.prefetch --enable-loop-data-prefetch \
  --data=app.fdata --loop-prefetch-hint=pldl1keep \
  --loop-prefetch-diagnostics --verify-cfg
```

默认 `D` 是包含候选的该层自然循环的迭代数，距离由模型计算，内部上限为 1000，超过则跳过，不截断。启用批量模式后，被提升的内层候选改以直接父循环迭代数解释 D，详见 P4-03。循环展开后的每次回边对应一个迭代，不能把它当作一个原始数组元素。base/index 的每轮更新分别累计，再乘以 D；64 位索引可将增量折叠到字节位移，32 位索引保留回绕后扩展的顺序。

例如每轮 X 索引增加 2、load 使用 `lsl #3` 时，`D=4` 对应 64 字节提前量。pre-index 使用更新后的访问地址，post-index 使用更新前的访问地址。成对 load 默认保留两个元素的地址，生成两条预取；启用下述缓存行节流时合并为一条。地址计算与原 load 同路径执行；零次循环不会新增循环外预取，短循环和尾部可以产生指向后续迭代的预取提示，不会为此增加实际 load。

候选必须具有有效的分支及 cache miss profile；无有效周期时按 CFG 和指令成本静态估算，无法建模则跳过。沿用 Lite 热度筛选及显式函数选择、忽略规则。`--print-loop-data-prefetch` 仅打印 Pass，不能代替 `--enable-loop-data-prefetch`。

## 基础地址切片和一步指针追逐

地址切片只推导临时寄存器在相邻迭代间的增量，保留原有赋值，不复制指令。例如：

```asm
add  x8, x0, x1, lsl #3
// D=4、x1 每轮加 1 时，插入 prfm pldl1keep, [x8, #32]
ldr  x3, [x8]
add  x1, x1, #1
```

每次查询都使用该定义之前的源寄存器状态，遇到覆盖优先检查最近定义。跨块时只沿循环内的唯一前驱查找，在合流及 loop header 边界停止，不把上一轮的定义当作当前定义；部分寄存器写入和未支持表达式会拒绝。寄存器 SUB 使用带符号的步长差，例如 `sub x8, x0, x1, lsl #3` 且 x1 每轮加 1 时，D=4 对应 `[x8, #-32]`。

该路径复用当前地址和推导出的增量，仍支持原有 W/X load 寻址、预算、去重及可选缓存行节流，不引入一般路径切片或合流框架。

一步指针追逐只复用原程序已经加载的 next 指针：

```asm
ldr  x0, [x0, #8]
// D=1 时，在这里插入 prfm pldl1keep, [x0, #8]
cbnz x0, loop
```

还支持常见的双寄存器形式：

```asm
ldr  x5, [x0, #8]
// D=1 时，插入 prfm pldl1keep, [x5, #8]
// 原程序继续使用 x0 处理当前节点。
mov  x0, x5
cbnz x0, loop
```

限单元素、64 位、无 index/writeback 的普通 load。它必须支配全部 latch，next 寄存器在循环内只能由该 load 定义；base 的唯一写入必须是原 load 或后续的一次完整 MOV，更新也须支配全部 latch。跨块 MOV 必须被该 load 支配，同块时必须位于 load 之后，部分覆盖和其他更新均拒绝。load 后插入时使用该位置的活跃性，并把紧随 load 的 CFI 规则保留在预取之前，保持原寄存器值和新增 PC 的展开状态。

预取的是下一次迭代将读取的指针字段，next 为空时也只发出原有支持范围内的预取提示，不进行实际解引用。多步提前及 `a[index[i+D]]` 的提前普通 load 不在此路径中。

两类路径均要求合法 profile，并使用内部模型计算距离。一步指针追逐只接受模型得到的 D=1，D>1 不截断为 1。它不使用固定字节步长节流，仍计入共享请求及指令预算，诊断用 `placement=after-load` 区分插入位置。这些能力的指令插入不代表已经取得性能收益。

## 索引数组间接预取（P4-02）

`a[index[i]]` 需要先获得未来索引，再形成数据地址。这条路径与一步指针追逐不同，会增加一次普通索引 load，默认关闭：

```sh
llvm-bolt app -o app.prefetch --enable-loop-data-prefetch \
  --loop-prefetch-indirect --data=app.fdata \
  --loop-prefetch-max-instructions=12 --loop-prefetch-diagnostics --verify-cfg
```

支持如下倒计数模式；循环可以是单块，也可以是从 header 到唯一 latch 的直线多块链。入口须以同宽度 CBZ/CBNZ 排除零次循环，尾部须为同一计数器减 1 后的非零回边：

```c
if (remaining != 0) {
  do {
    j = *indices++;
    result += a[j];
  } while (--remaining != 0);
}
```

识别最近的索引定义，支持普通 LDR/LDUR 的 32 位、64 位和符号扩展 32 位索引；保留消费者的 UXTW/SXTW、64 位索引及缩放语义。源索引地址可使用直接仿射 base/index 或 pre/post-index；目标数据 base 必须循环不变。多块链的中间块须有唯一前驱和唯一后继，仅 latch 可回 header 或退出；从消费者向前最多检查 32 个指令/块切换步骤，不跨越 header 查询上一迭代。索引 load 与消费者之间不得改写源地址输入或索引值，消费者须位于计数器更新之前。含 store、调用、分叉、提前退出、计数器额外更新或 SP 更新的循环拒绝，不新增通用别名分析。

仅当 `remaining > D` 时执行新索引 load 及数据预取。对于 post-index、每轮 4 字节的源流，原索引 load 已使指针前进，因此 D=4 对应新读取 `[indices, #12]`，即原始 `index[i+4]`，不是 `index[i+5]`。未来数据地址仅用于 PRFM，不新增对 `a[index[i+D]]` 的普通 load；原索引 load、数据 load 和 writeback 均保持原行为。

guard 对 32/64 位无符号计数使用不修改 NZCV 的 SUB/BIC/TBNZ。令 `K=D+1`，由于 K 小于符号位，`(remaining-K) & ~remaining` 的最高位可精确判断减法借位，包括 remaining 最高位为 1 的情况。余量不足时整个新访存块均跳过，空循环仍由原入口分支跳过。

新路径复用空闲 caller-saved、CFI 排除和现有 CFG 拆块；地址计算、新索引 load、guard、PRFM 及跳转全部计入原预算。不满足预算或寄存器条件时拒绝，不 spill。简单源流通常可使用默认候选预算 8；需要额外物化源地址时，可通过命令行 显式增加预算。可选缓存行节流仍用于直接索引流的预取，间接数据地址不套用固定字节步长节流；同一循环中的两类计划共享预算。

同样支持合法预取 profile 和内部距离模型，诊断标记 `kind=indirect`。静态指令、guard 算术和 profile 入口已有回归；LLVM 24 的限定用例已通过零次、短循环、尾部和 guard page 运行检查，见 [TODO 8.17](LoopDataPrefetch-TODO.md#817-批量与地址访问运行验证及限定扩展2026-11-03)。真实负载收益仍待验收。

## 常量内层循环的外层批量预取（P4-03）

对于每次父循环都完整执行固定次数内层访问的模式，可将预取放在父循环入口、进入内层之前，每次父循环发出一批提示：

```sh
llvm-bolt app -o app.prefetch --enable-loop-data-prefetch \
  --loop-prefetch-batch --data=app.fdata \
  --loop-prefetch-cache-line-size=64 --loop-prefetch-batch-max-bytes=256 \
  --loop-prefetch-diagnostics --verify-cfg
```

当前只接受“父入口 → 单块内层 → 父 latch”的三个块、单个子循环结构。内层计数须由入口同宽度 MOVZ 初始化为正值，尾部为 SUBS 1 后的非零回边，且没有其他计数器定义；不从 profile 内外回边之比猜测 trip count。动态次数、空/条件内层、提前退出和更复杂结构拒绝。内层基址必须是可证明的同宽度自更新，净步进绝对值等于标量或 pair 的实际读取宽度。允许父入口通过一次 64 位 MOV 从另一行基址设置内层指针；行基址须有可证明的父迭代递推，内层之外不得再改写遍历指针。W 部分覆盖、重复复制、从内存读取或用其他表达式重建行指针、寄存器索引仍拒绝。父循环整体仍受已有调用、原子、未知副作用和 CFI 限制。

例如父入口执行 `mov x6,x0`，内层用 `ldr x4,[x6],#8` 读取四个元素，父 latch 执行 `add x0,x0,#64`；在复制和内层计数初始化之后插入批量预取。D=1 时预取 `[x6+64,x6+95]`，父步长来自 x0 的 64 字节递推，不使用内层 x6 的 32 字节净步长，也不改写原 MOV 或指针更新。

例如每个内层执行 4 次 `ldr x4, [x0], #8`，父 latch 不额外修改 x0，则每次父循环推进 32 字节，D=2 对应未来行的 `[x0+64, x0+95]`。进入内层前生成 `prfm pldl1keep, [x0,#64]` 和 `prfum pldl1keep, [x0,#95]`；内层回边仍直接回到原 load，不重复执行整批提示。父循环中的额外立即数更新、反向遍历和原 load 前的指针更新均计入地址换算。

在未知缓存行对齐时，按范围起点、逐行位置及末字节覆盖整段，避免漏掉首尾跨行部分。两条不同地址的提示可能落在同一实际缓存行，不宣称已证明对齐或完成动态去重。请求数为 `ceil((span-1)/line)+1`，按请求数乘所指定行大小计费。`--loop-prefetch-batch-max-bytes` 默认 256，限制同一父循环所有批量计划的合计请求字节；它是资源上限，不是硬件 cache 容量模型。另受共享请求、候选/循环/函数指令预算限制，不能完整容纳时拒绝整批，不截断范围。

D 的单位为直接父循环迭代数；自动距离使用该父循环有效回边周期；缺失有效周期时静态估算父循环，并计入已证明的内层次数。请求了批量模式但结构或地址不支持时，不退回以内层迭代为单位的 D；非嵌套候选保持原路径。该模式不批量执行间接路径的新索引 load。

所有候选先完成计划，再共享预算并提交；临时寄存器按实际父入口位置的活跃性选择，沿用 CFI 排除，不复制或重排原 load。无 guard 的批量序列可使用下述受限 spill 回退；不会新增数据解引用，只可能增加临时寄存器的栈保存/恢复；原父循环零次入口检查保持不变。

诊断标记 `kind=batch`、`placement=outer-entry`、`distance-unit=outer`，并给出内层次数、请求数、指定行大小及请求字节数。限定用例已完成边界结果和新增预取 PC 的运行展开检查，见 [TODO 8.17](LoopDataPrefetch-TODO.md#817-批量与地址访问运行验证及限定扩展2026-11-03)；相对内层预取的性能收益仍待目标负载验收。

## 自动距离与候选选择

自动距离使用 `D = ceil(LatencyToHide / CyclesPerIteration)`。默认使用包含 load 的自然循环，批量候选使用其直接父循环。

当前 BOLT 创建 generic AArch64 子目标，没有具体目标 CPU 型号或可直接复用的 cache miss 时延模型。暂用内部缺省延迟 **200 CPU cycles**，不把普通 load 的缓存命中调度延迟当作 cache miss 时延，也不使用运行 BOLT 的宿主 CPU 冒充目标 CPU。该值是暂定估计，不是某款 CPU 的实测值；以后可由目标模型替换，不增加手工延迟参数。

每轮周期优先使用单 latch 循环有效 profile 的 `CycleTotal / Samples`。缺少周期、周期统计无效或为零、或者多 latch 区间有歧义时，按该循环静态估算，不设置统一的每轮周期：

- 基本块成本复用当前 MC 子目标的调度模型，考虑微操作数、发射宽度、执行资源、寄存器依赖延迟及保守的访存顺序；CFI 不计入指令成本。
- 去除回边后沿 CFG 计算能完成迭代的路径。分支具有完整、正值且非推导的计数时加权，否则取较长路径；不把互斥分支的成本直接相加，也不计入只离开循环的路径。
- 批量模式估算直接父循环，按已证明的常量次数累计内层块成本。
- 调用成本包含被调函数，不能按一条普通指令计算。当前变换仍拒绝含调用的循环；缺少可用调度信息或存在其他无法建模的指令时，不生成预取计划。

静态结果以 `Total=估算周期, Samples=1` 参与距离计算，不回写 profile。它是基本块成本与路径成本的近似组合，不模拟跨块/跨迭代重叠、cache miss 或分支预测；当前使用 generic 子目标，不表示实际目标 CPU 的实测周期。分支记录、cache miss/access 计数和输入身份校验要求保持不变。

不设置绝对样本数量门槛。候选要求访问计数非零、miss 计数不大于访问计数，且 miss 比例超过阈值。

使用 profile 时保留精确分数，通过宽整数计算 `ceil(200 * Samples / CycleTotal)`，不先截断均值，也不溢出乘积。例如总周期为 11、样本数为 2 时，D=37；静态估算为 25 cycles/iteration 时，D=8。这些是模型计算结果，不表示性能收益。

距离统一自动计算，固定上限为 1000；可关闭 `--enable-loop-data-prefetch` 禁用本特性。同一输入 profile 的候选先通过下述收益判断，再按估算净收益排序；去重只保留可证明等价的请求，不累加重复 miss 统计。miss 数不能解释为可消除的 stall cycles。

`--loop-prefetch-diagnostics` 输出 `latency-cycles`、`latency-source`、`iteration-cycle-total`、`iteration-cycle-samples`、`cycles-source` 和最终 D。当前延迟来源为 `default`，每轮周期来源为 `profile` 或 `static`，静态估算还输出 `timing-cpu`。请求数、指令数及寄存器合法性仍限制所有计划，超出最大距离的预测给出拒绝原因。

## 候选收益与执行成本

距离模型仅计算提前多少轮；完整生成 `PrefetchPlan` 后，再比较潜在收益与实际新增序列的估算成本。普通候选按一次 load 执行计费，批量候选按一次父循环迭代计费；不增加模型参数。

- 复用 MC 调度模型估算地址计算、预取、guard、分支和新增索引 load 的依赖延迟及执行资源成本。生成提示指令的副作用标记不影响使用其调度信息；原循环中的未知副作用仍拒绝。
- 对缓存行节流，始终执行的地址计算和 guard 全额计费，请求路径按 `abs(stride)/line_size` 加权。间接 guard 的通过率未知，按请求路径全部执行保守计费；两个 CFG 跳转均计费，不依赖后续分支整理消除跳转。
- 间接预取须有原索引 load 的有效 miss/access 计数。其命中延迟已在序列成本中，只额外计入 miss 带来的估算延迟；缺少该统计时跳过，不将新增普通 load 当作免费操作。
- 可隐藏延迟为 `clamp(D * iteration_cycles - request_ready_cost, 0, latency)`。潜在收益由 miss 比例乘以该延迟得到；节流和批量模式还限制可覆盖的 miss 数。有效测量周期存在时，收益不超过原迭代时间；静态周期不包含 cache miss，不能据此套用同一限制。
- 只接受潜在收益严格大于执行成本的候选。按 `净收益 * access_count / 内层次数` 排序，非批量模式的内层次数为 1；相同分数依次按 miss 数和较少新增指令决定稳定顺序。该权重仅在同一采样协议内比较，不把样本数当成实际执行次数。

使用空闲寄存器本身不扣分。诊断中的 `scratch-registers` 是实际生成序列写入的临时寄存器数量，不是成本系数。生成器最多需要两个临时 GPR；空闲不足时，仅对单块内无 guard 的序列尝试借用活跃 caller-saved，排除地址输入、保留寄存器及原有 CFI 引用寄存器。不保存未被生成代码实际改写的寄存器。

spill 回退限制如下：

- CFA 必须明确为 `SP + 固定偏移`，偏移与栈均满足 16 字节对齐，只支持可重建的简单 CFI。缺少 CFI、FP/表达式 CFA、复杂状态切换及预取地址依赖 SP 时跳过。
- 用 16 字节临时栈槽保存/恢复一个或两个完整 64 位 GPR。窗口内不执行原指令，恢复后才继续原程序；为栈调整和保存位置生成 CFI，并恢复原逻辑状态。跨 guard 的节流和间接路径暂不使用 spill。
- spill 成本只在实际保存活跃寄存器时产生。命中成本取真实保存/恢复序列的调度成本与 `2 * 保存寄存器数 * 目标 load latency` 的较大值，保守计入逐字栈传输，避免 pair 指令掩盖内存流量。
- 固定 CFA 使同一栈槽在一次函数调用内可复用。按函数与插入块的 profile 执行计数之比摊销每个保存字的一次潜在冷 miss，比例最多为 1；缺少有效频率时跳过。该项使用已有延迟模型，不假定缓存行大小。
- 保存/恢复指令计入代码预算，完整 spill 成本计入净收益及请求准备时间。净收益非正时拒绝，不增加寄存器数量或 spill 成本参数。

诊断增加 `estimated-benefit-cycles`、`estimated-cost-cycles`、`scratch-registers`、`spilled-registers` 和 `spill-cost-cycles`；无法建模或估算不盈利时给出原因。这是基于 generic 子目标的启发式潜在收益评估，尚未模拟跨候选的访存重叠、带宽竞争、cache 污染及分支误预测，正净收益不等于硬件上已证明加速，仍需实际负载校准。

## 缓存行请求节流

默认 `--loop-prefetch-cache-line-size=0` 关闭节流；明确知道目标 CPU 的缓存行大小后，可指定 16～4096 之间的二次幂，例如：

```sh
llvm-bolt app -o app.prefetch --enable-loop-data-prefetch \
  --data=app.fdata --loop-prefetch-cache-line-size=64 \
  --loop-prefetch-max-instructions=12 --loop-prefetch-diagnostics --verify-cfg
```

只对非零字节步长绝对值小于缓存行、且 load 所在块支配全部 latch 的候选节流。条件 load 可能错过所有边界迭代，故保留原请求频率；步长不小于缓存行的候选也保持原行为。以未来标量或 pair 访问范围的最后一个字节为目标，每经过一条缓存行只发送一次请求。初始和退出时的部分缓存行可能不发送请求，不影响原程序访存结果。

二次幂步长用低位掩码选择每行的一次请求；其他步长比较未来地址与虚拟前一迭代地址所在的行。计算与分支不修改 NZCV，不增加状态计数器、普通 load/store 或 spill；需要两个空闲临时寄存器，不足时跳过候选。原始地址仍按 W/X 回绕及扩展规则生成。全部地址计算、判断和最多两条新增跳转均计入指令预算；布局修复可能消除其中的跳转。

节流降低请求频率，也增加算术和分支开销。历史受控实验中连续扫描和部分多流场景出现明显退化，因此保持显式启用；当前执行收益模型检查，仍需在目标负载测量。没有对齐证明时，不把不同静态地址强行合并为同一缓存行。

## 预算与诊断

| 选项 | 默认值 | 含义 |
| --- | --- | --- |
| `--loop-prefetch-max-instructions` | 8 | 每个候选的新增指令上限，全局设置 |
| `--loop-prefetch-max-per-loop` | 2 | 每轮最多的预取请求数，pair 默认计两条、节流后计一条 |
| `--loop-prefetch-max-per-function` | 32 | 每个函数静态预取指令数上限 |
| `--loop-prefetch-analysis-limit` | 20000 | 每个函数分析的原指令数上限，包含 CFI pseudos |

内部固定每循环新增指令上限 32、每函数上限 256，作为代码增长保护，不替代收益判断。AArch64 每条新增指令为 4 字节。仅在上述范围尝试 spill；无法安全保存/恢复、收益不足或超预算时跳过。可直接编码的 base+位移预取不需要临时寄存器。可证明有效地址相同且中间没有地址寄存器定义的块内请求会去重；没有对齐依据时，不假定不同地址一定处于同一 cache line。多流共享循环及函数预算；先完成合法计划并评估收益，再按估算净收益分配预算。同等优先级保持原顺序，最终仍按原始位置逆序提交。排序仅是同协议下的资源分配依据，不是收益保证。

`--loop-prefetch-diagnostics` 输出函数、原始 PC、循环入口、配置来源、D、hint、候选预算、opcode、地址分量、实际增量、生成指令数及实际采用的 `cache-line` 大小（0 表示该候选不节流）。常见原因包括收益不足、缺少指令成本或索引 miss 统计、`unproven recurrence`、`unsupported address`、`unsupported loop context`、`opaque CFI expression`、寄存器不足和预算限制。函数/循环整体拒绝时可能没有单条候选 PC。其中 base-delta、offset、writeback 的单位为字节，index-delta 的单位为索引寄存器的增量；D 与步长的有符号乘积溢出时拒绝，W 索引再按 32 位截断。日志中的 `inserted` 表示计划通过并提交到 BOLT 内部；最终发射仍受普通 BOLT 约束，应核对输出机器码，也不代表已取得性能收益。

自动路径要求非零 access、有效的 miss/access 计数和超过阈值的 miss 比例；迭代时间优先使用有效单 latch profile，缺失时静态估算。随后执行收益判断，无法建模、净收益非正或超距离时跳过。不提供手工周期输入或绝对样本数量门槛。

## 验证与复现

无需真实采集即可运行[合成 profile 静态闭环示例](../examples/loop-data-prefetch/PROFILE-README.md)：构建三个小型 ELF 函数及匹配的扩展 fdata，检查原始 PC 绑定、自动距离、直接/间接/外层批量预取的最终机器码，同时检查错误身份和缺周期输入。脚本不执行目标程序，也不验证真实 SPE 采集或性能收益。

关键回归包含 `AArch64/loop-data-prefetch.s`、`AArch64/loop-data-prefetch-codegen.s`、`AArch64/loop-data-prefetch-spill.s` 和 MCPlusBuilder 单元测试，覆盖 profile 驱动的变换路径、W 索引、正负步长、writeback/pair、CFI/callee-saved base、NZCV、跨块、展开、大位移、拒绝、配置身份和预算。在 Arm 主机执行非 PIE、PIE 及 DSO 加载检查；地址、间接和批量的执行用例及展开检查见 TODO 8.16～8.17。完整 BOLT 回归的历史结果和已有失败见 [TODO 验证记录](LoopDataPrefetch-TODO.md)。

可复现的受控程序为 [loop-data-prefetch-bench.c](../utils/loop-data-prefetch-bench.c)，包含连续、大步长、结构体、双流和四流扫描；可用互不重叠的数据分片运行多线程。编译后须按前述流程采集并转换匹配该二进制的 profile，再运行 BOLT：

```sh
clang bolt/utils/loop-data-prefetch-bench.c -O2 \
  -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
  -fno-omit-frame-pointer -fuse-ld=lld -pthread \
  -Wl,--emit-relocs,--build-id -o bench
llvm-bolt bench -o bench.base --verify-cfg
llvm-bolt bench -o bench.prefetch --verify-cfg --enable-loop-data-prefetch \
  --data=bench.fdata --loop-prefetch-diagnostics

taskset -c 24 ./bench.base strided 512 30
taskset -c 24 ./bench.prefetch strided 512 30
```

按实际可用 CPU 调整绑定。程序接受 `模式 MiB 重复次数 [线程数]`，输出模式、MiB、重复次数、循环测量秒数、校验和及线程数。MiB 是全部线程共享的总数据规模；初始化和 warmup 在计时区间外，多线程计时包含开始同步和 join 开销。交错运行普通 BOLT 与自动预取配置，每种配置至少五次；比较校验和及中位运行时间。负例可用 `sequential 512 10` 和 `sequential 1 300`，结构体用 `records 512 30`。保留编译命令、完整诊断、原始结果和工具/二进制身份，保留无收益和退化结果。

以下为移除手工距离参数前的历史实验，当前版本不提供这些距离扫描入口。本机实验使用 HiSilicon implementer `0x48`、part `0xd02`、64 字节 cache line，固定 CPU 24；所有配置使用相同输入和普通 BOLT 参数。五次交错运行的中位秒数如下：

| 场景 | 普通 BOLT | D=4 | D=16 | D=64 |
| --- | ---: | ---: | ---: | ---: |
| 大步长，512 MiB，30 次 | 0.157245 | 0.167722 | 0.167710 | 0.136116 |
| 结构体，512 MiB，30 次 | 0.446066 | 0.445176 | 0.438599 | 0.450473 |
| 连续扫描，512 MiB，10 次 | 0.316206 | 0.366699 | 0.384244 | 0.400275 |
| 连续扫描，1 MiB，300 次 | 0.018473 | 0.021169 | 0.021416 | 0.021107 |

大步长 D=64 的吞吐比值约提升 15.5%，对应耗时减少约 13.4%；连续扫描和小工作集出现明确退化。该历史结果用于揭示距离敏感性和保留负例的必要性，不代表生产收益，也没有完成跨 CPU 或独立生产输入验收；后续多线程受控检查见下节。频率未锁定、共享主机竞争未消除；需要在目标环境复测。

商用验收仍须提供代表性生产程序、独立输入和目标 CPU，并明确最小收益、最大退化、代码增长及工具处理开销门槛。代码正确性要求零新增结果/异常/展开错误；性能结论必须同时记录无收益和退化配置，不能以 cache miss 下降替代运行时间/吞吐改善。

## 批量实验与验收门槛

[实验脚本](../utils/loop-data-prefetch-bench.py) 仅依赖 Python 标准库，在原生 AArch64 Linux 上读取预先编译的受控程序及匹配的 profile，重写、交错运行并归档结果。例如按实际可用 CPU 调整：

```sh
python3 bolt/utils/loop-data-prefetch-bench.py \
  --bolt /path/to/llvm-bolt --input /path/to/bench --profile /path/to/bench.fdata \
  --work-dir /tmp/prefetch-results --cpus 24,25,26,27 \
  --mib 512 --iterations 20 --samples 5 --threads 1,4 \
  --lines 0,64 --perf-stat
```

普通 BOLT 是比较基线，各配置使用相同的优化参数和输入。脚本保留原始二进制、各输出、源程序和脚本快照、命令/诊断日志、工具 SHA256/版本、CPU/频率策略等可读信息及 `report.json`；记录每次结果和校验和、中位数、范围、MAD 相对噪声、运行时间变化及吞吐比值。源码快照可直接重新编译；复跑须使用记录身份对应的工具，并自行保留工具文件。`perf stat` 单独保存计数，覆盖整个进程，包含初始化，不能等同于纯循环计数。

脚本不再扫描手工距离或生成独立的无 profile 压力程序；输入身份和距离均由同一份 profile 驱动。

可按目标产品指定以下门槛，未指定时只报告，不假设商用标准：

| 参数 | 检查 |
| --- | --- |
| `--min-speedup-pct` | 相对普通 BOLT 的最小吞吐比值提升，检查每个启用配置/场景 |
| `--max-regression-pct` | 最大中位运行时间退化百分比 |
| `--max-noise-pct` | 最大 MAD/中位数百分比，要求至少三次重复 |
| `--max-code-growth-pct` | 相对普通 BOLT 的可执行 section 增长百分比 |
| `--max-rewrite-seconds` | 每次重写最长时间 |
| `--max-rss-mib` | 每次重写峰值 RSS |

程序结果不一致、命令失败或超过显式门槛都会返回非零，已获取的记录仍写入 `report.json`；`completed` 区分完整运行与中途失败。生产验收还需独立输入、目标环境和约定的误差规则，脚本的受控程序不能提供这些条件。

以下同为移除手工距离前的历史实验。本机后续实验共执行 250 次：5 种访问模式 × 1/4 线程 × 普通 BOLT 和四种预取配置 × 每组 5 次，数据总量 512 MiB、重复 20 次、绑定 CPU 24～27。校验和全部一致；详细数据在 TODO 8.6。单线程相对普通 BOLT 的中位耗时变化如下，负值表示耗时减少：

| 场景 | D=16，无节流 | D=16，64 字节节流 | D=64，无节流 | D=64，64 字节节流 |
| --- | ---: | ---: | ---: | ---: |
| 连续扫描 | +21.4% | +56.3% | +27.4% | +71.1% |
| 大步长 | +3.3% | +12.9% | -9.7% | -9.8% |
| 结构体 | -1.8% | -1.3% | +0.9% | +0.8% |
| 双流 | +11.0% | +53.1% | +26.8% | +58.8% |
| 四流 | -20.7% | +11.4% | -45.9% | -25.5% |

四线程四流的 D=64 无节流/节流耗时变化为 -41.1%/-6.4%。本实验保留全部配置及退化结果，没有依据最好值开启默认节流或自动距离。频率未锁定、共享主机竞争及少量并行检查未完全排除；上述数据只用于工程验证和揭示调参风险，不作为生产验收。硬件预取覆盖和带宽饱和的因果归因仍需目标 CPU 的专用 PMU 及生产负载。

跨模块审阅、补丁分组及验证记录见 [集成审阅记录](LoopDataPrefetch-Review.md)。
