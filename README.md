# SysY compiler

C++17 实现的 SysY 编译器，输出 RV64GC / LP64D 汇编。SysY 的解析、语义检查、优化、寄存器分配和指令选择全部在仓库内实现，不使用 LLVM/GCC 等编译器框架，不调用外部语言编译器处理 SysY 源码。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/compiler -S -O2 test/functional/00_main.sy -o build/main.s
```

构建需要 CMake 和普通 C++17 编译器；编译器运行时只依赖 C++ 标准库。

## PASS 与 IR

每次变换都围绕数据表示展开，前后端之间没有工具或文本协议依赖：

```
Source → Token IR → Program IR → resolved Program IR
       → tensor expansion → control-flow IR → register IR → RISC-V assembly
```

- `frontend.cpp` 产生 Token IR 和结构化 Program IR。
- `semantic.cpp` 绑定符号、检查类型和数组形状、计算常量，并将嵌套初始化转为稀疏的行优先初始化。
- `tensor.cpp` 将 tensor 操作展开为循环。逐元素表达式融合到同一个循环，标量广播只求值一次；矩阵乘法使用临时结果保证别名安全。返回 tensor 使用调用方提供的缓冲区。
- `lowering.cpp` 将结构化控制流变成基本块，将短路表达式变成分支，将访存和调用显式化。优化裁剪不可达块、提升不逃逸的局部标量、删除无用计算。
- `backend.cpp` 在控制流图上计算活跃信息，以线性扫描分配整数和浮点寄存器，溢出到栈；生成符合 [RISC-V psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/) 的汇编。常量和地址按需重新生成，减少溢出槽。

优化前的临时值只有一个定义；标量提升后，变量对应可多次写入的虚拟寄存器。当前不要求 SSA，后续可以在这一边界独立引入 SSA、循环优化或其他分配器。每个优化 PASS 后检查终结指令、操作数范围、指令元数、类型和调用签名。

```sh
build/compiler --emit-tokens input.sy
build/compiler --emit-hir input.sy
build/compiler --emit-expanded-hir input.sy
build/compiler --emit-ir -O0 input.sy
build/compiler --emit-ir -O2 input.sy
build/compiler --emit-ir --passes=prune,promote,dce input.sy
```

默认 `-O0`。`-O1`、`-O2`、`-O3` 当前使用同一组优化 PASS。`-o -` 输出到标准输出，`-` 输入从标准输入读取。错误带文件、行、列；失败不会覆盖已有输出文件。

## QEMU 测试

运行提供的 `.sy`、`.in`、`.out` 用例：

```sh
python3 tools/test.py test
python3 tools/test.py test/functional test/h_functional --opt 0 --output build/o0-results
python3 tools/regression.py
```

测试需要 `qemu-riscv64`、`riscv64-linux-gnu-as`、`riscv64-linux-gnu-ld` 和 RISC-V Linux libc。汇编器和链接器只负责连接本编译器生成的汇编与 `libs/libsysy_riscv.a`；不调用任何外部语言编译器。测试执行器目前使用动态 libc，运行库本身使用提供的静态 archive。

工具优先从 PATH 查找，也可用 `--as`、`--ld`、`--qemu`、`--sysroot` 指定路径；支持用户目录 `.cache/sysy-toolchain/root` 中解包的工具。输出、汇编、可执行文件、运行时间和 JSON 汇总保存在 `build/test-results`。浮点输出按 `1e-6` 绝对或相对容差比较，整数精确比较；程序退出码作为最后一个输出项，计时 stderr 单独保存。

提供的 `test/` 数据保留在本地，不将大体积测试数据加入版本库。可复现的额外回归用例放在 `tools/regression.py`，覆盖变参、两组参数寄存器耗尽、调用中保留大量活跃值、NaN 条件、初始化副作用、大栈帧、tensor 别名与短路调用，以及非法程序诊断。

语言依据为 `docs/SysY-Language-Definition.md` 和 `docs/SysY-Runtime-Library.md`。运行库的浮点输出采用提供的 `%a` 格式。为兼容现有测试，普通表达式也接受关系和逻辑运算，非 void 函数落到末尾返回零。

## Tensor 扩展

根据额外测试支持 `tensor int/float` 数组、逐元素 `+ - * / %`、一元 `+ -`、标量广播、`@` 二维矩阵乘法、tensor 参数和返回值。逐元素操作要求相同形状；矩阵乘法要求内部维度匹配；`%` 只接受整数。返回形状从函数体推导，各个 return 必须形状一致。tensor 运算和返回需要静态已知的维度；省略首维的 tensor 参数可按普通数组索引访问。

本地验证：`-O2` 下全部 **210/210** 用例通过，`-O0` 下功能用例 **150/150** 通过；额外 **24** 次执行回归和 **20** 个诊断回归通过。
