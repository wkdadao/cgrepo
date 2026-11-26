# 区间 DP（Interval Dynamic Programming）系统总结

> C++17 / 算法面试复习笔记。重点：**状态语义、左右边界、决策点（分割/第一次/最后一次）、遍历顺序、复杂度推导、Memo 与 Bottom-up 转换**。

## 1. 什么是区间 DP？

区间 DP 将连续的一段序列视为一个子问题，典型状态：

\[
dp[l][r] = \text{区间 } [l,r] \text{ 上问题的最优值/方案数}
\]

与其他 DP 的区别：

| 类型 | 常见状态 | 子问题 |
|---|---|---|
| 背包 DP | dp[i][capacity] | 物品前缀 + 资源 |
| 树形 DP | dp[u] | 以 u 为根的子树 |
| 区间 DP | dp[l][r] | 连续子区间 |

**先证明决策以后子区间独立，才能套区间 DP。** 并非所有“操作数组”的问题都能直接拆成左右两段。

### 1.1 三类最常见决策

1. **两端收缩**：选择左端、右端，或者比较二者。代表 LC 516。
2. **分割点**：枚举 k，将 [l,r] 拆为 [l,k] 与 [k+1,r]。代表 Matrix Chain Multiplication。
3. **第一步 / 最后一步**：正向依赖复杂时，枚举一个能固定区间边界关系的操作。代表 LC 1547（第一刀）、LC 312（最后一个气球）。

| 类型 | 常见转移 | 单状态代价 | 常见总时间 |
|---|---|---|---|
| 两端收缩 | dp[l+1][r] / dp[l][r-1] / dp[l+1][r-1] | O(1) | O(n²) |
| 区间分割 | 枚举 k: dp[l][k] + dp[k+1][r] + cost | O(n) | O(n³) |
| 最先/最后操作 | 枚举 k 作为第一次/最后一次操作 | O(n) | O(n³) |

## 2. 边界定义：闭区间与开区间不能混用

### 2.1 闭区间 [l,r]

- 区间包含 l、r。
- 单元素区间 l==r 常作为 base case。
- 分成 **[l,k] + [k+1,r]**。
- 长度为 r-l+1。

常见于最长回文子序列、矩阵连乘、一般石子合并。

### 2.2 开区间 (l,r)

- l 与 r 是保留的边界，决策只处理内部元素。
- 内部没有元素时 r==l+1，代价通常为 0。
- 以 k 为最后操作时，分成 **(l,k) + (k,r)**。
- 区间内部元素数量为 r-l-1。

常见于 LC 312；LC 1547 使用切割位置数组的端点作为边界，也很适合这种状态设计。

**面试检查项：明确 dp[l][r] 是包含边界还是保留边界，再写循环和 base case。**

## 3. Bottom-up 通用模板

### 3.1 按长度递增：最通用

对于闭区间分割，若每个非平凡状态能由更短区间计算：

~~~cpp
// dp[l][r] = [l, r] 的最优值
vector<vector<long long>> dp(n, vector<long long>(n, 0));

// dp[i][i] 视题目设置：例如单个矩阵连乘代价为 0
for (int len = 2; len <= n; ++len) {
    for (int l = 0; l + len <= n; ++l) {
        int r = l + len - 1;

        long long best = INF; // 最大化则用 -INF
        for (int k = l; k < r; ++k) {
            long long candidate =
                dp[l][k] + dp[k + 1][r] + mergeCost(l, k, r);
            best = min(best, candidate);
        }
        dp[l][r] = best;
    }
}
~~~

依赖的两个子区间长度都 **小于 len**，因此保证已计算完成。

> 此模板展示结构，不是可直接编译的函数：INF 和 mergeCost 由具体题目定义。需要根据问题重新设置初始化/循环界限。

### 3.2 左端点倒序、右端点正序

适用于转移依赖 dp[l+1][r]、dp[l][r-1] 等的题目：

~~~cpp
for (int l = n - 1; l >= 0; --l) {
    dp[l][l] = base(l);
    for (int r = l + 1; r < n; ++r) {
        // dp[l+1][r] 在更早的 l 行；
        // dp[l][r-1] 在本行较小 r 列
        dp[l][r] = transition(l, r);
    }
}
~~~

这里 base 和 transition 同样是需按题意提供的占位符。关键不在循环长相，而是**所有被依赖状态必须先于当前状态计算**。

## 4. 例一：LC 516 — Longest Palindromic Subsequence

题目：https://leetcode.com/problems/longest-palindromic-subsequence/

### 4.1 状态和转移

dp[l][r]：字符串 s 的闭区间 [l,r] 中最长回文**子序列**长度；子序列不要求连续。

\[
dp[l][r] =
\begin{cases}
1, & l=r\\
dp[l+1][r-1]+2, & s[l]=s[r]\\
\max(dp[l+1][r],dp[l][r-1]), & s[l]\ne s[r]
\end{cases}
\]

其中空区间取 0。对于长度为 2、两端相等的情况，中间空区间的结果为 0。

### 4.2 C++17 完整代码

~~~cpp
class Solution {
public:
    int longestPalindromeSubseq(string s) {
        const int n = static_cast<int>(s.size());
        if (n == 0) return 0;

        vector<vector<int>> dp(n, vector<int>(n, 0));

        for (int l = n - 1; l >= 0; --l) {
            dp[l][l] = 1;
            for (int r = l + 1; r < n; ++r) {
                if (s[l] == s[r]) {
                    dp[l][r] = 2 + (r - l >= 2 ? dp[l + 1][r - 1] : 0);
                } else {
                    dp[l][r] = max(dp[l + 1][r], dp[l][r - 1]);
                }
            }
        }

        return dp[0][n - 1];
    }
};
~~~

复杂度：**时间 Θ(n²)、辅助空间 Θ(n²)**。每个区间 O(1) 工作。

## 5. 例二：Matrix Chain Multiplication（矩阵连乘）

假设矩阵 A_i 的维度为 p[i] × p[i+1]，一共有 n 个矩阵、n+1 个维度值。

结合律保证结果不变，但不同加括号顺序的标量乘法成本不同。例如：

- A = 10×30，B = 30×5，C = 5×60
- (AB)C 的乘法次数：10×30×5 + 10×5×60 = **4500**
- A(BC) 的乘法次数：30×5×60 + 10×30×60 = **27000**

### 5.1 状态与转移

dp[l][r]：计算矩阵 A_l · ... · A_r 的最小乘法次数。

最后一步一定将 [l,r] 在 k 处分为两段：

\[
dp[l][r]=\min_{l\le k<r}\left(
dp[l][k]+dp[k+1][r]+p[l]p[k+1]p[r+1]
\right)
\]

Base：dp[i][i] = 0（单个矩阵不需要乘法）。

### 5.2 C++17 完整代码

~~~cpp
long long matrixChain(const vector<int>& p) {
    const int n = static_cast<int>(p.size()) - 1;
    if (n <= 1) return 0;

    constexpr long long INF = LLONG_MAX / 4;
    vector<vector<long long>> dp(n, vector<long long>(n, 0));

    for (int len = 2; len <= n; ++len) {
        for (int l = 0; l + len <= n; ++l) {
            int r = l + len - 1;
            dp[l][r] = INF;

            for (int k = l; k < r; ++k) {
                long long cost =
                    dp[l][k] + dp[k + 1][r]
                    + 1LL * p[l] * p[k + 1] * p[r + 1];
                dp[l][r] = min(dp[l][r], cost);
            }
        }
    }
    return dp[0][n - 1];
}
~~~

假定 p 中维度均为正，且乘积与总成本不会超出 long long 范围。

复杂度：**时间 Θ(n³)、辅助空间 Θ(n²)**。

## 6. 例三：LC 1547 — Minimum Cost to Cut a Stick（枚举第一刀）

题目：https://leetcode.com/problems/minimum-cost-to-cut-a-stick/

长度为 n 的木棍，有若干必须切开的坐标；每切一刀的成本是**当下那根木棍的长度**。要求最小成本。

将切点排序，并在两端添加 0、n：

\[
c = [0,\ \text{sorted cuts},\ n]
\]

定义 dp[l][r]：将保留边界 c[l]、c[r] 之间需要的刀全部切完的最小成本。

区间内第一刀选在 k，那么这刀成本必为 c[r]-c[l]，其余两段独立：

\[
dp[l][r]=
\min_{l<k<r}\left(
dp[l][k]+dp[k][r]+c[r]-c[l]
\right)
\]

Base：dp[l][l+1] = 0（内部没有切点）。

### C++17 完整代码

~~~cpp
class Solution {
public:
    int minCost(int n, vector<int>& cuts) {
        vector<int> c = cuts;
        c.push_back(0);
        c.push_back(n);
        sort(c.begin(), c.end());

        int m = static_cast<int>(c.size());
        vector<vector<int>> dp(m, vector<int>(m, 0));

        for (int len = 2; len < m; ++len) {
            for (int l = 0; l + len < m; ++l) {
                int r = l + len;
                dp[l][r] = INT_MAX;

                for (int k = l + 1; k < r; ++k) {
                    dp[l][r] = min(
                        dp[l][r],
                        dp[l][k] + dp[k][r] + c[r] - c[l]
                    );
                }
            }
        }
        return dp[0][m - 1];
    }
};
~~~

若原切点数为 m0，则时间 **O(m0³)**，空间 **O(m0²)**（额外边界仅改变常数项）。

注意：**切第一刀**才能固定当前木棍长度，并把左右切割成本独立化。

## 7. 例四：LC 312 — Burst Balloons（枚举最后一个）

题目：https://leetcode.com/problems/burst-balloons/

每次戳破位置 k 的气球，获得 **当下** 左右邻居数值与当前数值之积；操作后邻接关系会改变。

### 7.1 为什么不能直接枚举第一个戳破的气球？

如果最先戳破 k，删除后两边会重新相邻，左右剩余区间通常不再相互独立。直接用“左区间最优 + 右区间最优”会丢失跨区间的邻接关系。

**改为枚举区间里最后被戳破的气球 k。**

在 k 被戳破的最后一刻，区间边界 l、r 仍保留，所以局部得分确定为 a[l] × a[k] × a[r]。

添加虚拟边界值 1：

\[
a=[1,\ nums[0], \ldots, nums[n-1],\ 1]
\]

定义 dp[l][r]：**保留两个边界气球 l、r，只戳破开区间 (l,r) 内所有气球**所得最大分数。

\[
dp[l][r] =
\max_{l<k<r}\left(
dp[l][k]+dp[k][r]+a[l]a[k]a[r]
\right)
\]

Base：dp[l][l+1]=0。

### 7.2 C++17 Bottom-up

~~~cpp
class Solution {
public:
    int maxCoins(vector<int>& nums) {
        const int n = static_cast<int>(nums.size());
        vector<int> a(n + 2, 1);
        for (int i = 0; i < n; ++i) a[i + 1] = nums[i];

        vector<vector<long long>> dp(
            n + 2, vector<long long>(n + 2, 0));

        // len = 右边界与左边界的下标差
        for (int len = 2; len <= n + 1; ++len) {
            for (int l = 0; l + len < n + 2; ++l) {
                int r = l + len;

                for (int k = l + 1; k < r; ++k) {
                    dp[l][r] = max(
                        dp[l][r],
                        dp[l][k] + dp[k][r]
                            + 1LL * a[l] * a[k] * a[r]
                    );
                }
            }
        }
        return static_cast<int>(dp[0][n + 1]);
    }
};
~~~

本实现按 LC 312 的非负数值约束初始化为 0；若推广到允许负贡献的问题，不能照搬这个初值。

复杂度：**时间 O(n³)、辅助空间 O(n²)**。

### 7.3 C++17 Memoization

~~~cpp
class Solution {
public:
    int maxCoins(vector<int>& nums) {
        const int n = static_cast<int>(nums.size());
        vector<int> a(n + 2, 1);
        for (int i = 0; i < n; ++i) a[i + 1] = nums[i];

        vector<vector<long long>> memo(
            n + 2, vector<long long>(n + 2, -1));

        auto dfs = [&](auto&& self, int l, int r) -> long long {
            if (r - l <= 1) return 0;

            long long& ans = memo[l][r];
            if (ans != -1) return ans;

            ans = 0;
            for (int k = l + 1; k < r; ++k) {
                ans = max(
                    ans,
                    self(self, l, k) + self(self, k, r)
                        + 1LL * a[l] * a[k] * a[r]
                );
            }
            return ans;
        };

        return static_cast<int>(dfs(dfs, 0, n + 1));
    }
};
~~~

- 不同状态至多 O(n²)。
- 每状态枚举 O(n) 个 k。
- **时间 O(n³)，Memo 数组 O(n²)，最大递归深度 O(n)**。
- 这里 -1 仅因有效得分非负才能充当未计算标记；一般情况应有独立 visited 标记。

## 8. 例五：LC 1039 — Minimum Score Triangulation of Polygon

题目：https://leetcode.com/problems/minimum-score-triangulation-of-polygon/

将凸多边形切分成三角形，三角形成本为三个顶点权重的乘积，最小化总成本。

定义 dp[l][r]：顶点 l..r 对应多边形链与边 (l,r) 构成部分的最小三角剖分成本。

枚举与边 (l,r) 构成三角形的第三顶点 k：

\[
dp[l][r] =
\min_{l<k<r}\left(
dp[l][k]+dp[k][r]+a[l]a[k]a[r]
\right)
\]

Base：r-l < 2 时没有三角形，代价为 0。

~~~cpp
class Solution {
public:
    int minScoreTriangulation(vector<int>& values) {
        int n = static_cast<int>(values.size());
        vector<vector<int>> dp(n, vector<int>(n, 0));

        for (int len = 3; len <= n; ++len) {
            for (int l = 0; l + len <= n; ++l) {
                int r = l + len - 1;
                dp[l][r] = INT_MAX;
                for (int k = l + 1; k < r; ++k) {
                    dp[l][r] = min(
                        dp[l][r],
                        dp[l][k] + dp[k][r]
                            + values[l] * values[k] * values[r]
                    );
                }
            }
        }
        return dp[0][n - 1];
    }
};
~~~

复杂度：时间 O(n³)，空间 O(n²)。乘积范围以原题约束为准。

**与 LC 312 结构相近，但题意不同**：LC 1039 是用三角形拆分；LC 312 是保留区间边界，逆向选择最后一个气球。

## 9. 例六：LC 1000 — Minimum Cost to Merge Stones（带堆数约束）

题目：https://leetcode.com/problems/minimum-cost-to-merge-stones/

每次**必须合并连续 K 堆**，合并成本是这些堆的重量之和；要求最终变成一堆的最小成本。

### 9.1 可行性先判断

一次合并把堆数减少 K-1，因此：

\[
(n-1)\bmod(K-1)=0
\]

是最终能合并为一堆的必要条件（本题规则下也是充分条件）。若不满足，返回 -1。

### 9.2 两种状态建模

完整三维状态：

\[
dp[l][r][p] = \text{区间 [l,r] 恰好合并成 p 堆的最小成本}
\]

也可以利用“每次堆数减少 K-1”的性质压缩成二维状态：

- dp[l][r] 表示把区间 [l,r] 合并到**规则允许的最少堆数**所需的最小成本。
- 枚举 k 将区间分成两段，利用 k 每次增加 K-1 的结构。
- 若 (len-1)%(K-1)==0，说明该区间能够合并到 1 堆，需再加整段重量。

### 9.3 C++17 二维实现

~~~cpp
class Solution {
public:
    int mergeStones(vector<int>& stones, int K) {
        int n = static_cast<int>(stones.size());
        if ((n - 1) % (K - 1) != 0) return -1;

        vector<int> prefix(n + 1, 0);
        for (int i = 0; i < n; ++i) {
            prefix[i + 1] = prefix[i] + stones[i];
        }

        constexpr int INF = 1e9;
        vector<vector<int>> dp(n, vector<int>(n, 0));

        for (int len = 2; len <= n; ++len) {
            for (int l = 0; l + len <= n; ++l) {
                int r = l + len - 1;
                dp[l][r] = INF;

                for (int k = l; k < r; k += K - 1) {
                    dp[l][r] = min(
                        dp[l][r],
                        dp[l][k] + dp[k + 1][r]
                    );
                }

                if ((len - 1) % (K - 1) == 0) {
                    dp[l][r] += prefix[r + 1] - prefix[l];
                }
            }
        }
        return dp[0][n - 1];
    }
};
~~~

假设 K>=2、stones 均为正且数值符合 LC 1000 约束。

复杂度保守上界：**O(n³)** 时间、**O(n²)** 空间。使用步长 K-1 会减少实际枚举的分割点数。

## 10. Memoization 与 Bottom-up 的统一复杂度分析

区间 DP 最常见的状态是 (l,r)。

对于 n 个元素，合法闭区间数量：

\[
\sum_{\mathrm{len}=1}^{n}(n-\mathrm{len}+1)
=\frac{n(n+1)}2=\Theta(n^2)
\]

如果每个区间需要枚举至多 O(n) 个 k：

\[
T(n)=O(n^2)\times O(n)=O(n^3)
\]

**Memo 特别注意：** 统计的是 *不同状态数量*，不是未经缓存的递归树调用量。

- Memoization：只计算实际访问到的区间；若用完整二维 vector 预分配缓存，空间仍是 O(n²)。
- Bottom-up：通常填满所有合法区间，顺序必须满足依赖。
- Memoization 额外有递归栈，区间缩小型递归最大深度通常 O(n)。
- 如果状态中还有其它维度（例如合并后堆数 p），状态数和复杂度必须相应增加。

## 11. 高频错误与验证清单

| 错误 | 后果 | 正确检查 |
|---|---|---|
| 混用 [l,r] 与 (l,r) | 转移越界、遗漏元素、重复计算 | 明确定义是否处理边界 |
| 用第一个操作划分，但左右仍会相互影响 | 最优子结构不成立 | 尝试改成最后一个操作，或反过来 |
| 认为每题都需要枚举 k | 不必要的 O(n³) | 两端收缩型可能 O(n²) |
| 计算顺序不满足依赖 | 使用尚未计算的 dp | 先证依赖长度严格减小 |
| 最大化/最小化初值错误 | 将不可达状态当有效答案 | 0、+INF、-INF 依据语义选择 |
| 将空区间和单元素区间混淆 | Base Case 错误 | 写明 dp[l][l] / dp[l][l+1] |
| 只看两层循环便断言 O(n²) | 漏掉内部枚举 k | 状态数量 × 每状态转移成本 |
| 使用 -1 充当 Memo 哨兵但 -1 可能是合法结果 | 缓存失效、重复计算 | 用 visited 数组 |
| 使用 int 累加大量成本未检查范围 | 整数溢出 | 必要时用 long long |
| 一味使用递归 | 深度 O(n) 时栈风险 | 大 n 可用 Bottom-up |

### 面试五步法

1. **State**：dp[l][r] 精确含义、边界是否包含？
2. **Decision**：收缩端点、选择 k、第一次还是最后一次操作？
3. **Independence**：选择以后两段真的独立吗？是否跨边界相互影响？
4. **Dependency order**：base case 和按长度递增的填表顺序能否保证依赖已算好？
5. **Complexity**：合法状态数 × 转移枚举代价；单独说明缓存和递归栈。

## 12. 练习顺序与推荐对比

| 顺序 | 题目 | 重点 | 难度 |
|---|---|---|---|
| 1 | [LC 516 Longest Palindromic Subsequence](https://leetcode.com/problems/longest-palindromic-subsequence/) | 两端收缩 | Medium |
| 2 | [LC 1039 Minimum Score Triangulation](https://leetcode.com/problems/minimum-score-triangulation-of-polygon/) | 分割点 / 三角形 | Medium |
| 3 | [LC 1547 Minimum Cost to Cut a Stick](https://leetcode.com/problems/minimum-cost-to-cut-a-stick/) | **第一刀** | Hard |
| 4 | [LC 312 Burst Balloons](https://leetcode.com/problems/burst-balloons/) | **最后一个气球** | Hard |
| 5 | [LC 1000 Minimum Cost to Merge Stones](https://leetcode.com/problems/minimum-cost-to-merge-stones/) | 合并 K 堆约束 | Hard |

重点对比：

- **LC 1547 vs LC 312**：为什么前者枚举*第一步*，后者枚举*最后一步*？核心是哪个选择能让左右子问题独立。
- **LC 1039 vs LC 312**：两者形式上都出现 dp[l][k]+dp[k][r]+三个点乘积，但状态语义与优化方向不同。
- **LC 516 vs Matrix Chain**：同为二维区间状态，前者每状态 O(1)，后者每状态 O(n)。
- **Memo vs Bottom-up**：同一状态转移，只是求值方式不同。

---

## 一页速记

| 模式 | 状态 | 转移抓手 | 常见遍历 |
|---|---|---|---|
| 两端收缩 | [l,r] | dp[l+1][r]、dp[l][r-1] | l 倒序、r 正序 |
| 分割 | [l,r] | 枚举 k，合并 [l,k] 与 [k+1,r] | len 递增 |
| 边界保留 | (l,r) | 枚举 k，合并 (l,k) 与 (k,r) | 边界距离递增 |
| 复杂区间合并 | [l,r] + 堆数/约束 | 合并数量、合法性、前缀和 | len 递增 |

> 最重要的不是背公式，而是**找到一种决策顺序，使原问题能拆为互不干扰的连续子区间**。能把这个理由讲清楚，区间 DP 的状态与公式通常就水到渠成。
