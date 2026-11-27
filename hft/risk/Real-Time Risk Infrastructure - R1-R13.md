# Real-Time Risk Infrastructure — 系统设计与高性能 C++ 工程手册

> R1–R13 综合学习笔记 | Senior / Staff C++ Engineer | Multi-Asset Trading / Hedge Fund / Risk Infrastructure
>
> 整理日期：2026-11-27（扩展版）。本文是系统设计学习材料，不是任何具体机构（包括 Squarepoint）的内部架构说明。所有 SLA、吞吐、硬件、成本与业务案例均为教学假设，除非另有注明。代码为说明核心算法与不变量的示例，未经生产级编译、并发验证或性能测试。监管规则、交易协议与金融模型必须以实际合同、市场规范和批准的模型为准。

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

### 1.6 完整 WAC/FIFO 状态转换与验证

设 Q 为旧 signed quantity，A 为旧均价，q 为成交 signed qty，P 为成交价格，Q'=Q+q，x=min(|Q|,|q|)。

| Transition | Condition | New average | Realized delta |
|---|---|---|---|
| Open | Q=0 | P | 0 |
| Increase | Q 与 q 同号 | (QA+qP)/(Q+q) | 0 |
| Reduce long | Q>0, -Q<q<0 | A | x(P-A) |
| Reduce short | Q<0, 0<q<-Q | A | x(A-P) |
| Close | Q'=0 | 0 | x(P-A)sign(Q) |
| Flip | Q 与 Q' 异号 | P | x(P-A)sign(Q) |

Worked example，mark=12：

| Fill | Q | WAC | Realized cumulative | Unrealized | Total |
|---|---:|---:|---:|---:|---:|
| Buy100@10 | 100 | 10 | 0 | 200 | 200 |
| Buy100@14 | 200 | 12 | 0 | 0 | 0 |
| Sell150@13 | 50 | 12 | 150 | 0 | 150 |
| Sell100@11 | -50 | 11 | 100 | -50 | 50 |
| Buy50@10 | 0 | 0 | 150 | 0 | 150 |

最终 cashflow=-1000-1400+1950+1100-500=150。FIFO 在 Sell150 时平100@10及50@14，realized=250、remaining50@14、unrealized=-100，总 PnL 仍150。

~~~cpp
struct Lot { int64_t qty; double px; };
struct FifoPosition {
    std::deque<Lot> lots;
    double realized = 0;
    void fill(int64_t q, double px) {
        while (q && !lots.empty() &&
               ((q > 0) != (lots.front().qty > 0))) {
            auto& a = lots.front();
            auto n = std::min(std::abs(q), std::abs(a.qty));
            int64_t sign = a.qty > 0 ? 1 : -1;
            realized += double(n) * (px - a.px) * sign;
            a.qty -= sign*n;
            q += sign*n;
            if (!a.qty) lots.pop_front();
        }
        if (q) lots.push_back({q, px});
    }
};
~~~

前提：单 owner、价格有效、无 INT64_MIN、算术范围已验证。单笔最坏 O(k)，无历史修正的序列中每个 lot 最多完全弹出一次，均摊成本低不代表 tail latency 低。生产代码还需预分配、费用分摊、异常安全、修正与 durable replay。不能在已经修改 lots 后因内存申请失败丢弃真实成交。

### 1.7 Economic state vs accounting view

~~~mermaid
flowchart LR
    T[Canonical trades] --> E[Additive quantity and cashflow]
    T --> A[Ordered WAC or FIFO]
    M[Mark and FX rates] --> E
    E --> P[Total PnL / exposure]
    A --> R[Realized / unrealized]
    R --> REC[Reconciliation]
    P --> REC
~~~

Economic state 支持可加 reduction；accounting view 需要顺序。例：Q100、mark10，buy20@11、mark→12。先 fill 后 mark 的增量=-20+240=220，先 mark 后 fill=200+20=220。最终相同但中间 drawdown/breach 不同。不能把终态等价当作业务决策等价。

PositionStore：Book+Instrument→dense ID→flat array；TradeID→canonical contribution；Instrument→active position IDs。Q=0 不必立即删除，可能仍有 realized、settlement、correction 或 subscription。逻辑 tombstone 与物理回收分离，ID reuse 需 generation 防止迟到事件污染新仓位。

| API / Event | Important fields |
|---|---|
| Fill | execution identity, book, instrument, signed qty, price, currency, value date |
| PositionDelta | source epoch, stream seq, key revision, delta qty/cashflow/realized |
| Mark | factor/instrument, market version, source timestamp |
| Snapshot | accounting policy, position state, dedup, offsets, reference/model versions |

### 1.8 面试深化与实验

- 实现 WAC/FIFO 并随机生成 reduce/flip/close，验证 total PnL invariant。
- 对历史 correction，比较 additive inverse 与 ordered replay 的适用边界。
- 分别测 eager/lazy/aggregate-first，在相同 freshness 和输出维度下比较。
- 处理 missing mark、NaN、overflow、费用、FX conversion、corporate action。
- 测试 snapshot/replay 重复 fill 不二次修改 state。
- 解释 FIFO 的 amortized complexity 与单次 worst-case 的区别。

**English answer:** I would maintain additive economic quantities and cashflows, while separating path-dependent accounting. I would share unit valuations across economically compatible positions, partition mutable state by ownership, and verify incremental results against an independent reference implementation.

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

### 2.6 通用 Incremental Materialized View

定义 contribution c(x,g)，x 是输入 record、g 是目标 aggregation key。

$
\Delta A_g=c(x_{new},g)-c(x_{old},g)
$

插入 old=0，删除 new=0，book A→B 则分别撤销 A 和增加 B，而不是只更新 B。

~~~cpp
template<class Emit>
void onChange(const Contribution* before,
              const Contribution* after, Emit emit) {
    if (before) emit(before->key, -before->value);
    if (after)  emit(after->key, after->value);
}
~~~

纯 delta 计算不保证跨 key 原子可见性。需要 transaction ID/commit marker、共同 owner 或独立 firm view。Absolute per-source value 从v10=100跳v12=130可恢复130；delta 若漏v11+50、仅v12-20则错误变80。若最后一条 absolute 丢失仍需 heartbeat/replay。

| Metric | Sufficient statistics / structure | Update | Query |
|---|---|---|---|
| Sum | sum | O(1) | O(1) |
| Mean | sum/count | O(1) | O(1) |
| Weighted mean | sum(w*x), sum(w) | O(1) | O(1) |
| Max | multiset/indexed heap | O(log N) | O(1) |
| Top K | ordered tree | O(log N) | O(K) output |
| Prefix/range sum | Fenwick tree | O(log N) | O(log N) |
| Range min + range update | lazy segment tree | O(log N) | O(log N) |
| Concentration | numerator and denominator | O(1) additive | O(1) single ratio |
| Empirical VaR | scenario PnL vector | O(S) | average O(S) |

Concentration total 从100变120，numerator20不变，ratio20%→16.7%。维护全部 ratio 会引入 O(B) fan-out。可以 lazy query，但若每个 ratio 必须即时 breach alert，则仍需相应计算或索引。Drawdown 依赖历史 peak，历史修正不能由当前值恢复。

### 2.7 Dependency Index 实现

CSR 采用 offsets[V+1] 与 edges[E]，每个 factor 的邻接表连续。低频 reference change 可 rebuild immutable graph，高频增删使用 overlay + compaction。RCU/epoch 确保旧图读者退出后才回收。

~~~cpp
void invalidate(InstrumentId id) {
    if (!dirty[id]) {
        dirty[id] = true;
        ready.push_back(id);
    }
}
void dispatchOne() {
    auto id = ready.front();
    auto context = currentContext(id);
    if (!trySubmit(id, context)) return;
    ready.pop_front();
    dirty[id] = false;
}
~~~

单 owner 示例，任务结果回来后验证 position/market/model version；期间再次 invalidated 必须保留新的任务，不能清除新 dirty。计算成本 O(affected V+E+sum actual valuation costs)。大规模共同 shock 时 full rebuild 可能优于 incremental，阈值需实测。DAG 防止重复归属导致 double count；若有真实循环依赖需明确迭代模型，不可简单拓扑排序。

### 2.8 Hierarchical aggregation、批处理与复杂度

32 source shards 每个同 key dirty 后100µs最多 flush 一次，持续活跃时每 key 理想上限320K messages/s。必须按实际 key 分布估算；低流量不应强制凑批。Firm counter 仍热点时，先 per-NUMA partial sums，再按定义频率 reduce。不能把每笔 breach、audit 或 path-dependent event 合并丢弃。

一百万 ticks/s，平均10 book positions/instrument，5 views，50M updates/s。若只需要 firm aggregate，可降到1M/s，但不再满足逐 book 输出；若只订阅2维度，20M/s。Benchmark 比较必须保证相同业务语义及 freshness，不能把少做业务误称纯性能提升。

**English answer:** I would classify metrics by additivity and path dependence, maintain sufficient statistics, and apply nonlinear transformations at the correct aggregation boundary. Reverse dependency indexes restrict recomputation, while versioned contributions and explicit transaction boundaries preserve correctness.

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

### 3.7 Runtime 与 SPSC 内存模型

Logical processor 指拥有业务 state 的执行单元，不是 OS thread 或 SMT logical CPU。Runtime 为每个 processor 提供 bounded doWork，允许静态部署映射、独立 idle strategy、统计 useful work。

~~~cpp
template<class T, size_t N>
class SpscRing {
    static_assert(N > 0 && (N & (N-1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_default_constructible_v<T>);
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) std::array<T,N> slots{};
public:
    bool push(const T& x) {
        auto h = head.load(std::memory_order_relaxed);
        auto t = tail.load(std::memory_order_acquire);
        if (h-t >= N) return false;
        slots[h & (N-1)] = x;
        head.store(h+1, std::memory_order_release);
        return true;
    }
    bool pop(T& x) {
        auto t = tail.load(std::memory_order_relaxed);
        auto h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        x = slots[t & (N-1)];
        tail.store(t+1, std::memory_order_release);
        return true;
    }
};
~~~

Producer release head / consumer acquire head 发布 payload；consumer release tail / producer acquire tail 防止读取中重用 slot。仅单 producer/consumer、trivial payload、固定容量、同进程教学实现。生产需验证 counter wrap、cache line size、object lifetime、atomic lock-free、batch cursor publication；跨进程原子语义不能直接由 ISO C++ 保证。

4 threads 12跨线程 queues，3 stage lanes=36；32 threads 992 queues，3 lanes=2976；4096×64B每队256KiB，总744MiB。应只创建实际通信边。Shared FIFO + bounded local demux 可支持 stage priority，但 ready queue 也要 bounded。不能因下游 queue full 让所有 owner 无界 spin，可能循环等待。采用异步 continuation、预留 control capacity 或明确 backpressure。

### 3.8 Migration failure matrix

| Phase | Old owner | New owner | Router |
|---|---|---|---|
| Normal | epoch e active | inactive | old route |
| Freeze | drain through cut | prepare | buffer |
| Transfer | read-only | install state | buffer |
| Commit | fenced | epoch e+1 | durable route decision |
| Catch-up | no new output | apply suffix | new route |

Old crash before transfer→checkpoint/log recovery；new crash before commit→discard staged/retry；commit ACK lost→read durable decision，不能两个 owner 同时激活。多 producer 要 vector cut 或统一 sequence。CPU affinity 改变不自动迁移 NUMA pages，working set locality 也需调整。

### 3.9 Scheduling 与 pure compute

~~~cpp
while (running()) {
    pollInboundBounded();
    for (auto stage : descendingStages)
        runReadyTasks(stage, maxMessages[stage], timeBudget[stage]);
    serviceOverdueReadyTasks();
}
~~~

同 thread direct dispatch 必须无重入/循环依赖、不会绕过旧事件；否则 local ready queue。高 stage 优先仅针对 ready task，不破坏 business order。Queue A [101,104]、Queue B [102,103] 即使各自 FIFO，merge order 仍不确定；对冲突 key 需要 sequencing/watermark/recorded merge。

Pure valuation task 带 context version、generation；owner 安装前验证。持续行情可能导致永远 stale，使用 per-key one outstanding + dirty-again、unit valuation、consistent snapshot batch 或有界 stale policy。Work stealing 不移动 mutable ownership。

**English answer:** I would partition mutable state by business ownership, preserve single-writer semantics, and use work stealing only for independent computation. Different stages can require different partitioning keys. A configurable mapping of logical processors to physical threads lets us optimize communication and NUMA locality without changing the business model.

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

### 4.6 订阅状态机与冷启动协议

~~~mermaid
stateDiagram-v2
    [*] --> Unsubscribed
    Unsubscribed --> Subscribing
    Subscribing --> SnapshotLoading: registration confirmed
    SnapshotLoading --> CatchingUp: snapshot validated
    CatchingUp --> Ready: suffix applied
    Ready --> Stale: gap or freshness failure
    Stale --> Recovering
    Recovering --> SnapshotLoading: new snapshot
    Recovering --> CatchingUp: replay available
~~~

协议建议：Subscribe(requestId,keys,lastEpoch,lastAppliedOffset)、Ack(subscriptionId,streamEpoch,cut)、SnapshotBegin(id,cut,chunks)、Chunk(index,data,checksum)、End、LiveUpdate、Heartbeat(progress)、ResyncRequired。由 owner 有序地建立 subscription + snapshot boundary，后台编码 immutable snapshot，避免整表序列化阻塞关键路径。

先 snapshot 后订阅存在 gap；先订阅后 snapshot 必须 buffer live 并识别精确 cut。Cold RTT 包含注册、snapshot build、传输、install、catch-up；first byte 不代表 ready-to-trade。Prewarm connection 不等于预同步风险；可预订阅、缓存版本化 snapshot、限制范围、合并 ACK 与初始响应。Cold/Warm 分开测量。

### 4.7 Versioned publication 与 conflation

~~~cpp
struct StateUpdate {
    TopicId topic;
    KeyId key;
    uint64_t ownerEpoch, keyVersion, streamOffset;
    uint64_t inputWatermark;
    bool tombstone;
    RiskValue value;
};
~~~

epoch 必须经权威 handover 验证，不是任意更大值都可信。keyVersion 仅在对应 key/epoch 比较；offset 用于 stream progress。Selective fan-out 的 global gap 可能正常。删除需要 tombstone/generation，防止 stale key resurrection。

~~~cpp
void update(KeyId k, RiskValue v) {
    auto& s = states[k];
    s.latest = v;
    ++s.version;
    if (!s.dirty) {
        s.dirty = true;
        pending.push_back(k);
    }
}
void flush(size_t maxItems) {
    while (maxItems-- && !pending.empty()) {
        auto k = pending.front();
        auto& s = states[k];
        if (!tryPublish(k, s.version, s.latest))
            break;
        pending.pop_front();
        s.dirty = false;
    }
}
~~~

单 owner、无重入、容量足够、version 不溢出、tryPublish 仅 transport acceptance。多 subscriber 必须各自 tracking/cursor，不能因A成功就标B成功。队头持续失败需隔离或公平 retry/resync。

### 4.8 Shared state 与 backpressure 边界

Atomic scalar 不保证 multi-field consistency；普通 payload 的 seqlock 可能 C++ data race，即便 retry；immutable RCU 需 reclamation；shared_ptr atomic 不保证 lock-free；double/triple buffering 需确认 reader 不再访问。Cross-process 不传裸 pointer，使用 offset/handle 与平台支持的同步。Broadcast log 需 slow reader gating/resync。

Gap→标 stale，replay 或 full snapshot；heartbeat alive 但 applied offset 不动→计算不健康；publisher failover→验证新 epoch 与 cut；subscriber reconnect→last applied 不是 last received。Gateway 未同步不能把风险设零。Aeron offer/Archive record/commit/business apply 分开定义。若生产持续快于消费，有限 queue 最终耗尽；UI 与权威流隔离。

**English answer:** I would separate lossless business events from conflatable derived state, subscribe selectively, bootstrap from a consistent snapshot and catch up on the live stream. I would track epochs and processing progress and isolate slow subscribers. A local risk replica is not permission to consume shared credit or inventory.

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

### 5.7 多 CAS 反例与授权不变量

Available10，A/B各请求7：A load10、B load10、A fetch_sub→3、B fetch_sub→-4，即使 seq_cst 也合法。单 counter CAS success 是 linearization point；relaxed 足以维护独立数值约束，不发布关联 state。CAS lock-free 不等于 wait-free。

两个 limit A10/B5，请求A7/B7：先 CAS A 成功、B失败，期间其他请求可能看到A3而误拒；crash 可能留下悬挂预留。只在全部成功时发单、失败精确幂等回收，可以保守维护独立上界，但没有 all-or-nothing visibility。Rollback 本身也需要身份，重复释放可能导致超发。

Signed exposure 例：limit [-100,100]，当前+80，working buy40/sell40。仅按净预期+80检查会漏掉 buy全成交而sell未成交的+120。保守区间[40,120]，超上限。除非有明确原子成交约束，不假设未成交 hedge 抵消。

### 5.8 Reservation lifecycle 与 idempotency

~~~mermaid
stateDiagram-v2
    [*] --> Reserved
    Reserved --> Working: send
    Reserved --> Rejected: confirmed not sent
    Working --> Partial: fill
    Partial --> Filled: final fill
    Working --> Filled: full fill
    Working --> CancelPending
    Partial --> CancelPending
    CancelPending --> Cancelled: terminal confirmed
    CancelPending --> Filled: remaining filled
    Working --> Unknown: disconnect
    Partial --> Unknown: crash
    Unknown --> Filled: reconcile
    Unknown --> Cancelled: reconcile
~~~

CancelPending 收到 partial fill 后仍可能 pending cancel；生产可用正交状态字段。

~~~cpp
struct Reservation {
    OrderId order;
    RequestId request;
    GrantId grant;
    int64_t initial, remaining, consumed, released;
    uint64_t epoch;
};
~~~

固定模型 initial=remaining+consumed+released。Fill 按 execution ID 去重，cancel terminal 才释放未成交部分。Replace 若旧新单可能同时生效需同时覆盖。Request record 存 ID、参数 fingerprint、result、commit position；重复相同参数返回原结果，不同参数同 ID 报 conflict。Dedup 与状态修改必须共用 recovery boundary。

### 5.9 Escrow transfer protocol

初始 L100、A40、B40、central20，转移A→B10：

| Phase | A usable | B usable | Central | In transit |
|---|---:|---:|---:|---:|
| Initial | 40 | 40 | 20 | 0 |
| A durable freeze | 30 | 40 | 20 | 10 |
| Authority commit | 30 | 40 | 20 | 10 |
| B install | 30 | 50 | 20 | 0 |

Transfer ID、source grant/epoch、target、amount、phase 持久化。Freeze ACK lost→查询/重试同 freeze，不假设超时已成功；grant ACK lost→重发同 grant ID，不能新发一份；abort 需证明 target 未获得或已 fence。旧 owner 不能继续使用已回收额度。中央账本必须能解释每份权利唯一归属。

Lease 到期不证明暂停进程不能恢复发单。Fencing 在真正的 external side-effect boundary 检查，或通过独占 session/process/network enforcement。已在 venue working 的 order 不会因 fencing 自动撤销。Partition 中本地有效 escrow 可按约定继续交易，但全局 new grant/revoke 仍受 quorum/协调约束。

### 5.10 Validation / interview

- Invariant：每份可用/预留/消耗/转移中权利不重复计算，固定资源守恒。
- 每个已发 order 有对应有效 reserve/intent。
- Crash every step：freeze、grant、send、dedup、outbox ACK。
- 重复 request/transfer/fill/cancel 不产生第二次经济效果。
- Dynamic limit reduction 和 market breach 不掩盖真实 exposure。
- 2PC prepared participant 不能自行超时释放；consensus 不等于跨 shard transaction。
- TLA+ 等有限模型可检查协议 safety，但不是生产实现已被证明。

**English answer:** Multiple atomic counters do not provide an atomic multi-resource transaction. I would identify the consistency boundary, use a single owner where possible, and distribute exclusive escrow rights for safely additive constraints. Transfers must revoke old rights before granting new ones. Nonlinear risk requires a validated conservative envelope or centralized authorization.

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

### 6.6 ACK Semantics 与 durable commit

| ACK | Guarantee / risk |
|---|---|
| Received | ingress accepted; may still be volatile |
| Appended | page cache or local buffer; power failure may lose |
| Durable | survives specified local storage failure model |
| Quorum committed | replicated according to protocol and persistence rules |
| Applied | business transition executed |
| Published | transport accepted output, consumer may not apply |
| Business response | outcome sent; response itself may be lost |

Binary record：magic/version、length、stream/epoch/seq、event ID、timestamp、payload、CRC；decode 检查长度、边界、schema，不能 dump native struct ABI。CRC 不等于 commit marker。Linux write 与 fsync、directory fsync、storage controller 的保证需明确。

Group commit 假设 sync50µs、500K/s、batch32：单条理想20K/s，批量640K/s，amortized1.5625µs/event。恒定输入必须凑满时第一条等62µs、平均31µs，再加 sync；使用 max batch + max wait。Independent journal thread 可 overlap CPU，但 durable-before-response 仍须等待 commit watermark。实际由 filesystem、controller、I/O depth、replication 等决定。

### 6.7 Snapshot install protocol

~~~mermaid
sequenceDiagram
    participant O as Owner
    participant W as Snapshot writer
    O->>W: Immutable state + cut N
    Note over O: Continue applying N+1...
    W->>W: Write tmp, checksum, fsync
    W->>W: Rename, fsync directory
    W->>W: Durable manifest
    Note over W: Preserve previous valid checkpoint
~~~

Manifest 包含 format/schema、checkpoint ID、offset vector、epoch、model/reference versions、file list/checksum。后台直接读取 owner 正在修改的 ordinary fields 会 data race；使用 copy/CoW/MVCC。CRC 正确不代表 snapshot 一致。恢复选择最新完整兼容 manifest，不按 mtime 取半成品。

Upstream position at1000、downstream aggregation at980、20 delta in flight：如果两个 snapshot 恢复后不重发 suffix，聚合永久少20。记录 channel state、replay derived log 或各 view 从共同权威输入重建。多输入 barrier 要满足依赖 cut，而不是每个 source 取最大 offset。

### 6.8 Recovery steps and unknown order

1. Verify journal segments and committed prefix.
2. Load latest complete compatible checkpoint.
3. Restore dedup, revisions, epoch, pending transactions/outbox.
4. Replay committed suffix, suppress historical external side effects.
5. Catch up live, identify gaps.
6. Reconcile unknown orders, fills, grants, collateral, settlement.
7. Enforce fencing and publish ready only when invariants hold.

Durable outbox 将 state/dedup/intent 放同一可恢复 commit boundary。Sender 使用 stable identity；若 venue 无可靠 dedup，send 后 crash 必须 session recovery/status/drop copy reconcile，不盲目重发。Unknown order 保留风险覆盖。

Business determinism（accept/reject/identity）与 numerical tolerance、bitwise determinism 区分。后者需固定 reduction order、FMA、rounding、compiler/CPU/library/model 等；不能默认所有 analytics 都要求 bitwise。历史 reproduction 使用历史版本，新模型计算是 restatement。External RPC/time/random/timer/merge 需记录或确定重建。

### 6.9 Quantitative recovery & fault testing

停机60s、live500K/s积压30M，replay2M/s时20s追平；若另有detect2s/load5s，积压再增3.5M，catch-up22.33s。recon10s若可与 live processing 并行且无额外等待，总示例RTO39.33s。真实需测 storage、reference rebuild、权限生效。

| Injection | Verification |
|---|---|
| Partial journal write | detect invalid tail; only committed prefix |
| Disk full | no false durable ACK |
| Snapshot crash | previous checkpoint valid |
| Duplicate after restart | one economic effect |
| Corrupt middle segment | alarm and replica recovery |
| Leader crash after commit | same request returns same result |
| Old owner resumes | no new external effect |
| Replay while live | catch-up headroom and freshness |
| Schema/model upgrade | compatible decode or migration |
| Independent rebuild mismatch | investigate before activation |

三节点集群能容忍一节点故障不代表机房/电源共同故障也能 RPO0。所有 durability/RTO/RPO 结论必须附 failure model。

**English answer:** I would persist authoritative events with explicit commit progress, checkpoint state at matching offsets and replay the committed suffix. Recovery must reconcile unknown external orders and reservations, enforce fencing, and validate state before trading resumes.

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

### 7.8 完整交易生命周期与 FIX Order State

Order state 与 trade state 必须分开：一个 filled order 的 execution 仍然可以被 bust/correct；一个 canceled order 也可以有已经发生的 fills。Order status 是剩余订单的可执行状态，不是经济交易是否存在。

| Message (FIX 4.4 example) | Purpose | Risk action |
|---|---|---|
| 35=D NewOrderSingle | submit order | reserve before send |
| 35=8, ExecType=0 | new accepted | keep working reservation |
| 35=8, ExecType=F | trade | apply unique LastQty/LastPx |
| 35=F OrderCancelRequest | request cancel | do not release |
| 35=8, ExecType=4 | cancelled | release confirmed remainder |
| 35=9 OrderCancelReject | cancellation rejected | retain applicable reservation |
| 35=G OrderCancelReplaceRequest | change order | cover revised/possibly coexisting order |
| 35=8, ExecType=5 | replaced | adjust reservation under venue semantics |
| 35=8, ExecType=G/H | trade correct/cancel | revise canonical economic contribution |

具体 venue 可能使用不同 FIX version、字段约束及消息顺序。不要只依赖 OrdStatus 或 MsgSeqNum 推断经济事件，必须读取相应规范。

Example order 1000 shares @ limit 11：300@10、500@10.1、200@10.2，weighted execution price=(3000+5050+2040)/1000=10.09。CumQty300/800/1000，LeavesQty700/200/0。若第二笔 revision 从500@10.1变450@10.2，canonical economic changes 为 Δqty=-50、Δcash=5050-4590=460（buy cashflow）。OMS 是否恢复50 working quantity 由 venue order lifecycle 定义，不能仅按 trade correction 推断。

### 7.9 Canonical identity / revision data model

~~~cpp
struct ExecutionIdentity {
    VenueId venue;
    SessionNamespace scope; // venue-defined; not merely transport session
    ExecId execId;
};
struct TradeRecord {
    TradeId tradeId;
    uint64_t revision;
    BookId book;
    InstrumentId instrument;
    CounterpartyId counterparty;
    NettingSetId nettingSet;
    int64_t signedQty;
    Price price;
    CurrencyId currency;
    Date valueDate;
    bool active;
};
struct TradeChange {
    EventId eventId;
    uint64_t expectedRevision;
    std::optional<TradeRecord> before, after;
};
~~~

Raw execution report 包含 transport seq 与 source ID；normalized event 保留原始引用；canonical record 代表经济事实。ExecID scope 由 venue adapter 定义，必要时包含 trade date、market、broker 或 participant。Transport retransmission、same execution from drop copy、economic correction 分三层去重。

| Case | Action |
|---|---|
| same source seq replay | protocol dedup / recovery |
| same canonical execution, same economics | idempotent no-op |
| same identity, different economics | conflict quarantine / reconcile |
| new correction referencing known revision | validate and revise |
| correction before referenced original | durable pending dependency |
| two corrections reference same old revision | resolve according to venue revision semantics |

建立 ExecutionIdentity→TradeId 与 TradeId→current revision；不能简单按新 correction ExecID hash 到另一个 owner。Raw capture 即使缺 instrument metadata 也要保留；risk health 暴露 incomplete exposure，必要时阻止新增风险。Missing data 不应伪造零值。

### 7.10 Trade capture pipeline 与 cross-source reconciliation

~~~mermaid
flowchart LR
    OE[Order-entry execution] --> RAW[Raw durable capture]
    DC[Drop copy] --> RAW
    RAW --> DEC[Normalize / session identity]
    DEC --> ID[Business identity resolution]
    ID --> TR[Trade revision owner]
    TR --> OUT[Canonical journal and outbox]
    OUT --> P[Position / economic risk]
    OUT --> A[Allocation / booking]
    A --> REC[Clearing / settlement recon]
~~~

典型 trade capture 并不一定只有一个 authority；order system、trade ledger、clearing record 各自有不同权威边界。Same trade 两个来源时可以 first valid arrival 降低延迟，但冲突需按 source contract 处理，不是 first-wins。直接从 OMS 更新即时 reservation，然后通过 canonical event reconcile，不可再次扣减。

Allocation：parent execution1000，child A400/B350/C250，firm total1000，不是2000。若初始未分配，则 unallocated1000→0，A/B/C增加对应值；跨 shard atomic visibility 或独立 firm economic view。Allocation correction 与 parent bust 要按 lineage 协调，不能先删除 parent 再放任 children 生效。

Native multi-leg execution 可在原子经济边界发布；synthetic order 各腿可能独立成交，必须及时反映 unhedged position。FX swap far/near value date、forward points、cashflow currency、NDF fixing 与 settlement 等字段不可在标准化过程中丢失。

### 7.11 RPC placement、late events 与容量

对于 client-facing flow 约2ms 的假想业务场景，post-trade capture RPC 可以是 business operation，但需要分清：
- query：本地订阅可替代每笔查询；
- command：执行结果必须幂等、可恢复；
- transactional dependency：即使异步发送仍需等待业务 ACK 才能完成特定状态转换；
- actual fill：任何下游 RPC failure 都不能拒绝记录已经发生的交易。

EventTime、IngressTime、CaptureTime、EffectiveTime 分开。Late correction 可以按原经济 effective time restate accounting view，同时即时修正当前 exposure；必须记录两套时间与 revision。跨 venue 不用墙钟时间强行推导全局顺序。Window reorder 用延迟换顺序完整性；如果 risk 不允许等待，应即时处理经济风险，随后修正 accounting。

50K fills/s、256B/record→12.8MB/s；order entry + drop copy 两份输入可能100K raw/s，但 unique economics50K/s。8h peak journal368.64GB；每条128B dedup state若全部驻内存184.32GB。实际用 hot table、historical index、cold journal，TTL 由合法 retransmission/correction horizon 决定。Bloom filter 只用于减少 lookup，不可凭可能命中丢成交。

### 7.12 面试追问与测试

| Question | Expected discussion |
|---|---|
| Can I use FIX MsgSeqNum as TradeId? | transport vs business identity |
| How to handle late correction? | revision chain, before/after, WAC replay |
| Can drop copy replace order entry? | latency, coverage, source authority, session recovery |
| What happens if booking fails after execution? | preserve actual trade, retry booking, risk exposure remains |
| How to process a multi-leg fill? | atomic contract vs independent leg execution |
| How to avoid duplicate effects after restart? | canonical identity + durable state/offset/outbox |

Fault test：duplicate source、out-of-order correction、same ID conflict、allocation after bust、partial fill during cancel、schema/metadata missing、crash after journal before publication。独立比较 OMS/external clearing/canonical trade ledger 与 position/cashflow/reservation。修复通过受控 revision/recovery event，而不是直接修改风险 counter。

**English answer:** I would capture both order-entry and drop-copy executions, normalize venue-specific identifiers into canonical business identities and maintain explicit trade revisions. I would durably record economic changes before publishing them, apply corrections as before-and-after contributions, and keep booking and settlement status separate from actual execution exposure.

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

### 8.8 Locate、Borrow、Long Inventory 的完整语义

不同市场与 broker 的 locate/short selling/settlement 规则不能从一个通用 counter 推导。美国 Regulation SHO 的 locate 通常涉及 broker-dealer 在规定条件下有合理依据相信证券可借用于交割；不是通用意义上的实际借券保证。是否允许 locate reuse、哪些豁免适用、如何标记 short、何时 close-out 均需查实际规则和 broker policy。新加坡适用不同规则。以下只设计内部授权模型。

| State | Represents | Not equivalent to |
|---|---|---|
| Economic position | long/short trade exposure | settled stock balance |
| Settled inventory | settled ownership/entitlement | necessarily unrestricted inventory |
| Pending settlement | future deliver/receive obligations | available now |
| Locate grant | permission/evidence under policy | actual borrowed stock |
| Stock loan | actual borrow with return obligation | short position |
| Reservation | exclusive rights for outstanding order | completed execution |

假设有 long300、sell order500，如果无其他 competing sells，且 policy 允许按经济库存分配，则 long portion300、short portion200，需要相应授权。另一个并发 sell300 不能也使用同一 long300。未成交 buy 不应无条件计入可用库存；marking、aggregation unit、settlement eligibility 按市场与 broker 定义。

### 8.9 Grant-level state 与 reserve/release

~~~cpp
struct LocateGrant {
    GrantId id;
    InventoryPoolId pool;
    InstrumentId instrument;
    int64_t granted;
    int64_t free;
    int64_t reserved;
    int64_t consumed;
    TimePoint expiry;
    uint64_t ownerEpoch, version;
    ReusePolicy reuse;
};
struct GrantAllocation {
    GrantId grant;
    int64_t remaining, consumed, released;
};
~~~

同 pool 的 grant 不一定完全 fungible：broker/clearing account、legal entity、settlement date、hard-to-borrow restriction、expiry、reuse policy 影响选择。Order reservation 应记录实际使用的 grant allocations，不能只维护 total remaining。

固定 grant 1000 shares：A reserve600，B申请500拒绝或按协议 partial400；A fill400，剩余 reserved200；confirmed cancel释放200。最终 free600/consumed400/reserved0。Buy-to-cover100 不自动恢复 consumed locate；取决于批准的 reuse policy，并区分实际 stock loan return。

### 8.10 Dynamic supply / recall / settlement deficit

固定授权期间 free+reserved+consumed=S。若 S 下降，令 U=consumed，R=reserved，简化：
$
A=\max(0,S-R-U),\quad D=\max(0,R+U-S)
$
$
A-D=S-R-U
$
D 是授权覆盖缺口（不是自动等于实际交割失败或 borrow recall）。例如 S1000→500，R200、U600，A0/D300。必须保存真实 obligations，暂停新增、按政策撤销工作单、补充来源。已发订单无法因 expiry/revoke 自动消失；late fill 仍记录。

实际 borrow recall 属于 stock loan ledger：lender 要求归还，可能用已有证券、替代借券、buy-in/cover 等方式处理。Borrow recall 不应直接修改 short position。Cover 成交后仍可能需等待结算或实际 return 才减少 outstanding loan。

### 8.11 Settlement curve 与数据结构细化

Settled inventory + future eligible receives - deliver obligations - restrictions/commitments。必须确保同一 sell/working reservation 不重复扣减。对按日期确定的现金流模型，每个新 sell at date d 影响所有 t≥d 的 projected inventory。

例：initial10k、D1 +5k、D2 -12k、D3 +10k，projected [10,15,3,13]k；D2新增sell5k后[10,15,-2,8]k，最终余额正但中间失败。Lazy segment tree 对 suffix rangeAdd(-q) + rangeMin 检查 O(log T)。若 horizon 很短，固定数组扫描 O(T) 可能更快且简单。Fenwick 适合 prefix sum，但一般不能直接支持任意 signed range-add 后 range-min。若需要按日/账户/settlement venue 管理，则增加独立 pool 与维度。

Corporate action（split、symbol change、merger）需要更新 security identity、qty conversion、borrow/grant terms 及 pending obligations；不能只乘 position 数量。日期和 calendar version 需明确，T+1/T+2 由具体市场与产品决定。

### 8.12 Multi-source sourcing & distributed allocation

Sourcing 属于 cold/warm path：broker quote/indication、stock loan availability、rate、expiry、eligibility。简单独立同质供给线性成本时按 rate ascending greedy；现实中 overlapping lender、min lot、borrow duration、cross-account constraints 可形成 min-cost flow/LP/MIP。外部 indication 不等于已确认 exclusive grant。

~~~mermaid
flowchart TD
    PB[Broker / lender updates] --> IA[Inventory authority]
    IA --> GL[Grant / stock loan ledger]
    GL --> AL[Exclusive budget allocation]
    AL --> G1[Gateway 1 local grants]
    AL --> G2[Gateway 2 local grants]
    G1 --> RES[Order reserve / fill / cancel]
    G2 --> RES
    RES --> GL
    TC[Canonical trade and settlement] --> GL
    GL --> REC[Borrow / delivery reconciliation]
~~~

Hot path 不同步向 broker 获取 locate：预先批准 grant，单 owner check-and-reserve；不足则 slow path 或 reject。采用 sparse per-demand grant mapping，避免 100k instruments×32 gateways×8 threads=25.6M 空闲组合。Revocation/rebalance 使用 R5 的 revoke-before-grant/fencing，并增加 expiry/effective boundary。授权未收到、过期或不确定时，不默认 available。

### 8.13 Interview questions

- Why is a locate not the same as borrowed stock?
- How do you reserve both long inventory and short locate without oversubscription?
- What happens to a working order when its locate expires?
- Can a buy-to-cover automatically replenish locate capacity?
- How would you handle a recall during a network partition?
- How would you efficiently check future settlement balances?
- How do you reconcile stock loan obligations with economic positions?

**English answer:** I would maintain separate economic position, locate-grant and stock-loan ledgers. I would reserve eligible securities or exclusive locate rights before sending an order, consume them on fills and release only confirmed unused quantities. Grants would be scoped by legal entity, instrument, account, settlement and broker terms, with explicit expiry and revocation handling. I would use distributed escrow only where the rights are safely transferable.

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

### 9.7 Greeks、Sensitivity Unit 与 Risk Factor Identity

对于同一 portfolio，delta 可以是 shares、base-currency amount、reporting-currency delta per 1% move 等不同口径。不能仅存 double delta。推荐：

~~~cpp
struct Sensitivity {
    RiskFactorId factor;
    SensitivityKind kind;
    CurrencyId reportingCurrency;
    SensitivityUnit unit;
    double value;
    uint64_t modelVersion;
};
~~~

Gamma 需要两个 factor ID（cross gamma 可包含 spot-vol、不同曲线 tenor），明确二阶展开是否含 1/2 系数、单位缩放、对称项是否存两次。对于 off-diagonal H_ij，完整对称矩阵的 1/2 会抵消双计；只存上三角时不能再错误乘一次1/2。

Delta/Gamma 的误差例：delta10k、gamma2k、ΔS=-2，二阶近似 -20k+4k=-16k；Gamma adjustment 与方向无关（此处 gamma>0），但在大幅价格变化、barrier、expiry 附近局部展开可能严重失效。对 option，时间、vol surface、rates 同时变化时不能只用 spot delta。Sensitivity-based approximation 不是天然安全上界。

### 9.8 Incremental covariance code 与复杂度推导

~~~cpp
struct PortfolioVariance {
    size_t n;
    std::vector<double> sigma; // symmetric row-major n*n
    std::vector<double> w, u;  // u = sigma*w
    double variance;
    void change(size_t k, double d) {
        const auto* row = sigma.data() + k*n;
        variance += 2*d*u[k] + d*d*row[k];
        for (size_t i=0; i<n; ++i)
            u[i] += d*row[i];
        w[k] += d;
    }
};
~~~

初始化 u 与 variance 需一致；固定对称 covariance、finite inputs、checked index。单因子 variance scalar 调整 O(1)，但 u 更新 O(N)，完整 O(N)，不能称总体 O(1)。多因子稀疏 k changes 可逐次 O(kN)；dense batch 用 matrix operations。Covariance 更新改变全部缓存，需 rebuild/version swap；长期 floating-point drift 定期 full calculation reconciliation。小负 variance 可能数值误差，但明显负值应诊断 PSD/算法，不能无条件 clamp。

Factor model：若 F 固定 PSD、F=LL^T，预计算 a_i=L^T B_i^T；维护 y=Σ_i w_i a_i，variance=||y||²+Σ_i D_iw_i²。单 w_i delta d：y+=d a_i，residual+=D_i(2w_i d+d²)，O(K)。如果 factor covariance 或 loading 变化，需要重新计算对应变换与暴露。额外预计算 O(NK²)（dense naïve）、storage O(NK)，与更新成本 trade-off。仅缓存 Fz 而未预计算每个 loading 的变换时，稠密 loading update 可能仍 O(K²)。

### 9.9 Quantile & ES：离散样本边界

设等权损失 [0,1,2,2,10]，alpha=0.8，nearest-rank VaR=2；最坏20%只有样本10，ES=10。若错误平均全部 ≥VaR，则(2+2+10)/3≈4.67，显著低估 tail。

对 n 个等权损失按 descending 排序，tail mass m=(1-alpha)n，累加 floor(m) 个完整样本，再加剩余 fractional weight 的下一个样本，除以 m。对于加权 scenarios 按 cumulative probability 处理。nth_element 可降低单 quantile 的期望复杂度，但需保留 scenario identity 与权重对齐；分位计算最好在 scratch copy 上执行。

VaR(A+B) 不等于 VaR(A)+VaR(B)。在相同 scenarios，先 componentwise sum，再计算 loss quantile/ES。Correlation stress 可能指重建 covariance/simulation model，而不是直接把一个 correlation number 当成某个 instrument 的 price shock。Historical shocks 应保留因子共同变化；不盲目独立抽样每个 factor。

### 9.10 Scenario matrix、parallel tiling 与 consistent context

~~~mermaid
flowchart TD
    CTX[Immutable market/position/model/scenario context] --> T1[Instrument block 0 x scenario block 0]
    CTX --> T2[Instrument block 0 x scenario block 1]
    CTX --> T3[Instrument block 1 x scenario block 0]
    T1 --> RED[Version-aligned reduction]
    T2 --> RED
    T3 --> RED
    RED --> Q[Portfolio loss distribution]
    Q --> V[VaR / ES / stress]
~~~

Unit scenario valuation 可以共享的前提是相同经济条款、model、market snapshot、reporting currency、collateral/funding assumptions。若 pricing 本身受 portfolio nonlinear effects 影响，不能简单 Q×P。

Instrument-major 布局适合一笔 fill 更新连续 scenarios（AXPY），scenario-major 适合单场景全仓扫描，blocked layout 兼顾 reuse。Pure task work stealing 缓解 exotic vs spot valuation cost skew；owner/aggregator 按 tile ID 与 context 检查重复、缺失与过期。Full snapshot result 不允许混用新 position/旧 market，除非业务明确定义 mixed live view。Position version 与 market version 分别递增，不能用任意单一 max 比较。

100k instruments×1000 scenarios×5µs=500 CPU seconds；32 cores、70% parallel efficiency≈22.32s。若目标1ms，不是简单 SIMD 或加线程可解决，应明确 fast incremental metric 与 periodic accurate view。GPU 只有在数据复用、batch size、transfer/launch、precision 满足条件时才有价值。

### 9.11 Stress test / numerical validation / model risk

Stress scenario 应指定 shock mapping、base curve version、dependent curve rebuild、保持不变的参数、time horizon。举例 USD yield +200bp，重新构造 discount factor 与 FX forward curve，而不是直接把所有 PV 乘一个比例。Liquidity stress 可能需要 spread、market depth、liquidation horizon，不能只看 mid price。

Validation:
- analytical Greek vs finite difference with h sweep；
- independent pricing reference 与 zero shock；
- linear scaling 与 equal opposite trades offset；
- covariance symmetry/PSD、factor reconstruction；
- incremental vs full calculation after long random sequences；
- same context with different scheduling/reduction order；
- scenario completeness、missing prices、NaN；
- VaR backtesting：预测 exceedance rate 及 clustering，统计模型风险与软件 correctness 分开。

**English answer:** I would separate fast sensitivity-based incremental risk from expensive full revaluation. A reverse dependency index limits recalculation. For portfolio VaR and ES I would aggregate scenario-level PnL or compatible factor exposures before computing nonlinear statistics. I would parallelize immutable versioned tasks, use data layouts matched to access patterns and validate approximations against independent revaluation.

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

### 10.7 Netting Set 不是 Counterparty Name

例：同 counterparty 两个 legal netting sets A MTM+5M、B MTM-4M。若不能跨 set 净额，则 CE=max(5,0)+max(-4,0)=5M，而非 max(1,0)=1M。Collateral 也只能按实际协议与认可条件使用；不能将同一份 collateral 重复降低多个 set exposure。

~~~cpp
struct NettingSetState {
    double totalMtm, eligibleNetCollateral, currentExposure;
    uint64_t marketVersion, collateralVersion;
};
void onMtmChange(NettingSetState& s, double oldV, double newV) {
    auto before = s.currentExposure;
    s.totalMtm += newV-oldV;
    s.currentExposure = std::max(s.totalMtm-s.eligibleNetCollateral,0.0);
    publishDelta(s.currentExposure-before);
}
~~~

单 owner、简化 CE 模型、finite validated inputs。MTM delta+3M、旧 net MTM-2M→+1M，CE delta仅+1M。Trade revision、FX conversion、collateral update 同样产生 nonlinear derived delta；聚合前需要正确的 legal/model context。Actual regulatory RC/SA-CCR 可能有更复杂的 margin formula，不能直接照用。

### 10.8 PFE profile and aggregation boundary

未来不同 t 下 E(s,t)=max(sum PV_i(s,t)-eligible collateral(s,t),0)。每 t 计算 EE、PFE；EPE 为在定义时间区间的加权平均 exposure（具体 regulatory definition 有区别）。Portfolio maturity 可能使 profile 下降，但 path、collateral、optional exercise 会改变形状。

同样 scenarios 下应先按 netting set 聚合 MTM，再 floor，然后才对多个 non-nettable sets 聚合适当的 exposure，不能先对每 trade 取 positive 再 sum，也不能跨 legal boundary 先 net。PFE quantile 与 SA-CCR add-on 命名区分。Wrong-way risk 要考虑 exposure 与 credit quality 的依赖；独立 market paths + fixed PD 可能不够。

10B trade-time-scenario PV 不必全部保存；可 tile by time/scenario/trade，直接 reduce 到 netting set cube，或按 time slice 求 quantile。若需要 path-dependent collateral/margin simulation，时间点未必可任意并行；必须保持 path dependency。Correction/incremental model 需要保留足够贡献信息或支持重算。

### 10.9 Credit authority state 与多维额度

| Constraint | Charge example (illustrative) |
|---|---:|
| Counterparty group | 500K |
| Legal entity | 500K |
| FX forward product | 500K |
| 1Y-5Y tenor bucket | 300K |
| Settlement | 1M |

不同约束单位与 risk model 可能不同；不是每个 limit 都扣同一 notional。子 limit 合计可以大于 parent，因为它是约束上限，不是已授予 rights。每个资源维度都独立守恒，gateway 必须获得所有必要 budget 才发单。

Credit calculation state 与 authorized budget ledger 独立。不能看到 derived exposure 减少就自动发放额外权利：需 reconcile pending/working reservations、source offsets、netting/valuation model、existing granted rights。授权额度的消耗与真实 exposure 的计算可以不同，但差异必须由保守模型或明确业务规则覆盖。

### 10.10 Nonlinear marginal risk counterexample

令 R(q)=q²（教学用非线性风险），初始 q=0，A/B 各+1。各自从同一 snapshot 得 ΔR=1；真实组合增量 R(2)-R(0)=4，不等于2。即使 read linearizable，边际风险仍不可直接相加。

方案：
1. Centralized authoritative state 对所有冲突交易按序 check-and-reserve。
2. 在有界 position/market domain 上证明每笔 charge 的 conservative upper bound。
3. 固定 scenario vector 的 per-scenario nonnegative envelope，可能比 quantile constraint 更严格。
4. Hybrid：普通产品和小单用本地批准表，超出范围的复杂订单交中央。

Scenario budget 例 S1剩10M/S2剩10M/S3剩5M/S4剩20M，请求[4,2,6,1]M，S3失败。O(S)；可 SIMD，但必须检查 NaN/invalid capacity、负 charge、version、duplicate resource。Scenario/model change 需要安全撤销或重新验证旧 grants，不能把旧 scenario budget 按新含义直接使用。

### 10.11 FX streaming and pending local overlay

Executable quote 的 reservation point 取决于是否 firm/binding、允许并发 hit 的数量及 cancel/expiry 规则。若两条各10M报价都可同时 hit，而全局 credit10M，独立读取无法保证。可预分配 quote execution capacity、在 execution request 做 local atomic check 或按业务协议控制 binding quote。

Pending overlay 示例 central used60M，local fill5M尚未被包含→effective65M；central publish65M且 appliedOffset 已涵盖 fill→移除 pending5M，effective仍65M。若 central65M 由其他交易造成，而本地 fill 未被涵盖，则 effective70M；仅比较数值不能判断。Central value 与 applied offsets 必须来自相同 state boundary。

Limit reduction 与 market-driven breach 要区分 authorization bug。Fixed charge 下 L100→60、used70/reserved10，available0/deficit20。先停止不安全新授权，安全回收 grants、处理 working orders；不强行改 used。Collateral request、delivery、settlement、recognition 分阶段。FX PvP 消除受保护交易的相应 principal risk，不消除所有 credit/liquidity risk。

### 10.12 Interview and operational metrics

| Question | Core reasoning |
|---|---|
| CE vs PFE vs EAD? | current vs future distribution vs regulatory exposure |
| Why netting set sharding? | legal boundary and nonlinear floor |
| Why not directly partition VaR/PFE? | non-additivity / concurrent marginal risk |
| Can credit calculation and check share one state? | freshness ≠ exclusive authorization |
| How to reduce false rejection? | dynamic refill, approved charge model, safe reallocation |
| What if collateral or limit changes? | version/effective time, breach, revoke/fence |

Measure local check p99/p99.9, remote fallback ratio, refill latency, idle granted capacity, reservation leakage, true/false rejection（需可验证参考决策）、netting/calculation lag、pending overlay age、revoke enforcement lag、external reconciliation. 不单独优化 check latency 而牺牲 credit utilization 和 safety。

**English answer:** I would calculate credit exposure at the legally valid netting-set level, taking collateral and future exposure into account. I would separate exposure calculation from authorization, generate sparse charges for hierarchical limits and distribute exclusive budgets where the credit measure supports safe decomposition. Nonlinear measures require a validated conservative envelope or centralized decisions, with explicit revocation and reconciliation.

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

### 11.7 Sequencer implementation：append、publication、commit

~~~cpp
struct SequencedHeader {
    OrderingDomainId domain;
    uint64_t epoch, sequence;
    RequestId request;
};
void doWork() {
    for (auto& in : inputs) {
        for (size_t i=0; i<maxBatch; ++i) {
            if (!pending) {
                auto m = in.tryPop();
                if (!m) break;
                pending = std::move(*m);
            }
            // Must retain the request if append fails; no silent loss.
            if (!tryAppend(nextSequence, *pending))
                return;
            ++nextSequence;
            pending.reset();
        }
    }
}
~~~

此代码只示意 single sequencer 的 append 路径，未实现 durable commit/replication、source acknowledgement、crash-safe pending queue。tryAppend 成功后仍须根据 log progress 完成可靠 commit 与 publish；append 结果不确定时不能盲目分配新 sequence，需查持久化状态。Atomic counter 给出唯一 ID 但不保证 publication order。Sequencer merge policy（round robin、timestamp、priority）决定逻辑顺序，不一定反映物理到达时间或 fairness。

### 11.8 RSM leader failure walkthrough

~~~mermaid
sequenceDiagram
    participant C as Client
    participant L as Leader
    participant F as Follower
    C->>L: Command with request ID
    L->>F: Replicate current-term entry
    F-->>L: Protocol ACK
    Note over L: Commit and apply
    L--xC: Response lost
    Note over L,F: Leader failure / new election
    C->>F: Retry same request ID
    F-->>C: Original committed outcome after recovery
~~~

简化示例假设 F 成为合法新 leader，满足 log/election/persistence requirements。Raft 三节点多数为2；current-term commit 的严格规则不可简化为任意2份副本。Leader may accept entries without quorum，但不能返回合法 committed outcome。未提交 suffix 可能被覆盖。Client timeout 不代表拒绝，也不代表未执行。Request result/dedup retention 必须覆盖合法 retry horizon。

Replicated state machine 的 deterministic timer：leader 生成有序 timer event，followers apply 相同逻辑；本地 wall clock 不能直接决定 release。External result 也需作为 sequenced input 或独立已提交 event。Replica 在相同 committed/applied prefix 等价，物理时刻可有 lag。

ReadIndex/linearizable query 需要确认 current leadership、确定 read index、等待 local applied 到该位置；读 stale follower 仅适用于允许陈旧的 view。Linearizable query 后再发单仍存在 TOCTOU。

### 11.9 Parallel execution、ROB 与 commit domains

Sequence [101:A,102:B,103:A,104:A+B]：101/102 independent 可并行，103依赖101，104需等待相关 A/B 状态。除了 read/write set，还要覆盖 external decisions、predicate/phantom、dynamic dependencies；不能只根据 instrument ID 认定无冲突。

若需要严格全局 retirement，可维护 result ring indexed by sequence，记录 generation 与 complete flag；最大 in-flight W，未退休前不可覆盖 slot。

~~~cpp
void complete(Result r) {
    // Single retirement owner, bounded window, validated sequence.
    buffer.insert(r.sequence, std::move(r));
    while (buffer.ready(nextToCommit)) {
        if (!tryCommit(buffer.peek(nextToCommit))) break;
        buffer.erase(nextToCommit);
        ++nextToCommit;
    }
}
~~~

101 pending、102-104 ready、105 pending、106 ready，watermark100；101完成后可到104；105完成后可到106。HOL blocking 由最早慢任务造成。对于独立 per-key publish，可不强制 global retirement，但要明确 observation/transaction semantics。完成结果不等于 commit，跨 key atomic transaction 另需协议。CPU ROB 是类比，不意味着 exchange side effects 可以 rollback。

Speculative pure work 与 quorum commit overlap：T≈max(T_consensus,T_compute)+validation（理想）。只在 commit 且 context valid 后安装/发布授权。未提交 command 可能被覆盖，不能提前发单。复杂 state mutation 需要隔离/MVCC/undo，比 speculative pure valuation 更难。

### 11.10 Aeron Cluster 与 LMAX 的工程边界

Aeron Transport 提供 IPC/UDP；Archive recording/replay；Cluster replicated ordered log/consensus/service lifecycle。三者的 offer、recording progress、quorum commit、business apply 是不同阶段。Cluster 业务 API 和 C++ Transport API 不同，具体支持语言、client/service integration 按版本核实。不要把 Archive replay 当作 consensus。

LMAX 公开模式强调单线程 business logic 与周边 parallel infrastructure。适用于串行 state mutation 成本足够低的 domain；全 firm 多维 valuation 不一定适用单 owner。Calvin deterministic scheduling 提供数据库研究中的相关思想，不是无需协调的跨 shard transaction。

### 11.11 Capacity、failure semantics、面试

单核 sequencing CPU200ns→5M/s ideal。2M/s×128B=256MB/s journal payload；两个 unicast followers4.096Gbps，不含 headers/retransmission。Window100、commit RTT50µs→2M/s ideal in-flight bound，仍受 CPU/storage/network/flow control。Pipelined replication 提高 throughput，不自动降低每笔 commit latency。避免复杂 risk calculation 与同步 RPC 占据整个 ordering domain。

| Failure | Response |
|---|---|
| missing sequence | explicit gap resolution / recovery |
| leader loses majority | no new lawful commit |
| old leader resumes | external fencing |
| follower lag | replay/snapshot install |
| client ACK lost | same request identity |
| replica divergence | compare at same logical position |
| slow task in ROB | bounded window / independent commit domains |
| snapshot includes uncommitted state | restore committed boundary |

**English answer:** A sequencer establishes logical order, not necessarily serial physical execution. I would partition independent ordering domains, maintain single-writer ownership and allow pure computations or non-conflicting operations to run concurrently. I would replicate critical authoritative logs using consensus, preserve deterministic business transitions, and distinguish completion, commit, publication and external side effects.

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

### 12.10 从 Event Rate 到资源需求：完整计算模板

先列 workload matrix：每种输入 i 的 λ_i（normal/peak/burst）、每 stage j 平均产生 a_ij tasks、每 task CPU service D_ij、bytes B_ij、访问 key skew。Stage CPU demand=sum λ_i*a_ij*D_ij；stage traffic=sum λ_i*a_ij*B_ij。不同 stage 的 messages/s 不能直接比较，必须换算到原始输入口径。

举例：
| Stage | Amplification | Service | Cores | Original-input capacity |
|---|---:|---:|---:|---:|
| Decode | 1 | 200ns | 1 | 5M/s |
| Valuation | 4 | 500ns | 4 | 2M/s |
| Aggregation | 20 | 150ns | 5 | 1.67M/s |
| Distribution | 5 | 200ns | 2 | 2M/s |

理想 bottleneck aggregation 1.67M/s，60%目标对应约1M/s；不含 queueing/overhead/skew。Shared physical thread 的多个 stages CPU demand 相加，不能分别使用同一 core 的100% capacity。

Little's Law 用于平均，不是 percentile。Burst λb>μ 时 backlog=(λb-μ)Tb，随后 λn<μ 才能 drain；新输入持续为 μ 时积压不消失。FIFO backlog 100k、service1M/s，新增 event 仅等待已有任务约100ms。对 burst 使用 queue age 与 admission threshold，而不是无限增大 buffer。

### 12.11 Bounded batch / queueing / latency budget

| Batch | CPU per event (50ns +200ns/B) | First formation wait @500K/s |
|---:|---:|---:|
| 1 | 250ns | 0 |
| 4 | 100ns | 6µs |
| 16 | 62.5ns | 30µs |
| 64 | 53.125ns | 126µs |

上述 wait 仅适用于恒定流量且必须凑满。Opportunistic draining 在空闲立即处理，繁忙形成 batch；bounded count/time quantum 防止某条 queue monopolize worker。Journal/GPU 可能需要 microbatch，限制 max wait 和 backlog。

Latency budget 示例 local5µs 不包括 venue；可拆 ingress/lookup/risk reserve/order encode/queue/commit 等独立阶段。如果各 stage p99 分别1µs，不能直接相加得到端到端 p99；随机变量分位数一般不可加。可用 union bound 规划阶段超时概率，并直接测 end-to-end。Critical path 中并行依赖取 max、串行才相加。

### 12.12 Placement quantitative exercise

Logical processors 的 CPU profile、working set、traffic graph：

| Processor | Useful CPU demand |
|---|---:|
| Position A | .35 core |
| Position B | .20 core |
| Valuation A | .25 core |
| Valuation B | .35 core |
| Aggregation A | .25 core |
| Distribution | .20 core |

Total1.6 core。可尝试4 threads：T0 Position A + Valuation A=.60，T1 Position B + Valuation B=.55，T2 Aggregation=.25，T3 Distribution=.20。若目标≤60%，该 mapping 名义可行；不含 polling/cache/queue 等，T0 没有额外余量。另一种 mapping 按 stage 隔离会多 handoff，但 workload burst isolation 更好。优化目标不仅平衡 CPU，还要降低关键边 handoff、cross-NUMA、cache pollution。

Downstream-priority 需要 per-stage bounded execution；高 stage 与低 stage 共用 FIFO 时，local demux 或 sparse lane。32 threads all-to-all 992 queues，每个4096×64B→248MiB。3 lanes→744MiB；每 consumer 轮询31或93 queues，idle overhead 可能巨大。实际按 DAG edge 配置，评估 active hint 自身的 contention。

### 12.13 Benchmark harness（概念实现）

~~~cpp
// Open-loop workload: intended arrival independent of completion.
for (const auto& e : trace) {
    waitUntil(e.scheduledTime);
    if (!tryOffer(e))
        recordAdmissionFailure(e.id, e.scheduledTime);
}
// At consumer:
void onComplete(const Event& e, TimePoint done) {
    histogram.record(done - e.scheduledTime);
}
~~~

实际 harness 需使用 calibrated monotonic clock、独立 generator/consumer core、避免 generator saturation；若 tryOffer 失败，必须记录 drop/reject/timeout 及 offered vs accepted throughput，不能只统计完成请求。排队包含 scheduled arrival 到实际发出的等待；跨机时钟需 PTP/误差校验。Closed-loop 也有用途（固定并发用户），但不能用它替代独立 MD/Trade arrivals。

Histogram：每 worker 本地收集，兼容 bucket configuration 后 merge；p99 不做 arithmetic average。Warm-up/cache state、steady duration、sample count、CPU frequency/SMT/NUMA、compiler flags、message size、distribution、offered load 都要记录。对于 p99.99 等稀有尾部，样本过少时置信度有限。重复多轮并报告波动，不只选择最快一次。

### 12.14 Diagnostic workflow（面试重点）

1. Reproduce regression with same workload and output semantics.
2. Locate queue age rise / stage service-time rise / stale input.
3. Compare per-shard useful utilization, hot keys, fan-out, downstream offer failures.
4. On-CPU profile: cycles/instructions, IPC, top-down, branch/cache/TLB, cache-line bouncing, memory bandwidth.
5. Off-CPU: scheduler, futex/locks, I/O/RPC, page fault, dirty throttling, interrupt.
6. Verify memory placement (first-touch), affinity/IRQ, SMT, frequency changes.
7. Change one factor, repeat open-loop load sweep, check correctness and tails.
8. Test burst, slow consumer, snapshot/replay, failover and multi-hour soak.

不能仅依据低 IPC 判定 DRAM bottleneck；可能是 frontend、branch miss、serialization、dependent cache loads。低带宽利用率也可能 latency-bound。Roofline 是带宽/算力 ceiling，不解释所有 random-access latency。RFO/cache coherence traffic 与 DRAM bytes 区分。

### 12.15 Interview answer and practice

**English answer:** I would model workload amplification and service demand at each stage, estimate CPU, memory bandwidth and communication requirements, and then validate the assumptions against realistic open-loop benchmarks. I would measure per-shard queue age and useful work rather than rely on average CPU utilization. I would also treat tail latency, risk freshness and failover headroom as separate constraints, and prioritize reducing work and cross-thread communication before micro-optimizing instructions.

Practice: 给定1M MD/s、50K fills/s、每 tick 4 valuations、每 valuation 5 aggregation updates，推导每 stage rate/core demand。若 hot shard 单独过载，说明为何 24 cores 平均空闲也无法解决。设计 A/B benchmark 比较 dedicated、mixed 和 compact logical placement，在相同 SLA 与正确性条件下选最佳 deployment。

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

### 13.12 深入：明确 Process API、事件协议与所有权

| Interface | Example request/event | Completion contract |
|---|---|---|
| Strategy→OMS | SubmitOrder(requestId, terms) | accepted/rejected/unknown，与 venue ACK 区分 |
| Gateway→Risk Authority | Refill(grantId, amount, epoch) | committed exclusive grant |
| Authority→Gateway | Revoke(grantId, effective boundary) | old rights actually disabled + ACK |
| Venue→Capture | ExecutionReport/DropCopy | canonical execution persisted/recoverable |
| Capture→Calculation | TradeChange(before,after,revision) | idempotent applied offset |
| Market Feed→Valuation | MarketUpdate(factor,version) | required market progress |
| Calculation→Distribution | RiskState(key,context,version) | latest accepted publication; not authorization |
| Distribution→Subscriber | Snapshot + live suffix | ready at agreed cut |
| Control→Order Entry | DisableTrading(epoch,scope) | enforcement at send boundary |

每个事件定义 identity、schema、ordering scope、owner epoch、event/effective timestamp、retry policy、durability、backpressure。只为必要的 domain 建 sequencer；独立 calculation stage 不需全局序列化。不要把所有 IPC 都设计成 request-response：state subscription、authoritative command、bulk snapshot、derived event stream 具有不同语义。

### 13.13 Detailed thread topology & logical execution

~~~mermaid
flowchart TD
    I[Ingress owners] --> P0[Position shard 0]
    I --> P1[Position shard 1]
    P0 --> V0[Valuation shard 0]
    P1 --> V0
    P0 --> V1[Valuation shard 1]
    P1 --> V1
    V0 --> A0[Book / strategy aggregation]
    V1 --> A0
    V0 --> A1[Factor / portfolio aggregation]
    V1 --> A1
    I --> INV[Inventory / settlement owner]
    A0 --> D[Distribution owner]
    A1 --> D
    INV --> D
~~~

Logical DAG 不改变，physical mapping 可配置：
- Dedicated：Position/Valuation/Aggregation 各用独立 cores，隔离好，但存在容量碎片和 handoff。
- Mixed：高通信 edges 共置，local ready queue，减少跨线程消息；需要 bounded scheduling 避免长 valuation 阻塞 position。
- Compact：多个轻量 logical shards 共享 worker，适合低负载或开发环境；生产要验证 burst/failure tail。
- Compute pool：昂贵纯 valuation 使用 immutable tasks，不偷 mutable ownership。

配置项：processor ID、stage/shard、ordering domain、owner epoch、thread/NUMA affinity、queue capacity、poll budget、priority、deadline、backpressure policy。启动时验证每个 mutable key 只有一个 owner、所有必需 routing edge 存在、DAG 无非法依赖环、queue memory 合理。在线改 mapping 用 R3 migration protocol。

### 13.14 Full order sequence: local authorization + durable intent

~~~mermaid
sequenceDiagram
    participant S as Strategy
    participant G as Gateway / local risk
    participant J as Order ledger
    participant E as Exchange
    participant C as Trade capture
    participant R as Risk calculation
    S->>G: Order request ID
    G->>G: Check + reserve all applicable local rights
    G->>J: Record reservation + order intent
    J-->>G: Required commit acknowledgment
    G->>E: Send stable client order ID
    E-->>G: Execution report
    E-->>C: Drop copy
    G->>G: Convert local reservation on unique fill
    G->>C: Order-entry execution
    C->>C: Canonical dedup + revision + journal
    C->>R: Canonical trade change
    R-->>G: Versioned risk state + applied progress
    Note over G: Reconcile without double-counting local fill
~~~

这是 durable-before-send 的一种选择，不代表所有系统必须采用此延迟路径。若用其他可靠 order recovery contract，应明确 crash windows 与风险覆盖，不能把本地 reserve-only 当作完整 durability。若 intent commit 失败且未发送，可按已确认状态释放；若 send 结果不确定，保留 reserve，恢复外部订单状态。Local pre-trade check 的5µs 与 order end-to-end latency 分开。

同时消耗 credit/locate/position limits 时，优先共置可用独占 budget，在同一 owner 两阶段 validate/install。若涉及跨 domain 非可拆分约束，使用 centralized transaction/保守 reservation；不允许只通过不同服务的 read query 就认为满足原子性。

### 13.15 Recovery runbook 与 degraded mode

| Component unavailable | Continue? | Required guard |
|---|---|---|
| Dashboard | usually yes | isolate UI |
| Advanced VaR/PFE | policy-dependent | stale analytics alert, hard constraints unaffected only if valid |
| Market data feed | restricted | source freshness, alternative feed, no unsafe quote |
| Central credit authority | limited | only valid already-granted rights with revocation policy |
| Inventory broker | limited | only valid confirmed grants |
| Trade capture | depends | reliable raw source buffering/replay, stop if loss/incomplete exposure unsafe |
| Journal disk full | no false acceptance | backpressure / fail-safe according to durability contract |
| Order gateway | failover | fence old session, restore unknown orders |

Start recovery: disable unsafe new actions; identify last committed log/cut; recover durable state; obtain current ownership epoch; reconcile venue orders/executions; reconcile grant/consumption and external loan/settlement; restore subscriptions; check source/applied progress; enable only required scopes. 不要求所有 dashboard 已 ready 才恢复交易，但必须满足授权、订单、数据新鲜度的真实 prerequisite。

### 13.16 Business invariants and acceptance criteria

| Invariant | Test oracle |
|---|---|
| One execution produces one economic effect | canonical ledger vs independent trade list |
| Position quantity / cashflow conservation | sum signed canonical contributions |
| Parent allocation equals children + unallocated | per trade lineage |
| Fixed budget is not over-issued | used+reserved+exclusive available+central+in-transit |
| No order sent without applicable authorization | order intent / reserve identity |
| Confirmed cancel releases only unused portion | execution/cancel ledger |
| Old owner cannot perform new side effect | epoch/session fence test |
| Snapshot corresponds to input cut | rebuild vs snapshot+replay |
| Nonlinear aggregation uses correct boundary | independent full model |
| Stale state cannot masquerade as current | version/progress/health assertions |

Acceptance benchmark：给出 workload trace、offered/achieved rate、burst duration、key skew、model/position count、per stage p99/p99.9、end-to-end freshness、queue age、resource consumption、failover time、reconciliation result。只有达到业务 invariant 与 latency SLA 的 throughput 才算有效容量。

### 13.17 System design alternatives & decision record

| Choice | Recommended when | Reconsider when |
|---|---|---|
| Local escrow | additive bounded rights, low latency | fragmentation excessive or non-additive |
| Dedicated risk owner | shared small resource within host | cross-thread RTT dominates |
| Replicated authority | critical grant/limit ledger | derived compute can be rebuilt |
| Async risk calculation | event-driven derived state | hard authorization depends on latest exact result |
| Per-key ordering | independent state | cross-key observable transaction |
| Global ordered commit | genuinely required global semantics | HOL blocking unnecessary |
| Latest-state conflation | only final state matters | breach/audit/path history required |
| Logical processor multiplexing | many lightly loaded dimensions | strict isolation, cache pollution, long tasks |
| GPU compute | large dense batch / MC | small microsecond task, transfer dominates |

上述推荐是设计起点，不是固定结论。具体 selection 由 workload、failure model、SLA、金融模型与实际 benchmark 决定。对任何性能优化，应说明它改变了什么资源瓶颈、引入什么新 failure mode、如何验证。

### 13.18 综合面试模拟：逐层追问

**Interviewer:** Why not simply use a shared atomic risk table?
- 单字段 atomic 无法保证 multi-field/business transaction；global cache-line contention；不可替代 durable reservation。先分 ownership，能拆分的资源用 local budget。

**Interviewer:** Why not use one global sequencer?
- 独立 domain 不需要 total order；全局排序限制 throughput，可能制造 HOL blocking。对真正共享的资源仍需统一决定顺序或 atomic commit。

**Interviewer:** What if your local budget is insufficient but the firm has capacity?
- Async refill、central reserve、动态 rebalancing、slow path；不能重复授予额度。衡量 false rejects 和 fragmentation。

**Interviewer:** What if a trade correction and a market update race?
- Additive economic changes、明确 accounting order，valuation result 携带 position/market/model context；版本不可比较时不使用 LWW，重新计算或明确 stale semantics。

**Interviewer:** Can you guarantee zero loss, instant revocation and microsecond end-to-end authorization during a partition?
- 不能在不限定 failure model 的情况下同时承诺。区分 local check 与 durable commit、有效预授予 rights 与 global revocation；要求业务明确 safety/availability/latency 优先级。

**Interviewer:** How would you demonstrate your configurable logical processor design is better?
- 固定业务语义与输入，benchmark dedicated/mixed/compact；比较 CPU useful demand、cross-thread hops、NUMA traffic、queue age、p99.9、sustainable throughput 和 failover headroom。不能只凭少用 cores 声称性能更好。

### 13.19 最终交付物（面试准备与实战）

- Architecture diagram + state ownership matrix。
- Order/trade/control path sequence diagrams。
- Data contracts + version/identity/ACK semantics。
- Core C++ state machine 与独立 reference implementation。
- Property tests、fault injection、replay/dedup/reconciliation。
- Capacity spreadsheet/model（normal/peak/burst/failover）。
- Open-loop benchmark 与 deployment A/B 测试。
- Decision record：tradeoff、assumptions、limitations。

这套设计的核心是 business invariants → state ownership → ordering/transaction → mathematical reduction → logical DAG → physical topology → recovery/testing。低延迟技术是最后几层的工具，而不是业务正确性的替代。

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

**范围声明：** 本文整合 R1–R13 的核心知识和工程讨论，并在扩展版补充状态转换、数据模型、协议推演、数学推导、故障矩阵、面试回答与实验清单；交互模拟器转换为公式、静态案例和 Mermaid 图。不是逐字聊天备份。示例未实际运行；需用生产协议、金融模型、硬件与负载验证设计。
