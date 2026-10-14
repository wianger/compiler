# 全阶段编译技术调研与实施取舍

调研截止 2026-10-14。比较对象是当前分支 `8366f03` 和本轮候选；没有读取其他分支。论文、作者仓库和生产编译器资料用于研究，不引入 LLVM/GCC 编译框架，也不调用外部编译器处理 SysY。

## 结论与选取标准

最值得继续做的是**增强可复用的语义事实与验证，再扩大优化的覆盖范围**。当前瓶颈不只是缺少几个算法名称：普通循环的域、内存位置、控制条件、目标约束之间仍有表示和推理断点。只增加一个独立 PASS，往往无法利用其他层已经证明的事实。

本轮已经实施四项：循环内存状态提升与出口回写；有作用域的分支约束与惰性范围查询；完整一些的 MIR 操作数、位宽、边及溢出槽检查；修正未进入分配池的 `t2`，同步寄存器约束和符号检查。另增加独立算术模型、MIR 破坏性测试及离线 SMT 规则证明。它们完善已有 SSA、内存分析、算术优化和分配器，性能保留条件与结果见 [Performance.md](Performance.md)。

下一批最有价值的候选是：符号域／别名版本化、live-out 与共享感知的 EGraph 提取、全路径内存前向／PRE，以及受预算约束的区域拆分。跨层持久等价图、RVV、自动调参具有潜力，但需要先补齐语义契约或目标条件。红绿树、GPU 专用调度、全局 ILP 求解目前没有足够的本项目收益依据。

所有候选同时检查六项：

1. 解决哪一种**当前可复现**的低效代码，是否能覆盖普通 SysY 和 tensor。
2. 等价性需要什么前提；是否依赖不别名、无溢出、浮点重结合或未提供的 ISA 扩展。
3. 需要什么 IR／分析，如何保留或使事实失效；接口是否支持后续迭代。
4. 成本由什么组成：运行、寄存器压力、临时内存、代码大小、编译时间与 RSS。
5. 如何构造独立预期值、反例和单项对照；不能仅凭原有测试全部通过决定采用。
6. 何时拒绝或撤回：无法证明、预算耗尽、稳定退化、收益不覆盖新增成本。

这里的 P0 是本轮完成或必须修正的基础缺口；P1 是优先构造实验的候选；P2 需要更多前提；P3 暂不采用。P1 不等于已经实现或已经证明有收益。基础算法与近期研究区分开，不能将一个较早算法的有限实现称为最新系统的完整移植。

## 覆盖清单

| 层 | 技术与代表资料 | 本项目的用途、缺口与判断 |
|---|---|---|
| 词法 | 源码切片、短字符串、SIMD 字符扫描：[Oxc](https://oxc.rs/docs/learn/architecture/parser.html) | 已用切片 Token；AST 仍复制文本。P2：先测长输入的词法占比，SIMD 不等于必然改善总编译时间 |
| 语法 | 无损红绿树：[rust-analyzer](https://rust-analyzer.github.io/book/contributing/syntax.html) | P3：本项目无持续编辑／格式化需求，增加不可变 CST 会产生重复表示 |
| AST 布局 | 索引节点、extra operand 池、SoA：[Zig AST](https://github.com/ziglang/zig/blob/0.14.1/lib/std/zig/Ast.zig) | 已有块状 AoS arena；16 字节引用、vector 和 Type 字段仍有成本。P2：比较压缩引用、操作符枚举和操作数池，不能用全局 arena 指针换取表面压缩 |
| 语义存储 | 临时函数体 IR 与及时释放：[rustc THIR](https://rustc-dev-guide.rust-lang.org/thir.html) | 已按阶段释放。P2：函数级 arena／释放可降低大文件峰值，但须保留签名、符号和全局初值 |
| 增量查询 | 依赖追踪与缓存：[Salsa](https://salsa-rs.github.io/salsa/reference/algorithm.html) | P3：当前单文件批处理缺少跨编辑复用场景；多文件或服务化后再评估 |
| CFG 组织 | 明确 CFG 与紧凑操作数组：[V8 Turboshaft](https://v8.dev/blog/leaving-the-sea-of-nodes) | 已有明确 CFG。P2：紧凑指令／操作数布局；没有必要为技术名称改成 Sea of Nodes |
| SSA 构造 | pruned SSA、块参数、直接构造：[Cranelift IR](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/docs/ir.md) | 已有活跃度过滤的 Phi 插入。P2：直接 SSA 可减少中间 homes；须兼容新循环状态提升、短路和多个前驱 |
| 分析管理 | 保留与失效：[New Pass Manager](https://llvm.org/docs/NewPassManager.html) | CFG 用精确拓扑快照。P1：指令、值和效果分析的版本／失效协议；避免过早复用陈旧事实 |
| 常量 | def-use SCCP、跨过程常量／返回摘要 | SCCP 已是工作队列。P1：常量参数专化与返回值摘要，须控制克隆增长及递归求解预算 |
| 控制条件 | 边／位置约束：[LazyValueInfo](https://llvm.org/docs/doxygen/classllvm_1_1LazyValueInfo.html)、[CVP](https://llvm.org/docs/doxygen/CorrelatedValuePropagation_8cpp_source.html) | P0：分支作用域范围、惰性查询和比较消除。P1：KnownBits、同余与区间组成产品域，处理更多 Phi／汇合 |
| 冗余计算 | phi translation、PRE：[NewGVN 工程记录](https://blog.llvm.org/posts/2024-09-01-reviving-newgvn/) | 已有支配 GVN，尚不能复用只在各前驱可用的不同值。P1：先做不新增执行的 Phi-of-Ops，再评估插入式 PRE |
| 等价图 | congruence rebuilding、bounded saturation：[egg](https://arxiv.org/abs/2004.03082) | 已实现有限整数区域。P1：只提取 live-out、改善共享成本；先修提取质量再扩大搜索 |
| 持久等价 | [AEGraph](https://cfallin.org/blog/2026/04/09/aegraph/)、[Persistent EGraphs，2026-08 修订](https://arxiv.org/abs/2602.16707) | P2：跨 PASS 保留替代项有价值，但内存版本、支配、作用域与循环需要独立协议；不能把局部协作遍历称为完整持久图 |
| 图成本 | [e-boost，2025](https://arxiv.org/abs/2508.13020) 的 DAG 成本、剪枝与精确求解 | P1：借鉴共享计数与候选剪枝；P3：直接引入并行 ILP。当前区域很小，复杂求解器成本可能大于收益 |
| 内存语义 | 字节 AA、Mod/Ref、MemorySSA：[AA](https://llvm.org/docs/AliasAnalysis.html)、[MemorySSA](https://llvm.org/docs/MemorySSA.html) | 已有对象＋字节分析和版本图。P1：跨 Phi 加载前向、字节级调用摘要及受预算的全路径 DSE |
| 标量替换 | 部分分区／逃逸感知 SROA：[SROA](https://llvm.org/doxygen/SROA_8cpp_source.html) | 当前动态访问拒绝整对象。P1：把固定、不受动态访问影响的分区单独提升，须证明未知访问与分区不重叠 |
| 循环内存 | 必别名状态提升、出口存储：[LICM](https://llvm.org/doxygen/LICM_8cpp_source.html) | P0：补上有状态读写，不再仅限不变加载；支持多个出口，拒绝观察者和重叠访问 |
| 循环形态 | preheader、dedicated exits、LCSSA：[Loop terminology](https://llvm.org/docs/LoopTerminology.html) | 已有自然循环／旋转。P1：统一规范化契约、循环出口值接口，降低各个变换重复处理边的成本 |
| 多面体合法性 | 域、映射、关系、整数集合：[MLIR Affine](https://mlir.llvm.org/docs/Dialects/Affine/) | 当前是二重常量矩形子集。P1：运行时符号域、三角域、floor/mod，必须遵守 int32 索引的溢出语义 |
| 多面体调度 | interchange、fusion、tiling、skewing／波前：[LOOPer](https://arxiv.org/abs/2403.11522) | 已有交换和 jam。P1：先扩大可证明区域，再增加小型组合候选；新调度不能替代依赖证明 |
| 版本化 | 运行时别名／范围保护、优化版本＋回退 | P1：普通数组参数的重要覆盖缺口。需要 RV64 无符号地址比较、区间端点防溢出及零次执行保护 |
| tensor 结构 | 计算与调度分离、tile IR：[MLIR Transform](https://mlir.llvm.org/docs/Dialects/Transform/) | 已有 TensorSchedule。P1：显式 producer／consumer、访问 footprint、临时生存期，支持可证明的融合和存储折叠 |
| 代数与调度 | [Cleave，2026-10](https://arxiv.org/abs/2610.07742) 的符号代数搜索与具体调度分离 | P2：结构思路适用；Split-K／重结合不能直接用于严格 float32。整数候选仍需额外 alias 和顺序证明 |
| 成本与搜索 | [LOOPerSet](https://arxiv.org/abs/2510.10209)、[参数爬山，2026-09](https://arxiv.org/abs/2609.03114) | P1：小候选集、数值参数与结构选择分开、保留原方案。P2：学习模型，需要独立训练／验证负载和目标数据 |
| 硬件约束 | [Loom，2026-09](https://arxiv.org/abs/2609.29219) 的符号资源约束 | P2：显式资源协议有价值；空间加速器 DMA／通信延迟不适用于当前 RV64GC |
| 数据布局 | packing、布局与 scalable vector length：[Scalable Packed Layouts，2026-05](https://arxiv.org/abs/2605.12445) | P2：packing 必须计入复制成本，保持源数组 ABI 和 alias；向量长度策略需要 RVV 目标 |
| 向量化 | loop／SLP、掩码与尾部：[Vectorizers](https://llvm.org/docs/Vectorizers.html) | P2：当前 GC 目标没有 V。先建立 RVV 类型、寄存器组、VL 和运行配置，不能仅在 scalar IR 标注“向量化” |
| 目标降低 | legalization、bank、ISel、target combine：[GlobalISel](https://llvm.org/docs/GlobalISel/Pipeline.html)、[ISLE](https://cfallin.org/blog/2023/01/20/cranelift-isle/) | 已有独立 MIR 和局部 combine。P0：补操作数类型与位宽检查；P1：集中 opcode 描述、规则约束和成本 |
| 规则合成 | [CGO 2026 ISA 规范合成](https://db.in.tum.de/~drescher/paper-isel-syn.pdf) | P2：语义索引＋SMT 可减少手写错误；先建立自有 IR／ISA 语义，不能把少量规则证明称为自动 ISel 合成 |
| 寄存器分配 | bundles、驱逐、拆分、重物化：[regalloc2](https://github.com/bytecodealliance/regalloc2/blob/main/doc/ION.md)、[regalloc3](https://github.com/Amanieu/regalloc3) | P0：修正池和约束编号；P1：边／区域拆分、选择热片段、栈槽复用。复杂寄存器组待 RVV 再扩展 |
| 指令与布局 | 调度、块频率、分支布局、压缩指令成本 | P1：显式块／边计数及 code-size 成本；P2：硬件延迟调度。QEMU 不模拟完整流水线，不能据其计时声称改善芯片 ILP |
| 跨过程／链接 | 热调用专化、LTO、接口摘要：[ThinLTO](https://clang.llvm.org/docs/ThinLTO.html) | P1：当前单模块可以先做预算内参数专化；P3：直接引入多模块 LTO 管线，当前没有对应输入需求 |
| PASS 搜索 | [Protean，TACO 2026](https://arxiv.org/abs/2602.06142)、[Magellan，2026](https://arxiv.org/abs/2601.21096) | P1：局部特征／对照数据；P2：自动选择。不能在 60 个已知用例上调参后称为泛化模型 |
| 正确性 | translation validation：[Alive2](https://github.com/AliveToolkit/alive2)、[Z3](https://github.com/Z3Prover/z3) | P0：自有规则的离线位向量证明及反例测试；P2：完整 CFG／内存翻译验证。没有使用 Alive2 或其 LLVM 输入管线 |
| 测试生成 | [CLIR，2026-06](https://arxiv.org/abs/2606.26977) 的结构与活跃度生成 | 已有独立模型压力／依赖测试。P0：增加边、位宽、守卫、出口和 ABI 反例；P1：自动缩减失败用例与 PASS 定位 |
| 性能验证 | [性能与编译预算形式化，2026-08](https://arxiv.org/abs/2608.20137) | P1：明确搜索上界、原方案回退和成本分项；没有形式证明真实机器性能单调改善 |

## 深入分析：事实的有效范围

现代分析的价值不在于用一个“更强”的区间替换 SCCP，而在于表达事实在哪里成立。`if (x>=0)` 可以简化 true 子树的 `x/8`，不能改变 false 分支、汇合后的使用，或随后赋值得到的新 SSA 值。循环回边也必须重新区分旧值与 Phi 的下一次值。[LazyValueInfo](https://llvm.org/docs/doxygen/classllvm_1_1LazyValueInfo.html)的边／位置查询和 [CVP](https://llvm.org/docs/doxygen/CorrelatedValuePropagation_8cpp_source.html)提供生产实践参照。

本轮实现将全局合法范围与支配子树内的条件事实分开；只有唯一前驱的条件边建立事实，离开子树撤销。对派生算术按需查询，每次最多处理 64 个新推导节点，不能证明不溢出的 Add/Sub/Mul 返回未知，Phi 不递归套用当前位置的事实。仅处理整数比较，避免把浮点比较的 false 边误判为相反关系而忽略 NaN。范围可消除冗余比较，也可将非负数的 2 次幂除法／余数降为移位／掩码。`arith-global` 保留只用原有全局范围的对照。

下一步的产品域应联合“已知位、带符号范围、同余”，而不是强行给所有值一个连续区间。`x & 255` 的高位和 `x%8==0` 的同余有用；带环绕的 `x+1` 不能凭数学整数范围忽略 int32 溢出。循环求解需要 widening／预算和失效协议，不能把不动点迭代写成无限收缩过程。

## 深入分析：内存状态与出口

不变加载、SROA、DSE 与循环状态提升解决不同问题。`state=state*a+b` 每次写入，不能作为不变加载外提；MemorySSA 虽能定位 clobber，仍可能每轮加载／存储。新的 `loop-promote` 在 SSA 前将已证明安全的固定单元改为 scalar home，再由现有 pruned SSA 生成 Phi。每条离开循环的边建立独立回写块，保证 break、continue 后退出、早返回和不同结果路径都提交各自到达的值。

这里的合法性包括：固定、对齐、在实际对象内的地址；对象可写；读写类型一致；没有可能重叠的其他访问或观察它的调用。只处理 Int/Float 四字节单元；Zero、未知索引、未知调用效果和潜在别名拒绝候选。每函数最多八个单元，每区域最多 2048 条指令及 16 个出口。也支持循环内只写的单元，保留零次执行及未命中条件时的旧值。参数未知地址、一过尾地址和 const 对象不能因零次执行而被新增加载／回写。局部对象依据现有函数级内存 IR 的生存期处理，尚无完整 C 生命周期模型。

[LICM 的当前实现](https://llvm.org/doxygen/LICM_8cpp_source.html)同样将 promotion 的安全性与出口、别名、观察者结合。此项是完善经典技术，不能因为引用了现代实现就称为新算法。它与 SROA 互补：不逃逸局部数组可在更早阶段整体替换；循环提升可以处理全局固定字段或因其他使用无法整体替换的对象。

P1 的部分 SROA、跨 Phi 加载前向和 PRE 应以同一位置／类型／版本协议工作。调用摘要目前只有对象精度，不能声称某个 callee 只观察另一字节字段就允许提升；必须先补字节 footprint。新的回写边改变 CFG，已有精确拓扑缓存自动失效。

## 深入分析：等价发现、提取与持久化

当前 EGraph 的树成本可能重复计算共享子式，且提取所有原始结果后再依赖 DCE 清除死值。扩大节点预算会加剧这一成本，不一定提高代码质量。优先实验应是 live-out roots、跨 root 的共享计数和有上界的候选比较，并记录目标展开后的实际指令、spill 和代码大小。

[e-boost](https://arxiv.org/abs/2508.13020)区分树成本与 DAG 成本，并用启发式剪枝后做精确求解；其评估主要针对逻辑综合／验证，面积收益不能直接当作 RV64 运行收益。我们优先借鉴共享与剪枝，暂不增加 ILP 或并行求解依赖。

[2026 年持久 EGraph 论文](https://arxiv.org/abs/2602.16707)把等价类保留在 IR 中，将选择与替换分开，允许多个成本模型接续使用；论文实现明确限制于纯直线片段，未探索结构化控制流。它的浮点准确度案例追求数值误差改善，并不等价于本项目的逐位严格计算。将这一机制扩展到我们的 CFG，需要支配／作用域协议、不可观察的纯操作、内存版本和退出表示；不能直接把加载、调用、Phi 都纳入原来局部整数图。

[AEGraph 的工程说明](https://cfallin.org/blog/2026/04/09/aegraph/)则强调效果 CFG 与纯等价计算分离、按需求协作分析。本项目的 GVN／MemorySSA 即时协作只是其中一部分。可行的增量是先让范围与值编号共享接口，再以小整数区域验证持久化；每步都有旧路径回退。

## 深入分析：循环、tensor 与数据布局

多面体算法的作用是证明迭代重排的合法性，调度搜索负责选择获利方案。它既不是向量化的替代品，也不保证 tiling 必然更快。当前二重常量域会漏掉运行时尺寸、三角域和数组参数；优先补这些覆盖条件，再扩大 interchange/jam 的组合候选，避免建立无法应用的大搜索空间。

运行时别名版本化必须证明**区间不相交**，而非只证明两个起始指针不同。端点计算不能溢出；RV64 地址比较需要无符号语义；失败时回退原循环。内部 Pointer 还用于宽整数临时值，增加地址关系前应明确这两类语义，不直接复用 int32 的比较或环绕范围。

[LOOPer](https://arxiv.org/abs/2403.11522)扩展变换组合、非矩形／多循环和学习成本；[LOOPerSet](https://arxiv.org/abs/2510.10209)展示训练数据的规模需求。当前原有 60 个性能程序不够支撑学习模型的泛化结论。先生成多种域、别名、依赖、尾块和压力的独立样本，保存每个候选的合法性与成本，再考虑学习。

[2026-09 的参数爬山](https://arxiv.org/abs/2609.03114)在结构优化后调整数值参数；适合先建立少量 tile 候选和保留原调度的对照。[Loom](https://arxiv.org/abs/2609.29219)依赖显式空间加速器描述与通信／流水线约束，不能用它的硬件延迟公式预测当前 QEMU。

[Cleave，2026-10-06](https://arxiv.org/abs/2610.07742)将符号代数搜索和具体形状调度分开，并用 iterative tiling／horizontal fusion减少多归约临时数据。其等价筛选包含概率测试，最终仍检验实际形状；这不能替代严格 SysY 的语义证明。我们可以先完善 producer/consumer、footprint、存储生存期，寻找不改变单个输出累加顺序的融合。Split-K、把缩放移过归约等改写只在相应整数环或额外精确性证明下考虑，不能对任意 float32 启用。

packing／存储折叠与 loop tiling 应共同计入复制、快照、别名及外部 ABI 成本。[scalable packed layouts](https://arxiv.org/abs/2605.12445)针对 SVE 的向量长度问题；当前 RV64GC 不具备 V，暂不能取得该类 SIMD 收益。真实缓存、预取和指令调度的评价需要具体硬件，QEMU 只作为当前执行器的测量。

## 深入分析：目标约束与分配

[GlobalISel](https://llvm.org/docs/GlobalISel/Pipeline.html)和 [ISLE](https://cfallin.org/blog/2023/01/20/cranelift-isle/)的价值在于明确合法化、目标匹配与约束。当前自有 MIR 已在 RA 前展开算术，但 ABI／帧与 spill pseudos 仍在分配后展开，必须保留真实 scratch；不能仅扩大寄存器池而不审计这些路径。

本轮审计确认 `t2` 没有被这些展开使用，新增到 GP 池，并将调用破坏集合、输入参数的 ABI 保护、分配颜色数以及符号位置空间统一到共享计数。原来用 `50` 固定分隔寄存器与 spill 的检查器也相应改为两个 bank 的总数，避免增加 GP 后与 FP 位置碰撞。`t0/t1/t3/t4/t6` 与 `ft0/ft1/ft2` 仍用于实际展开。MIR 检查区分 Float/Double、word 结果和 GP 宽临时值，并验证操作数、边、终结指令和 spill 槽。

[regalloc3](https://github.com/Amanieu/regalloc3)不仅是“greedy”这个名称，还包括显式频率、重物化、复杂 class／寄存器组及工作区复用。当前最适用的是按使用位置拆分和复用分配数据；现有拆分只在块内形成短簇，调用参数还被排除，区域／边拆分可以减少长区间的无谓存储，但须定义 home 的到达状态并验证所有汇合。

CGO 2026 的[规则合成研究](https://db.in.tum.de/~drescher/paper-isel-syn.pdf)组合语义规范化／索引与 SMT 校验。它生成 LLVM 后端规则，我们没有使用该系统。优先建立统一 opcode 描述和我们自己的位向量语义，再研究离线合成小模式；不能把目标 opcode 的类型检查当作整个指令选择正确性证明。

## 深入分析：验证、性能与研究可信度

三种检查必须分开：结构验证检查 IR／bank／边；符号分配验证检查当前位置包含哪个值；等价验证检查计算与内存行为。它们互补，都不是完整编译器正确性的替代物。

本轮 `tools/verify.py` 对有效 MIR 施加错误 bank、Float/Double、word 位宽、边、操作数、终结位置及 spill 类型变异，要求拒绝。可选 Z3 证明 195 个局部 int32 条件，包括正负 2 次幂除法、余数、零余数测试、安全整数条件累加及带界限的加法；去掉符号保护必须产生反例。该工具不证明动态访问安全、整段 CFG、内存提升或 Float 优化，不能称为完整 translation validation。

[Alive2](https://github.com/AliveToolkit/alive2)提供翻译验证的研究参照，但绑定 LLVM 语义；直接用它不能满足独立编译器约束。Z3 仅为可选离线测试依赖，没有进入 C++ 编译器或正常编译路径。源码／目标语义以及每项改写的前提应逐步显式化，之后才扩大验证范围。

[CLIR](https://arxiv.org/abs/2606.26977)强调合法结构、计算密度与活跃度；本项目继续用固定种子和独立 int32／binary32 模型，新增 guard 的 true/false／汇合、回边新 SSA 值、continue／break／return、观察调用、动态别名、只读对象和零次执行。失败还应自动缩减，记录首先失效的 PASS；这是 P1，尚未实现通用 reducer。

近期 [Protean](https://arxiv.org/abs/2602.06142)、[Magellan](https://arxiv.org/abs/2601.21096)与[性能预算形式化](https://arxiv.org/abs/2608.20137)都值得研究，但不能把“可搜索”当作“应默认搜索”。本项目首先保存固定候选、启用单项对照，统计编译与运行成本；需要自己的目标数据和独立验证集合才能选择模型或声称泛化。形式化成本模型的改进也不等于真实芯片上的普遍加速。

## 下一批实施顺序与验收实验

| 顺序 | 可交付增量 | 独立验证与采用门槛 |
|---|---|---|
| 1 | 分析版本／作用域协议，KnownBits＋区间＋同余的小型产品域 | 负数、溢出、Phi、回边、比较反例；无新增投机；SMT 局部规则＋独立源模型 |
| 2 | 多面体符号域与安全 alias 版本化 | 动态／零尺寸、同一缓冲与交叠子数组、端点溢出、triangular 域；guard 失败执行原版本 |
| 3 | EGraph live-out／DAG 成本与目标压力预算 | 共享子式、多个结果、不同区域尺寸；比较提取后的 MIR／spill／运行成本，不能只数图节点 |
| 4 | 无新增执行的 Phi-of-Ops、内存 Phi 前向、部分 SROA | 每个前驱独立的值与版本；调用／未知访问反例；随后才做插入式 PRE 和跨路径 DSE |
| 5 | 区域拆分、使用位置约束、栈槽复用与块频率 | 高压力、调用、边复制环、varargs、冷分支；独立符号检查及压力模型，保留原分配对照 |
| 6 | tensor footprint／producer-consumer／生存期 | 原地别名、快照、多消费者、严格浮点；只有消费顺序和写入可观察性证明成立才融合 |
| 7 | 紧凑 AST／操作数池、函数级存储复用 | 普通 210 输入＋长表达式／大初始化／大函数；看总编译 CPU／RSS，而非只看容器大小 |
| 8 | RVV、目标延迟模型或学习型调度 | 先确定 ISA／机器描述及独立负载；依赖类型、寄存器组与硬件验证，不在 GC 目标上伪实现 |

采用门槛是正确性条件可审查、至少有独立样本上的稳定收益、原有负载不出现无法解释的明显退化，并记录编译／RSS／代码大小代价。拒绝的候选也保留实验依据，不用扩大测试名称相关启发式弥补。
