# 系统级 C++ 设计面试：30 题（25 + HFT 专项 5）

> 整理日期：2026-10-23。原帖 25 题 + 5 道 HFT / Trading 专项题。面向 C++17、Linux、Infra / Low Latency / Trading；不是任何公司的真实题库。所有参考实现是**明确约束下的面试教学版本**，不是未经修改可直接上线的组件。
>
> [**一份完整的单文件版（题目描述 + C++ 答案 + 面试要点）**](../系统级C++设计30题.md)

## 使用方法

每道题包含：（1）需求/边界与接口；（2）C++17 参考代码；（3）复杂度与权衡；（4）面试追问和失败模式。各代码段是**参考片段**，按文中说明补齐头文件/依赖；本版本未配套自动化编译测试，不声称已逐段编译或达到生产级异常安全。

| 题号 | 题目 | 优先级 | 参考实现 |
|---|---|---|---|
| 1 | Thread-Safe LRU Cache | P0 | [并发与线程管理](01-并发与线程管理.md) |
| 2 | Bounded Blocking Queue | P0 | [并发与线程管理](01-并发与线程管理.md) |
| 3 | Thread Pool + Graceful Shutdown | P0 | [并发与线程管理](01-并发与线程管理.md) |
| 4 | Writer-Preferred Reader–Writer Lock | P1 | [并发与线程管理](01-并发与线程管理.md) |
| 5 | Work-Stealing Task Scheduler | P1 | [并发与线程管理](01-并发与线程管理.md) |
| 6 | Hash Map from Scratch | P1 | [内存与数据结构](02-内存管理与数据结构.md) |
| 7 | Custom Fixed-Block Allocator | P1 | [内存与数据结构](02-内存管理与数据结构.md) |
| 8 | Fixed-Size Object Pool | P1 | [内存与数据结构](02-内存管理与数据结构.md) |
| 9 | Lock-Free SPSC Ring | P1 | [内存与数据结构](02-内存管理与数据结构.md) |
| 10 | Configurable Eviction Cache | P1 | [内存与数据结构](02-内存管理与数据结构.md) |
| 11 | Asynchronous Logger | P1 | [OS 与系统编程](03-操作系统与系统编程.md) |
| 12 | Event-Driven I/O with epoll | P1 | [OS 与系统编程](03-操作系统与系统编程.md) |
| 13 | Shared-Memory IPC / SPSC | P1 | [OS 与系统编程](03-操作系统与系统编程.md) |
| 14 | Process Supervisor | P2 | [OS 与系统编程](03-操作系统与系统编程.md) |
| 15 | RAII FD Resource Manager | P1 | [OS 与系统编程](03-操作系统与系统编程.md) |
| 16 | Sharded Metrics Collector | P1 | [性能与可靠性](04-性能与可靠性.md) |
| 17 | Timer Scheduler | P0 | [性能与可靠性](04-性能与可靠性.md) |
| 18 | Connection Pool | P1 | [性能与可靠性](04-性能与可靠性.md) |
| 19 | Concurrent Rate Limiter | P1 | [性能与可靠性](04-性能与可靠性.md) |
| 20 | Producer–Consumer Pipeline | P0 | [性能与可靠性](04-性能与可靠性.md) |
| 21 | Hazard Pointer Reclamation | P1 | [高级系统设计](05-高级系统设计.md) |
| 22 | Non-blocking Networking Server | P1 | [高级系统设计](05-高级系统设计.md) |
| 23 | Distributed Job Scheduler | P2 | [高级系统设计](05-高级系统设计.md) |
| 24 | WAL + In-memory Index KV | P2 | [高级系统设计](05-高级系统设计.md) |
| 25 | CPU Resource Scheduler | P2 | [高级系统设计](05-高级系统设计.md) |
| 26 | Order State Machine / Fill/Cancel/Dedup | P0 | [HFT 专项](06-HFT交易系统专项.md) |
| 27 | Multi-Source Market Data Aggregator | P0 | [HFT 专项](06-HFT交易系统专项.md) |
| 28 | Price–Time Order Book / Matching | P0 | [HFT 专项](06-HFT交易系统专项.md) |
| 29 | Lock-Free MPSC Ring Buffer | P1 | [HFT 专项](06-HFT交易系统专项.md) |
| 30 | Snapshot + Delta Recovery | P0 | [HFT 专项](06-HFT交易系统专项.md) |

## 如何按 OA 时间分配

**P0（先练，2 小时 Coding）**：1、2、3、17、20、26、27、28、30。重点在 invariants、API、状态一致性、异常、关闭/恢复、测试覆盖。

**P1（熟悉关键实现 + 能白板解释）**：4–13（除 P0）、15、16、18、19、21、22、29。重点是数据结构、ownership、并发控制、内存序和性能。

**P2（系统设计讨论级）**：14、23、24、25。重点在故障模型、durability、版本/租约、故障恢复、隔离和复杂度，不需要 2 小时完成整个生产系统。

以上 P0/P1/P2 是备考收益的**主观优先级**，不是 DRW 实际考题频率，亦不是难度等级。

## 统一答题 checklist

1. **Contract**：谁调用？并发/单线程？容量？返回值？异常/取消/关闭含义？
2. **Invariants**：队列 size<=capacity；fill quantity<=order quantity；序列必须连续；资源有且仅有一个 owner。
3. **Linearization & Ordering**：提交、取消、执行、释放分别在哪一步生效？事件乱序如何处理？
4. **Ownership**：RAII、线程/任务/FD/内存生命周期、iterator 和 reference invalidation。
5. **Failure**：bad_alloc、回调抛异常、死锁、producer/consumer 卡住、断线、序列 gap、掉电。
6. **Performance**：先正确再优化；测 p50/p99/p99.9、吞吐与内存上限；避免伪共享、无界缓存、热点锁。
7. **Testing**：正常 + 极端边界 + 负载 + 随机 property testing + ASan/UBSan/TSan（注意 TSan 与部分 lock-free 模式要解读工具告警）。

## 与现有仓库内容的关系

- [有界阻塞队列与任务处理器](../有界阻塞队列与任务处理器.md)：#2/#3 的详细 OA 规格（推荐直接作为限时练习）。
- [订单状态机工程设计](../../hft/订单状态机工程设计.md)：#26 的恢复、event buffer、journal、outbox 与 FIX 讨论。
- 行情聚合、撮合、timer 等已有 OA 练习题：配合本索引使用，不替代其完整测试规格。

## 范围与限制

- #23 的 in-memory store 只是可替换的测试版，不支持真正跨机 durability；#25 是策略模拟，真正 OS 隔离仍要 cgroups/affinity。
- #24 没有 CRC、跨版本格式、compaction、目录 fsync；#22 未做全套连接错误处理/安全加固；#21 固定 reader slots。
- 测试驱动工程实现时，应把**示例 API + 前提条件**写入题目 harness，不允许默认业务客户端总是遵守隐含前提。
