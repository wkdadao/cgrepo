# C++ Compiler Reordering、CPU OoO 与 RDTSC 测量

> 适用场景：x86-64 / Intel、GCC 或 Clang、Low-Latency C++。区分语言标准保证、编译器实现及硬件指令语义。

## 1. 三种顺序

| 层次 | 含义 | 要点 |
|---|---|---|
| Compiler Order | 优化器生成机器指令的顺序 | 受 as-if rule、可观察行为、barrier 等约束 |
| Execution Order | CPU 实际发射/执行 µops 的顺序 | OoO、speculation 可能使其不同于程序顺序 |
| Retirement Order | 指令架构效果的提交顺序 | 典型 OoO CPU 按程序顺序 retire，提供精确异常 |

**In-order retirement 不等于 in-order execution。** Store 的对外可见时刻也未必等于指令执行或 retirement 时刻；Store Buffer 和架构内存模型共同约束可见性。

## 2. 为什么裸 RDTSC 测量不可靠

```cpp
auto t0 = __rdtsc();
doWork();
auto t1 = __rdtsc();
```

- `RDTSC` 不是 serializing instruction：读取 TSC 的执行时刻可以与周围指令乱序。
- 编译器可能移动、简化乃至消除 `doWork()`。
- 指令按序 retire **不能**保证时间戳是在被测工作之前/之后采样。
- TSC ticks 不一定等于 CPU core cycles；跨核同步、虚拟化与平台时钟稳定性应单独确认。

## 3. Compiler Barrier vs Hardware Fence

| 手段 | 编译器层面 | 硬件层面 |
|---|---|---|
| `asm volatile("" ::: "memory")` (GCC/Clang) | 阻止某些内存访问跨 barrier 重排；**不是所有寄存器计算的屏障** | 不产生指令，不约束 CPU OoO |
| `std::atomic_signal_fence(seq_cst)` | 标准规定的同线程/信号处理器间的内存访问排序；实际可作为编译器屏障使用 | 不要求生成硬件 fence |
| `std::atomic_thread_fence(...)` | 相应编译器内存排序约束 | 用于线程间内存同步，**不是通用指令执行序列化** |
| `LFENCE` (x86) | 通过合适 intrinsic/编译器接口约束相关编译器移动 | Intel 平台上对本地指令执行提供顺序约束；不保证 Store 全局可见 |
| `MFENCE` (x86) | 取决于调用形式及编译器接口 | 约束 Load/Store 内存排序，不等于所有类型操作的通用 serializing |
| `CPUID` | 取决于调用形式及编译器接口 | Serializing instruction，代价相对高 |

不要把 C++ 的 **happens-before / data-race freedom**、compiler barrier、CPU fence 混为一谈。

## 4. Intel x86 常见 RDTSC 计时模式

```cpp
#include <cstdint>
#include <x86intrin.h>

inline std::uint64_t tsc_start() noexcept {
    _mm_lfence();
    auto t = __rdtsc();
    _mm_lfence();
    return t;
}

inline std::uint64_t tsc_end() noexcept {
    _mm_lfence();
    return __rdtsc();
}
```

时间线：

```text
LFENCE
RDTSC        // t0
LFENCE
  doWork()
LFENCE
RDTSC        // t1
```

- 起始读取前的 `LFENCE`：让更早的指令先完成本地执行。
- 起始读取后的 `LFENCE`：避免被测工作跑到时间戳采样之前。
- 结束读取前的 `LFENCE`：让被测工作先完成本地执行。
- `LFENCE` 不保证此前 Store **已经全局可见**；这种需求须另外分析，可能需要 `MFENCE`。
- `RDTSCP` 对此前指令有部分排序保证，但**不是完整 serializing instruction**，也不替代后续执行的排序。
- 这些函数不是 ISO C++ 可移植语义；适用于已验证指令和编译器行为的 x86 平台。

**重要：** 对严格的编译器重排控制，应检查目标工具链的 intrinsic 定义/生成汇编。内存 clobber 并不能保证所有纯计算不跨越测量边界。

## 5. 防止 Benchmark 被优化掉

```cpp
// 错误示例：结果从未被使用，循环可能被删除
int x = 0;
for (int i = 0; i < 1000; ++i) x += i;
```

- 让结果在观测点被真实消费，或使用 Google Benchmark `benchmark::DoNotOptimize(result)`。
- `DoNotOptimize` 不保证所有代码形态完全保持原样；必要时检查汇编。
- Benchmark 不要把 `volatile` 当作线程同步或硬件 fence。
- 计时指令和 fence 有自身成本；短代码要进行空测量及重复采样。

### 5.1 Google Benchmark 的 \`DoNotOptimize(result)\` 是什么？

**一句话：将 \`result\` 暴露给编译器看不到内部实现的操作，使产生该结果所必需的计算不能仅因“结果无用”被消除。** 它不是关闭优化，也不是 CPU Fence。

源码（Google Benchmark 主分支，2026-11-26 核对）：

- [include/benchmark/utils.h](https://github.com/google/benchmark/blob/main/include/benchmark/utils.h)：\`DoNotOptimize\` / \`ClobberMemory\` 定义
- [src/benchmark.cc](https://github.com/google/benchmark/blob/main/src/benchmark.cc)：\`UseCharPointer\` fallback
- [test/donotoptimize_assembly_test.cc](https://github.com/google/benchmark/blob/main/test/donotoptimize_assembly_test.cc)：Assembly 级别预期
- [test/clobber_memory_assembly_test.cc](https://github.com/google/benchmark/blob/main/test/clobber_memory_assembly_test.cc)：Compiler Barrier 用例

在支持 GNU Extended Asm 的平台上，**简化后的普通可写对象版本**：

\`\`\`cpp
// Clang（省略模板、SFINAE）
asm volatile("" : "+r,m"(value) : : "memory");

// GCC（对适用的小型 Trivially Copyable 对象）
asm volatile("" : "+m,r"(value) : : "memory");
\`\`\`

**四个关键组件：**

| 组件 | Compiler 看到的语义 | 不应误解为 |
|---|---|---|
| 空 Assembly Template \`""\` | 不包含实际汇编指令 | 不代表零性能影响：Register Pressure / Spill 可能改变 |
| \`volatile\` | 不应将这个 Assembly 当作无用操作删除 | 不等于 C++ \`volatile\` 对象，也不序列化 CPU |
| \`+\` Output Operand | Read-Write：既消费旧值又假设可能产生新值 | 空 Assembly 在硬件上真的修改值 |
| \`"r,m"\` / \`"m,r"\` | Register 或 Memory Operand 的替代约束 | 并不强制两者同时存在 |
| \`"memory"\` Clobber | 编译器必须保守考虑潜在内存读写，影响重排/复用 | CPU Cache Flush、\`MFENCE\` 或线程同步 |

注意，GCC 和 Clang 的交替 Constraint 顺序不同。不同版本/对象类型可选择不同 Operand；上表是**已核对源码的实现选择**，不是 ISO C++ 规则。

### 5.2 Input-Only 与 Read-Write 的真正区别

\`\`\`cpp
// Input-Only：仅表示这个值被消费
asm volatile("" : : "r,m"(result) : "memory");

// Read-Write：表示它被消费，也可能被修改
asm volatile("" : "+r,m"(result) : : "memory");
\`\`\`

- Input-Only 迫使编译器在该点提供输入值，但可认为输入值不被修改。
- Read-Write 进一步要求编译器把输出视作可能变化，抑制相关的 Constant Propagation / Value Reuse。
- **两者均不保证函数调用、循环、依赖计算一定保持原先的源代码形态。**

对于 \`const T&\`，源码使用 Input-Only 的处理，并将这一重载标记为 **Deprecated**：它不能提供与可写局部变量一样的优化约束。官方建议先将表达式结果保存为**非 const 的局部变量**，再交给 \`DoNotOptimize\`。

### 5.3 GCC 对大型 Aggregate / 非平凡类型有特殊处理

最新源码中的 GNU GCC >= 5 路径大致按类型属性分流：

| 条件 | GCC Constraint |
|---|---|
| 可写 + Trivially Copyable + \`sizeof(T) <= sizeof(T*)\` | \`"+m,r"\` |
| 可写 + 非 Trivially Copyable 或大型类型 | \`"+m"\` |
| 不适合作为可写 Assembly Output 的类型 | Input-Only，常用 \`"r,m"\` |
| \`const T&\` 小型 Trivially Copyable | \`"r,m"\`（已 Deprecated） |
| \`const T&\` 较大或非 Trivially Copyable | \`"m"\`（已 Deprecated） |

源码中的 \`IsValidAsmOutput<T>\` 近似依据数组、Copy Assignability、Move Assignability 判断输出操作数是否适用。带 \`const\` 成员的类可能不能作为汇编可写输出，带 Reference Member 的类型也存在判定近似的限制。

**为什么使用 Memory-Only？** 避免编译器为把大型 Aggregate 放进寄存器而引入额外临时拷贝，污染 Microbenchmark。实际生成的代码仍须检查。

### 5.4 实验：\`DoNotOptimize\` 不禁止 Constant Folding

\`\`\`cpp
inline int add42(int x) { return x + 42; }

void example() {
    int result = add42(0);
    benchmark::DoNotOptimize(result);
}
\`\`\`

编译器在 \`-O3\` 下仍可把 \`add42(0)\` 折叠为常数 42。Google Benchmark 自己的 Assembly Test 对类似表达式检查了 \`movl $42, %eax\`，而非一次真正的 Add 或函数调用。

同理：

\`\`\`cpp
for (auto _ : state) {
    int result = fixed_input * 100;
    benchmark::DoNotOptimize(result);
}
\`\`\`

并不保证每轮真的进行 \`imul\`。Loop-Invariant Code Motion、CSE、Inlining、Constant Propagation 仍可在满足约束的前提下发生。**\`DoNotOptimize\` 只确保某种结果依赖，对计算过程没有“逐指令保留”的保证。**

### 5.5 \`ClobberMemory()\` 的真实实现

当前 \`utils.h\`：

\`\`\`cpp
inline BENCHMARK_ALWAYS_INLINE void ClobberMemory() {
    std::atomic_signal_fence(std::memory_order_acq_rel);
}
\`\`\`

这是一个 **Compiler-Side Fence**，约束跨越边界的内存访问优化；在常见编译器上不会生成 \`MFENCE\` / \`LFENCE\` / \`SFENCE\`。它也不会执行 GPU/CPU Cache Flush、让 Store Buffer 立即清空，或建立原本不存在的跨线程同步。

\`DoNotOptimize(x)\` 与 \`ClobberMemory()\` **不能相互替代**：

| 目标 | \`DoNotOptimize(x)\` | \`ClobberMemory()\` |
|---|---|---|
| 防止 x 的结果因无用而被消除 | 主要用途 | 否 |
| 对特定值建立 Compiler Operand Constraint | 是 | 否 |
| 抑制某些编译器 Memory Reordering | GNU/Clang asm 路径中带 \`"memory"\` | 是，Compiler Fence |
| CPU 执行序列化 | 否 | 否 |
| 数据竞争修复 / 线程间可见性 | 否 | 否 |

### 5.6 MSVC 路径与 GNU/Clang 路径不同

MSVC 不采用这里的 GNU Empty Inline Assembly，而是：

\`\`\`cpp
internal::UseCharPointer(
    &reinterpret_cast<char const volatile&>(value)
);
ClobberMemory();
\`\`\`

\`src/benchmark.cc\` 的 \`UseCharPointer\` 将对象地址存入一个 \`volatile\` 全局指针，制造 Pointer Escape，让编译器保守处理对象可达性。**它可能真实执行函数调用/内存写入**；不要假设跨平台的 \`DoNotOptimize\` 开销一致。该模块的 \`char\` 对象表示访问与普通 \`T* -> U*\` 不兼容类型解引用不同。

### 5.7 与 RDTSC 测量的关系：分别防两类问题

\`\`\`cpp
#include <benchmark/benchmark.h>

static void BM_Parse(benchmark::State& state) {
    for (auto _ : state) {
        auto result = parse_one_message(); // 待测函数，需保证输入和状态代表真实场景
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_Parse);
\`\`\`

\`DoNotOptimize(result)\` 的职责是**使结果不透明、降低 Dead Code Elimination 风险**。它并不能保证 \`parse_one_message()\` 必然在计时边界内完整执行，亦不保证没有 Loop Hoisting、Constant Folding 或状态漂移。Google Benchmark 的 Iteration Timing 与通过 \`RDTSC\` 手写的 Timing 还需要分开分析。

手写 TSC 示例：

\`\`\`cpp
auto t0 = tsc_start();
int result = work(input);
benchmark::DoNotOptimize(result);
auto t1 = tsc_end();
auto dt = t1 - t0;
\`\`\`

使用这一写法也**不意味着对所有编译器版本都能形式化保证全部纯计算不会跨越计时点**；要验证 \`tsc_start/end\` Intrinsic 的 Compiler Semantics、最终汇编，以及输入/工作状态。不要把 \`DoNotOptimize\` 当作 RDTSC / CPU OoO 序列化工具。

### 5.8 HFT Microbenchmark 的 Failure Modes

1. **Dead Code Elimination**：结果无人消费 → 被测代码可能彻底消失；用有效结果依赖、\`DoNotOptimize\` 并检查 Assembly。
2. **Constant Folding / Loop Hoisting**：输入在编译期已知或跨轮不变 → 待测算术可能消失或外提；设计多组真实输入、独立验证机器码。
3. **Measurement Perturbation**：\`"+m"\` Operand 可能引起 Stack Materialization、Spill；与真实 Hot Path 代码有差异。
4. **Workload Drift**：Order Book / Queue / Parser 状态逐次变化 → 每次执行的路径不等价；按目的设计 Reset 和代表性的工作集。
5. **Wrong Fence Assumption**：Compiler Memory Clobber 和 \`atomic_signal_fence\` 不等于 \`LFENCE\`，亦不保证 Store 全局可见。
6. **False Comparability**：要同时报告 Compiler/Flags/CPU、Input Distribution、Assembly 变化、Median/Tail Latency，以及重复测量的一致性。

**推荐解释：\`DoNotOptimize\` 是编译器可观察性与操作数约束的工具；\`ClobberMemory\` 是编译器内存屏障；\`LFENCE/RDTSCP/RDTSC\` 管的是不同的 CPU 执行/计时问题。**

---

## 6. HFT 测量 Checklist

1. Release 编译参数一致，检查汇编，排除 DCE、常量折叠和意外 inlining。
2. 预热代码和数据，明确测的是 warm cache 还是 cold cache。
3. Pin thread，减少调度迁移；注意 SMT sibling 和干扰线程。
4. 使用适当的执行排序和编译器约束；不依赖裸 `RDTSC`。
5. 记录 P50/P99/P99.9、吞吐量及测量本身开销；多轮重复。
6. 注意中断、抢占、频率变化、NUMA 和 TSC 跨核条件。
7. 对比 `perf` / VTune 等独立观测结果。
8. 记录 `DoNotOptimize` / `ClobberMemory` 是否影响寄存器分配、Spill 和最终热路径指令；不要只看源码。

## 7. 面试表达

> RDTSC is not a serializing instruction. An out-of-order CPU can execute instructions out of program order even though they retire in order. Accurate microbenchmarks need to control both compiler transformations and hardware execution ordering. On Intel x86 I would use an appropriate LFENCE/RDTSC sequence, make the result observable, inspect generated assembly, and measure instrumentation overhead. Google Benchmark's DoNotOptimize uses opaque compiler operands to prevent dead-code elimination, but does not prevent all compiler transformations or serialize CPU execution.

**一句话：** Compiler barrier 控制特定编译器重排；hardware fence 控制特定硬件顺序；in-order retirement 维护精确架构状态，但不是计时边界。

## 参考

- Intel SDM, instructions `RDTSC`, `RDTSCP`, `LFENCE`, `MFENCE`, `CPUID`
- GCC Extended Asm documentation: `memory` clobber
- C++ reference: `std::atomic_signal_fence`, `std::atomic_thread_fence`
- [Google Benchmark — utils.h (`DoNotOptimize` / `ClobberMemory`)](https://github.com/google/benchmark/blob/main/include/benchmark/utils.h)
- [Google Benchmark — `UseCharPointer` implementation](https://github.com/google/benchmark/blob/main/src/benchmark.cc)
- [Google Benchmark — DoNotOptimize Assembly Tests](https://github.com/google/benchmark/blob/main/test/donotoptimize_assembly_test.cc)
- [Google Benchmark — ClobberMemory Assembly Tests](https://github.com/google/benchmark/blob/main/test/clobber_memory_assembly_test.cc)
- [Google Benchmark — User Guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
