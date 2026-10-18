# FIX Order Flow：消息、订单状态与常见订单类型

> 面试复习笔记｜以 **FIX 4.4** 的标准语义为基准，偏向 Order Entry / Market Connectivity / OMS / SOR。  
> 真实系统以对手方的 **FIX Rules of Engagement / venue-specific dialect** 为准。以下 FIX 片段只展示业务字段，不是包含 Header、SOH、BodyLength、CheckSum 的可直接发送报文。

## 1. 核心 Request / Reply

| 场景 | Client → Broker/Venue | 正常成功 / 中间状态 | 业务拒绝 / 异常 | 核心字段 |
|---|---|---|---|---|
| New Order | `35=D` NewOrderSingle | `35=8` ExecutionReport：New、PendingNew、Trade 等 | 通常 `35=8,150=8` Rejected | `11`, `55`, `54`, `38`, `40`, `44`, `59` |
| Cancel | `35=F` OrderCancelRequest | `35=8,150=6` PendingCancel，最终 `150=4` Canceled | `35=9` OrderCancelReject | `11`, `41`, 可选 `37` |
| Replace / Amend | `35=G` OrderCancelReplaceRequest | `35=8,150=E` PendingReplace，最终 `150=5` Replaced | `35=9` OrderCancelReject | 新 `11`、`41`、`38`、`44` |
| Order Status | `35=H` OrderStatusRequest | 通常 `35=8,150=I` OrderStatus，`39` 表示当前状态 | 对未知订单/非法请求的处理取决于规范及 dialect | `11`（被查询订单的 ID）、`37`、`790` |
| Fill | 无需单独发请求 | `35=8,150=F` Trade；`39=1/2` | 后续可能有 TradeCorrect / TradeCancel | `17`, `31`, `32`, `14`, `151` |
| FIX Session / Protocol Error | 任意消息 | — | `35=3` Reject；有些业务层错误用 `35=j` BusinessMessageReject | `45`, `371`, `372`, `373`, `58` |

**注意：** `35=8` 是订单事件/状态报告，不是所有请求的通用 ACK。Cancel/Replace 失败典型地用 `35=9`。FIX 消息校验错误与业务订单拒绝必须区分。

## 2. 必背字段（Tag）

### 2.1 ID 与订单状态

| Tag | Field | 语义与注意事项 |
|---:|---|---|
| `11` | ClOrdID | 客户端分配的订单指令标识；每个 New / Cancel / Replace 请求都要使用符合 counterparty 唯一性规则的新 ID |
| `41` | OrigClOrdID | Cancel/Replace 指向此前的、未被拒绝的 ClOrdID；不是永远指向最初 New 的 ID |
| `37` | OrderID | Broker/Venue 侧分配的订单标识；Replace 后**可能不变，也可能改变** |
| `17` | ExecID | ExecutionReport 发送方分配的 execution report / execution event 标识；用于识别与去重，但不能简单等同于交易所撮合成交 ID |
| `39` | OrdStatus | 当前订单链（order chain）的状态 |
| `150` | ExecType | 当前这条 ExecutionReport 的事件/原因 |
| `38` | OrderQty | 订单总数量；Replace 里通常表达该订单链的**新总目标量**，包含已成交量 |
| `14` | CumQty | 当前订单链累计已成交量 |
| `151` | LeavesQty | 当前可继续执行的剩余量；终态通常为 0 |
| `32` | LastQty | 当前这次成交数量（增量） |
| `31` | LastPx | 当前这次成交价格 |
| `6` | AvgPx | 累计成交均价 |
| `19` | ExecRefID | 成交撤销/更正时用于关联被调整的执行报告 |
| `103` | OrdRejReason | New order 被拒绝时的订单拒绝原因（如提供） |
| `102` | CxlRejReason | Cancel/Replace 被拒绝的原因 |
| `434` | CxlRejResponseTo | CancelReject 是针对 Cancel (`1`) 还是 Replace (`2`) |
| `790` | OrdStatusReqID | OrderStatusRequest 的**可选请求相关 ID**；区别于 `11` 被查询的订单 ID |
| `58` | Text | 可选说明文本 |

### 2.2 下单参数

| Tag | Field | 常见值与含义 |
|---:|---|---|
| `35` | MsgType | `D` New、`F` Cancel、`G` Replace、`H` Status、`8` ExecutionReport、`9` CancelReject |
| `55` | Symbol | 产品/交易标的（具体标识方式看对手方要求） |
| `54` | Side | `1` Buy；`2` Sell |
| `40` | OrdType | `1` Market；`2` Limit；`3` Stop；`4` Stop Limit |
| `44` | Price | 限价；Buy 不以高于此价格成交，Sell 不以低于此价格成交 |
| `99` | StopPx | Stop / Stop Limit 的触发价格 |
| `59` | TimeInForce | `0` Day；`1` GTC；`3` IOC；`4` FOK |
| `111` | MaxFloor | 最大同时展示量，常用于 Iceberg；具体支持与隐藏量处理依 venue |
| `110` | MinQty | 最小可成交量/最小执行数量约束；不是 Iceberg 的显示量 |
| `60` | TransactTime | 指令发起时间 |

## 3. `39 OrdStatus` vs `150 ExecType`

> **ExecType = what happened in this report.**  
> **OrdStatus = resulting/current state of the order chain.**

### 3.1 常见取值（FIX 4.4）

| Code | `39 OrdStatus` | `150 ExecType` |
|---|---|---|
| `0` | New | New |
| `1` | Partially Filled | — |
| `2` | Filled | — |
| `4` | Canceled | Canceled |
| `5` | — | Replaced |
| `6` | Pending Cancel | Pending Cancel |
| `8` | Rejected | Rejected |
| `A` | Pending New | Pending New |
| `C` | Expired | Expired |
| `E` | Pending Replace | Pending Replace |
| `F` | — | Trade（Partial/Full Fill） |
| `I` | — | Order Status（状态查询回复） |
| `G` | — | Trade Correct |
| `H` | — | Trade Cancel |

**版本坑：** FIX 4.2 中 `ExecType=1` 表示 Partial Fill、`ExecType=2` 表示 Fill；FIX 4.4 合并为 `ExecType=F` Trade。不要混用版本的枚举。

### 3.2 典型组合

| 场景 | `150 ExecType` | `39 OrdStatus` | OMS 应做什么 |
|---|---|---|---|
| New Ack | `0 New` | `0 New` | 确认订单已接受、记录 `37` |
| Partial Fill | `F Trade` | `1 PartiallyFilled` | 处理该次成交，更新累计量 |
| Full Fill | `F Trade` | `2 Filled` | 成交入账，订单终结 |
| Cancel pending | `6 PendingCancel` | 通常 `6 PendingCancel` | 仅标记 pending，**不能当作撤单成功** |
| Cancel accepted | `4 Canceled` | `4 Canceled` | 结束剩余未成交量 |
| Replace accepted（未成交） | `5 Replaced` | `0 New` | 更新生效参数、ClOrdID chain |
| Replace accepted（已部分成交） | `5 Replaced` | `1 PartiallyFilled` | 更新生效参数、保持 CumQty |
| New rejected | `8 Rejected` | `8 Rejected` | 记录业务拒绝 |
| Status response | `I OrderStatus` | 实际当前状态 | reconciliation；**不意味着刚发生新事件** |

只看 `OrdStatus=1` 无法判断本条报文是新成交、Replace 已接受后仍然部分成交，还是状态查询回复。**不能用 OrdStatus 代替 ExecType**。

> 边界：`OrdStatus` 有自己的状态优先级规则（例如 PendingCancel 与 PartiallyFilled 的竞争）。不要仅靠 `39` 判定是否发生新的 fill；应综合 `150`、数量、ID、事件记录和对手方规则。

## 4. ClOrdID / OrigClOrdID / OrderID / ExecID：谁分配、何时变化？

```text
Client / OMS                                   Broker / Venue
  35=D 11=A --------------------------------------->
          <--------------------- 35=8 11=A 37=XYZ 17=E1 150=0 39=0
          <--------------------- 35=8 11=A 37=XYZ 17=E2 150=F 39=1
                                 32=30 31=101.50 14=30 151=70

  35=G 11=B 41=A 38=120 44=101.60 -------------->
          <--------------------- 35=8 11=B 37=XYZ 17=E3 150=5 39=1
                                 14=30 151=90

  35=F 11=C 41=B ------------------------------->
          <--------------------- 35=8 11=C 37=XYZ 17=E4 150=4 39=4
                                 14=30 151=0
```

- `11=A` 是原始 New 的 ID；`11=B` 是 **Replace 指令**的新 ID；`11=C` 是 **Cancel 指令**的新 ID。
- 普通成交一般沿用当前已接受的 ClOrdID，不是每成交一次就改变 ClOrdID。
- `37=XYZ` 常贯穿该订单链，但不能强行假设绝不改变；要维护映射和历史 ID。
- `17=E1/E2/E3/E4` 由 ExecutionReport 发送方分配；不仅成交报告需要 ExecID，其他 ExecutionReport 通常也有。
- 此例是简单的**串行请求**。如果快速连续发送未决 Replace：
  - Sender 按标准建议可 optimistic-chain，`41` 指向最后一个尚未被拒绝的 `11`；
  - Receiver 的确认/拒绝消息可能以最后**已接受**的 ClOrdID 作 `41`；
  - 并非每个 venue 都支持 outstanding cancel/replace 的 chaining。必须处理 rejected version 的回退。

### ID 设计提示

```text
LogicalOrder (internal stable ID)
  ├─ ClOrdID history: A [New accepted] -> B [Replace accepted]
  ├─ Outstanding request: C [Cancel pending / accepted / rejected]
  ├─ Venue OrderID history / mapping: XYZ, possibly new ID after replace
  └─ Execution ledger: ExecID + venue/session-scoped identity + correction links
```

`ExecID` 的唯一性范围、重用情况和 correction 语义受 counterparty 约束；**不要只以全局 ExecID 字符串作为唯一业务键**。

## 5. 数量与成交处理

正常活动订单常有：

```text
OrderQty = CumQty + LeavesQty
```

但它**不是所有 OrdStatus 下都成立**：

- 活动单：`38=100, 14=30, 151=70`。
- 取消后：`38=100, 14=30, 151=0, 39=4`；未成交的 70 不再可执行，故 `100 != 30+0`。
- Replace：`OrderQty` 通常是订单链的**总目标量**，不是单纯新追加的量；例如原目标 100、已成交 30，将目标改为 120，则剩余量 90（若没有其他事件）。
- IOC/FOK、Expired、DoneForDay 等状态同样要按 FIX/venue 语义核对 LeavesQty。
- 应以 `LastQty/LastPx` 描述**本次成交**；`CumQty/AvgPx` 表示订单链累计快照。Replay、重复报告、Trade Correct/Cancel 都应纳入幂等和一致性检查。

**OMS 核心约束**：不要对重复成交再加一次 LastQty；也不能无视合法的 trade bust/correction 而盲目要求 CumQty 在所有情况下单调递增。

## 6. 订单类型、TIF、Iceberg、Execution Algo 在哪里？

### 6.1 `OrdType` / `Price` / `StopPx`

| 订单类型 | 关键字段 | 含义 |
|---|---|---|
| Market | `40=1` | 不设置指定限价，具体执行受市场及风控约束 |
| Limit | `40=2, 44=101.50` | Buy 最多 101.50；Sell 最少 101.50 |
| Stop | `40=3, 99=100.00` | 满足 Stop 触发规则后触发 |
| Stop Limit | `40=4, 99=100.00, 44=99.80` | 触发后按限价规则执行 |
| Iceberg / Display-limited order | 例如 `40=2, 38=10000, 44=101.50, 111=1000` | 总量 10000，每次最大展示 1000；是否支持及补单细则由 venue 决定 |

**Limit 示例：**

```fix
35=D
11=A1001
55=XYZ
54=1          # Buy
40=2          # Limit
38=100000     # Total OrderQty (instrument-dependent units)
44=101.50     # Max buy price / limit
59=0          # Day
```

换成 `54=2` 则是 **Sell 100,000 at 101.50 or better**（卖出最低可接受价 101.50）。`38` 的计量单位、`44` 的价格刻度/报价方式要看产品和接口，不可脱离 instrument 假设。

### 6.2 `TimeInForce` 不属于 `OrdType`

| `59` | 名称 | 核心语义 |
|---|---|---|
| `0` | Day | 日内有效 |
| `1` | GTC | Good Till Cancel |
| `3` | IOC | 立即成交可成交部分，剩余取消 |
| `4` | FOK | 必须立即全部成交，否则取消 |

### 6.3 VWAP / TWAP / POV vs 子订单

- **VWAP / TWAP / POV**：上层 **execution strategy / algo**；不是固定的标准 `OrdType` 取值。
- **Limit / Market / IOC / FOK / Iceberg**：下层 child order 的委托类型、有效期或展示约束。
- Algo 由 broker-specific FIX tags 或约定的标准 strategy parameters 表达，**不能假设统一存在 `AlgoStrategy=VWAP` 这个标准 Tag**。

```text
Parent (VWAP / TWAP / POV)
            |
       Algo / SOR
       /    |     \
 Limit DAY  Limit IOC  Iceberg / venue-specific child
```

## 7. Cancel / Replace 的关键 Race

```text
Working / PartiallyFilled
     |
     +-- Send F -> Cancel requested (local intent, not final)
     |       ├─ 35=8,150=6 PendingCancel (optional)
     |       ├─ 35=8,150=4 Canceled   -> terminal for leaves
     |       ├─ 35=9 CancelReject     -> cancel failed; underlying order may still be working
     |       └─ 35=8,150=F Trade      -> fill may race with cancel
     |
     +-- Send G -> Replace requested
             ├─ 35=8,150=E PendingReplace
             ├─ 35=8,150=5 Replaced  -> new parameters active
             ├─ 35=9 CancelReject    -> old accepted parameters remain
             └─ fills / further state changes may race
```

- **Sending a cancel does not mean the order is canceled**；同理发送 Replace 不代表新价格已生效。
- `35=9` 可拒绝 Cancel 或 Replace，区分 `434=1` Cancel / `434=2` Replace，并保留 `102` / `58`。
- Cancel/Replace 可以与成交并发：只靠按发送顺序推断状态有风险；须以收到的实际事件及规范校验。
- Pending 状态和业务状态可能并存于内部 OMS 表达，避免让一个粗粒度 `status` enum 丢失 requested intent、working parameters、累计成交等信息。

## 8. TCP 断线、FIX Session Recovery、业务 Reconciliation

**典型题：发送 NewOrderSingle 后，TCP disconnect，未收到 ACK。**

存在多个可能：

1. Venue 根本没收到。
2. 收到了但未处理。
3. 已接受但 ACK 未送达。
4. 已接受并成交，但报告未送达。

所以本地状态是 **uncertain**，不能盲目另起一个新的经济等价 `35=D`。

建议处理顺序：

```text
TCP reconnect
  -> FIX Logon
  -> inspect session sequence numbers
  -> ResendRequest / replay / GapFill as appropriate
  -> recover missing ExecutionReports and CancelRejects
  -> dedupe / replay order events and reconstruct order state
  -> reconcile remaining uncertain orders
     (35=H, drop copy, venue query, journal, other agreed facilities)
  -> clear uncertainty only with authoritative evidence
```

- TCP 保证**一条连接**内部的有序字节流；FIX `34=MsgSeqNum` 提供跨重连的消息级 continuity / gap detection。
- FIX session 顺序恢复完成 **不等于** 订单业务状态一定已知；缺失业务消息、非法序列重置、交易对手重连语义等可能需要 reconciliation。
- `35=H` 查询订单时，FIX 4.4 `11` 是**要查询的订单的 ClOrdID**；`790=OrdStatusReqID` 可区分不同 status request；收到 `35=8,150=I` 后依据实际 `39` 和数量 reconciliate。
- 对于仍 uncertain 的订单，可隔离该订单/风险敞口并进行专门 reconciliation；是否允许其他订单继续处理取决于风险策略，不要求整个 engine 永远停机。
- Replay 的 session-level `PossDupFlag(43)` 与 application-level ExecID/order state 幂等是不同层次。

## 9. 面试速答（English）

**Q: Why do we need both OrdStatus and ExecType?**

> ExecType tells me what this ExecutionReport is reporting, while OrdStatus tells me the current state of the order chain. For example, a replace can be accepted with ExecType=Replaced while OrdStatus remains PartiallyFilled. I need both to update the active instruction and the order state correctly.

**Q: Does ClOrdID change on every fill? Does OrderID stay the same?**

> No. ClOrdID normally changes for a new cancel or replace instruction, not on every fill. OrderID is assigned by the broker or venue and often stays stable, but I never assume this across all venues. I keep the ClOrdID chain and the venue OrderID mapping.

**Q: Who assigns ExecID?**

> The sender of the ExecutionReport, typically the broker or venue. I use it for execution-event identification and replay deduplication, taking the counterparty's uniqueness scope and correction semantics into account.

**Q: Can I resend a NewOrderSingle after reconnect?**

> Not blindly. The order may already have been accepted or filled. I first recover the FIX session, replay reports and reconcile the order using supported status or drop-copy facilities before taking an action that could duplicate the economic exposure.

**Q: Where do Limit, Stop, Iceberg, TWAP and VWAP go?**

> Limit and Stop are OrdType semantics, Iceberg is generally a display-quantity instruction such as MaxFloor, and TWAP/VWAP are higher-level execution strategies using counterparty-specific algo parameters. IOC and FOK are TimeInForce.

## 10. References（FIX 4.4）

- [FIX Trading Community — Order State Changes](https://www.fixtrading.org/online-specification/order-state-changes/)
- [FIX Trading Community — Business Area: Trade](https://archive.fixtrading.org/online-specification/business-area-trade/)
- [FIX 4.4 NewOrderSingle (35=D)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/D)
- [FIX 4.4 OrderCancelRequest (35=F)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/F)
- [FIX 4.4 OrderCancelReplaceRequest (35=G)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/G)
- [FIX 4.4 OrderStatusRequest (35=H)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/H)
- [FIX 4.4 ExecutionReport (35=8)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/8)
- [FIX 4.4 OrderCancelReject (35=9)](https://fix.dev/organizations/fix-standards/specs/1000001/messages/9)
- [FIX 4.4 ExecType (150)](https://fix.dev/organizations/fix-standards/specs/1000001/fields/150)
- [FIX 4.4 OrdStatus (39)](https://fix.dev/organizations/fix-standards/specs/1000001/fields/39)
- [FIX 4.4 MaxFloor (111)](https://fix.dev/organizations/fix-standards/specs/1000001/fields/111)

---

相关仓库笔记：[订单状态机工程设计](./订单状态机工程设计.md) · [ExecutionAlgo](./ExecutionAlgo.md)
