# 验证循环预取指令插入

需要验证 profile 绑定和自动距离时，使用[合成 profile 三组示例](PROFILE-README.md)，覆盖直接、间接和外层批量预取。下面是使用固定合成 profile 的最小指令插入示例。

这个最小 AArch64 示例按顺序累加 16 个 `uint64_t`。循环只有一条 load，每轮指针增加 8 字节，源代码没有预取指令。合成 profile 的平均周期为 25，按内部延迟 200 计算得到 D=8，预期 BOLT 在 load 前插入对未来第 8 轮、即 64 字节之后地址的预取：

```asm
prfm pldl1keep, [x0, #64]
ldr  x3, [x0], #8
```

在 LLVM 工程根目录执行，工具必须来自本功能分支并包含 AArch64 支持：

```sh
bash bolt/examples/loop-data-prefetch/run.sh \
  /tmp/bolt-prefetch-build/bin
```

可通过第二个参数指定产物目录。脚本会汇编、保留重定位并链接，分别生成关闭预取的普通 BOLT 输出和启用预取的输出，再检查实际反汇编。原始程序及普通 BOLT 输出中应没有预取，启用后应出现上述 `prfm`，成功时打印 `PASS`。

产物包括 `input`、`baseline`、`prefetched` 三个 ELF，对应的 `.asm` 反汇编，以及两个 BOLT `.log`。脚本只做静态重写和 CFG 检查，不执行生成的程序，也不运行 benchmark。

这里使用随附的合成 `input.fdata`，不采集 SPE，也不使用 `-nl`。它验证变换引擎能够将预取发射到最终机器码；SPE 采集、profile 绑定、真实周期及性能收益仍须分别验证。入口 `_start` 包含求和结果检查，但本脚本不执行该检查。

## 本次结果（2026-10-26）

使用当前工作区构建的 `/tmp/bolt-prefetch-build/bin/llvm-bolt`，上述命令运行通过。BOLT 诊断为 `distance=8`、`base-delta=64`、`instructions=1`，汇总插入 1 条、拒绝 0 条。原始及普通 BOLT 反汇编没有预取，预取输出的循环为：

```asm
600008: f9802000  prfm pldl1keep, [x0, #64]
60000c: f8408403  ldr  x3, [x0], #8
600010: 8b030042  add  x2, x2, x3
600014: f1000421  subs x1, x1, #1
600018: 54ffff81  b.ne 0x600008
```

实际日志及 ELF 保存在 `/tmp/bolt-prefetch-example.Ze06A3/`。本次只检查最终机器码，没有执行目标程序或测量性能。
