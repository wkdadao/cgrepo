# Real-Time Risk Infrastructure — 系统设计与高性能 C++ 工程手册

> R1–R13 综合学习笔记 | Senior / Staff C++ Engineer | Multi-Asset Trading / Hedge Fund / Risk Infrastructure
>
> 整理日期：2026-11-27。本文是系统设计学习材料，不是任何具体机构（包括 Squarepoint）的内部架构说明。所有 SLA、吞吐、硬件、成本与业务案例均为教学假设，除非另有注明。代码为说明核心算法与不变量的示例，未经生产级编译、并发验证或性能测试。监管规则、交易协议与金融模型必须以实际合同、市场规范和批准的模型为准。

## 目录

- [0. 总体设计原则](#r0)
- [R1. Real-Time Position & PnL Engine](#r1)
- [R2. Incremental Risk Calculation & Aggregation](#r2)
- [R3. Risk Sharding & Multi-Threading Architecture](#r3)
- [R4. Real-Time Risk State Distribution](#r4)
- [R5. Distributed Consistency & Transaction Semantics](#r5)
- [R6. Event Sourcing、Snapshot、Replay & Recovery](#r6)
- [R7. Trade Capture & Trade Lifecycle](#r7)
- [R8. Inventory & Locate Management](#r8)
- [R9. Market Risk、Greeks、VaR & Stress Testing](#r9)
- [R10. Credit Risk & Limit Management](#r10)
- [R11. Sequencer & Replicated State Machine](#r11)
- [R12. Performance Engineering & Capacity Planning](#r12)
- [R13. Full Risk Infrastructure System Design](#r13)
- [附录 A. 面试与实战路线](#appendix-a)
- [附录 B. 参考资料](#appendix-b)

相关专题：[Pre-Trade Risk and Risk Gateway](Pre-Trade%20Risk%20and%20Risk%20Gateway.md)。本文保留其独立地位，不替换该笔记。

<a id="r0"></a>
## 0. 总体设计原则

Risk Infrastructure 的主要挑战不是单次计算的最低延迟，而是同时保证金融语义、数据完整性、授权正确性、计算新鲜度、吞吐量、尾延迟及恢复能力。

| Plane | 职责 | 关键约束 |
|---|---|---|
| Authorization | Pre-trade checks、Credit/Inventory reservation | 不重复授权、低延迟、可恢复 |
| Calculation | Position、PnL、Greeks、VaR、PFE | 增量计算、准确性、版本一致性 |
| Distribution | Subscription、Snapshot、Fan-out | 新鲜度、慢消费者隔离 |
| Control & Recovery | Limit、Kill switch、HA、Replay、Reconciliation | 明确生效边界、fencing、审计 |

设计顺序：Business Invariant → Ordering Domain → State Ownership → Mathematical Reduction → Processing DAG → Logical Processor Scheduling → Physical Deployment → Recovery & Validation。

- Single writer 是每个 mutable state 的所有权原则，不是全系统只能一个线程。
- 风险状态的本地副本是信息，不等于独占资源授权。
- 先聚合可加的 sufficient statistics，再计算不可加的指标。
- 不把不同业务维度强行放进一个全局事务、一个 Sequencer 或一个线性 Pipeline。
- 不可丢失的经济事件与允许 conflation 的派生状态必须分离。
- 先减少计算、通信和共享状态，再优化 CPU 指令。

<a id="r1"></a>
## R1. Real-Time Position & PnL Engine

### 1.1 业务状态与计算口径

Position 表示经济净仓位；Working Order、Pending Settlement、Borrowed Inventory 与 Position 并不是同一个概念。需要明确交易方向、数量单位、价格单位、合约乘数、估值币种、费用、业务时间。

采用 signed quantity：买入为正，卖出为负。简化线性产品，忽略费用、贴现与汇率转换：

$$
Q=\sum_i q_i,\quad C=-\sum_i q_i p_i,\quad PnL=C+Qm
$$

其中 C 是累计经济现金流（并非一定已实际交割），m 是 mark。市场变化只需更新 $Q(m_{new}-m_{old})$；新成交的总 PnL 增量为 $q(m-p)$。

| Event | Quantity | Cash flow | Mark | Total PnL |
|---|---:|---:|---:|---:|
| Buy 100 @ 10 | 100 | -1000 | 12 | 200 |
| Sell 40 @ 11 | 60 | -560 | 12 | 160 |
| Mark moves to 13 | 60 | -560 | 13 | 220 |

Realized PnL 与 Unrealized PnL 的拆分取决于成本核算规则。WAC（weighted average cost）与 FIFO 的总 PnL 在相同成交、mark 及费用口径下相同，但已实现/未实现分配不同。

### 1.2 Position State Machine（WAC）

同方向增加仓位：更新加权成本；反方向成交：先平掉 min(|Q|, |q|)，计算 realized，再判断平仓或反转。完全平仓时成本清零；反转后的剩余仓位成本为新成交价格。支持 Long / Short。

~~~cpp
struct Position {
    int64_t qty = 0;
    double avg = 0;
    double realized = 0;
};

void onFill(Position& p, int64_t q, double px) {
    if (q == 0) return;
    auto old = p.qty;
    auto next = old + q;
    if (old == 0 || (old > 0) == (q > 0)) {
        p.avg = (double(old) * p.avg + double(q) * px) /
                double(next);
    } else {
        auto closed = std::min(std::abs(old), std::abs(q));
        p.realized += double(closed) * (px - p.avg) *
                      (old > 0 ? 1.0 : -1.0);
        if (next == 0) p.avg = 0;
        else if ((old > 0) != (next > 0)) p.avg = px;
    }
    p.qty = next;
}

double unrealized(const Position& p, double mark) {
    return double(p.qty) * (mark - p.avg);
}
~~~

前提：数量、加法及 abs 均不会溢出（排除 INT64_MIN），输入价格有限且有效，单 owner 调用，已完成成交去重。生产实现需明确精度、合约乘数、币种、费用与原子持久化边界。

WAC 每笔普通成交 O(1)；FIFO 用 lot deque，平仓 O(k)（被消耗的 lot 数），空间 O(open lots)。历史 correction 可能改变之后所有 lot matching，不能简单用反向成交修复 realized。

### 1.3 FX、Swap 与时间维度

EUR/USD BUY EUR 1M @ 1.1000，对买方是 +1M EUR、-1.1M USD；银行作为对手方向相反。跨 EUR/USD、USD/JPY 可按币种净额聚合，但必须保留 value date、legal entity 与结算权限。

FX Forward 的简化 USD PV：
$$
PV=Q_{EUR}SDF_{EUR}-Q_{USD}DF_{USD}
$$
真实估值考虑 collateral currency、cross-currency basis 等。Swap 的 near/far 现金流必须保留日期；NDF 有 fixing date/rate/source、settlement currency 等。

### 1.4 Market Data Fan-out

每个 tick 更新 K 个 position 和 D 个风险维度，朴素 O(KD)。可选择：

| Method | Tradeoff |
|---|---|
| Eager valuation | 每个消费者及时看到结果，但 fan-out 大 |
| Lazy valuation | mark 更新 O(1)，读取时计算 |
| Aggregate before valuation | 兼容线性产品先汇总 quantity、cost、realized，再估值 |
| Hybrid | 热门、关键指标即时维护，其他按需计算 |

同一估值条件下 firm instrument PnL = sum(C)+m*sum(Q)，单一 instrument 的总 PnL 更新 O(1)。但不能把 gross exposure、account-specific risk、路径依赖指标错误地净额合并。

Position shard 按 Book+Instrument；valuation 可按 Instrument；aggregation 按 Book/Strategy/Portfolio。跨线程使用带 source shard、epoch、seq、key、quantity/cashflow delta 的消息。

### 1.5 Correction、精度、恢复与测试

- Bust 对线性 Q/C 可撤销原 contribution；历史 WAC/FIFO/realized 要按指定顺序局部 replay。
- 独立 Trade 与 MD 的最终线性状态可交换，不代表中间 breach、drawdown、realized 都相同。
- Fixed-point 用于数量、货币、风险额度时需定义 rounding、scale、overflow；浮点估值需误差容限、稳定求和（Kahan/pairwise）、定期 full reconciliation。
- Snapshot 必须包含对应输入 offset、epoch、reference/model version；去重和 state 具有同一恢复边界。
- Property test：Q=sum(q)，TotalPnL=realized+unrealized=C+Qm；随机 long/short/flip、partial close、duplicate、bust、late fill；与独立 reference implementation 对照。

**面试：** 如何处理一百万 position、一百万 tick/s？回答重点是先做经济状态建模、预聚合与依赖索引，再分片、减少数据搬运，最后优化 CPU。

<a id="r2"></a>
## R2. Incremental Risk Calculation & Aggregation

### 2.1 风险函数与分类

Risk = F(Position, MarketData, ReferenceData, Model)。先判断指标性质：

| Type | Example | Incremental approach |
|---|---|---|
| Additive | Net quantity、PV、factor exposure | new-old delta |
| Nonlinear reduction | Gross、netting floor | 先聚合再 transformation |
| Derived | Concentration、average | sum/count 等 sufficient statistics |
| Path-dependent | Drawdown、FIFO PnL | 维护顺序、必要时 replay |
| Model-based | VaR、PFE、ES | 场景/因子聚合后计算 |

$$
A_{new}=A_{old}+x_{new}-x_{old}
$$

多维度更新 O(D)。Gross exposure 必须定义聚合层级：long 100、short 80，book gross=180，instrument net=20，firm 对净仓位取绝对值为20。更新 gross 使用 |new|-|old|，不是 |delta|。

Average 可维护 sum/count；max 用 multiset/indexed heap/segment tree（更新 O(log N)）；concentration 的 denominator 变化可能影响所有 ratio，可按需计算；drawdown 的历史峰值不能只从当前 PnL 得到。

### 2.2 多维度与依赖图

Book、Desk、Strategy、Instrument、Currency、Value Date、Counterparty、Factor 组合的笛卡尔积很大。1000 books × 100k instruments × 100 currencies × 365 dates = 3.65e12 个潜在组合。只 materialize 真实需要的 sparse view；明确层级 DAG 的归属，避免共享子节点被重复统计。

~~~mermaid
flowchart TD
    MD[Market Data Factor] --> IDX[Reverse Dependency Index]
    IDX --> I[Dirty Instruments]
    T[Trade Delta] --> P[Position State]
    P --> V[Valuation]
    I --> V
    V --> B[Book View]
    V --> F[Factor View]
    V --> C[Currency View]
    B --> PORT[Portfolio Aggregation]
~~~

使用 dense ID、CSR adjacency、版本化 immutable index、dirty flags、topological scheduling。成本与受影响节点/边和实际计算成本相关，而不是全部市场状态规模。

### 2.3 Covariance-based incremental VaR

令 exposure vector w，covariance matrix Σ（对称）：
$$
V=w^T\Sigma w,\quad u=\Sigma w
$$
$$
V'=V+2\Delta w^Tu+\Delta w^T\Sigma\Delta w,\quad
u'=u+\Sigma\Delta w
$$

单因子 k 的变化 d：variance 调整为 2du[k]+d²Σ[k,k]，但更新 u 仍需 O(N)。Dense storage O(N²)；完整计算 O(N²)。低秩模型见 R9。

Scenario-based risk：维护 portfolio scenario PnL vector；每个 compatible instrument 的 unit PnL vector 预计算，一次 quantity delta 更新 O(S)。聚合场景之后再用 nth_element 提取 VaR，不能直接加各 book 的 VaR。1M × 1000 doubles = 8GB；共享相同经济条件的 unit valuation、分块聚合可以减少存储。

### 2.4 Delta、Absolute、Hierarchical Aggregation

Delta 消息小，丢一条会导致后续状态永久错误；absolute per-source contribution 可用 new-old 转换增量，并通过版本拒绝旧更新。Absolute 能恢复收到的最新值，但不能保证最后一条丢失被发现，也不恢复历史 breach；需要 heartbeat、snapshot、replay、tombstone。

跨 shard 的 book reclassification 可能产生暂时的 0 或双倍 exposure。Firm view 可直接消费原经济事件（内部转移 no-op），或用事务边界/一致快照聚合。

Local aggregation + batching 可降低消息数量。32 producers 各100K/s，假设每32条同类消息合并为1条，则理论从3.2M/s降至100K/s；恒定速率下第一条等待310µs（必须凑满时）。若涉及路径依赖事件不能这样合并。

### 2.5 性能与面试

1M ticks/s × 100 positions × 5 dimensions = 500M updates/s；若合法预聚合为10个单位，则50M/s。先数学降维 → incremental → sharding → batching → locality → SIMD。

**面试：** 怎样维护 gross/net、最大值、VaR？何时 delta 安全？如何处理 book transfer？为什么数据正确不代表所有中间风险决策正确？

<a id="r3"></a>
## R3. Risk Sharding & Multi-Threading Architecture

### 3.1 三种并行性

- Stateful parallelism：按业务所有权拆分可变状态。
- Compute parallelism：immutable input 上的纯计算可动态调度。
- Pipeline parallelism：不同 stage 处理不同批次事件。

| Architecture | Pros | Cons |
|---|---|---|
| Shared worker pool + shared state | 灵活 | 锁、CAS、cache-line bouncing、事务复杂 |
| Horizontal sharding | 单 owner、局部性 | hot shard |
| Dedicated vertical pipeline | stage isolation | 固定线程产生容量碎片 |
| Sharded multi-pipeline / DAG | 多维度可独立扩展 | repartition、queue、排序成本 |
| Logical processor multiplexing | 灵活物理映射、资源复用 | 调度公平、长任务干扰、恢复复杂 |

Single writer 管理普通 mutable fields；跨线程仍需正确的 release/acquire 或消息传输协议。

### 3.2 按业务不变量选择 key

Instrument key 适合 valuation，Book+Instrument 适合 position，Netting Set 适合 credit，Inventory Pool 适合 locate。不要为了 hash 均衡牺牲业务不变量。不同 stage 不同 key，必要时 repartition；可以在 owner 兼容时 fuse stage。

### 3.3 Pairwise SPSC vs MPSC vs MPMC

P producers × C consumers 的 SPSC matrix：O(PC) queues，单 consumer polling O(P)。MPSC 每 consumer 一个 queue，producer 有共享竞争；MPMC 更适合独立纯计算任务，不天然保证 key ownership/order。32×32×4096×64=256 MiB（含自连接）；排除自连接为248 MiB。

Polling 使用 bounded round robin、weighted quantum、active queue hint，防止 drain-until-empty starvation。控制消息优先级必须服从业务依赖。不要用无界 spin 等待下游 queue 空位，避免循环依赖 deadlock。

### 3.4 Hot shard、Virtual Shard 与 Migration

平均利用率40%可能掩盖单 shard 100%。按每 key 的 service demand（arrival rate × cost）而不是 key count 做 weighted placement。多个 hot keys 可 rebalance；单一严格串行 hot key 必须重新分解 state、提前聚合或减少计算，不能简单增加线程。

Virtual shard 映射：key → logical shard → physical worker。在线迁移的概念协议：

1. Router freeze old route，buffer new input。
2. Old owner drain through barrier N；停止修改。
3. Transfer state、dedup、offset、pending work。
4. New owner install & ACK。
5. Commit routing epoch，fence old owner。
6. Deliver buffered events after N。

多 producer 需要 offset vector 或统一有序 cut；迁移中的 crash 要由 durable coordination 恢复。Consistent hashing 只是 placement，不是迁移协议。

### 3.5 Work stealing 与版本化计算

Steal computation, not mutable ownership。Task 携带 immutable market/position/model version；owner 验证结果，再更新权威 state。持续行情变化可能造成计算一直被判 stale，需 dirty flag、合并、bounded staleness、unit PV 复用或定期一致快照。

NUMA：first-touch、per-node pool、局部 market replica、本地 aggregation，减少跨 socket cache-line 转移。巨大页面降低 TLB pressure，但需评估碎片及实际 workload。

共享 price state：两个 atomic bid/ask 不保证一致 pair；普通非原子 payload 的 seqlock 在标准 C++ 中仍可能 data race。RCU、immutable snapshots、QSBR 需要安全 reclamation；double/triple buffer 不自动保证读者退出后才复用。

### 3.6 Logical-Processor-Based, Deployment-Configurable Stage-Multiplexed Pipeline

这是讨论中提出的描述性架构名称，并非行业标准术语。

~~~mermaid
flowchart TD
    DAG[Logical processing DAG] --> LP[Stage + Shard + Ownership]
    LP --> MAP[Deployment configuration]
    MAP --> T0[Thread 0: S1/P0 + S2/P0]
    MAP --> T1[Thread 1: S1/P1 + S3/P0]
    MAP --> T2[Thread 2: S2/P1 + S3/P1]
    T0 <-->|SPSC| T1
    T1 <-->|SPSC| T2
    T0 <-->|SPSC| T2
~~~

每个 thread 可以拥有多个 logical processors；通信按 physical thread pair 或 sparse stage channels。自连接使用 bounded direct dispatch 或 local ready queue。优先执行较高 stage 的 ready task，限制 quantum，避免 starvation；同一 FIFO 内不能无条件越过旧消息，应采用 local demux 或 per-stage channel。

~~~cpp
while (running) {
    pollIngressBounded();
    for (auto stage : descendingStages)
        runReadyTasks(stage, stageBudget[stage]);
    serviceOverdueTasks();
}
~~~

依赖满足是 runnable 的前提，priority 不能违反 sequenced business order。迁移时保持单 owner。R11/R12 进一步讨论 determinism 与 placement。

**面试：** 如何解决 hot shard、cross-NUMA、SPSC matrix 开销？什么时候共享 compute pool，什么时候固定 owner？为什么同一队列的 FIFO 不等于整个系统的确定性？

<a id="r4"></a>
## R4. Real-Time Risk State Distribution

### 4.1 Event Stream vs State Stream

| Consumer | Semantic requirement |
|---|---|
| Gateway hard limit | 有效独占授权及必要的及时状态 |
| Trading strategy | 当前 exposure 与明确 freshness |
| Breach monitor | 不丢关键 breach transition |
| Dashboard | 可 conflation 的 latest state |
| Audit / recovery | 完整业务事件与可重放记录 |

例如 10→15→10，latest state 合并会丢掉中间15的 breach。完整事件与 latest-state publication 分离。同步 RPC 查询风险不阻止 TOCTOU；本地复制也不等于 reserve。

### 4.2 Selective fan-out

Topic/key → interested subscribers 的 reverse index；稠密 subscriber ID 可用 bitmap，遍历 set bits（countr_zero）而不是扫描所有 consumer。复杂度与订阅数量 K 相关；topic 粒度影响状态量和流量。单写者管理订阅，或使用 immutable routing index + RCU。

Aeron IPC/UDP、Solace、SPSC/shared memory、RPC 均可作为通信候选。Transport offer 成功、archive recorded、durable committed、business applied 是四种不同确认。

Shared broadcast ring 可供多个 reader 读取同一日志，但需处理最慢 reader、覆盖及 flow control；shared state table 适合随机读取但 notification 和生命周期更复杂。

### 4.3 Snapshot-to-live protocol

~~~mermaid
sequenceDiagram
    participant C as Consumer
    participant S as Risk Publisher
    C->>S: Subscribe
    S-->>C: Subscription confirmed
    S-->>C: Live events (buffer)
    S-->>C: Snapshot at cut N
    Note over C: Install snapshot; discard buffered <= N
    Note over C: Apply required contiguous events > N
    S-->>C: Live updates
~~~

需要明确 subscription 已经生效、snapshot cut、buffer capacity、gap recovery 与 staging install。Multi-input offset vector 不自动意味着一致 cut；跨依赖的 in-flight channel state 也可能需要处理。

Sequence（stream progress）、per-key version（state revision）、owner epoch（fencing）、schema/model version（计算语义）必须分离。Selective fan-out 下 global sequence 跳号可能正常，不能误判为漏包。按 subscriber stream 或明确可达 key 的进度判断 gap。

### 4.4 Conflation 与 Backpressure

每个 key 保存最新状态与 pending/dirty 标记；发布失败不清除 dirty。单线程 owner 可以在下一次 flush 重试。若已发版本与最新版本不同，应继续保留 pending。对 delta 求和只适合可加且不需中间路径的语义。

有限 buffer、持续过载时，无法同时无阻塞、无丢失、有限内存。恒定速率下 buffer 耗尽时间 = capacity/(producer-consumer)。慢 UI 与权威事件隔离，权威流可 journal/replay；必要时拒绝新授权。市场 tick 是否可合并取决于模型是否依赖每个 tick。

### 4.5 Freshness & subscription warming

指标区分 source time、ingress、calculate、publish、receive、apply。跨主机需 PTP 与误差界定；heartbeat 应携带真实 source/calculation progress，单独心跳不证明数据在推进。

首次订阅可能需要注册、snapshot、catch-up，RTT 明显大于 warm lookup。预订阅热资源；状态机 Unsubscribed → Subscribing → SnapshotLoading → CatchingUp → Ready / Stale / Recovering。未 ready 不默认风险为零。

两个 gateway 都看到10M，分别接受7M，得到14M。正确做法是 centralized atomic reserve 或 exclusive distributed budgets。详见 R5。

**面试：** 为什么不能每笔订单同步 RPC？如何实现 snapshot + live？怎么处理 dropped delta、慢 consumer、失效 epoch 和 stale risk？

<a id="r5"></a>
## R5. Distributed Consistency & Transaction Semantics

### 5.1 一致性层级

Memory safety → Thread safety → Local atomicity → Distributed consistency → Business invariant。前四层正确不自动推出第五层正确。

固定非负可加额度：Used+Reserved ≤ Limit。行情驱动的真实 exposure 可以被动突破 limit；系统不能篡改已发生交易来强行满足 invariant。

Linearizability：单操作表现为在调用与返回之间某一点生效，并尊重 real-time order。Sequential consistency 不要求真实时间顺序。Serializability 针对事务；strict serializability 还要求 real-time order。Snapshot isolation 可能出现 write skew（两个 book 各40，分别从同一个80总量快照增加15，最终110>100）。Eventual consistency 不适合作为硬授权保证。

### 5.2 CAS 与多字段 transaction

~~~cpp
bool tryReserve(std::atomic<int64_t>& available, int64_t q) {
    if (q <= 0) return false;
    auto old = available.load(std::memory_order_relaxed);
    while (old >= q) {
        if (available.compare_exchange_weak(
                old, old-q, std::memory_order_relaxed))
            return true;
    }
    return false;
}
~~~

这是单一独立 counter 的 check-and-reserve，CAS success 是 linearization point；relaxed 只对该 counter 的数值 invariant 足够，不发布关联数据，也不保证持久化、reservation identity 或 external send。

seq_cst load + fetch_sub 仍不是一个原子 check-and-reserve。先 fetch_sub 再回滚可能暂时负数。

多个 limit 的 CAS 分别预留，全部成功才发单、失败精确回收，可保守地控制独立上界，但并非原子多字段事务：会暴露部分预留、误拒绝及 crash recovery 问题。128-bit CAS 只适合可打包状态，C++ 不保证 lock-free，需检查 is_lock_free。Immutable pointer CAS 还涉及 ABA/reclamation。复杂状态可考虑 mutex、single owner、co-location。

Single writer 两阶段遍历 sparse charge vector：先验证所有约束，再修改，O(K)；必须合并重复 limit ID、检查 overflow、保证第二阶段无失败或可回滚，并对 state/ledger/commit 建立恢复语义。

### 5.3 Cross-shard transactions

Book A 转100到 B，中间观察可能得到0或200。可选：
- Co-locate 需要原子修改的 state。
- 同一事务事件 + staging + commit visibility。
- MVCC consistent view。
- Firm view 直接消费原始经济事件，内部转移 no-op。
- 2PC：prepare 持久化并保留资源；coordinator 持久化决定；commit/abort 幂等完成。prepared participant 不可随意超时回收；2PC 会阻塞，隔离另需定义。Consensus 复制决定，不自动代替跨 shard 事务。

### 5.4 Distributed Escrow / Hierarchical Budget

100M limit 分给 Gateway A=40M、B=40M、central=20M，每 gateway 可进一步分给线程各20M。正常固定授权模型，对每个资源维度：
$$
U+R+\sum_i A_i+B+T=L
$$
U=used，R=reserved，A=local unconsumed exclusive rights，B=central unallocated，T=in-transit unavailable。所有项非负、不能重复统计父子额度。

本地 A→R（reserve），R→U（fill），R→A（confirmed cancel）。可加且固定口径才可直接拆分。Negative signed exposure 可能需要上下两个约束；VaR、PFE 等非线性指标须使用保守上界或集中授权。

Refill watermark ≈ peak consumption rate × refill latency + safety margin，还需考虑最大单笔 request。异步 refill 提高利用率但仍有碎片。

Transfer protocol：durable freeze/revoke old owner → ACK → durable grant new owner → dedup same transfer ID。中间额度保持 unavailable，不能在旧 owner 未被阻止时重新授予。Lease 到期不等于 fencing；必须在真正发单边界 enforcement（session/network/process ownership 等），外部 venue 不一定理解内部 epoch。

### 5.5 Order lifecycle & exactly-once effect

Reserve before send；partial fill 把相应 reserve 转 used；cancel request 不释放，只有终态确认才释放未成交部分。Replace 若不能保证旧单与新单不共存，需要覆盖两者。示例：100额度 reserve20，fill8，cancel12，最后 available92/used8。

Crash after send before response：状态不确定，不能盲目重发或释放。使用 durable order intent、stable client order ID、outbox、venue dedup/reconciliation。Exactly-once delivery 不等于 exactly-once business effect；dedup identity + state + result 需要相同 commit boundary，重试参数不同应检测 conflict。

Network partition：有效的独占 escrow 可在明确授权条件内继续使用；无法获得 quorum 的全局变更不能假装成功。Limit reduction、market exposure 上涨是不同于重复授权的 breach，应单独记录与控制。

### 5.6 Consistent snapshot

同一个 snapshot ID 不代表跨 shard 一致。需要 barrier/offset、in-flight state、MVCC 或可重放的共同权威日志。Aligned barrier 等待各输入；unaligned 捕获 channel state；不同独立 view 可各自 checkpoint。

测试：并发 oversubscription、duplicate grant/release、partial transaction、crash every step、partition、old epoch、limit reduction；对有限状态模型用 TLA+ 等验证守恒与 safety。

**面试：** 多个 CAS 是否等于 transaction？为什么最新风险状态仍无法阻止超额？如何设计 per-thread budget transfer 与 fencing？

<a id="r6"></a>
## R6. Event Sourcing、Snapshot、Replay & Recovery

### 6.1 Durability、Recovery 与 Determinism

Authoritative trade/order/grant/reservation ledger 与 derived position/Greek/aggregation 分开。Event sourcing 保存业务历史；WAL 记录用于恢复的写入，不一定等同完整业务事件。

WAL before commit。ACK 必须声明是收到、记录、持久化、quorum commit 还是业务执行成功。仅当权威输入完整且 deterministic 时，下游 derived state 才可不逐条持久化。

### 6.2 Journal 设计

Binary event header：sequence、epoch、timestamp、type、schema version、payload length、checksum。避免直接 dump C++ struct ABI（padding/endian/version）。使用 segmented append-only file、preallocation、index、commit boundary、tail checksum。

Linux write 通常仅进入 page cache；fsync/fdatasync 才提供相应同步语义，mmap/msync、O_DIRECT、io_uring 均不天然等价 durability。新建或 rename 文件时还需要考虑父目录 fsync。Storage/controller 故障模型需明确。

Group commit：假设 sync cost T、batch B，理想 amortized sync cost T/B，吞吐受存储和其他资源限制；凑批等待增加延迟。Dedicated journal thread 不会消除 durable-before-send 的关键路径等待。

Replay 必须记录所有决策输入：market/ref/config/model、time/random、merge ordering。区分业务确定性、数值容差与 bitwise determinism。Replay 抑制外部副作用，pending outbox 按原 identity 恢复。

### 6.3 Checkpoint / Snapshot

~~~mermaid
flowchart LR
    IN[Authoritative log] --> SM[State owner]
    SM --> SNAP[Snapshot at offset N]
    IN --> REPLAY[Replay after N]
    SNAP --> REC[Recovery state]
    REPLAY --> REC
~~~

Snapshot 包括 positions、cost、realized、working order、reservations、budget、dedup、offsets、pending transaction、timer、epoch、version。Snapshot at seq101 却标记100会重复应用101。

| Method | Tradeoff |
|---|---|
| Stop-the-world | 简单，pause 大 |
| Copy then async write | copy 占带宽，2GB/20GBps≈100ms |
| Chunk CoW | 额外内存、复杂回收 |
| MVCC | 多版本与生命周期管理 |
| Incremental snapshot | tombstone、chain、compaction |

跨 pipeline 一致 cut：upstream=1000、downstream=980，必须记录或重放在途事件。若所有独立 view 可从原始日志重建，可独立 checkpoint。

Crash-safe installation：write temp → checksum/flush → fsync → rename → fsync directory → 持久化 manifest → 保留上一个有效 checkpoint。尾部坏记录可按协议截断；中间损坏应报警并从可靠来源恢复。CRC 有效不意味着业务已经 commit。

### 6.4 Catch-up、HA、Failover

积压 B，live arrival λ，replay rate μ：
$$
T_{catchup}=\frac{B}{\mu-\lambda},\quad \mu>\lambda
$$
若 μ≤λ，无法追上。例：500K/s 停60秒积压30M，replay 2M/s 则20秒追平（忽略启动/核对）。Bulk read、parallel replay、抑制无用派生发布、必要时只重建核心 state；路径依赖事件不可随意跳过。

Cold standby（重建）、warm（checkpoint+lagging log）、hot（持续消费并重建）。Active-active 按独占 partition 可行；同 key multi-writer 需要冲突协议。Raft quorum 用于关键权威 ledger，派生风险不一定逐事件 consensus。

Aeron Archive recording ID/byte position 不等于 business seq；recording progress 不自动代表 disk sync 或 business commit。Replay merge 仍需 gap/identity 验证。

Failover：detect → elect/promote → enforce epoch/fencing → restore committed progress → reconcile unknown orders & reservations → enable. Heartbeat timeout 不证明旧 leader 已停止。

RPO/RTO 按故障模型定义；RTO=检测+启动+加载+重放+对账+激活。RPO=0 只能针对已明确可靠提交的事件和指定故障模型承诺。

### 6.5 Reconciliation、Schema、Testing

三级核对：独立 rebuild；OMS/capture/risk ledger；external broker/venue/clearing/drop copy。按 trade ID、qty、notional、fee、revision、working orders、grant、settlement 检查。修复应使用受控 recovery/correction event 而不是裸改 counter。

Schema evolution 区分 backward/forward/semantic compatibility（单位从 shares 变成 lots 即使 binary 兼容也可能语义不兼容）。Historical reproduction vs restatement 要明确模型版本。

Metrics：received/recorded/durable/committed/applied/published/checkpoint offset、oldest gap age、replay rate、snapshot pause、recon difference。Fault injection：commit 前后、snapshot rename 前后、重发、损坏、partition、schema upgrade。

**面试：** 如何证明不丢成交？如何保证 checkpoint 与 offset 一致？怎样量化 RTO？为何不对所有 risk update 做 consensus？

<a id="r7"></a>
## R7. Trade Capture & Trade Lifecycle

### 7.1 Entity 与职责

Order（意图）→ Execution/Fill（成交事实）→ Canonical Trade（经济记录）→ Allocation/Booking → Clearing/Settlement。一个 order 有多个 fill；一个 fill 可能产生多条内部经济记录。Deal 定义依机构而异。Risk 应及时反映已发生的成交，而不是等待 booking/settlement。

Trade Capture：ingest、normalize、dedup、enrich、revision、journal、distribute、reconcile。

### 7.2 FIX 4.4 示例

| Tag | Field |
|---|---|
| 35 / 34 | MsgType / MsgSeqNum |
| 11 / 41 / 37 | ClOrdID / OrigClOrdID / OrderID |
| 17 / 19 | ExecID / ExecRefID |
| 150 / 39 | ExecType / OrdStatus |
| 32 / 31 | LastQty / LastPx |
| 14 / 151 / 6 | CumQty / LeavesQty / AvgPx |
| 60 | TransactTime |

Execution Report 35=8；普通成交 ExecType F，partial OrdStatus1/full2；trade correct G、trade cancel H，ExecRefID 引用相关历史 execution。New0、cancel4、replace5、reject8。实际 FIX version 与 venue RoE 可能不同。Trade Capture Report 35=AE 是另一类消息，不能默认与 35=8 的业务身份一一对应。

1000股 order，LastQty 300/500/200，CumQty 300/800/1000；不能累计 CumQty 或仅用 cum delta 重建完整交易事实（价格、correction、identity）。

### 7.3 Business identity、Drop Copy

Transport identity（session+seq）≠ execution identity（venue-defined namespace+exec ID）≠ canonical trade identity。ExecID scope 由协议决定；不同来源可能不同 ID，需 adapter mapping。qty/price/timestamp 不能作为可靠唯一 key。

~~~mermaid
flowchart TD
    E[Exchange / LP] --> OE[Order Entry]
    E --> DC[Drop Copy]
    OE --> N[Normalize / match / dedup]
    DC --> N
    N --> T[Canonical trade ledger]
    T --> P[Position / Risk]
    T --> B[Booking]
~~~

First valid arrival 可以降低延迟，但冲突字段必须按权威 source contract 解决。相同 identity 同 payload 为 duplicate，不同经济字段为 conflict；合法 correction 有独立 revision identity。原始消息可靠保存，去重后的 canonical event 才产生一次经济影响。

### 7.4 Orthogonal state machines

Economic active/busted，booking pending/booked，allocation partial/full，clearing，settlement，reconciliation 等是正交维度，不应使用巨大枚举强行互斥。

Correction：old contribution → new contribution。
$$
\Delta Q=q_{new}-q_{old},\quad
\Delta C=q_{old}p_{old}-q_{new}p_{new}
$$
100@10修正为120@11，mark=12：qty +20，cash -320，PnL -80。Bust 使 contribution 为零，并不是新增 opposite trade。历史 realized/FIFO/WAC 可能需要 partial replay。

Revision chain E100→E101→E102→E103(bust)。ExecutionKey→TradeId 与 TradeId→CurrentState 两层索引。Correction before original：buffer pending dependency，查询/reconcile；两个 correction 同时引用旧 revision 时不能盲目 LWW。明确 expected revision 与 idempotency。

~~~cpp
// Conceptual: atomic durable change + dedup + recoverable publication.
void onTradeEvent(const TradeEvent& e) {
    if (isIdenticalDuplicate(e)) return;
    auto before = findTrade(e.tradeId);
    if (!validateTransition(before, e)) {
        quarantine(e); return;
    }
    auto after = buildNextState(before, e);
    auto change = TradeChange{e.eventId, before, after};
    commitChangeAndOutbox(change);
    installState(after);
}
~~~

Downstream apply remove(before contribution)+add(after contribution)，state 与 applied ID 同恢复边界；跨 book/instrument 需事务语义。

### 7.5 Enrichment、Allocation、Multi-leg

Enrichment 使用 local versioned reference cache（instrument、book、account、multiplier、value date、fees）。Missing metadata 不丢真实 fill；先 raw capture，标记 pending/risk incomplete，再补全。避免每笔 N+1 RPC。Replay 保留历史 enrichment version。

Allocation 1000股 → book A400/B350/C250，sum child + unallocated = parent；不能 parent+children 重复计入 firm。先归 unallocated book，再原子 reclassify；firm view 可直接消费 original execution。Parent correction after allocation 必须协调 child revision。

FX swap near +EUR1M/-USD1.1M，far -EUR1M/+USD1.102M；跨日期 EUR nominal 为0不代表 funding/curve/credit/settlement risk 为0。Native atomic multi-leg contract 在经济事务边界发布；synthetic spread 各腿实际成交应及时反映，不能隐藏 unhedged risk。避免 summary+leg 双计数。

### 7.6 Late trade、RPC、Sharding

A buy100@10、B sell100@12、C buy100@14。Event order A B C：Q100/avg14/realized+200；arrival B C A：Q100/avg10/realized-200。相同总 PnL，但 accounting 结果分配不同。可用 venue sequence、watermark、bounded reorder、immediate exposure + correction、partial replay。跨 venue timestamp 不自动构成全局顺序。

RPC query（可考虑 local subscription）≠ business command（需幂等确认）≠ transaction coordination。已在 venue 成交后，post-trade risk operation 失败不能抹掉成交。Capture 与慢 enrichment/booking 解耦；如果业务要求 booking 等 risk ACK，异步化也不消除逻辑依赖。

Sharding：decode/session → identity resolution → trade lineage owner → allocation owner → position book+instrument → risk dimension。Correction 的新 ExecID 不可直接按独立 hash 修改同一个 trade。早期可 raw journal，轻量 stages 可 fuse。

Dedup hot table + historical index + durable ledger。TTL 必须覆盖合法 replay/correction horizon；Bloom filter false positive 不可作为最终丢弃依据。

50K unique fills/s、256B normalized、8h恒定：12.8MB/s，368.64GB journal；128B identity record 每成交一个则184.32GB（未考虑索引和清理），说明必须按实际平均速率及保留策略规划。

### 7.7 Testing / observability

Duplicate from two sources、identity conflict、correction before original、allocation after bust、spread leg、missing metadata、late trade、crash after commit before publish、downstream dedup。Metrics：execution-to-capture/risk、unmatched drop copy、pending correction age、booking lag、recon gap。核对外部/内部 trade ledger、position、risk、booking；修复事件而不是直接改数据库计数。

**面试：** 如何设计100K fills/s、多 venue、drop copy、correction 与 HA 的 trade capture？

<a id="r8"></a>
## R8. Inventory & Locate Management

### 8.1 Financial semantics

Economic position ≠ settled inventory ≠ lendable inventory ≠ locate ≠ actual stock borrow ≠ authorization。Locate 通常是合规的借券来源确认，不必然已借到，也不一定能重复使用；actual borrow 有归还义务、费用、collateral/recall terms。适用卖空规则、long/short marking 与 reuse 必须按 jurisdiction、broker、account、security、session、effective date 配置（见 SEC、SGX 官方资料）。

例：settled10k、unsettled buy3k、unsettled sell4k、working sell2k、restricted1k；经济仓位9k。假设扣减项无重叠且 settled 尚未扣 sell，保守不使用 unsettled buy 时 available-to-sell=3k；允许符合交割条件的 buy 后为6k。

### 8.2 Reservation

1000 long inventory，两策略 sell800/600，若各自只看 position 会产生400 short。Atomic reservation 按批准规则消耗 long inventory 与 short locate；订单标记/拆分依实际市场。

~~~cpp
struct Inventory { int64_t longFree, locateFree; };
struct Reservation { int64_t longQty, shortQty; };

std::optional<Reservation> reserveSell(Inventory& s, int64_t q) {
    if (q <= 0 || s.longFree < 0 || s.locateFree < 0)
        return std::nullopt;
    auto l = std::min(q, s.longFree);
    auto sh = q - l;
    if (sh > s.locateFree) return std::nullopt;
    s.longFree -= l;
    s.locateFree -= sh;
    return Reservation{l, sh};
}
~~~

单 owner，示例省略身份、有效期、marking 与 durable ledger。Locate100k → reserve20k → fill8k → cancel12k，最终 available92k/consumed8k。Buy-to-cover 不自动增加 locateFree，reuse 取决于授权。

### 8.3 Three ledgers、Recall & expiry

Trading ledger（short position）、locate ledger（grant/reservation/consumption）、stock loan ledger（borrow/return/recall/collateral）分别管理。Borrow10k、short8k、cover3k，short5k但 outstanding borrow 仍10k，实际 return 后才变7k。

简化日费用 = borrowedQty × price × annualRate / dayCountBasis；100k股×$50×5%/360≈$694.44/day；30天约$20,833.33，真实费用按合同。

Availability reduction/locate withdrawal/expiry/borrow recall/settlement failure 是不同事件。Recall 改变归还义务，不自动改变经济仓位。可能补借、使用已有可交割证券、买入平仓或升级处理。

外部授权若由100k降为50k，而 consumed60k、reserved20k，则新 available=0、按简化模型 deficit=30k。不能减少历史成交来强行恢复守恒；明确生效边界、取消未成交、fencing 与交割处理。

### 8.4 Multi-broker allocation

PB A40k@2%、B30k@3%、C50k@5%、D20k@12%；需求80k，假设独立同质供给与线性成本，greedy 选 A40k+B30k+C10k，加权2.75%。排序 O(B log B)，分配 O(B)。多个 broker indication 可能来自同一 lender，不能直接求和。约束多时可用 min-cost flow / LP / MIP；cold path 预计算，不在微秒热路径做大型优化。

Owner key = Instrument + Inventory Pool（legal entity/account/clearing/settlement/broker entitlement）。Grant-level ledger 包含 ID、qty、expiry、rate、version、reuse policy、reserved/consumed/withdrawn。Reservation 记录每个 grant allocation。Expiry min-heap / timer wheel，small vector 常优于小规模 heap。

### 8.5 Distributed inventory budget

与 R5 相同的 exclusive escrow，但新增 expiry、external revocation、source、reuse、settlement。100k instruments×32 gateways×8 threads=25.6M潜在组合，采用 sparse demand-driven allocation。Hot path 本地 check-reserve，warm async refill，cold broker request/optimization，control revoke/recall。分区期间不能依赖 stale grant 继续无条件发单。

### 8.6 Settlement date algorithm

Opening10k，Day1 +5k，Day2 -12k，Day3 +10k；projection 10k/15k/3k/13k。增加 Day2 sell5k，Day2 变-2k，即使最终余额为正也无法满足交割约束。

$$
I_t=I_0+\sum_{i=1}^tF_i
$$
Sell q settles at d: 对全部 t≥d 执行 I[t]-=q。若要求非负，则 min(I[d..horizon])≥q。

~~~cpp
if (tree.rangeMin(d, horizon) >= q) {
    tree.rangeAdd(d, horizon, -q);
    return true;
}
return false;
~~~

Lazy segment tree range add/min O(log T)，space O(T)；短 horizon 固定数组 O(T) 常更简单。模型假设交割流确定、同池资产可互换；现实需考虑 settlement fail、custodian、haircut、时间点。

### 8.7 Correctness & tests

固定 grant：available+reserved+consumed=granted；分层预算不重复统计；动态撤销时引入 deficit/pending revoke。Test duplicate grants/cancels、partial fill、expiry while working、recall、buy-to-cover policy、transfer ACK lost、gateway crash、corporate action、settlement fail。

**面试：** Locate 和 borrow 有什么区别？如何把固定库存安全分配给多个 gateway？为什么 buy-to-cover 不能无条件释放 locate？怎样高效检查未来交割库存？

<a id="r9"></a>
## R9. Market Risk、Greeks、VaR & Stress Testing

### 9.1 Risk factor & Greeks

Instrument 是交易对象，factor 是影响价值的市场变量。FX option 依赖 spot、volatility surface、EUR/USD curves、time/strike 等。Reverse dependency index 支持选择性 revaluation。

| Greek | Formula / unit |
|---|---|
| Delta | ∂V/∂S |
| Gamma | ∂²V/∂S² |
| Vega | ∂V/∂σ；注意 absolute vol 与每1 vol point |
| Theta | 时间导数，明确 elapsed time / time to maturity 的符号 |
| Rho | ∂V/∂r |
| DV01/PV01 | 1bp=0.0001，明确利率上/下移的符号约定 |

USD 2Y +20k、5Y -15k、10Y -5k DV01，总和0但 curve twist 仍有风险。Sensitivity 数据要包含 factor ID、curve/tenor、currency、unit、context。

### 9.2 Taylor approximation & FX

$$
\Delta V\approx g^T\Delta x+\frac12\Delta x^TH\Delta x
$$

Delta10k、Gamma2k、price change+2，近似 PnL=20k+4k=24k。Vega20k/vol point、15%→17%，+40k。DV01=-5k/bp、+10bp，-50k（指定符号约定）。

FX spot1M EUR@1.1000，mark1.1050，简化 PnL=USD5k。Forward PV = Q_EUR*S*DF_EUR - Q_USD*DF_USD，固定 DF 下 spot delta=Q_EUR*DF_EUR。

Dense Hessian 10k×10k doubles=800MB，使用 sparse sensitivities / factor reduction。Full revaluation 适合较大冲击或 nonlinear/barrier/path-dependent model；fast path 用局部敏感度，按误差、市场变化、版本及时间触发 full refresh。Approximation 不是天然保守上界。

### 9.3 VaR & ES

令 L=-PnL：
$$
VaR_\alpha(L)=\inf\{l:P(L\le l)\ge\alpha\}
$$
99% one-day VaR=2M 表示模型分布的99%损失分位，不是最大损失；超越概率约1%（连续分布）。

Historical：历史共同 factor shocks 应用到当前 portfolio，而非统计旧 portfolio 实际 PnL。
Parametric：线性暴露、零均值正态分布下 $VaR_\alpha=z_\alpha\sqrt{w^T\Sigma w}$，95% z≈1.645，99%≈2.326。
Monte Carlo：模拟相关 market paths，Σ=LL^T，Δx=Lz，z 独立标准正态；非正定或复杂模型需适当分解/模型。Historical/MC 可 full revalue 或 sensitivity-based。

$$
ES_\alpha=\frac{1}{1-\alpha}\int_\alpha^1VaR_u\,du
$$
连续分布可解释为尾部平均损失；离散分布需 boundary fractional weight，不能简单平均所有 ≥ VaR 的样本。

~~~cpp
double empiricalVaR(std::span<double> losses, double alpha) {
    if (losses.empty() || !(alpha > 0 && alpha < 1))
        throw std::invalid_argument("invalid input");
    // Assumes finite values and representable size.
    size_t k = size_t(std::ceil(alpha * losses.size())) - 1;
    std::nth_element(losses.begin(), losses.begin()+k, losses.end());
    return losses[k];
}
~~~

Nearest rank；sort O(S log S)，nth_element average O(S)，ES 尾部求和可 average O(S)。函数会重排输入；不能破坏后续 scenario ID 对齐。

### 9.4 Covariance optimization & factor model

R2 的单因子增量：cache u=Σw，V+=2du[k]+d²Σ[k,k]，u[i]+=dΣ[i,k]，O(N) 总更新。Matrix 对称，可用 contiguous row。

$$
\Sigma=BFB^T+D,\quad z=B^Tw,\quad
V=z^TFz+w^TDw
$$

B 为 N×K factor loading，F 为 K×K covariance，D 对角 residual。单因子变化 d 更新 z += d B[k,:]，O(K)；dense F 的矩阵乘法通常 O(K²)。固定 F 若预分解 F=LL^T 并预计算每行 B[k,:]L，可维护 transformed exposure y=L^Tz 与 norm²，从而单因子 O(K)，代价是额外预处理和 storage；F 变化须重建版本。

相关性与 diversification：两个100k PnL stddev，ρ=-0.5，组合 stddev100k，99% VaR≈232.6k，而 standalone sum465.2k。VaR 一般非可加，甚至不总满足 subadditivity。

### 9.5 Scenario engine

1M positions×1000 scenarios=1B valuations；如果同一100k instruments 可共享 unit valuation，则100M valuations。必须相同经济条款/模型/币种等。

$$
R_{B\times S}=Q_{B\times I}P_{I\times S}
$$

Sparse position matrix × dense scenario PnL matrix。CSR、SIMD、cache blocking、BLAS、GPU。Position delta 更新 AXPY：

~~~cpp
void update(std::span<double> port,
            std::span<const double> instrument, double dq) {
    // Equal lengths, no harmful aliasing.
    for (size_t s=0; s<port.size(); ++s)
        port[s] += dq * instrument[s];
}
~~~

Instrument-major 适合逐 trade 连续 scenario update；scenario-major 适合 scenario scan；2D tiling 兼顾 cache 与 load balancing。Worker 可 work steal immutable valuation task；owner 验证 context。

100k×1000 valuations，5µs CPU/valuation，总500 CPU seconds；32 cores×70%效率约22.32s，scenario matrix 800MB。不是实际 benchmark。

GPU 适合大 batch matrix/MC，微秒级 small order reservation 通常不适合；考虑 launch、PCIe transfer、branch divergence、precision、resident data。

### 9.6 Stress、Version、Validation

Stress scenarios：equity crash、FX shock、rates parallel/twist、vol shock、liquidity widening、correlation breakdown；不一定有概率。冲击必须作用于正确 market variable，重建依赖 curve，明确保持不变的量。

ValuationContext：positionSnapshot、marketSnapshot、reference/model/scenario/covariance version。Live incremental view 允许明确 bounded staleness；formal snapshot view 所有 worker 用同一 context。PV、delta、gamma、vega、scenario PnL 在兼容定义下可加；variance、VaR、ES、max drawdown 不可直接加。先按 scenario 聚合再 quantile。

Finite difference：delta≈(V(S+h)-V(S-h))/(2h)，gamma≈(V(S+h)-2V(S)+V(S-h))/h²。h 太大 truncation，小则 cancellation。AAD reverse mode 对多输入少输出的平滑计算图可降低 sensitivity 成本，需 tape 管理。

验证：analytic vs finite difference、opposite trades cancel、incremental vs full、zero shock、PSD covariance、independent reference、schedule variation、stale result。VaR backtesting 检查实际超越频率及模型假设，代码正确不代表模型有效。

**面试：** 如何优化十亿次估值？为什么不能加各 book VaR？如何保持多线程 valuation context？何时 GPU、何时 SIMD、何时数学降维？

<a id="r10"></a>
## R10. Credit Risk & Limit Management

### 10.1 Credit fundamentals

Market risk hedge 不代表 counterparty credit risk 抵消；不同 counterparty、legal entity、netting set 不可任意净额。

CE=current exposure，RC=replacement cost，PFE=potential future exposure，EE=expected exposure at time t，EPE=expected positive exposure across time，EAD=exposure at default，PD=default probability，LGD=loss given default，CVA=credit valuation adjustment。

同一可执行 netting set 的 MTM +5M/-3M/+2M：无 netting gross positive=7M；netting=4M；可认可净 collateral1M 时简化 CE=3M。
$$
CE=\max(\sum_i V_i-C,0)
$$

先按 netting set 聚合 MTM，再 floor/collateral，再按 counterparty/group 聚合。维护 totalMtm += new-old，CE update O(1)（固定简化模型）。不能直接用 MTM delta 当 CE delta。

### 10.2 PFE / collateral / settlement

每个 scenario s、future time t，先聚合 netting-set PV，再算 E[s,t]=max(V[s,t]-C[s,t],0)，EE 为均值，PFE 为指定分位数。100k trades×2000 scenarios×50 times=10B valuations/80GB cube；若1000 netting sets 聚合 cube 为800MB。共享 market paths、cashflow aggregation、scenario tiling、incremental contribution；不可丢掉所需历史或修正信息。

Basel SA-CCR EAD=1.4(RC+PFE_SA-CCR)；这里 PFE 为监管 add-on 组件，不等于 simulation quantile，也不等于 internal credit charge。

VM 覆盖已发生 MTM 变化，IM 覆盖违约后平仓期间潜在风险；haircut、threshold、minimum transfer amount、margin period of risk。Margin call 不等于 collateral received/settled/eligible。10M collateral haircut5%→9.5M simplified eligible value。Wrong-way risk：敞口增长同时 counterparty 信用恶化。

FX settlement：已不可撤销地付USD11M却未收EUR10M，可能承担已付本金风险；PvP 保证受保护双方最终付款互为条件，消除相应本金风险，但仍有其他风险。Pre-settlement/settlement/tenor/currency limits 区分。

### 10.3 Hierarchical / overlapping limit

Group100M、entities60M/50M、product/tenor/currency/settlement limit 可能交叉，不是纯 tree。Limit 是上限，budget 是独占权利。TradeContext → sparse applicable LimitIDs + corresponding approved charges。

~~~cpp
bool reserve(std::span<const CreditCharge> charges) {
    // One owner; unique limit IDs, validated arithmetic,
    // no partial failure in commit, durable reservation handled separately.
    for (auto c : charges)
        if (c.amount <= 0 || limits[c.id].available < c.amount)
            return false;
    for (auto c : charges) {
        limits[c.id].available -= c.amount;
        limits[c.id].reserved += c.amount;
    }
    return true;
}
~~~

O(K)，K 为涉及约束数。跨 shard 可共置、集中 authority、conservative multi-reservation、2PC 或 hierarchical escrow。详见 R5。每维独立满足守恒。

### 10.4 Nonlinear marginal credit

$$
\Delta R(T)=R(P+T)-R(P)
$$

一般 ΔR(A)+ΔR(B)≠R(P+A+B)-R(P)，即使两个 gateway 使用同样最新 snapshot。CE 下降不代表 future PFE 或 settlement risk 下降。

Fast authorization 使用经验证的保守 charge/envelope；accurate calculation 用 portfolio model。不能证明上界时使用集中授权。可选 scenario envelope：在固定场景、非负保守 charge 与明确 collateral 假设下，对每场景分配 budget，逐元素 check O(S)，适合 SIMD，但可能过度保守、占用内存，不等于直接限制 PFE quantile。

### 10.5 FX quote 与 reservation lifecycle

Executable quote 是否 reserve 取决于 binding/execution semantics。Reserve each quote 占用大；execution request 才 reserve 延迟较高；shared quote capacity 需防并发执行超额。Partial fill 固定charge模型：reserve10M→fill4M→reserved6M/used4M→cancel释放6M。真实成交消耗超出 reserve 时记录实际 exposure 与 breach。

Pending local overlay：central used60M，本地 fill+5M，effective65M；central update65M包含该 fill 时移除 pending delta，不能变70M。依据 event identity/applied offsets 与 epoch，不按 counter 数值猜测。非线性 overlay 需要兼容/保守模型。

Limit reduction100M→60M，used70M/reserved10M：available0、简化 deficit20M。区分 authorization breach、market-driven、limit reduction、collateral、data quality、operational。Revoke/fence local grants；不修改历史真实 used。

### 10.6 Architecture & metrics

Valuation key→netting set→counterparty/group exposure→credit authority→exclusive budgets。Collateral agreement、settlement date 有独立 owner；credit calculation 与 authorization 分不同路径。500K orders/s×8 limits×50ns=0.2 CPU cores（仅检查循环，实际含 lookup、commit、queue 等）。

Metrics：p99.9 check、budget hit/refill/fragmentation、false reject、reservation leakage、exposure freshness、revocation lag、reconciliation。Test legal netting boundary、collateral drop、pending margin、overlapping limit、duplicate fill、local overlay、partition/fencing、settlement failure。

**面试：** 为什么 PFE 不能像普通额度直接拆分？如何用 hierarchical budgets 减少 RPC，同时保证 global constraint？如何处理动态信用额度减少？

<a id="r11"></a>
## R11. Sequencer & Replicated State Machine

### 11.1 Ordering vs execution

Sequencer 建立 logical order。atomic fetch_add 只分配序号：A 获100暂停，B 获101先发布，仍乱序；A crash 可能永远缺100。Sequence assignment ≠ ordered publication ≠ durable commit。

| Order | Scope |
|---|---|
| Per-producer FIFO | 单 source |
| Per-key | order/trade/resource key |
| Causal | 有依赖操作 |
| Total order | 所有消费者交付同一顺序 |
| Atomic broadcast | 在定义故障模型下可靠一致交付与排序 |

独立操作可交换且业务决策/外部观察不受影响时无需全局排序。Market data per feed/instrument，reservation per credit/inventory pool，trade per lineage，config 对受影响资源定义有效顺序。

Command 请求可被拒绝；Event 是已决定的事实。Command log replay 重新决策，business event log 应用历史结果；均需确定性。相同初始状态、相同有序输入与 deterministic transition → 相同已提交进度的业务状态。

Non-determinism：本地 clock/random/RPC/timer/thread merge/floating-point reduction/unordered iteration。将外部结果、time/timer、model/version 作为有序输入；只允许 leader 产生外部 side effects 并 fence。

### 11.2 Single sequencer / partitioned sequencer

多 SPSC→bounded round robin→sequence→append→publish。Append failure 要保留已 pop 事件，避免 gap。单 sequencer 简单但串行瓶颈；按独立 ordering domain 分区提高容量，跨 partition transaction 另需协议。

~~~cpp
while (running) {
    for (auto& q : inputs)
        drainBounded(q, batchLimit, sequenceAndAppend);
    flushPendingPublications();
}
~~~

### 11.3 RSM & Raft

~~~mermaid
flowchart TD
    C[Command] --> L[Leader: append]
    L --> A[Follower A log]
    L --> B[Follower B log]
    A --> Q[Quorum commit]
    B --> Q
    L --> Q
    Q --> S[Deterministic state machine]
    S --> R[Business response]
~~~

Raft follower/candidate/leader，term、election、log matching、majority commit。Quorum=floor(N/2)+1，3 nodes 可容忍1 failure，5可容忍2（剩余多数可通信、满足故障模型）。不能仅按副本数直接提交旧任期 entry；当前任期 entry commit 可间接提交先前日志。

AppliedIndex ≤ CommitIndex ≤ LastLogIndex。收到 command、append、commit、apply、response 是不同进度。Follower read 可能 stale，旧 leader 也可能不知道失去多数。Linearizable read 需要正确 ReadIndex/quorum/ordered read + applied progress；授权仍须原子 reserve。

Pipelined replication：window100/RTT50µs=2M/s 理想窗口上限，单请求仍需 quorum latency。ACK lost 后用同 request ID 重试，不能重复业务效果。Leader election ≠ external fencing。

Replicate input（重复计算、便于 replay）vs replicate state（可能降低 replica CPU，state delta/commit 更复杂）。关键 ledger 适合 RSM；大型 VaR/MC 可从权威输入重建，不必每次 consensus。

### 11.4 Aeron 与 LMAX

Aeron Transport（IPC/UDP）、Archive（record/replay）、Cluster（replicated log、consensus、service snapshot/timers）、Clustered Service（business state machine）。Transport offer success ≠ cluster commit ≠ business acceptance。C++ Transport 与 Cluster service API 的实际集成依版本核实。

LMAX 公开架构：输入 Disruptor、journal/replication/decoding、single-threaded business logic、output Disruptor。思想是围绕必要的串行业务状态机并行基础设施；不是全系统只有一个线程。

### 11.5 Deterministic parallel execution

Global sequence 101(A),102(B),103(A),104(C) 可由 A/B/C owner 并行；A 101→103 保持序。跨 key transaction 依赖 read/write set：
$$
W_A\cap W_B=\emptyset,\quad W_A\cap R_B=\emptyset,\quad R_A\cap W_B=\emptyset
$$
满足完整依赖与观察语义时可并行。

Sequence order / execution / completion / commit / publication 不一定相同。严格全局 publication 需要 reorder buffer + contiguous watermark：101 pending,102–104 ready,105 pending,106 ready，watermark100；101完成→104；105完成→106。类似 CPU ROB，但业务 commit/外部副作用不能简单 rollback。慢前序造成 HOL blocking，独立 view 不要强制全局 retirement。

Calvin 是 deterministic transaction scheduling 的相关数据库研究（不是直接拿来实现全部交易协议）。

### 11.6 Speculation 与 Logical Processor

在 quorum 等待期间可提前执行 pure decode/lookup/charge/valuation，commit 后验证 position/market/model version 再安装。理想 overlap latency=max(consensus,compute)+validation。不能依据未确认授权提前向 venue 发单。

Ordering domain、logical stage、state ownership、physical deployment 独立。Downstream-first 只调度 ready task；bounded fairness。两个 SPSC 的 FIFO 并不保证 merge determinism，需统一 key sequencing、watermark、记录 merge order 或证明可交换。

Sequencer 单核200ns/event=5M/s理想 CPU 上限。2M/s×128B=256MB/s payload；两个 unicast replicas=4.096Gbps payload，不含 overhead。缩小 ordering scope、partition、batch、pipeline、减少复制优先于微优化。

**面试：** Total order 是否要求串行执行？Raft commit 与 apply 有何区别？如何把 global sequencing 与 sharded pipeline 组合？Speculation 的外部副作用边界在哪里？

<a id="r12"></a>
## R12. Risk Engine Performance Engineering & Capacity Planning

### 12.1 Service demand、utilization、freshness

Latency = queueing + service + communication/dependency waits（按路径定义）。CPU demand：
$$
C_{min}=\left\lceil\frac{\sum_i\lambda_iD_i}{\rho_{target}}\right\rceil
$$
1M/s×2µs=2 CPU seconds/s，60%目标至少4理想 core。Busy polling 的 OS CPU usage≈100% 不等于 business utilization；测 useful work、idle spins、queue age。

1M ticks×100 positions×5 dimensions=500M updates/s，100ns/update=50 cores；预聚合10单位后为5 cores。每 stage 以原始 event amplification a_i 建模：
$$
\lambda_{max}\le \min_i C_i/(a_i D_i)
$$

### 12.2 Queueing theory

Little's Law $L=\lambda W$，稳定系统长期平均；1M/s×100µs=100 in-flight，2ms=2000。Queue depth500、100K/s，平均 wait5ms（边界一致）。不能由瞬时 depth 推 p99。

M/M/1（Poisson arrival、exponential service、FCFS、稳定）：
$$
\rho=\lambda/\mu,\quad W=D/(1-\rho),\quad p99=-\ln(0.01)/(\mu-\lambda)
$$

| Utilization | Mean (D=1µs) | Model p99 |
|---:|---:|---:|
| 50% | 2µs | 9.21µs |
| 80% | 5µs | 23.03µs |
| 90% | 10µs | 46.05µs |
| 95% | 20µs | 92.10µs |
| 99% | 100µs | 460.52µs |

Kingman G/G/1 approximation：
$$
W_q\approx\frac{\rho}{1-\rho}\frac{C_a^2+C_s^2}{2}E[S]
$$
解释 arrival/service variability；不是 tail prediction。隔离 snapshot、RPC、慢 correction，降低 variance。

Burst：normal500K/s，capacity1.2M/s，burst3M/s持续100ms；backlog180k，结束后 drain≈257ms，65,536 queue约36ms满。Queue capacity 不能提高 service capacity；按可吸收 burst 与 SLA 决定策略。

### 12.3 Scalability、hot shard

Amdahl fixed workload $Speedup=1/(f+(1-f)/N)$；f=10%，32 cores≈7.80×，无限最大10×。Streaming capacity 另按 stage demand 建模。
USL 经验拟合：
$$
C(N)=\frac{N}{1+\alpha(N-1)+\beta N(N-1)}
$$
α contention、β coherency/coordination 参数需 benchmark 拟合，不代表唯一可识别的物理因果。增加线程可能反降吞吐。

Shard weight=sum λD，不是 key count。平均40%但 hot shard100%时全局相关尾延迟仍很高。rebalance、sub-shard、offload pure calculation、preaggregation、isolate slow path。

### 12.4 Memory / Roofline

示例（非实测）：L1~1ns、L2~4ns、LLC~15ns、DRAM~90ns、remote NUMA~150ns，依硬件变化。五次依赖 DRAM load 可能450ns；独立 load 可利用 MLP。

Latency-bound（pointer chasing）用 dense ID/flat map/prefetch/MLP；bandwidth-bound（scenario scan）用 blocking/reuse/SoA/SIMD。TLB、huge pages、NUMA placement、false sharing。

$$
Performance\le \min(PeakCompute,Bandwidth\times ArithmeticIntensity)
$$

AXPY 每元素2 FLOP，假设指定 memory 层读x8B/read y8B/write y8B，共24B；I≈0.083 FLOP/B，100GB/s→8.33GFLOP/s，约4.17B iterations/s理想上界。实际 writeback、RFO、cache reuse 需测量。

1M events/s×128B×4 cross-thread hops=512MB/s logical payload；不等于 DRAM traffic。Stage fusion 减少 handoff，但会影响 isolation、variance 与并行度。

### 12.5 Logical processor placement

Logical processor profile：CPU demand、working set、memory bandwidth；communication edge：rate×size、latency cost。Deployment 是带权 graph partition / scheduling：
$$
\min(\alpha\max_t U_t+\beta C_{cross-thread}+\gamma C_{cross-NUMA})
$$
约束 owner、deadline、CPU/memory、ordering。四个 pipeline 两 stage CPU demand [.15,.25],[.30,.20],[.10,.30],[.20,.10] 共1.6 cores，8 dedicated cores平均20%；4 multiplexed cores可各40%（理想 bin packing），但 tail isolation 需验证。

N threads 全连接 pairwise SPSC 排除 self：N(N-1)。32×31×4096×64=248MiB；每 consumer O(N) poll。Sparse graph、active hints、per-stage SPSC 或 local demux。高 stage ready first + bounded quantum + queue age/deadline；不能破坏 business ordering。

### 12.6 Batching

Per-batch fixed 200ns + per-item50ns：
$$
D(B)=50+200/B\text{ ns}
$$
B16→62.5ns，500K/s 恒定且必须凑满时第一条 wait30µs，平均15µs。Opportunistic batch：有消息立即处理，最多 N 条，不强制凑满；journal group commit/GPU 等可设最大等待。

### 12.7 Tail & benchmark correctness

1M/s 时长期约每秒1000条超过p99.9阈值。Fan-out all-of：32独立 shard 各99%在100µs内，整体0.99^32≈72.5%。实际相关性会改变结果。连续 materialize firm view，避免每次 query scatter/gather。

Coordinated omission：closed-loop sender 等前一响应导致系统 pause 时漏掉本应到达的请求。独立生产流应 open-loop schedule 或 trace replay，记录 completion-scheduled arrival、实际发送时间、achieved throughput、dropped/timeout。HdrHistogram correction 需已知预期间隔，不能恢复任意缺失负载。Percentile 不可直接平均，合并兼容 histogram。

### 12.8 Diagnosis

| Symptom | Tools |
|---|---|
| High CPU time / IPC | perf stat、perf record/report、VTune |
| Cache-line sharing | perf c2c |
| NUMA | numastat、model-specific PMU |
| Scheduler/IRQ interference | perf sched、ftrace |
| Blocking RPC / I/O | eBPF、USDT、tracepoints |
| Dirty throttling / writeback | VM/I/O metrics、ftrace |
| Rare spike | Intel PT / triggered tracing |
| Memory bandwidth | uncore PMU、Intel PCM |
| Instruction layout | PGO、BOLT |

~~~bash
perf stat -p "$PID" \
  -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -- sleep 10
~~~

Generic cache-misses 非全层级精确指标，PMU events 按 CPU 核实。Per-worker aligned metrics，不在每 event 更新 global atomic；后台汇总普通字段需同步或消息 snapshot。

Freshness：source→ingress→processing start→calculation→publish→apply；跨主机评估 PTP error。旧价格但健康 feed 不一定 stale；同时看 source/progress health。

### 12.9 Capacity & tests

R13 有完整算例。Failover 两 active 各一半，单机接管全部时利用率近似翻倍；目标故障后70%，正常每机≤35%（理想相同服务成本）。需要 burst/replay headroom。

Benchmark：uniform、Zipf、correlated market shock、open burst、correction、full subscribers、slow consumer、large working set、snapshot at peak、disk slowdown、node failure、live replay、soak。Load sweep 找满足 p99.9 SLA 的 sustainable throughput，而非 maximum throughput。

**面试：** CPU 使用率40%为什么 p99.9 很高？如何做 capacity plan、定位 latency-vs-bandwidth、证明 benchmark 没有 coordinated omission？

<a id="r13"></a>
## R13. Full Risk Infrastructure System Design

### 13.1 Requirements & business invariant

假设 multi-asset fund：50 venues、100k instruments、1M positions、500 strategies、1000 books、32 gateways；1M MD/s、500K orders/s、50K fills/s；local check p99≤5µs（仅检查）、real-time risk update p99≤1ms、VaR独立SLA、单节点 failure、已可靠提交 trade 的 RPO=0（指定故障模型）。真实参数需要业务确认。

Hard authorization safety：不重复使用独占额度；不会因为外部市场变化而承诺绝不出现被动 breach。Executed trade 不得因 post-trade risk fail 而丢失。

~~~mermaid
flowchart TD
    ST[Strategy / Client] --> GW[Trading Gateway / OMS]
    GW --> CK[Inline Pre-Trade Check & Reservation]
    CK --> EX[Exchange / LP]
    EX --> TC[Canonical Trade Capture]
    EX --> GW
    GW --> OL[Order / Reservation Ledger]
    TC --> J[Durable Authoritative Event Log]
    J --> CALC[Sharded Calculation DAG]
    MD[Market / Reference Data] --> CALC
    CALC --> RV[Position / PnL / Greeks / Exposure]
    CALC --> AR[Advanced VaR / PFE Compute]
    RV --> RA[Credit / Inventory Authority RSM]
    OL --> RA
    RA -->|Exclusive grant / refill / revoke| CK
    RV --> DIST[Risk Distribution]
    AR --> DIST
    DIST -->|Informational state| GW
    DIST --> UI[Strategy / Monitor / Reporting]
    CTRL[Control / Limits / Fencing] --> RA
    CTRL --> GW
    J --> REC[Checkpoint / Replay / Reconciliation]
    RA --> REC
~~~

四个 plane：authorization、calculation、distribution、control/recovery。三条 critical path：order（strategy→check→venue）、trade（execution→capture→risk）、control（limit change→authority→effective enforcement）。分别定义 SLA。

### 13.2 Process boundaries 与 ownership

| Process | State / responsibility | Thread model |
|---|---|---|
| Gateway/OMS | order lifecycle、local reservations | dedicated trading threads |
| Trade Capture | canonical trade lineage、journal | sharded single writer |
| Risk Calculation | position、valuation、multi-view aggregation | stage-multiplexed logical processors |
| Risk Authority | grant、budget、credit/inventory authorizations | replicated stateful shards |
| Advanced Compute | VaR、PFE、MC | immutable task compute pool |
| Distribution | subscriptions、snapshot、routing | stateful selective fan-out |
| Control/Operations | configuration、health、recon | event loop / RPC |

不是每种 risk dimension 一个 microservice；process 为 fault/security/scaling/resource boundary，logical processor 为 state ownership/parallelism boundary。

Order ID→OMS，canonical trade ID→trade capture，book+instrument→position，factor→market/valuation，netting set→credit calculation，credit pool→authority，instrument+pool→inventory。中央拥有 grant，gateway 拥有该独占权利内部的 consumption/reservation，避免两个 owner 修改同一份资源。

Authoritative trades/order intent/grants/reservations 与 derived position/PnL/Greek/VaR/cache 分离。Derived state 仍需正确性及恢复，只是不必每笔派生更新独立持久化。

### 13.3 End-to-end worked example

Short sell10,000 shares@50，locate15,000，credit800K，approved fixed charge500K：

| Stage | Locate free/reserved/used | Credit free/reserved/used |
|---|---|---|
| Initial | 15000/0/0 | 800/0/0 K |
| Reserve | 5000/10000/0 | 300/500/0 K |
| Fill4000 | 5000/6000/4000 | 300/300/200 K |
| Cancel remaining confirmed | 11000/0/4000 | 600/0/200 K |

Validate all constraints→reserve→record order intent→send→fill→convert reservation→canonical capture→position/risk→distribution→confirmed cancel release。模型假设固定线性 charge，与实际 credit risk 模型区别见 R10。

Durable-before-send 要把 ACK/commit latency 计入 order path。Venue order uncertain 时不能释放 reserve。OMS local fill 与 central trade event 按 identity/offset 合并，不能双计。

### 13.4 Pre-trade implementation choices

| Model | Strength | Limitation |
|---|---|---|
| Inline function in existing trading thread | no extra queue/RTT | 需要本地独占 state |
| Dedicated risk owner thread | 共享池统一管理 | SPSC round-trip、排队 |
| Remote authority | 非可拆分约束、独立扩容 | IPC/network/commit latency |

Hybrid：common case local check-and-reserve；不足 async refill 或 slow path；复杂 non-decomposable constraint authoritative check。订阅 state 提供信息，不替代授权。冷订阅 register + snapshot + catchup RTT 较大，热资源预订阅、prewarm。

### 13.5 Processing DAG、logical processors、transport

Position/PnL 按 book+instrument，valuation 按共享经济条款，currency/value-date、factor、strategy、netting set、inventory 各有独立 aggregation owner。不能从已丢弃 counterparty 维度的 position 反推 credit。

~~~cpp
struct LogicalProcessor {
    ProcessorId id;
    StageId stage;
    ShardId shard;
    OrderingDomainId domain;
};
struct Placement {
    ProcessorId processor;
    ThreadId thread;
    NumaNodeId numa;
};
struct MessageHeader {
    ProcessorId target;
    MessageType type;
    OrderingDomainId domain;
    uint64_t epoch, sequence;
};
~~~

同 logical owner direct access；同 thread bounded direct/local ready queue；跨 thread SPSC；跨 process Aeron IPC；跨 host UDP 或合适 transport；SBE/versioned binary。四 thread 12有向跨线程队列，可按 stage/channel sparse 配置。共享 FIFO + local demultiplex 或 per-stage SPSC 以支持 downstream-first；选择靠实测。

Global sequencer 不处理所有 MD；按 order/trade/credit/inventory/control 等 ordering domain 分区。关键 ledger 可用 RSM/Aeron Cluster；derived compute 使用 checkpoint/replay。跨 domain authorization 仍需 co-location、保守多资源预留或事务。

### 13.6 Three views / consistency / overload

Authorization view（strict reservation ownership），live calculation view（bounded freshness），consistent analytical view（明确 position/market/model snapshot）。不能用 dashboard 可用额度作为交易权利。

| Data | Policy |
|---|---|
| Executed trade / fill / cancel / grant / revoke | durable/recoverable，不任意丢 |
| Market data | 根据 path-dependent semantics 决定 conflation |
| Derived latest risk | 按 key/epoch/version 合并 |
| UI | 降频、隔离 |
| VaR / PFE tasks | cancel superseded work / reschedule |

Priority control→authoritative trading→real-time calculation→analytics→reporting；只调度满足依赖的 ready task。Kill switch 必须发单边界 enforcement。

Budget dynamic refill：100M pool A40/B35/central25，A请求50、B请求10，全局足但A本地不足。Async refill/rebalance，安全 revoke-before-grant。Peak $10M/s×2ms=20k watermark（还需最大订单和安全余量）。授权变更时明确有效期、epoch、fencing。

### 13.7 HA / Failure matrix

关键 authority 三节点 replicated ledger，quorum commit 与 durability 明确；derived engine active/hot standby 或 checkpoint/replay，publication epoch-fenced。

| Failure | Correct response |
|---|---|
| Gateway crash after send | restore order intent、venue reconcile、unknown reserve 保留 |
| Central state stale | 按有效独占权利和 freshness policy 降级，不统一 fail-open |
| Limit100→60 while used70 | 记录真实 breach、处理未用 grant 和 working order |
| Trade correction races MD | context version 验证；position/market 版本可能不可比较，不盲目 LWW |
| Missing execution | gap recovery/drop copy |
| Old leader alive | enforce fencing at side-effect boundary |
| Disk full | backpressure / stop unsafe acceptance |
| Slow dashboard | independent publication queue |
| Borrow recall | handle actual return obligation and revoke applicable grants |
| Model upgrade | versioned computation / reconciliation |

### 13.8 Capacity worked example

以下为假想经过预聚合的服务成本（不是实测）。

| Stage | Task rate | CPU/task | Cores |
|---|---:|---:|---:|
| Ingress | 1.05M/s | 300ns | 0.315 |
| Position | 50K/s | 2µs | 0.100 |
| Valuation | 4M/s | 500ns | 2.000 |
| Aggregation | 20M/s | 150ns | 3.000 |
| Distribution | 5M/s | 200ns | 1.000 |
| Journal CPU | 50K/s | 1µs | 0.050 |
| Total | | | 6.465 |

60%业务利用率理论11 core；逐 stage 向上取整为1+1+4+5+2+1=14。可初始测试24 physical cores：ingress2、position2、valuation6、aggregation6、distribution2、journal/snapshot2、OS/IRQ/spare4。Authority/gateway/standby/大型MC另算。双 NUMA 每节点12，local state、first-touch、controlled cross-node aggregation、IRQ affinity、SMT 依 benchmark。

Raw memory：

| Data | Assumption | Decimal bytes |
|---|---|---:|
| Position | 1M×128 | 128MB |
| Instrument | 100K×256 | 25.6MB |
| Trade index | 10M×128 | 1.28GB |
| Scenario matrix | 100K×1000×8 | 800MB |
| Book scenario | 1000×1000×8 | 8MB |
| SPSC | 32×31×4096×64 | 260.05MB (248MiB) |
| Total | excluding overhead | ~2.502GB |

另算 hash/allocator、snapshot CoW、page cache、market history、reference data、HA。Trade50K×256=12.8MB/s；MD1M×64=64MB/s；distribution5M×128=640MB/s=5.12Gbps payload。峰值持续8h：trade368.64GB、MD1.8432TB、合计2.21184TB（decimal，未复制/压缩/额外开销）。Peak 与 average、retention、replication factor 分开。

### 13.9 Observability / deployment validation

Performance（p50/p99/p99.9、achieved throughput）、freshness（queue age、offset、publish lag）、business correctness（trade/position/reservation difference）、resource efficiency（budget hit/fragmentation、refill、fan-out、memory BW）。示例监控：check p99 2.8µs、risk freshness p99 420µs、aggregation business utilization89%，后者需要检查 hot shard；均为模拟数据。

Benchmark 同一逻辑图不同 physical placement，比较 dedicated/mixed/compact，记录 CPU、queue、cache/NUMA、tail latency、failure headroom。在线调整 placement 必须 migration/fencing，不只是改 thread ID。

### 13.10 45-minute interview structure

| Time | Topic |
|---:|---|
| 5 min | Requirements、business invariants |
| 7 min | High-level four-plane architecture |
| 8 min | State ownership、sharding、DAG |
| 8 min | Local authorization、trade lifecycle |
| 7 min | Consistency、recovery、fencing |
| 7 min | Capacity、tail latency、benchmark |
| 3 min | Tradeoffs / limitations |

挑战：64 trading threads、100k instruments、sub-5µs、near-optimal credit utilization、instant revocation、zero loss。不能保证这些在所有故障模型下同时成立。本地 escrow 减 RTT 但碎片化；instant remote revoke 与 network partition 冲突；durable commit 有等待成本。要求业务排序优先级与 failure model。

### 13.11 English model answer

> I would separate risk authorization from risk calculation because they have fundamentally different consistency and performance requirements. For safely decomposable constraints, I would allocate exclusive budgets to gateways and trading threads so that most pre-trade reservations execute locally without synchronous RPC or shared atomic contention.
>
> I would capture canonical trade events and maintain positions through single-writer state machines. Incremental calculation, pre-aggregation and a reverse dependency index would reduce unnecessary work. Since different risk dimensions have different ownership boundaries, I would organize the engine as a sharded processing DAG.
>
> I would decouple logical processors from physical threads and configure their placement according to service demand, communication locality and NUMA topology. For distribution, I would use selective subscriptions, versioned state and snapshot-to-live synchronization, keeping informational state separate from authorization.
>
> For high availability, I would replicate critical authoritative ledgers, use checkpoint and event replay for derived state, and enforce fencing at external side-effect boundaries. Finally, I would validate business invariants, realistic workloads, tail latency, risk freshness, and failure recovery.

<a id="appendix-a"></a>
## 附录 A. 面试复习清单与工程实战

### A.1 各专题核心追问

| Topic | Must explain |
|---|---|
| R1 | WAC/FIFO、Q/C invariant、late correction |
| R2 | Additivity、sufficient statistics、factor/scenario、delta vs absolute |
| R3 | Single writer、different sharding keys、hot shard、SPSC、migration |
| R4 | Subscription、snapshot cut、gap/epoch、conflation、freshness |
| R5 | TOCTOU、multiple CAS、escrow、2PC、fencing、idempotency |
| R6 | WAL、durability ACK、checkpoint、replay、RTO/RPO |
| R7 | FIX identity、drop copy、revision、allocation、late trade |
| R8 | Locate vs borrow、grant、expiry/recall、settlement date |
| R9 | Greeks、Taylor、VaR/ES、covariance、matrix/tiling、versions |
| R10 | Netting、PFE、collateral、hierarchical limit、nonlinear reservation |
| R11 | Sequencer vs counter、RSM、Raft、deterministic parallel execution |
| R12 | Service demand、queueing、roofline、tail、benchmark |
| R13 | Four planes、process/thread boundaries、full lifecycle & failures |

### A.2 Practical engineering roadmap（尚未实现）

- [ ] P1 Multi-Threaded Position & PnL Engine：WAC、canonical fill、correction、single owner、SPSC、checkpoint、benchmark。
- [ ] P2 Distributed Credit Reservation Engine：multi-constraint reservation、escrow、grant transfer、crash injection。
- [ ] P3 Inventory & Locate Engine：grant-level expiry、reuse、recall、settlement-date data structure。
- [ ] P4 Sequencer-Based Risk State Machine：command/event log、deterministic replay、dedup、outbox。
- [ ] P5 Multi-Dimensional Risk Pipeline：logical processors、configurable placement、sparse SPSC graph、dependency-aware downstream-first scheduling、fairness、NUMA。
- [ ] P6 Benchmark Framework：open-loop、Zipf、burst、histogram、capacity sweep、failover。
- [ ] P7 Mock Interview：45-minute design、failure scenarios、tradeoff reasoning、English explanation。

建议 P1→P2→P4→P5→P6；再补 P3 和综合面试。P5 尤其适合验证 Logical-Processor-Based Deployment-Configurable Stage-Multiplexed Pipeline 的实际价值。

### A.3 最终原则

Thread safety ≠ business correctness；sequence ≠ commit；replica ≠ authority；lease ≠ fencing；delivery ≠ business effect；position ≠ inventory；limit ≠ budget；market risk ≠ credit risk；latency ≠ service time；CPU usage ≠ business utilization；maximum throughput ≠ sustainable throughput。

先从业务不变量消除不必要共享，再通过数学模型消除不必要计算，最后通过底层技术优化剩余工作。

<a id="appendix-b"></a>
## 附录 B. 参考资料与适用边界

以下为进一步学习的入口，不构成本文所有假想架构的外部背书；实际版本及当前监管条款应重新核实。

- [FIX Trading Community — Standards](https://www.fixtrading.org/standards/)：Execution Report、Trade Capture Report、版本与 venue RoE。
- [CME Group Developer Portal](https://www.cmegroup.com/confluence/)：iLink、Drop Copy、spread/leg identity（按市场协议核实）。
- [SEC — Regulation SHO](https://www.sec.gov/investor/pubs/regsho.htm)：美国卖空、locate、close-out 的背景；按最新规则及豁免确认。
- [SGX Rulebook](https://rulebook.sgx.com/)：新加坡证券交易与卖空标记规则。
- [Basel Framework](https://www.bis.org/basel_framework/)：Counterparty credit risk、SA-CCR、market risk capital framework。
- [BIS Committee on Payments and Market Infrastructures](https://www.bis.org/cpmi/)：FX settlement risk 与 PvP。
- [Raft consensus paper](https://raft.github.io/raft.pdf)：leader election、log replication、safety。
- [Aeron documentation](https://aeron.io/docs/)：Transport、Archive、Cluster、flow control、snapshot/replay。
- [The LMAX Architecture](https://martinfowler.com/articles/lmax.html)：公开的 single-threaded business logic architecture。
- [Calvin: Fast Distributed Transactions for Partitioned Database Systems](https://www.cs.yale.edu/homes/thomson/publications/calvin-sigmod12.pdf)：deterministic transaction scheduling。
- [Linux fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html)：文件同步与目录持久化语义。
- [Apache Flink checkpointing documentation](https://nightlies.apache.org/flink/flink-docs-stable/docs/ops/state/checkpoints/)：stream checkpoint、恢复与一致性边界。
- [Roofline Model, Williams et al.](https://doi.org/10.1145/1498765.1498785)：arithmetic intensity 与性能上界。
- [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram)：latency distribution 与 coordinated omission correction。
- [Linux perf wiki](https://perf.wiki.kernel.org/)：硬件性能事件及诊断。
- [TLA+](https://lamport.azurewebsites.net/tla/tla.html)：分布式协议及业务不变量建模。

**范围声明：** 本文整合 R1–R13 的核心知识和工程讨论，合并重复章节；交互模拟器转换为公式、静态案例和 Mermaid 图。不是逐字聊天备份。示例未实际运行；需用生产协议、金融模型、硬件与负载验证设计。
