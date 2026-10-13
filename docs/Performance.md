# 性能优化与测量

基线是优化前的 `4ab4a47` 编译器，使用 `-O2`；新版使用 `-O3`。基线可执行文件在修改开始前保存在本地，未读取其他分支。

2026-10-14，在本机 x86-64 Linux、QEMU RISC-V 用户态下测量全部 60 个 performance 用例：

- 编译、链接和正确性检查先完成，计时不包含编译、链接过程。
- 每个版本预热一次，再交替执行三次，取中位数。
- 测量进程绑定到同一个可用 CPU，串行执行。
- 分别记录整个 QEMU 进程的时间和程序 `starttime` / `stoptime` 区间的时间。
- 整体进程时间的几何平均加速为 **2.66 倍**。
- 55 个基线计时区间至少 10ms 的用例，区间时间几何平均加速为 **4.44 倍**。5 个较短用例仍完整记录，未纳入这个区间平均值，以减小计时噪声。
- 60 个可执行文件的 `.text` 总计从 **214,822** 字节降到 **187,242** 字节，减少约 **12.8%**；这个数包含链接的运行库代码。

| 用例 | 基线区间时间 / ms | 新版区间时间 / ms | 加速 |
|---|---:|---:|---:|
| crypto-1 | 644.883 | 148.713 | 4.34× |
| lcs-1 | 448.064 | 123.394 | 3.63× |
| tensor_g1_elementwise_int-1 | 308.873 | 0.869 | 355.43× |
| tensor_g2_elementwise_float-1 | 2735.868 | 7.807 | 350.44× |
| tensor_g3_matmul_int-3 | 309.516 | 1.896 | 163.25× |
| tensor_g4_matmul_float-3 | 1170.281 | 9.044 | 129.40× |
| tensor_g5_stencil_int-3 | 273.272 | 168.237 | 1.62× |
| gaussian-1 | 295.486 | 268.625 | 1.10× |

这些结果只描述本机 QEMU 的表现；真实 RISC-V 硬件需要另行测量。

全部 60 个用例的中位数和代码大小保存在 [performance.csv](performance.csv)。

## 优化依据

新增 SSA 层为常量传播、值编号、循环分析提供共同的数据表示。支配关系和 Phi 入边在每个 PASS 后验证；控制流改写同步修复 Phi。指令选择明确产生比较分支，寄存器分配能够看到比较操作数的实际活跃时间。设计参考 [Cranelift 的指令选择](https://cfallin.org/blog/2023/01/20/cranelift-isle/)与[寄存器分配](https://cfallin.org/blog/2022/06/09/cranelift-regalloc2/)讨论，实现保留在本仓库内。

寄存器分配使用冲突图、复制偏好、调用约束和循环权重，利用调用者保存寄存器，减少热值溢出。循环不变量可保留在寄存器中；经过下标范围证明的地址计算转为指针递增。后端选择立即数和常数除法的移位、倒数乘法序列。

tensor 阶段保留生产者与维度信息。具有一致行、列的整数矩阵可使用模 2³² 等价的二重循环。浮点矩阵必须额外证明输入、乘积和所有部分和都能以 binary32 精确表示；不满足条件时使用保持累加顺序的四列微内核。零符号和 [RISC-V NaN 规则](https://docs.riscv.org/reference/isa/unpriv/f-st-ext.html)保持一致，不启用 fast-math 或 FMA 重结合。

`-O3` 可以合并重复、只读的循环体。变换检查所有赋值、分支、调用和读依赖，要求辅助状态每次都重置，归约只依赖当前迭代的整数加法贡献。内存写入、具有副作用的调用和相互依赖的归约会阻止这个变换。高倍率用例因此减少了重复计算的次数；优化器不读取测试名称、输入或预期输出。

## 正确性与稳定性

- `-O3`：210/210 原有用例通过。
- `-O2`：210/210 原有用例通过。
- `-O1`：150/150 功能用例通过。
- 24 个额外程序在 `-O0`、`-O2`、`-O3` 下执行，共 72 次回归；另有 20 个诊断回归。
- 额外覆盖 108 个整数边界、随机被除数与 21 个正负常数除数、保存的比较结果、浮点累加舍入、负零、NaN、非方阵、结构失效、循环内存写入和调用副作用。
- AddressSanitizer / UndefinedBehaviorSanitizer 编译全部 210 个原有程序和 24 个额外程序，234/234 通过。

## 复现

先保存当前基线编译器，再构建候选版本。编译器路径可指向用户已有的基线文件；执行器不会访问其他分支。

```sh
python3 tools/test.py test/performance --compiler /path/to/baseline-compiler --opt 2 --jobs 1 --output build/bench/baseline
python3 tools/test.py test/performance --compiler build/compiler --opt 3 --jobs 1 --output build/bench/candidate
python3 tools/bench.py build/bench/baseline build/bench/candidate --repeats 3 --warmups 1 --output build/bench/report.json
```

JSON 保留全部样本、中位数、代码大小、CPU 绑定、版本路径与每个用例的加速比。`--filter` 可选择用例名子串用于单独排查。

```sh
build/compiler -O3 --emit-expanded-hir input.sy
build/compiler -O3 --emit-ir input.sy
build/compiler -O3 --emit-mir input.sy
build/compiler -O3 --emit-ir --passes=prune,inline,promote,ssa,sccp,prune,gvn,dce input.sy
```

`--passes` 控制 CFG/SSA 层的顺序；tensor 结构分析由 `-O2` / `-O3` 启用，重复循环归约由 `-O3` 启用。`promote` 和 `inline` 应在 SSA 构造之前运行，`lower-phi`、`isel`、`layout` 可单独检查后端边界。
