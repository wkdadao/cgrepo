# Market Data Expiry、Staleness 与 MDLite 行情恢复设计

> 适用场景：FX FeedHandler → MDLite → Pricing Engine（支持多 LP / 多 FeedHandler 行情聚合）
>
> 本文整理系统设计讨论。**“当前系统”**指讨论中明确提供的实现事实；**“建议”**是可选的工程改进，不应混写成已落地功能。FIX 字段以 FIX 4.4 为例，具体 LP 行为仍以对手方协议和双边约定为准。

## 1. 核心结论

1. **Entry Expiry → Delete**：FeedHandler 根据约定的 Expiry Rule 判定某条 Quote/Entry 生命周期结束，向 Pricing Engine 发送 Delete Delta，从 Active Book 移除；可保留内部原因码 \`Expired\` 供诊断。
2. **Source / Book 不可信 → Stale / Invalidate**：Heartbeat Timeout、不可恢复的 Sequence Gap 等表示当前 Book 不可靠；原则上不等价于 LP 撤销了每个订单/报价。
3. **MDLite 不需要逐 Price 携带 TTL**：当前协议传递行情 Delta 和状态（如 \`tradeable\`、\`stale\`、\`indicative\`），Price Expiry 由 FeedHandler 管理；Pricing Engine 维护重建后的状态。
4. **Heartbeat + Sequence + Snapshot 提供活性、完整性与恢复能力**，但 Sequence 连续不等于 Market Data 具备严格的端到端时效性。
5. **Client Quote Expiry / Last Look 是另一层生命周期**：客户报价的失效时间，与上游 LP 行情的 TTL 不应混用。

一句话：**FeedHandler 管行情生命周期；MDLite 可靠地传播状态；Pricing Engine 管本地行情可用性与聚合；Execution 管对客 Quote 能否成交。**

## 2. 明确不同的语义

| 概念 | 表示什么 | 典型触发 | 对 Pricing 的影响 |
| --- | --- | --- | --- |
| Delete | 指定 Entry 不再存在于有效 Book 中 | 上游删除、取消、单条 Quote 到期 | 移除其流动性贡献 |
| Expired | Entry 到达约定失效点，属于 Delete 的一种**原因** | TTL / 上游显式到期时间 | 不得继续用于新定价 |
| Stale / Invalidate | 现有数据仍可保存，但暂时不可信/不可交易 | Heartbeat 超时、Sequence Gap、数据源失联 | 排除受影响 Scope，等待校验恢复 |
| Indicative | 有价格但不是可直接依赖的可成交报价 | LP 状态/业务属性 | 按 Pricing Policy 处理，通常不可当作 firm liquidity |
| Tradeable | 当前行情来源声明可交易（或内部状态可交易） | 正常行情更新与恢复 | 仍需通过健康、一致性和业务检查 |
| Quote Expiry | 已向客户发布的 Quote 生命周期终点 | Quote TTL / \`ValidUntilTime\` | Execution 不接受过期 Quote |

### 特别注意

- **Stale != Delete**：Stale 是有效性/可信度问题，Delete 是存在性/生命周期问题；两者在聚合时可能都表现为“不贡献流动性”。
- **Delete != 一定在物理上释放内存**：Active Book 的逻辑删除可以与对象池复用、诊断缓存分离。
- **没有更新 != 必然过期**：事件驱动的交易所 L2/L3 Book，其挂单通常一直有效直到收到相应订单事件；不能凭“某 Level 很久未变”就任意 Delete。
- **Tradeable != 数据一定足够新**：协议业务状态、通道健康、序列完整性和端到端延迟是不同维度。
- 本地生成的 \`Expired\` Delete 不等于上游 FIX 真的发送了 Delete；审计中应区分 \`upstream_cancel\` 和 \`local_expiry\`。

## 3. FIX 中是否存在 Expiry 字段？

以下为 **FIX 4.4** 示例（字段是否出现取决于消息、对手方实现以及约定）：

| 消息 | 字段 | 作用范围与注意点 |
| --- | --- | --- |
| \`35=S\` Quote | \`62=ValidUntilTime\` | Quote 级的过期时间；可选 |
| \`35=W\` Snapshot/Full Refresh | \`126=ExpireTime\` | 位于 \`NoMDEntries(268)\` 内；对该 MD Entry 生效；可选，标准语境常用于 Bid/Offer 代表订单 |
| \`35=X\` Incremental Refresh | \`126=ExpireTime\` | 同上，按 Entry；可选 |
| \`35=W/X\` | \`432=ExpireDate\` | 对相应 Entry 的过期日期；同一 Entry 中不能与 \`126\` 同时指定 |
| \`35=W/X\` | \`59=TimeInForce\` | 当 Bid/Offer 代表订单时的有效期类型；不等于通用行情 Freshness |
| \`35=W/X\` | \`273=MDEntryTime\` | 时间信息，不是 Expire Time |
| \`35=X\` | \`279=MDUpdateAction\` | \`0=New\`、\`1=Change\`、\`2=Delete\` |
| \`35=X\` | \`285=DeleteReason\` | 可选 Delete 原因；不能假设存在标准枚举 \`Expired\` |

**重要：FIX 标准“允许发送 ExpireTime”不意味着 LP 一定发送，更不意味着所有 Book Entry 都应有 TTL。** 对无字段的情况，需要依据该行情模型及 Feed Specification 判断：

- **LP Quote / Snapshot/Refresh 模型**：如果 LP 约定某条 Quote 每 N ms 必须刷新，可在 FeedHandler 按约定管理 Expiry。
- **Exchange incremental order-book 模型**：以 Add / Change / Delete 等订单状态事件维护，网络健康/Book 可用性独立管理。
- 如果一个 Snapshot 一次性刷新多档、各档共同失效，可以共享 Expiry；若 Bid/Ask、Tier 或 Entry 独立更新，失效粒度应与更新粒度一致。

参考：
- [FIX 4.4 Quote S — OnixS](https://www.onixs.biz/fix-dictionary/4.4/msgtype_s_83.html)
- [FIX 4.4 Market Data Snapshot W — OnixS](https://www.onixs.biz/fix-dictionary/4.4/msgtype_w_87.html)
- [FIX 4.4 Market Data Incremental X — OnixS](https://www.onixs.biz/fix-dictionary/4.4/msgtype_x_88.html)
- [FIX 4.4 MDUpdateAction 279 — OnixS](https://www.onixs.biz/fix-dictionary/4.4/tagnum_279.html)

## 4. 当前 MDLite 架构（已确认）

~~~text
        LP / Exchange / Upstream Market Data
                        |
                        v
                   FeedHandler
          - 维护本地上游行情状态
          - 执行约定的 Entry Expiry Rule
          - Price 到期 -> 产生 Delete Delta
                        |
                        v
                MDLite Data Protocol
          - Add / Change / Delete Delta
          - tradeable / stale / indicative 等状态
          - 不携带每 Price 的 TTL / ExpireAt
          - Heartbeat 带 Sequence Number
                        |
                        v
                  Pricing Engine
          - 维护 Local Book / 多来源聚合
          - Heartbeat Timeout -> Stale 受影响行情
          - 发现丢包 -> 请求 Snapshot
          - 恢复后才重新纳入有效定价
                        |
                        v
             Client Quote / Execution
          - 客户 Quote 有独立 Expiry Timestamp
          - Last Look 阶段校验 Quote
~~~

**已确认行为：**
- FeedHandler 检测单 Price 到期，发 Delta 删除。
- MDLite 行情消息没有逐条 Price 的 TTL/Expire 字段。
- MDLite 提供 \`tradeable\`、\`stale\`、\`indicative\` 等状态。
- Heartbeat 携带 Sequence Number。
- Pricing Engine 发现丢包会请求 Snapshot。
- Heartbeat 超时，Pricing Engine 将相应 Price/行情标记 Stale。
- 对客 Quote 带 Expiry Timestamp，Last Look 时也检查有效性。

**仍需按真实协议核对（不作既定事实）：** Heartbeat 与 Delta 是否共用同一个连续 Sequence Domain；Sequence 按 Channel 还是 Instrument 编排；Snapshot Watermark 语义；恢复期间的 Delta Buffering；Heartbeat 对应的确切失效 Scope；传输是否有重排/背压处理。

## 5. Expiry：为什么采用 Delete，而不是只设 Stale？

例如 LP 报价：

~~~text
LP1 EUR/USD Bid = 1.08420, Qty = 5M
ExpireAt = t0 + 200ms (示意)
~~~

到期意味着**该 Entry 的生命周期已结束**。FeedHandler：

1. 找到对应 Entry（唯一 QuoteID/EntryID，或协议定义的 Key）；
2. 核对是否仍为当前版本、是否真的到期；
3. 发布 \`Delete(entry, reason=Expired)\`；
4. 从自己的 Active Book 中移除该 Entry。

Pricing Engine 消费 Delete 后，将该 LP Entry 从 Local Book 移除，并对 Aggregated Book 做相应更新。

对比：

~~~text
Expiry:  ACTIVE --[TTL reached]--> DELETED
             --[new Quote/Add]--> ACTIVE

Source Failure: ACTIVE --[heartbeat timeout / gap]--> STALE
             --[validated snapshot/replay]--> ACTIVE
~~~

### 防止旧 Expiry Timer 误删新版本

~~~cpp
struct ExpiryToken {
    EntryId id;
    uint64_t generation;  // monotonically increasing entry version
    uint64_t deadlineNs;
};

void onTimer(const ExpiryToken& token, uint64_t nowNs) {
    auto* entry = findEntry(token.id);
    if (!entry) return;

    if (entry->generation != token.generation) return;
    if (nowNs < entry->deadlineNs) return;

    publishDelete(token.id, DeleteReason::Expired);
    removeFromActiveBook(token.id);
}
~~~

上例是**概念代码**：假设单 Owner Thread 串行修改 Book、Timer 事件不会与更新无序竞争。否则还需要处理并发与发布顺序。实际逻辑亦需防止 ID 重用造成 ABA（Version/Generation 与 Quote 身份一致性）。

### 何时用 Invalidate / Stale？

- MDLite Heartbeat Timeout；
- 序列中断，使受影响区间的增量状态不可信；
- LP 断线但原 Book 可暂存以便诊断；
- Producer 尚活着，但其上游 Instrument 被判定不可用。

此时不应将 Stale 伪装成“LP 取消了全部订单”。可以在 Source/Instrument Scope 下标记 \`valid=false\`，保留 Book 但排除流动性贡献。

## 6. Sequenced Heartbeat、Gap Detection 与 Snapshot

### 6.1 能保障什么？

当 Heartbeat 和 Delta **属于相同的连续序列域**，并且 Consumer 对该域严格按序处理时：

~~~text
Seq 100: Add
Seq 101: Delete
Seq 102: Heartbeat
~~~

- 若 \`101\` 丢失但收到 \`102\`，Consumer 能发现 \`100 -> 102\` 的 Gap；
- 发现 Gap 后，立即将受影响范围标记 Stale / Invalidate，发起 Snapshot；
- 若严格按序完成 \`102\` 的应用，说明该 Sequence Domain 中之前的事件已经成功处理（不是仅仅“收到”）；
- Heartbeat Timeout 则可以检测一段时间内**未能接收到/处理到**该 Channel 的有效活性信息，具体使用哪种时间要明确。

**若 Heartbeat 用独立的序列域，或只携带“最新生产序号”但本身不按同一 Delta 顺序发送，推论必须重新核对。**

### 6.2 建议的恢复状态机

~~~text
                   +---------+
                   | ACTIVE  |
                   +----+----+
                        |
                 Gap / Timeout
                        |
                        v
                   +---------+
                   |  STALE  |  (stop using affected liquidity)
                   +----+----+
                        |
               request Snapshot
                        |
                        v
                 +------------+
                 | RECOVERING |
                 +------+-----+
                        |
       snapshot + required delta continuity verified
                        |
                        v
                   +---------+
                   | ACTIVE  |
                   +---------+
~~~

恢复注意事项：
1. 进入 STALE 后，**即使重新收到 Heartbeat，也不能默认将旧缓存全部恢复 Tradeable**；
2. Snapshot 必须有清楚的一致性边界（如 Sequence/Watermark），Consumer 能在该边界与后续 Delta 之间正确衔接；
3. 按传输语义缓存并应用 Snapshot 后的 Delta，避免覆盖更新或重复应用；
4. 完成一致性验证后恢复 ACTIVE；若恢复失败，继续 Stale 或重新请求 Snapshot；
5. 如果 Snapshot 天然就是某状态的权威替换，旧 Book 应整体替换，而不是简单叠加新 Entry；
6. 若某 Entry 已过期，恢复的 Snapshot 应体现 FeedHandler 当前有效状态，不应“复活”过期 Quote。

这里不强制指定 Snapshot 算法：Snapshot 是否携带序号、增量能否 Buffer、允许多少 Snapshot Lag，需由 MDLite 实际协议决定。

## 7. 为什么仍要考虑 Delay / Backlog？

**Sequence 连续性解决完整性，不直接解决时效性。**

示例（所有毫秒数仅为说明）：

~~~text
t=0ms    FeedHandler Add(A), A 的上游 TTL = 200ms
t=200ms  FeedHandler 生成 Delete(A)
t=205ms  Pricing Engine 仍在处理较早的一批 Delta
t=206ms  Delete(A) 尚未被 Pricing Engine 应用
~~~

在 \`t=205ms\`：

- MDLite 完全可能**没有丢包**；
- Sequence 可以完整且最终全部到达；
- Heartbeat 可能仍在正常发送；
- Pricing Engine 的缓存仍暂时包含 A。

如果 Heartbeat 超时基于“消费进度”，严重 Backlog 最终会引发超时；如果基于单独接收线程的收包时间，则 Consumer 业务线程积压可能被掩盖。**短于 Timeout 阈值的落后窗口始终可能存在**。

### 三种不同的业务保证

| 保证 | 当前机制的能力 | 备注 |
| --- | --- | --- |
| Eventual Consistency：过期最终会从 PE Book 删除 | 在 Delta 可达、恢复正确时成立 | FH → Delete → PE |
| Gap/Disconnection Safety：检测到异常后不继续用不完整 Book | 支持 | 依赖失效传播/作用范围正确 |
| Hard End-to-End Expiry：截止时间之后 PE 绝不使用该 Entry | **无法仅由无 TTL 的 MDLite + Sequence + Heartbeat 证明** | 需额外约束 |

**重要修正：** 不应因为具备 Sequenced Heartbeat 就断言不存在 Stale Price 使用窗口。要保证硬截止时间，需要让 PE 知道截止时间，或者有其他可证明的全链路时延上界/同步阻断机制。

### 是否要让 MDLite 增加逐 Price TTL？

**不一定。** 如果业务允许有限的异步传播延迟、Execution 侧另有明确风险政策，那么原有简洁的 MDLite 接口可能更好。若业务需要绝对的 Hard Expiry，则需要论证：传递 Deadline（Entry 或 Source/Book 粒度）、使用前验证，或其他同等严格的机制。

更轻量的可选补充：
- 监控 Consumer 的 Sequence Lag（\`latestPublishedSeq - lastAppliedSeq\`，前提是安全获得两个序号）；
- 监控 FeedHandler → PE 的 Processing Age / Queue Latency；
- 超过时延或积压门限时主动 Invalidate，独立于 Heartbeat 是否超时；
- 区分 \`last packet received\`、\`last sequence processed\` 与 \`last price updated\` 三种指标；
- 统计 \`Expire -> FH Delete -> PE Applied Delete\` 的 p99 / p99.9 延迟。

这些是**建议**，不是已确认的生产实现。

## 8. 多来源行情聚合时的 Stale / Delete

假设：

| Source | EUR/USD Bid | Size | 有效性 |
| --- | --- | --- | --- |
| LP1 | 1.08420 | 5M | ACTIVE |
| LP2 | 1.08420 | 3M | ACTIVE |
| LP3 | 1.08419 | 4M | ACTIVE |

聚合：

~~~text
Bid 1.08420 -> 8M
Bid 1.08419 -> 4M
~~~

- **LP1 一条 Entry 到期**：FH 发该 Entry 的 Delete，PE 删除 LP1 的 5M 贡献；聚合 \`1.08420 -> 3M\`。
- **LP1 整个 EUR/USD Book 因 Heartbeat Timeout 而 Stale**：无需把 LP1 所有 Entry 都当成“真实撤单”，PE 逻辑上排除 LP1 的贡献；聚合结果同样可为 \`1.08420 -> 3M\`。
- **LP1 恢复**：必须先验证状态一致性，不能把历史 Book 无条件重新打开。

可选实现策略：

1. **Physical Update**：失效时立即删除/撤销 Aggregate Book 中所有受影响贡献。代价是可能有大量更新，尾延迟较高。
2. **Logical Invalidate**：Local Book 原样保存，Source 状态设 INVALID；查询与聚合时只纳入有效 Source。
3. **Hybrid**：Source Validity 先使 Hot Path 停用该 Source；异步清理旧 Book/聚合缓存，但任何对外发布的已物化聚合视图必须同步保持有效，不能引用已失效贡献。

对于低延迟 Pricing，建议优先保证 **O(1) 级 Source/Book 失效传播的可见性**，再按聚合数据结构决定重算/增量维护策略；不能仅修改 \`valid\` flag，却继续使用包含 LP1 的旧 Aggregate Cache。

## 9. Client Quote Expiry 和 Last Look 是独立层

三种时钟/生命周期：

~~~text
Upstream LP Quote Expiry  -> FH 管理 Market Data Entry
MDLite Health/Staleness   -> PE 管理 Local Book 可用性
Client Quote Expiry       -> Execution 按已发布 QuoteID 校验
~~~

示例：

~~~text
t=0ms    LP Quote 发布（业务 TTL = 100ms）
t=80ms   PE 基于 LP Quote 生成客户 Quote（自身 Expiry=280ms）
t=100ms  LP Quote 到期并被删除
t=150ms  客户请求成交该报价
~~~

此时**上游 Quote 已失效，并不自动等于对客 Quote 已过期**。如果是银行 Principal Pricing，客户 Quote 是单独的交易承诺/报价机制；是否能接受、拒绝以及采用何种 Last Look Checks，取决于业务协议、风险政策和披露约束。

Execution 至少需要将不同检查分开建模：
- QuoteID 存在、属于该客户及交易请求；
- 客户 Quote Expiry；
- 业务状态、信用/权限/限额；
- 适用时按既定政策执行的 Last Look Validity / Price Checks。

**Last Look 不能被当作 FeedHandler 行情管理的替代品。**

## 10. 故障矩阵与推荐行为

| 场景 | FeedHandler | Pricing Engine |
| --- | --- | --- |
| 单 Entry 正常到期 | 生成 Delete Delta（内部原因 Expired） | 应用 Delete，更新 Aggregation |
| 上游显式撤销/删除 | 按协议删除并产生 Delta | 应用 Delete |
| 上游 LP 掉线但 MDLite 仍健康 | 根据 LP / Instrument Policy 失效行情 | 收状态/Invalidate，排除受影响 Source |
| MDLite Heartbeat Timeout | 未必知晓 Consumer 状态 | Stale 对应的 Channel/Source/Book |
| Delta 丢失、Heartbeat Sequence 暴露 Gap | 提供 Snapshot/恢复能力 | Stale → 请求 Snapshot → 校验 → Recover |
| 仅收到新 Heartbeat（未验证快照） | 正常发送 Heartbeat | **不应无条件恢复旧 Book** |
| Delta/Heartbeat 在队列中整体延迟 | 正常生产 | 可能出现短暂旧数据窗口；监控 Processing Lag |
| Price Timer 旧任务晚触发 | 核对 Entry Generation 与 Deadline | 不应错误删除新报价 |
| Snapshot 旧版本或 Delta 重排 | 提供可关联的版本/序列语义 | 按 Watermark/Sequence 拒绝旧数据或重新同步 |

## 11. HFT / Production Quality 设计检查表

- [ ] Expiry 的**业务定义**清楚：相对 Received Time 还是 LP Sending Time？是否源于协议的绝对 ExpireAt？
- [ ] TTL 粒度与 Entry 更新粒度一致：Quote、Side、Tier、Symbol 或 Book。
- [ ] Delete 精确定位 Entry，考虑 QuoteID 重用及 Version/Generation。
- [ ] Timer 事件与行情更新不会引起无序删除。
- [ ] Heartbeat、Delta 的 Sequence Domain 定义清楚；多播通道、分片及仪表化范围明确。
- [ ] Gap 立即触发相应的 Stale，不能在不可信 Book 上继续 Pricing。
- [ ] Snapshot 一致性边界和 Recovery Barrier 被严格定义。
- [ ] Reconnect / Heartbeat Resumed 不会自动让未经校验的旧报价重新 Tradeable。
- [ ] Aggregate Book 不会继续引用已失效 Source 的贡献。
- [ ] 监控 FH Expiry Timer Lag、MDLite Data Lag、PE Apply Lag、Snapshot Recovery Time。
- [ ] 如果要求 Hard Expiry，明确独立于 Heartbeat 的强时效保障。
- [ ] 对客 Quote Expiry 与上游行情 Expiry 独立管理，Execution / Last Look 检查符合业务和协议。

## 12. 面试用简洁表述

> Our feed handlers own the upstream quote lifecycle. They apply venue- or LP-specific expiry rules and publish delete deltas when entries expire. MDLite does not carry per-price TTLs; instead, it carries market data updates, status and sequenced heartbeats. The pricing engine uses sequence continuity to detect loss, requests a snapshot for recovery, and invalidates affected books on heartbeat timeout. This keeps venue-specific TTL logic out of the pricing hot path. The important limitation is that sequence correctness and heartbeat liveness do not by themselves guarantee a strict end-to-end freshness deadline; we monitor propagation lag and would add a deadline guard if the business required hard expiry. Client quote expiry and last-look checks remain a separate execution-layer concern.

### 最终取舍

**当前方案（不改 MDLite TTL Schema）**：职责清晰、消息更小、Pricing Hot Path 更简单，适合以 Delta/Status 驱动的多来源行情。

**主要失效模式**：无丢包但有 Queue Backlog、独立 Timer Stall、错误的 Snapshot/Delta 衔接、Source 标记 Stale 后 Aggregated Cache 未同步失效。

**应优先验证的不是“每 Price 加 TTL”，而是**：Sequence Scope、恢复屏障、失效传播及时性与端到端延迟分布。只有业务确实要求绝对 Hard Expiry 时，再推动 Deadline Metadata 或同等级别的独立保障。
