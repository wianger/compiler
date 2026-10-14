# 编译器架构审计与技术调研

调研日期：2026-10-14。实现审计基于当前分支的 `18aedc7`，未读取本仓库其他分支。本文区分实际实现、外部技术及建议方案；建议方案尚未实现，收益需要实验验证。外部项目只作为设计资料，不作为本编译器的框架或后端依赖。

## 当前实现的准确边界

| 层次 | 已实现 | 尚未实现 |
|---|---|---|
| 词法 | 手写扫描器，连续 Token 数组，行列诊断 | 源码切片 Token、标识符驻留、无损 trivia、SIMD 扫描 |
| 语法 | 递归下降、优先级爬升表达式解析 | 红绿 CST、错误恢复、增量解析 |
| AST / HIR | `unique_ptr` 指针树；语义信息写入节点；保留 tensor 形状 | 索引化 arena AST、字段分离的数组布局、独立语义侧表 |
| 标量 IR | 带 Phi 的 CFG/SSA、密集整数 Value ID、结构验证 | 块参数 SSA、稳定 Instruction ID、维护中的 def-use 索引 |
| 分析 / 优化 | 支配关系、循环、范围、指针来源、调用读写摘要、GVN、跨块加载消除 | MemorySSA、分析缓存与失效协议、AEGraph / equality saturation |
| 循环 / tensor | 在结构化源 IR 中分析生产者、展开 tensor，之后变换 CFG 循环 | 独立 Loop/Tensor IR、显式调度 IR、通用依赖距离分析 |
| 指令选择 | 手写规则；显式比较分支；很多目标选择仍在汇编输出器中完成 | GlobalISel 风格完整流水线、ISLE 风格规则系统、独立目标 MIR |
| 寄存器分配 | 冲突图、Briggs 合并、循环权重、ABI 约束、重物化常量/地址 | RAGreedy 或 regalloc2、活跃区间拆分、驱逐与重试、分段溢出 |
| 代码输出 / 验证 | 汇编输出；IR 结构验证、执行回归、差分测试、ASan/UBSan | 分配后语义检查器、规则等价性证明、独立 IR 解释器 |

可核对 [ir.hpp](../src/ir.hpp)、[frontend.cpp](../src/frontend.cpp)、[machine_ir.hpp](../src/machine_ir.hpp)、[optimizer.cpp](../src/optimizer.cpp)、[backend.cpp](../src/backend.cpp)。`--emit-mir` 当前打印的是降 Phi、选择比较分支后的寄存器 IR，仍复用 `FunctionIR` 和通用 `Op`；它还没有独立机器指令、操作数时序、固定寄存器或 clobber 的完整表示。

SSA、支配树、SCCP、GVN 和 Briggs 合并都是成熟基础技术。参考现代项目的讨论，不能等同于已经实现其架构或算法。

## 前端：红绿树与扁平 AST

[rust-analyzer 的语法架构](https://rust-analyzer.github.io/book/contributing/syntax.html)分为不可变 Green 数据、提供导航的 Red/Syntax 视图及 AST 视图，强调无损解析、错误恢复与解析器和树结构的解耦。红绿树与扁平 AST 可以分别存在于语法层和语义层，并不需要二选一。

本项目的工程判断：当前主要处理单文件批量编译，红绿树的增量编辑与源码保真能力不是现有性能瓶颈的已验证解决方案。如果增加 LSP、格式化或持续编辑，应引入包含注释、空白及错误节点的 CST；类型、符号和常量信息放在语义侧表中，保持 Green 数据不可变。

[Zig 0.14.1 的 Ast.zig](https://github.com/ziglang/zig/blob/0.14.1/lib/std/zig/Ast.zig)使用 `MultiArrayList` 存储 Token 和 Node，通过索引与 `extra_data` 表示额外数据；[MultiArrayList 实现](https://github.com/ziglang/zig/blob/0.14.1/lib/std/multi_array_list.zig)说明了字段分离布局的内存与缓存动机。这里引用固定版本作为可核查实例，不将它称为 Zig 的最新版本。

适合本项目先评估的方案：

- Token 保存 kind、源码起止位置；解码后的字符串单独存储，标识符使用驻留 ID。
- AST 节点使用 `NodeId`；常见一元、二元子节点内联，多子节点使用共享 operand pool。
- Type、Symbol、Function 使用各自的 ID；类型和数组形状驻留，语义结果放入侧表。
- 先采用连续 AoS arena，再根据访问模式比较 SoA；扁平存储也需要控制变长字段的额外分配。
- arena 扩容期间只保存 ID，避免保留失效引用。长表达式的解析、遍历和销毁都应有栈深度测试。

这些改变主要影响编译耗时、峰值内存和维护成本；生成程序的运行速度需要通过后续 IR 与后端改进获得。

## IR：保留 CFG，明确身份与副作用

[Cranelift IR](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/docs/ir.md)用块参数和分支实参表达 SSA 汇合。本项目可以评估这种形式，尤其是让 CFG 边直接携带传值关系，减少多个变换各自维护 Phi 入边的代码。Phi 和块参数的选择本身不保证性能提升。

[V8 在 2025-03 的架构说明](https://v8.dev/blog/leaving-the-sea-of-nodes)解释了迁移至 CFG 型 Turboshaft 的理由。这也说明 Sea of Nodes 并不是所有编译器都应采用的唯一方向。对当前实现，保留 CFG/SSA，补上稳定 ID、def-use 和明确副作用，更便于继续验证循环与别名变换。

[MemorySSA](https://llvm.org/docs/MemorySSA.html)用 MemoryDef、MemoryUse、MemoryPhi 表达内存版本关系，再结合别名查询寻找实际 clobber。当前 `memoryForward` 是可用加载的数据流分析，没有这种内存版本图。

建议从保守的全内存版本链开始，再结合现有对象来源和调用摘要消歧。不能为不同数组参数直接建立互不相关的内存链，因为参数之间、参数与全局对象之间可能别名。调用、未知地址和 CFG 改写都必须纳入更新规则。

[现代分析管理器的失效协议](https://llvm.org/docs/NewPassManager.html)允许 PASS 声明保留哪些分析，并更新或失效其余结果。当前 `runPasses` 是字符串分发，多个 PASS 会重复构造支配关系、循环和摘要。

适合本项目的最小实现是：PASS 返回 changed、CFG changed、memory changed；分析按函数和修订号缓存，默认保守失效。IR 重编号、删除函数和修改调用图都需要明确失效，不能仅以裸指针作为长期缓存键。每层 IR 的验证器仍保留。

## EGraph / AEGraph：改写基础设施

普通 GVN 合并相同计算；EGraph 进一步表示多种等价表达式。Equality saturation 是在这种表示上反复应用规则的一种搜索方式。它与指令选择流水线是不同层面的技术，可以配合使用。[egg 论文](https://arxiv.org/abs/2004.03082)介绍了相关基础设施；[egglog](https://github.com/egraphs-good/egglog)进一步结合等式推理和 Datalog。

[Cranelift 的 AEGraph 说明，2026-04-09](https://cfallin.org/blog/2026/04/09/aegraph/)提出保留副作用 CFG 骨架，将纯表达式提升到图中进行规范化和改写，再放回 CFG。它针对 GVN、加载消除、常量传播等需要互相反复触发的 PASS 顺序问题。这与当前流水线多次运行 `gvn,memory,simplify` 的情况直接相关。

本项目的候选方案是有预算限制的局部纯整数图，然后再评估跨块 AEGraph：

- 分别指定整数宽度、溢出、除零和 `INT_MIN / -1` 语义；避免照搬其他 IR 的未定义行为假设。
- 保持现有严格浮点语义；浮点结合律、消去规则和 FMA 合并需要单独证明条件。
- Load 和只读 Call 需要内存版本；无外部读写的调用也可能不终止，不能任意投机执行。
- 提取成本考虑 RV64 指令展开、表达式共享、热度与寄存器压力；单纯最小表达式树可能增加实际溢出。
- 限制节点数量、规则触发次数和迭代次数；饱和停止与最低成本提取不是同一件事。

## GlobalISel / ISLE：先建立真正的 MIR

[GlobalISel 的核心流水线](https://llvm.org/docs/GlobalISel/Pipeline.html)包含 IR 转换、合法化、寄存器 bank 选择和目标指令选择。[ISLE](https://cfallin.org/blog/2023/01/20/cranelift-isle/)是用于声明模式与改写的类型化 DSL。前者描述后端组织方式，后者提供规则表达机制；`ISEL` 则是 instruction selection 的通称。当前实现具备少量模式选择，但没有实现这些完整机制。

建议的 C++ 后端边界：

```text
CFG/SSA → legalize → generic register IR
        → target pattern selection → target MIR
        → instruction scheduling → allocation → allocated MIR
        → frame layout / assembly emission
```

目标 MIR 应显式表示 opcode、def/use、寄存器类别、立即数、内存操作、隐式 clobber、调用约束和临时寄存器。多条机器指令组成的常量除法、地址计算等序列，应在适当层次暴露实际临时值和存活区间，避免仅在文本输出时扩展。

本项目可以先用 C++ 的类型化模式表实现有限规则，不必先创建完整 DSL。规则需要适用条件、成本、覆盖关系和可测试的输出。显式 MIR 也为指令调度提供基础；调度同时考虑依赖、内存顺序和寄存器压力。QEMU 时间不足以单独校准真实硬件的延迟模型。

## RAGreedy / regalloc2：分配可拆分的活跃区间

[RAGreedy 源码](https://llvm.org/doxygen/RegAllocGreedy_8cpp_source.html)包含活跃区间拆分等机制；[regalloc2 设计文档](https://github.com/bytecodealliance/regalloc2/blob/main/doc/ION.md)描述了区间、bundle、寄存器约束、驱逐、拆分和溢出处理。当前分配器的贪心选色步骤并不使它成为 RAGreedy；Briggs 复制合并也不等同于 regalloc2 的 bundle 分配。

当前一个虚拟值主要被分配到一个寄存器或一个栈槽，跨调用存活就限制到被调用者保存的寄存器。下一阶段可在调用前后、循环边界或高压力区域拆分：热段使用寄存器，冷段或调用附近使用栈槽，并在分段连接处插入搬运。

需要的基础是精确到机器指令程序点的活跃区间、早/晚 use/def、固定寄存器、重用输入、clobber，以及显式并行复制。先验证这一约束模型，再实现优先队列分配、驱逐重试和拆分；否则复杂算法也看不到输出器中隐藏的约束。

## 其他值得调研与采用的方向

| 方向 | 一手依据 | 对本项目的判断 |
|---|---|---|
| 调度也作为 IR | [MLIR Transform dialect](https://mlir.llvm.org/docs/Tutorials/transform/) | 将 tensor 语义与分块、展开、布局、内核选择分开表达，符合“PASS 的结果也是 IR”的要求 |
| 查询式增量编译 | [Salsa 算法](https://salsa-rs.github.io/salsa/reference/algorithm.html) | 多文件或持续编辑时评估；当前先实现函数内分析缓存，收益与复杂度更容易测量 |
| 规则链形式验证 | [VeriISLE](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/isle/veri/README.md)、[OOPSLA 2025 ARRIVAL 论文](https://cfallin.org/pubs/oopsla2025_arrival.pdf) | 借鉴对规则链和 ISA 语义进行验证的方法，为自己的 RV64GC 规则建立语义；不直接依赖外部编译器 |
| 活跃度驱动的 IR 模糊测试 | [CLIR，2026-06 预印本](https://arxiv.org/abs/2606.26977) | 根据活跃区间和结构生成合法 SSA，集中覆盖高压力、调用、溢出及特殊操作数约束 |

现有 SSA 验证器检查结构合法性；QEMU 回归检查有限输入的输出；ASan/UBSan 检查宿主实现的部分错误。这些都不是优化或寄存器分配的完整等价性证明。建议补充独立 IR 解释器与分配后符号值检查器，再逐步引入对整数、浮点和机器规则的验证。

## 推荐的实施顺序与验收

以下是针对本仓库的工程建议，不是上述项目给出的结论，也不是已经获得的性能结果。

| 顺序 | 必要增量 | 验收重点 |
|---|---|---|
| 1 | 稳定 IR ID、分析修订与失效、独立目标 MIR、机器操作数约束 | IR 和目标指令可打印；所有阶段可独立验证；记录各 PASS 编译耗时和内存 |
| 2 | Token spans、驻留表、arena AST 与语义侧表 | 诊断与语义一致；测量编译耗时、峰值内存和分配次数；覆盖深表达式 |
| 3 | 类型化指令选择规则、精确区间、分配后检查、局部拆分 | ABI、参数交换和溢出语义正确；比较热循环 spill/reload 数量、代码大小与执行时间 |
| 4 | MemorySSA、受控 AEGraph、统一改写工作队列 | 检查别名与不终止调用；验证规则语义与提取质量；比较完整流水线与局部改写的成本 |
| 5 | 独立 Loop/Tensor IR、调度 IR 与自动内核选择 | 明确迭代域、依赖、形状和浮点顺序；分别测量算法、访存和目标代码收益 |

红绿树与跨文件增量查询在持续编辑需求明确时进入实施范围。生成程序运行速度、编译器自身速度、内存、代码大小和可验证性应分别记录，避免用一种指标代表全部架构质量。
