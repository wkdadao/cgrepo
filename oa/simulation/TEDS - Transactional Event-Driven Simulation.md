# TEDS — Transactional Event-Driven Simulation

> C1 / OA / Simulation 专题  
> 核心流程：**Copy → Simulate (Dispatch Events) → Validate → Commit**  
> 核心目标：**先在隔离的状态副本上完成动作与连锁反应，检查结果后再决定是否提交。**

## 1. 为什么需要这个模板

复杂模拟题常见的问题不是算法本身，而是：

- 一条指令触发多项后续动作，例如玩家移动 → 推箱子 → 重力 → 计分。
- 执行过程中需要多次修改状态，但整个操作可能失败。
- 在执行前一次性推导所有合法条件和变化（`Prepare`）成本很高。
- 如果直接修改原状态，失败后回滚困难、容易遗漏。
- 事件之间的执行顺序会影响最终结果。

**TEDS = Event-Driven 的规则展开方式 + Transactional 的状态提交方式。**

这里的 Transactional 是一种描述性命名，不代表数据库事务的全部 ACID 保证；Event-Driven 也不要求使用 Priority Queue。

## 2. 一张图理解 TEDS

~~~text
execute(state, command)
    |
    +-- Copy: next = state
    |
    +-- Initialize pending events
    |      enqueue(CommandEvent)
    |
    +-- Simulate:
    |      while pending is not empty:
    |          pop event
    |          applyEvent(next, event, pending)
    |              |- local validation
    |              |- mutate next
    |              |- enqueue follow-up events
    |          on fatal failure -> Abort (discard next)
    |
    +-- Validate(next)
    |      on failure -> Abort (discard next)
    |
    +-- Commit: state = move(next)
~~~

重要：**Event 描述发生了什么；Handler 处理该事件并可能产生后续事件；Dispatcher 决定执行顺序；Transaction 决定整次模拟是否提交。**

## 3. 通用 OA 骨架

以下是可直接套用的设计骨架；`State`、`Event`、`Command`、`applyEvent`、`validate` 需要按题目实现：

~~~cpp
#include <deque>
#include <utility>

using EventQueue = std::deque<Event>;

// May modify only the temporary state and local event queue.
// Returns false only when the entire transaction must abort.
bool applyEvent(State& next, const Event& event, EventQueue& queue);

// Invariants of the completed transaction.
bool validate(const State& state);

bool execute(State& state, const Command& cmd)
{
    // 1. Copy
    State next = state;

    // 2. Initialize
    EventQueue queue;
    queue.push_back(makeEvent(cmd));

    // 3. Simulate
    while (!queue.empty())
    {
        // Pop BEFORE invoking a handler that may enqueue events.
        Event event = std::move(queue.front());
        queue.pop_front();

        if (!applyEvent(next, event, queue))
            return false;              // Abort: discard next
    }

    // 4. Validate
    if (!validate(next))
        return false;

    // 5. Commit
    state = std::move(next);
    return true;
}
~~~

### 两个必不可少的约定

1. `applyEvent(false)` 表示**整个事务失败**，不只是“某次移动被阻挡”。若题目规定撞墙是合法 no-op，不能直接 abort。
2. `applyEvent` 必须接收 `State& next`，不能意外操作原状态或其他不可回滚的外部资源。

**为什么先 pop 再调用 handler？** Handler 可能往容器插入新事件；不要持续持有队头 / `priority_queue::top()` 的引用并同时修改调度容器。先取出一个局部 `Event` 最简单、安全。

## 4. 可编译的最小 C++17 示例：单行推箱子 + 连锁 Score Event

这是演示事件队列与事务的**小型教学例子**，不是 DRW 原题。人为约定：

- 世界是一条宽度为 `width` 的线。
- 玩家每次向左或右移动一格；可推动一个箱子一格。
- 越界或推动失败会使**整个 Command 失败**（不是 no-op）。
- 成功移动后生成 `Score` 事件，成功指令数加一。
- 没有重力；若题目存在重力，必须按题目规则补充对应事件与次序。

~~~cpp
#include <cassert>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

enum class EventType { MovePlayer, Score };

struct Event {
    EventType type;
    int dx = 0;
};

struct Command {
    int dx;  // -1 or +1
};

struct State {
    int width = 8;
    int player = 1;
    int box = 3;
    int score = 0;
};

using EventQueue = std::deque<Event>;

bool validate(const State& s)
{
    return s.width > 0 &&
           0 <= s.player && s.player < s.width &&
           0 <= s.box && s.box < s.width &&
           s.player != s.box &&
           s.score >= 0;
}

bool applyEvent(State& s, const Event& e, EventQueue& queue)
{
    switch (e.type) {
    case EventType::MovePlayer: {
        if (e.dx != -1 && e.dx != 1)
            return false;

        int to = s.player + e.dx;
        if (to < 0 || to >= s.width)
            return false;

        if (to == s.box) {
            int boxTo = s.box + e.dx;
            if (boxTo < 0 || boxTo >= s.width)
                return false;

            // Move box first, then player; no invalid overlap.
            s.box = boxTo;
        }

        s.player = to;
        queue.push_back({EventType::Score, 0});
        return true;
    }

    case EventType::Score:
        ++s.score;
        return true;
    }

    return false;
}

bool runEvents(State& next, EventQueue& queue)
{
    while (!queue.empty()) {
        Event event = std::move(queue.front());
        queue.pop_front();

        if (!applyEvent(next, event, queue))
            return false;
    }
    return true;
}

bool execute(State& state, Command cmd)
{
    State next = state;  // Copy

    EventQueue queue;
    queue.push_back({EventType::MovePlayer, cmd.dx});

    if (!runEvents(next, queue))   // Simulate
        return false;

    if (!validate(next))          // Validate
        return false;

    state = std::move(next);      // Commit
    return true;
}

int main()
{
    State s;
    assert(validate(s));

    assert(execute(s, {+1})); // player 1 -> 2, box 3
    assert(s.player == 2 && s.box == 3 && s.score == 1);

    assert(execute(s, {+1})); // push: player 3, box 4
    assert(s.player == 3 && s.box == 4 && s.score == 2);

    // Push until box reaches the right boundary.
    for (int i = 0; i < 3; ++i)
        assert(execute(s, {+1}));
    // player 6, box 7, score 5
    assert(s.player == 6 && s.box == 7 && s.score == 5);

    const State before = s;
    assert(!execute(s, {+1}));
    assert(s.player == before.player);
    assert(s.box == before.box);
    assert(s.score == before.score);
}
~~~

这段代码体现了：**在 `next` 上可以发生局部变化；只要任何事件失败，就放弃整个 `next`；原状态不需要手工回滚。**

## 5. Event-Driven 不是 Priority Queue

事件驱动关注 **“一个事件会导致哪些状态变化 / 后续事件？”**，调度器只负责决定何时处理它们。

| 语义 | 最简单的实现 | 典型情形 |
|---|---|---|
| 输入指令逐一执行 | `for` 循环 | 简单移动、状态机 |
| 后续动作立即展开 | 直接调用 / 递归 | 推动连续箱子 |
| 后续事件按产生顺序处理 | `std::deque` (FIFO) | 一般连锁反应 |
| 需要后进先出 | `vector` / stack | 深度优先展开 |
| 按时间、优先级执行 | `priority_queue` | 离散事件模拟 |
| 同时刻统一更新 | 批量收集 + 统一提交 | 多物体同步运动 |

**默认选最简单的调度器，不凭空引入 t1/t2/t3。**

如果确实要求时间排序，可采用：

~~~cpp
using Key = std::pair<int64_t, uint64_t>; // <time, uniqueEventId>
std::priority_queue<Key, std::vector<Key>, std::greater<>> pq;
~~~

`eventId` 推荐使用递增序号，确保同时间事件按约定顺序确定性执行。**确定性不等于题目语义正确**：若存在阶段优先级、同一时刻同步移动，则需要额外建模，而不能假设 ID 顺序代表物理规则。使用该代码片段还需包含 `<queue>`、`<functional>`、`<cstdint>`、`<utility>`、`<vector>`。

## 6. 推箱子 + 重力：先确认事件顺序

假设一条命令可能触发：

~~~text
MovePlayer
   |
   +--> PushBox
   |      |
   |      +--> DropBox
   |
   +--> DropPlayer
   |
   +--> CheckScore
~~~

以上是**潜在因果关系**，并不表示必须按此顺序执行。不同题目可能规定：

- **A：顺序连锁**：先推箱子 → 箱子下落 → 玩家移动 → 玩家下落。
- **B：命令后统一重力**：先完成推动与玩家水平移动 → 全部物体应用重力 → 计分。
- **C：同步更新**：在同一 tick 上根据旧状态计算所有物体的下一位置，再统一提交。

三者在有相互阻挡的物体时可能产生不同结果。FIFO、递归和 PQ 都不会自动解决这个规则问题。

特别注意：若玩家推动箱子必须一起成功，**不能**先把玩家提交到箱子旧位置，再在独立的延迟事件里发现箱子不能移动。应在同一个 handler 或具有明确事务语义的移动过程里确保二者一致。

## 7. Copy → Simulate → Validate → Commit vs Prepare → Commit

| 对比 | Prepare → Commit | TEDS（Copy → Simulate → Validate → Commit） |
|---|---|---|
| 主要思路 | 先算出 `MovePlan`，再应用 | 先在副本中执行完整过程，再接受结果 |
| 事前推导 | 需要确定计划和全部必要前置条件 | 可以在模拟中逐步发现影响 |
| 连锁反应 | Plan 可能复杂 | Event Handler 组合更自然 |
| 状态隔离 | 通常不需要复制 | 需要复制 State 或等价隔离机制 |
| 回滚 | 通常没有部分修改 | 丢弃副本即可 |
| 验证 | 通常主要在 Prepare 中 | 局部合法性 + 最终不变量 |
| 成本 | 更新局部字段往往更省 | 完整 State copy 可能是 O(|State|) |
| OA 推荐 | 简单、局部动作 | 复杂、需要整体成功/失败的动作 |

**TEDS 的精华不是“方便回滚”本身，而是：可以先在 copy 上完成过程、观察完整结果，再相对容易地进行整体 validation。**

### 为什么最终状态 Validation 仍然不够？

例如玩家穿过墙后又停在空地上，最终位置看似合法，但过程已经违反“不能穿墙”。

因此要分开：

- **Local Validation**：检查动作过程中不可违反的前置条件和约束（例如碰撞、边界、路径限制）。
- **Global Validation**：完成后检查 State 的一致性（例如唯一玩家、无非法重叠、数量守恒、计数合法）。

**不应把所有合法性检查推迟到最终状态。**

## 8. 事务边界：单条 Command vs 整串 Commands

这点对“判断整串指令是否合法”的 OA 特别重要。

### 方案 A：每条指令独立提交

~~~cpp
for (const Command& cmd : commands) {
    if (!execute(state, cmd))
        return false;
}
~~~

此时如果中途失败，**之前成功的指令已经提交**。适合逐步更新世界、允许保留前缀结果的语义。

### 方案 B：整串指令作为一个事务

~~~cpp
bool executeAll(State& state, const std::vector<Command>& commands)
{
    State next = state;   // Copy only once

    for (const Command& cmd : commands) {
        EventQueue queue;
        queue.push_back(makeEvent(cmd));

        if (!runEvents(next, queue))
            return false;

        // If invariants must hold after each command:
        if (!validate(next))
            return false;
    }

    state = std::move(next);
    return true;
}
~~~

任意一步失败，都丢弃 `next`，原始 `state` 完全不变。**如果题目真正要求整串指令全部合法才接受，应考虑以整个序列为事务边界。**

如果题目只要求最终状态满足某些限制，是否每条命令后都做 `validate` 需要以规则为准；不能把最终检查和过程约束混为一谈。

## 9. 正确性不变量和常见陷阱

**Invariant / Contract 清单**

1. **状态所有权**：Handler 只修改 `next` 和事务内部事件队列；不写原始 `state`。
2. **事件语义**：Handler 的 `false` 必须表示事务 abort，合法 no-op 应有单独处理。
3. **事件顺序**：同一时间的多个事件是否有顺序、优先级或同步语义，必须明确。
4. **状态一致性**：每个 Handler 不要让后续事件误读意外的中间重叠状态。
5. **终止性**：事件不能无限自我生成；需要单调推进依据，必要时按题目约束设置 guard。
6. **Stale Event**：已排队的事件可能因其他事件改变 State 而失效；需要重新检查条件，复杂定时任务可使用 generation/version。
7. **外部副作用**：文件写入、网络发送、外部计数器等不能靠复制 `State` 回滚。
8. **Commit 失败**：`state = std::move(next)` 对特定 State 类型未必是 noexcept；若需要严格的异常安全保证，应设计 noexcept swap / 提交方式。
9. **拷贝成本**：每条命令都拷贝完整网格，可能带来 `O(commands × stateSize)` 的代价；必要时按整个序列拷贝一次、使用 Undo Log 或局部变更。
10. **终止与提交条件**：事件队列清空只说明“没有待处理事件”，不能自动证明所有业务规则已满足。

## 10. OA 决策准则

先回答三个问题：

1. **事务边界是什么？** 单条指令、一个 tick，还是整串命令？
2. **执行语义是什么？** 顺序、递归因果链、FIFO、时间排序、还是同步更新？
3. **失败定义是什么？** 阻挡是合法 no-op、事件失败、命令失败，还是整个序列失败？

随后采用最小实现：

~~~text
Simple local rules
    -> Validate -> Commit

Complicated causal reaction + atomic accept/reject
    -> TEDS (Copy -> Dispatch Events -> Validate -> Commit)

Strict simultaneous movement
    -> Compute from old state -> Batch Commit
       (can still be enclosed in a transaction)
~~~

**不要为了“事件驱动”而拆出没有实际语义的 Event；不要因为有 Queue 就假设执行顺序一定正确。**

## 11. 训练用检查表

- [ ] 是否把输入、状态、事件和事务边界分别写清楚？
- [ ] 是否画出一条命令可能生成的事件链？
- [ ] 是否明确碰撞、重力、机关、计分的先后次序？
- [ ] 是否区分“动作无法进行”和“事务失败”？
- [ ] 是否所有可能 abort 的修改都发生在 `next` 上？
- [ ] 是否区分 local validation 和 final validation？
- [ ] 是否覆盖：边界、紧贴箱子、连续推动、阻挡、非法命令、连锁事件？
- [ ] 是否有正确的事件终止条件？
- [ ] 是否检查一次失败不会污染后续状态？
- [ ] 是否在功能正确前避免过度工程化？

---

## 一句话总结

> **Event-Driven 负责展开复杂行为；Transactional Copy 负责隔离尝试；Local + Global Validation 负责保证合法性；Commit 负责一次性接受结果。**

建议在 C1 Simulation 中将此模式称为 **TEDS — Transactional Event-Driven Simulation**，将四阶段流程简称为 **CSVC — Copy → Simulate → Validate → Commit**。
