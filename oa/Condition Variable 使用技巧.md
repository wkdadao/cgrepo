# C++ Condition Variable：典型模式、正确性与 OA 实战

> 原则：**State first, Notification second. Correctness before optimization.**
>
> 面向 2 小时 C++ OA：优先使用简单、可证明正确的 mutex + condition_variable 方案。

## 1. 基本模型

`std::condition_variable` 是等待与唤醒机制，**业务状态不由 CV 保存**。Predicate 和其依赖的共享状态才是同步协议。

```cpp
std::mutex m;
std::condition_variable cv;
bool ready = false;

// Consumer
std::unique_lock lk(m);
cv.wait(lk, [&] { return ready; });
// 返回时 lk 仍持锁，ready 已在锁内重新检查

// Producer（另一个线程）
{
    std::lock_guard lk(m);
    ready = true;
}
cv.notify_one();
```

`wait(lk, pred)` 概念上等价于：

```cpp
while (!pred())
    cv.wait(lk);
```

`wait` 先原子地释放 mutex 并进入等待阶段，被通知（或者虚假唤醒）后重新争取 mutex，**获得 mutex 后**才返回。Predicate overload 会继续判断，失败则再次等待。

- `notify_one()` 自身**不要求持锁**，且不把 mutex 直接交给被唤醒线程。
- 通常先持锁更新共享状态、解锁，再 notify，以减少竞争；锁内 notify 也正确。
- `notify_one` 不保证公平性或指定哪个等待者。
- CV 不保存历史通知，因此必须将条件持久化在共享状态。
- 当同一个 CV 关联多个 mutex 供并发等待时，属于不合法用法；同一组等待者须使用同一 mutex。

## 2. Lost Wakeup：Atomic 不等于正确的 CV 协议

错误示例：

```cpp
std::atomic<bool> ready = false;
// Producer: ready.store(true); cv.notify_one();
// Consumer: cv.wait(lk, [&]{ return ready.load(); });
```

如果 Producer 不使用 Consumer 的 mutex，可能发生：

1. Consumer 持锁检查 `ready == false`；
2. Producer 设置 `ready = true` 并 notify（此时 Consumer 尚未等待）；
3. Consumer 调用 `wait`，没有后续通知，可能无限等待。

**即使没有 data race，依然可能 lost wakeup。** 在标准的 CV 设计中，检查 Predicate 与修改其共享状态由同一 mutex 协调。不要把 Atomic 当作省掉这把 mutex 的理由。

## 3. Notify one vs all

| 场景 | 通常选择 |
| --- | --- |
| Queue 新增一条任务 | `notify_one()` |
| Semaphore 增加一个许可 | `notify_one()` |
| Shutdown / close | `notify_all()` |
| Barrier 或全局状态改变 | `notify_all()` |
| 不同 Predicate 混用一个 CV | 优先拆分 CV，或仔细论证 `notify_all()` |

通知过多通常损失性能；通知不足可能破坏活性。多个 Waiter 不能假设被唤醒者立即获得锁。

## 4. Blocking Queue：支持 Drain Shutdown

```cpp
template<class T>
class BlockingQueue {
public:
    bool push(T v) {
        {
            std::lock_guard lk(m_);
            if (closed_) return false;
            q_.push(std::move(v));
        }
        cv_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&]{ return closed_ || !q_.empty(); });
        if (q_.empty()) return std::nullopt; // closed and drained
        T v = std::move(q_.front());
        q_.pop();
        return v;
    }

    void close() {
        {
            std::lock_guard lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<T> q_;
    bool closed_ = false;
};
```

此处 `close()` 拒绝新任务，Consumer 仍能 drain 剩余队列。示例侧重同步逻辑；如果 `T` 的移动会抛异常，需要另行定义 `pop` 的异常保证。**销毁 Queue 前先让所有工作线程退出并 join。**

Bounded Queue 建议使用两个 CV：

- `notEmpty`：Consumer 等待 `closed || !q.empty()`。
- `notFull`：Producer 等待 `closed || q.size() < capacity`。

插入后通知 notEmpty；消费后通知 notFull；close 后通知两者所有等待线程。两个 CV 能避免唤醒错误类别的等待者。

## 5. TimerScheduler：动态 deadline 用外层 Event Loop

常见错误：

```cpp
cv.wait_until(lk, oldDeadline, [&]{ return stopped; });
```

如果新任务的 deadline 比 oldDeadline 更早，notify 虽然触发内部唤醒，但 Predicate 仍为 false，内部可能继续等待到**旧时间**。

简单方案：用原始 `wait_until`，不论超时、通知还是虚假唤醒，都重新计算调度状态。

```cpp
void run() {
    std::unique_lock lk(m_);
    while (!stopped_) {
        discardInvalidHeapEntries(); // 持锁；处理 lazy deletion / version

        if (queue_.empty()) {
            cv_.wait(lk, [&] { return stopped_ || !queue_.empty(); });
            continue;
        }

        const auto deadline = queue_.top().deadline;
        if (Clock::now() < deadline) {
            cv_.wait_until(lk, deadline);  // 故意不传 predicate
            continue; // 总是重新计算 earliest
        }

        Task task = takeValidExpiredTask(); // 持锁取出，实际实现需考虑异常
        lk.unlock();
        try {
            task(); // 用户回调不持锁
        } catch (...) {
            // 按明确的异常策略记录/处理；不可使 worker 意外终止
        }
        lk.lock();
    }
}
```

注意：例子是结构模板，不是完整 Scheduler；停止时是否丢弃或执行队列中任务，必须提前决定。

### 何时 Notify？

| 变化 | 是否必须及时 Notify |
| --- | --- |
| 空队列变为非空 | 是 |
| 新最早有效 deadline 提前 | 是 |
| 插入更晚任务 | 否 |
| 取消非最早任务 | 通常否 |
| 取消最早任务 | 正确性上可等原 deadline 到来；想及时重算则通知 |
| 最早任务延后 | 可等旧 deadline 到来（提前醒一次）；想及时调整则通知 |
| Shutdown | 是（唤醒所有等待者） |

当 Heap 使用 Lazy Deletion 时，须区分最早**有效**任务；过期 entry 不能导致执行取消任务。

## 6. Generation Counter：可选的优化/协议表达

需要让等待者识别“等待决策已改变”，可在同一 mutex 下更新 generation：

```cpp
const auto seen = generation_;
cv.wait_until(lk, deadline, [&] {
    return stopped_ || generation_ != seen;
});
```

任何要求 Worker 提前重算的修改：

```cpp
{
    std::lock_guard lk(m_);
    // 修改 queue / deadline
    ++generation_;
}
cv.notify_one();
```

对于仅一个 Worker 的 TimerScheduler，**外层循环 + 原始 wait_until 通常更简单**。Generation 不是正确性必需条件；也不是消息队列，多个变化可能被合并。

## 7. Timeout 与 Clock

- 调度相对延迟优先 `std::chrono::steady_clock`，避免墙上时间被调整。
- 不要在循环中反复 `wait_for(lk, 5s)` 重置总超时。
- 使用固定 deadline：

```cpp
auto until = std::chrono::steady_clock::now() + 5s;
bool ok = cv.wait_until(lk, until, [&]{ return ready; });
```

即使返回 timeout，也应依据实际共享状态决定业务行为；Predicate overload 的布尔返回值表达最终 Predicate 是否成立。

## 8. ThreadPool：直接 CV 还是先抽象 BlockingQueue？

**两小时 OA 默认：ThreadPool 内直接使用一个 mutex + CV + task queue。** 只有 Queue 本身有独立职责或多处复用时，再抽象 BlockingQueue。先定义 shutdown 语义，再选结构。

### A. 直接 CV（推荐的最小实现）

```cpp
class ThreadPool {
    using Task = std::function<void()>;
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    bool stopped_ = false;
    std::vector<std::thread> workers_;

    void worker() {
        for (;;) {
            Task task;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stopped_ || !tasks_.empty(); });
                if (stopped_ && tasks_.empty()) return; // drain shutdown
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task(); // 解锁后执行
            } catch (...) {
                // 记录异常或由 submit 包装任务向 future 传播
            }
        }
    }
};
```

以上是 Worker 核心片段，并非完整可构造类。实际还需：启动 Worker、提交任务（持锁检查 stopped）、shutdown（持锁设 stopped、notify_all）、join、析构及异常安全策略。避免在持锁时执行用户代码。

### B. BlockingQueue + ThreadPool

```cpp
class ThreadPool {
    BlockingQueue<Task> queue_;
    std::vector<std::thread> workers_;

    void worker() {
        while (auto task = queue_.pop()) {
            try { (*task)(); }
            catch (...) { /* 异常策略 */ }
        }
    }
};
```

此方案将 push/pop/close 与任务执行分离，适合 Queue 复用或复杂的队列独立测试，但也引入接口、异常安全和关闭语义的额外设计。

| 场景 | 推荐 |
| --- | --- |
| 简单 ThreadPool / TaskExecutor | 直接 CV |
| 独立 Bounded BlockingQueue | 独立 Queue，两个 CV |
| 多模块 Producer/Consumer 通道 | BlockingQueue |
| TimerScheduler / Priority + Cancel | 通常直接管理 CV 与调度状态 |
| 需要停止接收、取消待执行任务等复杂策略 | 优先统一管理状态机，再决定抽象边界 |

**关键原则：One synchronization domain, one owner。**

- Task queue、accept/stop、等待条件经常必须在同一锁域下保持不变量。
- `shutdown(drain)` 与 `shutdown(discard)` 语义不同；若有不同关闭模式，不要随意把它们隐藏在普通 `close()` 里面。
- `submit` 与 `shutdown` 并发时必须决定任务是被接受还是拒绝，检查和插入应在同一锁下。
- Queue 分层合理的前提是：队列能独立封装所有需要原子协调的入队、出队与关闭状态；不要跨两把锁拼接必须原子执行的逻辑。
- 生命周期需要保证：停止提交、唤醒等待者、join Worker，最后销毁 CV/Mutex/Queue；从 Worker 自身调用 join 需专门处理。
- 任务异常不能直接越过线程入口，否则可能调用 `std::terminate()`。

### 两小时 OA 决策顺序

1. 写出接口 `submit / shutdown / worker`。
2. 画出状态与不变量（Queue、stopped、drain/discard）。
3. 最小版本：一把 mutex、一个 CV、一个 queue。
4. 测试空 Queue shutdown、多 Worker、submit 与 shutdown 并发、任务抛异常。
5. 有明确复用价值时，再抽象 BlockingQueue；有性能指标后再优化锁粒度。

## 9. 多条件 Event Loop：外层循环 + 原始 wait（KeyedTaskExecutor / 多 Timer）

**OA 中很实用的模式：先在锁内检查所有业务状态，遇到无法继续的状态就 `wait` 后 `continue`，醒来从循环顶部重新决策。**

这不是一种绕开 Predicate 的技巧；它本身就是把 Predicate 和后续 action 展开写成显式状态机。尤其适合多个等待原因、不止一个 deadline、需要选择可执行 key 的场景。

### 9.1 KeyedTaskExecutor：多个条件可以直接展开

```cpp
std::unique_lock lk(m_);
while (true) {
    // completeCnt_ 在此实现中实际表示“已开始任务数”，
    // 更准确的名字是 startedCnt_，不是“已完成任务数”。
    if (stopped_ && completeCnt_ == submitCnt_)
        break;  // 没有尚未开始的任务；shutdown() 还需 join 正在运行的 worker

    if (completeCnt_ == submitCnt_) {
        cv_.wait(lk);
        continue;
    }

    // 可能有等待中的任务，但这些 key 已经被其他 worker 占用。
    auto it = taskInfosMap_.begin();
    for (; it != taskInfosMap_.end(); ++it) {
        if (!it->second.running && !it->second.tasks.empty())
            break;
    }
    if (it == taskInfosMap_.end()) {
        cv_.wait(lk);
        continue;
    }

    // 在锁内取任务、标记该 key 为 running，并更新 started counter。
    // 解锁执行用户 task；交付 promise 结果/异常；重新加锁恢复状态。
    // 然后自然进入下一轮 while，不必在此调用 wait()。
}
```

逻辑上相当于一个小型状态机：

| 状态 | 动作 |
| --- | --- |
| stopped && 没有未开始的任务 | worker 退出；外部 shutdown() join 所有 worker |
| 没有未开始的任务 | `wait(lk)` |
| 有任务但所有候选 key 都处于 running | `wait(lk)` |
| 找到可执行 key | 标记 running、取任务、解锁执行、加锁收尾、下一轮 |

这种写法的安全条件：

1. 所有决定是否等待的共享状态，必须用**同一把 mutex** 保护；检查与 `wait` 之间不能出现不受该锁约束的条件修改。
2. `wait(lk)` 会原子地释放 mutex 并进入等待；返回时已重新拿到 mutex。所有唤醒（包括 Spurious Wakeup）都通过 `continue` 回到顶部重检。
3. **每个使等待者可继续的状态变化，需要 notify 或有其他确定的执行线程继续推进。** 例如本例 worker 完成一个 key 后，自身直接进入下一轮寻找任务，所以**完成处不必为了同 key 后继任务额外 notify**；新任务提交、容量释放、shutdown 则按对应等待者的需要进行通知。
4. 示例省略实际取任务/执行部分。若使用 `unordered_map`，**不能保留 iterator 跨解锁区间**，因为其他线程插入可能 rehash；可保存 key 后重新 `find`，或使用插入时 iterator 稳定的 `std::map`（前提是元素未被删除）。

### 9.2 多个 deadline：不能简单从上往下逐个 wait_until

下面的草图存在两个问题：

```cpp
if (deadline < now) {
    cv_.wait_until(lk); // 错：缺少 deadline 参数
    continue;
}
```

- `deadline < now` 说明时间**已到**，应该处理事件，而不是等待。要“时间未到则等”，条件应是 `now < deadline`，调用应为 `cv_.wait_until(lk, deadline)`。
- 即使把两处条件分别改对，若先 `wait_until(deadline1)`，就可能阻塞 **deadline2 更早到期**，或者阻塞原本可执行的 key 任务。**不同来源的等待条件，不能盲目串行阻塞。**

适用于“两个 Timer + Keyed Tasks”共用一个 worker event loop 的结构如下（**调度骨架，不是完整实现**）：

```cpp
using Clock = std::chrono::steady_clock;

std::unique_lock lk(m_);
while (true) {
    if (stopped_ && completeCnt_ == submitCnt_)
        break; // 假定 shutdown 不再要求处理尚未到期的 timer

    const auto now = Clock::now();

    // 先消费已到期事件。处理函数必须使 timer 被移除/重新调度，
    // 否则 continue 后会不断命中相同过期条件，导致 busy loop。
    if (timer1Armed_ && deadline1_ <= now) {
        processTimer1StateLocked(); // 这里只更新调度状态，不在锁内跑用户回调
        continue;
    }
    if (timer2Armed_ && deadline2_ <= now) {
        processTimer2StateLocked();
        continue;
    }

    // 有立即可执行的任务，就直接处理，不要因为未来 deadline 先睡眠。
    auto it = taskInfosMap_.begin();
    for (; it != taskInfosMap_.end(); ++it) {
        if (!it->second.running && !it->second.tasks.empty())
            break;
    }
    if (it != taskInfosMap_.end()) {
        // 锁内 claim 任务、设置 running；
        // 解锁执行，交付结果/异常，重新加锁；
        // 注意 unordered_map iterator 跨解锁可能失效。
        executeOneReadyTaskAndRelock(lk, it); // 伪代码 helper：返回时 lk 已持锁
        continue;
    }

    // 走到这里：既没有到期 timer，也没有立即可执行的 key。
    // 可能是没有任务，也可能是任务都被其它 worker 占用。
    std::optional<Clock::time_point> next;
    if (timer1Armed_)
        next = deadline1_;
    if (timer2Armed_)
        next = next ? std::min(*next, deadline2_) : deadline2_;

    if (next)
        cv_.wait_until(lk, *next); // 最早 deadline；notify/timeout/spurious -> 重检
    else
        cv_.wait(lk);              // 无 deadline，等状态改变

    // 自然回到 while 顶部；不能认为唤醒就代表某个条件已满足。
}
```

**核心区别**：原始 `wait_until` 非常适合动态 deadline，因为每次唤醒后回到顶层，重新选择最早有效 deadline；但它不意味着“每个等待条件都各自 wait 一次”。要先决定**可执行的 work**，没有 work 时再选**最近一次需要醒来的时间**。

如果 deadline 表示“某个任务在这个时刻以前不能运行”，而不是 timer callback，原则相同：将尚未到期任务视为不可运行，统一找到所有候选中最近的可运行时间，再 `wait_until`。如果 shutdown 要 drain 未来定时任务，退出条件和 deadline 策略都需要另外定义。

### 9.3 notify 策略与 OA 取舍

| 状态改变 | 典型动作 | 原因 |
| --- | --- | --- |
| 新任务加入 / 队列从空变非空 | notify（混用不同 waiter 的单 CV 可用 `notify_all`） | 可能有 worker 可执行 |
| 有等待任务从队列进入 running，腾出容量 | 通知等待提交的 producer | 容量按“已接受但未开始”计算 |
| worker 完成任务，自己立即开始下轮扫描 | **不一定需要 notify** | 该 worker 本身能够继续推进后继任务 |
| 更早的有效 deadline 被插入 | notify 负责定时等待的 worker | 必须重新计算 wake time |
| shutdown / close | `notify_all` | 等待中的 producer / consumer 需要退出或 drain |

- **简单 BlockingQueue**：`cv.wait(lk, pred)` 通常最清楚。
- **复杂调度 / 多状态 / 动态 deadline**：`while(true) + 显式状态检查 + wait/continue` 通常更容易 review。
- **一个 CV 混用 producer、consumer、timer**：`notify_one` 可能唤醒不需要它的线程；为简化 OA 可先使用 `notify_all`，或者按等待职责拆分 CV，再做优化。
- **两小时 OA 范围**：优先满足题目明确要求的任务异常传播、同 key 顺序、并行、容量与 shutdown；内存分配失败的回滚可作为额外增强，但容器 iterator 有效性和正常路径的 race / deadlock **不能**因此忽略。

> 口诀：**Evaluate state → Execute if ready → Otherwise wait (until earliest deadline) → Re-evaluate.**
>
> `wait` 的唤醒只是“重新判断的机会”，**不是已经满足条件的证明**。

## 10. OA Checklist

1. **先定义状态和不变量**：谁修改、谁检查、由哪把 mutex 保护？
2. 每个 `wait` 都有循环式 Predicate 或外层重评估。
3. 每一种状态变化都检查：是否可能让某个 Waiter 的条件变为 true？谁来 notify？
4. Shutdown 后等待者是否都能退出？是否 Drain？是否 join？
5. 回调、耗时 IO、任务执行通常放在解锁后。
6. 考虑通知前/后并发、Spurious Wakeup、Timeout、取消、Reschedule、空队列。
7. 测试多个 Producer / Consumer、close 与 push/pop 竞争、提前插入 deadline。
8. 优先 STL + mutex + CV；无明确性能要求不抢先实现 lock-free。

**面试策略：Correctness → Simplicity → 必需的 Robustness → Performance。** 基本异常安全和线程生命周期安全属于正确性，不应留到最后。

核心口诀：**Mutex protects state; CV coordinates waiting; notify does not transfer mutex; wakeup is not predicate success.**
