# 使用合成 profile 验证预取插入

[profile-input.s](profile-input.s) 包含三个小函数：[run-profile.py](run-profile.py) 构建原始 AArch64 ELF，为各函数生成匹配的扩展 fdata，然后分别生成关闭和开启预取的 BOLT 输出。脚本检查原始指令 PC 绑定、自动距离和最终机器码，不执行生成的程序。

## 运行方式

在 LLVM 工程根目录执行，工具来自包含本功能且支持 AArch64 的构建：

```sh
python3 bolt/examples/loop-data-prefetch/run-profile.py \
  --tools /tmp/bolt-prefetch-build/bin \
  --output-dir /tmp/loop-prefetch-profile-demo
```

`--tools` 目录需要包含 `llvm-mc`、`ld.lld`、`llvm-readelf`、`llvm-objdump` 和 `llvm-bolt`。可用 `--bolt /path/to/llvm-bolt` 单独指定待验证的 BOLT，例如使用同一套汇编和链接工具检查另一种断言配置。输出目录必须尚不存在；省略 `--output-dir` 时创建临时目录。成功后打印三个 `PASS` 和反例检查结果，任何检查失败均以非零状态退出并保留产物。

## 三组输入及预期结果

三个函数分别计算连续数组、索引数组和二维连续数组的元素和，源码没有预取。入口 `_start` 含求和结果检查，但脚本不运行该检查。

| 函数 | 原始访存 | 合成周期均值、内部缺省延迟 | 预期自动距离 | 最终预取 |
| --- | --- | --- | --- | --- |
| `direct` | `ldr x3, [x0], #8` | 每轮 `500/10=50`，内部延迟 200 | D=4 个当前循环迭代 | `prfm pldl1keep, [x0, #32]` |
| `gather` | `ldr w3, [x0], #4` 后 `ldr x4, [x1, w3, uxtw #3]` | 每轮 `500/10=50`，内部延迟 200 | D=4 个当前循环迭代 | guard 内读取未来索引，再执行寄存器索引 `prfm` |
| `batch` | 外层每轮有 4 次 `ldr x4, [x0], #8` | 内层 `20/10=2`，父循环 `1000/10=100`，内部延迟 200 | D=2 个父循环迭代 | 外层入口 `prfm [x0,#64]` 和 `prfum [x0,#95]`，hint 均为 `pldl1keep` |

`gather` 使用 `--loop-prefetch-indirect`，`batch` 使用 `--loop-prefetch-batch --loop-prefetch-cache-line-size=64`，均通过 profile 决策，没有指定手工距离。批量用例刻意让内外层周期不同，检查使用父循环周期得到 D=2，防止误用内层周期得到 D=100。

批量范围是未来一行的 32 字节，距当前基址为 `[64,95]`。两个端点请求覆盖未知缓存行对齐的情况；若实际地址已对齐，两条 hint 可能指向同一缓存行。检查不将请求数当作实际独立缓存行数。

## profile 如何构造

脚本链接时保留重定位并生成真实 build-id，再从原始 ELF 反汇编读取函数地址、load PC 和条件回边 PC。fdata 的身份和偏移均从该 ELF 派生，避免手工写死 PC 或借用另一份二进制的身份。

例如本次 `gather/input.fdata` 为：

```text
bolt-prefetch llc_miss d4fdca7e4d93622fd0a002879312b2e88ea85dc4 1024 200000001 0 0
0 [unknown] 0 1 gather 0 0 1 0 0
1 gather 18 1 gather 8 0 15 500 10
7 gather c 8 10
```

`18`、`8` 和 `c` 是十六进制函数内偏移；分支记录提供回边、计数及周期统计，`7` 记录将 8 次 miss、10 次 access 绑定到偏移 `0xc` 的原始 load。头部 period=1024 是声明的采样间隔，不参与延迟计算；延迟采用当前 generic 模型的内部默认值 200 cycles。

**采样属性、计数和周期均为人工 fixture；内部默认延迟也不是某款 CPU 的实测值，没有真实 SPE 采集或性能依据。** 新生成 ELF 应使用脚本为其生成的 profile，不能直接复用文档中的 build-id。本例包含分支记录，不使用 `-nl`；它不表示支持仅含 SPE memory 信息的输入。

## 实际检查及产物

脚本对每组输入检查：

- 原始 ELF 及关闭预取的 BOLT 输出没有 `prfm/prfum`。
- miss 信息恰好绑定一个原始 load，没有非法 PC、非 load 或未匹配记录；诊断中的 PC、profile 来源、D 和候选类型符合预期。
- 输出 ELF 中确有预期条数及寻址形式的预取；直接访存预取紧邻原 load 之前。
- 间接 profile 同时提供索引生产者的 hit-only 统计，用于评估新增索引 load 的成本；间接访存仅增加一条未来索引普通 load，其位置受 guard 保护，guard 跳过该 load 和预取后回到原始数据 load；原有 load 保留。
- 批量预取在内层之前，内层回边指向原始 load、父回边指向计数初始化，预取随父循环执行。
- 错误 build-id 拒绝；保留分支但清除周期数据后按循环指令和 CFG 静态估算每轮周期，诊断标记 `cycles-source=static`，并按 `D=ceil(200/估算周期)` 验证生成的预取。

所有正常重写均启用 `--verify-cfg`。产物目录包括原始 `input`、`input.s`、`input.o`、`input.asm`、build-id 信息，以及各用例子目录中的 `input.fdata`、`baseline`、`prefetched`、两份反汇编及 BOLT 日志。`report.json` 保存 BOLT 文件 SHA-256、完整预取重写命令、原始 PC、距离、实际预取和检查结果，并明确记录 `synthetic_profile=true`、`target_executed=false`、`performance_validated=false`。

## 本次结果（2026-10-27）

当前工作区在断言开启及关闭两种 BOLT 构建中，三组正例及两个反例全部通过。原始 load PC 分别为 `direct=0x210278`、`gather=0x21029c`、`batch=0x2102c0`，输出预取条数分别为 1、1、2。

产物分别保存在 `/tmp/bolt-prefetch-profile-assert-on/` 和 `/tmp/bolt-prefetch-profile-assert-off/`。其中间接访存实际输出为：

```asm
ldr   w3, [x0], #4
sub   x4, x2, #5
bic   x4, x4, x2
tbnz  x4, #63, current_load
ldr   w4, [x0, #12]
prfm  pldl1keep, [x1, w4, uxtw #3]
current_load:
ldr   x4, [x1, w3, uxtw #3]
```

当前索引 load 已将 `x0` 增加 4 字节，因此未来第 4 轮索引相对新 `x0` 为 12 字节；新增 load 不覆盖原始 `w3`。本次只完成 fdata 读取到机器码发射的静态闭环，不包含真实 `perf.data`、`perf2bolt` 转换、目标程序执行或性能验收。
