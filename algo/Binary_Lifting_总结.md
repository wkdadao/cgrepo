# Binary Lifting（倍增）系统总结

> 核心思想：**预处理 1、2、4、8、16... 步的跳转结果，然后把任意步数按二进制拆分。**
>
> Binary Lifting 最经典用于树上的祖先 / LCA，但本质并不依赖“树”，只要每个状态都有一个确定的 next，就可以做倍增。

---

## 1. 核心思想

假设每个节点都有一个确定的下一跳：

```cpp
next[u]
```

最直接的做法是一步一步走：

```text
u -> next[u] -> next[next[u]] -> ...
```

如果要走 `k` 步，直接模拟需要 `O(k)`。

Binary Lifting 的想法是预处理：

```text
走 1 步之后在哪里
走 2 步之后在哪里
走 4 步之后在哪里
走 8 步之后在哪里
走 16 步之后在哪里
...
```

定义：

```cpp
up[u][j]
```

表示：

> 从节点 `u` 出发，连续执行 next `2^j` 次以后到达的节点。

因此：

```cpp
up[u][0] = next[u];
```

而：

```text
2^j = 2^(j-1) + 2^(j-1)
```

所以：

```cpp
up[u][j] = up[ up[u][j - 1] ][j - 1];
```

这就是 Binary Lifting 最核心的递推式。

---

# 2. 为什么叫 Binary Lifting

任意整数都可以拆成若干个 2 的幂。

例如：

```text
13 = 8 + 4 + 1
   = 1101₂
```

所以从 `u` 出发走 13 步，可以拆成：

```text
jump 8
jump 4
jump 1
```

代码：

```cpp
for (int j = 0; j < LOG; ++j)
{
    if (k & (1LL << j))
        u = up[u][j];
}
```

复杂度：

```text
预处理：O(N log K)
查询：  O(log K)
空间：  O(N log K)
```

树问题中通常 `K <= N`，于是也常写成：

```text
O(N log N)
```

---

# 3. Binary Lifting 本质上是 Doubling（倍增）

Binary Lifting 属于更大的思想：

> **Doubling / 倍增**

统一模式：

```text
长度 / 步数：

1
2
4
8
16
32
...
```

然后利用：

```text
2^j = 2^(j-1) + 2^(j-1)
```

把两个半段拼起来。

所以 Binary Lifting 不是一个只属于树的技巧。

更抽象地说：

```text
给定函数 f(x)

预处理：

f^1(x)
f^2(x)
f^4(x)
f^8(x)
...
```

如果：

```cpp
f(u) = parent[u];
```

就是树上的 Binary Lifting。

如果：

```cpp
f(u) = receiver[u];
```

就是 Functional Graph 上的 Binary Lifting。

---

# 4. 二维表到底是怎么构建的

Binary Lifting 通常维护：

```cpp
up[n][LOG]
```

其中：

```text
u 维度：节点
j 维度：2^j 步
```

例如：

```text
             1步    2步    4步    8步
             j=0    j=1    j=2    j=3

node 0       ...
node 1       ...
node 2       ...
node 3       ...
...
```

## 4.1 树上 DFS 构建

在树上，可以 DFS 时逐节点构建。

```cpp
void dfs(int u, int parent)
{
    up[u][0] = parent;

    for (int j = 1; j < LOG; ++j)
    {
        if (up[u][j - 1] != -1)
        {
            up[u][j] =
                up[ up[u][j - 1] ][j - 1];
        }
        else
        {
            up[u][j] = -1;
        }
    }

    for (int v : graph[u])
    {
        if (v == parent)
            continue;

        depth[v] = depth[u] + 1;
        dfs(v, u);
    }
}
```

这里 DFS 保证：

> parent 一定比 child 先处理。

因此当计算：

```cpp
up[u][j]
```

时，祖先节点的 `up[*][*]` 已经存在。

---

## 4.2 先求 parent，再整张表构建

这个写法更像二维 DP，也非常适合面试。

先得到：

```cpp
parent[u]
```

然后：

```cpp
for (int u = 0; u < n; ++u)
    up[u][0] = parent[u];
```

再逐列构建：

```cpp
for (int j = 1; j < LOG; ++j)
{
    for (int u = 0; u < n; ++u)
    {
        int p = up[u][j - 1];

        if (p != -1)
            up[u][j] = up[p][j - 1];
        else
            up[u][j] = -1;
    }
}
```

注意循环顺序：

```cpp
for (j)
    for (u)
```

因为：

```text
第 j 列依赖第 j-1 列
```

---

# 5. 第一类应用：K-th Ancestor

问题：

> 给定节点 `u`，求它向上第 `k` 个祖先。

这就是最纯粹的 Binary Lifting。

定义：

```cpp
up[u][j] = u 的第 2^j 个祖先
```

查询：

```cpp
int jump(int u, int k)
{
    for (int j = 0; j < LOG && u != -1; ++j)
    {
        if (k & (1 << j))
            u = up[u][j];
    }

    return u;
}
```

### 典型题

- LeetCode 1483 — Kth Ancestor of a Tree Node

这是学习 Binary Lifting 最标准的入门题。

---

# 6. 第二类应用：LCA

LCA：

> Lowest Common Ancestor，最近公共祖先。

例如：

```text
        1
       / \
      2   3
     / \
    4   5
       / \
      6   7
```

则：

```text
LCA(6, 7) = 5
LCA(4, 6) = 2
LCA(4, 3) = 1
```

Binary Lifting 求 LCA 分两步。

## Step 1：对齐深度

如果：

```text
depth[u] > depth[v]
```

先让 `u` 向上跳：

```cpp
u = jump(u, depth[u] - depth[v]);
```

使得：

```text
depth[u] == depth[v]
```

## Step 2：同时从大步向上跳

如果：

```cpp
u == v
```

那么已经找到 LCA。

否则：

```cpp
for (int j = LOG - 1; j >= 0; --j)
{
    if (up[u][j] != up[v][j])
    {
        u = up[u][j];
        v = up[v][j];
    }
}
```

最后：

```cpp
return up[u][0];
```

因为此时：

```text
u      v
 \    /
   LCA
```

二者已经处在 LCA 的两个直接子方向上。

---

# 7. LCA 完整模板

```cpp
class LCA
{
public:
    int n;
    int LOG;

    std::vector<std::vector<int>> graph;
    std::vector<std::vector<int>> up;
    std::vector<int> depth;

    explicit LCA(int n)
        : n(n),
          LOG(1),
          graph(n),
          depth(n, 0)
    {
        while ((1 << LOG) <= n)
            ++LOG;

        up.assign(n, std::vector<int>(LOG, -1));
    }

    void addEdge(int u, int v)
    {
        graph[u].push_back(v);
        graph[v].push_back(u);
    }

    void dfs(int u, int parent)
    {
        up[u][0] = parent;

        for (int j = 1; j < LOG; ++j)
        {
            if (up[u][j - 1] != -1)
            {
                up[u][j] =
                    up[ up[u][j - 1] ][j - 1];
            }
        }

        for (int v : graph[u])
        {
            if (v == parent)
                continue;

            depth[v] = depth[u] + 1;
            dfs(v, u);
        }
    }

    int jump(int u, int k) const
    {
        for (int j = 0; j < LOG && u != -1; ++j)
        {
            if (k & (1 << j))
                u = up[u][j];
        }

        return u;
    }

    int lca(int u, int v) const
    {
        if (depth[u] < depth[v])
            std::swap(u, v);

        u = jump(u, depth[u] - depth[v]);

        if (u == v)
            return u;

        for (int j = LOG - 1; j >= 0; --j)
        {
            if (up[u][j] != up[v][j])
            {
                u = up[u][j];
                v = up[v][j];
            }
        }

        return up[u][0];
    }
};
```

---

# 8. 第三类应用：Tree Distance

对于无权树：

```text
distance(u, v)
=
depth[u]
+ depth[v]
- 2 * depth[LCA(u, v)]
```

代码：

```cpp
int p = lca(u, v);

int dist =
    depth[u]
    + depth[v]
    - 2 * depth[p];
```

如果树有边权，则把 `depth` 换成：

```cpp
distFromRoot[u]
```

于是：

```text
distance(u, v)
=
distFromRoot[u]
+ distFromRoot[v]
- 2 * distFromRoot[LCA(u, v)]
```

---

# 9. 第四类应用：Path Aggregate

Binary Lifting 不只能记录：

```cpp
up[u][j]
```

还可以同时维护这段 `2^j` 路径上的信息。

例如：

```cpp
maxEdge[u][j]
minEdge[u][j]
sum[u][j]
xorValue[u][j]
gcdValue[u][j]
count[u][j]
```

## 9.1 最大边权

定义：

```cpp
maxEdge[u][j]
```

表示：

> 从 `u` 向上走 `2^j` 条边，这段路径上的最大边权。

假设：

```cpp
int mid = up[u][j - 1];
```

则：

```cpp
up[u][j] =
    up[mid][j - 1];

maxEdge[u][j] =
    std::max(
        maxEdge[u][j - 1],
        maxEdge[mid][j - 1]
    );
```

原因仍然是：

```text
完整 2^j
=
前 2^(j-1)
+
后 2^(j-1)
```

---

# 10. MST + Binary Lifting：Minimum Bottleneck Query

给定无向带权图，要查询：

> 从 `u` 到 `v` 的所有路径中，让“路径最大边权”尽可能小。

数学表达：

```text
min over path P ( max edge weight in P )
```

这是 **Minimum Bottleneck Path**。

MST 有一个重要性质：

> MST 中 `u -> v` 的唯一路径，其最大边权等于原图 `u -> v` 的 minimum bottleneck value。

所以：

```text
原图
 ↓ Kruskal
MST / Minimum Spanning Forest
 ↓
Binary Lifting
 ↓
query path max
```

查询：

```text
bottleneck(u, v)
=
max(
    maxEdge(u -> LCA),
    maxEdge(v -> LCA)
)
```

这是一种非常重要的“降维”：

```text
Graph Query
    ↓ MST
Tree Query
    ↓ Binary Lifting
O(log N)
```

### 典型题

- LeetCode 1724 — Checking Existence of Edge Length Limited Paths II

如果 query 问：

```text
是否存在 u -> v 路径，
使所有边权 < limit
```

那么等价于：

```cpp
pathMax(u, v) < limit
```

---

# 11. 第五类应用：Functional Graph

Functional Graph：

> 每个节点恰好有一个 outgoing edge。

也就是：

```cpp
next[u]
```

永远唯一。

例如：

```text
0 -> 3
1 -> 2
2 -> 5
3 -> 2
4 -> 3
5 -> 1
```

这里可能有：

- 环
- 链进入环
- 多个节点指向同一节点

但只要每个节点 next 唯一，就可以做 Binary Lifting。

定义：

```cpp
up[u][0] = next[u];

up[u][j] =
    up[ up[u][j - 1] ][j - 1];
```

然后可以在：

```text
O(log K)
```

时间内求：

> 从 `u` 连续执行 next `K` 次以后在哪里。

即使：

```text
K = 10^18
```

也只需要大约 60 层。

---

# 12. LeetCode 2836 — Maximize Value of Function in a Ball Passing Game

这是理解：

> **Functional Graph + Binary Lifting + Aggregate**

非常好的题。

---

## 12.1 题目模型

有 `n` 个玩家：

```text
0, 1, 2, ..., n-1
```

给定：

```cpp
receiver[i]
```

表示：

> 玩家 `i` 拿到球之后，一定传给 `receiver[i]`。

可以选择任意玩家作为起点。

一共传球：

```text
k 次
```

得分：

> 所有碰过球的玩家编号之和。

注意：

- 起始玩家算一次
- 每次传球到达的玩家算一次
- 同一个玩家如果重复经过，也重复计分

---

## 12.2 为什么是 Functional Graph

因为每个玩家：

```text
只有一个固定 receiver
```

所以：

```cpp
next[u] = receiver[u]
```

这就是 Functional Graph。

---

# 13. LC 2836 的两个倍增表

维护：

```cpp
jump[u][j]
```

表示：

> 从 `u` 开始，经过 `2^j` 次传球之后，球在哪里。

同时维护：

```cpp
sum[u][j]
```

表示：

> 从 `u` 开始进行 `2^j` 次传球，这些传球“到达的玩家编号之和”。

这里定义：

> `sum[u][j]` **不包含起始节点 u**。

例如：

```text
u -> a -> b -> c -> d
```

走 4 步：

```cpp
jump[u][2] = d;
sum[u][2] = a + b + c + d;
```

---

# 14. LC 2836 基础状态

因为：

```text
2^0 = 1
```

从 `u` 传一次：

```text
u -> receiver[u]
```

所以：

```cpp
jump[u][0] = receiver[u];
sum[u][0]  = receiver[u];
```

注意：

```cpp
sum[u][0] != u
```

因为我们定义 `sum` 不包含当前起点，只统计“接下来走的这一段”。

---

# 15. LC 2836 倍增递推

假设：

```cpp
int mid = jump[u][j - 1];
```

那么：

```text
u
│
│ 2^(j-1)
↓
mid
│
│ 2^(j-1)
↓
destination
```

于是：

```cpp
jump[u][j] =
    jump[mid][j - 1];
```

同时：

```cpp
sum[u][j] =
    sum[u][j - 1]
    + sum[mid][j - 1];
```

这两行就是整道题的核心。

---

# 16. LC 2836 例子

假设：

```text
receiver = [2, 0, 1]
```

也就是：

```text
0 -> 2 -> 1 -> 0
```

## j = 0：1 步

```text
u        jump[u][0]    sum[u][0]

0             2             2
1             0             0
2             1             1
```

## j = 1：2 步

从 0：

```text
0 -> 2 -> 1
```

所以：

```cpp
jump[0][1] = 1;
sum[0][1]  = 2 + 1;
```

利用递推：

```cpp
mid = jump[0][0]; // 2

jump[0][1]
    = jump[2][0]
    = 1;

sum[0][1]
    = sum[0][0] + sum[2][0]
    = 2 + 1
    = 3;
```

---

# 17. LC 2836 查询

假设：

```text
k = 13
```

则：

```text
13 = 8 + 4 + 1
   = 1101₂
```

所以查询时：

```text
跳 8
跳 4
跳 1
```

代码：

```cpp
long long score = start;
int cur = start;

for (int j = 0; j < LOG; ++j)
{
    if (k & (1LL << j))
    {
        score += sum[cur][j];
        cur = jump[cur][j];
    }
}
```

这里：

```cpp
score = start;
```

是因为起始玩家本身也计分。

---

# 18. LC 2836 完整 C++ 代码

```cpp
class Solution
{
public:
    long long getMaxFunctionValue(
        std::vector<int>& receiver,
        long long k)
    {
        const int n = static_cast<int>(receiver.size());

        /*
         * k <= 1e10 时大约只需要 34 层。
         *
         * LOG 表示要预处理多少个：
         *
         * 1, 2, 4, 8, ...
         */
        int LOG = 1;

        while ((1LL << LOG) <= k)
            ++LOG;

        /*
         * jump[u][j]:
         *
         * 从 u 开始传 2^j 次球后，
         * 球在哪个玩家手里。
         */
        std::vector<std::vector<int>> jump(
            n,
            std::vector<int>(LOG));

        /*
         * sum[u][j]:
         *
         * 从 u 开始传 2^j 次球，
         * 这些传球所到达的玩家编号之和。
         *
         * 不包含起始节点 u。
         */
        std::vector<std::vector<long long>> sum(
            n,
            std::vector<long long>(LOG));

        /*
         * Step 1:
         * 初始化 2^0 = 1 步。
         */
        for (int u = 0; u < n; ++u)
        {
            jump[u][0] = receiver[u];
            sum[u][0] = receiver[u];
        }

        /*
         * Step 2:
         * Binary Lifting 预处理。
         *
         * 2^j =
         * 2^(j-1) + 2^(j-1)
         */
        for (int j = 1; j < LOG; ++j)
        {
            for (int u = 0; u < n; ++u)
            {
                /*
                 * 先跳前半段后的节点。
                 */
                int mid = jump[u][j - 1];

                /*
                 * 再从 mid 跳后半段。
                 */
                jump[u][j] =
                    jump[mid][j - 1];

                /*
                 * 两半的路径得分相加。
                 */
                sum[u][j] =
                    sum[u][j - 1]
                    + sum[mid][j - 1];
            }
        }

        /*
         * Step 3:
         * 枚举每一个可能的起点。
         */
        long long answer = 0;

        for (int start = 0; start < n; ++start)
        {
            /*
             * 起始玩家也算入得分。
             */
            long long score = start;

            /*
             * 当前持球玩家。
             */
            int cur = start;

            /*
             * 把 k 按二进制拆成若干 2^j。
             */
            for (int j = 0; j < LOG; ++j)
            {
                if (k & (1LL << j))
                {
                    /*
                     * 累加这一整段 2^j 步的得分。
                     */
                    score += sum[cur][j];

                    /*
                     * 一次跳过 2^j 次传球。
                     */
                    cur = jump[cur][j];
                }
            }

            answer = std::max(answer, score);
        }

        return answer;
    }
};
```

复杂度：

```text
预处理：O(N log K)
枚举起点查询：O(N log K)

总时间：O(N log K)
空间：O(N log K)
```

---

# 19. LC 1483 vs LC 2836

LC 1483：

```text
只关心：

最后在哪里？
```

维护：

```cpp
up[u][j]
```

LC 2836：

```text
不仅关心：

最后在哪里？

还关心：

途中累计了什么？
```

所以维护：

```cpp
jump[u][j]
sum[u][j]
```

这正是 Binary Lifting 的重要推广：

> **jump + aggregate**

---

# 20. Binary Lifting 可以携带哪些 Aggregate

只要一段路径信息可以由两个连续半段组合得到，通常都可以一起维护。

例如：

| 信息 | 合并方式 |
|---|---|
| Sum | `a + b` |
| Maximum | `max(a, b)` |
| Minimum | `min(a, b)` |
| XOR | `a ^ b` |
| OR | `a | b` |
| AND | `a & b` |
| GCD | `gcd(a, b)` |
| Count | `a + b` |

通常希望操作满足 associativity：

```text
(a op b) op c
=
a op (b op c)
```

这样就能安全地按不同大小块组合。

---

# 21. 第六类应用：Path 上第 K 个节点

给定：

```text
u, v, k
```

要求：

> `u -> v` 路径上的第 `k` 个节点。

先：

```cpp
int p = lca(u, v);
```

定义：

```cpp
int du = depth[u] - depth[p];
int dv = depth[v] - depth[p];
```

路径：

```text
u
↑
↑ du
LCA
↓
↓ dv
v
```

如果：

```text
k <= du
```

那么：

```cpp
return jump(u, k);
```

否则：

```cpp
int total = du + dv;

return jump(v, total - k);
```

所以这类题本质：

```text
LCA + K-th Ancestor
```

---

# 22. 第七类应用：向上找最远满足条件的位置

Binary Lifting 不一定是“已知跳多少步”。

还可以解决：

> 找能向上跳到的最远位置，同时保持某个条件成立。

例如：

```text
从 u 向上，
要求 path maximum <= X，
最高能到哪里？
```

从最大的 `2^j` 开始试：

```cpp
for (int j = LOG - 1; j >= 0; --j)
{
    if (up[u][j] != -1 &&
        maxEdge[u][j] <= X)
    {
        u = up[u][j];
    }
}
```

这种模式可以理解成：

> **二进制贪心 / 从大块开始试跳。**

---

# 23. Prefix Sum 可以看成 Binary Lifting 吗？

严格来说：

> **Prefix Sum 不是 Binary Lifting。**

两者共同点是：

> 都是通过预处理减少查询时的重复工作。

但它们预处理的结构不同。

---

## 23.1 Prefix Sum

定义：

```cpp
prefix[i]
=
a[0] + a[1] + ... + a[i - 1];
```

于是区间：

```text
[l, r)
```

可以：

```cpp
prefix[r] - prefix[l]
```

时间：

```text
预处理 O(N)
查询 O(1)
```

Prefix Sum 实际保存的是：

```text
长度 0
长度 1
长度 2
长度 3
长度 4
长度 5
...
```

它没有：

```text
1, 2, 4, 8, 16...
```

这样的倍增结构。

---

# 24. 如果数组也按 1、2、4、8 预处理呢？

完全可以。

例如定义：

```cpp
sum[i][j]
```

表示：

> 从数组位置 `i` 开始，长度为 `2^j` 的区间和。

那么：

```cpp
sum[i][j] =
    sum[i][j - 1]
    + sum[i + (1 << (j - 1))][j - 1];
```

结构上和 Binary Lifting 极其类似：

```text
长度 2^j
=
前 2^(j-1)
+
后 2^(j-1)
```

但是对于静态 Range Sum，这样做通常没有意义，因为 Prefix Sum 更简单：

| 方法 | 预处理 | Range Sum |
|---|---:|---:|
| Prefix Sum | O(N) | O(1) |
| 2^j 分块 | O(N log N) | O(log N) |

所以静态数组区间和：

> 优先 Prefix Sum。

---

# 25. Sparse Table 和 Binary Lifting 的关系

这两个结构背后的思想非常接近。

Sparse Table：

```cpp
st[i][j]
```

表示：

> 从数组位置 `i` 开始，长度 `2^j` 的区间信息。

Binary Lifting：

```cpp
up[u][j]
```

表示：

> 从节点 `u` 开始，沿固定 next 走 `2^j` 步后的状态。

所以：

```text
                Doubling
                   │
        ┌──────────┴──────────┐
        │                     │
  Sparse Table         Binary Lifting
        │                     │
 array range 2^j        jump 2^j
```

可以简单记成：

> Sparse Table = 数组区间上的倍增  
> Binary Lifting = 状态跳转上的倍增

---

# 26. Fenwick Tree 和 Binary Lifting 的关系

Fenwick Tree / BIT 也强烈依赖二进制。

例如：

```text
13 = 1101₂
   = 8 + 4 + 1
```

Fenwick 查询 prefix 时：

```cpp
while (i > 0)
{
    ans += tree[i];
    i -= i & -i;
}
```

例如：

```text
13
↓ -1
12
↓ -4
8
↓ -8
0
```

对应：

```text
13 = 8 + 4 + 1
```

而 Binary Lifting 查询：

```text
k = 13

jump 8
jump 4
jump 1
```

所以它们共享一个非常重要的思想：

> **Binary Decomposition（二进制拆分）**

但用途不同：

```text
Fenwick Tree：
    二进制拆 index / range

Binary Lifting：
    二进制拆 jump count
```

---

# 27. Prefix Sum / Fenwick / Sparse Table / Binary Lifting 对比

| 技术 | 核心对象 | 是否使用 2^j | 主要用途 |
|---|---|---|---|
| Prefix Sum | 前缀 | 否 | 静态区间和 |
| Fenwick Tree | 对齐区间 | 是 | Prefix / Range + Update |
| Sparse Table | 长度 2^j 区间 | 是 | 静态 RMQ |
| Binary Lifting | 走 2^j 步后的状态 | 是 | ancestor / next / path |
| Segment Tree | 递归区间划分 | 间接 | 动态 Range Query / Update |

可以统一理解为：

```text
                 预计算 + 组合查询
                         │
          ┌──────────────┼──────────────┐
          │              │              │
     Prefix Sum      Binary Ideas   Segment Tree
                         │
             ┌───────────┼───────────┐
             │           │           │
          Fenwick   Sparse Table  Binary Lifting
```

---

# 28. Binary Lifting vs Segment Tree

Binary Lifting 更适合：

```text
固定 next / parent
静态树
祖先查询
LCA
路径 min/max
K-th node
Functional Graph
```

Segment Tree 更适合：

```text
数组 / Euler Tour 后的区间
动态修改
区间查询
区间更新
```

Binary Lifting 一般假设：

> parent / next 关系基本静态。

如果 parent 经常改变，普通 Binary Lifting 并不适合。

---

# 29. Binary Lifting vs Heavy-Light Decomposition

Binary Lifting：

```text
强项：

K-th ancestor
LCA
静态 path min/max
静态 path aggregate
```

一般查询：

```text
O(log N)
```

Heavy-Light Decomposition：

```text
强项：

path query
path update
节点 / 边动态更新
```

通常配合：

```text
Segment Tree / Fenwick Tree
```

常见复杂度：

```text
O(log² N)
```

简单判断：

```text
静态 Tree Path Query
        ↓
Binary Lifting

动态 Tree Path Query / Update
        ↓
HLD + Segment Tree
```

---

# 30. 题型识别

看到这些关键词，应考虑 Binary Lifting：

```text
第 K 个祖先
K-th ancestor

LCA
最近公共祖先

树上大量 query

树上路径 max / min / sum

u-v path 上第 K 个节点

Repeated parent

Repeated next

Functional Graph

K 次传送

K 次函数调用

K 很大：
10^9
10^18

MST 后大量 bottleneck query

固定 next + 大量 query
```

最强烈的识别信号：

> **固定 next / parent + 重复执行很多次 + 多次查询**

---

# 31. 推荐练习题

## ★ 入门

### LC 1483 — Kth Ancestor of a Tree Node

重点：

```text
up[u][j]
jump(u, k)
```

纯 Binary Lifting 模板。

---

## ★ LCA 基础

### LC 236 — Lowest Common Ancestor of a Binary Tree

这题不一定需要 Binary Lifting，但适合先理解 LCA 本身。

进一步应掌握：

```text
Binary Lifting LCA
```

用于大量 LCA queries。

---

## ★★ Functional Graph

### LC 2836 — Maximize Value of Function in a Ball Passing Game

重点：

```text
jump + sum
```

非常推荐。

---

## ★★ MST + Binary Lifting

### LC 1724 — Checking Existence of Edge Length Limited Paths II

重点：

```text
Graph
 ↓
MST
 ↓
Tree
 ↓
Binary Lifting
 ↓
Bottleneck Query
```

---

## ★★ Tree Path Aggregate

### LC 2846 — Minimum Edge Weight Equilibrium Queries in a Tree

重点：

```text
LCA
+
路径信息
```

---

## ★★★ 扩展

建议再练：

```text
Path Maximum Query
Path Minimum Query
K-th Node on Path
Weighted Tree Distance
Functional Graph + Sum
Functional Graph + Max
MST Bottleneck Query
```

---

# 32. 面试最应该记住的三个模板

## 模板 1：构建

```cpp
for (int u = 0; u < n; ++u)
    up[u][0] = next[u];

for (int j = 1; j < LOG; ++j)
{
    for (int u = 0; u < n; ++u)
    {
        up[u][j] =
            up[ up[u][j - 1] ][j - 1];
    }
}
```

---

## 模板 2：跳 K 步

```cpp
int jump(int u, long long k)
{
    for (int j = 0; j < LOG; ++j)
    {
        if (k & (1LL << j))
            u = up[u][j];
    }

    return u;
}
```

---

## 模板 3：Jump + Aggregate

```cpp
int mid = up[u][j - 1];

up[u][j] =
    up[mid][j - 1];

agg[u][j] =
    combine(
        agg[u][j - 1],
        agg[mid][j - 1]
    );
```

其中：

```cpp
combine = +
combine = max
combine = min
combine = gcd
combine = xor
...
```

---

# 33. 最终知识图

```text
                       Doubling
                          │
          ┌───────────────┴────────────────┐
          │                                │
     Sparse Table                    Binary Lifting
    Array Range 2^j                    Jump 2^j
                                           │
                 ┌─────────────────────────┼─────────────────────────┐
                 │                         │                         │
           K-th Ancestor                  LCA                Functional Graph
                 │                         │                         │
                 │                    Tree Path                 K-th next
                 │                         │                         │
                 │               ┌─────────┼─────────┐               │
                 │               │         │         │               │
                 │             dist      min/max    kth             sum
                 │                         │                         │
                 └────────────── Aggregate Information ─────────────┘
                                           │
                                       MST + LCA
                                           │
                                    Bottleneck Query
                                           │
                                        LC 1724
```

---

# 34. 一句话总结

Binary Lifting 不应只记成：

> “LCA 的一种算法”。

更准确的理解是：

> **对一个可以重复执行的确定性跳转操作，预处理执行 1、2、4、8、16... 次之后的结果，再用二进制拆分完成任意次数的跳转。**

如果同时维护 aggregate：

```text
jump + sum
jump + max
jump + min
jump + count
...
```

就可以解决大量：

```text
Tree Query
Functional Graph Query
MST Bottleneck Query
Repeated Function Query
```

问题。

最值得形成条件反射的是：

```text
parent(parent(parent(...)))
next(next(next(...)))
f(f(f(...)))
```

如果重复次数很大，而且 next 是固定的：

> **考虑 Binary Lifting / Doubling。**
