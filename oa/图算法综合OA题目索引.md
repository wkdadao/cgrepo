# 2 小时 C++ OA：图算法综合题目索引

> 自拟练习题，不是已证实的 DRW 或其他交易公司的真实面试原题。
> 每题独立提供完整的题面、输入输出协议、数值约束、样例、正确性要求、复杂度目标及加分扩展。
> **本套优先提供题面，不预置参考实现**，便于模拟限时独立完成。

## 题目导航

| 优先级 | 题目 | 核心算法 | 必做内容 |
| --- | --- | --- | --- |
| P0 | [Dependency Scheduler + Critical Path](./Dependency%20Scheduler%20%2B%20Critical%20Path.md) | Topo + DAG DP + Cycle | 动态依赖、判环、计算并返回关键路径 |
| P0 | [FX Arbitrage Engine](./FX%20Arbitrage%20Engine.md) | Bellman-Ford + Negative Cycle | 有向报价更新、全图套利判断 |
| P0 | [Routing Planner](./Routing%20Planner.md) | Dijkstra + State Graph | 跳数上限 + 一次性折扣 |
| P0/P1 | [Dependency Validation + SCC](./Dependency%20Validation%20%2B%20SCC.md) | Tarjan SCC + Condensation | 相互可达、是否在环上、压缩图查询 |
| P1 | [Dynamic Connectivity Queries](./Dynamic%20Connectivity%20Queries.md) | DSU + Reverse Offline | 仅删除边 + 连通查询；完整版 ADD/DEL 为扩展 |
| P1 | [Graph-Based Risk Exposure Propagation](./Graph-Based%20Risk%20Exposure%20Propagation.md) | SCC + DAG Bitset DP | 多路径汇合不能重复计入风险源 |
| P2 | [Capacity-Constrained Routing](./Capacity-Constrained%20Routing.md) | Min-Cost Flow | 流量守恒、容量约束、最小总费用 |
| P2 | [MST + Critical Connection Analysis](./MST%20%2B%20Critical%20Connection%20Analysis.md) | Kruskal + Tarjan Bridges | 最小生成森林与原始图 Bridge 区分 |

## 面试模拟规则

1. 每道题建议严格计时 120 分钟。先阅读全部要求、明确不变量和数据结构，再开始实现。
2. 基础要求为必须完成部分；“加分扩展”与 Production 优化不是默认验收范围。
3. 先确保完整输入输出与典型样例通过；预留时间覆盖边界、算法适用条件和复杂度。
4. 建议使用 C++17/20 + STL，默认单线程；不需要为了显得高性能而提前使用复杂锁结构或手写 allocator。
5. 对可达性、负环、最短路、流、桥等不同问题，不要直接套用‘一个通用 Graph 模板’而忽略语义差异。

## 重要建模区别

- **Dependency Scheduler**：在不限并发资源下完成时间是 DAG 顶点加权最长路径；如果有限制并发数，不再能简单套用。
- **FX Arbitrage**：有环不等于有套利；对可执行方向汇率取负对数后检测负环。
- **Routing Planner**：状态包含节点、已用跳数、是否用券；单个 dist[node] 不足以保留全部约束。
- **Dependency Validation**：SCC 表示相互可达；单节点 SCC 不一定在环上；压缩图是 DAG。
- **Dynamic Connectivity**：基础题为删除边 + 离线倒序 DSU；真正在线的混合 ADD/REMOVE 或离线线段树 + Rollback DSU 属于另一难度。
- **Risk Propagation**：按不同上游风险源去重计算，必须集合合并，简单标量 sum DP 会在菱形汇合时重复计数。
- **Capacity-Constrained Routing**：普通最短路不能直接代替有容量限制的分流优化；残量网络反向边会有负费用。
- **MST + Bridge**：某条边在一棵 MST 中、是原始图 Bridge、出现在所有 MST 中，是三个不同概念。

## 建议训练顺序

**第一梯队（优先）**：Dependency Scheduler → FX Arbitrage → Routing Planner → Dependency Validation + SCC。

**第二梯队（强化）**：Dynamic Connectivity Queries → Graph-Based Risk Exposure Propagation。

**第三梯队（选做）**：MST + Critical Connection Analysis → Capacity-Constrained Routing。

建议训练结束后自行总结：核心图模型、时间/空间复杂度、反例、关键不变量、输入输出最容易写错的点。真正评分依赖代码正确性、边界处理与约束下的性能，而不只是知道算法名称。
