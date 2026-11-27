# Logical-Processor-Based, Deployment-Configurable Stage-Multiplexed Pipeline

> 一种面向低延迟 C++ Trading / Real-Time Risk 的可组合执行架构：业务 Stage、状态 Shard、逻辑执行单元、物理线程相互解耦。
>
> Status: Architecture Proposal / Design Notes  
> Updated: 2026-11-27  
> Related concepts: Processor / AsyncRunTask / parallelIndex / partitioned SPSC / Run-to-Completion (RTC)

## 1. 核心结论

**Stage 是业务计算阶段，Shard 是状态所有权边界，Logical Processor（LP）是可调度执行单元，Physical Executor 是实际运行线程。四者不能绑定为同一个概念。**

建议：

1. 使用多个**逻辑上独立的小 Pipeline** 表达处理模块，不要求它们各自独占物理线程。
2. 每个 LP 可以运行来自**一个或多个 Pipeline 的多个 Stage / AsyncRunTask partition**，即 Stage Multiplexing。
3. 使用 DeploymentPlan 将 LP 映射到固定的 Physical Executors（可设置 affinity / NUMA），支持 Dedicated / Shared / Hybrid 部署。
4. 同一 State Shard 始终有明确的 Single Writer / Sequenced Owner；多个执行线程支持相同代码，不代表可并发修改同一状态。
5. 同线程可考虑 Local RTC；跨线程采用有明确生产者和消费者的 SPSC 通信；是否内联执行取决于排序与依赖约束。
6. 在**同一业务优先级、已就绪且不违反状态顺序**的任务间，采用 Downstream-First + Bounded Fairness。
7. 区分 Authoritative Trading State（不能随意丢事件）与 Derived Risk State（可按语义增量计算 / Conflate）。

目标不只是最低 p50 latency，而是较低的 p99.9、可持续吞吐量、合理的 CPU 利用率、易审计的一致性以及部署可控性。

## 2. 为什么不简单选择一个大 Pipeline 或多个固定线程的小 Pipeline？

| 设计 | 优势 | 典型缺陷 |
| --- | --- | --- |
| Single Large Stage-Multiplexed Pool | 所有 Stage 共享执行资源；跨维度 Repartition 更灵活 | Worker 间 Queue / Polling / NUMA 范围可能很大 |
| Multiple Small Pipelines, Dedicated Threads | 较好的局部性、隔离和简单调度 | 一组线程可能空闲而另一组拥塞；不同 Risk 维度跨组流动困难 |
| **Logical Pipelines + Configurable LP Deployment** | 业务 Pipeline 不变，按部署配置共享、融合或隔离执行资源 | 部署规划、排序保证和队列拓扑更复杂 |

关键观察：**多维度 Risk 需要不同的 Sharding Key，但维度数量不应该决定 OS Thread / Physical Pipeline 数量。**

## 3. 五个主要抽象

### 3.1 Stage：逻辑计算步骤

例如：

- Position / Execution Lifecycle：按 (Account, Instrument) 或 OrderID 分区；
- Market Data State：按 Instrument 分区；
- Exposure / Greeks：按 RiskFactor 或 FactorGroup 分区；
- Portfolio Aggregation：按 Account / Book 分区；
- Trading Decision / Hard Risk Reservation：按 Limit Scope / Credit Owner 分区。

一个 Stage 不必对应单独的 Thread Pool；不同 Stage 可以放在一个 LP 内执行，也可以分离。

### 3.2 Logical Shard：有状态实体的顺序边界

映射：

~~~text
(stage, business_key) -> logical_shard
~~~

一个逻辑 Shard 有明确的 State Owner，满足其状态机的串行执行与确定的业务排序要求。Virtual Shard 可以作为将 Business Key 与 LP 分离的中间层。

**注意**：某个 Key 的严格业务序列并不会因为所有事件最终送给同一个单写者而自动成立；多个输入源仍可能乱序，需要上游 Sequencer、因果依赖处理或版本协议。

### 3.3 AsyncRunTask：可复用的任务 / 状态模块

与此前的 TPSLite 风格保持一致：

- push(parallelIndex, event)：将事件送到某个分区的入口；
- run(parallelIndex, budget)：在该分区上执行有界工作；
- parallelIndex：代表独立 lane / state partition，而**不是线程编号**。

这是接口设计方向；并不要求实际运行时必须使用虚函数。可以用静态注册、函数指针表、模板或 CRTP 降低间接调用成本。

**SPSC 前置条件**：每一条物理 SPSC lane 必须恰有一个生产线程和一个消费线程。如果 push(parallelIndex, ...) 可能由多个线程调用，必须改为 per-producer SPSC lanes、Ingress Aggregator 或真正的 MPSC，而不能仅凭同一个 parallelIndex 假定为 SPSC。

### 3.4 Logical Processor（LP）：可组合执行单元

Logical Processor 是**软件调度实体，不是硬件 SMT Logical CPU**。

- 一个 LP 可以注册多个 AsyncRunTask partition / Stage。
- 一个 LP 的执行可以按 Stage Priority、Deadline / Age、Budget 调度。
- 一个 LP 可以被配置到某个 Physical Executor。
- LP 可承载多个逻辑 Pipeline 的工作；小 Pipeline 是业务组织方式，而不是线程所有权边界。

### 3.5 Physical Executor：实际 OS Thread

- 运行一个或多个 LP；
- 执行 spin / spin-then-park / blocking idle policy；
- 可以固定 CPU affinity，按 NUMA 规划；
- 负责执行器级 polling、预算、统计；
- 允许 Dedicated、Shared、Hybrid 部署。

**DeploymentPlan** 定义 LP -> Physical Executor -> CPU / NUMA 的映射。它不改变业务 Key 的 Shard 规则。

## 4. 三层映射

~~~text
Business Stage + Key
       |
       v
Logical Shard (state owner / sequencing boundary)
       |
       v
Logical Processor (execution / multiplexing / scheduling)
       |
       v
Physical Executor (OS thread)
       |
       v
Physical CPU / NUMA topology
~~~

逻辑所有权关系：

~~~text
(stage, key) -> shard_id
(stage, shard_id) -> lp_id
(lp_id) -> executor_id
(executor_id) -> cpu_affinity
~~~

其中 (stage, key) 可以使用不同策略：Account、Account+Instrument、RiskFactor、Book、Strategy、LimitScope。不要将所有 Risk Stage 强制绑定到单一顶层 Key。

## 5. 例子：一个 FX Real-Time Risk 系统

~~~text
Input: Fill / Bust / Cancel / Market Data
                |
                +--> [Trading State Pipeline]
                |      Order / Position / Reservation
                |      key = OrderID or Account+Instrument
                |
                +--> [Market State Pipeline]
                |      Price / Yield / Vol / Parameters
                |      key = Instrument / Factor
                |
                v
       [Incremental Exposure Pipeline]
           key = RiskFactor / FactorGroup
                |
                v
       [Portfolio Aggregation Pipeline]
           key = Account / Book
                |
                v
         Risk Snapshot / Signals

Separate hard authorization path:
Order -> Authoritative Limit Owner / Allocated Credit
      -> Check + Reserve -> Permit / Reject
~~~

重要事项：

- **Market Data 更新**不应强制重放每一笔持仓；对于线性 FX Currency Exposure，可先维护净货币头寸 / Factor Aggregates，再对最新价格求值。
- **Fill / Bust / Correction / Reservation**是权威交易状态转换，不应通过简单 Latest-Value Conflation 丢弃。
- 对于派生风险计算，可采用 Dirty Flag、Delta Aggregation、Versioned Snapshot、按需重新计算。
- RiskFactor -> Portfolio 经常是 **Fan-out / Fan-in**，而不是一个事件严格沿 Stage1 -> Stage2 -> Stage3 一进一出的固定链。
- **异步 Risk Snapshot 不等于 Hard Pre-Trade Authorization**：Account / Desk / Firm 的硬限额仍需要单一权威额度所有者、可靠分配的 Credit，或等价的一致性机制。
- 市场变动本身可能使敞口超限，系统能保证的是合理检测、策略响应及预交易授权语义，而不是市场敞口永不越界。

## 6. 可配置物理部署

假设 8 个 LP（来源可跨不同逻辑 Pipeline）：

| Deployment | Physical Executors | 示例分配 | 用途 |
| --- | ---: | --- | --- |
| Dedicated | 8 | E0:LP0 ... E7:LP7 | 关键路径隔离、基准测试 |
| Shared | 2 | E0:LP0-3, E1:LP4-7 | 低资源开销、轻负载 |
| Hybrid | 4 | E0:LP0; E1:LP1; E2:LP2-4; E3:LP5-7 | 热路径独占，较轻任务共享 |
| NUMA-aware | 按拓扑 | LP 尽量与其 State / Producer 同 NUMA | 降低 remote memory 和 handoff |

上述是静态拓扑示例，不表示负载自动平衡。

部署优化遵循：

1. State Ownership / Correctness 不变；
2. 先共置强依赖、低计算成本的 Stage；
3. 对重计算 / 长尾 / 阻塞阶段考虑拆分；
4. 用真实 CPU 工作量和事件分布配置 LP -> Executor；
5. 需要跨 NUMA 通信时，优先传紧凑 Delta 而非复制大状态；
6. 第一版支持**停机 / 重启生效**的部署切换；Hot Migration 需要额外协议。

## 7. Stage-Multiplexed Execution

一个 Physical Executor 可依次调度其全部 LP，一个 LP 可管理多个 Stage 的有界工作。

~~~cpp
struct RunSlot {
    void* context;
    bool (*run)(void* context, uint32_t partition, uint32_t budget);
    uint32_t partition;
    uint8_t stage;
};

struct LogicalProcessor {
    std::vector<RunSlot> slots;
    // Local mailboxes / schedulers / priorities / metrics
};

void executorLoop(PhysicalExecutor& executor) {
    while (executor.running()) {
        bool work = false;

        // scheduleReady() must respect dependencies, per-shard order,
        // risk criticality, downstream preference and fairness.
        for (auto& lp : executor.logicalProcessors()) {
            work |= lp.scheduleReady(/* bounded budget */);
        }

        if (!work) {
            executor.idleStrategy().idle();
        }
    }
}
~~~

这是接口级伪代码，不是可直接编译的生产实现。

热点路径如果任务集合固定，可用编译期任务注册、静态函数指针、模板方式，避免多层虚调用。但首先应 Benchmark 真正的队列、cache 和 scheduling overhead。

## 8. Pipeline 间路由和通信

### 8.1 路由决策

~~~text
next_stage, next_key
    -> logical_shard(next_stage, next_key)
    -> logical_processor
    -> physical_executor
~~~

- **Same LP + ordering safe**：可在 Execution Budget 内继续 RTC；
- **Different LP, same executor**：优先 local mailbox；仅在不会越过旧事件 / 破坏依赖时内联；
- **Different executor**：通过可靠的跨线程消息通道；
- **Multiple outputs**：由 Fan-out Router 发往多个明确 Owner；
- **Fan-in**：下游必须明确是可结合 Delta、版本合并，还是需等待齐备的依赖。

### 8.2 All-to-All SPSC Matrix

固定 E 个 Physical Executors，如果两两都需要单向通道：

~~~text
directed_spsc_count = E * (E - 1)
~~~

- E = 4：12 条跨执行器 SPSC；
- E = 16：240 条跨执行器 SPSC；
- 不是所有 Executor Pair 都通信时，只建立需要的通道即可。

这只统计**执行器间的有向通道**。多个 LP 共用同一个物理线程时，可以在该线程上串行向目标执行器的 SPSC 生产；但所有其他发送线程必须走各自通道。

**不要**将多个不同生产线程直接写入同一条 SPSC。

对于 NUMA / 大规模拓扑，可以测试 sparse SPSC、每消费者 MPSC ingress、分层路由，但这些是不同设计，不能无基准地假设某个一定更快。

### 8.3 Local RTC 与隐含的重排序风险

~~~text
LP-A -> LP-B (same Physical Executor)
~~~

不等于可以无条件直接调用 LP-B：

- LP-B 可能已有更早的同 Shard Event 在 Mailbox；
- LP-B 可能正在等待其他上游 Source Version；
- 较大的 Stage Number 不能优先于该实体前序状态转换。

因此默认做法是**追加到目标本地 Mailbox**，只有在 Queue Empty、当前 Owner 有执行权限、依赖已就绪、且排序安全时才做 Inline RTC。

## 9. Downstream-First Scheduling

在可执行任务中，优先处理 Stage Number 较大的 Stage，有利于较快完成已进入流水线的工作，降低 WIP（Work in Progress）；但不保证最高吞吐或最低平均时延。

优先级原则：

~~~text
1. Correctness / State Ownership / Dependency Readiness
2. Business Criticality (e.g. hard order authorization)
3. Stage Number: downstream first
4. Bounded Fairness / Age promotion / Work Budget
~~~

建议在多个 Producer Queue 之间使用 bounded polling / round-robin，并对同一优先级队列采用公平性策略。

例子（仅用于试验）：

| Stage | Scheduling Budget per Turn |
| --- | ---: |
| Portfolio (3) | 32 |
| Exposure (2) | 16 |
| Position (1) | 8 |

数字须由真实输入速率、阶段耗时与 p99.9 测试决定。

**严格 Downstream-First 的风险**：下游持续有任务时，上游可能饥饿。Age-Based Promotion、最大连续 Batch 和最小服务份额可以缓解，但需要在可证明的状态顺序范围内实施。

## 10. Spin Loop、Idle Strategy 与 Backpressure

LP 不应独占一个 Spin Thread；**Physical Executor 才拥有 Loop / Idle Strategy**。

| 模式 | 特征 | 典型用途 |
| --- | --- | --- |
| Busy Spin + PAUSE | 低唤醒延迟，高 idle CPU | 专用交易 hot path |
| Spin-then-Park | 平衡 burst latency 与空闲资源 | Real-Time Risk Worker |
| Blocking / Event Wakeup | 低 idle CPU，更多唤醒开销 | 慢速后台计算与控制任务 |

各 Logical Processor 的 run() 应为**有界 work**，防止同一执行器上的其他 LP 饥饿。

当跨线程 Ring 满时：

- Fill / Order State / Reservation / Bust 等：不得静默丢失；需要有界重试、停流、背压、故障升级或明确的 fail-closed 策略；
- 可覆盖的最新行情 / 派生 Risk Snapshot：可考虑 latest-value slot + scheduled-once + dirty/version；
- 调度时的 Dirty 状态必须使用不会丢失计算期间新增更新的协议；
- 优先考虑消费者速率、Queue Age 和快照新鲜度，而非单纯增大 Queue Capacity。

## 11. Risk Correctness Invariants

### I1. Single Writer / Sequenced State

同一权威 State Shard 在任意时刻有且只有一个有效 Owner；跨 Producer 的顺序另外定义。

### I2. Check + Reserve Atomicity

对同一硬限额的授权不能依赖多个异步、互不协调的局部读取。

简单单方向额度示意：

~~~text
committed_exposure + reserved_new_order <= hard_limit
~~~

真实业务需要支持双向头寸、减风险订单、部分成交、取消、重试与不同 Risk Metric。

### I3. Idempotence / Recovery

Fill / Bust / Correction / Cancel、额度预留释放都需要 Event ID / Order ID / Source Sequence 等，配合可恢复 Ledger / Checkpoint。重放不会重复预留或反复冲销。

### I4. Consistent Derived Snapshot

Snapshot 至少清楚标识 Position Version 与 Market Data Version；如果要求全局统一截面，应采用明确的 Epoch / Barrier / Watermark 或等效机制。仅给每个 Node 加 version **不自动**保证全局一致。

### I5. Routing Epoch During Reconfiguration

迁移 LP / Shard 时必须确保旧 Owner 停止写入、在途消息处理边界确定、新 Owner 恢复状态后接管，并阻止旧路由继续生产有效写入；需要 Routing Epoch / Fencing / Sequencing 方案。

### I6. Lock-Free Is Not Wait-Free, and Not a Business Transaction

SPSC Queue 只解决通信形式，不代替业务级原子性、持久化、业务顺序和回放机制。

## 12. 与旧 Node Graph Pricing 的联系

可以重用：

- Logical Dependency Graph；
- Dirty / Delta Propagation；
- Shared Upstream State；
- Versioned / Coalesced Derived Computation。

不应机械重用：

- 每个微小 Node 都创建独立可调度 Task；
- 每级 Node 强制跨线程 handoff；
- 每周期全局 Barrier；
- 将 Fill / Bust / Reservation 当作可覆盖的 Dirty Update。

更合适的方式：

~~~text
Logical Risk DAG
   -> Dependency / Incremental Planner
   -> Coarse-grained Stage / Partition Tasks
   -> Logical Processors
   -> Configurable Physical Executors
~~~

对于轻量计算优先考虑 Node Fusion、局部 RTC 和 State Locality；对于大型独立计算任务再考虑真正并行 DAG Execution。

## 13. 吞吐量与尾延迟的边界

固定 E 个 Executor、每个事件有效 CPU 工作 W 秒时，忽略开销的总吞吐计算上界近似：

~~~text
throughput <= E / W
~~~

但真实瓶颈还包括：

- 最大 Hot Shard（串行 State）服务能力；
- 跨线程 handoff 数量与队列等待；
- Queue Scan / Empty Polling；
- cache line bouncing、LLC misses、NUMA remote access；
- Fan-out 扩大计算量与 Fan-in 同步；
- 高负载下排队导致的尾延迟恶化。

不能仅用平均 CPU Utilization 判断是否健康；需要监控每个 LP、每个 Executor、每条 Channel 的负载和最大滞后。

## 14. 建议 Benchmark Matrix

固定业务正确性、硬件、Core 数、负载分布与总输入序列，比较：

1. Single-thread RTC baseline；
2. 4 / 8 / 16 个 Dedicated Executors；
3. Shared：多 LP -> 少 Executor；
4. Hybrid：Hot LP dedicated、Cold LP shared；
5. Stage-Dedicated Thread Pools；
6. Large Shared Worker Pool / Global MPMC baseline；
7. All-to-All SPSC vs Sparse SPSC vs MPSC Ingress；
8. Local RTC on/off、Downstream-First on/off、不同 Batch Budget。

**三组负载**：

- 高频 Market Data、轻量派生 Exposure，可 Conflate；
- 高频 Fill / Bust / Order Lifecycle，严格不可丢业务事件；
- 多 Factor / Portfolio 聚合、偏斜账户与单个 Hot Key。

**主要指标**：

| 类别 | 指标 |
| --- | --- |
| End-to-end | p50 / p99 / p99.9 / p99.99, max latency, snapshot age |
| Capacity | sustainable events/s under latency SLO, throughput/core |
| Scheduling | LP wait time, stage wait time, queue depth/age, handoffs/event |
| CPU | cycles/event, utilization per executor, empty polls, LLC miss, remote NUMA |
| Correctness | duplicates, gaps, dropped events, invariant violations, recovery digest |

重要测试条件：Open-loop Input（避免闭环负载自动降低到可处理速率）、稳定状态与 burst、balanced 与 skewed Workload，使用真实队列填满时的背压规则。

## 15. 落地顺序

### Phase 1 — Static Configurable Runtime

- 定义 Stage / Shard / LP / Executor；
- 保留 parallelIndex 与单写者；
- 支持重启生效的 Dedicated / Shared / Hybrid Mapping；
- 先实现 local mailbox 与明确的跨线程 SPSC；
- 建立 trace、统计和原子状态约束测试。

### Phase 2 — Multi-Dimensional Risk

- Position -> Exposure -> Portfolio，不同 Sharding Key；
- 支持 Fan-out / Fan-in、Delta Propagation；
- 区分交易事件状态机与异步派生 Risk；
- 引入 Versioned Risk Snapshot。

### Phase 3 — Advanced Scheduler

- Downstream-First + bounded fairness；
- stage / LP 的动态预算；
- optional latest-state coalescing；
- adaptive idle / queue scanning；
- 对比 RTC、局部 Fusion 与跨线程拆分。

### Phase 4 — Optional Dynamic Rebalance

只有在业务确实需要时，再设计 live Shard Migration、Routing Epoch / Fencing / State Transfer。不要将动态迁移视为静态可配置部署的必备条件。

## 16. 结论与取舍

**已建立的原则（高置信度）：**

- 逻辑 Pipeline、Sharding Key 和物理线程数量应该解耦；
- State Ownership 不等于 Worker 能够执行哪些 Stage；
- SPSC 有严格的生产者 / 消费者前提，不能代替业务原子性；
- 下游优先只能在就绪、正确排序、公平性约束下应用；
- Authoritative Trading Events 与 Derived Risk Computation 需要不同语义。

**有待 Benchmark 的实现选择（中等置信度）：**

- 一个大型 Execution Pool 与多个共享小 LP Group 谁的 p99.9 更好；
- All-to-All SPSC 是否比 MPSC Inbound / Sparse SPSC 快；
- 多大程度的 Node Fusion、Stage Multiplexing 能提升吞吐；
- Spin、Batch Size、Downstream-First 权重和 NUMA 部署的最佳参数。

最简描述：

> **A deployment-configurable, stage-multiplexed execution runtime that maps sharded logical processors onto physical executors, with local run-to-completion, explicit cross-thread message passing, and downstream-aware bounded scheduling.**
>
> 将它用于 Real-Time Risk 时，必须另行维持 Trade / Position / Reservation 的权威状态和硬限额一致性。

## 17. 术语速记

| Term | 含义 |
| --- | --- |
| Logical Pipeline | 一组相关的业务处理流程，不意味着独占物理线程 |
| Stage Multiplexing | 同一 LP / Executor 可执行多个不同 Stage |
| Logical Shard | 按业务 Key 分区的状态及顺序边界 |
| parallelIndex | AsyncRunTask 内的分区 / lane 标识 |
| Logical Processor | 可组合的软件执行 / 调度实体 |
| Physical Executor | 实际 OS Thread，控制 Polling / Affinity |
| DeploymentPlan | LP -> Executor -> CPU 映射 |
| Local RTC | 合法且有预算地在本地继续下一阶段 |
| Downstream-First | 在已就绪任务中优先接近终点的 Stage |
| Cross-Shard Repartition | 下一阶段根据新 Sharding Key 更换 State Owner |
| Hard Risk Authority | 有权执行限额检查与原子额度预留的实体 |
