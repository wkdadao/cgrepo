# Order Entry 状态机

> 自拟 C++ OA 练习，建议限时 120 分钟，并非已确认的 DRW 原题。以下是本练习的完整规则；它借鉴订单生命周期，但不等同于完整 FIX 规范或特定交易所协议。

## 题目

实现一个单线程订单管理程序。程序接收本地下单、撤单、改单命令，以及模拟交易所回报，维护每张订单的最新状态。

程序不负责撮合，也不自行生成交易所回报。不同订单的命令和回报可以交错出现。

从标准输入逐行读取，直到 EOF。每个输入行执行完成后，再处理下一行。

## 输入约定

- 字段以一个或多个空格分隔，没有空行。
- 命令名、字段数量、整数格式保证正确；订单引用、操作时机、价格和数量可能违反业务规则。
- ID 是长度 1～32 的字母、数字或下划线字符串；`-` 仅用于输出缺省值。
- 买卖方向为 `B` 或 `S`。价格是整数 tick，数量是整数，绝对值均不超过 `10^9`。
- 输入最多 `10^5` 行。
- 程序启动时没有订单；订单终结后仍保留记录，供查询和识别重复回报。

## ID 的含义

| 字段 | 含义 |
| --- | --- |
| `orderId` | 本地逻辑订单 ID，整个生命周期保持不变 |
| `reqId` | 每次发往交易所的新单、撤单或改单请求的 ID |
| `currentClOrdId` | 初始为新单请求 ID；只有改单成功才更新为该次改单请求 ID |
| `exchangeOrderId` | 交易所在新单接受回报中分配的订单 ID，改单后保持不变 |
| `execId` | 一笔成交的 ID，其唯一性范围为同一张交易所订单 |

已成功发出请求的 `reqId`，在整个输入期间不得再次用于任何请求。局部校验失败而没有发出的请求，不占用该 ID。

已创建的 `orderId` 和已分配的 `exchangeOrderId` 均不得复用。

## 本地命令

### 新单

```text
NEW orderId reqId side price qty
```

要求 `orderId` 尚未创建、`reqId` 未被使用、`price > 0`、`qty > 0`。

接受后创建订单，初始累计成交量为零，交易所订单 ID 未知，并等待新单回报。输出：

```text
SEND D reqId - orderId side price qty
```

### 撤单

```text
CANCEL orderId reqId
```

订单必须已被交易所接受、仍有未成交数量，且没有等待中的请求。`reqId` 必须未被使用。

接受后等待撤单回报，输出：

```text
SEND F reqId currentClOrdId exchangeOrderId
```

### 改单

```text
REPLACE orderId reqId newPrice newQty
```

订单必须已被交易所接受、仍有未成交数量，且没有等待中的请求。`reqId` 必须未被使用。

要求 `newPrice > 0`、`newQty > 0`，并且 `newQty >= 当前累计成交量`。`newQty` 表示修改后的订单总量，不是新增量或剩余量。

接受后等待改单回报，输出：

```text
SEND G reqId currentClOrdId exchangeOrderId newPrice newQty
```

在收到成功回报前，当前价格、当前订单总量和 `currentClOrdId` 均不变。

### 查询

```text
QUERY orderId
```

按“查询输出”一节打印订单快照。

## 交易所回报

成功处理以下回报时，不输出任何行。

### 新单接受或拒绝

```text
NEW_ACK reqId exchangeOrderId
NEW_REJECT reqId
```

只可回应当前等待中的新单请求。

接受时分配一个尚未使用的交易所订单 ID，订单开始生效。拒绝时订单进入 `Rejected`，交易所订单 ID 仍未知。

### 成交

```text
FILL exchangeOrderId execId lastQty
```

- 一笔新成交要求订单已接受、尚未终结，且 `0 < lastQty <= 当前未成交数量`。
- 将本次成交计入该订单的累计成交量。
- 等待撤单或改单回报期间，仍可按当前已生效的订单总量成交。
- 成交全部完成，不会自动清除尚未收到回报的撤单或改单请求。
- 相同 `(exchangeOrderId, execId)` 的成功成交回报再次出现时，直接忽略，不输出、不重复计量，即使订单已经终结。
- 重复成交的内容保证与首次回报相同。非法成交不登记为已处理成交。

本练习不接受新单确认之前的成交，也不接受订单终结后的新成交。

### 撤单成功或拒绝

```text
CANCEL_ACK reqId
CANCEL_REJECT reqId
```

只可回应当前等待中的撤单请求。

- 成功时取消未成交部分，订单进入 `Canceled`；累计成交量保留，剩余量变为零。
- 如果订单已经全部成交，撤单成功回报属于非法操作，必须由撤单拒绝回报结束该请求。
- 拒绝时清除等待中的撤单请求，订单保留期间发生的全部成交。
- 两种结果均不改变 `currentClOrdId`。

### 改单成功或拒绝

```text
REPLACE_ACK reqId
REPLACE_REJECT reqId
```

只可回应当前等待中的改单请求。

- 成功时，价格变为请求中的 `newPrice`，`currentClOrdId` 变为该次 `reqId`。
- 成功时实际订单总量为“请求的 `newQty`”与“确认时累计成交量”中的较大者。期间成交可能使累计成交量超过原先申请的总量。
- 拒绝时，原价格、原订单总量、原 `currentClOrdId` 保持不变，期间成交仍然有效。
- 两种结果均清除等待中的改单请求。

本练习没有交易所主动撤单、主动改单、成交撤销、成交更正或跨日恢复。

## 查询输出

```text
ORDER orderId currentClOrdId exchangeOrderId state side price orderQty cumQty leavesQty pendingReqId
```

未知的 `exchangeOrderId` 和不存在的 `pendingReqId` 输出 `-`。

| 状态 | 对外含义 |
| --- | --- |
| `PendingNew` | 等待新单回报 |
| `PendingCancel` | 等待撤单回报，包括期间全部成交但撤单尚未收到回报的情况 |
| `PendingReplace` | 等待改单回报，包括当前生效总量已经全部成交的情况 |
| `New` | 新单已接受，没有成交，也没有等待中的请求 |
| `PartiallyFilled` | 已部分成交，仍有剩余量，没有等待中的请求 |
| `Filled` | 当前订单总量已全部成交，没有等待中的请求 |
| `Canceled` | 撤单成功 |
| `Rejected` | 新单被拒绝 |

`PendingNew` 的 `pendingReqId` 是新单请求 ID。`PendingCancel`、`PendingReplace` 的 `pendingReqId` 是相应的撤单、改单请求 ID。

`Canceled` 和 `Rejected` 的 `leavesQty` 为零；其他情况下，它等于当前 `orderQty - cumQty`。终结订单的总量和价格仍然保留。

## 非法操作

任何业务条件不满足，包括未知订单、未知请求、回报类型与等待请求不匹配、重复的已使用请求 ID、对终结订单发起撤单或改单，均输出：

```text
ERROR lineNumber
```

`lineNumber` 从 1 开始。非法操作不改变任何订单、请求或成交记录，并继续处理下一行。

重复的成功成交回报按前述规则忽略，不输出错误。除成功成交回报外，本练习不提供其他回报的重放语义；已完成请求再次收到回报属于非法操作。

除 `SEND`、`ORDER` 和 `ERROR` 外，不得向标准输出打印其他内容。EOF 时不自动输出订单快照。

## 样例一

### 输入

```text
NEW O1 N1 B 100 10
QUERY O1
NEW_ACK N1 V1
FILL V1 E1 3
CANCEL O1 C1
FILL V1 E2 2
QUERY O1
CANCEL_REJECT C1
FILL V1 E2 2
QUERY O1
CANCEL O1 C2
CANCEL_ACK C2
QUERY O1
```

### 输出

```text
SEND D N1 - O1 B 100 10
ORDER O1 N1 - PendingNew B 100 10 0 10 N1
SEND F C1 N1 V1
ORDER O1 N1 V1 PendingCancel B 100 10 5 5 C1
ORDER O1 N1 V1 PartiallyFilled B 100 10 5 5 -
SEND F C2 N1 V1
ORDER O1 N1 V1 Canceled B 100 10 5 0 -
```

## 样例二

### 输入

```text
NEW O2 N2 S 200 10
NEW_ACK N2 V2
FILL V2 E3 4
REPLACE O2 R1 201 6
FILL V2 E4 3
QUERY O2
REPLACE_ACK R1
QUERY O2
CANCEL O2 C3
```

### 输出

```text
SEND D N2 - O2 S 200 10
SEND G R1 N2 V2 201 6
ORDER O2 N2 V2 PendingReplace S 200 10 7 3 R1
ORDER O2 R1 V2 Filled S 201 7 7 0 -
ERROR 9
```

## 规范背景

订单状态和撤改单交错场景参考 [FIX Trading Community — Order State Changes](https://www.fixtrading.org/wp-content/uploads/download-manager-files/Order-State-Changes.pdf)。本文命令、输出、错误处理以及请求并发限制均为自拟练习约定。

