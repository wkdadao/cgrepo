# HFT C++：低延迟通信、Fan-out、Replay 与 Memory Reclamation 设计

> 讨论总结：Reference Counting / Hazard Pointer / Epoch-Based Reclamation (EBR) / QSBR / SPSC / MPMC / Selective Multicast / Replay CAS Pin / Replay Pointer Window / Replay Copy Archive  
> 更新：2026-11-03。示例以 C++20 为主；GCC/Clang 使用 -std=c++20 -O2 -pthread。  
> **定位**：设计笔记 + 可单独编译的教学代码。示例演示最重要的同步关系，不承诺 production-ready（尤其是容错、异常安全、overflow、动态重配置、shutdown）。

## 目录

1. 设计需求与正确性不变量
2. SPSC/MPMC 与内存回收的边界
3. Atomic Shared Ownership
4. Hazard Pointers（Sequence Lookup、Folly）
5. EBR（Epoch-Based Reclamation）
6. QSBR（与 EBR 的保护区间比较）
7. 动态 Reader Registry
8. 固定容量 SPSC / MPMC
9. Multiple SPSC + Shared Data Pool + Selective Fan-out
10. 共享 Ring + Pending Mask
11. 共享 Ring + Completed Cursor
12. Recent Replay：Generation + CAS Pin
13. Replay Reader 指针窗口
14. Replay Reader Copy
15. Segment-Based Replay Archive
16. Reclamation 机制比较
17. Selective Fan-out 模型比较
18. **HFT 场景选型：动态 Reader、Segmented SPSC、Backpressure**
19. **异步 Data Escape、传递式 Completion、RefCount 与 Cursor 性能比较**
20. 正确性检查与 Benchmark
---

## 1. 实际需求及不变量

系统背景：单 Writer、多个 Reader（通常各自运行在固定 Processor / polling worker 上）；每条 Data 可能只送给 1 个 Reader，也可能送给任意子集。Writer 同时需要按 seq 随机查找最近历史数据以支持冷路径 Replay。原实现的固定 Slot 中有统一原子状态字，包含 seq/generation 与 refcount；Replay 通过 CAS Pin；**Writer 每次推进 Slot 也需要 CAS**。

### 必须区分的三种保护

- **Delivery ownership**：某条消息发给谁？只有目标 Reader 才负责处理。
- **Lifetime protection**：谁仍可能访问原始 Data？Live Reader 进度、Replay Pin、Replay 指针窗口、Archive 复制进度，均可能参与。
- **Retention policy**：即使没有 Reader 持有对象，为了未来按 seq 查询，仍需保留一段历史。refcount==0 **不等于**可以删除历史。

以 x 表示 Data、T(x) 表示目标 Reader 集合、S(x,i) 表示 x 在 Reader i 的本地投递序号、C(i) 表示 Reader i 已连续完成的本地序号：

~~~text
LiveDone(x) = AND_{i in T(x)} ( C(i) >= S(x,i) )

ReclaimableOriginal(x) =
    LiveDone(x)
    AND NoOutstandingReplayPin(x)
    AND ReplayRetentionNoLongerNeedsOriginal(x)
~~~

若使用一个所有 Reader 共用的 global sequence G(x)，并且每个 Reader 的完成进度是 **按投递顺序处理完成的最大 global seq**，可把 S(x,i) 换成 G(x)。但 Reader 的 cursor 必须体现「**全部下游原始 Data 使用结束**」而不是仅仅「dequeue」或「已转发」。Consumer 可能异步转发给 P3 时，见 §19 的 Transitive Completion / Intrusive RefCount 比较。

**关键安全不变量：**

1. 先发布受保护资格，后访问 Data；最后一次访问完毕后，才能发布完成。
2. Writer 确认不再有使用者后，才能覆盖非原子 Data 字段；访问期间绝不能发生并发覆盖（否则是 C++ data race，未必仅是 UAF）。
3. 同一 Slot 的 seq/generation 必须防止旧 Handle 误中新的对象；generation wrap、reader-id reuse 需要另外处理。
4. 新 Reader 必须先参加保护协议，才能获得可能被回收的旧指针。
5. 独立的 Archive 同样需要自身的 eviction/lifetime 保护。
6. 有限容量 + 无损 + 永远不阻塞 Writer + 无限慢 Reader，这四个目标不能同时保证。

## 2. SPSC/MPMC 与内存回收的边界

| 模型 | 解决的问题 | 典型额外回收机制 |
|---|---|---|
| 固定容量 SPSC/MPMC，按值传递 | Queue 内部同步、单条消息交接 | 通常不需要 |
| SPSC/MPMC 传独占对象 Handle | 线程间所有权转移 | 可以不需要；需显式 Pool 归还 |
| 多 SPSC，Data* 发给多个 Reader | Selective Fan-out、共享生命周期 | Completed Cursor / RefCount / ACK |
| 动态节点 MPMC linked queue | 既要并发操作，又要删除内部节点 | HP / EBR / 其他安全回收 |
| 不经 Queue 的共享 Config* | 直接并发 Reader + 版本更新 | QSBR / EBR / HP / shared_ptr |
| Cold Replay 任意 seq lookup | 防止 Slot 被复用时仍在读取 | CAS Pin / Archive lifetime / 受控串行化 |

最优先的架构优化顺序通常是：**single ownership → message passing → immutable snapshot / replica → 必要时才引入通用回收**。这是 HFT 选型习惯，不是数学上的统一性能排名。

## 3. 模型 1：Atomic shared_ptr / Reference Counting

概念：对象每个共享所有者持有一个引用；不需要注册全部 Reader；但不能在一个未经保护的 raw pointer 上先取指针再直接 fetch_add（acquisition race）。C++20 atomic<shared_ptr> 能安全取得共享所有权；是否 lock-free 取决于库实现。

**独立可运行示例：**

~~~cpp
// 01_atomic_shared_ptr.cpp
#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>
struct Config { int version, multiplier; };
int main() {
    std::atomic<std::shared_ptr<const Config>> current{
        std::make_shared<const Config>(Config{0, 2})
    };
    std::atomic<bool> go{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int n = 0; n < 10000; ++n) {
                auto p = current.load(std::memory_order_acquire);
                assert(p && p->multiplier == 2);
            } // p 在 scope 结束时释放
        });
    }
    go.store(true, std::memory_order_release);
    for (int v = 1; v <= 100; ++v)
        current.store(std::make_shared<const Config>(Config{v, 2}),
                      std::memory_order_release);
    for (auto& t : readers) t.join();
}
~~~

**Trade-off**：动态 Reader/异步 callback 简洁；每次 load 可能涉及引用计数同步和库内部锁；最后一次 release 可能触发 destructor / allocator，避免把它意外放在高优先级热路径上。循环引用仍需 weak_ptr。

## 4. 模型 2：Hazard Pointer（Load → Publish → Validate）

Reader 公布正在保护的 **具体指针**；Writer retire 后扫描 hazard 记录，并只删除不再受保护的 Node。下面使用固定 Reader 槽、seq_cst 保守排序；真实系统可采用可复用 Hazard Record Pool 与批量 retire 扫描。

~~~cpp
// 02_hazard_pointer.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
struct Node { int value; };
int main() {
    std::atomic<Node*> head{new Node{0}};
    std::array<std::atomic<Node*>, 2> hazard{};
    for (auto& h : hazard) h.store(nullptr);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 10000; ++n) {
            Node* p;
            do {
                p = head.load(std::memory_order_seq_cst);
                hazard[id].store(p, std::memory_order_seq_cst);
            } while (p != head.load(std::memory_order_seq_cst));
            if (p) assert(p->value >= 0);
            hazard[id].store(nullptr, std::memory_order_seq_cst);
        }
    };
    std::thread r0(reader, 0), r1(reader, 1);
    go.store(true, std::memory_order_release);
    std::vector<Node*> retired;
    auto scan = [&] {
        const Node* h0 = hazard[0].load(std::memory_order_seq_cst);
        const Node* h1 = hazard[1].load(std::memory_order_seq_cst);
        auto out = retired.begin();
        for (Node* p : retired) {
            if (p != h0 && p != h1) delete p;
            else *out++ = p;
        }
        retired.erase(out, retired.end());
    };
    for (int i = 1; i < 1000; ++i) {
        retired.push_back(head.exchange(new Node{i},
                                        std::memory_order_seq_cst));
        if (retired.size() > 16) scan();
    }
    r0.join(); r1.join();
    scan();
    delete head.load();
    assert(retired.empty());
}
~~~

**限制**：此例只有两个固定 Reader 和一个 Writer，Hazard Record 本身稳定存活。动态加入：先获取并注册可被 scanner 发现的空 Slot，再保护数据；退出：先清 Hazard，之后才归还 Slot。若扫描器并发访问 Record，不能直接 delete Record。HP 处理回收安全，不自动证明任意 lock-free 算法没有逻辑 ABA。


### 4.1 Hazard Pointer 是否只能读取最新版本？

不是。Hazard Pointer (HP) 管理 **Lifetime Protection**，而非 Lookup 或历史数据保留。Reader 一旦成功保护 Seq 100，即使 Writer 把 index[100 % N] 改为指向 Seq 100+N，Reader 仍能处理原有 Seq 100；新 Reader 再查询 Seq 100，因 index 中已是新版本而返回 Not Found。

完整模型为：固定容量的 atomic<Entry*> History Index，每次发布分配新的不可变 Entry；Writer exchange 指针、retire 旧 Entry；Reclaimer 根据全体 Hazard Record 决定何时 delete。注意：**固定的是索引 Slot，Entry 对象本身不在该 Slot 内原地覆盖**。它不同于现有固定 Pool Slot + Generation + RefCount CAS Pin。

### 4.2 完整 C++20：hazard_history_seq

独立源文件：[hazard_history_seq.cpp](hazard_history_seq.cpp)。包含以下场景：single writer、四个 Reader、随机按 seq 查找、RAII Pinned、Index Eviction、Retired List 回收、旧版本保护、并发测试。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic \
    hazard_history_seq.cpp -o hazard_history_seq
./hazard_history_seq
~~~

完整源代码如下（不依赖 Folly）：

~~~cpp
// hazard_history_seq.cpp
// g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic hazard_history_seq.cpp -o hazard_history_seq
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

// 1 writer, fixed reader slots, at most 1 Pinned per reader ID.
// Sequence does not wrap or repeat; stop all readers before destruction.
template <std::size_t Capacity, std::size_t ReaderCount>
class SequenceHistory {
    static_assert(Capacity > 0 && ReaderCount > 0);
public:
    struct Entry {
        const std::uint64_t seq;
        const std::uint64_t value;
    };
private:
    struct alignas(64) HazardSlot {
        std::atomic<const Entry*> ptr{nullptr};
    };
    std::array<std::atomic<const Entry*>, Capacity> index_{};
    std::array<HazardSlot, ReaderCount> hazards_{};
    std::atomic<std::uint64_t> latest_{0};
    std::vector<const Entry*> retired_;  // writer-only
    std::uint64_t lastPublished_ = 0;     // writer-only
public:
    class Pinned {
        friend class SequenceHistory;
        SequenceHistory* owner_ = nullptr;
        std::size_t readerId_ = 0;
        const Entry* ptr_ = nullptr;
        Pinned(SequenceHistory* owner, std::size_t id, const Entry* p)
            : owner_(owner), readerId_(id), ptr_(p) {}
    public:
        Pinned() = default;
        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;
        Pinned(Pinned&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              readerId_(other.readerId_),
              ptr_(std::exchange(other.ptr_, nullptr)) {}
        Pinned& operator=(Pinned&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                readerId_ = other.readerId_;
                ptr_ = std::exchange(other.ptr_, nullptr);
            }
            return *this;
        }
        ~Pinned() { reset(); }
        void reset() noexcept {
            if (owner_) {
                // Last dereference must occur before this store.
                owner_->hazards_[readerId_].ptr.store(
                    nullptr, std::memory_order_seq_cst);
                owner_ = nullptr;
                ptr_ = nullptr;
            }
        }
        explicit operator bool() const noexcept { return ptr_ != nullptr; }
        const Entry& operator*() const { assert(ptr_); return *ptr_; }
        const Entry* operator->() const { assert(ptr_); return ptr_; }
    };

    SequenceHistory() = default;
    SequenceHistory(const SequenceHistory&) = delete;
    SequenceHistory& operator=(const SequenceHistory&) = delete;
    ~SequenceHistory() {
        // Join all threads and destroy outstanding Pinned first.
        for (const auto& h : hazards_)
            assert(h.ptr.load(std::memory_order_seq_cst) == nullptr);
        for (auto& loc : index_)
            delete loc.load(std::memory_order_seq_cst);
        for (const Entry* p : retired_) delete p;
    }

    void publish(std::uint64_t seq, std::uint64_t value) {
        assert(seq == lastPublished_ + 1);
        const Entry* fresh = new Entry{seq, value};
        auto& location = index_[seq % Capacity];
        const Entry* old = location.exchange(
            fresh, std::memory_order_seq_cst);
        if (old) retired_.push_back(old); // NEVER delete old immediately
        lastPublished_ = seq;
        latest_.store(seq, std::memory_order_release);
        if ((seq & 31) == 0) collect();
    }
    std::uint64_t latest() const {
        return latest_.load(std::memory_order_acquire);
    }

    Pinned lookup(std::uint64_t seq, std::size_t readerId) {
        assert(readerId < ReaderCount);
        assert(hazards_[readerId].ptr.load(std::memory_order_seq_cst)
               == nullptr && "one Pinned per reader ID");
        if (seq == 0) return {};
        auto& location = index_[seq % Capacity];
        const Entry* candidate;
        do {
            // DO NOT dereference before successful validation.
            candidate = location.load(std::memory_order_seq_cst);
            hazards_[readerId].ptr.store(
                candidate, std::memory_order_seq_cst);
        } while (candidate !=
                 location.load(std::memory_order_seq_cst));

        if (!candidate || candidate->seq != seq) {
            hazards_[readerId].ptr.store(
                nullptr, std::memory_order_seq_cst);
            return {}; // evicted or not published
        }
        return Pinned{this, readerId, candidate};
    }

    // Single writer takes a full hazard snapshot and compacts retired_.
    void collect() {
        std::array<const Entry*, ReaderCount> snapshot{};
        for (std::size_t i = 0; i < ReaderCount; ++i)
            snapshot[i] = hazards_[i].ptr.load(
                std::memory_order_seq_cst);
        std::size_t keep = 0;
        for (const Entry* old : retired_) {
            if (std::find(snapshot.begin(), snapshot.end(), old)
                  != snapshot.end())
                retired_[keep++] = old;
            else delete old;
        }
        retired_.resize(keep);
    }
    // Writer thread only.
    std::size_t retiredCount() const { return retired_.size(); }
};

int main() {
    {
        SequenceHistory<8, 2> history;
        for (std::uint64_t s = 1; s <= 8; ++s)
            history.publish(s, s * 10);
        auto old = history.lookup(3, 0);
        assert(old && old->value == 30);
        for (std::uint64_t s = 9; s <= 11; ++s)
            history.publish(s, s * 10);
        history.collect();
        assert(history.retiredCount() == 1);
        auto missing = history.lookup(3, 1);
        assert(!missing);
        assert(old->seq == 3 && old->value == 30);
        old.reset();
        history.collect();
        assert(history.retiredCount() == 0);
        std::cout << "old-version pin & eviction: PASS\n";
    }
    {
        SequenceHistory<64, 4> history;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> hits{0}, misses{0};
        std::vector<std::thread> readers;
        for (std::size_t id = 0; id < 4; ++id)
            readers.emplace_back([&, id] {
                std::uint64_t rnd = id + 17;
                while (!stop.load(std::memory_order_acquire)) {
                    auto top = history.latest();
                    if (top == 0) continue;
                    rnd = rnd * 6364136223846793005ULL + 1;
                    auto back = (rnd >> 32) % 96;
                    auto wanted = top > back ? top - back : 1;
                    auto pin = history.lookup(wanted, id);
                    if (pin) {
                        assert(pin->seq == wanted);
                        assert(pin->value == wanted * 10);
                        hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        misses.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        for (std::uint64_t s = 1; s <= 50000; ++s)
            history.publish(s, s * 10);
        stop.store(true, std::memory_order_release);
        for (auto& t : readers) t.join();
        history.collect();
        assert(history.retiredCount() == 0);
        auto latest = history.lookup(50000, 0);
        assert(latest && latest->value == 500000);
        latest.reset();
        std::cout << "concurrent lookup: PASS; hits=" << hits
                  << ", misses=" << misses << '\n';
    }
}

~~~

**边界**：单 Writer、每 Reader ID 独占一个 Hazard Slot、一个 Reader ID 同时最多一个 Pinned，线程退出前清 Hazard，History 的析构发生在所有 Reader join 后。Sequence 不回绕、不复用；对象在发布后不可变。Index 脱链与对象内存删除是两个不同时间点。生产实现仍应处理动态 Reader 注册、allocator、异常安全和长时间 pin 引起的内存积压。

### 4.3 为什么 Load → Publish → Validate 正确？

Reader 三步：R1 = location.load; R2 = hazard.store(candidate); R3 = location.load 重新验证。**R3 成功前绝不能 dereference(candidate)**。Writer 两步：W1 = 从 index 中 unlink/replace old；W2 = 在 W1 之后扫描全部 Hazard Records，确认没有人保护才 delete。

典型情况：

- Reader R2、R3 在 Writer W1 前，Reader 可以继续处理旧对象；Writer W2 在保护仍有效时必须看到该 Hazard。
- Writer 已 W1、W2 并释放旧对象后，Reader 仍可以短暂拥有一个过时的地址值，却必须在 R3 验证失败后重试；绝不能解引用。
- Reader Publish 与 Writer Unlink 重叠，可能出现 false-positive 暂缓回收，但不能让成功验证的 Reader 遭遇 UAF。

在这个简化模型中（单一发布入口、无回绕/错误 ABA、Hazard 在最后一次读取之后才清除），若所有相关访问均是 seq_cst，则下列危险交错会产生矛盾：

~~~text
R2 < R3             Reader 顺序
W1 < W2             Writer 顺序
R3 < W1             R3 仍验证旧值 A
W2 < R2             W2 未看见仍有效的 A

=> R2 < R3 < W1 < W2 < R2     不可能
~~~

如果 Writer 扫描时看到 nullptr，也可能是 Reader 已结束访问并清除了 Hazard，这也是安全的。以上简洁证明不能直接替代对多入口、任意地址重用、可动态删除 Hazard Records 的完整算法证明。

Retired List 压缩示意：

~~~cpp
std::size_t keep = 0;
for (const Entry* old : retired_) {
    if (std::find(protectedPtrs.begin(), protectedPtrs.end(), old)
          != protectedPtrs.end())
        retired_[keep++] = old; // 保留
    else
        delete old;             // 本轮可回收
}
retired_.resize(keep);
~~~

protectedPtrs 为 Writer 获取的 Hazard 快照；在演示模型中 Writer 独占 retired_，因此 vector 压缩不需要 CAS。扫描若看到后来已清除的 Hazard，只会延迟释放，不会导致提前释放。

### 4.4 Hazard Pointer 是否必须使用 seq_cst？

**不必所有 Atomic 都是 seq_cst，但是不能把所有操作简单降成 Acquire/Release。**

只用 Reader release-store Hazard + acquire-load Index，和 Writer release-store Index + acquire-load Hazard，允许 Store Buffering：Reader 看见旧 Index，而 Writer 没看见新 Hazard，导致双向遗漏。

最易证明的实现：Hazard 发布、第二次验证、Writer Unlink、Reclaimer Hazard Scan 都采用 seq_cst（完整示例中第一次 Index Load 也选择 seq_cst；它在相应同步协议下通常可以用 acquire）。

可选方案：**配对的 SC fences**，示意协议骨架：

~~~cpp
// Reader:
auto* p = location.load(std::memory_order_acquire);
hazard.store(p, std::memory_order_release);
std::atomic_thread_fence(std::memory_order_seq_cst);
auto* check = location.load(std::memory_order_acquire);

// Writer/Reclaimer, only after old is unlinked:
auto* old = location.exchange(fresh, std::memory_order_acq_rel);
std::atomic_thread_fence(std::memory_order_seq_cst);
auto* observed = hazard.load(std::memory_order_acquire);
~~~

这要求配对 fence、原子修改序以及对象摘链/扫描次序共同满足协议。**这是结构性示意，不是完整的、覆盖全部数据结构与动态 Registry 的独立程序。** 随意只在 Reader 或 Writer 一端增加 fence、把两个 SC fences 都降成 acq_rel，均不能沿用上述正确性证明。

在常见 x86-64 编译器上，seq_cst hazard store 可能使用 locked xchg；release-store + seq_cst fence 可能使用 mov + mfence/locked instruction。两者都有 Store→Load 排序成本，不能声称后者一定更快。

### 4.5 Folly Hazard Pointer、API 与 Asymmetric Fence

官方资源：

- [Folly Hazptr.md](https://github.com/facebook/folly/blob/main/folly/docs/Hazptr.md)
- [Folly Hazptr.h](https://github.com/facebook/folly/blob/main/folly/synchronization/Hazptr.h)
- [Folly AsymmetricMemoryBarrier.cpp](https://github.com/facebook/folly/blob/main/folly/synchronization/AsymmetricMemoryBarrier.cpp)
- [P1202R0: Asymmetric Fences](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1202r0.pdf)

Folly 通过 hazptr_holder / make_hazard_pointer() 封装 Load–Publish–Validate，把 Reader 对 Object 的生命周期保护移到 holder 上。对象从所有可被 HP 查询的发布入口摘链后才能 retire，不能直接并发 delete。Holder 可复用，以摊销获取 Record 的成本。Folly 提供 Domain、Record 管理、批量/可选异步回收等能力。

**Folly API 用法示意（需要安装 Folly，consume 为应用逻辑，并非独立编译的标准库代码）：**

~~~cpp
#include <folly/synchronization/Hazptr.h>
#include <atomic>
#include <cstdint>

struct Entry : folly::hazptr_obj_base<Entry> {
    std::uint64_t seq = 0;
    std::uint64_t value = 0;
};
std::atomic<Entry*> history[1024]{};

void publish(std::uint64_t seq, std::uint64_t val) {
    Entry* p = new Entry;
    p->seq = seq; p->value = val;
    Entry* old = history[seq % 1024].exchange(p, std::memory_order_acq_rel);
    if (old) old->retire();
}

void replayLookup(std::uint64_t seq) {
    auto holder = folly::make_hazard_pointer();
    Entry* p = holder.protect(history[seq % 1024]);
    if (p && p->seq == seq) {
        // process(p->value) within holder's lifetime
    }
}
~~~

**Asymmetric Fence**：Folly 在特定平台/配置下以 Reader 侧轻量同步与 Reclaimer 侧重同步配对，Linux 可以利用进程级 membarrier 等机制。它**不是**通用 C++20 的「用 acquire/release 替换 seq_cst 就同样安全」，更不是随便调用 atomic_signal_fence 就能实现。若必要条件不满足，须走库的有效回退路径。具体 CPU/内核版本下的性能要测量，不能把历史的纳秒 Benchmark 当作本机指标。

Folly 官方文档指出：HP 对具体对象提供细粒度 lifetime protection，RCU 通常保护一个更大的读侧临界区；HP 在对象必须跨等待/阻塞被持有时较有优势，而 RCU/QSBR 若 Reader 长时间不报告进度可能阻塞整个 Domain 的回收。Read-mostly 与 Cold Replay 混合时，要比较 Reader 同步成本、Retired backlog、Writer update 延迟。

### 4.6 与既有 Fixed Slot CAS Pin 比较

| 维度 | Hazard Pointer History | Fixed Slot Generation + CAS Pin |
|---|---|---|
| Object 内存 | 每版本独立不可变 Entry | Pool 中固定 Slot 原地重用 |
| Reader | Publish Hazard + Validate | CAS 同时验证 Seq/Generation + 增 Pin |
| Writer | Exchange index pointer，旧对象 retire | CAS Closing，等待 RefCount 后覆盖 |
| 旧版本 | 索引被覆盖后已 pin 的 Reader 继续访问 | RefCount 未归零时旧 Slot 不可复用 |
| 回收 / 压力 | 对象级回收，可积压 Retired Entries | 固定容量，可能因 pinned slot 阻塞 Writer |
| Replay Lookup | 需 History Index / Retention Policy | 已集成 Seq Lookup |
| 最主要风险 | Hazard ordering、Record 生命周期、内存膨胀 | CAS contention、Writer stall、Generation 回绕 |

**HFT 结论**：你当前的固定 Slot + CAS Pin 并非一定比 HP 慢。HP 可以通过独立分配对象把 Index 更新与删除解耦，但如果仍必须在同一 Pool Slot 原地写入，HP 无法绕开正在被 Reader 使用的旧数据。必要时考虑 Overflow Pool / Immutable Segment / Replay Copy Archive；Benchmark 单 Writer p99.9、Reader protect latency、CAS retries、reclamation backlog 和 Replay 被抢占时的行为。

### 4.7 FAQ

- 已成功 pin 的 Reader 可以继续读旧 Seq；未 pin 的新 Reader 只能从仍存在的 Index/Archive 找到版本。
- Reader 第一次 Load 可能得到之后变悬空的地址；在成功验证前不能解引用、也不能直接访问对象内部 RefCount。
- HP 解决生命周期，不自动保证任意锁自由算法没有逻辑 ABA；按 Seq 查询还应在保护后验证 seq。
- Reader 新注册 Hazard Record 必须先对 Scanner 可见；注销前清理 Hazard，Record 自身也需要稳定生命周期。
- Cacheline、fence、TLS、membarrier、对象分配器、retired 扫描频率与 Reader/Writer 比例共同决定性能；不能只数 Atomic 指令数量。


## 5. 模型 3：EBR（Epoch-Based Reclamation）

Reader 在读取共享对象 **之前**登记 epoch，在最后一次使用 **之后**退出；Writer 对被替换对象登记 retirement epoch。当所有仍活动 Reader 都处于更新后的 epoch 或更晚，旧对象才可回收。

本示例使用稳定的固定槽、全部 seq_cst 排序以清楚展示协议；它不是通用高性能 EBR 库，真实库通常分批 pin、三 epoch、TLS + domain registry。

~~~cpp
// 03_ebr.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <utility>
struct Data { int version; };
int main() {
    std::atomic<Data*> current{new Data{0}};
    std::atomic<unsigned long long> epoch{1};
    // 0 = inactive；非零 = 当前受保护区间对应 epoch
    std::array<std::atomic<unsigned long long>, 2> announced{};
    for (auto& a : announced) a.store(0);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 30000; ++n) {
            auto e = epoch.load(std::memory_order_seq_cst);
            announced[id].store(e, std::memory_order_seq_cst);
            Data* p = current.load(std::memory_order_seq_cst);
            assert(p && p->version >= 0);
            announced[id].store(0, std::memory_order_seq_cst);
        }
    };
    std::thread a(reader, 0), b(reader, 1);
    go.store(true, std::memory_order_release);
    struct Retired { Data* p; unsigned long long token; };
    std::vector<Retired> retired;
    auto collect = [&] {
        auto out = retired.begin();
        for (auto item : retired) {
            bool safe = true;
            for (auto& slot : announced) {
                auto e = slot.load(std::memory_order_seq_cst);
                if (e != 0 && e < item.token) safe = false;
            }
            if (safe) delete item.p;
            else *out++ = item;
        }
        retired.erase(out, retired.end());
    };
    for (int v = 1; v <= 1000; ++v) {
        Data* old = current.exchange(new Data{v},
                                     std::memory_order_seq_cst);
        auto token = epoch.fetch_add(1, std::memory_order_seq_cst) + 1;
        retired.push_back({old, token});
        if (v % 16 == 0) collect();
    }
    a.join(); b.join();
    collect();
    assert(retired.empty());
    delete current.load();
}
~~~

**为什么安全**：Reader 的 epoch 公布先于它的 Data load；Writer 的 pointer exchange 先于推进 epoch。任何真正读到旧对象的 Reader，必然在回收之前已公布旧 epoch，除非它已经退出。新 Reader 若晚于 exchange 才开始，只能读新指针。此教学实现没有动态注销；进程级的 Reader State 必须在 Scanner 停止访问后才能释放。

**成本与风险**：一次 epoch pin 可覆盖临界区多个节点；被抢占的旧 Reader 能拖住整个回收域，retired memory 可能无界增长。

### 5.1 EBR 保护历史 Sequence：原地复用固定 Ring Slot（完整 C++20）

独立源码：**[ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp)**。这是与前一个 atomic<Entry*> + new/delete EBR 示例不同的版本：内存中始终保留固定的 Slot 数组，每个 Slot 直接包含普通非原子 Data；Writer 周期性复用 Slot，**不 delete 任何对象**。这是本项目 Replay Ring 的更贴近实际的模型。

~~~cpp
template <std::size_t Capacity>
struct Ring {
    struct Slot {
        std::atomic<uint64_t> publishedSeq{0}; // 0: invalid
        Data data; // in-place, non-atomic
    };
    Slot slots[Capacity];
};
~~~

**Reader：Pin 新 Epoch 后查询任何仍保留的旧 Seq**

~~~cpp
auto guard = ring.pin(readerId);             // 先发布保护区间
auto* old = guard.lookup(wantedSeq);          // seq % capacity
if (old) process(*old);                      // 不要求创建于当前 Epoch
// old 不得逃逸出 guard 的生命周期
~~~

只要 Slot 当前仍装有目标 Seq（例如最新 Seq 12，Capacity=8，可查 Seq 5–12），Reader 即使 guard.epoch() 已经大于 old->createdEpoch，也可以安全读取。Epoch **不是查询起始 Seq、不是 Slot 版本，也不是对象创建时刻的过滤器**。

**Writer：先 Invalidate，再推进 Epoch，再等待 Grace Period，最后原地覆盖**

~~~cpp
auto& slot = slots_[seq % Capacity];
auto oldSeq = slot.publishedSeq.exchange(0, std::memory_order_seq_cst);
if (oldSeq != 0) {
    auto threshold =
        epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
    waitForReadersBefore(threshold); // active old epochs must leave
}
slot.data = Data{seq, value, epoch_.load(std::memory_order_seq_cst)};
slot.publishedSeq.store(seq, std::memory_order_seq_cst);
~~~

waitForReadersBefore(threshold) 在演示中依次检查所有固定 Reader Record 是否为 Inactive (0) 或大于等于 threshold。**Invalidate 必须先于 Epoch Advance**，否则进入新 Epoch 的 Reader 仍可能从旧 Slot 读到旧 seq，并被 Reclaimer 错误跳过。这里采用全部关键操作 seq_cst 的保守协议，且只有单 Writer；不是可直接替换为 acquire/release 的生产库。

安全原因：

- 已经 Pin 旧 Epoch、并从 Slot 取得旧 seq 的 Reader，会阻止 Writer 完成对应 Grace Period；Writer 在 Guard 结束前不得写入 slot.data。
- Writer 完成 Invalidate+Advance 后，进入新 Epoch 的 Reader 不可能再经该 Slot 成功取得旧 Seq。
- Reader 无需 HP 式 Pointer Revalidation；它仅需检查 Slot 的 publishedSeq == wantedSeq，而且只能在 Guard 内访问 Data。
- **没有 delete，不等于没有并发安全问题**：直接在旧 Reader 访问 slot.data 时覆盖会产生 C++ data race/UB。必须先完成 Grace Period。

源码 main() 包含三项验证：(1) Pin 较新的 Epoch 读取较旧 Seq；(2) Writer 复用被持有的 Slot 时必须等待，旧 Reader 仍可安全使用已经取得的指针，但新的 lookup 返回 Not Found；(3) 4 个 Reader 并发随机查询、Writer 连续发布。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_ring_history_replay.cpp -o ebr_ring_history_replay
./ebr_ring_history_replay
~~~

**正确性与 HFT Trade-off**：

- 查询支持任意「尚在 Ring 保留窗口」的历史 Seq；**已经被覆盖的历史不能靠 Epoch 恢复**，更长时段需要独立 Archive/更多容量。
- Reader 的一个 Guard 可以访问多个旧 Slot，但慢 Reader 可能阻止 Writer 覆盖**任何**仍受同一 Epoch 保护的 Slot，即使该 Reader 实际上只看过某一个。
- 本例为单 Writer + 固定 Reader 组 + 无 seq/epoch 回绕 + seq_cst + Writer 同步等待；它没有做到 Writer wait-free / lock-free，不适合无条件照搬到 ultra-low-latency Writer 热路径。
- 实际上线还需处理 Reader 注册/注销、停止/崩溃、Guard 逃逸、Writer stall/backpressure、异常、CPU pinning 和超时恢复。对于固定 Pool + 随机 Replay，原有 **CAS Pin + Generation** 可以做到更精确的逐 Slot 保护；而 EBR 用更粗粒度的 Grace Period 换取 Reader 侧较少的逐对象操作。


### 5.2 EBR 核心概念与 Replay 需求总结

**EBR 的 Epoch 是 Reader 进入保护区间的时间标记，不是 Seq、Slot Generation、对象 Creation Epoch，也不是 Epoch→Pointer/Slot 的登记表。**

| 概念 | 回答的问题 | 典型记录 |
|---|---|---|
| Seq / Generation | 现在索引到的是否是我想要的数据？ | slot.seq == wantedSeq |
| History Retention | 旧 Seq 是否仍然可被新的 Reader 查到？ | Ring Capacity、Archive Index、Eviction |
| Epoch Pin | 已经查到对象的 Reader 能否安全继续访问？ | readerEpoch[id] = e |
| Retirement/Reclamation Threshold | Writer 何时可以 delete 或重写旧对象？ | retired.ptr / threshold；或 Slot 的 reuseThreshold |

**因此，Reader 可以 Pin 最新 Epoch 100，然后访问在 Epoch 20 创建的 Seq 1000。** 前提是该 Seq 仍在可查询的 History Index/Ring 中，并且之后的 Reuse/Retirement 遵循同一个 EBR 协议。Epoch 的值不能作为历史 Seq 的查询下界，也不需要把 Reader Epoch 回退到对象的 Creation Epoch。

EBR 没有维护精确的 Reader → Slot 映射。一个 Reader Pin Epoch 10，实际只读 Slot 3，却可能阻止 Writer 回收多个其他 Slot。这是保守的 **Temporal Protection（时间区间保护）**。

**三条不变量**：

1. 先正确 Pin Epoch、后取得被保护对象；最后一次使用后才能 Unpin。
2. Writer 先让旧对象/旧版本不能被新的 Reader 取得（unlink / invalidate），再发布相应的 Grace Period Threshold。
3. 在 Grace Period 成功完成前，不能 delete 旧对象，也不能原地覆盖仍可能被 Reader 访问的非原子 Data。

### 5.3 模型 A：Pointer-based History EBR（完整程序）

[ebr_history_seq.cpp](ebr_history_seq.cpp) 是完整的、可独立编译的 C++20 示例：固定大小 atomic<Entry*> History Index、每次发布不可变 Entry、单 Writer、多 Reader、RAII Guard、按 Seq Random Lookup、Retired List、Batch Lookup、并发测试。

简化的接口使用：

~~~cpp
auto guard = history.pin(readerId);
const auto* old = guard.lookup(wantedSeq); // 创建时间不受 guard.epoch 限制
if (old) process(*old);                    // Guard 退出前使用
~~~

Writer 发布与退休：

~~~cpp
const Entry* old = index_[seq % Capacity].exchange(
    new Entry{seq, value}, std::memory_order_seq_cst);

const auto threshold = globalEpoch.fetch_add(
    1, std::memory_order_seq_cst) + 1;

if (old) retired.push_back({old, threshold});
// 以后扫描 readers，满足 Grace Period 时 delete old
~~~

这里 Writer 可以先发布新 Entry、再**延迟删除**旧 Entry；即使慢 Reader 持有旧对象，Writer 通常仍可更新 Index（但 Retired Memory 可能积压，内存分配也有成本）。

Reader 无需 HP 的 Load→Publish Hazard→Reload：因为 Epoch Pin 发生在 Pointer Load 之前。**但仍要验证 Entry::seq == wantedSeq**，因为同一个 Index 可能被下一圈 Sequence 覆盖。

### 5.4 模型 B：固定 Ring Slot 原地复用（完整程序）

[ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp) 是第二份完整 C++20 程序：固定大小的 Slot 数组，每个 Slot 内嵌普通 Data，**没有 Entry new/delete**，Writer 按 Seq 循环复用 Slot。

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0}; // 0 == INVALID
    Data data;                              // 非原子数据
};

// Reader
auto guard = ring.pin(readerId);
const Data* p = guard.lookup(wantedSeq);
if (p) process(*p);

// Writer：旧 Slot 已被发布时
slot.publishedSeq.exchange(0, std::memory_order_seq_cst);
auto threshold = globalEpoch.fetch_add(
    1, std::memory_order_seq_cst) + 1;
waitForGracePeriod(threshold);
slot.data = newData; // 只有经过 Grace Period 才能写入
slot.publishedSeq.store(newSeq, std::memory_order_seq_cst);
~~~

**固定 Ring 的重要转折**：Slot 不删除并不等于无需回收协议。复用旧 Slot 就是在破坏其旧 Data 的存储内容，如果有 Reader 同时读写非原子 Data，仍然是 C++ Data Race / UB。相比 Pointer-based 方案的延迟 delete，固定 Ring 需要**延迟覆盖**。

例：Capacity=8，最新 Seq=12；Reader Pin Epoch=5，依旧可以读取 Seq=5（即使这个 Data 在 Epoch=1 创建）。Writer 发布 Seq=13 会复用 Seq=5 的 Slot：必须 Invalidate Seq 5、推进 Epoch=6，并等待旧 Reader 退出，再写新 Data。在等待期间 Reader 已取得的旧 Data 指针仍安全，**新查 Seq 5 可以返回 Not Found**。

**不能假设 latest=12 就保证 [5,12] 每个 Seq 都随时可读**：并发 Writer 可以先 invalidate 最旧 Slot，再等待 Grace Period，期间最新已完成发布 Seq 仍是 12，但 Seq 5 查询可能已经失效。Retention Window 是容量/策略的约束，不是当前瞬间全窗口无缺口的 Snapshot 保证。

### 5.5 为什么 Reader 的新 Epoch 可以保护旧数据？SC 时序证明

下例专指本项目教学实现的单 Writer、固定 Reader、关键操作全部 seq_cst 的协议；它与经典 3-Epoch EBR 的算法细节不同。

~~~text
Reader                                Writer
R1  load(globalEpoch) -> e           W1  invalidate old slot
R2  readerEpoch.store(e)             W2  advance globalEpoch -> T
R3  load(slot.publishedSeq)          W3  scan readerEpoch[]
R4  dereference slot.data            W4  overwrite only after grace period
R5  readerEpoch.store(0)
~~~

对 Reader 已经通过 R3 读到旧 Seq 且尚未结束 Guard 的情况：

- R3 看到旧 Seq → 在 SC 总序中 R3 < W1。
- 如果 Reader 公布的新 Epoch e >= T，则 W2 < R1。组合 W2 < R1 < R2 < R3 < W1 < W2，形成循环，不可能。
- 如果 Reader 公布的旧 Epoch e < T，Writer 仍想在 W3 错过它并看到 0，那么 Reader 未退出时 W3 < R2。组合 R2 < R3 < W1 < W2 < W3 < R2，也不可能。
- 如果 Reader 已经完成 R5，Writer 看到 Inactive 是安全的。若 Reader 在 W1 之后才 Load，它会看到 INVALID 或新版 Seq，无法取得旧 Seq。

因此 **旧对象不是在创建时绑定某个 Epoch**。真正的安全保证是：当 Writer 要删除或覆盖对象时，必须与所有可能仍持有旧版本的 Reader 正确完成 Grace Period。

回收条件，在本项目示例定义的 Retirement/Reuse Threshold T 下：

~~~text
Safe(T) = all readers satisfy:
          (readerEpoch[i] == 0) OR (readerEpoch[i] >= T)
~~~

真实 EBR 常使用三 Epoch、批量 Epoch 进展或更复杂的 Pin/Unpin 机制，回收公式不得直接移植。动态 Reader 注册、Slot 重用、Epoch/Seq wrap、异步回调指针逃逸等都需要额外证明。

### 5.6 EBR 的 Validate、Snapshot 和 Memory Ordering

**Validate**：

- HP：Reader 必须在公布某个 Hazard Pointer 后重新验证 Pointer，排除 acquire 与保护发布之间的 UAF 窗口。
- EBR：Reader 先建立保护区间再取 Pointer，因此不需要 HP 式二次 Pointer Validate。
- Replay 无论采用 EBR 还是 HP，都需要 **Seq/Generation 身份检查**；这不是生命期保护的二次验证。

**Snapshot**：Epoch Guard 并不锁定整个 Ring 的逻辑快照。一个 Guard 读取多个 Slot，Writer 可以在它们之间更新索引，出现旧版/新版混合或 Not Found。若要完全一致的历史切片，需要 Snapshot Version、不可变 Snapshot、MVCC 或 Replay Consistency Protocol。

**Memory Ordering**：

- 本项目的代码将重要操作设置为 seq_cst，方便使用一致的全局顺序证明。
- 不能未经证明就把 Pin Store 改为 Release、Pointer Load 改为 Acquire、Scanner Load 改为 Acquire；两个线程可能因 Store Buffering 互相错过公告和更新。
- **并非所有 EBR 实现都必须全部 seq_cst**。成熟 EBR 可用 Acquire/Release 加配对 SC Fences / RMW 或其他已验证协议；Unpin 通常可能采用更弱排序。但务必根据完整算法证明，不能从某一句“EBR 不需要 SC”直接推导具体实现安全。
- 示例每次 Publish 后推进 Epoch 仅为方便教学；真正高吞吐 EBR 通常按批次/Grace Period 推进，避免每条消息都执行 Global Epoch RMW。

### 5.7 Snapshot Lock 类比与固定 Ring Trade-off

**像锁之处**：固定 Ring 全满且 Writer 必须原地覆盖 Data 时，一个被抢占的老 Epoch Reader 可能使 Writer 的下一次 Slot 复用无法完成，甚至它实际读取的是完全不相关的 Slot。对于这种存储布局，Writer **发布新 Data** 和 **回收旧 Slot 使用权** 是同一个操作的两个阶段。

**不像锁之处**：EBR 不提供整个 Ring 的一致性 Snapshot，不禁止 Writer 修改一切不相关的状态；Writer 可以先推进 Epoch，或在空 Slot 写入，也可以在独立 Entry 模型中发布新 Pointer 并延迟释放旧对象。

| 维度 | Pointer-based EBR | In-place Ring EBR | Generation + RefCount CAS Pin |
|---|---|---|---|
| Writer 更新时 | 通常先 Exchange 新对象 | 先 Invalidate，等待 Grace Period | CAS Close，等这个 Slot 的 Pin 归零 |
| 保护粒度 | Epoch 临界区，间接保留多个 Entry | Epoch 临界区，影响多 Slot 复用 | 精确 Slot/Generation |
| 新对象内存 | new / Pool / Segment | 无 Data new，原地覆盖 | 固定 Pool，原地复用 |
| 慢 Reader | retired backlog 可增长 | 容易阻塞顺序 Writer | 阻塞对应 Slot；Writer 最终遇到它也会停 |
| Replay Batch | 一次 Pin 可以读多个 Entry | 一次 Pin 可读多个 Slot | 通常逐 Slot CAS Pin |
| Random Replay | 需要 Index + Seq 检查 | 需要 Seq 检查 | Generation 验证通常并入 CAS |

对于已有 **Selective Fan-out + Fixed Ring + Random Replay** 架构：Live Reader 优先考虑目标 Reader Completed Cursor；Cold Replay 单条随机 Seq，继续以稳定 Slot Metadata 的 Generation + RefCount CAS Pin 为基线；若选择 EBR，应测量老 Guard 停顿对 Writer p99.9/max 的影响。真正希望 Writer 不等待 Replay，通常需要额外的不可变 Snapshot、Overflow Pool 或独立 Copy Archive，而不仅是更换回收算法。

### 5.8 两份完整 C++20 示例及验证记录

本章节有两套可以**独立复制、编译、执行**的程序，而非只有省略了 EpochDomain 的伪代码：

- [ebr_history_seq.cpp](ebr_history_seq.cpp)：Pointer-based History Index，独立不可变 Entry，延迟 delete；测试多个 Seq 退休后的指针仍安全、4 Reader 并发 Batch Lookup。
- [ebr_ring_history_replay.cpp](ebr_ring_history_replay.cpp)：Slot 内嵌 Data，Invalidate → Advance → Wait → Overwrite → Publish；测试 **读取早于 Reader Epoch 创建的旧 Seq**、Writer 阻塞直到 Guard 退出、4 Reader 并发随机历史查询。

~~~bash
g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_history_seq.cpp -o ebr_history_seq
./ebr_history_seq

g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    ebr_ring_history_replay.cpp -o ebr_ring_history_replay
./ebr_ring_history_replay
~~~

**本次更新验证**：GCC 14.2 两份程序按以上配置编译、各运行 12 次均通过；额外用 ASan + UBSan 各运行 3 次未发现错误。测试不构成形式化的 C++ Memory Model 证明，本次未执行 TSAN/Model Checking。仅作为教学和 Benchmark 起点，不应未经额外审查用于生产交易系统。

**延伸阅读**：Crossbeam Epoch [官方文档](https://docs.rs/crossbeam-epoch/latest/crossbeam_epoch/)、DPDK [QSBR Programmer Guide](https://doc.dpdk.org/guides/prog_guide/rcu_lib.html)、C++ [Memory Order Reference](https://en.cppreference.com/w/cpp/atomic/memory_order.html)。


### 5.9 EBR 典型应用与 Pointer / Embedded Storage 的真正区别

本轮讨论的最终结论：**EBR 并非“只适合 Pointer、不适合 Embedded Data”；决定优势能否发挥的是：新对象的发布（Publication）能否与旧存储的回收／复用（Reclamation/Reuse）解耦。**

在允许新旧版本并存的设计中，Writer 可以先发布新版本，让已经获取旧版本的 Reader 继续安全使用旧对象；等到 Grace Period 结束再释放或重用旧存储。EBR 的读侧开销还能摊销到临界区内的多个对象访问。它管理**生命周期**，不替代并发容器的算法正确性、可见性协议或全局逻辑快照。

#### 5.9.1 典型应用

| 场景 | EBR 如何工作 | 优势与风险 |
|---|---|---|
| Lock-free Linked List / Skip List / Tree | Unlink 动态节点，延迟释放/复用 | Reader 在一个 Guard 内走过多个节点；仍需独立解决 CAS/逻辑 ABA |
| Concurrent Hash Map | Erase/Replace/Rehash 后延迟回收 Node 或整张旧 Table | 读侧避免每节点 RefCount；Rehash 语义需另外证明 |
| Immutable Config / Routing Table / Instrument Metadata | 建立新版，原子替换根 Pointer，Retire 旧版 | 高频读、低频写、版本不可变；需考虑复制和内存峰值 |
| Lock-free Queue 动态节点 | Dequeue/Unlink 后延迟回收 Node | 消除节点的 UAF；不自动保证 Queue 的线性化或 ABA 安全 |
| Preallocated Object Pool | 新版本写到别的 Free Slot，旧 Slot 经过 Grace Period 后回 Pool | 不需要每次 new/delete，但必须有空闲容量 |
| 固定 Slot Ring 原地覆盖 | 旧 Seq invalidated 后，Writer 等待 Grace Period 才能覆盖旧 Data | 正确但限制 Writer 前进，通常不是 EBR 最大优势的场景 |

**相关而非等同**：网络和 Packet Processing 中的 RCU/QSBR 也处理读多写少的数据结构、延迟回收与 Grace Period；例如 DPDK QSBR 的 Reader 报告 Quiescent State，与本页的简化 EBR Reader Epoch 算法不是同一个实现。

#### 5.9.2 案例 A：Pointer-based Immutable Publication

下面是生命周期 API 的**示意片段**，需要与同一 Epoch Domain 的真实 pin/retire 实现配套；不是可单独编译的 EBR 库。

~~~cpp
std::atomic<const Config*> current;

// Reader：Guard 期间的 *整个* Config 版本不可变
auto guard = epoch.pin(readerId);
const Config* cfg = current.load(std::memory_order_acquire);
use(*cfg);  // 最后一次解引用必须在 Guard 结束之前

// Writer：
const Config* next = buildImmutableConfig();
const Config* old = current.exchange(next, std::memory_order_acq_rel);
epoch.retire(old);  // 不是立即 delete
~~~

核心并非“用了 Pointer 就必然安全”，而是 **current 作为发布入口**与 **old、next 分别占用的内存**互相独立。Reader 在 Guard 内可以完成很多基于同一 cfg 的查询，Writer 也可以继续构造与发布新版本。

EBR 不保证多个独立 Pointer Load 得到同一时间点的数据。若要求同一次业务计算使用一致的 Config，通常只在 Guard 内 Load 一次 cfg，并从这个不可变 Root 访问所有关联字段。

#### 5.9.3 案例 B：Embedded Data 仍然可以配合 EBR（预分配 Pool）

~~~cpp
struct PoolSlot {
    Data data;   // Embedded, non-atomic
};
std::array<PoolSlot, 4096> pool;

// 假设：Free List 仅包含已经通过 Grace Period 的 Slot。
// Reader：先 Pin，再从原子 Index 得到 PoolSlot*，然后访问 Data。
// Writer：取另一个安全空闲的 PoolSlot，写 Data，原子发布新地址，
//         将旧 PoolSlot 退休，未来 Grace Period 结束才回 Free List。
~~~

假设 Reader 正在访问 pool[10]，Writer 可以在 pool[20] 初始化新版并发布 Index；pool[10] 即使被移出 Index，也不能在旧 Guard 结束前复用。这没有每条 Data 的 new/delete，却与不可变 Pointer 版本一样实现了 **Publication/Reclamation 解耦**。

如果 Pool 的空闲 Slot 耗尽，Writer 也会遇到 Stall、Overflow 或 Backpressure；EBR 本身不创造额外容量。

#### 5.9.4 案例 C：Fixed Ring 的 seq % Capacity 带来硬约束

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0};
    Data data; // Embedded, non-atomic
};

Slot& slot = ring[nextSeq % Capacity];
// Writer 不能任意选择另一块尚未退休的内存存放 nextSeq
~~~

在 Ring 满载、每条新 Seq 都必须原地复用已发布 Slot 的前提下：

~~~text
Pointer / Free-Pool:
  construct B elsewhere
    -> publish B
    -> retire A
    -> recycle A after Grace Period

Fixed In-place Ring:
  invalidate A
    -> wait Grace Period
    -> overwrite the same Slot with B
    -> publish B
~~~

此时 Writer 的“更新已发布 Data”和“回收旧 Slot 的使用权”是**同一次发布流程不可分的阶段**。旧 Reader 读取的普通 Data 与 Writer 的覆盖绝不能并发发生，否则是 C++ data race / UB。固定地址没有被 free，也无济于事。

**Pointer 不是本质条件**：Pool Slot 可以是嵌入式对象，只要不同版本能使用不同地址，就仍适合 EBR。反过来，即使用 Data* 指向固定 Ring，Writer 直接重写该地址，也不会自动获得 Pointer-based EBR 的好处。

同样须区分：
- Writer 推进 Global Epoch：通常不需要等 Reader；
- Writer 写入从未发布的新空 Slot：不需等待旧对象的 Grace Period；
- Writer 换指针发布独立新版：可以延迟回收旧对象；
- Writer 原地覆盖已发布 Slot：需先保证旧 Reader 都已离开相应受保护区间。

#### 5.9.5 “像 Snapshot Read Lock”——准确的部分与不准确的部分

Reader Pin 的 Epoch 不是快照版本，不会冻结全 Ring；Reader 可能在同一 Guard 内看到来自不同发布时间点的 Slot，甚至看到已经失效的 Seq。

但对已满的固定 Ring 而言，**复用被旧 Guard 可能访问过的 Slot**必须等待 Grace Period，所以一个很慢或被 OS 抢占的 Reader 可能令 Writer 在尝试复用**无关的 Slot**时也阻塞；这是 EBR 的粗粒度、时间区间保护成本。多轮 Writer 发布最终可能被同一慢 Guard 阻碍，尤其在单 Writer 必须按 Seq 顺序提交时。

~~~text
Ring Slots:  [Seq 100] [Seq 101] [Seq 102] [Seq 103]
Reader:       Pin Epoch 10; 实际只读 Seq 100

EBR：
    Writer 要覆盖 Seq 101 的 Slot，也可能被该 Reader 的旧 Epoch 阻止。

Per-Slot CAS Pin：
    Writer 一般可覆盖未被 Pin 的 Seq 101 Slot；
    真正遇到被 Pin 的 Seq 100 Slot 才需要等它的 RefCount 归零。
~~~

不过 CAS Pin 也不保证 Writer 永不阻塞：严格顺序发布时，最终遇到真正被 Pin 的 Slot 仍须等待、溢出或丢弃，取决于业务约束。CAS Pin 必须同时正确处理 Slot Generation、Closing/禁止新 Pin、RefCount 和复用时的 Memory Ordering，不能只用一个孤立的 RefCount++ 代替完整获取保护的协议。

#### 5.9.6 选型原则与 HFT 场景对应

| 场景 / 约束 | 倾向方案 |
|---|---|
| Read-mostly Config、Instrument Static Data、Venue Routing、ACL | Immutable Root + EBR/QSBR/RCU 风格延迟回收 |
| Concurrent List / Hash Node，多对象短时间遍历 | EBR 值得评估，仍要验证容器算法 |
| 预分配 Object Pool，有足够安全的备用 Slot | EBR + Retire/Free List 可行 |
| Fixed Ring，严格按 seq % Capacity 原地覆写 | 更倾向 Generation + RefCount CAS Pin |
| Replay 单条按 Seq 随机查询，热路径尾延迟敏感 | 更倾向 Per-Slot CAS Pin；测量 CAS 竞争及慢 Reader |
| Selective Fan-out Live Reader 交付与完成确认 | Per-Target Completed Cursor / ACK，不直接与 Replay Pin 混为一谈 |
| 长时间 Replay、历史跨度超过 Ring Capacity | Copy/Segment Archive 或额外不可变存储 |

建议的设计判断顺序：

1. **是否允许新旧版本共存于不同存储？** 如可以，EBR 的发布与回收解耦很有价值。
2. **Reader 能否短时 Pin、并在一个 Guard 内访问多个对象？** 如可以，EBR 的成本更易摊销。
3. **慢 Reader 是否允许推迟回收，并可能消耗额外内存／Pool 容量？** 如不允许，优先更精确的保护和有界方案。
4. **核心目标是 Reader 吞吐还是 Writer p99.9/max？** 在 Fixed Ring 上尤其应测长 Guard、Reader 被抢占、Grace Period Stall 和备用容量用尽。

**最终结论：EBR 更适合可以延迟复用旧存储的架构；Pointer-based 只是常见表达形式，并非必要条件。对本项目 Fixed Ring + Embedded Data + Random Replay，Per-Slot Generation + RefCount CAS Pin 通常更贴切。** 这是一条有充分架构依据的选型倾向，**不是**“只要可以解耦就一定该用 EBR”的数学充分必要条件；真实表现仍依赖缓存、NUMA、竞争、Guard 长度和实测尾延迟。


## 6. 模型 4：QSBR（Quiescent-State-Based Reclamation）

不一定逐次 pin/unpin。Reader 在明确的事件边界报告：当前 **没有任何之前取得的旧版本指针仍在使用**。Writer 在替换 pointer 后发出 grace-period token，等待各相关 Reader 报告 token 或更晚。

~~~cpp
// 04_qsbr.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
struct Config { int version; };
int main() {
    std::atomic<Config*> current{new Config{0}};
    std::atomic<unsigned long long> token{1};
    std::array<std::atomic<unsigned long long>, 2> quiescent{};
    for (auto& q : quiescent) q.store(0);
    std::atomic<bool> go{false};
    auto reader = [&](int id) {
        while (!go.load(std::memory_order_acquire)) {}
        for (int n = 0; n < 30000; ++n) {
            Config* p = current.load(std::memory_order_seq_cst);
            assert(p && p->version >= 0);
            // 在此之后绝不再使用 p
            auto observed = token.load(std::memory_order_seq_cst);
            quiescent[id].store(observed, std::memory_order_seq_cst);
        }
        // 最终 QS：线程完成后不会再访问 current 的旧对象
        quiescent[id].store(token.load(std::memory_order_seq_cst),
                            std::memory_order_seq_cst);
    };
    std::thread a(reader, 0), b(reader, 1);
    go.store(true, std::memory_order_release);
    struct Retired { Config* p; unsigned long long grace; };
    std::vector<Retired> retired;
    auto collect = [&] {
        auto out = retired.begin();
        for (auto item : retired) {
            bool safe = true;
            for (auto& q : quiescent)
                if (q.load(std::memory_order_seq_cst) < item.grace)
                    safe = false;
            if (safe) delete item.p;
            else *out++ = item;
        }
        retired.erase(out, retired.end());
    };
    for (int v = 1; v <= 1000; ++v) {
        Config* old = current.exchange(new Config{v},
                                       std::memory_order_seq_cst);
        auto grace = token.fetch_add(1, std::memory_order_seq_cst) + 1;
        retired.push_back({old, grace});
        if (v % 16 == 0) collect();
    }
    a.join(); b.join();
    // 上面的终态报告可能发生在 writer 最后一个 token 之前；
    // join 保证两个 Reader 已退出，不可能继续访问旧对象。
    for (auto item : retired) delete item.p;
    delete current.load();
}
~~~

**生产版必须实现**：register/online/quiescent/offline/unregister 的正确同步，Reader offline 前没有逃逸指针，后台回收、不让 busy-poll 快路径扫描 retired list。Reader 停止后可以通过 join/注销将其从参与者集合移除，不应让过期的 token 永久阻塞。切勿在仍被异步回调使用时报告 QS。

### 6.1 EBR vs QSBR：共同目标与核心区别

**两者非常接近，不是两套完全不同的回收思想。** EBR 和 QSBR 都是用 Grace Period 判断：从共享数据结构移除的旧版本，是否已经不可能再被某个 Reader 继续使用。区别主要在于 Reader 如何声明**保护区间**与**安全进度**。

| 问题 | EBR（典型 Pin/Unpin 模型） | QSBR（典型 Online/QS/Offline 模型） |
|---|---|---|
| Reader 进入保护 | 显式 Pin：读取并宣布一个 Epoch | Online 后成为持续参与回收检查的 Reader |
| Reader 表示使用结束 | Unpin：声明不再使用当前 Guard 的任何旧对象 | QS：声明截至该安全点，之前获得的受保护引用均已结束 |
| Reader 空闲 | Unpin 后为 Inactive，通常不阻止新 Grace Period | Online 但不继续 QS，后续 Grace Period 可能一直等待 |
| 一个 Batch 多次 Lookup | 一个 Guard 包住 Batch | Online 后直接 Lookup，Batch 结束报告一次 QS |
| Writer 何时可以回收 | 相关旧 Epoch Reader 都已退出或进入足够新的保护 Epoch | 相关 Online Reader 都已经越过目标 QS，或合法进入 Offline |
| Writer 可先发布 Immutable 新版 | 可以 | 可以 |
| 回收进度受谁影响 | 最慢的仍 Pin 的 Reader | 最慢的 Online 且未报告足够新 QS 的 Reader |
| 固定 Ring Embedded Data 原地覆盖 | 先 Invalidate，再等待 Grace Period，之后才能写旧 Slot | 同样需要 |
| 典型集成方式 | RAII Scoped Guard、通用容器 | 长驻 Busy-Poll Worker、天然 Event/Batch 边界 |

**重要术语修正**：

- Online **类似“开始长期参与读保护”**，但不等价于 EBR Pin 一个具体 Epoch：Online 是 QSBR Reader 的参与状态，报告 QS 后仍保持 Online。
- Offline **类似退出读侧保护域**，但必须先确认没有任何旧引用继续使用，并正确执行协议中的注销/在线状态同步。
- QS **逻辑上类似“结束上一段保护区间，然后继续参加下一段”**，可以用“Unpin + Pin”的思想来理解，但它并不真的原子地执行这两步，更不保证每次都递增 Global Epoch。
- 两者都**不是 RWLock 或一致性快照锁**：Writer 仍可以发布新 Pointer；保护的是旧对象生命期，而不是冻结所有数据。

### 6.2 两段对称的核心代码：完全相同的 Immutable Data 发布模型

公共数据（下面是展示协议的核心代码片段，不包含完整 Reader 注册、异常处理及线程关闭。完整 C++20 EBR 示例见前文 ebr_history_seq.cpp；本节开头还给出了可运行的固定 Reader QSBR 例子）：

~~~cpp
struct Data {
    uint64_t seq;
    uint64_t value;
};

constexpr auto SC = std::memory_order_seq_cst;
std::atomic<const Data*> current{nullptr};

// 假设一个 Writer，两个固定 Reader，Seq/Epoch/Token 均不会回绕。
// Data 发布后不可修改。process 不保存指针给异步回调。
~~~

**代码 A：EBR。一次处理周期显式 Pin → Lookup → Process → Unpin。**

~~~cpp
std::atomic<uint64_t> globalEpoch{1};
std::array<std::atomic<uint64_t>, 2> activeEpoch{};
// activeEpoch[i] == 0 表示该 Reader 不在保护区间

void ebrRead(size_t id) {
    // 对同一 Reader ID，禁止并发 Pin 或重叠 Guard
    auto e = globalEpoch.load(SC);
    activeEpoch[id].store(e, SC);          // Pin；必须先于 Pointer Load

    const Data* p = current.load(SC);
    if (p) process(*p);

    activeEpoch[id].store(0, SC);          // Unpin，最后一次解引用之后
    // 生产代码应使用 RAII Guard，保证所有退出路径都会 Unpin
}

void ebrPublish(const Data* next) {
    const Data* old = current.exchange(next, SC); // Unlink old
    const auto threshold = globalEpoch.fetch_add(1, SC) + 1;

    // 忽略 absent old 的回收；下面为简化选择同步等待
    bool safe = false;
    while (!safe) {
        safe = true;
        for (auto& r : activeEpoch) {
            const auto e = r.load(SC);
            if (e != 0 && e < threshold) safe = false;
        }
    }
    delete old;                            // Grace Period 已完成
}
~~~

真实生产实现通常 Retire 后延迟/批量回收，而不是每次 Publish 同步等待。这个版本的关键 Memory Ordering 依赖全部 SC；不可随意改为仅 Acquire/Release。

**代码 B：QSBR。一次 Online，重复 Lookup → Process → Report QS，最后 Offline。**

~~~cpp
std::atomic<uint64_t> globalToken{1};
std::array<std::atomic<uint64_t>, 2> passedToken{};
// 此处仅演示两个事先注册且始终 Online 的固定 Reader。
// 对动态 Online/Offline 必须使用完整 QSBR 库的状态协议。

void qsbrWorker(size_t id, std::atomic<bool>& running) {
    // 在本固定 Reader 模型下，启动前已成为 Online 参与者
    while (running.load(std::memory_order_acquire)) {
        const Data* p = current.load(SC);
        if (p) process(*p);

        // 真实 QS：先结束所有旧引用，再报告已越过的 Token
        const auto t = globalToken.load(SC);
        passedToken[id].store(t, SC);
    }
    // 退出前：加入 Offline / Unregister 协议，或由 Writer join。
}

void qsbrPublish(const Data* next) {
    const Data* old = current.exchange(next, SC); // Unlink old
    const auto threshold = globalToken.fetch_add(1, SC) + 1;

    // 两个固定 Reader 在 Writer 运行期间必须一直报告 QS
    bool safe = false;
    while (!safe) {
        safe = true;
        for (auto& p : passedToken) {
            if (p.load(SC) < threshold) safe = false;
        }
    }
    delete old;                            // Grace Period 已完成
}
~~~

**两段 Writer 的形状几乎相同：Exchange → Start Threshold → Wait/Collect → Delete。** 真正不同的是各自 Reader Record 的含义：

- EBR 的 activeEpoch 保存 Reader **当前正处于哪个保护区间**；0 表示明确不活跃。Writer 可以忽略 Inactive Reader。
- QSBR 的 passedToken 保存 Reader **已经越过哪个安全边界**。Reader 仍 Online 时，旧 Token 只能证明过去的某次 QS，不能证明当前新启动的 Grace Period 已经完成。

注意：代码 A 和 B 使用相同的 current 只是为**独立对照**；它们是两种互斥的示例运行方式，不应把两个回收者同时挂到一个 Pointer 上。QSBR 的 Online/Offline/Registration 并发同步、Retired List 批处理、关闭及异常安全在真实系统中不可省略。

### 6.3 “EBR 临时读锁；QSBR 持续参与、不断滑动安全边界”

这是有用的**心理模型**，但不是标准意义上的锁。

~~~text
EBR:
  Inactive --Pin(10)--> [Read A] --Unpin--> Inactive
  Inactive --Pin(11)--> [Read B] --Unpin--> Inactive
            ^显式开始              ^显式结束

QSBR:
  Offline --Online--> [Read A] --QS(10)--> [Read B] --QS(11)--> ... --Offline
                      ^       ^安全边界前移；仍然 Online
                      长期参与 Writer 的 Grace Period 检查
~~~

可将 QS 想象为 **结束上一段旧引用的保护，并将 Reader 已安全越过的时间边界向前推进**。但是 QS 调用后，Reader 仍然 Online；下一次 Lookup 无需重新 Online。

**Writer 的典型等待场景**：

~~~text
时间         EBR Reader                  QSBR Reader                 Writer
T1           Pin Epoch 10                Online / Read A
T2           Read A                      Process A                   Exchange A -> B
T3           Process A                   Process A                   Start threshold 11
T4           Unpin (= Inactive)          QS(11)                      Both: grace done
T5           Sleep 10s                   Sleep 10s (still Online)    Start threshold 12
T6           不阻止新 GP                 不再报告 QS                 EBR 可继续；QSBR 等待
T7           --                          QS(12) / Offline            QSBR GP 才能完成
~~~

严格说，T4 的 QS(11) 必须是在 Writer 发布 Token 11 之后、且 Reader 不再使用之前取得的 Data 时报告。**QS(10) 即使刚发生，也不能满足未来才启动的 Token 11。** Writer 等的是后续安全点，而不是“此 Reader 曾经调用过 QS”。

对于 EBR，在 T5 Reader 已经 Unpin 的情况下，它睡眠期间不阻止 T5 之后启动的 Grace Period；QSBR Reader 若保持 Online 且不再报告 QS，则可能一直阻止后续回收，哪怕它实际没有读取任何对象。若预计阻塞很久，应在不保留引用时 Offline，并在恢复 Lookup 之前重新 Online。

### 6.4 QS 后再次 Lookup 到同一个 Data：合法，但必须重新获取引用

~~~text
Reader                              Writer
Lookup A; Process A
QS(10)
Lookup A; Process A again            // A 如果仍在 Index，则完全合法
QS(10 or newer)

Writer 要回收 A：
    Unlink A -> Start Token T -> 等待所有相关 Reader 的 QS >= T -> Reclaim A
~~~

**QS 是时间边界，不是对象销毁信号。** 如果 A 仍可从 History Index 获得，可以在不同 QS 区间重复查到同一个 A。若 Writer 已经 Unlink A，新 Lookup 不允许再获得旧版 A；此前取得的指针只可持续使用到当前保护区间结束。

错误做法：

~~~cpp
const Data* p = history.lookup(100);
use(*p);
quiescent(readerId);
use(*p); // 错误：已跨越 QS，旧指针不再有生命周期保护
~~~

正确方式是 QS 之后通过受同一回收协议约束的 Index **重新 Lookup、重新检查 Seq/Generation**。在重复查询之间，Writer 可能已经淘汰旧 Seq，此时返回 Not Found 很正常。EBR 中 Unpin 以后重新 Pin 再 Lookup 也是同样逻辑。

**历史查询是否成功**依然分两层：

~~~text
HistoryAvailable(seq)   // Index / Ring / Archive 仍保留该 Seq
AND
LifetimeProtected(seq)  // 当前 Guard 或未跨 QS 的区间能安全访问
~~~

EBR/QSBR 只解决后者，不凭空保存历史。

### 6.5 固定 Ring Embedded Data：QSBR 与 EBR 有相同的基本限制

~~~cpp
struct Slot {
    std::atomic<uint64_t> publishedSeq{0};
    Data data; // 非原子
};
auto& slot = ring[seq % Capacity];
~~~

对于 Writer 的下一圈原地覆盖：

~~~text
EBR:  Invalidate Old Seq -> Advance Epoch -> Wait old Guards -> Overwrite
QSBR: Invalidate Old Seq -> Start Token  -> Wait new QS     -> Overwrite
~~~

两者都不能在 Reader 仍可能读旧 Data 时覆写普通 Data。**没有 delete，不代表不需要防止复用竞态。** 两者都属于粗粒度时间保护，可能因某个不访问目标 Slot 的慢 Reader 而阻塞 Writer。QSBR 只是更容易在 Busy-Poll Loop 的 Batch 边界报告 QS，并不将保护粒度变为单 Slot。

| 对本项目的工作负载 | 首选方向与理由 |
|---|---|
| 常驻 Busy-Poll Worker + Immutable Pricing Config / Routing Table | QSBR：一次 Online，按稳定的 Batch/Loop 边界 QS；需测量 Reader 抢占与 Grace Period 的尾延迟 |
| 不规则函数调用、短 RAII 临界区、通用并发动态节点容器 | EBR：Pin/Unpin Scope 明确，异常/提前退出更易约束 |
| Fixed Ring + Embedded Data + Random Seq Replay | Per-Slot Generation + RefCount CAS Pin：保护具体 Slot/Generation，减少无关 Reader 的阻塞 |
| 长跨度 Replay、Writer 不能等待 | 独立 Immutable Archive / Copy / Segment + 明确的容量和回收策略 |

**性能不要机械地认为 QSBR 一定更快。** 简化 SC 模型中 EBR 一次操作通常包含 Pin Global Load、Pin Record Store、Unpin Store；QSBR 每批 QS 通常有 Global Token Load、Progress Store。但 EBR 同样可以整 Batch Pin 一次，成熟库可能优化 Memory Ordering；真正要量的是吞吐、Reader cache miss、Writer p99.9/max、长期 Online 不报告 QS、Worker 被 OS 抢占与 shutdown 时的回收进度。

### 6.6 工程注意事项与延伸

1. **Online 并不是实际持有 RWLock**，且不能把 Quiescent 当作普通的计数器更新；QS 前必须真实结束对受保护旧对象的引用。
2. **QSBR 不要求不停调用 QS**：只在有新的 Grace Period 时需要足够新的安全点才能推进回收；长期 Online 不 QS 的 Reader 会使相关回收积压。
3. **Writer 的发布与回收不总是一起阻塞**：Immutable Pointer 版可以发布新 Pointer，然后异步退休旧对象；Fixed Ring 原地覆盖则需要在重写之前等待。
4. **线程上线/下线可能和 Token 推进并发**：不能单纯用一个 bool online 数组搭配普通 acquire/release 就声称安全，须采用已验证的 QSBR Online/Offline/Registration 协议。
5. **Memory Ordering**：这里 SC 片段供证明与教学。真实 DPDK QSBR、liburcu QSBR 等有各自的屏障与 Token/Registration 规则，不应把上述教学片段直接替换进生产系统。
6. **来源与进一步阅读**：DPDK RCU QSBR Programmer Guide https://doc.dpdk.org/guides/prog_guide/rcu_lib.html；liburcu https://liburcu.org/；Crossbeam Epoch https://docs.rs/crossbeam-epoch/latest/crossbeam_epoch/。


## 7. 动态 Reader：注册表本身也是并发对象

| 技术 | 加入 | 离开 | 常见做法 |
|---|---|---|---|
| Atomic shared_ptr | 无需注册 | 最后一个 owner 释放 | atomic shared ownership |
| HP | 先注册 Hazard Record，再读取 | 清 Hazard，归还 Slot | 预分配或稳定 Record Pool |
| EBR | 注册 inactive Participant，后 pin | unpin、转移 retired list、注销 | 固定 Slot / Registry 锁 |
| QSBR | register → online | quiescent/offline → unregister | 固定容量 Slot Pool + generation |

注意：Reader A 离开后立刻把相同 ID/Slot 给 Reader B，如果 Writer 手里的旧 Reader Record 还在等待，必须建立 generation / membership handoff；不能把 B 的新进度误用作 A 的已完成证明。

一个实用实现是 **慢路径用 mutex 管理 Registry，热路径完全不加锁**。Reclaimer 可在锁内快照稳定的 ReaderState 指针，必要时为 metadata 设计单独的生命周期。不要持 Registry 锁长时间等待某个 Reader 的 grace period。线程异常退出时不能强制把它标为 offline 并立刻回收潜在仍被访问的对象。

## 8. 模型 5：固定容量 SPSC 与 MPMC（按值消息）

### 8.1 SPSC：可复用的最小实现

后续选择性 Fan-out 代码共用这个头文件，按下列内容保存为 **mini_spsc.hpp**：

~~~cpp
// mini_spsc.hpp
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
template <class T, std::size_t N>
struct Spsc {
    static_assert(N > 0);
    std::array<T, N> data{};
    std::atomic<std::size_t> write{0}, read{0};
    bool push(const T& x) { // 单 Producer
        auto w = write.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == N) return false;
        data[w % N] = x;
        write.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& x) { // 单 Consumer
        auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        x = data[r % N];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
};
~~~

使用约束：一个 Queue 永远只有一个 Producer 和一个 Consumer；不得销毁还在被访问的 Queue；T 在消费前不可被 Producer 擅自重写。生产版注意计数回绕、padding、cache-line alignment、批处理与异常安全。

### 8.2 MPMC：固定容量 Array + 每 Slot Sequence

这是经典 bounded sequence-cell MPMC 思路的独立 C++20 示例；无动态链表 Node，因此**队列内部不需要 HP/EBR**：

~~~cpp
// 05_mpmc.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
template<class T, std::size_t N>
class Mpmc {
    static_assert(N >= 2 && (N & (N - 1)) == 0);
    struct Cell {
        std::atomic<std::size_t> seq;
        T value{};
    };
    std::array<Cell, N> cells{};
    alignas(64) std::atomic<std::size_t> put{0};
    alignas(64) std::atomic<std::size_t> get{0};
public:
    Mpmc() {
        for (std::size_t i = 0; i < N; ++i)
            cells[i].seq.store(i, std::memory_order_relaxed);
    }
    bool push(T value) {
        std::size_t pos = put.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells[pos & (N - 1)];
            auto s = c.seq.load(std::memory_order_acquire);
            auto dif = static_cast<std::intptr_t>(s) -
                       static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                if (put.compare_exchange_weak(pos, pos + 1,
                          std::memory_order_relaxed)) {
                    c.value = value;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) return false; // 满
            else pos = put.load(std::memory_order_relaxed);
        }
    }
    bool pop(T& result) {
        std::size_t pos = get.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells[pos & (N - 1)];
            auto s = c.seq.load(std::memory_order_acquire);
            auto dif = static_cast<std::intptr_t>(s) -
                       static_cast<std::intptr_t>(pos + 1);
            if (dif == 0) {
                if (get.compare_exchange_weak(pos, pos + 1,
                          std::memory_order_relaxed)) {
                    result = c.value;
                    c.seq.store(pos + N, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) return false; // 空
            else pos = get.load(std::memory_order_relaxed);
        }
    }
};
int main() {
    Mpmc<int, 64> q;
    std::atomic<int> consumed{0};
    std::atomic<long long> sum{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < 2; ++p)
        threads.emplace_back([&, p] {
            for (int k = 1; k <= 1000; ++k)
                while (!q.push(p * 1000 + k)) std::this_thread::yield();
        });
    for (int c = 0; c < 2; ++c)
        threads.emplace_back([&] {
            int x;
            while (consumed.load() < 2000) {
                if (q.pop(x)) {
                    sum.fetch_add(x);
                    consumed.fetch_add(1);
                } else std::this_thread::yield();
            }
        });
    for (auto& t : threads) t.join();
    assert(consumed == 2000 && sum == 2001000);
}
~~~

**提醒**：固定 Ring 的 Cell 状态和普通消息内存由 Sequence Protocol 保护；若 payload 是多个 Consumer 共享的 Data*，其对象生命周期仍须另外管理。

## 9. 模型 6A：多个 SPSC + Shared Data Pool + Selective Fan-out

每条 Data 只进入目标 Reader 的 SPSC；Data 在 Pool 中存一份。Writer 记录每个目标 Reader 所要求完成的 **本地 Queue 序号**。Writer 是唯一 Reclaimer，无每 Data 的 Reader-side atomic RMW；非目标 Reader 无需扫描或确认消息。

> **关键适用前提**：下游在将本地 Completed Cursor 推进之前，必须结束该 Data 的**全部**原始引用使用。如果 Consumer 把 Data Handle 异步传给 P3，不能在“转发成功”时就报告 Safe Completed；应改用 Transitive Completion Gating、Reference Counting 或 Completion Ack。详见 §19。

~~~cpp
// 06a_selective_spsc_pool.cpp  (需要同目录 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>
struct Data { uint64_t seq{}; int value{}; };
struct Item { std::size_t slot{}; uint64_t seq{}, order{}; };
struct Retired {
    std::size_t slot{};
    uint32_t targets{};
    std::array<uint64_t,3> required{};
};
int main() {
    constexpr std::size_t N = 3, POOL = 8;
    std::array<Data, POOL> pool{};
    std::array<Spsc<Item, 16>, N> qs;
    std::array<std::atomic<uint64_t>, N> completed{};
    for (auto& c : completed) c.store(0);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (std::size_t i=0; i<N; ++i) {
        readers.emplace_back([&,i] {
            Item x;
            for (;;) {
                if (qs[i].pop(x)) {
                    assert(pool[x.slot].seq == x.seq);
                    assert(pool[x.slot].value == int(x.seq * 10));
                    completed[i].store(x.order, std::memory_order_release);
                } else if (done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    }
    std::vector<std::size_t> freeSlots;
    for (std::size_t j=0; j<POOL; ++j) freeSlots.push_back(j);
    std::deque<Retired> pending;
    std::array<uint64_t,N> produced{};
    auto reclaim = [&] {
        for (auto it=pending.begin(); it!=pending.end();) {
            bool safe=true;
            for (std::size_t i=0;i<N;++i)
                if ((it->targets & (1u<<i)) &&
                    completed[i].load(std::memory_order_acquire)
                        < it->required[i]) safe=false;
            if (safe) {
                freeSlots.push_back(it->slot);
                it=pending.erase(it);
            } else ++it;
        }
    };
    for (uint64_t seq=1; seq<=100; ++seq) {
        while (freeSlots.empty()) { reclaim(); std::this_thread::yield(); }
        auto slot=freeSlots.back(); freeSlots.pop_back();
        pool[slot]={seq,int(seq*10)};
        uint32_t mask = 1u << (seq%N);
        if (seq%4==0) mask |= 1u << ((seq+1)%N);
        Retired r{slot,mask,{}};
        for (std::size_t i=0;i<N;++i) if (mask & (1u<<i)) {
            auto order=++produced[i];
            r.required[i]=order;
            while (!qs[i].push(Item{slot,seq,order}))
                std::this_thread::yield();
        }
        pending.push_back(r);
        reclaim();
    }
    while (!pending.empty()) { reclaim(); std::this_thread::yield(); }
    done.store(true,std::memory_order_release);
    for (auto& t:readers) t.join();
    assert(freeSlots.size()==POOL);
}
~~~

**条件**：Reader 完成进度在最后一次使用对象之后发布；Queue 不会重复投递旧 Handle；单 Writer 维护 free/pending。多 SPSC 的多目标发布不是原子事务：若其中一条 Queue 满而另一个已经投递，必须显式实现预检容量、重试或定义部分投递语义。

## 10. 模型 6B1：共享 Ring + Per-Slot Pending Mask（目标 Reader ACK）

每个 Slot 的 pending bits 表示尚未完成的目标 Reader。只给目标 Reader 的 SPSC 投递 global seq；Reader 处理完成对该 Slot fetch_and 清除自己 bit；Writer 观察 pending==0 后才允许覆盖。这个模型与上一节共享 Pool 的根本区别：按固定 Ring Slot 顺序复用；一个慢目标可能使 Writer 卡在该 Slot 上。

~~~cpp
// 06b1_selective_pending_mask.cpp  (需要 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
struct Data { uint64_t seq{}; int value{}; };
struct Slot {
    Data data{};
    std::atomic<uint32_t> pending{0};
};
int main() {
    constexpr std::size_t N=3, CAP=8;
    std::array<Slot,CAP> ring;
    std::array<Spsc<uint64_t,16>,N> qs;
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (std::size_t i=0;i<N;++i)
        readers.emplace_back([&,i] {
            uint64_t seq;
            for (;;) {
                if (qs[i].pop(seq)) {
                    auto& s=ring[seq%CAP];
                    assert(s.data.seq==seq);
                    assert(s.data.value==int(seq*10));
                    // 最后一次访问 s.data 后 ACK
                    s.pending.fetch_and(~(1u<<i),
                                        std::memory_order_release);
                } else if(done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    for (uint64_t seq=1;seq<=100;++seq) {
        auto& slot=ring[seq%CAP];
        while(slot.pending.load(std::memory_order_acquire)!=0)
            std::this_thread::yield();
        uint32_t mask = 1u<<(seq%N);
        if(seq%4==0) mask |= 1u<<((seq+1)%N);
        slot.data={seq,int(seq*10)};
        slot.pending.store(mask,std::memory_order_release);
        for(std::size_t i=0;i<N;++i) if(mask&(1u<<i))
            while(!qs[i].push(seq)) std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);
    for(auto& t:readers)t.join();
    for(auto& s:ring) assert(s.pending.load()==0);
}
~~~

ACK 是 **每个目标 Reader / 每条消息一个原子 RMW**，可能导致同一 Slot cache-line 的 ownership bouncing。需要明确 slot 的 generation、异常 reader 移除、以及若支持多个并发 Writer 时的发布协调。此例只在单 Writer、每个 Reader FIFO、不提前清 bit 的条件下成立。

## 11. 模型 6B2：共享 Ring + Per-Reader Completed Global Sequence

本方案仍然只向目标 Reader 的 SPSC 投递 seq，而不让所有 Reader 全表扫描。每个 Reader 仅写自己的 completedGlobal；对于旧 Slot 中的 Data x，Writer 只检查该 Data 的 targetMask 对应 Reader 是否满足 C(i) >= x.seq。

不同于最保守的 Disruptor 全员 min(cursor)：**这里不能让非目标 Reader 扫描已经被提前覆盖的 metadata**。SPSC 负责告诉每个 Reader 它具体需要读取哪些全局序号；FIFO 证明 C(i) >= x.seq 意味该 Reader 已完成 x（若它是目标）。

~~~cpp
// 06b2_selective_global_cursor.cpp  (需要 mini_spsc.hpp)
#include "mini_spsc.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
struct Slot {
    uint64_t seq{0};
    uint32_t targets{0};
    int value{0};
};
int main() {
    constexpr std::size_t N=3, CAP=8;
    std::array<Slot,CAP> ring{};
    std::array<Spsc<uint64_t,16>,N> qs;
    std::array<std::atomic<uint64_t>,N> completed{};
    for(auto& c:completed)c.store(0);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for(std::size_t i=0;i<N;++i)
        readers.emplace_back([&,i] {
            uint64_t seq;
            for(;;) {
                if(qs[i].pop(seq)) {
                    auto const& s=ring[seq%CAP];
                    assert(s.seq==seq && s.value==int(seq*10));
                    completed[i].store(seq,std::memory_order_release);
                } else if(done.load(std::memory_order_acquire)) break;
                else std::this_thread::yield();
            }
        });
    for(uint64_t seq=1;seq<=100;++seq) {
        Slot& s=ring[seq%CAP];
        auto ready = [&] {
            if(s.seq==0) return true;
            for(std::size_t i=0;i<N;++i)
                if((s.targets&(1u<<i)) &&
                   completed[i].load(std::memory_order_acquire)<s.seq)
                    return false;
            return true;
        };
        while(!ready()) std::this_thread::yield();
        uint32_t mask=1u<<(seq%N);
        if(seq%4==0) mask|=1u<<((seq+1)%N);
        s={seq,mask,int(seq*10)};
        for(std::size_t i=0;i<N;++i) if(mask&(1u<<i))
            while(!qs[i].push(seq)) std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);
    for(auto& t:readers)t.join();
}
~~~

**为什么这里可检查单条 target subset**：非目标 Reader 永不访问这个 Slot 对应的老消息；目标 Reader 只有经 Queue 取得 seq 才会访问。若改成所有 Reader 顺序扫描全局 Ring，则必须为被跳过/覆盖的 sequence 设计完整的 generation-aware skip 协议，否则不能直接按目标最小进度覆盖 Slot。

## 12. 模型 7：原有 Replay 设计——Stable Slot State (Generation + RefCount) CAS

你的真实设计核心：固定 Pool Slot 的 **稳定 atomic 状态字** 包含 generation/sequence + pin count；Replay 通过 CAS 对 Slot Pin，Writer 想要推进/复用 Slot 也必须 CAS 先 Close，禁止新 Pin，待旧 Pin 清零才能覆盖非原子 Data。

**这比先 load Data* 再 fetch_add 对象内部 refcount 安全得多**，因为 CAS 的 metadata 地址在整个 Pool 生命周期内稳定。但它仍然要求维护保留窗口 / seq index，有效 seq 被逐出后 Lookup 必须失败。

下面用一个 64-bit 状态字演示：高 32 位 seq、低 31 位 pinCount、低位最高 bit 为 CLOSING。示例不处理超过 2^32-1 的 seq 和 refcount overflow；生产版请设计更大的 generation 或显式 wrap 机制。

~~~cpp
// 07_replay_cas_pin.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
struct Data { uint64_t seq{}; int value{}; };
struct Slot {
    std::atomic<uint64_t> state{0};
    Data data{};
};
constexpr uint64_t CLOSING = 1ull<<31;
constexpr uint64_t PINS = CLOSING-1;
constexpr std::size_t CAP=8;
struct ReplayBuffer {
    std::array<Slot,CAP> ring;
    std::atomic<uint64_t> latest{0};
    bool lookup(uint32_t seq, Data& output) {
        Slot& s=ring[seq%CAP]; // 稳定 metadata，不能先读 Data*
        uint64_t st=s.state.load(std::memory_order_acquire);
        for(;;) {
            if ((st>>32)!=seq || (st&CLOSING) || (st&PINS)==PINS)
                return false;
            if(s.state.compare_exchange_weak(
                    st,st+1,std::memory_order_acquire,
                    std::memory_order_relaxed)) break;
        }
        output=s.data; // Pin 成功后 Writer 无法覆盖
        s.state.fetch_sub(1,std::memory_order_release);
        return output.seq==seq;
    }
    void publish(uint32_t seq, int value) { // 只有单 Writer
        Slot& s=ring[seq%CAP];
        for(;;) {
            uint64_t st=s.state.load(std::memory_order_acquire);
            if (st&CLOSING) {
                if ((st&PINS)==0) break;
            } else if (s.state.compare_exchange_weak(
                           st,st|CLOSING,std::memory_order_acq_rel,
                           std::memory_order_acquire)) {
                if ((st&PINS)==0) break;
            }
            std::this_thread::yield(); // pinned reader 慢时 Writer 可能 stall
        }
        s.data={seq,value}; // 当前无 Pin、无新 Pin
        s.state.store(uint64_t(seq)<<32,std::memory_order_release);
        latest.store(seq,std::memory_order_release);
    }
};
int main() {
    ReplayBuffer b;
    std::atomic<bool> done{false};
    std::thread replay([&] {
        Data out;
        while(!done.load(std::memory_order_acquire)) {
            auto latest=b.latest.load(std::memory_order_acquire);
            if(latest>0) {
                auto wanted=uint32_t(latest);
                if(b.lookup(wanted,out))
                    assert(out.value==int(out.seq*10));
            }
        }
    });
    for(uint32_t seq=1;seq<=1000;++seq) b.publish(seq,int(seq*10));
    done.store(true,std::memory_order_release);
    replay.join();
}
~~~

**线性化点**：Replay Pin CAS 与 Writer VALID→CLOSING CAS 在同一个 atomic 状态字上竞争；谁先成功谁受协议保护。Reader release pin 完毕后，Writer acquire 读到 zero pins，才可写新的 Data。

**成本**：Writer 每次复用 Slot 至少一个 RMW/CAS；Replay Pin 还有 RMW；即使 Replay 极少发生也要支付 Writer 常态 CAS。更严重的可能是 Replay 被抢占时 Writer 卡在 pinned Slot（Tail latency）。Single-Writer 的 CAS 不一定 contention 高，需测量后才能判断是否是瓶颈。

**与 Live Reader 的关系**：此例只演示 Replay Pin！如果同一个 Slot 还被 Live Reader 使用，Writer 除了等待 Pins，还必须额外等待该 Slot 对应目标 Live Reader 的完成进度。

## 13. 模型 8A：专用 Replay Reader 缓存 Data*，以窗口限制复用

这是你提出的 Replay 方案 A。唯一 Replay Worker 按 seq 顺序读取、缓存一段 **指向原始 Ring 的指针**，在释放最早指针后才推进 releasedThrough。Writer 复用 Slot 前必须保证原 seq 已被 Release。

重点：scannedThrough != releasedThrough；不能因取到一个指针就认为它已无需保护。

~~~cpp
// 08a_replay_pointer_window.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <thread>
struct Data { uint64_t seq{}; int value{}; };
int main() {
    constexpr uint64_t CAP=16, WINDOW=4, TOTAL=100;
    std::array<Data,CAP> ring{};
    std::atomic<uint64_t> published{0}, releasedThrough{0};
    std::atomic<bool> done{false};
    std::thread replay([&] {
        std::deque<const Data*> cache; // 仅此线程使用指针
        uint64_t next=1;
        for(;;) {
            auto end=published.load(std::memory_order_acquire);
            if(next<=end) {
                const Data* p=&ring[next%CAP];
                assert(p->seq==next && p->value==int(next*10));
                cache.push_back(p);
                // 缓存窗口内随机访问仍合法
                for(const Data* c:cache) assert(c->value==int(c->seq*10));
                if(cache.size()>WINDOW) {
                    auto* old=cache.front();
                    auto oldSeq=old->seq;
                    cache.pop_front(); // 之后不再触碰 old
                    releasedThrough.store(oldSeq,std::memory_order_release);
                }
                ++next;
            } else if(done.load(std::memory_order_acquire)) break;
            else std::this_thread::yield();
        }
        cache.clear();
        releasedThrough.store(next-1,std::memory_order_release);
    });
    for(uint64_t seq=1;seq<=TOTAL;++seq) {
        while(seq-releasedThrough.load(std::memory_order_acquire)>CAP)
            std::this_thread::yield();
        ring[seq%CAP]={seq,int(seq*10)};
        published.store(seq,std::memory_order_release);
    }
    done.store(true,std::memory_order_release);
    replay.join();
    assert(releasedThrough==TOTAL);
}
~~~

此例中 Cold Lookup 必须 **串行化到 Replay Worker** 或额外保护缓存指针。如果别的 Cold Thread 也能随意取缓存 Data* 并跨越 Worker release，依然会 UAF。保留指针窗口为零拷贝，但 Replay 卡住时仍可能阻塞 Writer（或造成额外容量要求）。

## 14. 模型 8B：专用 Replay Reader 复制 Data，快速推进 copiedThrough

这是你提出的方案 B：Replay Worker 在受到原始 Ring 保护时深拷贝 Data 到独立 Archive，然后推进 copiedThrough。Writer 只需对这个 copy watermark 做 acquire load；正常 Slot 发布不用 Replay Pin CAS。Cold Lookup 从 Archive 查找，不再依赖原始 Ring。

~~~cpp
// 08b_replay_copy_archive.cpp
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
struct Data { uint64_t seq{}; int value{}; std::array<char,32> payload{}; };
int main() {
    constexpr uint64_t CAP=16, TOTAL=100;
    std::array<Data,CAP> ring{};
    std::atomic<uint64_t> published{0}, copiedThrough{0};
    std::atomic<bool> done{false};
    std::mutex archiveMutex; // 仅在 Cold Path 使用
    std::unordered_map<uint64_t,Data> archive;
    std::thread replay([&] {
        uint64_t next=1;
        for(;;) {
            auto end=published.load(std::memory_order_acquire);
            if(next<=end) {
                Data copy=ring[next%CAP];  // 完整 Value Copy
                assert(copy.seq==next);
                {
                    std::lock_guard lk(archiveMutex);
                    archive.emplace(next,copy);
                }
                copiedThrough.store(next,std::memory_order_release);
                ++next;
            } else if(done.load(std::memory_order_acquire)) break;
            else std::this_thread::yield();
        }
    });
    for(uint64_t seq=1;seq<=TOTAL;++seq) {
        while(seq-copiedThrough.load(std::memory_order_acquire)>CAP)
            std::this_thread::yield();
        ring[seq%CAP]=Data{seq,int(seq*10),{}};
        published.store(seq,std::memory_order_release);
    }
    done.store(true,std::memory_order_release);
    replay.join();
    {
        std::lock_guard lk(archiveMutex);
        assert(archive.size()==TOTAL);
        assert(archive.at(50).value==500);
    }
}
~~~

上面 Data 所有字段均按值存储。如果真实 Data 含 char*、span 或外部 Buffer，则必须 **deep copy 实际 payload** 或对外部 Buffer 加保护。copiedThrough 表示 Archive 已持有独立存储副本；不自动代表写入磁盘完成或 crash-safe。如果 Archive 还要提供持久化保证，应另外设置 durableThrough（fsync / flush / storage acknowledgment）。

## 15. Segment-Based Replay Archive 与折中

按 seq 切分 immutable Segment：WRITING → SEALED → COPYING / ARCHIVED → EVICTABLE。Reader 只访问已经 SEALED 的 Segment；Cold Lookup 用 seq→segment+offset；可以基于 segment 粒度进行 Pin、shared_ptr 或 service-owner 串行查找。

Segment 的优势：批量 copy、批量回收、Cold Path index 管理简单，降低每消息 metadata 成本；劣势：单个慢消息或迟迟不完整的 Segment 能拖住整个 Segment 的回收，且需要显式管理最近尚未 archive 的 tail。

可以考虑的 Hybrid：

~~~text
Live Delivery   : Per-Reader SPSC / Selective Fan-out
Original Memory : Fixed Pool + Writer-owned Reclamation
Recent Replay   : Stable Slot Gen+Ref CAS Pin（可按需启用）
Archive         : Replay Copy Worker + Immutable Segments
Cold Lookup     : Archive owner / independent Segment lifetime
~~~

注意：**在保留 CAS Pin 的同时启用 Copy Worker** 是可以渐进迁移的：先让 Cold Lookup 优先读 Archive，在 newest tail 仍走受 CAS 保护的原始 Slot；确认全量迁移后才能删除原始 Slot CAS 机制。不能在新旧两种路径共存时贸然改用普通 store。

## 16. 四种回收机制完整比较

| 维度 | RefCount | Hazard Pointer | EBR | QSBR |
|---|---|---|---|---|
| Reader 保护单位 | 单对象共享所有权 | 单个具体对象指针 | 一个受保护的 epoch 临界区 | 经过一次 quiescent 之前的所有读取 |
| 读侧 RMW | 通常有 | 不一定；可能需要 fence | 通常不需要 | 一般只在 checkpoint 报告 |
| Reader 动态增减 | 天然支持 | Record 注册/清空/复用 | Participant 注册/unpin/注销 | register/online/offline/unregister |
| 慢 Reader 阻塞回收 | 主要阻塞被持有对象 | 主要阻塞 Hazard 指向对象 | 可阻塞整个 domain | 可阻塞整个 domain |
| 非侵入性 | shared_ptr 可非侵入 | 外部 metadata | 外部 epoch state | 外部 thread progress |
| 典型 HFT | Cold async ownership | 动态 lock-free nodes | 短多节点读临界区 | 固定线程 event loop / RCU |
| 典型风险 | 获取 raw ptr 后 ref++ 竞态 | 错误的 Publish→Validate 顺序 | 长期 pin / 线程暂停 | 遗漏 QS / pointer escape |

QSBR 可用 Per-Reader Monotonic Progress + Global Grace Period 实现；Selective Fan-out 中用 **per-object 的目标集合条件** 取代全部 Reader 的无差别 min，可以避免被无关 Reader 拖慢。只要符合对象访问图与严格发布顺序，此类模型与 QSBR-like 回收思想相通，但不意味着任何 min(cursor) 代码天然满足完整 QSBR 证明。

## 17. 三种 Selective Fan-out 设计比较

| 维度 | A：多 SPSC + Pool | B1：Ring + Pending Mask | B2：Ring + Completed Cursor |
|---|---|---|---|
| 每条 Data 选任意 Reader 子集 | 支持 | 支持 | 支持 |
| Data 只存一份 | 支持 | 支持 | 支持 |
| 非目标 Reader 遍历消息 | 不需要 | 不需要（通过目标 SPSC） | 不需要（通过目标 SPSC） |
| 完成时 Reader RMW | 无 | 每目标一次 fetch_and | 无 |
| Writer 回收检查 | 每退休对象按 target 进度检查 | pending==0 | 按旧 Slot target 进度检查 |
| Slot 复用顺序 | Pool 可乱序 | 固定 Ring 顺序 | 固定 Ring 顺序 |
| 慢 Reader 影响 | 只拖住其使用的 Pool 对象 / queue | 可堵住即将复用的 Ring Slot | 同左 |
| 主要成本 | Per-object retirement metadata | 多核共享 ACK cache line | Per Reader 独占 cursor + 检查 |

A 最贴合已有 Processor + 多 SPSC 框架；B1 简单直接但有 per-message contention；B2 对 Reader 更轻，却要求 target 元数据与 per-reader 投递队列严格配对。所有方案都需要考虑 Queue full、Reader 失联/注销、slot generation、startup/shutdown、安全停止和共享对象写入可见性。

## 18. HFT 业务场景驱动的架构选型：通信、Fan-out、Replay 与 Backpressure

本节是前面各数据结构与回收算法的**选型总览**。原则：先确定业务交付语义、顺序、对象生命周期、历史保留、故障策略，再决定用什么 Ring、Queue 或回收机制。以下为面向 C++ 低延迟交易系统的工程建议，不是各 HFT 公司采用情况的统计，也不是未经 Benchmark 的性能排名。

### 18.1 六个必须拆开的架构维度

| 维度 | 必须回答的问题 | 可能采用的机制 |
|---|---|---|
| Delivery | 一条消息给一个、所有、还是指定目标子集？ | SPSC / MPMC Work Queue / Multicast / Selective Fan-out |
| Ordering | 按 Session、Order、Instrument 还是全局需要有序？ | Single Owner / Sharding / Sequencer |
| Lifetime | 哪些 Reader 仍在访问原始 Data？ | Ownership Transfer / Completed Cursor / CAS Pin / QSBR / EBR / HP |
| Retention | Reader 完成后还要保留多久？ | Recent Window / Immutable Archive / Durable Journal |
| Backpressure | Reader 停顿、Pool 满、Archive 落后时怎么办？ | Wait / Reject / Conflate / Drop+Recover / Copy / Isolate |
| Membership | Reader 增删或订阅更改从哪个 Seq 生效？ | Writer-Owned Registry + Control Event + Generation |

必须维护的不变量：**非原子 Payload 不能在旧 Reader 仍可能访问时被覆写**。Message Consumed、Business Completed、Original Memory Reclaimable、History EVICTABLE 和 Durable Commit 均不等价。

### 18.2 按 HFT/FX 实际流水线选择

| 场景 | 优先考虑 | 优点 | 主要限制 |
|---|---|---|---|
| 两个固定 Pipeline Stage，一条消息交给一个 Consumer | Value SPSC / Exclusive Handle SPSC | Ownership 简单，通常无额外 Memory Reclamation | Queue Full / Handle Return |
| Dispatcher → Calculation Workers，按 Instrument 分片 | N×SPSC + Single-Owner Sharding | Worker 局部修改状态、减少共享写 | 分片倾斜、跨 Instrument 聚合 |
| Feed Handler → 不同 Strategy / Pricing Task 的订阅子集 | Multiple SPSC + Selective Routing；大 Payload 可共享 Pool | 仅给目标投递、Subscriber 可变 | 部分 Fan-out 投递与旧消息回收 |
| 所有 Reader 都必须接收完整顺序流 | Single-Producer Multicast Ring + Gating Cursor | Data 写一次，统一 Published Seq | 最慢 Reader 可能阻止整个 Ring 覆盖 |
| 多 Producer → Order / FIX Session State Owner | N×SPSC 合流，必要时 MPSC | State Machine 仍由单线程串行更新 | 合流顺序、公平性、Queue Full |
| 一条任务只需多个 Worker 中某一个处理 | MPMC Work Queue / Dispatcher | 任务负载均衡 | 不是 Broadcast，可能有较多竞争 |
| Read-Mostly Instrument / Routing / Pricing Config | Immutable Snapshot + QSBR / EBR | 新旧版本共存、可延迟回收 | 长期 Pin、Online 不报 QS |
| Fixed Embedded Slot + Random Recent Replay | Generation + RefCount CAS Pin | 精确到 Slot + Version | Reader RMW 与 Writer Reuse Stall |
| Research / Recovery / Long Replay | Immutable Segment Archive | 批量 Copy / Index / Reclaim | Archive Lag、Tail Gap、Retention |
| Logger / Telemetry / Persistence | Per-Core SPSC / Chunk Ownership Transfer | 格式化和 I/O 放在 Cold Path | Buffer 耗尽与日志丢失策略 |
| OMS / Execution / Risk / FIX Gateway | Single-Owner State Machine + 显式确认及持久化语义 | 顺序和失败模式明确 | 不能 Silent Drop 或随意 Conflate |

**术语：通常的 SPMC Queue 是多个 Consumer 竞争消费，每条消息只被其中一个取走；本文所谓 SPMC 广播，更准确是 Single-Producer Multicast Ring。**

### 18.3 什么时候选择 Multiple SPSC，什么时候选择 Multicast？

~~~text
每条消息的消费语义：
  ├─ Exactly one fixed Consumer        → SPSC
  ├─ One-of-N competing Consumers       → MPMC / N×SPSC Dispatcher
  ├─ Every Consumer gets every message → Multicast Ring
  └─ Dynamic subset of Consumers       → Multiple SPSC + Routing
~~~

| 维度 | Multiple SPSC + Shared Pool | Multicast Shared Ring |
|---|---|---|
| Target 特征 | 稀疏、动态、各 Reader 不同 | 全量或几乎全量 |
| 每条发布 | 向 k 个目标各 Push 一个 Handle | 写一次 Data + Published Seq |
| Reader 获取下一条 | 各自 SPSC | Published Seq + 本地 Cursor |
| 回收依据 | 消息发布时的 Target Requirements | 全部相关 Gating Cursor |
| Slow Consumer | 拖住所持 Pool Slot；Pool 满可能扩散 | 拖住整个固定 Ring 的复用 |
| Dynamic Subscription | Writer 修改 Routing | 需要额外跳过/筛选协议 |

**小型 Payload 直接复制 N 次可能优于共享 Data Pool**：跨 Core 共享对象的生命周期和 Cache Coherence 并非零成本；大型 Payload 或高 Fan-out 则更有理由共享。不能仅由少一次 Copy 推断 p99.9 更好。

### 18.4 Multiple SPSC + Shared Pool：如何证明可复用？

假设单 Writer、每个 Reader 只通过自己的 FIFO SPSC 获取对象、Reader 按连续顺序处理并在最后一次访问后发布 completed：

~~~text
Data x:
    targets = {R1, R3}
    requiredOrder[R1] = 105
    requiredOrder[R3] = 88

LiveDone(x) =
    completed[R1] >= 105
    AND completed[R3] >= 88
~~~

如果 Reader 把任务再次异步分发出去并乱序完成，最大已完成 Sequence **不代表连续完成**；需要 Contiguous Frontier / Completion Bitmap 或明确禁止异步指针逃逸。

**部分投递**必须处理：

~~~text
Push(R1, x) = success
Push(R3, x) = queue full
~~~

R1 已收到，无法悄悄回滚。可选：预留全部目标 Queue 容量、可恢复的 Pending Publication、或者业务明确规定部分交付/失败处理。不要无限 Spin 等 R3 释放 Queue 却饿死 Writer 的 RemoveReader Control Event。

### 18.5 Free Slot LIFO Stack、FIFO Ring、Fixed Data Ring 是不同东西

~~~text
Per-Reader SPSC Ring:  传递 Slot Handle
Data Pool:             真正存储 Payload
Free Slot Stack/Ring:  Writer-only，存放可复用 Slot ID
Pending Retired:       等待所有旧 Reader、Replay 与 Archive 义务结束
~~~

| 维度 | LIFO Free Stack | FIFO Free Ring |
|---|---|---|
| Push/Pop | O(1)，管理成本较低 | O(1)，维护 Head/Tail/Count |
| 单 Writer 是否需要 Atomic | 否 | 否 |
| 复用模式 | 最近归还的先用 | 最早归还的先用 |
| 潜在缓存优势 | Working Set 更集中 | 相邻 Reuse 间隔更长 |
| 潜在缓存风险 | 热 Slot 在 Core 间反复迁移 | 更大 Working Set / LLC Miss |

先以预分配的 Stack 作简单基线，再根据 HITM、LLC Miss、NUMA、Pool 空闲水位、Writer p99.9 比较 FIFO Ring。**FIFO Free Slot Ring != Data[seq % Capacity] 固定原地覆盖 Ring**：前者可选任何安全 Slot，后者必须面对指定位置的 Head-of-Line Blocking。

若追求运行期 Zero Allocation，注意不止 Free Slot 容器，还包括当前示例的 Pending deque、动态 Registry 和可能扩容的记录数组。Pending 完成顺序可能乱序，不能只从队首回收，否则会制造新的 Head-of-Line Blocking。

### 18.6 普通 SPSC 可以 Segment 化，但别混淆两个 Batch

**Batch Synchronization**：可按消息及时 Publish，Consumer 成批处理和更新 Completed，减少共享 Metadata 更新；仍然可以逐消息可见。

**Batch Visibility / Segment Ownership**：Producer 独占 WRITING Segment，Fill/Deadline/Explicit Flush 后 SEALED 并发布；Consumer Acquire 整段，消费完 Release 归还。这里可以按 Segment 摊销同步，但早到的消息等待 Seal。

~~~text
Producer owns WRITING Segment
    → Fill / Timeout / Explicit Flush
    → SEALED Publish (release)
    → Consumer Acquire and Process
    → Consumer Return (release)
    → Producer Reuse
~~~

Segment 级所有权转移本身可保证普通 Data 生命周期，无须在这个模型上额外叠加 EBR/HP；但 Consumer 归还后绝不可保存 Data 指针。要支持 Partial Flush，尤其是低流量、Remove Reader、Shutdown，否则 Tail 可能永远不可见。

**交易 Hot Path 优先逐条可见，Logger / Archive / 天然 NIC Poll Batch 更适合整段交接**。优秀的普通 SPSC 本来也可缓存 Head/Tail 并批量更新，不应假定 Segmented SPSC 必然更快。

### 18.7 动态 Reader：Single-Owner Event-Driven Membership

优先 **Static Physical Topology + Dynamic Logical Membership**：固定 CPU-Pinned Workers / Queue，动态修改 Instrument Subscription、Task Routing、Session。运行期频繁改的是逻辑订阅，而未必是物理线程数；这是工程经验判断，不是行业数量统计。

~~~text
Management / Cold Path
    AddReader / RemoveReader / UpdateSubscription / UpdateRouting
      ↓
Control Queue (SPSC 或 MPSC，取决于控制 Producer 数量)
      ↓
Single Writer / Registry Owner
      ↓
在确定的消息边界应用变更，返回 effectiveSeq
~~~

推荐状态：CREATED → STARTING → ACTIVE → DRAINING / CANCELLING → STOPPED → RECLAIMED。

| 生命周期操作 | Multiple SPSC Selective | Multicast Ring |
|---|---|---|
| Add | SPSC/Worker 已 Ready，加入 Active Targets | 初始化 startSeq/completed，**先加入 Gating**，再允许接收后续 Seq |
| Remove 生效 | 不再投递新消息，旧 Retired Requirements 保留 | 设置 stopAt Seq，Reader 继续完成旧消息 |
| Drain | 消费到对应本地完成序号 | 完成 stopAt，确认不再访问 Ring |
| Reclaim ReaderState | 没有 Pending 指向旧 ReaderState | 先安全移出 Gating，再管理线程退出与资源回收 |
| Reader ID 复用 | Stable ReaderState + Generation | Stable Gating Record + Generation |

**Remove Membership 并不表示旧 Reader 已不持有 Data**。取消未处理消息可以由业务允许，但绝不能忽略 Reader 正在使用的 Slot；一个被 OS 抢占的 Reader 可能暂时无法安全移除。不能用当前 Active Mask 回收历史消息，而要保留发布时确定的旧义务。

广播 Fixed Ring 的回收条件（Seq 从 1 开始，容量 C，全部相关 Reader 连续按序完成）：

~~~text
SafeToPublish(nextSeq) =
    nextSeq <= C
    OR min(completed[gatingReaders]) >= nextSeq - C
~~~

Writer 可以缓存最慢 Reader 的 Completed 并仅在容量临界时重查，**AddReader 必须更新 Gating 与可能缓存的边界**。History Join 要在加入保护协议前验证保留窗口，不能从已覆盖的 Seq 开始读。

### 18.8 选择 Reclamation，不要全局统一一套

| Storage / Access Model | 更自然的生命周期机制 |
|---|---|
| SPSC 值拷贝、独占 Handle | Ownership Transfer，无通用回收 |
| Multiple SPSC 共享 Data | Target Completed Cursor |
| Multicast 固定 Ring | Per-Reader Gating Cursor |
| Read-Mostly Immutable Config | QSBR / EBR |
| Lock-Free Linked Nodes / Pointer Index | EBR / HP |
| Fixed Slot + Random Recent Replay | Generation + CAS Pin |
| SEALED Immutable Segment Archive | Segment Pin / shared_ptr / Service Owner |

**决定 EBR/QSBR 是否合适的关键是 Publication 与 Physical Reuse 能否解耦**。有备用 Immutable Object/Pool Slot，Writer 可以先发布再延迟回收；固定满 Ring 必须在覆盖前等 Reader。HP 更精确保护对象地址但固定 Slot 还要 Seq/Generation 验证；CAS Pin 精确保护 Slot+Version，却付出 Reader 原子 RMW 的代价。

QSBR 在线 Worker 若持续不报告 QS，可能阻止未来 Grace Period；EBR 的 Scoped Unpin 结束后即使线程暂停也不阻止未来回收。二者适合 Immutable Object Reclamation，而非直接替代每个固定 Embedded Slot 的任意并发覆写协议。

### 18.9 Recent Replay + Segment Archive：两层职责

~~~text
Live Processing:     Multiple SPSC / Multicast
Original Storage:    Fixed Pool / Ring + Lifecycle
Recent Replay:       Generation + CAS Pin
Archive Handoff:     Copy ACK / Transfer Ownership
Historical Replay:   Immutable SEALED Segment Index
Durable Recovery:    可选的 File / Journal / Storage
~~~

对连续 Seq、固定记录大小，Segment Lookup 可使用：

~~~cpp
segmentId = (seq - 1) / SEG_SIZE;
offset    = (seq - 1) % SEG_SIZE;
~~~

SEALED 表示不再修改，**不是 Durably Persisted**。Segment 的 WRITING、SEALED、COPYING、ARCHIVED 描述构造/迁移；EVICTABLE 另须满足 Retention 与 Reader 保护全部结束。最新 WRITING Tail 可选 Hot Replay Fallback、Partial Seal、或另行证明的 Tail Protocol。

在需要完整无损归档的配置下：

~~~text
SafeToReuseOriginal(x) =
    LiveDone(x)
    AND NoOutstandingOriginalReplayPin(x)
    AND ArchiveOwnsIndependentSafeCopy(x)
    AND OtherOriginalRetentionRequirementsSatisfied(x)
~~~

Archive Copy ACK 不等于 Durable Commit。History Reader 赶上 Live 时，需要在 Writer 顺序点取得明确切换边界，处理历史与实时的 Gap、Overlap、Duplicate；不能简单“先 Replay 完，再注册 Live”。

### 18.10 Backpressure：算法无法绕开的容量约束

**有限容量、完全无损、任意慢/永久停顿 Consumer、Writer 永远不阻塞**无法无条件同时保证。要么有额外资源和持久化，要么显式定义 Reject/Drop/Retry/Disconnect/Conflation 的业务策略。

| 业务 | 更合理的策略 | 禁忌 |
|---|---|---|
| 只要求最新价态的 Pricing Trigger | Version-Based Conflation / Coalescing | 合并不可跳过的业务转移 |
| 可恢复 Market Data State | Gap Detect + Snapshot/Replay，必要时标记 Stale | 未确认数据连续就继续报价 |
| FIX Order / Execution / Booking | 有界 Backpressure、明确拒绝、重试或持久化 | Silent Drop |
| Risk / Audit | 明确的保序、Durability、Fail-Closed 策略 | 仅依赖可覆盖的 In-Memory Ring |
| Metrics / 可丢失 Telemetry | Sampling / Batch / Bounded Drop | 非关键日志阻塞核心下单 |
| Historical Archive | Lag 监测、容量预留、分层存储与降级决策 | 默认 Background Worker 不会落后 |

QueueConsumed != BusinessCommitted != DurablyRecorded != ExchangeAccepted != Filled。Completed Cursor 的业务层含义必须由接口精确定义。

### 18.11 对当前 Polling Processor 的推荐组合

~~~text
1. State/Sequencing:      Single-Owner Per-Partition State Machine
2. Worker Communication:  Small Value SPSC / Exclusive Handle SPSC
3. Sparse Subscriptions:  Writer-Owned Routing + Multiple SPSC + Optional Shared Pool
4. Full Broadcast:        Multicast Ring + Per-Reader Gating
5. Read-Mostly Config:    Immutable Snapshot + QSBR / EBR
6. Recent Random Replay:  Stable Slot Generation + CAS Pin
7. Logger / Long Replay:  Segmented SPSC + Immutable Segment Archive
8. Dynamic Membership:    Control Event → Owner → effectiveSeq → Drain / Reclaim
~~~

不要先开发通用 Lock-Free MPMC Scheduler、全局 Hazard Pointer Runtime 或任意动态线程拓扑，除非存在明确需求及可测量收益。主要目标是**单一写所有权、减少跨核共享写、限制慢消费者影响、明确生命周期和可恢复性**。

### 18.12 选型验证清单

1. Value SPSC vs Handle SPSC：Payload Size、Core/NUMA、Writer p99.9。
2. Selective vs Multicast：Target Count 1/2/4/8/all、慢 Reader、Queue 满。
3. Free LIFO Stack vs FIFO Ring：Pool 水位、HITM/LLC Miss、Pending Scan。
4. Recent CAS Pin Replay：随机 Lookup、Reader 抢占、Writer Closing CAS、RMW 竞争。
5. QSBR/EBR/HP：Reader 暂停、Grace Period、Hazard Scan、回收峰值。
6. 普通 SPSC vs Segmented：逐条可见性、Batch Flush Delay、End-to-End Latency。
7. Dynamic Add/Remove：Reader ID/Generation、旧 Target Obligations、Cancel/Drain、Controller Starvation。
8. Archive：Copy Worker 落后、WRITING Tail、Segment Eviction、History→Live Join。
9. 所有测试记录 p50/p99/p99.9/max、Queue Depth、Free Slot、Retired Bytes、Archive Lag、Lost/Duplicate/Out-of-Order 数。

**架构判断置信度：高（协议与安全不变量），中等（各场景的优先级），具体性能排名必须以部署机器上的真实负载为准。**

---

## 19. 异步 Data Escape、Transitive Completion 与 Reference Counting

本章接续 §9 Selective Fan-out、§11 Per-Reader Completed Cursor、§12 CAS Pin 和 §18 架构选型。新约束：**初级 Consumer 收到 Pool 中的 Data Handle 后，可能异步转发给下游；真正的最后一次 Payload 访问发生在 P3/P4 等更后面的阶段**。必须明确区分“Consumer 已转发”与“原始 Data 已无人访问”。

### 19.1 业务处理阶段、线程边界、Data Lifetime 不是一回事

~~~text
业务处理：Market Data → Book → Pricing → Strategy → Risk → Order Entry → DMA

可能的线程布局：
  Core A: Feed Decode + Book Update + Strategy（同步函数调用）
  Core B: Risk + Order Entry（单独 State Owner）
  Core C: DMA Session / Network Writer
  Core D: Logger / Archive（后台）
~~~

即使存在七个业务阶段，也不等于有七次 SPSC 交接或七级跨线程生命周期依赖。公开资料没有可信的 HFT 全行业最大 Pipeline 深度统计；“2~4 级”只能是讨论架构时的经验起点，**不是行业上限、平均值或优化定律**。LMAX Disruptor 的 Gating Sequence / Dependency Graph 是工业界使用进度依赖的公开设计实例，但不能由此推断各 HFT 企业采用传递式 Completion Cursor 的比例。

对延迟尤其敏感的路径，首先考虑同一个 Owner Thread 内串行执行多个业务函数；仅当并行度、隔离需求、阻塞特性或状态所有权值得时才增加线程交接。

### 19.2 什么是 Asynchronous Pointer Escape？

术语区分：

- **Pointer / Reference Escape**：原始指针或引用离开当前函数/组件的可控生命周期范围；在 Compiler Escape Analysis 中也有特定语境，**不等于必然跨线程**。
- **Asynchronous Data Escape**：下游在上游调用结束后仍访问原始 Data（典型 P2 SPSC → P3）；需要明确的 Lifetime Contract。
- **Ownership Transfer**：当前 Owner 把持有权转给下游；转移不等于新增引用。
- **Shared Ownership / Fork**：两个或多个下游同时保留使用权；必须登记额外保护。
- **Lifetime Decoupling**：下游转为独立 Value/Copy，原始 Pool 不再受它的完成时间限制。

~~~text
A. 同步借用（Non-Escaping）
    P1 → P2.process(Data&) → P3.process(Data&) → 返回
    P3 不保存指针；P2 可在返回后报告最终完成；无需 Copy。

B. Copy/Transform at Async Boundary
    P1 Data → P2 → 独立 DerivedEvent → P3 Async
    P2 完成后即可放开原始 Slot；P3 只依赖自己的数据。

C. Async Ownership Transfer
    P1 Data → P2 → move OwnedHandle → P3
    不复制 Payload；P2 不再使用原始 Data；P3 负责最终 Release。

D. Async Shared Borrow / Fork
    P1 Data → P2 ─┬→ P2 继续使用
                   └→ P3 Async 使用
    两边都结束使用后才能释放；RefCount / Completion DAG / 等价协议。
~~~

**不逃逸不意味着必须复制**：同步调用可直接 Borrow；异步转发也可不复制，但要跟踪生命周期。**Copy/Transform 不保证解耦**：若 DerivedEvent 含有指向原始 Pool 的 Data*、string_view、span，依然有 Escape。

### 19.3 完成语义分级：P2 已转发不能宣布 Safe

~~~text
P1 (Data Pool Owner)
   ├─ SPSC → P2 → P3 Async → Final Use Done
   └─ SPSC → P4 Sync → Local Done

错误：
  P2 将 Handle push 给 P3 后立刻 completed[P2]=n；
  P4 done 后 P1 认为 Data[n] 安全；
  P1 Overwrite；P3 继续读旧 Data ⇒ C++ Data Race。

正确：
  P2 对外报告的 SafeCompleted[n] 必须包括其所有下游原始引用
  P4 完成之后 P1 还需等待 P2 的 Transitive Safe Completion。
~~~

**Completion Cursor 应表示所有 Seq <= C 的相关 Payload 使用都已结束（Contiguous Completion Frontier）**，不是 Dequeued、Forwarded、Scheduled 或“已完成的最大 Seq”。异步 callback 只要可能在 Cursor 提交后访问原始数据，Cursor 就不是 Safe Completion。

### 19.4 Transitive Completion Gating：固定 FIFO 链

最简单的场景：P1 → P2 → P3，P2、P3 处理所有事件、使用兼容的连续 Seq、无未跟踪的额外 Fork、P2 本地最后访问可记录为 localDone2，P3 的安全完成水位为 safeDone3：

~~~text
safeDone2 = min(localDone2, safeDone3)
P1 可回收 Seq s ⇔ safeDone2 >= s  （还要满足 Replay/Retention 条件）
~~~

若 P2 完全把 Data 的唯一引用移交 P3，且 P3 对所有相关消息的完成水位已经能证明 P2 不再访问，P1 可以直接使用 P3 Cursor；不必无意义地让 P2 中转 ACK。

**示意代码，假设同一连续 Seq 域**：

~~~cpp
// P3：最后一次访问旧 Payload 之后
processP3(pool[handle.slot]);
p3Done.store(handle.seq, std::memory_order_release);

// P2：确保本地对前缀内的 Data 全部结束访问，且
// 后续所有逃逸都已被 P3 的完成水位覆盖
void updateSafe() {
    const auto downstream =
        p3Done.load(std::memory_order_acquire);
    const auto candidate = std::min(localDone2, downstream);
    if (candidate > publishedSafe2) {
        publishedSafe2 = candidate;
        p2Safe.store(candidate, std::memory_order_release);
    }
}

// P1：必须观察到覆盖目标旧 Seq 的安全进度
bool reusable(uint64_t oldSeq) {
    return p2Safe.load(std::memory_order_acquire) >= oldSeq;
}
~~~

这段不是完整可编译的组件：省略了队列管理、RAII、初始化与退出、Sequence Wrap、Retention、History Lookup 和异常处理；且当 P2/P3 并非对**同一完整 Seq 前缀**处理消息时，不能照搬 min()。

**Happens-before 关键链**：

~~~text
P3 最后一次读 Data
   → P3 release-store SafeDone
   → P2 acquire-load 对应 SafeDone
   → P2 release-store Transitive SafeDone
   → P1 acquire-load 对应 SafeDone
   → P1 Overwrite 原始 Data
~~~

P2 自身的最后一次读取同样需要 Sequenced-Before 它的 SafeDone Release。这里 Acquire 必须实际观察到足够新的有效 Release/Release Sequence 值；单纯使用两个没有关联的 Relaxed Loads/Stores 不会自动建立 Data 复用所需的同步关系。

### 19.5 Selective Fan-out：为什么简单 min() 常常错误？

~~~text
Global Seq       100       101       102
P2 收到          yes       yes       yes
P3 收到          yes       no        yes
~~~

P3 的 Global Seq 完成进度可能永远不会等于 101；若跳过 101，直接按连续 Global Seq 用 min() 则可能永不前进，反过来用“最大完成 Seq”又会错误跳过尚未完成的消息。

可选协议：

1. **Per-Consumer Local Delivery Order**：P3 收到的两条消息编为 order 1、2；P2 保存每个原始消息关联的 P3 requiredLocalOrder，在对应义务完成时才能推进自己的连续 Safe Frontier。
2. **显式 Skip / No-Work Watermark**：所有 Consumer 都在同一 Global Sequence 域推进，包括确认无工作、过滤掉的 Seq；代价是额外进度协议。
3. **Per-Object Completion / RefCount**：直接记录每个 Data 的未完成分支，允许独立回收；动态 Fork/乱序工作流往往更自然。
4. **Completion Event / Ack Queue**：下游最终使用后发回 (slot,generation,order)；上游维护乱序完成集合或归还空闲 Slot，而不必强迫连续 Cursor。

当 P3 只收到部分消息时，P2 的 SafeCompleted **应是对 P2 自己的本地输入流的连续安全前缀**，并结合自己投递到各下游的映射判断前缀，不能简单比较不同流里的整数。

### 19.6 Cursor 应该是 Per-Data、Per-Thread、还是 Per-Pipeline？

**推荐归属：每一个具有明确 FIFO 投递与完成语义的 Logical Consumer Stream / Pipeline Endpoint，一个 Completed Cursor。** 它描述某条独立流的连续完成前缀，并不天然绑定 OS Thread。

| 处理拓扑 | 应如何设置 Cursor |
|---|---|
| 一个 Thread 仅处理一个 FIFO SPSC | 可简单做成 Per-Thread Cursor（恰好等于 Stream） |
| 一个 Thread 消费三个独立 SPSC | 通常三个独立 Stream Cursor |
| P2 → P5 多级 Pipeline | P2 对 P1 公布 Pipeline SafeCompleted（包括下游最终使用） |
| 一个 Pipeline 下有多 Worker 并发乱序执行 | 需要聚合的 Contiguous Completion Frontier |
| 一个 Thread 轮询多个无关 Pipeline | 各自独立 Cursor；不能用线程最大 Seq 替代 |
| 每条 Data 一个 atomic<bool> completed | 这是 Per-Object Completion Flag，不是 Cursor |
| 每条 Data 一个 pendingMask / refs | 这是 Per-Object Lifetime Tracking，不是 Cursor |

Selective Fan-out 的 Writer 除了这些 Per-Stream Cursor，还需为各个 Outstanding Data 保存**发布时的 Target Set 与对应 Required Order**：

~~~text
Data x:
  slot = 7
  targets = {P2, P4}
  required[P2] = localOrder 105
  required[P4] = localOrder 88

SafeToRecycle(x) =
  completed[P2] >= 105
  AND completed[P4] >= 88
  AND NoReplayPin(x)
  AND NoOutstandingArchiveDependency(x)
  AND OriginalRetentionSatisfied(x)
~~~

其中 completed[P2] / completed[P4] 必须是**包含全部异步下游使用的 Transitive Safe Frontier**，或由 Writer 额外追踪所有最终下游。

### 19.7 Intrusive Reference Counting：Transfer 与 Fork 不同

对于固定的 Shared Data Pool + 任意多级下游逃逸，Intrusive Atomic RefCount 是自然的 Per-Object Lifetime Tracking。**Move Handle 不应无意义地 ref++ 然后 ref--；只有新增并行 Owner 才 Fork 引用。**

~~~text
P1 → P2 → P3 → Done
       Ownership Transfer: ref 不变，P3 Final Release。

P1 → P2 ─┬→ P2 Process
          └→ P3 Async
       Ownership Fork: 创建 P3 的第二份引用；
       P2 / P3 分别在最终使用完毕后 Release。
~~~

如果 Writer 在任何目标可见前已确认 k 个接收者并准备发布，可以初始化 k+1 个引用（+1 是 Writer 的临时持有），再把 k 个引用的 Handle 交给各目标，发布完成后释放 Writer 引用。也可先初始化 k，在**确保没有消费端能够使 Slot 被 Writer 回收且完成发布前 Writer 仍持有所有权**的等价协议下交接，但 k+1 的临时所有权更容易解释。

~~~cpp
struct Slot {
    std::atomic<uint32_t> refs{0};
    uint64_t generation{};
    Data data{};
};

// Only Writer, only when no old owners/pins remain:
void beginPublish(Slot& s, uint32_t targetCount) {
    // 目标计数 + Writer 临时引用
    s.refs.store(targetCount + 1, std::memory_order_relaxed);
    // Data、generation 初始化与 queue release-publish 协议另外保证
}

// Caller MUST ALREADY own a valid reference:
void forkRef(Slot& s) {
    s.refs.fetch_add(1, std::memory_order_relaxed);
}

void releaseRef(Slot& s) {
    const auto before =
        s.refs.fetch_sub(1, std::memory_order_release);
    assert(before > 0);
}

// Writer / Reclaimer: check after all publishing obligations resolved
bool noOwners(const Slot& s) {
    return s.refs.load(std::memory_order_acquire) == 0;
}
~~~

安全条件：

- 若只做 Ownership Transfer，队列 Push 成功后原 Owner 不得再访问/Release 同一份 Handle；失败则仍由原 Owner 持有、重试或按既定策略释放。
- Fork 的前提是调用者仍持有合法引用；**不可通过已经失效的 raw Data* 或被复用的 Slot 直接 fetch_add 企图“复活”对象**。
- 先为每条成功或计划发布的分支建立有效引用，才允许 Reader 取得 Handle。部分 Fan-out 失败时必须正确撤销未投递引用并处理已投递分支。
- 释放完成用 Release，Writer 观察零用 Acquire；对象可变 Payload 只有在所有 Reader 结束读取之后才能写入。实际生产实现还要审计 RefCount Overflow/Underflow、异常、Handle 双重移动/双重 Release、同一 Cache Line 上 Data 与 RefCount 的 False Sharing、Generation 复用。
- Writer 不应仅因为 refs==0 就默认历史可以淘汰；Recent Replay CAS Pin、Archive Copy 与 Retention 条件可能独立存在。
- 若 Reader 能绕过 OwnedHandle 随机按 Seq Lookup，此处的 refs 不能直接保证原子引用取得；仍需要 Stable State + Generation CAS Pin / HP / 等价安全 Lookup 协议。

**特别注意初始目标数 k 并非越早决定越好**：只要队列可能满/订阅变化/部分发布，RefCount 的分支承诺与消息投递都必须一致，不能在仍有 Writer 临时引用时错误回收。

### 19.8 RefCount 与 Transitive Completion Cursor：性能、复杂度及回收粒度

| 维度 | Intrusive Per-Data RefCount | Transitive Per-Stream Completion |
|---|---|---|
| Reader-side Atomic | Fork 时 fetch_add；最终所有者 fetch_sub | 通常为每 Stream release-store Cursor；可批量 |
| Cache Traffic | 多分支共享同一 RefCount Cache Line：RMW Bouncing | 单 Writer 更新自己的 Cursor；被其他 Core Read |
| 中间 P2→P3 转发 | Move Handle 不需额外 Ref RMW | P2 可透传/聚合下游 Safe Frontier |
| 发布者每条初始化 | 可以先 refs=k+1，一次 Store | 必须保存该 Data 的 Target + Required Orders |
| 乱序完成 | 不依赖连续完成水位，容易独立回收 | 需要 Completion Bitmap / Reorder / Ack 集合 |
| 慢消息 | 阻止该对象释放 | 可卡住整个连续 Cursor |
| 动态 Fork/DAG | 增加引用，最终释放 | 必须追踪新依赖、完成映射和失效边界 |
| 存储成本 | Per-Slot RefCount；已有 Pool 中可复用 | Per-Stream Cursor + Per-Data Retirement Requirements |
| Writer 观察 | 可查询该 Slot refs==0 | 查询对应 Target/Required Cursor |
| 独立 Pool 乱序回收 | 自然支持 | 纯单连续 Cursor 不支持 |
| 主要逻辑复杂度 | 所有权、Fork/Transfer、Final Release、ABA | Forwarded/Final Done 区分、Seq 对齐、Skip、HOL、动态拓扑 |

比较成本时不能误写成 Depth×2 次 Atomic RMW：

~~~text
优化 RC:
  Writer 原始 target 初始化 O(1) Store
  + 真正 Fork 次数 × fetch_add
  + 最终 Owner Release 次数 × fetch_sub
  + Writer 的观察与 Pool 回收

TCC:
  每 Stream Cursor Store/Batch
  + 上游 Acquire Poll
  + 每条消息的下游依赖 Mapping / Retired Metadata
  + 必要的乱序 Completion Tracker
~~~

若 k 个初始目标都独立使用且 Writer 预先初始化 refs=k+1、通过 Move 传递所有权，通常不需要 k 次独立 fetch_add，但最终仍有 k 次 Release RMW（加 Writer 临时引用的 Release）。Cursor 模式可通过 Batch 减少跨核 Progress Store，但可能加大 Reclamation Lag。

**Head-of-Line Blocking 示例**：

| Data Seq | 下游最终状态 | RefCount | 单个 Contiguous Cursor |
|---|---|---|---|
| 100 | 长耗时、未完成 | 1 | 99 |
| 101 | 已完成 | 0 | 99 |
| 102 | 已完成 | 0 | 99 |

若 Writer 拥有可任意分配 Free Slot 的 Pool，RC 可释放 101、102 对应 Slot；仅有 Cursor 前缀时不能安全判定它们可回收。若原始存储是固定 seq%Capacity Ring，即使 RC 知道 101、102 完成，Writer 仍可能必须等待 Seq100 的指定 Slot；**Per-Object Reclaimability 与 Fixed Ring Reuse Order 不同**。

性能是权衡，不是算法排名：共享 RefCount RMW 在高 Fan-out 热点上可能昂贵；但复杂 Cursor DAG、ACK Poll、Completion Bitmap、过晚回收造成的 Pool 压力，也可能比 RMW 更贵。必须测 Writer p99.9、End-to-End、Retired Bytes、Reader Suspension、HITM、NUMA。

### 19.9 第三种方案：Completion Event / Ack Queue

如果需要 Per-Object 乱序完成，又不希望所有下游对同一 Data 的 RefCount 执行 RMW，可以让最终 Reader 向 Owner 返回事件：

~~~text
P1 → P2 → P3
           ↓
       CompletionQueue
           ↓
P1 receives Done(slot,generation,requiredOrder)
   → marks corresponding outstanding obligation complete
   → reclaims Data when all obligations clear
~~~

优势：Reader 主要写自己独占的 SPSC Completion Queue，状态归并由 P1 完成；不同 Slot 可乱序回收；也可批量 ACK。代价：要有额外的返回 Queue 和容量预算、Generation 验证、双重 ACK 防护、故障/超时处理；若 P3 不止一个 Return Producer，则需要 N×SPSC 或 MPSC。Ack Queue 本身满了也有 Backpressure。

对固定少量下游、P1 是唯一 Reclaimer 的场景，这是一种值得与 RC/TCC 同台 Benchmark 的模型，不是通用免费替代品。

### 19.10 Hot Path 的 atomic<shared_ptr>、HP 与 CAS Pin：不要混入 Completion 语义

| 机制 | 解决什么 | 主要注意点 |
|---|---|---|
| C++20 atomic<shared_ptr<T>> | 并发安全地获取共享所有权、发布新 Snapshot | **不保证 Lock-Free**；反复 RefCount/内部同步及最后一次 Release 触发析构，对严格 p99.9 不友好 |
| Worker-Local shared_ptr + Control Event | 低频切换配置、Hot Path 只读本地版本 | 旧对象 Final Release/Destructor 仍可能发生在 Worker；必要时转给 Cold Reclaimer |
| HP | 保护已安全 Publish/Validate 的具体指针/对象，避免回收 | Per-Reader Hazard 发布与验证、Writer 退休扫描；HP 本身不通知新消息 |
| Stable Slot + Generation CAS Pin | 随机查询固定 Slot 当前版本，原子获得 Pin | Reader RMW / Writer Closing CAS；需防止旧版本复活；Pin 不通知新消息 |
| SPSC / Published Seq | 告诉 Reader 有什么新消息，以及顺序 | Delivery 并不天然替代 Lifetime Protection |

HP 与 CAS Pin 比较时要牢记：**HP = Per-Reader Hazard Records + Writer Hazard Scan；CAS Pin = Per-Slot Generation/Closing/RefCount Atomic State**。二者都不能让固定 Ring 在 Reader 仍访问目标 Slot 时安全原地覆盖。HP 在 Dynamic Pointer-Based Immutable Nodes 更自然；CAS Pin 在稳定 Slot、按 Seq Random Replay 更直接。实际性能需要实测；HP 即使主要用独占 Record Store，也可能需要昂贵的 Memory Ordering/Fence，并非零同步成本。

**新数据通知和对象保护是两个不同协议**：

~~~text
Notification / Visibility:
  SPSC tail, Published Seq, Atomic Current Pointer

Lifetime:
  OwnedHandle, Completion Frontier, RefCount, HP, CAS Pin

Retention:
  Recent Window, Segment Archive, Journal
~~~

最后一个 RefCount Owner 可以是 Hot Worker：可能触发 Destructor/Allocator，甚至取决于运行环境进入内核。Atomic SharedPtr 的具体实现也可能采用内部锁/Spin；C++20 只保证其原子语义，不承诺每次 load 都是普通 Lock-Free Pointer Load。Hot Path 应优先把 Config Ownership 按事件边界移交并隔离复杂销毁，而不是简单认为“没有显式 mutex 就绝不会等待”。

### 19.11 如何针对实际 Polling Processor 选择

推荐先确定每条消息的**生命周期合同**：

| Contract | 下游怎么使用 | 优先模型 |
|---|---|---|
| Sync Borrow | 当前函数返回前结束所有访问 | 原始引用 + 本地 Completed |
| Own/Move | 原始 Data 异步传给唯一最终使用者 | Move-Only OwnedHandle；可用 RC 或最终 Cursor |
| Fork Shared | P2/P3 同时继续读原始 Data | Intrusive RC / 严格 Completion Dependency |
| Independent Copy | 下游有独立 Payload | Copy/Transform at Async Boundary |
| Fixed FIFO End-to-End | 完整统一 Seq、无遗漏 Fork、按序完成 | Transitive Completion Cursor |
| Dynamic Selective DAG | 不同目标、不确定异步深度、乱序 | Intrusive RC，或更复杂 Ack/Completion Graph |
| Random Historical Lookup | 绕过原有 Queue 的新读取者 | Generation CAS Pin / Archive Lifetime |

**我对当前设计的默认建议**：

1. 保留 Preallocated Shared Pool 和现有 Intrusive RC，先确保 Move-Only Handle 真正做到 Transfer 无 ref++/ref--。
2. 对初始已知 Fan-out，优先评估一次 Store 建立目标引用和 Writer 临时持有；严格处理部分投递。
3. 对稳定的同步/FIFO Pipeline，可试 Per-Logical-Stream Completed；下游异步使用时，对外公布真正的 Transitive Safe Frontier。
4. 对动态多分支异步逃逸和乱序下游，不要为了消除 RefCount 强制引入全局连续 Cursor；先评估 Per-Object RC / Ack Queue。
5. 对 Recent Random Replay 保留与 Live Delivery 独立的 Generation/Pin/Retention 协议。
6. 在真实 Fan-out、消息大小、Pipeline 深度和慢 Reader 故障注入下量化尾延迟、Cache Bouncing、Pool 耗尽和回收滞后。

**判断置信度**：正确性不变量和模型区别高；哪个方案在目标 CPU/库上更快中等，必须用真实负载 Benchmark。课程式代码仅展示协议轮廓，不直接承诺 Production Ready。

---

## 20. 关键工程问题与 Benchmark

### 必测的正确性边界

- Writer 发布 Data 后，某个目标 SPSC 满；部分投递成功时如何回滚/重试？
- Reader 在 dequeue 之后、真正访问 Data 之前被 OS 抢占；Writer 是否会过早复用？
- Replay 在 Pin 成功之后被抢占；Writer tail latency / fallback 如何处理？
- Replay CAS 与 Writer Closing CAS 谁先赢？seq mismatch/closing/ref overflow 分别怎样处理？
- Reader 动态注销时，未完成投递如何 drain？新 Reader 是否会误用相同 ID 的历史进度？
- Reader 完成一个 Task 后仍有异步 callback 访问 Config*，checkpoint 是否错误地宣告安全？
- Pool 和 Queue 的 Slot 分开复用时，各自 generation / sequence 何时改变？
- 冷路径按 seq Lookup 超出 retention window，必须返回 unavailable，而不是读到被复用的 Slot。
- Copy Worker 落后时：Writer backpressure / bounded memory / 允许历史丢失，必须明确选择。
- Archive 中 seq index 和 Segment eviction 是否具有自己的 lifetime protocol？
- 即使引用计数为 0，Retention Policy 仍可能要求继续保留数据。

### Benchmarks

1. Writer 无 Replay：uncontended CAS 对比无 CAS Store/Progress；
2. Replay 随机 Lookup：成功率、CAS failure、延迟、Lookup 结果一致性；
3. Replay 线程被人为暂停 1–10ms，测 Writer p99.9/max 与 pinned Stall；
4. Selective fan-out target size = 1、2、4、8、全部，测 RMW/cache bouncing；
5. Per-Reader completed cursor 按每消息 vs batch 更新，测 cache-line traffic；
6. 各 Reader 速度不一致，比较多 SPSC Pool 的乱序回收与 Ring head-of-line blocking；
7. Copy Worker：messages/s × avg payload bytes 的额外内存带宽、LLC、NUMA 与 backlog；
8. Segment size 对 archive lookup、reclamation latency 和内存峰值的影响；
9. 除 p50/p99.9 之外，记录 max、backlog、retired bytes、CAS retries、data corruption / TSAN 结果。

### 性能结论（需现场实测验证）

- Fixed-size Value SPSC / MPMC 可能根本不需要通用内存回收。
- 如果只通过目标 Reader 的 Queue 取得对象，Per-Reader Progress 可以替代每条 Data 的原子引用计数。
- 如果 Replay Lookup 频率远低于 Writer Publish，现有稳定 Slot CAS Pin 可能在总成本上胜过始终复制全部 Data。
- 如果 Writer 的 CAS 或 Replay Pin 造成难以接受的 tail latency，专用 Replay Copy Archive 可以将 Cold Lookup 与 Original Pool 的生命周期解耦。
- 方案 B 不是免费的：Archive Copy Worker 是额外 Consumer；如果它跟不上，Writer 依旧需要 backpressure 或更大的保留容量。

### 推荐阅读

- Maged M. Michael, Hazard Pointers (IEEE TPDS, 2004)。
- Keir Fraser, Practical Lock-Freedom (2004)：epoch / non-blocking memory management。
- Userspace RCU (liburcu) 文档：QSBR 的 register / online / offline / quiescent。
- DPDK RCU QSBR Programmer's Guide。
- LMAX Disruptor：Gating Sequence 和多 Consumer 完成进度。
- Dmitry Vyukov bounded MPMC queue：per-cell sequence protocol。
- C++ standard / cppreference：atomic shared_ptr、atomic memory ordering、happens-before、object lifetime。

**代码安全声明**：本文件的多线程演示均以线程不异常终止、序号不回绕、容量约束满足、单 Writer 等明确假设为前提。不要把 demo 中的 spin-wait、线程管理、内存分配和简单回收逻辑直接用于生产 HFT 热路径；应先进行更严格的形式化审查、TSAN/ASAN/UBSAN、长时间 stress 与故障注入。
