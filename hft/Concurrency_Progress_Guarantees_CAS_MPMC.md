# 并发进展保证：Lock-Based、Obstruction-Free、Lock-Free、Wait-Free 与 MPMC

> 更新：2026-11-21  
> 主题：C++ 并发算法、CAS、MPMC Queue、线性化与 HFT Tail Latency

## 1. 核心结论

**Wait-Free 是对完整操作的算法级进展保证，不是毫秒级时间保证，也不等于「使用一次 CAS」。**

要分别证明三个维度：

1. **Correctness / Safety**：数据竞争、FIFO、无丢失/重复、slot 生命周期、内存可见性是否正确。
2. **Progress / Liveness**：竞争与线程暂停下，哪个线程能在多少自身步骤内完成。
3. **Latency**：实际运行的 p50/p99/p99.9/worst-observed，受 OS 与硬件影响。

不能通过一个线程「CAS 失败立即返回」来掩盖 API 语义错误。

## 2. Progress Guarantee 的严格定义

| 类型 | 保证 | 可能发生 |
|---|---|---|
| 传统 Lock-Based（如 mutex） | 不提供一般非阻塞进展保证 | 持锁者被暂停导致其他线程永远等待 |
| Obstruction-Free | 线程在独占执行足够久后能完成 | 持续竞争导致所有线程 livelock |
| Lock-Free | 系统层面保证持续取得操作完成进展 | 单线程饥饿 |
| Wait-Free | 每个操作在有统一有限上界的**自身算法步骤**内完成 | 仍然可能被 OS 抢占、遇到 cache miss |

逻辑包含关系：

```text
Wait-Free ⊂ Lock-Free ⊂ Obstruction-Free
```

传统 lock-based 不属于这条非阻塞层级中的一个严格「更弱等级」；它描述一种同步设计方式。

严格表述依赖执行模型、操作定义及原子原语的进展假设。Lock-Free 不是「每轮必有线程成功」，而是排除无限执行中系统始终不完成任何操作的调度（在模型允许的公平性/执行假设下）。

### Obstruction-Free vs mutex

- Mutex 的持有者 A 可能被 deschedule；B 即使独占 CPU 也不能获得锁。
- Obstruction-Free 操作在竞争者不再执行干扰步骤后，当前线程可以自行完成。
- Obstruction-Free 依然可能 livelock：两个线程持续覆盖对方中间状态，谁也不返回。
- **共享状态发生变化或某次 CAS 成功，未必代表某个完整操作完成。**

### Lock-Free vs Wait-Free

- Lock-Free 是 **system-wide progress**，允许某个线程 CAS 永远失败。
- Wait-Free 是 **per-operation bounded progress**，不能依赖其他线程继续执行以完成自己的操作。
- Wait-Free 的上界可依赖固定配置参数（如线程数 P、容量 N），但不能随着运行时竞争无限增长。

## 3. CAS 不自动决定算法等级

### CAS retry loop：通常不是 Wait-Free

```cpp
std::atomic<unsigned> counter{0};

void increment() {
    unsigned old = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(
        old, old + 1,
        std::memory_order_relaxed,
        std::memory_order_relaxed)) {
        // old 被更新后重试
    }
}
```

在适当底层原子进展保证下，这是典型 Lock-Free counter；某个线程可能无限重试，所以不能判定为 Wait-Free。注意 `compare_exchange_weak` 可出现 spurious failure；不要仅凭「失败必然有其他线程成功」对任意平台证明进展。

### Single CAS：仅说明一次尝试有界

```cpp
bool try_change(std::atomic<int>& x, int expected, int desired) {
    return x.compare_exchange_strong(expected, desired);
}
```

若底层 load/CAS 有界完成，该函数可视为 Wait-Free 的**尝试操作**。但：

- C++ 标准的 `is_lock_free()` / `is_always_lock_free` 反映原子实现是否 lock-free，**不直接保证 Wait-Free 的执行时间或步骤上界**。
- 「单 CAS 尝试完成」不等于「完整数据结构操作完成」。
- CAS 失败返回 false 是否符合语义，取决于 API 契约。

### 有界 CAS 次数

最多重试 K 次，随后返回竞争失败，只能证明**该尝试的循环次数有界**；还需检查整个函数（包括分配、拷贝、回收、atomic、等待及回调）。如果 API 要求 `false` 仅表示「队列满」，因竞争返回 false 违反了语义，即使耗时有界。

## 4. 为什么正确的 MPMC 比单条 CAS 复杂

以下代码是**故意不正确的**示意：

```cpp
bool broken_try_push(const T& value) {
    auto pos = tail.load();
    if (!tail.compare_exchange_strong(pos, pos + 1))
        return false;
    buffer[pos % capacity] = value;
    return true;
}
```

问题包括：

1. **Reservation ≠ Publication**：tail 提前更新，consumer 可能读取尚未初始化的槽位。
2. **Paused producer**：producer 在预留后、写入前暂停；consumer 不能无限等待它，否则失去 Wait-Free。
3. **Ring wrap/reuse**：需要每槽位 sequence/generation 或等价状态，防止覆盖尚未消费的数据与 ABA。
4. **Linearizability/FIFO**：需要定义操作的线性化点和合法并发顺序。
5. **Element lifetime**：构造/析构、异常、内存回收也须安全；一般 T 操作未必有界。
6. **Full/empty**：返回 false 的真实含义必须与队列 API 一致。

如果 CAS 冲突就立即返回 `false`，可能实现合法的 **best-effort claim attempt**；但若 `try_push` 规定「失败仅表示队列满」，此返回是错误的。**Wait-Free 是针对正确、完整且明确定义的操作的保证。**

### 队列的 Wait-Free 可实现吗？

可以；但通用 MPMC 需要证明多个 producer/consumer、publication、reclamation 与调度交错下的有界进展。**Helping**（线程发布 descriptor、其他线程协助完成）是常见技术之一，而非定义要求；帮助机制本身也不自动证明 Wait-Free。

特别注意：`try_push` 在满队列时有界返回 false 仍可以是 Wait-Free。Wait-Free 不承诺业务动作必然成功，而是承诺**所声明的操作语义有界完成**。

## 5. SPSC、MPMC 的工程区别

SPSC ring buffer 的 producer 独占 tail、consumer 独占 head，通常不需要 CAS 抢占索引；在固定容量、正确 acquire/release 发布、无不受控的 T 构造/析构、原子原语有界等条件下，非阻塞 `try_push/try_pop` 可以 Wait-Free。

MPMC 常需要争抢索引、处理每槽位状态、reservation/publication 顺序与线程暂停；常见 bounded MPMC queue 使用 CAS retry loop，通常只能给出 Lock-Free（甚至具体实现需要单独检查）。**MPMC / SPSC 是参与者数量，不是 progress classification。**

## 6. 「有界」不是 wall-clock deadline

算法中可给出例如

```text
steps(op) <= K(P, N)
```

这里的步骤数由操作和固定配置决定，不随着其他线程持续竞争无限增长。

但真实执行时间仍可能受：

- OS deschedule / preemption、SMI、IRQ；
- cache coherence、RFO、cache miss、NUMA；
- TLB miss/page fault；
- 内存分配、初始化、异常、调度等

影响。**Wait-Free 不自动等于 hard real-time，也不保证 p99.9 低于 Lock-Free。** Helping 与 metadata 也可能增大 fast path 成本。

## 7. 设计与面试 checklist

1. 明确操作是 `push`、`try_push` 还是 `claim_slot_once`；何时允许失败？
2. 对每个成功/失败返回，给出线性化语义。
3. 假设任意线程在任意指令处永远暂停，其他线程能否继续完成？
4. 是否可构造「所有线程无限运行，但没有操作完成」的 livelock？若可以，则不是 Lock-Free。
5. 是否可构造某单个线程无限重试、其他线程不断完成？若可以，则不是 Wait-Free。
6. CAS 失败后是否无限重试？若 bounded retry，失败返回是否符合 API？
7. 预留 slot 但未 publish 时 consumer 如何处理？是否存在无界等待？
8. 检查 ABA、sequence、slot lifecycle、memory ordering、allocation/reclamation。
9. 分别测量 throughput、p99.9 和 contention/high-load 下的行为；不要用 benchmark 代替进展证明。

## 8. 一句话面试回答

> Lock-Free guarantees system-wide progress, while Wait-Free guarantees each operation finishes within a bounded number of its own steps. A single CAS can make a claim attempt wait-free, but it does not by itself implement a correct wait-free MPMC queue: reservation, publication, FIFO, slot reuse and API failure semantics must all be handled correctly.

**关键原则：Correctness、Progress、Latency 必须分开证明与测量。**
