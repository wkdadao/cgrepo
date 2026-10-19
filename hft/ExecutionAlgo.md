# Execution Algo：常见算法、Passive/Aggressive 与 SOR/Execution Engine

> 适用场景：Equities / Futures / FX 电子交易；Execution Algo、SOR、OMS、低延迟 C++ 系统设计与技术面试。
>
> 本文是概念及设计讨论，**不是某个现有系统已经实现的功能清单**。不同 Asset Class、Venue、Broker 对 Algo 名称和具体执行细节可能不同；订单语义以具体交易场所规则、接口和业务约定为准。

## 1. 首先区分三个层次

| 层次 | 典型例子 | 核心问题 |
| --- | --- | --- |
| Order Type / Instruction | Market、Limit、Stop、Stop Limit、IOC、FOK、Post Only、Pegged | 订单在什么条件下可成交或失效？ |
| Execution Algo | TWAP、VWAP、POV、Implementation Shortfall、Liquidity Seeking | **何时**交易、**交易多少**、有多积极？ |
| Smart Order Routing（SOR） | Venue selection、split、sweep、dark/lit routing | **在哪里**成交、使用哪个对手方/流动性、怎样路由？ |

- Stop、Limit 是常见订单类型或指令，严格说不等于 VWAP/TWAP 这类执行算法。
- Iceberg、Pegged、Trailing Stop、Bracket 有时是交易所原生订单类型，有时由 Broker/Algo Engine 合成实现。
- Passive/Aggressive 是**执行行为/积极程度**，不是互斥的 Order Type。
- SOR 不仅仅选择 Venue，也可能做拆单、盘口扫描、价格保护及动态路由。
- Algo 与 SOR 的实际架构边界并不固定；有些系统会把 Liquidity Seeking、Order Placement 放在一个策略里。

概念链路：

~~~text
Client Parent Order (size, side, limit, deadline, benchmark)
     |
     v
Execution Algo / Scheduler
  - target trajectory / urgency
  - passive vs aggressive
  - quantity, price limits, child order decisions
     |
     v
SOR / Venue Selector
  - venue availability / displayed & hidden liquidity
  - price, fee, latency, fill probability, last-look policy
     |
     v
Child Order Execution Engine / OMS
  - new / replace / cancel / timeout
  - acknowledgements, fills, recovery, reconciliation
     |
     v
Exchange / ECN / Broker / LP
~~~

这个划分是**设计思路**，不是必须遵循的组件边界。

## 2. 常见 Order Type / Instruction

| 名称 | 主要语义 | 关键风险/注意点 |
| --- | --- | --- |
| Market | 尽可能立即执行，不提供显式价格上限/下限 | 滑点；流动性不足；场所保护机制 |
| Limit | 仅在限价或更优价格执行 | 可能不成交；可 passive，也可 aggressive |
| Stop | 触发条件满足后提交指定类型订单（常见为 Market） | Stop Price **不是成交价保证** |
| Stop Limit | 触发后提交 Limit | 避免超出限价执行，但可能不成交 |
| Trailing Stop | Stop Trigger 随有利价格方向调整 | Gap、触发价格来源、更新时序 |
| IOC | Immediate-Or-Cancel：立即成交可成交部分，取消剩余部分 | Partial Fill；是否与 Venue 具体规则兼容 |
| FOK | Fill-Or-Kill：须立即全量成交，否则不执行 | 流动性不够时完全不成交 |
| Post Only | 避免一开始吃掉对手盘；否则拒绝或按 Venue 规则重定价 | 各交易所的 reject/reprice 行为可能不同 |
| Iceberg / Reserve | 分批暴露可见数量，隐藏总订单量 | Queue Priority、识别风险、补单成本 |
| Pegged（Primary/Mid/Market） | 按参考价浮动，通常有 price cap/floor | 重定价、行情延迟、venue-specific semantics |
| OCO | 一笔订单执行/触发后取消另一笔 | 非原子合成逻辑可能导致两笔同时成交 |
| Bracket | Entry + Take Profit + Stop Loss | Child order 数量联动、partial fill、竞态 |

**Stop 触发不保证最终 Fill；IOC 的“立即执行”也不等于一定成交。**

## 3. 常见 Execution Algo 分类

| 类别 | Algo | 典型目的 | 主要约束与风险 |
| --- | --- | --- | --- |
| 时间驱动 | TWAP | 将订单均匀分配到执行时段 | 时间切片可预测；无视实际流动性 |
| 预测成交量 | VWAP | 追踪预期市场成交量曲线 | Volume Curve 预测偏差；Benchmark 定义 |
| 实际参与率 | POV / PVol | 尽量以市场成交量的某个百分比执行 | 实际 Volume 滞后；低成交量环境完成度差 |
| 到达价优化 | Arrival Price / Implementation Shortfall（IS） | 控制相对于 Decision/Arrival Price 的执行成本 | Market Impact 与 Timing/Non-fill Risk 的权衡 |
| 主动寻找流动性 | Liquidity Seeking / Opportunistic | 在合适价格/venue 发现可成交流动性 | Adverse Selection、信息泄漏、流动性不确定 |
| 快速执行 | Sweep / Sniper | 快速捕获多档或多 venue 流动性 | 更高冲击成本、竞争、过度吃单 |
| 被动执行 | Passive / Maker-oriented | 尽量挂单，捕获 spread 或降低费用 | Non-fill、Queue Position、Adverse Selection |
| 动态执行 | Adaptive / Dynamic Urgency | 随 spread、volatility、progress 调整 aggressiveness | 反馈失稳、策略过度反应 |
| 隐藏执行 | Iceberg / Randomized Slicing / Dark Seeking | 减少暴露数量、寻找隐性流动性 | Fill Probability 与信息泄漏 |
| 多腿执行 | Basket / Spread / Pair Algo | 统一控制多腿执行与 hedging | Leg Risk、Partial Fill、相关性变化 |
| 对冲执行 | Hedging / Risk Reducing | 降低 inventory/delta/exposure | 滑点、跨市场时序和 basis risk |

**名称不是严格标准**：不同 Broker 所谓的 Adaptive、Sniper 或 Liquidity Seeking 可能有不同实现与目标。

## 4. Passive 与 Aggressive：核心定义

**Passive Execution**：通常下非可立即成交（non-marketable）订单，排队提供流动性（Make Liquidity）。

**Aggressive Execution**：提交可与当前对手盘匹配的订单，主动消耗流动性（Take Liquidity）。

### 4.1 Order Book 例子

~~~text
                 Order Book
Bid       Quantity       Ask       Quantity
100.00       5,000      100.02        3,000

Need to BUY 1,000 shares.
~~~

- **Passive Buy**：以 100.00 提交 Buy Limit；通常加入 Best Bid 的挂单队列，等待卖方打进来。
- **Aggressive Buy**：以 100.02 提交 Buy Limit；如果 Ask 仍有效、匹配规则允许，且现存流动性足够，通常能立即成交。
- 二者都是 **Limit Order**。Limit 不等于 Passive；Market 不等于“一定成交”。
- 交易所限价单即使带有可成交价格，**未成交的剩余部分**仍可能继续挂在 Book 上；如不想留下剩余单，需结合 IOC 等有效期指令。

当前 Spread = 100.02 - 100.00 = 0.02。Aggressive Buy 通常支付 Spread 的一部分或全部；Passive Buy 可能捕获价格改善，**但等待期间价格可能向不利方向变化**。

### 4.2 对比表

| 维度 | Passive | Aggressive |
| --- | --- | --- |
| 流动性角色 | 通常 Maker | 通常 Taker |
| 执行速度 | 较慢且不确定 | 通常更快 |
| 成交概率 | 较低，受排队与市场影响 | 通常较高，但非保证 |
| Spread | 可能获得价格改善 | 通常跨价差 |
| 直接 Market Impact | 通常较低 | 通常较高，特别是 Sweep 多档 |
| Fee | 可能有 Maker Rebate，取决于 Venue | 常见 Taker Fee，取决于 Venue |
| 主要风险 | Non-fill、Adverse Selection、Queue Position | Slippage、Market Impact、Liquidity Exhaustion |
| 常见 TIF | GTC/Day 等 | IOC/FOK 等经常使用，但并非唯一 |

**Maker/Taker 由订单实际是否新增/消耗挂单流动性决定，而不是仅凭报单字段判断。** 不同 Venue 的费用体系和撮合规则也可能不同。

### 4.3 Aggressiveness 是连续尺度

以买入为例，可能存在：

~~~text
Deep Passive
    -> Join Best Bid
    -> Improve Bid (inside spread)
    -> Cross Spread (marketable limit)
    -> Sweep multiple price levels / venues
~~~

这只是常见排序，不是任何交易所都保证支持的固定策略链。例如 Spread 仅一个 Tick 时，无法继续 Improve Bid。

算法可依据 Deadline、Urgency、Spread、Volatility、Liquidity、Fill Rate、Target-vs-Actual 进度动态调节。

## 5. Passive 为什么也有成本？

### 5.1 Adverse Selection（逆向选择）

~~~text
Original Book: 100.00 / 100.02
Passive Buy:   100.00
Large sells hit the Bid; your order fills.
Market moves:  99.95 / 99.97
~~~

挂在 Bid 并不等于成交质量一定更好。你可能恰好在市场即将下跌时被卖方选中成交。

成本可**概念性**拆成：

- Spread Capture / Price Improvement：降低成交成本的潜在收益。
- Maker Fee / Rebate：按实际 Venue 费用正负计入。
- Adverse Selection：被动成交后的不利市场价格变化。
- Queue Opportunity Cost：排队未成交导致之后不得不追价。
- Non-fill / Timing Risk：等不到 Fill，错过价格或 Deadline。

不要将这些概念项不加定义地相加当作严格统一的数学公式：计价方向、时间窗口、基准以及重叠项必须先固定。

### 5.2 Queue Position

对于 Price-Time Priority / FIFO 场所，同价更早进入队列通常更有优先权；Cancel/Replace 是否丢失队列优先级取决于规则与变更类型。Pro-rata 市场则需要另一套队列/Fill 模型。

因此 Peg Repricing 或频繁 Replace 可能改善价格，却降低 Fill Probability。

## 6. 重点 Algo 的执行思路

### 6.1 TWAP（Time-Weighted Average Price）

目标：在给定时间区间内按照时间轨迹拆单。

~~~text
Parent: BUY 120,000
Window: 12:00-13:00
12 intervals x target 10,000 shares
~~~

实践中需要决定：

- Schedule：固定切片、随机化时间/数量、尾部 catch-up。
- Placement：每个切片先 passive，还是直接 aggressive？
- Shortfall：落后进度时是否提高 urgency？
- Guards：涨跌停、price limit、max participation、交易暂停。

TWAP 并不天然意味着每次必须在固定时刻发送一个相同数量的 Market Order。

### 6.2 VWAP（Volume-Weighted Average Price）

目标：跟随**预期/估计**的市场成交量分布，尽量贴近 VWAP Benchmark。

~~~text
Expected intraday volume profile:
09:30-10:00    20%
10:00-11:00    15%
11:00-12:00    10%
...
15:30-16:00    25%
~~~

目标执行数量通常根据 Parent Quantity × 各时间段预测成交量占比制定，再结合已经执行量、实际成交量、实时流动性和约束动态修正。

核心风险：**历史曲线不等于当天真实成交量**。VWAP Benchmark 的具体数据范围、交易时段、是否排除特殊成交应事先约定。

### 6.3 POV / PVol（Percentage of Volume）

目标：以某个比例参与**观测到的**市场交易量。

~~~text
Target participation = 10%
Observed eligible market traded volume = 100,000
Indicative cumulative participation target ~= 10,000
~~~

工程约束：

- 成交量报告可能延迟，必须确定数据源与过滤范围。
- 市场几乎无交易时，严格 POV 可能几乎不执行。
- 参与率受 parent remaining、market access、hard limit、deadline 约束。
- 完成进度可能追不上目标；有些 POV 产品支持收盘前加速，语义取决于配置。

**VWAP vs POV**：VWAP 基本参考预测成交量曲线；POV 基本参考实际观测成交量。真实算法可以结合两者，不应把它们视为完全互斥。

### 6.4 Arrival Price / Implementation Shortfall（IS）

用 Parent Decision/Arrival Price 作为执行基准，尝试降低相对于基准的总执行成本。

关键取舍：

~~~text
Execute faster:
  less timing risk / missed opportunity
  more spread crossing & market impact

Execute slower:
  potentially lower immediate impact
  more non-fill / adverse price movement risk
~~~

常见评估项：

- Executed quantity 相对 Arrival Benchmark 的 signed slippage。
- Fees / commissions。
- Unfilled quantity 的 Opportunity Cost（需明确何时/如何计价）。
- Market Impact 和 Volatility / Timing Risk。

**IS 与 VWAP 的 Benchmark 不是一回事**。IS 通常更关心从订单决策开始的真实交易成本和未成交风险。

### 6.5 Iceberg / Randomized Slicing

~~~text
Parent = 1,000,000
Displayed slice = 10,000
On fill -> replenish
~~~

- Venue-native Iceberg 与 Broker 合成 Child Orders 在 Queue Priority、消息量和可见性方面不同。
- Random Slice Size、Random Timing 可以降低规律性，但不能保证防止识别。
- Replenishment 之前需要 reconcile Fill / LeavesQty，避免超额执行。

### 6.6 Pegged Order

示例行情：

~~~text
Bid = 100.00
Ask = 100.02
Mid = 100.01
~~~

- **Primary Peg**：常按本方最优价及偏移定价。
- **Midpoint Peg**：参考中间价，例如 100.01。
- **Market Peg**：通常参考对手方报价，精确定义取决于 Venue。
- Venue 原生 Peg、Broker 合成 Peg 的规则可能不同；可能存在 offset、price cap、tick rounding、post-only 或 crossing 约束。

Peg 对 Execution Engine 的影响是持续重新定价、Replace/Cancel、状态管理，以及行情延迟导致的错误报单风险。

### 6.7 Liquidity Seeking / Sweep

- Sweep：在价格上限/下限内，快速跨越多个 level / Venue 消耗可用流动性。
- Liquidity Seeking：结合 Lit/Dark 或多 LP，在市场、成本和业务约束下搜索 Fill。
- Sniper/Opportunistic：发现特定条件或流动性时迅速执行；具体定义依 Broker 策略。

风险包括：同时向多个 Venue 发送造成超额成交、行情快照过时、Hidden Liquidity 无法保证 Fill、多个子订单 Cancel 竞态。

## 7. Execution Cost / 风险指标

在比较 Passive 与 Aggressive 时，应明确以下 Benchmark 和指标：

| 指标 | 含义 |
| --- | --- |
| Fill Rate | 成交数量 / 目标数量；应明确计算窗口 |
| Time to Fill | 从报单/决策到部分或全部成交的时间 |
| Slippage | 相对于指定基准价的成交偏离 |
| Spread Paid / Captured | 订单跨价差成本或价格改善 |
| Market Impact | 自身交易可能引起的价格变化；估计需模型 |
| Adverse Selection | Fill 后市场朝不利方向移动的程度 |
| Participation Rate | 自身成交量占合格市场成交量比例 |
| Implementation Shortfall | 对 Decision/Arrival Benchmark 的整体成本 |
| Cancel / Replace Rate | 子订单维护强度、消息压力 |
| Completion Risk | 截止时间前无法完成 Parent Order |
| Information Leakage | 订单意图或规模被市场推断的风险 |

一个指标改善可能让另一个恶化：例如过度追求 Fill Rate 往往增加 Market Impact；过度追求 Spread Capture 可能导致严重 Non-fill。

## 8. 从 Algo 到 SOR：职责与约束

### 8.1 一个通用策略决策

~~~text
ParentOrder:
  side = BUY
  totalQty = 100,000
  hardLimit = 100.10
  deadline = 15:30
  maxParticipation = 20%

Algo:
  targetDoneByNow = 40,000
  actualFilled = 30,000
  lag = 10,000

Decision:
  increase urgency (if policy allows)
  choose a more marketable price
  submit bounded child quantity

SOR:
  compare venues and available levels
  account for fees, stale quotes, venue status
  route within hard limit and exposure budget
~~~

可将 Algo 理解成定义 execution trajectory 与 urgency，将 SOR 理解成执行细节的 venue/route optimizer。但不同产品会存在重叠。

### 8.2 必须保持的不变量

1. **Quantity Safety**：Parent 已成交 + 全部未结束 Child 可能成交的剩余风险，不能无约束超过 Parent Total（需纳入 replace/cancel in-flight 风险）。
2. **Price Protection**：BUY 不得突破 hard limit 上限；SELL 不得突破 hard limit 下限。所有重报价与 Sweep 必须受此保护。
3. **Risk / Permission**：Market Access、Credit、Position、Order Size、Fat Finger、Venue Trading Status 必须检查。
4. **Market Data Validity**：Stale / Gap / Source Down / Expired Quote 不应被当作正常可交易流动性。
5. **At-least-once Events / Idempotency**：重复 Ack、重复 Fill、Replay、Reconnect 需要去重和一致性管理。
6. **Cancel Pending != Cancelled**：撤单请求发出后仍可能成交，不能立即释放所有可成交数量预算。
7. **Deadlines != Guaranteed Completion**：超时后应有明确的 Halt / Cancel / Escalation / Reconcile 策略。

“所有 Child potential leavesQty 之和不得超过 parent remaining”是最保守的控制方式之一；若允许 overbooking，则必须有独立、显式且有风险上界的 policy，不能靠乐观假设撤单会成功。

## 9. Execution Engine 的关键状态与事件

### 9.1 Child Order 典型内部状态

~~~text
NewRequested
    |
    v
PendingNew ---Reject---> Rejected
    |
   Ack
    v
Working <---PartialFill--- Working
   |   \
   |    \---FullFill---> Filled
   |
   +---CancelRequest---> PendingCancel
   |                         |
   |                   CancelAck / Reject / Fill
   |                         |
   |                    terminal or Working
   |
   +---ReplaceRequest--> PendingReplace
                             |
                       Ack / Reject / Fill
~~~

图仅为教学简化版，不代表真实 FIX 状态枚举。

- **Ack、Trade、Cancel Reject 可能乱序或重复**；事实层要允许和约束这些消息。
- **Fill 是成交事实**：不能只因为本地已经提交 Cancel 而拒绝合法晚到 Fill。
- Replace/Cancel 过程中应跟踪请求版本、OrigClOrdID/ClOrdID、LeavesQty、CumQty、ExecID 及 Venue-specific semantics。
- New/Replace/Cancel Timeout 表示本地尚未获得确认，**不等于外部订单没有生效**。
- Event Ordering、Defer / Reconciliation、Recovery 策略独立于执行策略本身。

### 9.2 策略层与事实层隔离

推荐的概念划分：

~~~text
Market/Timer/Progress Inputs
      |
      v
Algo Policy
  "Need to increase urgency"
  "Wish to place/replace/cancel Child"
      |
      v
Risk & Execution Constraints
      |
      v
Order State Machine
  validate requests and apply confirmed facts
      |
      v
Durable State / Outbox (if required)
      |
      v
Side Effects: send messages, notify, schedule
~~~

- **Algo Policy** 决定期望行动；**Order State Machine** 管理内部订单状态和外部已确认事实。
- Send Order/Cancel/Replace 是**可能失败的 side effect**，不要直接当作已经确认的状态转移。
- 对需要恢复和避免重复执行的系统，持久化的状态、外发意图/outbox、幂等关联及 replay/reconciliation 机制必须协同设计。
- 不必为了“强状态机”制造过多状态；关键是 Invariants、事实优先、失败后的可恢复路径。

## 10. 结合 Market Data：什么时候加速/停止执行？

### 10.1 因进度落后而加速

~~~text
Current target = 40,000
Actual filled = 30,000
Behind schedule = 10,000
~~~

可选策略：增加 Child 频率/大小，提高报价，从 Passive 切到 Marketable Limit，或 Sweep。**必须先通过 price/risk/participation 等约束**，且策略需要防止来回频繁振荡。

### 10.2 遇到 Stale / Sequence Gap

- 如果某 Venue/LP 的 Book 数据过时，应该暂停依赖该来源的新增成交决策，或采用明确的 fallback policy。
- 不能把旧价格当作可继续 Sweep 的确定流动性。
- Cancel Existing Order 是否仍可发送，通常与 Market Data Staleness 不同；应把“停止新下单”和“尝试减仓/撤单”分开处理。
- 快行情下可能需要 Quote Age、Feed Latency、End-to-end Processing Latency、Source Health 等多种保护。

## 11. FX 特殊性

与拥有集中 Order Book / Consolidated Tape 的市场相比，FX Spot 常见特性包括：

- 市场分散、很多交易发生在 OTC/双边关系中，缺少覆盖全部 FX 市场的统一 Tape 或 NBBO。
- LP 报价可能有 Credit、Relationship、Min Size、Full Amount/Sweepable、Last Look、Firm/Indicative 等附加条件。
- **Full Amount / Sweepable 是流动性/成交模式，不是 TWAP/VWAP 那样的执行目标函数。**
- RFQ/RFS 是询报价工作流，不应简单归类成 Execution Algo。
- 因缺少统一的已成交量观测，FX VWAP/POV 需明确定义使用何种可观测 Volume Proxy；不能直接照搬股票市场 consolidated volume 的假设。
- Internalization-first / External Hedging 可以是路由与风险控制策略，是否称 Algo 取决于内部产品边界。
- 路由不能只按 Best Price；还要比较 Firmness、Last-look rejection risk、Credit、Latency、Hit Ratio、Toxicity、Hedge Cost。

一个可能的 FX 链路：

~~~text
Client Parent / Algo Order
        |
   Execution Strategy
        |
   Liquidity Aggregator
   + Credit / Venue Policy
        |
   SOR / Child Execution
        |
   Internal Liquidity or External LP / ECN
        |
   Fills + Rejections + Hedge / Reconciliation
~~~

## 12. 面试常见问题

**Q1：Limit Order 是不是 Passive？**

不是。以 Ask 价格提交的 Buy Limit 是 Marketable Limit，通常立即 Take Liquidity；以 Bid 价格提交的 Buy Limit 通常是 Passive。部分成交后的剩余单可能变成 Resting Order。

**Q2：Passive 一定比 Aggressive 便宜吗？**

不一定。Passive 可能获得更好的执行价格/费用，但面临 Non-fill、Queue Position 和 Adverse Selection。比较需要固定 Benchmark、时间窗口及未成交的处理方式。

**Q3：为什么 TWAP/VWAP 也要支持 Aggressive？**

落后 Schedule、接近 Deadline、市场流动性短暂充足、风险骤增时可能需要提高 Urgency；不能只靠固定的 Passive 挂单完成目标。

**Q4：VWAP 和 POV 的最大差别？**

VWAP 倾向跟随**预测市场成交量时间分布**，POV 倾向维持**对实际观测成交量的参与比例**。两者都受可用成交量及输入数据质量影响。

**Q5：Algo 和 SOR 的边界是什么？**

典型分工：Algo 决定 When/How Much/How Urgent；SOR 决定 Where/How to Route。但 Venue Liquidity、Fees、Latency 等因素会相互反馈，实际系统可融合。

**Q6：Cancel Request 发出后能否立刻认为 Child 不会再成交？**

不能。只发送了 Cancel 不代表 Exchange/LP 接受；PendingCancel 期间可能仍有 Fill。Parent 数量预算应计入这些潜在成交。

**Q7：Adaptive Algo 最容易出现什么 Failure Mode？**

- Market Data Stale 造成错误提价/Sweep。
- Schedule Lag 导致激进度控制失稳、频繁 Replace。
- 多 Venue 同时挂单或并发 Cancel 造成 overfill。
- Fill / Cancel Ack 乱序导致数量算错。
- Queue Position 因频繁改价损失。
- Restart/Replay 后重复下单或未恢复未结订单。
- Model 假设（成交量/Fill Probability）随市场环境变化失效。

## 13. 一页总结

~~~text
Order Type:
  Market / Limit / Stop / Stop Limit / IOC / FOK / Peg / Iceberg

Execution Algo:
  TWAP / VWAP / POV / Arrival Price(IS) / Liquidity Seeking
  Adaptive / Opportunistic / Passive / Sweep

Execution Intent:
  Passive = generally make liquidity, seek better price
  Aggressive = take liquidity, seek quicker execution

Central Trade-off:
  Cost / Market Impact / Adverse Selection / Non-fill / Timing Risk

Algo:
  WHEN? HOW MUCH? HOW URGENT?

SOR:
  WHERE? WHICH VENUE / LIQUIDITY? AT WHAT EXECUTION COST?

Execution Engine:
  HOW TO HANDLE NEW / REPLACE / CANCEL / FILL / TIMEOUT
  WITHOUT BREAKING QUANTITY, PRICE, RISK AND RECOVERY INVARIANTS?
~~~

**最重要的一句话：Execution Algo 不是简单把大单拆成小单，而是在价格、流动性、时间、执行风险和系统约束之间动态权衡；SOR 与 Execution Engine 则负责把策略意图安全、可追踪地落地。**
