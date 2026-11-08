# Low-Level Systems Performance Reasoning — C++ / CPU / Linux / HFT（完整八讲）

> **用途**：Senior / Staff Low-Latency C++、HFT / Prop Trading、Market Data、Order Entry、Trading Infrastructure 的性能推理、实验及面试准备。  
> **版本**：2026-11-08；由 Sessions 1–8 系统重组、补充和校订。  
> **定位**：技术教材，而非聊天记录。数据中的 CPU 延迟、带宽、吞吐数字若未注明设备和来源，均为**假设性示例**，不能视为通用性能规格。

## 阅读说明

从一条 C++ 语句到最终 Tick-to-Trade Latency，至少跨越四个层次：

1. **语言及正确性**：C++ Object Lifetime、Data Race、Atomic、Happens-Before、Ordering、协议语义。
2. **编译及执行**：Assembly、µops、Dependency Graph、Frontend、OoO、Retirement。
3. **硬件及内核**：Cache、TLB、NUMA、Coherence、PCIe、DMA、Scheduler、Page Fault、Filesystem。
4. **系统及工作负载**：Queueing、Microburst、Backpressure、Tail Latency、Recovery、Measurement Bias。

全文将 **事实/规范保证**、**微架构常见实现**、**简化 Cost Model**、**条件性预测** 分开表述。Cost Model 用于估量数量级和生成可证伪预测，不能代替实测。

### 目录

- [Session 1：Performance Reasoning Fundamentals](#s1)
- [Session 2：CPU & Memory Access Reasoning](#s2)
- [Session 3：Atomic, Synchronization & Contention](#s3)
- [Session 4：Inter-Thread Communication & Data Ownership](#s4)
- [Session 5：OS, Virtual Memory & I/O Reasoning](#s5)
- [Session 6：Network & Low-Latency I/O Pipeline](#s6)
- [Session 7：Tail Latency, Overload & System Stability](#s7)
- [Session 8：Performance Investigation & HFT Case Studies](#s8)
- [Appendix A：实验设计矩阵](#appendix-a)
- [Appendix B：Senior HFT 面试题与答案要点](#appendix-b)
- [Appendix C：常见错误结论与校正](#appendix-c)
- [Appendix D：工具、资料与术语](#appendix-d)

---

<a id="s1"></a>
# Session 1 — Performance Reasoning Fundamentals

## 1.1 不从“代码行数”推导性能

性能推理的一般流程：

~~~mermaid
flowchart TD
  A[Workload / semantic invariants] --> B[C++ source and compiler]
  B --> C[Assembly and dependency graph]
  C --> D[Microarchitecture constraints]
  D --> E[Queueing and system state]
  E --> F[Observed latency and throughput]
  F --> G{Falsifiable prediction?}
  G -->|Test| H[Controlled experiment]
  H --> A
~~~

针对固定工作量，一种**理想化下界**：

$$
T\gtrsim\max(T_{\rm critical\ path},T_{\rm execution\ resources},T_{\rm memory\ requests},T_{\rm bandwidth}).
$$

各项可能互相影响，不能把它们当作独立测得的可加耗时。若跨进程/线程、存在队列，还需要显式建模等待时间。

### Latency 与 Throughput 不同

- **Latency**：从一次操作开始到结果可供依赖者使用的时间。
- **Throughput**：在持续工作量下单位时间可完成的操作数量。
- **Service Time**：服务器开始处理一条工作到完成的时间。
- **Response Time**：请求从定义好的到达边界到完成的时间，可能包括排队、网络及其他等待。

假设一类依赖操作延迟 $L=4$ cycles、硬件接收速率上限 $R=2$ ops/cycle。$K$ 条独立的持续操作链的稳态吞吐近似受限于：

$$
R_{\rm effective}\le\min(K/L,R).
$$

| 独立链数 $K$ | 依赖上界 $K/L$ | 理想有效吞吐 |
|---:|---:|---:|
| 1 | 0.25 | 0.25 ops/cycle |
| 2 | 0.50 | 0.50 ops/cycle |
| 4 | 1.00 | 1.00 ops/cycle |
| 8 | 2.00 | 2.00 ops/cycle |
| 16 | 4.00 | 2.00 ops/cycle |

这里是**持续链吞吐模型**。对于仅有几条操作的短片段，启动/收尾和指令发射时序也需要考虑。

## 1.2 Critical Path / Work / Span

考虑：

~~~cpp
// Serial dependency
x = f(x);
x = f(x);
x = f(x);
x = f(x);

// Independent work
a = f(a);
b = f(b);
c = f(c);
d = f(d);
~~~

第一段形成链式依赖；第二段可以由 OoO Scheduler 在资源允许时重叠执行。

理想并行计算中的通用下界：

$$
T_P\ge\max(W/P,S)
$$

其中 $W$ 是工作量、$S$ 是依赖图最长路径（Span）、$P$ 是抽象处理资源。这是**解释性类比**；实际 CPU 存在 µop 调度、Port、ROB、Load Buffer、Cache 和 Pipeline 细节。

### 为什么 Unrolling 有时提速？

它可能：
- 减少 Loop Branch / Index Update；
- 暴露多个 Independent Accumulators；
- 增加 ILP / MLP；
- 使 Compiler 更容易 Vectorize。

也可能变慢：
- Register Spilling；
- Frontend / I-cache 压力；
- 更长代码；
- 破坏原有 Compiler Optimization；
- 已经被 Load/Store Bandwidth 限制。

对于浮点加法，改变结合顺序还可能改变舍入结果；不能默认为语义等价。

## 1.3 Memory Latency vs Bandwidth：Little's Law

若平均请求延迟为 $L$、系统能保持 $M$ 个有效独立请求：

$$
R_{\rm requests}\lesssim M/L.
$$

例如一条 Cache Line 64 B、DRAM Miss 平均 100 ns、10 个有效 Outstanding Lines：

$$
R\lesssim10/100{\rm ns}=100M\ {\rm lines/s},
$$

相当于理想交付 $6.4$ GB/s。这**不是任何 CPU 的实测 DRAM 上限**，也没有计入 Prefetcher 带来的额外并发、Memory Controller、Cache Traffic 和其他瓶颈。

要从软件描述区分：
- 100 条**独立随机** Load：可能同时有多个未完成 Miss；
- 20 条**依赖指针** Load：下次地址只能在上次返回后知道。

因此 100 条 Load 完全可能比 20 条更快。

## 1.4 Resource-Based Throughput

对于资源 $r$，每个工作需要 $D_r$ 单位资源、资源单位时间容量 $C_r$：

$$
II\ge \max_r\left(D_r/C_r\right),
$$

其中 $II$ 是理想化的 initiation interval。依赖递推也可给出额外下界。

例：如果 Loop 每轮需要 2 个 Load，但硬件最多完成 2 个 Load/cycle，那么就算 ALU 有很大余量，Load 资源仍可能限制吞吐。减少无关 ALU 指令不一定提高速度。

## 1.5 Amdahl's Law

某段路径耗时 10 µs，其中 2 µs 可被加速 4 倍：

$$
T_{\rm new}=8+2/4=8.5\ \mu s,\quad {\rm speedup}=10/8.5\approx1.176.
$$

局部加速 4 倍，整体只改善约 15%。当系统接近饱和时，Service Time 下降又可能**间接减少 Queueing**；此时实际 Response Time 收益不一定受简单串行 Amdahl 模型限制。

## 1.6 案例：Shared Atomic Counter

100 个线程各执行一次普通 `++i`，如果 `i` 是非原子整型且无同步：C++ Data Race → Undefined Behavior。不能把“硬件上可能发生丢失更新”的推测当成语言保证。

若改为：

~~~cpp
std::atomic<std::uint64_t> i{0};
// 100 threads each do:
i.fetch_add(1, std::memory_order_relaxed);
~~~

全部完成且没有其他写入时，结果为 100。**正确性成立不代表线性扩展**：同一 Cache Line 的写权限争用可能使更多线程带来更低的总吞吐。

## 1.7 案例：Hot Path Logger

比较：
1. 单 Writer 直接写入已经准备好的独占缓冲；
2. 写 File-Backed `MAP_SHARED` 映射；
3. SPSC 转发给 Cold Logger；
4. 传递指针并做 Buffer Ownership Reclamation。

要先定义日志是否必须 Durable、允许丢弃/阻塞、是否需要 ACK。SPSC 引入跨核 Publication/Copy，却可能隔离文件系统 I/O；Direct Store 最快的微基准不意味着系统 p99.9 最好。

## 1.8 Session 1 实验与面试

**实验**：固定编译器、CPU、亲和性、工作集，比较 1/2/4/8 条依赖链；观察 Cycles/op、Loads/cycle、是否达到资源上限。再将数据从 L1 尺度扩展至 DRAM 尺度，观察瓶颈如何改变。

**问题**：
- Why does fewer instructions not always mean faster execution?
- Why can four independent loads complete much sooner than four dependent loads?
- How do you distinguish latency-bound from bandwidth-bound?
- Can a larger queue improve stability while worsening latency?

**回答结构**：Workload → Generated Instructions → Dependency/Resource Model → Measurable Prediction → Counterexample → Experiment。

---

<a id="s2"></a>
# Session 2 — CPU & Memory Access Reasoning

## 2.1 一次 Load 的微架构路径

~~~mermaid
flowchart TD
  A[Decode / Rename / OoO scheduling] --> B[Effective address generation]
  B --> C[Load queue and store-forward checks]
  C --> D[DTLB / L1D]
  D -->|hit| E[Wake dependent instructions]
  D -->|miss| F[L2 / LLC / DRAM request]
  F --> G[Fill / request tracking]
  G --> E
  E --> H[Architectural retirement rules]
~~~

这是概念图：DTLB、Cache Lookup、Store Forwarding 的实际时序可能重叠；Load Queue 与 Line Fill Buffer/MSHR 不是完全同一个结构。Load 的数据返回、依赖者可执行、指令 Retire 也不等价。

常见现代 x86 简单 L1 Load-to-Use Latency 约几个 cycles，**具体数值依处理器而定**。L1 Hit 不等于零延迟。

## 2.2 有效 MLP 而非“线程数”

一个 Core 可以发出多个 Independent Miss，但受到：
- Load Queue Entries；
- LFB / MSHR；
- ROB / Scheduler；
- 下层 Cache Request Tracking；
- Memory Controller / Interconnect；
- 地址依赖。

不要把 `N in-flight loads`、`N outstanding cache lines`、`N DRAM transactions` 当作相同数量。多条 Load 可能合并到同一条 Line Fill；Prefetch Requests 又可能具有不同资源路径。

**Predict**：增加独立链数时吞吐先提高、最终饱和。若性能提前恶化，检查 Spills、Frontend、Working Set、TLB 和 Cache Pressure。

## 2.3 Pointer Chasing vs Sequential Array

~~~cpp
struct Node {
    Node* next;
    std::uint64_t qty;
};

std::uint64_t sum_list(Node* p) {
    std::uint64_t s = 0;
    for (; p; p = p->next) s += p->qty;
    return s;
}
~~~

每次 `next` 决定下一地址，形成串行依赖；随机散布节点还可能触发 Cache/TLB Miss。若 100 次完全串行的 Miss 每次约 80 ns，理想量级为 8 µs，但不能假设实际每次都 Miss。

~~~cpp
std::uint64_t sum_array(const std::vector<std::uint64_t>& a) {
    std::uint64_t s = 0;
    for (auto x : a) s += x;
    return s;
}
~~~

线性访问带来较好 Cache-Line 利用率、Hardware Prefetch、潜在 SIMD 和并发加载。

**反例**：链表只需要访问 5 个 L1-resident 节点，而数组需要扫描 100,000 项；链表可能更快。算法操作数量及访问模式要一起比较。

## 2.4 AoS、SoA、AoSoA

假设 100,000 个对象、每个对象 64 B，每次只使用其中一个 8 B 价格字段。

- AoS Scan Footprint：约 6.4 MB。
- SoA Price Array：约 0.8 MB。
- 理想有效数据密度从 12.5% 到接近 100%。

但如果每次同时访问同一对象的 Price、Qty、Timestamp，AoS 可能减少需要访问的独立 Cache Lines。对 Top-of-Book 与 Full-Book Scanning，不一定选择相同布局。AoSoA 或 Hot/Cold Split 是折衷。

## 2.5 TLB、Page Walk 与 Huge Page

4 KiB Base Page 有 12-bit Page Offset。TLB Miss 可由 Hardware Page Walk 解决，不必引发 Page Fault。Page Fault 是权限/映射/页状态等导致的 CPU 异常处理。

简化覆盖量：

$$
\text{TLB coverage}=\text{entries}\times\text{page size}.
$$

64 条 Entry 的理想覆盖：
- 4 KiB → 256 KiB；
- 2 MiB → 128 MiB。

实际各 Page Size 的 Entry 数量与 TLB 层级不同，不能直接等价。Huge Pages 可能减小 TLB Pressure，但也可能增加 Fragmentation、资源预留及 NUMA 管理成本。

## 2.6 Prefetch：提前发起而非降低物理延迟

假设 DRAM Miss 为 100 ns、循环每 2 ns 迭代一次，则理论 Lead Distance 约：

$$
D\approx100/2=50\ \text{iterations}.
$$

这只是初步估计。实际还受 Miss Resources、Code Cost、Cache Eviction、Stride Detection 和不同数据层级影响。顺序数组通常应先验证 Hardware Prefetcher 是否已经足够。

~~~cpp
for (std::size_t i = 0; i < n; ++i) {
    if (i + 64 < n)
        __builtin_prefetch(&data[i + 64], 0, 3);
    consume(data[i]);
}
~~~

**反例**：当 Hardware Prefetch 已工作良好，软件 Prefetch 可能增加 µops、污染 Cache、占用 Outstanding Miss 资源。

对单链表，`prefetch(p->next)` 的地址仍需要先读取 `p->next`，不能消除基本依赖；多条独立链交错推进才真正增加 MLP。

## 2.7 Store Buffer、RFO、Non-Temporal Stores

在正常 Write-Back Cacheable Memory 上，Store 可以在进入 Store Buffer 并满足架构提交要求后，在后台逐步更新 Coherent Cache；**Store Retire ≠ 对其他 Core 可见 ≠ Durable on storage**。

当 Stream Store 向此前未缓存的 Line 写入时，典型 Write-Allocate 路径可能产生 RFO，然后 Dirty Line Writeback。粗略可有约 64 B Read + 64 B Write 的 Line Traffic，但不意味着每条 Store 都产生同样 DRAM Traffic。

Non-Temporal Store 可能帮助长流式写入减少普通 Cache Pollution / Write Allocate，却可能使随后马上读取的数据更慢；需处理 Alignment、Fence 和 Visibility 要求。

## 2.8 Store-to-Load Forwarding

CPU 可能从 Store Buffer 为稍后的 Load 提供数据。访问宽度、地址重叠、对齐关系不合适时可能导致 Forwarding Stall。

分析 Binary Protocol Serialization 时：
1. 确认 C++ 对象生命周期、Aliasing 和 Alignment 合法；
2. 检查 Assembly 中真实 Load/Store 宽度；
3. 做不触发 UB 的 Microbenchmark；
4. 观察该路径是否真的位于 Critical Path。

## 2.9 NUMA

固定线程在 Socket 0，不能保证其数据也分配在 Node 0。First-Touch、Memory Policy、NUMA Balancing 和初始化线程影响 Page Placement。

理想化依赖链对比：Local DRAM 90 ns、Remote 150 ns，100 次串行访问分别约 9 µs、15 µs；缓存命中或 MLP 存在时不能直接套用。

## 2.10 Session 2 实验

1. 随机化单链与 1/2/4/8 条独立链，扫描 L1/L2/LLC/DRAM 尺度。
2. Array Sequential、Random Gather、True Pointer Chasing 分开测。
3. AoS/SoA 按单字段扫描与多字段对象访问分别测。
4. 4 KiB vs explicit Huge Page，检查 DTLB Walk 与 Tail。
5. Software Prefetch Distance Sweep，观察收益和回归。
6. NUMA Local vs Remote，同时分别测试 Pointer Chasing 和 Streaming Throughput。

**面试要点**：100 independent loads 可能快于 20 dependent loads；随机访问≠依赖访问；Cache Miss Count≠Memory Stall Time；Huge Page 不是所有 TLB / Cache 问题的答案。

---

<a id="s3"></a>
# Session 3 — Atomic, Synchronization & Contention

## 3.1 先分 C++ 语义、硬件原子性和性能

~~~cpp
int i = 0;
// 100 threads concurrently execute ++i  // data race → undefined behavior
~~~

无同步地同时读写普通 `int` 属于 C++ Data Race。不能依据理想化硬件指令交错给出语言层“最终 1–100”的保证。

~~~cpp
std::atomic<std::uint64_t> i{0};
// Each of 100 threads executes exactly once:
i.fetch_add(1, std::memory_order_relaxed);
~~~

假设所有操作完成、没有其他修改，最终值为 100。`relaxed` 保证该对象的原子性与单一 Modification Order，但不提供跨不同对象的额外同步。

## 3.2 x86 常见指令映射

| C++ 操作 | 典型 x86-64（自然对齐且 lock-free） | 备注 |
|---|---|---|
| `load(relaxed)` | `mov` | 不要求独占 Cache-Line 写权限 |
| `load(acquire)` | `mov` | x86 TSO 对应映射通常已足够 |
| `store(release)` | `mov` | 普通 Store 即可承担常见 Release |
| `store(seq_cst)` | `xchg` 或其他 SC 实现 | 取决于编译器/上下文 |
| `fetch_add`，旧值未使用 | `lock add` | RMW |
| `fetch_add`，需要旧值 | `lock xadd` | 返回旧值 |
| `compare_exchange` | `lock cmpxchg` | 成功/失败按 API 处理 |
| `exchange` | `xchg [mem],reg` | 内存形式隐含 Locking |

不要把这些典型映射当作 C++ 标准或所有 Compiler 的固定汇编。`relaxed fetch_add` 与 `seq_cst fetch_add` 在常见 x86 上可能生成同一 Locked RMW，降低 Memory Order 不必然提速。

## 3.3 LOCK 与 Cache-Line Ownership

对普通 Cacheable Memory，现代 x86 Locked RMW 一般利用 Cache Coherence 协调原子更新，而非每条都“锁住整个外部总线”。特殊内存类型或不合适的跨界访问可能采用不同、更昂贵的机制。

~~~mermaid
sequenceDiagram
  participant C0 as Core 0
  participant CC as Coherent cache line
  participant C1 as Core 1
  C0->>CC: Request writable ownership
  CC-->>C0: Granted
  C0->>CC: Atomic RMW
  C1->>CC: Request writable ownership
  CC-->>C1: Granted after coordination
  C1->>CC: Atomic RMW
~~~

该图是逻辑协调示意，不代表某个中央锁或保证公平轮流。

**重要修正**：同一 Core 连续 RMW 且无其他 Core 争用时，通常不需要每次重新跨核获得 Cache Line；Locked Instruction 成本与 Coherence Transfer 成本必须分开。

## 3.4 100 线程竞争 Cost Model

设：
- $C_0$：无竞争的有效 RMW 成本；
- $H$：Ownership Handoff 的额外有效成本；
- $p$：需要跨 Core Handoff 的比例；
- $f$：参考 CPU cycles/s。

简化模型：

$$
C_{\rm eff}\approx C_0+pH,\quad R\approx f/C_{\rm eff}.
$$

假设 3 GHz、$C_0=12$ cycles、$H=80$ cycles：

| $p$ | Effective cost | 假设总吞吐 |
|---:|---:|---:|
| 0 | 12 cycles | 250 Mops/s |
| 0.5 | 52 cycles | 57.7 Mops/s |
| 1 | 92 cycles | 32.6 Mops/s |

这是**趋势示例**，不是特定 CPU 的性能规格。不同核心调度、请求重叠、SMT、NUMA 和仲裁都可能改变结果。

100 个软件线程不等于 100 个 Core 同时竞争；如果硬件只有 16 个并行执行上下文，其余线程还要等待 Scheduler。

## 3.5 CAS Retry 与 Failure

~~~cpp
std::uint64_t expected = state.load(std::memory_order_relaxed);
while (!state.compare_exchange_weak(
    expected, expected + 1,
    std::memory_order_relaxed,
    std::memory_order_relaxed)) {
    // expected updated on failure
}
~~~

`compare_exchange_weak` 允许 Spurious Failure。一次失败不等价于“另一个线程刚刚成功”。x86 上 Locked CAS Failure 也不能无条件按普通 Load 的成本估算。

若简化为各次成功概率恒定 $q$、尝试独立：

$$
E[\text{attempts}]=1/q.
$$

真实 Contention 往往有相关性，不保证满足几何分布。简单加法优先表达为 `fetch_add`，通常避免软件 CAS Retry Loop。

## 3.6 True Sharing、False Sharing、Reader/Writer

~~~cpp
struct Counters {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};
~~~

两个 Thread 写不同的 `a`、`b`，没有 Data Race，但若共用 Cache Line 可能有 False Sharing。可用合适的 `alignas` 和布局隔离。

如果都写同一个 `counter`，则是 True Sharing；Padding 不能解决本质上的全局写入协调。

纯只读多个 Core 可以同时持有 Shared Cache Copies；一 Writer 多 Reader 则可能因为无效化/重新读取产生 Coherence Activity，但不等价于 Writer/Writer RMW 竞争。

## 3.7 Spinlock、TTAS、Mutex

简单 Test-and-Set Spinlock 在失败时不断执行 RMW，可能制造大量 Cache-Line Contention；TTAS 先用普通 Load 检查，减少失败 RMW。

~~~cpp
class SpinLock {
    std::atomic<bool> locked_{false};
public:
    void lock() noexcept {
        for (;;) {
            while (locked_.load(std::memory_order_relaxed))
                _mm_pause();
            bool expected = false;
            if (locked_.compare_exchange_weak(
                    expected, true,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
                return;
        }
    }
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }
};
~~~

需要 `<atomic>`、`<immintrin.h>` 及 x86。代码用于说明机制，不包含公平性和指数退避。对于超短等待且 Holder 一直运行，Spin 可能很好；如果 Holder 被抢占，Spin 可能产生极长等待。`std::mutex` 的 Uncontended Fast Path 不必每次进入 Kernel。

**Lock-Free** 是系统总体进展性质，不表示每个线程 Wait-Free、无竞争、无 Tail、或优于 Mutex。

## 3.8 HFT 设计：Sharded Counters 与 Global Sequencer

Per-Core Counter 避免争夺单一 Cache Line，但聚合读取不是每次 Increment 的同一全局线性化结果。

若 Order Sequence 要求唯一且全局分配顺序，比较：
1. Global Atomic `fetch_add`；
2. Batch Reservation（降低 RMW 频率，但具体发布顺序可能与序号大小不一致）；
3. Single-Writer Sequencer（减少 Multi-Writer RMW，但有 Handoff 与串行服务限制）。

首先确定到底按 **allocation、submission、wire egress、exchange acceptance** 的哪个事件定义顺序。

## 3.9 Session 3 实验

- 1/2/4/8/16/32/100 Thread Shared Atomic；
- Same-Core SMT、Same Socket 独立 Core、Cross Socket；
- Same-Line False Sharing vs Separate Lines；
- CAS Retry Ratio 与 Fetch-Add；
- TTAS / TAS / Mutex，区分 Contention、Preemption 与公平性；
- 记录 Throughput、Per-Thread Completion、Context Switch、有效 Coherence Events。

---

<a id="s4"></a>
# Session 4 — Inter-Thread Communication & Data Ownership

## 4.1 三种 Ownership

| 概念 | 定义 | 管理者 |
|---|---|---|
| Logical Ownership | 谁被允许修改对象 | 程序不变量 |
| Lifetime Ownership | 对象何时仍然有效、何时可复用 | C++ / Pool / Reclamation |
| Cache-Line Ownership | 哪个 Core 当前能修改 Line | Coherence Protocol |

Logical Transfer 不等于物理数据“已转入”另一个 Core 的 Cache。

## 4.2 Publication / Reclaim 协议

单次发布：

~~~cpp
// Producer
payload = 42;
ready.store(true, std::memory_order_release);

// Consumer, after observing the matching release:
while (!ready.load(std::memory_order_acquire)) {}
use(payload);
~~~

Acquire 读取到对应 Release 所发布的值时建立 Synchronizes-With，保护之前 Payload Write 的可见性和非原子对象的访问顺序。**但不能直接把这段单次协议重复循环，除非还定义了 Consumer ACK 和 Producer 安全复用的约束。**

## 4.3 SPSC Ring 的核心不变量

假设容量 $C$、单调逻辑 Cursor `head/tail`：

$$
0\le tail-head\le C.
$$

- Producer 不覆写尚未消费的 Slot。
- Consumer 不读取尚未 Publish 的 Slot。
- 只有 Producer 写 `tail`；只有 Consumer 写 `head`。
- Slot Release / Acquire 的同步使旧内容的读取与后续覆写不重叠。
- 整数回绕与容量必须明确设计；绝对数学上的单调增长不能直接等同于无界的 `uint64_t`。

下面是**教学用 SPSC**：`T` 必须可默认构造、可复制赋值；假设 64-bit Cursor 在实验期间不回绕。此版本为易于证明而不是绝对最低延迟。

~~~cpp
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

template<class T, std::size_t C>
class Spsc {
    static_assert(C > 0);

    struct alignas(64) Cursor {
        std::atomic<std::uint64_t> value{0};
    };

    std::array<T, C> slots_{};
    Cursor head_;
    Cursor tail_;

public:
    bool push(const T& item) {
        auto t = tail_.value.load(std::memory_order_relaxed);
        auto h = head_.value.load(std::memory_order_acquire);
        if (t - h >= C) return false;
        slots_[t % C] = item;
        tail_.value.store(t + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        auto h = head_.value.load(std::memory_order_relaxed);
        auto t = tail_.value.load(std::memory_order_acquire);
        if (t == h) return false;
        out = slots_[h % C];
        head_.value.store(h + 1, std::memory_order_release);
        return true;
    }
};
~~~

进一步优化：
- Producer Cache Consumer Head，仅接近 Full 时 Refresh；
- Consumer Cache Producer Tail，仅接近 Empty 时 Refresh；
- 适当分离 Cursor Cache Lines；
- Batch Publish / Reclaim；
- Power-of-Two Ring（Index Mask）；
- Pipeline / Slot Layout 结合实际生产消费距离选择。

**反例**：每条 Message 都 Padding 到 64 B 可能避免一些邻接 Slot 读写干扰，却增大 Working Set 和 TLB Pressure。不能脱离访问模式下结论。

## 4.4 MPSC/MPMC：Reservation ≠ Publication

MPSC 的 Producer 通过 `tail.fetch_add(1)` 领取 Ticket 只说明 Slot 被预留；若 A 领取 Slot 10 之后被 Preempt，而 B 已填好 Slot 11，Consumer 不能因此假设 Slot 10 已初始化。

常见正确设计需要 Per-Slot Sequence、Ready State、Separate Commit Cursor 或其他完成协议。

MPMC 还要解决多个 Consumer 对同一消息的领取与归还。较多 RMW / State Transitions 可能增加争用；但性能取决于具体实现与工作负载。

## 4.5 Cross-Core Handoff Cost

一次交接可分解为：

$$
T_{\rm handoff}\approx
T_{\rm publish}+
T_{\rm visibility/detection}+
T_{\rm payload\ access}.
$$

这只是机制分类，不保证各项串行。一个 64 B Copy 可能很便宜，跨 Core Observation、Ring Cursor 和 Queueing 可能比 Copy 更昂贵。

## 4.6 Copy vs Zero-Copy

**Copy Queue**：Producer 将消息复制进 Slot，Consumer 读取 Slot；Lifetime 简单，可能有良好的本地 Cache 行为。

**Pointer Queue**：仅传递指针，但 Consumer 仍需访问 Payload，且需要 Buffer Pool、Reclamation、NUMA 和复用协议。

$$
T_{\rm copy\ design}\approx T_{\rm copy}+T_{\rm handoff}+T_{\rm consume}
$$

$$
T_{\rm pointer\ design}\approx T_{\rm handoff}+T_{\rm lifetime}+T_{\rm remote\ access}
$$

不要把这两个公式直接当作精确可加的耗时。对于 16–64 B Message，小 Copy 可能比共享引用计数或复杂 Pool 更优。对于大 Payload，避免额外 Copy 更可能有价值；Break-even 必须测量。

## 4.7 Batch：摊销成本与排队成本相冲突

每次 Publish 固定成本 $F$、每条处理成本 $P$、Batch Size $B$：

$$
C_{\rm avg}(B)\approx P+F/B.
$$

若每隔 $\Delta$ ns 到达一条，必须凑齐 $B$ 条后发布，平均 Batch Fill Wait：

$$
E[W_{\rm fill}]=\frac{(B-1)\Delta}{2}.
$$

例如 $B=16$、$\Delta=1\ \mu s$，第一条等 15 µs，平均等 7.5 µs。对于 15 µs p99.9 预算，这可能不可接受。

可结合 Size Threshold、Max-Age Deadline、Idle Flush，但需考虑 Timer Precision、Burst 和 CPU Overhead。

## 4.8 Backpressure / Ring Size

设 Arrival Rate $\lambda$、Consumer Rate $\mu$、Burst Duration $D$：

$$
Q_{\rm growth}\approx\max(0,\lambda-\mu)D.
$$

容量 $C$ 只能暂时吸收积压；长期 $\lambda>\mu$ 时，有限 Ring 终将满。扩容不改变 $\mu$，还可能增大最大容许 Queue Age。

## 4.9 Pipeline vs Direct Call

单线程：

$$
L_A=T_{\rm feed}+T_{\rm pricing}+T_{\rm publish}.
$$

分线程：

$$
L_B=T_{\rm feed}+T_{\rm handoff}+T_{\rm queue}
+T_{\rm pricing}+T_{\rm publish}.
$$

在相同各阶段服务成本、无排队时，B 增加通信开销；但 B 的稳态吞吐可能接近各阶段 Throughput 最小值，而单线程受阶段耗时总和限制。若分线程改变 Cache、CPU 或 Overload 行为，实际数字会随之变化。

## 4.10 HFT 三个应用案例

1. **Market Data Fan-Out**：共享可变 Book vs 每个 Consumer Replicated Book。Replication 避免部分 Coherence，却增加内存、复制计算和一致性成本。
2. **Async Logger**：将 File IO 隔离到 Cold Thread；必须处理 Full Queue、Durability、Crash Recovery。
3. **Zero-Copy Buffer Pool**：`FREE → PRODUCER → PUBLISHED → CONSUMER → RECLAIMED`；单一 Owner 时可避免热路径共享 Refcount，但多个 Reader 需要 ACK、Epoch、QSBR 或其他 Reclamation。

**面试重点**：Single Writer 不等于允许 Reader 直接无同步读取非原子数据；Buffer Ownership 的不变量先于性能优化。

---

<a id="s5"></a>
# Session 5 — OS, Virtual Memory & I/O Reasoning

## 5.1 Virtual Memory / Lazy Allocation

成功 `mmap` 或 `reserve` 地址空间，不意味着立即拥有对应 Physical Pages。

区分：
- Virtual Address Space；
- VMA / Page Tables；
- Committed Memory；
- Resident Memory；
- Cache Residency。

对 4 KiB Page 的匿名映射，首次写入尚未分配的页可能触发 Minor Page Fault：Kernel 检查 VMA、分配和清零 Page、建立 PTE，再返回重试原指令。

**TLB Miss** 通常可由硬件 Page Walk 解决，不必进入 Kernel；**Page Fault** 需要异常处理，可能由 Missing Mapping、COW、Protection 等触发。

假设 1 GiB 映射、4 KiB Pages、每页首次访问平均 Fault Handling 2 µs：

$$
N=1GiB/4KiB=262144,\quad
T\approx0.524s.
$$

这是模型示意；并非所有访问都必然 Fault，2 µs 不是通用数据。

## 5.2 File-Backed mmap vs Anonymous Shared Memory

| 映射 | 数据来源 | File Writeback |
|---|---|---|
| Anonymous `MAP_PRIVATE` | 匿名页 | 不是普通文件写回，可能涉及 Swap |
| File `MAP_SHARED` | 文件页缓存 | 修改可进入 Dirty Writeback |
| File `MAP_PRIVATE` | 文件映射 + COW | 私有修改不回写原文件 |
| tmpfs / memfd | 内存支持的文件对象 | 与普通磁盘后备文件不同 |

对 File-Backed `MAP_SHARED`：

~~~cpp
mapped[offset] = 42;
~~~

这通常是普通 Cacheable Store；但成功 Store 不代表数据 Durable。Linux 负责 Page Cache、Dirty Tracking 和 Writeback。显式 `msync(MS_SYNC)`、`fsync` 的语义与文件系统和存储设备相关，不能把它们理解为单条 Store。

## 5.3 Dirty Pages / Writeback

设产生**新的待回写文件数据**的速率 $R_p$、写回完成速率 $R_w$：

$$
\frac{dD}{dt}\approx R_p-R_w.
$$

若长期 $R_p>R_w$，待回写积压有增长趋势。重复修改同一个 Page 不会让 Dirty Footprint 按 Store Byte Count 无限线性增长。

假设 1 GiB/s 的顺序追加、200 MiB/s 的写回能力，2 GiB 额外 Dirty Budget：

$$
T\approx 2048/(1024-200)\approx2.49s.
$$

**关键校正**：Buffered `write()` 可以在内核 Write Path 直接触发 Dirty Throttling；普通 File-Backed `mmap` Store 不会每次执行相同的 `balance_dirty_pages` 路径。mmap 的延迟可能通过 Fault、Writeback Wait、Reclaim、Filesystem 或显式同步等**间接**体现。一定要 Trace 具体阻塞路径，不可仅凭磁盘繁忙推断每条 mmap Store 直接等待磁盘。

## 5.4 Reclaim、THP 和 NUMA

- Background Reclaim 例如 `kswapd` 维持可用内存；
- Direct Reclaim 可能出现在物理页分配需要同步回收时；
- Compaction 可能影响大页/高阶分配；
- Local NUMA Node 内存不足，即使全机仍有空闲 RAM，也可能产生压力。

THP 改善部分 TLB / Page Table Overhead，但运行时分配、Compaction、Split 等机制可能影响尾部。Explicit HugeTLB 可以提供更明确的资源管理，但有预留及部署成本。不能断言 THP 必然造成 Spike。

## 5.5 Pretouch、MAP_POPULATE、mlock

| 手段 | 主要目的 | 不能保证 |
|---|---|---|
| Pretouch | 把首次物理页准备提前 | 永远无 Fault |
| `MAP_POPULATE` | 映射时尝试预填充 | 成功返回代表全部成功 Populate |
| `mlock` | 防止正常换出/回收 | 数据固定在 L1/L2/LLC |
| CPU Affinity | 限制线程运行 CPU | 物理页一定在本地 NUMA |
| Explicit NUMA Policy | 限制/引导 Page Placement | 不发生 Coherence Traffic |

`std::vector::reserve` 保证容量扩展规则，不意味着所有内存页已触页，也不意味着成员对象内部没有 Allocation。

## 5.6 Syscall vs Interrupt vs Context Switch

- System Call：进入 Kernel 请求服务，可返回同一任务。
- Interrupt / Exception：CPU 转入处理程序，不必 Task Switch。
- Page Fault：异常处理，可同步修复后返回原线程。
- Task Context Switch：Scheduler 换一个任务运行。

因此 `syscall count`、`context-switches`、`page-faults` 衡量不同事件，不能互相替代。

## 5.7 On-CPU / Off-CPU

对于单线程请求时间：

$$
T_{\rm wall}=T_{\rm onCPU}+T_{\rm offCPU}.
$$

On-CPU 包括用户执行、Kernel 执行、Memory Stall、Spin；不等于 Useful Work。Off-CPU 包括睡眠、等待 Scheduler 等。

一个 25 µs Spike 可能是：
- 5 µs 执行 + 20 µs 被调度出去；
- 25 µs 全在 CPU 上等待依赖内存；
- 5 µs User + 20 µs Kernel Fault Handling；
- 其他复合情况。

## 5.8 HFT Logger RCA

对“mmap append 偶发 100 µs”：
1. 先明确是单条 Store 还是整段 Append 函数；
2. 区分 Anonymous vs File-Backed；
3. 检查 `/proc/PID/maps`、`smaps`、`numa_maps`；
4. 计数 Minor/Major Fault；
5. 观察 Reclaim / Writeback / Scheduler；
6. 关联具体 Spike；
7. 用 Pretouch、Memory Placement、File vs Anonymous 的受控实验验证。

不能在没 Trace 的情况下直接宣布“mmap 被 Dirty Throttling”。

## 5.9 实验

- Anonymous Mapping 首次写 vs 第二次写；
- Pretouch / `MAP_POPULATE` / `mlock` 对照；
- Buffered Write vs File mmap，分别测 Append Latency 与 Durability Latency；
- NUMA Local/Remote；
- 受控 Consumer Preemption 与 Page Fault；
- THP On/Off 仅在受控 Test Environment 中进行。

---

<a id="s6"></a>
# Session 6 — Network & Low-Latency I/O Pipeline

## 6.1 定义 RX、TX Timestamp

~~~mermaid
flowchart LR
  W[Wire RX] --> N[NIC PHY/MAC]
  N --> D[DMA + RX completion]
  D --> P[Poll / IRQ / NAPI]
  P --> F[Feed handler]
  F --> S[Strategy / Risk]
  S --> T[TX enqueue / CTPIO]
  T --> O[Wire TX]
~~~

至少区分：
- NIC RX Hardware Timestamp；
- Application Observes RX；
- Decode / Book Apply；
- Strategy Decision；
- TX Submission；
- NIC TX Hardware Timestamp；
- Remote ACK / Exchange Business Event。

**PHC、TSC、System Clock 是不同 Clock Domains**，不能未经同步校准直接相减。TX Completion 不一定代表 Wire Egress，更不代表 Exchange ACK。

## 6.2 Wire Serialization 与 PPS

帧占用链路的字节数 $S_{\rm wire}$、速率 $B_{\rm link}$：

$$
T_{\rm serialization}=8S_{\rm wire}/B_{\rm link}.
$$

以无 VLAN、64B Ethernet Frame（含 FCS）、8B Preamble/SFD、12B IFG 的总链路占用 84B 为例：

| Line Rate | 每帧链路占用 |
|---|---:|
| 1 Gb/s | 672 ns |
| 10 Gb/s | 67.2 ns |
| 25 Gb/s | 26.88 ns |
| 100 Gb/s | 6.72 ns |

注意 IFG 是连续发帧时的链路空闲间隔，不等同于单帧 first-bit→last-bit 时长；这个表强调**每包链路容量成本**。10 GbE 最小帧约 14.88 Mpps 只是理想链路上界，不是实际可达应用处理率。

## 6.3 NIC RX：DMA + Event Ownership

~~~text
Preposted RX buffers
  → NIC receives frame
  → queue classification / filters
  → NIC DMA writes host memory
  → RX completion/event becomes visible
  → driver/application consumes
  → buffer is safely reposted
~~~

数据 DMA 省去一类 CPU Copy，但并未消除 PCIe、Memory、Cache Coherence、IOMMU、Descriptor 和 Buffer Ownership。C++ `acquire` 并不能替代所有设备 DMA Memory Ordering Contract；遵守 Driver / NIC API。

## 6.4 Interrupt / NAPI / Busy Poll

传统 Kernel RX 可能经过 IRQ、NAPI Poll、Softirq、Protocol Stack、Socket Queue、Application Wakeup。现代 NAPI 与 NIC Interrupt Coalescing 能减少 per-packet Interrupt；两者机制不同。

NAPI 对 Batch $B$ 的简化模型：

$$
C_{\rm packet}\approx P+F/B
$$

其中 $F$ 为每批固定成本、$P$ 为每包处理成本。更大 Batch 提高摊销效率，却可能带来延迟与公平性问题。

Busy Poll 通过主动检测减少 Wakeup，但持续消耗 CPU。`SO_BUSY_POLL` 是 Linux Socket/NAPI 相关机制，不等于直接 User-Level `ef_vi` Event Poll。

## 6.5 RSS / RPS / RFS / XPS 与拓扑

- RSS：NIC 将 Flow 映射到 RX Queues；
- RPS/RFS：Linux 软件处理路径的 CPU 分配；
- XPS：TX Queue Selection；
- IRQ Affinity、CPU Affinity、NUMA 与 LLC Topology 决定部分 Locality。

Same Core 可能减少 Handoff，却增加执行竞争；Cross-Core 增加 Communication，但能流水化工作。**最优配置依工作负载与目标指标而变。**

## 6.6 TX DMA vs CTPIO

典型 DMA TX：

~~~text
Application builds packet → TX descriptor / doorbell
→ NIC fetches host data via PCIe DMA Read
→ NIC transmit
~~~

CTPIO（Cut-Through Programmed I/O）允许 CPU 把数据直接推送到支持的 NIC TX 路径，减少部分 DMA Fetch Critical Path。CTPIO **仍使用 PCIe**，不是“绕过 PCIe”。

Cut-Through 可以在获得足够数据后开始发送；`ct_threshold` 影响开始发送的阈值、延迟和失败/回退风险。

CTPIO 可能失败，因此对应 `ef_vi` API 需要遵循 **Fallback Descriptor** 的正确提交与 Completion 约束。不能把 Fallback 实现为不加判断地独立发送同一订单第二次，否则可能 Duplicate Submission。

官方设备/驱动支持的模式、链路速率与 API 细节因 NIC Generation 不同；不得假设所有 Solarflare NIC 都具备同样的 Cut-Through 能力。

## 6.7 Onload vs ef_vi

| 维度 | Solarflare Onload | ef_vi |
|---|---|---|
| 编程抽象 | Accelerated Sockets | Raw L2 / Virtual Interface |
| TCP/UDP 栈职责 | 保留较多 Socket 语义 | 应用/外部栈承担更多 |
| Buffer / Event 控制 | 按 Socket Stack 机制 | 较直接 |
| 更常见适用 | TCP FIX / UDP Socket Acceleration | 专用 UDP Market Data RX、特定 TX |

一个系统可能用 `ef_vi` 接收 UDP Market Data，用 Onload 加速 TCP Order Entry。实际选择取决于协议、工程复杂度、Fallback、Recovery 与 End-to-End Latency。

## 6.8 Packet Buffers / Completion 语义

RX：

`FREE → POSTED → DMA → RX_COMPLETE → APP_OWNED → RECYCLE`

TX：

`FREE → PREPARED → TX_SUBMITTED → NIC_USES_BUFFER → RECLAIMABLE`

**TX API 返回成功不等于 TX Buffer 立刻可复用，也不等于远端收到。** 要以具体 Completion Contract 为准。

## 6.9 综合网络容量模型

统一单位后：

$$
R_{\rm pipeline}\lesssim\min(
R_{\rm link},R_{\rm NIC},R_{\rm PCIe},
R_{\rm RX},R_{\rm CPU},R_{\rm app}).
$$

例如 Feed Handler 2 Mpps、Pricing 1.5 Mpps、NIC 4 Mpps，那么在其余条件充分时，端到端服务能力最多约 1.5 Mpps；单独把 NIC 升级到 8 Mpps 不解决 Pricing Bottleneck。

## 6.10 Session 6 实验

- Interrupt vs Kernel Busy Poll vs Onload vs ef_vi；
- RX Ring Size × Microburst；
- Same Core / Same LLC / Cross Socket；
- TX DMA vs CTPIO：以 Hardware TX Timestamp 和 Fallback 统计衡量；
- RX Copy vs Buffer Ownership Transfer；
- 全链路 Tick-to-Trade，分段时间戳但不直接相加各阶段 p99.9；
- 记录 NIC Drop、No-Buffer、RX Refill、TX Completion 和 Queue Occupancy。

---

<a id="s7"></a>
# Session 7 — Tail Latency, Overload & System Stability

## 7.1 Service Time 与 Response Time

在单 Server、FIFO、非抢占的简化模型中：

$$
D_i=\max(A_i,D_{i-1})+S_i,
$$

$$
W_i=\max(0,D_{i-1}-A_i),\qquad R_i=W_i+S_i.
$$

其中 $A_i$ 是到达时刻，$S_i$ 是服务时间，$D_i$ 是完成时刻；$W_i$ 是 Queue Wait，$R_i$ 是 Response Time。

一批 5 条消息同时到达，每条处理 5 µs：完成时间为 5、10、15、20、25 µs。即使 Service Time 不变，最后一条响应也比第一条慢 5 倍。

## 7.2 p99、p99.9 与尾部样本量

Nearest-rank：

$$
p_q=L_{(\lceil qN\rceil)}
$$

其中样本按延迟升序排序。对于 1M 个请求，p99.9 对应约第 999,000 个；最慢约 1,000 条仍可远高于 p99.9。

| 样本量 | 最慢 0.1% 中的期望样本数 |
|---:|---:|
| 1,000 | 1 |
| 10,000 | 10 |
| 100,000 | 100 |
| 1,000,000 | 1,000 |

只看 Average、p99.9 仍可能漏掉更罕见的 Critical Events；同时记录 Threshold Exceedances、p99.99、Max、Queue Age、Drops 和相关负载。

## 7.3 M/M/1 模型

假设 Poisson Arrivals、指数分布服务、单 Server、稳定：

$$
\rho=\lambda/\mu<1,\qquad
E[R]=\frac1{\mu-\lambda}.
$$

平均服务 5 µs、$\mu=200k/s$ 时：

| $\rho$ | M/M/1 平均响应 |
|---:|---:|
| 50% | 10 µs |
| 80% | 25 µs |
| 90% | 50 µs |
| 95% | 100 µs |
| 99% | 500 µs |

但 D/D/1 规律到达、规律服务，即使 95% 利用率也可能不形成 Queue。利用率重要，**Arrival/Service Variability 同样重要**。

## 7.4 Kingman Approximation（G/G/1）

$$
E[W_q]\approx
\frac{\rho}{1-\rho}
\frac{C_a^2+C_s^2}{2}\ E[S].
$$

$C_a,C_s$ 是 Inter-Arrival 与 Service Time 的变异系数（标准差/均值）。这是平均等待的近似，不是 p99.9 公式，不适用于任意强相关、批量或不稳定队列。

## 7.5 Microburst

若 Burst Rate $\lambda_b$、Service Rate $\mu$、Burst Duration $D_b$：

$$
\Delta Q\approx\max(0,\lambda_b-\mu)D_b.
$$

例如 1 Mpps 到达 1 ms、Consumer 400 kpps，积压约 600；Burst 后没有新输入时，以 400 kpps Drain 需要约 1.5 ms。平均输入即使仅 100 kpps，也可能有毫秒级 Tail。

必须明确 Peak Rate 的统计窗口：1 秒、1 ms、10 µs 结论可能完全不同。

## 7.6 Deterministic Arrival Envelope

若 Arrival Envelope：

$$
A(t+\tau)-A(t)\le \sigma+\rho\tau
$$

恒定速率 Server 保证 $\mu>\rho$，零额外 Startup Latency 的流体模型中可推导积压上界 $\sigma$，以及 Virtual Delay 上界约 $\sigma/\mu$。这要求**真实、可信的上界条件**，不能把历史最大 Burst 当作严格未来保证。

## 7.7 Head-of-Line Blocking

一个 100 µs 慢任务排在三个各 1 µs 的快任务前面，单 FIFO 完成时间约为 100、101、102、103 µs。后面快任务的延迟主要来自前面任务，不是自身执行变慢。

解决候选：
- 可正确 Partition 的 per-Instrument Queues；
- 独立 Worker；
- Bounded Drain / Priority；
- 将慢操作拆成更小步骤；
- 预计算。

但 Priority Queue 不能抢占已经在运行的非抢占任务；必须满足业务 Ordering。

## 7.8 Coordinated Omission

错误的单客户端 Closed Loop：

~~~cpp
while (running) {
    send_request();
    wait_for_response();
    record_latency();
}
~~~

Server Stall 100 ms 时，客户端不再按外部原定速率发出请求，于是遗漏本该继续到达并排队的工作。

更合理的 Open-Loop / Independent-Schedule 需要记录：
- Scheduled Arrival；
- Actual Send；
- Server Receive；
- Service Start；
- Completion；
- Generator Lag；
- Drop/Reject；
- Incomplete Requests。

不能把 Generator 未按时发出的延迟无条件归因于 Server；Histogram Correction 也不能完全替代实际施加持续负载。

## 7.9 Overload 与 Positive Feedback

如果长期 $\lambda>\mu$，有限 Queue 最终满。更严重时，Lock Contention、Retries、Memory Pressure、Timeout Work 可能使 **有效服务能力 $\mu$ 下降**，产生 Overload Collapse。

稳定性必须同时看 Offered Rate、Accepted Rate、Completed Rate、Queue Depth、Queue Age、Retry、Drop 和 Recovery Time。

## 7.10 Backpressure 是正确性约束

- Full-State Snapshot：在语义允许时可能 Coalesce/Replace；
- State-Dependent Incremental Book：不能随意丢弃中间更新；
- Order Gateway：Reject/Throttle/优先级策略必须符合状态机；
- Audit Logger：Drop、Block、Fail-Closed 取决于 Durability Policy。

若 Consumer 500k/s、允许 Queue Wait 最大 100 µs，单服务者固定成本模型下，新消息之前约 50 条就可能耗尽排队预算。即使 Ring Capacity 是 8,192，也不应等 Full 才控制流量。

## 7.11 Little's Law

在稳定系统：

$$
L=\lambda W,
$$

其中 $L$ 为平均在途工作数，$\lambda$ 为长期有效吞吐、$W$ 为平均停留时间。它**不直接预测 p99.9**，不能用 Queue Depth p99.9 简单除以平均流量得到 Latency p99.9。

## 7.12 统计置信度

若做 $N$ 次独立同分布试验，零次超时，单侧 95% 超时概率上界近似 $3/N$。例如 $N=10,000$ 时约 0.03%。如果事件有时间相关性、Workload 不平稳，不能直接套用。

---

<a id="s8"></a>
# Session 8 — Performance Investigation & Senior HFT Case Studies

## 8.1 Hypothesis-Driven RCA：八阶段闭环

~~~mermaid
flowchart TD
  A[Define boundary / correctness / workload] --> B[Reproduce baseline]
  B --> C[Decompose queue / service / off-CPU / network]
  C --> D[Competing hypotheses]
  D --> E[Falsifiable predictions]
  E --> F[Controlled A/B experiments]
  F --> G{Evidence consistent?}
  G -->|No| D
  G -->|Yes| H[End-to-end + correctness validation]
  H --> I[Regression guardrail / runbook]
~~~

**RCA 的中心问题**不是“能不能提出一个可能原因”，而是“什么证据能将它与其他解释区分开”。

## 8.2 时间戳分段与分位数

Feed→Quote 示例：

~~~text
T0 NIC RX
T1 RX observed in application
T2 Decode complete
T3 SPSC enqueue
T4 Pricing starts
T5 Pricing completes
T6 TX submitted
T7 NIC wire egress
~~~

分段：
- RX Delivery $T_1-T_0$；
- Decode $T_2-T_1$；
- Enqueue/other $T_3-T_2$；
- Queue $T_4-T_3$；
- Pricing $T_5-T_4$；
- TX Prep $T_6-T_5$；
- TX $T_7-T_6$。

同**一条消息**的这些区间相加得到对应端到端时间；**不同请求群体的各阶段 p99.9 不能直接相加**。

## 8.3 第一层定位

| 症状 | 优先观察 |
|---|---|
| Service Time 变慢 | Assembly、PMU、Dependency、Cache |
| Queue Wait 增大 | Arrival Burst、Slow Consumer、HOL |
| Wall Time 变慢但 CPU Work 未增长 | Scheduler、Fault、I/O |
| Multi-Core Throughput 退化 | Coherence、Atomic、Mutex、SMT |
| NIC Packet Drop | RX Ring、Refill、Burst、Filter、NIC Counters |
| TX Wire Delay | TX Queue、DMA、CTPIO Fallback、Timestamp |
| p99.9 变化但 p50 稳定 | Rare Path、Burst、OS、Queue Amplification、Measurement Bias |

## 8.4 perf 基本工作流

~~~bash
# Aggregate counters, repeated runs
perf stat -r 10 -e cycles,instructions,branches,branch-misses ./bench

# Look for page faults or scheduler disruption
perf stat -e page-faults,minor-faults,major-faults,context-switches,cpu-migrations ./bench

# Profile cycle samples
perf record -e cycles -g -- ./bench
perf report
perf annotate

# Inspect availability; event names differ by CPU and kernel
perf list

# Assembly / debugging symbols
g++ -std=c++20 -O3 -g -march=native -fno-omit-frame-pointer \
    example.cpp -o bench
objdump -drwC ./bench
~~~

`perf stat` 给总体事件；`perf record` 做 Sampling；`perf annotate` 把采样关联到指令。Generic `cache-misses` 不一定是 DRAM Miss，Cache Miss Count 不等于 Stall Cycles。

如果 PMU Events 多于同时可用 Counter，可能发生 Multiplexing；检查 Enabled/Running 比例，避免把不同时间窗口的派生指标当成严格同步观测。

## 8.5 Top-Down 框架

Intel TMA 顶层按 Pipeline Slots 归因：
1. **Retiring**；
2. **Frontend Bound**；
3. **Bad Speculation**；
4. **Backend Bound**（Memory / Core）。

Top-Down 帮助缩小瓶颈类别，但总体采样可能掩盖罕见 p99.9 Spike。应结合 Request-Level Timestamp、Kernel Trace 和工作负载变化。部分 PMU Metric 的具体事件、阈值、拓扑归因因 CPU 不同。

## 8.6 Experiment Quality

微基准必须说明：
- 测的是什么：单操作依赖延迟还是稳态吞吐？
- 是否可能被 Dead-Code Elimination？
- 输入工作集：L1/L2/LLC/DRAM？
- Core Affinity / NUMA？
- Warmup / Cache State？
- Compiler、Flags、Assembly？
- Sampling Cost？
- 是否跨越系统 Tail 场景？
- 是否与真实业务保持同样的正确性语义？

简单减去 Empty Loop Time 不总是有效；删除操作可改变 Compiler Optimization 与 Pipeline 行为。

A/B 建议 Interleaved、Paired，并考虑 Order/Cache Carryover。对于稀有尾部，必须收集足够样本，按独立运行或时间块考察不确定性；仅看一轮 p99.9 差 0.2 µs 不构成可靠的优化结论。

## 8.7 Case A：Pricing p99.9 从 15 µs 到 40 µs

**观察**：p50 基本不变，p99.9 上升，Queue Wait 增加。

候选：
- 新布局带来更长 Pointer Chain；
- L3/TLB Miss；
- Logging Slow Path；
- Scheduler / Fault；
- Burst 负载不同。

**证伪过程**：
1. 记录 Queue Enqueue、Service Start/Finish；
2. 找到第一条服务时间异常的请求；
3. 将其后的 Queueing Victims 与原始事件关联；
4. 比较 Assembly / PMU / Working Set；
5. Old vs New Layout、Cache Hot vs Cold；
6. 复测 End-to-End p99.9 和 Correctness。

不能把 Queue Depth 上升当作最初根因，它可能只是 Slow Pricing 的传播结果。

## 8.8 Case B：SPSC Logger 比 Direct Shared Memory 慢

先确认双方语义是否相同：
- Anonymous Buffer 还是 File mmap？
- Direct Writer 是否需要 ACK/Full Check？
- Async Logger 是否承担 Durability？
- 数据 Copy 的 Size？
- Consumer 所属 Core / NUMA？

候选实验：
- Cached Head vs Uncached；
- Same Socket vs Cross Socket；
- Small vs Large Payload；
- Shared File Mapping vs Anonymous Memory；
- Batch Publication；
- 不同 Consumer Stall。

**可能结论**：SPSC 增加单条 Producer Work，但隔离 I/O Long Tail；是否有价值取决于优化目标和完整故障语义。

## 8.9 Case C：Atomic 线程越多越慢

测试矩阵：
- 单线程连续 RMW；
- 两个 SMT Siblings；
- 独立 Core 同 Socket；
- 跨 Socket；
- 同 Line 两个独立变量；
- 分离的 Per-Core Counters；
- CAS Loop vs Fetch-Add。

分清 True Sharing、False Sharing、Scheduler Oversubscription、SMT 和 NUMA。Shared Counter Throughput 下降不一定意味着 DRAM Bandwidth 饱和。

## 8.10 Case D：平均 CPU 40% 但 RX Drop

可能原因：
- 1 ms Microburst 超过 RX/Consumer Capacity；
- Processing Spike 阻塞 RX Poll；
- RX Buffers 没有及时 Refill；
- NIC Filter / Queue Imbalance；
- Kernel/Driver Drop；
- Upstream Feed Gap。

关联 Per-Queue NIC Counters、Short-Window PPS、Service Time、Queue Occupancy、Gap Detection 和 Scheduler。不能以平均 CPU 未满排除短期过载。

## 8.11 Case E：一次 Stall 放大为多条 Tail

正常处理 2 µs/条、输入间隔 4 µs，平均利用率 50%。某条任务额外 Stall 100 µs：其间可新增约 25 条待处理；恢复后净排空速率约 $500k-250k=250k/s$，约 100 µs 才能清理。一个 100 µs Primary Stall 可能造成约 200 µs 影响窗口。

这是 **Primary Cause → Secondary Queueing**，需要在 Trace 中区分。

## 8.12 常驻轻量观察 + 条件深入调查

- Per-Core / Per-Thread Histograms，降低共享 Atomic Contention；
- Preallocated Flight Recorder：记录 RX、Enqueue、Start、Finish、TX；
- Spike 前的 Ring Events 不能被后续高负载覆盖；
- Spike 触发时 Freeze/Swap Buffer；
- 离线分析；
- 需要时启用更重的 PMU、ftrace、eBPF、Intel PT。

Intel PT 适合重建控制流，不直接提供每条 Load 的真实 DRAM Latency；LBR 可帮助 Branch Path 研究；Scheduler Trace 更适合定位 Off-CPU。

## 8.13 RCA 报告模板

~~~markdown
# Performance Regression RCA
## Observation / business impact
## Measurement boundaries and workload
## Reproduction steps and baseline
## Candidate hypotheses
## Predictions and falsification criteria
## Evidence: timestamps, PMU, traces, assembly
## Controlled experiments and competing explanations
## Root cause (with confidence and limitations)
## Fix and correctness invariants
## End-to-end A/B: p50/p99/p99.9, throughput, drops
## Residual risk and follow-up monitoring
## Regression benchmarks and owner
~~~

---

<a id="appendix-a"></a>
# Appendix A — 系统实验矩阵

| ID | 核心问题 | 受控变量 | 必须记录 | 可证伪预测 |
|---|---|---|---|---|
| A1 | ILP / 依赖 | 1/2/4/8 独立链 | Cycles/op、Throughput、Assembly | 增加链数初期提高吞吐，之后资源饱和 |
| A2 | Cache 层级 | Working Set L1→DRAM | Cache/TLB、Latency | 大工作集触发不同限制 |
| A3 | MLP | Random Independent vs Pointer Chain | Outstanding Miss、Bandwidth | Independent Loads 能更好重叠 |
| A4 | AoS/SoA | 单字段 vs 全字段读取 | Bytes、Miss、ns/element | SoA 不必在对象访问中更优 |
| A5 | Huge Page | 4 KiB / 2 MiB | DTLB Walk、RSS、Tail | TLB 受限时可能收益更明显 |
| A6 | False Sharing | Same-Line / Separate-Line | Throughput、Coherence | 分离 Line 减少无关 Ownership Transfers |
| A7 | True Sharing | Shared / Sharded Atomic | Ops/s、Retry、Placement | Shard 改善竞争，代价是聚合语义 |
| A8 | SPSC Layout | Padding / Cached Cursors | Producer/Consumer Latency | 最优结构依距离和 Cache Footprint |
| A9 | Batch | 1/2/4/8/16 | Throughput、Batch Wait、p99.9 | 吞吐提高但部分消息等更久 |
| A10 | Copy vs Pointer | 64 B→4 KiB，Local/Remote | Lifetime、Copy、End-to-End | Break-even 由环境决定 |
| A11 | First Touch | 首次/重复/Pretouch | Minor Fault、Wall、RSS | 前置 Fault 改变 Hot Path Tail |
| A12 | mmap vs write | File/Anonymous、Durability | Fault、Dirty、Writeback | 不同路径阻塞机制不同 |
| A13 | NUMA | Local/Remote | Page Placement、Latency、BW | 依赖访存更受 Remote Latency 影响 |
| A14 | NIC RX | IRQ/NAPI/Busy Poll | HW Timestamp、IRQ、CPU | Busy Poll 以 CPU 换检测延迟 |
| A15 | NIC TX | DMA/CTPIO | HW TX Timestamp、Fallback | Call Time 不等于 Wire Time |
| A16 | Burst | Constant/Poisson/Batched | Queue Age、Drop、Tail | 相同平均流量可有不同 Tail |
| A17 | Coordinated Omission | Open/Closed Loop | Scheduled/Actual/Completed | Closed Loop 低估持续到达下的积压 |
| A18 | Stall Propagation | 单次人工 Slow Path | 第一条慢请求、后续 Queue | 一个 Spike 产生多个 Queueing Victims |
| A19 | Overload Recovery | 逐步升降 Offered Load | Completed Rate、Retry、Recovery | 过载可产生 Hysteresis |
| A20 | Compiler Regression | 版本/Flags/PGO | Assembly、PMU、End-to-End | 源码相同不意味着执行形态相同 |

### Linux/CPU 快速命令（以当前系统支持为准）

~~~bash
lscpu
numactl --hardware
numactl --show

perf list
perf stat -r 10 -e cycles,instructions,branches,branch-misses ./bench
perf stat -e page-faults,minor-faults,major-faults,context-switches ./bench
perf record -e cycles -g -- ./bench
perf report
perf annotate

cat /proc/interrupts
cat /proc/meminfo
cat /proc/vmstat
cat /proc/<PID>/numa_maps
cat /proc/<PID>/smaps

ethtool -i eth0
ethtool -l eth0
ethtool -c eth0
ethtool -S eth0
~~~

提示：某些老 Kernel（例如 RHEL 7 时代）或受限 Developer Environment 不提供新 PMU Events、BPF Tracing Hooks 或必要权限。应先查询支持和权限，不要假设所有命令都能在生产环境使用。

---

<a id="appendix-b"></a>
# Appendix B — Senior HFT C++ 面试题与回答要点

每题的答题框架建议为：

**Semantics / Invariant → Mechanism → Cost Model → Counterexample → Experiment → Trade-off。**

## B1. CPU / Memory

**Q1. Why can pointer chasing be slow even when operations are O(1)?**  
每个节点访问为 O(1) 不代表低成本；随机布局、链式地址依赖、MLP 不足、Cache/TLB Miss 可能主导。检查依赖链、工作集，比较独立链与连续数组。

**Q2. Why may 100 independent random loads outperform 20 dependent loads?**  
并发 Miss 可以重叠，依赖地址不能提前知道；比较 $M/L$ 吞吐下界与串行 Critical Path。

**Q3. Can lower instruction count produce slower code?**  
可以：Dependency、Cache、Port、Branch、Vectorization、Spills 变化；先查 Assembly、PMU，后做 A/B。

**Q4. Is SoA always faster than AoS?**  
否。单字段批量扫描更可能受益；对象中心访问可能偏向 AoS。比较实际字段组合和 Cache-Line Footprint。

## B2. Atomic / Concurrency

**Q5. What happens when 100 threads execute ++i?**  
普通 `int` 并发无同步为 UB；Atomic RMW 全部完成结果 100。硬件不保证线程公平或按启动顺序执行；Core 争夺 Cache-Line 修改权限，不是 99 个线程自动 OS-block。

**Q6. What is the difference between relaxed fetch_add and seq_cst fetch_add on x86?**  
两者常产生同一 Locked RMW；C++ 语义与 Compiler 约束不同，不能据此推导其他架构或所有上下文完全相同。

**Q7. Why can failed CAS be expensive?**  
Locked CAS Failure 不一定是普通 Load；获取权限、排序、重试形成额外成本。用 Retry Ratio、Coherence 和控制变量验证。

**Q8. Does lock-free imply low latency?**  
不。Lock-Free 给系统总体进展，个别线程可长期失败；Wait-Free 也不自动给具体 Wall-Clock Deadline。

**Q9. Why is TTAS often better than repeated test-and-set under contention?**  
多数失败等待可用 Shared State Load，减少不断争夺修改权限；但 Unlock 时仍可能 Thundering Herd。

## B3. Inter-Thread Data

**Q10. Why can SPSC avoid CAS?**  
Cursor Single-Writer，Release Publish / Acquire Observe，需正确设计 Full/Empty、Slot Reuse；仍存在 Coherence 和 Queueing。

**Q11. Why is Reservation not Publication in MPSC?**  
获得 Ticket 不表示 Payload 已初始化。Producer 可乱序完成；必须有 Per-Slot Ready/Sequence 或等价 Commit Protocol。

**Q12. Can zero-copy be slower than copying?**  
是。小 Copy 便宜；Zero-Copy 仍有 Cache Transfer、Pointer Chasing、Pool/Reclamation。按 Message Size、NUMA、Lifetime 做 A/B。

**Q13. Why may a two-thread pipeline improve throughput but worsen single-message latency?**  
阶段并行提高稳态容量，跨 Core Handoff/Queue 增加请求路径。明确指标和工作负载。

## B4. OS / Network

**Q14. Does every page fault cause a context switch?**  
否，异常可能在 Kernel 内修复并返回同一线程；I/O Wait 或其他阻塞才可能调度其他任务。

**Q15. Can mmap writes be slowed by disk activity?**  
File-Backed mmap 修改文件页面，存在 Fault、Writeback、Reclaim、显式同步等路径；不等于每次 Store 都直接进入 Buffered Write 的 Dirty Throttling。

**Q16. Is mlock sufficient to make memory access predictable?**  
否。它不是 Cache Lock，也不固定 NUMA 或消除所有 Fault。配合 Pretouch、Placement、稳定的对象生命周期和监控。

**Q17. What does DMA eliminate?**  
减少每字节 CPU Copy 的特定路径，不消除 PCIe、Memory Access、Descriptor、Buffer Ownership。

**Q18. How does Onload differ from ef_vi?**  
Onload 加速 Socket 抽象；ef_vi 更直接的 L2 / DMA VI API，协议处理和 Ownership 责任更多。

**Q19. Why can CTPIO help TX and when can it hurt?**  
CPU Push 避免部分 NIC DMA Read Critical Path，可 Cut-Through；但受设备支持、Threshold、Payload、PCIe、CPU 成本与 Fallback 影响。测 HW TX Timestamp，而非只测调用。

## B5. Tail / Investigation

**Q20. Why can 40% average CPU utilization coexist with RX drop?**  
Microburst、短时间 Service Stall、RX Buffer/Refill、Queue Imbalance 等。观察短窗口 PPS 与 Per-Queue Counters。

**Q21. What is coordinated omission?**  
Closed-Loop Generator 等待响应再生成请求，Server Stall 时遗漏原定到达的请求/积压。记录计划与实际到达并检查未完成/拒绝请求。

**Q22. Why can't we add stage p99 values to obtain end-to-end p99?**  
各阶段 p99 可能来自不同请求，分位数不具可加性。必须记录同一请求的端到端时间。

**Q23. How to investigate p99.9 15 µs → 40 µs?**  
验证边界和 Workload；分解 Queue/Service/Off-CPU/Network；提出可证伪假设；Assembly+PMU+Trace；受控 A/B；验证 Correctness 与 Tail。

**Q24. How would you design a globally increasing order sequence without a bottleneck?**  
先澄清全局顺序定义（Allocation / Publish / Wire / Exchange）。Batch Reservation 减少 RMW 但可能破坏 Publish 顺序；Single Sequencer 增加 Handoff。不能在改语义的情况下宣称等价优化。

---

<a id="appendix-c"></a>
# Appendix C — 常见错误结论与校正

| 容易说错的话 | 更准确的结论 |
|---|---|
| “L1 Hit 不会形成瓶颈” | Load-to-Use Latency 和 Load Throughput 仍可限制 Critical Path |
| “Random Access 一定串行” | 独立地址可形成 MLP；Pointer Chasing 才有明显链式依赖 |
| “10 个 LFB = 最多 10 条在途 Load” | Load Instructions、Outstanding Cache Lines、DRAM Transactions 不等价 |
| “Cache Miss 越少一定越快” | Miss Cost、Overlap、Dependency、Bandwidth 和额外指令共同决定 |
| “x86 relaxed atomic 没有硬件排序成本” | Relaxed RMW 常仍用 Locked Instructions |
| “LOCK 每次锁总线” | 正常 Cacheable Memory 通常通过 Coherence 保证原子性 |
| “Atomic 100 线程，其余 99 个都阻塞在 OS” | 正在运行的 Core 在硬件层协调；其他软件线程可能未获 CPU |
| “Lock-Free = Wait-Free = 更快” | Progress Guarantee 与 Latency/Throughput 不同 |
| “SPSC 不会造成 Cache-Line Transfer” | Head/Tail Publication 和 Payload Ownership 仍有 Coherence |
| “Zero-Copy 就没有数据移动” | NIC DMA / PCIe / CPU Loads / Reclamation 仍存在 |
| “mmap Store 等于 Disk Write” | 内存修改与 File Persistence 解耦 |
| “每次 mmap Store 都调用 Dirty Throttling” | mmap 与 Buffered Write 的 Dirty Paths 不相同 |
| “System Call 一定 Context Switch” | Kernel Entry 与 Task Switch 是不同事件 |
| “CPU Affinity 就实现 NUMA Local” | 线程位置与物理 Page Placement 需分别控制 |
| “无 RX Drop 说明 Feed 延迟健康” | 可能积压大量旧消息，Queue Age 很高 |
| “Buffer 越大越稳定” | 增加 Burst Capacity 也增加潜在等待和 Working Set |
| “Kernel Bypass 一定降低业务端到端延迟” | 可能新增 Handoff/Queue/Protocol 处理 |
| “CTPIO API 返回就是已上 Wire” | 必须区分提交、Completion、Egress、远端 ACK |
| “平均 CPU 低说明系统不会过载” | 短窗口 Burst 可以超过服务能力 |
| “每次优化都应该减少指令数” | 要优化限制资源和关键依赖，而非抽象指令数 |
| “各阶段 p99.9 相加是总体 p99.9” | 分位数不可直接相加 |
| “一个 p99.9 数值足以比较性能” | 需负载、样本量、丢弃、失败、环境与不确定性 |

---

<a id="appendix-d"></a>
# Appendix D — 工具、来源与进一步阅读

### Linux / CPU

- [Linux perf wiki](https://perf.wiki.kernel.org/)
- [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html)
- [Linux VM sysctl](https://docs.kernel.org/admin-guide/sysctl/vm.html)
- [mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [mlock(2)](https://man7.org/linux/man-pages/man2/mlock.2.html)
- [Linux procfs](https://docs.kernel.org/filesystems/proc.html)
- [Linux NAPI](https://docs.kernel.org/networking/napi.html)
- [Linux AF_XDP](https://docs.kernel.org/networking/af_xdp.html)
- Intel Optimization Reference Manual 与 Intel Top-Down Microarchitecture Analysis 文档（具体 CPU PMU Events 以架构版本为准）
- Agner Fog 的 Instruction Tables / Microarchitecture Guides（指令吞吐、延迟的参考，不是生产机器的保证）
- Brendan Gregg：Systems Performance / perf / Flame Graph / Off-CPU Analysis

### NIC / HFT

- [AMD Solarflare Onload User Guide](https://docs.amd.com/r/en-US/ug1586-onload-user)
- AMD Solarflare `ef_vi` / CTPIO 文档：按 NIC、Driver、Firmware、Onload 版本核对 API 和支持列表
- [Linux ethtool](https://man7.org/linux/man-pages/man8/ethtool.8.html)

### 原理性参考

- Little's Law：$L=\lambda W$；
- Amdahl's Law；
- M/M/1 Queueing；
- Kingman Approximation；
- Network Calculus / Arrival-Service Curves；
- C++ Memory Model：Happens-Before、Modification Order、Acquire/Release、Atomic RMW；
- Intel / AMD 的 CPU Cache Coherence、TSO、Memory Ordering Documentation。

### 常用英文面试术语

| Term | 简短定义 |
|---|---|
| Critical Path | 决定完成时间的最长依赖路径 |
| Initiation Interval | 稳态相邻操作可启动的最短间隔 |
| Memory-Level Parallelism | 在途独立 Memory Requests 的重叠 |
| Cache-Line Bouncing | Cache Line 修改所有权在 Core 之间频繁迁移 |
| False Sharing | 不同对象共用 Cache Line 引发干扰 |
| Store-to-Load Forwarding | 从尚在 Store Buffer 的写入向之后 Load 供数 |
| Dirty Throttling | 因待写回数据积压限制写入速度 |
| Queue Age | 消息进入队列后已经等待的时间 |
| Head-of-Line Blocking | 队首慢工作阻碍后续工作 |
| Coordinated Omission | 负载生成与响应耦合导致部分本应出现的延迟未被测量 |
| Backpressure | 限制进入速率以防队列/资源无界增长 |
| Hardware Timestamp | 设备在定义好的硬件事件边界上记录的时间 |
| Hypothesis-Driven Profiling | 用明确可证伪的性能假设指导测量 |
| End-to-End Validation | 验证真实业务边界内的延迟、吞吐、正确性和稳定性 |

---

# Final Checklist：每次做 Low-Latency 优化前问自己

- [ ] **Semantics**：优化是否改变 Ordering、Ownership、Lifetime、Failure Policy？
- [ ] **Boundary**：正在优化 Function、Thread、Pipeline 还是 Wire-to-Wire？
- [ ] **Mechanism**：已找到关键依赖或限制资源，而不是仅凭经验猜测？
- [ ] **Model**：能否用 cycles、ns、Cache Lines、Mpps、Bytes/s 或 Queue Depth 估量？
- [ ] **Counterexample**：哪些负载/拓扑下优化可能变慢？
- [ ] **Experiment**：哪一个测量能推翻当前解释？
- [ ] **Measurement**：是否控制 Compiler、Cache、NUMA、Affinity、Sampling 和 Clock？
- [ ] **Tail**：是否报告了 p99.9、Threshold Exceedances、Drops 和 Incomplete Requests？
- [ ] **End-to-End**：局部收益是否真的传递到目标业务路径？
- [ ] **Preservation**：是否留下 Benchmark、RCA 和 Correctness Invariants？

**总结**：Low-Latency Engineering 的核心不是“尽量避免所有锁和系统调用”，而是先找出工作负载与正确性约束，再定位 Critical Path、限速资源和 Queueing 放大机制，使用可证伪的实验建立因果证据，最终验证真正的 End-to-End Performance。
