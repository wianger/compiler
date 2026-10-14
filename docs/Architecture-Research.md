# 编译器架构与技术依据

2026-10-14，基于当前分支实现及本地测量。未读取本仓库其他分支。所有算法均由本仓库的 C++ 实现，外部项目提供设计资料，没有引入编译器框架或后端依赖。

## 实际采用的设计

| 阶段 | 实现与依据 | 实际用途与边界 |
|---|---|---|
| Token / 解析 | 源码切片 Token、连续 Token 数组、稳定索引的块状 AoS arena | 普通 Token 不复制字符；节点每 128 个成块分配，避免逐节点分配与递归销毁。孩子引用仍包含 arena 指针和索引，变长字段仍用 vector；尚非完整 SoA |
| 语义 / HIR | 显式栈的有序后序遍历；保留类型、符号及 tensor 形状 | 消除长表达式的递归语义栈；2 万项表达式在默认 8MB 栈下通过 ASan/UBSan。语义属性仍在节点中，没有不可变 CST 或增量查询系统 |
| CFG / SSA 分析 | 密集 Value ID、按准确拓扑快照缓存支配分析 | 指令改写保留缓存，改边、重排或增删块自动失效；比较完整边数组，避免哈希碰撞导致错误复用。不是所有分析都已缓存 |
| 常量 / 值编号 | def-use 工作队列 SCCP、结构化 hash-cons 键、支配树作用域撤销表 | 只重新处理受变化影响的用户；GVN 避免字符串序列化和逐子树复制整个表达式表 |
| 内存 | 全内存 MemorySSA，MemoryDef / Use / Phi、别名 clobber walker、查询缓存 | 替换可用加载状态的反复全图扫描；按对象来源与调用读写摘要跳过不相关写入。数组参数之间及参数与全局对象仍可能别名 |
| 等价改写 | 有节点、匹配次数及迭代预算的整数 EGraph | union-find、hash-cons、congruence rebuild、恒等式、常量结合、公共因子提取、成本提取；在纯整数区域内工作，副作用和浮点运算保持 CFG 顺序。尚非跨 CFG 的完整 AEGraph |
| tensor / 循环 | 独立 TensorSchedule IR，显式迭代域、列分块、尾块及归约顺序 | 语义和调度分开；一般内核使用四列，较宽浮点矩阵使用八列；已证明一致性的矩阵保留专用算法。没有通用 polyhedral 搜索或自动调参框架 |
| 指令选择 | 独立目标 MIR、RV opcode、GP / FP bank、early use / late def、调用 clobber | 常量除法、比较及地址计算在 RA 前展开，所有算术临时值参与分配。普通操作数内联；ABI 搬运、frame、访存、清零及重物化仍为显式 pseudo |
| 寄存器分配 | 精确程序点区间、复制 bundle、优先队列、驱逐重试、热溢出区间拆分 | 使用 Briggs 保守合并；拆分为指令附近的短片段，重新分配，产生显式 spill slot。调用参数保留原范围；尚无完整跨调用区域拆分和 regalloc2 全部约束模型 |
| 验证 / 输出 | 独立冲突图与 ABI 检查、分配后符号 must-dataflow、活跃压力生成测试 | 符号检查跟踪物理位置包含哪些当前虚拟值，汇合取交集，检查复制、调用破坏及溢出。不是完整机器语义的形式证明 |

代码分别位于 [arena.hpp](../src/arena.hpp)、[ir.hpp](../src/ir.hpp)、[optimizer.cpp](../src/optimizer.cpp)、[rewrite.cpp](../src/rewrite.cpp)、[tensor.cpp](../src/tensor.cpp)、[target_ir.hpp](../src/target_ir.hpp)、[selection.cpp](../src/selection.cpp)、[backend.cpp](../src/backend.cpp)。`--stats` 记录每个 PASS、验证、选择、分配的耗时及计数；指标与复现见 [Performance.md](Performance.md)。

```text
Source → Token IR → arena Program IR → resolved HIR
       → tensor facts → TensorSchedule IR → expanded HIR
       → CFG/SSA → MemorySSA / bounded EGraph → optimized CFG
       → phi edge copies → target MIR → split / allocate
       → allocated locations → frame / ABI expansion → assembly
```

`--emit-mir` 已打印目标机器指令，而非此前复用的通用寄存器 IR。`--emit-allocated-mir` 打印物理位置、重物化及拆分栈槽；`--check-alloc` 运行符号检查器。`--emit-memory-ir` 和 `--emit-tensor-plan` 分别查看内存版本及调度。

## 前端选择：arena 与红绿树

[rust-analyzer 的语法架构](https://rust-analyzer.github.io/book/contributing/syntax.html)分为不可变 Green 数据、可导航 Red/Syntax 视图及 AST 视图，服务于无损语法、错误恢复及持续编辑。[Zig 0.14.1 的 AST](https://github.com/ziglang/zig/blob/0.14.1/lib/std/zig/Ast.zig)和 [MultiArrayList](https://github.com/ziglang/zig/blob/0.14.1/lib/std/multi_array_list.zig)展示了节点索引、额外操作数池及字段分离布局。此处 Zig 引用固定版本。

本项目是单文件批量编译器，优先采用源码切片和稳定 arena，验证分配、遍历和长表达式的收益。红绿树在增加 LSP、格式化或持续编辑时有明确用途。进一步压缩孩子 ID、共享操作数池和语义侧表需要结合内存测量实施；当前没有把块状 AoS 称为完整扁平 SoA。

## CFG、分析与等价图

[V8 在 2025-03 的 Turboshaft 说明](https://v8.dev/blog/leaving-the-sea-of-nodes)支持保留 CFG 的工程选择。[Cranelift 的块参数 SSA](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/docs/ir.md)可作为后续边上传值表示的参照；本项目目前仍用 Phi，保留唯一定义、支配和入边验证。

[现代分析管理器的失效协议](https://llvm.org/docs/NewPassManager.html)强调分析保留与失效。本项目先实现精确拓扑失效的 CFG 缓存，其他分析按 PASS 重建，保持失效规则可验证。[MemorySSA](https://llvm.org/docs/MemorySSA.html)提供内存版本与 alias walker 的设计依据；本项目自行构造版本图，保守合并潜在别名，不假定数组参数独立。

[egg 论文](https://arxiv.org/abs/2004.03082)提供 EGraph、rebuilding 和 extraction 的依据。[Cranelift AEGraph，2026-04-09](https://cfallin.org/blog/2026/04/09/aegraph/)保留副作用 CFG 骨架，将纯计算放入等价图再展开。本项目采用其逐步引入等价表示的方向，先限制到纯 int32 区域：所有结合与因式改写遵守模 2³²，严格浮点不参与。每区域最多 256 条原指令、4096 个图节点、五轮改写，每轮匹配有独立预算。成本提取限制搜索开销，尚未统一跨块 GVN、MemorySSA 和代码移动。

## MIR 与分配

[GlobalISel 的流水线](https://llvm.org/docs/GlobalISel/Pipeline.html)区分合法化、bank 选择和指令选择；[ISLE](https://cfallin.org/blog/2023/01/20/cranelift-isle/)用类型化规则描述模式。本项目借鉴边界和约束表达，用 C++ 规则生成自己的 RV64 MIR，没有实现 ISLE DSL 或使用 GlobalISel。三地址目标指令暴露真实临时值，ABI pseudo 承担固定位置搬运，并声明调用者保存寄存器的破坏。

[regalloc2 的 ION 文档](https://github.com/bytecodealliance/regalloc2/blob/main/doc/ION.md)与[作者设计说明](https://cfallin.org/blog/2022/06/09/cranelift-regalloc2/)提供区间、bundle、优先级、驱逐及拆分的依据。[RAGreedy 实现](https://llvm.org/doxygen/RegAllocGreedy_8cpp_source.html)也包含区间拆分机制。本项目将这些可用设计落到自己的分配器：early/late 半开区间、ABI mask、复制合并、低成本 bundle 驱逐、最多三次尝试及热溢出拆分。独立冲突分析验证实际分配，符号检查再验证位置中的值；它不是 regalloc2 或 RAGreedy 的完整移植。

## 调度与验证的后续边界

[MLIR Transform 教程](https://mlir.llvm.org/docs/Tutorials/transform/)将调度表达为变换 IR；本项目的 TensorSchedule 使用同一原则，迭代域、归约顺序、结果快照和列分块都能在展开前检查。当前选择使用形状和寄存器预算，不依赖测试名称或预期输出，不据 QEMU 时间推断硬件指令延迟。

[CLIR，2026-06 预印本](https://arxiv.org/abs/2606.26977)以结构及活跃度指导生成测试。本项目借鉴压力构造方向，固定种子生成 24 个具有 25–48 个重叠循环状态的程序，独立计算 int32 结果，覆盖因式提取、交换边及拆分；这不是直接实现论文生成器。[VeriISLE](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/isle/veri/README.md)与 [ARRIVAL 论文](https://cfallin.org/pubs/oopsla2025_arrival.pdf)为规则链形式验证提供后续依据。

[Salsa](https://salsa-rs.github.io/salsa/reference/algorithm.html)适合未来的多文件及持续编辑查询；[egglog](https://github.com/egraphs-good/egglog)可供研究更统一的推理模型。目前优先维护可测量的批量编译性能、严格浮点、别名安全和独立验证。编译耗时、RSS、程序运行时间与代码大小分别记录。
