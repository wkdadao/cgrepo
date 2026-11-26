# 动态规划（DP）与 Memoization 系统总结

> 范围：本系列讨论的 **DP 基础、Top-down Memo / Bottom-up、复杂度分析、0/1 与各种背包、树形 DP、Rerooting DP、区间 DP**，以及相关的序列 DP、状态压缩 DP 等扩展。
>
> 面向 C++17 算法面试、OA。重点是**推导状态和依赖关系，而不是死记模板**。
>
> 另有更专门的详解：[区间 DP 系统总结](./区间DP系统总结.md)。

## 目录

1. [统一分析框架](#1-统一分析框架)
2. [Memoization vs Tabulation](#2-memoization-vs-tabulation)
3. [线性与序列 DP](#3-线性与序列-dp)
4. [背包问题的统一分类](#4-背包问题的统一分类)
5. [0/1、完全、多重、混合、分组背包](#5-01完全多重混合分组背包)
6. [多维、双背包和依赖背包](#6-多维双背包和依赖背包)
7. [背包的最值、可行性、计数、恰好装满](#7-背包的最值可行性计数恰好装满)
8. [树形 DP：选/不选与路径约束](#8-树形-dp选不选与路径约束)
9. [换根 DP：Rerooting](#9-换根-dprerooting)
10. [区间 DP：两端、分割、第一步与最后一步](#10-区间-dp两端分割第一步与最后一步)
11. [其他 DP 扩展](#11-其他-dp-扩展)
12. [统一复杂度表与面试检查表](#12-统一复杂度表与面试检查表)
13. [LeetCode 练习路线](#13-leetcode-练习路线)

---

## 1. 统一分析框架

### 1.1 DP 在解决什么问题

DP 通过复用子问题结果来避免重复计算。典型特征：

- **最优子结构（Optimal Substructure）**：原问题的最优解能由某些子问题的最优解构造。
- **重叠子问题（Overlapping Subproblems）**：多个决策路径可能反复访问相同子问题。

这不是说只要存在这两个特征，随意设计的状态和转移就一定正确：**状态必须保留足以决定未来转移的信息**，且转移的候选集合要完整。

DP 不一定只求最大值，也可以求最小值、可达性、方案数、概率和其他可组合结果。

### 1.2 统一五步

1. **State（状态）**：某个状态精确表示什么？是否包含当前元素/端点？已处理前缀还是未处理后缀？
2. **Decision（决策）**：下一步有几种合法选择？是否遗漏/重复？
3. **Transition（转移）**：写出数学方程；每个依赖是否是更小、已可求的子问题？
4. **Base Case（边界）**：终止条件、不可达初值、空区间、叶子、容量 0 等。
5. **Evaluation & Complexity（计算顺序与复杂度）**：Top-down 还是 Bottom-up？状态数 × 单状态工作量；统计缓存、表和递归栈。

### 1.3 DP 是状态依赖图

将每个状态看作一个图节点。如果 A 的答案依赖 B，可画一条 A → B 的依赖边。

- **Top-down + Memo**：从目标状态沿依赖 DFS，先算子状态，回溯时缓存。
- **Bottom-up**：按满足依赖顺序的次序求值，类似依赖 DAG 的逆拓扑顺序。
- 如果存在不可消除的循环依赖，简单 DFS + Memo/单次填表往往不够，需要重新定义状态、使用图算法或迭代求解。

DP 与图最短路径可以存在联系，但**不代表所有 DP 都应调用 Dijkstra**：要先看是否具备非负边权、最短路径等适用条件。

### 1.4 复杂度核心

\[
\boxed{T=O(\text{实际求值状态数}\times\text{每状态计算成本})}
\]

- 线性状态 i，O(1) 转移：O(n)。
- 二维背包状态 (i,w)，O(1) 转移：O(nW)。
- 双资源背包状态 (i,a,b)：O(nAB)。
- 区间 (l,r)：O(n²) 种，若每状态枚举 k，通常 O(n³)。
- 树形 DP：若每条边常数次工作，O(n)；如果合并容量 DP，可能上升为 O(nW²) 等。

**Memo 要统计不重复的状态**，而不是没有缓存时的递归树。若缓存采用预分配二维数组，即使访问很少，已分配内存仍与数组总大小有关。

**辅助空间（Auxiliary Space）**通常计额外数组、哈希表、递归栈，不计只读输入和题目要求的输出；面试中说明口径。

---

## 2. Memoization vs Tabulation

### 2.1 Fibonacci 对比

朴素递归：

\[
F(n)=F(n-1)+F(n-2),\quad F(0)=0,\ F(1)=1
\]

时间 Θ(φⁿ)，递归栈 O(n)。

**Top-down Memo：**

~~~cpp
int fibMemo(int n) {
    vector<int> memo(n + 1, -1);

    auto dfs = [&](auto&& self, int x) -> int {
        if (x <= 1) return x;
        if (memo[x] != -1) return memo[x];
        return memo[x] = self(self, x - 1) + self(self, x - 2);
    };

    return dfs(dfs, n);
}
~~~

**Bottom-up Tabulation：**

~~~cpp
int fibDP(int n) {
    if (n <= 1) return n;
    vector<int> dp(n + 1, 0);
    dp[1] = 1;
    for (int i = 2; i <= n; ++i) {
        dp[i] = dp[i - 1] + dp[i - 2];
    }
    return dp[n];
}
~~~

**Bottom-up 滚动变量：**

~~~cpp
int fibRolling(int n) {
    if (n <= 1) return n;
    int a = 0, b = 1;
    for (int i = 2; i <= n; ++i) {
        int c = a + b;
        a = b;
        b = c;
    }
    return b;
}
~~~

这里使用 int 的实现只适用于不会溢出的输入范围，**例如 n ≤ 46**。

| 方法 | 时间 | 辅助空间 | 特性 |
|---|---|---|---|
| 朴素递归 | Θ(φⁿ) | O(n) | 大量重复计算 |
| Memo | Θ(n) | Θ(n) | 按需计算，额外递归栈 |
| 二维/一维 Tabulation | Θ(n) | Θ(n) | 显式计算顺序 |
| 滚动变量 | Θ(n) | Θ(1) | 利用短距离依赖 |

### 2.2 Memo 的通用结构

~~~cpp
dfs(state):
    if is_base(state):
        return base_value

    if visited(state):
        return memo[state]

    result = combine(dfs(next_state), ...)
    memo[state] = result
    visited(state) = true
    return result
~~~

- **Memo 并不是与 DP 并列的另一类算法**，而是 DP 的 Top-down 求值策略。
- 如果合法答案本身可能为 -1、0、INT_MAX，就不能随意用同一数值当未计算标记。使用 visited 或 optional。
- 如果状态依赖层数很大，递归可能栈溢出，即便时间复杂度是线性的。

### 2.3 什么时候优先使用哪一种

| 场景 | 通常优先 |
|---|---|
| 从目标递归拆分很自然，依赖复杂 | Memo |
| 状态空间巨大，但可达状态稀疏 | Memo（配合稀疏缓存） |
| 线性 DP、普通背包、空间压缩 | Bottom-up |
| 需要严格控制栈和内存访问 | Bottom-up |
| 树结构天然递归 | DFS Tree DP；深树可改迭代后序 |
| 区间 DP | 两者均可；Bottom-up 通常按长度递增 |

两者可能计算相同的数学状态，却因**被实际访问的状态数量、缓存布局、递归调用开销**而表现不同。

---

## 3. 线性与序列 DP

### 3.1 线性 DP：LC 198 House Robber

每个位置选/不选，相邻不能同时选：

\[
dp[i]=\max(dp[i-1],dp[i-2]+a[i])
\]

这里 i 使用数组下标，dp[-1] 可按 0 解释；更好实现为滚动状态：

~~~cpp
int rob(vector<int>& nums) {
    int prev2 = 0, prev1 = 0;
    for (int x : nums) {
        int cur = max(prev1, prev2 + x);
        prev2 = prev1;
        prev1 = cur;
    }
    return prev1;
}
~~~

时间 O(n)，空间 O(1)。

### 3.2 二维序列 DP：LC 1143 LCS

定义 dp[i][j]：字符串 a 的前 i 个字符与 b 的前 j 个字符的最长公共子序列长度。

\[
dp[i][j] =
\begin{cases}
dp[i-1][j-1]+1, & a[i-1]=b[j-1]\\
\max(dp[i-1][j],dp[i][j-1]), & \text{otherwise}
\end{cases}
\]

Base：dp[0][j] = dp[i][0] = 0。

~~~cpp
int longestCommonSubsequence(string a, string b) {
    int n = static_cast<int>(a.size());
    int m = static_cast<int>(b.size());

    vector<vector<int>> dp(n + 1, vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            dp[i][j] = a[i - 1] == b[j - 1]
                ? dp[i - 1][j - 1] + 1
                : max(dp[i - 1][j], dp[i][j - 1]);
        }
    }
    return dp[n][m];
}
~~~

时间 O(nm)，空间 O(nm)，仅求长度可压缩为 O(min(n,m)) 空间。

### 3.3 前缀 DP、网格 DP、DAG DP

- **Prefix DP**：dp[i] 表示前 i 个元素/字符的答案。LC 139 Word Break。
- **Grid DP**：dp[r][c] 依赖上方、左方等方向。LC 62 Unique Paths。
- **DAG DP**：按拓扑顺序更新从某节点到其后继的结果。与树形 DP 的共同点是无环依赖。

注意：这些是状态组织方式，不是互斥类型，同一道题可以同时有“前缀”和“计数”等属性。

---

## 4. 背包问题的统一分类

### 4.1 三个独立维度

**选物品规则**

| 类型 | 约束 | 典型处理 |
|---|---|---|
| 0/1 | 每件 0 或 1 次 | 当前物品选择后推进 i；容量倒序 |
| 完全（Unbounded） | 每种无限次 | 选择后可保持 i；容量正序 |
| 多重（Bounded） | 第 i 种至多 k_i 次 | 枚举数量 / 二进制拆分 / 单调队列 |
| 混合 | 不同物品使用不同规则 | 对应转移组合 |
| 分组 | 每组最多选择一件 | 当前组只读取上一组结果 |
| 依赖/树形 | 选子项必须选其前提 | 合法组合 / Tree DP |

**资源约束**：单容量、重量+体积等多资源、两个独立背包、恰好装满、不超过容量。

**优化目标**：最大值、最小值、可行性、组合计数、排列计数。

> 这三个维度可组合：例如 LC 474 是**0/1 选择 + 双资源容量 + 最大化个数**。

### 4.2 统一决策状态

最常见的递归状态：

\[
f(i,w)=\text{从第 i 件开始，容量还剩 w 时的最优结果}
\]

多维时扩展为 f(i,a,b)。但不必所有题目都保留 i，例如 LC 322 的 Memo 只需 remainingAmount 就足够。

要先决定：
- 容量是**上限**还是**必须恰好达到**？
- 同一个物品能否复用？
- 状态用**已处理前 i 件**，还是**待处理从 i 开始**？二者都可以，但公式不同。

---

## 5. 0/1、完全、多重、混合、分组背包

以下最大价值背包示例默认重量为正整数、价值非负、可不选物品、不要求恰好装满。

### 5.1 0/1 背包

设 dp[i][w]：前 i 件物品、容量不超过 w 的最大价值。

\[
dp[i][w]=\max(dp[i-1][w],dp[i-1][w-weight_i]+value_i)
\]

第二项要求 w≥weight_i。

**二维版：**

~~~cpp
int zeroOne2D(const vector<int>& weight, const vector<int>& value, int W) {
    int n = static_cast<int>(weight.size());
    vector<vector<int>> dp(n + 1, vector<int>(W + 1, 0));
    for (int i = 1; i <= n; ++i) {
        for (int w = 0; w <= W; ++w) {
            dp[i][w] = dp[i - 1][w];
            if (w >= weight[i - 1]) {
                dp[i][w] = max(dp[i][w],
                    dp[i - 1][w - weight[i - 1]] + value[i - 1]);
            }
        }
    }
    return dp[n][W];
}
~~~

**一维压缩版：容量倒序**

~~~cpp
int zeroOne(const vector<int>& weight, const vector<int>& value, int W) {
    vector<int> dp(W + 1, 0);
    for (size_t i = 0; i < weight.size(); ++i) {
        for (int w = W; w >= weight[i]; --w) {
            dp[w] = max(dp[w], dp[w - weight[i]] + value[i]);
        }
    }
    return dp[W];
}
~~~

**为什么倒序？** 对当前物品，读取的 dp[w-weight[i]] 必须仍是上一轮物品集合的结果，避免同一物品被本轮多次使用。

时间 O(nW)；二维空间 O(nW)，一维空间 O(W)。

### 5.2 完全背包（Unbounded Knapsack）

选过第 i 种物品后，仍可继续选择相同种类。

二维转移中的“选”来自**当前行**：

\[
dp[i][w]=\max(dp[i-1][w],dp[i][w-weight_i]+value_i)
\]

**一维容量正序：**

~~~cpp
int unbounded(const vector<int>& weight, const vector<int>& value, int W) {
    vector<int> dp(W + 1, 0);
    for (size_t i = 0; i < weight.size(); ++i) {
        for (int w = weight[i]; w <= W; ++w) {
            dp[w] = max(dp[w], dp[w - weight[i]] + value[i]);
        }
    }
    return dp[W];
}
~~~

时间 O(nW)、空间 O(W)。

**前提**：重量严格为正。若重量为 0、价值为正且可无限取，最大价值无上界。

### 5.3 多重背包（每种最多 k_i 件）

朴素转移：

\[
dp[i][w]=\max_{0\le t\le k_i,\ tw_i\le w}
\left(dp[i-1][w-tw_i]+tv_i\right)
\]

朴素时间上界 O(W Σk_i)、空间可优化到 O(W)。

**Binary Splitting（二进制拆分）：** 例如数量 13，可拆成 1、2、4、6 四组；每组当作一个不可重复的 0/1 物品。可表示 0..13 的任意合法数量。

~~~cpp
int boundedBinary(const vector<int>& weight,
                  const vector<int>& value,
                  const vector<int>& quantity, int W) {
    vector<int> dp(W + 1, 0);
    int n = static_cast<int>(weight.size());

    for (int i = 0; i < n; ++i) {
        int remaining = quantity[i];
        long long group = 1;

        while (remaining > 0) {
            int take = static_cast<int>(min(group, 1LL * remaining));
            long long packWeight = 1LL * take * weight[i];
            long long packValue = 1LL * take * value[i];

            if (packWeight <= W) {
                for (int w = W; w >= static_cast<int>(packWeight); --w) {
                    dp[w] = max(dp[w],
                        dp[w - static_cast<int>(packWeight)]
                            + static_cast<int>(packValue));
                }
            }
            remaining -= take;
            group *= 2;
        }
    }
    return dp[W];
}
~~~

此例默认拆包后的价值及总最优值可安全存入 int；真实约束较大时应将 dp 改为 long long。

时间 O(W Σlog(k_i+1))，空间 O(W)（若一次生成全部拆包则另占空间）。

**注意**：拆包适用于最优值和可行性，但**不宜原封不动用于原物品不可区分时的精确方案数**；相同数量可能由不同拆包子集表示，造成重复计数。

另外还有按重量模数分组的**单调队列优化多重背包**，可以将常见最大值版本优化到 O(nW)，属于进阶。

### 5.4 混合背包

每种物品可能是 0/1、完全或多重：
- 0/1：倒序。
- 完全：正序。
- 多重：枚举数量、拆包，或其他优化。

不是新的统一转移，而是依照**每类物品的可选次数**应用对应转移。

### 5.5 分组背包

每组最多选一件。关键是**同一组的候选全部从组开始前的 dp 读取**。

~~~cpp
struct Item {
    int weight;
    int value;
};

int groupKnapsack(const vector<vector<Item>>& groups, int W) {
    vector<int> dp(W + 1, 0);

    for (const auto& group : groups) {
        auto next = dp; // 允许当前组不选
        for (const Item& item : group) {
            for (int w = item.weight; w <= W; ++w) {
                next[w] = max(next[w],
                    dp[w - item.weight] + item.value);
            }
        }
        dp = move(next);
    }
    return dp[W];
}
~~~

时间 O(W × 总物品数)，辅助空间 O(W)。

**错误警惕**：如果一边枚举组内物品一边更新、读取同一个 dp，可能在同一组里选择多件。

---

## 6. 多维、双背包和依赖背包

### 6.1 Multidimensional Knapsack（多维容量）

每个选中的物品**同时消耗**多种资源，如重量 a_i 和体积 b_i。

状态 dp[a][b]：已处理物品中，两个资源容量分别不超过 a、b 时最大价值。

0/1 版本每个资源维度都要倒序：

~~~cpp
int twoResource(const vector<int>& wa,
                const vector<int>& wb,
                const vector<int>& value,
                int A, int B) {
    vector<vector<int>> dp(A + 1, vector<int>(B + 1, 0));
    for (size_t i = 0; i < wa.size(); ++i) {
        for (int a = A; a >= wa[i]; --a) {
            for (int b = B; b >= wb[i]; --b) {
                dp[a][b] = max(dp[a][b],
                    dp[a - wa[i]][b - wb[i]] + value[i]);
            }
        }
    }
    return dp[A][B];
}
~~~

时间 O(nAB)、空间 O(AB)。

**LC 474 Ones and Zeroes**：每个字符串的 0 数、1 数分别是两个资源消耗，价值都是 1。

### 6.2 Double Knapsack（两个独立背包）

两个独立背包容量 A、B；每件物品**最多放一个背包**，或不选。

\[
f(i,a,b)=\max
\begin{cases}
f(i+1,a,b),\\
v_i+f(i+1,a-w_i,b) & (w_i\le a),\\
v_i+f(i+1,a,b-w_i) & (w_i\le b)
\end{cases}
\]

与多维背包不同：放进 A **不会同时消耗 B**。

**Bottom-up：**（这里要求所有重量严格为正）

~~~cpp
int doubleKnapsack(const vector<int>& weight,
                   const vector<int>& value,
                   int A, int B) {
    vector<vector<int>> dp(A + 1, vector<int>(B + 1, 0));

    for (size_t i = 0; i < weight.size(); ++i) {
        int wt = weight[i], val = value[i];
        for (int a = A; a >= 0; --a) {
            for (int b = B; b >= 0; --b) {
                if (a >= wt)
                    dp[a][b] = max(dp[a][b],
                                   dp[a - wt][b] + val);
                if (b >= wt)
                    dp[a][b] = max(dp[a][b],
                                   dp[a][b - wt] + val);
            }
        }
    }
    return dp[A][B];
}
~~~

正重量及两个容量都倒序确保同一物品不会在两个背包中被重复计入。若允许 0 重物品，建议使用两层快照 dp/next，防止同一状态重复加价值。

时间 O(nAB)，辅助空间 O(AB)。

### 6.3 Dependent / Tree Knapsack（依赖背包）

**规则**：选中子项必须先选父项。例：主设备与附件、组织/课程依赖。

- 只有“主件+少量附件”时：**枚举合法组合 → 分组背包**。
- 一般树形依赖：固定树根，使用 Tree DP。

一种常见状态：

\[
dp[u][w]=\text{已选 u，且在 u 子树中恰好使用容量 w 时的最大价值}
\]

所有不可达状态初始化为 -INF；仅 dp[u][weight[u]]=value[u] 作为初始合法状态。

处理子节点 v 时：
- 可**不选 v 子树**，即保留现有 dp[u]。
- 若选 v，则分配 j 容量给它，并从其合法 dp[v][j] 转移。
- 对每个子节点合并时使用上一阶段的数组/正确倒序，以免一个子树被重复合并。

一般合并型转移是：

\[
next[w]=\max\left(
dp_u[w],
\max_{j>0}
\left(dp_u[w-j]+dp_v[j]\right)
\right)
\]

上式只是结构示意；具体还需处理每个 dp 的下标有效性、容量上界和不可达状态。

如果对每条父子边朴素枚举全部两个容量维度，常见时间上界 O(nW²)，实际可按子树资源上限裁剪。空间取决于存储全部节点 DP 还是逐子树合并。

### 6.4 Multi-choice vs Dependency

- **分组背包**：同组最多选一件，互斥。
- **依赖背包**：选子项必须选前提，体现先决条件。

约束本质不同；不能因为都出现“主件/附件”就自动把任意依赖树当普通分组背包。

---

## 7. 背包的最值、可行性、计数、恰好装满

### 7.1 初始化由状态语义决定

| 目标 | dp[0] | 其他容量初值 |
|---|---:|---|
| 最大价值，允许不装满（非负价值） | 0 | 0 |
| 最大价值，必须恰好装满 | 0 | -INF |
| 凑成目标的最少物品数 | 0 | +INF |
| 可行性（恰好凑出） | true | false |
| 恰好凑出的方案数 | 1 | 0 |

对于**恰好装满**，不能初始化所有容量为 0：那会把不可达状态视作合法选择。

对于 +INF/-INF，转移前应检查来源状态是否可达，以避免错误转移或溢出。

### 7.2 LC 416：Partition Equal Subset Sum，0/1 可行性

~~~cpp
bool canPartition(vector<int>& nums) {
    int sum = accumulate(nums.begin(), nums.end(), 0);
    if (sum % 2 != 0) return false;
    int target = sum / 2;

    vector<bool> dp(target + 1, false);
    dp[0] = true;

    for (int x : nums) {
        for (int s = target; s >= x; --s) {
            dp[s] = dp[s] || dp[s - x];
        }
    }
    return dp[target];
}
~~~

时间 O(n×target)，空间 O(target)，要求 nums 为非负整数。

**Bitset 优化**（目标和上界适配静态容量）：

~~~cpp
bitset<10001> reachable;
reachable[0] = 1;
for (int x : nums) {
    reachable |= reachable << x;
}
// reachable[target] 是否为 true
~~~

移位表示“每个可达和都可额外加一次 x”；使用新旧 bitset 的按位 OR 可避免在同轮无限重复使用当前物品。

### 7.3 LC 322：Coin Change，完全背包最小值

~~~cpp
int coinChange(vector<int>& coins, int amount) {
    const int INF = amount + 1;
    vector<int> dp(amount + 1, INF);
    dp[0] = 0;

    for (int coin : coins) {
        for (int s = coin; s <= amount; ++s) {
            dp[s] = min(dp[s], dp[s - coin] + 1);
        }
    }
    return dp[amount] == INF ? -1 : dp[amount];
}
~~~

前提：硬币面值均为正整数。复杂度 O(n×amount) 时间、O(amount) 空间。

### 7.4 LC 518：Coin Change II，组合计数

完全背包，**顺序不重要**。

~~~cpp
long long countCombinations(const vector<int>& coins, int amount) {
    vector<long long> dp(amount + 1, 0);
    dp[0] = 1;

    for (int coin : coins) {           // 物品在外
        for (int s = coin; s <= amount; ++s) {
            dp[s] += dp[s - coin];     // 容量正序
        }
    }
    return dp[amount];
}
~~~

每种面额按固定顺序加入，避免将 1+2 与 2+1 当成两个组合。

### 7.5 LC 377：Combination Sum IV，排列计数

完全背包，**顺序不同算不同方案**。

~~~cpp
long long countSequences(const vector<int>& nums, int target) {
    vector<long long> dp(target + 1, 0);
    dp[0] = 1;

    for (int s = 1; s <= target; ++s) {  // 容量在外
        for (int x : nums) {             // 枚举最后选择的数字
            if (x <= s) dp[s] += dp[s - x];
        }
    }
    return dp[target];
}
~~~

这两种计数模板都要求候选是**正整数且没有重复面额**；实际题目还需处理结果范围、溢出或模运算。

例如目标 4，数字 {1,2}：
- 组合：{1+1+1+1, 1+1+2, 2+2} → **3 种**。
- 排列：{1+1+1+1, 1+1+2, 1+2+1, 2+1+1, 2+2} → **5 种**。

### 7.6 LC 494：Target Sum

为每个非负数选正负号，使总和等于 target，属于**0/1 计数的转换**。

若所有数之和为 S，选为正号的元素和为 P：

\[
P-(S-P)=target \Rightarrow P=(S+target)/2
\]

当 S+target 为负、奇数或 target 不在 [-S,S] 范围时无解。随后统计**选取和恰为 P 的子集数量**，0/1 计数，容量倒序；注意元素为 0 时会使方案数翻倍。

### 7.7 循环方向速查

| 问题 | 外层 | 容量方向 | 原因 |
|---|---|---|---|
| 0/1 最值/可行性/计数 | 物品 | 倒序 | 每物品用一次 |
| 完全背包最值/可行性 | 物品 | 正序 | 可重复当前物品 |
| 完全背包组合计数 | 物品 | 正序 | 固定物品种类次序 |
| 完全背包排列计数 | 容量 | 正序；内层枚举物品 | 按最后一个元素分类 |
| 分组背包 | 组 | 从上一组快照读取 | 组内互斥 |

> “正序/倒序”描述的是**原地一维压缩时如何保持状态含义**；二维 DP 可以直接引用上一行或当前行，原则更清晰。

### 7.8 伪多项式复杂度

O(nW) 对数值 W 是多项式，但二进制输入表示 W 只需要 Θ(log W) 位，因此通常被称为**伪多项式**。

当 W 很大（如 10⁹）时，不应直接创建 W 大小的数组；可能需要利用 n 较小的 Meet-in-the-Middle、按价值 DP、稀疏状态等方案。

---

## 8. 树形 DP：选/不选与路径约束

树形 DP 通常使用一次 DFS **post-order（后序）**计算：子节点状态完成后，合并成父节点状态。除非状态依赖额外上下文，树上这种无环结构通常不需要 Memo 数组。

### 8.1 LC 337：House Robber III（独立集）

相邻父子节点不可同时选。

对每个节点 u 返回两个状态：

- skip[u]：不选 u 的最大价值；
- take[u]：选 u 的最大价值。

\[
take[u]=value[u]+\sum_{v\in child(u)} skip[v]
\]
\[
skip[u]=\sum_{v\in child(u)}\max(skip[v],take[v])
\]

~~~cpp
// LC337 提供 TreeNode 结构
class Solution {
public:
    int rob(TreeNode* root) {
        auto dfs = [&](auto&& self, TreeNode* u) -> pair<long long,long long> {
            if (!u) return {0, 0}; // {skip, take}

            auto [ls, lt] = self(self, u->left);
            auto [rs, rt] = self(self, u->right);

            long long skip = max(ls, lt) + max(rs, rt);
            long long take = u->val + ls + rs;
            return {skip, take};
        };

        auto [skip, take] = dfs(dfs, root);
        return static_cast<int>(max(skip, take));
    }
};
~~~

时间 O(n)，额外栈 O(h)，其中 h 为树高；退化为链时 O(n)。

### 8.2 LC 2925：Maximum Score After Applying Operations on a Tree

题目：https://leetcode.com/problems/maximum-score-after-applying-operations-on-a-tree/

可取走节点数值作为得分，但必须保证**每条从根到叶子的路径至少有一个节点保留非零价值**。原题 values[i] 为正。

直接最大化拿走的和不容易表达路径条件。反过来：

\[
\text{最大得分}=\sum_u value[u]-\text{最小必须保留的总价值}
\]

定义 keep[u]：在**仅考虑以 u 为根的子树**时，保证从 u 到每个叶子的路径至少保留一个节点的最小总保留价值。

- 若 u 是叶子，必须保留：keep[u]=value[u]。
- 若 u 非叶子，可以：
  1. 保留 u：路径条件已满足，子树其他节点都可以取走，保留成本 value[u]；
  2. 不保留 u：每个孩子分支必须各自满足条件，成本 sum keep[v]。

所以：

\[
keep[u]=
\begin{cases}
value[u], & u\text{ 为叶子}\\
\min\left(value[u],\ \sum_{v\in child(u)}keep[v]\right), & \text{否则}
\end{cases}
\]

**C++17 迭代后序**（避免深树递归栈风险）：

~~~cpp
class Solution {
public:
    long long maximumScoreAfterOperations(
        vector<vector<int>>& edges, vector<int>& values) {
        int n = static_cast<int>(values.size());
        vector<vector<int>> graph(n);
        for (const auto& e : edges) {
            graph[e[0]].push_back(e[1]);
            graph[e[1]].push_back(e[0]);
        }

        vector<int> parent(n, -1), order;
        order.reserve(n);
        parent[0] = 0;
        order.push_back(0);
        for (size_t i = 0; i < order.size(); ++i) {
            int u = order[i];
            for (int v : graph[u]) {
                if (v == parent[u]) continue;
                parent[v] = u;
                order.push_back(v);
            }
        }

        vector<long long> keep(n, 0);
        long long total = 0;
        for (int x : values) total += x;

        for (int i = n - 1; i >= 0; --i) {
            int u = order[i];
            long long childrenCost = 0;
            bool leaf = true;

            for (int v : graph[u]) {
                if (v == parent[u]) continue;
                leaf = false;
                childrenCost += keep[v];
            }

            keep[u] = leaf ? values[u]
                           : min(1LL * values[u], childrenCost);
        }

        return total - keep[0];
    }
};
~~~

时间 O(n)、辅助空间 O(n)。

**易错点**：叶子不能套用 min(value[u],0)，否则会错误地允许整条路径全部被取走。

---

## 9. 换根 DP：Rerooting

### 9.1 LC 834 — Sum of Distances in Tree

题目：https://leetcode.com/problems/sum-of-distances-in-tree/

求树上每个节点到所有其他节点的距离之和：

\[
ans[u]=\sum_{v=0}^{n-1}dist(u,v)
\]

最直观的方法：每个节点启动 BFS / DFS。无权树每次 O(n)，总共 **O(n²)**；不用 Floyd-Warshall 或 Dijkstra。

但不同起点的距离总和有可复用关系，可以 **O(n)** 完成。

### 9.2 第一次 DFS：根的答案 + 子树节点数

固定节点 0 为根：

\[
size[u]=1+\sum_{v\in child(u)} size[v]
\]
\[
ans[0]=\sum_u depth[u]
\]

### 9.3 第二次 DFS：父节点到子节点的换根转移

将根从 u 移到其子节点 v：

- v 子树内 size[v] 个节点距离减少 1；
- 其余 n-size[v] 个节点距离增加 1。

\[
\boxed{ans[v]=ans[u]+n-2size[v]}
\]

**注意**：这里不需要重新求最短路径；一个节点的结果可以从它父节点的结果 O(1) 推出。

### 9.4 C++17 迭代实现（避免深树递归）

~~~cpp
class Solution {
public:
    vector<int> sumOfDistancesInTree(
        int n, vector<vector<int>>& edges) {

        vector<vector<int>> graph(n);
        for (const auto& e : edges) {
            graph[e[0]].push_back(e[1]);
            graph[e[1]].push_back(e[0]);
        }

        vector<int> parent(n, -1), depth(n, 0), order;
        order.reserve(n);
        parent[0] = 0;
        order.push_back(0);

        // 先构建根为 0 的树、深度与遍历顺序
        for (size_t i = 0; i < order.size(); ++i) {
            int u = order[i];
            for (int v : graph[u]) {
                if (v == parent[u]) continue;
                parent[v] = u;
                depth[v] = depth[u] + 1;
                order.push_back(v);
            }
        }

        vector<int> size(n, 1);
        vector<long long> ans(n, 0);

        for (int d : depth) ans[0] += d;

        // 后序统计子树大小
        for (int i = n - 1; i > 0; --i) {
            int u = order[i];
            size[parent[u]] += size[u];
        }

        // 先序换根
        for (size_t i = 1; i < order.size(); ++i) {
            int v = order[i];
            int u = parent[v];
            ans[v] = ans[u] + n - 2LL * size[v];
        }

        return vector<int>(ans.begin(), ans.end());
    }
};
~~~

时间 O(n)、辅助空间 O(n)。在 LC 834 的 n≤30000 约束内答案可以存入 int；中间用 long long 更稳妥。

### 9.5 Tree DP 和 Rerooting 的区别

- **普通树形 DP**：子节点 → 父节点，用子树答案求根答案。
- **Rerooting DP**：把某个根的全局答案转移到相邻新根，利用子树内/子树外的信息。
- 并不是所有 Rerooting 都有像 LC 834 这么简单的 O(1) 转移；一般可能需要前后缀合并或记录“来自父方向”的贡献。

---

## 10. 区间 DP：两端、分割、第一步与最后一步

专题详解与更多完整代码见：[区间 DP 系统总结](./区间DP系统总结.md)。

### 10.1 基础状态与区间边界

\[
dp[l][r]=\text{区间 } [l,r]\text{ 上某个子问题的解}
\]

常见两种：

- **闭区间 [l,r]**：包含端点，长度 r-l+1，分割为 [l,k]、[k+1,r]。
- **开区间 (l,r)**：保留端点，只处理内部，拆成 (l,k)、(k,r)，base 常为 r=l+1。

必须先固定状态含义，再写 base case 和循环界限。

### 10.2 类型 A：两端收缩（LC 516）

最长回文子序列：

\[
dp[l][r]=
\begin{cases}
1,&l=r\\
dp[l+1][r-1]+2,&s[l]=s[r]\\
\max(dp[l+1][r],dp[l][r-1]),&s[l]\ne s[r]
\end{cases}
\]

空区间贡献为 0。每状态 O(1)，共有 O(n²) 状态，时间 O(n²)、空间 O(n²)。

~~~cpp
int longestPalindromeSubseq(string s) {
    int n = static_cast<int>(s.size());
    if (n == 0) return 0;
    vector<vector<int>> dp(n, vector<int>(n, 0));

    for (int l = n - 1; l >= 0; --l) {
        dp[l][l] = 1;
        for (int r = l + 1; r < n; ++r) {
            if (s[l] == s[r])
                dp[l][r] = 2 + (r - l > 1 ? dp[l+1][r-1] : 0);
            else
                dp[l][r] = max(dp[l+1][r], dp[l][r-1]);
        }
    }
    return dp[0][n-1];
}
~~~

### 10.3 类型 B：枚举分割点（Matrix Chain Multiplication）

矩阵 A_i 尺寸为 p[i] × p[i+1]：

\[
dp[l][r] =
\min_{l\le k<r}\left(
dp[l][k]+dp[k+1][r]+p[l]p[k+1]p[r+1]
\right)
\]

单个矩阵代价为 0。时间 O(n³)、空间 O(n²)。

### 10.4 类型 C：枚举第一次操作（LC 1547）

切木棍：将切点排序并补上两端 0、n。

将区间 (l,r) 的**第一刀**放在 k，成本固定为 c[r]-c[l]；后面左右两段独立：

\[
dp[l][r]=\min_{l<k<r}
\left(dp[l][k]+dp[k][r]+c[r]-c[l]\right)
\]

Base：dp[l][l+1]=0。时间 O(m³)、空间 O(m²)，m 为切点数。

### 10.5 类型 D：枚举最后一次操作（LC 312）

Burst Balloons：从左到右直接戳破会改变邻接关系，左右区间难以独立。

给数组加两端虚拟的 1；定义 dp[l][r] 为**保留 l、r 两端，戳完开区间 (l,r) 内气球**的最大得分。

枚举最后被戳破的 k，此时邻居一定是 l、r：

\[
dp[l][r]=\max_{l<k<r}
\left(dp[l][k]+dp[k][r]+a[l]a[k]a[r]\right)
\]

Base：dp[l][l+1]=0；时间 O(n³)、空间 O(n²)。

~~~cpp
class Solution {
public:
    int maxCoins(vector<int>& nums) {
        int n = static_cast<int>(nums.size());
        vector<int> a(n + 2, 1);
        for (int i = 0; i < n; ++i) a[i + 1] = nums[i];

        vector<vector<long long>> dp(
            n + 2, vector<long long>(n + 2, 0));

        for (int len = 2; len <= n + 1; ++len) {
            for (int l = 0; l + len < n + 2; ++l) {
                int r = l + len;
                for (int k = l + 1; k < r; ++k) {
                    dp[l][r] = max(
                        dp[l][r],
                        dp[l][k] + dp[k][r]
                            + 1LL * a[l] * a[k] * a[r]);
                }
            }
        }
        return static_cast<int>(dp[0][n + 1]);
    }
};
~~~

### 10.6 其他区间题

**LC 1039 Minimum Score Triangulation of Polygon**

\[
dp[l][r]=\min_{l<k<r}
\left(dp[l][k]+dp[k][r]+a[l]a[k]a[r]\right)
\]

在顶点链 l..r 不足 3 个点时成本为 0。典型 O(n³)/O(n²)。

**LC 1000 Minimum Cost to Merge Stones**

每次合并连续 K 堆，最终变为 1 堆需满足：

\[
(n-1)\bmod (K-1)=0
\]

可以建三维状态 dp[l][r][p] 表示合并成 p 堆的最小成本，或根据可行堆数规则压缩成二维 DP。朴素常见上界 O(n³) 时间、O(n²) 空间（优化二维版本）。

### 10.7 区间 DP 的两类通用求值顺序

**按区间长度递增**（适合分割型）：

~~~cpp
for (int len = 1; len <= n; ++len) {
    for (int l = 0; l + len <= n; ++l) {
        int r = l + len - 1;
        // 先小区间，再大区间
    }
}
~~~

**左端点倒序、右端点正序**（适合收缩型）：

~~~cpp
for (int l = n - 1; l >= 0; --l) {
    for (int r = l + 1; r < n; ++r) {
        // 依赖 dp[l+1][r], dp[l][r-1] 等
    }
}
~~~

### 10.8 Memo vs Bottom-up 的区间复杂度

区间 (l,r) 最多 O(n²) 个不同状态；每个状态枚举 O(n) 个 k，因此 O(n³)。

Memo：
- 使用实际访问的不同区间数来推导时间；
- 二维预分配缓存 O(n²)；
- 递归深度最坏 O(n)。

Bottom-up：
- 明确依赖长度和边界；
- 不需要递归栈，但通常会遍历所有需要的区间。

---

## 11. 其他 DP 扩展

本节是本系列之外的知识地图，帮助建立框架，不等于上面每个专题都已完整覆盖。

| 类别 | 典型状态 | 核心约束/转移 | 代表题目 |
|---|---|---|---|
| DAG / 路径 DP | dp[u] | 按拓扑序，路径累计 | LC 2050 |
| Grid DP | dp[r][c] | 上/左等方向依赖 | LC 62、64 |
| 前缀/分段 DP | dp[i] | 枚举最后一段 | LC 139 |
| 双序列 DP | dp[i][j] | 前缀交互 | LC 1143、72 |
| LIS 型 DP | dp[i] | 结尾元素/单调结构 | LC 300 |
| 状态压缩 DP | dp[mask][u] | 已选择集合 + 末尾节点 | LC 847（也可 BFS） |
| 树形 DP | dp[u][state] | 子节点合并 | LC 337、2925 |
| 换根 DP | dp[u] | 父子/子树外贡献 | LC 834 |
| 数位 DP | dfs(pos,tight,started,...) | 数位约束、前导零 | LC 233、902 |
| 期望/概率 DP | dp[state] | 加权期望或概率转移 | 依题目 |

这些模型可能相互组合：例如树形背包就是**树形 DP + 容量维度**。

---

## 12. 统一复杂度表与面试检查表

### 12.1 常见上界

| 模式 | 状态数量 | 单状态工作 | 时间上界 | 常用辅助空间 |
|---|---:|---:|---:|---:|
| Fibonacci / 简单线性 DP | O(n) | O(1) | O(n) | O(1) 滚动 |
| LCS | O(nm) | O(1) | O(nm) | O(nm)，可压缩 |
| 0/1 背包 | O(nW) | O(1) | O(nW) | O(W) |
| 完全背包 | O(nW) | O(1) | O(nW) | O(W) |
| 多重背包朴素 | O(nW) | O(K) | O(nWK) | O(W) |
| 多重背包二进制 | O(WΣlog(k_i+1)) 次 | O(1) | O(WΣlog(k_i+1)) | O(W) |
| 分组背包 | O(W × 组内总物品数) | O(1) | 同左 | O(W) |
| 双资源/双背包 | O(nAB) | O(1) | O(nAB) | O(AB) |
| 树形 DP（每边常数工作） | O(n) | O(1) | O(n) | O(n) 最坏 |
| 树形背包朴素合并 | O(nW) | O(W) | O(nW²) | 依实现 |
| 换根 DP LC 834 | O(n) | O(1) | O(n) | O(n) |
| 区间两端型 | O(n²) | O(1) | O(n²) | O(n²) |
| 区间分割型 | O(n²) | O(n) | O(n³) | O(n²) |
| 状态压缩 DP | O(n2ⁿ) | O(n) | O(n²2ⁿ) | O(n2ⁿ) |

- 上表多是**上界**，并非每题都能宣称 Θ。条件判断、可达状态、剪枝、输入取值范围都可能改变实际计算数量。
- nW 等容量 DP 对**数值 W** 而不是输入编码长度多项式，应说明伪多项式。
- 当分析空间时，Memo 数组、递归栈、输出存储需要区分。

### 12.2 面试常见失败模式

1. **只记递推式，不知道 dp 的精确定义** → 一改题就错。
2. **Memo 的未计算标记与合法答案冲突** → 缓存不生效或返回错误。
3. **二维压一维但没考虑读取的是上一轮还是本轮** → 0/1 变完全背包。
4. **最小值初始化为 0、恰好装满初始化为 0** → 不可达状态被当作合法解。
5. **组合与排列计数混淆** → 循环嵌套顺序错误。
6. **组内直接复用当前更新结果** → 同一组选择多个物品。
7. **二进制拆分直接统计方案数** → 可能重复计数。
8. **双背包当双资源背包** → 资源消耗模型错误。
9. **树形 DP 忘记叶子约束** → 路径条件未满足。
10. **树形 DP 中没有固定父节点/visited** → 无向树反复访问父节点。
11. **区间 DP 不能保证子问题独立** → 转移根本不成立。
12. **开区间与闭区间混用** → off-by-one 和错误 base。
13. **复杂度直接数递归调用，不去重状态** → 错判 Memo 复杂度。
14. **容量/价值可能很大，却使用 int** → 溢出。
15. **长链树仍使用递归** → 可能出现 C++ 调用栈溢出。

### 12.3 我会在面试中按这个顺序回答

1. **Baseline**：先说直观暴力算法、为什么正确、哪里重复。
2. **State**：一句话说清状态语义。
3. **Transition**：把选择分支列全，并论证子问题独立。
4. **Base Case**：说明不可达与空状态的初值。
5. **Order**：是 Memo / Bottom-up？依赖何时计算完成？
6. **Complexity**：不同状态数 × 每状态工作量；空间口径。
7. **Optimization**：滚动数组、bitset、稀疏缓存、拆包、换根。
8. **Failure Modes**：非法状态、溢出、边界、递归深度。

---

## 13. LeetCode 练习路线

### Level 1：基本状态、Memo、Bottom-up

- [70 Climbing Stairs](https://leetcode.com/problems/climbing-stairs/) — 一维转移、滚动。
- [198 House Robber](https://leetcode.com/problems/house-robber/) — 选/不选。
- [1143 Longest Common Subsequence](https://leetcode.com/problems/longest-common-subsequence/) — 二维序列状态。
- [139 Word Break](https://leetcode.com/problems/word-break/) — 前缀可行性。

### Level 2：背包核心（优先掌握）

- [416 Partition Equal Subset Sum](https://leetcode.com/problems/partition-equal-subset-sum/) — 0/1 + bool + bitset。
- [322 Coin Change](https://leetcode.com/problems/coin-change/) — 完全背包 + min + INF。
- [518 Coin Change II](https://leetcode.com/problems/coin-change-ii/) — 完全背包组合计数。
- [377 Combination Sum IV](https://leetcode.com/problems/combination-sum-iv/) — 完全背包排列计数。
- [494 Target Sum](https://leetcode.com/problems/target-sum/) — 0/1 方案数，含 0 的边界。
- [474 Ones and Zeroes](https://leetcode.com/problems/ones-and-zeroes/) — 双资源 0/1 背包。
- [1049 Last Stone Weight II](https://leetcode.com/problems/last-stone-weight-ii/) — 差值最小化。
- [879 Profitable Schemes](https://leetcode.com/problems/profitable-schemes/) — 多维计数 DP。

### Level 3：树形与换根

- [337 House Robber III](https://leetcode.com/problems/house-robber-iii/) — 树节点选/不选。
- [2925 Maximum Score After Applying Operations on a Tree](https://leetcode.com/problems/maximum-score-after-applying-operations-on-a-tree/) — 根到叶路径约束。
- [834 Sum of Distances in Tree](https://leetcode.com/problems/sum-of-distances-in-tree/) — 子树大小 + Rerooting。

### Level 4：区间 DP（推荐每题写 Memo + Bottom-up）

- [516 Longest Palindromic Subsequence](https://leetcode.com/problems/longest-palindromic-subsequence/) — 两端收缩。
- [1039 Minimum Score Triangulation of Polygon](https://leetcode.com/problems/minimum-score-triangulation-of-polygon/) — 枚举分割点。
- [1547 Minimum Cost to Cut a Stick](https://leetcode.com/problems/minimum-cost-to-cut-a-stick/) — 枚举第一次操作。
- [312 Burst Balloons](https://leetcode.com/problems/burst-balloons/) — 枚举最后一次操作。
- [1000 Minimum Cost to Merge Stones](https://leetcode.com/problems/minimum-cost-to-merge-stones/) — 区间合并 + 堆数约束。

### 三组最值得比较的题

1. **LC 416 vs 494**：相同的 0/1 结构，为什么一个求 bool、一个统计数量？
2. **LC 322 vs 518 vs 377**：同样无限次使用硬币/数字，为什么分别是 min、组合计数、排列计数？
3. **LC 1547 vs 312**：为什么一个枚举第一步、一个枚举最后一步？何时左右区间才真正独立？

---

## 最终总结：一张思维导图

~~~text
Dynamic Programming (DP)
├── 求值方式
│   ├── Top-down + Memoization
│   └── Bottom-up + Tabulation / 滚动压缩
├── 状态结构
│   ├── 线性 / Prefix
│   ├── 序列 / 双序列
│   ├── 背包（i, capacity...）
│   │   ├── 0/1、完全、多重、混合
│   │   ├── 分组、依赖 / Tree Knapsack
│   │   ├── 多维容量、双背包
│   │   └── 最大/最小、可行性、方案数、恰好装满
│   ├── 树形（u, state）
│   │   ├── 选/不选
│   │   ├── 子树合并
│   │   └── Rerooting（子树内 / 子树外）
│   ├── 区间（l, r）
│   │   ├── 两端收缩
│   │   ├── 枚举分割点
│   │   └── 枚举第一次 / 最后一次操作
│   ├── DAG / Grid
│   └── Bitmask / Digit / Probability ...
└── 统一分析
    ├── 定义状态
    ├── 完整转移 + 子问题独立性
    ├── Base Case + 不可达初值
    ├── 依赖顺序
    └── 状态数量 × 单状态成本 + 空间
~~~

> **DP 最难的部分通常不是写循环或缓存，而是证明“为什么这个状态包含了足够的信息，以及为什么这些转移足以覆盖所有合法解”。**
