# OA 中的字符串解析（C++）

> 目标：面向 Online Assessment / 面试中的工程型字符串解析题，重点掌握：
>
> - `std::getline` 读取整行
> - Scanner / Tokenizer
> - `std::string_view` 避免不必要拷贝
> - `std::from_chars` 严格解析数字
> - malformed input 校验
> - delimiter / state machine / stack / recursive parser
> - 交易记录、日志、版本号、IP、表达式等常见模型

---

## 1. 总体思路

字符串解析题可以统一理解为：

```text
Raw Input
    ↓
Read Line
    ↓
Tokenize / Scan
    ↓
Parse Each Field
    ↓
Validate
    ↓
Structured Data
    ↓
Algorithm / Business Logic
```

最重要的原则：

> **Scanner 负责确定 token 边界；具体字段的 parser 负责类型转换和合法性校验。**

不要把“找 token”和“判断 token 是否合法”混在一起。

---

# 2. 第一层：如何读取输入

## 2.1 `std::getline`：读取整行

例如输入：

```text
20270302 1.233 1000000 buy
```

推荐：

```cpp
std::string line;

while (std::getline(std::cin, line))
{
    // parse line
}
```

### 为什么推荐 getline？

因为它保留整条 record：

```text
20270302 1.233 1000000 buy
```

然后可以自己控制 parsing 和 malformed handling。

---

# 3. Whitespace-separated 输入

例如：

```text
20270302 1.233 1000000 buy
```

也可能是：

```text
20270302      1.233   1000000     buy
```

甚至包含 tab。

这里有两种常见方法。

---

## 3.1 简单场景：`istringstream >>`

```cpp
std::istringstream iss(line);

std::string date;
double price;
long long qty;
std::string side;

if (!(iss >> date >> price >> qty >> side))
{
    // malformed
}
```

`operator >>` 会自动跳过任意 whitespace：

- 空格
- 多个空格
- tab
- newline

### 检查 extra token

仅仅成功读出 4 个字段还不够。

例如：

```text
20270302 1.233 1000000 buy garbage
```

前四个字段仍然能成功解析。

所以需要：

```cpp
std::string extra;

if (iss >> extra)
{
    // malformed
}
```

---

# 4. 推荐方法：Scanner + string_view

如果 OA 明确要求 malformed handling，我更推荐手写 Scanner。

核心思想：

```text
skip whitespace
      ↓
找到 token begin
      ↓
扫描到下一个 whitespace
      ↓
返回 [begin, end)
```

## 4.1 Scanner 模板

```cpp
#include <cctype>
#include <optional>
#include <string_view>

std::optional<std::string_view>
nextToken(std::string_view s, size_t& i)
{
    // Skip whitespace
    while (i < s.size() &&
           std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }

    if (i == s.size())
        return std::nullopt;

    size_t begin = i;

    while (i < s.size() &&
           !std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }

    return s.substr(begin, i - begin);
}
```

优点：

- 支持任意数量 whitespace
- 支持 tab
- 不产生大量临时 `std::string`
- `string_view` 是 pointer + length
- malformed handling 清晰
- 很适合工程型 OA

---

# 5. 示例：解析 Trade Record

输入：

```text
20270302 1.233 1000000 buy
```

假设 schema：

```text
date      price   quantity   side
20270302  1.233   1000000    buy
```

结构体：

```cpp
struct Trade
{
    std::string date;
    double price;
    long long qty;
    std::string side;
};
```

---

# 6. 严格解析整数：from_chars

推荐：

```cpp
#include <charconv>

bool parseInt(std::string_view s, long long& value)
{
    auto [ptr, ec] =
        std::from_chars(
            s.data(),
            s.data() + s.size(),
            value);

    return ec == std::errc{} &&
           ptr == s.data() + s.size();
}
```

两个条件都重要：

```cpp
ec == std::errc{}
```

表示：

- 转换成功
- 没 overflow / invalid input

而：

```cpp
ptr == end
```

表示：

> 整个 token 必须全部被消费。

例如：

```text
1000000abc
```

不应该被当成合法的：

```text
1000000
```

---

# 7. 严格解析浮点数

现代 C++ 可以：

```cpp
bool parseDouble(std::string_view s, double& value)
{
    auto [ptr, ec] =
        std::from_chars(
            s.data(),
            s.data() + s.size(),
            value);

    return ec == std::errc{} &&
           ptr == s.data() + s.size();
}
```

注意：

> 浮点版 `from_chars` 是标准 C++17 功能，但较老的标准库实现历史上支持不完整。

如果 OA 环境较旧，可以使用：

```cpp
bool parseDouble(const std::string& s, double& value)
{
    try
    {
        size_t pos = 0;
        value = std::stod(s, &pos);

        return pos == s.size();
    }
    catch (...)
    {
        return false;
    }
}
```

重点仍然是：

```cpp
pos == s.size()
```

否则：

```text
1.233abc
```

可能被部分解析成：

```text
1.233
```

---

# 8. 字段自己的 Validation

## 8.1 Date

简单格式校验：

```cpp
bool validDateToken(std::string_view s)
{
    if (s.size() != 8)
        return false;

    for (char c : s)
    {
        if (!std::isdigit(
                static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    return true;
}
```

这只能证明格式是：

```text
YYYYMMDD
```

但：

```text
20270230
```

仍然会通过。

因此需要区分：

### Syntax Validation

```text
是不是 8 个数字？
```

### Semantic Validation

```text
这个日期真的存在吗？
```

OA 是否需要检查真实日期，要看 specification。

---

## 8.2 Side

```cpp
bool validSide(std::string_view s)
{
    return s == "buy" || s == "sell";
}
```

如果题目说明 case-insensitive，则可以先 normalize。

不要自己假设。

---

# 9. 完整 Trade Parser

```cpp
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

struct Trade
{
    std::string date;
    double price;
    long long qty;
    std::string side;
};

std::optional<std::string_view>
nextToken(std::string_view s, size_t& i)
{
    while (i < s.size() &&
           std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }

    if (i == s.size())
        return std::nullopt;

    size_t begin = i;

    while (i < s.size() &&
           !std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }

    return s.substr(begin, i - begin);
}

bool parseLongLong(std::string_view s, long long& value)
{
    auto [ptr, ec] =
        std::from_chars(
            s.data(),
            s.data() + s.size(),
            value);

    return ec == std::errc{} &&
           ptr == s.data() + s.size();
}

bool parseDouble(std::string_view s, double& value)
{
    auto [ptr, ec] =
        std::from_chars(
            s.data(),
            s.data() + s.size(),
            value);

    return ec == std::errc{} &&
           ptr == s.data() + s.size();
}

bool validDateToken(std::string_view s)
{
    if (s.size() != 8)
        return false;

    for (char c : s)
    {
        if (!std::isdigit(
                static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    return true;
}

std::optional<Trade>
parseTrade(std::string_view line)
{
    size_t i = 0;

    auto date  = nextToken(line, i);
    auto price = nextToken(line, i);
    auto qty   = nextToken(line, i);
    auto side  = nextToken(line, i);

    // Missing field
    if (!date || !price || !qty || !side)
        return std::nullopt;

    // Extra field
    if (nextToken(line, i))
        return std::nullopt;

    Trade t;

    // date
    if (!validDateToken(*date))
        return std::nullopt;

    t.date = std::string(*date);

    // price
    if (!parseDouble(*price, t.price))
        return std::nullopt;

    if (t.price <= 0)
        return std::nullopt;

    // quantity
    if (!parseLongLong(*qty, t.qty))
        return std::nullopt;

    if (t.qty <= 0)
        return std::nullopt;

    // side
    if (*side != "buy" && *side != "sell")
        return std::nullopt;

    t.side = std::string(*side);

    return t;
}
```

---

# 10. Malformed Input Checklist

对于：

```text
20270302 1.233 1000000 buy
```

建议至少检查：

## 10.1 Missing Token

```text
20270302 1.233 buy
```

---

## 10.2 Extra Token

```text
20270302 1.233 1000000 buy garbage
```

---

## 10.3 Invalid Integer

```text
20270302 1.233 100x buy
```

---

## 10.4 Integer Overflow

例如 quantity 超过 `long long` 范围。

`from_chars` 会通过 `ec` 报告错误。

---

## 10.5 Invalid Double

```text
20270302 abc 1000000 buy
```

---

## 10.6 Partially Valid Number

```text
20270302 1.233abc 1000000 buy
```

必须确保：

```cpp
ptr == end
```

---

## 10.7 Invalid Enum / String

```text
20270302 1.233 1000000 hold
```

如果只允许：

```text
buy
sell
```

则必须 reject。

---

## 10.8 Business Constraint

例如：

```text
20270302 -1.233 1000000 buy
```

语法上是合法 double。

但如果 price 必须 > 0，则业务上非法。

---

# 11. Parsing Validation vs Semantic Validation

非常重要：

## Parsing Validation

判断：

> 输入能不能按照 schema 解析？

例如：

```text
20270230 1.233 1000000 buy
```

date 确实是 8 个数字。

---

## Semantic Validation

判断：

> 解析出来的数据在业务上是否合法？

例如：

```text
20270230
```

不是一个真实日期。

又如：

```text
price = -1.233
quantity = 0
```

类型上合法，但业务规则可能不允许。

---

# 12. getline(ss, token, delimiter)

对于固定 delimiter 很方便。

例如：

```text
20270302,1.233,1000000,buy
```

可以：

```cpp
std::stringstream ss(line);
std::string token;

while (std::getline(ss, token, ','))
{
    // process token
}
```

适合：

- CSV 的简单版本
- `|`
- `;`
- `/`
- 其他单字符 delimiter

---

# 13. 为什么不推荐 getline(..., ' ') 处理 whitespace？

例如：

```text
20270302    1.233
```

如果：

```cpp
std::getline(ss, token, ' ');
```

连续空格会产生 empty token。

并且 tab：

```text
\t
```

不会被 `' '` 捕获。

所以：

## Whitespace-separated

推荐：

```text
scanner
or
istringstream >>
```

## Fixed delimiter

推荐：

```text
getline(ss, token, delimiter)
```

---

# 14. 字符串解析的层级

可以把 OA 字符串 parsing 分成以下几类。

## Level 1：固定 whitespace

```text
20270302 1.233 1000000 buy
```

方法：

```text
getline + scanner
```

---

## Level 2：固定 delimiter

```text
20270302,1.233,1000000,buy
```

方法：

```text
getline(ss, token, ',')
```

---

## Level 3：不同 Token 类型

例如：

```text
abc123def456
```

Scanner：

```cpp
while (i < n)
{
    if (std::isalpha(s[i]))
    {
        // parse word
    }
    else if (std::isdigit(s[i]))
    {
        // parse number
    }
    else
    {
        ++i;
    }
}
```

这已经是简单 lexer/tokenizer。

---

# 15. State Machine

有些 delimiter 是否有效取决于当前状态。

例如 CSV：

```text
Jason,43,"Singapore, SG",Engineer
```

`"Singapore, SG"` 内部的逗号不能当 delimiter。

可以维护：

```cpp
bool inQuote = false;
```

核心：

```cpp
for (char c : s)
{
    if (c == '"')
    {
        inQuote = !inQuote;
    }
    else if (c == ',' && !inQuote)
    {
        // finish field
    }
    else
    {
        // append
    }
}
```

关键思想：

> delimiter 是否有效取决于当前 parser state。

---

# 16. Stack / Nested Parsing

例如：

```text
3[a2[c]]
```

不能简单 split。

通常用：

- stack
- recursion

典型题：

- LC 394 Decode String
- LC 385 Mini Parser

---

# 17. Recursive Parser

经典模式：

```cpp
parse(s, i)
```

其中：

```cpp
int& i
```

作为共享 parser cursor。

这样避免不断使用：

```cpp
substr()
```

递归解析 nested structure。

---

# 18. Expression Parsing

例如：

```text
3 + 2 * 5
```

进一步需要 operator precedence。

经典 grammar：

```text
expr
    = term { ('+' | '-') term }

term
    = factor { ('*' | '/') factor }

factor
    = number
    | '(' expr ')'
```

这已经进入真正 parser 的范畴。

---

# 19. Path Parsing

例如：

```text
/home/user/../tmp/./file
```

通常：

```text
"/" split

"."   → ignore
".."  → pop
name  → push
```

本质：

```text
Parsing + Stack
```

典型题：

- LC 71 Simplify Path

---

# 20. Version Parsing

例如：

```text
1.01.002
```

可以解析成：

```text
1
1
2
```

典型题：

- LC 165 Compare Version Numbers

可以使用 delimiter：

```text
.
```

也可以用 scanner 直接逐段解析整数。

---

# 21. IP Validation

典型：

- LC 468 Validate IP Address

例如：

```text
172.16.254.1
256.256.256.256
01.1.1.1
1..1.1
1.1.1.1.
```

重点检查：

- token 数量
- empty token
- 合法字符
- 数字范围
- leading zero
- trailing delimiter
- IPv4 / IPv6 不同 schema

这类题和工程 OA 的 malformed record parsing 非常接近。

---

# 22. Log / Trading Data Parsing

交易系统 OA 很容易出现：

```text
2026-10-02 08:30:01|BUY|AAPL|100|255.30
```

解析成：

```cpp
struct Trade
{
    std::string timestamp;
    std::string side;
    std::string symbol;
    int qty;
    double price;
};
```

之后题目可能要求：

- 按 symbol aggregate
- 计算 VWAP
- position
- PnL
- timestamp 排序
- deduplication
- 找最大 exposure

建议始终：

```text
String
   ↓
Parse
   ↓
Structured Data
   ↓
Algorithm
```

不要让 parsing logic 和核心算法混在一起。

---

# 23. C++ OA 常用字符串 API

建议熟悉：

```cpp
s.size()
s.empty()

s[i]

s.substr(pos)
s.substr(pos, len)

s.find(x)
s.rfind(x)

std::isdigit(c)
std::isalpha(c)
std::isalnum(c)
std::isspace(c)

std::stoi()
std::stoll()
std::stod()

std::to_string()

std::stringstream
std::getline()

std::from_chars()
```

以及：

```cpp
std::string::npos
```

---

# 24. cctype 的一个 C++ 坑

严谨写法：

```cpp
std::isdigit(static_cast<unsigned char>(c))
```

而不是：

```cpp
std::isdigit(c)
```

原因：

`char` 可能是 signed，负值传入 `isdigit/isspace/isalpha` 等函数可能导致未定义行为。

OA 输入明确是 ASCII 时通常不会踩到，但工程代码建议养成正确习惯。

---

# 25. OA 推荐决策树

看到字符串解析题，先判断：

```text
String Parsing
│
├── whitespace separated?
│      ├── simple
│      │      └── istringstream >>
│      └── strict malformed handling
│             └── scanner + string_view
│
├── fixed delimiter?
│      └── getline(ss, token, delim)
│
├── token type depends on character?
│      └── scanner / lexer
│
├── delimiter depends on context?
│      └── state machine
│
├── nested structure?
│      └── stack / recursion
│
└── operator precedence?
       └── expression parser / grammar
```

---

# 26. 推荐的能力升级路线

```text
split
  ↓
two pointers / scanner
  ↓
state machine
  ↓
stack
  ↓
recursive parser
  ↓
expression grammar
```

对于普通 OA：

> Scanner + Validation 是最重要的一层。

---

# 27. LeetCode 推荐

## 第一组：最值得先做

### LC 8 — String to Integer (atoi)

重点：

- skip spaces
- sign
- parse digit
- malformed suffix
- overflow
- scanner cursor

这是数字 scanner 的基础题。

---

### LC 165 — Compare Version Numbers

重点：

- delimiter
- leading zero
- parse integer segment
- scanner / split

---

### LC 468 — Validate IP Address

非常推荐。

重点：

- strict validation
- token 数量
- empty token
- illegal character
- leading zero
- numeric range
- malformed handling

和工程型 record parser 思维最接近。

---

### LC 71 — Simplify Path

重点：

- delimiter
- empty token
- `.`
- `..`
- stack

---

# 28. 第二组：Token Classification / Stack

### LC 150 — Evaluate Reverse Polish Notation

重点：

```text
token is number?
token is operator?
```

注意负数：

```text
-11
```

不能简单依赖：

```cpp
isdigit(token[0])
```

---

### LC 394 — Decode String

例如：

```text
3[a2[c]]
```

重点：

- number
- bracket
- nesting
- stack
- recursive parser

---

# 29. 第三组：真正 Parser

### LC 227 — Basic Calculator II

例如：

```text
3 + 2 * 5
```

重点：

- whitespace
- multi-digit number
- operator
- precedence

---

### LC 224 — Basic Calculator

重点：

- parentheses
- nested expressions
- recursion / stack

---

### LC 385 — Mini Parser

例如：

```text
[123,[456,[789]]]
```

重点：

- number
- comma
- brackets
- nested structure
- recursive parser

---

### LC 726 — Number of Atoms

例如：

```text
K4(ON(SO3)2)2
```

综合训练：

- token type
- number
- atom name
- parentheses
- nesting
- recursive parser / stack

---

# 30. 推荐刷题顺序

如果目标是 OA / trading firm：

```text
LC 8    String to Integer
   ↓
LC 165  Compare Version Numbers
   ↓
LC 468  Validate IP Address
   ↓
LC 71   Simplify Path
   ↓
LC 150  Evaluate RPN
   ↓
LC 394  Decode String
   ↓
LC 227  Basic Calculator II
   ↓
LC 385  Mini Parser
   ↓
LC 224  Basic Calculator
   ↓
LC 726  Number of Atoms
```

其中最优先：

```text
8
468
165
71
```

这几题最接近典型 OA parsing / malformed handling。

---

# 31. 最终记忆模板

对于：

```text
20270302 1.233 1000000 buy
```

脑子里直接建立：

```text
getline(cin, line)
        ↓
scanner
        ↓
string_view tokens
        ↓
field-specific parser
        ↓
from_chars
        ↓
full-consumption check
        ↓
semantic validation
        ↓
check missing / extra token
        ↓
Trade
```

严格 malformed validation checklist：

```text
1. token 数量正确
2. 每个 token 类型正确
3. numeric token 必须完整消费
4. 检查 overflow
5. enum/string value 合法
6. 检查业务约束
7. 检查 missing token
8. 检查 extra token
```

最重要的两个容易漏的点：

```text
"123abc" != 123

"valid fields + garbage"
也应该被认为 malformed
```

---

# 32. 面试时的一句话总结

> I usually separate tokenization from field parsing: first scan the input into token boundaries, preferably using string_view to avoid copies, then parse numeric fields with from_chars and require full token consumption, and finally apply schema and semantic validation such as field count, enum values, ranges, and extra-token checks.
