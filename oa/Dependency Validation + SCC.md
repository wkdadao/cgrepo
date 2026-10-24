# Dependency Validation + SCC（依赖校验与强连通分量）

> 自拟 C++17/20 两小时 OA 综合题，**不是已核实的 DRW 真题**。
> 重点：Tarjan / Kosaraju SCC、Condensation DAG、可达性查询、环分析。
> 建议用时：120 分钟。P0/P1。

## 1. 背景与任务

给定一个由 N 个组件构成的**有向依赖图**。边 u -> v 表示：从组件 u 的依赖链能够进一步到达组件 v。

多个组件可能形成循环依赖。请实现一个静态依赖分析器：

1. 找出所有 Strongly Connected Components（SCC）。
2. 判定两个节点是否处于同一个 SCC。
3. 判定单个节点是否位于一个有向环上。
4. 判定从 u 是否存在一条有向路径到 v。

分析器需要使用 SCC 压缩图理解依赖层级：将每个 SCC 视为一个超级节点，则所有 SCC 之间的边组成 DAG。

本题中“路径”允许长度为 0，因此 REACH u u 恒为 YES；“位于有向环上”则要求至少有一条有向边形成闭合回路（长度 1 的自环也算）。

## 2. 输入格式

~~~text
N M Q
u1 v1
...
uM vM
command1
...
commandQ
~~~

- 顶点编号 1..N；边有方向。
- 允许自环与重复有向边。
- 所有命令在同一张静态图上查询，不改变边集合。

命令：

| 命令 | 含义 |
| --- | --- |
| REPORT | 输出所有 SCC 的完整划分 |
| SAME u v | u 和 v 是否在同一个 SCC |
| CYCLIC u | u 是否属于某条有向环 |
| REACH u v | u 是否能够沿有向边到达 v |

## 3. 输出格式

对于 REPORT：

~~~text
SCC componentCount
COMP k1 member1 member2 ...
COMP k2 member1 member2 ...
...
~~~

- 每行 COMP 后的第一个整数 k 是该 SCC 内的顶点数。
- 每个 SCC 内部按顶点编号升序排列。
- 各 SCC 按其**最小顶点编号升序**排列。
- REPORT 有 componentCount 个 COMP 行。

对于 SAME/CYCLIC/REACH，只输出 YES 或 NO，各占一行。

## 4. 样例

输入：

~~~text
6 7 6
1 2
2 1
2 3
3 4
4 3
4 5
5 6
6 5
REPORT
SAME 1 2
SAME 2 3
REACH 1 6
REACH 6 1
CYCLIC 5
~~~

注意：上面样例实际包含 **8 条边**，因此输入第一行应为：

~~~text
6 8 6
~~~

将此行与后面 8 条边及 6 条命令组合，完整有效输入是：

~~~text
6 8 6
1 2
2 1
2 3
3 4
4 3
4 5
5 6
6 5
REPORT
SAME 1 2
SAME 2 3
REACH 1 6
REACH 6 1
CYCLIC 5
~~~

输出：

~~~text
SCC 3
COMP 2 1 2
COMP 2 3 4
COMP 2 5 6
YES
NO
YES
NO
YES
~~~

解释：SCC 为 {1,2}、{3,4}、{5,6}；压缩 DAG 为 SCC1 -> SCC2 -> SCC3。

## 5. 单节点 SCC 与环

单节点 SCC **不一定有环**：

- 如果 u -> u 存在，自环使 CYCLIC u 返回 YES。
- 如果 SCC 只有 u 且不存在自环，则 CYCLIC u 返回 NO。
- 如果 SCC 有两个或更多节点，则其中每个节点都位于某条有向环上，CYCLIC 均为 YES。

即使图是 DAG，每个节点也都属于一个大小为 1 的 SCC；REPORT 仍需显示它们。

## 6. 约束

- 1 <= N <= 20000；0 <= M <= 100000。
- 1 <= Q <= 20000；其中 REACH 总数不超过 50。
- 所有 REACH 中，(SCC 数量 + 压缩边数) 之和不超过 3000000。
- 最多出现 5 条 REPORT 命令；所有 REPORT 输出顶点数之和不超过 100000。
- 输入编号与格式保证合法。
- 图可能存在长链，递归 DFS 时要注意系统栈深度；允许显式栈实现。

## 7. 正确性要求

- SAME 是“相互可达”，不等价于单方向的 REACH。
- SCC 压缩后每条跨 SCC 边可以去重，不应产生 SCC 层面的环。
- 对同一个 SCC 内的两个节点，REACH 恒为 YES。
- 不可从 u 到达 v，不代表不可从 v 到达 u。
- 自环不会改变 SCC 成员集合，但会改变 CYCLIC 结果。
- REPORT 的输出次序必须稳定，不依赖 Tarjan 弹栈的顺序。

## 8. 复杂度目标与建议组织

- 一次 Tarjan 或 Kosaraju：O(N+M)。
- 为每个节点建立 componentId，并统计每个 SCC 的大小、自环标记。
- 压缩图构造：O(N+M) 预期时间（若对边去重用排序则另有 log 因子）。
- SAME 和 CYCLIC：O(1)。
- REACH：在压缩 DAG 上 BFS/DFS，单次 O(C+Ec)。

可以用 REPORT 作为验收，也可以测试 SCC 压缩图是否真的无环（例如再做一次 Kahn 拓扑排序）。

## 9. 加分扩展（不属于必做）

1. 返回任意一个实际的循环依赖路径。
2. 输出 SCC 压缩 DAG 的拓扑顺序。
3. 对大量 REACH 查询，讨论 bitset 可达性 DP、离线查询以及空间换时间。
4. 定义“删除最少依赖边消除全部环”的问题，解释为何不能简单对每个 SCC 删除一条边就保证最优。
5. 增加动态 ADD/DEL，说明为什么一次性 SCC 不再直接满足更新要求。

## 10. 自测清单

- N=1，无边 / 有自环。
- 纯 DAG；完全强连通；多个彼此不连通的 SCC。
- 单向边连接两个 SCC；从后一个 SCC 查询前一个返回 NO。
- 平行边、自环、深 DFS 链。
- REPORT 的成员排序；重复 REPORT 结果一致。
