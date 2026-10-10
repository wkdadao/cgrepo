# C++ 知识体系与 Senior C++ 面试检查清单

> 定位：C++ 语言与通用工程能力的**总目录 / 查漏补缺清单**。覆盖 Senior C++ / Production-Quality Coding 面试；HFT、算法、Linux/网络、交易系统设计分别独立整理。  
> 更新：2026-10-10；学习主线：C++11/14 → C++17 → C++20，按需要扩展 C++23/26。

## 使用方法

### 知识等级（优先级，而非熟练度）

| 等级 | 含义 | 面试期望 |
| --- | --- | --- |
| **[基础]** | 日常编码的基本语义 | 能正确使用，了解常见陷阱 |
| **[必掌]** | Senior C++ 高频及正确性核心 | 能解释原理、前置条件、失败路径，必要时手写 |
| **[扩展]** | 深入实现或新标准特性 | 知道适用场景、权衡；按岗位深入 |

- **勾选规则**：只有达到掌握度 **2 或 3** 才勾选；默认全部未勾选，**不表示尚未学习**。
- **掌握度**：`0` 不会；`1` 知道概念；`2` 能解释且正确编码；`3` 能分析实现、复杂度、边界和 UB。
- **复习优先级**：先补 **[必掌]** 的 0/1，再用 coding exercise 验证；**[基础]** 中的错误也要立即修复。
- 本文件每一行是一个知识簇；需要进一步拆分时，新建独立 `.md` 并在末尾“已有专题笔记”链接。
- 知识等级是**针对面试目标的建议**，不是 C++ 标准中的分类。

## 目录地图

1. **A · Core Language**：1 类型系统；2 初始化与推导；3 值类别；4 表达式/重载；5 编译与链接  
2. **B · Object Model & Ownership**：6 对象生命周期；7 类与特殊成员；8 继承多态；9 资源管理；10 异常安全  
3. **C · Templates & Modern C++**：11 模板；12 编译期编程；13 Lambda/可调用对象  
4. **D · STL**：14 容器；15 迭代器/算法/Ranges；16 Utility Types  
5. **E · Concurrency**：17 多线程同步；18 Atomics/Memory Ordering  
6. **F · Correctness & Engineering**：19 UB/正确性；20 性能；21 API/生产代码；22 工具、调试与测试  
7. **附录**：C++ 版本、Production Coding 练习、面试优先级、专题索引与复习记录

---

## A. Core Language — 语言基础

### 1. 类型系统与基本语法

- [ ] **[基础]** Fundamental Types、`bool`、整数/浮点/字符、`sizeof`、`alignof`
- [ ] **[基础]** 指针/引用/数组、`const`、`volatile`、`struct`/`class`/`union`/`enum class`
- [ ] **[基础]** Scope、Namespace、`using`、Name Hiding、Storage Duration
- [ ] **[必掌]** `const` 指针/引用/成员函数语义；Top-level vs Low-level const
- [ ] **[必掌]** Integer Promotions、Usual Arithmetic Conversions、Signed/Unsigned、溢出
- [ ] **[必掌]** Explicit / Implicit Casts、`static_cast`/`const_cast`/`reinterpret_cast`/`dynamic_cast`
- [ ] **[扩展]** Fixed-width Integers、Endianness、Bit Fields、对象的字节表示

### 2. Initialization & Type Deduction

- [ ] **[基础]** Default / Value / Zero / Direct / Copy / List Initialization
- [ ] **[基础]** `auto`、`decltype`、`decltype(auto)`、`typedef`/`using`
- [ ] **[必掌]** 初始化方式差异、`initializer_list` 重载优先、Narrowing
- [ ] **[必掌]** `auto`/`decltype` 推导规则、括号对 `decltype` 的影响、引用保留
- [ ] **[必掌]** `explicit`、转换构造、Conversion Operators、Overload 中的隐式转换
- [ ] **[扩展]** Aggregate Initialization、CTAD、Deduction Guides

### 3. Value Categories & References

- [ ] **[基础]** Lvalue / Rvalue、`T&` / `T&&`
- [ ] **[必掌]** glvalue / prvalue / xvalue 与临时对象
- [ ] **[必掌]** `std::move`、`std::forward`、引用折叠、Forwarding Reference
- [ ] **[必掌]** Perfect Forwarding、值传递 / const 引用 / 右值引用的 API 取舍
- [ ] **[必掌]** Lifetime Extension、Dangling、Copy Elision、RVO / NRVO
- [ ] **[扩展]** Ref-qualified Methods、Explicit Object Parameter（C++23）

### 4. Expressions & Overload Resolution

- [ ] **[基础]** 运算符优先级、短路求值、`operator` Overloading、函数重载
- [ ] **[必掌]** Overload Resolution、Implicit Conversion Ranking、Template vs Non-template
- [ ] **[必掌]** 求值顺序、Sequenced / Unsequenced、Side Effects 与 UB
- [ ] **[必掌]** ADL、Name Lookup、`friend` 与 Hidden Friend
- [ ] **[扩展]** Three-way Comparison (`<=>`)、自定义比较和语义约束

### 5. Compilation, Linkage & ODR

- [ ] **[基础]** Preprocessing → Compilation → Assembly → Linking
- [ ] **[基础]** Header / Source 分离、Include Guards、`#pragma once`
- [ ] **[必掌]** Declaration vs Definition、Translation Unit、ODR
- [ ] **[必掌]** `inline`、`static`、`extern`、`constexpr` 变量与 Linkage
- [ ] **[必掌]** 模板定义可见性、符号解析、Static / Shared Library
- [ ] **[扩展]** Name Mangling、Visibility、ABI、Pimpl、C++20 Modules

---

## B. Object Model & Resource Management

### 6. Object Lifetime & Memory Layout

- [ ] **[基础]** Automatic / Static / Thread / Dynamic Storage Duration
- [ ] **[基础]** 对象构造与析构顺序、Stack / Heap 的实现区别
- [ ] **[必掌]** **Object Lifetime ≠ Storage Lifetime**；对象创建、销毁、复用存储
- [ ] **[必掌]** Temporary Lifetime、Dangling Pointer / Reference / `string_view`
- [ ] **[必掌]** Alignment、Padding、Object Representation、`memcpy` 的合法边界
- [ ] **[必掌]** Strict Aliasing / Type Accessibility、`reinterpret_cast` 限制
- [ ] **[必掌]** Placement New、Union Active Member、Explicit Lifetime Management
- [ ] **[扩展]** `std::launder`、Implicit-lifetime Types、`std::start_lifetime_as`（C++23）

### 7. Class Design & Special Member Functions

- [ ] **[基础]** Constructor、Destructor、Member Initializer List、Access Control
- [ ] **[必掌]** Rule of 0 / 3 / 5；所有权类与普通值类的区别
- [ ] **[必掌]** Copy/Move Constructor 和 Assignment、Self-assignment / Self-move
- [ ] **[必掌]** `=default` / `=delete`、隐式生成、抑制、删除条件
- [ ] **[必掌]** Member Initialization / Destruction Order、Class Invariant
- [ ] **[必掌]** Trivial / Trivially Copyable / Standard Layout；Type Traits
- [ ] **[必掌]** `noexcept` 的推导及对容器移动/复制策略的影响
- [ ] **[扩展]** EBO、`[[no_unique_address]]`、Aggregate / Layout 特殊规则

### 8. Inheritance & Polymorphism

- [ ] **[基础]** Public / Protected / Private Inheritance、Virtual / Pure Virtual
- [ ] **[基础]** `override`、`final`、Abstract Class、`virtual` Destructor
- [ ] **[必掌]** Static vs Dynamic Dispatch；Object Slicing；Override vs Hiding
- [ ] **[必掌]** 构造/析构中的虚调用规则；Virtual Destructor 的正确性
- [ ] **[必掌]** Multiple / Virtual Inheritance、Diamond、指针调整
- [ ] **[必掌]** RTTI、`dynamic_cast`、Interface Design
- [ ] **[扩展]** Vtable / Vptr 常见 ABI 实现（非标准保证）、CRTP、Type Erasure

### 9. Ownership & RAII

- [ ] **[基础]** `new/delete`、`new[]/delete[]` 成对使用；RAII
- [ ] **[必掌]** Owning / Non-owning Pointer、Borrowed View、Ownership Contracts
- [ ] **[必掌]** `unique_ptr` Move、Custom Deleter、Array Specialization
- [ ] **[必掌]** `shared_ptr`/`weak_ptr`、Control Block、Circular Reference
- [ ] **[必掌]** Aliasing Constructor、`enable_shared_from_this`、`make_shared` 的取舍
- [ ] **[必掌]** Move 后有效但状态通常未指定；资源释放与异常安全
- [ ] **[扩展]** Intrusive Refcount、Over-aligned Allocation、Custom `operator new/delete`
- [ ] **[扩展]** Allocator、`allocator_traits`、`std::pmr`、Memory Pool

### 10. Exception Safety & Failure Handling

- [ ] **[基础]** `throw`/`try`/`catch`、Exception Propagation、Stack Unwinding
- [ ] **[必掌]** Basic / Strong / No-throw Guarantees（指状态保证，而非绝不失败）
- [ ] **[必掌]** `noexcept`、`std::terminate`、析构函数抛异常的风险
- [ ] **[必掌]** Rollback、Copy-and-Swap、Commit/Rollback、Two-container Invariant
- [ ] **[必掌]** `std::move_if_noexcept`、Copy/Move 与 Strong Guarantee
- [ ] **[必掌]** `bad_alloc` / 构造失败、无资源泄漏、原对象状态一致
- [ ] **[扩展]** Fault Injection、Exception Neutrality、`std::expected` / Error Code

---

## C. Templates & Modern C++

### 11. Templates & Generic Programming

- [ ] **[基础]** Function / Class / Alias / Variable Templates、Type / Non-type Parameters
- [ ] **[必掌]** Template Argument Deduction、Explicit Arguments、Specialization / Partial Specialization
- [ ] **[必掌]** Variadic Templates、Parameter Packs、Pack Expansion、Fold Expressions
- [ ] **[必掌]** SFINAE、`enable_if`、Overload Constraints
- [ ] **[必掌]** Two-phase Lookup、Dependent Name、`typename` / `template` Disambiguation
- [ ] **[必掌]** Concepts、`requires`、Constrained Overload、Structural vs Runtime Interface
- [ ] **[扩展]** Template Instantiation、Compile Time / Code Bloat、Advanced TMP

### 12. Compile-Time Programming

- [ ] **[基础]** `constexpr` Functions / Variables、Constant Expression
- [ ] **[必掌]** `if constexpr` vs Runtime `if`；丢弃分支的语义
- [ ] **[必掌]** `consteval`、`constinit` 与 `constexpr` 的边界
- [ ] **[必掌]** Type Traits、`integral_constant`、`is_same_v`、`remove_cvref_t`
- [ ] **[必掌]** 编译期计算的约束、诊断与适用范围
- [ ] **[扩展]** `integer_sequence` / `index_sequence`、Type Lists、Compile-time Dispatch

### 13. Lambdas, Callables & Type Erasure

- [ ] **[基础]** Lambda Syntax、By-value / By-reference Capture、`mutable`
- [ ] **[必掌]** Closure Type、Capture Lifetime、Dangling / `this` 捕获
- [ ] **[必掌]** Generic Lambda、Init Capture、Move-only Capture
- [ ] **[必掌]** Function Pointer vs Function Object vs `std::function`、`std::invoke`
- [ ] **[必掌]** Type Erasure 的对象模型、分配/间接调用代价
- [ ] **[扩展]** `std::move_only_function`（C++23）、Small Buffer Optimization、`std::bind`

---

## D. Standard Library & STL

### 14. Containers

- [ ] **[基础]** `array`/`vector`/`deque`/`list`、Stack/Queue/Priority Queue Adaptors
- [ ] **[基础]** `map`/`set`、`unordered_map`/`unordered_set`
- [ ] **[必掌]** Complexity、Contiguous vs Node-based、Memory Locality
- [ ] **[必掌]** Iterator / Reference / Pointer Invalidation（尤其 `vector`、`deque`、Hash）
- [ ] **[必掌]** `vector` Growth、`reserve` vs `resize`、Reallocation、Exception Safety
- [ ] **[必掌]** Hashing、Load Factor、Rehash、Collision；Ordering / Comparator Contracts
- [ ] **[必掌]** `insert`/`emplace`/`try_emplace`、Erase、Node Handle、Heterogeneous Lookup
- [ ] **[扩展]** `flat_map`（C++23）、Intrusive Containers、PMR / Custom Allocation

### 15. Iterators, Algorithms & Ranges

- [ ] **[基础]** Iterator Categories、`begin/end`、`sort`/`find`/`lower_bound`
- [ ] **[必掌]** Algorithm Preconditions、Complexity、Valid Ranges
- [ ] **[必掌]** Strict Weak Ordering、Stable vs Unstable Sort、Binary Search Preconditions
- [ ] **[必掌]** Erase-remove Idiom、Iterator Invalidation、Output Iterators
- [ ] **[必掌]** Range-based `for`、C++20 Ranges、Views、Projection
- [ ] **[扩展]** 自定义 Iterator、Sentinel、Borrowed Range、Lazy View Lifetime

### 16. Strings & Utility Types

- [ ] **[基础]** `string`、`pair`/`tuple`、`optional`、`chrono`、`filesystem`、`bitset`
- [ ] **[必掌]** `string_view` / `span`：Non-owning View、Lifetime、Invalidation
- [ ] **[必掌]** `optional`：Engaged State、In-place Construction、Object Lifetime
- [ ] **[必掌]** `variant`：Active Alternative、`visit`、`valueless_by_exception`
- [ ] **[必掌]** `any`、Type Erasure、Allocation / Small Object Optimization（实现相关）
- [ ] **[必掌]** `std::hash`、`std::less`、`std::reference_wrapper`
- [ ] **[扩展]** `expected`、`mdspan`、`format`、`charconv`、Coroutines

---

## E. Concurrency & C++ Memory Model

### 17. Multithreading & Synchronization

- [ ] **[基础]** `thread`、`mutex`、`lock_guard`、Race、Deadlock
- [ ] **[必掌]** `unique_lock` / `scoped_lock` / `shared_mutex`、Lock Ordering
- [ ] **[必掌]** `condition_variable`、Predicate Loop、Spurious Wakeup、Lost Wakeup
- [ ] **[必掌]** `future`/`promise`/`async`、Exception Propagation
- [ ] **[必掌]** Thread Lifetime、Join / Detach、Exception-safe Shutdown
- [ ] **[必掌]** `jthread` / `stop_token`、Cooperative Cancellation
- [ ] **[扩展]** Semaphore、Latch、Barrier、Thread Pool、Task Scheduler

### 18. Atomics & Memory Ordering

- [ ] **[基础]** `std::atomic`、Data Race、Atomic vs Thread Safety
- [ ] **[必掌]** C++ Memory Model、Sequenced-before、Happens-before、Synchronizes-with
- [ ] **[必掌]** `relaxed` / `acquire` / `release` / `acq_rel` / `seq_cst`
- [ ] **[必掌]** Atomic RMW、CAS、`compare_exchange_weak` vs `strong`
- [ ] **[必掌]** Producer/Consumer Publication；用 Happens-before 证明正确性
- [ ] **[必掌]** Compiler / Hardware Reordering、Cache Coherence、False Sharing
- [ ] **[扩展]** Fences、Release Sequences、ABA、Lock-free / Wait-free Progress
- [ ] **[扩展]** Memory Reclamation：Hazard Pointers、Epoch/RCU、Atomic Wait/Notify

---

## F. Correctness, Performance & Production Engineering

### 19. UB & Language Correctness

- [ ] **[基础]** Undefined / Unspecified / Implementation-defined Behavior 的差别
- [ ] **[必掌]** OOB、Use-after-free、Dangling、Double Free、Uninitialized Read
- [ ] **[必掌]** Signed Overflow、Shifts、Integer Conversion、Index/Signed-Unsigned Bugs
- [ ] **[必掌]** Strict Aliasing、Object Lifetime、Misalignment、Illegal Type Punning
- [ ] **[必掌]** Data Race、Invalid Iterator/Pointer Arithmetic、Null Dereference
- [ ] **[必掌]** C++ Correctness vs “测试通过”；As-if Rule / Compiler Assumptions
- [ ] **[扩展]** Pointer Provenance、Lifetime-related Optimization、Sanitizer Blind Spots

### 20. Performance & Optimization

- [ ] **[基础]** Time/Space Complexity、Memory Locality、Contiguous Storage
- [ ] **[必掌]** Copy/Move/Allocation Cost、SSO/SBO（实现相关）、Data-oriented Layout
- [ ] **[必掌]** Inlining / Devirtualization、Branch Prediction、Cache / TLB 基础
- [ ] **[必掌]** Compiler Optimization、As-if Rule、Aliasing / `volatile` / UB 与优化
- [ ] **[必掌]** Benchmark Methodology、Warm-up、Measurement Bias、Tail Latency
- [ ] **[必掌]** Profiling、Flame Graph、`perf`、Generated Assembly 与瓶颈定位
- [ ] **[扩展]** SIMD / Autovectorization、LTO / PGO、Alignment / Branch Hints
- [ ] **[扩展]** 深层 CPU/NUMA/Kernel Bypass 放入独立 HFT 专题

### 21. API / Class Design & Production Coding

- [ ] **[基础]** Encapsulation、Const Correctness、RAII、Composition
- [ ] **[必掌]** Invariants、Preconditions、Postconditions、Failure Contracts
- [ ] **[必掌]** Value / Reference Semantics、Ownership-aware APIs、View Lifetime
- [ ] **[必掌]** Exception-safe State Transition、Rollback / Commit、No-throw Destruction
- [ ] **[必掌]** Boundary Handling、Integer-safe Indexing、Error Reporting
- [ ] **[必掌]** Strong Types、Explicit Interfaces、Making Invalid States Unrepresentable
- [ ] **[必掌]** Thread-safety Contract、Reentrancy、Cancellation / Shutdown
- [ ] **[扩展]** ABI-stable Interface、Pimpl、Policy-based Design、Dependency Injection

### 22. Toolchain, Debugging & Testing

- [ ] **[基础]** GCC / Clang、Compiler Options、CMake、GDB、Assertions
- [ ] **[必掌]** Warnings（`-Wall -Wextra -Wconversion` 等）、Debug vs Release
- [ ] **[必掌]** ASan / UBSan / TSan：能检测什么，不能保证什么
- [ ] **[必掌]** GDB、Core Dump、Stacktrace、Symbol / Disassembly
- [ ] **[必掌]** `perf`、Sampling、Hotspot、CPU / Allocation Profiling
- [ ] **[必掌]** Unit / Integration / Regression Tests、Boundary / Failure Tests
- [ ] **[扩展]** Fuzzing、Property-based Testing、Static Analysis、Compiler Explorer
- [ ] **[扩展]** CI / Multi-compiler Testing、Reproducible Builds、Sanitizer Trade-offs

---

## 附录 A：C++ 标准版本速览

| 版本 | 应重点记住的代表特性 | 建议 |
| --- | --- | --- |
| **C++11** | Move、Smart Pointers、Lambda、`auto`、Atomics、Variadic Templates、`constexpr` | **必掌** |
| **C++14** | Generic Lambda、`make_unique`、Return Type Deduction | **基础** |
| **C++17** | `if constexpr`、Structured Bindings、Fold Expressions、`optional`/`variant`/`string_view`、CTAD | **必掌** |
| **C++20** | Concepts、Ranges、`span`、`jthread`、`consteval`、Coroutines、`<=>` | **核心必掌 / 其他扩展** |
| **C++23** | `expected`、`mdspan`、`flat_map`、Explicit Object Parameter、`move_only_function` | **扩展** |
| **C++26** | 新语言/库特性、工具链支持差异 | **按需扩展，核对实际编译器支持** |

> 以上是版本归纳，不是完整的“新特性列表”；语言支持与标准库实现支持需要分别验证。

## 附录 B：Production-Quality C++ 编码练习

建议每题依次检查：**接口约束 → Invariant → Ownership / Lifetime → Copy / Move → Failure Path → 边界 → Tests → Performance**。

| 练习 | 核心考点 | 优先级 |
| --- | --- | --- |
| Mini `unique_ptr` | Unique Ownership、Move、RAII、Deleter | P0 |
| Mini `vector` | Allocation、Lifetime、Reallocation、Strong Guarantee | P0 |
| Mini `optional` | In-place Construction、Union、Active Lifetime | P0 |
| Thread-safe Blocking Queue | CV、Lock、Shutdown、Lost Wakeup | P0 |
| Fixed-size Ring Buffer | Capacity / Index、Wraparound、Invariants | P0 |
| SPSC Ring Buffer | Atomic Publication、Acquire/Release、Cache | P0（低延迟） |
| Small Vector / SBO | Storage Strategy、Alignment、Exception Safety | P1 |
| Mini `shared_ptr` | Control Block、Refcount、Weak Reference | P1 |
| Mini `function` | Type Erasure、Callable、Move、SBO | P1 |
| Mini `variant` | Active Alternative、Exception Safety | P1 |
| Pool Allocator / Intrusive List | Alignment、Stable Address、Ownership | P2 |

### 面试时每道题的质量检查

- [ ] 定义数据结构的 **class invariant**，尤其 empty / moved-from / failed state
- [ ] 明确所有权、对象生命周期、资源释放责任
- [ ] 解释 Copy / Move / Destructor 是否需要以及 `noexcept` 条件
- [ ] 分析边界：`size=0`、极大值、非法索引、溢出、自赋值
- [ ] 分析失败：分配失败、元素构造异常、中途操作失败
- [ ] 指明异常安全保证，验证无泄漏且状态一致
- [ ] 解释复杂度、额外分配、迭代器失效、线程安全范围
- [ ] 给出最小测试集：正常、边界、异常、重复操作

## 附录 C：面试复习优先级

| 优先级 | 模块 | 目标 |
| --- | --- | --- |
| **P0** | 3/6 Value Category & Lifetime；7/9 Rule of Five & Ownership | 能分析/修复所有权和生命周期问题 |
| **P0** | 10 Exception Safety；14/15 STL 正确性 | 能写出状态一致、失败可控的容器类 |
| **P0** | 11/12 模板、推导、Concepts | 能解释常见推导、重载和约束 |
| **P0** | 17/18 Concurrency、Atomic Memory Model | 能说明同步语义并证明简单并发队列正确性 |
| **P0** | 19/21 UB、Invariant、Boundary & API Design | 能区分“运行正常”和“符合 C++ 语义” |
| **P1** | 5/8 ABI/Polymorphism；13 Type Erasure；20 Perf | 能解释实现策略与性能取舍 |
| **P1** | 22 Debugging / Testing | 能建立故障定位、验证流程 |
| **P2** | Advanced TMP、Coroutines、前沿标准库 | 按岗位 JD 和面试信号扩展 |

**推荐学习闭环**：查缺口 → 口述原理（3–5 分钟）→ 写最小代码 → 找反例/异常路径 → 写测试 → 标记掌握度。

## 附录 D：已存在的专题笔记（已核实文件）

下列链接均为本仓库 `c++/` 目录中已存在的 Markdown，和本文件同级，便于继续扩展。

- [模板、Concepts、`constexpr` 与编译期编程](./C++模板与编译期编程完整总结.md) — 第 11/12 章
- [`decltype` / `decltype(auto)` 总结](./C++%20decltype与decltype(auto)总结.md) — 第 2/3 章
- [Aliasing、Strict Aliasing、对象生命周期](./C++%20Aliasing与Strict%20Aliasing总结.md) — 第 6/19 章
- [`noexcept` 推导与传播](./C++%20noexcept推导与传播.md) — 第 7/10 章
- [`shared_ptr` Control Block / Aliasing](./shared_ptr_Control_Block_Aliasing与所有权转移.md) — 第 9 章
- [Linux/C++ Performance Hints](./Linux_C++_Performance_Hints_系统总结.md) — 第 20 章

> 新建的专题笔记只需在这里新增相对链接；总纲仅维护知识点与状态，不复制长篇解释。

## 附录 E：边界与其他知识库

**本总纲不深入以下独立专题**，但核心 C++ 交叉点仍在上文保留：

1. **HFT / Low-Latency**：CPU Pipeline、Cache/TLB、NUMA、Lock-free、Memory Reclamation、NIC/Kernel Bypass、Latency、Market Data、Execution。
2. **Linux / OS / Networking**：Virtual Memory、Scheduling、Syscalls、I/O Multiplexing、TCP/UDP、Socket、Kernel Tooling。
3. **Algorithms / OA**：数组/字符串、二分、树/图、DP、贪心、区间结构、搜索与复杂度分析。
4. **Trading System Design**：Order Book、State Machine、Market Data Pipeline、Persistence、Replay、Idempotency、HA、Recovery。

## 附录 F：掌握度记录模板

按章节简写，**不要把已整理成笔记误判为已完全掌握**：

| 章节 | 自评 0–3 | 最近复习 | 最薄弱问题 / 下一步练习 |
| --- | --- | --- | --- |
| 1–5 Core Language | — | — | — |
| 6–10 Object Model / RAII | — | — | — |
| 11–13 Templates / Modern C++ | — | — | — |
| 14–16 STL | — | — | — |
| 17–18 Concurrency | — | — | — |
| 19–22 Correctness / Engineering | — | — | — |

**完成标准**：所有 [必掌] 达到 **2**，关键 P0 专题达到 **3**；能现场实现至少一个带动态资源的类和一个带同步的并发类，并解释边界和失败语义。
