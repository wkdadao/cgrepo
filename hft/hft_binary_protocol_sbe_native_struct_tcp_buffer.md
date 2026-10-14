# HFT Hot Path 消息协议与接收缓冲区设计
## SBE vs Native Struct · Shared Memory vs Network · C++ Object Lifetime · TCP Stream Recycle

> 场景：C++17/20/23、Linux、低延迟 Pricing / Market Data / Gateway / OMS / Execution。  
> 范围：讨论固定长度 binary message、SBE、`reinterpret_cast`、alignment、object lifetime、异步 TCP stream 直接写入 `std::vector<Data>`、tail compaction，以及 hot-path benchmark。  
> 重点：**协议（wire format）、C++ 对象（object representation/lifetime）、传输（copy/zero-copy）、同步（shared-memory publication）是不同维度。**

## 1. 结论摘要

- **Shared Memory 同机构造受控、同版本 C++ IPC**：可以使用 Native Struct，但需保证对象确实位于共享内存、各进程一致理解布局，并在目标 Linux/CPU 上验证进程间同步语义。直接在一个进程里构造含 `std::atomic` 的对象并映射到其他进程，并不能仅靠 ISO C++ 标准保证可移植正确性。
- **Network**：SBE 与 Native Struct 均可进行原地（zero-copy at application decode level）访问。SBE 不代表必须把整个消息反序列化并复制成另一个对象。
- **Native Struct**：必须单独解决 alignment、lifetime、wire layout、endianness、padding、有效值表示、版本兼容和消息长度验证。
- **`std::vector<Data> datas(120)`**：120 个 `Data` 对象生命周期已开始且地址对齐。用 `recv` 或 Asio 写入其**对象表示字节**是可行的；之后完整合法的消息可以直接通过 `datas[i]` 访问，**不需要 `std::start_lifetime_as`**。
- **`reserve(120)` 不行，`resize(120)` / `vector<Data>(120)` 才创建相应数量的有效 `Data` 对象**。`data()` 的 capacity 尾部不是 vector 元素。
- **TCP 是 byte stream**：一次接收可能拿到零条、半条、多条、半条加多条；只有完整消息才能交付业务层。
- **Recycle/compaction**：`std::memmove` 能搬运剩余字节，包含不完整的尾包；完整对象的生命周期始终存在。处理尽可能多的完整消息后，仅拷贝少于 `sizeof(Data)` 的尾巴，通常更好。
- **最终胜负看 benchmark**：Native typed load 与 SBE codec load 经过内联优化后可能是同一条或近似的机器指令。不要预设 native 一定显著更快。

## 2. 四个必须分开的概念

| 概念 | 问题 | Native Struct | SBE |
| --- | --- | --- | --- |
| Wire format | 哪些字节、offset、大小、字节序 | 编译器 ABI 的对象布局，除非人为固定 | Schema 明确定义 |
| C++ object lifetime | 目标地址是否存在有效 `Data` 对象 | Typed dereference 必须满足 | 常按字节访问，不要求创建 `Data` |
| Alignment | 地址是否满足对象/字段对齐 | `Data*` 必须满足 `alignof(Data)` | codec 可用 `memcpy` 读取潜在非对齐字段 |
| Copy/transport | 会不会搬运/复制 bytes | 不保证内核/NIC 层 zero-copy | 同样不保证内核/NIC 层 zero-copy |

不要把 "没有 decode loop" 等同于 "真正 network zero-copy"。

## 3. Native 固定消息类型的前提

```cpp
#include <cstddef>
#include <cstdint>
#include <type_traits>

struct alignas(8) Data {
    std::uint64_t orderId;
    std::int64_t  priceTicks;
    std::uint32_t quantity;
    std::uint32_t flags;
};

static_assert(std::is_trivially_copyable_v<Data>);
static_assert(std::is_standard_layout_v<Data>);
static_assert(sizeof(Data) == 24);
static_assert(alignof(Data) == 8);
static_assert(offsetof(Data, orderId) == 0);
static_assert(offsetof(Data, priceTicks) == 8);
static_assert(offsetof(Data, quantity) == 16);
static_assert(offsetof(Data, flags) == 20);
```

注意：
1. `trivially_copyable` 是按对象表示拷贝的关键条件；`standard_layout` 便于使用 `offsetof`。二者**不自动**保证完整的跨编译器 wire ABI。
2. 不要把 `std::string`、`std::vector`、进程内指针、引用等直接作为网络协议字段。
3. 对 `bool`、bitfield、枚举、浮点 NaN/特殊值及平台相关类型要特别谨慎；建议 wire 侧使用固定宽度整数、固定 byte arrays 和明确的标志位。
4. 字节序要明确：直接发送 struct 默认就是本机表示。若跨架构、跨语言或长期维护，SBE 更稳妥。
5. 不要泄露未初始化 padding：**发送整个 struct 的原始字节时 padding bytes 也可能被发送**。固定布局消除/显式初始化 padding 或使用显式编码。
6. 校验 message type、长度、版本、flags 和所有业务字段；trivially copyable 并不代表随意输入的字节可安全解释为所有可能的类型/值。

## 4. Alignment 与 Object Lifetime

### 4.1 Alignment 不是 Lifetime

```cpp
alignas(Data) std::byte raw[sizeof(Data)];
auto* p = reinterpret_cast<Data*>(raw);
```

`alignas` 只解决地址对齐；`reinterpret_cast` **自身不创建对象**。C++20 的 implicit object creation 规则与存储分配方式有关，不能对所有 `std::byte[]`、网络 buffer、offset 一概推断安全。

如果要取得真正的对象生命周期，可以：

- 直接创建 `Data d;` 或 `std::vector<Data>`、`std::array<Data, N>`；
- 对恰当的原始存储使用 placement new / `std::construct_at`（需确保不会意外覆盖刚收到的 bytes）；
- 在 C++23 中满足前提时使用 `std::start_lifetime_as<Data>`；
- 只使用 byte buffer 并 `std::memcpy` 到已经存在的 `Data` 对象（跨 ABI 的 wire 格式前提仍需要处理）。

### 4.2 最自然的设计：让对象先存在

```cpp
std::vector<Data> datas(120); // 120 个 Data 对象已经存在

auto* rx = reinterpret_cast<std::byte*>(datas.data());
// recv(fd, rx + usedBytes, capacityBytes - usedBytes, 0);
```

向已存在 trivially copyable 对象的 object representation 写入字节，并在收齐且内容具有有效对象表示后读取对象，**不需要每次接收之后重新构造对象**。

对比：

```cpp
std::vector<Data> a;
a.reserve(120); // a.size() == 0: 只有 capacity，不能访问 a[0]...a[119]

std::vector<Data> b(120); // size == 120: 创建了真实元素
std::vector<Data> c;
c.resize(120);            // 同样创建 120 个元素
```

`vector<Data> x{120}` 可能受到 `initializer_list` 构造重载影响，推荐明确用 `vector<Data>(120)`。

**容易忽视的事实**：对于内含已经创建 `Data` 对象的 `vector`，每次接收若仅写入了某个对象的部分 bytes，该对象**仍然存在**，只是目前不允许当作完整业务消息读取。剩余 bytes 到达之后，才可以访问已验证的字段。

### 4.3 其他预创建对象的方式

```cpp
std::array<Data, 120> a; // 生命周期已经存在；存储随 array 对象

// C++20：需要在 heap 分配固定数量元素、并避免不必要的 value-initialization 时：
auto p = std::make_unique_for_overwrite<Data[]>(120);
```

`make_unique_for_overwrite` 默认初始化元素，适用于这里的简单平凡数据类型；不要读取在接收前尚未初始化的字段。`std::array` 作为局部变量可能占用栈，而放在 receiver 成员中通常更合适。

### 4.4 一次 `memcpy` 往往值得测试

```cpp
Data value;
std::memcpy(&value, wireBytes, sizeof(value)); // value 是真正的 Data 对象
process(value);
```

24/32 bytes 的 copy 可能被编译器优化成少量 load/store，且规避了源地址 alignment 和源 buffer lifetime 问题。**但仍要求 bytes 能构成合法的 `Data` 对象表示，且 wire ABI 与本机定义相容。**

## 5. TCP Stream：120 个 slot、目标 batch 100、到达 110.33 个消息

假设每条消息 `sizeof(Data) == 24`：

- 容量：`120 * 24 = 2880` bytes。
- 目标批处理：每批最多 100 个（只是业务 dispatch 策略，不应成为 stream parsing 限制）。
- 接收时可能到达 110 条完整消息 + 8 bytes 下一条消息。
- 当前 `usedBytes = 110 * 24 + 8 = 2648`。
- 完整条数 `complete = usedBytes / 24 = 110`。
- 不完整尾部 `tail = usedBytes % 24 = 8`。

**方案 A，推荐（handler 能及时消费）**：
1. Dispatch 全部 110 条完整消息，可分为两批（100+10）。
2. `memmove(rx, rx + 110*24, 8)`。
3. 令 `usedBytes = 8`；下一次接收从 `rx + 8` 开始。
4. 只拷贝 8 bytes，而不是 10 条+8 bytes。

**方案 B，刻意限制单轮处理 100 条**：
1. 仅处理前 100 条。
2. 后续 10 条完整 + 8 bytes 尾包必须保留。
3. `memmove(rx, rx + 100*24, 10*24 + 8)`。
4. `usedBytes = 248`；下一轮**先处理积压的完整消息**或做好 backpressure，然后继续 recv。
5. 复制更多字节；也可能减小单次 dispatch 的占用时间。在严格 latency budget 下可能有理由这样做。

`memmove` 而非 `memcpy`：compact 时源与目标区域可能重叠；`memcpy` 对重叠区域是 UB。

**Lifetime 结论（高置信度）**：整个 `vector<Data>(120)` 的 120 个对象已经存在，且移动的是其 trivially copyable 对象表示；对已经完整且有效的数据做 `memmove` 不需要重新开始 object lifetime。不要在有业务处理者持有 slot 引用、异步读尚未完成、或其他线程访问时 recycle。

## 6. 一个无 hot-path allocation 的固定长度 TCP Byte Accumulator

下例将 socket I/O 与 buffer state machine 分离；适用于 `boost::asio::async_read_some`，也能接在同步 `recv` 后使用。

```cpp
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

struct alignas(8) Data {
    std::uint64_t orderId;
    std::int64_t  priceTicks;
    std::uint32_t quantity;
    std::uint32_t flags;
};

static_assert(std::is_trivially_copyable_v<Data>);
static_assert(sizeof(Data) == 24);
static_assert(alignof(Data) == 8);

class FixedDataRx {
public:
    static constexpr std::size_t Slots = 120;
    static constexpr std::size_t BatchSize = 100;
    static constexpr std::size_t MsgBytes = sizeof(Data);

    FixedDataRx() : datas_(Slots) {}

    std::byte* write_ptr() noexcept {
        assert(usedBytes_ < capacity_bytes());
        return bytes() + usedBytes_;
    }

    std::size_t writable_bytes() const noexcept {
        return capacity_bytes() - usedBytes_;
    }

    // 调用方保证 n 是刚才网络读取实际写入的字节数。
    // 处理完整消息；按批次调用 handler，不保留 Data 的引用。
    // handler(data) 不得重入接收器 / 修改内部 storage。
    template <typename Handler>
    void on_received(std::size_t n, Handler&& handler) {
        assert(n <= writable_bytes());
        usedBytes_ += n;

        const std::size_t complete = usedBytes_ / MsgBytes;
        std::size_t offset = 0;
        while (offset < complete) {
            const std::size_t count =
                std::min(BatchSize, complete - offset);
            for (std::size_t i = 0; i < count; ++i) {
                const Data& msg = datas_[offset + i];
                // 此处应验证消息字段、版本与协议约束。
                handler(msg);
            }
            offset += count;
        }

        const std::size_t consumed = complete * MsgBytes;
        const std::size_t tail = usedBytes_ - consumed;

        if (tail != 0) {
            std::memmove(bytes(), bytes() + consumed, tail);
        }
        usedBytes_ = tail; // 0 <= usedBytes_ < sizeof(Data)
    }

private:
    std::byte* bytes() noexcept {
        return reinterpret_cast<std::byte*>(datas_.data());
    }

    std::size_t capacity_bytes() const noexcept {
        return datas_.size() * MsgBytes;
    }

    // 不要在 outstanding async receive 期间 resize/reallocate。
    std::vector<Data> datas_;
    std::size_t usedBytes_ = 0;
};
```

使用 Boost.Asio 的核心示意：

```cpp
// rx 是一个在整个 async operation 期间存活的 FixedDataRx。
// 保证任何时候对此 buffer 最多只有一个 outstanding async read。
socket.async_read_some(
    boost::asio::buffer(rx.write_ptr(), rx.writable_bytes()),
    [this](const boost::system::error_code& ec, std::size_t n) {
        if (ec) {
            // EOF / reset / cancellation 等，完成错误处理
            return;
        }

        rx.on_received(n, [this](const Data& msg) {
            process(msg);  // 同步使用，不保存引用到异步队列
        });

        // 安排下一次 async_read_some()，不要在旧 read 完成前修改 buffer
        start_next_read();
    }
);
```

**生产使用前额外处理**：
- socket、receiver、handler 捕获对象生命周期正确，例如由 `shared_from_this()` 管理会话；示例省略完整类定义。
- 同一 receive storage 不能同时有多个 pending readers/consumer writers；如采用多个线程处理 handler，必须先转移 ownership 或 copy 到另一块安全内存。
- callback/handler 异常策略需明确，否则中断后状态和下一次 read 计划可能不一致。
- 对恶意、损坏或不可信输入，先完整验证协议与值表示；不能把 arbitrary bytes 当作任意 C++ 类型。
- 如果消息并非定长，不能按 `usedBytes / sizeof(Data)` 分帧；必须先解析固定 header，检查 length 上限，再拿到完整 frame。
- 注意 stream end：EOF 到来时若残余半条消息，应作为 truncated frame 报错或按协议处理。

### 6.1 为什么 `write_ptr()` 可以是未对齐地址？

`datas_.data()` 的起点满足 `alignof(Data)`；但 `usedBytes_` 可能为 8、16 等未对齐 offset。

`write_ptr()` 返回的是 **`std::byte*`，用于写入对象表示字节**，所以这个位置不要求对齐成 `Data*`。**不允许把 `bytes()+usedBytes_` 随意 cast 为 `Data*` 解引用**。

`datas_[0]` 始终在合法对齐地址；等收到一个完整 `Data` 的所有 bytes 后，访问 `datas_[0]` 没有地址对齐问题。

## 7. Shared Memory IPC 的特别问题

C++ object lifetime 仍然重要，但此外必须考虑：
- 跨进程的共享映射如何分配与初始化对象（creator/attacher 的流程）。
- 初始化一次、只读 attach、崩溃重启、版本升级时的协议和状态一致性。
- SPSC/MPMC 的 producer/consumer ownership、release/acquire publication、读写 slot 次序、空满判断、cache-line contention / false sharing。
- 跨进程 `std::atomic` 在 Linux/x86-64 特定实现上常见且可行，但 **ISO C++ 不承诺所有进程共享映射上的标准 atomics 均具备跨进程语义**；确认 lock-free、平台和 ABI 条件，不要依赖可能为进程私有的内部锁实现。
- Shared Memory 可能直接共享同一对象的表示，因此无需做 SBE 编解码；但若要持久化、replay、混合语言或跨版本部署，具有明确 schema 的 wire format 仍然有价值。

## 8. SBE vs Native Struct 实际性能与维护成本

| 指标 | SBE Generated Codec | 直接 Native Struct |
| --- | --- | --- |
| 字节布局 | Schema + field offset + version | 依赖 ABI，需 static_assert 严格锁定 |
| 解码 | 原地读字段；常用 fixed-size load 或 memcpy | 直接 load 结构成员 |
| Endianness | 明确定义，codec 处理 | 默认 native，需要额外约束 |
| 非对齐访问 | 生成 accessor 可按字节/`memcpy` 安全访问 | `Data*` 必须对齐 |
| 生命周期 | 通常不需要构造业务 `Data` | 必须已有正确 lifetime |
| 长期升级 | 更成熟的 schema evolution | 自己维护版本/fingerprint |
| 跨语言 | 适用性更强 | 一般不推荐 |
| 极端受控同构环境 | 性能很强 | 可能最简洁 |
| 与历史 replay 结合 | 便于版本化消息存档 | 需要主动维护 ABI/兼容策略 |

对于 24~64 bytes 的定长消息，编译器优化后的 Native field access 和 SBE accessor 可能非常接近；不要未经测量就声称 SBE 一定慢。

### 8.1 网络接收的额外 alignment 细节

- 普通 socket/Onload：应用可以自己提供 aligned buffer，但业务 payload 起点需要单独计算。
- DPDK/raw Ethernet：整个 frame 的起点即使对齐，Ethernet/IP/UDP header 加起来也可能使业务 payload 非 8-byte aligned；要验证**业务消息地址**，而不只是 mbuf 的起点。
- 变长/压缩/扩展 header 会破坏先前约定的 payload offset，不能把 `reinterpret_cast<Data*>(payload)` 当作普适操作。

### 8.2 Wire 与 Business Representation 可分开

可以让业务内部使用 `Data`，网络层 SBE 直接访问字段；也可以在入口将网络 wire view 转成内部 `Data`。选择取决于：
- 下游使用时长、buffer ownership / reuse；
- 是否多线程传递，是否需要引用保持；
- 某字段是否反复读取、cache locality；
- 是否要在 journal / replay 复用同一 wire representation；
- 最后的 end-to-end p99.9 / cycles benchmark。

## 9. Benchmark 建议

同一业务语义下测试：

- **A**：`std::vector<Data>` 预创建 + recv 写入 + Typed View / direct member access；
- **B**：byte buffer + `std::memcpy` 到局部 `Data`，比较一份小 copy 的真实成本；
- **C**：SBE-generated decoder 在原始 buffer 原地读取；
- **D**（可选）：SBE view 一直传递 vs 入口转 Native `Data` 后在多阶段使用。

建议分别测 Codec-only、真实 Async TCP/Onload、共享内存 SPSC，避免将 transport/copy/codec 混为一谈。

指标：
- cycles/message、instructions/message、cache miss；
- throughput 和 backpressure；
- p50/p99/p99.9（生产类似负载）；
- buffer reuse 时是否有 use-after-recycle；
- 编译器实际汇编、bounded-work 与 worst-case handling；
- Replay 的 schema version、endianness、time/clock semantics。

## 10. 设计检查清单

- [ ] `Data` trivially copyable、布局受静态断言保护；
- [ ] Sender/receiver 协议版本、字节序、sizeof/offsetof 一致；
- [ ] 只访问**完整且合法**的消息；不访问部分覆盖的对象值；
- [ ] 异步接收 buffer 内存生命周期覆盖整个 I/O；
- [ ] 没有 outstanding receive 时才允许 compaction；
- [ ] 没有 downstream 引用时才允许覆盖/recycle；
- [ ] `vector.size()` 而不是 capacity 代表已经存在的对象；
- [ ] 用 `memmove` 处理可能重叠的 tail；
- [ ] 对变长消息先 parse header 再处理 payload；
- [ ] 收到 EOF 时识别残缺 frame；
- [ ] 业务处理异常/错误返回不会破坏 buffer state；
- [ ] 大流量下制定 backpressure / fairness / batch-size 策略；
- [ ] 同架构也要检查 ABI，跨架构优先显式 wire format；
- [ ] 比较完整链路 p99.9，而不仅是函数级 codec benchmark。

## 11. 最终推荐

- **同进程 SPSC**：真正的 Native Object。
- **共享内存 IPC**：平台验证过的 Native Struct + 明确的共享生命周期/同步与恢复策略。
- **完全受控 C++ 同构内部 TCP/UDP**：可以用固定布局 Native Struct；`vector<Data>(N)` 是一条可行的 lifetime-safe receive 路线。
- **跨语言、跨架构、需要长期协议版本演进与 Replay**：优先 SBE。
- **对于本题的 100/120 slots TCP 场景**：预创建对象，socket 直接写入其 bytes，尽可能处理全部完整消息，`memmove` 保留不足一条的尾部消息；无需 `std::start_lifetime_as`。

> 总结：**Native Object 是 C++ 内存模型选择；SBE 是 Wire Format 选择；zero-copy 与 alignment 是另外两个正交问题。** 将四者区分清楚，比争论“哪个一定更快”更重要。

## 延伸阅读

- C++ `std::is_trivially_copyable` / `std::memcpy` / implicit-lifetime types / `std::start_lifetime_as`
- Boost.Asio `async_read_some` 的 buffer lifetime 要求
- FIX Simple Binary Encoding (SBE) specification 与生成的 C++ codec
