# C++ decltype 与 decltype(auto) 总结

> 适用标准：C++11–C++20。重点：精确类型推导、表达式值类别、泛型返回类型、SFINAE、Concepts 和生命周期安全。

## 1. 核心概念

- \`decltype(expr)\` 在编译期得到表达式对应的类型，表达式通常处于未求值上下文。
- \`decltype(auto)\` 使用 \`decltype\` 规则推导变量或函数返回类型（C++14）。
- \`auto\` 的推导规则不同：按值推导通常忽略引用与顶层 cv 限定。
- 最大陷阱：\`decltype(x)\` 和 \`decltype((x))\` 可能不同。

## 2. decltype 两套精确规则

**规则 A**：如果操作数是不加括号的 id-expression，或不加括号的类成员访问表达式，则返回所指实体的*声明类型*；例如：

~~~cpp
int x = 1;
int& lref = x;
int&& rref = 1;
const int cx = 2;
struct S { int field; };
S s;

static_assert(std::is_same_v<decltype(x), int>);
static_assert(std::is_same_v<decltype(lref), int&>);
static_assert(std::is_same_v<decltype(rref), int&&>);
static_assert(std::is_same_v<decltype(cx), const int>);
static_assert(std::is_same_v<decltype(s.field), int>);
~~~

**规则 B**：其他表达式根据值类别决定结果（设其非引用类型为 \`T\`）。

| 表达式值类别 | \`decltype(expr)\` |
| --- | --- |
| lvalue | \`T&\` |
| xvalue | \`T&&\` |
| prvalue | \`T\` |

~~~cpp
int x = 1;
int&& rr = 2;
int* p = &x;
static_assert(std::is_same_v<decltype((x)), int&>);
static_assert(std::is_same_v<decltype((rr)), int&>);
static_assert(std::is_same_v<decltype(std::move(x)), int&&>);
static_assert(std::is_same_v<decltype(x + 1), int>);
static_assert(std::is_same_v<decltype(*p), int&>);
static_assert(std::is_same_v<decltype(++x), int&>);
static_assert(std::is_same_v<decltype(x++), int>);
~~~

**为什么括号重要？** \`x\` 符合规则 A；\`(x)\` 不再符合 A，转而走规则 B。括号本身并没有改变表达式的值类别。

注意：命名的右值引用变量用作表达式仍是 **lvalue**，所以 \`decltype(rr)\` 是 \`int&&\`，\`decltype((rr))\` 是 \`int&\`。

## 3. auto / decltype(auto) / auto& / auto&&

~~~cpp
int x = 1;
int& ref = x;
const int cx = 2;

auto a = ref;              // int，复制
auto& b = ref;             // int&
auto&& c = ref;            // int&，引用折叠
decltype(auto) d = ref;    // int&，声明类型规则
decltype(auto) e = x;      // int
decltype(auto) f = (x);    // int&，值类别规则
decltype(auto) g = (cx);   // const int&
~~~

| 写法 | 典型语义 |
| --- | --- |
| \`auto a = expr;\` | 值推导，通常去掉顶层 cv 和引用 |
| \`auto& a = expr;\` | 左值引用推导 |
| \`auto&& a = expr;\` | 推导场景中可能是 forwarding reference |
| \`decltype(auto) a = expr;\` | 根据整个初始化表达式应用 decltype 规则 |

注意：\`decltype(auto)&\` 和 \`decltype(auto)&&\` 都是无效语法。

## 4. 四种返回类型组合：面试必会

~~~cpp
template<class T>
auto f1(T& x) { return x; }              // T

template<class T>
auto f2(T& x) { return (x); }            // T

template<class T>
decltype(auto) f3(T& x) { return x; }    // T&

template<class T>
decltype(auto) f4(T& x) { return (x); }  // T&
~~~

为什么 \`f3\` 和 \`f4\` 都返回 \`T&\`？

- \`decltype(x)\` 命中规则 A；函数形参 \`x\` 声明为 \`T&\`。
- \`decltype((x))\` 命中规则 B；命名形参表达式是 lvalue。
- 普通 \`auto\` 返回类型推导丢弃引用，\`f1\` 与 \`f2\` 都按值返回。

**对比一个真正不同的例子：**

~~~cpp
decltype(auto) good() {
    int local = 10;
    return local;    // int，按值
}

decltype(auto) bad() {
    int local = 10;
    return (local);  // int&，悬空引用：不可使用返回结果
}
~~~

## 5. 泛型访问器与完美转发

~~~cpp
template<class F, class... Args>
decltype(auto) invoke_exact(F&& f, Args&&... args) {
    return std::forward<F>(f)(std::forward<Args>(args)...);
}
~~~

返回类型 \`decltype(auto)\` 可保留被调用函数返回的值、左值引用或右值引用。

一个需要格外注意的访问器：

~~~cpp
template<class T>
decltype(auto) get(T&& obj) {
    return std::forward<T>(obj).get();
}
~~~

它可能返回引用，因此调用者必须确保被引用对象仍然存活。对临时对象的访问如果暴露内部引用，可能产生悬空引用。

## 6. decltype、std::declval 与类型检测

\`std::declval<T>()\` 不创建对象，只可在未求值上下文中使用（需要 \`<utility>\`）。

~~~cpp
#include <type_traits>
#include <utility>
#include <vector>

template<class T>
using size_result_t = decltype(std::declval<T&>().size());

static_assert(std::is_same_v<
    size_result_t<std::vector<int>>,
    std::vector<int>::size_type
>);
~~~

与 \`std::void_t\` 配合检测成员函数（C++17）：

~~~cpp
template<class T, class = void>
struct has_size : std::false_type {};

template<class T>
struct has_size<T,
    std::void_t<decltype(std::declval<T&>().size())>>
    : std::true_type {};

static_assert(has_size<std::vector<int>>::value);
static_assert(!has_size<int>::value);
~~~

注意：检测的是 **非 const 左值** 上能否调用 \`size()\`。改为 \`std::declval<const T&>()\` 才是检查 const 对象。

## 7. decltype + 逗号运算符 + SFINAE

~~~cpp
template<class T>
auto test_size(T& value)
    -> decltype((void)value.size(), void())
{
    // 只有 value.size() 合法时才参与重载决议
}
~~~

- \`(void)value.size()\` 要求 \`size()\` 表达式有效，并避免用户自定义 \`operator,\` 的干扰。
- 内置逗号表达式结果由右操作数 \`void()\` 决定，返回类型是 \`void\`。
- SFINAE 仅在相应的模板实参替换上下文中生效；并非所有模板内部错误都能被 SFINAE 吞掉。

## 8. C++20 Concepts / requires

~~~cpp
#include <concepts>
#include <cstddef>

template<class T>
concept HasSize = requires(T& t) {
    t.size();
};

template<class T>
concept Sized = requires(const T& t) {
    { t.size() } -> std::convertible_to<std::size_t>;
};

template<class T>
concept IntegralSize = requires(T& t) {
    requires std::integral<decltype(t.size())>;
};
~~~

- 简单 requirement 检查表达式有效性。
- 复合 requirement 中的 \`->\` 约束表达式结果类型。
- 嵌套 requirement 能显式使用 \`decltype\` 判断返回类型。

## 9. cv/ref-qualified 成员函数

~~~cpp
#include <utility>
#include <type_traits>
struct Data {
    int& get() &;
    const int& get() const &;
    int&& get() &&;
};

static_assert(std::is_same_v<
    decltype(std::declval<Data&>().get()), int&>);
static_assert(std::is_same_v<
    decltype(std::declval<const Data&>().get()), const int&>);
static_assert(std::is_same_v<
    decltype(std::declval<Data&&>().get()), int&&>);
~~~

泛型 wrapper 与 accessor 必须考虑对象的 const、lvalue/rvalue 状态，不能只按 \`T\` 粗略决定返回类型。

## 10. 未求值上下文的边界

~~~cpp
int x = 0;
using R = decltype(++x);  // int&；++x 不执行
~~~

但未求值 **不等于不检查合法性**：

~~~cpp
struct S {};
// using R = decltype(S{}.missing()); // 编译错误
~~~

C++17 起，\`decltype\` 顶层 prvalue 操作数有特殊的非物化规则；即使结果类型不能复制/移动，也通常可以求得其类型：

~~~cpp
struct Immovable {
    Immovable() = default;
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    ~Immovable() = default;
};
using R = decltype(Immovable{}); // OK
~~~

该特殊规则不必然适用于更深层的子表达式；析构函数仍需满足相关可访问性等要求。

## 11. 实践陷阱

### 11.1 不要无意间返回局部对象的引用

~~~cpp
decltype(auto) bad() {
    int x = 0;
    return (x); // dangling
}
~~~

### 11.2 别把 decltype(auto) 当作万能 forwarding

它**精确保留表达式类型**，但不能替你管理生命周期，也不能自动修复内部 getter 返回临时对象引用等错误。

### 11.3 ref-qualified 参数的声明类型和值类别不同

~~~cpp
template<class T>
void inspect(T&& x) {
    using A = decltype(x);               // T&&（引用折叠后）
    using B = decltype((x));             // 始终是左值引用
    using C = decltype(std::forward<T>(x)); // 按 T 恢复值类别
}
~~~

### 11.4 auto 返回值可能复制，且不可复制类型可能直接编译失败

~~~cpp
struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
};

template<class T>
auto copy_value(T& x) { return x; }
// MoveOnly m; copy_value(m); // 需要从左值复制：编译失败
~~~

## 12. 面试速查表

| 表达式 | 结果 |
| --- | --- |
| \`decltype(x)\`，\`int x\` | \`int\` |
| \`decltype((x))\` | \`int&\` |
| \`decltype(r)\`，\`int& r\` | \`int&\` |
| \`decltype(rr)\`，\`int&& rr\` | \`int&&\` |
| \`decltype((rr))\` | \`int&\` |
| \`decltype(std::move(x))\` | \`int&&\` |
| \`decltype(x + 1)\` | \`int\` |
| \`decltype(*ptr)\`，\`int* ptr\` | \`int&\` |
| \`decltype(&x)\` | \`int*\` |
| \`decltype(++x)\` | \`int&\` |
| \`decltype(x++)\` | \`int\` |
| \`decltype(std::as_const(x))\` | \`const int&\` |

## 13. 练习题（含答案）

**Q1.** \`int&& r = 42; decltype((r))\` 是什么？

**A1.** \`int&\`。命名变量表达式是 lvalue。

**Q2.** \`T& x\` 参数下，\`decltype(auto) f(){ return x; }\` 和 \`return (x)\` 有区别吗？

**A2.** 若 \`x\` 是 \`T&\` 形参，两者都为 \`T&\`；规则分别是声明类型和表达式值类别。

**Q3.** \`decltype(auto) f(){ int x=1; return (x); }\` 有什么问题？

**A3.** 返回局部对象的悬空引用。

**Q4.** \`std::declval<T&>()\` 会构造 \`T\` 吗？

**A4.** 不会；只能在未求值上下文使用。

**Q5.** 如何检查对象 \`size()\` 的返回类型必须是整数类型？

**A5.** 使用 \`requires(T& t) { requires std::integral<decltype(t.size())>; }\`。

---

## 记忆口诀

1. **裸名字 → 声明类型**；其他表达式 → **看值类别**。
2. **lvalue → &，xvalue → &&，prvalue → 值类型**。
3. **auto 常按值；decltype(auto) 精确保留**。
4. **引用保留意味着生命周期责任，不能只看编译是否通过**。
