# MST + Critical Connection Analysis（最小生成森林与关键连接）

> 自拟 C++17/20 两小时 OA 综合题，**不是已核实的 DRW 真题**。
> 重点：Kruskal + DSU、Tarjan Bridge、无向多重图、边 ID。
> 建议用时：120 分钟。P2。

## 1. 背景与任务

基础设施由 N 个节点和 M 条**无向加权连接**组成。每条连接 i 有唯一 edgeId=i，连接两个节点 u_i、v_i，建设成本为 w_i（可为负值，表示补贴或收益）。

请同时分析两个不同问题：

1. **Minimum Spanning Forest（最小生成森林）**：在每个原始连通分量内部选出使所有节点连通、总权重最小且不产生环的边集。计算整个森林的最小权重和，以及原始图的连通分量个数。
2. **Critical Connection / Bridge（桥）**：找出所有这样的 edgeId：从原始无向图删除**这一条具体的边**后，整个图的连通分量数会增加。

两者不可混淆：一条 MST 中被选中的边，不一定是原始图的 Bridge。

## 2. 输入格式

~~~text
N M
u1 v1 w1
...
uM vM wM
~~~

- 顶点编号 1..N。
- 第 i 行对应边 edgeId=i。
- 允许平行边、权重相同的边、自环和不连通图。
- 权重 w 可以为负、零或正。

## 3. 输出格式

恰好输出两行：

~~~text
FOREST componentCount minimumTotalWeight
BRIDGES bridgeCount id1 id2 ... idK
~~~

- componentCount 是原始图（忽略权重）的连通分量个数，包含孤立节点。
- minimumTotalWeight 是覆盖所有连通分量的 Minimum Spanning Forest 的最小权重之和；如果没有边，结果为 0。
- BRIDGES 中的 edgeId 按数字升序排列。
- 如果不存在桥，只输出 BRIDGES 0，不添加任何编号。
- 不要求输出具体最小生成森林的边列表；只输出权重。

## 4. 样例一：MST 边不一定是 Bridge

输入：

~~~text
5 5
1 2 1
2 3 2
1 3 2
3 4 3
4 5 1
~~~

输出：

~~~text
FOREST 1 7
BRIDGES 2 4 5
~~~

解释：

- 节点 1、2、3 构成一个三角形，因此它们内部的三条边都不是 Bridge。
- 边 4（3--4）和边 5（4--5）一旦删除就会增加连通分量数。
- 最小生成树可以选择边 {1,2,4,5} 或 {1,3,4,5}，总权重均为 1+2+3+1=7。
- 边 2 与边 3 虽然可能出现在某棵 MST 中，但都不是任何原始图意义下的 Bridge。

## 5. 样例二：平行边、自环与不连通图

输入：

~~~text
4 3
1 2 -2
1 2 5
3 3 -100
~~~

输出：

~~~text
FOREST 3 -2
BRIDGES 0
~~~

解释：节点 1、2 之间有两条不同边，删除任意一条仍然连通。节点 3 的自环不属于生成森林，也不是桥。原始图的连通分量是 {1,2}、{3}、{4}。

## 6. 规模约束

- 1 <= N <= 200000；0 <= M <= 200000。
- -10^9 <= w_i <= 10^9。
- 森林权重和保证在有符号 64 位整数范围内。
- 输入合法，无重复 edgeId（edgeId 由输入行自动生成）。
- 某些测试存在很长的 DFS 链；递归版 Tarjan 需要关注系统栈大小，允许改用显式栈。

## 7. 正确性要求

- Bridge 与边权无关，必须基于**原始无向图**计算。
- Kruskal 可以处理负边权：若连通分量中有可连接两个不同集合的负边，应照常选择。
- 不连通图输出的是最小生成**森林**，而不是仅报失败。
- 有向 SCC 的 Tarjan 与无向 Bridge 的 low-link 规则不可混用。
- 对于平行边，DFS 判断父边时必须按**edgeId**跳过来时那条边，而不能简单跳过所有指向父顶点的边，否则会把平行边错误识别为桥。
- 自环既不会连接两个不同 DSU 集合，也不会成为 Bridge。
- BRIDGES 按 edgeId 排序，与 DFS 遍历顺序无关。

## 8. 算法与复杂度目标

- Kruskal：按权重排序，使用 DSU 判断是否连接两个不同集合。目标 O(M log M + (N+M) alpha(N))。
- Tarjan Bridges：记录 discovery time 与 low-link；原始图中的边 e=(u,v) 是 DFS 树边时，若 low[v] > tin[u]，则 e 是桥。目标 O(N+M)。
- 输出桥编号排序：O(B log B)，B 为桥数量。

两套算法可以共享原始 edge list，但不要把“最小生成森林选中的边”和“Bridge”误认为同一个集合。

## 9. 加分扩展：所有 MSF 必选边（不属于必做）

**Minimum Spanning Forest Critical Edge** 是第三种不同定义：

> 某边若出现在该图的**每一棵**最小生成森林中，就称为 MSF Critical Edge。

对样例一，所有 MSF 必选边为 edgeId {1,4,5}，而原始图 Bridge 只有 {4,5}。因此 MSF Critical Edge 不等同于原始图 Bridge。

扩展输出（基础题不需要）：

~~~text
MSF_CRITICAL 3 1 4 5
~~~

思路：Kruskal 按相同权重分组；对于每一组，先把更小权重边连接的 DSU 分量压缩成临时图，再对**同权重候选边构成的多重图**做 Bridge 检测。临时图中的桥对应这一权重组里的必选边。完成判断之后，统一 union 本权重组的边。

额外思考：

- 为什么不能按单一某棵 Kruskal MST 的边集输出 MSF Critical？
- 为什么一定要**同权重边整组**判断，而不是逐边 union 后立刻判断？
- 如何处理平行边、同成本的多种选择和原始图不连通？

## 10. 自测清单

- N=1、M=0；全部孤立点；不连通多分量。
- 唯一一条边；链；三角形；完全图。
- 平行边阻止成为 Bridge；自环永远不是 Bridge。
- 负权、零权、相同权重；总权重可能为负。
- 某条边被 Kruskal 选中但不是 Bridge。
- 极深 DFS 链及桥结果排序。
