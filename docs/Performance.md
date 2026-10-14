# 性能优化与测量

## 现代架构增量（2026-10-14）

本轮原生编译基线为预先保存的 `2ec5e42` 编译器，其代码与 `18aedc7` 相同；QEMU 基线为 `18aedc7` 的已验证可执行文件。均来自当前分支的已有结果，未读取其他分支。

| 指标 | 基线 | 本轮 | 结果 |
|---|---:|---:|---|
| 210 个输入的原生编译 wall 中位数总计 | 1.23782 s | 0.89442 s | 耗时减少 **27.7%**，总量比 **1.384×** |
| 同组原生编译 CPU 时间总计 | 1.20851 s | 0.86481 s | 耗时减少 **28.4%** |
| 逐用例编译 wall 加速几何平均 | — | — | **1.084×**；大函数占总时间的比重更高 |
| 前端 `--emit-hir` wall 总计 | 0.18716 s | 0.17788 s | 耗时减少 **5.0%** |
| 编译平均进程峰值 RSS | 5555 KiB | 5994 KiB | 增加 **7.9%** |
| 60 个原有 performance 的 QEMU wall 几何平均 | — | — | **1.006×**，总体基本持平 |
| 44 个基线区间至少 10ms 的计时区间几何平均 | — | — | **1.004×**，不视为显著运行加速 |
| 60 个程序链接后 `.text` 总计 | 201160 B | 201226 B | 增加 **0.03%** |
| 生成的整数压力用例计时区间 | 93.788 ms | 61.366 ms | **1.528×** |

原生测量绑定同一 CPU，两个版本交替运行，预热一次，七次取中位数；默认不启用统计或符号检查。RSS 和阶段统计通过独立的小型 `/usr/bin/time` 启动器各采样一次，避免 Python 父进程的继承峰值污染 RSS。小程序的 wall 时间包含进程启动和输出 HIR 的开销。RSS 增长与新 IR、缓存及 arena 生命周期相关，当前没有证明整个编译器的内存收益。

QEMU 测量在正确性检查完成后串行运行，预热一次，原有用例五次取中位数；编译和链接时间不计入运行时间。全部原有运行结果见 [performance-modern.csv](performance-modern.csv)。新架构主要改善编译成本、表示和验证；本轮原有程序的运行均值基本持平。

压力用例取固定种子 `260626` 的第 2 个生成程序：47 个循环状态，系数 9 和 -12，执行 100 万次；预热一次、交替七次取中位数。其预期结果由两步递推矩阵在模 2³² 下独立求幂计算。整数 EGraph 因式提取、暴露的机器临时值与分配拆分协同改善此例；这是定向压力实验，不能代替原有 60 个程序的均值。

210 个输入的阶段统计中，CFG 缓存命中 **26100** 次、构建 **1407** 次；最终产生 **48902** 条机器指令、**88** 个拆分栈槽。SCCP 已使用 def-use 工作队列，GVN 使用结构化哈希与作用域撤销，MemorySSA 缓存 clobber 查询。绝对 PASS 耗时受计时开销影响，仅用于定位瓶颈。

验证结果：

- `-O2`、`-O3` 各 **210/210**，启用分配后符号检查；压缩 MIR 后，O2 全部汇编与已执行版本逐字节相同。
- **99** 个额外程序在 O0 / O2 / O3 下共 **297** 次执行检查，另有 **20** 个诊断检查。
- ASan / UBSan 与符号分配检查覆盖全部原有和额外程序；额外的 2 万项表达式在默认 **8MB** 栈下通过。
- 新增 24 个高活跃压力程序、宽矩阵的逐元素浮点顺序比较及长表达式回归。没有启用 fast-math 或 FMA 重结合。

技术依据和实现边界见 [Architecture-Research.md](Architecture-Research.md)。复现：

```sh
# BASE_COMPILER 和 BASE_PROGRAMS 指向事先保存的当前分支基线
python3 tools/compile_bench.py "$BASE_COMPILER" build/compiler --repeats 7
python3 tools/compile_bench.py "$BASE_COMPILER" build/compiler --hir --repeats 7
python3 tools/test.py --opt 3 --check-alloc --output build/modern-results
python3 tools/bench.py "$BASE_PROGRAMS" build/modern-results --repeats 5
python3 tools/regression.py

# 独立预期值的定向压力实验
python3 tools/regression.py --benchmark
python3 tools/test.py build/pressure-fixture/performance --compiler "$BASE_COMPILER" --opt 3 --output build/pressure-before
python3 tools/test.py build/pressure-fixture/performance --opt 3 --check-alloc --output build/pressure-after
python3 tools/bench.py build/pressure-before build/pressure-after --tests build/pressure-fixture/performance --repeats 7
```

## 上一轮优化（18aedc7）

比较使用两份事先保存在本地的编译器：首次基线 `4ab4a47` 使用 `-O2`，上一轮优化 `1ab8c39` 使用 `-O3`；该轮 `18aedc7` 使用 `-O3`。未读取其他分支。

2026-10-14，在本机 x86-64 Linux、QEMU RISC-V 用户态下测量全部 60 个 performance 用例：

- 编译、链接和正确性检查先完成，计时不包含编译、链接过程。
- 每个版本预热一次，再交替执行五次，取中位数。
- 测量进程绑定到同一个可用 CPU，串行执行。
- 分别记录整个 QEMU 进程的时间和程序 `starttime` / `stoptime` 区间的时间。
- 相对首次基线：整体进程时间几何平均加速 **3.18 倍**；55 个基线区间至少 10ms 的用例，区间时间几何平均加速 **5.96 倍**。
- 相对上一轮优化：整体进程时间几何平均再加速 **1.20 倍**；44 个上一轮区间至少 10ms 的用例，区间时间几何平均再加速 **1.33 倍**。两个计时区间平均值采用各自基线的筛选条件，不能直接相乘。
- 所有用例均保存结果；较短用例不纳入对应的区间平均值，以减小计时噪声。
- `.text` 总计 **201,160** 字节，较首次基线的 **214,822** 字节减少 **6.4%**。循环展开以代码大小换取执行速度，较上一轮的 **187,242** 字节增加 **7.4%**。这些数包含链接的运行库代码。

本轮增量的代表性测量：

| 用例 | 上一轮区间时间 / ms | 当前区间时间 / ms | 增量加速 |
|---|---:|---:|---:|
| h-1-01 | 54.853 | 11.414 | 4.81× |
| bitonic_sort-1 | 482.247 | 176.196 | 2.74× |
| fft1 | 85.817 | 34.468 | 2.49× |
| many_mat_cal-1 | 164.038 | 122.472 | 1.34× |
| matmul2 | 152.688 | 128.065 | 1.19× |
| conv2d-1 | 676.259 | 532.628 | 1.27× |
| tensor_g5_stencil_int-3 | 166.547 | 139.674 | 1.19× |
| gaussian-1 | 271.875 | 263.293 | 1.03× |

这些结果只描述本机 QEMU 的表现；真实 RISC-V 硬件需要另行测量。

全部 60 个用例的首次基线对比保存在 [performance.csv](performance.csv)，上一轮对比保存在 [performance-incremental.csv](performance-incremental.csv)。两组实验独立测量，候选版本中位数可能略有差异。很短的计时区间仍有噪声；Gaussian 等用例的本轮收益较小，尚有优化空间。

## 优化依据

新增 SSA 层为常量传播、值编号、循环分析提供共同的数据表示。支配关系和 Phi 入边在每个 PASS 后验证；控制流改写同步修复 Phi。指令选择明确产生比较分支，寄存器分配能够看到比较操作数的实际活跃时间。设计参考 [Cranelift 的指令选择](https://cfallin.org/blog/2023/01/20/cranelift-isle/)与[寄存器分配](https://cfallin.org/blog/2022/06/09/cranelift-regalloc2/)讨论，实现保留在本仓库内。

该轮的参考主要涉及 IR 边界与约束表达。指令选择使用手写规则，该轮分配器是 Briggs 图着色，尚未实现 ISLE 或 regalloc2。各阶段的准确状态和架构调研见 [Architecture-Research.md](Architecture-Research.md)。

寄存器分配使用冲突图、复制偏好、调用约束和循环权重，利用调用者保存寄存器，减少热值溢出。循环不变量可保留在寄存器中；经过下标范围证明的地址计算转为指针递增。后端选择立即数和常数除法的移位、倒数乘法序列。

调用图上的不动点分析生成函数的外部读写摘要；参数读写映射到调用方的实际指针来源。不同参数可能互相别名，也可能指向全局对象。没有外部读写和可观察副作用的调用能够被合并；只读调用的读依赖阻止不合法外提。跨块访存前向分析在汇合处取可用值交集，写入只使可能别名的值失效。`--emit-effects` 可直接检查这些摘要。

尾递归转换成带参数 Phi 的循环；具有局部内存对象的函数保持原调用方式，避免改变逃逸对象的生存期。小函数内联放宽热循环中的预算。循环旋转保留零次执行入口检查，支持在受保护的入口外提纯函数调用；提前返回和多个出口会限制变换。整数范围与取模使用位置分析将合适的除法、整除测试降为移位和掩码，负数截断语义保持不变。

循环展开借鉴 [MLIR 的循环变换](https://mlir.llvm.org/docs/Passes/)所采用的显式循环与依赖分析思路，代码由本仓库实现。固定次数循环选择可整除的展开因子；动态次数循环使用四次主循环和标量尾循环，入口条件保证展开边界计算不会溢出。展开按原顺序执行加载、存储与浮点累加，不需要假定数组之间无别名。

寄存器分配的 Briggs 合并同时检查冲突图和 ABI 允许寄存器的交集。非叶函数也使用没有跨调用存活的参数寄存器。参数搬运先保存栈参数，再处理寄存器并行复制和交换环；分配完成后跳过无操作复制块，消除循环中的多余跳转。

tensor 阶段保留生产者与维度信息。具有一致行、列的整数矩阵可使用模 2³² 等价的二重循环。浮点矩阵必须额外证明输入、乘积和所有部分和都能以 binary32 精确表示；不满足条件时使用保持累加顺序的四列微内核。零符号和 [RISC-V NaN 规则](https://docs.riscv.org/reference/isa/unpriv/f-st-ext.html)保持一致，不启用 fast-math 或 FMA 重结合。

`-O3` 可以合并重复、只读的循环体。变换检查所有赋值、分支、调用和读依赖，要求辅助状态每次都重置，归约只依赖当前迭代的整数加法贡献。内存写入、具有副作用的调用和相互依赖的归约会阻止这个变换。高倍率用例因此减少了重复计算的次数；优化器不读取测试名称、输入或预期输出。

## 正确性与稳定性

- `-O3`：210/210 原有用例通过。
- `-O2`：210/210 原有用例通过。
- `-O1`：150/150 功能用例通过。
- 73 个额外程序在 `-O0`、`-O2`、`-O3` 下执行，共 219 次回归；另有 20 个诊断回归。
- 额外覆盖 108 个整数边界、随机被除数与 21 个正负常数除数、保存的比较结果、浮点累加舍入、负零、NaN、非方阵、结构失效、循环内存写入和调用副作用。
- 新增递归读写摘要、参数别名、尾递归状态交换、零次执行的发散函数、多个出口、动态展开的各种余数和两组参数寄存器交换环。
- 40 个固定种子 `8143` 的组合用例覆盖不同数组别名与递归读写，预期值由 Python 独立计算；额外与事先保存的未优化编译器进行差分执行，40/40 一致。
- AddressSanitizer / UndefinedBehaviorSanitizer 编译全部 210 个原有程序和 73 个额外程序，283/283 通过。

## 复现

先保存当前基线编译器，再构建候选版本。编译器路径可指向用户已有的基线文件；执行器不会访问其他分支。

```sh
python3 tools/test.py test/performance --compiler /path/to/baseline-compiler --opt 2 --jobs 1 --output build/bench/baseline
python3 tools/test.py test/performance --compiler build/compiler --opt 3 --jobs 1 --output build/bench/candidate
python3 tools/bench.py build/bench/baseline build/bench/candidate --repeats 5 --warmups 1 --output build/bench/report.json
```

JSON 保留全部样本、中位数、代码大小、CPU 绑定、版本路径与每个用例的加速比。`--filter` 可选择用例名子串用于单独排查。

```sh
build/compiler -O3 --emit-expanded-hir input.sy
build/compiler -O3 --emit-ir input.sy
build/compiler -O3 --emit-mir input.sy
build/compiler -O3 --emit-effects input.sy
build/compiler -O3 --emit-ir --passes=prune,inline,promote,ssa,sccp,prune,gvn,dce input.sy
```

`--passes` 控制 CFG/SSA 层的顺序。新增 `memory`、`tail`、`arith`、`rotate`、`unroll` 可分别检查访存、尾递归、整数算术、循环旋转与展开。`unroll` 通常放在 `rotate` 和 `licm` 之后。tensor 结构分析由 `-O2` / `-O3` 启用，重复循环归约与展开由 `-O3` 启用。`promote` 和 `inline` 应在 SSA 构造之前运行，`lower-phi`、`isel`、`layout` 可单独检查后端边界。
