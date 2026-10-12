# C++ 类型转换：Implicit / Explicit Casts、四种 Named Cast、bit_cast 与 memcpy

> C++11–C++23 面试与 Production C++ 专题。重点：**转换是否合法 ≠ 转换结果能否安全访问**，以及 **cast 不会凭空创建一个目标类型对象**。

## 1. 全景：Implicit vs Explicit

| 类别 | 典型例子 | 主要特性 |
|---|---|---|
| Implicit conversion | `double d = 3;`、`Derived* → Base*` | 由语境触发；也可能窄化或改变 signedness |
| `static_cast` | `static_cast<int>(3.14)` | 编译期可判定的转换；不做动态类型检查 |
| `const_cast` | `const_cast<int*>(p)` | 修改访问路径中的 cv-qualification |
| `reinterpret_cast` | 对象指针之间的低级转换 | 不保证类型访问、对齐、生命周期合法 |
| `dynamic_cast` | `dynamic_cast<Derived*>(base)` | 多态体系中可进行运行时检查 |
| C-style cast | `(T)expr` | 可能组合不同转换，意图不够清楚 |
| `std::bit_cast` (C++20) | `bit_cast<uint32_t>(float)` | 对象表示转换，返回目标类型的独立值 |
| `std::memcpy` | `memcpy(&dst,&src,n)` | 按字节复制，不等于通用对象复制 |

**重要：** `std::bit_cast` 不是第五种语言级 named cast，而是 C++20 的标准库函数。

## 2. Implicit Conversion 与 explicit 关键字

常见标准转换：integer promotion、usual arithmetic conversions、整数/浮点数转换、array-to-pointer、function-to-pointer、qualification conversion、derived-to-base。

```cpp
int n = -1;
unsigned u = n;       // 合法，但按模 2^N 转换

double d = 3.8;
int x = d;            // 3；若截断后的值超出 int 可表示范围则 UB
```

用户定义转换：

```cpp
struct Price {
    Price(double p) : value(p) {}
    double value;
};
void send(Price);
send(1.25); // 隐式构造 Price
```

用 `explicit` 抑制不希望的隐式构造：

```cpp
struct Price {
    explicit Price(double p) : value(p) {}
    double value;
};
send(Price{1.25}); // OK
// send(1.25);     // 错误
```

`explicit` 关键字与 explicit type cast 有联系，但概念不等价。对于有潜在窄化的初始化，`T{x}` 往往比 `T(x)` 更易发现错误。

## 3. static_cast

### 数值转换

```cpp
double d = 3.75;
int i = static_cast<int>(d); // 3，向零截断
```

数值转换可能丢失信息；`static_cast` 并非 range-checked cast。

### 上下行继承转换

```cpp
struct Base { virtual ~Base() = default; };
struct Derived : Base { int id = 7; };

Derived d;
Base* b = &d;                            // upcast，通常隐式即可
Derived* p = static_cast<Derived*>(b);   // 确实指向 Derived 时合法
```

`static_cast` downcast 不查动态类型：

```cpp
Base base;
Derived* bad = static_cast<Derived*>(&base); // 不满足实际类型前提：UB
```

不要把编译通过视为类型安全。

### void* 往返

```cpp
int value = 42;
void* raw = &value;
int* p = static_cast<int*>(raw);
int v = *p; // OK

double* wrong = static_cast<double*>(raw); // 不会创建 double 对象
// double d = *wrong; // 不合法的类型访问等风险：UB
```

### std::move 实际是一种 cast

```cpp
std::move(x); // 主要相当于向去引用后的 T&& 做 static_cast，产生 xvalue
```

它不移动任何资源；移动发生在随后选择的 move constructor / assignment 中。

## 4. const_cast：cv 限定 vs 对象实际 constness

`const_cast` 改变指针/引用访问路径的 cv-qualification，**不改变底层对象自身是否 const**。

```cpp
int x = 10;
const int* p = &x;
int* q = const_cast<int*>(p);
*q = 20; // OK：原始对象 x 不是 const
```

```cpp
const int x = 10;
const int* p = &x;
int* q = const_cast<int*>(p);
*q = 20; // UB：修改真正的 const 对象
```

通过 `const_cast` 形成非 const 指针本身不等于修改；读取仍可以合法。写入是否允许取决于实际对象。

### const 引用与临时对象

```cpp
void update(const int& v) {
    int& x = const_cast<int&>(v);
    x = 100;
}

int a = 1;
update(a); // OK：底层为非 const
const int b = 1;
// update(b); // UB
// update(1); // UB：绑定的是 const 临时对象
```

### 指针 const 的不同位置

| 类型 | 能改 pointee | 能改 pointer |
|---|---|---|
| `int*` | 是 | 是 |
| `const int*` | 否（通过此路径） | 是 |
| `int* const` | 是 | 否 |
| `const int* const` | 否（通过此路径） | 否 |

旧 C API 的 `char*` 实际只读时可能使用 `const_cast` 适配，**前提是完全保证不修改**；更好的方案是修正 API 的 const-correctness。

## 5. reinterpret_cast：指针转换不赋予访问权限

```cpp
float f = 1.0f;
auto p = reinterpret_cast<std::uint32_t*>(&f);
// auto bits = *p; // 通常 UB：float 不是可通过 uint32_t 左值访问的对象
```

`reinterpret_cast` 不：
- 创建所指类型的对象；
- 开始所指类型的对象生命周期；
- 保证地址满足新类型的 alignment；
- 取消 strict aliasing / type accessibility 限制；
- 处理网络字节序、padding 或对象布局。

**指针转换本身和随后解引用是两件不同的事。**

允许通过 `char`、`unsigned char`、`std::byte` 访问对象表示（C++ type accessibility 规则）；不要推广成任意 `T*` 都能读取任意对象。

### 网络报文中的典型陷阱

```cpp
struct Header {
    std::uint32_t length;
    std::uint16_t type;
};

void parse(const char* buffer) {
    auto* h = reinterpret_cast<const Header*>(buffer);
    // auto len = h->length; // 不自动安全
}
```

必须分别考虑：
1. buffer 长度与 bounds；
2. 对齐要求；
3. Header 对象的 lifetime 是否已开始；
4. type accessibility；
5. 结构体 padding、ABI layout 和 endianness。

**推荐**按字段用 `memcpy` 或字节解码；SBE 生成的访问器通常基于偏移和协议字节序读写，不应把它简单视为 struct overlay。

## 6. dynamic_cast 与 RTTI

对多态类型进行运行时检查的下行转换：

```cpp
struct Animal { virtual ~Animal() = default; };
struct Dog : Animal { void bark(); };
struct Cat : Animal {};

Animal* a = /* 指向 Dog 或 Cat */;
Dog* dog = dynamic_cast<Dog*>(a);
if (dog) dog->bark();
```

- 失败的指针转换通常得到 `nullptr`。
- 失败的引用转换抛出 `std::bad_cast`。
- 需要运行时检查的 downcast/cross-cast 要求多态源类型（至少一个虚函数）。
- 已知派生到可访问基类的 upcast 通常不需 RTTI 查询。
- 不同 ABI 和继承层次下，运行时检查开销不同；HFT hot path 中不宜凭空假设其恒定时间。

### Cross-cast

```cpp
struct A { virtual ~A() = default; };
struct B { virtual ~B() = default; };
struct C : A, B {};

C c;
A* a = &c;
B* b = dynamic_cast<B*>(a); // 指向同一个 C 内的 B 子对象
```

## 7. C-style cast / function-style cast

```cpp
double d = 3.14;
int a = (int)d;
int b = int(d);
int c = static_cast<int>(d);
```

C-style cast 可以在标准指定的搜索顺序下结合多个转换种类，可能执行类似 `const_cast`、`static_cast`、`reinterpret_cast` 的效果：

```cpp
const int* p = nullptr;
int* q = (int*)p;                  // 可以移除 const，意图不清晰
// int* x = static_cast<int*>(p); // 编译失败
int* y = const_cast<int*>(p);      // 表达意图
```

Named Cast 的主要优点是表达转换的类别和约束，而不是运行速度保证。

## 8. std::bit_cast（C++20）

`std::bit_cast<To>(from)` 将 `From` 的对象表示映射到 **新的 `To` 值**。

编译期硬约束：
- `sizeof(To) == sizeof(From)`；
- `std::is_trivially_copyable_v<To>`；
- `std::is_trivially_copyable_v<From>`。

```cpp
#include <bit>
#include <cstdint>
#include <type_traits>

float f = 1.0f;
static_assert(sizeof(float) == sizeof(std::uint32_t));
std::uint32_t bits = std::bit_cast<std::uint32_t>(f);
// 在常见 IEEE-754 binary32 实现上，bits = 0x3F800000
```

`std::bit_cast` 不需要通过错误类型的指针解引用，因此是这类 type-punning 的常见安全替代方案。它是 **value conversion，不是 zero-copy view**。

### 并非任意位模式都合法

`TriviallyCopyable` 是必要条件，但目标类型并非对所有位模式都有有效值；无效表示会涉及未定义行为/不确定的位语义（具体按 C++ 版本与目标类型规则）。另外，对象 padding、endianness、ABI 依赖不会因为 bit_cast 自动消失。

### constexpr 限制

`bit_cast` 在满足额外限制时可常量求值，但不是对含任意指针、union 等子对象都保证 constexpr。

### 大对象

`bit_cast` **没有固定的对象大小上限**，但返回独立目标值，不保证免除数据搬运或大对象临时存储成本：

```cpp
struct A { std::array<std::uint64_t, 1024> x; };
struct B { std::array<std::byte, 8192> bytes; };
static_assert(sizeof(A) == sizeof(B));
auto b = std::bit_cast<B>(A{}); // 8 KB，合法
```

## 9. bit_cast vs memcpy

| 维度 | `std::bit_cast` | `std::memcpy` |
|---|---|---|
| 标准版本 | C++20 | C++98 起 |
| 目的 | 完整对象表示→新目标类型值 | 指定字节数复制到目标存储 |
| trivially copyable | 源和目标都必须 | memcpy API 本身不要求 |
| 大小 | sizeof(To) == sizeof(From) | 可不同，明确指定 n |
| 部分/动态长度 | 否 | 是 |
| 返回 | To 类型的值 | 原目标地址 |
| `constexpr` | 有条件支持 | C++20 标准 memcpy 不是 constexpr |
| aliasing | 不做不兼容类型左值访问 | 复制字节，不做不兼容类型左值访问 |
| 大对象 | 能处理，但可能复制较多字节 | 更直接地操作已有目标缓冲区 |
| 性能 | 小定长类型通常高效 | 小定长复制通常被内联优化 |

### 等价用途示例（常见平台）

```cpp
float f = 1.0f;
std::uint32_t a = std::bit_cast<std::uint32_t>(f);

std::uint32_t b;
std::memcpy(&b, &f, sizeof b);
// 二者通常会产生相同结果，也可能生成相同机器指令
```

不要认为 `bit_cast` 一定比 `memcpy` 快。

### Byte copy ≠ Object copy

```cpp
struct S { std::string text; };
S a{"hello"}, b{"world"};
// std::memcpy(&b, &a, sizeof(S)); // 不应这样复制资源所有者，会破坏语义/可能 UB
b = a; // 正确的复制赋值
```

`memcpy` 可以复制非 trivially-copyable 对象的字节，但不意味着语义上安全克隆该对象。

对于完整复制 trivially copyable 对象到一个已存在的同类型对象，`memcpy` 通常是合法的字节复制方法。

### 从不对齐 buffer 读取字段

```cpp
std::array<std::byte, 128> buffer{};
std::uint32_t len;
std::memcpy(&len, buffer.data() + 1, sizeof len);
```

不要求 `buffer.data()+1` 按 `uint32_t` 对齐。然后按协议处理 endian。相比之下，`reinterpret_cast<const uint32_t*>(buffer.data()+1)` 解引用有对齐/类型/生命周期风险。

### zero-copy 字节视图

```cpp
A obj{};
std::span<const std::byte> bytes = std::as_bytes(std::span{&obj, 1});
```

只读 byte-view，**不产生新 B 对象，也不进行复制**；必须保证 `obj` 在 view 使用期间仍然存活。

## 10. 进阶知识：Object Lifetime / Aliasing / Alignment

这三项必须分别证明，而不能由 cast 自动保证：

- **Lifetime**：目标地址上是否真的存在该类型的活跃对象？
- **Aliasing**：能否通过该类型的 glvalue 合法访问对象？
- **Alignment**：地址是否满足所需对齐？
- **Bounds**：访问是否位于有效内存区域？
- **Representation**：复制的位模式是否对目标类型有效？

C++20 对某些 **implicit-lifetime types** 引入隐式对象创建规则，使部分 byte storage 操作可合法开始对象生命周期；但并不意味着任意类型的指针 cast 都自动安全。

## 11. Senior C++ / HFT 选型建议

| 需求 | 首选 |
|---|---|
| 已验证范围的数值转换 | `static_cast` |
| Derived* → Base* | 隐式 upcast |
| 动态类型不确定的 downcast | `dynamic_cast` 或重新设计分发 |
| 底层实际上为非 const 的对象，需要恢复可写访问 | 谨慎使用 `const_cast` |
| 读取对象的原始字节 | `std::as_bytes(std::span{&obj,1})` |
| `float` ↔ `uint32_t` 位解释 | `std::bit_cast` |
| 收到网络 buffer 后读取协议字段 | `std::memcpy` / 字节解码 |
| 复制动态长度的 byte buffer | `std::memcpy` |
| 处理重叠区域 | `std::memmove` |
| 复制 std::string/vector 等资源所有者 | 正确的 copy/move ctor 或 assignment |
| ABI / 内存地址相关特殊转换 | 经证明合法后使用 `reinterpret_cast` |

**HFT 提醒：** zero runtime cast cost 不等于后续内存访问合法，也不等于低延迟；网络输入处理应明确验证长度、字节序、对齐和对象模型边界。

## 12. 面试自测题（附答案）

### Q1. 修改 const_cast 后的指针是否一定 UB？

```cpp
int x = 10;
const int* p = &x;
int* q = const_cast<int*>(p);
*q = 20;
```

**答：不一定；本例 OK，因为 x 本身不是 const。**

### Q2. 真正 const 对象经 const_cast 写入？

```cpp
const int x = 10;
int* q = const_cast<int*>(&x);
*q = 20;
```

**答：UB。const_cast 只改变访问路径的限定，不改变底层对象 constness。**

### Q3. `float*` 强转成 `uint32_t*` 后能否解引用？

**答：不能仅凭大小相同就认为安全；通常违反 strict aliasing/type accessibility。推荐 `bit_cast` 或 `memcpy`。**

### Q4. `dynamic_cast<Derived*>(p)` 运行时失败？

**答：指针形式得到 nullptr；引用形式失败抛 std::bad_cast。**

### Q5. `static_cast<Derived*>(&base_obj)` 若实际对象是 Base？

**答：不满足向下转换前提，UB；不是只有解引用才会出问题。**

### Q6. `bit_cast` 是否可以处理 8 KB、1 MB 对象？

**答：没有语言规定的固定大小上限，只要类型、大小和表示要求满足；但不是 zero-copy view。**

### Q7. `memcpy` 是否要求源和目标 trivially copyable？

**答：memcpy API 本身不要求；但逐字节复制 non-trivially-copyable 资源所有者，并不能安全代替复制构造/赋值。**

### Q8. `bit_cast` 与 `memcpy` 哪个更快？

**答：不保证谁快；固定小尺寸通常可优化成相同指令。按语义和约束选 API，并按实际平台 benchmark。**

### Q9. 从 `buffer+1` 读取一个可能未对齐的 uint32_t？

**答：先 bounds check，再用 memcpy 到本地 uint32_t，随后处理 endian；避免强转 uint32_t* 解引用。**

### Q10. `reinterpret_cast<T*>` 是否会隐式创建一个 T 对象？

**答：cast 本身不会；需单独满足对象生命周期规则。**

## 13. 英文面试回答

> C++ offers four named casts. static_cast expresses conversions without runtime type checking. const_cast changes cv-qualification but does not change whether the underlying object is actually const. reinterpret_cast performs low-level conversions but does not bypass aliasing, alignment, or object-lifetime rules. dynamic_cast can check downcasts and cross-casts within polymorphic class hierarchies.
>
> For object-representation conversion, C++20 bit_cast requires both types to be trivially copyable and the same size and produces a new value. memcpy copies arbitrary bytes into storage without those template constraints, but byte copying is not equivalent to copying a non-trivial object's semantic state. Small fixed-size bit_cast and memcpy operations may produce equivalent optimized machine code.

## 参考资料

- [Implicit conversions](https://en.cppreference.com/w/cpp/language/implicit_conversion)
- [static_cast](https://en.cppreference.com/w/cpp/language/static_cast)
- [const_cast](https://en.cppreference.com/w/cpp/language/const_cast)
- [reinterpret_cast](https://en.cppreference.com/w/cpp/language/reinterpret_cast)
- [dynamic_cast](https://en.cppreference.com/w/cpp/language/dynamic_cast)
- [std::bit_cast](https://en.cppreference.com/w/cpp/numeric/bit_cast)
- [std::memcpy](https://en.cppreference.com/w/cpp/string/byte/memcpy)
