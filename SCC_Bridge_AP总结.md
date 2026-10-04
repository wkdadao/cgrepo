# SCC / Bridge / Articulation Point 总结

> 目标：系统理解有向图 SCC、无向图 Bridge / Articulation Point（AP）的定义、Tarjan / Low-Link 思想、常见易错点和面试可复用 C++ 模板。

---

## 1. 总体框架

图的“连通性”问题可以先按 **有向图 / 无向图** 分开：

```text
Graph Connectivity
│
├── Directed Graph
│   └── Strongly Connected Components (SCC)
│       ├── Tarjan SCC
│       ├── Kosaraju
│       └── SCC Condensation DAG
│
└── Undirected Graph
    ├── Edge Criticality
    │   └── Bridge / Cut Edge
    │
    └── Vertex Criticality
        └── Articulation Point / Cut Vertex
```

最核心的定义：

- **SCC**：有向图中，一组节点之间任意两点互相可达。
- **Bridge**：无向图中，删除某条边后，连通分量数量增加。
- **Articulation Point (AP)**：无向图中，删除某个节点以及相关边后，连通分量数量增加。

三者都会使用 DFS 时间戳和 `low`，但 **SCC 的 low-link 语义和 Bridge/AP 的 low-link 语义不完全一样**，不能机械混用。

---

# 2. DFS 时间戳：dfn / tin

常见命名：

```cpp
dfn[u]
tin[u]
disc[u]
```

含义相同：

> 节点 `u` 第几次被 DFS 首次访问。

例如：

```text
DFS visit order:

0 -> 1 -> 3 -> 4 -> 2

dfn:
0 = 1
1 = 2
3 = 3
4 = 4
2 = 5
```

通常：

```cpp
dfn[u] = low[u] = ++timer;
```

---

# 3. Low-Link 的统一直觉

虽然 SCC 和 Bridge/AP 中 `low` 的严格定义不同，但可以先形成一个统一直觉：

> `low[u]` 描述从 `u` 或 `u` 的 DFS 子树出发，能够“绕回”多早的位置。

对于无向图 Bridge/AP，可以理解成：

> `low[u]` = `u` 的 DFS 子树通过 tree edge + 至多一个向祖先的 back edge，能够到达的最小 `dfn`。

于是：

```text
u
|
v
|
v subtree
```

如果 `v subtree` 有“后门”回到很早的祖先，那么 `low[v]` 会很小。

如果完全没有后门，那么父子 tree edge 就可能是 Bridge，父节点就可能是 AP。

---

# 4. SCC：Strongly Connected Components

## 4.1 定义

在 **有向图** 中，如果一个节点集合中的任意两个节点 `u, v` 都满足：

```text
u -> v reachable
v -> u reachable
```

那么这些节点属于同一个 SCC。

例如：

```text
0 -> 1 -> 2
^         |
|---------|

2 -> 3 -> 4
     ^    |
     |----|
```

可以得到：

```text
SCC 1 = {0,1,2}
SCC 2 = {3,4}
```

---

## 4.2 SCC 最重要的应用：缩点

把一个 SCC 压缩成一个节点。

原图可能有大量环：

```text
A <-> B -> C
^     |    |
|-----D    E <-> F
```

压缩 SCC 后：

```text
SCC1 -> SCC2 -> SCC3
```

### 关键性质

> **SCC 缩点后一定是 DAG。**

原因：

如果缩点后还有：

```text
A -> B -> C -> A
```

那么 A、B、C 实际上互相可达，本来就应该属于同一个 SCC。

所以：

```text
Directed Graph with cycles
        ↓
       SCC
        ↓
Condensation DAG
        ↓
Topo Sort / DAG DP
```

这是 SCC 最重要的建模价值。

---

# 5. Tarjan SCC

Tarjan SCC 维护：

```cpp
dfn[u]
low[u]

stack
inStack[u]
```

核心 DFS：

```cpp
dfn[u] = low[u] = ++timer;
stack.push_back(u);
inStack[u] = true;

for (int v : graph[u])
{
    if (!dfn[v])
    {
        dfs(v);
        low[u] = min(low[u], low[v]);
    }
    else if (inStack[v])
    {
        low[u] = min(low[u], dfn[v]);
    }
}

if (low[u] == dfn[u])
{
    // u is the root of one SCC
}
```

---

## 5.1 为什么 SCC 需要 inStack

在有向图中，遇到一个已经访问过的节点 `v`，它可能：

- 是当前 DFS 搜索中尚未完成的节点；
- 已经属于一个完成的 SCC；
- 是其他 DFS 路径中的节点。

只有：

```cpp
inStack[v] == true
```

才说明 `v` 仍属于当前尚未完成的 SCC 搜索区域。

所以必须：

```cpp
else if (inStack[v])
{
    low[u] = min(low[u], dfn[v]);
}
```

而不是对所有 visited 节点都更新。

---

## 5.2 为什么这里用 dfn[v]，不是 low[v]

对于：

```cpp
else if (inStack[v])
```

这条边是当前 `u -> v` 直接看到的一条边。

我们明确知道：

```text
u can directly reach v
```

所以用：

```cpp
low[u] = min(low[u], dfn[v]);
```

DFS 子节点返回时才传播：

```cpp
low[u] = min(low[u], low[v]);
```

这是标准 Tarjan SCC 写法。

---

# 6. Tarjan SCC 完整 C++ 模板

```cpp
#include <bits/stdc++.h>
using namespace std;

class TarjanSCC
{
public:
    explicit TarjanSCC(int n)
        : n_(n),
          graph_(n),
          dfn_(n, 0),
          low_(n, 0),
          inStack_(n, false),
          compId_(n, -1)
    {
    }

    void addEdge(int u, int v)
    {
        // Directed edge: u -> v
        graph_[u].push_back(v);
    }

    void build()
    {
        // Directed graph may not be reachable from one start node.
        for (int u = 0; u < n_; ++u)
        {
            if (dfn_[u] == 0)
            {
                dfs(u);
            }
        }
    }

    int componentCount() const
    {
        return compCount_;
    }

    const vector<int>& componentIds() const
    {
        return compId_;
    }

    const vector<vector<int>>& components() const
    {
        return components_;
    }

private:
    void dfs(int u)
    {
        dfn_[u] = low_[u] = ++timer_;

        stack_.push_back(u);
        inStack_[u] = true;

        for (int v : graph_[u])
        {
            if (dfn_[v] == 0)
            {
                // DFS tree edge.
                dfs(v);

                // Child can propagate its low-link value upward.
                low_[u] = min(low_[u], low_[v]);
            }
            else if (inStack_[v])
            {
                // v belongs to the current unfinished DFS/SCC region.
                low_[u] = min(low_[u], dfn_[v]);
            }
        }

        // u is the root of one SCC.
        if (low_[u] == dfn_[u])
        {
            vector<int> component;

            while (true)
            {
                int x = stack_.back();
                stack_.pop_back();

                inStack_[x] = false;
                compId_[x] = compCount_;
                component.push_back(x);

                if (x == u)
                    break;
            }

            components_.push_back(move(component));
            ++compCount_;
        }
    }

private:
    int n_;

    vector<vector<int>> graph_;

    vector<int> dfn_;
    vector<int> low_;

    int timer_ = 0;

    vector<int> stack_;
    vector<bool> inStack_;

    vector<int> compId_;
    vector<vector<int>> components_;

    int compCount_ = 0;
};


int main()
{
    TarjanSCC scc(8);

    scc.addEdge(0, 1);
    scc.addEdge(1, 2);
    scc.addEdge(2, 0); // SCC {0,1,2}

    scc.addEdge(2, 3);

    scc.addEdge(3, 4);
    scc.addEdge(4, 5);
    scc.addEdge(5, 3); // SCC {3,4,5}

    scc.addEdge(5, 6);

    scc.addEdge(6, 7);
    scc.addEdge(7, 6); // SCC {6,7}

    scc.build();

    cout << "SCC count = "
         << scc.componentCount()
         << '\n';

    const auto& comps = scc.components();

    for (int i = 0; i < static_cast<int>(comps.size()); ++i)
    {
        cout << "SCC " << i << ": ";

        for (int u : comps[i])
        {
            cout << u << ' ';
        }

        cout << '\n';
    }

    return 0;
}
```

复杂度：

```text
Time:  O(V + E)
Space: O(V + E)
```

---

# 7. Kosaraju vs Tarjan SCC

## Kosaraju

步骤：

```text
1. DFS 原图，记录 finish order
2. 图反向
3. 按逆 finish order DFS
4. 每次 DFS 得到一个 SCC
```

复杂度：

```text
O(V + E)
```

优点：

- 逻辑清楚；
- 很容易解释；
- 现场实现稳定。

缺点：

- 两遍 DFS；
- 需要 reverse graph。

## Tarjan

特点：

- 一遍 DFS；
- 使用 `dfn + low + stack + inStack`；
- 常用于进一步学习 low-link 系列。

面试中两种都可以，重点是写对。

---

# 8. Bridge：桥 / Cut Edge

## 8.1 定义

在无向图中，如果删除边：

```text
(u, v)
```

以后，connected components 数量增加，那么它是 Bridge。

例如：

```text
0 --- 1 --- 2
      |
      3
```

所有边都是 Bridge。

而：

```text
0 --- 1
|     |
3 --- 2
```

四条边都不是 Bridge，因为每条边都有替代路径。

一个很实用的直觉：

> **Bridge = 没有替代路径的边。**

---

# 9. Bridge 的 Low-Link 条件

假设 DFS tree 中：

```text
u
|
v
|
v subtree
```

`u -> v` 是 DFS tree edge。

如果：

```cpp
low[v] > dfn[u]
```

那么：

```text
(u, v) is a bridge
```

含义：

> `v` 的整个 DFS 子树无法通过 back edge 回到 `u` 或 `u` 的任何祖先。

因此 `u-v` 是唯一连接。

---

# 10. 为什么 Bridge 是 >，不是 >=

假设：

```text
low[v] == dfn[u]
```

说明 `v subtree` 中存在一条替代路径能够回到 `u`。

因此即使删除 tree edge：

```text
u -- v
```

`v subtree` 仍然能绕回 `u`。

所以：

```cpp
Bridge:
low[v] > dfn[u]
```

不是：

```cpp
low[v] >= dfn[u]
```

---

# 11. Articulation Point：割点 / Cut Vertex

## 11.1 定义

在无向图中，如果删除节点 `u` 以及所有 incident edges 后：

```text
connected components 数量增加
```

那么 `u` 是 AP。

例如：

```text
0 --- 1 --- 2
      |
      3
```

节点 `1` 是 AP。

---

# 12. AP 的判断条件

对于 **非 DFS root** 的节点 `u`：

```text
u
|
v
|
v subtree
```

如果存在一个 DFS tree child `v` 满足：

```cpp
low[v] >= dfn[u]
```

那么：

```text
u is an articulation point
```

---

# 13. 为什么 AP 是 >=

如果：

```text
low[v] == dfn[u]
```

说明：

```text
v subtree 可以回到 u
但不能绕过 u 回到 u 的祖先
```

对于 Bridge 没问题，因为边 `u-v` 被删后仍可通过别的边回到 `u`。

但对于 AP：

> `u` 节点本身也会被删除。

那么 `v subtree` 和 `u` 上方的图仍然断开。

所以：

```cpp
AP:
low[v] >= dfn[u]
```

而 Bridge：

```cpp
Bridge:
low[v] > dfn[u]
```

---

# 14. DFS Root 为什么特殊

对于普通节点 `u`：

```text
parent
  |
  u
  |
 child
```

我们可以讨论 child subtree 是否还能绕过 `u` 回到上方。

但 DFS root 没有 parent。

因此不能套普通 AP 条件。

DFS root 是 AP 的条件：

> **它有至少两个 DFS-tree children。**

即：

```cpp
if (parentEdge == -1 &&
    childCount >= 2)
{
    isAP[u] = true;
}
```

为什么？

如果 root 有两个 DFS-tree child：

```text
        root
       /    \
      a      b
```

如果 `a subtree` 和 `b subtree` 之间有路径，那么 DFS 从 `a` 出发时就会访问到 `b`，`b` 不会成为 root 的另一个 DFS child。

所以两个 DFS-tree children 表示它们只能通过 root 连接。

删除 root 后图被断开。

---

# 15. 一个图可能有多个 DFS Root 吗？

可以。

如果原图不是 connected graph：

```text
component 1     component 2

0 -- 1 -- 2     3 -- 4
```

需要：

```cpp
for (int u = 0; u < n; ++u)
{
    if (!dfn[u])
        dfs(u, -1);
}
```

这形成的是一个：

```text
DFS Forest
```

每个 connected component 都有自己的 DFS root。

AP 的 root 特殊规则，需要对每一棵 DFS tree 分别应用。

---

# 16. 为什么无向图推荐 parentEdge，而不是 parent vertex

无向边：

```text
u -- v
```

通常邻接表存成两个 adjacency entries：

```text
u -> v
v -> u
```

但物理上它们表示 **同一条无向边**。

推荐：

```cpp
struct Edge
{
    int to;
    int id;
};
```

添加：

```cpp
int id = edgeCount++;

graph[u].push_back({v, id});
graph[v].push_back({u, id});
```

两个 adjacency entries 使用同一个 `edgeId`。

这是一种：

> **edge-centric modeling：两个方向的邻接记录，共享同一个逻辑 edge identity。**

---

# 17. 为什么需要跳过 parentEdge

例如：

```text
u -- v
```

DFS：

```text
u -> v
```

进入 `v` 后，邻接表中会马上看到：

```text
v -> u
```

这只是刚才 tree edge 的反向 adjacency entry。

它 **不是 back edge**。

所以必须：

```cpp
if (edgeId == parentEdge)
    continue;
```

否则：

```cpp
low[v] = min(low[v], dfn[u]);
```

会错误地把父边本身当成替代路径。

结果就是：

- Bridge 判断可能错误；
- Low-Link 含义被破坏。

---

# 18. 只用 visited[] 不可以吗？

仅仅：

```cpp
if (visited[v])
```

不够。

原因是：

```text
v -> parent
```

这个 parent 本来就已经 visited。

但它可能只是 **当前 DFS tree edge 的反向邻接记录**，不能当成 back edge。

所以无向图 DFS 至少需要知道：

```text
“我到底是从哪条边进来的？”
```

最稳健的方法就是：

```cpp
parentEdge
```

---

# 19. 简单无向图中，parent vertex 是否够用？

如果题目明确保证：

- simple undirected graph；
- 任意两点之间最多一条边；
- 没有 parallel edges；

那么可以写：

```cpp
dfs(u, parent);

for (int v : graph[u])
{
    if (v == parent)
        continue;

    ...
}
```

这种写法通常也正确。

但更通用、更稳健的是：

```cpp
dfs(u, parentEdge);
```

因为它天然支持：

```text
u == v 之间存在多条平行边
```

---

# 20. parallel edges 为什么会让 parent vertex 写法出错

假设：

```text
u ===== v
```

这里 `u` 和 `v` 之间有两条不同的边。

DFS 通过 edge #1：

```text
u --(#1)--> v
```

在 `v` 中：

- edge #1 的反向记录应该跳过；
- edge #2 是真正的替代路径，应该作为 back edge 处理。

如果写：

```cpp
if (v == parent)
    continue;
```

会把两条边都跳掉。

于是错误判断 edge #1 是 Bridge。

而：

```cpp
if (edgeId == parentEdge)
    continue;
```

只跳过进入当前节点的那一条具体 edge。

---

# 21. 无向图的 Back Edge 必须指向祖先吗？

在标准递归 DFS 的 **无向图** 中：

> 对当前节点遇到的已访问邻居，如果不是 parent edge，那么它连接到当前节点的祖先。

也就是说，无向 DFS 不会出现有向 DFS 那种真正意义上的 cross edge。

直觉原因：

如果两个 DFS subtree 之间存在无向边，那么第一个 subtree DFS 时就会沿那条边访问另一个节点，它们不会最终形成两个独立的 DFS subtree。

因此无向图 low-link 可以很自然地解释成：

```text
subtree 能回到的最早 ancestor
```

---

# 22. 有向图为什么更复杂

有向图中，DFS 遇到 visited 节点时，可能是：

- ancestor；
- descendant；
- 已完成节点；
- cross edge 指向其他 DFS 分支。

所以 Tarjan SCC 不能简单写：

```cpp
else
    low[u] = min(low[u], dfn[v]);
```

而要判断：

```cpp
else if (inStack[v])
```

这正是 SCC 和无向 Bridge/AP low-link 的重要区别之一。

---

# 23. 无向图转成两个 directed adjacency entries，能直接用 SCC 吗？

通常不能这样理解。

如果无向边：

```text
u -- v
```

机械地转成：

```text
u -> v
v -> u
```

那么同一个 connected component 中所有节点往往都会互相可达。

于是 SCC 只会退化成：

```text
Connected Components
```

它不会直接告诉你：

- 哪些边是 Bridge；
- 哪些点是 AP。

因此：

- SCC 使用 directed SCC low-link；
- Bridge/AP 使用 undirected low-link；

虽然都叫 Tarjan / Low-Link，但解决的问题不同。

---

# 24. 有向图有 Bridge / AP 吗？

标准算法课程中：

- Bridge；
- Articulation Point；

通常指 **无向图 connectivity**。

有向图也存在类似概念，例如：

- strong bridge；
- strong articulation point；
- dominator-related algorithms；

但定义和算法明显更复杂。

所以普通算法面试中看到：

```text
Bridge / AP
```

优先理解为无向图。

---

# 25. Bridge + AP 可以一次 DFS 同时求

它们共用：

```cpp
dfn
low
timer
parentEdge
```

区别只有判断条件：

```cpp
// Bridge
low[v] > dfn[u]

// AP, non-root
low[v] >= dfn[u]

// AP, root
childCount >= 2
```

因此实际面试中，非常推荐记一个合并模板。

---

# 26. Bridge + AP 完整 C++ 模板

```cpp
#include <bits/stdc++.h>
using namespace std;

class UndirectedLowLink
{
public:
    struct Edge
    {
        int to;
        int id;
    };

    explicit UndirectedLowLink(int n)
        : n_(n),
          graph_(n),
          dfn_(n, 0),
          low_(n, 0),
          isAP_(n, false)
    {
    }

    void addEdge(int u, int v)
    {
        int id = edgeCount_++;

        // One physical undirected edge is stored as two adjacency entries.
        // Both entries share the same edge id.
        graph_[u].push_back({v, id});
        graph_[v].push_back({u, id});
    }

    void build()
    {
        // The graph can contain multiple connected components.
        for (int u = 0; u < n_; ++u)
        {
            if (dfn_[u] == 0)
            {
                dfs(u, -1);
            }
        }
    }

    const vector<pair<int, int>>& bridges() const
    {
        return bridges_;
    }

    vector<int> articulationPoints() const
    {
        vector<int> result;

        for (int u = 0; u < n_; ++u)
        {
            if (isAP_[u])
            {
                result.push_back(u);
            }
        }

        return result;
    }

private:
    void dfs(int u, int parentEdge)
    {
        dfn_[u] = low_[u] = ++timer_;

        // Number of DFS-tree children.
        // This is mainly needed for the special root rule.
        int childCount = 0;

        for (const auto& edge : graph_[u])
        {
            int v = edge.to;
            int edgeId = edge.id;

            // Only skip the exact physical edge used to enter u.
            //
            // Do NOT skip every edge whose endpoint equals the parent vertex,
            // because parallel edges may exist.
            if (edgeId == parentEdge)
                continue;

            if (dfn_[v] == 0)
            {
                // u -> v is a DFS tree edge.
                ++childCount;

                dfs(v, edgeId);

                // Child subtree may have a back edge to an ancestor.
                low_[u] = min(low_[u], low_[v]);

                // ------------------------------------------------
                // Bridge
                //
                // v-subtree cannot reach u or any ancestor of u
                // without using edge (u, v).
                // ------------------------------------------------
                if (low_[v] > dfn_[u])
                {
                    bridges_.push_back({u, v});
                }

                // ------------------------------------------------
                // Articulation Point: non-root
                //
                // v-subtree cannot reach any strict ancestor of u.
                // If u itself is removed, v-subtree becomes separated.
                // ------------------------------------------------
                if (parentEdge != -1 &&
                    low_[v] >= dfn_[u])
                {
                    isAP_[u] = true;
                }
            }
            else
            {
                // In an undirected DFS, this is a back edge to an ancestor
                // (after excluding the exact parent edge).
                low_[u] = min(low_[u], dfn_[v]);
            }
        }

        // --------------------------------------------------------
        // DFS root is special.
        //
        // Root is an AP iff it has at least two DFS-tree children.
        // --------------------------------------------------------
        if (parentEdge == -1 &&
            childCount >= 2)
        {
            isAP_[u] = true;
        }
    }

private:
    int n_;

    vector<vector<Edge>> graph_;

    vector<int> dfn_;
    vector<int> low_;

    vector<bool> isAP_;
    vector<pair<int, int>> bridges_;

    int timer_ = 0;
    int edgeCount_ = 0;
};


int main()
{
    UndirectedLowLink solver(7);

    // Triangle: 0-1-2-0
    solver.addEdge(0, 1);
    solver.addEdge(1, 2);
    solver.addEdge(2, 0);

    // Bridge
    solver.addEdge(1, 3);

    // Triangle: 3-4-5-3
    solver.addEdge(3, 4);
    solver.addEdge(4, 5);
    solver.addEdge(5, 3);

    // Bridge
    solver.addEdge(5, 6);

    solver.build();

    cout << "Bridges:\n";

    for (auto [u, v] : solver.bridges())
    {
        cout << u << " - " << v << '\n';
    }

    cout << "\nArticulation Points:\n";

    for (int u : solver.articulationPoints())
    {
        cout << u << '\n';
    }

    return 0;
}
```

复杂度：

```text
Time:  O(V + E)
Space: O(V + E)
```

---

# 27. Bridge 单独模板

如果题目只要求 Bridge，可以简化为：

```cpp
#include <bits/stdc++.h>
using namespace std;

class BridgeFinder
{
public:
    struct Edge
    {
        int to;
        int id;
    };

    explicit BridgeFinder(int n)
        : graph_(n),
          dfn_(n, 0),
          low_(n, 0)
    {
    }

    void addEdge(int u, int v)
    {
        int id = edgeCount_++;

        graph_[u].push_back({v, id});
        graph_[v].push_back({u, id});
    }

    vector<pair<int, int>> findBridges()
    {
        for (int u = 0; u < static_cast<int>(graph_.size()); ++u)
        {
            if (!dfn_[u])
            {
                dfs(u, -1);
            }
        }

        return bridges_;
    }

private:
    void dfs(int u, int parentEdge)
    {
        dfn_[u] = low_[u] = ++timer_;

        for (const auto& [v, edgeId] : graph_[u])
        {
            if (edgeId == parentEdge)
                continue;

            if (!dfn_[v])
            {
                dfs(v, edgeId);

                low_[u] = min(low_[u], low_[v]);

                if (low_[v] > dfn_[u])
                {
                    bridges_.push_back({u, v});
                }
            }
            else
            {
                low_[u] = min(low_[u], dfn_[v]);
            }
        }
    }

private:
    vector<vector<Edge>> graph_;

    vector<int> dfn_;
    vector<int> low_;

    vector<pair<int, int>> bridges_;

    int timer_ = 0;
    int edgeCount_ = 0;
};
```


## 27.1 LeetCode 1192：Bridge 简化版

下面是一个更紧凑的实现。它保留 `edgeId`，因此仍然能正确跳过“进入当前节点的那一条具体父边”，并支持 parallel edges。

```cpp
class Solution {
public:
    vector<vector<int>> criticalConnections(int n, vector<vector<int>>& connections) {
        graph_ = std::vector<std::vector<Info>>(n);
        disc_ = std::vector<int>(n);
        low_ = std::vector<int>(n);

        int edgeId = 0;
        for (const auto& c : connections)
        {
            graph_[c[0]].push_back(Info{.v = c[1], .edgeId=edgeId});
            graph_[c[1]].push_back(Info{.v = c[0], .edgeId=edgeId});
            ++edgeId;
        }

        for (int i = 0; i < n; ++i)
        {
            if (disc_[i] == 0)
            {
                dfs(i, -1);
            }
        }

        return result_;
    }

    void dfs(int u, int parentEdgeId)
    {
        disc_[u] = low_[u] = ++timer_;

        for (const auto& info : graph_[u])
        {
            if (info.edgeId == parentEdgeId)
                continue;

            if (disc_[info.v] == 0)
            {
                dfs(info.v, info.edgeId);
            }

            low_[u] = std::min(low_[u], low_[info.v]);

            if (low_[info.v] > disc_[u])
            {
                result_.push_back({u, info.v});
            }
        }
    }

    struct Info
    {
        int v;
        int edgeId;
    };

    std::vector<std::vector<Info>> graph_;
    std::vector<int> disc_;
    std::vector<int> low_;
    int timer_{0};
    vector<vector<int>> result_;
};
```

这个写法比标准模板更“合并”：

```cpp
if (disc_[v] == 0)
{
    dfs(v, edgeId);
}

low_[u] = min(low_[u], low_[v]);

if (low_[v] > disc_[u])
{
    // bridge
}
```

也就是说，它没有显式区分：

```text
tree edge:
    low[u] = min(low[u], low[v])

back edge:
    low[u] = min(low[u], disc[v])
```

对于 **无向图 Bridge 判断**，这个简化版可以成立。

原因：

- 如果 `v` 是 `u` 的祖先，那么 `low[v] <= disc[v] < disc[u]`，所以 `low[v] > disc[u]` 不可能成立，不会把 back edge 误判成 Bridge。
- 如果 `v` 是一个已经访问过的 descendant，那么无向边 `(u, v)` 本身就说明该 subtree 能回到 `u`，因此也不会满足 Bridge 条件。
- 所以把 Bridge 判断统一放在外面，在这个问题中不会产生错误结果。

不过，**标准模板仍然更推荐作为通用记忆版本**：

```cpp
if (disc_[v] == 0)
{
    dfs(v, edgeId);

    low_[u] = min(low_[u], low_[v]);

    if (low_[v] > disc_[u])
    {
        // bridge
    }
}
else
{
    low_[u] = min(low_[u], disc_[v]);
}
```

因为它直接体现 low-link 的语义：

```text
DFS child:
    inherit child's low

already visited ancestor:
    use ancestor's discovery time
```

因此建议：

- **LC 1192 / 单独求 Bridge**：简化版可以使用；
- **作为 Tarjan / Low-Link 通用模板**：优先记标准版；
- 不要把“visited neighbor 一律使用 `low[v]`”推广到 SCC 等其他 low-link 算法。


---

# 28. AP 单独模板

```cpp
#include <bits/stdc++.h>
using namespace std;

class ArticulationPointFinder
{
public:
    struct Edge
    {
        int to;
        int id;
    };

    explicit ArticulationPointFinder(int n)
        : graph_(n),
          dfn_(n, 0),
          low_(n, 0),
          isAP_(n, false)
    {
    }

    void addEdge(int u, int v)
    {
        int id = edgeCount_++;

        graph_[u].push_back({v, id});
        graph_[v].push_back({u, id});
    }

    vector<int> findArticulationPoints()
    {
        for (int u = 0; u < static_cast<int>(graph_.size()); ++u)
        {
            if (!dfn_[u])
            {
                dfs(u, -1);
            }
        }

        vector<int> result;

        for (int u = 0; u < static_cast<int>(graph_.size()); ++u)
        {
            if (isAP_[u])
            {
                result.push_back(u);
            }
        }

        return result;
    }

private:
    void dfs(int u, int parentEdge)
    {
        dfn_[u] = low_[u] = ++timer_;

        int childCount = 0;

        for (const auto& [v, edgeId] : graph_[u])
        {
            if (edgeId == parentEdge)
                continue;

            if (!dfn_[v])
            {
                ++childCount;

                dfs(v, edgeId);

                low_[u] = min(low_[u], low_[v]);

                // Non-root rule.
                if (parentEdge != -1 &&
                    low_[v] >= dfn_[u])
                {
                    isAP_[u] = true;
                }
            }
            else
            {
                low_[u] = min(low_[u], dfn_[v]);
            }
        }

        // Root rule.
        if (parentEdge == -1 &&
            childCount >= 2)
        {
            isAP_[u] = true;
        }
    }

private:
    vector<vector<Edge>> graph_;

    vector<int> dfn_;
    vector<int> low_;
    vector<bool> isAP_;

    int timer_ = 0;
    int edgeCount_ = 0;
};
```

---

# 29. 三个条件最终记忆版

这是最值得直接记住的三行：

```cpp
// SCC root
low[u] == dfn[u]

// Bridge: tree edge u-v
low[v] > dfn[u]

// AP: non-root u with tree child v
low[v] >= dfn[u]
```

以及：

```cpp
// AP root
childCount >= 2
```

---

# 30. 为什么 Bridge >，AP >=：统一解释

考虑：

```text
u
|
v
|
v subtree
```

## Bridge

删除的是：

```text
edge u-v
```

只要 `v subtree` 能回到 `u`：

```text
low[v] == dfn[u]
```

就仍然有替代路径。

所以必须：

```text
low[v] > dfn[u]
```

才是 Bridge。

## AP

删除的是：

```text
vertex u
```

即使：

```text
low[v] == dfn[u]
```

这个替代路径也只回到 `u`。

但 `u` 自己已经被删除。

所以仍然断开。

因此：

```text
low[v] >= dfn[u]
```

即可判断 AP。

---

# 31. childCount 为什么只统计 DFS-tree child

代码：

```cpp
if (!dfn[v])
{
    ++childCount;
    dfs(v, edgeId);
}
```

只有：

```text
unvisited v
```

才会成为 DFS tree child。

遇到 visited 邻居：

```cpp
else
{
    low[u] = min(low[u], dfn[v]);
}
```

只是 back edge，不增加 child count。

因此：

```cpp
childCount
```

天然就是：

```text
DFS-tree children count
```

---

# 32. Bridge / AP 的更高层结构

## 32.1 Edge-Biconnected Components

删除所有 Bridge 后，每个剩余 connected component 是一个：

```text
2-edge-connected component
```

在其中：

> 删除任意一条边仍然保持连通。

例如：

```text
triangle --- bridge --- square
```

删除 bridge 后：

```text
triangle

square
```

两个 component 可以继续缩点。

缩点后形成：

```text
Bridge Tree / Bridge Forest
```

---

## 32.2 Vertex-Biconnected Components

AP 对应更进一步的：

```text
Vertex-Biconnected Components
Block-Cut Tree
```

普通算法面试通常不要求直接手写 Block-Cut Tree，但理解这个方向有助于形成完整体系。

---

# 33. SCC / Bridge / AP 对比表

| 项目 | SCC | Bridge | AP |
|---|---|---|---|
| 图类型 | Directed | Undirected | Undirected |
| 问题 | 互相可达 | 关键边 | 关键点 |
| DFS 时间戳 | 是 | 是 | 是 |
| low-link | 是 | 是 | 是 |
| stack | 是 | 否 | 否 |
| inStack | 是 | 否 | 否 |
| 核心条件 | `low[u] == dfn[u]` | `low[v] > dfn[u]` | `low[v] >= dfn[u]` |
| root 特殊规则 | 否 | 否 | child >= 2 |
| 典型后续 | SCC DAG | Bridge Tree | Block-Cut Tree |

---

# 34. 常见错误

## 错误 1：SCC 对所有 visited 节点更新 low

错误：

```cpp
else
{
    low[u] = min(low[u], dfn[v]);
}
```

SCC 应该：

```cpp
else if (inStack[v])
{
    low[u] = min(low[u], dfn[v]);
}
```

---

## 错误 2：无向图把 parent reverse edge 当成 back edge

错误：

```cpp
if (dfn[v])
{
    low[u] = min(low[u], dfn[v]);
}
```

如果 `v` 就是刚才进入 `u` 的 parent，那么这个 reverse adjacency entry 不是 back edge。

需要先：

```cpp
if (edgeId == parentEdge)
    continue;
```

---

## 错误 3：Bridge 写成 >=

错误：

```cpp
if (low[v] >= dfn[u])
    // bridge
```

正确：

```cpp
if (low[v] > dfn[u])
    // bridge
```

---

## 错误 4：AP 写成 >

错误：

```cpp
if (low[v] > dfn[u])
    isAP[u] = true;
```

正确：

```cpp
if (low[v] >= dfn[u])
    isAP[u] = true;
```

---

## 错误 5：AP root 套普通规则

root 必须单独判断：

```cpp
if (parentEdge == -1 &&
    childCount >= 2)
{
    isAP[u] = true;
}
```

---

## 错误 6：只从节点 0 开始 DFS

如果图不是 connected：

```cpp
dfs(0);
```

会漏掉其他 component。

应该：

```cpp
for (int u = 0; u < n; ++u)
{
    if (!dfn[u])
        dfs(u, ...);
}
```

---

## 错误 7：默认 parent vertex 写法支持重边

```cpp
if (v == parent)
    continue;
```

只适合 simple graph。

通用模板优先：

```cpp
if (edgeId == parentEdge)
    continue;
```

---

# 35. 推荐题目

## SCC

### 基础 / 模板

- **CSES — Planets and Kingdoms**
  - 标准 SCC。
  - 很适合练 Tarjan / Kosaraju。

### SCC + DAG

- **CSES — Coin Collector**
  - SCC；
  - 缩点；
  - DAG；
  - DP。

这是理解 SCC 真正价值的经典题。

### SCC 思维

- **LeetCode 802 — Find Eventual Safe States**
  - cycle；
  - SCC；
  - reverse graph + topo 也可以。

### Functional Graph

- **LeetCode 2127 — Maximum Employees to Be Invited to a Meeting**
- **LeetCode 2360 — Longest Cycle in a Graph**

它们是特殊有向图：

```text
outdegree <= 1
```

结构可以理解为：

```text
cycles + incoming trees
```

---

# 36. Bridge 推荐题

## LeetCode 1192 — Critical Connections in a Network

标准 Bridge 裸题。

必须熟悉：

```cpp
dfn[u]
low[u]

low[v] > dfn[u]
```

优先级非常高。

---

# 37. AP 推荐题

LeetCode 直接的裸 AP 题不多。

推荐：

- GFG — Articulation Point
- UVA 315 — Network
- UVA 10199 — Tourist Guide

## LeetCode 1568 — Minimum Number of Days to Disconnect Island

非常值得做。

建模：

```text
Grid
 ↓
Graph
 ↓
land cell = vertex
 ↓
Articulation Point
```

整体逻辑：

```text
already disconnected -> 0

has articulation point -> 1

otherwise -> 2
```

这是很好的：

> Grid → Graph → AP

建模题。

---

# 38. 推荐学习顺序

```text
1. LC 1192
   Bridge

2. 裸 Articulation Point
   理解 >= 和 root rule

3. LC 1568
   AP 建模

4. CSES Planets and Kingdoms
   SCC

5. CSES Coin Collector
   SCC + shrink + DAG DP

6. LC 802
   SCC / cycle / safe node

7. LC 2127
   Functional Graph

8. 2-SAT
   SCC 高阶应用
```

---

# 39. 面试现场快速判断

看到：

```text
Directed graph
+ cycles
+ mutually reachable
```

想：

```text
SCC
```

看到：

```text
复杂有向图
但一个环内部可以当成整体
```

想：

```text
SCC
→ shrink
→ DAG
```

看到：

```text
Undirected graph
+ critical edge
+ delete one edge disconnects graph
```

想：

```text
Bridge

low[v] > dfn[u]
```

看到：

```text
Undirected graph
+ critical vertex
+ delete one vertex disconnects graph
```

想：

```text
Articulation Point

non-root:
low[v] >= dfn[u]

root:
childCount >= 2
```

---

# 40. 最终面试速记版

```cpp
// ============================================================
// SCC
// ============================================================

dfn[u] = low[u] = ++timer;

stack.push_back(u);
inStack[u] = true;

for (int v : graph[u])
{
    if (!dfn[v])
    {
        dfs(v);
        low[u] = min(low[u], low[v]);
    }
    else if (inStack[v])
    {
        low[u] = min(low[u], dfn[v]);
    }
}

if (low[u] == dfn[u])
{
    // pop one SCC
}
```

```cpp
// ============================================================
// Bridge + AP
// ============================================================

dfn[u] = low[u] = ++timer;

for (auto [v, edgeId] : graph[u])
{
    if (edgeId == parentEdge)
        continue;

    if (!dfn[v])
    {
        ++childCount;

        dfs(v, edgeId);

        low[u] = min(low[u], low[v]);

        // Bridge
        if (low[v] > dfn[u])
        {
        }

        // AP: non-root
        if (parentEdge != -1 &&
            low[v] >= dfn[u])
        {
        }
    }
    else
    {
        low[u] = min(low[u], dfn[v]);
    }
}

// AP: root
if (parentEdge == -1 &&
    childCount >= 2)
{
}
```

最终最值得背下来的四个条件：

```text
SCC:
low[u] == dfn[u]

Bridge:
low[v] > dfn[u]

AP non-root:
low[v] >= dfn[u]

AP root:
DFS-tree childCount >= 2
```

以及一个无向图实现原则：

> **用 edgeId 跳过“进入当前节点的那条具体父边”，而不是笼统跳过 parent vertex。**

这样模板既能处理普通 simple graph，也天然支持 parallel edges。
