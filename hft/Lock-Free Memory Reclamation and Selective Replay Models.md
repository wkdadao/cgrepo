# HFT C++：低延迟通信、Fan-out、Replay 与 Memory Reclamation 设计

> **重构版：2026-11-04**。以 C++20、Linux、固定核心 Polling Processor、Single-Owner 与严格 Tail Latency 为主要设计背景。  
> **文档性质**：架构决策手册 + 正确性不变量 + 完整教学代码附录。各完整程序为协议演示，非生产级库；请勿在缺少完整审计和故障测试时直接部署。
>
> **阅读路线**：先读第 1–3 章明确语义；第 4–9 章选 Lifetime/Completion 协议；第 10–11 章处理 Fan-out、Membership 与 Replay；第 12–13 章做选型和验证；仅在实现具体算法时阅读附录 A–J。

## 一页结论与使用指南

| 情况 | 首选起点 | 注意 |
|---|---|---|
| 同 Core 同步调用，函数返回后不再保存引用 | 同步 Borrow，通常不需要跨核回收 | 不应为每个函数调用设计 ACK |
| 单 Consumer 跨 Core 移交所有权 | Move-Only Handle + SPSC | Queue Publish / Release 必须正确 |
| 固定全量广播、最终使用 FIFO | Multicast Ring + Per-Reader Gating Cursor | 慢 Reader 引起 Ring HOL |
| 多 Source → 多固定 Reader，扁平有序流 | Reader × Source Completion Matrix | Selective Skip 和动态订阅需要额外机制 |
| Sparse Selective Fan-out，共享大 Payload | N×SPSC + Shared Pool + Per-Target Completion | 投递部分成功与历史义务 |
| 固定多级 FIFO，完整同源 Seq | 主动聚合 Transitive Cumulative Cursor | Idle Progress / Flush 是必要条件 |
| Data2 引用 Data1，动态多级 DAG / 乱序完成 | Owner-Local Dependency + Hierarchical Per-Object ACK | 反向 Queue、去重、取消、死锁 |
| 任意动态 Fork，强共享所有权 | 预分配 Intrusive Atomic RefCount | Move 不加引用；Fork/Final Release 才操作 |
| 最近历史按 Seq Random Replay | Generation + Closing/CAS Pin，或复制独立快照 | 不能仅靠 Live Completion |
| 长 Replay / Research | Immutable Segment Archive + Index | Copy ACK / Retention / Eviction |
| Read-mostly immutable 配置 | HP / EBR / QSBR / Immutable Ownership | 与固定 Ring 原地覆盖不是同一个问题 |

**最重要的区别**：Delivery ≠ Last-Use Completion ≠ Reclamation ≠ Historical Retention ≠ Durable Commit。  
**安全和活性分开验证**：所有旧引用结束才能覆盖；所有引用已经结束时回收进度仍须最终传播。  
**性能不作无依据的排名**：Atomic RMW、Cursor Poll、反向 ACK、Copy、Cache Bouncing、Pool Headroom 必须用同负载测 p99.9 和 max。

## 文档结构

**Part I｜正确性基础（第 1–3 章）**
- 1. 需求、不变量与生命周期合同
- 2. 消息交付、异步逃逸与线程边界
- 3. 数据存储、Ring、Pool 与批量语义

**Part II｜跨线程 Completion / Reclamation（第 4–9 章）**
- 4. Intrusive Reference Counting 与 Zero Resurrection
- 5. Per-Object Marker 与 Sharded RefCount 陷阱
- 6. Cumulative Cursor：完成水位和单向 Mailbox
- 7. Reader × Source Matrix 与 Selective Fan-out
- 8. Hierarchical ACK 与 Parent–Child Dependencies
- 9. Cascaded Progress：Safety、Liveness 与 Hot-Path 隔离

**Part III｜实际拓扑与存储（第 10–11 章）**
- 10. Multiple SPSC、Multicast、动态 Reader Membership
- 11. Replay / Archive / HP / EBR / QSBR 的职责边界

**Part IV｜工程决策与验证（第 12–13 章）**
- 12. 场景选型和机制 Trade-off
- 13. 实验设计、正确性检查与故障注入

**Part V｜教学代码与深入参考（附录 A–J）**
- A. SPSC / MPMC；B. Selective Shared Pool；C. Pending Mask；D. Multicast Cursor
- E. CAS Pin；F. Replay Pointer Window；G. Replay Copy
- H. Hazard Pointer；I. EBR；J. QSBR

---

## Part I · 正确性基础与数据模型

### 1. 需求、生命周期合同与不变量

系统背景：单 Writer、多个 Reader（通常各自运行在固定 Processor / polling worker 上）；每条 Data 可能只送给 1 个 Reader，也可能送给任意子集。Writer 同时需要按 seq 随机查找最近历史数据以支持冷路径 Replay。原实现的固定 Slot 中有统一原子状态字，包含 seq/generation 与 refcount；Replay 通过 CAS Pin；**Writer 每次推进 Slot 也需要 CAS**。

#### 六个相互独立的设计问题

| 维度 | 要明确的语义 |
|---|---|
| Delivery | 交给一个、全部还是某个目标子集？ |
| Ownership | 谁能继续读取原始 Payload？唯一 Owner 还是 Shared Fork？ |
| Completion | 仅 Dequeue、已处理本地逻辑、最终使用完成，还是业务完成？ |
| Ordering | 完成前缀以哪个 Source/Edge 的 Sequence 定义？ |
| Retention / Replay | Live Consumer 已完结后，还需保持多久历史？ |
| Liveness / Capacity | 上游如何获知完成？Slow Reader 和 Queue Full 怎么处理？ |

**统一回收条件**（每个对象 x，针对它的 Generation）：

~~~text
LiveReclaimable(x) =
    PublicationClosed(x)
    AND OwnerLocalLastUseDone(x)
    AND EveryCommittedDownstreamObligationFinallyDone(x)

Reusable(x) =
    LiveReclaimable(x)
    AND NoReplayPin(x)
    AND NoArchiveCopyDependency(x)
    AND RetentionPolicyAllowsOverwrite(x)
~~~

注意：如果一个 Consumer 仍把原始 Data* / span / string_view 留给异步 Handler，那么该 Consumer 的完成义务尚未结束。Queue Head 推进、Handler 返回、P2 向 P3 Push 成功，都**不自动**证明 Reusable。

#### 必须分别证明的三个性质

1. **Safety**：Writer 对非原子 Payload 的覆盖发生在所有旧访问的最后一次读取之后（C++ happens-before），同时无 ABA / Generation 复用错误。
2. **Forward Progress**：在明确的公平调度和消费者不永久挂起的前提下，最终完成能够让 Owner 得知并回收；不能只证明“永远不会错误回收”。
3. **Bounded Resource Behavior**：Pool / Queue / Retired List 满时，系统采取明确策略，不因反向 ACK 或控制消息被饿死而永久停滞。


#### 六个必须拆开的架构维度

| 维度 | 必须回答的问题 | 可能采用的机制 |
|---|---|---|
| Delivery | 一条消息给一个、所有、还是指定目标子集？ | SPSC / MPMC Work Queue / Multicast / Selective Fan-out |
| Ordering | 按 Session、Order、Instrument 还是全局需要有序？ | Single Owner / Sharding / Sequencer |
| Lifetime | 哪些 Reader 仍在访问原始 Data？ | Ownership Transfer / Completed Cursor / CAS Pin / QSBR / EBR / HP |
| Retention | Reader 完成后还要保留多久？ | Recent Window / Immutable Archive / Durable Journal |
| Backpressure | Reader 停顿、Pool 满、Archive 落后时怎么办？ | Wait / Reject / Conflate / Drop+Recover / Copy / Isolate |
| Membership | Reader 增删或订阅更改从哪个 Seq 生效？ | Writer-Owned Registry + Control Event + Generation |

必须维护的不变量：**非原子 Payload 不能在旧 Reader 仍可能访问时被覆写**。Message Consumed、Business Completed、Original Memory Reclaimable、History EVICTABLE 和 Durable Commit 均不等价。

---

### 2. 交付、异步逃逸、线程交接与 Memory Ordering

| 模型 | 解决的问题 | 典型额外回收机制 |
|---|---|---|
| 固定容量 SPSC/MPMC，按值传递 | Queue 内部同步、单条消息交接 | 通常不需要 |
| SPSC/MPMC 传独占对象 Handle | 线程间所有权转移 | 可以不需要；需显式 Pool 归还 |
| 多 SPSC，Data* 发给多个 Reader | Selective Fan-out、共享生命周期 | Completed Cursor / RefCount / ACK |
| 动态节点 MPMC linked queue | 既要并发操作，又要删除内部节点 | HP / EBR / 其他安全回收 |
| 不经 Queue 的共享 Config* | 直接并发 Reader + 版本更新 | QSBR / EBR / HP / shared_ptr |
| Cold Replay 任意 seq lookup | 防止 Slot 被复用时仍在读取 | CAS Pin / Archive lifetime / 受控串行化 |

最优先的架构优化顺序通常是：**single ownership → message passing → immutable snapshot / replica → 必要时才引入通用回收**。这是 HFT 选型习惯，不是数学上的统一性能排名。

#### 业务处理阶段、线程边界、Data Lifetime 不是一回事

~~~text
业务处理：Market Data → Book → Pricing → Strategy → Risk → Order Entry → DMA

可能的线程布局：
  Core A: Feed Decode + Book Update + Strategy（同步函数调用）
  Core B: Risk + Order Entry（单独 State Owner）
  Core C: DMA Session / Network Writer
  Core D: Logger / Archive（后台）
~~~

即使存在七个业务阶段，也不等于有七次 SPSC 交接或七级跨线程生命周期依赖。公开资料没有可信的 HFT 全行业最大 Pipeline 深度统计；“2~4 级”只能是讨论架构时的经验起点，**不是行业上限、平均值或优化定律**。LMAX Disruptor 的 Gating Sequence / Dependency Graph 是工业界使用进度依赖的公开设计实例，但不能由此推断各 HFT 企业采用传递式 Completion Cursor 的比例。

对延迟尤其敏感的路径，首先考虑同一个 Owner Thread 内串行执行多个业务函数；仅当并行度、隔离需求、阻塞特性或状态所有权值得时才增加线程交接。

#### 什么是 Asynchronous Pointer Escape？

术语区分：

- **Pointer / Reference Escape**：原始指针或引用离开当前函数/组件的可控生命周期范围；在 Compiler Escape Analysis 中也有特定语境，**不等于必然跨线程**。
- **Asynchronous Data Escape**：下游在上游调用结束后仍访问原始 Data（典型 P2 SPSC → P3）；需要明确的 Lifetime Contract。
- **Ownership Transfer**：当前 Owner 把持有权转给下游；转移不等于新增引用。
- **Shared Ownership / Fork**：两个或多个下游同时保留使用权；必须登记额外保护。
- **Lifetime Decoupling**：下游转为独立 Value/Copy，原始 Pool 不再受它的完成时间限制。

~~~text
A. 同步借用（Non-Escaping）
    P1 → P2.process(Data&) → P3.process(Data&) → 返回
    P3 不保存指针；P2 可在返回后报告最终完成；无需 Copy。

B. Copy/Transform at Async Boundary
    P1 Data → P2 → 独立 DerivedEvent → P3 Async
    P2 完成后即可放开原始 Slot；P3 只依赖自己的数据。

C. Async Ownership Transfer
    P1 Data → P2 → move OwnedHandle → P3
    不复制 Payload；P2 不再使用原始 Data；P3 负责最终 Release。

D. Async Shared Borrow / Fork
    P1 Data → P2 ─┬→ P2 继续使用
                   └→ P3 Async 使用
    两边都结束使用后才能释放；RefCount / Completion DAG / 等价协议。
~~~

**不逃逸不意味着必须复制**：同步调用可直接 Borrow；异步转发也可不复制，但要跟踪生命周期。**Copy/Transform 不保证解耦**：若 DerivedEvent 含有指向原始 Pool 的 Data*、string_view、span，依然有 Escape。

#### 完成语义分级：P2 已转发不能宣布 Safe

~~~text
P1 (Data Pool Owner)
   ├─ SPSC → P2 → P3 Async → Final Use Done
   └─ SPSC → P4 Sync → Local Done

错误：
  P2 将 Handle push 给 P3 后立刻 completed[P2]=n；
  P4 done 后 P1 认为 Data[n] 安全；
  P1 Overwrite；P3 继续读旧 Data ⇒ C++ Data Race。

正确：
  P2 对外报告的 SafeCompleted[n] 必须包括其所有下游原始引用
  P4 完成之后 P1 还需等待 P2 的 Transitive Safe Completion。
~~~

**Completion Cursor 应表示所有 Seq <= C 的相关 Payload 使用都已结束（Contiguous Completion Frontier）**，不是 Dequeued、Forwarded、Scheduled 或“已完成的最大 Seq”。异步 callback 只要可能在 Cursor 提交后访问原始数据，Cursor 就不是 Safe Completion。

#### 两段 SPSC 交接的必要同步

~~~text
P1 writes Data1
  → forwardQueue release-publish
P2 forwardQueue acquire-pop
  → P2/P3 async final Data1 read ends
  → completion release-publish
P1 completion acquire-consume
  → P1 may overwrite Data1 (when all other obligations also done)
~~~

这是多级协议的基础：**唯一 Writer 的数据写入 → 正向发布 → 最后读取 → 反向发布 → 安全覆盖**。如果中间级只 Forward 并没有保留 Borrow，它可以通过 Move 退出所有权；如果保留 Fork，则必须等待全部子依赖。不能把队列的“收到消息”误认为生命周期完成。

---

### 3. Free Pool、Fixed Ring、Segment 与 Batch 的语义

#### 什么时候选择 Multiple SPSC，什么时候选择 Multicast？

~~~text
每条消息的消费语义：
  ├─ Exactly one fixed Consumer        → SPSC
  ├─ One-of-N competing Consumers       → MPMC / N×SPSC Dispatcher
  ├─ Every Consumer gets every message → Multicast Ring
  └─ Dynamic subset of Consumers       → Multiple SPSC + Routing
~~~

| 维度 | Multiple SPSC + Shared Pool | Multicast Shared Ring |
|---|---|---|
| Target 特征 | 稀疏、动态、各 Reader 不同 | 全量或几乎全量 |
| 每条发布 | 向 k 个目标各 Push 一个 Handle | 写一次 Data + Published Seq |
| Reader 获取下一条 | 各自 SPSC | Published Seq + 本地 Cursor |
| 回收依据 | 消息发布时的 Target Requirements | 全部相关 Gating Cursor |
| Slow Consumer | 拖住所持 Pool Slot；Pool 满可能扩散 | 拖住整个固定 Ring 的复用 |
| Dynamic Subscription | Writer 修改 Routing | 需要额外跳过/筛选协议 |

**小型 Payload 直接复制 N 次可能优于共享 Data Pool**：跨 Core 共享对象的生命周期和 Cache Coherence 并非零成本；大型 Payload 或高 Fan-out 则更有理由共享。不能仅由少一次 Copy 推断 p99.9 更好。

#### Free Slot LIFO Stack、FIFO Ring、Fixed Data Ring 是不同东西

~~~text
Per-Reader SPSC Ring:  传递 Slot Handle
Data Pool:             真正存储 Payload
Free Slot Stack/Ring:  Writer-only，存放可复用 Slot ID
Pending Retired:       等待所有旧 Reader、Replay 与 Archive 义务结束
~~~

| 维度 | LIFO Free Stack | FIFO Free Ring |
|---|---|---|
| Push/Pop | O(1)，管理成本较低 | O(1)，维护 Head/Tail/Count |
| 单 Writer 是否需要 Atomic | 否 | 否 |
| 复用模式 | 最近归还的先用 | 最早归还的先用 |
| 潜在缓存优势 | Working Set 更集中 | 相邻 Reuse 间隔更长 |
| 潜在缓存风险 | 热 Slot 在 Core 间反复迁移 | 更大 Working Set / LLC Miss |

先以预分配的 Stack 作简单基线，再根据 HITM、LLC Miss、NUMA、Pool 空闲水位、Writer p99.9 比较 FIFO Ring。**FIFO Free Slot Ring != Data[seq % Capacity] 固定原地覆盖 Ring**：前者可选任何安全 Slot，后者必须面对指定位置的 Head-of-Line Blocking。

若追求运行期 Zero Allocation，注意不止 Free Slot 容器，还包括当前示例的 Pending deque、动态 Registry 和可能扩容的记录数组。Pending 完成顺序可能乱序，不能只从队首回收，否则会制造新的 Head-of-Line Blocking。

#### 普通 SPSC 可以 Segment 化，但别混淆两个 Batch

**Batch Synchronization**：可按消息及时 Publish，Consumer 成批处理和更新 Completed，减少共享 Metadata 更新；仍然可以逐消息可见。

**Batch Visibility / Segment Ownership**：Producer 独占 WRITING Segment，Fill/Deadline/Explicit Flush 后 SEALED 并发布；Consumer Acquire 整段，消费完 Release 归还。这里可以按 Segment 摊销同步，但早到的消息等待 Seal。

~~~text
Producer owns WRITING Segment
    → Fill / Timeout / Explicit Flush
    → SEALED Publish (release)
    → Consumer Acquire and Process
    → Consumer Return (release)
    → Producer Reuse
~~~

Segment 级所有权转移本身可保证普通 Data 生命周期，无须在这个模型上额外叠加 EBR/HP；但 Consumer 归还后绝不可保存 Data 指针。要支持 Partial Flush，尤其是低流量、Remove Reader、Shutdown，否则 Tail 可能永远不可见。

**交易 Hot Path 优先逐条可见，Logger / Archive / 天然 NIC Poll Batch 更适合整段交接**。优秀的普通 SPSC 本来也可缓存 Head/Tail 并批量更新，不应假定 Segmented SPSC 必然更快。

#### Pool 数据结构分开管理

~~~text
SPSC<Handle>            : 消息投递与可见性
SharedDataPool[slot]    : 实际 Payload，受 Lifetime 协议保护
FreeSlots / RecycleFIFO : Owner 独占，已可复用的 Slot ID
Retired / Pending       : 尚不能回收的旧对象与目标依赖
History / Archive       : Retention 与过去 Seq 的独立查询
~~~

Free Slot LIFO、Free Slot FIFO 和固定 slot=seq%Capacity 的 Ring 没有相同的 HOL 特征。已完成的 Seq101 能否独立复用，取决于是否能自由分配 Slot，而不只是 RefCount 已经为零。

---

## Part II · Completion 与对象生命周期协议

### 4. Intrusive Reference Counting、Move/Fork 和 Zero Resurrection

概念：对象每个共享所有者持有一个引用；不需要注册全部 Reader；但不能在一个未经保护的 raw pointer 上先取指针再直接 fetch_add（acquisition race）。C++20 atomic<shared_ptr> 能安全取得共享所有权；是否 lock-free 取决于库实现。

**独立可运行示例：**

~~~cpp
// 01_atomic_shared_ptr.cpp
#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>
struct Config { int version, multiplier; };
int main() {
    std::atomic<std::shared_ptr<const Config>> current{
        std::make_shared<const Config>(Config{0, 2})
    };
    std::atomic<bool> go{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int n = 0; n < 10000; ++n) {
                auto p = current.load(std::memory_order_acquire);
                assert(p && p->multiplier == 2);
            } // p 在 scope 结束时释放
        });
    }
    go.store(true, std::memory_order_release);
    for (int v = 1; v <= 100; ++v)
        current.store(std::make_shared<const Config>(Config{v, 2}),
                      std::memory_order_release);
    for (auto& t : readers) t.join();
}
~~~

**Trade-off**：动态 Reader/异步 callback 简洁；每次 load 可能涉及引用计数同步和库内部锁；最后一次 release 可能触发 destructor / allocator，避免把它意外放在高优先级热路径上。循环引用仍需 weak_ptr。

#### Intrusive Reference Counting：Transfer 与 Fork 不同

对于固定的 Shared Data Pool + 任意多级下游逃逸，Intrusive Atomic RefCount 是自然的 Per-Object Lifetime Tracking。**Move Handle 不应无意义地 ref++ 然后 ref--；只有新增并行 Owner 才 Fork 引用。**

~~~text
P1 → P2 → P3 → Done
       Ownership Transfer: ref 不变，P3 Final Release。

P1 → P2 ─┬→ P2 Process
          └→ P3 Async
       Ownership Fork: 创建 P3 的第二份引用；
       P2 / P3 分别在最终使用完毕后 Release。
~~~

如果 Writer 在任何目标可见前已确认 k 个接收者并准备发布，可以初始化 k+1 个引用（+1 是 Writer 的临时持有），再把 k 个引用的 Handle 交给各目标，发布完成后释放 Writer 引用。也可先初始化 k，在**确保没有消费端能够使 Slot 被 Writer 回收且完成发布前 Writer 仍持有所有权**的等价协议下交接，但 k+1 的临时所有权更容易解释。

~~~cpp
struct Slot {
    std::atomic<uint32_t> refs{0};
    uint64_t generation{};
    Data data{};
};

// Only Writer, only when no old owners/pins remain:
void beginPublish(Slot& s, uint32_t targetCount) {
    // 目标计数 + Writer 临时引用
    s.refs.store(targetCount + 1, std::memory_order_relaxed);
    // Data、generation 初始化与 queue release-publish 协议另外保证
}

// Caller MUST ALREADY own a valid reference:
void forkRef(Slot& s) {
    s.refs.fetch_add(1, std::memory_order_relaxed);
}

void releaseRef(Slot& s) {
    const auto before =
        s.refs.fetch_sub(1, std::memory_order_release);
    assert(before > 0);
}

// Writer / Reclaimer: check after all publishing obligations resolved
bool noOwners(const Slot& s) {
    return s.refs.load(std::memory_order_acquire) == 0;
}
~~~

安全条件：

- 若只做 Ownership Transfer，队列 Push 成功后原 Owner 不得再访问/Release 同一份 Handle；失败则仍由原 Owner 持有、重试或按既定策略释放。
- Fork 的前提是调用者仍持有合法引用；**不可通过已经失效的 raw Data* 或被复用的 Slot 直接 fetch_add 企图“复活”对象**。
- 先为每条成功或计划发布的分支建立有效引用，才允许 Reader 取得 Handle。部分 Fan-out 失败时必须正确撤销未投递引用并处理已投递分支。
- 释放完成用 Release，Writer 观察零用 Acquire；对象可变 Payload 只有在所有 Reader 结束读取之后才能写入。实际生产实现还要审计 RefCount Overflow/Underflow、异常、Handle 双重移动/双重 Release、同一 Cache Line 上 Data 与 RefCount 的 False Sharing、Generation 复用。
- Writer 不应仅因为 refs==0 就默认历史可以淘汰；Recent Replay CAS Pin、Archive Copy 与 Retention 条件可能独立存在。
- 若 Reader 能绕过 OwnedHandle 随机按 Seq Lookup，此处的 refs 不能直接保证原子引用取得；仍需要 Stable State + Generation CAS Pin / HP / 等价安全 Lookup 协议。

**特别注意初始目标数 k 并非越早决定越好**：只要队列可能满/订阅变化/部分发布，RefCount 的分支承诺与消息投递都必须一致，不能在仍有 Writer 临时引用时错误回收。

#### Zero Resurrection 不能直接解决 Sharded RefCount 的全零快照

统一强引用计数的规则是：只有当前 Count 非零才允许增加；最后一个强引用消失后不能通过同一个生命周期从 0→1。类似 weak_ptr::lock() 对受保护 Control Block 的 Atomic Acquire 原理：

~~~cpp
#include <atomic>
#include <cstdint>
#include <limits>

bool tryAcquire(std::atomic<uint32_t>& count) {
    uint32_t current = count.load(std::memory_order_relaxed);
    while (current != 0 && current != UINT32_MAX) {
        if (count.compare_exchange_weak(
                current, current + 1,
                std::memory_order_acquire,
                std::memory_order_relaxed))
            return true;
    }
    return false;
}
~~~

**控制块/Slot 本身必须仍存活且版本未变**；CAS 并不能保护已被 free/reuse 的裸地址。已有有效 Strong Owner 的 Fork 可以先 fetch_add(relaxed) 后发布新 Handle；Move-Only Transfer 无须 AddRef。

对于 Per-Reader Mutable Sharded RefCount，即使不允许**全局总数**从 0 增加，也不意味着一次多分片扫描的全零结果可靠：

~~~text
初始：R1=1, R2=0
Writer load(R2) -> 0
R1 合法地把引用交给 R2：R2 0→1（先增），R1 1→0（后减）
Writer load(R1) -> 0
扫描读到全零，但任意时刻真正总和都 >= 1。
~~~

甚至 seq_cst 也允许上述 Atomic Interleaving。若让**每个分片**对该 Generation 的 0 都成为不可逆终态，且发布前全部使用义务已建立、不再动态新增，那么 Writer 扫全零可安全成立，模型就接近 Per-Reader Completion Marker。

若允许动态跨分片 Fork/Transfer，要增加严谨的 Closing/Freeze、阻止新的引用获取、协调已经开始但还未计数的 Acquire、等待 Quiescence / 完整验证。不能简单 closing.store(true) 然后立即扫描。此类同步对每个 HFT Data 都运行可能很昂贵。

#### 统一强引用计数和多分片计数的边界

**已有有效强引用**可以在释放自身之前安全增加另一份引用；没有有效强引用的裸地址不能直接尝试“复活”，即使采用 CAS Increment-If-Nonzero。Control Block 自身必须由有效 Weak Handle、稳定 Slot + Pin/Generation 或等价协议保活。

需要特别注意：单个 Atomic Strong Count 的 Zero Resurrection 保护的是**统一计数器**；对于 Reader-Local 的多个可增减计数器，逐个扫描全零仍非原子快照。下章单独分析此陷阱。

---

### 5. Per-Object Completion Marker 与 Sharded Counter 陷阱

#### 最初设想：每个 Data 配一组 Reader-Local 原子 RefCount

~~~cpp
struct ShardedRefCount {
    std::atomic<uint32_t> perReader[ReaderCount];
};

bool allZero(const ShardedRefCount& s) {
    for (const auto& n : s.perReader) {
        if (n.load(std::memory_order_seq_cst) != 0)
            return false;
    }
    return true;
}
~~~

**不能把上述 allZero() 直接视为通用可回收证明！** 当分片计数能够增减、引用允许从一个 Reader 转移到另一 Reader 时，Writer 逐个 Load 所得的不是一个 Atomic Snapshot。即便全部使用 seq_cst，也可能得到不存在于任何瞬间的全零组合。

~~~text
初始：R1=1, R2=0
Writer: load(R2) → 0
R2:     fetch_add(R2) → 1    （先获取新引用）
R1:     fetch_sub(R1) → 0    （再释放旧引用）
Writer: load(R1) → 0

Writer 观察全零，但 R1+R2 始终 >= 1。
~~~

这不是 Cache 可见性问题，而是**跨对象快照 + 仍允许新引用出现**的问题。通用 Distributed/Sharded RefCount 需要关闭新引用入口、正确迁移、Epoch / Freeze / Validation / 归并或等价的 Reclamation Protocol。类似 Linux percpu_ref 的做法不能简化成“扫一遍所有 CPU 为零”。

如果每个 Reader 对某个 Slot Generation 只有一次完成义务、且完成后绝不重新取得此 Generation 的引用，改为**单调的 Completion Marker** 更容易证明正确性。

#### Per-Data × Per-Reader Completion Marker：支持对象级乱序回收

~~~cpp
struct Handle {
    uint32_t slot{};
    uint64_t generation{};
};
struct ReaderMarker {
    std::atomic<uint64_t> doneGeneration{0};
};
// 概念结构：实际生产中优先预分配、分离 Metadata 和 Payload
struct SlotState {
    uint64_t generation{};    // 只有 Owner Writer 安全地分配/复用
    uint32_t targetMask{};    // 发布时确定的旧对象目标集合
    std::array<ReaderMarker, ReaderCount> done;
};

bool safeToRecycle(const SlotState& s) {
    for (size_t r = 0; r < ReaderCount; ++r) {
        if ((s.targetMask & (uint32_t{1} << r)) == 0)
            continue;
        if (s.done[r].doneGeneration.load(std::memory_order_acquire)
                != s.generation)
            return false;
    }
    return true;
}
~~~

Reader 必须在对应原始 Data 的**最后一次使用结束**（包括所有下游异步使用）后，才对自己拥有的 Marker 做 Release Store。Writer Acquire-Load 每个实际目标的 Marker，全部匹配之后才能覆盖旧 Payload。示例隐含 ReaderCount <= 32、同一 Marker 同一时刻只有一个 Reader Writer、无 Generation Wrap、Slot 不会在未满足全部保护时复用。

特点：

- Reader 不执行共享 RefCount 的 Atomic RMW，只对自己的 Marker 做 Store；但跨核 Cache Line 仍有 Coherence 成本；
- 任意单条 Data 允许乱序完成，Writer 可以独立回收；
- Metadata 为 O(PoolSlots × ReaderCount)，Writer 回收单个 Slot 需要扫该 Data 的实际 Target Set；
- 如果不同 Reader 的 Marker 紧挨同一 Cache Line，即使 Atomic 各自独立也会产生 False Sharing；Per-Reader Array Layout / Padding 需实测；
- 如果下游 P3 继续借用 P2 原始 Data，P2 的 Marker 仍必须等待 P3 完成，不能在 SPSC Push 成功时提前发布。

**这是 Per-Object Completion Tracking，不是 Cumulative Cursor。**

#### 为什么 Marker 的单调性关键

如果 Publication 已关闭，所有目标义务已登记，每个 Reader 对该 Generation 只执行一次从 Pending→Done 的发布，则读到“完成”之后不能再被新的旧引用撤销。Writer 对各目标分别 Acquire Load 可以得到安全但可能保守的结论，不需要对所有目标做原子 Snapshot。若分片是可反复增减的 RefCount，**不能**套用该证明。

---

### 6. Per-Stream Cumulative Cursor 与 CompletionChannel

#### Transitive Completion Gating：固定 FIFO 链

最简单的场景：P1 → P2 → P3，P2、P3 处理所有事件、使用兼容的连续 Seq、无未跟踪的额外 Fork、P2 本地最后访问可记录为 localDone2，P3 的安全完成水位为 safeDone3：

~~~text
safeDone2 = min(localDone2, safeDone3)
P1 可回收 Seq s ⇔ safeDone2 >= s  （还要满足 Replay/Retention 条件）
~~~

若 P2 完全把 Data 的唯一引用移交 P3，且 P3 对所有相关消息的完成水位已经能证明 P2 不再访问，P1 可以直接使用 P3 Cursor；不必无意义地让 P2 中转 ACK。

**示意代码，假设同一连续 Seq 域**：

~~~cpp
// P3：最后一次访问旧 Payload 之后
processP3(pool[handle.slot]);
p3Done.store(handle.seq, std::memory_order_release);

// P2：确保本地对前缀内的 Data 全部结束访问，且
// 后续所有逃逸都已被 P3 的完成水位覆盖
void updateSafe() {
    const auto downstream =
        p3Done.load(std::memory_order_acquire);
    const auto candidate = std::min(localDone2, downstream);
    if (candidate > publishedSafe2) {
        publishedSafe2 = candidate;
        p2Safe.store(candidate, std::memory_order_release);
    }
}

// P1：必须观察到覆盖目标旧 Seq 的安全进度
bool reusable(uint64_t oldSeq) {
    return p2Safe.load(std::memory_order_acquire) >= oldSeq;
}
~~~

这段不是完整可编译的组件：省略了队列管理、RAII、初始化与退出、Sequence Wrap、Retention、History Lookup 和异常处理；且当 P2/P3 并非对**同一完整 Seq 前缀**处理消息时，不能照搬 min()。

**Happens-before 关键链**：

~~~text
P3 最后一次读 Data
   → P3 release-store SafeDone
   → P2 acquire-load 对应 SafeDone
   → P2 release-store Transitive SafeDone
   → P1 acquire-load 对应 SafeDone
   → P1 Overwrite 原始 Data
~~~

P2 自身的最后一次读取同样需要 Sequenced-Before 它的 SafeDone Release。这里 Acquire 必须实际观察到足够新的有效 Release/Release Sequence 值；单纯使用两个没有关联的 Relaxed Loads/Stores 不会自动建立 Data 复用所需的同步关系。

#### Cursor 应该是 Per-Data、Per-Thread、还是 Per-Pipeline？

**推荐归属：每一个具有明确 FIFO 投递与完成语义的 Logical Consumer Stream / Pipeline Endpoint，一个 Completed Cursor。** 它描述某条独立流的连续完成前缀，并不天然绑定 OS Thread。

| 处理拓扑 | 应如何设置 Cursor |
|---|---|
| 一个 Thread 仅处理一个 FIFO SPSC | 可简单做成 Per-Thread Cursor（恰好等于 Stream） |
| 一个 Thread 消费三个独立 SPSC | 通常三个独立 Stream Cursor |
| P2 → P5 多级 Pipeline | P2 对 P1 公布 Pipeline SafeCompleted（包括下游最终使用） |
| 一个 Pipeline 下有多 Worker 并发乱序执行 | 需要聚合的 Contiguous Completion Frontier |
| 一个 Thread 轮询多个无关 Pipeline | 各自独立 Cursor；不能用线程最大 Seq 替代 |
| 每条 Data 一个 atomic<bool> completed | 这是 Per-Object Completion Flag，不是 Cursor |
| 每条 Data 一个 pendingMask / refs | 这是 Per-Object Lifetime Tracking，不是 Cursor |

Selective Fan-out 的 Writer 除了这些 Per-Stream Cursor，还需为各个 Outstanding Data 保存**发布时的 Target Set 与对应 Required Order**：

~~~text
Data x:
  slot = 7
  targets = {P2, P4}
  required[P2] = localOrder 105
  required[P4] = localOrder 88

SafeToRecycle(x) =
  completed[P2] >= 105
  AND completed[P4] >= 88
  AND NoReplayPin(x)
  AND NoOutstandingArchiveDependency(x)
  AND OriginalRetentionSatisfied(x)
~~~

其中 completed[P2] / completed[P4] 必须是**包含全部异步下游使用的 Transitive Safe Frontier**，或由 Writer 额外追踪所有最终下游。

#### Per-Object ACK vs Cumulative ACK vs CompletionChannel

Cumulative ACK 语义（按**某条有向 Edge 自己的连续 Delivery Order**）：

~~~text
ACK through n
  ⇔ all original Data obligations with local orders <= n
     have reached FinalUseDone on this Edge
~~~

它不是“最大的已经完成 Seq”，不能跳过有未完成的早期 Order。

| Completion 机制 | 传递什么 | 保存中间 ACK | Queue Full | 乱序独立回收 |
|---|---|---|---|---|
| Reverse SPSC<Ack> | 每个对象/分支的 ACK 事件 | 是 | 可能 | 是 |
| Reverse SPSC<CumulativeAck> | 单调递增的安全水位事件 | 是，除非主动 Batch | 可能 | 仅连续前缀 |
| Atomic CompletionChannel | 最新累计完成水位 | **否，可覆盖旧水位** | **没有 ACK Queue 满** | 单一 Cursor 本身不支持 |
| Atomic Cursor + SPSC<SACK> | 连续水位 + 非连续例外完成 | 前缀合并；例外记录 | SACK 可能满 | 可结合 Owner-Local Completion Bitmap |

**关键类比**：CompletionChannel 在通信方向和生产/消费关系上类似“单槽、只保留最新值的 SPSC”，但**不是向 SPSC 发送空消息**：它保存一个有语义的单调数值，且没有可按条 Pop 的 Event Stream。旧 ACK 可以被覆盖的前提是 ACK(n) 已经蕴含所有较小 ACK 的完成语义。

#### CompletionChannel 的最小 C++20 接口

~~~cpp
#include <atomic>
#include <cstdint>

struct alignas(64) CompletionChannel {
    std::atomic<uint64_t> completed{0};
};

// 下游 Single Writer：仅在全部 <= safeThrough 的原始 Data
// 使用者都退出且本地完成状态可证明时发布；不允许递减。
void publishSafe(CompletionChannel& channel, uint64_t safeThrough) {
    channel.completed.store(safeThrough, std::memory_order_release);
}

// 上游 Single Reader：用于回收判定，且必须继续检查其它义务。
bool doneThrough(const CompletionChannel& channel, uint64_t required) {
    return channel.completed.load(std::memory_order_acquire) >= required;
}
~~~

**使用约束**：
- 只有一个发布线程；如果多个执行线程都完成任务，必须先汇聚到一个 Completion Owner，不能让它们随意 store 各自的 Max Seq。
- Cursor 单调递增；若同一 Edge 重启并重用 Order，需先切换独立 Generation / Stream Epoch，旧 ACK 绝不可跨新 Epoch 生效。
- 对于不同 Stream 的 Seq，不能直接比较大小；同一个 OS Thread 若处理三个 SPSC，通常需要三个 CompletionChannel。
- 同一 Cache Line 应避免混排别的线程频繁写入的 Hot Metadata。alignas(64) 仅是示例；硬件 destructive-interference size、成员布局和数组间距仍需在目标平台验证。
- Release/Acquire 解决的是有序的数据访问与同步，不能替代完整的生命周期登记。

对应的逻辑拓扑：

~~~text
            Forward SPSC<Handle>
P1 ------------------------------------> P2
P1 <------------------------------------ P2
      CompletionChannel: atomic<uint64_t>
      (only Latest Safe Completion)
~~~

P1 即使暂停 10ms，P2 仍可发布最新安全水位，不会发生 ACK Queue Full；**但 P1 不回收 Pool 时仍可能耗尽可用 Slot**。读取最新水位并不是无限内存或零成本的保证。

#### 为什么单调 Cursor 的非同时读取是安全的？

这是此方案相对分片 RefCount 的关键数学性质。

假设 C[r][s] **只增不减**，每次发布的值都是该 Reader 已经完成全部相关访问的真实前缀；同时订阅集合在扫描期间不变，并且没有事后追加比已宣告水位更早的义务。

Writer 在不同时间读入每个 Reader 的值，得到：

~~~text
observed[r] <= actualSafe[r] at/after that read
observedMin = min_r(observed[r])

则 observedMin 是所有目标 Reader
当时/此后真实安全前缀的保守下界。
~~~

因此，取到不同时刻的值**可能回收得晚，但不会凭空跳过未完成引用**。不需要一个同时读取全部 Cursor 的原子快照。这个推理不适用于可以从 0 增加到 1 的 Sharded RefCount。

C++ 同步链：

~~~text
Reader LastAccess(Data[source][seq])
    → Reader completed[source].store(seq, release)
    → Source Writer completed.load(acquire) 观察到 >= seq
    → Source Writer Overwrite old Data[source][seq]
~~~

Reader 对 Cursor 的 Release 必须在全部相关下游 Access 完成之后；Source Writer 的 Acquire 必须观察到足够新的有效完成水位；Forward Queue 本身还需要正确的 Publish/Consume 协议。若源码中允许一个 Reader 并发发布同一 Cursor 的较大和较小值，可能倒退；要由单一 Completion Owner 串行聚合。

#### 不要混淆 Cumulative ACK 与单独 ACK Event

Cumulative ACK(n) 意味着该逻辑 Edge 上此前所有已交付义务都已完成；它可以放在 Reverse SPSC<CumulativeAck> 中，也可以由一个 Atomic Latest-Value CompletionChannel 表示。后一种形式不是“向 SPSC 发空消息”：中间的 Cursor 值可以被覆盖，前提是完成进度严格单调且连续。

**Cursor 属于逻辑 Consumer Stream，不属于 Data 对象，也不一定属于 OS Thread。** 如果同一物理线程管理三条独立 Source 流，通常需要三套进度；Source/Edge 的 Local Sequence 不可随意混合比较。

---

### 7. Reader × Source Completion Matrix 与 Selective Watermark

#### 新方案：Reader × Source Completion Matrix

固定一组 Source Writer（每个 Source 拥有独立 Sequence 与原始 Data Pool），每个 Reader 按自身 Reader ID 为多个 Source 保存 Completion Cursor：

~~~text
                          Source / Writer
                       A           B           C
Reader R0           C[0][A]     C[0][B]     C[0][C]
Reader R1           C[1][A]     C[1][B]     C[1][C]
Reader R2           C[2][A]     C[2][B]     C[2][C]

Owner of row 0: R0  （只有 R0 更新）
Owner of row 1: R1
Owner of row 2: R2

Writer A scans column A → min over relevant Readers
Writer B scans column B → min over relevant Readers
Writer C scans column C → min over relevant Readers
~~~

定义：

~~~text
C[r][s] = Reader r 对 Source s 的 Contiguous Safe Completion Frontier

FullBroadcastSafe[s] = min { C[r][s] | r ∈ FixedSubscribers[s] }

CanOverwrite(source=s, oldSeq=x) =
    FullBroadcastSafe[s] >= x
    AND NoOtherOriginalPins/ArchiveObligations
    AND RetentionAllowsOverwrite
~~~

示例：

| Reader | Source A | Source B | Source C |
|---|---:|---:|---:|
| R0 | 100 | 50 | 80 |
| R1 | 98 | 55 | 90 |
| R2 | 105 | 52 | 85 |
| **Source Writer 可观察的最小完成水位** | **98** | **50** | **80** |

**必要前提**：
- 同一 Source 的 Sequence 严格单调，并且对每个相关 Reader 的完成顺序能表达为连续安全前缀；
- Reader 完成某 Source 的 Seq N，意味着该 Reader 在 N 及之前所有**已投递、仍需要它负责的**原始 Data 的使用均已结束；
- 对于固定全量广播，各 Reader 的 Frontier 可直接按 Source Global Seq 比较并取 min；
- 每个 Logical Reader-Source Stream 只有一个 Cursor Publisher；一个 OS Thread 可能处理多个流，而并发任务需要先聚合成连续完成前缀；
- 多级异步 Fork 必须由当前 Reader 对外提供真正的 **Transitive Safe Completion**；否则上游看到的只是本地 Forwarded，不是最终完成；
- 动态 Membership / Retention / Replay 的额外约束要单独加入。

**Source ID 不一定与 OS Thread ID 一一对应**：一个 Worker 可拥有多个 Source，Source 的稳定标识、Sequence Epoch、发布所有权才是关键。Reader ID 也应该代表稳定的 Logical Completion Owner；线程更换时需执行安全移交。

#### 最小 C++20 示例：Fixed Full-Broadcast Source Gating

下面代码展示完成状态与 Safe Frontier，不包含消息队列、Pool、Replay 与重配置；适用于**固定完整订阅且无漏报、无乱序最终使用**的基础模型：

~~~cpp
#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>

constexpr std::size_t kReaders = 3;
constexpr std::size_t kSources = 3;

// 一个 Reader 独占写自己的一行；alignas 确保行起点分离。
// 当单行大于 64 bytes 时可跨多条 Cache Line。
struct alignas(64) ReaderProgressRow {
    std::array<std::atomic<std::uint64_t>, kSources> completed{};
};

std::array<ReaderProgressRow, kReaders> progress{};

// r 的所有 Source-s 投递（到 safeThrough 为止）都最终使用完毕。
// 各 source 的 Seq 从 1 开始，completed 初始为 0。
void readerPublishSafe(std::size_t r, std::size_t s,
                       std::uint64_t safeThrough) {
    // 调用者保证：这是对该 (Reader,Source) 的单 Writer 单调发布。
    progress[r].completed[s].store(
        safeThrough, std::memory_order_release);
}

// Source s 的 Writer 独占调用；subscribed[r] 是稳定的全量订阅集。
// publishedThrough 用于处理无订阅读者时的上界。
std::uint64_t sourceSafeThrough(
    std::size_t s,
    const std::array<bool, kReaders>& subscribed,
    std::uint64_t publishedThrough) {

    std::uint64_t safe = publishedThrough;
    for (std::size_t r = 0; r < kReaders; ++r) {
        if (!subscribed[r]) continue;
        const auto c = progress[r].completed[s].load(
            std::memory_order_acquire);
        safe = std::min(safe, c);
    }
    return safe;
}

// 只有当 seq 已发布、所有该 Source 的目标 Reader 最终完成，
// 且其它 Replay/Retention/Pin 条件也通过时，才允许重用旧 Slot。
~~~

Source Writer 不必每发布一条消息就完整扫描 Reader。可缓存上次的 safeThrough，**仅当准备覆盖的 oldSeq 超过 cachedSafeThrough 时**再扫描。此缓存只可由 Source Owner 自己更新，缓存的值不允许大于实际已证明的安全前缀；还需处理首次启动、Ring Wrap、Reader Membership 变更。

#### Selective Fan-out：直接 min 会出现停滞

~~~text
Source A:
  Seq100 → R0
  Seq101 → R1
  Seq102 → R1
  Seq103 → R1

R0 已完成 Seq100，今后暂时没有新消息：
  C[R0][A] = 100

R1 已完成 Seq103：
  C[R1][A] = 103

naive min(100, 103) = 100
=> R0 永远不更新，就可能永远阻止 A 的累计回收前缀跨过 100。
~~~

这里的问题通常是**活性/回收效率**，不是过早回收。不能简单把 Reader 的最大曾完成 Seq 当作它已经确认过所有被跳过的消息。

改进选项：

| 方案 | 语义与条件 | 主要代价 |
|---|---|---|
| Full Broadcast + min | 每个订阅 Reader 按源完整消费 | 对稀疏 Fan-out 不经济 |
| Skip / No-Work Watermark | 明确宣布某 Reader 对某区间没有未完成义务 | 控制消息/同步协议 |
| Writer-Local Outstanding Tracking | Source Writer 自行登记每个目标实际已经承诺投递的范围与状态 | Owner Metadata、HOL / 进度映射 |
| Per-Data Target Metadata | 针对待回收对象只检查其实际目标 | Per-Data Target/RequiredOrder |
| Reverse Per-Object ACK / SACK | 精确报告某个 Data 或某个非连续范围最终完成 | 反向队列/位图/去重 |

**一个保守但可实现的优化**：Source Writer 本地维护 per-Reader lastDelivered[r]。当且仅当：

~~~text
completed[r][source] >= lastDelivered[r]
    AND 不存在仍等待 Publish/ACK 的旧使用义务
~~~

该 Reader 对目前**已经承诺的 Source 数据**没有任何 Outstanding 使用，可以在这次扫描中忽略它。若没有任何 Pending Reader，则当前源的已发布前缀都可以通过 Live Completion 条件（但仍需满足其它 Pin/Retention）。

否则对仍 Pending 的 Reader 做 min，安全但可能过度保守：

~~~text
ConservativeSafe[source] =
    min { C[r][source] | C[r][source] < lastDelivered[r]
                           or hasPendingPublicationOrOtherUse[r] }

若集合为空：LiveSafe = publishedThrough
~~~

**注意不能由上游观察一个 Reader 的 lastDelivered 就猜它已收到消息**：必须由 Source Writer 登记已成功 Publish 或已预留、已承诺的异步投递。Queue Full、部分 Fan-out、取消、异步回调仍引用旧 Data 都属于未完结义务。Source Writer 扫描期间不能与其它线程随意修改该 Source 的投递/订阅表。

上面的方法仅提供**保守的 Prefix Reclamation**。例如 R0 已完成 100、还在处理 103，Source Seq101/102 根本没投递给 R0，naive min 仍可能因 R0 的 100 卡住 101/102；要独立释放这两个 Slot，仍需 Per-Data Target Metadata 或更强的 Skip/ACK 状态。对于 Fixed Ring，连续安全前缀的 HOL 问题可能是必要代价；对于 Free Slot Pool，可以考虑单对象精确回收。

#### Cache Line Layout：Reader-Major vs Source-Major

建议第一版采用**Reader-Major Layout**：

~~~text
Reader R0 cache line(s): C[R0][A], C[R0][B], C[R0][C] ...
Reader R1 cache line(s): C[R1][A], C[R1][B], C[R1][C] ...
Reader R2 cache line(s): C[R2][A], C[R2][B], C[R2][C] ...
~~~

每个 Reader 只写自己的行，因此不同 Reader 不会写同一行。但 Source A、B、C Writer 分别读取同一行中的不同字段，R0 修改 C[R0][A] 时可能令正在读取 C[R0][B] 的 Writer B 也失去该 Cache Line 的共享副本——**跨 Source 的 False Sharing / 读取干扰**并未完全消失。

对比：

| Layout | 优点 | 主要问题 |
|---|---|---|
| Reader-Major，Per-Row 64B Alignment | Reader 局部写入、Metadata 较紧凑 | 不同 Source Writer 的读取可能被同一 Reader 行的写入连带失效 |
| 每 (Reader,Source) 独立 Cache Line | 隔离每个 Cell | O(R×S×CacheLineSize) 空间、TLB/Cache Footprint |
| Source-Major | Source Writer 连续扫描本列 | 不同 Reader 的 Store 容易落入相同 Cache Line 造成写写争用 |
| Per-Source Group / Hierarchical Aggregation | 减少超大量 Reader 的扫描 | 增加中间聚合与进度传播延迟 |

初始版本用 64B Row Alignment 即可；要根据目标 CPU 的 Coherence Block Size、Reader Source 数量和 perf c2c/HITM 结果决定是否进一步 Padding。一次 Atomic Release Store 通常比跨 Core Locked RMW 轻，但**仍可能引起 Cache Line Invalidations**。

#### Matrix 最自然的适用范围

每个 Reader 独占写自己对不同 Source 的一行 Cursor；每个 Source Writer 扫描本 Source 的列，取相关 Reader 的最小**真实安全完成水位**。这可以避免 Reader 之间对相同 Atomic Counter 的 RMW 争用；缓存的最小水位可以减少 Writer 扫描次数。

但当 R0 只收到 Seq100，R1 收到 Seq101–103 时，R0 的水位停在 100 不能说明其应该阻止 101–103 回收。需要 Skip Watermark、Per-Data Target Requirement 或 Writer-Local Outstanding Tracking。矩阵只表达已证明的安全水位，不自动表达完整 DAG 依赖。

---

### 8. Hierarchical ACK：Owner-Local Dependencies、Parent 与 Child

#### 基本拓扑：每条 Pipe 一对方向相反的 SPSC

~~~text
                   Forward SPSC<DataHandle>
P1 [owns Data1]  --------------------------> P2 [owns Data2]
P1 [recycles D1] <--------------------------  P2 [ACK Data1]
                   Reverse SPSC<Ack>

P2 [owns Data2]  --------------------------> P3
P2 [recycles D2] <--------------------------  P3 [ACK Data2]
                   Forward / Reverse SPSC

P2 [owns Data2]  --------------------------> P4
P2 [recycles D2] <--------------------------  P4 [ACK Data2]
~~~

示例：P1 创建 Data1；P2 接收 Data1，创建 Data2，Data2 里可能包含指向 Data1 的原始引用；P2 将 Data2 发给 P3、P4；P3/P4 用完 Data2 及其可达的 Data1 后分别回传 ACK；P2 收齐这两个 ACK、确认自己已经结束使用、且没有其他子对象/回调访问 Data1，才回收 Data2 并 ACK Data1 给 P1。**这里传播的是原始 Payload 的最终使用结束证明，不是“Queue 已 Pop”或“Handler 已 Forward”。**

不变量：

~~~text
OwnerAllowsReuse(x)
  ⇔ OwnerLocalDone(x)
     AND PublicationClosed(x)
     AND AllDownstreamObligationsDone(x)
     AND NoOtherOriginalAccess(x)
     AND ReplayAndRetentionConditionsAllowReuse(x)
~~~

如果 Data2 引用了 Data1，即使 Data2 自身的处理函数已返回，**Data1 的生命期仍必须覆盖 Data2 的所有可能使用阶段**。Data1 如果另发给 P5，P1 也必须等待 P5 分支；P2 的 ACK 只解除 P2 这一条已登记的依赖。

#### Owner-Local Pending Count 不等于 Shared Atomic RefCount

各 Owner 独占修改自己的 Pending / FreePool，因此它们可以是普通整数与预分配数组；其它线程**不得**直接访问和修改这些非原子字段：

~~~cpp
struct Handle {
    uint32_t slot{};
    uint64_t generation{};
};

struct Ack {
    Handle handle{};
    uint32_t sourceEdge{};    // 分辨实际下游分支，防重复/伪造 ACK
    uint64_t deliveryOrder{}; // 可选：用于与该 Edge 的累计进度关联
};

struct ChildState {
    Handle child;
    Handle parent;
    uint32_t pendingTargets;  // 只允许 Owner Thread 改写
    bool localDone;
    bool publicationClosed;
};

struct ParentState {
    Handle parent;
    uint32_t outstandingChildren; // 可包含多个 Data2 / 异步依赖
    bool localDone;
    bool publicationClosed;
};
~~~

状态与顺序规则：

1. Owner 在可被下游观察之前，创建并登记**全部相关投递义务**，或预留能保持生命期的 Publication Token；不能在最后一个 ACK 已经到达后再增加尚未登记的依赖。
2. 发送消息成功才代表某个 Handle 的接收权发生转移；Queue Full 必须保留待发送的生命周期义务，不能把未送达当成已完成。
3. 每个有效 ACK 必须精确匹配当前 Outstanding 的 (Owner, Slot, Generation, Edge, Delivery Order)，并确保**每条义务仅完成一次**。
4. Child 的 localDone + publicationClosed + 所有下游 ACK 后，才算该 Child 的最终使用结束；再解除相应 ParentState.outstandingChildren。
5. Parent 的本地使用结束、PublicationClosed、全部子依赖结束后，才能向上游 ACK 原始 Data1。
6. 如果 P2 存在多个 Data2 都引用 Data1，不能因第一个 Data2 完成就发送 Data1 ACK。
7. 同样的 Data 被 P2、P3 并行访问时，不得将 Data2 的本地完成与下游完成混为一谈。

用 **有向无环生命周期依赖**更容易证明正确；出现循环依赖时，即使所有线程都活着，也可能永远收不到最终 ACK。

#### 内存可见性：逐级 Release → Acquire 构成传递链

假设 Forward/ACK SPSC 对元素 Publish 使用 Release、对元素 Pop 使用 Acquire；ACK 只能在真实最后一次 Payload 读取后发出：

~~~text
P1:
    Write Data1
    → Forward SPSC release-publish
P2:
    Forward SPSC acquire-pop
    → Create Data2 referencing Data1
    → Forward P3/P4 release-publish
P3/P4:
    Forward acquire-pop → read Data2 / Data1
    → ACK release-push
P2:
    ACK acquire-pop from BOTH edges
    + LocalDone + PublicationClosed
    → ACK Data1 release-push
P1:
    ACK Data1 acquire-pop
    → Reuse Data1 (only after other obligations)
~~~

因此在所有引用路径被正确登记时：

~~~text
P3/P4 last Data1 read
  happens-before
P2 acknowledge Data1
  happens-before
P1 overwrite old Data1
~~~

多个下游 ACK 的读取都必须发生在 P2 的上游 ACK 发布之前。没有收到某个必要 ACK 时，不能依靠超时或者 Reader 从 Active Set 删除就宣布完成。Reader Crash/Stop 需要另行定义安全取消或隔离，不允许继续潜在旧引用的情况下复用 Pool。

#### 多级 + Selective Fan-out：不要对不同 Seq 域简单取 min()

~~~text
P1 edge → P2:  order 100, 101, 102

P2 → P3 edge:  order 200 (parent100), 201 (parent101)
P2 → P4 edge:  order 80  (parent100), 81  (parent102)
~~~

如果 P3 的安全水位为 201，P4 为 80：
- P2 可以确认 parent100 对两个下游的义务完成。
- parent101 对 P3 已完成；若 P2 本地及其他依赖结束也可标记完成。
- parent102 对 P4 尚未完成。
- 向 P1 发送的累计安全 ACK 只能到 parent101（若 100 和 101 的全部其它依赖均完成）。

不能写成 min(201,80)，因为 P3/P4 的本地 Order 域不同。P2 需要 Owner-Local Requirement Table，例如：

~~~cpp
struct Dependency {
    Handle parent;
    uint64_t parentOrder;
    // 每个实际下游 Edge 的 requiredLocalOrder 与受理标志；
    // 对没有该依赖的 Edge 不生成 ACK 义务。
};
~~~

Owner 可以：
1. 在给下游发 Handle 时记下该 Edge 的 RequiredOrder；
2. 从该 Edge 的 CompletionChannel acquire-load 最新连续 Safe Cursor；
3. 满足全部相关义务后，标记具体 Parent/Child 已完成；
4. 维护 **自己对上游 Edge 的连续 Safe Frontier**；
5. 向上游 CompletionChannel release-store 新 Safe Frontier。

一种简单的 Owner-Local 实现使用 Completed Bitset / Reorder Buffer 跟踪已完成但处于 Frontier 之后的条目；若 Fixed Pool 允许乱序回收，仍可以先回收已完成的本地 Data2，但向 P1 的累计 ACK 只能连续推进。

#### 为什么还需要 Reverse SPSC？什么时候混合使用？

**只有连续累计确认需求时**，反向 ACK SPSC 可直接由一个 Atomic CompletionChannel 替代，不必同时维护两套回传机制。

但下面几种场景需要 Event ACK：

- 完成可乱序且上游希望立即回收非连续的特定 Slot；
- 下游可能 Cancel/Reject，必须报告独立的完成/失败语义；
- 需要传递异常、资源归还、Debug Trace、明确的终止 Handshake；
- Owner 需要区分具体子任务、Fork、Refcount 归还等明细。

可混合：

~~~text
Forward plane:
    SPSC<Handle>

Reverse fast path:
    Atomic Cumulative CompletionChannel

Reverse exception path:
    SPSC<SelectiveAck / Cancel / Error>
~~~

但这不是简单叠加：**同一个 Outstanding Obligation 不能同时被 Cursor 覆盖与 SelectiveAck 再次计数**。Owner 必须有去重状态（Generation + Edge + LocalOrder / Acked Bit），对反向两个通道做幂等合并。

如果每个 Data 完成顺序基本严格 FIFO，Cumulative Cursor 更简洁；若完成大量乱序，额外 SACK 状态与 Owner Bitmap 的成本可能超过直接的 Per-Object ACK Queue。

#### ACK 聚合与发布频率：Batch、Idle Flush、Pressure Flush

示意：下游每秒连续完成 1,000,000 条，理想情况下按 B 条合并一次 Cursor Publication：

| Batch B | 约需发布的累计水位/s |
|---|---:|
| 1 | 1,000,000 |
| 8 | 125,000 |
| 32 | 31,250 |
| 64 | 15,625 |

这是**发布次数**的示例，不是实际延迟/CPU 开销按同比例降低的实测结论。

缺点：Batch 越大，最早完成的 Data 可被上游识别为已完成的时间越长，Owner Pool 可能需要更多 Buffer。最末尾不足 B 的 Completed 如果永远没有新的消息，可能永远不报告：

~~~text
Flush Conditions:
  - Completed advanced by >= B;
  - Idle / timeout / cooperative polling checkpoint;
  - Upstream Pool low-watermark (有反向压力反馈协议时);
  - Drain / Shutdown / Reconfiguration.
~~~

时间触发不一定需要 OS Timer 或 Syscall，可以在 Polling Loop 中检查本地 TSC/时间预算；具体频率与准确性应量化。若上游仅被动读取 Atomic Completion，没有 Pressure Signal，**下游无法自动感知上游 Free Pool 水位**，就必须依赖 Batch 上限、周期 Flush、或另外加控制信号。

Batch 的初衷是减少下游 Store 引起的跨核 Cache Coherence；P1 Acquire Load 仍有成本，但通常不需要 Reader 对每条 Data 执行共享 RefCount 的 Atomic RMW。

#### Forward / Reverse Queue Full 与 Deadlock

两条 SPSC 不是自动 Deadlock-Free。典型等待环：

~~~text
P2 等待 P3 的 Forward SPSC 出现空位
P3 等待反向 ACK SPSC 出现空位
P2 正忙于 Spin 等 Forward，无法 Drain ACK
⇒ 两线程互等，永久停顿
~~~

**Polling Owner 必须同时推进 Forward、Reverse、Reclamation、Control Plane**，不能无限等某条 Queue：

~~~cpp
void ownerPollIteration() {
    drainAcksBounded();         // 接收下游最终完成
    refreshCursorsBounded();    // 累计进度
    reclaimReadyBounded();      // 回收已安全 Data
    retryForwardBounded();      // Pending Publication
    processInputBounded();      // 新数据
    processControlBounded();    // 动态订阅 / Drain / Cancel
}
~~~

这是调度结构示意，不是可直接编译完整代码。对于 Cumulative CompletionChannel，反向水位不会 Queue Full，能够减少一个等待环来源；但 **Forward Queue Full、Pool Exhaustion、未响应 Reader、Pending Publication、Control Event Starvation** 仍需处理。

#### 由 Owner 消费下游完成并向上游确认

~~~text
P1 owns Data1 → P2 owns Data2 (Data2 references Data1)
                    ├─ P3 reads Data2/Data1 → ACK
                    └─ P4 reads Data2/Data1 → ACK
P2: all ACKs + own last use + publicationClosed
    → reclaim Data2 → ACK the outstanding Data1 obligation to P1
P1: all original Data1 obligations done + Replay/Retention safe → reuse
~~~

Owner 本地 Dependency Count 可以是普通整数，因为只有 Owner Thread 修改；其他 Reader 不直接访问它，只通过 Completion Event 或独占的 Cursor 发布。在多个 Data2 引用同一个 Data1 时，P2 不得因某个 Child 完成就提前 ACK Parent。对已经完成的 Edge 不能因 SACK 和 Cursor 都覆盖而重复计数。

此协议的优势是对象级乱序回收与局部状态所有权；代价是额外 ACK 流量、Pending Graph、异步 Queue Full / Deadlock 风险。它不自动比优化后的 Intrusive RefCount 更快。

---

### 9. 级联完成的 Safety、Liveness 与 Hot-Path 隔离

#### ACK vs Cursor：事件通知与单调进度状态

| 维度 | Reverse SPSC Per-Object ACK | Cumulative ACK / CompletionChannel |
|---|---|---|
| 完成表达 | 每对象/每 Edge 的终止事件 | 连续完成前缀 Watermark |
| Writer 得到信息 | Drain ACK Queue | 按需 Acquire-Load 最新 Cursor |
| 乱序完成 | 天然支持独立回收 | 需要 SACK / Bitmap / Per-Object State |
| 负载 | 每对象通常有 ACK，可 Batch | 只在 Watermark 前进时发布，可 Batch |
| Backpressure | 反向 ACK Queue 可以满 | Atomic Cursor 不会 Queue Full |
| 级联 | ACK 到达 → 解除依赖 → 向上级 ACK | 检查下级 Safe → 聚合 → 发布上级 Safe |
| 进度延迟 | ACK 未被消费时不会推进回收 | Cursor 未被观察时不会推进回收 |

**ACK 更主动只是默认架构习惯，不是绝对优势**：ACK 进入 SPSC 并不自动触发上游处理；Cursor 更新也不自动触发上游 Load。若下游每条完成都发布最新真实安全 Cursor，且上游及时读取，Cursor 不必天然比 ACK 晚。

真正差别是 **Per-Object Final Done** vs **Contiguous Prefix Final Done**。如果 Seq100 未完成，101..200 已完成，ACK 可单独释放 101..200（取决于 Pool），纯 Cursor 最多到 99，属于 Head-of-Line Blocking。若采用固定 seq%Capacity Ring，独立完成也未必能立刻重用被 Seq100 占据的具体 Slot。

#### 级联 FIFO：Safety 成立却不保证 Liveness

在 P1 → P2 → P3 → P4 的固定、同 Sequence、完整 FIFO 生命周期模型：

~~~text
P4.localDone=100; P4.safe=100
P3.localDone=100; P3.safe=60  (没有新 Data，未读取 P4.safe)
P2.localDone=100; P2.safe=60  (没有新 Data，未读取 P3.safe)
P1 读取 P2.safe=60，无法释放 61..100
~~~

P1 读取 P2.safe 并不会迫使 P2 去读 P3；P2 如果只在自己需要回收、或接收到新 Forward Data 时更新，可能长时间没有进度。这不一定导致数据竞争，但会产生 Reclamation Stall / Pool Exhaustion。

必须独立保证：

- **Safety**：P2 发布 safeCompleted=N，说明 N 及之前所有 P2 直接或间接负责的旧 Data 引用全部停止；
- **Forward Progress（在公平调度、下游可运行的前提下）**：当所有必要下游最终完成，上游的安全完成水位最终会推进。

只有 **相同、完整、连续的 Seq 域，且没有额外未追踪引用** 时，才能写：

~~~text
P4.safe = P4.localDone
P3.safe = min(P3.localDone, P4.safe)
P2.safe = min(P2.localDone, P3.safe)
P1.reclaimSafe = P2.safe
~~~

如果 P2/P3/P4 的 Local Order 不同或 Selective Fan-out，各级必须将子流完成义务映射回父流，再生成父流的 Safe Frontier，不能比较裸 Seq。

#### 级联的三种 Progress Propagation 策略

**A. P1 直接读相关末端的 Cursor**：

~~~text
P1 → P2 → P3 → P4
P1 查询 P2/P3/P4 各自的 LocalFinalDone（同 Sequence）
P1 计算 min
~~~

优点：省掉逐级传播等待；缺点：P1 需要知道所有 Stage 及生命周期依赖，并支付多 Core Cursor Loads。若每层独占 Move Handle，且 P4 的最终完成与前级最后访问之间已有完整 Happens-Before 链，可能只读 P4。但不能默认每条 Pipeline 都成立。

**B. 每一级主动、非阻塞发布 Transitive Completion**（推荐给固定 FIFO 级联）：

~~~text
P4 last use → publish safeCompleted
P3 polling checks P4.safe → publish P3.safe
P2 polling checks P3.safe → publish P2.safe
P1 checks P2.safe without waiting for new requests
~~~

Polling Loop **即使没有新 Forward Data 也必须有机会刷新下游 Safe**。可以在处理完数据后顺带计算安全水位；在无输入时执行 Idle Flush / Periodic Check，避免最后一批水位永久停留。只在安全水位变化时 Release-Store；高吞吐时 Batch，但要求 Idle、Drain、Shutdown 能 Flush。

~~~cpp
// 示意：处理固定同源 FIFO 的中间 Pipeline，非完整可编译组件。
void stagePoll() {
    drainDownstreamAcksBounded();
    processForwardBatchBounded();  // 本地处理后推进 localDone

    // 无新 Forward Data 时，同样要走到这里
    const auto downstream =
        downstreamSafe.load(std::memory_order_acquire);
    const auto safe = std::min(localDone, downstream);

    if (safe > lastPublishedSafe && flushPolicy(safe)) {
        upstreamSafe.store(safe, std::memory_order_release);
        lastPublishedSafe = safe;
    }

    retryForwardBounded();
    processControlBounded();
}
~~~

注意：如果 localDone 的含义只是 Forwarded，转发后 P2 仍会访问 Data，则它**不能**用于该 min 安全表达。每级发布的是对上游原始 Data 的真实最终完成；推导要包含 Parent-Child Dependencies。

**C. Demand-Driven Progress Request**：

~~~text
P1 Pool Low → request P2 → request P3 → request P4
P4 完成状态 → P3 → P2 → P1
~~~

如果 P1 等待整条链响应，多个 Polling Scheduling / Round-Trip 都会进入 **P1 Hot Path 的 p99.9**。异步、提前的 Request 可以减少直接阻塞，但增加通信协议、调度逻辑、饥饿与丢失唤醒风险。Pool 满时不能一边等待请求/ACK 一边阻止处理其他 Queue/Control，否则可能形成等待环。

#### 回收与写入解耦：不在 Pool Exhaustion 时才开始

推荐设计：P1 Fast Path 先取已有 Free Slot → Write/Publish；各 Owner 在其 Polling Loop 内单独设置预算 Drain Completion、更新 Frontier、Reclaim。使用 Free-Slot Low-Watermark 提升回收频率，但不把跨级 Progress Request 作为写入必须等待的同步 RPC。

~~~text
RequiredHeadroom ≳ PeakArrivalRate × CompletionPropagationLag + BurstMargin
~~~

这是保守容量规划的数量级公式，并非严格上界。对任意慢 Reader，有限 Pool、永不阻塞、无丢失、无穷时间运行不可能同时保证。

测试至少区分：
- ProcessingLatency：真正完成业务访问的时间；
- PropagationLatency：最后使用完成 → Owner 知道全部下游完成；
- ReclamationLag：最后使用完成 → Slot 可再次分配。

此外测 Writer p50/p99.9/max、Free Slot Low Watermark、Pending Graph 大小、Completion Store/Load 次数、Cache HITM 与 Batch Idle Flush。

#### Reader × Source Matrix：更适合扁平 Fan-out，级联需额外协议

**扁平模型**：

~~~text
Source A ──→ R0, R1, R2
Source B ──→ R0, R2
每个 Reader 写 C[Reader][Source]
每个 Source Writer 扫自己列的安全最小水位
~~~

没有中间级 Transitive Completion Propagation，模型简洁；尤其适合固定 Source / Reader、完整有序流。

**级联模型**：

~~~text
P1 Source A → P2 ReaderA / Source B → P3 ReaderB / Source C → P4
  ↑                     ↑                    ↑
  只在 P2 的所有 Data1 派生引用结束后 C[P2][A] 才可前进
                        P3/P4 子流的完成还需映射回父流
~~~

Matrix **并非不能级联**，但中间级必须另外维护父子对象依赖、不同 Source 的 Seq Mapping、Source-Local Pending、主动 Propagation、Idle Flush 和动态 Reader Membership。矩阵记录“完成水位”，不能自动描述复杂 DAG 的生命周期拓扑。

**架构定位**：

| 场景 | 优先模型 | 主要理由 |
|---|---|---|
| 多 Source → 固定 Reader，扁平完整 FIFO | Reader × Source Completion Matrix | 每 Reader 独占写，Source 扫完成水位 |
| P1→P2→P3→P4，固定 FIFO 同 Seq | 主动级联 Cumulative Cursor，或 P1 直接读末端 | Batch，避免每对象 ACK |
| P1→P2→P3/P4，Data2 引 Data1，多级 Selective Fan-out | Owner-Local Dependencies + Hierarchical ACK | 事件驱动解除子依赖，支持乱序 |
| 大多数 FIFO、少数异步乱序 | Cumulative Cursor + SACK/Completion Bitmap | 常规批量，例外单对象 |
| 任意共享引用 / 动态 Fork | 优化 Intrusive Atomic RefCount | 清晰的 Owner Fork/Final Release |
| Sharded Mutable RefCounts 且任意转移 | 仅在严格 Closing/Validation 协议下考虑 | seq_cst 全零扫描不成立 |

#### Selective Fan-out：为什么简单 min() 常常错误？

~~~text
Global Seq       100       101       102
P2 收到          yes       yes       yes
P3 收到          yes       no        yes
~~~

P3 的 Global Seq 完成进度可能永远不会等于 101；若跳过 101，直接按连续 Global Seq 用 min() 则可能永不前进，反过来用“最大完成 Seq”又会错误跳过尚未完成的消息。

可选协议：

1. **Per-Consumer Local Delivery Order**：P3 收到的两条消息编为 order 1、2；P2 保存每个原始消息关联的 P3 requiredLocalOrder，在对应义务完成时才能推进自己的连续 Safe Frontier。
2. **显式 Skip / No-Work Watermark**：所有 Consumer 都在同一 Global Sequence 域推进，包括确认无工作、过滤掉的 Seq；代价是额外进度协议。
3. **Per-Object Completion / RefCount**：直接记录每个 Data 的未完成分支，允许独立回收；动态 Fork/乱序工作流往往更自然。
4. **Completion Event / Ack Queue**：下游最终使用后发回 (slot,generation,order)；上游维护乱序完成集合或归还空闲 Slot，而不必强迫连续 Cursor。

当 P3 只收到部分消息时，P2 的 SafeCompleted **应是对 P2 自己的本地输入流的连续安全前缀**，并结合自己投递到各下游的映射判断前缀，不能简单比较不同流里的整数。

#### 对固定 FIFO 的关键实现不变量

在 P1→P2→P3→P4 级联中，P1 读取 P2.safeCompleted 不会自动迫使 P2 读取 P3。只在有新 Forward Message 时刷新，下游最后一批处理完后可能出现 **Safety 正确但进度永不传播**。

推荐各级主动且非阻塞地检查下游 Safe，**即使没有新输入，也执行 Idle Check / Flush**；仅当真实安全水位变化时 Release-Store。不要让 P1 在 Pool 耗尽时才同步发起多跳 Progress Request 并等待回复：这种设计会把多级 Polling Delay 引入 Writer 的尾延迟。

**特殊简化**：如果固定同 Sequence、全部消息严格按序经过每级，且下游最终完成能证明前级已经结束访问，P1 可以直接 Gate 在末级 Cursor；如果前级仍 Borrow 或存在多支线，必须检查所有相关完成义务。

---

## Part III · 拓扑、Replay 与特殊回收

### 10. Selective Fan-out、Multicast 与动态 Reader

#### Multiple SPSC + Shared Pool：如何证明可复用？

假设单 Writer、每个 Reader 只通过自己的 FIFO SPSC 获取对象、Reader 按连续顺序处理并在最后一次访问后发布 completed：

~~~text
Data x:
    targets = {R1, R3}
    requiredOrder[R1] = 105
    requiredOrder[R3] = 88

LiveDone(x) =
    completed[R1] >= 105
    AND completed[R3] >= 88
~~~

如果 Reader 把任务再次异步分发出去并乱序完成，最大已完成 Sequence **不代表连续完成**；需要 Contiguous Frontier / Completion Bitmap 或明确禁止异步指针逃逸。

**部分投递**必须处理：

~~~text
Push(R1, x) = success
Push(R3, x) = queue full
~~~

R1 已收到，无法悄悄回滚。可选：预留全部目标 Queue 容量、可恢复的 Pending Publication、或者业务明确规定部分交付/失败处理。不要无限 Spin 等 R3 释放 Queue 却饿死 Writer 的 RemoveReader Control Event。

#### 动态 Reader：Single-Owner Event-Driven Membership

优先 **Static Physical Topology + Dynamic Logical Membership**：固定 CPU-Pinned Workers / Queue，动态修改 Instrument Subscription、Task Routing、Session。运行期频繁改的是逻辑订阅，而未必是物理线程数；这是工程经验判断，不是行业数量统计。

~~~text
Management / Cold Path
    AddReader / RemoveReader / UpdateSubscription / UpdateRouting
      ↓
Control Queue (SPSC 或 MPSC，取决于控制 Producer 数量)
      ↓
Single Writer / Registry Owner
      ↓
在确定的消息边界应用变更，返回 effectiveSeq
~~~

推荐状态：CREATED → STARTING → ACTIVE → DRAINING / CANCELLING → STOPPED → RECLAIMED。

| 生命周期操作 | Multiple SPSC Selective | Multicast Ring |
|---|---|---|
| Add | SPSC/Worker 已 Ready，加入 Active Targets | 初始化 startSeq/completed，**先加入 Gating**，再允许接收后续 Seq |
| Remove 生效 | 不再投递新消息，旧 Retired Requirements 保留 | 设置 stopAt Seq，Reader 继续完成旧消息 |
| Drain | 消费到对应本地完成序号 | 完成 stopAt，确认不再访问 Ring |
| Reclaim ReaderState | 没有 Pending 指向旧 ReaderState | 先安全移出 Gating，再管理线程退出与资源回收 |
| Reader ID 复用 | Stable ReaderState + Generation | Stable Gating Record + Generation |

**Remove Membership 并不表示旧 Reader 已不持有 Data**。取消未处理消息可以由业务允许，但绝不能忽略 Reader 正在使用的 Slot；一个被 OS 抢占的 Reader 可能暂时无法安全移除。不能用当前 Active Mask 回收历史消息，而要保留发布时确定的旧义务。

广播 Fixed Ring 的回收条件（Seq 从 1 开始，容量 C，全部相关 Reader 连续按序完成）：

~~~text
SafeToPublish(nextSeq) =
    nextSeq <= C
    OR min(completed[gatingReaders]) >= nextSeq - C
~~~

Writer 可以缓存最慢 Reader 的 Completed 并仅在容量临界时重查，**AddReader 必须更新 Gating 与可能缓存的边界**。History Join 要在加入保护协议前验证保留窗口，不能从已覆盖的 Seq 开始读。

| 技术 | 加入 | 离开 | 常见做法 |
|---|---|---|---|
| Atomic shared_ptr | 无需注册 | 最后一个 owner 释放 | atomic shared ownership |
| HP | 先注册 Hazard Record，再读取 | 清 Hazard，归还 Slot | 预分配或稳定 Record Pool |
| EBR | 注册 inactive Participant，后 pin | unpin、转移 retired list、注销 | 固定 Slot / Registry 锁 |
| QSBR | register → online | quiescent/offline → unregister | 固定容量 Slot Pool + generation |

注意：Reader A 离开后立刻把相同 ID/Slot 给 Reader B，如果 Writer 手里的旧 Reader Record 还在等待，必须建立 generation / membership handoff；不能把 B 的新进度误用作 A 的已完成证明。

一个实用实现是 **慢路径用 mutex 管理 Registry，热路径完全不加锁**。Reclaimer 可在锁内快照稳定的 ReaderState 指针，必要时为 metadata 设计单独的生命周期。不要持 Registry 锁长时间等待某个 Reader 的 grace period。线程异常退出时不能强制把它标为 offline 并立刻回收潜在仍被访问的对象。

#### 动态 Reader Membership、Generation、Recovery

动态订阅必须由 Source Owner 在确定的 Publication Boundary 管理：

- Add Reader：先建立 ReaderState、Stream Generation、Completion 初始值以及后续订阅起点，再允许获得数据。对只处理加入后的消息，可把 Cursor 初始化为加入起点前最后一个 Source Seq；要读历史则必须另有 Catch-up / Replay 保护。
- Remove Reader：只终止**未来**投递；已经交付或承诺的旧消息仍需 Drain 或安全 Cancel/证明不再访问；不能立即从 min 集合删除导致旧 Slot 过早复用。
- Reuse Reader ID：必须有 Generation / Epoch，旧 Cursor 不能被误解为新 Reader 的完成；Source ID / Seq 重启同理。
- Reader Stall/Crash：不凭超时就跳过；需安全的停机、撤销访问权或复制/隔离旧数据，以及定义 Pool Exhaustion Policy。
- Multiple Processes / IPC：共享 Atomic 的布局、地址映射与平台保证需单独验证；本文简单 C++ 代码只讨论单进程线程间通信。
- Async Data Escape：任何层级仍能访问原始数据的 Callback、Queue、Replay Pin，都必须纳入 Completion 协议。

#### 不同拓扑不是同一个队列问题

| 模型 | 每条消息由谁接收 | 如何回收 |
|---|---|---|
| 单 Producer / 单 Consumer SPSC | 唯一接收者 | Move Ownership 或 ACK |
| Work Queue / MPMC | N 个 Worker 中恰好一个 | 单任务完成，不是广播 |
| Multicast Ring | 所有订阅 Reader | 统一 Gating Cursor |
| Multiple SPSC Selective Fan-out | 目标集合内多个 Reader | Per-Target Required Order / ACK |
| Multi-Stage Derived DAG | 新子对象引用旧父对象 | Parent-Child Dependency Closure |

对应可运行/完整的教学示例在附录 A–D。所有 Fan-out 分支的生命期义务必须先登记，不能在部分 Push 成功后因另一个队列满而提前复用 Slot。

---

### 11. Replay / Archive 与 HP、EBR、QSBR、CAS Pin 的边界

#### Recent Replay + Segment Archive：两层职责

~~~text
Live Processing:     Multiple SPSC / Multicast
Original Storage:    Fixed Pool / Ring + Lifecycle
Recent Replay:       Generation + CAS Pin
Archive Handoff:     Copy ACK / Transfer Ownership
Historical Replay:   Immutable SEALED Segment Index
Durable Recovery:    可选的 File / Journal / Storage
~~~

对连续 Seq、固定记录大小，Segment Lookup 可使用：

~~~cpp
segmentId = (seq - 1) / SEG_SIZE;
offset    = (seq - 1) % SEG_SIZE;
~~~

SEALED 表示不再修改，**不是 Durably Persisted**。Segment 的 WRITING、SEALED、COPYING、ARCHIVED 描述构造/迁移；EVICTABLE 另须满足 Retention 与 Reader 保护全部结束。最新 WRITING Tail 可选 Hot Replay Fallback、Partial Seal、或另行证明的 Tail Protocol。

在需要完整无损归档的配置下：

~~~text
SafeToReuseOriginal(x) =
    LiveDone(x)
    AND NoOutstandingOriginalReplayPin(x)
    AND ArchiveOwnsIndependentSafeCopy(x)
    AND OtherOriginalRetentionRequirementsSatisfied(x)
~~~

Archive Copy ACK 不等于 Durable Commit。History Reader 赶上 Live 时，需要在 Writer 顺序点取得明确切换边界，处理历史与实时的 Gap、Overlap、Duplicate；不能简单“先 Replay 完，再注册 Live”。

按 seq 切分 immutable Segment：WRITING → SEALED → COPYING / ARCHIVED → EVICTABLE。Reader 只访问已经 SEALED 的 Segment；Cold Lookup 用 seq→segment+offset；可以基于 segment 粒度进行 Pin、shared_ptr 或 service-owner 串行查找。

Segment 的优势：批量 copy、批量回收、Cold Path index 管理简单，降低每消息 metadata 成本；劣势：单个慢消息或迟迟不完整的 Segment 能拖住整个 Segment 的回收，且需要显式管理最近尚未 archive 的 tail。

可以考虑的 Hybrid：

~~~text
Live Delivery   : Per-Reader SPSC / Selective Fan-out
Original Memory : Fixed Pool + Writer-owned Reclamation
Recent Replay   : Stable Slot Gen+Ref CAS Pin（可按需启用）
Archive         : Replay Copy Worker + Immutable Segments
Cold Lookup     : Archive owner / independent Segment lifetime
~~~

注意：**在保留 CAS Pin 的同时启用 Copy Worker** 是可以渐进迁移的：先让 Cold Lookup 优先读 Archive，在 newest tail 仍走受 CAS 保护的原始 Slot；确认全量迁移后才能删除原始 Slot CAS 机制。不能在新旧两种路径共存时贸然改用普通 store。

| 维度 | RefCount | Hazard Pointer | EBR | QSBR |
|---|---|---|---|---|
| Reader 保护单位 | 单对象共享所有权 | 单个具体对象指针 | 一个受保护的 epoch 临界区 | 经过一次 quiescent 之前的所有读取 |
| 读侧 RMW | 通常有 | 不一定；可能需要 fence | 通常不需要 | 一般只在 checkpoint 报告 |
| Reader 动态增减 | 天然支持 | Record 注册/清空/复用 | Participant 注册/unpin/注销 | register/online/offline/unregister |
| 慢 Reader 阻塞回收 | 主要阻塞被持有对象 | 主要阻塞 Hazard 指向对象 | 可阻塞整个 domain | 可阻塞整个 domain |
| 非侵入性 | shared_ptr 可非侵入 | 外部 metadata | 外部 epoch state | 外部 thread progress |
| 典型 HFT | Cold async ownership | 动态 lock-free nodes | 短多节点读临界区 | 固定线程 event loop / RCU |
| 典型风险 | 获取 raw ptr 后 ref++ 竞态 | 错误的 Publish→Validate 顺序 | 长期 pin / 线程暂停 | 遗漏 QS / pointer escape |

QSBR 可用 Per-Reader Monotonic Progress + Global Grace Period 实现；Selective Fan-out 中用 **per-object 的目标集合条件** 取代全部 Reader 的无差别 min，可以避免被无关 Reader 拖慢。只要符合对象访问图与严格发布顺序，此类模型与 QSBR-like 回收思想相通，但不意味着任何 min(cursor) 代码天然满足完整 QSBR 证明。

#### Hot Path 的 atomic<shared_ptr>、HP 与 CAS Pin：不要混入 Completion 语义

| 机制 | 解决什么 | 主要注意点 |
|---|---|---|
| C++20 atomic<shared_ptr<T>> | 并发安全地获取共享所有权、发布新 Snapshot | **不保证 Lock-Free**；反复 RefCount/内部同步及最后一次 Release 触发析构，对严格 p99.9 不友好 |
| Worker-Local shared_ptr + Control Event | 低频切换配置、Hot Path 只读本地版本 | 旧对象 Final Release/Destructor 仍可能发生在 Worker；必要时转给 Cold Reclaimer |
| HP | 保护已安全 Publish/Validate 的具体指针/对象，避免回收 | Per-Reader Hazard 发布与验证、Writer 退休扫描；HP 本身不通知新消息 |
| Stable Slot + Generation CAS Pin | 随机查询固定 Slot 当前版本，原子获得 Pin | Reader RMW / Writer Closing CAS；需防止旧版本复活；Pin 不通知新消息 |
| SPSC / Published Seq | 告诉 Reader 有什么新消息，以及顺序 | Delivery 并不天然替代 Lifetime Protection |

HP 与 CAS Pin 比较时要牢记：**HP = Per-Reader Hazard Records + Writer Hazard Scan；CAS Pin = Per-Slot Generation/Closing/RefCount Atomic State**。二者都不能让固定 Ring 在 Reader 仍访问目标 Slot 时安全原地覆盖。HP 在 Dynamic Pointer-Based Immutable Nodes 更自然；CAS Pin 在稳定 Slot、按 Seq Random Replay 更直接。实际性能需要实测；HP 即使主要用独占 Record Store，也可能需要昂贵的 Memory Ordering/Fence，并非零同步成本。

**新数据通知和对象保护是两个不同协议**：

~~~text
Notification / Visibility:
  SPSC tail, Published Seq, Atomic Current Pointer

Lifetime:
  OwnedHandle, Completion Frontier, RefCount, HP, CAS Pin

Retention:
  Recent Window, Segment Archive, Journal
~~~

最后一个 RefCount Owner 可以是 Hot Worker：可能触发 Destructor/Allocator，甚至取决于运行环境进入内核。Atomic SharedPtr 的具体实现也可能采用内部锁/Spin；C++20 只保证其原子语义，不承诺每次 load 都是普通 Lock-Free Pointer Load。Hot Path 应优先把 Config Ownership 按事件边界移交并隔离复杂销毁，而不是简单认为“没有显式 mutex 就绝不会等待”。

#### “过去某个 Seq 的 Data 仍可访问”需要独立的保护协议

Live Reader 已完成，并不代表 Replay Reader 可以在任意时间安全拿到旧 Slot 指针；只有按 Seq 的版本检验与获得 Pin 是原子的（或使用 Pointer Snapshot + HP/EBR/QSBR / 独立 Archive）才可以规避 Slot Reuse 的数据竞争。

这些方案保护的对象不同：

| 原始数据访问模式 | 优先研究的技术 | 详细示例 |
|---|---|---|
| Stable Slot + (generation, closing, pin) CAS | 最近窗口随机 Replay / Fixed Slot | 附录 E |
| 独立 Replay Reader 借用短窗口指针 | 快速读取但拖住旧槽回收 | 附录 F |
| Cold Reader 复制原始 Payload | Copy 完成后尽快解除原始依赖 | 附录 G |
| Immutable Pointer-Based Snapshot | HP / EBR / QSBR | 附录 H–J |
| 大量历史事件与 Research | Immutable Sealed Segment + Seq Index | 本章及附录示例 |

HAZARD/EBR/QSBR 不等于“固定 Ring Data 还在用时允许 Writer 原地覆盖”。Writer 仍然必须在旧读者退出相关保护区间后才能覆写；如果原地复用的 Slot 被长期 Pin，固定容量本身会遭遇 Backpressure。

---

## Part IV · 决策、Benchmark 和故障注入

### 12. 按工作负载选型：不要按 Lock-Free 名称排名

#### 按 HFT/FX 实际流水线选择

| 场景 | 优先考虑 | 优点 | 主要限制 |
|---|---|---|---|
| 两个固定 Pipeline Stage，一条消息交给一个 Consumer | Value SPSC / Exclusive Handle SPSC | Ownership 简单，通常无额外 Memory Reclamation | Queue Full / Handle Return |
| Dispatcher → Calculation Workers，按 Instrument 分片 | N×SPSC + Single-Owner Sharding | Worker 局部修改状态、减少共享写 | 分片倾斜、跨 Instrument 聚合 |
| Feed Handler → 不同 Strategy / Pricing Task 的订阅子集 | Multiple SPSC + Selective Routing；大 Payload 可共享 Pool | 仅给目标投递、Subscriber 可变 | 部分 Fan-out 投递与旧消息回收 |
| 所有 Reader 都必须接收完整顺序流 | Single-Producer Multicast Ring + Gating Cursor | Data 写一次，统一 Published Seq | 最慢 Reader 可能阻止整个 Ring 覆盖 |
| 多 Producer → Order / FIX Session State Owner | N×SPSC 合流，必要时 MPSC | State Machine 仍由单线程串行更新 | 合流顺序、公平性、Queue Full |
| 一条任务只需多个 Worker 中某一个处理 | MPMC Work Queue / Dispatcher | 任务负载均衡 | 不是 Broadcast，可能有较多竞争 |
| Read-Mostly Instrument / Routing / Pricing Config | Immutable Snapshot + QSBR / EBR | 新旧版本共存、可延迟回收 | 长期 Pin、Online 不报 QS |
| Fixed Embedded Slot + Random Recent Replay | Generation + RefCount CAS Pin | 精确到 Slot + Version | Reader RMW 与 Writer Reuse Stall |
| Research / Recovery / Long Replay | Immutable Segment Archive | 批量 Copy / Index / Reclaim | Archive Lag、Tail Gap、Retention |
| Logger / Telemetry / Persistence | Per-Core SPSC / Chunk Ownership Transfer | 格式化和 I/O 放在 Cold Path | Buffer 耗尽与日志丢失策略 |
| OMS / Execution / Risk / FIX Gateway | Single-Owner State Machine + 显式确认及持久化语义 | 顺序和失败模式明确 | 不能 Silent Drop 或随意 Conflate |

**术语：通常的 SPMC Queue 是多个 Consumer 竞争消费，每条消息只被其中一个取走；本文所谓 SPMC 广播，更准确是 Single-Producer Multicast Ring。**

#### 选择 Reclamation，不要全局统一一套

| Storage / Access Model | 更自然的生命周期机制 |
|---|---|
| SPSC 值拷贝、独占 Handle | Ownership Transfer，无通用回收 |
| Multiple SPSC 共享 Data | Target Completed Cursor |
| Multicast 固定 Ring | Per-Reader Gating Cursor |
| Read-Mostly Immutable Config | QSBR / EBR |
| Lock-Free Linked Nodes / Pointer Index | EBR / HP |
| Fixed Slot + Random Recent Replay | Generation + CAS Pin |
| SEALED Immutable Segment Archive | Segment Pin / shared_ptr / Service Owner |

**决定 EBR/QSBR 是否合适的关键是 Publication 与 Physical Reuse 能否解耦**。有备用 Immutable Object/Pool Slot，Writer 可以先发布再延迟回收；固定满 Ring 必须在覆盖前等 Reader。HP 更精确保护对象地址但固定 Slot 还要 Seq/Generation 验证；CAS Pin 精确保护 Slot+Version，却付出 Reader 原子 RMW 的代价。

QSBR 在线 Worker 若持续不报告 QS，可能阻止未来 Grace Period；EBR 的 Scoped Unpin 结束后即使线程暂停也不阻止未来回收。二者适合 Immutable Object Reclamation，而非直接替代每个固定 Embedded Slot 的任意并发覆写协议。

#### Backpressure：算法无法绕开的容量约束

**有限容量、完全无损、任意慢/永久停顿 Consumer、Writer 永远不阻塞**无法无条件同时保证。要么有额外资源和持久化，要么显式定义 Reject/Drop/Retry/Disconnect/Conflation 的业务策略。

| 业务 | 更合理的策略 | 禁忌 |
|---|---|---|
| 只要求最新价态的 Pricing Trigger | Version-Based Conflation / Coalescing | 合并不可跳过的业务转移 |
| 可恢复 Market Data State | Gap Detect + Snapshot/Replay，必要时标记 Stale | 未确认数据连续就继续报价 |
| FIX Order / Execution / Booking | 有界 Backpressure、明确拒绝、重试或持久化 | Silent Drop |
| Risk / Audit | 明确的保序、Durability、Fail-Closed 策略 | 仅依赖可覆盖的 In-Memory Ring |
| Metrics / 可丢失 Telemetry | Sampling / Batch / Bounded Drop | 非关键日志阻塞核心下单 |
| Historical Archive | Lag 监测、容量预留、分层存储与降级决策 | 默认 Background Worker 不会落后 |

QueueConsumed != BusinessCommitted != DurablyRecorded != ExchangeAccepted != Filled。Completed Cursor 的业务层含义必须由接口精确定义。

#### 对当前 Polling Processor 的推荐组合

~~~text
1. State/Sequencing:      Single-Owner Per-Partition State Machine
2. Worker Communication:  Small Value SPSC / Exclusive Handle SPSC
3. Sparse Subscriptions:  Writer-Owned Routing + Multiple SPSC + Optional Shared Pool
4. Full Broadcast:        Multicast Ring + Per-Reader Gating
5. Read-Mostly Config:    Immutable Snapshot + QSBR / EBR
6. Recent Random Replay:  Stable Slot Generation + CAS Pin
7. Logger / Long Replay:  Segmented SPSC + Immutable Segment Archive
8. Dynamic Membership:    Control Event → Owner → effectiveSeq → Drain / Reclaim
~~~

不要先开发通用 Lock-Free MPMC Scheduler、全局 Hazard Pointer Runtime 或任意动态线程拓扑，除非存在明确需求及可测量收益。主要目标是**单一写所有权、减少跨核共享写、限制慢消费者影响、明确生命周期和可恢复性**。

#### 方案之间如何选

| 机制 | Completion Metadata | Reader 更新 | 回收能力 | 典型瓶颈 |
|---|---|---|---|---|
| Intrusive Atomic RefCount | Per-Data Count | Atomic RMW | Per-Object Out-of-Order | 多核写相同 RefCount，Final Release |
| Sharded Mutable RefCounts | Per-Data × Reader Count | 各分片 RMW/Store | 理论可 Per-Object，需 Freeze/Reclamation Protocol | 全零快照不可靠、关闭新引用复杂 |
| Per-Data Per-Reader Marker | Per-Data × Target | 各 Reader Release Store | Per-Object Out-of-Order | 内存占用 / False Sharing / Writer Scan |
| Reverse SPSC Per-Object ACK | Per-Edge Queue + Owner State | 向 ACK Queue Push | Per-Object Out-of-Order | ACK 流量、Queue Full、Pending Graph |
| Per-Stream Cumulative Cursor | Per-Logical Stream | Release Store，可 Batch | 连续 Prefix | HOL、Selective Skip |
| **Reader × Source Matrix** | **O(Readers × Sources) Cursor** | **Per-Reader 自己的一行做 Store** | **每 Source 连续 Prefix** | **Source Writer 扫描 / Selective Fan-out 停滞** |

对“固定 Source、多核处理同一有序事件流、FIFO 最终完成、Producer 需要回收自己的 Ring”，Reader × Source Matrix 很值得 Prototype；它也是多 Source 版本的 Gating Sequence。对于大量 Selective Fan-out 或跨级异步乱序，不能因矩阵形式漂亮就强行使用一个 min：可选择 Per-Object Marker、反向 SPSC ACK、混合 SACK 或 Owner-Local Completion Map。

#### 多级 FIFO 和 Selective DAG 的最终取舍

| 问题 | 合理默认 |
|---|---|
| 是否只有同步 Borrow？ | 能不跨线程就不增加 Lifetime Protocol |
| 是否只有一次异步 Move？ | Move-Only Handle，避免无谓的 AddRef/Release |
| 是否是完整 FIFO Broadcast？ | Multicast Gating Cursor |
| 是否是多 Source 扁平 Reader 拓扑？ | Reader × Source Matrix |
| 多级 Fixed FIFO、相同 Sequence？ | Proactive Cumulative Safe Cursor |
| Data2 引用 Data1，动态 Selective DAG？ | Owner-Local Dependency + Hierarchical ACK |
| 乱序很多、精准对象回收？ | Per-Object ACK / Intrusive RefCount / Marker |
| 少量乱序、绝大多数按序？ | Cumulative Cursor + Bitmap / SACK |
| Recent Random Replay？ | CAS Pin / Stable Slot Generation |
| Immutable Config 与 Cold Lifetime？ | QSBR / EBR / HP |

**性能排序为未验证假说**：避免宣称反向 ACK 必然比 Atomic RefCount 快，或者 Cursor 必然降低 p99.9。应在相同 CPU 亲和、消息大小、Fan-out、Burst、慢 Consumer、回收策略下比较 End-to-End 和 Writer p99.9。

---

### 13. Benchmark、Interleaving、正确性与活性验证

### 必测的正确性边界

- P4 已完成但 P2/P3 无新消息，测试级联 Cursor 的 Idle Progress Liveness；
- P1 Pool 枯竭时按需跨级 Progress Request 是否抬高 Writer p99.9；
- Sharded Mutable RefCount 的 Seq-Cst 非一致性全零快照，禁止无 Freeze/Validation 的提前回收；

- Writer 发布 Data 后，某个目标 SPSC 满；部分投递成功时如何回滚/重试？
- Reader 在 dequeue 之后、真正访问 Data 之前被 OS 抢占；Writer 是否会过早复用？
- Replay 在 Pin 成功之后被抢占；Writer tail latency / fallback 如何处理？
- Replay CAS 与 Writer Closing CAS 谁先赢？seq mismatch/closing/ref overflow 分别怎样处理？
- Reader 动态注销时，未完成投递如何 drain？新 Reader 是否会误用相同 ID 的历史进度？
- Reader 完成一个 Task 后仍有异步 callback 访问 Config*，checkpoint 是否错误地宣告安全？
- Pool 和 Queue 的 Slot 分开复用时，各自 generation / sequence 何时改变？
- 冷路径按 seq Lookup 超出 retention window，必须返回 unavailable，而不是读到被复用的 Slot。
- Copy Worker 落后时：Writer backpressure / bounded memory / 允许历史丢失，必须明确选择。
- Archive 中 seq index 和 Segment eviction 是否具有自己的 lifetime protocol？
- 即使引用计数为 0，Retention Policy 仍可能要求继续保留数据。

### Benchmarks

1. Writer 无 Replay：uncontended CAS 对比无 CAS Store/Progress；
2. Replay 随机 Lookup：成功率、CAS failure、延迟、Lookup 结果一致性；
3. Replay 线程被人为暂停 1–10ms，测 Writer p99.9/max 与 pinned Stall；
4. Selective fan-out target size = 1、2、4、8、全部，测 RMW/cache bouncing；
5. Per-Reader completed cursor 按每消息 vs batch 更新，测 cache-line traffic；
6. 各 Reader 速度不一致，比较多 SPSC Pool 的乱序回收与 Ring head-of-line blocking；
7. Copy Worker：messages/s × avg payload bytes 的额外内存带宽、LLC、NUMA 与 backlog；
8. Segment size 对 archive lookup、reclamation latency 和内存峰值的影响；
9. 除 p50/p99.9 之外，记录 max、backlog、retired bytes、CAS retries、data corruption / TSAN 结果。

### 性能结论（需现场实测验证）

- Fixed-size Value SPSC / MPMC 可能根本不需要通用内存回收。
- 如果只通过目标 Reader 的 Queue 取得对象，Per-Reader Progress 可以替代每条 Data 的原子引用计数。
- 如果 Replay Lookup 频率远低于 Writer Publish，现有稳定 Slot CAS Pin 可能在总成本上胜过始终复制全部 Data。
- 如果 Writer 的 CAS 或 Replay Pin 造成难以接受的 tail latency，专用 Replay Copy Archive 可以将 Cold Lookup 与 Original Pool 的生命周期解耦。
- 方案 B 不是免费的：Archive Copy Worker 是额外 Consumer；如果它跟不上，Writer 依旧需要 backpressure 或更大的保留容量。

### 推荐阅读

- Maged M. Michael, Hazard Pointers (IEEE TPDS, 2004)。
- Keir Fraser, Practical Lock-Freedom (2004)：epoch / non-blocking memory management。
- Userspace RCU (liburcu) 文档：QSBR 的 register / online / offline / quiescent。
- DPDK RCU QSBR Programmer's Guide。
- LMAX Disruptor：Gating Sequence 和多 Consumer 完成进度。
- Dmitry Vyukov bounded MPMC queue：per-cell sequence protocol。
- C++ standard / cppreference：atomic shared_ptr、atomic memory ordering、happens-before、object lifetime。

**代码安全声明**：本文件的多线程演示均以线程不异常终止、序号不回绕、容量约束满足、单 Writer 等明确假设为前提。不要把 demo 中的 spin-wait、线程管理、内存分配和简单回收逻辑直接用于生产 HFT 热路径；应先进行更严格的形式化审查、TSAN/ASAN/UBSAN、长时间 stress 与故障注入。

#### 选型验证清单

1. Value SPSC vs Handle SPSC：Payload Size、Core/NUMA、Writer p99.9。
2. Selective vs Multicast：Target Count 1/2/4/8/all、慢 Reader、Queue 满。
3. Free LIFO Stack vs FIFO Ring：Pool 水位、HITM/LLC Miss、Pending Scan。
4. Recent CAS Pin Replay：随机 Lookup、Reader 抢占、Writer Closing CAS、RMW 竞争。
5. QSBR/EBR/HP：Reader 暂停、Grace Period、Hazard Scan、回收峰值。
6. 普通 SPSC vs Segmented：逐条可见性、Batch Flush Delay、End-to-End Latency。
7. Dynamic Add/Remove：Reader ID/Generation、旧 Target Obligations、Cancel/Drain、Controller Starvation。
8. Archive：Copy Worker 落后、WRITING Tail、Segment Eviction、History→Live Join。
9. 所有测试记录 p50/p99/p99.9/max、Queue Depth、Free Slot、Retired Bytes、Archive Lag、Lost/Duplicate/Out-of-Order 数。

**架构判断置信度：高（协议与安全不变量），中等（各场景的优先级），具体性能排名必须以部署机器上的真实负载为准。**

---

#### Prototype / Benchmark / 故障测试

建议固定数据处理、消息大小、Pool 和 CPU Affinity，避免模型比较时夹带不同的分配器/Copy/Queue 实现。至少比较：

1. 优化的 Atomic RefCount（预先初始化 Fan-out 引用、Move-Only Handle）。
2. Per-Data Per-Reader Completion Marker（两种 Cache Line Layout）。
3. Reverse SPSC Per-Object ACK 与 Hierarchical ACK。
4. Per-Stream Cumulative CompletionChannel。
5. **Reader × Source Matrix**：Reader-Major Row、可选 Cell Padding、Cached Min；从固定 Full Broadcast 开始。
6. Selective Fan-out：100% / 50% / 10% 命中、Subscriber 长期无新数据、Partial Publish。
7. Out-of-Order：特定 Seq 长暂停、其它 Seq 完成；记录 HOL 与 Free Slot 耗尽。
8. 动态 Remove/Add/Generation Reuse；Forward Queue Full、Reader 被停顿、Shutdown Drain。
9. 多级 Data1→Data2→P3、一个 Data1 对应多个 Data2、多个 Data2 并行完成。
10. 记录 Source Writer p99.9/max、Completion Publication 频率、Cache-to-Cache HITM、RMW 数、Scan 频率、Reclamation Lag、Pool Free Slots、CPU/NUMA/LLC。

**结论置信度**：单调 Cursor 的安全下界证明高；Sharded Mutable RefCount 的不一致快照风险高；该 Matrix 与 RC 的性能优劣中等，需要目标平台与真实 Data/Reader 分布验证。

---

#### 应新增的验证场景

1. P4 完成 Seq100，P2/P3 无新数据：验证无 Idle Poll 时级联 Safe 不前进；引入 Proactive/Idle Poll 后最终正确推进。
2. P1 Pool 快满时同步 Request 与后台主动推送，对比 Writer p99.9/max、进度传播延迟。
3. ACK Queue Full + Forward Queue Full；测试所有阶段的 Bounded Poll、Drain、Shutdown，禁止双向永久自旋。
4. Same-Seq FIFO 链改为 Selective Fan-out 和不同 Local Orders，测试裸 min 会错误/停滞。
5. P2 创建多个 Data2 引用一个 Data1，P3/P4 乱序完成；验证 Parent 完成不早于全部 Child。
6. 执行 Sharded Mutable RefCount 的全零非一致快照反例；不得在未 Freeze/Validate 时依靠扫描回收。

**结论**：ACK 和 Cursor 都需要异步调度推进；Cumulative Cursor 在 FIFO 中可高效级联，但纯被动传播缺少 Liveness；Reader × Source Matrix 适合扁平广播，复杂多级 Selective DAG 更倾向 Owner-Local ACK。正确性判断置信度高，性能排序须 Benchmark。

---

#### 统一实验记录模板

| 字段 | 说明 |
|---|---|
| Workload | Message Size、Rate、Burst、Fan-out、FIFO/OOO 比例、Async Depth |
| CPU / NUMA | Affinity、同 Socket 或跨 Socket、Cache-Line Layout |
| Completion | Per-Object ACK / Cursor / Matrix / RefCount、Batch/Flush 策略 |
| Hot Path | Publish p50/p99/p99.9/max、CPU cycles/message |
| Reclamation | LastUse→ObservedDone→FreeSlot 时间，Pending Bytes 和 Pool Low Watermark |
| Coherence | Cache-to-Cache HITM、RMW 数、Atomic Store/Load、Queue Head/Tail |
| Faults | 停顿 Reader、ACK Queue Full、Partial Fan-out、Shutdown、Reconfigure |
| Verification | 用固定 Seq + Generation 断言，不提前回收、不丢 ACK、不无限等待 |

**证明工作分离**：用有限状态 Interleaving / 手工时序分析证明安全，用 Stress/TSAN/ASAN/UBSAN 辅助发现实现问题；测试通过不等于形式证明，也不代表生产延迟目标达成。

---

## Part V · 教学实现与深度参考（保持原代码）

> **附录代码说明**：为了让正文保持架构层次，原有长代码被移到此处，而非删除；HP/EBR/QSBR 部分还包含多个模型、SC 时序说明与库实现讨论。示例默认预分配/固定线程/不异常退出等条件，并不覆盖所有 Restart、Wrap、Crash、Partial Publish 或正式部署要求。

### 附录 A · 固定容量 SPSC / MPMC

#### A.1 SPSC：可复用的最小实现

后续选择性 Fan-out 代码共用这个头文件，按下列内容保存为 **mini_spsc.hpp**：

~~~cpp
// mini_spsc.hpp
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
template <class T, std::size_t N>
struct Spsc {
    static_assert(N > 0);
    std::array<T, N> data{};
    std::atomic<std::size_t> write{0}, read{0};
    bool push(const T& x) { // 单 Producer
        auto w = write.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == N) return false;
        data[w % N] = x;
        write.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& x) { // 单 Consumer
        auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        x = data[r % N];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
};
~~~

使用约束：一个 Queue 永远只有一个 Producer 和一个 Consumer；不得销毁还在被访问的 Queue；T 在消费前不可被 Producer 擅自重写。生产版注意计数回绕、padding、cache-line alignment、批处理与异常安全。

#### A.2 MPMC：固定容量 Array + 每 Slot Sequence

这是经典 bounded sequence-cell MPMC 思路的独立 C++20 示例；无动态链表 Node，因此**队列内部不需要 HP/EBR**：

~~~cpp
// 05_mpmc.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
template<class T, std::size_t N>
class Mpmc {
    static_assert(N >= 2 && (N & (N - 1)) == 0);
    struct Cell {
        std::atomic<std::size_t> seq;
        T value{};
    };
    std::array<Cell, N> cells{};
    alignas(64) std::atomic<std::size_t> put{0};
    alignas(64) std::atomic<std::size_t> get{0};
public:
    Mpmc() {
        for (std::size_t i = 0; i < N; ++i)
            cells[i].seq.store(i, std::memory_order_relaxed);
    }
    bool push(T value) {
        std::size_t pos = put.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells[pos & (N - 1)];
            auto s = c.seq.load(std::memory_order_acquire);
            auto dif = static_cast<std::intptr_t>(s) -
                       static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                if (put.compare_exchange_weak(pos, pos + 1,
                          std::memory_order_relaxed)) {
                    c.value = value;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) return false; // 满
            else pos = put.load(std::memory_order_relaxed);
        }
    }
    bool pop(T& result) {
        std::size_t pos = get.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells[pos & (N - 1)];
            auto s = c.seq.load(std::memory_order_acquire);
            auto dif = static_cast<std::intptr_t>(s) -
                       static_cast<std::intptr_t>(pos + 1);
            if (dif == 0) {
                if (get.compare_exchange_weak(pos, pos + 1,
                          std::memory_order_relaxed)) {
                    result = c.value;
                    c.seq.store(pos + N, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) return false; // 空
            else pos = get.load(std::memory_order_relaxed);
        }
    }
};
int main() {
    Mpmc<int, 64> q;
    std::atomic<int> consumed{0};
    std::atomic<long long> sum{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < 2; ++p)
        threads.emplace_back([&, p] {
            for (int k = 1; k <= 1000; ++k)
                while (!q.push(p * 1000 + k)) std::this_thread::yield();
        });
    for (int c = 0; c < 2; ++c)
        threads.emplace_back([&] {
            int x;
            while (consumed.load() < 2000) {
                if (q.pop(x)) {
                    sum.fetch_add(x);
                    consumed.fetch_add(1);
                } else std::this_thread::yield();
            }
        });
    for (auto& t : threads) t.join();
    assert(consumed == 2000 && sum == 2001000);
}
~~~

**提醒**：固定 Ring 的 Cell 状态和普通消息内存由 Sequence Protocol 保护；若 payload 是多个 Consumer 共享的 Data*，其对象生命周期仍须另外管理。

---

### 附录 B · Multiple SPSC + Shared Pool / Selective Fan-out

每条 Data 只进入目标 Reader 的 SPSC；Data 在 Pool 中存一份。Writer 记录每个目标 Reader 所要求完成的 **本地 Queue 序号**。Writer 是唯一 Reclaimer，无每 Data 的 Reader-side atomic RMW；非目标 Reader 无需扫描或确认消息。

> **关键适用前提**：下游在将本地 Completed Cursor 推进之前，必须结束该 Data 的**全部**原始引用使用。如果 Consumer 把 Data Handle 异步传给 P3，不能在“转发成功”时就报告 Safe Completed；应改用 Transitive Completion Gating、Reference Counting 或 Completion Ack。详见 第 2–6 章。

~~~cpp
// 06a_selective_spsc_pool.cpp  (需要同目录 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>
struct Data { uint64_t seq{}; int value{}; };
struct Item { std::size_t slot{}; uint64_t seq{}, order{}; };
struct Retired {
    std::size_t slot{};
    uint32_t targets{};
    std::array<uint64_t,3> required{};
};
int main() {
    constexpr std::size_t N = 3, POOL = 8;
    std::array<Data, POOL> pool{};
    std::array<Spsc<Item, 16>, N> qs;
    std::array<std::atomic<uint64_t>, N> completed{};
    for (auto& c : completed) c.store(0);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (std::size_t i=0; i<N; ++i) {
        readers.emplace_back([&,i] {
            Item x;
            for (;;) {
                if (qs[i].pop(x)) {
                    assert(pool[x.slot].seq == x.seq);
                    assert(pool[x.slot].value == int(x.seq * 10));
                    completed[i].store(x.order, std::memory_order_release);
                } else if (done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    }
    std::vector<std::size_t> freeSlots;
    for (std::size_t j=0; j<POOL; ++j) freeSlots.push_back(j);
    std::deque<Retired> pending;
    std::array<uint64_t,N> produced{};
    auto reclaim = [&] {
        for (auto it=pending.begin(); it!=pending.end();) {
            bool safe=true;
            for (std::size_t i=0;i<N;++i)
                if ((it->targets & (1u<<i)) &&
                    completed[i].load(std::memory_order_acquire)
                        < it->required[i]) safe=false;
            if (safe) {
                freeSlots.push_back(it->slot);
                it=pending.erase(it);
            } else ++it;
        }
    };
    for (uint64_t seq=1; seq<=100; ++seq) {
        while (freeSlots.empty()) { reclaim(); std::this_thread::yield(); }
        auto slot=freeSlots.back(); freeSlots.pop_back();
        pool[slot]={seq,int(seq*10)};
        uint32_t mask = 1u << (seq%N);
        if (seq%4==0) mask |= 1u << ((seq+1)%N);
        Retired r{slot,mask,{}};
        for (std::size_t i=0;i<N;++i) if (mask & (1u<<i)) {
            auto order=++produced[i];
            r.required[i]=order;
            while (!qs[i].push(Item{slot,seq,order}))
                std::this_thread::yield();
        }
        pending.push_back(r);
        reclaim();
    }
    while (!pending.empty()) { reclaim(); std::this_thread::yield(); }
    done.store(true,std::memory_order_release);
    for (auto& t:readers) t.join();
    assert(freeSlots.size()==POOL);
}
~~~

**条件**：Reader 完成进度在最后一次使用对象之后发布；Queue 不会重复投递旧 Handle；单 Writer 维护 free/pending。多 SPSC 的多目标发布不是原子事务：若其中一条 Queue 满而另一个已经投递，必须显式实现预检容量、重试或定义部分投递语义。

---

### 附录 C · Multicast Pending Mask

每个 Slot 的 pending bits 表示尚未完成的目标 Reader。只给目标 Reader 的 SPSC 投递 global seq；Reader 处理完成对该 Slot fetch_and 清除自己 bit；Writer 观察 pending==0 后才允许覆盖。这个模型与上一节共享 Pool 的根本区别：按固定 Ring Slot 顺序复用；一个慢目标可能使 Writer 卡在该 Slot 上。

~~~cpp
// 06b1_selective_pending_mask.cpp  (需要 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
struct Data { uint64_t seq{}; int value{}; };
struct Slot {
    Data data{};
    std::atomic<uint32_t> pending{0};
};
int main() {
    constexpr std::size_t N=3, CAP=8;
    std::array<Slot,CAP> ring;
    std::array<Spsc<uint64_t,16>,N> qs;
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (std::size_t i=0;i<N;++i)
        readers.emplace_back([&,i] {
            uint64_t seq;
            for (;;) {
                if (qs[i].pop(seq)) {
                    auto& s=ring[seq%CAP];
                    assert(s.data.seq==seq);
                    assert(s.data.value==int(seq*10));
                    // 最后一次访问 s.data 后 ACK
                    s.pending.fetch_and(~(1u<<i),
                                        std::memory_order_release);
                } else if(done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    for (uint64_t seq=1;seq<=100;++seq) {
        auto& slot=ring[seq%CAP];
        while(slot.pending.load(std::memory_order_acquire)!=0)
            std::this_thread::yield();
        uint32_t mask = 1u<<(seq%N);
        if(seq%4==0) mask |= 1u<<((seq+1)%N);
        slot.data={seq,int(seq*10)};
        slot.pending.store(mask,std::memory_order_release);
        for(std::size_t i=0;i<N;++i) if(mask&(1u<<i))
            while(!qs[i].push(seq)) std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);
    for(auto& t:readers)t.join();
    for(auto& s:ring) assert(s.pending.load()==0);
}
~~~

ACK 是 **每个目标 Reader / 每条消息一个原子 RMW**，可能导致同一 Slot cache-line 的 ownership bouncing。需要明确 slot 的 generation、异常 reader 移除、以及若支持多个并发 Writer 时的发布协调。此例只在单 Writer、每个 Reader FIFO、不提前清 bit 的条件下成立。

---

### 附录 D · Multicast Gating Cursor

本方案仍然只向目标 Reader 的 SPSC 投递 seq，而不让所有 Reader 全表扫描。每个 Reader 仅写自己的 completedGlobal；对于旧 Slot 中的 Data x，Writer 只检查该 Data 的 targetMask 对应 Reader 是否满足 C(i) >= x.seq。

不同于最保守的 Disruptor 全员 min(cursor)：**这里不能让非目标 Reader 扫描已经被提前覆盖的 metadata**。SPSC 负责告诉每个 Reader 它具体需要读取哪些全局序号；FIFO 证明 C(i) >= x.seq 意味该 Reader 已完成 x（若它是目标）。

~~~cpp
// 06b2_selective_global_cursor.cpp  (需要 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
struct Slot {
    uint64_t seq{0};
    uint32_t targets{0};
    int value{0};
};
int main() {
    constexpr std::size_t N=3, CAP=8;
    std::array<Slot,CAP> ring{};
    std::array<Spsc<uint64_t,16>,N> qs;
    std::array<std::atomic<uint64_t>,N> completed{};
    for(auto& c:completed)c.store(0);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for(std::size_t i=0;i<N;++i)
        readers.emplace_back([&,i] {
            uint64_t seq;
            for(;;) {
                if(qs[i].pop(seq)) {
                    auto const& s=ring[seq%CAP];
                    assert(s.seq==seq && s.value==int(seq*10));
                    completed[i].store(seq,std::memory_order_release);
                } else if(done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    for(uint64_t seq=1;seq<=100;++seq) {
        Slot& s=ring[seq%CAP];
        auto ready = [&] {
            if(s.seq==0) return true;
            for(std::size_t i=0;i<N;++i)
                if((s.targets&(1u<<i)) &&
                   completed[i].load(std::memory_order_acquire)<s.seq)
                    return false;
            return true;
        };
        while(!ready()) std::this_thread::yield();
        uint32_t mask=1u<<(seq%N);
        if(seq%4==0) mask|=1u<<((seq+1)%N);
        s={seq,mask,int(seq*10)};
        for(std::size_t i=0;i<N;++i) if(mask&(1u<<i))
            while(!qs[i].push(seq)) std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);
    for(auto& t:readers)t.join();
}
~~~

**为什么这里可检查单条 target subset**：非目标 Reader 永不访问这个 Slot 对应的老消息；目标 Reader 只有经 Queue 取得 seq 才会访问。若改成所有 Reader 顺序扫描全局 Ring，则必须为被跳过/覆盖的 sequence 设计完整的 generation-aware skip 协议，否则不能直接按目标最小进度覆盖 Slot。

---

### 附录 E · Stable Slot Generation + CAS Pin

你的真实设计核心：固定 Pool Slot 的 **稳定 atomic 状态字** 包含 generation/sequence + pin count；Replay 通过 CAS 对 Slot Pin，Writer 想要推进/复用 Slot 也必须 CAS 先 Close，禁止新 Pin，待旧 Pin 清零才能覆盖非原子 Data。

**这比先 load Data* 再 fetch_add 对象内部 refcount 安全得多**，因为 CAS 的 metadata 地址在整个 Pool 生命周期内稳定。但它仍然要求维护保留窗口 / seq index，有效 seq 被逐出后 Lookup 必须失败。

下面用一个 64-bit 状态字演示：高 32 位 seq、低 31 位 pinCount、低位最高 bit 为 CLOSING。示例不处理超过 2^32-1 的 seq 和 refcount overflow；生产版请设计更大的 generation 或显式 wrap 机制。

~~~cpp
// 07_replay_cas_pin.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
struct Data { uint64_t seq{}; int value{}; };
struct Slot {
    std::atomic<uint64_t> state{0};
    Data data{};
};
constexpr uint64_t CLOSING = 1ull<<31;
constexpr uint64_t PINS = CLOSING-1;
constexpr std::size_t CAP=8;
struct ReplayBuffer {
    std::array<Slot,CAP> ring;
    std::atomic<uint64_t> latest{0};
    bool lookup(uint32_t seq, Data& output) {
        Slot& s=ring[seq%CAP]; // 稳定 metadata，不能先读 Data*
        uint64_t st=s.state.load(std::memory_order_acquire);
        for(;;) {
            if ((st>>32)!=seq || (st&CLOSING) || (st&PINS)==PINS)
                return false;
            if(s.state.compare_exchange_weak(
                    st,st+1,std::memory_order_acquire,
                    std::memory_order_relaxed)) break;
        }
        output=s.data; // Pin 成功后 Writer 无法覆盖
        s.state.fetch_sub(1,std::memory_order_release);
        return output.seq==seq;
    }
    void publish(uint32_t seq, int value) { // 只有单 Writer
        Slot& s=ring[seq%CAP];
        for(;;) {
            uint64_t st=s.state.load(std::memory_order_acquire);
            if (st&CLOSING) {
                if ((st&PINS)==0) break;
            } else if (s.state.compare_exchange_weak(
                           st,st|CLOSING,std::memory_order_acq_rel,
                           std::memory_order_acquire)) {
                if ((st&PINS)==0) break;
            }
            std::this_thread::yield(); // pinned reader 慢时 Writer 可能 stall
        }
        s.data={seq,value}; // 当前无 Pin、无新 Pin
        s.state.store(uint64_t(seq)<<32,std::memory_order_release);
        latest.store(seq,std::memory_order_release);
    }
};
int main() {
    ReplayBuffer b;
    std::atomic<bool> done{false};
    std::thread replay([&] {
        Data out;
        while(!done.load(std::memory_order_acquire)) {
            auto latest=b.latest.load(std::memory_order_acquire);
            if(latest>0) {
                auto wanted=uint32_t(latest);
                if(b.lookup(wanted,out))
                    assert(out.value==int(out.seq*10));
            }
        }
    });
    for(uint32_t seq=1;seq<=1000;++seq) b.publish(seq,int(seq*10));
    done.store(true,std::memory_order_release);
    replay.join();
}
~~~

**线性化点**：Replay Pin CAS 与 Writer VALID→CLOSING CAS 在同一个 atomic 状态字上竞争；谁先成功谁受协议保护。Reader release pin 完毕后，Writer acquire 读到 zero pins，才可写新的 Data。

**成本**：Writer 每次复用 Slot 至少一个 RMW/CAS；Replay Pin 还有 RMW；即使 Replay 极少发生也要支付 Writer 常态 CAS。更严重的可能是 Replay 被抢占时 Writer 卡在 pinned Slot（Tail latency）。Single-Writer 的 CAS 不一定 contention 高，需测量后才能判断是否是瓶颈。

**与 Live Reader 的关系**：此例只演示 Replay Pin！如果同一个 Slot 还被 Live Reader 使用，Writer 除了等待 Pins，还必须额外等待该 Slot 对应目标 Live Reader 的完成进度。

---

### 附录 F · Recent Replay Pointer Window

这是你提出的 Replay 方案 A。唯一 Replay Worker 按 seq 顺序读取、缓存一段 **指向原始 Ring 的指针**，在释放最早指针后才推进 releasedThrough。Writer 复用 Slot 前必须保证原 seq 已被 Release。

重点：scannedThrough != releasedThrough；不能因取到一个指针就认为它已无需保护。

~~~cpp
// 08a_replay_pointer_window.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <thread>
struct Data { uint64_t seq{}; int value{}; };
int main() {
    constexpr uint64_t CAP=16, WINDOW=4, TOTAL=100;
    std::array<Data,CAP> ring{};
    std::atomic<uint64_t> published{0}, releasedThrough{0};
    std::atomic<bool> done{false};
    std::thread replay([&] {
        std::deque<const Data*> cache; // 仅此线程使用指针
        uint64_t next=1;
        for(;;) {
            auto end=published.load(std::memory_order_acquire);
            if(next<=end) {
                const Data* p=&ring[next%CAP];
                assert(p->seq==next && p->value==int(next*10));
                cache.push_back(p);
                // 缓存窗口内随机访问仍合法
                for(const Data* c:cache) assert(c->value==int(c->seq*10));
                if(cache.size()>WINDOW) {
                    auto* old=cache.front();
                    auto oldSeq=old->seq;
                    cache.pop_front(); // 之后不再触碰 old
                    releasedThrough.store(oldSeq,std::memory_order_release);
                }
                ++next;
            } else if(done.load(std::memory_order_acquire)) break;
            else std::this_thread::yield();
        }
        cache.clear();
        releasedThrough.store(next-1,std::memory_order_release);
    });
    for(uint64_t seq=1;seq<=TOTAL;++seq) {
        while(seq-releasedThrough.load(std::memory_order_acquire)>CAP)
            std::this_thread::yield();
        ring[seq%CAP]={seq,int(seq*10)};
        published.store(seq,std::memory_order_release);
    }
    done.store(true,std::memory_order_release);
    replay.join();
    assert(releasedThrough==TOTAL);
}
~~~

此例中 Cold Lookup 必须 **串行化到 Replay Worker** 或额外保护缓存指针。如果别的 Cold Thread 也能随意取缓存 Data* 并跨越 Worker release，依然会 UAF。保留指针窗口为零拷贝，但 Replay 卡住时仍可能阻塞 Writer（或造成额外容量要求）。

---

### 附录 G · Replay Copy Archive

这是你提出的方案 B：Replay Worker 在受到原始 Ring 保护时深拷贝 Data 到独立 Archive，然后推进 copiedThrough。Writer 只需对这个 copy watermark 做 acquire load；正常 Slot 发布不用 Replay Pin CAS。Cold Lookup 从 Archive 查找，不再依赖原始 Ring。

~~~cpp
// 08b_replay_copy_archive.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
struct Data { uint64_t seq{}; int value{}; std::array<char,32> payload{}; };
int main() {
    constexpr uint64_t CAP=16, TOTAL=100;
    std::array<Data,CAP> ring{};
    std::atomic<uint64_t> published{0}, copiedThrough{0};
    std::atomic<bool> done{false};
    std::mutex archiveMutex; // 仅在 Cold Path 使用
    std::unordered_map<uint64_t,Data> archive;
    std::thread replay([&] {
        uint64_t next=1;
        for(;;) {
            auto end=published.load(std::memory_order_acquire);
            if(next<=end) {
                Data copy=ring[next%CAP];  // 完整 Value Copy
                assert(copy.seq==next);
                {
                    std::lock_guard lk(archiveMutex);
                    archive.emplace(next,copy);
                }
                copiedThrough.store(next,std::memory_order_release);
                ++next;
            } else if(done.load(std::memory_order_acquire)) break;
            else std::this_thread::yield();
        }
    });
    for(uint64_t seq=1;seq<=TOTAL;++seq) {
        while(seq-copiedThrough.load(std::memory_order_acquire)>CAP)
            std::this_thread::yield();
        ring[seq%CAP]=Data{seq,int(seq*10),{}};
        published.store(seq,std::memory_order_release);
    }
    done.store(true,std::memory_order_release);
    replay.join();
    {
        std::lock_guard lk(archiveMutex);
        assert(archive.size()==TOTAL);
        assert(archive.at(50).value==500);
    }
}
~~~

上面 Data 所有字段均按值存储。如果真实 Data 含 char*、span 或外部 Buffer，则必须 **deep copy 实际 payload** 或对外部 Buffer 加保护。copiedThrough 表示 Archive 已持有独立存储副本；不自动代表写入磁盘完成或 crash-safe。如果 Archive 还要提供持久化保证，应另外设置 durableThrough（fsync / flush / storage acknowledgment）。

---

### 附录 H · Hazard Pointer：算法、内存序与 Folly

Reader 公布正在保护的 **具体指针**；Writer retire 后扫描 hazard 记录，并只删除不再受保护的 Node。下面使用固定 Reader 槽、seq_cst 保守排序；真实系统可采用可复用 Hazard Record Pool 与批量 retire 扫描。

~~~cpp
// 02_hazard_pointer.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
struct Node { int value; };
int main() {
    std::atomic<Node*> head{new Node{0}};
    std::array<std::atomic<Node*>, 2> hazard{};
    for (auto& h : hazard) h.store(nullptr);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 10000; ++n) {
            Node* p;
            do {
                p = head.load(std::memory_order_seq_cst);
                hazard[id].store(p, std::memory_order_seq_cst);
            } while (p != head.load(std::memory_order_seq_cst));
            if (p) assert(p->value >= 0);
            hazard[id].store(nullptr, std::memory_order_seq_cst);
        }
    };
    std::thread r0(reader, 0), r1(reader, 1);
    go.store(true, std::memory_order_release);
    std::vector<Node*> retired;
    auto scan = [&] {
        const Node* h0 = hazard[0].load(std::memory_order_seq_cst);
        const Node* h1 = hazard[1].load(std::memory_order_seq_cst);
        auto out = retired.begin();
        for (Node* p : retired) {
            if (p != h0 && p != h1) delete p;
            else *out++ = p;
        }
        retired.erase(out, retired.end());
    };
    for (int i = 1; i < 1000; ++i) {
        retired.push_back(head.exchange(new Node{i},
                                        std::memory_order_seq_cst));
        if (retired.size() > 16) scan();
    }
    r0.join(); r1.join();
    scan();
    delete head.load();
    assert(retired.empty());
}
~~~

**限制**：此例只有两个固定 Reader 和一个 Writer，Hazard Record 本身稳定存活。动态加入：先获取并注册可被 scanner 发现的空 Slot，再保护数据；退出：先清 Hazard，之后才归还 Slot。若扫描器并发访问 Record，不能直接 delete Record。HP 处理回收安全，不自动证明任意 lock-free 算法没有逻辑 ABA。


#### H.1 Hazard Pointer 是否只能读取最新版本？

不是。Hazard Pointer (HP) 管理 **Lifetime Protection**，而非 Lookup 或历史数据保留。Reader 一旦成功保护 Seq 100，即使 Writer 把 index[100 % N] 改为指向 Seq 100+N，Reader 仍能处理原有 Seq 100；新 Reader 再查询 Seq 100，因 index 中已是新版本而返回 Not Found。

完整模型为：固定容量的 atomic<Entry*> History Index，每次发布分配新的不可变 Entry；Writer exchange 指针、retire 旧 Entry；Reclaimer 根据全体 Hazard Record 决定何时 delete。注意：**固定的是索引 Slot，Entry 对象本身不在该 Slot 内原地覆盖**。它不同于现有固定 Pool Slot + Generation + RefCount CAS Pin。

#### H.2 完整 C++20：hazard_history_seq

独立源文件：[hazard_history_seq.cpp](hazard_history_seq.cpp)。包含以下场景：single writer、四个 Reader、随机按 seq 查找、RAII Pinned、Index Eviction、Retired List 回收、旧版本保护、并发测试。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic \
    hazard_history_seq.cpp -o hazard_history_seq
./hazard_history_seq
~~~

完整源代码如下（不依赖 Folly）：

~~~cpp
// hazard_history_seq.cpp
// g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic hazard_history_seq.cpp -o hazard_history_seq
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

// 1 writer, fixed reader slots, at most 1 Pinned per reader ID.
// Sequence does not wrap or repeat; stop all readers before destruction.
template <std::size_t Capacity, std::size_t ReaderCount>
class SequenceHistory {
    static_assert(Capacity > 0 && ReaderCount > 0);
public:
    struct Entry {
        const std::uint64_t seq;
        const std::uint64_t value;
    };
private:
    struct alignas(64) HazardSlot {
        std::atomic<const Entry*> ptr{nullptr};
    };
    std::array<std::atomic<const Entry*>, Capacity> index_{};
    std::array<HazardSlot, ReaderCount> hazards_{};
    std::atomic<std::uint64_t> latest_{0};
    std::vector<const Entry*> retired_;  // writer-only
    std::uint64_t lastPublished_ = 0;     // writer-only
public:
    class Pinned {
        friend class SequenceHistory;
        SequenceHistory* owner_ = nullptr;
        std::size_t readerId_ = 0;
        const Entry* ptr_ = nullptr;
        Pinned(SequenceHistory* owner, std::size_t id, const Entry* p)
            : owner_(owner), readerId_(id), ptr_(p) {}
    public:
        Pinned() = default;
        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;
        Pinned(Pinned&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              readerId_(other.readerId_),
              ptr_(std::exchange(other.ptr_, nullptr)) {}
        Pinned& operator=(Pinned&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                readerId_ = other.readerId_;
                ptr_ = std::exchange(other.ptr_, nullptr);
            }
            return *this;
        }
        ~Pinned() { reset(); }
        void reset() noexcept {
            if (owner_) {
                // Last dereference must occur before this store.
                owner_->hazards_[readerId_].ptr.store(
                    nullptr, std::memory_order_seq_cst);
                owner_ = nullptr;
                ptr_ = nullptr;
            }
        }
        explicit operator bool() const noexcept { return ptr_ != nullptr; }
        const Entry& operator*() const { assert(ptr_); return *ptr_; }
        const Entry* operator->() const { assert(ptr_); return ptr_; }
    };

    SequenceHistory() = default;
    SequenceHistory(const SequenceHistory&) = delete;
    SequenceHistory& operator=(const SequenceHistory&) = delete;
    ~SequenceHistory() {
        // Join all threads and destroy outstanding Pinned first.
        for (const auto& h : hazards_)
            assert(h.ptr.load(std::memory_order_seq_cst) == nullptr);
        for (auto& loc : index_)
            delete loc.load(std::memory_order_seq_cst);
        for (const Entry* p : retired_) delete p;
    }

    void publish(std::uint64_t seq, std::uint64_t value) {
        assert(seq == lastPublished_ + 1);
        const Entry* fresh = new Entry{seq, value};
        auto& location = index_[seq % Capacity];
        const Entry* old = location.exchange(
            fresh, std::memory_order_seq_cst);
        if (old) retired_.push_back(old); // NEVER delete old immediately
        lastPublished_ = seq;
        latest_.store(seq, std::memory_order_release);
        if ((seq & 31) == 0) collect();
    }
    std::uint64_t latest() const {
        return latest_.load(std::memory_order_acquire);
    }

    Pinned lookup(std::uint64_t seq, std::size_t readerId) {
        assert(readerId < ReaderCount);
        assert(hazards_[readerId].ptr.load(std::memory_order_seq_cst)
               == nullptr && "one Pinned per reader ID");
        if (seq == 0) return {};
        auto& location = index_[seq % Capacity];
        const Entry* candidate;
        do {
            // DO NOT dereference before successful validation.
            candidate = location.load(std::memory_order_seq_cst);
            hazards_[readerId].ptr.store(
                candidate, std::memory_order_seq_cst);
        } while (candidate !=
                 location.load(std::memory_order_seq_cst));

        if (!candidate || candidate->seq != seq) {
            hazards_[readerId].ptr.store(
                nullptr, std::memory_order_seq_cst);
            return {}; // evicted or not published
        }
        return Pinned{this, readerId, candidate};
    }

    // Single writer takes a full hazard snapshot and compacts retired_.
    void collect() {
        std::array<const Entry*, ReaderCount> snapshot{};
        for (std::size_t i = 0; i < ReaderCount; ++i)
            snapshot[i] = hazards_[i].ptr.load(
                std::memory_order_seq_cst);
        std::size_t keep = 0;
        for (const Entry* old : retired_) {
            if (std::find(snapshot.begin(), snapshot.end(), old)
                  != snapshot.end())
                retired_[keep++] = old;
            else delete old;
        }
        retired_.resize(keep);
    }
    // Writer thread only.
    std::size_t retiredCount() const { return retired_.size(); }
};

int main() {
    {
        SequenceHistory<8, 2> history;
        for (std::uint64_t s = 1; s <= 8; ++s)
            history.publish(s, s * 10);
        auto old = history.lookup(3, 0);
        assert(old && old->value == 30);
        for (std::uint64_t s = 9; s <= 11; ++s)
            history.publish(s, s * 10);
        history.collect();
        assert(history.retiredCount() == 1);
        auto missing = history.lookup(3, 1);
        assert(!missing);
        assert(old->seq == 3 && old->value == 30);
        old.reset();
        history.collect();
        assert(history.retiredCount() == 0);
        std::cout << "old-version pin & eviction: PASS\n";
    }
    {
        SequenceHistory<64, 4> history;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> hits{0}, misses{0};
        std::vector<std::thread> readers;
        for (std::size_t id = 0; id < 4; ++id)
            readers.emplace_back([&, id] {
                std::uint64_t rnd = id + 17;
                while (!stop.load(std::memory_order_acquire)) {
                    auto top = history.latest();
                    if (top == 0) continue;
                    rnd = rnd * 6364136223846793005ULL + 1;
                    auto back = (rnd >> 32) % 96;
                    auto wanted = top > back ? top - back : 1;
                    auto pin = history.lookup(wanted, id);
                    if (pin) {
                        assert(pin->seq == wanted);
                        assert(pin->value == wanted * 10);
                        hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        misses.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        for (std::uint64_t s = 1; s <= 50000; ++s)
            history.publish(s, s * 10);
        stop.store(true, std::memory_order_release);
        for (auto& t : readers) t.join();
        history.collect();
        assert(history.retiredCount() == 0);
        auto latest = history.lookup(50000, 0);
        assert(latest && latest->value == 500000);
        latest.reset();
        std::cout << "concurrent lookup: PASS; hits=" << hits
                  << ", misses=" << misses << '\n';
    }
}

~~~

**边界**：单 Writer、每 Reader ID 独占一个 Hazard Slot、一个 Reader ID 同时最多一个 Pinned，线程退出前清 Hazard，History 的析构发生在所有 Reader join 后。Sequence 不回绕、不复用；对象在发布后不可变。Index 脱链与对象内存删除是两个不同时间点。生产实现仍应处理动态 Reader 注册、allocator、异常安全和长时间 pin 引起的内存积压。

#### H.3 为什么 Load → Publish → Validate 正确？

Reader 三步：R1 = location.load; R2 = hazard.store(candidate); R3 = location.load 重新验证。**R3 成功前绝不能 dereference(candidate)**。Writer 两步：W1 = 从 index 中 unlink/replace old；W2 = 在 W1 之后扫描全部 Hazard Records，确认没有人保护才 delete。

典型情况：

- Reader R2、R3 在 Writer W1 前，Reader 可以继续处理旧对象；Writer W2 在保护仍有效时必须看到该 Hazard。
- Writer 已 W1、W2 并释放旧对象后，Reader 仍可以短暂拥有一个过时的地址值，却必须在 R3 验证失败后重试；绝不能解引用。
- Reader Publish 与 Writer Unlink 重叠，可能出现 false-positive 暂缓回收，但不能让成功验证的 Reader 遭遇 UAF。

在这个简化模型中（单一发布入口、无回绕/错误 ABA、Hazard 在最后一次读取之后才清除），若所有相关访问均是 seq_cst，则下列危险交错会产生矛盾：

~~~text
R2 < R3             Reader 顺序
W1 < W2             Writer 顺序
R3 < W1             R3 仍验证旧值 A
W2 < R2             W2 未看见仍有效的 A

=> R2 < R3 < W1 < W2 < R2     不可能
~~~

如果 Writer 扫描时看到 nullptr，也可能是 Reader 已结束访问并清除了 Hazard，这也是安全的。以上简洁证明不能直接替代对多入口、任意地址重用、可动态删除 Hazard Records 的完整算法证明。

Retired List 压缩示意：

~~~cpp
std::size_t keep = 0;
for (const Entry* old : retired_) {
    if (std::find(protectedPtrs.begin(), protectedPtrs.end(), old)
          != protectedPtrs.end())
        retired_[keep++] = old; // 保留
    else
        delete old;             // 本轮可回收
}
retired_.resize(keep);
~~~

protectedPtrs 为 Writer 获取的 Hazard 快照；在演示模型中 Writer 独占 retired_，因此 vector 压缩不需要 CAS。扫描若看到后来已清除的 Hazard，只会延迟释放，不会导致提前释放。

#### H.4 Hazard Pointer 是否必须使用 seq_cst？

**不必所有 Atomic 都是 seq_cst，但是不能把所有操作简单降成 Acquire/Release。**

只用 Reader release-store Hazard + acquire-load Index，和 Writer release-store Index + acquire-load Hazard，允许 Store Buffering：Reader 看见旧 Index，而 Writer 没看见新 Hazard，导致双向遗漏。

最易证明的实现：Hazard 发布、第二次验证、Writer Unlink、Reclaimer Hazard Scan 都采用 seq_cst（完整示例中第一次 Index Load 也选择 seq_cst；它在相应同步协议下通常可以用 acquire）。

可选方案：**配对的 SC fences**，示意协议骨架：

~~~cpp
// Reader:
auto* p = location.load(std::memory_order_acquire);
hazard.store(p, std::memory_order_release);
std::atomic_thread_fence(std::memory_order_seq_cst);
auto* check = location.load(std::memory_order_acquire);

// Writer/Reclaimer, only after old is unlinked:
auto* old = location.exchange(fresh, std::memory_order_acq_rel);
std::atomic_thread_fence(std::memory_order_seq_cst);
auto* observed = hazard.load(std::memory_order_acquire);
~~~

这要求配对 fence、原子修改序以及对象摘链/扫描次序共同满足协议。**这是结构性示意，不是完整的、覆盖全部数据结构与动态 Registry 的独立程序。** 随意只在 Reader 或 Writer 一端增加 fence、把两个 SC fences 都降成 acq_rel，均不能沿用上述正确性证明。

在常见 x86-64 编译器上，seq_cst hazard store 可能使用 locked xchg；release-store + seq_cst fence 可能使用 mov + mfence/locked instruction。两者都有 Store→Load 排序成本，不能声称后者一定更快。

#### H.5 Folly Hazard Pointer、API 与 Asymmetric Fence

官方资源：

- [Folly Hazptr.md](https://github.com/facebook/folly/blob/main/folly/docs/Hazptr.md)
- [Folly Hazptr.h](https://github.com/facebook/folly/blob/main/folly/synchronization/Hazptr.h)
- [Folly AsymmetricMemoryBarrier.cpp](https://github.com/facebook/folly/blob/main/folly/synchronization/AsymmetricMemoryBarrier.cpp)
- [P1202R0: Asymmetric Fences](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1202r0.pdf)

Folly 通过 hazptr_holder / make_hazard_pointer() 封装 Load–Publish–Validate，把 Reader 对 Object 的生命周期保护移到 holder 上。对象从所有可被 HP 查询的发布入口摘链后才能 retire，不能直接并发 delete。Holder 可复用，以摊销获取 Record 的成本。Folly 提供 Domain、Record 管理、批量/可选异步回收等能力。

**Folly API 用法示意（需要安装 Folly，consume 为应用逻辑，并非独立编译的标准库代码）：**

~~~cpp
#include <folly/synchronization/Hazptr.h>
#include <atomic>
#include <cstdint>

struct Entry : folly::hazptr_obj_base<Entry> {
    std::uint64_t seq = 0;
    std::uint64_t value = 0;
};
std::atomic<Entry*> history[1024]{};

void publish(std::uint64_t seq, std::uint64_t val) {
    Entry* p = new Entry;
    p->seq = seq; p->value = val;
    Entry* old = history[seq % 1024].exchange(p, std::memory_order_acq_rel);
    if (old) old->retire();
}

void replayLookup(std::uint64_t seq) {
    auto holder = folly::make_hazard_pointer();
    Entry* p = holder.protect(history[seq % 1024]);
    if (p && p->seq == seq) {
        // process(p->value) within holder's lifetime
    }
}
~~~

**Asymmetric Fence**：Folly 在特定平台/配置下以 Reader 侧轻量同步与 Reclaimer 侧重同步配对，Linux 可以利用进程级 membarrier 等机制。它**不是**通用 C++20 的「用 acquire/release 替换 seq_cst 就同样安全」，更不是随便调用 atomic_signal_fence 就能实现。若必要条件不满足，须走库的有效回退路径。具体 CPU/内核版本下的性能要测量，不能把历史的纳秒 Benchmark 当作本机指标。

Folly 官方文档指出：HP 对具体对象提供细粒度 lifetime protection，RCU 通常保护一个更大的读侧临界区；HP 在对象必须跨等待/阻塞被持有时较有优势，而 RCU/QSBR 若 Reader 长时间不报告进度可能阻塞整个 Domain 的回收。Read-mostly 与 Cold Replay 混合时，要比较 Reader 同步成本、Retired backlog、Writer update 延迟。

#### H.6 与既有 Fixed Slot CAS Pin 比较

| 维度 | Hazard Pointer History | Fixed Slot Generation + CAS Pin |
|---|---|---|
| Object 内存 | 每版本独立不可变 Entry | Pool 中固定 Slot 原地重用 |
| Reader | Publish Hazard + Validate | CAS 同时验证 Seq/Generation + 增 Pin |
| Writer | Exchange index pointer，旧对象 retire | CAS Closing，等待 RefCount 后覆盖 |
| 旧版本 | 索引被覆盖后已 pin 的 Reader 继续访问 | RefCount 未归零时旧 Slot 不可复用 |
| 回收 / 压力 | 对象级回收，可积压 Retired Entries | 固定容量，可能因 pinned slot 阻塞 Writer |
| Replay Lookup | 需 History Index / Retention Policy | 已集成 Seq Lookup |
| 最主要风险 | Hazard ordering、Record 生命周期、内存膨胀 | CAS contention、Writer stall、Generation 回绕 |

**HFT 结论**：你当前的固定 Slot + CAS Pin 并非一定比 HP 慢。HP 可以通过独立分配对象把 Index 更新与删除解耦，但如果仍必须在同一 Pool Slot 原地写入，HP 无法绕开正在被 Reader 使用的旧数据。必要时考虑 Overflow Pool / Immutable Segment / Replay Copy Archive；Benchmark 单 Writer p99.9、Reader protect latency、CAS retries、reclamation backlog 和 Replay 被抢占时的行为。

#### H.7 FAQ

- 已成功 pin 的 Reader 可以继续读旧 Seq；未 pin 的新 Reader 只能从仍存在的 Index/Archive 找到版本。
- Reader 第一次 Load 可能得到之后变悬空的地址；在成功验证前不能解引用、也不能直接访问对象内部 RefCount。
- HP 解决生命周期，不自动保证任意锁自由算法没有逻辑 ABA；按 Seq 查询还应在保护后验证 seq。
- Reader 新注册 Hazard Record 必须先对 Scanner 可见；注销前清理 Hazard，Record 自身也需要稳定生命周期。
- Cacheline、fence、TLS、membarrier、对象分配器、retired 扫描频率与 Reader/Writer 比例共同决定性能；不能只数 Atomic 指令数量。

---

### 附录 I · Epoch-Based Reclamation：Pointer 与 Embedded Ring

Reader 在读取共享对象 **之前**登记 epoch，在最后一次使用 **之后**退出；Writer 对被替换对象登记 retirement epoch。当所有仍活动 Reader 都处于更新后的 epoch 或更晚，旧对象才可回收。

本示例使用稳定的固定槽、全部 seq_cst 排序以清楚展示协议；它不是通用高性能 EBR 库，真实库通常分批 pin、三 epoch、TLS + domain registry。

~~~cpp
// 03_ebr.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <utility>
struct Data { int version; };
int main() {
    std::atomic<Data*> current{new Data{0}};
    std::atomic<unsigned long long> epoch{1};
    // 0 = inactive；非零 = 当前受保护区间对应 epoch
    std::array<std::atomic<unsigned long long>, 2> announced{};
    for (auto& a : announced) a.store(0);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 30000; ++n) {
            auto e = epoch.load(std::memory_order_seq_cst);
            announced[id].store(e, std::memory_order_seq_cst);
            Data* p = current.load(std::memory_order_seq_cst);
            assert(p && p->version >= 0);
            announced[id].store(0, std::memory_order_seq_cst);
        }
    };
    std::thread a(reader, 0), b(reader, 1);
    go.store(true, std::memory_order_release);
    struct Retired { Data* p; unsigned long long token; };
    std::vector<Retired> retired;
    auto collect = [&] {
        auto out = retired.begin();
        for (auto item : retired) {
            bool safe = true;
            for (auto& slot : announced) {
                auto e = slot.load(std::memory_order_seq_cst);
                if (e != 0 && e < item.token) safe = false;
            }
            if (safe) delete item.p;
            else *out++ = item;
        }
        retired.erase(out, retired.end());
    };
    for (int v = 1; v <= 1000; ++v) {
        Data* old = current.exchange(new Data{v},
                                     std::memory_order_seq_cst);
        auto token = epoch.fetch_add(1, std::memory_order_seq_cst) + 1;
        retired.push_back({old, token});
        if (v % 16 == 0) collect();
    }
    a.join(); b.join();
    collect();
    assert(retired.empty());
    delete current.load();
}
~~~

**为什么安全**：Reader 的 epoch 公布先于它的 Data load；Writer 的 pointer exchange 先于推进 epoch。任何真正读到旧对象的 Reader，必然在回收之前已公布旧 epoch，除非它已经退出。新 Reader 若晚于 exchange 才开始，只能读新指针。此教学实现没有动态注销；进程级的 Reader State 必须在 Scanner 停止访问后才能释放。

**成本与风险**：一次 epoch pin 可覆盖临界区多个节点；被抢占的旧 Reader 能拖住整个回收域，retired memory 可能无界增长。

#### I.1 EBR 保护历史 Sequence：原地复用固定 Ring Slot（完整 C++20）

独立源码：**[ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp)**。这是与前一个 atomic<Entry*> + new/delete EBR 示例不同的版本：内存中始终保留固定的 Slot 数组，每个 Slot 直接包含普通非原子 Data；Writer 周期性复用 Slot，**不 delete 任何对象**。这是本项目 Replay Ring 的更贴近实际的模型。

~~~cpp
template <std::size_t Capacity>
struct Ring {
    struct Slot {
        std::atomic<uint64_t> publishedSeq{0}; // 0: invalid
        Data data; // in-place, non-atomic
    };
    Slot slots[Capacity];
};
~~~

**Reader：Pin 新 Epoch 后查询任何仍保留的旧 Seq**

~~~cpp
auto guard = ring.pin(readerId);             // 先发布保护区间
auto* old = guard.lookup(wantedSeq);          // seq % capacity
if (old) process(*old);                      // 不要求创建于当前 Epoch
// old 不得逃逸出 guard 的生命周期
~~~

只要 Slot 当前仍装有目标 Seq（例如最新 Seq 12，Capacity=8，可查 Seq 5–12），Reader 即使 guard.epoch() 已经大于 old->createdEpoch，也可以安全读取。Epoch **不是查询起始 Seq、不是 Slot 版本，也不是对象创建时刻的过滤器**。

**Writer：先 Invalidate，再推进 Epoch，再等待 Grace Period，最后原地覆盖**

~~~cpp
auto& slot = slots_[seq % Capacity];
auto oldSeq = slot.publishedSeq.exchange(0, std::memory_order_seq_cst);
if (oldSeq != 0) {
    auto threshold =
        epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
    waitForReadersBefore(threshold); // active old epochs must leave
}
slot.data = Data{seq, value, epoch_.load(std::memory_order_seq_cst)};
slot.publishedSeq.store(seq, std::memory_order_seq_cst);
~~~

waitForReadersBefore(threshold) 在演示中依次检查所有固定 Reader Record 是否为 Inactive (0) 或大于等于 threshold。**Invalidate 必须先于 Epoch Advance**，否则进入新 Epoch 的 Reader 仍可能从旧 Slot 读到旧 seq，并被 Reclaimer 错误跳过。这里采用全部关键操作 seq_cst 的保守协议，且只有单 Writer；不是可直接替换为 acquire/release 的生产库。

安全原因：

- 已经 Pin 旧 Epoch、并从 Slot 取得旧 seq 的 Reader，会阻止 Writer 完成对应 Grace Period；Writer 在 Guard 结束前不得写入 slot.data。
- Writer 完成 Invalidate+Advance 后，进入新 Epoch 的 Reader 不可能再经该 Slot 成功取得旧 Seq。
- Reader 无需 HP 式 Pointer Revalidation；它仅需检查 Slot 的 publishedSeq == wantedSeq，而且只能在 Guard 内访问 Data。
- **没有 delete，不等于没有并发安全问题**：直接在旧 Reader 访问 slot.data 时覆盖会产生 C++ data race/UB。必须先完成 Grace Period。

源码 main() 包含三项验证：(1) Pin 较新的 Epoch 读取较旧 Seq；(2) Writer 复用被持有的 Slot 时必须等待，旧 Reader 仍可安全使用已经取得的指针，但新的 lookup 返回 Not Found；(3) 4 个 Reader 并发随机查询、Writer 连续发布。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_ring_history_replay.cpp -o ebr_ring_history_replay
./ebr_ring_history_replay
~~~

**正确性与 HFT Trade-off**：

- 查询支持任意「尚在 Ring 保留窗口」的历史 Seq；**已经被覆盖的历史不能靠 Epoch 恢复**，更长时段需要独立 Archive/更多容量。
- Reader 的一个 Guard 可以访问多个旧 Slot，但慢 Reader 可能阻止 Writer 覆盖**任何**仍受同一 Epoch 保护的 Slot，即使该 Reader 实际上只看过某一个。
- 本例为单 Writer + 固定 Reader 组 + 无 seq/epoch 回绕 + seq_cst + Writer 同步等待；它没有做到 Writer wait-free / lock-free，不适合无条件照搬到 ultra-low-latency Writer 热路径。
- 实际上线还需处理 Reader 注册/注销、停止/崩溃、Guard 逃逸、Writer stall/backpressure、异常、CPU pinning 和超时恢复。对于固定 Pool + 随机 Replay，原有 **CAS Pin + Generation** 可以做到更精确的逐 Slot 保护；而 EBR 用更粗粒度的 Grace Period 换取 Reader 侧较少的逐对象操作。


#### I.2 EBR 核心概念与 Replay 需求总结

**EBR 的 Epoch 是 Reader 进入保护区间的时间标记，不是 Seq、Slot Generation、对象 Creation Epoch，也不是 Epoch→Pointer/Slot 的登记表。**

| 概念 | 回答的问题 | 典型记录 |
|---|---|---|
| Seq / Generation | 现在索引到的是否是我想要的数据？ | slot.seq == wantedSeq |
| History Retention | 旧 Seq 是否仍然可被新的 Reader 查到？ | Ring Capacity、Archive Index、Eviction |
| Epoch Pin | 已经查到对象的 Reader 能否安全继续访问？ | readerEpoch[id] = e |
| Retirement/Reclamation Threshold | Writer 何时可以 delete 或重写旧对象？ | retired.ptr / threshold；或 Slot 的 reuseThreshold |

**因此，Reader 可以 Pin 最新 Epoch 100，然后访问在 Epoch 20 创建的 Seq 1000。** 前提是该 Seq 仍在可查询的 History Index/Ring 中，并且之后的 Reuse/Retirement 遵循同一个 EBR 协议。Epoch 的值不能作为历史 Seq 的查询下界，也不需要把 Reader Epoch 回退到对象的 Creation Epoch。

EBR 没有维护精确的 Reader → Slot 映射。一个 Reader Pin Epoch 10，实际只读 Slot 3，却可能阻止 Writer 回收多个其他 Slot。这是保守的 **Temporal Protection（时间区间保护）**。

**三条不变量**：

1. 先正确 Pin Epoch、后取得被保护对象；最后一次使用后才能 Unpin。
2. Writer 先让旧对象/旧版本不能被新的 Reader 取得（unlink / invalidate），再发布相应的 Grace Period Threshold。
3. 在 Grace Period 成功完成前，不能 delete 旧对象，也不能原地覆盖仍可能被 Reader 访问的非原子 Data。

#### I.3 模型 A：Pointer-based History EBR（完整程序）

[ebr_history_seq.cpp](ebr_history_seq.cpp) 是完整的、可独立编译的 C++20 示例：固定大小 atomic<Entry*> History Index、每次发布不可变 Entry、单 Writer、多 Reader、RAII Guard、按 Seq Random Lookup、Retired List、Batch Lookup、并发测试。

简化的接口使用：

~~~cpp
auto guard = history.pin(readerId);
const auto* old = guard.lookup(wantedSeq); // 创建时间不受 guard.epoch 限制
if (old) process(*old);                    // Guard 退出前使用
~~~

Writer 发布与退休：

~~~cpp
const Entry* old = index_[seq % Capacity].exchange(
    new Entry{seq, value}, std::memory_order_seq_cst);

const auto threshold = globalEpoch.fetch_add(
    1, std::memory_order_seq_cst) + 1;

if (old) retired.push_back({old, threshold});
// 以后扫描 readers，满足 Grace Period 时 delete old
~~~

这里 Writer 可以先发布新 Entry、再**延迟删除**旧 Entry；即使慢 Reader 持有旧对象，Writer 通常仍可更新 Index（但 Retired Memory 可能积压，内存分配也有成本）。

Reader 无需 HP 的 Load→Publish Hazard→Reload：因为 Epoch Pin 发生在 Pointer Load 之前。**但仍要验证 Entry::seq == wantedSeq**，因为同一个 Index 可能被下一圈 Sequence 覆盖。

#### I.4 模型 B：固定 Ring Slot 原地复用（完整程序）

[ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp) 是第二份完整 C++20 程序：固定大小的 Slot 数组，每个 Slot 内嵌普通 Data，**没有 Entry new/delete**，Writer 按 Seq 循环复用 Slot。

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0}; // 0 == INVALID
    Data data;                              // 非原子数据
};

// Reader
auto guard = ring.pin(readerId);
const Data* p = guard.lookup(wantedSeq);
if (p) process(*p);

// Writer：旧 Slot 已被发布时
slot.publishedSeq.exchange(0, std::memory_order_seq_cst);
auto threshold = globalEpoch.fetch_add(
    1, std::memory_order_seq_cst) + 1;
waitForGracePeriod(threshold);
slot.data = newData; // 只有经过 Grace Period 才能写入
slot.publishedSeq.store(newSeq, std::memory_order_seq_cst);
~~~

**固定 Ring 的重要转折**：Slot 不删除并不等于无需回收协议。复用旧 Slot 就是在破坏其旧 Data 的存储内容，如果有 Reader 同时读写非原子 Data，仍然是 C++ Data Race / UB。相比 Pointer-based 方案的延迟 delete，固定 Ring 需要**延迟覆盖**。

例：Capacity=8，最新 Seq=12；Reader Pin Epoch=5，依旧可以读取 Seq=5（即使这个 Data 在 Epoch=1 创建）。Writer 发布 Seq=13 会复用 Seq=5 的 Slot：必须 Invalidate Seq 5、推进 Epoch=6，并等待旧 Reader 退出，再写新 Data。在等待期间 Reader 已取得的旧 Data 指针仍安全，**新查 Seq 5 可以返回 Not Found**。

**不能假设 latest=12 就保证 [5,12] 每个 Seq 都随时可读**：并发 Writer 可以先 invalidate 最旧 Slot，再等待 Grace Period，期间最新已完成发布 Seq 仍是 12，但 Seq 5 查询可能已经失效。Retention Window 是容量/策略的约束，不是当前瞬间全窗口无缺口的 Snapshot 保证。

#### I.5 为什么 Reader 的新 Epoch 可以保护旧数据？SC 时序证明

下例专指本项目教学实现的单 Writer、固定 Reader、关键操作全部 seq_cst 的协议；它与经典 3-Epoch EBR 的算法细节不同。

~~~text
Reader                                Writer
R1  load(globalEpoch) -> e           W1  invalidate old slot
R2  readerEpoch.store(e)             W2  advance globalEpoch -> T
R3  load(slot.publishedSeq)          W3  scan readerEpoch[]
R4  dereference slot.data            W4  overwrite only after grace period
R5  readerEpoch.store(0)
~~~

对 Reader 已经通过 R3 读到旧 Seq 且尚未结束 Guard 的情况：

- R3 看到旧 Seq → 在 SC 总序中 R3 < W1。
- 如果 Reader 公布的新 Epoch e >= T，则 W2 < R1。组合 W2 < R1 < R2 < R3 < W1 < W2，形成循环，不可能。
- 如果 Reader 公布的旧 Epoch e < T，Writer 仍想在 W3 错过它并看到 0，那么 Reader 未退出时 W3 < R2。组合 R2 < R3 < W1 < W2 < W3 < R2，也不可能。
- 如果 Reader 已经完成 R5，Writer 看到 Inactive 是安全的。若 Reader 在 W1 之后才 Load，它会看到 INVALID 或新版 Seq，无法取得旧 Seq。

因此 **旧对象不是在创建时绑定某个 Epoch**。真正的安全保证是：当 Writer 要删除或覆盖对象时，必须与所有可能仍持有旧版本的 Reader 正确完成 Grace Period。

回收条件，在本项目示例定义的 Retirement/Reuse Threshold T 下：

~~~text
Safe(T) = all readers satisfy:
          (readerEpoch[i] == 0) OR (readerEpoch[i] >= T)
~~~

真实 EBR 常使用三 Epoch、批量 Epoch 进展或更复杂的 Pin/Unpin 机制，回收公式不得直接移植。动态 Reader 注册、Slot 重用、Epoch/Seq wrap、异步回调指针逃逸等都需要额外证明。

#### I.6 EBR 的 Validate、Snapshot 和 Memory Ordering

**Validate**：

- HP：Reader 必须在公布某个 Hazard Pointer 后重新验证 Pointer，排除 acquire 与保护发布之间的 UAF 窗口。
- EBR：Reader 先建立保护区间再取 Pointer，因此不需要 HP 式二次 Pointer Validate。
- Replay 无论采用 EBR 还是 HP，都需要 **Seq/Generation 身份检查**；这不是生命期保护的二次验证。

**Snapshot**：Epoch Guard 并不锁定整个 Ring 的逻辑快照。一个 Guard 读取多个 Slot，Writer 可以在它们之间更新索引，出现旧版/新版混合或 Not Found。若要完全一致的历史切片，需要 Snapshot Version、不可变 Snapshot、MVCC 或 Replay Consistency Protocol。

**Memory Ordering**：

- 本项目的代码将重要操作设置为 seq_cst，方便使用一致的全局顺序证明。
- 不能未经证明就把 Pin Store 改为 Release、Pointer Load 改为 Acquire、Scanner Load 改为 Acquire；两个线程可能因 Store Buffering 互相错过公告和更新。
- **并非所有 EBR 实现都必须全部 seq_cst**。成熟 EBR 可用 Acquire/Release 加配对 SC Fences / RMW 或其他已验证协议；Unpin 通常可能采用更弱排序。但务必根据完整算法证明，不能从某一句“EBR 不需要 SC”直接推导具体实现安全。
- 示例每次 Publish 后推进 Epoch 仅为方便教学；真正高吞吐 EBR 通常按批次/Grace Period 推进，避免每条消息都执行 Global Epoch RMW。

#### I.7 Snapshot Lock 类比与固定 Ring Trade-off

**像锁之处**：固定 Ring 全满且 Writer 必须原地覆盖 Data 时，一个被抢占的老 Epoch Reader 可能使 Writer 的下一次 Slot 复用无法完成，甚至它实际读取的是完全不相关的 Slot。对于这种存储布局，Writer **发布新 Data** 和 **回收旧 Slot 使用权** 是同一个操作的两个阶段。

**不像锁之处**：EBR 不提供整个 Ring 的一致性 Snapshot，不禁止 Writer 修改一切不相关的状态；Writer 可以先推进 Epoch，或在空 Slot 写入，也可以在独立 Entry 模型中发布新 Pointer 并延迟释放旧对象。

| 维度 | Pointer-based EBR | In-place Ring EBR | Generation + RefCount CAS Pin |
|---|---|---|---|
| Writer 更新时 | 通常先 Exchange 新对象 | 先 Invalidate，等待 Grace Period | CAS Close，等这个 Slot 的 Pin 归零 |
| 保护粒度 | Epoch 临界区，间接保留多个 Entry | Epoch 临界区，影响多 Slot 复用 | 精确 Slot/Generation |
| 新对象内存 | new / Pool / Segment | 无 Data new，原地覆盖 | 固定 Pool，原地复用 |
| 慢 Reader | retired backlog 可增长 | 容易阻塞顺序 Writer | 阻塞对应 Slot；Writer 最终遇到它也会停 |
| Replay Batch | 一次 Pin 可以读多个 Entry | 一次 Pin 可读多个 Slot | 通常逐 Slot CAS Pin |
| Random Replay | 需要 Index + Seq 检查 | 需要 Seq 检查 | Generation 验证通常并入 CAS |

对于已有 **Selective Fan-out + Fixed Ring + Random Replay** 架构：Live Reader 优先考虑目标 Reader Completed Cursor；Cold Replay 单条随机 Seq，继续以稳定 Slot Metadata 的 Generation + RefCount CAS Pin 为基线；若选择 EBR，应测量老 Guard 停顿对 Writer p99.9/max 的影响。真正希望 Writer 不等待 Replay，通常需要额外的不可变 Snapshot、Overflow Pool 或独立 Copy Archive，而不仅是更换回收算法。

#### I.8 两份完整 C++20 示例及验证记录

本章节有两套可以**独立复制、编译、执行**的程序，而非只有省略了 EpochDomain 的伪代码：

- [ebr_history_seq.cpp](ebr_history_seq.cpp)：Pointer-based History Index，独立不可变 Entry，延迟 delete；测试多个 Seq 退休后的指针仍安全、4 Reader 并发 Batch Lookup。
- [ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp)：Slot 内嵌 Data，Invalidate → Advance → Wait → Overwrite → Publish；测试 **读取早于 Reader Epoch 创建的旧 Seq**、Writer 阻塞直到 Guard 退出、4 Reader 并发随机历史查询。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_history_seq.cpp -o ebr_history_seq
./ebr_history_seq

g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_ring_history_replay.cpp -o ebr_ring_history_replay
./ebr_ring_history_replay
~~~

**本次更新验证**：GCC 14.2 两份程序按以上配置编译、各运行 12 次均通过；额外用 ASan + UBSan 各运行 3 次未发现错误。测试不构成形式化的 C++ Memory Model 证明，本次未执行 TSAN/Model Checking。仅作为教学和 Benchmark 起点，不应未经额外审查用于生产交易系统。

**延伸阅读**：Crossbeam Epoch [官方文档](https://docs.rs/crossbeam-epoch/latest/crossbeam_epoch/)、DPDK [QSBR Programmer Guide](https://doc.dpdk.org/guides/prog_guide/rcu_lib.html)、C++ [Memory Order Reference](https://en.cppreference.com/w/cpp/atomic/memory_order.html)。


#### I.9 EBR 典型应用与 Pointer / Embedded Storage 的真正区别

本轮讨论的最终结论：**EBR 并非“只适合 Pointer、不适合 Embedded Data”；决定优势能否发挥的是：新对象的发布（Publication）能否与旧存储的回收／复用（Reclamation/Reuse）解耦。**

在允许新旧版本并存的设计中，Writer 可以先发布新版本，让已经获取旧版本的 Reader 继续安全使用旧对象；等到 Grace Period 结束再释放或重用旧存储。EBR 的读侧开销还能摊销到临界区内的多个对象访问。它管理**生命周期**，不替代并发容器的算法正确性、可见性协议或全局逻辑快照。

#### 5.9.1 典型应用

| 场景 | EBR 如何工作 | 优势与风险 |
|---|---|---|
| Lock-free Linked List / Skip List / Tree | Unlink 动态节点，延迟释放/复用 | Reader 在一个 Guard 内走过多个节点；仍需独立解决 CAS/逻辑 ABA |
| Concurrent Hash Map | Erase/Replace/Rehash 后延迟回收 Node 或整张旧 Table | 读侧避免每节点 RefCount；Rehash 语义需另外证明 |
| Immutable Config / Routing Table / Instrument Metadata | 建立新版，原子替换根 Pointer，Retire 旧版 | 高频读、低频写、版本不可变；需考虑复制和内存峰值 |
| Lock-free Queue 动态节点 | Dequeue/Unlink 后延迟回收 Node | 消除节点的 UAF；不自动保证 Queue 的线性化或 ABA 安全 |
| Preallocated Object Pool | 新版本写到别的 Free Slot，旧 Slot 经过 Grace Period 后回 Pool | 不需要每次 new/delete，但必须有空闲容量 |
| 固定 Slot Ring 原地覆盖 | 旧 Seq invalidated 后，Writer 等待 Grace Period 才能覆盖旧 Data | 正确但限制 Writer 前进，通常不是 EBR 最大优势的场景 |

**相关而非等同**：网络和 Packet Processing 中的 RCU/QSBR 也处理读多写少的数据结构、延迟回收与 Grace Period；例如 DPDK QSBR 的 Reader 报告 Quiescent State，与本页的简化 EBR Reader Epoch 算法不是同一个实现。

#### 5.9.2 案例 A：Pointer-based Immutable Publication

下面是生命周期 API 的**示意片段**，需要与同一 Epoch Domain 的真实 pin/retire 实现配套；不是可单独编译的 EBR 库。

~~~cpp
std::atomic<const Config*> current;

// Reader：Guard 期间的 *整个* Config 版本不可变
auto guard = epoch.pin(readerId);
const Config* cfg = current.load(std::memory_order_acquire);
use(*cfg);  // 最后一次解引用必须在 Guard 结束之前

// Writer：
const Config* next = buildImmutableConfig();
const Config* old = current.exchange(next, std::memory_order_acq_rel);
epoch.retire(old);  // 不是立即 delete
~~~

核心并非“用了 Pointer 就必然安全”，而是 **current 作为发布入口**与 **old、next 分别占用的内存**互相独立。Reader 在 Guard 内可以完成很多基于同一 cfg 的查询，Writer 也可以继续构造与发布新版本。

EBR 不保证多个独立 Pointer Load 得到同一时间点的数据。若要求同一次业务计算使用一致的 Config，通常只在 Guard 内 Load 一次 cfg，并从这个不可变 Root 访问所有关联字段。

#### 5.9.3 案例 B：Embedded Data 仍然可以配合 EBR（预分配 Pool）

~~~cpp
struct PoolSlot {
    Data data;   // Embedded, non-atomic
};
std::array<PoolSlot, 4096> pool;

// 假设：Free List 仅包含已经通过 Grace Period 的 Slot。
// Reader：先 Pin，再从原子 Index 得到 PoolSlot*，然后访问 Data。
// Writer：取另一个安全空闲的 PoolSlot，写 Data，原子发布新地址，
//         将旧 PoolSlot 退休，未来 Grace Period 结束才回 Free List。
~~~

假设 Reader 正在访问 pool[10]，Writer 可以在 pool[20] 初始化新版并发布 Index；pool[10] 即使被移出 Index，也不能在旧 Guard 结束前复用。这没有每条 Data 的 new/delete，却与不可变 Pointer 版本一样实现了 **Publication/Reclamation 解耦**。

如果 Pool 的空闲 Slot 耗尽，Writer 也会遇到 Stall、Overflow 或 Backpressure；EBR 本身不创造额外容量。

#### 5.9.4 案例 C：Fixed Ring 的 seq % Capacity 带来硬约束

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0};
    Data data; // Embedded, non-atomic
};

Slot& slot = ring[nextSeq % Capacity];
// Writer 不能任意选择另一块尚未退休的内存存放 nextSeq
~~~

在 Ring 满载、每条新 Seq 都必须原地复用已发布 Slot 的前提下：

~~~text
Pointer / Free-Pool:
  construct B elsewhere
    -> publish B
    -> retire A
    -> recycle A after Grace Period

Fixed In-place Ring:
  invalidate A
    -> wait Grace Period
    -> overwrite the same Slot with B
    -> publish B
~~~

此时 Writer 的“更新已发布 Data”和“回收旧 Slot 的使用权”是**同一次发布流程不可分的阶段**。旧 Reader 读取的普通 Data 与 Writer 的覆盖绝不能并发发生，否则是 C++ data race / UB。固定地址没有被 free，也无济于事。

**Pointer 不是本质条件**：Pool Slot 可以是嵌入式对象，只要不同版本能使用不同地址，就仍适合 EBR。反过来，即使用 Data* 指向固定 Ring，Writer 直接重写该地址，也不会自动获得 Pointer-based EBR 的好处。

同样须区分：
- Writer 推进 Global Epoch：通常不需要等 Reader；
- Writer 写入从未发布的新空 Slot：不需等待旧对象的 Grace Period；
- Writer 换指针发布独立新版：可以延迟回收旧对象；
- Writer 原地覆盖已发布 Slot：需先保证旧 Reader 都已离开相应受保护区间。

#### 5.9.5 “像 Snapshot Read Lock”——准确的部分与不准确的部分

Reader Pin 的 Epoch 不是快照版本，不会冻结全 Ring；Reader 可能在同一 Guard 内看到来自不同发布时间点的 Slot，甚至看到已经失效的 Seq。

但对已满的固定 Ring 而言，**复用被旧 Guard 可能访问过的 Slot**必须等待 Grace Period，所以一个很慢或被 OS 抢占的 Reader 可能令 Writer 在尝试复用**无关的 Slot**时也阻塞；这是 EBR 的粗粒度、时间区间保护成本。多轮 Writer 发布最终可能被同一慢 Guard 阻碍，尤其在单 Writer 必须按 Seq 顺序提交时。

~~~text
Ring Slots:  [Seq 100] [Seq 101] [Seq 102] [Seq 103]
Reader:       Pin Epoch 10; 实际只读 Seq 100

EBR：
    Writer 要覆盖 Seq 101 的 Slot，也可能被该 Reader 的旧 Epoch 阻止。

Per-Slot CAS Pin：
    Writer 一般可覆盖未被 Pin 的 Seq 101 Slot；
    真正遇到被 Pin 的 Seq 100 Slot 才需要等它的 RefCount 归零。
~~~

不过 CAS Pin 也不保证 Writer 永不阻塞：严格顺序发布时，最终遇到真正被 Pin 的 Slot 仍须等待、溢出或丢弃，取决于业务约束。CAS Pin 必须同时正确处理 Slot Generation、Closing/禁止新 Pin、RefCount 和复用时的 Memory Ordering，不能只用一个孤立的 RefCount++ 代替完整获取保护的协议。

#### 5.9.6 选型原则与 HFT 场景对应

| 场景 / 约束 | 倾向方案 |
|---|---|
| Read-mostly Config、Instrument Static Data、Venue Routing、ACL | Immutable Root + EBR/QSBR/RCU 风格延迟回收 |
| Concurrent List / Hash Node，多对象短时间遍历 | EBR 值得评估，仍要验证容器算法 |
| 预分配 Object Pool，有足够安全的备用 Slot | EBR + Retire/Free List 可行 |
| Fixed Ring，严格按 seq % Capacity 原地覆写 | 更倾向 Generation + RefCount CAS Pin |
| Replay 单条按 Seq 随机查询，热路径尾延迟敏感 | 更倾向 Per-Slot CAS Pin；测量 CAS 竞争及慢 Reader |
| Selective Fan-out Live Reader 交付与完成确认 | Per-Target Completed Cursor / ACK，不直接与 Replay Pin 混为一谈 |
| 长时间 Replay、历史跨度超过 Ring Capacity | Copy/Segment Archive 或额外不可变存储 |

建议的设计判断顺序：

1. **是否允许新旧版本共存于不同存储？** 如可以，EBR 的发布与回收解耦很有价值。
2. **Reader 能否短时 Pin、并在一个 Guard 内访问多个对象？** 如可以，EBR 的成本更易摊销。
3. **慢 Reader 是否允许推迟回收，并可能消耗额外内存／Pool 容量？** 如不允许，优先更精确的保护和有界方案。
4. **核心目标是 Reader 吞吐还是 Writer p99.9/max？** 在 Fixed Ring 上尤其应测长 Guard、Reader 被抢占、Grace Period Stall 和备用容量用尽。

**最终结论：EBR 更适合可以延迟复用旧存储的架构；Pointer-based 只是常见表达形式，并非必要条件。对本项目 Fixed Ring + Embedded Data + Random Replay，Per-Slot Generation + RefCount CAS Pin 通常更贴切。** 这是一条有充分架构依据的选型倾向，**不是**“只要可以解耦就一定该用 EBR”的数学充分必要条件；真实表现仍依赖缓存、NUMA、竞争、Guard 长度和实测尾延迟。

---

### 附录 J · QSBR：Quiescence 与 Grace Period

不一定逐次 pin/unpin。Reader 在明确的事件边界报告：当前 **没有任何之前取得的旧版本指针仍在使用**。Writer 在替换 pointer 后发出 grace-period token，等待各相关 Reader 报告 token 或更晚。

~~~cpp
// 04_qsbr.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
struct Config { int version; };
int main() {
    std::atomic<Config*> current{new Config{0}};
    std::atomic<unsigned long long> token{1};
    std::array<std::atomic<unsigned long long>, 2> quiescent{};
    for (auto& q : quiescent) q.store(0);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 30000; ++n) {
            Config* p = current.load(std::memory_order_seq_cst);
            assert(p && p->version >= 0);
            // 在此之后绝不再使用 p
            auto observed = token.load(std::memory_order_seq_cst);
            quiescent[id].store(observed, std::memory_order_seq_cst);
        }
        // 最终 QS：线程完成后不会再访问 current 的旧对象
        quiescent[id].store(token.load(std::memory_order_seq_cst),
                            std::memory_order_seq_cst);
    };
    std::thread a(reader, 0), b(reader, 1);
    go.store(true, std::memory_order_release);
    struct Retired { Config* p; unsigned long long grace; };
    std::vector<Retired> retired;
    auto collect = [&] {
        auto out = retired.begin();
        for (auto item : retired) {
            bool safe = true;
            for (auto& q : quiescent)
                if (q.load(std::memory_order_seq_cst) < item.grace)
                    safe = false;
            if (safe) delete item.p;
            else *out++ = item;
        }
        retired.erase(out, retired.end());
    };
    for (int v = 1; v <= 1000; ++v) {
        Config* old = current.exchange(new Config{v},
                                       std::memory_order_seq_cst);
        auto grace = token.fetch_add(1, std::memory_order_seq_cst) + 1;
        retired.push_back({old, grace});
        if (v % 16 == 0) collect();
    }
    a.join(); b.join();
    // 上面的终态报告可能发生在 writer 最后一个 token 之前；
    // join 保证两个 Reader 已退出，不可能继续访问旧对象。
    for (auto item : retired) delete item.p;
    delete current.load();
}
~~~

**生产版必须实现**：register/online/quiescent/offline/unregister 的正确同步，Reader offline 前没有逃逸指针，后台回收、不让 busy-poll 快路径扫描 retired list。Reader 停止后可以通过 join/注销将其从参与者集合移除，不应让过期的 token 永久阻塞。切勿在仍被异步回调使用时报告 QS。

#### J.1 EBR vs QSBR：共同目标与核心区别

**两者非常接近，不是两套完全不同的回收思想。** EBR 和 QSBR 都是用 Grace Period 判断：从共享数据结构移除的旧版本，是否已经不可能再被某个 Reader 继续使用。区别主要在于 Reader 如何声明**保护区间**与**安全进度**。

| 问题 | EBR（典型 Pin/Unpin 模型） | QSBR（典型 Online/QS/Offline 模型） |
|---|---|---|
| Reader 进入保护 | 显式 Pin：读取并宣布一个 Epoch | Online 后成为持续参与回收检查的 Reader |
| Reader 表示使用结束 | Unpin：声明不再使用当前 Guard 的任何旧对象 | QS：声明截至该安全点，之前获得的受保护引用均已结束 |
| Reader 空闲 | Unpin 后为 Inactive，通常不阻止新 Grace Period | Online 但不继续 QS，后续 Grace Period 可能一直等待 |
| 一个 Batch 多次 Lookup | 一个 Guard 包住 Batch | Online 后直接 Lookup，Batch 结束报告一次 QS |
| Writer 何时可以回收 | 相关旧 Epoch Reader 都已退出或进入足够新的保护 Epoch | 相关 Online Reader 都已经越过目标 QS，或合法进入 Offline |
| Writer 可先发布 Immutable 新版 | 可以 | 可以 |
| 回收进度受谁影响 | 最慢的仍 Pin 的 Reader | 最慢的 Online 且未报告足够新 QS 的 Reader |
| 固定 Ring Embedded Data 原地覆盖 | 先 Invalidate，再等待 Grace Period，之后才能写旧 Slot | 同样需要 |
| 典型集成方式 | RAII Scoped Guard、通用容器 | 长驻 Busy-Poll Worker、天然 Event/Batch 边界 |

**重要术语修正**：

- Online **类似“开始长期参与读保护”**，但不等价于 EBR Pin 一个具体 Epoch：Online 是 QSBR Reader 的参与状态，报告 QS 后仍保持 Online。
- Offline **类似退出读侧保护域**，但必须先确认没有任何旧引用继续使用，并正确执行协议中的注销/在线状态同步。
- QS **逻辑上类似“结束上一段保护区间，然后继续参加下一段”**，可以用“Unpin + Pin”的思想来理解，但它并不真的原子地执行这两步，更不保证每次都递增 Global Epoch。
- 两者都**不是 RWLock 或一致性快照锁**：Writer 仍可以发布新 Pointer；保护的是旧对象生命期，而不是冻结所有数据。

#### J.2 两段对称的核心代码：完全相同的 Immutable Data 发布模型

公共数据（下面是展示协议的核心代码片段，不包含完整 Reader 注册、异常处理及线程关闭。完整 C++20 EBR 示例见前文 ebr_history_seq.cpp；本节开头还给出了可运行的固定 Reader QSBR 例子）：

~~~cpp
struct Data {
    uint64_t seq;
    uint64_t value;
};

constexpr auto SC = std::memory_order_seq_cst;
std::atomic<const Data*> current{nullptr};

// 假设一个 Writer，两个固定 Reader，Seq/Epoch/Token 均不会回绕。
// Data 发布后不可修改。process 不保存指针给异步回调。
~~~

**代码 A：EBR。一次处理周期显式 Pin → Lookup → Process → Unpin。**

~~~cpp
std::atomic<uint64_t> globalEpoch{1};
std::array<std::atomic<uint64_t>, 2> activeEpoch{};
// activeEpoch[i] == 0 表示该 Reader 不在保护区间

void ebrRead(size_t id) {
    // 对同一 Reader ID，禁止并发 Pin 或重叠 Guard
    auto e = globalEpoch.load(SC);
    activeEpoch[id].store(e, SC);          // Pin；必须先于 Pointer Load

    const Data* p = current.load(SC);
    if (p) process(*p);

    activeEpoch[id].store(0, SC);          // Unpin，最后一次解引用之后
    // 生产代码应使用 RAII Guard，保证所有退出路径都会 Unpin
}

void ebrPublish(const Data* next) {
    const Data* old = current.exchange(next, SC); // Unlink old
    const auto threshold = globalEpoch.fetch_add(1, SC) + 1;

    // 忽略 absent old 的回收；下面为简化选择同步等待
    bool safe = false;
    while (!safe) {
        safe = true;
        for (auto& r : activeEpoch) {
            const auto e = r.load(SC);
            if (e != 0 && e < threshold) safe = false;
        }
    }
    delete old;                            // Grace Period 已完成
}
~~~

真实生产实现通常 Retire 后延迟/批量回收，而不是每次 Publish 同步等待。这个版本的关键 Memory Ordering 依赖全部 SC；不可随意改为仅 Acquire/Release。

**代码 B：QSBR。一次 Online，重复 Lookup → Process → Report QS，最后 Offline。**

~~~cpp
std::atomic<uint64_t> globalToken{1};
std::array<std::atomic<uint64_t>, 2> passedToken{};
// 此处仅演示两个事先注册且始终 Online 的固定 Reader。
// 对动态 Online/Offline 必须使用完整 QSBR 库的状态协议。

void qsbrWorker(size_t id, std::atomic<bool>& running) {
    // 在本固定 Reader 模型下，启动前已成为 Online 参与者
    while (running.load(std::memory_order_acquire)) {
        const Data* p = current.load(SC);
        if (p) process(*p);

        // 真实 QS：先结束所有旧引用，再报告已越过的 Token
        const auto t = globalToken.load(SC);
        passedToken[id].store(t, SC);
    }
    // 退出前：加入 Offline / Unregister 协议，或由 Writer join。
}

void qsbrPublish(const Data* next) {
    const Data* old = current.exchange(next, SC); // Unlink old
    const auto threshold = globalToken.fetch_add(1, SC) + 1;

    // 两个固定 Reader 在 Writer 运行期间必须一直报告 QS
    bool safe = false;
    while (!safe) {
        safe = true;
        for (auto& p : passedToken) {
            if (p.load(SC) < threshold) safe = false;
        }
    }
    delete old;                            // Grace Period 已完成
}
~~~

**两段 Writer 的形状几乎相同：Exchange → Start Threshold → Wait/Collect → Delete。** 真正不同的是各自 Reader Record 的含义：

- EBR 的 activeEpoch 保存 Reader **当前正处于哪个保护区间**；0 表示明确不活跃。Writer 可以忽略 Inactive Reader。
- QSBR 的 passedToken 保存 Reader **已经越过哪个安全边界**。Reader 仍 Online 时，旧 Token 只能证明过去的某次 QS，不能证明当前新启动的 Grace Period 已经完成。

注意：代码 A 和 B 使用相同的 current 只是为**独立对照**；它们是两种互斥的示例运行方式，不应把两个回收者同时挂到一个 Pointer 上。QSBR 的 Online/Offline/Registration 并发同步、Retired List 批处理、关闭及异常安全在真实系统中不可省略。

#### J.3 “EBR 临时读锁；QSBR 持续参与、不断滑动安全边界”

这是有用的**心理模型**，但不是标准意义上的锁。

~~~text
EBR:
  Inactive --Pin(10)--> [Read A] --Unpin--> Inactive
  Inactive --Pin(11)--> [Read B] --Unpin--> Inactive
            ^显式开始              ^显式结束

QSBR:
  Offline --Online--> [Read A] --QS(10)--> [Read B] --QS(11)--> ... --Offline
                      ^       ^安全边界前移；仍然 Online
                      长期参与 Writer 的 Grace Period 检查
~~~

可将 QS 想象为 **结束上一段旧引用的保护，并将 Reader 已安全越过的时间边界向前推进**。但是 QS 调用后，Reader 仍然 Online；下一次 Lookup 无需重新 Online。

**Writer 的典型等待场景**：

~~~text
时间         EBR Reader                  QSBR Reader                 Writer
T1           Pin Epoch 10                Online / Read A
T2           Read A                      Process A                   Exchange A -> B
T3           Process A                   Process A                   Start threshold 11
T4           Unpin (= Inactive)          QS(11)                      Both: grace done
T5           Sleep 10s                   Sleep 10s (still Online)    Start threshold 12
T6           不阻止新 GP                 不再报告 QS                 EBR 可继续；QSBR 等待
T7           --                          QS(12) / Offline            QSBR GP 才能完成
~~~

严格说，T4 的 QS(11) 必须是在 Writer 发布 Token 11 之后、且 Reader 不再使用之前取得的 Data 时报告。**QS(10) 即使刚发生，也不能满足未来才启动的 Token 11。** Writer 等的是后续安全点，而不是“此 Reader 曾经调用过 QS”。

对于 EBR，在 T5 Reader 已经 Unpin 的情况下，它睡眠期间不阻止 T5 之后启动的 Grace Period；QSBR Reader 若保持 Online 且不再报告 QS，则可能一直阻止后续回收，哪怕它实际没有读取任何对象。若预计阻塞很久，应在不保留引用时 Offline，并在恢复 Lookup 之前重新 Online。

#### J.4 QS 后再次 Lookup 到同一个 Data：合法，但必须重新获取引用

~~~text
Reader                              Writer
Lookup A; Process A
QS(10)
Lookup A; Process A again            // A 如果仍在 Index，则完全合法
QS(10 or newer)

Writer 要回收 A：
    Unlink A -> Start Token T -> 等待所有相关 Reader 的 QS >= T -> Reclaim A
~~~

**QS 是时间边界，不是对象销毁信号。** 如果 A 仍可从 History Index 获得，可以在不同 QS 区间重复查到同一个 A。若 Writer 已经 Unlink A，新 Lookup 不允许再获得旧版 A；此前取得的指针只可持续使用到当前保护区间结束。

错误做法：

~~~cpp
const Data* p = history.lookup(100);
use(*p);
quiescent(readerId);
use(*p); // 错误：已跨越 QS，旧指针不再有生命周期保护
~~~

正确方式是 QS 之后通过受同一回收协议约束的 Index **重新 Lookup、重新检查 Seq/Generation**。在重复查询之间，Writer 可能已经淘汰旧 Seq，此时返回 Not Found 很正常。EBR 中 Unpin 以后重新 Pin 再 Lookup 也是同样逻辑。

**历史查询是否成功**依然分两层：

~~~text
HistoryAvailable(seq)   // Index / Ring / Archive 仍保留该 Seq
AND
LifetimeProtected(seq)  // 当前 Guard 或未跨 QS 的区间能安全访问
~~~

EBR/QSBR 只解决后者，不凭空保存历史。

#### J.5 固定 Ring Embedded Data：QSBR 与 EBR 有相同的基本限制

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0};
    Data data; // 非原子
};
auto& slot = ring[seq % Capacity];
~~~

对于 Writer 的下一圈原地覆盖：

~~~text
EBR:  Invalidate Old Seq -> Advance Epoch -> Wait old Guards -> Overwrite
QSBR: Invalidate Old Seq -> Start Token  -> Wait new QS     -> Overwrite
~~~

两者都不能在 Reader 仍可能读旧 Data 时覆写普通 Data。**没有 delete，不代表不需要防止复用竞态。** 两者都属于粗粒度时间保护，可能因某个不访问目标 Slot 的慢 Reader 而阻塞 Writer。QSBR 只是更容易在 Busy-Poll Loop 的 Batch 边界报告 QS，并不将保护粒度变为单 Slot。

| 对本项目的工作负载 | 首选方向与理由 |
|---|---|
| 常驻 Busy-Poll Worker + Immutable Pricing Config / Routing Table | QSBR：一次 Online，按稳定的 Batch/Loop 边界 QS；需测量 Reader 抢占与 Grace Period 的尾延迟 |
| 不规则函数调用、短 RAII 临界区、通用并发动态节点容器 | EBR：Pin/Unpin Scope 明确，异常/提前退出更易约束 |
| Fixed Ring + Embedded Data + Random Seq Replay | Per-Slot Generation + RefCount CAS Pin：保护具体 Slot/Generation，减少无关 Reader 的阻塞 |
| 长跨度 Replay、Writer 不能等待 | 独立 Immutable Archive / Copy / Segment + 明确的容量和回收策略 |

**性能不要机械地认为 QSBR 一定更快。** 简化 SC 模型中 EBR 一次操作通常包含 Pin Global Load、Pin Record Store、Unpin Store；QSBR 每批 QS 通常有 Global Token Load、Progress Store。但 EBR 同样可以整 Batch Pin 一次，成熟库可能优化 Memory Ordering；真正要量的是吞吐、Reader cache miss、Writer p99.9/max、长期 Online 不报告 QS、Worker 被 OS 抢占与 shutdown 时的回收进度。

#### J.6 工程注意事项与延伸

1. **Online 并不是实际持有 RWLock**，且不能把 Quiescent 当作普通的计数器更新；QS 前必须真实结束对受保护旧对象的引用。
2. **QSBR 不要求不停调用 QS**：只在有新的 Grace Period 时需要足够新的安全点才能推进回收；长期 Online 不 QS 的 Reader 会使相关回收积压。
3. **Writer 的发布与回收不总是一起阻塞**：Immutable Pointer 版可以发布新 Pointer，然后异步退休旧对象；Fixed Ring 原地覆盖则需要在重写之前等待。
4. **线程上线/下线可能和 Token 推进并发**：不能单纯用一个 bool online 数组搭配普通 acquire/release 就声称安全，须采用已验证的 QSBR Online/Offline/Registration 协议。
5. **Memory Ordering**：这里 SC 片段供证明与教学。真实 DPDK QSBR、liburcu QSBR 等有各自的屏障与 Token/Registration 规则，不应把上述教学片段直接替换进生产系统。
6. **来源与进一步阅读**：DPDK RCU QSBR Programmer Guide https://doc.dpdk.org/guides/prog_guide/rcu_lib.html；liburcu https://liburcu.org/；Crossbeam Epoch https://docs.rs/crossbeam-epoch/latest/crossbeam_epoch/。

---

