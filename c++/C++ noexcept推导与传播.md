# C++ noexcept：推导、传播与析构函数

## 1. 两种 noexcept

- `void f() noexcept;`：函数承诺异常不逃出；若逃出则 `std::terminate()`。
- `noexcept(expr)`：编译期判断表达式是否被认定为不抛异常，不执行表达式。

```cpp
void f() noexcept;
void g();
static_assert(noexcept(f()));
static_assert(!noexcept(g()));
```

普通手写函数**不会沿调用链自动推导**异常规格：

```cpp
void f() noexcept;
void g() { f(); } // g 仍然 potentially-throwing
```

## 2. 特殊成员函数的隐式异常规格

编译器隐式声明或**首次声明处** `= default` 的特殊成员函数，会依据其实际需要调用的基类、成员操作等推导异常规格。

```cpp
#include <type_traits>
struct Safe {
    Safe(const Safe&) noexcept = default;
};
struct X {
    Safe s;
    X(const X&) = default; // 推导为 noexcept
};
static_assert(noexcept(X(std::declval<const X&>())));
```

重要：**决定因素不是头文件还是 cpp，而是首次声明是否 defaulted。**

```cpp
struct A {
    A(const A&) = default; // 首次声明处 default：推导
};

struct B {
    B(const B&); // 首次声明：默认 potentially-throwing
};
inline B::B(const B&) = default; // 类外 default 不会重新推导 noexcept
```

若要类外定义不抛异常，应在两处声明：

```cpp
struct C { C(const C&) noexcept; };
inline C::C(const C&) noexcept = default;
```

手写构造函数即使内部只调用 noexcept 函数，也不会自动推导：

```cpp
struct D {
    int n;
    D(const D& rhs) : n(rhs.n) {} // potentially-throwing
};
```

## 3. 条件 noexcept：手动传播

```cpp
#include <utility>
template<class T>
void transfer(T& dst, T& src)
    noexcept(noexcept(dst = std::move(src))) {
    dst = std::move(src);
}
```

外层 `noexcept(...)` 是函数异常规格，内层 `noexcept(expr)` 是查询表达式。也可以使用 `std::is_nothrow_move_assignable_v<T>` 等 traits。

## 4. 析构函数 noexcept(false)

```cpp
struct A {
    ~A() noexcept(false) { /* 可能抛异常 */ }
};
struct B { A a; };
static_assert(!noexcept(std::declval<B&>().~B()));
```

成员析构可能抛异常会传播到隐式析构函数的异常规格。若析构函数在异常展开期间让另一个异常逃出，调用 `std::terminate()`。一般 RAII 析构应不抛异常；需要报告失败的操作应提供显式 `close()` 等接口。

**注意：** `noexcept(false)` 不意味着析构必然抛异常，只意味着允许异常逃出。

## 5. 类型特征的微妙之处

```cpp
struct T {
    T(const T&) noexcept = default;
    T(T&&) noexcept = default;
    ~T() noexcept(false) {}
};
static_assert(noexcept(T(std::declval<T&&>())));
// 但下面可能为 false；对这里的 T 确实为 false
static_assert(!std::is_nothrow_move_constructible_v<T>);
```

`noexcept(T(std::declval<T&&>()))` 对此表达式直接查询构造的异常属性；`is_nothrow_move_constructible` 依赖 `is_nothrow_constructible`，其构造测试也考虑析构是否可能抛异常。两者不可随意等同。

## 6. 对 vector 的影响

扩容时标准库实现通常倾向在元素 nothrow-movable 时移动；若移动可能抛异常且复制可用，可能复制以维护强异常保证。`std::move_if_noexcept` 体现这一策略：

```cpp
// 近似决策逻辑
if constexpr (std::is_nothrow_move_constructible_v<T> ||
              !std::is_copy_constructible_v<T>) {
    // 使用 T&&
} else {
    // 使用 const T&
}
```

具体容器行为及异常保证应以相应操作的标准要求为准，不能断言 vector 必然选择某一路径。

## 7. Resource Owner 设计原则

- 纯指针/长度所有权转移通常可以提供 `noexcept` move。
- 需要内存分配的构造、扩容、复制通常不能无条件 `noexcept`。
- 析构应尽可能不抛异常；释放失败通过显式 API 报告。
- `= default` 的特殊成员函数可推导异常规格，但前提是默认行为符合所有权语义。
- 不要为了让 traits 为 true 而错误地声明 `noexcept`；实际抛出将终止程序。

## 8. 面试检查清单

1. 区分 `noexcept` specifier 与 operator。
2. 区分普通函数、类内首次声明 defaulted、类外 defaulted。
3. 说明成员和基类异常规格如何传播。
4. 解释析构期间二次异常与 `std::terminate`。
5. 区分 `std::move` 类型转换和实际移动构造。
6. 解释 `move_if_noexcept` 与 vector 强异常保证的关系。

参考：
- https://en.cppreference.com/w/cpp/language/noexcept
- https://en.cppreference.com/w/cpp/types/is_constructible
- https://en.cppreference.com/w/cpp/utility/move_if_noexcept
