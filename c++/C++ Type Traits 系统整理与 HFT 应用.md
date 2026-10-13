# C++ Type Traits 系统整理（HFT / Senior C++ 面试）

> C++17/20/23；重点是类型性质、约束、STL 行为、对象生命周期和低延迟工程实践。

## 1. 速查：优先级

| 等级 | Traits | 关键问题 |
|---|---|---|
| 必须 | `is_same`, `is_constructible`, `is_convertible` | 模板 API 的合法性与隐式转换 |
| 必须 | `is_copy/move_constructible`, `is_copy/move_assignable` | 构造和赋值的区别 |
| 必须 | `is_nothrow_move_constructible`, `is_nothrow_move_assignable` | 异常安全、容器迁移 |
| 必须 | `is_trivially_copyable`, `is_standard_layout` | 对象表示、字段偏移 |
| 必须 | `is_trivially_default_constructible`, `is_trivially_destructible` | 内存池、生命周期 |
| 必须 | `remove_cvref`, `decay`, `conditional` | 泛型类型变换 |
| 熟练 | `is_invocable`, `invoke_result`, `is_nothrow_invocable` | Callable 泛型 |
| 熟练 | `is_integral`, `is_enum`, `is_pointer`, `is_reference`, `is_const`, `is_base_of` | 分类与约束 |
| 扩展 | `is_aggregate`, `is_empty`, `is_final`, `has_unique_object_representations` | 布局与对象表示 |

大多数布尔 traits 在 C++17 可使用 `_v`；类型变换使用 `_t`。

## 2. Copy / Move / Exception Safety

```cpp
#include <type_traits>
#include <utility>

static_assert(std::is_move_constructible_v<int>);
static_assert(std::is_nothrow_move_constructible_v<int>);
static_assert(std::is_nothrow_move_assignable_v<int>);
```

- `is_nothrow_move_constructible_v<T>` 检查 `T(std::declval<T&&>())` 是否有效且不抛异常。
- `is_nothrow_move_assignable_v<T>` 检查 `std::declval<T&>() = std::declval<T&&>()` 是否有效且不抛异常。
- 两者检测表达式，不保证类实际声明了 move 成员；`const T&` copy 成员可能接受右值。
- `std::vector` 扩容通常需要在新存储上**构造**元素；`erase` 通常需要对现存元素**赋值**。
- `std::move_if_noexcept(x)`：若 nothrow move 或不可 copy，返回右值引用；否则返回 `const T&`。它不执行移动。
- `noexcept` 是契约：异常逃逸将导致 `std::terminate`，不是“捕获异常”。

```cpp
struct CopyOnly {
    CopyOnly(const CopyOnly&) noexcept = default;
};
static_assert(std::is_nothrow_move_constructible_v<CopyOnly>);
// 并没有显式 move constructor
```

资源类通常设计 `noexcept` move ctor/assignment，但仅当释放旧资源和所有其他步骤确实不会抛异常时。

## 3. Trivial、Trivially Copyable、Standard Layout、POD

```cpp
struct Message {
    unsigned long long id;
    int price;
    unsigned qty;
};
static_assert(std::is_trivially_copyable_v<Message>);
static_assert(std::is_standard_layout_v<Message>);
static_assert(std::is_trivially_default_constructible_v<Message>);
```

- **Trivially copyable**：可以把一个有效对象的底层字节复制到另一个同类型对象（或复制回来）以保留值；不等于可以随意把网络字节转成对象，也不等于线程安全。
- **Standard layout**：为 `offsetof` 和某些首成员 pointer-interconvertibility 提供保证；不保证没有 padding、固定 sizeof、跨 ABI 一致或 memcpy 合法。
- **Trivial default construction**：默认构造不需要用户定义逻辑；`T x;` 对简单标量成员不保证初始化，`T x{};` 通常会零初始化这些成员。
- **Trivial destruction**：无须执行非平凡析构逻辑；但不能忽略对象生命周期和存储回收规则。
- **POD**：历史概念，C++20 已弃用 `is_pod`；应按需求选择更精确 traits。

```cpp
struct A { int x; };
struct B { int x = 0; };
struct C { int x; C() : x(0) {} };

static_assert(std::is_trivially_default_constructible_v<A>);
static_assert(!std::is_trivially_default_constructible_v<B>);
static_assert(!std::is_trivially_default_constructible_v<C>);
static_assert(std::is_trivially_copyable_v<B>);
static_assert(std::is_trivially_copyable_v<C>);
```

**HFT 场景**：
- SPSC ring slot：trivially copyable 消息便于预分配和按值传递；必须独立处理 release/acquire、slot ownership 和生命周期。
- L3 OrderBook intrusive node：即使节点 trivially copyable，复制链表指针也可能破坏链表不变量；语言可复制不等于语义上可复制。
- Shared memory：优先固定大小值与 offset；普通指针跨进程可能无效；原子跨进程同步须依赖具体平台/实现保证。
- Wire format：必须明确 endian、padding、alignment、schema/version；`standard_layout` 与 `trivially_copyable` 均不足以定义网络协议。

## 4. 类型变换

```cpp
using A = const int&;
static_assert(std::is_same_v<std::remove_cvref_t<A>, int>);
static_assert(std::is_same_v<std::decay_t<A>, int>);

using Arr = const int[10];
static_assert(std::is_same_v<std::remove_cvref_t<Arr>, int[10]>);
static_assert(std::is_same_v<std::decay_t<Arr>, const int*>);
```

- `remove_reference_t`：去掉引用。
- `remove_cv_t`：去掉顶层 cv。
- `remove_cvref_t`（C++20）：去引用再去顶层 cv。
- `decay_t`：另会把数组转指针、函数转函数指针。
- `conditional_t<B,A,C>`：编译期选择类型。
- `underlying_type_t<E>`：枚举底层类型。
- `common_type_t<T,U>`：推导共同类型（不保证适合所有业务语义）。

## 5. 分类、可调用与泛型约束

常用分类：`is_integral`、`is_floating_point`、`is_arithmetic`、`is_enum`、`is_pointer`、`is_reference`、`is_lvalue_reference`、`is_rvalue_reference`、`is_const`、`is_class`、`is_base_of`。

```cpp
using R = const int&;
static_assert(!std::is_const_v<R>);
static_assert(std::is_const_v<std::remove_reference_t<R>>);

template<class F>
void run(F&& f) noexcept(std::is_nothrow_invocable_v<F&>) {
    std::forward<F>(f)();
}
```

- `is_invocable_v<F,Args...>`：`std::invoke` 是否有效。
- `is_nothrow_invocable_v`：调用是否不抛异常。
- `invoke_result_t`：调用返回类型。
- `is_invocable_r_v<R,...>`：调用结果是否满足目标返回类型要求。
- `void_t`：C++17 detection idiom 基础；`is_detected` **不是**标准 `std` trait。
- `enable_if_t`：SFINAE；C++20 通常优先 `requires` / Concepts 表达约束。

```cpp
#include <concepts>
template<std::integral T>
T add(T a, T b) { return a + b; }

template<class T>
requires std::is_trivially_copyable_v<T>
void copy_value(T& dst, const T& src) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(&dst, &src, sizeof(T)); // 需 #include <cstring>
}
```

## 6. 其他值得了解

- `is_aggregate_v`：能否 aggregate initialization。
- `is_empty_v`：空类；联系 EBO 和 `[[no_unique_address]]`，不能假设一定省空间。
- `is_final_v`、`is_polymorphic_v`、`is_abstract_v`：类层次。
- `has_unique_object_representations_v`：同值对应唯一对象表示的性质；仍需明确比较语义。
- `alignment_of_v<T>`：等价于 `alignof(T)` 的 trait 形式。
- `is_scoped_enum_v`（C++23）。
- `is_trivial` 在 C++26 被弃用。

## 7. Standard Layout 与 extern "C"

`extern "C"` 控制 language linkage（常见体现为 C 符号命名及函数类型的语言链接）；它不自动保证 C/C++ 结构体二进制布局一致。

- `extern "C"`：函数接口的链接。
- 平台 C ABI：调用约定、参数传递。
- Standard Layout + 具体 ABI 检查：结构体布局的部分依据。
- C 接口若不需要共享结构体布局，优先 opaque handle（`struct EngineHandle;`）。

## 8. 高频面试问题

1. 为什么 `vector` 有 move constructor 仍可能 copy？——异常安全与 `move_if_noexcept`。
2. `is_nothrow_move_assignable` 和 `is_nothrow_move_constructible` 有何不同？——现有对象赋值 vs 新存储构造。
3. `is_move_constructible` 为 true 是否证明有 move ctor？——否，copy ctor 也可能匹配右值。
4. Trivially copyable 是否意味着 `memcmp` 可以比较值？——否，padding、表示与语义需要考虑。
5. Standard layout 是否保证固定大小或无 padding？——否。
6. `remove_cvref` 与 `decay` 区别？——数组/函数退化。
7. 为什么 HFT 不应该一律追求 POD？——应选择实际操作需要的性质，并保持数据结构语义。
8. Traits vs Concepts？——Traits 查询/变换类型；Concepts 约束模板接口。

## 9. 推荐练习

实现 `mini_vector<T>`，覆盖：
- `reserve` 扩容与 `std::move_if_noexcept`；
- 构造失败 rollback、析构已构造元素；
- `erase` 与 move assignment；
- allocator propagation 和 exception guarantee；
- 用 `static_assert` 检查 type traits；
- 使用 sanitizer 与抛异常的测试类型验证正确性。

**原则**：Traits 首先表达可验证的类型性质与 API 契约；性能提升要由实际机器码和 benchmark 证明。
