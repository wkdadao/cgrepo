# C++ 模板与编译期编程完整总结

> 覆盖 C++11/14/17/20/23；重点关注高级 C++、生产级泛型库、Low-Latency / HFT 工程及面试。  
> 整理日期：2026-10-09。示例主要基于 C++20；使用 C++23 功能时会明确标记。

## 目录

1. [统一知识框架](#1-统一知识框架)
2. [模板基本形式及参数](#2-模板基本形式及参数)
3. [模板参数推导规则](#3-模板参数推导规则)
4. [index_sequence 与 tuple_has 完整解析](#4-index_sequence-与-tuple_has-完整解析)
5. [CTAD：类模板实参推导](#5-ctad类模板实参推导)
6. [特化、重载、实例化与名称查找](#6-特化重载实例化与名称查找)
7. [Variadic Templates 与 Fold Expression](#7-variadic-templates-与-fold-expression)
8. [Type Traits 与 TMP](#8-type-traits-与-tmp)
9. [SFINAE、enable_if 与检测惯用法](#9-sfinaeenable_if-与检测惯用法)
10. [Concepts 与 requires](#10-concepts-与-requires)
11. [constexpr、consteval、constinit、if constexpr](#11-constexprconstevalconstinitif-constexpr)
12. [Concept Interface 与 Virtual Interface](#12-concept-interface-与-virtual-interface)
13. [CRTP、Policy-based Design 与低延迟工程](#13-crtppolicy-based-design-与低延迟工程)
14. [编译期协议描述案例](#14-编译期协议描述案例)
15. [性能、编译成本、陷阱与面试练习](#15-性能编译成本陷阱与面试练习)
16. [参考资料](#16-参考资料)

---

## 1. 统一知识框架

### 1.1 模板是代码参数化机制，不等于编译期执行

模板在编译期接受**类型、值或其他模板**作为参数，完成推导、约束检查、特化选择和实例化。它允许创建类型安全的泛型实现，且具体类型信息能帮助优化器生成专门化代码。

`constexpr` 是常量求值机制，**不属于模板独有能力**。二者经常组合，但不能画等号。

| 要回答的问题 | 机制 | 关键点 |
|---|---|---|
| 参数是什么？ | Type / Non-type / Template-template parameter | 类型、编译期值、模板 |
| 模板参数如何确定？ | Template Argument Deduction / CTAD | 从形参模式与实参类型匹配 |
| 哪个实现被选中？ | Overload / Specialization / Constraints | 与重载决议和特化匹配有关 |
| 类型具备什么性质？ | Type Traits / Concepts | 类型信息、操作约束 |
| 如何处理可变长度参数？ | Parameter Pack / Pack Expansion / Fold | 展开与归约 |
| 如何编译期生成新类型？ | TMP / Partial Specialization | 类型级计算 |
| 能否调用这个模板？ | SFINAE / `requires` / Concepts | 候选可行性和约束 |
| 选哪条实现路径？ | `if constexpr` | 被丢弃分支的模板实例化规则 |
| 何时计算？ | `constexpr` / `consteval` | 允许 / 要求常量求值 |
| 如何保证静态初始化？ | `constinit` | 初始化时机，不代表不可修改 |

### 1.2 语言特性年代

- **C++11**：Variadic Templates、`decltype`、`constexpr`、`static_assert`、`enable_if`、`integer_sequence` 的前置机制（`integer_sequence` 本身为 C++14）、Alias Templates。
- **C++14**：`std::integer_sequence` / `std::index_sequence`、Variable Templates、泛型 Lambda、较强的 `constexpr` 循环能力。
- **C++17**：Fold Expressions、`if constexpr`、`std::void_t`、`std::is_*_v`、`decltype(auto)` 实际来自 C++14、CTAD。
- **C++20**：Concepts / `requires`、`consteval`、`constinit`、`std::remove_cvref_t`、显式模板参数列表 Lambda、更多常量求值能力。
- **C++23**：`if consteval`。

> 年代表只为快速定位，具体项目必须按实际编译标准和标准库支持确认。

---

## 2. 模板基本形式及参数

### 2.1 四类模板

```cpp
#include <type_traits>

// Function Template
template<class T>
T max_value(T a, T b) { return a < b ? b : a; }

// Class Template
template<class T>
struct Box { T value; };

// Alias Template (C++11)
template<class T>
using Ptr = T*;

// Variable Template (C++14)
template<class T>
constexpr bool is_pointer_type = std::is_pointer_v<T>; // 此表达式需 C++17

static_assert(is_pointer_type<int*>);
```

模板定义是泛型模式；例如 `max_value<int>` 是具体函数模板特化。编译器实际是否生成独立机器代码，取决于使用情况与后续优化。

### 2.2 三类模板参数

**Type Template Parameter**

```cpp
template<typename T>
struct Buffer { T value; };
```

在声明类型模板参数时，`class` 和 `typename` 等价。

**Non-type Template Parameter（NTTP）**

```cpp
#include <cstddef>
template<class T, std::size_t N>
struct FixedBuffer { T storage[N]; };

template<auto V> // C++17
struct Constant { static constexpr auto value = V; };

static_assert(Constant<42>::value == 42);
```

C++20 扩大 NTTP 可使用的类型，包括浮点数和满足**结构类型（structural type）**条件的类类型。并非任意对象都能当模板实参：必须满足相应常量表达式和类型规则。

**Template-template Parameter**

```cpp
template<class T>
struct Storage {};

template<class T, template<class> class C>
struct Holder { C<T> value; };

Holder<int, Storage> h;
```

### 2.3 默认模板参数

```cpp
template<class T = int, std::size_t N = 64>
struct RingConfig {};

RingConfig<> default_cfg;
RingConfig<double> custom_cfg;
```

函数模板、类模板的默认实参和推导规则并不完全相同；不要把函数模板的“部分显式指定再推导”套到类模板的 `X<A,B>` 上。

---

## 3. 模板参数推导规则

### 3.1 函数模板参数的三个来源

1. **显式提供**：`f<int>(...)`。
2. **从函数调用实参推导**：`f(10)` 得到 `T=int`。
3. **使用默认模板实参**：未推导且符合默认规则时使用默认值。

```cpp
template<class A, class B, class C>
void f(A, B, C);

f(1, 2.0, 'x');                 // A=int, B=double, C=char
f<int>(1, 2.0, 'x');            // A 显式；B、C 推导
f<int, double>(1, 2.0, 'x');    // C 推导
f<int, double, char>(1, 2.0, 'x');
```

函数模板可以**显式给出前缀模板实参**，让后面的参数从函数实参中推导。不能使用 `f<int, , char>(...)` 这种“跳过中间一个参数”的语法。

### 3.2 `T` / `T&` / `const T&` / `T&&`

```cpp
template<class T> void by_value(T);
template<class T> void by_ref(T&);
template<class T> void by_cref(const T&);
template<class T> void by_forward(T&&);  // 在这里是 forwarding reference
```

对于 `int x` 和 `const int cx`：

| 调用模式 | 实参 | 推导 T | 形参类型 |
|---|---|---|---|
| `by_value` | `x` | `int` | `int` |
| `by_value` | `cx` | `int` | `int` |
| `by_ref` | `cx` | `const int` | `const int&` |
| `by_cref` | `cx` | `int` | `const int&` |
| `by_forward` | `x` | `int&` | `int&` |
| `by_forward` | `cx` | `const int&` | `const int&` |
| `by_forward` | `10` | `int` | `int&&` |

**按值参数通常丢弃顶层 const、引用信息；引用参数保留更多类型信息。**

`T&&` 只有在 `T` 是需要推导的、适合该规则的类型模板参数时，才是 **Forwarding Reference**。`const T&&` 不是。

引用折叠：

| 原始组合 | 折叠结果 |
|---|---|
| `& &` | `&` |
| `& &&` | `&` |
| `&& &` | `&` |
| `&& &&` | `&&` |

```cpp
#include <utility>

template<class T>
void wrapper(T&& arg) {
    consume(std::forward<T>(arg));
}
```

`std::forward<T>` 根据所推导的 `T` 保留左/右值类别。

### 3.3 数组退化与长度推导

```cpp
#include <cstddef>

template<class T>
void by_value(T);

template<class T, std::size_t N>
void by_array_ref(T (&)[N]);

int a[10];
by_value(a);      // T = int*；数组退化
by_array_ref(a);  // T = int, N = 10
```

类似地，可以从类型 `std::array<T,N>` 推导 `T` 和 `N`，也可以从 `std::tuple<Ts...>` 推导整个类型参数包。

### 3.4 从复合类型反推模板参数

```cpp
#include <array>
#include <tuple>
#include <utility>

template<class T, std::size_t N>
void inspect_array(const std::array<T, N>&);

template<class... Ts>
void inspect_tuple(const std::tuple<Ts...>&);

template<std::size_t... I>
void inspect_indexes(std::index_sequence<I...>);

inspect_array(std::array<int, 3>{});      // T=int, N=3
inspect_tuple(std::tuple<int, double>{}); // Ts...=int,double
inspect_indexes(std::index_sequence<0,1,2>{}); // I...=0,1,2
```

核心是匹配**形参类型模式（P）**与**实参类型（A）**：

```text
P: std::index_sequence<I...>
A: std::index_sequence<0,1,2>

=> I... = 0,1,2
```

**一个函数实参对象的类型，可以编码任意多个编译期类型或整数参数。**

### 3.5 哪些参数不能自动推导？

```cpp
template<class T, class U>
void f(T);

// f(10);           // U 无法推导
f<int, double>(10); // OK
```

函数模板实参推导通常不依赖函数体，也不会仅根据接收调用结果的变量类型从普通返回类型“倒推”模板参数：

```cpp
template<class T>
T make_default() { return T{}; }

// int x = make_default(); // T 无法由目标类型推导
int x = make_default<int>();
```

**Non-deduced Context**：模板参数即使出现在形参类型中，也可能无法从该位置推导。

```cpp
#include <type_traits>

template<class T>
void use(typename T::value_type);

// use(10); // 无法反推 T

template<class T>
void equal_to(T a, std::type_identity_t<T> b);

equal_to(10, 20.5); // T 由 a 推导成 int；b 可转换为 int
```

相同 `T` 被两个位置分别推导为不同类型，通常发生推导冲突：

```cpp
template<class T>
void both(T, T);

both(1, 2);          // T=int
// both(1, 2.0);     // int 与 double 冲突
both<double>(1, 2.0); // 显式指定 T=double，可以转换第一个实参
```

### 3.6 `auto` 与 `decltype(auto)`

```cpp
int x = 10;

auto a = x;          // int
decltype(x) b = 10;  // int（名字表达式特殊规则）
decltype((x)) c = x; // int&（左值表达式）
```

```cpp
template<class T>
auto get_copy(T& x) { return (x); } // 返回 T（按值）

template<class T>
decltype(auto) get_ref(T& x) { return (x); } // 返回 T&
```

不要把 `decltype(x)` 和 `decltype((x))` 混为一谈。`decltype(auto)` 能保留引用，但也可能带来悬垂引用风险。

---

## 4. index_sequence 与 tuple_has 完整解析

### 4.1 原始代码

```cpp
#include <tuple>
#include <utility>
#include <type_traits>
#include <cstddef>

template<class MyTuple, class T, std::size_t... I>
consteval bool tuple_has_impl(std::index_sequence<I...>) {
    return (std::is_same_v<std::tuple_element_t<I, MyTuple>, T> || ...);
}

template<class MyTuple, class T>
consteval bool tuple_has() {
    return tuple_has_impl<MyTuple, T>(
        std::make_index_sequence<std::tuple_size_v<MyTuple>>{}
    );
}

using Tuple = std::tuple<int, double, char>;
static_assert(tuple_has<Tuple, double>());
static_assert(!tuple_has<Tuple, float>());
static_assert(!tuple_has<std::tuple<>, int>());
```

### 4.2 为什么只写 `tuple_has_impl<MyTuple,T>`？

因为 `MyTuple`、`T` 已显式提供，而函数实参：

```cpp
std::make_index_sequence<3>{}
```

具有等价于 `std::index_sequence<0,1,2>` 的类型。

函数形参模式：

```cpp
std::index_sequence<I...>
```

因此自动推导出：

```text
I... = 0, 1, 2

tuple_has_impl<Tuple, double, 0, 1, 2>(...)
```

折叠表达式相当于：

```cpp
std::is_same_v<int, double> ||
std::is_same_v<double, double> ||
std::is_same_v<char, double>
```

得到 `true`。

**I... 不是由函数体内的 `tuple_element_t<I,MyTuple>` 推导；推导发生在函数调用匹配形参类型时。**

### 4.3 `index_sequence` 的定义思想

```cpp
template<class T, T... Ints>
struct integer_sequence {};

template<std::size_t... I>
using index_sequence =
    integer_sequence<std::size_t, I...>;
```

`std::make_index_sequence<N>` 构造索引集合 `0,1,...,N-1` 的**类型**。它不需要在运行期循环生成索引。

### 4.4 C++20 可以用模板 Lambda 代替独立 impl

```cpp
template<class Tuple, class T>
consteval bool tuple_has_lambda() {
    return []<std::size_t... I>(std::index_sequence<I...>) {
        return (std::is_same_v<std::tuple_element_t<I, Tuple>, T> || ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}
```

### 4.5 如果只查找类型，可以直接匹配 `std::tuple<Ts...>`

```cpp
#include <tuple>
#include <type_traits>

template<class Tuple, class T>
struct tuple_contains;

template<class... Ts, class T>
struct tuple_contains<std::tuple<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts,T> || ...)> {};

template<class Tuple, class T>
inline constexpr bool tuple_contains_v = tuple_contains<Tuple,T>::value;

static_assert(tuple_contains_v<std::tuple<int,double>, double>);
```

这种做法无需索引序列；需要同时操作第 `I` 个元素的值或类型时，`index_sequence` 更有用。

---

## 5. CTAD：类模板实参推导

**CTAD = Class Template Argument Deduction**，C++17 引入。

### 5.1 基于构造函数推导完整类模板实参

```cpp
template<class T>
struct Box {
    explicit Box(T) {}
};

Box b(42); // CTAD: Box<int>
```

更贴近 `index_sequence` 的例子：

```cpp
#include <utility>
#include <type_traits>

template<std::size_t... I>
struct IndexHolder {
    explicit IndexHolder(std::index_sequence<I...>) {}
};

IndexHolder h(std::index_sequence<0,1,2>{});

static_assert(std::is_same_v<decltype(h), IndexHolder<0,1,2>>);
```

CTAD 会利用构造函数生成隐式推导指引，也可以显式定义 **Deduction Guide**。

### 5.2 函数模板与类模板的关键区别

```cpp
template<class T, class U, std::size_t... I>
void f(std::index_sequence<I...>);

f<int,double>(std::index_sequence<0,1>{}); // OK: I... 被推导
```

相反：

```cpp
template<class T, class U, std::size_t... I>
struct X {
    explicit X(std::index_sequence<I...>) {}
};

// X<int,double> x(std::index_sequence<0,1>{}); // 不可行
```

这里写出 `X<int,double>` **不是启动“部分 CTAD”**；类型已经固定为 `X<int,double>`，其尾部参数包 `I...` 是空包，构造函数期待 `std::index_sequence<>`。

**类模板 CTAD 推导的是没有写明模板实参列表时的完整特化；不支持“已显式写前两个类模板实参、其余由构造实参继续推导”的函数模板式用法。**

### 5.3 类模板偏特化也能做类型模式匹配

```cpp
template<class Seq>
struct SequenceInfo;

template<std::size_t... I>
struct SequenceInfo<std::index_sequence<I...>> {
    static constexpr std::size_t count = sizeof...(I);
    static constexpr std::size_t sum = (std::size_t{0} + ... + I);
};

using Info = SequenceInfo<std::index_sequence<1,2,3>>;
static_assert(Info::count == 3);
static_assert(Info::sum == 6);
```

这不是 CTAD，而是**类模板偏特化的模式匹配**：给定完整模板实参 `std::index_sequence<1,2,3>`，匹配 `std::index_sequence<I...>`。

> 补充：只有聚合成员而无构造函数的 `Pair p{1,2};` 使用聚合 CTAD 是 C++20 才支持的场景；C++17 CTAD 不等于所有聚合都能自动推导。

---

## 6. 特化、重载、实例化与名称查找

### 6.1 全特化与偏特化

```cpp
template<class T>
struct IsPointer : std::false_type {};

template<class T>
struct IsPointer<T*> : std::true_type {}; // 类模板偏特化

template<>
struct IsPointer<void> : std::false_type {}; // 示例：全特化

static_assert(IsPointer<int*>::value);
static_assert(!IsPointer<int>::value);
```

函数模板可全特化，但**不能偏特化**；函数模板通常用重载或 Concepts 处理类型类别：

```cpp
template<class T> void process(T);
template<class T> void process(T*);
```

一个重要陷阱：函数模板显式全特化本身不是普通的“新增重载候选”。当多组函数模板重载与显式特化混用时，需要先完成重载决议，再考虑所选模板的特化；不要简单认为“最具体的全特化一定优先”。

### 6.2 实例化与头文件

模板定义通常放在头文件，供需要隐式实例化的翻译单元使用。也可以显式实例化：

```cpp
// header
template<class T>
T twice(T x) { return x + x; }

extern template int twice<int>(int);

// one .cpp
template int twice<int>(int);
```

`extern template` / Explicit Instantiation 可以用于减少重复实例化编译成本，必须保证相应定义可链接。

### 6.3 Two-phase Name Lookup

- **非依赖名称（Non-dependent Name）**：通常在模板定义时查找。
- **依赖名称（Dependent Name）**：根据规则推迟到实例化相关阶段，并可能受 ADL 影响。

```cpp
template<class T>
void use_type() {
    typename T::value_type x{};
}

template<class T>
void call_member(T& obj) {
    obj.template run<int>();  // template 消歧
}
```

`typename` 和 `template` 在这些位置是**消歧关键字**，用于指导编译器解释依赖名称。

---

## 7. Variadic Templates 与 Fold Expression

### 7.1 参数包与展开

```cpp
template<class... Ts>
struct type_list {};

template<class... Args>
void forward_all(Args&&... args) {
    consume(std::forward<Args>(args)...);
}
```

- `class... Ts`：模板类型参数包。
- `Args&&... args`：函数参数包。
- `std::forward<Args>(args)...`：**Pack Expansion**，展开为逗号分隔的多个函数实参。

Pack Expansion 不等于 Fold Expression。

### 7.2 Fold Expression 四种形式（C++17）

| 形式 | 语法 | 展开（以减法为例） |
|---|---|---|
| Unary left fold | `(... - args)` | `((a-b)-c)` |
| Unary right fold | `(args - ...)` | `(a-(b-c))` |
| Binary left fold | `(0 - ... - args)` | `(((0-a)-b)-c)` |
| Binary right fold | `(args - ... - 0)` | `(a-(b-(c-0)))` |

```cpp
template<class... Args>
auto sum(Args... args) {
    return (0 + ... + args);
}

template<class... Ts>
constexpr bool all_integral = (std::is_integral_v<Ts> && ...);
```

空包时只有特定的一元 Fold 有预定义结果：`&&` 为 `true`、`||` 为 `false`、逗号为 `void()`；其它一元 Fold 不可对空包实例化。二元 Fold 有初始值，可处理空包。

### 7.3 顺序遍历与短路

```cpp
template<class F, class... Args>
void for_each(F&& f, Args&&... args) {
    (f(std::forward<Args>(args)), ...); // 逗号运算符，按从左到右的顺序求值
}

template<class F, class... Args>
bool all_until_false(F&& f, Args&&... args) {
    return (f(std::forward<Args>(args)) && ...); // && 短路
}
```

Fold 本身不能直接写 `break` / `continue`；可以利用 `&&` / `||` 短路控制后续表达式是否执行。Fold 处理**表达式层的归约**，不是直接生成新的类型参数包的“类型级 filter”。

---

## 8. Type Traits 与 TMP

### 8.1 Traits：类型信息与变换

| 类别 | 例子 | 输出 |
|---|---|---|
| Type Property | `std::is_integral_v<T>` | bool |
| Type Relationship | `std::is_same_v<T,U>` | bool |
| Type Transformation | `std::remove_cvref_t<T>` | 类型 |
| Operation Trait | `std::is_nothrow_move_constructible_v<T>` | bool |
| Custom Trait | `ProtocolTraits<T>` | 自定义类型 / 值 |

`std::integral_constant<T,V>` 是把编译期值封装到类型上的基础工具；`std::true_type` 和 `std::false_type` 是它的常见别名。

### 8.2 Type-level 编程

```cpp
template<class... Ts>
struct type_list {};

template<class T, class List>
struct push_front;

template<class T, class... Ts>
struct push_front<T, type_list<Ts...>> {
    using type = type_list<T, Ts...>;
};

template<class T, class List>
using push_front_t = typename push_front<T,List>::type;

using L = push_front_t<char, type_list<int,double>>;
static_assert(std::is_same_v<L, type_list<char,int,double>>);
```

常见类型级算法：`front`、`at`、`concat`、`push_back`、`transform/map`、`filter`、`find`、`contains`、`unique`、`type_fold`。

`type_list<Ts...>` 只编码类型序列，本身不存对象；`std::tuple<Ts...>` 还负责存储实际值，二者用途不同。

### 8.3 `std::conditional_t` 不一定具备需要的惰性

```cpp
template<class T>
using Bad = std::conditional_t<
    std::is_integral_v<T>,
    int,
    typename T::value_type
>;

// Bad<int> x; // 错误：形成第三个模板实参时 int::value_type 无效
```

`std::conditional_t` 只能在**两个模板类型实参都已合法形成**后选择其中之一。需要延迟实例化的类型级分支时，使用偏特化或其它惰性技术。

---

## 9. SFINAE、enable_if 与检测惯用法

### 9.1 SFINAE

**Substitution Failure Is Not An Error**：在适用的模板实参替换上下文内失败时，候选可被排除，而不一定立刻产生硬编译错误。

```cpp
template<class T>
std::enable_if_t<std::is_integral_v<T>, T>
half(T x) {
    return x / 2;
}
```

注意：函数体实例化的错误、某些实例化副作用等**不自动获得 SFINAE 保护**。

### 9.2 Detection Idiom

```cpp
#include <type_traits>
#include <utility>
#include <vector>

template<class T, class = void>
struct HasSize : std::false_type {};

template<class T>
struct HasSize<T, std::void_t<
    decltype(std::declval<T&>().size())
>> : std::true_type {};

static_assert(HasSize<std::vector<int>>::value);
static_assert(!HasSize<int>::value);
```

`decltype(std::declval<T&>().size())` 只检查表达式类型和有效性，不实际构造对象或调用 `size()`。

C++20 通常可写为：

```cpp
template<class T>
concept HasSizeConcept = requires(T& x) {
    x.size();
};
```

### 9.3 与 Concepts / if constexpr 的职责区别

| 技术 | 作用 |
|---|---|
| Traits | 提供类型信息或类型变换 |
| SFINAE / `enable_if` | 控制模板候选可行性 |
| `void_t` / Detection | 检测表达式是否合法 |
| `requires` / Concepts | 声明和复用模板约束 |
| `if constexpr` | 选择已经选中的函数或类模板内部代码路径 |

---

## 10. Concepts 与 requires

### 10.1 Concept 是命名编译期约束

```cpp
#include <concepts>

template<class T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::same_as<T>;
};

template<Addable T>
T add(T a, T b) { return a + b; }
```

Concept 可以组合：

```cpp
template<class T>
concept SmallIntegral =
    std::integral<T> && (sizeof(T) <= 4);
```

### 10.2 Concept 的使用位置

```cpp
template<std::integral T>
void a(T);

template<class T>
requires std::integral<T>
void b(T);

void c(std::integral auto);

template<class T>
struct Holder {
    void send() requires std::movable<T>;
};
```

还可用于受约束的 `auto` 返回类型、泛型 Lambda 参数和复合要求的返回约束。

### 10.3 两类 `requires`

**requires-clause**：

```cpp
template<class T>
requires std::integral<T>
void send(T);
```

**requires-expression**：

```cpp
template<class T>
concept HasSize = requires(T& x) {
    x.size();
};
```

二者可以合用：

```cpp
template<class T>
requires requires(T& x) { x.size(); }
void send(T&);
```

两个 `requires` 并不重复：前一个引入约束子句，后一个引入约束表达式。

### 10.4 requires-expression 的四种 Requirement

```cpp
template<class T>
concept ContainerLike = requires(T& x, const T& cx) {
    x.begin();                                             // Simple
    typename T::value_type;                                // Type
    { cx.size() } -> std::convertible_to<std::size_t>;      // Compound
    requires std::movable<T>;                              // Nested
};
```

| Requirement | 语法 | 检查 |
|---|---|---|
| Simple | `expr;` | 表达式合法 |
| Type | `typename T::type;` | 嵌套类型存在 |
| Compound | `{ expr } noexcept -> C;` | 表达式、是否不抛、结果类型约束 |
| Nested | `requires predicate;` | 编译期布尔约束 |

**细节**：

```cpp
template<class T>
concept Interface = requires(T& x) {
    { x.f1() } -> std::same_as<void>;
    { x.f2() }; // 仅检查合法性；不限制返回类型
};
```

如果 `f2()` 也必须返回 `void`，应写：

```cpp
{ x.f2() } -> std::same_as<void>;
```

`requires(T& x)` 只声明用于检查的局部参数，并不真正构造一个 `T`。

### 10.5 Constraint Subsumption（约束包含关系）

```cpp
template<class T>
concept HasSize = requires(T& x) { x.size(); };

template<class T>
concept HasSizeAndEmpty =
    HasSize<T> &&
    requires(T& x) { x.empty(); };

template<HasSize T>
int classify(const T&) { return 1; }

template<HasSizeAndEmpty T>
int classify(const T&) { return 2; }
```

对同时满足二者的类型，若其它排序条件相同，后者通常因约束更加严格而被优先选择。

但编译器不会任意进行数学蕴含证明：两个独立写出的 `sizeof(T)>4` 和 `sizeof(T)>0` 不会仅因数学逻辑就自动建立约束包含关系。推荐复用已命名 Concept 来构造约束层级。

### 10.6 Concept 不会执行函数

`requires(x.f1())`（准确语法见上例）检查语法与类型性质，不能验证函数是否真的执行正确业务逻辑，也不保证无内存分配、线程安全或固定延迟。

---

## 11. constexpr、consteval、constinit、if constexpr

### 11.1 `constexpr` 函数：允许常量求值

```cpp
constexpr int square(int x) { return x*x; }

constexpr int a = square(5); // 必须是常量表达式
int b = square(5);           // 不要求常量求值；可被优化
int n = 10;
int c = square(n);           // 运行期可执行
```

`constexpr` 变量则必须用适当的常量表达式初始化，并有相应 const 语义。

常量表达式需求包括 `static_assert`、NTTP、`std::array` 长度模板实参、`if constexpr` 条件等。

C++14 允许更多循环 / 局部变量变更参与常量求值：

```cpp
constexpr int factorial(int n) {
    int result = 1;
    for (int i=2; i<=n; ++i) result *= i;
    return result;
}
static_assert(factorial(5) == 120);
```

C++20 支持**受限制**的常量求值动态分配：

```cpp
constexpr int build() {
    int* p = new int(42);
    int result = *p;
    delete p;
    return result;
}
static_assert(build() == 42);
```

不代表动态分配可跨出常量求值保留为运行期堆对象；生命周期与释放规则仍然严格。

### 11.2 `consteval`：立即函数（C++20）

```cpp
consteval int compile_square(int x) { return x*x; }

constexpr int x = compile_square(5); // OK
int y = compile_square(5);           // OK；调用必须常量求值
int runtime = 10;
// int z = compile_square(runtime);  // 普通立即调用不合法
```

| 语义 | `constexpr` 函数 | `consteval` 函数 |
|---|---|---|
| 支持常量求值 | 是 | 是 |
| 可按运行期实参正常调用 | 是 | 通常不行 |
| 调用必须满足立即函数规则 | 否 | 是 |
| 用于模板 | 可以 | 可以 |
| 声明变量 | `constexpr` 可以 | `consteval` 不行 |

**常见陷阱：立即函数的形参不是 NTTP。**

```cpp
consteval int abs_ct(int x) {
    // static_assert(x >= 0); // 不合法：x 并非定义处可使用的常量表达式
    // if constexpr (x >= 0) ... // 同样不合法
    return x < 0 ? -x : x;
}
```

如果需要值直接作为编译期类型 / 分支条件：

```cpp
template<int N>
consteval int abs_nttp() {
    if constexpr (N < 0) return -N;
    else return N;
}
```

### 11.3 `if constexpr`：编译期分支（C++17）

```cpp
#include <type_traits>

template<class T>
auto process(T x) {
    if constexpr (std::is_integral_v<T>) {
        return x + 1;
    } else {
        return x.size();
    }
}
```

对于 `T=int`，不要求实例化不适合它的 `x.size()` 分支。被丢弃分支中的 `return` 不参与该实例的 `auto` 返回类型推导。

与普通 `if` 的区别是：普通 `if` 不会由于运行时条件为真，就免除另一分支在实例化时的合法性要求。

**边界**：`if constexpr` 不是预处理器。语法错误或某些非依赖且无效的代码不会凭空变合法；在非模板上下文也不能借它绕过语义检查。

### 11.4 `if constexpr` vs Concepts

```cpp
// 模板是否可调用：约束在接口上
template<std::integral T>
void constrained(T);

// 模板已被选中后，决定函数内部路径
template<class T>
void conditional(T x) {
    if constexpr (std::integral<T>) { /* A */ }
    else { /* B */ }
}
```

一句话：**Concepts / SFINAE 控制候选；`if constexpr` 控制所选实现内部实例化哪些分支。**

### 11.5 `std::is_constant_evaluated()`（C++20）与 `if consteval`（C++23）

```cpp
#include <type_traits>

constexpr int compute(int x) {
    if (std::is_constant_evaluated()) {
        return x*x;
    }
    return x*x;
}
```

陷阱：

```cpp
if constexpr (std::is_constant_evaluated()) {
    // 条件在必须常量求值的上下文求值，因此这里总是 true
}
```

C++23：

```cpp
consteval int fast_ct(int x) { return x*x; }

constexpr int dispatch(int x) {
    if consteval {
        return fast_ct(x);
    } else {
        return x*x;
    }
}
```

`if consteval` 依据是否在常量求值上下文中求值选择路径，其 true 分支具有支持立即函数调用的特殊规则；不要与 `if constexpr` 混淆。

### 11.6 `constinit`（C++20）

```cpp
constinit int counter = 100;
void tick() { ++counter; } // OK：不是 const
```

- 要求具有静态 / 线程存储期的变量满足静态初始化要求。
- **不表示不可修改**，不保证线程安全。
- 能帮助发现意外动态初始化，但不能自动解决所有全局生命周期或依赖问题。

### 11.7 选择总结

| 需求 | 推荐 |
|---|---|
| 查询类型是否支持操作 | `requires` expression |
| 约束公开模板接口 | Concept |
| 根据类型选择函数重载 | Concepts / Overloading |
| 根据类型选函数体 | `if constexpr` |
| 编译期求值与运行期复用 | `constexpr` |
| 只允许常量求值调用 | `consteval` |
| 根据是否常量求值走不同代码 | `if consteval` / `is_constant_evaluated` |
| 保证静态初始化 | `constinit` |

---

## 12. Concept Interface 与 Virtual Interface

### 12.1 两种接口写法

**运行期抽象基类：**

```cpp
class Interface {
public:
    virtual ~Interface() = default;
    virtual void f1() = 0;
    virtual void f2() = 0;
};

class Live : public Interface {
public:
    void f1() override {}
    void f2() override {}
};

class Mock : public Interface {
public:
    void f1() override {}
    void f2() override {}
};

void run(Interface& obj) {
    obj.f1();
    obj.f2();
}
```

**编译期 Concept 接口：**

```cpp
#include <concepts>

template<class T>
concept Interface2 = requires(T& val) {
    { val.f1() } -> std::same_as<void>;
    { val.f2() } -> std::same_as<void>;
};

class Live2 {
public:
    void f1() {}
    void f2() {}
};

class Mock2 {
public:
    void f1() {}
    void f2() {}
};

void run2(Interface2 auto& obj) {
    obj.f1();
    obj.f2();
}
```

`run2(Interface2 auto&)` 等价于受约束的函数模板形式：

```cpp
template<Interface2 T>
void run2(T& obj);
```

### 12.2 相似点与根本差异

**相似点**：两者都可以描述“实现类型应该具备 `f1()` 和 `f2()`”这类接口要求，可用于 Live / Mock。

**根本差异**：

- 纯虚基类采用显式继承关系，属于 **Nominal Interface**；调用可以通过基类引用在**运行期动态分发**。
- Concept 根据类型满足的操作要求进行检查，更接近 **Structural Interface**；Concept **本身不执行分发**，静态多态来自受约束**函数模板实例化**。
- 无继承关系的 `Live2`、`Mock2` 仍可满足 Concept。
- `run2(live2)` / `run2(mock2)` 通常分别实例化 `run2<Live2>`、`run2<Mock2>`。
- 若具体类的方法本来就是虚函数，Concept **不会自动消除虚调用**。

### 12.3 Concept 不是可以实例化的“接口对象类型”

```cpp
#include <memory>
#include <vector>

std::vector<std::unique_ptr<Interface>> objects;
objects.push_back(std::make_unique<Live>());
objects.push_back(std::make_unique<Mock>());

// std::vector<std::unique_ptr<Interface2>> bad; // 错：Concept 不是类型
```

要在运行期保存多种互不相关的具体类型，可使用虚基类、`std::variant`（封闭类型集合）、Type Erasure（类型擦除）、回调表等机制。

### 12.4 对比

| 属性 | Virtual Interface | Concept + Template |
|---|---|---|
| 是否显式继承 | 是 | 否 |
| 接口机制 | 名义关系 | 操作/结构约束 |
| 多态形式 | 运行期动态分发 | 编译期实例化；可做静态分派 |
| 异构容器 | 基类指针即可 | 需 Variant / Type Erasure 等 |
| 运行期换实现 | 自然支持 | 需额外抽象 |
| 内联潜力 | 依赖去虚化等 | 具体类型可见，有利于内联 |
| 代码体积 | 可能共享同一实现 | 多实例可能膨胀 |
| Mock | 继承实现 | 满足操作要求即可 |
| ABI 边界 | 常有固定接口 | 模板实现与实例化耦合更强 |

结论：**Concept 可以类比“编译期接口契约”，但不能等同于“编译期虚基类”。**

### 12.5 Mock 与依赖注入案例

```cpp
#include <concepts>

template<class T>
concept MarketDataFeed = requires(T& feed) {
    { feed.connect() } -> std::same_as<bool>;
    { feed.poll() } -> std::same_as<void>;
};

template<MarketDataFeed Feed>
class TradingEngine {
    Feed& feed_;
public:
    explicit TradingEngine(Feed& feed) : feed_(feed) {}
    void run() {
        if (feed_.connect())
            feed_.poll();
    }
};

struct MockFeed {
    bool connect() { return true; }
    void poll() {}
};

MockFeed mock;
TradingEngine engine(mock); // CTAD：从构造函数推导 Feed=MockFeed
```

这里的 `MockFeed` 不需要继承任何基类。

---

## 13. CRTP、Policy-based Design 与低延迟工程

### 13.1 CRTP

```cpp
template<class Derived>
struct Handler {
    void process() {
        static_cast<Derived*>(this)->handle();
    }
};

struct MarketHandler : Handler<MarketHandler> {
    void handle() {}
};
```

CRTP 提供静态多态和实现复用；Concept 可以约束类型的操作，但二者没有必然依赖关系。CRTP 的 `static_cast` 需要保证实际对象确实具有正确派生类型。

### 13.2 Policy-based Design

把固定策略放进模板参数，而不是热路径反复读取配置：

```cpp
#include <cstdint>
#include <utility>

struct NoMetrics {
    void on_message() noexcept {}
};

struct CounterMetrics {
    std::uint64_t count = 0;
    void on_message() noexcept { ++count; }
};

template<class Parser, class Metrics = NoMetrics>
class FeedHandler {
    [[no_unique_address]] Metrics metrics_;
    Parser parser_;
public:
    void handle(const char* data, std::size_t size) {
        auto message = parser_.parse(data, size);
        if (message) {
            metrics_.on_message();
            consume(*message);
        }
    }
};
```

这是结构示例，`Parser::parse` 和 `consume` 需按业务提供；代码设计假设 parse 返回可检查并可解引用的结果。`[[no_unique_address]]` 为 C++20。

应用：FIX / SBE Codec、日志策略、内存分配策略、锁策略、数据源和验证策略。

### 13.3 工程选择的判断准则

- 热路径的实现组合稳定、需求可静态决定时：模板 + Concept + Policy 值得考虑。
- 实现需要运行时加载、热切换、异构管理时：Virtual / Type Erasure 更直接。
- 模板化不自动等于更快；它只是让更多信息在编译期可见，给优化器提供机会。
- 要衡量代码体积、Instruction Cache、构建时间、错误信息、ABI 与扩展性。

---

## 14. 编译期协议描述案例

### 14.1 用类型表示字段

```cpp
#include <cstddef>

struct Price    { static constexpr std::size_t width = 8; };
struct Quantity { static constexpr std::size_t width = 4; };
struct Flags    { static constexpr std::size_t width = 1; };

template<class... Fields>
struct MessageLayout {
    static constexpr std::size_t fixed_width =
        (std::size_t{0} + ... + Fields::width);
};

using NewOrder = MessageLayout<Price, Quantity, Flags>;
static_assert(NewOrder::fixed_width == 13);
```

结合其他机制，可进一步实现：

- `Required<Tag>`、`Optional<Tag>`：在类型层表示字段规则；
- Type Traits：字段宽度、编码、合法值集合；
- `consteval` / `static_assert`：重复字段或不合法组合检查；
- `index_sequence` / Fold：按索引生成静态验证与编码操作；
- Concepts：对 Codec、Parser、Field 描述类型施加接口要求；
- Policy：是否校验、日志、字节序路径等固定策略。

**边界**：`fixed_width` 之和只是概念示例。真实 SBE / FIX 需要额外考虑字段位置、对齐、字节序、版本、presence、可变长字段和边界校验。模板不能替代 wire-format 的正确实现，也不能绕过对象生命周期、strict aliasing 与对齐规则。

---

## 15. 性能、编译成本、陷阱与面试练习

### 15.1 常见错误清单

| 错误理解 | 正确认识 |
|---|---|
| 函数模板所有参数必须显式指定 | 可以显式指定前缀，其余由调用实参推导 |
| `index_sequence<I...>` 里的 `I` 不能推导 | 可从同形的实参类型反向推导整数包 |
| 类模板不能进行类似推导 | CTAD 和类模板偏特化均可模式匹配 |
| CTAD 支持 `X<A,B>` 再继续推导剩余参数 | 不支持这类部分显式 CTAD |
| 模板推导会扫描函数体 | 不会 |
| `requires` 会实际执行操作 | 不会；主要检查表达式及类型性质 |
| Concept 就是虚基类 | 不是；Concept 只表示约束 |
| `constexpr` 调用总在编译期完成 | 不是；只有常量表达式上下文等有要求 |
| `consteval` 的形参可直接用于 `static_assert` | 不能把普通形参视为 NTTP |
| `if constexpr` 关闭所有语义检查 | 错；被丢弃分支有边界 |
| `std::conditional_t` 自动懒惰实例化无效分支 | 错；候选类型参数需先合法形成 |
| 模板必然零开销、无虚调用、无运行期分支 | 需要具体实现与优化、测量验证 |

### 15.2 可复用的练习题

1. 实现 `is_pointer`、`remove_pointer`、`remove_cvref`，包含 `const` 和引用测试。
2. 实现 `HasSize` 和 `NoSize`：分别用 `void_t` 和 `requires`。
3. 对 `T`、`T&`、`const T&`、`T&&` 列出 12 组推导结果。
4. 实现 `array_size(T(&)[N])`，解释为什么不会数组退化。
5. 实现 `tuple_has<Tuple,T>` 的索引序列版本、C++20 Lambda 版本、偏特化版本。
6. 实现 `tuple_find_index<Tuple,T>`；若不存在，返回 `tuple_size_v<Tuple>`。
7. 实现 `tuple_for_each`：支持左值/右值 tuple、完美转发、从左到右求值。
8. 实现四种 Fold，构造非结合运算展示左右 Fold 的差别。
9. 构建 `type_list` 的 `concat` / `transform` / `filter` / `fold`。
10. 用 Concepts 限制具有 `connect()/poll()` 的 Market Data Feed。
11. 在 `Concept + Template` 与 `Virtual Base` 两种接口下实现 Live/Mock 并比较 ABI、构建成本与动态选择能力。
12. 写一个 `constexpr` 函数并分别用编译期和运行期实参调用。
13. 写 `consteval` 校验协议字段宽度；解释为何普通参数不能直接用于 `static_assert`。
14. 解释 `if constexpr(std::is_constant_evaluated())` 为什么总为 true。
15. 使用 `if consteval`（C++23）区别常量求值与运行期实现。
16. 实现 Policy-based Parser，检查编译器生成汇编和二进制大小，而非只凭模板语法推断性能。
17. 解释 Two-phase Lookup、`typename`、`template` 消歧。
18. 解释为何函数模板不能偏特化、全特化不等同于额外重载候选。
19. 结合 `noexcept(std::is_nothrow_move_constructible_v<T>)` 讨论异常规范传播。
20. 在一个工程项目中评估模板实例化膨胀、I-cache 压力和 incremental build 开销。

### 15.3 面试回答的“分层模型”

按以下顺序分析任何模板问题：

1. **参数声明**：是类型、NTTP，还是模板参数包？
2. **参数来源**：显式、推导、默认；是否进入 Non-deduced Context？
3. **候选选择**：重载、SFINAE、Concept 满足情况、偏序？
4. **实例化**：依赖名称何时查找？是否有未实例化的错误分支？
5. **常量求值**：`constexpr` 允许，还是 `consteval` 强制？值是否真是常量表达式？
6. **代码生成**：能否内联、去虚化、消除分支？是否有代码膨胀？
7. **工程约束**：安全性、可扩展性、ABI、构建效率、热路径延迟。

**最终心智模型：模板负责参数化与具体化；Concepts 负责约束；`if constexpr` 负责实例化时的分支选择；`constexpr` / `consteval` 负责常量求值；虚函数与 Type Erasure 负责不同形式的运行期多态。**

---

## 16. 参考资料

- [cppreference — Templates](https://en.cppreference.com/w/cpp/language/templates)
- [cppreference — Template argument deduction](https://en.cppreference.com/w/cpp/language/template_argument_deduction)
- [cppreference — Class template argument deduction](https://en.cppreference.com/w/cpp/language/class_template_argument_deduction)
- [cppreference — Partial specialization](https://en.cppreference.com/w/cpp/language/partial_specialization)
- [cppreference — Parameter packs](https://en.cppreference.com/w/cpp/language/parameter_pack)
- [cppreference — Fold expressions](https://en.cppreference.com/w/cpp/language/fold)
- [cppreference — Dependent name](https://en.cppreference.com/w/cpp/language/dependent_name)
- [cppreference — Constraints and concepts](https://en.cppreference.com/w/cpp/language/constraints)
- [cppreference — requires expression](https://en.cppreference.com/w/cpp/language/requires)
- [cppreference — constexpr](https://en.cppreference.com/w/cpp/language/constexpr)
- [cppreference — consteval](https://en.cppreference.com/w/cpp/language/consteval)
- [cppreference — constinit](https://en.cppreference.com/w/cpp/language/constinit)
- [cppreference — if](https://en.cppreference.com/w/cpp/language/if)
- [cppreference — integer_sequence](https://en.cppreference.com/w/cpp/utility/integer_sequence)
