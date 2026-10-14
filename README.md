# SysY compiler

C++17 实现的 SysY 编译器，输出 RV64GC / LP64D 汇编。SysY 的解析、语义检查、优化、寄存器分配和指令选择全部在仓库内实现，不使用 LLVM/GCC 等编译器框架，不调用外部语言编译器处理 SysY 源码。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/compiler -S -O3 test/functional/00_main.sy -o build/main.s
```

构建需要 CMake 和普通 C++17 编译器，不依赖编译器框架。`--stats` 的 RSS 采样使用宿主 POSIX 接口。

## PASS 与 IR

每次变换都围绕数据表示展开，前后端之间没有工具或文本协议依赖：

```
Source → source-slice Token IR → arena Program IR → resolved HIR
       → tensor analysis / schedule IR → expanded HIR → polyhedral region IR → CFG/SSA
       → MemorySSA / affine summaries / bounded EGraph → target MIR → local combine / compact
       → interval bundles / split / allocation → RV64 assembly
```

- `frontend.cpp` 使用源码切片 Token 和稳定索引 arena，产生结构化 Program IR。
- `semantic.cpp` 绑定符号、检查类型和数组形状、计算常量，并将嵌套初始化转为稀疏的行优先初始化。
- `tensor.cpp` 保留并分析数组形状和生产者，推导行、列一致性，再选择矩阵算法。调度 IR 明确记录迭代域、列分块和尾块；一般矩阵使用四列或八列微内核，保持每个元素的浮点累加顺序；符合证明条件的矩阵使用二重循环。逐元素表达式融合，标量广播只求值一次；临时结果保证别名安全，返回值使用调用方提供的缓冲区。`-O3` 还分析无副作用的重复循环与整数加法归约。
- `polyhedral.cpp` 为普通数组的二重矩形循环建立整数迭代域、仿射访问映射与候选依赖关系。用有预算的整数集合求解证明循环交换或 unroll-and-jam 合法，再按访问步长、公共加载及寄存器预算选择调度；整数条件累加只在贡献可安全求值时转换为掩码。浮点累加顺序保留，未知别名和未证实的关系阻止变换。已选择的 tensor 内核带调度标记，避免重复分块。这是受限多面体优化，尚无一般符号域、skewing、波前或自动搜索框架。
- `lowering.cpp` 将结构化控制流变成基本块，短路表达式变成分支，访存和调用显式化；提升不逃逸的局部标量，检查 PASS 前后约束。很长的算术表达式以迭代方式降级，减少编译器栈消耗。降低完成后释放语法 arena，保留符号、类型及全局初值。
- `optimizer.cpp` 缓存支配分析，使用 def-use 工作队列 SCCP、结构化哈希 GVN、按字节区间查询的 MemorySSA；值编号与加载替换在一次支配遍历中即时协作。稀疏 SROA 只为不逃逸数组的观测元素建立 SSA；循环内的固定可写单元可提升为状态，并在每个出口回写，未知访问和观察调用阻止变换。局部 DSE 删除完整覆盖的死写。`rewrite.cpp` 先用单基仿射摘要归一化单次使用的整数链，再进入有预算的 EGraph 做等价改写和成本提取。计算支配树、活跃变量、支配边界和自然循环，构造带 Phi 的 SSA。通过调用图不动点与指针来源分析推导函数的读写摘要，合并相同纯函数调用，并跨基本块消除冗余加载。实现小函数内联、尾递归消除、循环不变量外提、受支配作用域约束的范围推理、整数算术简化、指针归纳变量和循环旋转。`-O3` 对合适的固定、动态次数循环展开，保留标量尾循环和浮点顺序。
- `selection.cpp` 将 Phi 降为边上复制，选择布局和 RV64 目标指令；常量除法、地址计算及比较序列在寄存器分配前展开。目标 MIR 暴露临时值、bank、early/late 使用时刻及调用 clobber。展开后进行局部值编号和复制传播，以定义版本保护可多次写入的值，保留常量重物化配方，再用 def-use 工作队列删除无用临时值并压缩 ID。
- `backend.cpp` 用稀疏 64 位块计算活跃度，以有序占用区间查询冲突，按优先队列分配复制 bundle 并驱逐重试；避免冷路径的受限复制压缩热 bundle 的寄存器选择。热溢出区间拆分后重新分配，较高压力下相邻使用共享重载。独立冲突与 ABI 检查，以及可选的符号数据流检查，验证分配结果。ABI 参数按并行复制处理交换环；分配后消除无操作复制块和多余跳转，输出符合 [RISC-V psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/) 的汇编。

标量提升后，`ssa` PASS 将可多次写入的变量转换为单赋值，并在汇合处插入 Phi。每个 PASS 后检查终结指令、操作数、类型、调用签名，以及 SSA 定义唯一性、支配关系和 Phi 入边。CFG 变化会同步修复 Phi；退出 SSA 时拆分关键边，处理并行复制中的环。Token、Program、SSA 和降 Phi 后的 IR 可单独打印，各 PASS 可以重新组合。

采用的现代设计及准确实现边界见 [Architecture-Research.md](docs/Architecture-Research.md)，全阶段调研、取舍与下一批实验见 [Compiler-Research.md](docs/Compiler-Research.md)。红绿 CST、完整 SoA、跨 CFG AEGraph、ISLE DSL 和完整 regalloc3 尚未实现；当前批量编译使用源码切片、块状 arena、自有目标 MIR 和区间分配器。

```sh
build/compiler --emit-tokens input.sy
build/compiler --emit-hir input.sy
build/compiler --emit-expanded-hir input.sy
build/compiler --emit-ir -O0 input.sy
build/compiler --emit-ir -O2 input.sy
build/compiler --emit-mir -O3 input.sy
build/compiler --emit-effects -O3 input.sy
build/compiler --emit-memory-ir -O3 input.sy
build/compiler --emit-tensor-plan -O3 input.sy
build/compiler --emit-poly-ir -O3 input.sy
build/compiler --no-polyhedral -O3 input.sy -o baseline.s
build/compiler --emit-allocated-mir --check-alloc -O3 input.sy
build/compiler --stats --check-alloc -O3 input.sy -o output.s
build/compiler --emit-ir --passes=prune,inline,promote,ssa,sccp,prune,gvn,dce input.sy
```

默认 `-O0`。`-O1` 使用标量 SSA 优化，`-O2` 增加调用、内存、循环和 tensor 结构优化，`-O3` 增加循环展开与经过数据流检查的重复循环归约。追求执行性能时使用 `-O3`。`-o -` 输出到标准输出，`-` 输入从标准输入读取。错误带文件、行、列；失败不会覆盖已有输出文件。

`loop-promote` 在 SSA 前运行；`arith-global` 可替换 `arith`，用于关闭条件／惰性范围推理的对照。MIR 验证始终运行，`--check-alloc` 额外运行符号分配检查。构建后可执行 `python3 tools/verify.py` 检查故意破坏的 MIR；可选 `--smt` 使用 Python `z3-solver` 证明局部整数规则，编译器本身无需此依赖。

优化保留浮点运算顺序，不启用 fast-math 或 FMA 重结合。浮点矩阵算法变换要求证明每一步乘积和部分和都能精确表示；未知浮点值走普通内核。常量求值采用目标的 [NaN 规则](https://docs.riscv.org/reference/isa/unpriv/f-st-ext.html)。

## QEMU 测试

运行提供的 `.sy`、`.in`、`.out` 用例：

```sh
python3 tools/test.py test
python3 tools/test.py test --opt 3
python3 tools/test.py test/functional test/h_functional --opt 0 --output build/o0-results
python3 tools/regression.py
```

测试需要 `qemu-riscv64`、`riscv64-linux-gnu-as`、`riscv64-linux-gnu-ld` 和 RISC-V Linux libc。汇编器和链接器只负责连接本编译器生成的汇编与 `libs/libsysy_riscv.a`；不调用任何外部语言编译器。测试执行器目前使用动态 libc，运行库本身使用提供的静态 archive。

工具优先从 PATH 查找，也可用 `--as`、`--ld`、`--qemu`、`--sysroot` 指定路径；支持用户目录 `.cache/sysy-toolchain/root` 中解包的工具。输出、汇编、可执行文件、运行时间和 JSON 汇总保存在 `build/test-results`。浮点输出按 `1e-6` 绝对或相对容差比较，整数精确比较；程序退出码作为最后一个输出项，计时 stderr 单独保存。

提供的 `test/` 数据保留在本地，不将大体积测试数据加入版本库。可复现的额外回归用例放在 `tools/regression.py`，覆盖变参、两组参数寄存器耗尽、调用中保留大量活跃值、NaN 条件、初始化副作用、大栈帧、tensor 别名与短路调用，以及非法程序诊断。

语言依据为 `docs/SysY-Language-Definition.md` 和 `docs/SysY-Runtime-Library.md`。运行库的浮点输出采用提供的 `%a` 格式。为兼容现有测试，普通表达式也接受关系和逻辑运算，非 void 函数落到末尾返回零。

## Tensor 扩展

根据额外测试支持 `tensor int/float` 数组、逐元素 `+ - * / %`、一元 `+ -`、标量广播、`@` 二维矩阵乘法、tensor 参数和返回值。逐元素操作要求相同形状；矩阵乘法要求内部维度匹配；`%` 只接受整数。返回形状从函数体推导，各个 return 必须形状一致。tensor 运算和返回需要静态已知的维度；省略首维的 tensor 参数可按普通数组索引访问。

性能验证和复现方法见 [Performance.md](docs/Performance.md)。当前 `-O2`、`-O3` 下全部 **210/210** 用例通过，`-O0`、`-O1` 功能用例各 **150/150** 通过；115 个额外程序（含 40 个别名组合、24 个活跃压力和 16 个仿射边界生成用例）分别验证 `-O0`、`-O2`、`-O3`，另有 **20** 个诊断回归。
