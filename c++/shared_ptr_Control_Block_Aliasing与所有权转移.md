# std::shared_ptr Control Block 复用与所有权转移

> C++ Production-Quality 专题：Aliasing Constructor、Converting Move Constructor、引用计数与生命周期。
>
> 核心结论：`shared_ptr` 可以让**存储指针（stored pointer）**和**控制块所管理的对象（managed object）**不同。借助 C++20 的 **move aliasing constructor**，甚至能把 control block 的所有权转交给任意类型的 `shared_ptr`，无需新分配 control block，也无需增加/减少共享引用计数。

## 1. 先区分两个概念

通常可以把 `shared_ptr<T>` 理解为持有两类信息（这是常见实现模型，不是标准强制的内存布局）：

- **stored pointer**：`get()` 返回的 `T*`，`operator*` / `operator->` 使用它。
- **ownership / control block**：负责共享所有权计数、deleter、最终销毁被管理对象；`make_shared` 通常会把对象和 control block 放在一次分配中。

两者**不必指向同一个对象**：

```text
shared_ptr<Message> owner
    stored pointer -----> Message
    control block  -----+
                        +----> control block (manages Message)
    control block  -----+
shared_ptr<Payload> alias
    stored pointer -----> Message::payload
```

最后一个 owner 消失时，由 control block 负责按原始管理方式销毁 `Message`；不会单独对 `payload` 执行 `delete`。

## 2. Aliasing Constructor（C++11）：共享 ownership

签名（简化）：

```cpp
template<class Y>
shared_ptr(const shared_ptr<Y>& owner, element_type* ptr) noexcept;
```

示例：

```cpp
#include <memory>

struct Payload { int id = 42; };
struct Message { Payload payload; };

auto owner = std::make_shared<Message>();
std::shared_ptr<Payload> alias(owner, &owner->payload);

// owner 和 alias 共享一个 control block
// owner.use_count() == 2
// alias.get() == &owner->payload
```

行为：

- 不分配新的 control block，也不复制 `Message` 或 `Payload`。
- **共享引用计数增加 1**（常见多线程实现中涉及原子 RMW）。
- `owner` 保持不变。
- `ptr` 不要求能由 `Y*` 转换到 `T*`；甚至可以是其他相关数据的指针，但调用者必须保证其有效期。

注意：`use_count()` 反映的是共享所有权数量，不能当作并发同步机制。

## 3. Move Aliasing Constructor（C++20）：转移 ownership

签名（简化）：

```cpp
template<class Y>
shared_ptr(shared_ptr<Y>&& owner, element_type* ptr) noexcept; // C++20
```

示例：

```cpp
#include <memory>
#include <utility>

struct Payload { int id = 42; };
struct Message { Payload payload; };

auto owner = std::make_shared<Message>();
Payload* p = &owner->payload; // 必须先取得指针

std::shared_ptr<Payload> alias(std::move(owner), p);

// owner 变为空；alias 接管之前的共享所有权
// alias.use_count() == 1
```

**区别于 copy aliasing**：

- 不新建 control block。
- 不增加或减少共享所有权引用计数（所有权本身被转移）。
- `owner` 移动后为空。
- 对典型实现而言相当于迁移 control-block pointer 并设置新的 stored pointer；但 **“零内存操作”并不准确**：复制/写入对象内部指针仍是内存操作。更准确地说是 **无新增堆分配、无引用计数增减**。

特别注意：先保存 `p`，再 `std::move(owner)`。不要写成

```cpp
// 不推荐：参数求值顺序不应成为正确性前提
std::shared_ptr<Payload> alias(std::move(owner), &owner->payload);
```

因为函数实参求值次序不能用来保证 `owner` 在取地址时尚未被移动。

## 4. Converting Move Constructor：可转换类型之间转移

```cpp
#include <memory>
#include <utility>

struct Base { virtual ~Base() = default; };
struct Derived : Base {};

std::shared_ptr<Derived> d = std::make_shared<Derived>();
std::shared_ptr<Base> b(std::move(d));
```

其行为是：

- 不产生新的 control block。
- 不增加/减少引用计数。
- 源 `d` 变为空。
- 要求指针类型满足相应的兼容转换（例如 `Derived*` -> `Base*`）。

与之相比，**move aliasing 不要求 `Y*` 和 `T*` 可相互转换**，因为它显式接收目标 stored pointer。

## 5. 对比表

| 操作 | 目标类型 | 新 control block | 共享引用计数 | 源对象 |
| --- | --- | --- | --- | --- |
| `shared_ptr<Base> b(d)` | 可转换类型 | 否 | +1 | 不变 |
| `shared_ptr<Base> b(std::move(d))` | 可转换类型 | 否 | 不变 | 变空 |
| `shared_ptr<T> a(owner, ptr)` | 任意 `T*` | 否 | +1（owner 非空） | 不变 |
| `shared_ptr<T> a(std::move(owner), ptr)` | 任意 `T*` | 否 | 不变 | 变空 |

表格描述的是正常、非空 ownership 的情况；空 owner 也可以参与 aliasing，而这会带来额外陷阱。

## 6. Production 场景：返回子对象，同时保留整个消息的生命周期

```cpp
#include <memory>
#include <utility>

struct Header { int sequence = 0; };
struct Payload { int price = 0; };
struct Message {
    Header header;
    Payload payload;
};

std::shared_ptr<const Payload>
extractPayload(std::shared_ptr<Message> msg) noexcept
{
    const Payload* p = &msg->payload; // 前提：msg 非空
    return std::shared_ptr<const Payload>(std::move(msg), p);
}
```

这个例子把原本 `Message` 的 ownership 转给返回的 `shared_ptr<const Payload>`。

**API 注意**：以上函数要求 `msg != nullptr`。若它是公开边界 API，应显式定义空指针行为，例如：

```cpp
std::shared_ptr<const Payload>
extractPayload(std::shared_ptr<Message> msg) noexcept
{
    if (!msg)
        return {};

    const Payload* p = &msg->payload;
    return {std::move(msg), p}; // C++20 move aliasing
}
```

这不会复制 Payload，且在转移阶段不需要新增 allocation 或引用计数操作。不过**调用方在按值传入左值 shared_ptr 时会先复制它、增加一次引用计数**：

```cpp
auto msg = std::make_shared<Message>();

auto a = extractPayload(msg);            // 传参复制：引用计数 +1
auto b = extractPayload(std::move(msg)); // 传参移动：不增加引用计数
```

## 7. 容易踩的坑

### 7.1 owner 保活的只是 managed object，未必保活 ptr

如果 `ptr` 指向 `Message` 内可提前释放或失效的动态资源，aliasing 不会自动延长那个资源的生命周期：

```cpp
struct Message {
    std::unique_ptr<Payload> payload = std::make_unique<Payload>();
};

auto owner = std::make_shared<Message>();
std::shared_ptr<Payload> alias(owner, owner->payload.get());

owner->payload.reset(); // alias 现在悬空，即便 control block 仍存活
```

因此最典型、安全的用法是指向与被管理对象同生共死的稳定子对象。

### 7.2 owner 为空，但 alias.get() 非空

```cpp
int value = 7;
std::shared_ptr<int> empty;
std::shared_ptr<int> alias(empty, &value);

assert(alias.get() == &value);
assert(alias.use_count() == 0);
assert(static_cast<bool>(alias));
```

这种 alias **不拥有任何对象**，不能依赖它延长 `value` 的寿命。注意 `operator bool()` 检查 stored pointer 是否非空，而不是是否拥有 control block。

### 7.3 `get()` 相同不代表共享 ownership；`get()` 不同也可能共享 ownership

判断是否共享 ownership 可以使用 `owner_before()` 定义的所有权等价关系，而不能只比较 `get()`。

```cpp
template<class A, class B>
bool sameOwner(const std::shared_ptr<A>& a,
               const std::shared_ptr<B>& b) noexcept
{
    return !a.owner_before(b) && !b.owner_before(a);
}
```

### 7.4 thread safety 与 low-latency

- 以移动方式构造只是避免了**这次**共享引用计数增减，不代表对象的后续复制/析构没有成本。
- 不同 `shared_ptr` 实例共享同一个 control block 时，引用计数管理是线程安全的；但**并发修改同一个 `shared_ptr` 实例**仍需要同步，或使用合适的 `std::atomic<std::shared_ptr<T>>`。
- `shared_ptr` 不自动保护指向对象的业务字段不被并发修改。
- 热路径若不需要跨生命周期的共享所有权，`T&`、`T*`、`std::span<T>` 通常更轻量，但必须由其他机制保证生命周期。

## 8. 面试要点

**问题：不同类型的 shared_ptr 能不能共享/转移 control block，而不触发 heap allocation 或原子计数操作？**

回答分三层：

1. **Aliasing constructor（C++11）**：可以指向任意 `T*` 并共享现有 control block，无新分配，但 copy aliasing 会增加共享引用计数。
2. **Move aliasing constructor（C++20）**：可将现有 ownership 转移到 `shared_ptr<T>`，无新分配且不需要引用计数增减。
3. **Converting move constructor**：同样不需要新分配或增减计数，但要求指针类型可兼容转换；move aliasing 更灵活。

最后补充正确性约束：**control block 控制的是 managed object 的生命周期，不保证显式传入的 `ptr` 始终有效**。

## 参考

- [cppreference: std::shared_ptr constructors](https://en.cppreference.com/w/cpp/memory/shared_ptr/shared_ptr)
- [cppreference: std::shared_ptr owner_before](https://en.cppreference.com/w/cpp/memory/shared_ptr/owner_before)
- C++20: rvalue aliasing overload of `std::shared_ptr` constructor
