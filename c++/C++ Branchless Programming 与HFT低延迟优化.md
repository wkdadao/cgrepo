# C++ Branchless Programming（无分支编程）与 HFT 低延迟优化

> 适用：C++17/20、GCC/Clang、x86-64、Market Data / Order Book / Pricing / Execution hot path。
>
> 核心结论：**Branchless 的目标不是消灭所有 `if`，而是在真实工作负载下减少关键路径的平均执行成本和尾延迟。** 源码没有 `if` 不代表机器码没有分支；源码有 `if` 也不代表机器码一定有分支。

## 目录

1. [基础模型：Branch 与 Branchless](#1-基础模型branch-与-branchless)
2. [x86 指令：jcc / setcc / cmovcc](#2-x86-指令jcc--setcc--cmovcc)
3. [五类常见 Branchless 技巧](#3-五类常见-branchless-技巧)
4. [Branch 与 Branchless 成对代码示例](#4-branch-与-branchless-成对代码示例)
5. [性能原理：预测、依赖与额外工作](#5-性能原理预测依赖与额外工作)
6. [C++ 语义与安全陷阱](#6-c-语义与安全陷阱)
7. [HFT 场景与设计取舍](#7-hft-场景与设计取舍)
8. [编译器与汇编检查](#8-编译器与汇编检查)
9. [Benchmark 与 perf 测量](#9-benchmark-与-perf-测量)
10. [面试速记与判断清单](#10-面试速记与判断清单)

---

## 1. 基础模型：Branch 与 Branchless

### 1.1 Branch 版本

~~~cpp
if (price > threshold) {
    ++count;
}
~~~

可能的 x86-64 assembly（示意，具体寄存器和分支方向由编译器决定）：

~~~asm
cmp  edi, esi       ; 比较 price / threshold
jle  .skip          ; condition false 则跳过
inc  eax
.skip:
~~~

- `cmp` 更新 EFLAGS。
- `jcc`（conditional jump）决定下一条取指路径。
- Branch predictor 预测方向和（相关情况下的）目标。
- 预测正确时，控制流开销通常很小；预测错误时，错误路径上的 speculative work 被丢弃，需要恢复正确路径。
- Misprediction penalty 是微架构、上下文相关的，常见是十几个到数十个 cycles 的量级，**不是固定值**。

### 1.2 Branchless 版本

~~~cpp
count += (price > threshold);
~~~

可能编译为：

~~~asm
cmp   edi, esi
setg  dl            ; signed greater -> dl=1/0
movzx edx, dl       ; zero extend
add   eax, edx
~~~

这段在数据流中表达条件，不使用条件跳转。实际编译器可能选不同组合，例如提前清零寄存器再 `setcc`。

**重要：**“Branchless”通常指消除 *data-dependent control-flow branch*，而不是没有任何分支。循环跳转仍然可能存在。

### 1.3 不可从源码断言机器码

~~~cpp
int f(int a, int b) {
    if (a > b) return a;
    return b;
}

int g(int a, int b) {
    return a > b ? a : b;
}
~~~

优化器可能把两者编译成完全相同的 `cmov`、`max`、`jcc` 等形式。**先看汇编，再讨论是否真正消除了分支。**

---

## 2. x86 指令：jcc / setcc / cmovcc

| 指令族 | 含义 | 典型用途 | 是否改变控制流 |
|---|---|---|---|
| `jcc` | 条件跳转 | 有条件执行一段代码 | 是 |
| `setcc` | 条件成立写 byte=1，否则 byte=0 | 布尔值实体化、计数、生成 mask | 否 |
| `cmovcc` | 条件成立时更新目标寄存器 | 两个 scalar 值之间选择 | 否 |

### 2.1 有符号与无符号条件

~~~asm
cmp edi, esi        ; 逻辑比较 left - right

setg al             ; signed left > right
setl al             ; signed left < right
seta al             ; unsigned left > right
setb al             ; unsigned left < right
sete al             ; equal
setne al            ; not equal
~~~

- Signed comparisons 使用 `g/ge/l/le`（依据 SF/OF/ZF）。
- Unsigned comparisons 使用 `a/ae/b/be`（依据 CF/ZF）。
- 相同二进制值在 signed/unsigned 下可能得到不同条件结果。不要手写错 `setg` 与 `seta`。

### 2.2 SETcc 的 byte 输出

~~~asm
cmp   edi, esi
setg  al            ; 只定义 AL
movzx eax, al       ; 得到 0 或 1 的完整 eax
~~~

`setcc` 只写 8-bit 目标。若后续需要完整整数，经常会看到 `movzx`，也可能由编译器采用其他组合避免不必要的寄存器依赖。

### 2.3 CMOVcc

~~~cpp
int choose(int a, int b) {
    return a > b ? a : b;
}
~~~

可能的 assembly：

~~~asm
mov   eax, esi      ; 先选 b
cmp   edi, esi
cmovg eax, edi      ; 条件成立时选 a
~~~

**CMOV 与 branch 的本质差别：**

- `jcc` 产生控制依赖，由 predictor 猜测控制流。
- `cmovcc` 产生数据依赖：结果要等待条件 flags 与候选输入。
- `cmov` 两个候选值若来自复杂运算，通常需要在选择前已经可用；不能指望它自动省去未选分支的工作。
- x86 `cmovcc` 的内存源操作数不适合用于保护可能无效的地址：源操作数可能被无条件读取，不能把它当作 C++ 短路保护。

---

## 3. 五类常见 Branchless 技巧

### 3.1 Boolean arithmetic

~~~cpp
count += (price > threshold);        // bool 转 0/1
errors += (status != Status::OK);
matches += (qty > 0 && price <= limit);
~~~

特点：易读、适合简单 predicate；编译器可能使用 `setcc`，也可能按全局优化选择其他形式。

### 3.2 Conditional move / ternary selection

~~~cpp
bestBid   = newBid > bestBid ? newBid : bestBid;
remaining = qty < remaining ? qty : remaining;
~~~

编译器可能生成 `cmovcc`。整数上的 `std::min` / `std::max` 也常见。不要把“写成三元表达式”等同于“强制 CMOV”。

### 3.3 Bitwise mask

对于 `uint32_t`，把一个 `bool` 转换成全 0 / 全 1 mask：

~~~cpp
#include <cstdint>

std::uint32_t mask =
    std::uint32_t{0} - static_cast<std::uint32_t>(condition);
// false => 0x00000000
// true  => 0xFFFFFFFF
~~~

由于采用无符号减法，该构造是按模 2^32 定义的，不依赖 signed overflow。

选择：

~~~cpp
std::uint32_t result = (a & mask) | (b & ~mask);
// 或等价地：
std::uint32_t result2 = b ^ ((a ^ b) & mask);
~~~

等价于：

~~~cpp
std::uint32_t result = condition ? a : b;
~~~

**不保证 mask 写法更快。** 它可能比单条 `cmov` 用更多指令、增加串行数据依赖。

### 3.4 Arithmetic transformation

~~~cpp
// 定义良好的 unsigned 情况
std::uint32_t increment = static_cast<std::uint32_t>(condition);
counter += increment;
~~~

数学性质允许时可以用加减和布尔算术替代条件更新，但先检查溢出、类型提升、signed/unsigned、浮点 NaN 和异常语义。

### 3.5 SIMD predication / vectorization

对大量数据做同一 predicate：

~~~cpp
std::size_t count = 0;
for (std::size_t i = 0; i < n; ++i) {
    count += (prices[i] > threshold);
}
~~~

编译器可能 vectorize，批量比较、产生 mask，再累计结果。具体取决于指针别名、数据类型、编译选项、循环依赖与目标 ISA。

**SIMD masked operation 与 scalar bitwise mask 不是一回事**：AVX-512 支持按 lane 进行 predication；AVX2 通常使用向量 compare/blend/logic。

---

## 4. Branch 与 Branchless 成对代码示例

### 4.1 条件计数（SETcc）

Branch：

~~~cpp
if (x > threshold) {
    ++count;
}
~~~

表达式形式：

~~~cpp
count += (x > threshold);
~~~

适合随机条件或批量统计；若条件可预测，两者都应实测。

### 4.2 取两个整数较大值（CMOV）

Branch：

~~~cpp
if (a > b) {
    result = a;
} else {
    result = b;
}
~~~

表达式形式：

~~~cpp
result = a > b ? a : b;
~~~

可能编译成 `cmp + cmovg`，但并非语言保证。

### 4.3 条件累加数量

Branch：

~~~cpp
if (matched) {
    totalQty += qty;
}
~~~

易读的 branchless 候选：

~~~cpp
totalQty += matched ? qty : 0u;
~~~

手动 mask（要求均为 `uint32_t`）：

~~~cpp
std::uint32_t mask =
    0u - static_cast<std::uint32_t>(matched);
totalQty += (qty & mask);
~~~

手动 mask 不一定优于三元表达式；对有符号值、不同宽度类型，不要直接复用。

### 4.4 条件置位 flag

Branch：

~~~cpp
if (condition) {
    flags |= FLAG_ACTIVE;
}
~~~

Mask：

~~~cpp
std::uint32_t mask =
    0u - static_cast<std::uint32_t>(condition);
flags |= (FLAG_ACTIVE & mask);
~~~

其中 `flags` 和 `FLAG_ACTIVE` 使用 `uint32_t`（或经过明确的无符号转换）。

### 4.5 条件设置/清除 flag

Branch：

~~~cpp
if (condition) {
    flags |= FLAG_ACTIVE;
} else {
    flags &= ~FLAG_ACTIVE;
}
~~~

Mask：

~~~cpp
std::uint32_t mask =
    0u - static_cast<std::uint32_t>(condition);

flags = (flags & ~FLAG_ACTIVE) | (FLAG_ACTIVE & mask);
~~~

结果：只改变指定 flag bit(s)，其余 bit 保持不变。

### 4.6 条件选择：AND/OR 与 XOR mask

Branch：

~~~cpp
std::uint32_t selected;
if (condition) selected = a;
else           selected = b;
~~~

Mask：

~~~cpp
std::uint32_t mask =
    0u - static_cast<std::uint32_t>(condition);

std::uint32_t x = (a & mask) | (b & ~mask);
std::uint32_t y = b ^ ((a ^ b) & mask);
~~~

`x == y == (condition ? a : b)`。分析 assembly 时不要只数源码运算符，常量传播和指令合并可能显著改变结果。

### 4.7 数组循环中的 predicate 聚合

Branch：

~~~cpp
for (std::size_t i = 0; i < n; ++i) {
    if (prices[i] >= limit) ++count;
}
~~~

候选：

~~~cpp
for (std::size_t i = 0; i < n; ++i) {
    count += (prices[i] >= limit);
}
~~~

适合检验 vectorization 与 branch-miss trade-off：高概率成立、交替、随机 50/50 三种分布都测。

### 4.8 简单序号推进

Branch：

~~~cpp
if (accepted) {
    ++sequence;
}
~~~

候选：

~~~cpp
sequence += static_cast<std::uint64_t>(accepted);
~~~

注意只有**无条件执行这一步也合法**时才这样改；如果业务还有写 buffer、发订单、更新状态机等副作用，不应把整个业务流程套入算术 mask。

---

## 5. 性能原理：预测、依赖与额外工作

### 5.1 分支预测正确时：Branch 可能占优

~~~cpp
if (unlikely(error)) {
    handleError();
}
~~~

如果错误极罕见，普通 branch 让正常路径简单；未发生错误时不会执行重工作。适合 exceptional/slow path。

`unlikely` 是自定义宏或编译器提示，不是标准 C++ 关键字；C++20 可在适用分支处使用 `[[unlikely]]`，但提示不会保证固定的机器码与更快性能。

### 5.2 随机条件时：Branchless 更有机会

不可预测的 50/50 条件可能带来大量 misprediction，`cmov` / `setcc` 可避免因控制流错误预测造成的恢复成本。

但“50/50 比例”不等同于“不可预测”：交替序列、周期模式或与历史相关的条件可能被现代 predictor 学习。**应测试真实序列而不只看 true 的比例。**

### 5.3 Dependency chain 可以使 Branchless 更慢

例子（仅表示依赖关系，不代表唯一的生成结果）：

~~~asm
cmp    edi, esi
setg   al
movzx  eax, al
neg    eax
and    eax, edx
add    ecx, eax
~~~

数据依赖：`cmp -> setcc -> movzx -> neg -> and -> add`。

一个高度可预测的 branch 可以让处理器预测执行更新路径，部分工作提前开展；branchless 不再有预测错误，却可能延长关键的数据依赖链。

如果热点循环已经受到 execution port、load bandwidth、register pressure 约束，多出来的指令也会影响吞吐。

### 5.4 额外工作 / eagerly evaluating both sides

~~~cpp
if (condition) {
    result = cheap();
} else {
    result = expensive();
}
~~~

不能为了 mask 选择而盲目改成：

~~~cpp
auto a = cheap();
auto b = expensive();
result = condition ? a : b;
~~~

改变：两个函数都执行；可能增加 CPU、cache miss、内存分配、副作用、异常等成本，也可能改变程序行为。

**判断模型（概念层面）：**

- Branch：预测正确成本 + 预测错误概率 × misprediction penalty + 被选路径工作。
- Branchless：条件求值 + select/mask 数据依赖 + 必须执行的工作。
- 真实吞吐由并行度、缓存层次、分支相关性和其他指令共同决定，不能把模型当作精确计时公式。

---

## 6. C++ 语义与安全陷阱

### 6.1 Mask 无法保护非法内存访问

错误示例：

~~~cpp
// index 在 condition=false 时可能越界：
auto value = data[index] & mask;
~~~

即使 `mask == 0`，读取 `data[index]` 仍需合法。Mask 运算发生在 load 之后。

正确方式：保证地址始终合法，或保留真正的控制流保护。

~~~cpp
if (index < size) {
    use(data[index]);
}
~~~

同样不能把受条件保护的空指针解引用变为无条件解引用。

### 6.2 短路求值不能随意改变

~~~cpp
if (ptr && ptr->ready()) { /* ... */ }
~~~

`&&` 的右操作数只有在左操作数为 true 时才求值。手工提前计算 `ptr->ready()` 可能直接导致 UB；不要为了 branchless 破坏短路语义。

### 6.3 有符号溢出、位移与类型转换

- Signed integer overflow 在 C++ 中是 UB；常见的 `-int(condition)` 在 bool 0/1 范围内本身安全，但后续类型提升、不同宽度和位运算容易出错。
- 建议明确使用无符号、固定宽度整数来构造 mask。
- 对 signed 负数位移不要依赖旧标准/特定实现的右移细节来写所谓“通用 abs bit trick”。
- `std::abs(INT_MIN)` 的结果无法用 `int` 表示；绝对值 branchless 技巧必须处理这一边界。
- 与 `size_t`、enum、bit-field 混合操作时核对整数提升规则。

### 6.4 浮点语义

`std::min`、`std::max`、三元表达式与硬件 floating-point min/max 在 NaN、signed zero、异常标志等边界上可能有不同语义；不要按整数比较的结论直接推广到浮点。特别注意 `-ffast-math` 可能放宽 IEEE 严格语义。

### 6.5 不要混淆 timing leakage 与 Branchless

没有 branch 不等于 constant-time：内存访问模式、缓存、浮点特殊值、指令实现仍可能泄露时间差。这里讨论的是性能优化，不是密码学常数时间安全保证。

### 6.6 不会消除数据竞争

Mask、CMOV、SETcc 都不是原子同步原语。并发共享变量依旧需要合适的 `std::atomic`、内存序或同步机制。

---

## 7. HFT 场景与设计取舍

| HFT 位置 | 例子 | 推荐思路 |
|---|---|---|
| Market Data feed handler | 简单状态位、计数器、消息类别统计 | `bool` 算术 / ternary，检查是否能 vectorize |
| Order Book | `bestBid` / `bestAsk` 简单选择 | 优先清晰的 `min/max/?:`，观察 CMOV |
| Order Book 更新 | 插入、删除、链表维护、复杂内存访问 | 通常保留 branch；mask 不会省去不必要指针追踪 |
| Pricing | clamp / cap / floor / scalar selection | 比较 CMOV、SIMD，注意浮点语义 |
| Risk/Permission | 快速 `allowed` predicate | 可无分支构造结果；涉及拒单副作用、状态机则保留明确控制流 |
| Error/Recovery | rare malformed message、slow path | 高预测率 branch，隔离 cold path |
| Batch Analytics | 连续 prices 比较统计 | 自动向量化、SIMD masked processing 往往比手工 scalar mask 更有价值 |

**设计原则**：简单且纯粹的条件选择是 branchless 的好候选；包含内存分配、异常、I/O、锁、状态机迁移和 pointer chasing 的分支通常不适合机械转换。

---

## 8. 编译器与汇编检查

### 8.1 编译

~~~bash
g++ -std=c++20 -O3 -march=native -S -masm=intel test.cpp -o test.s
clang++ -std=c++20 -O3 -march=native -S -masm=intel test.cpp -o test-clang.s
~~~

需要可比测试时固定目标微架构，例如 `-march=x86-64-v3`（取决于机器是否支持），避免使用 `native` 后跨机器产生不可比的 ISA。

也可以把函数标记为 `noinline` 做局部观察（GCC/Clang 扩展），**但正式性能测试要回到真实 inline / LTO 情况**，因为调用边界本身会改变优化。

### 8.2 关键指令

~~~text
cmp / test           flags 更新
jcc                  conditional branch
setcc                byte = condition ? 1 : 0
movzx                zero-extend
cmovcc               conditional register selection
and / or / xor / neg mask 运算
vpcmp / blend / mask SIMD candidate
~~~

检查包括：
1. 源码是否被完全优化掉？
2. `if` 是否已经被编译器改为 `cmov`？
3. 手工 mask 是否反而增加了指令和依赖？
4. 是否使原有循环丧失自动向量化？
5. 内存访问是否由 conditional 变成 unconditional？
6. Release build 的 inline、LTO、PGO 是否改变最终结果？

### 8.3 Hints 不等于 Branchless

~~~cpp
if (error) [[unlikely]] {
    slowPath();
}
~~~

`[[likely]]`/`[[unlikely]]` 提供静态优化提示；PGO 提供更有依据的实际分布反馈。它们不保证 CPU predictor 总是正确，也不保证编译器生成某种固定指令序列。

---

## 9. Benchmark 与 perf 测量

### 9.1 同时测多种数据分布

- Always true / always false：极易预测。
- 99.9% true：强偏置。
- 50/50 随机：通常更难预测。
- 交替与周期 pattern：比例相同，但 predictor 未必表现相同。
- 真实的 market-data / order-flow replay：**最关键**。

### 9.2 指令和硬件计数器

~~~bash
g++ -O3 -std=c++20 -march=native benchmark.cpp -o benchmark

taskset -c 2 perf stat -r 10 \
  -e cycles,instructions,branches,branch-misses \
  ./benchmark

perf record -g -- ./benchmark
perf report
perf annotate
~~~

有的服务器或企业环境因 `perf_event_paranoid` / 权限无法取得全部计数器。具体事件也取决于 CPU 和内核支持。必要时使用 `perf list` 确认。

重点指标：
- Cycles/op 与 ns/op：衡量实际结果。
- Branch-misses/op：是否真的解决问题。
- Instructions/op：是否引入过多额外工作。
- IPC、front-end / back-end stall、cache/TLB miss：判断瓶颈是否转移。
- p50、p99、p99.9：**按实际单事件处理延迟采样**；单纯的批次总时间无法推导真实尾延迟。

### 9.3 Benchmark 设计注意事项

1. 用同一输入数据分别跑两个等价实现，校验输出一致。
2. 防止 Dead Code Elimination：观察/汇总结果；不要让整个函数被常量折叠。
3. 预热，固定编译器、编译参数、CPU 亲和性和频率策略；避免调试模式。
4. 将准备数据和随机数生成放在测量窗口外。
5. 数据是否 L1/L2/LLC resident 会影响结论；必要时分别测试。
6. 区分 throughput benchmark 与 request-by-request latency benchmark。
7. 不要只测随机 50/50 并将结论推广到高度相关的行情序列。
8. 检查测试结果的统计稳定性，避免低样本下以微小差异下结论。

---

## 10. 面试速记与判断清单

### 10.1 一句话模型

> **Branch trades the risk of misprediction for avoiding unnecessary work; branchless trades control-flow uncertainty for explicit data dependencies and potentially more instructions/work.**

### 10.2 什么时候优先尝试？

- 条件难预测，且 miss rate 高；
- 两个候选值已经在寄存器中；
- predicate 简单，逻辑无副作用；
- 大量同构元素适合 SIMD；
- 实测 hot path 延迟/吞吐因 branch misprediction 而受限。

### 10.3 什么时候保留 branch？

- 条件高度可预测；
- 一条分支包含昂贵、少见的工作；
- 需要短路保护访问、异常或副作用；
- branchless 引入长串行依赖链；
- 源码已有 `if`，但编译器已经自动 if-convert。

### 10.4 高频面试问答

**Q: Is branchless always faster?**

No. It avoids misprediction, but may increase instruction count, dependency-chain latency, and unnecessary computation. Predictable branches may be faster.

**Q: Does ternary guarantee CMOV?**

No. The compiler can choose a branch, CMOV, SETcc, or other code according to target ISA, semantics, and cost model.

**Q: What is the difference between SETcc and CMOV?**

SETcc materializes a predicate into a byte (0/1); CMOV conditionally chooses a source value for a destination register without control-flow branching.

**Q: Why can manual mask operations be slower than CMOV?**

Extra `neg/and/or/xor` operations and serial dependencies can exceed the cost of a single conditional move; source operands may also require extra loads.

**Q: How do you optimize this in a low-latency trading system?**

I check the generated assembly, use realistic input distributions, measure cycles, branch misses, and per-event tail latency, and keep only changes that improve the actual hot path.

### 10.5 最终判断

> **不要为了代码看起来 branchless 而手写 mask。先使用清晰、类型安全的 C++ 表达业务语义，再通过 assembly 和 perf 找出是否需要人工干预。**
>
> 对 HFT：优化目标是 predictable / bounded latency，而不是简单最少的 `if` 或最低的 branch-misses 数字。

---

*整理日期：2026-10-15。示例汇编均为可能的 lowering，不是 GCC/Clang 在所有版本和选项下的保证输出。*
