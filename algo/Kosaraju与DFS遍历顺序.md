# Kosaraju 与 DFS Order：Discovery、Finish、Reverse Finish

> 适用：有向图 SCC、DFS 拓扑排序、DAG DP、Graph Condensation。C++17 / OA 复习笔记。

## 1. 三种 Order 的本质

DFS 有两种关键事件：进入节点（enter）和退出节点（exit）。第三种顺序只是第二种顺序的反转。

| 名称 | 同义词 | 记录时机 | 核心含义 |
|---|---|---|---|
| Discovery Order | DFS Preorder | 节点第一次被发现时 | DFS 树的祖先先于后代 |
| Finish Order | DFS Postorder | DFS 遍历完该节点的所有出边时 | DFS 树的后代先于祖先完成 |
| Reverse Finish Order | Reverse Postorder (RPO) | 将 Finish Order 反转 | DAG 中的合法拓扑序；一般有向图中可用于确定 SCC 的提取顺序 |

示例 DFS 树（假设左邻居优先）：

```text
    A
   / \
  B   C
 /
D

Enter A → Enter B → Enter D → Exit D → Exit B
        → Enter C → Exit C → Exit A

Discovery:       A, B, D, C
Finish:          D, B, C, A
Reverse Finish:  A, C, B, D
```

记录模板（`visited` 应在首次进入时标记）：

```cpp
void dfs(int u) {
    visited[u] = true;
    discovery.push_back(u);
    for (int v : adj[u]) {
        if (!visited[v]) dfs(v);
    }
    finish.push_back(u);
}
```

注意：DFS Tree 的父子关系不等于原图所有边的依赖关系。Discovery Order **不能**保证是拓扑序。

## 2. DAG 中为何 Reverse Finish 是 Topological Order？

对于 DAG 任意边 `u -> v`，都有 `finishTime[u] > finishTime[v]`：

- DFS 先到 u：若 v 未访问，将先完成 v；若 v 已完成，则 v 也早于 u 完成。v 不可能正在递归栈中，否则出现环。
- DFS 先到 v：v 不可能在 DFS 中到达 u（否则再加上 u -> v 构成环），因此 v 在发现 u 前已经完成。

所以逆完成顺序对每一条边都满足 u 位于 v 前面。

反例说明 Discovery 不能替代 RPO：

```text
A -> C <- B
从 A 开始 DFS，再从 B 开始
Discovery:      A, C, B   (非法：B -> C)
Finish:         C, A, B
Reverse Finish: B, A, C   (合法)
```

DAG DP 必须采用合法拓扑序（或用 DFS Memoization 保证依赖先算完），不能直接沿普通 Discovery 顺序只更新一次。

## 3. 有环图中的 Reverse Finish：能用，但不是节点拓扑序

例：

```text
A <-> B -> C

SCC_X = {A,B}
SCC_Y = {C}
Condensation: SCC_X -> SCC_Y
```

原图有 B -> A 和 A -> B，不存在合法的**节点**拓扑序。但在 DFS 的 RPO 中，可以借助**每个 SCC 的最大 finishTime** 决定 SCC 处理先后：

若缩点 DAG 存在 `SCC_X -> SCC_Y`（X != Y），则

```text
maxFinish(SCC_X) > maxFinish(SCC_Y)
```

这是 Kosaraju 的基础。**不要误认为 RPO 会自动把每个 SCC 的节点连续排列**；同 SCC 的所有成员未必在 RPO 中连续。要通过第二次 DFS 才能分组。

证明直觉：如果 DFS 先进入 X，能沿 X -> Y 走到 Y，Y 会先完成；如果先进入 Y，Y 不可能通过某条路径反向到达 X，否则 X、Y 应属于同一个 SCC。故 X 的最大退出时间更晚。

## 4. Kosaraju 两遍 DFS

1. 原图 G 上进行完整 DFS forest，节点**退出时** append 到 `finish`。
2. 构造反向图 G^R：`u -> v` 改成 `v -> u`。
3. **按照 `finish` 从后往前**选取尚未分配的节点，在 G^R 上 DFS。
4. 每次第二遍 DFS 访问到的节点属于一个 SCC。直到全部分配完毕。

### 为什么必须反向？

原图缩点 DAG：

```text
X -> Y -> Z
```

第一遍的 RPO 按 SCC 的最大完成时间会首先选到源 SCC X。若第二遍仍用原图，则从 X 可到 Y、Z，错误地混在一起。

反向后：

```text
X <- Y <- Z
```

从 X 出发在反向图中无法走入其他尚未处理的 SCC；而 X 内部每个节点依然彼此可达，因此恰好遍历完整的 X。随后处理 Y、Z；通向已处理 SCC 的边会被 `comp != -1` 跳过。

### 具体实例

```text
A -> B -> C -> A
     |
     v
     D <-> E -> F

SCC: {A,B,C}, {D,E}, {F}
Condensation: {A,B,C} -> {D,E} -> {F}
```

假设 DFS 邻居顺序为 B 先到 C 再到 D，D 先访问 E、E 先访问 F，则可能得到：

```text
Discovery:       A, B, C, D, E, F
Finish:          C, F, E, D, B, A
Reverse Finish:  A, B, D, E, F, C
```

第二遍在反向图上：先从 A 提取 {A,B,C}；之后遇到 B 跳过；再从 D 提取 {D,E}；最后提取 {F}。

实际节点顺序取决于邻接表顺序，但 SCC 划分不变。

## 5. C++17 完整模板

```cpp
#include <algorithm>
#include <vector>
using namespace std;

class Kosaraju {
public:
    explicit Kosaraju(int n)
        : g_(n), rg_(n), seen_(n, false), comp_(n, -1) {}

    void addEdge(int u, int v) {
        g_[u].push_back(v);
        rg_[v].push_back(u);
    }

    // Single-build interface. Returns number of SCCs.
    int build() {
        for (int u = 0; u < (int)g_.size(); ++u) {
            if (!seen_[u]) dfs1(u);
        }

        int count = 0;
        // Iterate Finish Order backwards; no reverse() needed.
        for (int i = (int)finish_.size() - 1; i >= 0; --i) {
            int u = finish_[i];
            if (comp_[u] == -1) dfs2(u, count++);
        }
        return count;
    }

    const vector<int>& componentOf() const { return comp_; }

private:
    void dfs1(int u) {
        seen_[u] = true;
        for (int v : g_[u]) {
            if (!seen_[v]) dfs1(v);
        }
        finish_.push_back(u); // EXIT, not ENTER
    }

    void dfs2(int u, int id) {
        comp_[u] = id;
        for (int v : rg_[u]) {
            if (comp_[v] == -1) dfs2(v, id);
        }
    }

    vector<vector<int>> g_, rg_;
    vector<bool> seen_;
    vector<int> finish_, comp_;
};
```

复杂度：构造反向图与两次 DFS 都是 `O(V+E)`；额外空间 `O(V+E)`。递归 DFS 的最坏调用栈深度 O(V)，巨大图需改迭代版。模板设计为只调用一次 `build()` 且构建后不修改图；动态增删边后需重新初始化及计算。

## 6. 缩点图 Condensation

求得 `comp[u]` 后，枚举原图每条边 `u -> v`：若 `comp[u] != comp[v]`，在分量图添加 `comp[u] -> comp[v]`。

```cpp
vector<vector<int>> dag(componentCount);
for (int u = 0; u < n; ++u) {
    for (int v : g[u]) {
        int a = comp[u], b = comp[v];
        if (a != b) dag[a].push_back(b);
    }
}
for (auto& a : dag) {
    sort(a.begin(), a.end());
    a.erase(unique(a.begin(), a.end()), a.end());
}
```

缩点图**必然无环**：若多个 SCC 缩点后还能形成环，它们相互可达，本应属于同一 SCC。随后可以 Kahn + DAG DP、最长路径、最大收益、路径计数等。注意去重排序可能额外需要 O(E log E) 时间；允许重复边时仅构造可以 O(V+E)。

Kosaraju 按上面模板输出的 SCC 编号是缩点 DAG 的一个拓扑序（跨 SCC 边 a -> b 时，通常 a < b，这里事实上由该编号方法保证）；工程代码为了稳健、可读，仍可显式 Kahn 排序。

## 7. 常见误区与反例

| 说法 | 正确结论 |
|---|---|
| Discovery Order 可直接代替拓扑序 | 错：A -> C <- B，从 A 先 DFS 得 A,C,B |
| Finish Order 本身就是 DAG 的拓扑序 | 错：应取 **Reverse Finish** |
| 有环图不能求 RPO | 错：可以求，但不是原图的合法节点拓扑序 |
| 有环图的 RPO 自带 SCC 分组 | 错：只保证 SCC 的最大完成时间次序 |
| Kosaraju 第二次 DFS 可以继续在原图上跑 | 错：需要反向图（或使用对偶的反向图先遍历版本） |
| 第二遍按 Discovery Order 处理也行 | 错：例如 A -> B，首次 DFS 从 B 开始会出错 |
| 每个 SCC 都必须包含一个环 | 错：单点也是 SCC，即使没有自环 |
| 缩点能修复任务调度中的循环依赖 | 错：缩点只表达循环依赖组，不代表这些任务可执行 |

## 8. DFS / 图顺序速查表

| Order | 来源 | 常见用途 |
|---|---|---|
| DFS Discovery / Preorder | Enter | Tarjan 的 dfn、DFS 树、Euler 进入时间 |
| DFS Finish / Postorder | Exit | Kosaraju 第一遍、树上后序 DP |
| Reverse Finish / RPO | 倒序 Exit | DAG 拓扑序、Kosaraju SCC 提取顺序 |
| Topological Order | 所有边 u->v 都要求 u 先于 v | DAG 最短/最长路、Critical Path |
| BFS Order | 到起点的边数分层 | 无权最短路、连通性 |
| SCC Order | 强连通分量输出顺序 | 缩点 DAG；与实现编号约定有关 |

**要点**：Kosaraju 用 RPO 不是因为原图无环，而是因为 **SCC 缩点图一定无环**；第一遍的完成时间足以给出 SCC 级别的处理优先级，第二遍反向 DFS 才真正提取 SCC。

## 9. OA 测试清单

- n = 0，n = 1（无边 / 自环）
- A -> B（两个独立 SCC），A <-> B（单个 SCC）
- 两个环之间只有单向边
- 不连通图，需要 DFS forest（遍历所有节点）
- 多条重复边
- 复杂交叉边；邻接表顺序变化不应改变划分
- SCC 缩点后检查边方向与拓扑序
- 深链图的递归栈溢出风险

**记忆口诀**：第一次原图 DFS 按 *Exit* 排序；第二次沿 *反向图* 按 *Exit 倒序* 抽取 SCC。
