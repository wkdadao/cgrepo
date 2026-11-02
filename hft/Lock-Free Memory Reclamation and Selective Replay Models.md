# HFT C++：Memory Reclamation、Selective Fan-out 与 Sequence Replay 设计

> 讨论总结：Reference Counting / Hazard Pointer / Epoch-Based Reclamation (EBR) / QSBR / SPSC / MPMC / Selective Multicast / Replay CAS Pin / Replay Pointer Window / Replay Copy Archive  
> 更新：2026-11-02。示例以 C++20 为主；GCC/Clang 使用 -std=c++20 -O2 -pthread。  
> **定位**：设计笔记 + 可单独编译的教学代码。示例演示最重要的同步关系，不承诺 production-ready（尤其是容错、异常安全、overflow、动态重配置、shutdown）。

## 目录

1. 设计需求与正确性不变量
2. 为什么 SPSC/MPMC 有时足够，有时不够
3. Atomic Shared Ownership（引用计数）
4. Hazard Pointers（新增 Sequence Lookup、SC Fence、Folly）
5. EBR
6. QSBR
7. 动态 Reader 注册与退出
8. 固定容量 SPSC / MPMC 基础模型
9. 方式 A：多个 SPSC + 共享 Data Pool + Selective Fan-out
10. 方式 B1：共享 Ring + Per-Slot Pending Mask
11. 方式 B2：共享 Ring + Per-Reader Completed Sequence
12. Replay 方案 0：固定 Pool Slot 的 Generation + RefCount CAS Pin
13. Replay 方案 A：专用 Replay Reader 缓存 Data*
14. Replay 方案 B：专用 Replay Reader 完整复制 Data
15. Segment-Based Archive、折中方案、正确性检查与 Benchmarks

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

若使用一个所有 Reader 共用的 global sequence G(x)，并且每个 Reader 的完成进度是 **按投递顺序处理完成的最大 global seq**，可把 S(x,i) 换成 G(x)。但 Reader 的 cursor 必须体现「完成处理」而不是仅仅「dequeue」。

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

## 18. 关键工程问题与 Benchmark

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
