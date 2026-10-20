# Boost.Intrusive、Flat Hash Map 与 HFT Order Book 索引设计

> 面向 Feed Handler / L2、L3 Order Book / Multi-Venue Aggregator / FX Quote Expiry（C++17/20）。
>
> 设计讨论稿；具体 Venue 的协议语义优先于这里的通用策略。

## 0. 核心结论

**优先采用稳定对象池（OrderNode Pool / Level Pool）+ 按访问模式分别选索引。**

- OrderId → OrderNode*：优先 Benchmark Flat Hash Map。
- L3 每个 Price Level 的 FIFO：Boost.Intrusive List。
- 按 Price 排序：小簿 Sorted Vector，深簿且结构变化频繁时比较 Boost.Intrusive Set。
- 频繁 Top N 发布：可以考虑 Full-Depth Intrusive Set + Hot Sorted Vector<Level*>。
- 独立 Quote Expiry：可使用 Intrusive Expiry Set，也可选 PMR Set / Heap / Timing Wheel。
- Intrusive Unordered Set 适合静态 Bucket、稳定对象、严格管理内存的情形，但**不会天然比 Flat Hash 更快**。

**理由**：Intrusive 主要解决无额外容器节点分配、稳定地址、多索引共享对象；Flat Hash 主要优化 Hash Probe 局部性。这两类收益可以同时取得。

## 1. 先明确问题及不变量

Feed Handler / L3 需要处理：

1. OrderId / QuoteId 查找、Add/Modify/Cancel/Fill。
2. Price 排序、Best Bid/Ask、Top K、Level Insert/Delete。
3. Order 在同一 Price Level 内的顺序（若协议确实定义 FIFO）。
4. Venue Quote / Book 生命周期与 Expiry。
5. 单 Owner 热路径、预分配、NUMA、p99.9 延迟。

必须满足：

- 一个活跃 ID 映射到唯一活跃对象，不能通过 Pool Slot 重用错误复活旧 ID。
- OrderNode::level、Level::totalQty、FIFO 和 Price Index 一致。
- 用于排序/Hash 的 Key 改变前应解除索引、改变后重新插入。
- 对象在所有 Intrusive Hooks 解除之前**不可移动或销毁**。
- Hot Vector 中的 Level* 不能悬空。
- Quote 删除或过期后，必须保证聚合贡献被撤销/逻辑排除。

## 2. Boost.Intrusive 有哪些容器？

| 容器 | 底层 | 典型 HFT 用途 | 注意 |
| --- | --- | --- | --- |
| intrusive::slist | 单向链表 | Free List | 删除通常需要前驱 |
| intrusive::list | 双向链表 | L3 FIFO、Timing Wheel | 已知节点 O(1) unlink |
| intrusive::set / multiset | 红黑树 | Price、Expiry 排序 | O(log N) 查找/插入 |
| intrusive::unordered_set / unordered_multiset | Separate Chaining Hash | OrderId、QuoteId | 用户管理 Bucket Array |
| intrusive::avl_set | AVL 树 | 读多写少的有序索引候选 | 平衡更严格 |
| intrusive::splay_set / treap_set 等 | 特殊树 | 特殊访问分布 | 不是默认选择 |

Boost.Intrusive 没有独立的 intrusive::map<Key, Value> 类型；把 Key 放进业务对象，再使用 intrusive::set<T, Compare> 或 intrusive::unordered_set<T, Hash, Equal> 实现类似 Map 的查找。

对象可同时参与多个容器，但每个容器需要不同的 Hook。

## 3. Intrusive Unordered Set vs Flat Hash Map

### 3.1 内存结构

**Boost.Intrusive Unordered Set：**

~~~text
Bucket Array (preallocated)
   [0] -> OrderNode A -> OrderNode B
   [1] -> OrderNode C
   [2] -> null
   ...
OrderNode 属于业务 Pool，含 Hash Hook
~~~

- Separate chaining，Hash 冲突时沿节点链遍历。
- 每个 OrderNode 的 Hash Hook 在业务对象内部，无独立 Map Entry 分配。
- Bucket Array 由用户创建并维持生命周期；容器**不会自动 rehash**。
- 可明确控制 Bucket Count、Rehash 时机；但固定容量不保证固定碰撞链长度。

**boost::unordered_flat_map<OrderId, OrderNode*>：**

~~~text
Control Metadata: [..][..][..][..]
Entries: [Key,Ptr][Empty][Key,Ptr][Key,Ptr]...
                  |
                  +----> OrderNode in stable Pool
~~~

- Open addressing，Probe 多数发生在较紧凑的 Entry/Metadata 区域。
- Map Entry 可能在 Rehash 时移动，但指向 Pool 的 OrderNode* 地址保持稳定。
- reserve 可以减少扩容，不是不可逾越的硬容量上限。
- Insert/Delete 可能改变内部布局；不要把指向 Map Entry 的指针存入业务对象。

### 3.2 对比

| 维度 | Intrusive Unordered Set | Flat Hash Map<ID, Node*> |
| --- | --- | --- |
| Find 平均复杂度 | O(1) | O(1) |
| 极端碰撞最坏情况 | O(N) | O(N) |
| 查找局部性 | 依赖 Hook 与对象位置、链长 | 一般较好，集中 Probe |
| Node 对象地址 | 稳定，取决于 Pool | 稳定，因保存的是 Pool 指针 |
| Hash Index 空间 | Hook + Bucket Array | Key/Pointer Slots + Metadata |
| 元素节点分配 | 不需要 | Entry 在 Flat 表内 |
| Rehash | 显式管理 | 容量变化时可能发生 |
| 容量/生命周期 | 调用者承担较多职责 | 更易使用 |
| 可能的优势 | 固定容量、多 Hook、显式控制 | Lookup-intensive workload |

**关键辨析**：Intrusive 查找到的就是业务对象，Flat Map 查到后需要再解引用 Node*，但不能因此断言 Intrusive 更快。Separate chaining 可能多次随机读，Flat 的 Metadata/Entry Probe 可能更便宜。

64-bit 下 Hash Hook 可能在一个指针量级，但实际取决于 Hook 类型、选项、对齐、实现；Flat 表内存也必须计入空槽与 Metadata。不得把“每节点 8B”作为通用定论。

### 3.3 我的选择

- **优先**：flat_hash_map<OrderId, OrderNode*> + 稳定对象 Pool。
- 如果系统要求严格固定 Bucket、希望将 Hash Index 与业务对象整合，而且真实 Trace 显示其性能合格：采用 intrusive::unordered_set。
- 如果 OrderId 是可校验的紧凑整数 Slot ID，还可考虑 Direct Index / Generational Slot Array；但外部随机 ID 不适用。

## 4. 推荐的 L3 Order Book 结构

~~~text
Market Data Add / Modify / Cancel / Fill
                    |
      flat_hash_map<OrderId, OrderNode*>
                    |
             OrderNode Pool
                    |
             OrderNode::level
                    |
               Level Pool
                /     \
               /       \
     Level::FIFO      Price Index
     Intrusive List   Intrusive Set (full depth)
                              |
                       Hot Top-K Vector<Level*>
                       (可选派生缓存)
~~~

示例类型：

~~~cpp
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>

namespace bi = boost::intrusive;

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Qty = std::int64_t;

struct Level;

struct OrderNode {
    OrderId id{};
    Qty leavesQty{};
    Level* level{};                   // Stable pool object
    bi::list_member_hook<> fifoHook;
};

using OrderFifo = bi::list<
    OrderNode,
    bi::member_hook<OrderNode,
                    bi::list_member_hook<>,
                    &OrderNode::fifoHook>,
    bi::constant_time_size<false>
>;

struct Level {
    Price price{};
    Qty totalQty{};
    OrderFifo fifo;
    bi::set_member_hook<> priceHook;
};

struct PriceLess {
    bool operator()(const Level& a,
                    const Level& b) const noexcept {
        return a.price < b.price;
    }
};

using PriceTree = bi::set<
    Level,
    bi::member_hook<Level,
                    bi::set_member_hook<>,
                    &Level::priceHook>,
    bi::compare<PriceLess>,
    bi::constant_time_size<false>
>;

using OrderIndex = boost::unordered_flat_map<OrderId, OrderNode*>;
~~~

代码仅展示数据模型，不包括 Pool 生命周期、异常处理、Bid/Ask 方向、Venue 规则及 I/O。

**典型更新路径：**

- Qty-only Modify：找到 OrderNode*，修改 qty 与 Level::totalQty；不修改 Price Tree。
- Cancel/Fill：按 ID 找到 Node，处理剩余数量；若 Order 结束，则从 FIFO 与 ID Index 移除。
- Level 变空：先更新 Hot Vector，再从 Price Tree 移除，最后回收 Level Pool Slot。
- Add New Price：在 Price Tree 中插入新 Level，再更新 Top K 边界。

## 5. Intrusive Set vs Sorted Vector<Level*>

| 操作/特性 | Sorted Vector<Level*> | Intrusive Set<Level> |
| --- | --- | --- |
| Price Lookup | O(log N) | O(log N) |
| 已知 Level* 更新 Qty | O(1) | O(1) |
| 插入新 Level | O(N) 指针搬移 | O(log N) |
| 删除已有 Level | O(N) 指针搬移 | 按 Iterator 删除可很便宜 |
| Best Bid/Ask | O(1) | O(1) |
| Top K 遍历 | 连续指针数组 | Tree Iterator |
| Level 地址稳定性 | 可通过 Pool 保持 | 可通过 Pool 保持 |
| 每 Level 内存 | Vector 指针容量 | Tree Hook |

**不能按 Big-O 直接判胜负。** Top 10 / Top 20 时，连续内存移动很便宜；树的指针跳转未必有优势。

选择建议：

- N 较小、Top K 读取特别频繁：Sorted Vector / vector<Level*>。
- N 较大、Price 插删频繁：Intrusive Set 值得 Benchmark。
- 需 Full Depth + 高频 Top K：Intrusive Set + Hot Vector，但维护双索引也有成本。
- Tick 非常密集的 Futures：Direct Indexed Array / Ring Book 是独立候选。

注意：Full-Depth Intrusive Set 的内存节点来自 Pool，不意味着按 Price 顺序在物理内存中连续。

## 6. Intrusive Unordered Set 的核心用法

它与 Flat Hash 的一个重要区别是 Bucket Array 由调用者提供：

~~~cpp
#include <boost/intrusive/unordered_set.hpp>
#include <array>
#include <cstdint>
#include <functional>

namespace bi = boost::intrusive;

struct IndexedOrder {
    std::uint64_t id{};
    bi::unordered_set_member_hook<> hashHook;
    bi::list_member_hook<> fifoHook;
};

struct IdHash {
    std::size_t operator()(const IndexedOrder& x) const noexcept {
        return std::hash<std::uint64_t>{}(x.id);
    }
};
struct IdEqual {
    bool operator()(const IndexedOrder& a,
                    const IndexedOrder& b) const noexcept {
        return a.id == b.id;
    }
};

using IdSet = bi::unordered_set<
    IndexedOrder,
    bi::member_hook<IndexedOrder,
                    bi::unordered_set_member_hook<>,
                    &IndexedOrder::hashHook>,
    bi::hash<IdHash>,
    bi::equal<IdEqual>
>;

// Bucket 在 IdSet 整个生命周期内必须有效
std::array<IdSet::bucket_type, 32768> buckets{};
IdSet ids{IdSet::bucket_traits(buckets.data(), buckets.size())};
~~~

注意事项：

1. 每个 Intrusive Hash 容器需要自己独占的 Bucket Array，不能共享 Bucket 对象。
2. 节点由外部 Pool 拥有，销毁前解除 Hook。
3. 已经插入的对象不得直接修改 id；先 remove 再 reinsert。
4. 若要仅用整数 ID 查找，建议使用 Key-only Hash/Equal Lookup 适配器，避免构造完整 IndexedOrder。
5. 静态 Bucket 不会扩容，但装载太高会产生长 Collision Chain。

## 7. std::set 的连续内存预分配 vs Intrusive Pool

### 7.1 PMR 路线

~~~text
std::pmr::set<Expiry>
         |
unsynchronized_pool_resource
         |
monotonic_buffer_resource
         |
预分配对齐 Buffer
~~~

可复用标准树节点，并降低通用分配器开销。**monotonic_buffer_resource 单独使用时删除节点不回收内存，因此不能直接用于长期高频 Replace。**

std::set 的 Tree Node 仍是内部对象，Pool 只改善节点来源；按 Key 遍历仍会沿树结构移动。

### 7.2 Intrusive 路线

~~~text
Level Pool: [L0][L1][L2][Free][L4]...
            |   |   |
        每个 Level 含 Tree Hook
               |
          Intrusive Set
~~~

优点：不单独 Allocate Tree Node，对象身份、内存和多个索引可统一管理。

代价：固定地址、解除 Hook 后回收、生命周期、Pool 容量耗尽、ABA、防悬空指针均由业务代码负责。

C++17 std::set 还可以使用 node handle 的 extract → 修改 Key → insert，复用节点存储而不做 erase+allocate；仍有树操作成本，适合作为 Benchmark 基线。

## 8. Price Expiry：何时给 Level/Book 设置 Timer？

Price Expiry 属于 Feed Handler 对上游协议语义的解释，不应所有 Venue 都给每个 Level 默认 TTL。

- Exchange L2/L3：没更新的 Level 可能是合法静止挂单；一般依赖显式 Delete，以及 Sequence Gap / Feed Health 的 Stale 管理。
- FX LP Streaming：按协议决定 Quote-Level、QuoteSet-Level 或 Book-Level Expiry。
- Quote 过期时：Feed Handler 应转换为带自身 MDLite Sequence 的 Delete Delta；Pricing Engine 按序撤销对应聚合贡献。
- Book/Source 失效：可独立发送 Invalidate，而不强制先删除数千个 Level；Cached Aggregate 必须相应过滤/撤销失效 Venue 贡献。
- Heartbeat 与 Sequence 能发现若干故障，但无丢包、未超时的 Queue Backlog 仍可能使 Pricing Engine 使用旧数据；严格 Freshness 需独立业务约束。

### 8.1 Expiry 容器对比

| 方案 | 优势 | 成本/风险 |
| --- | --- | --- |
| std::set + PMR Pool + iterator | 简洁、可删除、内存有界 | RB Tree 随机访问 |
| std::set extract + insert | Replace 复用存储 | 仍需重排 |
| Intrusive Set + Quote Pool | 多索引共享对象、无额外节点分配 | Hook 生命周期更复杂 |
| Lazy priority_queue + generation | 连续 Heap、写路径简单 | 更新频繁时积累失效 Timer |
| Indexed Heap | 有界，准确排序 | Sift 影响多个 heapIndex，必须同步更新 |
| Intrusive Timing Wheel | Tick 粒度下 O(1) 取消/加入 | 精度、跨圈、到期风暴 |
| One-Shot + Recheck | 频繁刷新稳定 Slot 时减少调度 | Replace 语义和晚触发边界 |

**One-Shot 的边界：** 每次 Quote Update 若只是同一稳定 Quote Slot 延长 expireAt，可保持旧 Timer，到期时再检查并重排。若业务上是 Delete old Quote + Add new Quote 且用新对象管理生命周期，取消旧 Timer、安排新 Timer 往往更直接。逻辑 Replace 不必然要求物理删除，但不要为了优化而更改协议生命周期语义。

Strict TTL 还应区分 logical expiry 与 timer cleanup 的时间误差。

## 9. Multi-Venue Order Book 聚合补充

- Local Book 是真实状态，Aggregated Book 是派生视图。
- 相同 Price 的 Total Qty 需保留 Venue Contribution，或者可在失效时重建有效聚合。
- Source Stale 不意味着全部逐 Level Delete，但如果缓存 totalQty 包含 Stale Venue，仅设置一个 Flag 而继续返回旧 totalQty 是错误的。
- Venue 原生 Sequence 与 MDLite 发布 Sequence 是两套概念；本地 Expiry Delete 必须进入 MDLite 顺序及 Snapshot/Delta 恢复机制。
- 多个 Venue 不存在天然统一 FIFO；聚合 L2 Total Qty 不等于跨 Venue 的 L3 时间优先队列。
- FX 累积规模报价不一定能按价格档位直接相加，须确认 LP 语义。

## 10. Benchmark 设计

不能只测单一 Key 的 find()。使用相同 Trace 与预热条件做公平测试：

**对比组**：std::unordered_map、boost::unordered_flat_map、absl::flat_hash_map（若允许）、boost::intrusive::unordered_set、Direct Index（若 ID 可映射）。

| 维度 | 示例 |
| --- | --- |
| 活跃 Orders | 1K / 10K / 100K / 1M |
| Lookup 比例 | 50% / 80% / 95% |
| Insert / Erase | 平衡与 Burst |
| OrderId 分布 | 递增、随机、Hot/Cold 倾斜 |
| 负载 | 仅 Lookup vs Lookup+更新 OrderNode |
| 内存 | 连续 First-touch Pool、随机空洞、固定 NUMA |
| 碰撞 | 正常分布、刻意恶化 |

收集：p50 / p99 / p99.9、cycles/op、LLC miss、DTLB miss、branch miss、吞吐、峰值内存、额外分配次数；报告 CPU / 编译器 / Boost 版本 / 优化选项 / affinity。

保证各方案处理的是相同操作序列并验证最终状态。要测真实 OrderNode 访问，而不是只找到 Map Entry 就结束。对于 Pool 和 Intrusive，应特别关注 Delete/Re-add 的极端情况、容量耗尽和内存回收。

## 11. 面试速答

> 我会把业务对象与索引分离：OrderNode 和 Level 由预分配 Pool 管理，地址稳定。OrderId 查找先用 Flat Hash Map<OrderId, OrderNode*>，因为 Hash Probe 的局部性通常较好；L3 FIFO 使用 Intrusive List；全深度价格排序根据 Level 数和结构变更频率比较 Sorted Vector 与 Intrusive Set。必要时加 Top K 指针缓存。Intrusive Unordered Set 可以免去额外节点分配并显式控制 Buckets，但 collision chaining 未必比 Flat Hash 快，需以真实操作分布和 p99.9 Benchmark 决定。

## 12. 官方参考

- [Boost.Intrusive](https://www.boost.org/doc/libs/latest/doc/html/intrusive.html)
- [Boost.Intrusive unordered_set / unordered_multiset](https://www.boost.org/latest/doc/html/intrusive/unordered_set_unordered_multiset.html)
- [Boost.Unordered flat_hash_map](https://www.boost.org/doc/libs/latest/libs/unordered/doc/html/unordered/reference/unordered_flat_map.html)
- [Boost.Intrusive auto_unlink 注意事项](https://www.boost.org/doc/libs/release/doc/html/intrusive/auto_unlink_hooks.html)

**结论置信度**：结构、内存与复杂度原则为高；具体性能排序无法仅凭理论确定，须 Benchmark。
