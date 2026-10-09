# Linux / C++ Performance Hints 系统总结

> 面向 Linux、GCC/Clang、C++17/20、低延迟交易系统。核心原则：hint 不等于保证；先确认语义契约，再检查汇编与性能数据。

## 1. 分层模型

| 层次 | 手段 | 主要影响 | 关键风险 |
|---|---|---|---|
| 语言/优化器 | `__restrict__`, assume, unreachable | alias 分析、常量传播 | 错误承诺导致 UB |
| 分支/布局 | likely, expect, hot/cold, PGO | basic block 布局、inlining | profile 不匹配 |
| 数据布局 | alignas, assume_aligned, SoA | cache line、SIMD、访存 | 对齐不成立、内存膨胀 |
| CPU 指令 | prefetch, pause, streaming stores | cache、SMT、写入策略 | 竞争/污染/排序问题 |
| 并发语义 | atomic memory_order | 同步和重排约束 | data race、可见性错误 |
| OS/内存 | madvise, mlock, hugepages | paging、TLB、缺页 | 资源限制、延迟波动 |
| 调度/NUMA | affinity, NUMA policy, FIFO | 迁核、远端内存、抢占 | 饥饿、运维复杂性 |
| 工具链 | -O2/-O3, LTO, PGO, BOLT | 全程序优化、布局 | 构建可移植性 |

## 2. Alias 与编译器假设

### `__restrict__`
标准 C++ 没有 `restrict`；GCC/Clang 扩展支持 `__restrict__`。它约束的是基于特定指针的访问关系，不是简单地宣称“所有指针地址都不同”；具体契约参考编译器文档。

```cpp
void add(float* __restrict__ out,
         const float* __restrict__ x,
         const float* __restrict__ y,
         size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = x[i] + y[i];
}
```

不得在存在违反 restrict 契约的重叠读写时使用。**strict aliasing 与 restrict 是不同规则**：前者是类型访问规则，后者是额外的无别名承诺。`const` 也不等于 no-alias。

### 假设与不可达
- C++23: `[[assume(expr)]];`（实现支持需核实）。
- Clang: `__builtin_assume(expr)`。
- GCC/Clang: `__builtin_unreachable()`。
- GCC 的 `__builtin_expect_with_probability(expr, value, probability)` 可指定概率。

```cpp
if (n == 0) return;
if (n % 16 != 0) __builtin_unreachable(); // 只有调用契约保证时才安全
```

**错误的 assume/unreachable 可能引入 UB**；不要用来代替输入验证。

## 3. 分支提示

```cpp
if (ok) [[likely]] { fast(); }
else [[unlikely]] { slow(); }

if (__builtin_expect(error != 0, 0)) handle_error();
```

`[[likely]]` / `[[unlikely]]` 是 C++20 属性，通常影响编译器优化与布局，**不是直接设置 CPU 分支预测器**。建议先使用真实负载 PGO，只有明确偏斜的冷错误路径才手工标注。

## 4. Alignment、Cache、False Sharing

```cpp
struct alignas(64) Cursor { std::atomic<uint64_t> value{0}; };
auto* p = std::assume_aligned<64>(ptr); // C++20；必须真实满足对齐
```

- `alignas`：要求对象对齐；并不自动保证两个不同对象永不共享 cache line。
- `std::assume_aligned<N>` / `__builtin_assume_aligned`：向优化器声明既有对齐，不能修复未对齐指针。
- `posix_memalign` / aligned `operator new`：实际获得对齐内存。
- `std::hardware_destructive_interference_size`：C++17 提供实现相关的干扰尺度，跨平台 ABI 需注意。
- `__attribute__((packed))`：改变布局并可能产生非对齐访问；线格式优先显式解码，不应任意 reinterpret_cast。
- AoS vs SoA：批处理和 SIMD 常适合 SoA；逐对象处理可能更适合 AoS。
- Cache-line padding 主要减少不同核写入造成的 coherence traffic，不是通用加速技巧。

## 5. SIMD / Loop Hints

```cpp
#pragma GCC ivdep
for (size_t i = 0; i < n; ++i) out[i] = a[i] + b[i];
```

- `#pragma GCC ivdep`：声明不存在阻止 SIMD 的相关循环依赖，错误声明可能产生错误结果。
- `#pragma omp simd`：可表达 aligned、reduction 等；需要对应编译器/OpenMP 支持。
- `-march=...`：允许使用指定 CPU ISA；`-mtune=...`：调整调度优化而不必扩展 ISA。
- `__attribute__((target("avx2")))`、`target_clones`：函数级 ISA 多版本（具体编译器/平台支持不同）。
- GCC: `-fopt-info-vec-optimized -fopt-info-vec-missed`。
- Clang: `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize`。

`restrict + alignment + 连续访存` 可改善向量化机会，但不保证生成 SIMD。

## 6. 函数与代码布局

| 属性/选项 | 意义 | 注意 |
|---|---|---|
| `inline` | C++ ODR/定义规则；编译器可自主 inline | 不保证内联 |
| `always_inline` | 强烈要求内联 | 可能增加 I-cache 压力 |
| `noinline` | 阻止通常的内联 | 多一次调用 |
| `hot` / `cold` | 优化优先级与代码布局 | PGO 可能覆盖手工判断 |
| `flatten` | 尝试内联函数内部调用 | 容易代码膨胀 |
| `-flto` | 跨翻译单元优化 | 链接成本/工具链要求 |
| PGO / BOLT | 基于真实 profile 优化 | 样本必须有代表性 |

## 7. CPU / Cache Hints

### Prefetch
```cpp
__builtin_prefetch(next_ptr, 0, 1);
```
参数分别为地址、读写意图（0 读/1 写）、局部性（0–3）。提示不保证 cache 命中，也不保证无缺页；计算 prefetch 地址本身必须安全。指针追逐可尝试，但如果下一地址只能在当前 load 完成后得知，无法隐藏该依赖链的全部延迟。顺序数组往往由硬件预取处理得更好。

### Spin wait
```cpp
while (!ready.load(std::memory_order_acquire)) {
    _mm_pause(); // x86 PAUSE，提示自旋等待
}
```
`PAUSE` 不释放 CPU，不保证公平性或低延迟；等待较长时可考虑自适应 spin + futex/atomic wait。

### Non-temporal stores
`_mm_stream_si128` 等用于较大的、短期不复用的写入；需要满足指令对齐要求，且与普通存储混用及跨线程发布时须处理适当的 ordering（例如必要时 `_mm_sfence()`）。小数据、热数据不一定受益。

### Bit / arithmetic builtins
- `__builtin_popcount`, `__builtin_ctz`, `__builtin_clz`；传统 clz/ctz 对零输入未定义。
- C++20 `std::popcount`, `std::countl_zero`, `std::countr_zero` 对零有定义。
- `__builtin_add_overflow` / `__builtin_mul_overflow` 可安全检测整数溢出。
- `__builtin_trap()` 直接终止，不是性能优化。

## 8. 原子操作不是“可选 Hint”

```cpp
payload = value;
ready.store(true, std::memory_order_release);

// another thread:
if (ready.load(std::memory_order_acquire)) consume(payload);
```

- `relaxed`：仅原子性，不建立跨对象同步。
- `release/acquire`：在匹配同步条件下建立 happens-before。
- `seq_cst`：提供额外单一全序约束，不等于每个平台上都一定最贵。
- `atomic_signal_fence`：编译器层面的信号处理排序约束。
- `atomic_thread_fence`：线程间排序语义；是否生成硬件 fence 依架构/用法而定。

**不能靠 `volatile`、`PAUSE` 或普通 compiler barrier 修复 data race。**

## 9. Linux 内存 / IO Hints

| API | 作用 | 限制 |
|---|---|---|
| `madvise(MADV_SEQUENTIAL/RANDOM)` | 虚拟内存访问模式建议 | 具体效果依映射和内核 |
| `madvise(MADV_WILLNEED)` | 预取/准备提示 | 不保证之后无缺页 |
| `madvise(MADV_DONTNEED)` | 放弃内容/页的建议或操作 | 可能改变后续读取结果 |
| `madvise(MADV_HUGEPAGE/NOHUGEPAGE)` | THP 策略提示 | 不保证立即分配大页 |
| `posix_fadvise` | 文件访问模式/缓存建议 | 作用于文件 IO |
| `mlock/mlockall` | 锁定内存页，防止被换出 | 受 RLIMIT_MEMLOCK/权限约束 |
| `MAP_HUGETLB` | 显式 huge pages | 需要可用 hugepage 资源 |
| `MAP_POPULATE` | 尝试预填充映射页表 | 不保证完全成功或消除未来缺页 |
| `mmap` + page touch | 预热内存页 | 需保证合法可写映射；page size 不应硬编码 |

**注意**：`MADV_DONTNEED` 不是“无害的 cache hint”；匿名私有映射的后续读取可能得到零页内容。

## 10. Scheduling / NUMA / OS

- `sched_setaffinity` / `pthread_setaffinity_np`：约束线程可运行 CPU 集合；属于调度约束，不只是 hint。
- `numactl --cpunodebind=... --membind=...`：CPU/内存节点约束；first-touch、迁移和分配策略影响实际 NUMA 位置。
- `SCHED_FIFO` / `SCHED_RR`：实时调度策略，错误配置可能使系统其他线程饥饿。
- `isolcpus`, `nohz_full`, `rcu_nocbs`：隔离/减少 tick/迁移 RCU 回调；需同时考虑 IRQ、housekeeping CPUs、内核版本。
- CPU governor、C-states、SMT、IRQ affinity：对尾延迟可能比局部 builtin 更重要，但有功耗/吞吐/运维成本。
- `mlockall` 不等于锁定 CPU cache；绑核也不等于独占物理核。

## 11. 编译与链接参数

- `-O2`：通常适合生产基线。
- `-O3`：更积极的循环/向量化优化；不保证更快。
- `-Ofast` / `-ffast-math`：放宽浮点语义；定价与风控数值计算必须验证。
- `-march=native`：针对构建机 CPU；跨机器部署有非法指令风险。
- `-fno-exceptions` / `-fno-rtti`：改变可用语言机制，并非无风险的“性能开关”。
- LTO、PGO、BOLT：适合用真实 workload 评估代码布局与跨函数优化。

## 12. HFT 实战优先级

**优先验证**：数据布局/false sharing、cache miss 与 TLB、线程 affinity + NUMA、内存预热、原子同步正确性、PGO/LTO。

**有明确证据再尝试**：`restrict`、`assume_aligned`、`likely`、手动 prefetch、always_inline、huge pages、non-temporal stores。

**测量流程**：
1. 明确目标：p50/p99/p99.9、吞吐、CPU 使用率；固定输入和环境。
2. `perf stat` / `perf record`，结合 branch-miss、cache/TLB、context-switch、page-fault 等指标（事件支持因 CPU 而异）。
3. 编译器优化报告 + `objdump -drC` / Compiler Explorer 查看实际生成代码。
4. 一次只改变一个因素，重复多轮；记录 warmup、CPU frequency、NUMA、负载和尾延迟。
5. 性能收益不足以抵消 UB、可维护性或可移植性风险时撤销优化。

## 13. 重要纠偏

1. `restrict` 不等于 strict aliasing；`const` 也不等于 restrict。
2. `[[likely]]` 不直接修改硬件分支预测器。
3. `alignas(64)` 不保证所有平台 cache line 恰好 64 字节。
4. `std::assume_aligned` 不会实际重新对齐内存。
5. `mlockall` 不能保证零 page fault；`MADV_WILLNEED` 不是硬保证。
6. `seq_cst` 不总是最慢；应依据目标 ISA 和实际汇编。
7. hint 不保证收益，错误的契约型 hint 可能直接导致 UB。

## 14. 参考资料

- [GCC Other Builtins](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html)
- [GCC Function Attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html)
- [Clang Language Extensions](https://clang.llvm.org/docs/LanguageExtensions.html)
- [Linux madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)
- [Linux sched_setaffinity(2)](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
- [Linux mlock(2)](https://man7.org/linux/man-pages/man2/mlock.2.html)
- [Linux perf](https://perf.wiki.kernel.org/)
