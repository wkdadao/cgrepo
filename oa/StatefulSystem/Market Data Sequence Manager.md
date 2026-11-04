# Market Data Sequence Manager — Sequence Gap、Replay 与 Snapshot Recovery

> **C2 / Stateful System / Market Data Sequence Manager**  
> 建议用时：**120 分钟** ｜语言：**C++17/20**｜难度：**高**  
> 训练重点：序列连续性、乱序缓冲、Replay、Snapshot 恢复、数据新鲜度、状态一致性与操作原子性  
> **本文件仅包含完整题目描述、样例和测试要求，不包含参考代码或解法。**

## 1. Problem Statement

实现一个简化的 **Market Data Sequence Manager**。

交易所行情通过一个逻辑 Feed 按序列号发送。每条增量消息代表对一个 Symbol 的当前报价进行**绝对值覆盖更新**。网络传输可能导致增量消息重复、乱序到达或丢失；系统必须保证只有 **Sequence 连续**的增量消息，才能按正确顺序应用到本地 Market State。

系统支持：

1. 接收实时增量消息（`UPDATE`）。
2. 接收缺失序列的补发消息（`REPLAY`）。
3. 用权威快照恢复本地行情状态（`SNAPSHOT`）。
4. 查询 Feed 的同步状态（`STATUS`）。
5. 查询当前已确认的行情（`QUERY`）。
6. 查询尚未接收到的序列区间（`GAPS`）。

所有命令顺序处理，每条命令输出一行。本题不需要真正建立网络连接，也不需要实现交易所、Level 2/3 Order Book、双 Feed 仲裁或线程。

### 1.1 简化的行情模型

本题每个 Symbol 最多保存一条报价：`price` 与 `qty`。

- 当 `qty > 0` 时，更新操作将该 Symbol 的报价**替换**为新 `(price, qty)`。
- 当 `qty == 0` 时，更新操作删除这个 Symbol 的报价；即使 Symbol 原先不存在，也视为合法的删除操作。
- `price` 必须为正整数，单位为价格 Tick。
- 行情更新是**绝对值赋值**，**不是增量加减**。
- 同一 Symbol 可以跨多个 Sequence 被重复更新，最终由**已应用的最高 Sequence** 决定当前值。

## 2. 初始状态与全局不变量

系统启动时：

~~~text
appliedSeq = 0
highSeq = 0
buffer = empty
market = empty
feedState = SYNCED
~~~

- `appliedSeq`：已**按顺序成功应用**的最大 Sequence。
- `buffer`：已收到、尚不能应用的乱序消息，按 Sequence 唯一保存。
- `highSeq`：当前最大已知 Sequence，严格定义为：

~~~text
highSeq = max(appliedSeq, all sequence numbers in buffer)
~~~

这里 `max` 包含 `appliedSeq`，即缓冲区为空时 `highSeq = appliedSeq`。

### 2.1 必须满足的不变量

1. 所有已应用的增量消息都属于连续的、已验证的前缀，不允许跳过缺失 Sequence 应用后续数据。
2. 缓冲区中的每个 `seq > appliedSeq`。
3. 缓冲区中每个 Sequence 最多保存**一条**消息。
4. 如果缓冲区非空，则它的最小 Sequence **必然大于** `appliedSeq + 1`，否则应继续应用并清空当前可用的连续前缀。
5. `highSeq >= appliedSeq`。
6. `SYNCED` 时缓冲区为空；缓冲区非空时状态必为 `GAPPED` 或 `RECOVERING`。
7. 任何业务错误必须完全不改变系统状态，包括行情、缓冲区、Sequence、状态标记。

**重要：`appliedSeq` 只表示已提交的连续进度，不是最新收到的 Sequence。**

### 2.2 为什么初始 Sequence 是 0？

本题假设新 Feed 的第一个预期增量 Sequence 是 **1**。

如果希望从 Sequence 100 开始恢复，可以先接收 `SNAPSHOT 99 ...`，将已经验证的基线设置为 99，再按序处理 100、101、102……。

因此，系统不能因为首次看到 Sequence 100，就自动假设 1–99 都已经收到。

## 3. Feed 状态

Feed 有三个可见状态：

| 状态 | 语义 |
|---|---|
| `SYNCED` | 当前没有等待中的乱序增量；已知数据全部连续应用 |
| `GAPPED` | 存在缺失 Sequence，尚未接受当前缺口的恢复消息 |
| `RECOVERING` | 缺口仍存在，但已经接受过当前缺口的有效 Replay，或进行过有效 Snapshot 恢复 |

**RECOVERING 不是额外的后台线程，也没有定时器。** 它只是一个可观察状态，表明当前未解决的 Gap 已经进入过恢复过程。

状态规则：

- 一旦接受高于 `appliedSeq + 1` 的新增量而无法直接应用，就进入 `GAPPED`。
- 如果此前处于 `GAPPED`，且某条 `REPLAY` 成功补入缺失 Sequence，但仍存在 Gap，则变成 `RECOVERING`。
- 如果有效 `SNAPSHOT` 之后仍留有 Gap，也变成 `RECOVERING`。
- 后续普通 `UPDATE` 不会清除已经进入的 `RECOVERING` 标记。
- 当缺口完全补齐、缓冲区清空时，状态立即恢复 `SYNCED`，并重置恢复标记。
- 新一轮独立 Gap 出现时，重新从 `GAPPED` 开始。
- 无效或被忽略的消息不会改变 Feed 状态。

本题没有自动发起 Replay Request 的命令；`REPLAY` 是外部已经取得的补发消息。

## 4. 六种命令及详细语义

### 4.1 `UPDATE seq symbol price qty`

接收一条普通实时增量消息。

先做字段合法性验证，再按以下顺序处理：

1. 如果 `seq <= appliedSeq`：这是旧消息或已应用消息，输出 `IGNORED`，不修改系统。
2. 如果 `seq` 已在缓冲区：
   - 若 `symbol`、`price`、`qty` 都与已缓存内容相同，输出 `IGNORED`。
   - 否则代表同一 Sequence 的消息内容冲突，输出 `ERROR`，状态完全不变。
3. 如果 `seq == appliedSeq + 1`：
   - 将消息应用到行情。
   - 更新 `appliedSeq`。
   - 只要缓冲区里有下一个连续 Sequence，就按 Sequence 顺序继续应用并移除它。
   - 输出 `OK`。
4. 如果 `seq > appliedSeq + 1`：
   - 将该条消息放入乱序缓冲区，**不更新行情**。
   - 更新 `highSeq`。
   - 输出 `BUFFERED`。

**`OK` 不代表仅应用了一条消息**：一次 UPDATE 可能触发若干缓存消息连续提交。

旧消息即使内容不同也输出 `IGNORED`，因为本题**不保存已提交的历史消息内容**；但当前缓冲区中同一 Sequence 的不同内容必须报冲突。

### 4.2 `REPLAY seq symbol price qty`

收到一条用于填补当前 Gap 的历史补发消息。

先做字段合法性验证。处理顺序：

1. 如果 `seq <= appliedSeq`，输出 `IGNORED`。
2. 如果 `seq` 已缓存：
   - 内容完全相同：`IGNORED`。
   - 内容冲突：`ERROR`。
3. 其余情况，仅当同时满足以下条件才能接受：
   - 当前存在 Gap（缓冲区非空）；
   - `appliedSeq < seq <= highSeq`；
   - 该 Sequence 在当前缓冲区中尚不存在。

不符合这些条件输出 `ERROR`，状态不变。特别是：**Replay 不能引入比已知高水位更大的新 Sequence**；普通新数据应使用 UPDATE。

若 Replay 被接受：

- 如果 `seq == appliedSeq + 1`，像 UPDATE 一样应用该条消息，并持续应用缓存中紧随其后的连续消息，输出 `OK`。
- 否则先缓存该 Replay，输出 `BUFFERED`。
- 如果仍存在 Gap，Feed 状态为 `RECOVERING`；否则为 `SYNCED`。

注意：`REPLAY` **不因曾经有缺口就自动应用**，必须遵守连续顺序。

### 4.3 `SNAPSHOT seq k symbol1 price1 qty1 ... symbolk pricek qtyk`

收到一份权威**完整行情快照**，它描述了交易所在 **Sequence = seq 完成之后**的所有存活报价。

该快照可以替代原有行情状态，跨过缺失的历史增量。

成功条件：

- `seq >= appliedSeq`，不接受回退到已提交进度之前的旧快照。
- `0 <= k`，Symbol 名称在本次 Snapshot 内不能重复。
- 每个快照条目的 `price > 0` 且 `qty > 0`。
- 快照字段全部满足 `7. 数据范围`。

**原子性：先完整验证快照，再修改任何系统状态。任何校验失败输出 `ERROR`，并保持原有行情、缓存、进度、状态不变。**

成功后按以下严格顺序处理：

1. 使用本次快照里的 `k` 个条目，**完整替换**原行情；之前未出现在快照中的 Symbol 不再存在。
2. 将 `appliedSeq` 设为 `seq`。
3. 丢弃缓存中所有 `seq <= snapshot seq` 的消息。
4. 保留缓存中 `seq > snapshot seq` 的消息（内容不变）。
5. 从 `appliedSeq + 1` 开始，按 Sequence 顺序应用缓冲区里现在可以连续提交的消息，直到遇到下一个缺口。
6. 重新计算 `highSeq`。
7. 若缓冲区已清空，状态为 `SYNCED`；否则为 `RECOVERING`。

成功输出 `OK`。

**注意：快照并不一定能完全恢复同步。** 如果在快照点之后仍存在未补齐的 Gap，Feed 必须继续处于 RECOVERING；不能跳过后续缺口。

### 4.4 `STATUS`

查询 Feed 状态，输出：

~~~text
STATUS state appliedSeq highSeq bufferedCount
~~~

例如：

~~~text
STATUS GAPPED 101 104 1
~~~

这里 `bufferedCount` 是已收到但未提交的**独立 Sequence 个数**，不是缺失 Sequence 数量。

查询不修改任何状态。

### 4.5 `QUERY symbol`

查询**已提交行情状态**，绝不能从乱序缓冲区读取未提交的报价。

- 若该 Symbol 当前不存在于已提交行情中：输出 `NOT_FOUND`。
- 若存在：输出

~~~text
QUOTE symbol price qty freshness
~~~

`freshness` 的取值：

| 值 | 含义 |
|---|---|
| `LIVE` | 当前 Feed 是 `SYNCED`，未检测到未解决的缺口 |
| `STALE` | 当前 Feed 是 `GAPPED` 或 `RECOVERING`，行情可能落后于真实交易所状态 |

**LIVE 仅代表序列连续性，不保证真实世界中的低延迟或交易所无延迟。** `STALE` 数据仍可以查询，但必须清楚标记为可能过期。

`NOT_FOUND` 也仅表示**在当前已提交进度上的已知状态中**没有该 Symbol；若 Feed 存在 Gap，不可据此断言交易所当前也不存在它。

### 4.6 `GAPS`

查询当前仍然缺失的 Sequence 范围。

取闭区间 `[appliedSeq+1, highSeq]`，排除缓冲区中已有的 Sequence，剩余整数按连续段压缩为不重叠的最大闭区间，按起点升序输出：

~~~text
GAPS k start1 end1 start2 end2 ... startk endk
~~~

- `k` 是缺失区间的数量，**不是缺失消息总数**。
- 即使某段只缺一个 Sequence，也输出 `start end`，两者相同。
- 没有缺失时输出 `GAPS 0`。
- 绝不能逐个枚举从 1 到 10^12 的所有 Sequence；序列号可能非常稀疏。
- 查询不改变任何状态。

## 5. 命令输出摘要

| 命令 | 输出 |
|---|---|
| `UPDATE` | `OK` / `BUFFERED` / `IGNORED` / `ERROR` |
| `REPLAY` | `OK` / `BUFFERED` / `IGNORED` / `ERROR` |
| `SNAPSHOT` | `OK` / `ERROR` |
| `STATUS` | `STATUS state appliedSeq highSeq bufferedCount` |
| `QUERY` | `QUOTE symbol price qty LIVE/STALE` / `NOT_FOUND` |
| `GAPS` | `GAPS k start1 end1 ...` |

对 `UPDATE` 和 `REPLAY`，`IGNORED` 属于**成功处理但不改变状态**，并不是 `ERROR`。

## 6. 输入格式

标准输入只有**一组测试数据**：

~~~text
N
command[1]
command[2]
...
command[N]
~~~

每一条命令语法如下：

~~~text
UPDATE seq symbol price qty
REPLAY seq symbol price qty
SNAPSHOT seq k [symbol1 price1 qty1 ... symbolk pricek qtyk]
STATUS
QUERY symbol
GAPS
~~~

- 第一行 `N` 表示命令数，之后恰好 N 行。
- `k = 0` 时，Snapshot 不包含报价条目，例如 `SNAPSHOT 99 0`。
- 方括号是语法说明，不属于真实输入。
- 参数间用空格分隔。
- 保证输入行的语法、参数个数和数字格式正确；但数值可能违反业务规则，程序仍需返回 `ERROR`。
- 不需要处理未知命令、负数、缺失参数或非数字字段。
- 每条命令必须恰好输出一行，**失败命令之后仍需继续执行**。

## 7. 数据范围

| 参数 | 约束 |
|---|---|
| `1 <= N <= 200,000` | 命令数量 |
| `1 <= seq <= 10^12` | UPDATE / REPLAY 的 Sequence |
| `0 <= seq <= 10^12` | SNAPSHOT 的基线 Sequence |
| Symbol | 长度 1–32；仅包含大小写字母、数字与下划线 |
| `0 <= price <= 10^9` | UPDATE / REPLAY / SNAPSHOT 输入；0 视为业务错误 |
| `0 <= qty <= 10^9` | 增量 qty 为 0 表示删除，快照 qty 必须大于 0 |
| `0 <= k <= 200,000` | 单次 Snapshot 报价数量 |
| 所有 Snapshot 的 k 之和 | 不超过 200,000 |
| 建议时间限制 | 2 秒 |
| 建议内存限制 | 256 MB |

- 序列号必须使用**足够宽的整数类型**。
- 快照可以跳到远大于当前高水位的 Sequence。
- 允许同一 Sequence 的重复或冲突消息在输入中出现。
- 本题不涉及 Sequence 回绕（wrap-around），也不设缓冲区容量上限或超时。

## 8. 样例

### Sample 1 — 乱序缓存与 Replay 完整修复 Gap

**Input**

~~~text
16
SNAPSHOT 99 1 AAPL 100 10
STATUS
UPDATE 100 AAPL 101 9
UPDATE 101 MSFT 200 5
UPDATE 104 AAPL 104 4
STATUS
QUERY AAPL
GAPS
UPDATE 103 MSFT 203 6
GAPS
REPLAY 102 AAPL 102 7
STATUS
QUERY AAPL
QUERY MSFT
GAPS
REPLAY 103 MSFT 203 6
~~~

**Output**

~~~text
OK
STATUS SYNCED 99 99 0
OK
OK
BUFFERED
STATUS GAPPED 101 104 1
QUOTE AAPL 101 9 STALE
GAPS 1 102 103
BUFFERED
GAPS 1 102 102
OK
STATUS SYNCED 104 104 0
QUOTE AAPL 104 4 LIVE
QUOTE MSFT 203 6 LIVE
GAPS 0
IGNORED
~~~

**Explanation**

- 收到 104 时，102–103 缺失，所以 104 只能缓存。
- 103 虽已到达，但缺少 102，不能提前应用。
- REPLAY 102 后顺序应用 102、103、104。
- 已应用的 103 再次以 REPLAY 到达，输出 IGNORED。

### Sample 2 — 多个 Gap、RECOVERING 状态与 Replay 高水位限制

**Input**

~~~text
15
SNAPSHOT 10 0
UPDATE 14 A 140 2
UPDATE 12 A 120 2
STATUS
GAPS
REPLAY 13 A 130 3
STATUS
REPLAY 11 A 110 5
QUERY A
STATUS
REPLAY 15 A 150 1
UPDATE 14 A 140 2
UPDATE 16 A 160 4
REPLAY 15 A 150 3
STATUS
~~~

**Output**

~~~text
OK
BUFFERED
BUFFERED
STATUS GAPPED 10 14 2
GAPS 2 11 11 13 13
BUFFERED
STATUS RECOVERING 10 14 3
OK
QUOTE A 140 2 LIVE
STATUS SYNCED 14 14 0
ERROR
IGNORED
BUFFERED
OK
STATUS SYNCED 16 16 0
~~~

**Explanation**

- 只补入 13 时仍缺 11，不能应用任何缓存消息，状态转成 RECOVERING。
- 11 到达后依次应用 11、12、13、14，最终 A 的价格是 140。
- Feed 已同步时，`REPLAY 15` 不可用于引入新高水位；普通 `UPDATE 16` 可以触发新的 Gap。
- 新 Gap 由 Replay 15 补齐后再次同步。

### Sample 3 — Snapshot 覆盖历史状态，但后面仍然存在 Gap

**Input**

~~~text
17
SNAPSHOT 50 2 A 100 10 B 200 20
UPDATE 53 A 103 9
UPDATE 55 B 205 0
STATUS
GAPS
SNAPSHOT 52 1 C 300 4
STATUS
QUERY C
QUERY A
QUERY B
GAPS
REPLAY 54 C 302 8
STATUS
QUERY C
QUERY A
QUERY B
GAPS
~~~

**Output**

~~~text
OK
BUFFERED
BUFFERED
STATUS GAPPED 50 55 2
GAPS 2 51 52 54 54
OK
STATUS RECOVERING 53 55 1
QUOTE C 300 4 STALE
QUOTE A 103 9 STALE
NOT_FOUND
GAPS 1 54 54
OK
STATUS SYNCED 55 55 0
QUOTE C 302 8 LIVE
QUOTE A 103 9 LIVE
NOT_FOUND
GAPS 0
~~~

**Explanation**

- 快照 Sequence 52 替换之前行情，旧 A/B 被清除，只剩 C。
- 已缓存的 Sequence 53 在快照之后立刻可以应用，使 A 重新出现。
- Sequence 55 仍需等待 54，因此状态仍是 RECOVERING。
- Replay 54 后可以继续应用 55；55 的 qty=0 删除 B（尽管 B 在新快照之后本就不存在）。
- 最终已同步行情是 A 和 C。

### Sample 4 — 重复、冲突、无效快照的原子性与删除操作

**Input**

~~~text
16
SNAPSHOT 5 1 AAA 100 3
UPDATE 8 AAA 108 3
UPDATE 8 AAA 108 3
UPDATE 8 AAA 109 3
SNAPSHOT 7 2 AAA 107 5 AAA 110 1
STATUS
QUERY AAA
SNAPSHOT 7 1 BBB 70 2
QUERY AAA
QUERY BBB
GAPS
SNAPSHOT 4 0
STATUS
UPDATE 9 AAA 108 0
QUERY AAA
STATUS
~~~

**Output**

~~~text
OK
BUFFERED
IGNORED
ERROR
ERROR
STATUS GAPPED 5 8 1
QUOTE AAA 100 3 STALE
OK
QUOTE AAA 108 3 LIVE
QUOTE BBB 70 2 LIVE
GAPS 0
ERROR
STATUS SYNCED 8 8 0
OK
NOT_FOUND
STATUS SYNCED 9 9 0
~~~

**Explanation**

- 重复但完全相同的缓存消息被 IGNORED；同 Sequence 不同内容是 ERROR。
- 有重复 Symbol 的 Snapshot 必须整体失败，不可部分修改市场状态。
- 合法 Snapshot 7 把市场替换成 BBB，并紧接着应用缓存中的 8，因此 AAA 被重新创建。
- 旧 Snapshot 4 不能使 `appliedSeq` 回退。
- 数量为 0 的增量删除 AAA。

## 9. 建议自行补充的测试（不提供答案）

至少覆盖以下情况：

1. 刚启动时执行 STATUS、QUERY 和 GAPS。
2. 没有快照，依次 UPDATE 1、2、3。
3. 首次 UPDATE 就收到 100；不应自动视为 SYNCED。
4. 先 Snapshot 99，再 UPDATE 100。
5. 重复 UPDATE 已提交 Sequence，即使报价不同也 IGNORED。
6. 相同 Sequence 的缓存重复（内容相同 / 不同）。
7. Gap 范围只缺一个 Sequence。
8. 多段互不相邻的 Gap。
9. Sequence 相差很大，例如 1 与 10^12，GAPS 不能逐项枚举。
10. Replay 位于缺口的中间，尚无法连续提交，转为 RECOVERING。
11. Replay 恰好填补最早缺口，引发多个缓存消息连续应用。
12. 无 Gap 时使用 Replay 送入新的 Sequence，必须 ERROR。
13. Replay 的 Sequence 大于 highSeq，必须 ERROR。
14. Replay 已经应用或已经缓冲的消息。
15. Replay 重复缓存但内容冲突。
16. 普通 UPDATE（非 REPLAY）补齐 Gap，随后应恢复 SYNCED。
17. RECOVERING 期间又收到更大的 UPDATE，状态不能错误重置。
18. Snapshot 恰好覆盖所有缓存 Sequence。
19. Snapshot 只跳过部分缺口，后面仍然有缺口。
20. Snapshot 跳到超过 highSeq 的位置。
21. Snapshot 的 Sequence 小于 appliedSeq，必须 ERROR。
22. Snapshot 的 Sequence 恰好等于 appliedSeq，允许替换行情。
23. Snapshot 重复 Symbol，确保任何状态均不改变。
24. Snapshot 的 price=0 或 qty=0，必须 ERROR。
25. 空 Snapshot 清空所有报价。
26. Snapshot 删除旧 Symbol，但后续缓存的增量将其重新创建。
27. 乱序消息更新同一 Symbol，必须按 Sequence 顺序得到最终价格。
28. qty=0 删除已有 Symbol / 删除不存在 Symbol。
29. QUOTE 在 GAPPED / RECOVERING 下输出 STALE，SYNCED 下输出 LIVE。
30. Gap 时查询不存在的 Symbol，不要误读为交易所当前也不存在。
31. UPDATE/REPLAY price=0、Snapshot 业务失败后继续处理后续命令。
32. 大量连续 UPDATE，缓冲区不应无界积累。
33. 大量乱序 UPDATE，最后一条补齐最早 Gap，触发批量提交。
34. 200,000 条命令与极端稀疏 Sequence，检验时间和空间复杂度。
35. 多次 Snapshot、Replay、UPDATE 交错，检查 highSeq 的一致性。

## 10. OA 训练要求（非题目规则）

建议动手前先回答以下问题：

- **State / Invariant**：已提交进度、最高已知序列、乱序缓存与市场状态分别代表什么？
- **Contiguous commit**：何时允许将一条增量真正应用到 Market State？
- **Out-of-order buffering**：为什么缓存数据不能直接被 QUERY 看到？
- **Idempotency / Conflict**：历史重复与未提交缓存冲突应如何区分？
- **Recovery**：Replay 与 Snapshot 在覆盖范围和状态替换方面有哪些不同？
- **Failure Atomicity**：非法 Snapshot 为什么不能替换部分行情？
- **Complexity**：如何以与已缓存消息数或输出长度相关的复杂度报告 GAPS，而不扫描整段 Sequence？
- **Testing**：哪些用例最容易暴露忘记 drain、错误应用顺序、Snapshot 边界和陈旧状态错误？

**建议时间分配（120 分钟）：**

| 阶段 | 时间 |
|---|---|
| 阅读、提炼规则、不变量、选择数据结构 | 0–20 分钟 |
| UPDATE / STATUS / QUERY / 连续应用与乱序缓冲 | 20–55 分钟 |
| REPLAY / GAPS / SNAPSHOT | 55–100 分钟 |
| 样例、极端边界、错误操作与性能验证 | 100–120 分钟 |

优先确保所有命令都能完整正确执行，再考虑优化。**不要为了模拟真实交易所而引入线程、UDP、多 Feed Arbitration 或完整 Order Book。**

## 11. 提交要求

提交可独立编译的 C++17/20 程序：

- 从标准输入读取一组测试数据。
- 每条命令恰好输出一行。
- 输出严格符合格式，不能有调试日志或额外说明。
- 任意 ERROR 不得改变系统状态；IGNORED 也不得改变系统状态。
- 失败后仍继续处理后续命令。
- 必须可以处理较大的 Sequence，以及大量乱序消息和 Snapshot 条目。

---

**核心考点**：正确区分消息“已收到”和“已应用”，在连续序列不变量约束下处理乱序缓存、恢复与快照替换，并避免未确认数据污染可查询的行情状态。