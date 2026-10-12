# C++ Integer Promotion、Usual Arithmetic Conversions、Signed/Unsigned 与溢出

> 面试专题（C++11–C++23，以 Linux x86-64 / LP64 为常见示例）。重点区分**表达式的计算类型**、**转换的目标类型**、**算术溢出**与**转换后的取模结果**。

## 1. 一页速记

1. 小整数（`bool`、`char`、`short` 等）参与内置算术时通常先 **integer promotion** 到 `int`。
2. 对两个不同整数类型的内置操作数，**usual arithmetic conversions** 决定共同计算类型；先 promotion，再依据 signedness、rank、可表示范围选择。
3. **`auto` 推导表达式的结果类型**，赋值目标类型不会逆向改变右侧表达式的中间计算类型。
4. **signed arithmetic overflow 是 UB**；**unsigned arithmetic 按模 `2^N` 运算**，定义良好，但可能造成逻辑 bug。
5. **integral conversion ≠ arithmetic overflow**。C++20 起，转换到有符号目标整数时，超出目标范围的结果由模 `2^N` 同余规则确定；C++17 及以前此类转换结果为 implementation-defined。
6. 对 signed/unsigned 混合比较，尤其注意 `size_t`、`-1`、`size()-1`、索引递减。

---

## 2. Integer Promotion（整型提升）

适用于若干整数类型（例如 `bool`、`char`、`signed/unsigned char`、`short`、`unsigned short`；枚举和位字段另有细节）。

对这些小整数类型：
- 如果 `int` 能表示其全部值，提升为 `int`；
- 否则提升为 `unsigned int`（常见普通小整数场景）。
- **注意：先提升，然后执行运算。**

```cpp
#include <cstdint>
#include <type_traits>

std::uint8_t a = 200, b = 100;
auto sum = a + b; // 通常 int，值 300
static_assert(std::is_same_v<decltype(sum), int>);

std::uint8_t out = a + b; // 300 modulo 256 = 44
```

### 容易忽略的例子：bitwise NOT

```cpp
std::uint8_t x = 0;
auto y = ~x;                            // 通常 int，值 -1
auto z = static_cast<std::uint8_t>(~x); // 255
```

### 16-bit 相乘可能发生 signed UB

```cpp
std::uint16_t a = 60000, b = 60000;
auto bad = a * b;                 // 常见 32-bit int 平台：int * int，UB
auto good = std::uint64_t{a} * b; // 3,600,000,000
```

即便声明 `uint64_t result = a * b;`，也无法修复右侧已经发生的溢出。

---

## 3. Usual Arithmetic Conversions（通常算术转换）

对于很多内置二元算术、位运算、比较（如 `+`、`-`、`*`、`/`、`%`、`&`、`<`、`==`），在整数操作数之间确定共同类型。**移位 `<<` / `>>` 是例外：分别整型提升，不套用二元共同类型规则。**

简化流程（限整数）：

1. 先做 integral promotion；
2. 相同类型：用该类型；
3. 同为 signed 或同为 unsigned：用 integer conversion rank 更高的类型；
4. 一 signed（`S`）一 unsigned（`U`）：
   - 若 `rank(U) >= rank(S)`，共同类型为 `U`；
   - 否则若 `S` 能表示 `U` 的所有值，共同类型为 `S`；
   - 否则共同类型为与 `S` 对应的 unsigned 类型。

### Integer Conversion Rank

一般标准整数类型的 rank 顺序（严格递增）：

```text
bool < char < short < int < long < long long
```

这里的 `char` 代表与 `signed char`、`unsigned char` 对应的等级；同宽 signed/unsigned 对应类型 rank 相同。扩展整数类型有额外规则。

**Rank ≠ 位宽。** 在 Linux x86-64 LP64 上，`long` 与 `long long` 通常都为 64 位，**但 `rank(long long) > rank(long)`**。

| 运算（常见 LP64） | 共同类型 | 原因 |
|---|---|---|
| `short + short` | `int` | promotion |
| `uint8_t + uint8_t` | `int` | promotion |
| `int + unsigned int` | `unsigned int` | 同 rank，U 胜 |
| `int + long` | `long` | 同为 signed，long rank 更高 |
| `long + unsigned int` | `long` | 64-bit signed long 可覆盖全部 32-bit unsigned |
| `long long + unsigned long` | `unsigned long long` | LP64 下 signed long long 无法覆盖全部 unsigned long |
| `long + long long` | `long long` | 即便同为 64-bit，rank 仍不同 |

### 有符号/无符号比较陷阱

```cpp
int a = -1;
unsigned int b = 1;
bool r = a < b; // false，常见 32-bit unsigned：a 转为 UINT_MAX
```

**不能简单说“混合 signed/unsigned 必转 unsigned”**：

```cpp
long a = -1;
unsigned int b = 5;
auto c = a + b; // LP64: long，c == 4
```

### C++20 安全混合比较

```cpp
#include <utility>
int i = -1;
std::vector<int> v(10);

bool wrong = i < v.size();              // 常见 64-bit 平台：false
bool right = std::cmp_less(i, v.size()); // true
```

---

## 4. Signed / Unsigned Overflow

### Signed arithmetic overflow：Undefined Behavior

```cpp
int x = INT_MAX;
++x;                    // UB
int y = INT_MIN;
--y;                    // UB
int z = INT_MIN / -1;   // UB，商不可表示
int w = INT_MIN % -1;   // UB
```

编译器可以假设正确程序不产生 signed overflow；**不能依赖“两补码回绕”作为 C++ signed overflow 的结果**：

```cpp
bool f(int x) { return x + 1 > x; }
// 对一切定义良好的执行恒为 true；x == INT_MAX 时表达式 UB。
```

### Unsigned arithmetic：Modulo `2^N`

```cpp
unsigned a = 3, b = 5;
auto diff = a - b;
// 若 unsigned 为 32 位：4294967294 == (3 - 5) modulo 2^32

unsigned x = 0;
--x; // UINT_MAX，定义良好
```

8-bit 示意：`3 - 5 = -2`，`-2 mod 256 = 254`；32-bit 为 `4294967294`。

**虽然不是 UB，但这种回绕常是边界、长度、索引、权限判断的严重 bug**：

```cpp
if (a - b < 0) { /* 永远不会成立：无符号表达式 */ }
if (a < b) { /* 正确比较 */ }
```

要得到有符号差值，**先转换操作数**，且确保更宽 signed 类型可表示输入及差值：

```cpp
unsigned a = 3, b = 5; // 常见 unsigned int 32 位
auto diff = static_cast<std::int64_t>(a) -
            static_cast<std::int64_t>(b); // -2

std::int64_t bad = a - b; // 右侧 unsigned 减法先发生，已经回绕
```

不能无条件把所有 `uint64_t` 减法转为 `int64_t`：`int64_t` 不一定容纳全部输入/差值。

---

## 5. Integral Conversion（整数类型转换）及版本区别

将整数转换到目标类型，与在整数类型内执行算术是**两个不同动作**。

### 转到 unsigned：模 `2^N`

```cpp
int x = -1;
unsigned y = x; // 若 32 位：UINT_MAX

int n = 300;
std::uint8_t z = static_cast<std::uint8_t>(n); // 44
```

### 转到 signed：C++20 前后

```cpp
std::uint32_t u = 0xFFFFFFFFu;
std::int32_t s = static_cast<std::int32_t>(u);
```

- C++17 及以前：`u` 无法由目标类型表示，结果 **implementation-defined**（主流平台通常为 -1）。
- C++20 起：得到目标类型中与原值模 `2^N` 同余的唯一值，此处 `s == -1`，定义良好。
- **这绝不意味着 signed arithmetic overflow 从 C++20 开始合法。**

以 `int8_t` 为目标的 C++20 示例：

| 输入 | 对应 `int8_t` 值 |
|---:|---:|
| 127 | 127 |
| 128 | -128 |
| 255 | -1 |
| 256 | 0 |
| 300 | 44 |

转换丢失信息时不可逆：

```cpp
std::int32_t x = 1000;
std::int8_t y = static_cast<std::int8_t>(x); // C++20: -24
std::int32_t z = y;                          // -24，不是 1000
```

### `{}` 列表初始化禁止常见 narrowing

```cpp
int n = 300;
std::uint8_t a = n;   // 允许，结果 44
std::uint8_t b(n);    // 允许，结果 44
// std::uint8_t c{n}; // 编译错误：非恒定表达式的窄化转换
```

`static_cast` 明确表达转换意图，但**不会自动验证范围**。

---

## 6. 中间表达式类型与目标类型不同

```cpp
std::uint64_t bad = 100000 * 100000;       // int * int 先溢出，UB
std::uint64_t ok = 100000ULL * 100000;     // uint64/ULL 级别运算

double x = 5 / 2;   // 2.0：先做整数除法
double y = 5.0 / 2; // 2.5：浮点除法

std::uint8_t a = 200, b = 100;
auto c = a + b; // 通常 int，300；auto 不会沿用 uint8_t
```

---

## 7. 移位、除法、余数、条件运算符

### 移位

`<<` 和 `>>` 对左右操作数分别做 integral promotion；结果类型是**提升后左操作数**类型。

```cpp
std::uint8_t x = 1;
auto y = x << 8; // 通常 int，256
```

移位量必须在 `[0, 左操作数提升后的位宽)`，否则 UB。

```cpp
std::uint64_t mask = std::uint64_t{1} << bit; // 仍须保证 bit < 64
```

处理掩码优先选 unsigned；对 signed 左移的可表示性/历史版本差异不要凭经验猜测。

### 整数除法向零截断

```cpp
int a = -7, b = 3;
a / b; // -2
a % b; // -1
```

当结果可表示且除数非零，满足 `(a / b) * b + (a % b) == a`。

### 条件表达式也有共同类型规则

```cpp
int x = -1;
unsigned y = 1;
auto z = true ? x : y; // unsigned；即使选中 x，仍转换为 unsigned
```

---

## 8. `size_t`、容器与逆向索引陷阱

```cpp
std::vector<int> v;

auto last = v.size() - 1; // 空时回绕为 SIZE_MAX，定义良好但逻辑错误
```

正确：

```cpp
if (!v.empty()) {
    auto last = v.size() - 1;
}
```

**错误反向循环**：

```cpp
for (std::size_t i = v.size() - 1; i >= 0; --i) {
    process(v[i]); // i >= 0 对 unsigned 恒真
}
```

正确：

```cpp
for (std::size_t i = v.size(); i-- > 0;) {
    process(v[i]);
}
// 无需索引时优先 rbegin()/rend()
```

### 二分中点

```cpp
int mid = (l + r) / 2;    // 可能 signed overflow
int mid = l + (r - l)/2;  // 在 l<=r 且差值可表示时更安全
// C++20: std::midpoint(l, r)  // <numeric>
```

---

## 9. 安全溢出检查

对 `int` 加法，可在执行前检查：

```cpp
#include <climits>

bool add_checked(int a, int b, int& result) {
    if (b > 0 && a > INT_MAX - b) return false;
    if (b < 0 && a < INT_MIN - b) return false;
    result = a + b;
    return true;
}
```

GCC/Clang 可用 `__builtin_add_overflow` 等扩展辅助，但不是标准 C++ 接口。

如果数据范围由业务约束能严格证明，尽量在输入边界做合法性检查；在需要的地方用 checked arithmetic。

---

## 10. long vs long long，以及 rank 与位宽

标准规定 `long long` 的 rank 高于 `long`；即使大小相同也不是同一类型。

| 常见数据模型 | int | long | long long |
|---|---:|---:|---:|
| Linux x86-64 LP64 | 32-bit | 64-bit | 64-bit |
| Windows x64 LLP64 | 32-bit | 32-bit | 64-bit |
| 常见 32-bit ILP32 | 32-bit | 32-bit | 64-bit |

```cpp
static_assert(!std::is_same_v<long, long long>);

long a = 10;
long long b = 20;
auto c = a + b; // long long，即使两者在 LP64 都为 64 位
```

`std::int64_t` 是固定宽度整数类型别名，具体可别名为 `long` 或 `long long`（平台相关）。协议字段、定宽数量和时间戳适合选固定宽度类型。

---

## 11. OA / Senior C++ / HFT 实践建议

### OA
- `int * int` 可能溢出：`1LL * a * b`（并证明 long long 足够）。
- 下标通常可用 `int`（确保长度可表示），与 `size_t` 比较要留意符号转换。
- 数组/容器为空时避免 `size()-1`。
- 二分不直接写 `(l+r)/2`。
- 对负数的除法与取模要按“向零截断”理解。

### Production / HFT
- Price Ticks、Quantity、Notional：分别规定取值范围和**中间乘积最大范围**。
- Sequence Number 回绕：明确使用 unsigned 取模，以及业务层的序列顺序比较语义。
- Buffer Length、Offset、Packet Length：即便 unsigned 运算合法，越界仍可能导致严重问题。
- 不要在 signed overflow 或超范围隐式转换上隐含依赖编译器行为。
- 用 `-Wall -Wextra -Wconversion -Wsign-conversion` 发现潜在转换；UBSan 可辅助查部分 signed overflow，不能替代逻辑分析。

示例：

```cpp
struct Order {
    std::int32_t priceTicks;
    std::uint32_t quantity;
};

// 错误风险：int32 + uint32 的 usual arithmetic conversions 可能导致 unsigned 乘法。
auto notional_bad = order.priceTicks * order.quantity;

// 当业务范围保证乘积可由 int64_t 表示：
std::int64_t notional =
    std::int64_t{order.priceTicks} * order.quantity;
```

---

## 12. 面试自测题

1. `uint8_t a=200,b=100; auto c=a+b;` 的类型和值？**常见平台：int, 300**。
2. `int a=-1; unsigned b=1; a<b`？**false**。
3. `unsigned a=3,b=5; auto d=a-b;`？**32-bit unsigned 为 4294967294**。
4. `uint64_t x=100000*100000;`？**右侧 int 乘法已 UB**。
5. `uint8_t x=0; auto y=~x;`？**常见平台：int, -1**。
6. `uint32_t u=UINT32_MAX; int32_t s=(int32_t)u;`？**C++20：-1；C++17：implementation-defined**。
7. `long + unsigned int`？**LP64：long；其他数据模型须重新判断**。
8. `long long + unsigned long`？**LP64：unsigned long long**。
9. `vector.empty(); v.size()-1`？**unsigned wrap 到 SIZE_MAX**。
10. `a/b`、`a%b` 对负数？**整数除法向零截断，余数按相应恒等式确定**。

---

## 13. 面试英文总结

> Integer promotions convert small integer types to int or unsigned int before arithmetic. The usual arithmetic conversions determine a common type based on signedness, integer conversion rank, and representable ranges. Signed arithmetic overflow is undefined behavior, whereas unsigned arithmetic wraps modulo 2 to the power of its width. Integral conversion is a separate operation: since C++20, an out-of-range conversion to a signed integer type has a modulo-defined result. In production code, I pay particular attention to mixed signed/unsigned comparisons, intermediate expression types, and overflow in sizes, offsets and monetary calculations.

## 参考

- [cppreference: Implicit conversions](https://en.cppreference.com/w/cpp/language/implicit_conversion)
- [cppreference: Usual arithmetic conversions](https://en.cppreference.com/w/cpp/language/usual_arithmetic_conversions)
- [cppreference: Arithmetic operators](https://en.cppreference.com/w/cpp/language/operator_arithmetic)
- [cppreference: Fixed-width integer types](https://en.cppreference.com/w/cpp/types/integer)


---

## 14. 进阶练习：15 道单选题（C++20 / Linux x86-64 / LP64）

> 假定 `int` 32-bit、`long` 与 `long long` 64-bit、`unsigned` 32-bit、`size_t` 64-bit、8-bit byte。先独立完成，再看答案解析。

### 第 1 题

```cpp
unsigned short a = 60000;
unsigned short b = 60000;
auto x = a + b;
```

**x 的类型和值？**

A. unsigned short，54464

B. int，120000

C. unsigned int，120000

D. Undefined Behavior

### 第 2 题

```cpp
unsigned short a = 60000;
unsigned short b = 60000;
auto x = a * b;
```

**发生什么？**

A. unsigned wraparound

B. long long，3600000000

C. Signed overflow，UB

D. unsigned int，3600000000

### 第 3 题

```cpp
int a = -1;
unsigned int b = 2;
auto x = a + b;
```

**x 的类型和值？**

A. int，1

B. unsigned int，1

C. unsigned int，UINT_MAX

D. Undefined Behavior

### 第 4 题

```cpp
long a = -1;
unsigned int b = 2;
auto x = a + b;
```

**x 的类型和值？**

A. unsigned long，1

B. unsigned int，1

C. long，1

D. long long，1

### 第 5 题

```cpp
long long a = -3;
unsigned long b = 2;
auto x = a + b;
```

**x 的类型和值？**

A. long long，-1

B. unsigned long，UINT64_MAX

C. unsigned long long，UINT64_MAX

D. Undefined Behavior

### 第 6 题

```cpp
uint8_t x = 255;
auto y = x + 1;
```

**y 的类型和值？**

A. uint8_t，0

B. int，256

C. unsigned int，256

D. Undefined Behavior

### 第 7 题

```cpp
uint8_t x = 255;
++x;
```

**x 的值？**

A. 256

B. 255

C. 0

D. Undefined Behavior

### 第 8 题

```cpp
int x = INT_MAX;
auto y = x + 1u;
```

**y 的类型和值？**

A. int，UB

B. unsigned int，2147483648

C. long long，2147483648

D. Undefined Behavior

### 第 9 题

```cpp
int x = INT_MIN;
auto y = -x;
```

**发生什么？**

A. y 为 INT_MIN

B. y 为 unsigned int

C. Signed overflow，UB

D. 编译失败

### 第 10 题

```cpp
unsigned a = 3;
unsigned b = 5;
int64_t x = a - b;
```

**x 的值？**

A. -2

B. 4294967294

C. Undefined Behavior

D. Implementation-defined

### 第 11 题

```cpp
int a = -1;
size_t b = 10;
bool x = a < b;
```

**x 的值？**

A. true

B. false

C. Undefined Behavior

D. 由优化决定

### 第 12 题

```cpp
uint32_t x = UINT32_MAX;
int32_t y = static_cast<int32_t>(x);
```

**C++20 下 y 的值？**

A. -1

B. Implementation-defined

C. Undefined Behavior

D. 2147483647

### 第 13 题

```cpp
uint8_t a = 0;
auto x = ~a;
```

**x 的类型和值？**

A. uint8_t，255

B. unsigned int，UINT_MAX

C. int，-1

D. Undefined Behavior

### 第 14 题

```cpp
int a = -1;
unsigned b = 5;
auto x = true ? a : b;
```

**x 的类型和值？**

A. int，-1

B. unsigned int，UINT_MAX

C. unsigned int，5

D. Undefined Behavior

### 第 15 题

```cpp
uint64_t x = 1;
auto y = x << 64;
```

**C++20 下行为？**

A. y == 0

B. y == 1

C. Unsigned wraparound，定义良好

D. Undefined Behavior

### 答案与解析

1. **B — int，120000**：unsigned short 的所有值可被 32-bit int 表示，所以操作数先提升为 int。120000 在 int 范围内。

2. **C — Signed overflow，UB**：两个 unsigned short 先提升为 int；乘积为 3,600,000,000，超出 INT_MAX，因此 UB。

3. **B — unsigned int，1**：int 与 unsigned int 同 rank，采用 unsigned int；-1 变成 UINT_MAX，之后模 2^32 加 2 得到 1。

4. **C — long，1**：LP64 下 long 是 signed 64-bit，可以表示 unsigned int 的全部值，因此共同类型为 long。

5. **C — unsigned long long，UINT64_MAX**：LP64 下 long long 不能覆盖 unsigned long 的全部范围，因此共同类型是 unsigned long long；(-3 + 2) modulo 2^64 为 UINT64_MAX。

6. **B — int，256**：uint8_t 提升至 int 后做加法，结果为 int 256。

7. **C — 0**：最终赋回 uint8_t，值按模 256 转换，回绕到 0；不同于 auto y=x+1。

8. **B — unsigned int，2147483648**：1u 使共同类型变为 unsigned int。INT_MAX+1u 得到 2147483648u，未触发 signed overflow。

9. **C — Signed overflow，UB**：-INT_MIN 超出 int 可表示范围。一元负号在 int 中运算，导致 UB。

10. **B — 4294967294**：unsigned 减法先按模 2^32 计算得到 4294967294，再无损转成 64-bit signed。

11. **B — false**：LP64 中 size_t 是 64-bit unsigned，负 int 转为很大的 unsigned 值，因此比较为 false。

12. **A — -1**：C++20 对超范围整数转换到 signed 的结果采用模 2^N 同余规则，故为 -1。

13. **C — int，-1**：uint8_t 先提升为 int，再对 0 逐位取反，得到 int -1。

14. **B — unsigned int，UINT_MAX**：条件表达式的第二、三操作数确定共同类型 unsigned int；实际选择的 -1 也转为 UINT_MAX。

15. **D — Undefined Behavior**：移位量必须小于提升后左操作数宽度。64-bit 值左移 64 位是 UB，即使是 unsigned。

### 解题方法

按 **Integer Promotion → Common Type → Arithmetic → Conversion to Destination** 的顺序追踪类型和值；不要先入为主地用赋值目标类型推断中间运算。
