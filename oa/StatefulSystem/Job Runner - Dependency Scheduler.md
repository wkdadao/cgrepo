# Job Runner / Dependency Scheduler — 有状态任务依赖调度系统

> **C2 / Stateful System / Job Runner & Dependency Scheduler**  
> 建议用时：**120 分钟** ｜语言：**C++17/20**｜难度：**高**  
> 训练重点：状态机、依赖图、任务就绪传播、失败级联、非法操作的原子性与复杂度控制  
> **本文件仅提供完整题目描述、样例和测试要求；不包含参考代码或解法。**

## 1. Problem Statement

实现一个简化的 **Job Runner（任务执行管理器）**，维护多项任务之间的依赖关系，以及任务在执行过程中的状态。

每个 Job 拥有一个全局唯一 ID，可以依赖零个或多个其他 Job。一项任务必须等其**所有直接依赖任务成功完成**后才能开始执行。任务也可能执行失败；一旦任务失败，所有直接或间接依赖它的、尚未执行成功的任务都无法执行。

系统按输入顺序处理命令，包括添加任务、启动调度、查看就绪任务、运行任务、结束任务，以及查询状态。

**你不需要真正执行 Job**：`RUN`、`COMPLETE` 与 `FAIL` 都是外部输入的状态转换指令，不涉及线程、异步事件或运行耗时。

### 1.1 为什么有 `START` 命令？

允许在添加任务时引用**尚未创建的 Job（forward reference）**。这样能表达任意依赖图，但同时必须处理不存在的依赖和循环依赖。

因此系统分为两个阶段：

1. **BUILD（构建阶段）**：通过 `ADD` 注册任务及其依赖。
2. **ACTIVE（运行阶段）**：执行一次成功的 `START`，验证整个任务图有效之后，才能通过其余调度命令运行任务。

`START` 是阶段切换命令；它不启动任何具体 Job。

## 2. Job 状态与生命周期

每个 Job 在 ACTIVE 阶段恰好处于以下六种状态之一：

| 状态 | 含义 |
|---|---|
| `PENDING` | 尚有至少一个直接依赖未成功完成，且没有失败的依赖祖先 |
| `READY` | 所有直接依赖均为 `SUCCESS`，可以运行 |
| `RUNNING` | 已运行，尚未被报告完成或失败 |
| `SUCCESS` | 已成功完成，终态 |
| `FAILED` | 任务自身执行失败，终态 |
| `BLOCKED` | 某个直接或间接依赖任务已失败，无法运行，终态 |

状态转换：

~~~text
                         all dependencies SUCCESS
PENDING ------------------------------------------------> READY
  |                                                        |
  |  dependency failure                                    | RUN
  v                                                        v
BLOCKED                                                   RUNNING
                                                           /   \
                                              COMPLETE    /     \   FAIL
                                                         v       v
                                                      SUCCESS   FAILED
                                                                    |
                                                                    v
                                              recursively BLOCK downstream
~~~

特别注意：

- `SUCCESS`、`FAILED`、`BLOCKED` 都是终态，不允许重新运行、重试或恢复。
- `READY` 的 Job **尚未运行**。
- `PENDING` 在其最后一个未完成依赖成功时转换为 `READY`。
- 失败产生的 `BLOCKED` **必须立即向下游递归传播**，不能等到下游查询时才临时决定。
- 某一任务的失败不影响与其**不存在依赖路径**的其他任务。
- ACTIVE 阶段不允许再添加或修改依赖关系。

## 3. 依赖图与合法性

如果 `ADD A 2 B C`，则表示 Job A 依赖 Job B 和 Job C；A 只能在 B 和 C **均成功**后运行。

- 依赖关系是有向的。
- 允许先添加 A，再添加 B、C。
- 一个 Job 可以同时被多个下游 Job 依赖。
- 同一 Job 的依赖列表中不得重复出现相同 ID。
- Job 不允许直接依赖自己：例如 `ADD X 1 X` 是非法命令。
- 不允许存在循环依赖，包括长度为 2 或更长的环。
- **构建期间只需拒绝直接自依赖和重复依赖；全局缺失依赖与循环检测统一在 `START` 时执行。**
- 图中每个被引用的依赖在成功 `START` 时都必须已经通过 `ADD` 注册。
- 图不满足条件时，`START` 失败，系统仍处于 BUILD 阶段；不改变已有的任务定义。

例如：

~~~text
ADD A 1 B     # A -> B
ADD B 1 C     # B -> C
ADD C 1 A     # C -> A
START         # ERROR: cyclic dependency
~~~

上面的 `#` 注释只用于解释，真实输入没有行内注释。

由于本题不提供删除或修改任务定义的命令，一旦添加了无法消除的循环，该测试用例之后的 `START` 都无法成功；程序仍必须继续正确处理余下命令。

## 4. 支持的命令

共有 **7 种命令**。每条命令都输出恰好一行。

### 4.1 `ADD id k dep1 dep2 ... depk`

在 BUILD 阶段添加一个 Job，并登记它的直接依赖。

- `k` 为非负整数，`k = 0` 表示无依赖，此时命令仅有 `ADD id 0`。
- Job ID 全局唯一。已成功添加的 Job 不可重复添加。
- 不允许 Job 直接依赖自身。
- 依赖列表内不得出现重复 ID。
- 依赖 ID 暂时不存在是允许的；留待 `START` 验证。
- 在 ACTIVE 阶段执行 `ADD` 一律失败。

成功输出 `OK`；否则输出 `ERROR`。

**失败的 ADD 不创建 Job，不占用 ID，不修改任何其他状态。**

### 4.2 `START`

仅在 BUILD 阶段允许执行，用于一次性验证依赖图并进入 ACTIVE 阶段。

成功条件：

1. 至少有一个成功添加的 Job。
2. 每个被引用的依赖 ID 都存在。
3. 整个图无有向环。

成功后：

- 系统切换到 ACTIVE。
- 没有依赖的 Job 初始化为 `READY`。
- 至少有一个依赖的 Job 初始化为 `PENDING`。
- 不会自动把任何 Job 变成 `RUNNING`。

成功输出 `OK`。若不满足条件，输出 `ERROR`，保持 BUILD 阶段及已有任务定义不变。

已经成功 `START` 的系统不能再次执行 `START`；此时输出 `ERROR`。

### 4.3 `READY`

在 ACTIVE 阶段查询当前所有处于 `READY` 状态的 Job。

输出格式：

~~~text
READY k id1 id2 ... idk
~~~

- `k` 为当前 READY Job 的数量。
- 所有 ID 必须按**区分大小写的字典序（ASCII lexicographical order）升序**输出。
- 没有 READY Job 时，严格输出 `READY 0`。
- `RUNNING`、`PENDING`、`SUCCESS`、`FAILED`、`BLOCKED` Job 都不会出现在结果中。
- 查询不改变系统状态。

在 BUILD 阶段使用该命令输出 `ERROR`。

### 4.4 `RUN id`

在 ACTIVE 阶段启动一个 Job。

- 只有当前处于 `READY` 的 Job 才能执行该命令。
- 成功后将其变为 `RUNNING`，从 READY 集合中移除。
- 可以同时有多个 RUNNING Job；**没有执行槽位数量限制**。
- 对不存在的 Job、尚未 READY 的 Job、已 RUNNING 或已终止的 Job 执行该命令均失败。

成功输出 `OK`，失败输出 `ERROR`。

### 4.5 `COMPLETE id`

将一项正在运行的 Job 标记为成功完成。

- 仅 `RUNNING` Job 可执行。
- 成功后其状态变为 `SUCCESS`。
- 此任务的完成，可能使一个或多个直接下游任务满足全部依赖，从 `PENDING` 变为 `READY`。
- 已进入 READY 的任务不会自动运行。
- 对不存在、未运行或已终止任务执行 COMPLETE，输出 `ERROR`，不产生部分更新。

成功输出 `OK`，失败输出 `ERROR`。

注意：`COMPLETE` 不会自动完成下游任务；它只解除依赖约束。

### 4.6 `FAIL id`

报告一项 RUNNING Job 执行失败。

- 仅 `RUNNING` Job 可执行。
- 成功后此 Job 状态变为 `FAILED`。
- 所有直接或间接依赖它的 Job 立即变为 `BLOCKED`。
- 被 BLOCKED 的 Job 不可再运行，即使其其他依赖后来都成功。
- 与该 Job 无依赖路径的任务状态保持不变。
- 不存在或非 RUNNING Job 执行 FAIL 一律返回 `ERROR`，且不得影响任何下游 Job。

成功输出 `OK`，失败输出 `ERROR`。

对于合法 ACTIVE 图，一个失败任务的下游不可能已经进入 RUNNING 或 SUCCESS（它们此前必须等该任务成功），因此失败级联只会影响未运行的下游任务。已 BLOCKED 的任务保持 BLOCKED。

### 4.7 `STATUS id`

在 ACTIVE 阶段查询一个 Job 的当前状态。

成功输出：

~~~text
JOB id state
~~~

其中 `state` 为 `PENDING` / `READY` / `RUNNING` / `SUCCESS` / `FAILED` / `BLOCKED` 中的一种。

- ACTIVE 阶段查询不存在的 ID 输出 `NOT_FOUND`。
- BUILD 阶段调用 STATUS，**无论 ID 是否已经添加，都输出 `ERROR`**。
- 查询不改变状态。

## 5. 原子性与错误规则

所有命令依次执行；一个失败命令应当视为完全没有执行过：

- 不能产生部分状态转移。
- 不能提前占用新 Job ID。
- 不能部分修改依赖图。
- 失败的 `START` 不能把任何任务初始化为 ACTIVE 状态。
- 无论某个命令是否失败，都要继续处理后续命令。

**仅 ACTIVE 阶段的 `STATUS` 对不存在对象输出 `NOT_FOUND`；其他所有非法操作均输出 `ERROR`。**

| 命令 | 成功输出 | 失败输出 |
|---|---|---|
| `ADD` | `OK` | `ERROR` |
| `START` | `OK` | `ERROR` |
| `READY` | `READY k ...` | `ERROR`（BUILD 阶段） |
| `RUN` | `OK` | `ERROR` |
| `COMPLETE` | `OK` | `ERROR` |
| `FAIL` | `OK` | `ERROR` |
| `STATUS` | `JOB id state` | `NOT_FOUND`（ACTIVE & 不存在），否则 `ERROR` |

## 6. 输入格式

标准输入为**一组测试数据**：

~~~text
N
command[1]
command[2]
...
command[N]
~~~

其中命令的语法是：

~~~text
ADD id k [dep1 dep2 ... depk]
START
READY
RUN id
COMPLETE id
FAIL id
STATUS id
~~~

- `[dep1 ... depk]` 表示仅当 `k > 0` 时才出现的恰好 k 个 ID；方括号不是输入的一部分。
- 参数由空格隔开，允许多余空格。
- 保证每行命令名称有效、参数数量及类型符合语法。
- 不需要处理未知指令、不完整输入或负的 k。
- 命令语法合法不代表业务操作合法，例如重复 Job ID 仍需输出 ERROR。
- 不保证一定会成功执行 START；也不保证任何 Job 会成功完成。

## 7. 数据范围

| 参数 | 约束 |
|---|---|
| `1 <= N <= 200,000` | 输入命令总数 |
| Job ID | 长度 1–32，字符为 A–Z、a–z、0–9、下划线 |
| `0 <= k <= 200,000` | 每次 ADD 声明的依赖数 |
| 所有 ADD 的 k 之和 | 不超过 200,000（包含失败的 ADD） |
| 成功 ADD 的 Job 总数 | 不超过 200,000 |
| 练习建议时间限制 | 2 秒 |
| 练习建议内存限制 | 256 MB |

注意：

- 任务依赖链可能达到 200,000 层，不应假设递归深度很小。
- 可能有多个任务同时 READY，输出顺序必须稳定且符合字典序。
- `READY` 命令可能输出大量 ID；输出总量不受仅 `N` 约束，性能应与实际输出量相适应。
- 所有输入均为单线程命令序列；无需实现并发安全性或任务工作线程。

## 8. 样例

### Sample 1 — 正常依赖解锁与调度顺序

**Input**

~~~text
24
ADD parse 0
ADD build 1 parse
ADD test 1 build
ADD docs 1 parse
ADD deploy 2 test docs
START
READY
RUN parse
COMPLETE parse
READY
RUN build
RUN docs
COMPLETE build
READY
COMPLETE docs
READY
RUN test
COMPLETE test
READY
STATUS deploy
RUN deploy
COMPLETE deploy
READY
STATUS deploy
~~~

**Output**

~~~text
OK
OK
OK
OK
OK
OK
READY 1 parse
OK
OK
READY 2 build docs
OK
OK
OK
READY 0
OK
READY 1 test
OK
OK
READY 1 deploy
JOB deploy READY
OK
OK
READY 0
JOB deploy SUCCESS
~~~

**Explanation**

- `parse` 不依赖其他 Job，START 后直接 READY。
- `parse` 成功后，`build` 和 `docs` 同时 READY，按字典序输出。
- `build` 成功后，`test` READY 的条件满足，但 `docs` 仍处于 RUNNING，故 `deploy` 不 READY。
- `docs` 和 `test` 都成功后，`deploy` 才能运行。
- 只查看 READY 列表不会启动任何任务。

### Sample 2 — 失败级联与无关任务继续执行

**Input**

~~~text
22
ADD A 0
ADD B 0
ADD C 1 A
ADD D 2 C B
ADD E 1 D
ADD F 1 B
START
READY
RUN A
FAIL A
STATUS C
STATUS D
STATUS E
READY
RUN B
COMPLETE B
READY
RUN F
COMPLETE F
STATUS F
RUN C
STATUS A
~~~

**Output**

~~~text
OK
OK
OK
OK
OK
OK
OK
READY 2 A B
OK
OK
JOB C BLOCKED
JOB D BLOCKED
JOB E BLOCKED
READY 1 B
OK
OK
READY 1 F
OK
OK
JOB F SUCCESS
ERROR
JOB A FAILED
~~~

**Explanation**

- A 失败使 C、D、E 立即 BLOCKED，其中 D 还依赖 B，但 B 的后续成功不能解除 BLOCKED。
- B 本身与 A 没有依赖关系，仍然可以执行。
- F 只依赖 B，因此 B 完成后 F 变为 READY，最终可成功。
- `RUN C` 失败，不能将 BLOCKED 的任务重新启动。

### Sample 3 — Forward Reference 与 START 失败后的修复

**Input**

~~~text
12
ADD high 1 low
START
ADD low 0
START
READY
RUN high
RUN low
COMPLETE low
READY
STATUS high
ADD extra 0
START
~~~

**Output**

~~~text
OK
ERROR
OK
OK
READY 1 low
ERROR
OK
OK
READY 1 high
JOB high READY
ERROR
ERROR
~~~

**Explanation**

- 第一次 START 失败，因为 `low` 尚不存在；原有 `high` 定义不丢失。
- 后续添加 `low` 后可以成功 START。
- ACTIVE 阶段无法继续 ADD，也不能重复 START。

### Sample 4 — 循环依赖

**Input**

~~~text
6
ADD X 1 Y
ADD Y 1 Z
ADD Z 1 X
START
READY
STATUS X
~~~

**Output**

~~~text
OK
OK
OK
ERROR
ERROR
ERROR
~~~

**Explanation**

三个 ADD 分别合法，因为允许前向引用；START 发现 `X -> Y -> Z -> X` 的环，失败并保持 BUILD 状态。

### Sample 5 — 重复依赖、自依赖与非法状态转换

**Input**

~~~text
13
ADD A 1 A
ADD B 2 A A
ADD A 0
ADD B 1 A
START
READY
RUN A
RUN A
COMPLETE B
COMPLETE A
FAIL A
READY
STATUS B
~~~

**Output**

~~~text
ERROR
ERROR
OK
OK
OK
READY 1 A
OK
ERROR
ERROR
OK
ERROR
READY 1 B
JOB B READY
~~~

**Explanation**

- 直接自依赖、重复依赖在 ADD 时拒绝，且失败不占用 ID。
- 已 RUNNING 的任务不可重复 RUN。
- `COMPLETE B` 在 B 处于 PENDING 时失败。
- 已 SUCCESS 的 A 不能 FAIL。

## 9. 建议自行补充的测试

下面只给出测试目标，不给出结果；请自行推导预期输出并验证：

1. 未添加任何 Job 就执行 START。
2. BUILD 阶段执行 READY / RUN / COMPLETE / FAIL / STATUS。
3. 单个 Job，无依赖，完成全部生命周期。
4. 单个 Job，无依赖，执行失败。
5. 同一个 ID 重复 ADD，确保原定义未改变。
6. 同一 ADD 命令中出现重复依赖，确保该 Job 未被添加。
7. 自依赖的 ADD 失败后，使用同一 ID 正常创建。
8. 前向引用缺失导致 START 失败，补充缺失 Job 后再次 START 成功。
9. 依赖环长度分别为 2、3 和更大。
10. 有一个合法的独立子图，同时存在另一个有环子图；START 必须失败。
11. 一个 Job 同时依赖多个 Job，只有最后一个依赖 SUCCESS 才 READY。
12. 多个 Job 依赖同一个上游，成功完成时同时变为 READY。
13. READY 输出的 ID 包含大小写、数字、下划线，验证 ASCII 字典序。
14. 多个 READY Job 被任意顺序 RUN；READY 不能再列出 RUNNING Job。
15. RUN 同一个 Job 两次。
16. COMPLETE 或 FAIL 一个尚未 RUN 的 Job。
17. COMPLETE 或 FAIL 一个不存在的 ID。
18. SUCCESS Job 再 RUN / COMPLETE / FAIL。
19. FAILED Job 再 RUN / COMPLETE / FAIL。
20. BLOCKED Job 再 RUN / COMPLETE / FAIL。
21. 一项任务失败后，下游多条路径汇合到同一个 Job，确保不会重复处理导致状态异常。
22. 下游 Job 还有其他未完成依赖，但一个依赖失败时必须立即 BLOCKED。
23. 两条互不依赖的任务链，只有其中一条失败，另一条应继续执行。
24. 很长的依赖链（例如 200,000 个 Job），触发深层失败级联。
25. 高扇出、高扇入图，验证完成传播与 READY 查询性能。
26. 大量 READY 和 STATUS 交错；每次查询不得修改状态。
27. START 成功后执行 ADD 或重复 START。
28. 从未执行 START 的输入结束时，所有此前命令仍有规定的输出。

## 10. OA 训练要求（非题目规则）

建议编码前先简要回答：

- **Graph direction**：`A depends on B` 的有向边如何理解？如何定位 A 的下游？
- **State vs derived state**：Job 状态与已满足依赖数之间有什么关系？
- **Ready condition**：什么事件会将 PENDING 变成 READY？
- **Failure propagation**：FAILED 与 BLOCKED 如何区分？传播何时结束？
- **Atomicity**：START 检测失败时，哪些已有内容必须原样保留？
- **Invalid operation**：重复 RUN、未 RUN 先 COMPLETE、对终态操作如何处理？
- **Complexity**：如何避免每次完成 Job 都遍历整个任务集合？

**建议时间分配（120 分钟）：**

| 阶段 | 时间 |
|---|---|
| 阅读题意、状态定义与边界条件 | 0–20 分钟 |
| 建立 Job / 依赖模型，实现 ADD 与 START | 20–50 分钟 |
| 实现 READY / RUN / COMPLETE / FAIL / STATUS | 50–95 分钟 |
| 样例验证、边界测试、复杂度检查 | 95–120 分钟 |

**先实现正确的完整版本，再优化。** 无需使用真实线程池、协程、数据库或通用工作流框架。

## 11. 提交要求

提交一个可在 C++17 / C++20 下编译的程序：

- 从标准输入读取一组测试。
- 对每条命令恰好输出一行规定结果。
- 不得输出调试日志、额外文本或空行。
- 命令失败后必须继续处理后续输入。
- 需要能应对大规模任务与依赖链，而不仅是样例中的小图。
- 允许本地通过断言、测试程序和 `stderr` 调试；标准输出必须严格符合格式。

---

**核心考点**：从任务依赖图中维护有状态生命周期，区分依赖成功与失败两种传播方向，保证跨多个 Job 的状态一致性，并使每条命令的行为可验证、可预测。