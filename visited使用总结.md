# visited 使用总结：BFS、DFS、最短路径、MST、迷宫、排列组合与状态搜索

## 1. 核心原则

不要先机械地问：

> visited 应该在 push 前标记，还是在 pop 后标记？

先问两个问题：

1. **visited 在当前算法中到底表示什么？**
2. **第一次发现一个状态后，它是否已经可以被最终确定（finalize）？**

很多算法真正的区别，不是“有没有 visited”，而是 visited 的语义不同：

- **discovered**：这个状态已经被发现 / 入队
- **finalized / settled**：这个状态的最优结果已经最终确定
- **on path**：这个状态当前正在递归路径中
- **in MST**：这个点已经正式加入 MST
- **full-state visited**：完整状态已经访问，例如 `(node, mask)`
- 有些算法根本不需要 visited，而由 `dist`、`parent`、`indegree`、DSU 等状态代替

---

## 2. 最重要的判断：push 时占坑，还是 pop 时占坑？

可以用下面这个判断：

```text
push(state)
    |
    v
这个 state 以后还可能出现“更优版本”吗？
    |
  +---+--------------------+
  |                        |
  No                       Yes
  |                        |
push 时即可占坑          不能 push 时永久占坑
  |                        |
BFS / 普通图遍历        Dijkstra / Prim / 某些 weighted search
                           |
                           v
                    通常 pop 最优候选时 finalize
```

这里“更优版本”是核心。

注意：

> priority queue 中已经 push 的旧 entry 本身不会变化。

例如 Dijkstra：

```text
push (10, A)

后来找到更短路径：

push (2, A)
```

此时 heap 中可能同时存在：

```text
(2, A)
(10, A)   <- stale entry
```

因此，真正的问题不是“queue 里的元素会不会被修改”，而是：

> **同一个逻辑状态是否可能再次以更好的值进入容器。**

---

# 3. 总表

| 场景 | visited 的含义 | 推荐标记位置 | 是否回退 |
|---|---|---|---|
| 普通 BFS | 已发现 / 已入队 | **push 前立即标记** | 否 |
| Multi-source BFS | 已发现 / 已入队 | source 初始化 + push 前 | 否 |
| 无权最短路 BFS | 第一次发现即最短 | **push 前** | 否 |
| Recursive DFS 图遍历 | 已经被 traversal 发现 | 进入 `dfs(u)` 后立即标记 | 否 |
| Iterative DFS | 已发现 | 通常 **push 前** | 否 |
| Dijkstra | 最短距离已 finalized | 通常不用 visited；用 stale check | 否 |
| Dijkstra + visited | settled | **pop 后** | 否 |
| 0-1 BFS | 当前最优距离 | 通常不用 visited，用 dist relax | 否 |
| Bellman-Ford | 无 | 不用 visited | - |
| Floyd-Warshall | 无 | 不用 visited | - |
| Lazy Prim | 已加入 MST | **pop 后** | 否 |
| Eager Prim | 已加入 MST | **pop 后** | 否 |
| Kruskal | 无节点 visited | DSU 判断 component | - |
| 迷宫 BFS | cell/state 已发现 | **push 前** | 否 |
| 迷宫 DFS 可达性 | 已经完整搜索过 | 进入 DFS 后 | 否 |
| 枚举所有简单路径 | 当前 path 中 | 进入标记，退出取消 | **是** |
| Permutation | 当前排列已使用 | choose 后标记，回溯取消 | **是** |
| Combination | 通常不用 visited | 用 `start` | - |
| Subset | 通常不用 visited | 用 `start/index` | - |
| DFS 拓扑排序 | 0/1/2 三状态 | 进入 visiting，退出 visited | 否 |
| Kahn 拓扑排序 | 通常不用 visited | 用 indegree | - |
| Tree DFS/BFS | 通常不用 visited | 用 parent | - |
| 状态压缩 BFS | 完整状态已访问 | **push state 前** | 否 |

---

# 4. BFS：push 前占坑

标准模板：

```cpp
queue<int> q;

q.push(src);
visited[src] = true;

while (!q.empty())
{
    int u = q.front();
    q.pop();

    for (int v : graph[u])
    {
        if (visited[v])
            continue;

        visited[v] = true;   // 关键：push 前占坑
        q.push(v);
    }
}
```

## 为什么不是 pop 后？

例如：

```text
    A
   / \
  B   C
   \ /
    D
```

如果 D 只在 pop 时才 visited：

```text
处理 B:
    push D

处理 C:
    D 还没 pop
    D 仍然没有 visited
    再次 push D
```

queue 中会出现：

```text
D, D
```

因此 BFS 最推荐：

> **一旦决定 enqueue，就立即 claim 这个状态。**

这里 visited 的语义是：

```text
visited = discovered
```

---

# 5. BFS 为什么第一次发现就可以封死？

无权图 BFS 是按层扩展：

```text
distance = 0
distance = 1
distance = 2
distance = 3
...
```

因此第一次发现 `v` 时：

```cpp
dist[v] = dist[u] + 1;
```

这个距离已经是最短距离。

于是：

```text
first discovered
=
shortest distance finalized
```

所以 BFS 可以 push 时 visited。

甚至可以不用单独的 visited：

```cpp
vector<int> dist(n, -1);

dist[src] = 0;
q.push(src);

while (!q.empty())
{
    int u = q.front();
    q.pop();

    for (int v : graph[u])
    {
        if (dist[v] != -1)
            continue;

        dist[v] = dist[u] + 1;
        q.push(v);
    }
}
```

此时：

```text
dist[v] != -1
```

本身就代表 visited。

---

# 6. Multi-source BFS

完全继承普通 BFS 的规则。

初始化时：

```cpp
for (int s : sources)
{
    visited[s] = true;
    q.push(s);
}
```

扩展时：

```cpp
if (!visited[v])
{
    visited[v] = true;
    q.push(v);
}
```

可以把 multi-source BFS 理解为：

> 增加一个 dummy source，距离 0 地连向所有真实 source。

---

# 7. Recursive DFS：进入函数就标记

```cpp
void dfs(int u)
{
    visited[u] = true;

    for (int v : graph[u])
    {
        if (!visited[v])
            dfs(v);
    }
}
```

原因是图可能有环：

```text
A -> B -> C
     ^    |
     |____|
```

如果进入 B 不立刻标记，就可能无限递归。

这里：

```text
visited = 已经被整个 traversal 发现
```

所以不会回退。

---

# 8. Iterative DFS：通常也是 push 时标记

推荐：

```cpp
stack<int> st;

st.push(src);
visited[src] = true;

while (!st.empty())
{
    int u = st.top();
    st.pop();

    for (int v : graph[u])
    {
        if (visited[v])
            continue;

        visited[v] = true;
        st.push(v);
    }
}
```

也可以写成 pop 后判断：

```cpp
st.push(src);

while (!st.empty())
{
    int u = st.top();
    st.pop();

    if (visited[u])
        continue;

    visited[u] = true;

    for (int v : graph[u])
        if (!visited[v])
            st.push(v);
}
```

通常结果仍然正确，但可能产生重复 stack entry。

所以如果可以安全地在 push 时标记，通常更高效。

---

# 9. Dijkstra：第一次发现不等于最终最短

这是 BFS 和 Dijkstra 最核心的区别。

例如：

```text
S --10--> A
|
1
v
B --1--> A
```

第一次：

```text
S -> A = 10
```

但之后：

```text
S -> B -> A = 2
```

因此：

```text
first discovered != finalized
```

绝对不能把 BFS 写法直接套进 Dijkstra：

```cpp
// 错误思想
if (!visited[v])
{
    visited[v] = true;  // 太早
    ...
}
```

---

# 10. Dijkstra 推荐写法：不用 visited

现代 heap Dijkstra 最推荐：

```cpp
using P = pair<long long, int>;

priority_queue<P, vector<P>, greater<P>> pq;

vector<long long> dist(n, INF);
dist[src] = 0;

pq.push({0, src});

while (!pq.empty())
{
    auto [d, u] = pq.top();
    pq.pop();

    if (d != dist[u])
        continue;       // stale entry

    for (auto [v, w] : graph[u])
    {
        long long nd = d + w;

        if (nd < dist[v])
        {
            dist[v] = nd;
            pq.push({nd, v});
        }
    }
}
```

为什么可以不用 visited？

因为：

```cpp
if (d != dist[u])
    continue;
```

已经识别旧 entry。

例如：

```text
heap:
(2, A)
(10, A)
```

pop `(2, A)`：

```text
2 == dist[A]
=> 有效
```

以后 pop `(10, A)`：

```text
10 != dist[A]
=> stale，丢弃
```

因此 `dist[]` 同时承担：

- best known distance
- stale entry detection

---

# 11. Dijkstra 如果使用 visited：必须 pop 后

另一种正确写法：

```cpp
while (!pq.empty())
{
    auto [d, u] = pq.top();
    pq.pop();

    if (visited[u])
        continue;

    visited[u] = true;   // 此时才 finalized

    for (auto [v, w] : graph[u])
    {
        if (dist[v] > d + w)
        {
            dist[v] = d + w;
            pq.push({dist[v], v});
        }
    }
}
```

这里 visited 的含义不是 discovered，而是：

```text
visited[u]
=
shortest path to u has been settled/finalized
```

因此：

```text
push v 时  ❌
pop u 后   ✅
```

---

# 12. 为什么 Dijkstra pop 后就能 finalize？

Dijkstra 依赖非负边。

当 `u` 以当前最小距离从 min-heap 中弹出时，未来不可能通过其他尚未处理的节点再得到更短路径。

所以：

```text
tentative distance
      |
      | pop minimum valid entry
      v
final distance
```

这也是为什么目标点可以 early exit：

```cpp
auto [d, u] = pq.top();
pq.pop();

if (d != dist[u])
    continue;

if (u == dst)
    return d;
```

---

# 13. 0-1 BFS：通常不用 visited

```cpp
deque<int> dq;

dist[src] = 0;
dq.push_front(src);

while (!dq.empty())
{
    int u = dq.front();
    dq.pop_front();

    for (auto [v, w] : graph[u])
    {
        if (dist[v] > dist[u] + w)
        {
            dist[v] = dist[u] + w;

            if (w == 0)
                dq.push_front(v);
            else
                dq.push_back(v);
        }
    }
}
```

一个节点可能被更好的路径重新 relax，因此不能简单套：

```cpp
if (visited[v])
    continue;
```

核心是：

```text
relaxation
```

而不是：

```text
first seen wins
```

---

# 14. Bellman-Ford：不用 visited

Bellman-Ford 的本质是：

```text
不断允许 dist 被后续 round 改善
```

因此：

```text
某一时刻访问过
```

完全不代表：

```text
它已经最终确定
```

所以不应该有普通 visited。

---

# 15. Floyd-Warshall：不用 visited

它是 DP：

```cpp
for (int k = 0; k < n; ++k)
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            dist[i][j] =
                min(dist[i][j],
                    dist[i][k] + dist[k][j]);
```

状态是：

```text
dist[i][j]
```

没有 discovered / finalized 的 traversal 概念。

---

# 16. Prim：非常像 Dijkstra

Lazy Prim：

```cpp
using P = pair<int, int>; // {edgeWeight, vertex}

priority_queue<P, vector<P>, greater<P>> pq;

pq.push({0, 0});

while (!pq.empty())
{
    auto [w, u] = pq.top();
    pq.pop();

    if (inMST[u])
        continue;

    inMST[u] = true;
    mstCost += w;

    for (auto [v, weight] : graph[u])
    {
        if (!inMST[v])
            pq.push({weight, v});
    }
}
```

这里不能 push 时：

```cpp
inMST[v] = true;   // 错
```

例如：

```text
S --10-- A
|
1
|
B --1-- A
```

第一次看见 A：

```text
S-A = 10
```

但后来：

```text
B-A = 1
```

才是更好的连接边。

所以 Prim：

```text
first discovered != joins MST
```

必须：

```text
pop minimum candidate
        |
        v
正式加入 MST
```

---

# 17. Eager Prim / minDist Prim

```cpp
vector<int> minDist(n, INF);
vector<bool> inMST(n, false);

minDist[0] = 0;
pq.push({0, 0});

while (!pq.empty())
{
    auto [w, u] = pq.top();
    pq.pop();

    if (inMST[u])
        continue;

    inMST[u] = true;

    for (auto [v, edgeWeight] : graph[u])
    {
        if (!inMST[v] && edgeWeight < minDist[v])
        {
            minDist[v] = edgeWeight;
            pq.push({edgeWeight, v});
        }
    }
}
```

建议变量名直接叫：

```cpp
inMST
```

比 `visited` 更清楚。

---

# 18. Dijkstra 与 Prim 的结构类比

两者都属于：

```text
priority queue
      |
      v
允许同一个 vertex 出现多个 candidate
      |
      v
pop 当前最优 candidate
      |
      v
finalize
```

区别在于优化目标：

```text
Dijkstra:
    candidate = 从 source 到 vertex 的总距离
    finalize = 最短路径确定

Prim:
    candidate = vertex 接入当前 MST 的最小代价
    finalize = vertex 正式进入 MST
```

---

# 19. Kruskal：不用 visited，用 DSU

Kruskal 判断的不是：

```text
这个节点以前见过吗？
```

而是：

```text
u 和 v 是否已经属于同一个 connected component？
```

所以：

```cpp
if (dsu.find(u) != dsu.find(v))
{
    dsu.unite(u, v);
    mstCost += w;
}
```

因此：

```text
Prim     -> inMST[]
Kruskal  -> DSU
```

---

# 20. 迷宫 BFS：push 前 visited

迷宫最短路本质是普通 BFS：

```cpp
queue<pair<int,int>> q;

q.push({sr, sc});
visited[sr][sc] = true;

while (!q.empty())
{
    auto [r, c] = q.front();
    q.pop();

    for (auto [dr, dc] : dirs)
    {
        int nr = r + dr;
        int nc = c + dc;

        if (outOfBound(nr, nc))
            continue;

        if (grid[nr][nc] == '#')
            continue;

        if (visited[nr][nc])
            continue;

        visited[nr][nc] = true;
        q.push({nr, nc});
    }
}
```

这里：

```text
cell = graph vertex
```

所以完全继承 BFS 规则。

---

# 21. 迷宫 DFS：找“是否可达”

如果只是找是否存在一条路径：

```cpp
bool dfs(int r, int c)
{
    if (isTarget(r, c))
        return true;

    visited[r][c] = true;

    for (...)
    {
        if (!visited[nr][nc] && dfs(nr, nc))
            return true;
    }

    return false;
}
```

这里 visited 通常不回退。

原因：

> 某个位置如果已经完整 DFS 搜过，之后没必要重复搜索。

---

# 22. 枚举所有简单路径：visited 必须回退

如果要求所有 simple paths：

```cpp
void dfs(int u)
{
    if (u == dst)
    {
        ans.push_back(path);
        return;
    }

    visited[u] = true;

    for (int v : graph[u])
    {
        if (!visited[v])
        {
            path.push_back(v);
            dfs(v);
            path.pop_back();
        }
    }

    visited[u] = false;
}
```

此时：

```text
visited[u]
=
u 是否在“当前路径”中
```

不是：

```text
u 是否曾经访问过
```

例如：

```text
A -> B -> D
 \       ^
  -> C ---|
```

搜索完：

```text
A -> B -> D
```

后必须允许：

```text
A -> C -> D
```

所以 D 不能永久 visited。

---

# 23. Permutation：used 是当前 path 状态

```cpp
void dfs()
{
    if (path.size() == nums.size())
    {
        ans.push_back(path);
        return;
    }

    for (int i = 0; i < nums.size(); ++i)
    {
        if (used[i])
            continue;

        used[i] = true;
        path.push_back(nums[i]);

        dfs();

        path.pop_back();
        used[i] = false;
    }
}
```

经典模式：

```text
choose
    used[i] = true

explore
    dfs()

unchoose
    used[i] = false
```

---

# 24. Combination：通常不需要 visited

组合可以用 `start` 限制只能向后选：

```cpp
void dfs(int start)
{
    if (path.size() == k)
    {
        ans.push_back(path);
        return;
    }

    for (int i = start; i < nums.size(); ++i)
    {
        path.push_back(nums[i]);
        dfs(i + 1);
        path.pop_back();
    }
}
```

因此：

```text
Permutation:
    下一层仍然可以从整个数组选择
    -> 需要 used[]

Combination:
    下一层只从当前位置之后选择
    -> start 已经足够
```

---

# 25. Subset：通常也不需要 visited

```cpp
void dfs(int start)
{
    ans.push_back(path);

    for (int i = start; i < nums.size(); ++i)
    {
        path.push_back(nums[i]);
        dfs(i + 1);
        path.pop_back();
    }
}
```

依然由 `start` 保证不会重复使用前面的元素。

---

# 26. 有重复元素的 permutation

例如：

```text
[1, 1, 2]
```

标准写法：

```cpp
sort(nums.begin(), nums.end());

for (int i = 0; i < n; ++i)
{
    if (used[i])
        continue;

    if (i > 0 &&
        nums[i] == nums[i - 1] &&
        !used[i - 1])
        continue;

    used[i] = true;
    path.push_back(nums[i]);

    dfs();

    path.pop_back();
    used[i] = false;
}
```

这里两个判断含义不同：

```text
used[i]
    -> 当前 path 是否已经用了这个具体 index

nums[i] == nums[i-1] && !used[i-1]
    -> 同一层去重
```

不要混淆。

---

# 27. DFS 拓扑排序：bool visited 不够

检测有向图 cycle 时需要三状态：

```text
0 = unvisited
1 = visiting
2 = visited / done
```

模板：

```cpp
bool dfs(int u)
{
    state[u] = 1;   // 当前 recursion stack

    for (int v : graph[u])
    {
        if (state[v] == 1)
            return false;   // back edge

        if (state[v] == 0 && !dfs(v))
            return false;
    }

    state[u] = 2;
    return true;
}
```

关键区别：

```text
visiting
=
当前递归路径中

visited
=
这个节点已经完整处理完成
```

---

# 28. Kahn 拓扑排序：通常不用 visited

```cpp
for (int i = 0; i < n; ++i)
{
    if (indegree[i] == 0)
        q.push(i);
}

while (!q.empty())
{
    int u = q.front();
    q.pop();

    for (int v : graph[u])
    {
        if (--indegree[v] == 0)
            q.push(v);
    }
}
```

为什么不用 visited？

因为 `indegree[v]` 只会恰好一次从正数变成 0。

所以 indegree 本身已经表达状态。

---

# 29. Tree DFS/BFS：通常不需要 visited

对于树：

```cpp
void dfs(int u, int parent)
{
    for (int v : graph[u])
    {
        if (v == parent)
            continue;

        dfs(v, u);
    }
}
```

树没有 cycle，只需要防止：

```text
u -> parent -> u
```

因此 parent 比 visited 更直接。

---

# 30. 状态压缩 BFS：visited 的不是 node，而是完整 state

这是 LC 847、LC 864 等题最重要的点。

例如状态：

```text
(node, mask)
```

不能只写：

```cpp
visited[node]
```

而要：

```cpp
visited[node][mask]
```

例如 LC 847：

```cpp
for (int i = 0; i < n; ++i)
{
    int mask = 1 << i;

    visited[i][mask] = true;
    q.push({i, mask});
}

while (!q.empty())
{
    auto [u, mask] = q.front();
    q.pop();

    for (int v : graph[u])
    {
        int nextMask = mask | (1 << v);

        if (visited[v][nextMask])
            continue;

        visited[v][nextMask] = true;
        q.push({v, nextMask});
    }
}
```

依然遵守 BFS 原则：

> **完整 state 在 push 前标记。**

---

# 31. LC 864 Keys and Locks

状态不是单纯：

```text
(r, c)
```

而是：

```text
(r, c, keysMask)
```

同一个位置：

```text
(r, c, 001)
```

和：

```text
(r, c, 101)
```

能力不同，因此是两个不同状态。

所以：

```cpp
visited[r][c][mask]
```

核心原则：

> visited 必须对应 **完整状态空间中的节点**，而不是只对应物理位置。

---

# 32. LC 1293 Obstacle Elimination

完整状态可以写成：

```text
(r, c, remainingK)
```

因此可以：

```cpp
visited[r][c][remainingK]
```

但还可以使用 dominance optimization：

```cpp
best[r][c] = 到达这个位置时见过的最大 remainingK
```

如果：

```text
remainingK <= best[r][c]
```

当前状态被之前状态支配，可以直接丢弃。

这说明：

> visited 并不一定非得是 bool；有时可以被“更强的 best state”替代。

---

# 33. Bidirectional BFS

两边分别维护：

```text
visitedA
visitedB
```

仍然都是：

```text
push 时 visited
```

例如：

```cpp
if (!visitedA[v])
{
    visitedA[v] = true;
    qA.push(v);
}
```

如果发现：

```cpp
visitedB[v]
```

说明两边相遇。

---

# 34. A*：更接近 Dijkstra，而不是普通 BFS

A*：

```text
f(n) = g(n) + h(n)
```

通常维护：

```text
gScore[]
```

因为某节点可能以后发现更好的 `g`。

所以本质也是：

```text
best-known-cost + relaxation
```

而不是：

```text
first seen wins
```

什么时候可以永久 closed，还与 heuristic 是否 consistent 有关。

---

# 35. 用户常见的一个理解：pop 后占坑是否更通用？

可以这么理解：

> **pop 后再 visited，确实比 push 时 visited 更“宽松”，很多普通 BFS/DFS case 也仍然能得到正确答案。**

例如 BFS 可以写：

```cpp
q.push(src);

while (!q.empty())
{
    int u = q.front();
    q.pop();

    if (visited[u])
        continue;

    visited[u] = true;

    for (int v : graph[u])
        if (!visited[v])
            q.push(v);
}
```

通常仍然正确。

但是它有明显缺点：

```text
同一个状态可能被重复 push 很多次
```

例如多个父节点同时指向 X：

```text
B -> X
C -> X
D -> X
```

可能产生：

```text
queue = X, X, X
```

所以：

> **能安全地 push 时占坑，就应该尽早占坑，避免重复入队。**

因此更准确的规则是：

```text
push 时能确定“不需要再被更优版本替代”
    -> push 时标记

push 后仍可能出现更优 candidate
    -> 不能 push 时永久标记
    -> 通常等 pop 最优 candidate 时 finalize
```

---

# 36. discovered 与 finalized

这是理解全部 visited 问题的核心。

## BFS

```text
discovered
=
shortest distance finalized
```

所以：

```text
push 时即可 visited
```

## Dijkstra

```text
discovered
!=
finalized
```

第一次发现只是：

```text
tentative
```

第一次以最小有效距离 pop：

```text
finalized
```

## Prim

同理：

```text
第一次发现 vertex
!=
它已经确定加入 MST
```

只有最优 connecting edge 被 pop 时才加入 MST。

---

# 37. 一个统一判断框架

遇到新题时，依次问下面的问题。

## 问题 1：第一次发现状态后，是否已经不可能被改善？

如果 YES：

```text
普通 BFS
Multi-source BFS
无权迷宫最短路
```

推荐：

```cpp
if (!visited[v])
{
    visited[v] = true;
    q.push(v);
}
```

---

## 问题 2：第一次发现以后还可能出现更好的 candidate 吗？

如果 YES：

```text
Dijkstra
Prim
A*
0-1 BFS
某些 weighted state search
```

不能第一次 push 时永久 visited。

进一步：

```text
Dijkstra / Prim
    -> 通常 pop 最优候选时 finalize

0-1 BFS / Bellman-Ford
    -> 主要依赖 relaxation / dist
```

---

## 问题 3：visited 是否表示“当前递归路径正在使用”？

如果 YES：

```text
Permutation
所有简单路径
Word Search
棋盘 backtracking
```

必须：

```cpp
used[i] = true;
dfs(...);
used[i] = false;
```

---

## 问题 4：已有其他状态能否代替 visited？

常见：

```text
BFS          -> dist[v] != -1
Tree         -> parent
Kahn         -> indegree
Dijkstra     -> dist[]
Kruskal      -> DSU
Combination  -> start
State BFS    -> visited[full_state]
```

如果已有更明确的状态，就不要机械地额外增加 visited。

---

# 38. BFS / Dijkstra / Prim 最重要的对照

| | BFS | Dijkstra | Prim |
|---|---|---|---|
| 数据结构 | FIFO Queue | Min Heap | Min Heap |
| 第一次发现是否最优 | **是** | 否 | 否 |
| push 时 visited | **推荐** | **错误** | **错误** |
| pop 时 finalize | 不需要 | 是 | 是 |
| 核心状态 | visited / dist | dist | inMST / minDist |
| 同状态多次进入容器 | 应避免 | 正常 | 正常 |
| stale candidate | 基本没有 | 很常见 | 很常见 |
| finalize 含义 | shortest level 已确定 | shortest distance 已确定 | vertex 已加入 MST |

最值得记住：

```text
BFS:
    first discovered = optimal

Dijkstra:
    first valid minimum pop = optimal

Prim:
    first valid minimum connecting candidate pop = joins MST
```

---

# 39. 面试推荐模板

## BFS

```cpp
if (visited[v])
    continue;

visited[v] = true;
q.push(v);
```

---

## Dijkstra

```cpp
auto [d, u] = pq.top();
pq.pop();

if (d != dist[u])
    continue;
```

---

## Dijkstra + settled

```cpp
auto [d, u] = pq.top();
pq.pop();

if (visited[u])
    continue;

visited[u] = true;
```

---

## Prim

```cpp
auto [w, u] = pq.top();
pq.pop();

if (inMST[u])
    continue;

inMST[u] = true;
```

---

## Recursive DFS

```cpp
void dfs(int u)
{
    visited[u] = true;

    for (int v : graph[u])
        if (!visited[v])
            dfs(v);
}
```

---

## Backtracking

```cpp
used[i] = true;
path.push_back(nums[i]);

dfs();

path.pop_back();
used[i] = false;
```

---

## State-space BFS

```cpp
State next = ...;

if (visited[next])
    continue;

visited[next] = true;
q.push(next);
```

---

# 40. 常见错误

## 错误 1：把 BFS 的 visited 规则直接套进 Dijkstra

错误：

```cpp
if (!visited[v])
{
    visited[v] = true;
    pq.push(...);
}
```

问题：

> 第一次发现 v 时，其最短距离可能还没确定。

---

## 错误 2：Prim 在发现 vertex 时就 inMST

错误：

```cpp
inMST[v] = true;
pq.push({w, v});
```

问题：

> 后续可能出现更小的 connecting edge。

---

## 错误 3：BFS 到 pop 时才 visited

很多时候结果仍正确，但可能重复入队。

如果第一次发现已经足够确定，应尽量：

```cpp
visited[v] = true;
q.push(v);
```

---

## 错误 4：所有 DFS 都永久 visited

Backtracking 中：

```text
visited = 当前 path 占用
```

必须 undo。

---

## 错误 5：状态搜索只 visited node

例如：

```text
(node, mask)
(r, c, keys)
(r, c, remainingK)
```

完整状态不同，即使 node/cell 相同，也不能简单去重。

---

# 41. 最终口诀

```text
BFS：
    入队即占坑。

普通 DFS traversal：
    进入即占坑。

Dijkstra：
    第一次发现不算数；
    最优有效 entry 出堆才定案；
    通常 dist + stale check 就够。

Prim：
    出堆才正式入树。

Kruskal：
    不看 visited，看 DSU。

Backtracking：
    进入占坑，退出还坑。

Combination / Subset：
    通常 start/index 代替 visited。

状态搜索：
    visited 的是完整 state，不一定只是 node。
```

---

# 42. 最精炼的统一原则

> **如果一个状态第一次进入容器后，就已经不可能再有“更好的版本”，可以在 push 时标记。**

> **如果之后仍可能出现更优 candidate，就不能在第一次 push 时永久标记，通常要等最优 candidate pop 时才 finalize，或者干脆用 dist / relaxation 来管理。**

> **如果 visited 表示当前递归路径，则必须在回溯时取消。**

最后再加一个性能原则：

> **能安全地 push 时占坑，就不要拖到 pop，因为越早去重，越能避免重复入队。**
