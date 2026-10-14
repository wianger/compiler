# 编译器架构与技术依据

2026-10-14，基于当前分支实现及本地测量。未读取本仓库其他分支。所有算法均由本仓库的 C++ 实现，外部项目提供设计资料，没有引入编译器框架或后端依赖。

## 实际采用的设计

| 阶段 | 实现与依据 | 实际用途与边界 |
|---|---|---|
| Token / 解析 | 源码切片 Token、连续 Token 数组、稳定索引的块状 AoS arena | 普通 Token 不复制字符；节点每 128 个成块分配，避免逐节点分配与递归销毁。孩子引用仍包含 arena 指针和索引，变长字段仍用 vector；尚非完整 SoA |
| 语义 / HIR | 显式栈的有序后序遍历；保留类型、符号及 tensor 形状；按阶段释放语法 arena | 消除长表达式的递归语义栈；降低后清除语法引用并释放节点，符号身份和全局数据继续有效。没有不可变 CST 或增量查询系统 |
| CFG / SSA 分析 | 密集 Value ID、按准确拓扑快照缓存支配分析 | 指令改写保留缓存，改边、重排或增删块自动失效；比较完整边数组，避免哈希碰撞导致错误复用。不是所有分析都已缓存 |
| 常量 / 值编号 | def-use 工作队列 SCCP、结构化 hash-cons 键、支配树作用域撤销表 | 只重新处理受变化影响的用户；GVN 避免字符串序列化和逐子树复制整个表达式表 |
| 内存 | 字节区间别名分析、MemorySSA、协作 GVN、稀疏 SROA、局部 DSE | 区分同一对象的不同元素；值编号与加载替换即时传播。仅标量化不逃逸且访问位置固定的数组；DSE 保留所有可能观察旧值的读与调用。参数间仍可能别名 |
| 等价改写 | 单基 SSA 仿射摘要、共享感知归一化、有预算的整数 EGraph | 仿射 PASS 跨区域保留模 2³² 的系数与偏移，只吸收单次使用的整数算术；EGraph 做 hash-cons、congruence rebuild 和成本提取。尚非跨 CFG 的完整 AEGraph |
| tensor / 循环 | 独立 TensorSchedule IR；普通数组的 PolyRegion、仿射映射、整数依赖集合及调度 | tensor 保留专用算法；二重矩形循环以空集证明保护交换与 unroll-and-jam，按访存及寄存器预算选择。尚无一般符号域、skewing、波前或通用调度搜索 |
| 指令选择 | 独立目标 MIR、RV opcode、bank、early / late、clobber；展开后局部值编号、复制传播和工作队列 DCE | 消除除法与取余等展开的重复子式；定义版本保护降 Phi 后的可变值，常量仍为独立重物化配方。压缩有效 Value ID；尚无跨块目标 GVN |
| 寄存器分配 | 稀疏 64 位活跃块、有序占用区间、复制 bundle、优先队列、驱逐、热溢出拆分 | Briggs 合并保留热 bundle 的寄存器选择；较高压力下使用短重载簇，调用及块边界终止共享。尚无 min-cut 全局区域拆分或 regalloc2 全部约束模型 |
| 验证 / 输出 | 独立冲突图与 ABI 检查、分配后符号 must-dataflow、活跃压力生成测试 | 符号检查跟踪物理位置包含哪些当前虚拟值，汇合取交集，检查复制、调用破坏及溢出。不是完整机器语义的形式证明 |

代码分别位于 [arena.hpp](../src/arena.hpp)、[ir.hpp](../src/ir.hpp)、[optimizer.cpp](../src/optimizer.cpp)、[rewrite.cpp](../src/rewrite.cpp)、[tensor.cpp](../src/tensor.cpp)、[polyhedral.cpp](../src/polyhedral.cpp)、[target_ir.hpp](../src/target_ir.hpp)、[selection.cpp](../src/selection.cpp)、[backend.cpp](../src/backend.cpp)。`--stats` 记录每个 PASS、验证、选择、分配的耗时及计数；指标与复现见 [Performance.md](Performance.md)。

```text
Source → Token IR → arena Program IR → resolved HIR
       → tensor facts → TensorSchedule IR → expanded HIR → PolyRegion / dependence sets
       → CFG/SSA → MemorySSA / affine summaries / bounded EGraph → optimized CFG
       → phi edge copies → target MIR → local combine / compact → split / allocate
       → allocated locations → frame / ABI expansion → assembly
```

`--emit-mir` 已打印目标机器指令，而非此前复用的通用寄存器 IR。`--emit-allocated-mir` 打印物理位置、重物化及拆分栈槽；`--check-alloc` 运行符号检查器。`--emit-memory-ir`、`--emit-tensor-plan`、`--emit-poly-ir` 分别查看内存版本、tensor 调度、普通循环域与依赖约束。

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

## 进一步调研与实测筛选（2026-10-14）

[rustc 的 THIR 文档](https://rustc-dev-guide.rust-lang.org/thir.html)明确区分稳定的语义身份与临时的函数体 IR：不再需要的 THIR 及时释放。本项目在所有函数完成 CFG 降低后释放整个语法 arena，并先清除函数体、维度和初始化器中的节点引用；函数签名、Symbol 和全局初值继续有效。`hir.release` 单独计时。

[Cranelift 的有类型整数重写规则](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/codegen/src/opts/arithmetic.isle)与 AEGraph 提供组合整数恒等式的参照。检查长表达式时发现，有预算的 EGraph 把区域外表达式当作普通叶子，不能归一化跨区域的长链。新增独立 `affine` PASS，以 RPO 单次扫描构造 `(base Value ID, uint32 scale, uint32 offset, cost)` 摘要；只展开单次使用的整数算术，保留共享值，按操作数成本决定重建。可被下一条算术吸收的值只传摘要，避免先构造大量无用常量。新常量置于参数读取之后，维持 ABI 约束；随后 DCE 再进入 EGraph。这是模 2³² 的单基标量归一化，不是一般 polyhedral 分析；Phi、加载、调用和浮点转换作为不透明的已求值输入。

[GlobalISel 的 Combiner 扩展点](https://llvm.org/docs/GlobalISel/Pipeline.html)支持在不同降低阶段插入优化。这里的 `target.combine` 是自有 C++ PASS，处理合法化后才出现的子式。局部表达式键使用操作码、类型、立即数及输入的值版本；复制传播在源值被重新定义时失效。常量按内容编号，但保留各自配方，避免把重物化变成额外复制和寄存器压力。无用单定义通过工作队列删除，最终压缩 ID。

[regalloc2 的稀疏 BitVec 与 allocation map](https://github.com/bytecodealliance/regalloc2/blob/main/doc/ION.md)指出，活跃度通常全局稀疏、局部密集。本项目使用排序的 `(word ID, uint64)` 数组，按字做并集与差集，空块不储存；物理寄存器的占用则以 `std::map` 保存不相交半开区间。它是平衡有序树，不是 regalloc2 的 BTree 实现。重叠查询从前驱及 lower_bound 开始，驱逐按 bundle 去重。

[Factor 0.102 的 greedy 分配文档，2026-10-02](https://docs.factorcode.org/content/article-compiler.cfg.register-allocation.greedy.html)包含全局 min-cut 和局部使用簇。本项目先实现可测量的局部部分：当 16 个拆分名额全部用满时，相邻使用共享短片段；最大间隔八条指令、最大跨度十六条指令。定义写穿到私有 home，跨块与跨调用不缓存。没有实现 Factor 的物理空闲窗口搜索、min-cut 或末次重着色。

[Magellan，2026-01 预印本](https://arxiv.org/abs/2601.21096)强调把可维护的启发式与参数搜索分开，并在其他负载上检查泛化。此次分别保存候选编译器，比较指令局部拆分、无条件重载簇、压力门控，以及展开后不同的值编号方案；使用原有 60 个程序和独立算术模型筛选。无条件共享及将常量替换为复制的方案均出现退化，未采用。没有引入论文的 AlphaEvolve 服务或 LLVM/XLA 管线。

检查器仍在每个 CFG PASS 后运行。Phi 前驱改用精确的排序数组比较，减少集合分配，并拒绝过去集合相等检查会漏过的重复入边；Phi 在索引操作数前检查结果与 arity。

## 内存位置与细粒度协作（2026-10-14）

[2026 年 AEGraph 实践](https://cfallin.org/blog/2026/04/09/aegraph/)用“加载 → 地址计算 → 加载”的链说明粗粒度 PASS 顺序问题。新增的 `memory` 把结构化 GVN、复制传播、加载消除和只读调用复用放入一次带作用域撤销的支配树遍历：新发现立即规范化后续操作数，末尾统一改写回边。默认管线据此删除三个紧邻 `memory` 的独立 `gvn`；其他位置及单独调用 `gvn` 保留。这是局部推理协作，EGraph 提取和跨块代码移动仍是独立 PASS，没有宣称完整 AEGraph。

[现代 MemorySSA DSE](https://llvm.org/doxygen/DeadStoreElimination_8cpp_source.html)使用访问大小与覆盖关系，提供字节位置分析的参照。本项目以 `(object / opaque SSA base, uint64 byte offset)` 表示位置，Offset 的常量部分按 RV64 模运算归一化；动态索引与 Phi 保留 SSA 身份，不假定不同参数互斥。MemorySSA 和 LICM 可跳过同一对象内已证明不相交的访问；调用仍用保守对象摘要。加载键使用规范位置、类型及 clobber 版本。自有 `dse` 仅在块内反向扫描，最多保留 64 个候选写；必须完整覆盖才能删写，可能别名的加载及调用终止候选。尚无跨 CFG 的全路径 DSE 或部分写拆分。

[SROA 的访问分区与 SSA 提升](https://llvm.org/doxygen/SROA_8cpp_source.html)提供另一条避免循环访存的路径。自有 `sroa` 在 SSA 前将不逃逸、固定位置的局部数组提升为标量，只为实际读取的元素建立 home；未读元素的 store 删除，初始化表达式中的调用继续执行。Zero 按原位置重置观测元素，支持循环内重复声明、分支与多维数组。每对象最多 256 个 home、每函数总预算 1024；动态访问、未知 Phi 地址和传出地址拒绝整对象。它是稀疏标量替换，尚不处理逃逸数组的部分分区或向量提升，也不等同于 LICM 的出口 store 下沉。

这些资料用于研究设计；没有使用 LLVM 库、其 PASS 或外部 SysY 编译器。定向实验分别关闭 SROA、DSE，以及对比分开的 GVN / MemorySSA；四个内存实验使用 Python 整数模型或独立递推矩阵生成预期值。性能和限制见 [Performance.md](Performance.md)。

## 普通循环的多面体 IR 与合法性证明（2026-10-14）

此前只有 tensor 专用调度和标量仿射摘要，缺少普通数组循环的迭代级依赖表示。这确实遗漏了重要优化层。新增 `polyhedral` 在 tensor 展开、HIR 重复归约之后，CFG 降低之前运行；`-O2`、`-O3` 启用。这里的表示与标量 `affine` PASS 分开：前者研究不同迭代的访存与顺序，后者仅归一化 int32 计算。

[MLIR Affine](https://mlir.llvm.org/docs/Dialects/Affine/)说明迭代维度、共享符号参数、访问映射和整数集合；其[依赖分析源码](https://mlir.llvm.org/doxygen/AffineAnalysis_8cpp_source.html)展示源／目的访问关系与执行顺序约束的组合。本项目自行实现一个有限子集，没有调用 MLIR、Polly、isl、Pluto 或 SMT 库：

- `PolyRegion` 保留两维矩形迭代域、Symbol 身份、共享不变参数、读写访问映射、候选依赖及调度决定；`--emit-poly-ir` 输出这些对象及求解结果。循环必须是已知常量起止、正次数、步长一的 int32 循环；内层可以含条件、私有标量、前置初始化和结束存储。
- 访问抽象检查每一步整数索引计算的范围，不能证明不溢出便拒绝仿射映射。内侧下标范围保证行优先映射单射时按逻辑坐标比较，否则使用精确的线性元素偏移。不同局部对象不相交；参数间及参数／全局之间可能别名，未知相对地址阻止变换。
- 对潜在 RAW、WAR、WAW 建立访存地址相等的整数关系，条件按可能执行保守近似。源／目的使用同一组符号参数；相同系数可投影到迭代差空间。外层并行性查询禁止任何跨外层冲突；交换查询禁止两个维度方向相反的冲突。只有禁止集合被证明为空才允许调度，保留各点内语句顺序。
- 整数集合求解使用单位系数等式消元、整除／GCD 检查、区间收缩及分支定界。每个查询最多 96 个搜索节点、深度 24，每轮区间传播最多 32 次；溢出、不支持或预算耗尽返回 `unknown`。非空结果只说明保守模型存在冲突，未必是实际执行的冲突。
- 完美循环按读写步长成本选择交换，写入权重高于读取。没有外层冲突且存在公共内层加载时考虑 unroll-and-jam，默认四列，依据私有整数／浮点临时量和表达式预算缩小；保留标量尾循环。所有私有声明按列重命名，外部循环变量保留最终值。TensorSchedule 产生的循环带 `scheduled` 标记，避免覆盖已有内核决定。
- 对整数私有累加器，单分支 `sum=sum+term` 可变为 `sum=sum+bool(cond)*term`；贡献必须无调用、类型为整数，所有加载的各维下标都已证明有效，除数必须为非零常量。原条件仍按原短路语义求值。浮点累加、带 else 的更新、动态除数和受条件保护的越界加载保留分支，每个浮点输出的归约顺序完全保留。

例如，`A[i][j]=A[i-1][j]+A[i][j-1]` 有跨迭代依赖，但交换不反转方向，能够通过证明；`A[i-1][j+1]` 的依赖方向阻止交换。原地转置也被拒绝。行内偶数位置写入、奇数位置读取的实验则需要整数整除性质，不能只用实数线性松弛判断。这些例子的预期值按原循环顺序由 Python 独立计算。

性能筛选发现：单独四列 jam 让原有三个条件矩阵程序慢约 2–4%，该候选未作为最终实现。安全整数掩码让协作 GVN 复用加载，并使下游展开预算能接纳直线循环，组合效果再实测。不会将“变换合法”当作“必定更快”；完整结果、代码大小和编译开销见 [Performance.md](Performance.md)。`--no-polyhedral` 提供整个 PASS 的对照，`--poly-tile=1|2|4|8` 设置 jam 宽度上限，仍受合法性与资源预算约束。

当前不是完整多面体编译器：尚无运行时符号上下界、三角域、floor/mod 访问关系、一般调度树、skewing／波前、跨循环融合、缓存多级 tiling、运行时别名版本化或自动成本模型训练。普通源代码三重循环可分析其中满足条件的二重子区域；不能进行一般三维重调度。没有把受限整数求解称为完整 Presburger 决策过程。

## 最新研究的覆盖与取舍（2026-10-14）

调研按表示、合法性、成本、降低和验证分别评估。发表年份与算法基础区分开；多面体分析、图着色等基础技术较早，近期工作的价值通常在组合、可扩展性、自动选择和证明。以下资料是原论文或作者仓库，明确区分已采用与待研究部分。

| 方向与原始资料 | 对本项目的实际价值 | 本轮状态与限制 |
|---|---|---|
| [LOOPer，PACT 2025，2025-12 修订](https://arxiv.org/abs/2403.11522)；[LOOPerSet，2025-12 修订](https://arxiv.org/abs/2510.10209) | 组合循环变换与学习型成本模型，需要丰富的调度／程序样本 | 引入可观测域、关系、决定及独立模型生成测试；尚未实现论文的调度搜索、数据集或训练模型 |
| [参数调优与结构优化分离，2026-09](https://arxiv.org/abs/2609.03114) | 先证明并选择循环结构，再搜索 tile 等数值参数，便于区分合法性与性能 | jam 宽度独立于证明，可用 CLI 做对照；目前是预算启发式，尚无坐标爬山或自动调优器 |
| [Loom，2026-09](https://arxiv.org/abs/2609.29219) | 显式硬件描述导出合法性、资源与延迟约束，将调度参数保持为符号 | 资源预算与合法性分别处理；Loom 针对空间数据流加速器，其 DMA／多核延迟模型不适用于当前 RV64GC，未照搬 CP-SAT 管线 |
| [Scalable Packed Layouts，2026-05](https://arxiv.org/abs/2605.12445) | 向量长度、布局、tiling 与 vectorization 共同决定收益 | 当前 ISA 无向量扩展，不能应用 SVE 代码。RVV 后端需要独立的目标约束、MIR、分配和硬件测量，尚未实现 |
| [稀疏结构专化，IMPACT 2025](https://www.impact-workshop.org/impact2025/papers/asppss.pdf) | 输入稀疏结构已知时传播非零迭代域，避免无效工作 | SysY 没有稀疏布局契约，严格浮点还涉及 NaN／零乘法，不能按整数模型跳过所有零项；尚未实现稀疏专化 |
| [Protean，TACO 2026，2026-06 修订](https://arxiv.org/abs/2602.06142) | 细粒度程序特征、PASS 顺序与外部模型协作 | 现有 `--passes`、统计、候选保存和单项对照支持实验；它们不是 Protean 的特征库或学习型 phase-ordering |
| [ISA 规范合成指令选择，CGO 2026](https://db.in.tum.de/~drescher/paper-isel-syn.pdf) | 位向量规范化索引筛选匹配，用 SMT 验证其余候选；减少手写规则维护成本 | 本项目已有结构键与独立整数模型，但没有 ISA 形式语义、SMT 或规则合成器。论文系统生成 LLVM 规则，本项目未使用该系统 |
| [regalloc3 作者仓库](https://github.com/Amanieu/regalloc3) | 分块频率、重物化、工作区复用，以及复杂 bank／组合寄存器约束 | 已有循环权重、常量重物化和两种 bank；未实现真实块频率、通用组合寄存器、跨函数分配工作区或完整 regalloc3。当前标量目标暂不需要复杂寄存器组 |

前端也按负载重新评估：一次性批量 SysY 输入仍没有红绿树的编辑复用场景；已有 arena 与长表达式测试针对当前需求。更紧凑节点索引／操作数池、函数级内存复用，需先测编译 RSS 和耗时；不会仅因其他编译器采用某种结构就增加一套重复表示。

后续优先级是补足可证明的符号域与别名版本化，再扩展 skewing／tiling 的调度候选和成本测量；向量化需要先确定 RVV 目标。每项都要有独立语义模型、关闭该项的对照、编译时间／RSS、代码大小和运行测量。QEMU 只能评估当前执行器，不能作为真实芯片缓存及延迟的完整成本模型。

## 调度与验证的后续边界

[MLIR Transform 教程](https://mlir.llvm.org/docs/Tutorials/transform/)将调度表达为变换 IR；本项目的 TensorSchedule 使用同一原则，迭代域、归约顺序、结果快照和列分块都能在展开前检查。当前选择使用形状和寄存器预算，不依赖测试名称或预期输出，不据 QEMU 时间推断硬件指令延迟。

[CLIR，2026-06 预印本](https://arxiv.org/abs/2606.26977)以结构及活跃度指导生成测试。本项目借鉴压力构造方向，固定种子生成 24 个具有 25–48 个重叠循环状态的程序，独立计算 int32 结果，覆盖因式提取、交换边及拆分；这不是直接实现论文生成器。[VeriISLE](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/isle/veri/README.md)与 [ARRIVAL 论文](https://cfallin.org/pubs/oopsla2025_arrival.pdf)为规则链形式验证提供后续依据。

[Salsa](https://salsa-rs.github.io/salsa/reference/algorithm.html)适合未来的多文件及持续编辑查询；[egglog](https://github.com/egraphs-good/egglog)可供研究更统一的推理模型。目前优先维护可测量的批量编译性能、严格浮点、别名安全和独立验证。编译耗时、RSS、程序运行时间与代码大小分别记录。
