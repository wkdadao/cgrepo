# HFT C++ 类设计原则（Class Design Policy）

> 面向 Linux / C++17–20、低延迟行情、定价、订单管理与网关系统。
>
> **核心原则：先明确语义与生命周期，再决定 Copy / Move；优先 Rule of Zero、RAII、显式 Ownership 和稳定的 Hot Path。**
>
> 本文是**默认设计策略**，不是 C++ 语言强制规定。需要以具体对象的不变量、线程模型、性能数据为准。

## 1. 从 Data / Tool 出发，但不要把类别当成互斥集合

一个类至少要从以下四个维度判断：

| 维度 | 核心问题 |
| --- | --- |
| **Value / Identity** | 复制它是否代表一个新的独立值？它是否有外部可观察的身份？ |
| **Ownership** | 它是否拥有内存、FD、连接、线程或其他资源？ |
| **Lifetime / Address Stability** | 谁创建和销毁？是否存在长期引用或固定地址约束？ |
| **Concurrency / Affinity** | 由哪个线程修改？是否要求线程绑定或同步？ |

**Data ≠ Resource。Value Semantics 与 Resource Ownership 可以重叠。**

- `Price`：通常是 Value，不拥有独立资源。
- `std::vector<Order>`：既是 Value，也管理动态内存。
- `std::span<Order>`：是可复制的 View，但不拥有底层数据。
- `SocketHandle`：通常是独占资源所有者，Move-only。
- `MarketDataSession`：有连接、序号、订阅等生命周期及身份约束。
- `Parser`：可能是无状态的轻量 Tool，也可能是有状态的流式 Parser。

## 2. 推荐默认分类

| 类型 | Copy | Move | 默认管理方式 | 示例 |
| --- | --- | --- | --- | --- |
| **Data / Value** | 通常允许 | 通常允许 | 直接成员 / 局部对象；Rule of Zero | `Price`、`Quote`、`OrderRequest` |
| **Resource-owning Value** | 取决于业务语义 | 通常允许 | 标准 RAII 成员 | `std::vector`、`std::string` |
| **Unique Resource Owner** | 禁止 | 通常允许 | RAII Wrapper / `unique_ptr` | FD、mmap region、独占缓冲区 |
| **Service / Identity** | 默认禁止 | 默认禁止 | 直接成员或 `unique_ptr` | `PricingEngine`、`MarketDataSession` |
| **Lightweight Stateless Tool** | 通常允许 | 通常允许 | 局部变量或 Free Function | Validator、Comparator |
| **Heavyweight Shared Tool** | 依语义决定 | 依语义决定 | Context-owned，必要时进程级共享 | 预计算表、配置解析基础设施 |
| **Non-owning View** | 通常允许 | 通常允许 | `span` / `string_view` / reference | BufferView、MessageView |

这里的“允许/禁止”是**API 的语义承诺**，不能仅凭成员类型是否可复制或可移动做决定。

## 3. Data / Value：优先 `struct` + Rule of Zero

```cpp
#include <cstdint>
#include <type_traits>

struct Quote {
    std::uint32_t instrument_id{};
    std::int64_t bid_ticks{};
    std::int64_t ask_ticks{};
    std::uint32_t quantity{};
};

static_assert(std::is_trivially_copyable_v<Quote>);
```

设计原则：

1. 用值表达业务数据；Copy 得到一个独立的值。
2. 默认构造、Copy / Move、析构由编译器生成，**不手写特殊成员函数**。
3. 对 Hot Path 消息尽量使用明确的整数单位、ID、固定尺寸数据，避免不必要的动态分配。
4. 需要二进制传输时，**trivially copyable 只是一个相关条件，不代表可直接用 `reinterpret_cast` 解析网络字节**；还需考虑端序、对齐、对象生命周期、填充与协议版本。
5. 大型 Value（如含 `vector` 的快照）即使语义上可复制，复制成本也可能很高；应通过 API / Profiling 控制频繁复制。

`struct` 和 `class` 的语言层面主要区别是默认访问权限与默认继承权限；用 `struct` 表示 Data 只是团队风格约定。

### 不拥有数据的 View

```cpp
#include <cstddef>
#include <span>

struct PacketView {
    std::span<const std::byte> payload;
};
```

复制 `PacketView` **只复制视图**，不会复制底层字节；必须确保底层存储在 View 使用期间仍然有效。这是 HFT 解码、零拷贝接口中常见的 Lifetime 风险。

## 4. Resource Owner：优先标准 RAII Container / Wrapper

**首选组合已有 RAII 类型，而不是亲自写 `new/delete` 或 `malloc/free`。**

```cpp
#include <cstddef>
#include <memory>
#include <vector>

class MessageBuffer {
public:
    explicit MessageBuffer(std::size_t capacity)
        : storage_(capacity) {}

    std::size_t capacity() const noexcept { return storage_.size(); }

private:
    std::vector<std::byte> storage_;
};

// 独占资源所有权：默认生成的 move 可用于 unique_ptr 成员
class ScratchBuffer {
public:
    explicit ScratchBuffer(std::size_t capacity)
        : storage_(std::make_unique<std::byte[]>(capacity)) {}

    ScratchBuffer(const ScratchBuffer&) = delete;
    ScratchBuffer& operator=(const ScratchBuffer&) = delete;
    ScratchBuffer(ScratchBuffer&&) noexcept = default;
    ScratchBuffer& operator=(ScratchBuffer&&) noexcept = default;

private:
    std::unique_ptr<std::byte[]> storage_;
};
```

- `MessageBuffer` 是 Resource-owning Value：默认可 Copy/Move，维持 Rule of Zero。
- `ScratchBuffer` 是 Move-only Owner：不能 Copy；Move 转移其唯一资源。
- 自定义 FD、`mmap`、Aeron / NIC / Huge Page 资源时，用专用 RAII Wrapper，并审查创建失败、清理失败及 move 后状态。
- 使用 `unique_ptr` 并不意味着资源一定要是通用堆内存：它也能绑定自定义 Deleter；但并非所有句柄都适合直接表示成指针。
- **警惕默认 Move 破坏类不变量**：例如同时保存 `unique_ptr<T[]>` 和 `size_`，默认 Move 只移动指针，却复制整数；源对象的 `size_` 可能不再反映其真实资源状态。必要时手写 Move 或只保留能一致表达状态的成员。

### 自定义分配器和内存池

在 HFT 中，`vector` / flat hash map / intrusive container 等可以结合自定义 Allocator、`pmr`、pool、slab、arena 或 Huge Pages。

- **Allocator 的生命周期必须覆盖依赖它的 Container**。
- 明确对齐、块尺寸、回收策略、容量和线程归属。
- Stateful Allocator 需要满足容器的传播及释放兼容性要求（尤其 Copy / Move / Swap）。
- 预分配减少 Hot Path 分配，但 `reserve` 不代表无限容量，也不保证元素地址永久稳定。

## 5. Service / Identity：默认禁止 Copy 和 Move

适用于行情 Session、订单网关、定价引擎、线程绑定状态机、具有外部长期引用的组件。

```cpp
#include <cstdint>

class MarketDataSession final {
public:
    MarketDataSession() = default;

    MarketDataSession(const MarketDataSession&) = delete;
    MarketDataSession& operator=(const MarketDataSession&) = delete;
    MarketDataSession(MarketDataSession&&) = delete;
    MarketDataSession& operator=(MarketDataSession&&) = delete;

    void onMessage(std::uint64_t sequence) noexcept {
        next_sequence_ = sequence + 1;  // 仅示意；真实实现需处理溢出及 gap
    }

private:
    std::uint64_t next_sequence_{1};
};
```

默认禁止 Copy / Move 的**原因是语义约束**，而非“Move 必然昂贵”：

- Session ID、订单/序号状态及独立身份。
- 外部组件保存指针、引用或回调。
- 内部 self-reference、intrusive list、注册表或异步任务。
- Thread Affinity、锁、socket/event loop 等非平凡生命周期。
- 对象地址在整个运行周期必须稳定。

**Non-movable 不等于必须 Heap Allocation。** 以下都合法：

```cpp
MarketDataSession local_session; // 固定作用域，地址稳定

struct TradingContext {
    MarketDataSession session;    // 直接嵌入，随 Context 生灭
};
```

需要动态生命周期、可转移 Ownership、容器管理或运行时创建数量时：

```cpp
#include <memory>
#include <vector>

std::unique_ptr<MarketDataSession> session =
    std::make_unique<MarketDataSession>();

std::vector<std::unique_ptr<MarketDataSession>> sessions;
// vector 扩容会移动 unique_ptr，但不移动其指向的 Session。
```

**例外**：一个 Service 如果具有清晰、可保持不变量的 Move 语义，也可以支持 Move。禁止 Move 应是默认审慎选择，而非教条。

> 显式 `= delete` 用于表达 API 意图；它严格说不再是“完全不声明特殊成员”的 Rule of Zero，但也不意味着需要实现 Rule of Five。

## 6. Tool：轻量局部创建，重型 Context-owned

### 6.1 无状态且轻量：优先 Free Function 或局部对象

```cpp
struct PriceValidator {
    bool valid(std::int64_t bid, std::int64_t ask) const noexcept {
        return bid > 0 && ask >= bid;
    }
};

bool validateQuote(std::int64_t bid, std::int64_t ask) noexcept {
    return bid > 0 && ask >= bid;
}

void processQuote() {
    PriceValidator validator;
    const bool ok = validator.valid(100, 101);
    (void)ok;
}
```

- 没有状态、无需构造成本时，Free Function 往往最简单。
- 小 Tool 局部创建，代码清晰、依赖最少，编译器经常能内联/优化。
- **Stateless Tool 并不天然需要禁止 Copy/Move**。

### 6.2 构造昂贵、需要共享：优先明确 Ownership

- 大型只读索引 / 预计算表：可以全系统共享，初始化阶段构建。
- Thread-specific parser / scratch buffer：通常每线程一份，避免共享可变状态。
- Stateful decoder / sequence tracker：应当由对应流、Session 或工作线程持有，而不是当作“无状态工具”。

推荐将长期依赖放进 `TradingContext`，用引用、指针或构造函数依赖注入连接：

```cpp
struct TradingContext {
    // Configuration config;
    // MarketDataSession market_data;
    // PricingEngine pricing;
    // Gateway gateway;
};
```

相较全局可变 Singleton，Context-owned 更容易控制初始化顺序、销毁顺序、测试隔离与多实例运行。

### 6.3 Global 使用条件

Global **不是**默认性能优化。只有在确实需要进程级唯一/共享，并已解决以下问题时考虑：

- 初始化顺序（跨翻译单元的 static initialization order 问题）。
- Shutdown / 静态析构时序。
- 线程安全、可变共享状态和 Cache Line Contention。
- 测试隔离、多环境实例、重启/重新配置。
- C++20 `constinit` 可约束静态初始化方式，但不会自动解决线程安全或销毁顺序。

## 7. HFT Hot Path 的额外约束

类设计的性能价值主要在于**稳定且可预测的运行时行为**，并不是“全部禁止 Move”。

1. **Init / Hot Path 分离**：创建连接、申请容量、注册回调、构建索引尽量在启动阶段完成。
2. **Bounded Work**：优先有上界的循环、队列容量、哈希探测及失败处理；明确 overflow/backpressure 策略。
3. **Address Stability**：特别是 L3 OrderBook、Intrusive List、回调和长期引用；`vector` 扩容会使元素地址失效。
4. **Single Writer / Ownership per Thread**：能靠架构独占状态时，尽量不要在 Hot Path 引入共享可变状态与锁。
5. **Memory / NUMA**：预分配、页面预触碰、亲和性、pool / slab、cache locality；先测量再选择布局。
6. **Error Handling**：初始化错误可用异常或错误返回；Hot Path 的失败语义应清晰。只在**确实不会抛出**时声明 `noexcept`，否则异常逃出会调用 `std::terminate`。
7. **Observability**：保留必要的指标、失败计数与慢路径诊断，但避免无界同步日志进入 Hot Path。
8. **Copy Discipline**：警惕昂贵快照、`shared_ptr` 引用计数操作、隐式临时对象；是否构成问题要用实际 Profile 验证。

## 8. Copy / Move / Destructor 的决策顺序

```text
1. 它的业务语义是 Value 还是 Identity？
   ├─ Value    → Copy 是否创建独立的正确值？
   └─ Identity → 默认禁 Copy

2. 它拥有独占资源吗？
   ├─ No       → 考虑默认 Copy/Move 或 Non-owning View
   └─ Yes      → RAII；禁 Copy，评估 Move

3. Move 后：源和目标对象是否都满足不变量？
   ├─ Yes      → 可支持 Move（必要时 noexcept）
   └─ No       → 禁 Move 或重新设计成员结构

4. 对象必须保持固定地址 / 线程归属 / 外部引用吗？
   ├─ Yes      → 默认 Non-movable
   └─ No       → 不因习惯而禁止 Move

5. 资源清理由现成成员类型完成吗？
   ├─ Yes      → Rule of Zero 优先
   └─ No       → 专用 RAII Wrapper，认真设计特殊成员函数
```

补充判断：

- `std::move(x)` **只是将表达式转换成右值类别**，不保证一定调用 Move Constructor；在不可 Move 的某些情形可能调用 Copy，或直接编译失败。
- 默认/隐式特殊成员函数是否存在以及是否被删除，取决于成员类型与其他已声明的特殊成员函数。
- C++ 标准库容器对象被移动后通常仍有效，但其具体值往往未指定；业务类更应该明确自己的 moved-from 契约。
- `const` 成员函数不等于线程安全；Non-copyable / Non-movable 也不等于线程安全。
- 不要为了符合“Rule of Five”机械地手写五个函数；**Rule of Zero + 明确禁止不合法操作**是更稳健的组合。

## 9. 常见反模式与失效模式

| 反模式 | 风险 | 推荐替代 |
| --- | --- | --- |
| 为所有 Data 手写 Copy / Move / Destructor | 重复逻辑、意外破坏异常保证 | Rule of Zero |
| 所有 Service 都 `new` + `unique_ptr` | 不必要堆分配、间接访问 | 生命周期固定时直接成员/局部对象 |
| 所有 Tool 都放 Global Singleton | 隐式依赖、并发争用、测试困难 | Free Function / Local / Context / Thread-local |
| 为性能禁掉所有 Move | 约束无根据、降低组合性 | 基于地址、身份、不变量判断 |
| 手写资源清理同时使用可复制默认成员 | Double Free、Dangling | RAII 成员、禁 Copy 或实现深拷贝 |
| 默认 Move 包含“指针 + 长度”组合 | moved-from 不变量可能不一致 | 单一权威状态或自定义 Move |
| 直接 Copy 一个 View 当成复制数据 | 悬空视图、生命周期错误 | 显式拷贝数据或保证 Owner 长寿 |
| 动态增长容器中的元素被外部长期引用 | 地址/迭代器失效 | 固定容量、index / handle、stable storage 或 pool |
| 用 `shared_ptr` 表达所有组件依赖 | 隐式共享 Ownership、生命周期难推理 | 明确 Owner，其他位置用非拥有引用 |
| 对可能抛异常的操作强加 `noexcept` | 触发 `std::terminate` | 先证明或处理异常，再标注 |

## 10. Code Review / OA / 面试 Checklist

- [ ] 这个类是 Value、Resource Owner、Identity Service、View，还是 Tool？
- [ ] Ownership 是否清楚？谁拥有，谁引用，谁负责 Shutdown？
- [ ] Copy 有意义吗？是深拷贝、共享所有权，还是复制 View？
- [ ] Move 有意义吗？是否满足对象地址、外部引用和不变量？
- [ ] 是否能遵循 Rule of Zero？自定义析构与特殊成员函数是否必要？
- [ ] Constructor 是否明确建立不变量？失败后是否资源安全？
- [ ] 运行时状态是否有合法的初始化、停止、恢复路径？
- [ ] 线程归属、同步与生命周期跨线程传递是否明确？
- [ ] Hot Path 中有隐式分配、潜在无界耗时或意外异常吗？
- [ ] 容器增长、回调、异步操作会不会造成引用失效？
- [ ] 如果异常/背压/容量耗尽，是否保持 Class Invariant？

## 11. 一句话版 Policy

> **Data → Rule of Zero / Value Semantics；Resource → RAII / 明确 Ownership；Service → 默认 Non-copyable + Non-movable / 显式 Lifetime；轻量无状态 Tool → Free Function 或局部对象；重型 Tool → Context / Thread-owned。**
>
> **Init 时完成资源构建，Hot Path 维持明确 Ownership、稳定地址、有限工作量和可预测失败语义。**

### 参考标准库主题

- Special member functions / Rule of Zero, Three, Five
- `std::unique_ptr`、`std::vector`、`std::span`、`std::string_view`
- Value Semantics / RAII / Object Lifetime
- Move Semantics / Exception Safety / `noexcept`
- Stateful Allocator / `std::pmr` / Container Invalidation Rules
- C++20 `constinit` / Atomic / Thread Affinity
