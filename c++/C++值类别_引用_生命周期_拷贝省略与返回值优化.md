# C++ 类型、Value Category、引用、对象生命周期与 Copy Elision：统一专题

> 范围：C++11/14/17/20/23；以 **C++17 及以后**为默认讨论版本。  
> 面向：Senior C++ / Production-Quality C++ / Low-Latency 面试。  
> 原则：先区分**实体、表达式、对象、存储空间、生命周期**，再判断 copy/move/elision。  
> 文档日期：2026-10-10。

## 目录

1. [一张总图：实体、表达式、对象、类型与生命周期](#1-一张总图实体表达式对象类型与生命周期)
2. [C++11 为什么引入 xvalue、move 和 forwarding？](#2-c11-为什么引入-xvaluemove-和-forwarding)
3. [Declared Type、Expression Type、Value Category](#3-declared-typeexpression-typevalue-category)
4. [lvalue、xvalue、prvalue：三种基本值类别](#4-lvaluexvalueprvalue三种基本值类别)
5. [`T` / `T&` / `T&&` 与引用绑定](#5-t--t--t-与引用绑定)
6. [`auto`、`auto&`、`auto&&`、`decltype`、`decltype(auto)`](#6-autoautoautodecltypedecltypeauto)
7. [`std::move`、`std::forward`、移动语义](#7-stdmovestdforward移动语义)
8. [对象的生命周期、临时对象和 Lifetime Extension](#8-对象的生命周期临时对象和-lifetime-extension)
9. [Copy Elision、RVO、NRVO、C++17 Guaranteed Elision](#9-copy-elisionrvonrvoc17-guaranteed-elision)
10. [函数返回值与六种接收方式](#10-函数返回值与六种接收方式)
11. [重载、`decltype(auto)` 与引用生命周期的常见陷阱](#11-重载decltypeauto-与引用生命周期的常见陷阱)
12. [Production/HFT 建议](#12-productionhft-建议)
13. [附录：vector / deque / list 的存储与生命周期](#13-附录vector--deque--list-的存储与生命周期)
14. [面试速查与自测](#14-面试速查与自测)

---

## 1. 一张总图：实体、表达式、对象、类型与生命周期

**不要把以下概念混为一谈：**

| 概念 | 英文 | 属于谁 | 典型问题 |
| --- | --- | --- | --- |
| 声明类型 | Declared type | 变量、函数等实体 | `r` 声明为 `T&` 还是 `T&&`？ |
| 表达式类型 | Expression type | 表达式 | `r`、`std::move(r)` 的结果类型是什么？ |
| 值类别 | Value category | 表达式 | 表达式是 lvalue、xvalue 还是 prvalue？ |
| 引用绑定 | Reference binding | 初始化/调用语境 | 可以绑定到 `T&` / `const T&` / `T&&` 吗？ |
| 对象生命周期 | Object lifetime | 对象 | 何时开始/结束，是否已悬空？ |
| 存储空间生命周期 | Storage duration / allocation | 存储区域 | 底层内存何时可用/释放？ |
| copy / move / elision | 初始化行为 | 构造新对象的过程 | 是复制、移动，还是直接构造？ |

一个类型为 `T&&` 的**引用实体**，使用其名字时可以形成 **lvalue 表达式**。一个 `T` 类型的 prvalue，在 C++17 可以直接初始化最终对象而不创建独立的中间临时对象。

> 核心区分：**引用类型不是 Value Category；Value Category 不是对象的物理存储位置；对象生命周期不是存储空间生命周期。**

## 2. C++11 为什么引入 xvalue、move 和 forwarding？

C++03 已有 lvalue/rvalue、RVO/NRVO；C++11 细化为 lvalue/xvalue/prvalue，并新增 rvalue reference 与 move semantics。

### 2.1 值语义的性能问题

```cpp
std::vector<int> a(1'000'000);
std::vector<int> b = a;            // 复制元素：两个独立 vector
std::vector<int> c = std::move(a); // 转移资源：a 仍活着但处于有效的移动后状态
```

复制有时必须昂贵；但若源对象的资源可被接管，重新分配和深复制便不是必要成本。

- **Move semantics**：支持将现有对象的资源转给另一个对象。
- **xvalue**：标识一个已有对象，表达式允许其资源被复用；不意味着它立即死亡。
- **Perfect forwarding**：泛型包装函数保留实参左值/右值属性。
- **Copy elision**：避免创建不必要的中间对象（其中 C++17 prvalue 规则变成语言保证）。

### 2.2 为什么 Java 没有对应的 `T&&`？

Java 普通对象变量保存对象引用；`b = a` 复制引用而非复制对象。C++ `T b = a` 通常构造独立值对象。Java 有共享别名、GC、逃逸分析等不同的设计取舍；Java 也需要管理文件/连接等非内存资源，但没有同样的语言级移动构造/所有权转移机制。

## 3. Declared Type、Expression Type、Value Category

```cpp
int a = 1;
int& lr = a;
int&& rr = 2;
```

| 表达式 | 对应实体的声明类型 | 表达式类型 | Value Category |
| --- | --- | --- | --- |
| `a` | `int` | `int` | lvalue |
| `(a)` | `int` | `int` | lvalue |
| `lr` | `int&` | `int` | lvalue |
| `rr` | `int&&` | `int` | lvalue |
| `std::move(a)` | — | `int` | xvalue |
| `42` | — | `int` | prvalue |

**为什么 `(a)` 仍是 lvalue？** 括号表达式保留其内部表达式的类型与值类别（此处不涉及额外运算）。`(a)` **不是引用类型**；只是 lvalue 表达式。

表达式不会以引用类型作为其经规则调整后的最终表达式类型；如果初始确定得到引用类型，使用被引用类型作为表达式类型，值类别另按规则确定。

```cpp
struct A {};
A   f();   // f() : 表达式类型 A，prvalue
A&  g();   // g() : 表达式类型 A，lvalue
A&& h();   // h() : 表达式类型 A，xvalue

A&& r = h();
r;         // lvalue：具名引用表达式
h();       // xvalue：返回右值引用的函数调用
```

**函数声明的返回类型**能够预测普通函数调用表达式的值类别，但**变量的声明类型**不能直接预测用变量名形成的表达式类别。

## 4. lvalue、xvalue、prvalue：三种基本值类别

```text
             expression
           /     |      \
       lvalue  xvalue  prvalue
         \      / \       /
          glvalue   rvalue

glvalue = lvalue + xvalue
rvalue  = xvalue + prvalue
```

| 值类别 | 是否有 identity | 常见表达式 | 主要语义 |
| --- | --- | --- | --- |
| lvalue | 是 | `a`、`*p`、返回 `T&` 的调用 | 表示某个已存在对象/函数 |
| xvalue | 是 | `std::move(a)`、返回 `T&&` 的调用 | 表示可以复用资源的已有对象 |
| prvalue | 不要求先有独立对象身份 | `42`、`T{}`、返回 `T` 的调用 | 计算值/初始化结果对象 |

**不能作物理解释：** lvalue 不保证对象在 RAM、prvalue 不保证在寄存器、xvalue 不保证即将析构。

常见表达式的值类别（这里运算符均假设为内建或指定的普通返回类型）：

| 表达式 | 类别 |
| --- | --- |
| `a`、`(a)`、`*p` | lvalue |
| 前置 `++i` | lvalue |
| 后置 `i++`、整数 `a+b`、字面量 `42` | prvalue |
| `T{}` | prvalue |
| `std::move(a)`、`static_cast<T&&>(a)` | xvalue |
| 返回 `T` / `T&` / `T&&` 的调用 | prvalue / lvalue / xvalue |
| `a.member` （普通非引用数据成员） | 通常 lvalue |
| `std::move(a).member`（普通非引用数据成员） | 通常 xvalue |

自定义运算符、引用成员、静态成员、条件运算符等有额外规则，不应根据符号形状一概推断。

### 4.1 C++17 prvalue 与 xvalue 最重要的区别

```cpp
struct Immovable {
    Immovable() = default;
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    ~Immovable() = default;
};

Immovable a = Immovable{};   // C++17: OK，直接构造 a
Immovable b = std::move(a); // Error，没有可用的 copy/move
```

- `Immovable{}` 是 prvalue，可以直接初始化目标 `a`。
- `std::move(a)` 是 xvalue，标识已有对象 `a`；若需要构造新对象 `b`，仍需合法的构造路径。

## 5. `T` / `T&` / `T&&` 与引用绑定

```cpp
T obj;
T&  lref = obj;
T&& rref = T{};

obj;              // lvalue
lref;             // lvalue
rref;             // lvalue
std::move(rref);  // xvalue
```

**`T&`、`T&&` 是引用类型；lvalue、xvalue、prvalue 是表达式类别。**

| 初始化表达式 | `T&` | `const T&` | `T&&` | `const T&&` |
| --- | :---: | :---: | :---: | :---: |
| 非 const lvalue `obj` | ✅ | ✅ | ❌ | ❌ |
| const lvalue `cobj` | ❌ | ✅ | ❌ | ❌ |
| 非 const xvalue `std::move(obj)` | ❌ | ✅ | ✅ | ✅ |
| prvalue `T{}` | ❌ | ✅ | ✅ | ✅ |

这里假设是相同基础类类型的直接绑定、没有自定义转换。const xvalue 不能绑定到非 const `T&&`。

```cpp
T obj;
T& x = std::move(obj);    // ERROR：非 const lvalue ref 不能绑定 xvalue
T&& rr = std::move(obj); // OK
T& y = rr;                // OK：具名 rr 是 lvalue
// rr、y、obj 都表示同一个对象；以上代码并未真正移动资源。
```

**重载选择**（假设存在三个重载）：

```cpp
void use(T&);
void use(const T&);
void use(T&&);

T obj;
const T cobj;

use(obj);             // T&
use(cobj);            // const T&
use(std::move(obj));  // T&&
use(T{});             // T&&
use(std::move(cobj)); // const T&（常见 move ctor 也无法接收 const T&&）
```

## 6. `auto`、`auto&`、`auto&&`、`decltype`、`decltype(auto)`

### 6.1 auto 推导（按值 / 引用）

```cpp
T obj;
const T cobj{};

auto a = obj;              // T：复制（假设可复制）
auto b = std::move(obj);   // T：移动（假设可移动）
auto c = T{};              // T：C++17 直接构造
auto& d = obj;             // T&
const auto& e = T{};       // const T&，临时对象生命周期延长
auto&& f = obj;            // T&，引用折叠
auto&& g = T{};            // T&&，临时对象生命周期延长
const auto&& h = T{};      // const T&&，临时对象生命周期延长
// auto& bad = T{};        // ERROR：非 const lvalue ref 不能绑定右值
```

`auto&&` 在上述推导语境中是 **forwarding reference**；不是所有字面上的 `T&&` 都是 forwarding reference。

**引用折叠：**

| 原始组合 | 结果 |
| --- | --- |
| `T& &` | `T&` |
| `T& &&` | `T&` |
| `T&& &` | `T&` |
| `T&& &&` | `T&&` |

### 6.2 decltype 有两套规则

**特殊规则**：未加括号的 id-expression / 成员访问表达式，得到所指定实体的类型。

**一般规则**（`decltype((expr))`）：以表达式的类型 `T` 和值类别推导：

| 类别 | decltype 结果 |
| --- | --- |
| lvalue | `T&` |
| xvalue | `T&&` |
| prvalue | `T` |

```cpp
int a = 10;
int& lr = a;
int&& rr = 20;

static_assert(std::is_same_v<decltype(a), int>);
static_assert(std::is_same_v<decltype((a)), int&>);
static_assert(std::is_same_v<decltype(lr), int&>);
static_assert(std::is_same_v<decltype(rr), int&&>);
static_assert(std::is_same_v<decltype((rr)), int&>);
static_assert(std::is_same_v<decltype(std::move(a)), int&&>);
static_assert(std::is_same_v<decltype(42), int>);
```

**`(a)` 本身不是 `int&` 类型；`decltype((a))` 因 `(a)` 是 lvalue 而得到 `int&`。** 这句话非常值得牢记。

### 6.3 decltype(auto) 与 return 括号陷阱

```cpp
int a = 10;

decltype(auto) x = a;    // int（特殊规则）
decltype(auto) y = (a);  // int&（一般规则）

decltype(auto) by_value() {
    int local = 42;
    return local;          // int；安全返回值
}

decltype(auto) dangling() {
    int local = 42;
    return (local);        // int&；返回悬空引用，危险
}
```

`auto` 返回推导通常丢弃引用；`decltype(auto)` 保留 `decltype` 的精确规则，不可机械替换。

## 7. `std::move`、`std::forward`、移动语义

### 7.1 std::move 不移动

其核心可理解为：

```cpp
template<class T>
constexpr std::remove_reference_t<T>&& move(T&& x) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(x);
}
```

它执行类型转换并产生 xvalue。真正的资源转移由被选择的构造/赋值函数完成。

```cpp
std::string a = "hello";
auto b = std::move(a); // b 移动构造；a 仍活着、处于有效但值未指定的状态
```

### 7.2 const 与 move

```cpp
const std::string a = "hello";
std::string b = std::move(a); // std::move(a) 是 const std::string&&
                               // 标准 string 通常选择 copy constructor
```

普通 `T(T&&)` 无法接收 `const T&&`。所以 **`std::move` 不能保证一定发生 move**。

### 7.3 std::forward 保留调用方左/右值属性

```cpp
void sink(const T&);
void sink(T&&);

template<class U>
void wrapper(U&& arg) {
    sink(std::forward<U>(arg));
}

T obj;
wrapper(obj);            // U = T&，传 lvalue
wrapper(std::move(obj)); // U = T，传 xvalue
wrapper(T{});            // U = T，传 xvalue
```

如果直接写 `sink(arg)`，`arg` 是具名表达式，始终作为 lvalue 传递。`std::forward` 主要保留 lvalue/rvalue、cv 属性，不保证区分传入时的 prvalue 和 xvalue。

**`std::move_if_noexcept`**：某些容器在搬迁已有元素时，会基于移动构造的 `noexcept` 性质与复制能力选择移动或复制，以维护异常安全保证。

## 8. 对象的生命周期、临时对象和 Lifetime Extension

### 8.1 Storage ≠ Object lifetime

```cpp
std::vector<int> v;
v.reserve(100);  // 分配容纳至少 100 个 int 的存储空间
                 // v.size() 仍是 0，没有 100 个可访问的 vector 元素
// v[0] = 1;    // UB：索引不在 [0, size()) 内
v.resize(3);    // 新增三个 int 元素，其值为 0
```

存储可存在而对象生命周期尚未开始；对象被销毁后，存储也可能仍保留。尤其是 placement new、容器容量与对象生命周期管理。

### 8.2 Temporary Materialization（C++17）

```cpp
struct A {};

A a = A{};           // 同类型 prvalue 直接构造最终对象 a
const A& r = A{};    // 需要临时对象实体化 + 引用绑定
A&& rr = A{};        // 同上
```

`A{}` 本身是 prvalue；当语境需要一个可绑定的实际对象时，会执行 temporary materialization。不要说“prvalue 永远没有对象”，也不要说“所有 prvalue 一定先形成临时对象”。

### 8.3 full-expression 与生命周期延长

```cpp
struct A { ~A(); };

void use(const A&);

use(A{}); // 参数绑定的临时对象，通常在整个调用表达式结束后销毁

const A& r = A{}; // 此直接绑定将临时对象生命周期延至 r 的生命周期结束
A&& rr = A{};     // 此直接绑定也延长生命周期
const A&& cr = A{}; // 同样可以延长
```

局部引用直接绑定临时对象时，通常能将临时对象生命周期延到该引用的生命周期结束。

**关键例外与危险写法：**

1. **函数引用形参**：临时对象存活到包含调用的 full-expression 结束，不会因为函数返回引用而继续延长。
2. **通过函数返回的引用**：不会替调用者延长对象生命；不能返回对局部对象或临时对象的悬空引用。
3. **`new` 表达式中的引用初始化**：临时对象通常只持续到包含 `new` 的完整表达式结束。
4. **聚合体引用成员**：C++20 中 `Agg a{7};` 与 `Agg a(7);` 在临时对象生命周期上可能不同。
5. **生命周期不能经由第二个引用再次延长**。

```cpp
const A& id(const A& x) { return x; }

const A& bad = id(A{}); // id 的形参直接绑定临时对象；
                        // 该临时对象在本条声明的完整表达式结束后销毁
                        // bad 随即悬空

auto&& bad2 = std::move(A{}); // std::move 返回引用，不能借此再次延长
                              // 临时对象在语句结束后销毁
```

不要将“`const T&` 或 `T&&` 能延长临时对象生命周期”误读为“只要出现引用就能延长”，必须检查初始化路径和表达式类别。

### 8.4 返回引用的典型错误

```cpp
const std::string& broken() {
    return std::string("temporary"); // 悬空引用；不应这样返回
}

std::string safe() {
    return std::string("value"); // C++17 直接构造返回对象
}
```

C++17/20/23 对前一种情况并不会自动延长到调用者生命周期。C++26 起部分此类返回引用绑定临时对象的写法直接不合法。

## 9. Copy Elision、RVO、NRVO、C++17 Guaranteed Elision

### 9.1 术语表

| 名称 | 示例 | C++17 起是否保证 |
| --- | --- | --- |
| Copy Elision | 省略本可发生的 copy/move 对象创建 | 视场景 |
| RVO / 传统 URVO | `return T{};` | **保证**（更准确是 C++17 prvalue 语义） |
| NRVO | `T local; return local;` | **不保证** |
| 直接初始化同类型 prvalue | `T obj = T{};` | **保证** |
| 返回已有对象的 xvalue | `return std::move(local);` | 不属于 NRVO，通常调用 move |

“RVO”在日常交流有时被用作返回值优化的泛称；正式讨论时建议明确：**`return T{}` 的 guaranteed elision 与 `return local` 的可选 NRVO 是两回事。**

### 9.2 对照代码

```cpp
struct A {
    A() = default;
    A(const A&);
    A(A&&);
    ~A() = default;
};

A rvo() {
    return A{};            // C++17 guaranteed：直接构造返回结果
}

A nrvo() {
    A a;
    return a;              // NRVO eligible，但非强制
}

A moved() {
    A a;
    return std::move(a);   // 阻止 NRVO；通常使用 move ctor
}

A forwarded() {
    return rvo();          // 同类型 prvalue：guaranteed
}

A param(A a) {
    return a;              // 函数形参不符合 NRVO，通常隐式移动
}
```

### 9.3 Guaranteed Elision 和“没有可用的移动构造函数”

```cpp
struct NonMovable {
    NonMovable() = default;
    NonMovable(const NonMovable&) = delete;
    NonMovable(NonMovable&&) = delete;
    ~NonMovable() = default;
};

NonMovable good() {
    return NonMovable{};  // C++17 OK
}

NonMovable bad() {
    NonMovable obj;
    return obj;           // NRVO 不保证；没有合法的后备 copy/move，ill-formed
}
```

即使没有发生中间临时对象的析构，**返回类型析构函数仍须可访问且未删除**。

### 9.4 多个 return、C++23 implicit move

```cpp
A f(bool flag) {
    A a;
    if (flag) return a;
    return a;   // 两个 return 都返回 a，仍可能 NRVO
}

A g(bool flag) {
    A a, b;
    if (flag) return a;
    return b;   // 各语句可符合 NRVO 候选条件，但通常不能统一消除
}
```

未执行 NRVO 时，`return local;` 在 C++11 起有特殊的**隐式移动**规则；C++23 进一步简化 move-eligible return 表达式的处理。不要把这个机制误认为“具名 local 本身是 xvalue”：**在普通表达式上下文中 `local` 仍是 lvalue**。

### 9.5 Guaranteed elision 的边界

同类型 prvalue 直接构造要求符合相应语境和类型条件（类类型相同、忽略顶层 cv 的相关规则）。在构造**可能重叠的子对象**（如某些基类子对象）时，不能套用普通的 guaranteed elision 推断。

```cpp
A a = A{};              // C++17：guaranteed direct construction
A b = std::move(a);     // xvalue：需要合法的移动/复制路径
A c = std::move(A{});   // std::move 先产生 xvalue，通常要移动构造
```

优化级别、调试输出和 `-fno-elide-constructors` 不应被当作验证 guaranteed prvalue 语义的依据；编译器可以关闭可选 NRVO，但不能违反 C++17 必须直接构造的语义。

## 10. 函数返回值与六种接收方式

以下假设：

```cpp
A f() { return A{}; }   // 返回 A by value / prvalue
```

| 初始化语句 | 合法 | 推导后的变量声明类型 | 初始化时 copy/move | 生命周期 |
| --- | :---: | --- | --- | --- |
| `const auto a = f();` | ✅ | `const A` | 0 | a 是独立对象，至其生命周期结束 |
| `const auto& a = f();` | ✅ | `const A&` | 0 | 临时 A 实体化并被延长 |
| `const auto&& a = f();` | ✅ | `const A&&` | 0 | 临时 A 实体化并被延长 |
| `auto a = f();` | ✅ | `A` | 0 | a 是独立对象，直接构造 |
| `auto& a = f();` | ❌ | — | — | 非 const 左值引用不能绑定 prvalue |
| `auto&& a = f();` | ✅ | `A&&` | 0 | 临时 A 实体化并被延长 |

重点：
- by-value 的两项属于 **C++17 直接构造**，不是“先临时再移动”。
- 三项引用绑定需要 **temporary materialization**，零次 copy/move 不等于没有真实对象。
- **所有合法具名变量表达式 `a` 均为 lvalue**，即使变量声明类型是 `A&&` / `const A&&`。
- 对普通移动构造而言 `std::move(const_a)` 得到 `const A&&`，通常无法调用 `A(A&&)`。

### 10.1 若 f 返回引用，结论完全不同

```cpp
A& by_lref();
A&& by_rref();

auto x = by_lref();   // 从 lvalue 构造新的 A：通常 copy
auto&& y = by_lref(); // 推导 A&，绑定原对象
auto z = by_rref();   // 从 xvalue 构造新的 A：通常 move
auto&& w = by_rref(); // 推导 A&&，绑定原对象
```

返回引用的函数调用**没有权力延长底层对象的生命周期**。若 `by_rref()` 已经返回悬空引用，接收方也会悬空。

## 11. 重载、`decltype(auto)` 与引用生命周期的常见陷阱

### 11.1 函数参数不符合 NRVO

```cpp
A identity(A arg) {
    return arg; // 对参数不能 NRVO，但可隐式移动
}
```

“复制/移动几次”必须先说明是否包含**传入参数的成本**、标准版本、构造函数可用性与可选 NRVO；不能笼统回答一定 0/1 次。

### 11.2 const 阻止常规移动

```cpp
const A src{};
A dst = std::move(src); // 通常 copy（需要 A(const A&)）
```

### 11.3 ref-qualified member functions

```cpp
struct Buffer {
    std::string data;

    const std::string& get() const & { return data; }
    std::string get() && { return std::move(data); }
};

Buffer b;
const std::string& view = b.get(); // 指向 b 内部，要求 b 活着
std::string own = Buffer{}.get();  // 将资源移出临时 Buffer
```

第二个返回按值 `std::string`：对成员调用 `std::move` 可以合理；**“return 不写 std::move”主要针对可 NRVO 的局部对象**，不能不加条件地推广到成员返回值。

### 11.4 `auto&&` 不是所有时候都能延长临时对象生命

```cpp
A make();          // 返回 A by value
A&& bad_ref();     // 如果实现返回悬空引用就是问题

auto&& r1 = make();   // 直接绑定该 prvalue：临时对象延长
auto&& r2 = bad_ref();// 只绑定返回的引用：不会修复悬空
```

### 11.5 容器/视图的悬空问题

`std::string_view`、`std::span`、裸指针、迭代器、引用不会自动管理底层对象的生命周期。即使代码没有任何 copy/move，仍可能因对象销毁或 vector reallocation 而悬空。

## 12. Production/HFT 建议

- **默认按值返回普通值对象：** `return result;`；NRVO 优先，后备隐式移动。不要盲目 `return std::move(result);`。
- **构造时避免不必要复制：** 对大资源拥有者优先考虑 move-only RAII 类型，并正确编写/默认生成特殊成员。
- **泛型封装使用 `std::forward`：** forwarding reference 形参在函数体内总是具名 lvalue。
- **不要把 `std::move` 当性能承诺：** 它只转换值类别；移动构造也可能分配，`const` 可能造成 copy。
- **检查所有借用型 API 的生命周期：** `T&`、`T&&`、`span`、`string_view` 不能代替所有权。
- **Copy Elision 不等于零成本函数：** 构造最终对象自身、堆分配、同步等仍然有成本。
- **先保证正确性，再看生成代码：** 不能以一次编译运行的 copy/move 日志来推断标准保证，NRVO 仍可选。

## 13. 附录：vector / deque / list 的存储与生命周期

这部分属于 STL 容器语义，与上面“Storage ≠ Object Lifetime”和移动/失效规则直接相关。

| 容器 | 典型布局 | `reserve` | `resize` | `shrink_to_fit` |
| --- | --- | --- | --- | --- |
| `std::vector` | 一块连续元素存储 | ✅ | ✅ | ✅，非强制缩容 |
| `std::deque` | 多个连续块，由内部索引结构管理 | ❌ | ✅ | ✅，非强制缩容 |
| `std::list` | 独立链表节点 | ❌ | ✅ | ❌ |

### 13.1 vector：size vs capacity

```cpp
std::vector<int> v;
v.reserve(100); // capacity >= 100；size == 0
v.resize(3);    // size == 3；新增元素值初始化
v.resize(1);    // 销毁多余元素；capacity 不变
v.clear();      // size == 0；capacity 不变
v.shrink_to_fit(); // 仅请求减少 capacity，不保证缩到 size
```

- `reserve(n)` 不创建额外的 vector 元素，不能通过下标使用“预留区域”。
- `resize(n)` 改变对象数量：增加时构造，减少时销毁；缩小不减少 capacity。
- vector **确实 reallocate** 时，原元素的指针、引用、迭代器失效。
- `shrink_to_fit` 即使缩容并释放旧分配，也不保证 allocator 立刻把内存还给 OS。

### 13.2 deque：块内连续，不是全局连续

```text
vector: [0][1][2][3][4][5]    一个连续区间
deque : map -> [0][1]  [2][3]  [4][5]    分段连续
```

- deque 两端扩张通常不必搬迁已有元素对象；块/索引表自身可能重新分配。
- 在两端插入后，已有元素引用/指针通常保持有效，但 iterator 可全部失效。
- deque 没有标准化的 `capacity()` / `reserve()`，其空闲空间不等同于一个连续容量。
- list 通过单独节点分配实现两端插入/删除；迭代器和引用稳定性良好，但遍历缓存局部性通常较差。

**低延迟取舍：** 连续批量遍历优先评估 vector；若确实需要稳定地址、两端插入或固定容量 FIFO，再评估 deque / pool / 预分配 ring buffer。不要因为均摊 O(1) 就认定每次操作延迟固定。

## 14. 面试速查与自测

### 14.1 必背的 12 句话

1. `T` / `T&` / `T&&` 是类型；lvalue / xvalue / prvalue 是**表达式值类别**。
2. `(a)` 保持 `a` 的值类别：具名变量 `a` 一般是 lvalue。
3. `decltype(a)`（未加括号的名字）按声明类型；`decltype((a))` 按值类别。
4. `decltype((expr))`：lvalue → `T&`；xvalue → `T&&`；prvalue → `T`。
5. 具名右值引用变量 `T&& r`，表达式 `r` 是 lvalue。
6. `std::move` 是 cast，不是移动操作。
7. `std::forward` 在 forwarding reference 模板语境中保留实参左/右值属性。
8. xvalue 有 identity，prvalue 不要求先有独立的对象身份。
9. C++17 同类型 prvalue 初始化可以**保证直接构造目标对象**。
10. `return T{};` guaranteed；`return local;` NRVO **不保证**；`return std::move(local);` 阻止 NRVO。
11. 函数按值返回 `T` / 按引用返回 `T&` / `T&&`，其调用表达式分别为 prvalue / lvalue / xvalue。
12. 引用绑定有时延长临时对象生命周期，但函数返回引用、参数引用等有关键例外；**存储空间不是对象生命周期**。

### 14.2 自测（先判断，再运行或查标准）

```cpp
struct A {
    A() = default;
    A(const A&) = default;
    A(A&&) = default;
};

A make() { return A{}; }
A make_named() { A a; return a; }
A& get_lref();
A&& get_rref();

void take(A&);
void take(const A&);
void take(A&&);

void test() {
    A a;
    A&& rr = A{};

    // Q1: decltype(rr) 与 decltype((rr)) 分别是什么？
    // Q2: take(rr) 匹配哪一个？
    // Q3: take(std::move(rr)) 匹配哪一个？
    // Q4: A x = make(); C++17 复制/移动次数？
    // Q5: A y = make_named(); 能否保证零次移动？
    // Q6: const auto& c = make(); 临时对象存活多久？
    // Q7: auto&& d = get_lref(); d 的声明类型是什么？
    // Q8: auto e = get_rref(); 通常如何构造 e？
}
```

**答案**：Q1 `A&&` 和 `A&`；Q2 `take(A&)`；Q3 `take(A&&)`；Q4 保证 0 次；Q5 不保证（可选 NRVO）；Q6 直接绑定延至 `c` 生命周期结束；Q7 `A&`；Q8 通常 move，但要求引用指向存活对象。

### 14.3 编译验证建议

```bash
g++ -std=c++17 -Wall -Wextra -pedantic main.cpp
g++ -std=c++17 -O0 -fno-elide-constructors main.cpp
g++ -std=c++23 -Wall -Wextra -pedantic main.cpp
```

`-fno-elide-constructors` 可帮助观察**可选** copy elision 的差异；不能用来否定 C++17 的 guaranteed prvalue 语义。实验应与规范规则分开解读。

## 参考资料

- [cppreference — Value categories](https://en.cppreference.com/w/cpp/language/value_category)
- [cppreference — decltype](https://en.cppreference.com/w/cpp/language/decltype)
- [cppreference — Reference initialization](https://en.cppreference.com/w/cpp/language/reference_initialization)
- [cppreference — Temporary object lifetime](https://en.cppreference.com/w/cpp/language/lifetime)
- [cppreference — Copy elision / prvalue semantics](https://en.cppreference.com/w/cpp/language/copy_elision)
- [cppreference — Move constructor](https://en.cppreference.com/w/cpp/language/move_constructor)
- [cppreference — std::forward](https://en.cppreference.com/w/cpp/utility/forward)

---

> 最终心智模型：**先问“这是哪个实体/对象”，再问“表达式的类型和值类别是什么”，然后问“是否需要真实对象/引用绑定、生命周期到哪里”，最后才判断“copy / move / direct construction / elision”。**
