# DRW 俄罗斯方块：Simplified Tetris Engine

> 根据公开流传的四页题面整理为中文练习版，保留其标准输入／输出形式。未核实它是否与 DRW 当前面试使用的版本相同。

## 题目

编写一个简化的俄罗斯方块程序。棋盘宽度为 **10** 格，列号从左到右为 **0～9**。依次投放给定的方块，输出所有方块落定并完成消行后的棋盘高度。

每行输入是一局独立游戏，开始时棋盘为空。

## 方块

每个方块由四个单位格组成，是不可拆分的刚体。方块始终保持下表中的方向，**不旋转**。

图中 🟦 为方块占据的格子，⬜ 为空格；每行图形从左向右阅读，多行图形从上向下阅读。

| 字母 | 固定形状 |
| --- | --- |
| `Q` | 🟦🟦<br>🟦🟦 |
| `Z` | 🟦🟦⬜<br>⬜🟦🟦 |
| `S` | ⬜🟦🟦<br>🟦🟦⬜ |
| `T` | 🟦🟦🟦<br>⬜🟦⬜ |
| `I` | 🟦🟦🟦🟦 |
| `L` | 🟦⬜<br>🟦⬜<br>🟦🟦 |
| `J` | ⬜🟦<br>⬜🟦<br>🟦🟦 |

## 游戏规则

1. 按输入顺序逐个投放方块。每个方块从棋盘上方进入，保持指定的水平位置，竖直下落。
2. 方块落到底部，或继续下落将与已有格子重叠时，整个方块停止。任何两个格子不能占据同一位置。
3. 每个方块落定后，所有被完全填满的行消失。
4. 消行后，上方的行整体向下补位，行内图案保持不变。已有格子不会单独下落去填补下方的空洞。
5. 最终高度是从棋盘底部到最高已占用格子顶部的距离，包含中间空洞；棋盘为空时高度为 `0`。

## 输入格式

从标准输入读取，直到 EOF。每行是一个以逗号分隔的投放序列，每个条目由以下两部分直接连接：

- 一个方块字母：`Q`、`Z`、`S`、`T`、`I`、`L` 或 `J`。
- 一个一位数字，表示该方块占据的**最左侧列号**。

例如，`Q0` 表示把 `Q` 方块投放到最左侧列为 `0` 的位置。

输入保证合法，所有方块均在棋盘宽度内，不需要处理非法输入。题面给出的高度上限为 `100`，不要求实现超高判负。

## 输出格式

对每行输入，向标准输出打印一行整数，表示该局游戏的最终高度。

不得输出标签、解释文字或棋盘图形。

## 样例

以下四行分别是四局独立游戏。

### 输入

```text
Q0
I0,I4,Q8
T1,Z3,I4
Q0,I2,I6,I0,I6,I6,Q2,Q4
```

### 输出

```text
2
1
4
3
```

## 题面来源

- 公开题面扫描：[第 1 页](https://www.fastprep.io/api/problem-source-images/drw-simplified-tetris-engine/0)、[第 2 页](https://www.fastprep.io/api/problem-source-images/drw-simplified-tetris-engine/1)、[第 3 页](https://www.fastprep.io/api/problem-source-images/drw-simplified-tetris-engine/2)、[第 4 页](https://www.fastprep.io/api/problem-source-images/drw-simplified-tetris-engine/3)。
- [同一份四页题面的公开文本版本](https://www.scribd.com/document/823741121/TetrisProgrammingExercise)。
- [Glassdoor 候选人记录](https://www.glassdoor.com/Interview/Build-a-variant-of-tetris-where-after-a-clearing-a-line-shapes-don-t-fall-into-empty-spaces-below-unless-an-entire-line-i-QTN_7445638.htm)与 [Hacker News 候选人记录](https://news.ycombinator.com/item?id=26629891)可佐证 DRW 曾使用此类题目，不能确认当前版本。
