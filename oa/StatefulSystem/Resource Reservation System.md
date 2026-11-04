# Resource Reservation System — 多资源原子预留与容量一致性

> **C2 / Stateful System / Resource Reservation System**  
> 预计用时：**120 分钟** ｜语言：**C++17/20**｜难度：**高**  
> 训练重点：多对象一致性、事务式校验与提交、状态机、资源额度聚合、AMEND 差量计算与边界测试  
> **本文件只包含完整题目描述、样例和测试要求；不包含参考实现或解法。**

## 1. Problem Statement

请实现一个简化的 **Resource Reservation System（资源预留系统）**。

系统维护多种具有固定容量的资源，例如 CPU、MEMORY、NETWORK。客户端可以通过一条命令**同时**预留多个资源，也可以修改预留清单、确认预留、取消预留及查询资源的可用容量。

每个 Reservation 都具有唯一的 ID 和生命周期状态。所有 **RESERVED** 与 **CONFIRMED** 状态的 Reservation 都占用资源；只有取消（**CANCELED**）才会释放已占用的资源。

**核心要求：一条命令涉及的所有资源必须同时成功或同时失败。** 如果其中任何一个资源不足、不存在，或命令违反状态规则，所有资源的占用量、Reservation 记录及其状态都不得发生变化。

本题为**单线程、有状态的命令处理题**。不要求数据库、锁、真实资源分配、超时、并发交易或回滚日志；只需在内存中处理命令并按输入顺序输出结果。

## 2. 数据模型

### 2.1 Resource

每种资源具有：

| 字段 | 含义 |
|---|---|
| `id` | 资源 ID，全局唯一（仅在资源命名空间内） |
| `capacity` | 固定总容量，创建后不可修改 |
| `used` | 所有活动 Reservation 占用的资源数量 |
| `available` | 当前可用数量，`capacity - used` |

例如：

~~~text
RESOURCE CPU 10
RESOURCE MEMORY 32
RESOURCE NETWORK 4
~~~

系统创建之初没有任何资源。资源一经创建就一直存在，不能删除、重命名或修改容量。

### 2.2 Reservation

每笔预留记录包含：

| 字段 | 含义 |
|---|---|
| `id` | Reservation ID，全局唯一（仅在预留命名空间内） |
| `status` | `RESERVED` / `CONFIRMED` / `CANCELED` |
| `demands` | 从资源 ID 到**正整数**需求量的映射 |

- `RESERVED`：预留成功，但尚未确认；占用清单中的全部资源。
- `CONFIRMED`：已确认使用资源；仍然占用清单中的全部资源。
- `CANCELED`：已取消；不再占用任何资源。
- 成功创建的 Reservation ID **永久不可复用**，即使随后取消。
- **取消并不删除 Reservation 记录或原有需求清单**。因此，查询一个已取消的 Reservation 仍会返回它取消时保存的需求量；这些数量仅用于历史显示，不再计入资源占用。

Resource ID 与 Reservation ID 的命名空间相互独立：允许一个 Resource 和一个 Reservation 恰好使用同一个字符串 ID。

### 2.3 状态机

~~~text
                  AMEND (RESERVED only)
              ┌────────────────────┐
              │                    │
              ▼                    │
           RESERVED ───────────────┘
             │    \
     CONFIRM │     \ CANCEL
             ▼      ▼
         CONFIRMED  CANCELED
             |
             | CANCEL
             ▼
          CANCELED
~~~

- `AMEND` 仅允许在 `RESERVED` 状态下执行。
- `CONFIRM` 仅允许将 `RESERVED` 转成 `CONFIRMED`。
- `CANCEL` 允许取消 `RESERVED` 或 `CONFIRMED`。
- `CANCELED` 为终态，不能再次 AMEND、CONFIRM 或 CANCEL。
- `CONFIRMED` 不允许 AMEND，但可以 CANCEL。
- 本题没有自动到期，也没有将 CONFIRMED 转回 RESERVED 的操作。

## 3. 系统不变量

对于每个存在的 Resource `x`：

~~~text
0 <= used(x) <= capacity(x)
available(x) = capacity(x) - used(x)
~~~

定义：

~~~text
Active = {r | r.status == RESERVED or r.status == CONFIRMED}
demand(r, x) = r.demands[x] if x in r.demands else 0

used(x) = sum(demand(r, x) for r in Active)
~~~

同时始终满足：

1. 每一个 Reservation 的需求清单包含至少一种资源。
2. 同一份需求清单里，每种 Resource 最多出现一次，且需求数量为正整数。
3. 每个 Reservation 引用的 Resource 必须存在。
4. `RESERVED` 和 `CONFIRMED` 都计入 `used`，不因 CONFIRM 而重复扣减。
5. `CANCELED` 不计入 `used`，即使其需求清单仍保留。
6. 每个成功的变更必须让上述所有不变量继续成立。
7. 任意返回 `ERROR` 的命令不得改变任何系统状态；失败的 RESERVE 也不得占用 Reservation ID。

## 4. 支持的命令

共 **7 种命令**。每条命令恰好输出一行。

### 4.1 `RESOURCE id capacity`

创建一种固定容量的资源。

成功条件：

- Resource ID 尚不存在。
- `capacity > 0`。

成功后：

- 新增 Resource。
- `used = 0`，`available = capacity`。

成功输出 `OK`，否则输出 `ERROR`。重复 RESOURCE 不会修改之前的容量。

### 4.2 `RESERVE id k resource1 qty1 ... resourcek qtyk`

创建一笔新的资源预留，**一次性**申请完整的资源需求清单。

成功条件：

- Reservation ID 之前没有被成功创建过。
- `k >= 1`。
- 本次清单中所有 Resource ID 均已存在。
- 本次清单中没有重复 Resource ID。
- 所有需求数量 `qty > 0`。
- 每一种涉及的资源都具有足够可用容量。

成功后：

- 创建状态为 `RESERVED` 的 Reservation。
- 将其需求清单保存下来。
- 所有涉及资源的 `used` 分别增加对应需求量。

失败输出 `ERROR`，任何状态不变；失败的 Reservation ID **允许之后再次使用**。

**不允许部分预留**：即使只有最后一种资源不足，也不能先占用前几种资源。

### 4.3 `AMEND id k resource1 qty1 ... resourcek qtyk`

修改一笔已有预留的**全部目标资源清单**。这不是添加额外资源，而是**完整替换**原先的需求清单。

成功条件：

- Reservation 存在且状态为 `RESERVED`。
- `k >= 1`。
- 所有目标资源存在、没有重复，且目标需求量均为正。
- **假设本次 AMEND 全部生效后**，所有 Resource 的 `used` 仍满足 `0 <= used <= capacity`。

成功后：

- 用目标需求清单完整替换旧清单。
- 资源占用量反映新旧需求的变化。
- Reservation 仍为 `RESERVED`。

关键语义：

- 旧清单中有、目标清单中没有的资源：**完全释放该 Reservation 对该资源的占用**。
- 旧清单中没有、目标清单中有的资源：新增占用。
- 两份清单里都有的资源：按新旧需求数量的**净变化**更新占用。
- 需求数量和清单都未发生实际变化时，合法 AMEND 仍输出 `OK`。
- 不能先不可撤销地释放旧资源，再在后续检查失败时留下半修改的结果。
- 失败输出 `ERROR`，旧需求清单、Reservation 状态和**所有**资源占用完全不变。

例如当前：

~~~text
capacity(CPU) = 10, used(CPU) = 8
capacity(MEM) = 20, used(MEM) = 12

R1 = { CPU: 4, MEM: 8 }  (RESERVED)
~~~

如果执行：

~~~text
AMEND R1 2 CPU 6 MEM 6
~~~

目标占用将是：

~~~text
CPU: 8 - 4 + 6 = 10
MEM: 12 - 8 + 6 = 10
~~~

因此该 AMEND 合法。检查目标容量时，**必须考虑当前预留已经占用的资源**，不能把整个新清单当作额外占用。

### 4.4 `CONFIRM id`

确认一笔尚未确认的预留。

- 只有 `RESERVED` 可以确认。
- 确认后状态变为 `CONFIRMED`。
- **资源占用量不变**，不会再次扣除资源。
- 对不存在、已 CONFIRMED 或 CANCELED 的 Reservation 执行 CONFIRM 均失败。

成功输出 `OK`，否则输出 `ERROR`。

### 4.5 `CANCEL id`

取消一笔活动预留。

- 仅 `RESERVED` 或 `CONFIRMED` 可以取消。
- 成功后状态变为 `CANCELED`。
- 释放这笔 Reservation 对**每一种**资源的占用。
- 保留原始需求清单及 Reservation ID，以供 QUERY 使用。
- 重复 CANCEL 失败，且不重复释放资源。

成功输出 `OK`，否则输出 `ERROR`。

### 4.6 `QUERY id`

查询一笔 Reservation。

若 ID 不存在，输出 `NOT_FOUND`；否则输出：

~~~text
RESERVATION id status k resource1 qty1 ... resourcek qtyk
~~~

其中：

- `status` 为 `RESERVED` / `CONFIRMED` / `CANCELED`。
- `k` 是当前记录中需求清单的资源种类数。
- 所有资源 ID 必须按**ASCII 字典序**升序输出，与 RESERVE / AMEND 输入顺序无关。
- 对 CANCELED 状态，输出的是**取消时保留的最后需求清单**，不是当前占用；它们已经全部释放。
- QUERY 不修改系统状态。

### 4.7 `AVAILABLE resource`

查询指定 Resource 的剩余容量。

若资源不存在，输出 `NOT_FOUND`；否则输出：

~~~text
AVAILABLE resource available capacity
~~~

其中 `available = capacity - used`。

例如：

~~~text
AVAILABLE CPU 4 10
~~~

表示 CPU 总容量为 10，剩余容量为 4，当前已占用 6。

查询不修改系统状态。

## 5. 错误处理与事务边界

- `RESOURCE` / `RESERVE` / `AMEND` / `CONFIRM` / `CANCEL`：成功输出 `OK`，失败输出 `ERROR`。
- `QUERY` / `AVAILABLE`：对象存在时输出查询结果，不存在时输出 `NOT_FOUND`。
- 对不存在的 Reservation 调用 `AMEND` / `CONFIRM` / `CANCEL`，输出的是 `ERROR`，不是 `NOT_FOUND`。
- 只要有一项前置条件不满足，**整条命令**视为失败，不能更新任何 Resource 或 Reservation。
- 同一条命令中的资源处理顺序不影响最终成功、失败或结果。
- 系统应继续处理失败命令后面的所有命令。

对 AMEND，最容易犯的错误是把检查和修改交织在一起：在更新了第一个资源之后才发现第二个资源超出容量。题目要求保证整体原子性，而不规定必须使用哪种实现技术。

## 6. 输入格式

输入为**一组测试数据**：

~~~text
N
command[1]
command[2]
...
command[N]
~~~

每条命令语法：

~~~text
RESOURCE id capacity
RESERVE id k resource1 qty1 ... resourcek qtyk
AMEND id k resource1 qty1 ... resourcek qtyk
CONFIRM id
CANCEL id
QUERY id
AVAILABLE resource
~~~

- 第一行 `N` 表示操作数量，后面恰好 N 行命令。
- 参数之间由一个或多个空格分隔。
- `k = 0` 的 RESERVE / AMEND 在输入语法上是合法的，但按照业务规则应返回 `ERROR`。
- 保证命令名、参数个数、整数文本格式正确。无须处理未知命令、非整数或缺失字段。
- 输入数值可能为 0，必须依照对应的业务条件判定成功或失败。
- 每条命令都必须输出一行。

## 7. 数据范围

| 参数 | 范围 |
|---|---|
| `1 <= N <= 200,000` | 命令条数 |
| Resource ID / Reservation ID | 长度 1–32，使用 A–Z、a–z、0–9、下划线 |
| `0 <= capacity <= 10^12` | 0 为业务非法 |
| `0 <= qty <= 10^12` | 0 为业务非法 |
| `0 <= k <= 200,000` | 每条 RESERVE / AMEND 的资源条目数 |
| 所有 RESERVE / AMEND 的 k 之和 | 不超过 200,000（包含失败的命令） |
| 练习时间限制 | 建议 2 秒 |
| 练习内存限制 | 建议 256 MB |

数量与容量要使用可容纳 `10^12` 的整数类型，不要使用 32 位有符号整数。

资源和预留数量合计最多为成功创建命令的数量，因此各自均不超过 `N`。

本题不提供时间窗口、优先级、等待队列、抢占、资源超卖、部分分配或自动释放机制。

## 8. 样例

### Sample 1 — 跨资源容量检查、确认与取消

**Input**

~~~text
22
RESOURCE CPU 10
RESOURCE MEM 32
RESOURCE NET 4
RESERVE R1 2 CPU 6 MEM 16
RESERVE R2 2 CPU 4 NET 3
AVAILABLE CPU
RESERVE R3 3 CPU 1 MEM 8 NET 1
AVAILABLE MEM
AMEND R2 2 CPU 3 NET 4
AVAILABLE CPU
RESERVE R3 2 CPU 1 MEM 16
QUERY R3
CONFIRM R2
AMEND R2 1 CPU 2
CANCEL R2
AVAILABLE NET
CANCEL R2
QUERY R2
RESERVE R2 1 CPU 1
AVAILABLE CPU
CANCEL R3
AVAILABLE MEM
~~~

**Output**

~~~text
OK
OK
OK
OK
OK
AVAILABLE CPU 0 10
ERROR
AVAILABLE MEM 16 32
OK
AVAILABLE CPU 1 10
OK
RESERVATION R3 RESERVED 2 CPU 1 MEM 16
OK
ERROR
OK
AVAILABLE NET 4 4
ERROR
RESERVATION R2 CANCELED 2 CPU 3 NET 4
ERROR
AVAILABLE CPU 3 10
OK
AVAILABLE MEM 16 32
~~~

**Explanation**

- R1、R2 使 CPU 用满。R3 第一次预留因 CPU 不足失败，故 ID R3 未被占用。
- AMEND R2 将 CPU 从 4 降到 3、NET 从 3 升到 4，成功后 CPU 余量为 1。
- R3 第二次预留成功，占用 CPU 1 和 MEM 16。
- R2 确认后仍占用资源，不能再 AMEND，但可以 CANCEL。
- CANCEL R2 释放 CPU 3、NET 4；重复撤销失败，ID 不能复用。
- CANCELED R2 的 QUERY 仍保留取消前的需求清单。

### Sample 2 — AMEND 必须按目标最终占用整体校验

**Input**

~~~text
20
RESOURCE CPU 8
RESOURCE MEM 16
RESERVE A 2 CPU 5 MEM 10
RESERVE B 2 CPU 3 MEM 6
AMEND A 2 CPU 6 MEM 9
QUERY A
AMEND A 2 CPU 4 MEM 12
AVAILABLE MEM
AMEND A 2 MEM 8 CPU 4
AVAILABLE CPU
AVAILABLE MEM
AMEND A 2 CPU 4 MEM 8
AMEND B 1 CPU 2
QUERY B
AVAILABLE MEM
CONFIRM A
CONFIRM A
CANCEL A
QUERY A
AVAILABLE CPU
~~~

**Output**

~~~text
OK
OK
OK
OK
ERROR
RESERVATION A RESERVED 2 CPU 5 MEM 10
ERROR
AVAILABLE MEM 0 16
OK
AVAILABLE CPU 1 8
AVAILABLE MEM 2 16
OK
OK
RESERVATION B RESERVED 1 CPU 2
AVAILABLE MEM 8 16
OK
ERROR
OK
RESERVATION A CANCELED 2 CPU 4 MEM 8
AVAILABLE CPU 6 8
~~~

**Explanation**

- A 想增加 CPU 至 6，会导致 CPU 总使用量为 9，失败。
- 另一次将 A 的 MEM 增至 12，会导致 MEM 总使用量为 18，失败。
- 改成 CPU 4、MEM 8 后，两种资源容量均符合要求，成功；输入顺序不会影响 QUERY 输出顺序。
- B 的 AMEND 完全移除 MEM，从而释放此前占用的 MEM 6。
- 对 CONFIRMED 的 A 只能 CANCEL，不能再次 CONFIRM。
- 最后 CANCEL A 释放 CPU 4，余下只有 B 占用 CPU 2。

### Sample 3 — 不存在资源、重复条目、失败 ID 不占用

**Input**

~~~text
16
RESOURCE DISK 5
RESOURCE DISK 8
RESERVE X 1 DISK 6
QUERY X
RESERVE X 1 DISK 5
RESERVE Y 2 DISK 1 DISK 1
RESERVE Y 1 GPU 1
RESOURCE GPU 2
RESERVE Y 1 GPU 1
CONFIRM X
CANCEL X
QUERY X
RESERVE X 1 DISK 1
AVAILABLE DISK
QUERY Y
AVAILABLE UNKNOWN
~~~

**Output**

~~~text
OK
ERROR
ERROR
NOT_FOUND
OK
ERROR
ERROR
OK
OK
OK
OK
RESERVATION X CANCELED 1 DISK 5
ERROR
AVAILABLE DISK 5 5
RESERVATION Y RESERVED 1 GPU 1
NOT_FOUND
~~~

**Explanation**

- 重复创建 DISK 不会更改容量。
- 失败的 X 不占用 ID；后来可以成功创建 X。
- 同一份需求清单出现重复 DISK，必须拒绝整个 RESERVE。
- 未创建 GPU 时的 RESERVE Y 失败，但不会占用 Y。
- X 即使已 CONFIRMED，仍可以 CANCEL 并释放 DISK；其 ID 不能复用。

### Sample 4 — 完整替换资源清单与失败原子性

**Input**

~~~text
19
RESOURCE B 10
RESOURCE A 5
RESERVE R 2 B 3 A 2
QUERY R
AMEND R 3 A 1 B 8 C 1
QUERY R
RESOURCE C 1
AMEND R 3 C 1 B 8 A 1
AVAILABLE A
AVAILABLE B
AVAILABLE C
CONFIRM R
QUERY R
AMEND R 2 A 1 B 1
CANCEL R
QUERY R
AVAILABLE C
RESOURCE Z 0
RESERVE EMPTY 0
~~~

**Output**

~~~text
OK
OK
OK
RESERVATION R RESERVED 2 A 2 B 3
ERROR
RESERVATION R RESERVED 2 A 2 B 3
OK
OK
AVAILABLE A 4 5
AVAILABLE B 2 10
AVAILABLE C 0 1
OK
RESERVATION R CONFIRMED 3 A 1 B 8 C 1
ERROR
OK
RESERVATION R CANCELED 3 A 1 B 8 C 1
AVAILABLE C 1 1
ERROR
ERROR
~~~

**Explanation**

- AMEND 第一次失败，因为目标清单引用了尚不存在的 C。原有 A/B 占用不得变动。
- C 创建后，AMEND 成功：A 从 2 降到 1、B 从 3 增到 8，新增 C 1。
- CONFIRMED 不允许 AMEND；随后 CANCEL 成功释放三种资源。
- 即使没有列任何资源，`RESERVE EMPTY 0` 在语法上合法，但业务上失败。

## 9. 建议自行补充的测试（不提供答案）

至少覆盖以下情况：

1. 查询不存在的 Resource 和 Reservation。
2. RESOURCE 使用 0 容量，以及重复创建已有资源。
3. 单资源 RESERVE 恰好用满容量。
4. 预留多个资源时只有最后一个资源不足，确认任何资源都未改变。
5. 预留清单有不存在的资源或重复资源。
6. 预留清单中的某个 qty 为 0。
7. `RESERVE id 0`，然后重试相同 ID 的合法预留。
8. RESERVE 失败后使用相同 ID 再次 RESERVE 成功。
9. 已 CANCELED 的 Reservation ID 不能复用。
10. Resource 与 Reservation 使用相同字符串 ID，不应冲突。
11. AMEND 一个不存在的 Reservation。
12. AMEND 将一个资源需求调高，恰好用满容量。
13. AMEND 将一个资源需求调高，超过容量；验证所有原有字段不变。
14. AMEND 同时释放一种资源并增加另一种资源。
15. AMEND 完整替换清单，删除原有一种资源并新增另一种。
16. AMEND 目标清单与当前完全一样，必须成功。
17. AMEND 输入资源顺序不同，但目标需求内容相同。
18. AMEND 目标清单中有重复 Resource ID。
19. AMEND 目标清单中有不存在 Resource ID。
20. AMEND 目标清单有 qty=0 或 k=0。
21. AMEND 中目标 CPU 数量较小，但另一种资源不足，验证 CPU 没有被提前释放。
22. 对 CONFIRMED / CANCELED 的 Reservation 执行 AMEND。
23. CONFIRM 后资源 used 不应变化。
24. 重复 CONFIRM 失败，状态和资源都不变。
25. 对 RESERVED 与 CONFIRMED 分别 CANCEL。
26. 重复 CANCEL 失败，不能重复释放资源。
27. QUERY CANCELED：保留最后的需求清单，但 AVAILABLE 显示该预留已完全释放。
28. QUERY 输出资源 ID 必须按 ASCII 字典序排序。
29. 多个 Reservation 占用相同资源，某笔取消不影响其他预留。
30. 容量和数量接近 10^12 时，避免整数溢出或错误比较。
31. 超过十万次交错的 RESERVE / AMEND / CANCEL / QUERY / AVAILABLE。
32. 一个预留包含大量资源，考察性能与完整原子性。

## 10. OA 训练要求（非题目规则）

在编码前建议先明确：

- **Entities**：Resource 和 Reservation 有哪些字段？哪个实体持有原始需求，哪个维护聚合占用？
- **Invariants**：所有活动 Reservation 的需求总和如何等于 Resource.used？
- **Transaction boundary**：一条涉及多个资源的 RESERVE / AMEND / CANCEL 是否整体提交？
- **AMEND delta**：目标需求清单的净变化如何计算？尤其是移除某一种资源的情况。
- **Lifecycle**：RESERVED、CONFIRMED、CANCELED 在额度计算上有什么区别？
- **Error atomicity**：如果最后一个资源验证失败，前面资源的占用能否保持不变？
- **Representation**：如何避免每次操作扫描所有历史 Reservation？
- **Testing**：怎样设计失败之后的 QUERY / AVAILABLE 来验证状态完全未变化？

**建议时间分配（120 分钟）：**

| 阶段 | 时间 |
|---|---|
| 读题、提炼状态机及 Invariants | 0–20 分钟 |
| 实现 RESOURCE、RESERVE、QUERY、AVAILABLE | 20–50 分钟 |
| 实现 AMEND、CONFIRM、CANCEL | 50–95 分钟 |
| 跑样例、边界测试与复杂度检查 | 95–120 分钟 |

**策略：先实现全部命令的正确性，再考虑优化。** 本题不要求多线程、锁、数据库、通用事务框架；简洁的状态结构和清晰的操作边界更适合 OA。

## 11. 提交要求

提交一个可以从标准输入读取**一组测试数据**的 C++17/20 程序：

- 每条输入命令恰好对应一行输出。
- 输出严格使用指定格式，不得输出额外日志或诊断信息。
- 对非法操作返回 `ERROR`，不修改任何状态。
- 所有查询均不得修改状态。
- 失败后仍需继续处理后续命令。
- 必须考虑大输入、长需求清单和资源额度边界。

---

**核心考点**：在有状态系统里维护**多个独立资源的聚合容量约束**，实现跨多个对象的全有或全无更新，正确处理 AMEND 的全量目标语义、状态生命周期和失败原子性。