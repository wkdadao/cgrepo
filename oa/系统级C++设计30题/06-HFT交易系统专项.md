# 系统级 C++ 设计面试题 06：HFT / Trading 专项（26–30）

> 新增五题，针对 Market Data、OMS、Execution、Connectivity 与低延迟平台。代码使用 C++17；每题为清晰的面试最小实现，真实交易系统必须补充交易所方言、持久化、风控与异常恢复。本系列与仓库已有《订单状态机工程设计》等文档互补。

## 26. Order State Machine: Fills, Cancel & Dedup（P0）

**题目**：实现单订单状态变更：New Ack、Fill、Cancel Request、Cancel Ack、Cancel Reject。Fill 按 ExecID 去重；PendingCancel 期间的 Fill 必须正常累计；CancelAck 后可能仍收到迟到 Fill。订单固定原始数量，成交量不可越界。外部 Event Buffer 已校验 OrderID 与请求关联。

~~~cpp
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>

class OrderState {
public:
    enum class Status { PendingNew, Working, PendingCancel, Canceled, Filled };
private:
    int64_t total_, filled_ = 0;
    bool newAck_ = false, cancelPending_ = false, cancelAck_ = false;
    std::unordered_set<std::string> seenExecIds_;
public:
    explicit OrderState(int64_t qty) : total_(qty) {
        if (qty <= 0) throw std::invalid_argument("quantity");
    }
    Status status() const {
        if (filled_ == total_) return Status::Filled;
        if (cancelAck_) return Status::Canceled;
        if (cancelPending_) return Status::PendingCancel;
        return newAck_ ? Status::Working : Status::PendingNew;
    }
    void onNewAck() { newAck_ = true; }
    void requestCancel() {
        if (status() == Status::Working || status() == Status::PendingCancel)
            cancelPending_ = true;
        else throw std::logic_error("cancel not allowed");
    }
    void onCancelAck() { cancelPending_ = false; cancelAck_ = true; }
    void onCancelReject() { cancelPending_ = false; /* retain fills */ }
    bool onFill(const std::string& execId, int64_t qty) {
        if (seenExecIds_.count(execId)) return false;
        if (qty <= 0 || qty > total_ - filled_)
            throw std::invalid_argument("invalid fill quantity");
        seenExecIds_.insert(execId); // insertion before count, so OOM does not alter qty
        filled_ += qty;
        return true;
    }
    int64_t filled() const { return filled_; }
    int64_t leaves() const {
        return cancelAck_ ? 0 : total_ - filled_;
    }
};
~~~

**复杂度**：每个事件平均 O(1)；内存 O(distinct ExecIDs)。**面试要点**：状态不应作为所有事实的唯一来源；PendingCancel 与 PartialFill 是两个维度；late fill 不能静默丢弃；CancelReject 与 NewReject 不相同；FIX 39 OrdStatus vs 150 ExecType；ExecID 去重作用域；Exec correction/bust 并不在本题范围；CancelAck 后 late Fill 是否覆盖终态按 venue 规则定义。生产版须先 journal 再应用，保证 durable idempotency，并记录请求 ID、交易所 ID、发生时间。

## 27. Multi-Source Market Data Aggregator + Expiry（P0）

**题目**：多个 venue/source 各给 best bid/ask；每源的更新 seq 严格递增，若出现 gap 则不再信任该源，等待 snapshot；当心跳超时或报价 TTL 超时时剔除该源；聚合出 best bid 与 ask（可来自不同源）。price、qty 使用 int64_t。

~~~cpp
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>

class BestQuoteAggregator {
public:
    using Clock = std::chrono::steady_clock;
    struct Quote {
        int64_t bid = 0, bidQty = 0, ask = 0, askQty = 0;
        uint64_t seq = 0;
        Clock::time_point received{};
        bool stale = true;
    };
    struct Best {
        std::optional<std::pair<int64_t, int>> bid; // price, sourceId
        std::optional<std::pair<int64_t, int>> ask;
    };
private:
    std::map<int, Quote> sources_;
public:
    bool delta(int sourceId, Quote q) {
        if (q.bidQty < 0 || q.askQty < 0)
            throw std::invalid_argument("negative quantity");
        auto it = sources_.find(sourceId);
        if (it == sources_.end() || it->second.stale) return false;
        auto& old = it->second;
        if (q.seq <= old.seq) return false; // duplicate/old
        if (q.seq != old.seq + 1) {
            old.stale = true; // signal snapshot request to feed handler
            return false;
        }
        q.stale = false;
        old = q;
        return true;
    }
    void snapshot(int sourceId, Quote q) {
        if (q.bidQty < 0 || q.askQty < 0)
            throw std::invalid_argument("negative quantity");
        q.stale = false;
        sources_[sourceId] = q;
    }
    void markStale(int sourceId) { sources_[sourceId].stale = true; }
    Best best(Clock::time_point now, Clock::duration ttl) const {
        Best out;
        for (auto& [source, q] : sources_) {
            if (q.stale || now < q.received || now - q.received >= ttl) continue;
            if (q.bidQty > 0 && (!out.bid || q.bid > out.bid->first))
                out.bid = {{q.bid, source}};
            if (q.askQty > 0 && (!out.ask || q.ask < out.ask->first))
                out.ask = {{q.ask, source}};
        }
        return out;
    }
};
~~~

**复杂度**：delta O(log sources)，best O(sources)；每源固定 O(1) 数据。**面试要点**：严格序列号更新 vs heartbeat（heartbeat 可能单独提供 liveness）；本例仅 BBO，不支持多档 depth 和 source 删除；network backlog 会让 TTL 基于 receive time 显得“新”，必要时检查 exchange/source timestamp；stale 与 delete 业务含义；source stale 后聚合 cache 必须失效；跨 venue top-of-book 可 crossed，是否允许由策略决定；完整实现应允许 snapshot seq 与 buffered deltas 拼接。

## 28. Price–Time Priority Order Book / Matching（P0）

**题目**：实现单品种限价单撮合：价格优先，同价 FIFO；add 自动执行 crossing，未成交剩余挂单；cancel(id) 可以撤掉 resting order。整数价格、数量，单线程 owner。返回本次撮合明细。

~~~cpp
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>
#include <stdexcept>

class SimpleMatchBook {
    using Id = uint64_t;
    using Price = int64_t;
    struct Order { Id id; Price px; int64_t qty; };
    using Queue = std::list<Order>;
    using Bids = std::map<Price, Queue, std::greater<Price>>;
    using Asks = std::map<Price, Queue>;
    struct Ref { bool bid; Price px; Queue::iterator it; };
    Bids bids_;
    Asks asks_;
    std::unordered_map<Id, Ref> byId_;
public:
    struct Fill { Id maker, taker; Price px; int64_t qty; };
    bool cancel(Id id) {
        auto it = byId_.find(id);
        if (it == byId_.end()) return false;
        auto ref = it->second;
        if (ref.bid) {
            auto level = bids_.find(ref.px);
            level->second.erase(ref.it);
            if (level->second.empty()) bids_.erase(level);
        } else {
            auto level = asks_.find(ref.px);
            level->second.erase(ref.it);
            if (level->second.empty()) asks_.erase(level);
        }
        byId_.erase(it);
        return true;
    }
    std::vector<Fill> add(Id id, bool isBuy, Price price, int64_t qty) {
        if (qty <= 0 || byId_.count(id))
            throw std::invalid_argument("bad order");
        std::vector<Fill> fills;
        if (isBuy) {
            while (qty > 0 && !asks_.empty() && asks_.begin()->first <= price) {
                auto level = asks_.begin();
                auto& maker = level->second.front();
                int64_t q = std::min(qty, maker.qty);
                fills.push_back({maker.id, id, maker.px, q});
                maker.qty -= q; qty -= q;
                if (maker.qty == 0) {
                    byId_.erase(maker.id);
                    level->second.pop_front();
                    if (level->second.empty()) asks_.erase(level);
                }
            }
            if (qty) {
                auto& level = bids_[price];
                level.push_back({id, price, qty});
                byId_.emplace(id, Ref{true, price, std::prev(level.end())});
            }
        } else {
            while (qty > 0 && !bids_.empty() && bids_.begin()->first >= price) {
                auto level = bids_.begin();
                auto& maker = level->second.front();
                int64_t q = std::min(qty, maker.qty);
                fills.push_back({maker.id, id, maker.px, q});
                maker.qty -= q; qty -= q;
                if (maker.qty == 0) {
                    byId_.erase(maker.id);
                    level->second.pop_front();
                    if (level->second.empty()) bids_.erase(level);
                }
            }
            if (qty) {
                auto& level = asks_[price];
                level.push_back({id, price, qty});
                byId_.emplace(id, Ref{false, price, std::prev(level.end())});
            }
        }
        return fills;
    }
};
~~~

**复杂度**：add O(log price levels + matched resting orders)；cancel 预期 O(1) hash + O(log levels) map 查找。**面试要点**：同价 FIFO 和 list iterator 稳定性；maker price execution；Duplicate ID 只检查当前 resting order（已成交 ID 需外部全局去重）；partial fill；stop/iceberg 不在题目范围；异常在撮合循环中抛出可能发生部分 mutation，生产版需 transactional event/journal 或预分配；高性能可用 intrusive list + pool + price index，避免动态分配。

## 29. Bounded Lock-Free MPSC Ring Buffer（P1）

**题目**：多个 producer，一个 consumer；固定容量为 2 的幂。生产者 CAS 预定 slot，slot sequence 发布消息，消费者独占头指针；满时立即返回 false。T 必须 noexcept move construct/destruct，避免 reservation 后抛异常导致 slot 永久卡住。

~~~cpp
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

template<class T, size_t Capacity>
class MpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);
    struct Slot {
        std::atomic<size_t> seq{0};
        std::aligned_storage_t<sizeof(T), alignof(T)> storage;
    };
    std::unique_ptr<Slot[]> slots_{new Slot[Capacity]};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) size_t head_ = 0; // consumer only
public:
    MpscRing() {
        for (size_t i = 0; i < Capacity; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }
    bool try_push(T value) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        Slot* s;
        for (;;) {
            s = &slots_[pos & (Capacity - 1)];
            size_t seq = s->seq.load(std::memory_order_acquire);
            auto diff = static_cast<std::intptr_t>(seq - pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                       std::memory_order_relaxed)) break;
            } else if (diff < 0) return false;
            else pos = tail_.load(std::memory_order_relaxed);
        }
        new (&s->storage) T(std::move(value));
        s->seq.store(pos + 1, std::memory_order_release);
        return true;
    }
    std::optional<T> try_pop() {
        auto& s = slots_[head_ & (Capacity - 1)];
        size_t seq = s.seq.load(std::memory_order_acquire);
        if (seq != head_ + 1) return std::nullopt;
        auto* p = std::launder(reinterpret_cast<T*>(&s.storage));
        std::optional<T> out(std::move(*p));
        p->~T();
        s.seq.store(head_ + Capacity, std::memory_order_release);
        ++head_;
        return out;
    }
    ~MpscRing() {
        // Preconditions: all producer/consumer threads have stopped.
        while (try_pop()) {}
    }
};
~~~

**复杂度**：平均 O(1) 尝试；空间 O(Capacity)；存在 CAS 自旋，故操作并非 wait-free。**面试要点**：head/tail ownership、sequence 区分轮次与满空、publish release / consume acquire、slot reuse 同步、producer 在保留 slot 后挂起会阻止消费者进展（不是 lock-free overall progress 的无条件保证）；64-bit wrap-around 假设；false sharing；批处理；在确定性低延迟系统中优先选 SPSC partition/owner 而不是无条件使用 MPSC。

## 30. Snapshot + Delta Sequencing & Recovery（P0）

**题目**：market data 有 snapshot(seq, full book) 和 delta(seq, px, qty)。只按连续 seq 应用 delta；出现 gap 时暂停并请求 snapshot，暂存后续消息；snapshot 后丢弃老 delta，按顺序 replay 可衔接部分。设 qty=0 表示删除价位、单流单 symbol。

~~~cpp
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>

class SequencedBook {
public:
    using Price = int64_t;
    using Qty = int64_t;
    struct Delta { uint64_t seq; Price price; Qty qty; };
    enum class Result { Applied, Duplicate, NeedSnapshot, Buffered };
private:
    uint64_t lastSeq_ = 0;
    bool ready_ = false;
    std::map<Price, Qty> levels_;
    std::map<uint64_t, Delta> pending_;
    static constexpr size_t MAX_BUFFER = 4096;

    void apply(const Delta& d) {
        if (d.qty < 0) throw std::invalid_argument("negative qty");
        if (d.qty == 0) levels_.erase(d.price);
        else levels_[d.price] = d.qty;
    }
    void replayContiguous() {
        for (;;) {
            auto it = pending_.find(lastSeq_ + 1);
            if (it == pending_.end()) return;
            apply(it->second);
            ++lastSeq_;
            pending_.erase(it);
        }
    }
public:
    Result onDelta(Delta d) {
        if (d.qty < 0) throw std::invalid_argument("qty");
        if (ready_ && d.seq <= lastSeq_) return Result::Duplicate;
        if (pending_.size() >= MAX_BUFFER) {
            pending_.clear(); ready_ = false;
            return Result::NeedSnapshot;
        }
        pending_.emplace(d.seq, d);
        if (!ready_) return Result::NeedSnapshot;
        if (d.seq != lastSeq_ + 1) {
            ready_ = false;  // conservative: freeze until next snapshot
            return Result::NeedSnapshot;
        }
        replayContiguous();
        return Result::Applied;
    }
    bool onSnapshot(uint64_t seq, std::map<Price, Qty> fresh) {
        for (auto& [px, qty] : fresh)
            if (qty <= 0) throw std::invalid_argument("snapshot qty");
        levels_.swap(fresh);
        lastSeq_ = seq;
        for (auto it = pending_.begin(); it != pending_.end(); )
            if (it->first <= seq) it = pending_.erase(it);
            else ++it;
        ready_ = true;
        replayContiguous();
        if (!pending_.empty() && pending_.begin()->first != lastSeq_ + 1) {
            ready_ = false;
        }
        return ready_;
    }
    bool isReady() const { return ready_; }
    uint64_t lastSeq() const { return lastSeq_; }
    const std::map<Price, Qty>& book() const { return levels_; }
};
~~~

**复杂度**：每个 delta O(log buffered + log price levels)；snapshot O(full depth + buffered replay)。**面试要点**：snapshot 的 seq 边界，snapshot-before/after delta 的语义必须由 feed 协议定义；seq wrap-around/stream reset；一旦 gap，缓存的后续消息是否足够恢复取决于 snapshot 与 replay 来源；不能静默直接跳过缺失增量；心跳超时与 seq-gap 不同；ready=false 时禁止交易；恢复成功后原子替换对外可见 book；buffer 上限与快照反复失败策略。上述示例原子快照发布未实现，且 onSnapshot 对异常仅提供基本保证。

---
## HFT 专项的共同工程要点

- **单写者与可恢复输入**：保持每个 book/order 的明确 owner；接收、journal、状态提交、outbox 的一致性边界要写清。
- **Trading correctness 优先于微优化**：duplicate fill、seq gap、cancel/fill race、超时未知结果都不能假设是普通失败。
- **性能**：对象池/boost intrusive、preallocation、NUMA/affinity、cache alignment、bounded queues、Aeron/共享内存 IPC。
- **测试**：乱序、重复、部分执行、gap、snapshot 重放、timer stall、backpressure、进程重启、随机事件 property test。
