# C++ 并发工具与 Linux 跨进程 IPC 系统总结

> 更新：2026-10-12  
> 范围：C++11/14/17/20、Linux/POSIX、oneTBB、HFT 低延迟架构与面试。  
> 原则：区分 **语言标准保证**、**操作系统接口保证**、**特定编译器/硬件的工程实践**。代码示例主要使用 C++20。

## 0. 总体框架

先问三个问题：

1. **数据在哪里？** 同进程共享地址空间，还是跨进程共享内存或消息？
2. **需要什么保证？** 互斥、通知、资源许可、任务完成、结果传递、顺序与可见性？
3. **性能目标是什么？** Throughput、CPU 利用率、平均延迟，还是严格的 p99.9 尾延迟？

| 类别 | 典型工具 | 解决的问题 |
|---|---|---|
| Mutual Exclusion | `mutex`、`shared_mutex`、RAII locks | 防止共享状态并发冲突 |
| State Waiting | `condition_variable`、`atomic::wait/notify` | 等待状态改变 |
| Permits | `counting_semaphore`、`binary_semaphore` | 计数许可、资源容量 |
| Rendezvous | `latch`、`barrier` | 多个参与者到达同步点 |
| Async Result | `promise`、`future`、`packaged_task`、`async` | 传递异步结果与异常 |
| Atomic & Memory Model | `atomic`、`atomic_ref`、memory order | 原子访问、内存可见性 |
| Concurrent Data Structure | oneTBB containers、SPSC/MPSC 队列 | 容器并发访问 |
| Task Parallelism | oneTBB `task_group`、`parallel_for`、`task_arena` | 任务调度、负载均衡 |
| IPC Data | `mmap`、`shm_open`、`memfd_create` | 跨进程共享内存 |
| IPC Synchronization | process-shared pthread、POSIX semaphore、Futex | 跨进程协调 |
| IPC Messages/Notification | `eventfd`、Pipe、Unix Socket、Message Queue | 通知与消息传递 |

版本记忆：C++11 包括 mutex、condition_variable、atomic、future/promise/async；C++14 有 shared_timed_mutex；C++17 有 shared_mutex、scoped_lock；C++20 有 semaphore、latch、barrier、atomic_ref、atomic wait/notify、jthread/stop_token。

**第一性原理**：Atomicity ≠ Synchronization ≠ Scheduling。

- **Atomicity**：一次操作不可被观察成部分完成。
- **Synchronization**：建立访问顺序与可见性，例如 release/acquire、mutex unlock/lock。
- **Scheduling**：工作由哪个线程、何时执行。oneTBB 解决这一层。
- **Isolation / Ownership**：避免共享可变状态。低延迟系统往往优先设计 Single Owner，再选同步工具。

---

## 1. Mutex / RAII Lock

### 1.1 工具矩阵

| 工具 | 语义 | 备注 |
|---|---|---|
| `std::mutex` | 独占锁 | 常用默认选择 |
| `std::recursive_mutex` | 同一线程允许递归上锁 | 易掩盖锁边界不清 |
| `std::timed_mutex` | 超时尝试获取 | 适合明确超时策略 |
| `std::shared_mutex` | 多读单写 | 需评估写饥饿、读竞争 |
| `std::lock_guard` | 作用域管理锁 | 简单、不可提前 unlock |
| `std::unique_lock` | 可延迟加锁、手动解锁、移动 | condition_variable 必需 |
| `std::scoped_lock` | RAII、可管理多把锁 | 多锁时使用死锁避免算法 |

~~~cpp
std::mutex m;
int value = 0;

void update() {
    std::lock_guard<std::mutex> lock(m);
    ++value;
}
~~~

**内存语义**：对同一 mutex，unlock 与后续成功 lock 可构成 `synchronizes-with`，保护区间内的普通内存读写因此建立 happens-before。mutex 不保证哪一个线程先获得锁。

**常见坑**：

- 两把锁获取顺序不一致导致 deadlock；多锁使用 `std::scoped_lock(m1, m2)`。
- 持锁执行 I/O、等待外部服务或回调，拉长临界区并造成尾延迟。
- `shared_mutex` 不是“读多必然更快”；短临界区中读写锁成本可能更高。
- Mutex 的竞争路径可触发线程阻塞/唤醒；未竞争时不一定进行系统调用。

### 1.2 Mutex、Semaphore 的重要区别

Mutex 有“锁所有权”语义：持有者负责解锁；Semaphore 是许可计数，允许不同线程 release/acquire。`binary_semaphore` 不等价于一个完整的 Mutex，也不保证 Mutex 所有权规则。

---

## 2. Condition Variable：等待 Predicate

核心不是 “notify”，而是 **共享状态 + Mutex + Predicate**。

~~~cpp
std::mutex m;
std::condition_variable cv;
std::queue<int> q;
bool stopping = false;

void produce(int x) {
    {
        std::lock_guard lock(m);
        q.push(x);
    }
    cv.notify_one();
}

bool consume(int& out) {
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return stopping || !q.empty(); });
    if (q.empty()) return false;  // 已停止，且无剩余任务
    out = q.front();
    q.pop();
    return true;
}
~~~

`cv.wait(lock, predicate)` 逻辑上相当于：

~~~cpp
while (!predicate()) {
    cv.wait(lock); // 原子地释放锁并等待；返回前重新获得锁
}
~~~

为什么必须 predicate？

1. **Spurious Wakeup**：被唤醒不代表条件成立。
2. **Lost Notification**：CV 不保存通知；notify 可能发生在 wait 之前。
3. **多个消费者竞争**：被叫醒后，状态可能已经被其他消费者改变。

不要把 `notify_one` 理解为“给未来消费者保留一个事件”。需要保留次数时应使用计数状态 / semaphore / queue。

---

## 3. Semaphore：许可计数

C++20：

~~~cpp
#include <semaphore>
std::counting_semaphore<16> ready{0};

void producer() {
    publish_item();    // 共享数据仍需自己的安全协议
    ready.release();
}

void consumer() {
    ready.acquire();
    consume_item();
}
~~~

- `release(n)` 增加许可；`acquire()` 消耗许可，没有许可时等待。
- 初始化为 0 表示没有可用许可；许可会“记住”早于 acquire 的 release。
- `std::binary_semaphore` 最大许可计数为 1，适合单一状态信号；不是 Mutex。
- 多项消息/资源仍需要与许可计数一致的队列/资源状态。
- 释放数不能超过允许范围；关闭/停止语义需由应用设计。

**比较**：

| | Mutex | Semaphore | Condition Variable |
|---|---|---|---|
| 核心 | 独占所有权 | 许可计数 | 等 predicate |
| 是否记住事件 | 不适用 | 保存许可 | notify 本身不保存 |
| 是否依赖配套 mutex | 内置 | 否 | 通常是 |
| 常见场景 | 临界区 | 并发资源限额 | 队列非空、状态变化 |

---

## 4. Latch / Barrier：参与者同步

### `std::latch`（C++20）

一次性 countdown，初始计数降到 0 后所有等待者可继续，**不能重置**。

~~~cpp
std::latch initialized{3};

auto worker = [&] {
    initialize();
    initialized.count_down();
};

std::thread a(worker), b(worker), c(worker);
initialized.wait();
a.join(); b.join(); c.join();
~~~

`initialized.wait()` 等的是完成信号，不等于 join；实际线程仍可能继续工作，所以这里最后仍要 join。

### `std::barrier`（C++20）

多线程每轮到达同步点后统一推进到下一 phase：

~~~cpp
std::barrier sync{3};
auto worker = [&] {
    for (int phase = 0; phase < 5; ++phase) {
        process(phase);
        sync.arrive_and_wait();
    }
};
~~~

支持 completion step 与 `arrive_and_drop`。如果参与者提前退出而未调整 expected count，其他线程可能永久等待。

| | Latch | Barrier |
|---|---|---|
| 次数 | 一次 | 多轮 |
| 典型问题 | 等 N 个初始化完成 | 分阶段并行 |
| 其他 | countdown 可由任意参与者进行 | 每轮需要协调到达数 |

---

## 5. Promise / Future / Packaged Task / Async

它们解决 **异步结果/异常传递**，不是共享容器的互斥。

### 5.1 Promise + Future

~~~cpp
std::promise<int> p;
auto f = p.get_future();

std::thread worker([&] {
    try {
        p.set_value(calculate());
    } catch (...) {
        p.set_exception(std::current_exception());
    }
});

int result = f.get(); // 阻塞等待，取得结果或重新抛出异常
worker.join();
~~~

模型：

~~~text
Producer -> promise -> shared state (value / exception) -> future -> Consumer
~~~

- `future<T>`：move-only，`get()` 只能消耗结果一次。
- `shared_future<T>`：多个使用者可共享同一结果，`get()` 可多次调用；多个线程可使用不同的 `shared_future` 副本。
- 未设置结果就销毁 promise，consumer 可获得 `broken_promise`。
- `future.wait_for` / `wait_until`：等待状态或超时；对 deferred future，`wait_for` 可以返回 `deferred`。

### 5.2 Packaged Task

~~~cpp
std::packaged_task<int()> task([] { return calculate(); });
auto f = task.get_future();
std::thread t(std::move(task));
int result = f.get();
t.join();
~~~

`packaged_task` = Callable + Future Shared State；**自己不负责创建线程**。

### 5.3 std::async

~~~cpp
auto f = std::async(std::launch::async,
                    [] { return calculate(); });
int result = f.get();
~~~

- 默认 policy 允许 `async` 或 `deferred`；后者可能在调用 `get/wait` 的线程中执行。
- 当 shared state 来自 `std::async(std::launch::async)` 时，最后一个关联 future 的析构可能阻塞直至任务完成。
- `std::async` 不等于一个通用固定容量的 Thread Pool。
- Future/Promise 的 shared state 在 C++ 标准中没有跨进程共享保证。

---

## 6. Atomic 与 Memory Ordering（基础但极重要）

| Memory order | 用途 |
|---|---|
| `relaxed` | 仅保证原子性，不建立与其他线程的数据发布关系 |
| `release` | 在 store/RMW 处发布此前的普通写入 |
| `acquire` | 通过匹配的 load/RMW 观察发布并获得可见性 |
| `acq_rel` | 常用于同时读写的 RMW |
| `seq_cst` | 最强的全局一致顺序抽象，默认 |
| `consume` | 历史上用于依赖顺序，实践不宜作为首选 |

**经典发布模型**：

~~~cpp
int payload = 0;
std::atomic<bool> ready{false};

// Producer
payload = 42;
ready.store(true, std::memory_order_release);

// Consumer
if (ready.load(std::memory_order_acquire)) {
    use(payload);
}
~~~

当 Consumer 的 acquire 读到了发布的值，先前对 payload 的普通写才获得所需的 happens-before。

注意：`volatile` 不提供线程同步；“x86 TSO 很强”也不能替代 C++ 层面的正确原子访问与编译器顺序约束。

### Spin Lock

~~~cpp
std::atomic_flag locked = ATOMIC_FLAG_INIT;
void lock() {
    while (locked.test_and_set(std::memory_order_acquire)) {
        // busy spin
    }
}
void unlock() {
    locked.clear(std::memory_order_release);
}
~~~

优点：极短等待时可以避免睡眠/唤醒。缺点：竞争时消耗 CPU、缓存一致性流量、可能导致尾延迟或调度公平性问题。

---

## 7. C++20 Atomic Wait / Notify：不是 Spin Lock

~~~cpp
std::atomic<int> state{0};

// Consumer
state.wait(0, std::memory_order_acquire); // 直到观察到 state != 0
consume();

// Producer
publish_data();
state.store(1, std::memory_order_release);
state.notify_one();
~~~

- `wait(old)` 等待观察到值**不再等于 old**；它不会获取锁，也不会修改变量。
- 实现可能先短暂 spin，随后 park/block；C++ 标准不要求固定策略或一定调用 futex。
- wait 内部处理虚假唤醒，只有观察到不同的值才返回。
- **ABA**：若数值 `0 -> 1 -> 0` 太快，消费者可能没观察到变化；事件计数 / sequence number 通常比 bool 更稳妥。
- `notify_one/all` 不替你记录事件，状态本身决定是否满足条件。

### 7.1 Blocking Mutex 示例（教学版）

~~~cpp
class TinyMutex {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (flag.test_and_set(std::memory_order_acquire))
            flag.wait(true, std::memory_order_relaxed);
    }
    void unlock() {
        flag.clear(std::memory_order_release);
        flag.notify_one();
    }
};
~~~

这个实现有原子互斥状态以及阻塞等待，但仍需考虑公平性、超时和生产级工程要求；并非普通纯自旋锁。

### 7.2 为什么 `std::atomic<int>` 可以只占 4 bytes？

在常见 Linux x86-64 + GCC/libstdc++：

~~~cpp
sizeof(int)              // 通常 4
sizeof(std::atomic<int>) // 通常 4
alignof(std::atomic<int>) // 通常 4
~~~

**标准不保证大小相等。** 内核等待队列通常**不存储在 atomic 对象内部**；Linux 标准库可借助 futex 或用户态等待表。

经典 Linux `futex` 接收一个 **对齐的 32-bit futex word**，实现“只有预期值仍相等才原子地入队阻塞”，避免“先检查状态、刚好被通知、之后睡死”的 lost wakeup：

~~~text
load state
  -> != expected : return
  -> == expected : futex WAIT(expected)
     Kernel 再次比较并原子地决定是否阻塞
notify -> FUTEX_WAKE -> waiter 重新检查状态
~~~

对 `atomic<uint64_t>` 的 wait，传统 32-bit futex 不能直接覆盖全部 64-bit 值，标准库可能使用额外等待表/代理 word。具体实现需检查版本。

**特别注意**：`std::atomic::wait/notify` 没有标准层面的跨进程唤醒保证；实现还可能使用 `FUTEX_WAIT_PRIVATE`（进程内专用）或每进程私有辅助表。

---

## 8. Concurrent Containers 与 oneTBB

标准库没有通用的 `std::concurrent_hash_map` 或 `std::concurrent_vector`。

| oneTBB 容器 | 典型并发保证 / 注意点 |
|---|---|
| `concurrent_hash_map` | `accessor` 写、`const_accessor` 读；访问器协调元素访问 |
| `concurrent_unordered_map` | 支持部分并发操作；`unsafe_erase` 等明确非并发安全 |
| `concurrent_vector` | 并发增长；不等于同一元素并发写安全，也非普通 vector 连续存储保证 |
| `concurrent_queue` | MPMC 场景；具体接口语义依赖实现 |
| `concurrent_bounded_queue` | 容量限制与阻塞队列行为 |

~~~cpp
#include <oneapi/tbb/concurrent_hash_map.h>
using Map = oneapi::tbb::concurrent_hash_map<int, int>;
Map map;

void increment(int key) {
    Map::accessor a;
    map.insert(a, key);
    ++a->second;
} // accessor 释放访问控制
~~~

**Concurrent container ≠ 多步骤业务操作是事务。** 例如 L3 Orderbook 需要同步维护 Order Map、Level、Intrusive List、聚合数量，单个 Hash Map 线程安全不保证跨结构不变量。

### oneTBB Task Scheduler

~~~cpp
#include <oneapi/tbb/task_group.h>
oneapi::tbb::task_group group;
group.run([] { calculate_a(); });
group.run([] { calculate_b(); });
group.wait();
~~~

~~~cpp
#include <oneapi/tbb/parallel_for.h>
oneapi::tbb::parallel_for(
    std::size_t{0}, data.size(),
    [&](std::size_t i) { process(data[i]); }
);
~~~

- **Thread**：OS 调度实体。**Task**：逻辑工作，由调度器映射到 worker。
- oneTBB 典型使用 work-stealing 实现动态负载均衡。
- `task_arena` 可约束并发度，但不意味着创建固定数量的独占线程，亦不自动保证固定 CPU affinity。
- Task 之间不能假设同时执行；互相阻塞等待可能导致调度不良甚至死锁。
- 适合批量计算、回测、后台任务；**不天然适合需要独占核心和稳定 p99.9 的 hot path**。
- Concurrent Containers/oneTBB Task Scheduler 本身不是跨进程 IPC 协议。

---

## 9. 跨进程 IPC：数据与同步分离

### 9.1 机制矩阵

| 机制 | 共享/消息/同步 | 重点 |
|---|---|---|
| `mmap(MAP_SHARED)` | 共享数据页 | 两个虚拟地址可不同 |
| `shm_open` | 命名共享内存对象 | 可供独立进程打开 |
| `memfd_create` | FD 支持的内存对象 | 可通过 FD 传递；可选 huge pages |
| Process-shared pthread mutex/cond | 跨进程同步 | 属性需要 `PTHREAD_PROCESS_SHARED` |
| POSIX `sem_init(..., pshared=1, ...)` | 共享内存中的 Semaphore | sem_t 必须位于共享内存 |
| POSIX `sem_open` | 命名 Semaphore | 通过名字访问同一许可对象 |
| Linux `futex` | 跨进程等待唤醒 | 非 PRIVATE futex + 共享 word |
| Linux `eventfd` | 事件计数/通知 | FD 可继承/传递，可 `epoll` |
| Pipe/FIFO | 流式 IPC | 内核缓冲 |
| Unix Domain Socket | 本机 IPC | 可传消息、凭据与 FD |
| POSIX Message Queue | 按消息通信 | 内核管理队列 |
| Boost.Interprocess | C++ 封装 | managed_shared_memory、offset_ptr、interprocess mutex |

### 9.2 C++ 标准与跨进程的保证边界

| 工具 | 跨进程保证 |
|---|---|
| `std::mutex` / `std::condition_variable` | **C++ 标准不保证** |
| `std::semaphore`、`std::future`、`std::latch` | **C++ 标准不保证** |
| `std::atomic<T>` / `std::atomic_ref<T>` | **C++ 标准不保证 address-free 跨进程互操作** |
| `std::atomic::wait/notify` | **不保证跨进程 wakeup** |
| pthread process-shared mutex/cond | **POSIX 接口明确支持（平台实现支持时）** |
| POSIX process-shared/named semaphore | **明确支持** |
| Linux shared futex | **Linux 内核明确支持** |

要区分三个层次：

1. **ISO C++**：atomic 的 lock-free 实现被鼓励具备 address-free 性质，但并非必须。两个独立进程的 C++ 对象生命周期/同步不是跨平台 C++ 内存模型的完整承诺。
2. **POSIX**：提供标准化的跨进程 pthread mutex/cond/semaphore；POSIX.1-2024 包含 C11 `<stdatomic.h>` 接口，但**并未因此定义一个独立的、跨进程 lock-free 整数原子保证**。
3. **Linux + x86-64 + GCC/Clang 的工程实践**：正确映射的共享页上，经验证的 lock-free、address-free 标量原子 load/store/RMW 通常可用于 IPC；这是目标 ABI/编译器/硬件依赖，不是无条件可移植结论。

GCC `__atomic_load_n`、`__atomic_store_n`、`__atomic_compare_exchange_n` 是**编译器 Builtins**，不是 POSIX 系统调用。非 lock-free 类型可能退化到 `libatomic`，不能轻易当作跨进程安全。

### 9.3 Shared Memory 的地址与对象

~~~text
Process A VA: 0x7f... ─┐
                       ├── 相同底层共享物理页
Process B VA: 0x6a... ─┘
~~~

普通裸指针地址可能只对创建它的进程有意义。共享结构尽量使用 **offset/index/handle**，不要随意在共享内存里保存只对某进程有效的绝对虚拟地址。Boost.Interprocess 的 `offset_ptr` 是典型解决方案。

独立启动的进程：

~~~cpp
// Creator：省略错误处理、权限与初始化握手
int fd = shm_open("/ipc_ring", O_CREAT | O_EXCL | O_RDWR, 0600);
ftruncate(fd, sizeof(Shared));
void* mem = mmap(nullptr, sizeof(Shared),
                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

// Attacher
int fd2 = shm_open("/ipc_ring", O_RDWR, 0);
void* mem2 = mmap(nullptr, sizeof(Shared),
                  PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
~~~

`MAP_SHARED | MAP_ANONYMOUS` 常用于 **fork 前创建映射，fork 后父子进程共享**；两个独立进程不能各自匿名 mmap 就期待共享同一块页。

**初始化握手很重要**：Creator 的 shm_open 成功不代表 ftruncate、mmap、对象构造和版本检查已经完成。应使用命名 semaphore、状态协议或其他外部协调，避免第二个进程读取半初始化对象。

### 9.4 Process-shared Mutex 与 Robust Mutex

~~~cpp
struct Shared {
    pthread_mutex_t mutex;
    int counter;
};

// mutex 位于共享内存；仅初始化者执行一次
pthread_mutexattr_t attr;
pthread_mutexattr_init(&attr);
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
pthread_mutex_init(&shared->mutex, &attr);
pthread_mutexattr_destroy(&attr);
~~~

获取锁可能返回 `EOWNERDEAD`：当前调用者已获得互斥锁，但受保护的数据可能不一致。必须恢复数据，然后 `pthread_mutex_consistent`，再解锁。Robust Mutex **发现进程/线程持锁死亡，不替你自动回滚业务状态**。

### 9.5 POSIX Semaphore 与 eventfd

~~~cpp
// sem_t 位于共享内存
sem_init(&shared->sem, 1 /* pshared */, 0);
sem_post(&shared->sem);
sem_wait(&shared->sem);  // 生产级代码需处理 EINTR
~~~

`eventfd(0, ...)` 在内核维护 64-bit 计数，`write` 通知，`read` 取数，`epoll` 可以监听；进程之间需继承或用 `SCM_RIGHTS` **传递同一个 eventfd 的 FD**，各自新建不会共享。

### 9.6 Futex：跨进程阻塞原语，非通用 Atomic API

传统 Linux futex 操作使用 4-byte 对齐的 32-bit word：

~~~text
User space atomic store/load/CAS：负责状态本身
FUTEX_WAIT(expected)：值相等时原子地比较并入队睡眠
FUTEX_WAKE(n)：叫醒等待者
~~~

跨进程要求 word 在共同共享映射中，使用 **`FUTEX_WAIT` / `FUTEX_WAKE`（非 `_PRIVATE`）**。Futex 的 wait 与 notify 不取代数据发布的原子 load/store 和内存顺序协议。

普通共享内存可见性不需要为每条消息调用 `msync`；`msync` 主要用于文件映射的存储同步，不是跨核发布的 acquire/release 替代品。

---

## 10. 如何在 Shared Memory 上创建 std::atomic

### 10.1 错误：仅 reinterpret_cast

~~~cpp
void* mem = mmap(...);
auto* state = reinterpret_cast<std::atomic<int>*>(mem);
state->store(1);  // 缺少显式对象构造，不能仅靠 cast 证明正确
~~~

`reinterpret_cast` 只变更指针类型，不自动完成你预期的 `std::atomic<int>` 对象初始化。

### 10.2 正确的对象构造流程（Linux 工程方式）

~~~cpp
#include <atomic>
#include <new>
#include <sys/mman.h>

struct alignas(64) Shared {
    std::atomic<int> ready{0};
    int payload{0};
};

// Creator：mem 是有效、可写、足够大且正确对齐的 MAP_SHARED 映射
Shared* shared = new (mem) Shared{};  // placement new，仅一个初始化者

// 发布初始化完成给 Attacher：使用独立可靠的启动握手
// Attacher 在初始化完成后，将其映射地址作为 Shared* 使用
// 不应再次 placement new 覆盖已被使用的对象
~~~

完整设计还需：检查 mmap/ftruncate/shm_open 错误、确认 ABI/`sizeof`/对齐、唯一初始化者、初始化完成信号、退出后的统一销毁/回收规则，以及同一映射不被存活使用者提前回收。

**Placement New 只解决对象生命周期/构造；不负责跨进程可移植性，也不负责普通 Message 的发布同步。**

`sizeof(std::atomic<int>) == sizeof(int)` 在常见 x86-64/libstdc++ 成立，但**C++ 标准不保证**；不能凭大小相等来决定可否强制转换。

---

## 11. atomic_ref 在 Shared Memory 中的优势和限制

~~~cpp
#include <atomic>
#include <cstdint>

struct SharedCounters {
    alignas(64) std::uint64_t head{0};
    alignas(64) std::uint64_t tail{0};
};

// 两个进程的各自执行线程均可创建本地 atomic_ref（平台已验证）
std::atomic_ref<std::uint64_t> tail(shared->tail);
tail.store(42, std::memory_order_release);
~~~

**为什么适合 IPC 布局**：

- Shared Memory 里保存普通整数，二进制 ABI 更容易直接约定。
- `atomic_ref` wrapper 放在各进程本地，不需要把 wrapper 自身存入共享内存。
- 更容易与 GCC `__atomic_*` 或其他语言的固定格式互操作。
- 与 `std::atomic<T>` 对等地使用 atomic load/store/RMW，不意味着它天生更快。

**严格前提**：

1. 引用的普通 T 对象已正确创建并完成初始化，生命周期长于 `atomic_ref`。
2. 对象实际地址满足 `std::atomic_ref<T>::required_alignment`，它可能比 `alignof(T)` 更严格。
3. `atomic_ref` 引用期间，同一对象必须只通过兼容的 `atomic_ref` 访问；不能混用 `shared->head = ...` 的普通读写。
4. 应要求/验证目标原子操作为 lock-free，而且在使用的环境里具备 address-free 的跨进程语义。
5. 即使 `is_always_lock_free` 为 true，**也不等于 C++ 标准承诺跨进程可用**。
6. `atomic_ref` 并不解决队列容量控制、崩溃恢复、初始化/重启协议。

~~~cpp
using Ref = std::atomic_ref<std::uint64_t>;
static_assert(Ref::is_always_lock_free); // 可作为目标平台约束
static_assert(offsetof(SharedCounters, head) % Ref::required_alignment == 0);
static_assert(offsetof(SharedCounters, tail) % Ref::required_alignment == 0);
~~~

> 布局约束应由目标编译器验证；必要时给成员使用 `alignas(Ref::required_alignment)`。跨进程通信的抽象 C++ 生命周期仍不能只靠这个 static_assert 获得可移植标准保证。

### std::atomic vs atomic_ref vs GCC __atomic

| 维度 | `std::atomic<T>` | `std::atomic_ref<T>` | GCC `__atomic_*` |
|---|---|---|---|
| C++ 对象表示 | 原子对象本身 | 普通 T 对象 | 通常普通 T 对象 |
| 对象初始化 | 构造 Atomic | 初始化普通 T | 初始化普通 T |
| C++ 标准接口 | 是 | 是，C++20 | 否，编译器扩展 |
| Shared Layout 可控性 | 受实现影响 | 更直接 | 更直接 |
| 自动得到跨进程保证 | 否 | 否 | 否 |
| 性能 | 同类原生原子操作时接近 | 同类原生原子操作时接近 | 同类原生原子操作时接近 |

---

## 12. Shared Memory SPSC Ring：面试重点

### 12.1 设计不变量

- **Single Producer / Single Consumer**：一个线程/进程只写 tail，一个只写 head。
- Ring 中 Msg slot 由 Producer 填充；tail **release** 发布完成。
- Consumer 用 tail **acquire** 确认消息发布后才读 slot。
- Consumer 读完 slot，head **release** 发布回收。
- Producer 用 head **acquire** 确认 slot 可重用。
- Producer 不在 full 时覆盖；Consumer 不在 empty 时读取。
- 不需要把 Msg 的各个字段都做成 atomic；通过 head/tail 的同步保证普通 Msg 的读写时序。
- 进程间的 release/acquire 效果依赖已验证的平台实现，而非 ISO C++ 跨进程标准保证。

### 12.2 典型 C++20 Ring 逻辑

> 以下为算法核心；`Ring` 必须已经在 MAP_SHARED 区域内正确初始化，且已完成跨进程启动握手；目标平台须满足 lock-free/address-free 原子实现要求。

~~~cpp
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

struct Message {
    std::uint64_t id;
    std::uint64_t price;
};

constexpr std::uint64_t CAP = 1024;  // power of two
using CounterRef = std::atomic_ref<std::uint64_t>;

struct Ring {
    alignas(64) std::uint64_t head{0}; // consumer writes
    alignas(64) std::uint64_t tail{0}; // producer writes
    std::array<Message, CAP> data{};
};

static_assert(std::is_trivially_copyable_v<Message>);
static_assert(CAP != 0 && (CAP & (CAP - 1)) == 0);
static_assert(CounterRef::is_always_lock_free);
static_assert(offsetof(Ring, head) % CounterRef::required_alignment == 0);
static_assert(offsetof(Ring, tail) % CounterRef::required_alignment == 0);

bool push(Ring& r, const Message& msg) {
    CounterRef head(r.head);
    CounterRef tail(r.tail);
    const std::uint64_t t = tail.load(std::memory_order_relaxed);
    const std::uint64_t h = head.load(std::memory_order_acquire);

    if (t - h >= CAP) return false; // full
    r.data[t & (CAP - 1)] = msg;
    tail.store(t + 1, std::memory_order_release);
    return true;
}

bool pop(Ring& r, Message& out) {
    CounterRef head(r.head);
    CounterRef tail(r.tail);
    const std::uint64_t h = head.load(std::memory_order_relaxed);
    const std::uint64_t t = tail.load(std::memory_order_acquire);

    if (h == t) return false; // empty
    out = r.data[h & (CAP - 1)];
    head.store(h + 1, std::memory_order_release);
    return true;
}
~~~

这里使用 64-bit **单调递增 unsigned 计数器**，逻辑上以模 2^64 运算；要求运行时严格维护 `0 <= tail - head <= CAP`（在其模意义下的有效窗口），且两端不能重置一个计数器而不协调整个 Ring。对跨进程重启要引入新的 epoch/generation 协议。

**False Sharing**：将 head 和 tail 分隔至不同 cache lines 通常有益，但 `alignas(64)` 是目标机器 cache-line 假设，并非所有机器通用。

### 12.3 Backpressure 与等待策略

| 策略 | CPU 利用率 | 延迟/应用场景 |
|---|---|---|
| Busy Polling / Spin | 高 | Dedicated core、极短周期 Hot Path |
| Spin + pause | 高但可降低自旋压力 | 仍需测量竞争与 SMT 影响 |
| Yield | 不确定 | 依赖 OS 调度，尾延迟可能很差 |
| Spin then Park | 折中 | 空闲负载波动明显时 |
| Futex / Semaphore | 阻塞时 CPU 低 | 系统调用/调度增加尾延迟 |
| eventfd + epoll | 可集成多 FD | 与 socket/控制通道一起等待 |

**千万别假设 `std::atomic::wait/notify` 自动适用于独立进程**：即使核心状态在 shared memory 上，标准库可能采用 process-private futex 或辅助表。跨进程阻塞需要明确支持共享进程的机制。

### 12.4 Production-Quality IPC 的额外问题

- Creator/Attacher 初始化竞态：SHM 创建成功并不等于 Ring ready。
- Producer 写一半 crash：tail 未发布时，Consumer 不应消费残缺消息；但系统仍需识别进程已死。
- Consumer 读一半 crash：slot 无法及时释放，需重启恢复。
- 断电/服务重启：共享内存通常不等于 durable persistence。
- 数据格式版本：magic、ABI version、endianness、alignment、capacity。
- 序列号：loss、gap、duplicate、stale epoch/replay 处理。
- Monitoring：heartbeat、backpressure、queue depth、drop/overrun。
- 物理内存和 NUMA：huge pages、prefault、CPU affinity、页归属要结合 profile 评估。

---

## 13. HFT 工程选型

| 场景 | 优先评估 | 核心原因 |
|---|---|---|
| Market Data Handler / Feed -> Pricing | Shared-memory SPSC / Aeron IPC / Dedicated Thread | 低共享写、稳定路径 |
| 单个 L3 Orderbook | Single Owner + flat hash map + intrusive list | 避免多容器不变量竞争 |
| Order Routing / OMS 单订单状态机 | Single Owner + 消息队列 | 线性状态迁移、重放恢复 |
| 异步日志 | Queue + Background Consumer | 避免同步慢 I/O |
| 启动等待依赖 | Latch / Future / named semaphore | 明确 ready 协议 |
| 后台配置更新 | Mutex / shared_ptr snapshot / RCU 型设计 | 更新少、读取多 |
| 批量风险/回测 | TBB parallel_for / task_group | 吞吐量、动态均衡 |
| 共享资源并发上限 | Semaphore | 明确许可计数 |
| 空闲后台消费者 | CV / Semaphore / eventfd | 降低 idle CPU |
| IPC 与 socket 混合监听 | eventfd + epoll | 统一等待多个 FD |

**不能泛化**：Spin 不一定比 Mutex 快、TBB 不一定比 Dedicated Thread 慢、Lock-free 不等于 Wait-free。要测量 p50、p99、p99.9、CPU 利用率、cache miss、context switch、系统调用和 NUMA 跨节点访问。

---

## 14. QRT/HFT 面试回答模板与追问

### 推荐的英文简答

> I would implement an SPSC ring buffer in shared memory, mapped by both processes using mmap with MAP_SHARED.
>
> Head and tail would be separate, cache-line-aligned monotonically increasing counters. The producer owns tail; the consumer owns head. Message publication uses release store on tail, and the consumer uses acquire load before reading the message. The reverse head publication is used to reclaim slots.
>
> Strictly speaking, ISO C++ does not guarantee std::atomic synchronization across independent process boundaries. On a validated Linux x86-64 toolchain, I can use lock-free address-free atomic operations in shared memory: either placement-constructed std::atomic counters, or C++20 std::atomic_ref over suitably aligned ordinary counters. GCC __atomic builtins are another compiler-specific option, not a POSIX API.
>
> In the hot path I can busy poll without syscalls. If blocking is desirable, I would use a documented process-shared mechanism such as shared futex, POSIX semaphore or eventfd. I'd also design a startup handshake, backpressure and crash recovery.

### 追问速答

**Q：为什么 `atomic_ref` 更适合 SHM？**  
A：数据 layout 是普通整数；wrapper 本地创建；二进制 ABI 容易固定。但不更快、不自动跨进程安全，且必须遵守对齐/生命周期/只通过 atomic_ref 访问。

**Q：能否直接 `reinterpret_cast<std::atomic<int>*>(mem)`？**  
A：不能仅靠 cast 完成对象构造。要有正确的对象生命周期，比如初始化者 placement-new 一次，之后其他进程在约定 ABI 下访问。

**Q：`std::atomic<int>` 与 `int` 一样大吗？**  
A：Linux x86-64 通常都 4 字节；标准不保证，`alignof` 也需检查。

**Q：POSIX 有跨进程 Atomic API 吗？**  
A：POSIX 有 `<stdatomic.h>`（现代 POSIX 版本）及明确的 process-shared pthread/semaphore；但并无通用的、独立承诺 address-free lock-free 跨进程整数 Atomic API。GCC `__atomic_*` 是编译器扩展。

**Q：Futex 是 Atomic 吗？**  
A：不是通用 Atomic RMW API。用户态 Atomic 负责状态，Futex 负责条件等待和内核唤醒。跨进程使用非 `_PRIVATE` 操作。

**Q：为什么不调用 `msync`？**  
A：普通 SHM 消息可见性依赖 CPU/编译器内存同步协议，不是写回磁盘；每条消息不必 msync。

**Q：为什么 Msg 字段不需要 Atomic？**  
A：单一 slot 在发布/消费协议下只有当前 owner 访问；release/acquire 的 head/tail 构成访问的先行顺序（在已验证的跨进程实现上）。

**Q：为什么不用 Concurrent Hash Map 做 L3 Orderbook？**  
A：业务状态跨 Order Map、Price Level、Linked List 和聚合数量，需要跨结构的原子不变量；Single Owner 更容易推理一致性和尾延迟。

**Q：进程 crash 时怎么办？**  
A：Atomic 不解决故障恢复；需要 heartbeat、epoch/sequence、初始化协议、状态重建、持久化/回放及 failover 策略。

---

## 15. 常见错误清单（面试检查表）

- [ ] 把 CV 的 notify 当成会记住的许可，没写 predicate。
- [ ] 把 Semaphore 当作有线程所有权的 Mutex。
- [ ] 认为 Atomic Wait 是持续 Busy Spin 或保证每次唤醒必走 Futex。
- [ ] 认为 `std::atomic<int>` 的大小被标准保证等于 4。
- [ ] 认为 lock-free 自然意味着 address-free / 跨进程可用。
- [ ] 认为 POSIX 的 `stdatomic.h` 就是特制的跨进程 Atomic API。
- [ ] 认为 GCC `__atomic` Builtin 是 Linux 系统调用。
- [ ] 只 `reinterpret_cast` 一片字节就声称 std::atomic 对象已构造。
- [ ] 在同一底层共享位置上由每个 Attacher 重新 placement-new。
- [ ] 对 `atomic_ref` 引用中的对象混合普通读写。
- [ ] 忘记 `atomic_ref<T>::required_alignment`。
- [ ] 跨进程直接依赖 `std::atomic::wait/notify`，未验证 _PRIVATE/辅助表。
- [ ] 在独立进程里分别创建 MAP_ANONYMOUS，误以为会共享。
- [ ] 在 Shared Memory 保存另一个进程的裸指针。
- [ ] 用 Concurrent Container 误认为跨多个容器的业务不变量已自动保证。
- [ ] 忽略 SPSC 的 full/empty、stop、启动握手、进程死亡和重启版本。

---

## 16. 参考资料（官方及常用技术文档）

- C++ atomic_ref：<https://en.cppreference.com/w/cpp/atomic/atomic_ref>
- atomic_ref required_alignment：<https://en.cppreference.com/w/cpp/atomic/atomic_ref/required_alignment>
- C++ atomic wait：<https://en.cppreference.com/w/cpp/atomic/atomic/wait>
- Linux futex(2)：<https://man7.org/linux/man-pages/man2/futex.2.html>
- Linux mmap(2)：<https://man7.org/linux/man-pages/man2/mmap.2.html>
- Linux shm_open(3)：<https://man7.org/linux/man-pages/man3/shm_open.3.html>
- Linux eventfd(2)：<https://man7.org/linux/man-pages/man2/eventfd.2.html>
- pthread_mutexattr_setpshared(3)：<https://man7.org/linux/man-pages/man3/pthread_mutexattr_setpshared.3.html>
- pthread_mutexattr_setrobust(3)：<https://man7.org/linux/man-pages/man3/pthread_mutexattr_setrobust.3.html>
- sem_overview(7)：<https://man7.org/linux/man-pages/man7/sem_overview.7.html>
- GCC __atomic Builtins：<https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html>
- oneTBB 官方代码：<https://github.com/uxlfoundation/oneTBB>
- Boost.Interprocess：<https://www.boost.org/doc/libs/release/doc/html/interprocess.html>

> **最终要点**：在 HFT 中首先减少共享状态；共享状态不可避免时选择正确的原子发布/回收协议；跨进程时再额外证明映射、对象布局、平台 Atomic 实现、初始化与故障恢复。严格标准保证、Linux API 与常见工程实现应始终分层表达。
