# Order Management & Risk Engine — 订单生命周期与账户风险额度

> **C2 / Stateful System / Order Management & Risk Engine**  
> 预计用时：**90–120 分钟** ｜ 语言：**C++17/20** ｜ 难度：**中等偏上**  
> 训练重点：状态机、跨对象一致性、聚合额度维护、操作原子性、边界测试  
> **本文件仅包含完整题目描述、样例与测试要求，不包含参考实现或解法。**

## 1. Problem Statement

请实现一个简化的 **Order Management & Risk Engine**。

系统维护多个账户（Account）和订单（Order）。每个账户有一个预先设定的风险额度；创建或修改订单时，必须保证该账户所有**尚未完成的订单**所占用的数量之和不超过账户额度。

系统接收连续的命令，依次执行；每执行一条命令，就输出一行结果。命令包括创建账户、创建订单、成交、修改订单、撤单，以及查询订单和账户额度。

这是一道**单线程、有状态的在线命令处理题**，不是模拟真实交易所或 FIX 协议。重点是保证每一步操作之后的数据正确且一致。

**特别注意：账户额度约束的是所有 OPEN 订单的剩余数量之和，不是订单笔数，也不是累计成交数量。**

## 2. 数据模型与状态

### 2.1 Account

每个账户包含：

| 字段 | 含义 |
|---|---|
| `account` | 账户名，全局唯一 |
| `limit` | 固定风险额度（正整数） |
| `exposure` | 当前占用额度，初始为 0 |

`limit` 创建后不变。一个账户允许拥有多个订单。

### 2.2 Order

每个订单包含：

| 字段 | 含义 |
|---|---|
| `id` | 订单 ID，全局唯一 |
| `account` | 所属账户，创建后不可改变 |
| `totalQty` | 当前订单总数量 |
| `filledQty` | 历史累计成交数量 |
| `status` | `OPEN` / `FILLED` / `CANCELED` |

订单创建时：

- `totalQty = qty`。
- `filledQty = 0`。
- `status = OPEN`。

订单的**当前开放数量**（`openQty`）定义为：

~~~text
if status == OPEN:
    openQty = totalQty - filledQty
else:
    openQty = 0
~~~

因此，已撤销订单即使满足 `totalQty > filledQty`，其 `openQty` 仍然是 0。

### 2.3 状态转换

~~~text
                  partial FILL / AMEND
                 ┌───────────────────┐
                 │                   │
                 ▼                   │
                OPEN ────────────────┘
                 │  \
      full FILL  │   \ CANCEL
                 ▼    ▼
               FILLED  CANCELED
~~~

- `OPEN` 为活动状态，可成交、修改或撤销。
- 当一次成交使 `filledQty == totalQty`，订单转为 `FILLED`。
- `CANCEL` 使 OPEN 订单转为 `CANCELED`。
- `FILLED` 和 `CANCELED` 都是**终态**；不得重新激活。
- 终态订单继续存在于系统中，可以查询；其 ID **永久不可复用**。

## 3. 系统不变量（必须始终满足）

对每个存在的订单：

1. `totalQty > 0`。
2. `0 <= filledQty <= totalQty`。
3. 若 `status == OPEN`，则 `filledQty < totalQty`，且 `openQty = totalQty - filledQty > 0`。
4. 若 `status == FILLED`，则 `filledQty == totalQty`，且 `openQty = 0`。
5. 若 `status == CANCELED`，则 `openQty = 0`；允许 `filledQty < totalQty`。
6. 每个订单引用的账户必须存在。

对每个存在的账户 `a`：

~~~text
exposure(a) =
    sum(openQty(o) for all orders o where o.account == a)
~~~

并且：

~~~text
0 <= exposure(a) <= limit(a)
~~~

**原子性要求：任意命令一旦失败，系统状态必须与该命令执行前完全相同。**

这包括订单状态、总量、累计成交量、账户额度，以及订单 ID 是否被占用。失败命令不应影响后续命令。

## 4. 支持的命令

命令和参数均区分大小写，格式如下。

### 4.1 `ACCOUNT account limit`

创建新账户。

**成功条件：**

- 账户名此前不存在。
- `limit > 0`。

成功后账户额度为 `limit`，当前占用额度为 0。

- 成功输出：`OK`。
- 否则输出：`ERROR`，且不修改系统。

### 4.2 `NEW id account qty`

在指定账户下创建一个新订单。

**成功条件：**

- `id` 在整个系统中从未被成功使用。
- `account` 已存在。
- `qty > 0`。
- 新订单所需的风险额度不会超过账户剩余额度。

成功后订单处于 `OPEN` 状态，`totalQty=qty`、`filledQty=0`，账户的 `exposure` 增加 `qty`。

- 成功输出：`OK`。
- 否则输出：`ERROR`。
- **失败的 NEW 不会占用 ID**；之后可以用同一 ID 再试。
- 已被成功创建、后来 FILLED 或 CANCELED 的 ID 不能复用。

### 4.3 `FILL id qty`

对已有订单执行一次成交。

**成功条件：**

- 订单存在，且状态为 `OPEN`。
- `qty > 0`。
- `qty <= totalQty - filledQty`。

成功后：

- `filledQty` 增加 `qty`。
- 所属账户的 `exposure` 减少 `qty`。
- 如果累计成交量达到总数量，订单变为 `FILLED`；否则仍为 `OPEN`。

- 成功输出：`OK`。
- 否则输出：`ERROR`，没有部分成交。

### 4.4 `AMEND id newTotal`

修改已有订单的**总数量**，而非剩余数量。

**成功条件：**

- 订单存在，且状态为 `OPEN`。
- `newTotal > filledQty`。
- 修改后的账户风险占用不能超过 `limit`。

成功后：

- `totalQty = newTotal`，`filledQty` 保持不变，`status` 仍为 `OPEN`。
- 账户额度占用按修改前后**开放数量的差值**变化。

例如：总量为 10、已成交 3 的订单，`AMEND id 8` 后开放数量是 5，而不是 8。

- 允许调大、调小，也允许修改成原值（如果所有条件满足）。
- 不允许修改成恰好等于已成交数量；完全成交只能通过 `FILL` 进入 `FILLED`。
- 成功输出：`OK`。
- 否则输出：`ERROR`，任何字段都不能变化。

### 4.5 `CANCEL id`

撤销一笔活动订单。

**成功条件：**

- 订单存在。
- 订单状态为 `OPEN`。

成功后：

- 状态变为 `CANCELED`。
- 原有的 `totalQty` 和 `filledQty` 保持不变。
- 释放该订单剩余的全部风险占用。
- 此后该订单的 `openQty` 为 0。

- 成功输出：`OK`。
- 否则输出：`ERROR`。
- 重复撤单、撤销已完全成交的订单均失败。

### 4.6 `QUERY id`

查询订单。

- 如果订单不存在，输出 `NOT_FOUND`。
- 如果存在，输出：

~~~text
ORDER id status totalQty filledQty openQty
~~~

其中 `status` 严格使用 `OPEN`、`FILLED` 或 `CANCELED`。

例如，已撤销的部分成交订单可能输出：

~~~text
ORDER O1 CANCELED 10 3 0
~~~

查询不改变系统状态。

### 4.7 `RISK account`

查询账户风险额度。

- 如果账户不存在，输出 `NOT_FOUND`。
- 如果存在，输出：

~~~text
ACCOUNT account exposure limit
~~~

查询不改变系统状态。

## 5. 输入格式

标准输入包含**一组测试数据**：

~~~text
N
command[1]
command[2]
...
command[N]
~~~

- 第一行整数 `N` 表示命令数。
- 后续每行恰好一条上述七种命令。
- 参数间用一个或多个空格分隔。
- 保证命令名称、参数数量以及参数类型符合语法。
- 不要求处理未知命令、格式错误、缺失参数或非整数字符串。
- 但仍须按题意处理合法整数数值带来的**业务失败**（例如 0、超额度、已存在 ID）。

## 6. 输出格式

**每条输入命令恰好输出一行**，依输入顺序输出。

| 命令 | 成功 | 失败 |
|---|---|---|
| `ACCOUNT` | `OK` | `ERROR` |
| `NEW` | `OK` | `ERROR` |
| `FILL` | `OK` | `ERROR` |
| `AMEND` | `OK` | `ERROR` |
| `CANCEL` | `OK` | `ERROR` |
| `QUERY` | `ORDER id status totalQty filledQty openQty` | `NOT_FOUND` |
| `RISK` | `ACCOUNT account exposure limit` | `NOT_FOUND` |

注意：对不存在订单执行 `FILL` / `AMEND` / `CANCEL` 时输出 `ERROR`，**不是** `NOT_FOUND`。`NOT_FOUND` 仅用于两个查询命令。

不要输出额外的说明、诊断日志或中间状态。

## 7. 数据范围

| 参数 | 范围 / 约束 |
|---|---|
| `1 <= N <= 200,000` | 命令数量 |
| 账户名 / 订单 ID | 长度 1–32；由大小写英文字母、数字和下划线组成 |
| `limit` | `0 <= limit <= 10^12`；0 在语法上合法但会业务失败 |
| `qty`、`newTotal` | `0 <= value <= 10^12` |
| 单个账户的 `exposure` | 必须始终不超过该账户 `limit` |
| 语言 | C++17 / C++20 |
| 练习时间限制 | 2 秒（建议） |
| 练习内存限制 | 256 MB（建议） |

所有数量、额度及涉及的计算都应使用能够安全容纳以上范围的整数类型；不要使用 32 位有符号整数存储数量。

本题不含并发操作、数据库、网络 I/O、手续费、价格、订单撮合、持久化或账户间转账。

## 8. 样例

### Sample 1 — 超额度、释放额度、修改订单、完全成交

**Input**

~~~text
12
ACCOUNT A 10
NEW O1 A 6
NEW O2 A 5
NEW O2 A 4
FILL O1 2
AMEND O2 7
CANCEL O1
AMEND O2 7
FILL O2 7
QUERY O2
RISK A
CANCEL O2
~~~

**Output**

~~~text
OK
OK
ERROR
OK
OK
ERROR
OK
OK
OK
ORDER O2 FILLED 7 7 0
ACCOUNT A 0 10
ERROR
~~~

**Explanation**

1. 账户 A 的额度为 10。
2. O1 使用 6；尝试新增 O2 数量 5 会超额，因此失败。
3. 失败的 NEW 不占用 O2；再次创建 O2 数量 4 成功，占用达到 10。
4. O1 成交 2 后，占用从 10 降到 8。
5. O2 调为总量 7 会增加占用 3，达到 11，因此 AMEND 失败。
6. 撤销 O1 释放剩余数量 4；O2 再次调为 7 成功。
7. O2 完全成交，额度占用降为 0；对 FILLED 订单再撤单失败。

### Sample 2 — 部分成交、AMEND 缩量、多个订单共享额度

**Input**

~~~text
13
ACCOUNT ALPHA 12
NEW A ALPHA 9
FILL A 3
QUERY A
AMEND A 8
RISK ALPHA
NEW B ALPHA 7
AMEND A 10
CANCEL B
FILL A 5
RISK ALPHA
QUERY B
FILL B 1
~~~

**Output**

~~~text
OK
OK
OK
ORDER A OPEN 9 3 6
OK
ACCOUNT ALPHA 5 12
OK
ERROR
OK
OK
ACCOUNT ALPHA 0 12
ORDER B CANCELED 7 0 0
ERROR
~~~

**Explanation**

- A 成交 3 后，占用 6。
- 将 A 总量从 9 调为 8 后，占用 5。
- 新建 B 数量 7，账户刚好达到额度 12。
- 此时把 A 总量增至 10 会使占用达到 14，因此失败。
- 撤销 B 后只剩 A 占用的 5；A 再成交 5，全部成交，占用为 0。

### Sample 3 — ID 唯一性、非法操作与查询不存在对象

**Input**

~~~text
11
ACCOUNT X 3
NEW O1 X 2
NEW O1 X 1
FILL O1 3
CANCEL O1
NEW O1 X 1
QUERY O1
QUERY O9
RISK Y
FILL O9 1
RISK X
~~~

**Output**

~~~text
OK
OK
ERROR
ERROR
OK
ERROR
ORDER O1 CANCELED 2 0 0
NOT_FOUND
NOT_FOUND
ERROR
ACCOUNT X 0 3
~~~

**Explanation**

- 重复创建 O1 失败。
- 超过订单开放数量的成交失败，不得部分执行。
- 撤单后 O1 仍存在，因此不能复用 ID。
- 查询不存在对象输出 `NOT_FOUND`；对不存在对象执行修改类命令输出 `ERROR`。

## 9. 需要自行补充的测试（不提供预期输出）

至少检查以下情形：

1. 只有一个账户、一个订单；恰好用满额度。
2. 创建重复账户；确认原账户额度不变。
3. 对不存在账户执行 NEW；之后创建该账户，再次用相同订单 ID 执行 NEW。
4. NEW 数量为 0，随后用同一 ID 创建正数量订单。
5. NEW 失败之后，使用相同 ID 创建成功订单。
6. PARTIAL FILL 多次累加；最后一次恰好成交剩余数量。
7. FILL 数量大于剩余数量；之后查询，验证没有部分成交。
8. FILL 数量为 0。
9. AMEND 使总量小于已成交数量；应该失败。
10. AMEND 使总量**等于**已成交数量；应该失败。
11. AMEND 减少总量；随后的 NEW 恰好使用释放的额度。
12. AMEND 增大总量，恰好达到风险上限。
13. AMEND 增大总量，超过风险上限；验证原总量及额度均不变。
14. AMEND 为当前总量；这是有效操作。
15. 已部分成交订单 CANCEL：保留总量和累计成交量，开放数量变为 0。
16. 对 FILLED / CANCELED 订单分别尝试 FILL、AMEND、CANCEL。
17. 订单完全成交或撤销后，ID 不可复用。
18. 多个账户同名订单 ID 冲突（订单 ID 是全局唯一，不是每账户唯一）。
19. 两个账户的额度互相独立。
20. 混合上十万次 NEW、FILL、AMEND、CANCEL、QUERY、RISK；检查性能及状态一致性。
21. 使用接近 `10^12` 的数量，检查整数类型与边界比较。

## 10. OA 训练要求（非题目规则）

建议在动手之前，用 10–20 分钟完成以下检查：

- **Entities**：系统有哪些对象？每个对象的身份是什么？
- **State & Derived State**：哪些字段是原始状态？哪些是派生数据或聚合数据？
- **Transitions**：订单状态可以如何变化？哪些状态不可再操作？
- **Invariants**：单个订单、单个账户以及订单到账户的关系必须满足什么约束？
- **Atomicity**：一个操作涉及订单和账户两个对象，失败时如何避免只更新其中一个？
- **Boundary Cases**：完全成交、部分成交、撤销、重复 ID、超额度、非法缩量的边界在哪里？

**推荐的 120 分钟分配：**

| 阶段 | 时间 |
|---|---|
| 阅读、建模、列出不变量 | 0–20 分钟 |
| P0：ACCOUNT / NEW / QUERY / RISK | 20–50 分钟 |
| P1：FILL / AMEND / CANCEL | 50–90 分钟 |
| 测试、修复、检查复杂度 | 90–120 分钟 |

建议采用**先通过所有功能，再优化**的策略。无需为了工程美感拆出多层框架；只要数据结构清晰、状态转换一致、边界可测试即可。

## 11. 提交要求

提交一个可独立编译运行的 C++17 或 C++20 程序：

- 从标准输入读取一组测试数据。
- 对每条命令恰好输出一行规定结果。
- 不输出解释文本、调试信息或额外空行。
- 允许在本地使用断言、单元测试和标准错误输出调试。
- 不要求网络服务、并发安全、持久化、FIX 消息或图形界面。
- **不得跳过失败命令后的剩余命令**；必须继续处理。

---

**核心考点**：在连续命令驱动的有状态系统中，正确维护订单生命周期、账户聚合额度与跨对象不变量，确保每个失败操作没有副作用，并用边界测试发现状态不一致问题。