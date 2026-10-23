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

## 9. OA Checklist

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
