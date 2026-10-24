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

## 建议补充的 8 道题（待编写完整题面）

> 以下为扩展候选题目，**尚未创建独立题面 MD**。优先级是准备收益判断，并非 DRW 已确认的出题概率。

| 优先级 | 题目 | 核心算法 | 主要难点 |
| --- | --- | --- | --- |
| 很高 | Event Ordering & Replay Engine | Topo + Priority Queue | 事件因果顺序、乱序到达、确定性重放 |
| 很高 | Build System / Incremental Dependency Engine | DAG + Topo + DFS | 依赖变更、增量失效、最小重建集合 |
| 很高 | Multi-Venue Smart Order Router | Greedy + Heap + Optimization | 多交易所深度、手续费、容量、最优分配 |
| 高 | Resource-Constrained Task Scheduler | Topo + Heap + Scheduling | 有限 Worker、任务依赖、资源冲突 |
| 高 | Time-Dependent Routing | Dijkstra + Time State | 边的延迟随时间变化、最早到达 |
| 高 | Exchange Connectivity Failure Analysis | Tarjan Bridge + BCC | 关键节点、单点故障、网络韧性 |
| 中高 | Difference Constraints Validator | Bellman-Ford + Graph | 时间先后约束、矛盾检测、负环 |
| 中 | Minimum-Cost Dependency Resolution | DP + Graph / Set Cover | 依赖选择、共享成本、最小激活代价 |

### 补充题目设计提示

- **Event Ordering & Replay Engine**：定义 eventId、依赖集合、到达顺序、可执行条件和稳定 tie-break 规则；区分缺失依赖与循环依赖。
- **Build System / Incremental Dependency Engine**：明确边方向（依赖 -> 使用者）、变更后的受影响集合、增量重建顺序，以及新增环的处理。
- **Multi-Venue Smart Order Router**：基础版按可执行价格和手续费分配数量；固定费用、最小成交量等非线性约束应作为扩展，不能直接套用普通贪心。
- **Resource-Constrained Task Scheduler**：明确 Worker 数、任务时长、调度策略；有限资源下最优 makespan 一般不能简单用 DAG 关键路径计算。
- **Time-Dependent Routing**：指定边在何时可用、等待是否允许及 FIFO 性质；只有满足合适条件才能直接使用时间依赖 Dijkstra。
- **Exchange Connectivity Failure Analysis**：区分 Bridge（关键边）、Articulation Point（关键点）、Biconnected Components，并正确处理平行边。
- **Difference Constraints Validator**：将形如 x[v] - x[u] <= c 的约束转化为有向边，检测是否存在不可行的负环，并支持给出可行解。
- **Minimum-Cost Dependency Resolution**：必须约束输入规模或图结构；允许任意共享依赖和组合选择时，可能归约为 NP-hard 问题。

### 补充训练顺序

**优先补充**：Event Ordering & Replay Engine → Multi-Venue Smart Order Router → Build System / Incremental Dependency Engine。

**随后考虑**：Difference Constraints Validator → Resource-Constrained Task Scheduler → Time-Dependent Routing → Exchange Connectivity Failure Analysis。

**选做**：Minimum-Cost Dependency Resolution（先限制模型与规模）。

## 新增 8 道题：完整可编码题面

以下均为自拟 2 小时 OA 训练题，不是已确认的 DRW 真题。每道题独立运行，输入均为标准输入、输出均为标准输出，推荐 C++17/20。

### 9. Event Ordering & Replay Engine

**业务背景与任务**：多来源事件乱序到达，必须按依赖顺序执行；多个就绪事件按 (timestamp,eventId) 升序处理，保证重放结果确定。

**输入与约束**：N M；N 行 eventId timestamp；M 行 beforeId afterId。1≤N≤100000，M≤200000；ID 唯一，依赖可重复、可引用不存在 ID。

**输出与规则**：ORDER id1 ... idN；缺失引用输出 MISSING（优先于环）；存在环输出 CYCLE。

**样例输入**：
```text
4 3
40 5
10 1
30 3
20 2
10 30
20 30
30 40
```

**样例输出**：
```text
ORDER 10 20 30 40
```

**算法及边界重点**：哈希表映射 ID、依赖去重、Kahn + 小根堆；O((N+M)log N)。重点测自环、重复边、同时间戳和缺失依赖。

### 10. Build System / Incremental Dependency Engine

**业务背景与任务**：模块 v 依赖 u 记作 u→v；模块发生变更后，仅重建它和全部下游依赖者。支持依赖关系动态增删。

**输入与约束**：N M Q；M 行 u v；Q 行 ADD u v、DEL u v、CHANGE x。N≤2000，M≤10000，Q≤2000，CHANGE≤100；边为集合，重复操作无影响。

**输出与规则**：CHANGE 时如全图有环输出 CYCLE；否则输出 BUILD k v1 ... vk，包含受影响模块，按 ID 最小优先的拓扑顺序。

**样例输入**：
```text
5 4 4
1 3
2 3
3 4
2 5
CHANGE 2
DEL 3 4
CHANGE 2
CHANGE 1
```

**样例输出**：
```text
BUILD 4 2 3 4 5
BUILD 3 2 3 5
BUILD 2 1 3
```

**算法及边界重点**：每次 CHANGE 全图判环、求下游闭包，再对诱导子图小根堆拓扑排序；注意无关子图中的环也使查询失败。

### 11. Multi-Venue Smart Order Router

**业务背景与任务**：多交易所订单簿含价格、可成交量及每单位手续费。BUY 最小化含费总成本，SELL 最大化扣费净收入；每次查询基于同一快照。

**输入与约束**：L Q；L 行 venueId levelId price qty fee；Q 行 BUY qty 或 SELL qty。L≤100000，Q≤100；整数价格/费用/数量，档位 (venueId,levelId) 唯一。

**输出与规则**：量不足输出 IMPOSSIBLE；否则 TOTAL amount k，随后 k 行 venueId levelId executedQty；同有效价先选较小 venueId，再选较小 levelId。

**样例输入**：
```text
3 2
1 1 100 3 2
2 1 99 4 1
3 1 101 2 0
BUY 5
SELL 4
```

**样例输出**：
```text
TOTAL 502 2
2 1 4
1 1 1
TOTAL 398 2
3 1 2
1 1 2
```

**算法及边界重点**：BUY 有效价 price+fee，SELL 有效价 price-fee；排序后贪心分配，可部分成交。固定费用、最小成交量作为独立扩展，不可直接套贪心。

### 12. Resource-Constrained Task Scheduler

**业务背景与任务**：W 个相同 Worker 执行有依赖关系、不可抢占的任务。按确定性策略模拟调度，不要求全局最优 makespan。

**输入与约束**：N M W；N 行 taskId duration；M 行 u v（u 完成后才能开始 v）。N≤100000，M≤200000，1≤W≤N，duration≥1。

**输出与规则**：环输出 CYCLE；否则 MAKESPAN t，随后按 taskId 升序输出 taskId startTime workerId。就绪任务先选小 ID，空闲 Worker 先选小 ID；同刻先处理全部完成事件。

**样例输入**：
```text
3 1 2
1 3
2 2
3 4
1 3
```

**样例输出**：
```text
MAKESPAN 7
1 0 1
2 0 2
3 3 1
```

**算法及边界重点**：Kahn 判环 + 就绪任务堆 + 空闲 Worker 堆 + 完成事件堆；注意同一时刻多个完成、64 位时间。

### 13. Time-Dependent Routing

**业务背景与任务**：每条有向链路只能在周期性时刻发送，节点允许等待，求消息最早到达时间。

**输入与约束**：N M Q；M 行 u v period offset travel，允许发出的时刻 t 满足 t mod period = offset；Q 行 source target startTime。N≤100000，M≤200000，Q≤30，period≥1，0≤offset<period，travel≥1。

**输出与规则**：每个查询输出绝对最早到达时间，无法到达输出 -1；source=target 输出 startTime。

**样例输入**：
```text
3 3 2
1 2 5 0 2
2 3 4 1 3
1 3 10 0 10
1 3 0
1 3 1
```

**样例输出**：
```text
8
12
```

**算法及边界重点**：时间依赖 Dijkstra：计算不早于当前时间的下次发车时刻；允许等待且满足 FIFO。测试刚错过窗口和取模边界。

### 14. Exchange Connectivity Failure Analysis

**业务背景与任务**：无向网络存在多条平行物理链路，分析删除单条链路或单个网关后是否增加连通分量。

**输入与约束**：N M；M 行 edgeId u v；N≤200000，M≤300000；允许自环和平行边，edgeId 唯一。

**输出与规则**：BRIDGES k；下一行升序边 ID；ARTICULATIONS p；下一行升序节点 ID。没有结果时对应 ID 行为空行。

**样例输入**：
```text
5 5
10 1 2
11 2 3
12 3 1
13 3 4
14 4 5
```

**样例输出**：
```text
BRIDGES 2
13 14
ARTICULATIONS 2
3 4
```

**算法及边界重点**：Tarjan dfn/low；DFS 必须按父边 ID 跳过而非父节点；正确处理根节点、平行边、自环及非连通图。

### 15. Difference Constraints Validator

**业务背景与任务**：事件时间戳须满足差分约束 x[v]-x[u]≤c，判断是否存在一组整数时间戳。

**输入与约束**：N M；M 行 u v c；N≤2000，M≤10000，|c|≤10^6，变量可为负数。

**输出与规则**：不可行输出 IMPOSSIBLE；可行输出 POSSIBLE，下一行给出 N 个满足全部约束的整数 x1...xN。

**样例输入**：
```text
3 3
1 2 4
2 3 -2
1 3 5
```

**样例输出**：
```text
POSSIBLE
0 0 -2
```

**算法及边界重点**：约束转有向边 u→v、权重 c；Bellman-Ford 全点初始化 dist=0 检测负环，并以 dist 为可行解。

### 16. Minimum-Cost Dependency Resolution

**业务背景与任务**：根功能可以选择若干替代方案，每个方案要求激活一组子功能；激活节点需付成本，求根功能最低总成本。基础版限定为无共享子节点的树形方案依赖。

**输入与约束**：N；接下来 N 行 cost[i] k_i，后接 k_i 组 m child1...childm。N≤100000，方案引用总数≤200000；每个非根节点在所有方案中恰好出现一次，所有边形成以 1 为根的无环树，允许空方案。

**输出与规则**：输出 MIN_COST value；选择节点时支付自身 cost 并恰好选一个方案，方案中的所有孩子必须激活。

**样例输入**：
```text
5
3 2 1 2 1 3
4 1 2 4 5
6 1 0
2 1 0
1 1 0
```

**样例输出**：
```text
MIN_COST 9
```

**算法及边界重点**：树形后序 DP：dp[u]=cost[u]+min(各方案子节点 dp 之和)。若允许共享依赖且成本只计一次，普通树 DP 不再适用，可能 NP-hard。

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
