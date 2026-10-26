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

## 6. HFT 测量 Checklist

1. Release 编译参数一致，检查汇编，排除 DCE、常量折叠和意外 inlining。
2. 预热代码和数据，明确测的是 warm cache 还是 cold cache。
3. Pin thread，减少调度迁移；注意 SMT sibling 和干扰线程。
4. 使用适当的执行排序和编译器约束；不依赖裸 `RDTSC`。
5. 记录 P50/P99/P99.9、吞吐量及测量本身开销；多轮重复。
6. 注意中断、抢占、频率变化、NUMA 和 TSC 跨核条件。
7. 对比 `perf` / VTune 等独立观测结果。

## 7. 面试表达

> RDTSC is not a serializing instruction. An out-of-order CPU can execute instructions out of program order even though they retire in order. Accurate microbenchmarks need to control both compiler transformations and hardware execution ordering. On Intel x86 I would use an appropriate LFENCE/RDTSC sequence, make the result observable, inspect generated assembly, and measure instrumentation overhead.

**一句话：** Compiler barrier 控制特定编译器重排；hardware fence 控制特定硬件顺序；in-order retirement 维护精确架构状态，但不是计时边界。

## 参考

- Intel SDM, instructions `RDTSC`, `RDTSCP`, `LFENCE`, `MFENCE`, `CPUID`
- GCC Extended Asm documentation: `memory` clobber
- C++ reference: `std::atomic_signal_fence`, `std::atomic_thread_fence`
