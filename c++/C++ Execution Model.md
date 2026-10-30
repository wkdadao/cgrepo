# C++ Execution Model：完整中文解析

> 基于 Bryce Adelstein Lelbach 在 Core C++ 2019 的演讲 *The C++ Execution Model*（124 页）。
>
> 原始 PDF：https://corecppil.github.io/CoreCpp2019/Presentations/Bryce_C++_Execution_Model.pdf
>
> 本文按主题重新组织，并补充 C++20/23/26 更新、编译器优化、CPU 微架构及 HFT 实践。注意区分 **标准语义保证**、**实现行为** 与 **性能特性**。

## 目录

1. C++ Abstract Machine
2. Storage Model、Object 与 Memory Location
3. Threads of Execution 与 Storage Duration
4. Expressions、Full-expressions 和 Evaluations
5. Sequenced-before 与求值顺序
6. 函数调用、初始化列表和运算符的顺序
7. Synchronizes-with
8. Happens-before 与 Data Race
9. Atomic Memory Orders
10. Execution Steps
11. Forward Progress
12. Blocking with Forward Progress Guarantee Delegation
13. C++20/23/26 标准演进
14. 语言语义 → 编译器优化 → CPU 微架构
15. HFT 工程案例与验证方法
16. 总结与参考资料

---

## 1. C++ Abstract Machine：抽象机

C++ 标准主要定义程序的**可观察语义**，而非 CPU 的指令执行时序。抽象机是描述对象、表达式、函数、线程及其执行关系的概念模型。

~~~
源代码
   ↓
C++ 抽象机语义（对象 / 求值 / 顺序 / 同步 / 进展）
   ↓
编译器、标准库和运行时
   ↓
机器指令、OS 调度、缓存一致性、硬件
~~~

### 1.1 As-if Rule

只要满足标准要求的可观察行为，编译器可以更改中间计算、删除变量、合并读写、内联、向量化与重排指令。

~~~cpp
int calculate() {
    int a = 10, b = 20;
    return a + b;
}
// 可合法优化为 return 30;
~~~

**不要由源代码中出现几次赋值或读取，直接推断实际生成几条 load/store 指令。**

### 1.2 语义正确与性能最优是两件事

- 语义问题：是否触犯类型访问、对象生命周期、顺序、并发与进展规则？
- 编译器问题：实现怎样在语义允许范围内优化？
- 硬件问题：指令如何被 OoO、store buffer、cache coherence、NUMA 等机制执行？

## 2. Storage Model：对象与内存位置

### 2.1 抽象存储不是硬件缓存图

C++ 不直接规定 L1/L2/L3/DRAM 的层次结构。标准中的 object、storage、memory location 与实际物理地址不是同一个概念。

对象可以存在于寄存器或被彻底优化掉；声明局部变量并不意味着一定有一个物理栈槽。

### 2.2 Memory Location 是 Data Race 的判断单位

~~~cpp
struct S {
    unsigned a : 4;
    unsigned b : 4;
    int c;
};
~~~

连续的非零宽度 bit-field `a`、`b` 属于同一个 memory location；`c` 是另一个。因此不同线程分别写 `a` 与 `b`，仍可能构成 data race。

~~~cpp
struct Pair { int a, b; };
~~~

`a` 和 `b` 是不同 memory locations；线程分别修改它们不因属于同一结构体而自动产生 data race，但它们若位于同一 cache line，可能产生 **false sharing**。

> Data race 是语言层正确性问题；false sharing 是硬件性能问题。

### 2.3 存储重叠：EBO 与 no_unique_address

~~~cpp
struct Empty {};
struct A : Empty { int value; };  // 空基类优化 EBO

struct B {
    [[no_unique_address]] Empty policy;
    int value;
};
~~~

允许某些无状态子对象不占用独立额外存储，但不是任意对象均可重叠。

### 2.4 线程间共享与进程间共享

同一程序的 `std::thread` 共享对象，属于 ISO C++ 内存模型直接讨论的情形。多个进程通过 `mmap(MAP_SHARED)` 共享物理页，并不意味着 ISO C++ 本身完整规定了跨进程的 atomic 同步。实践中需要 Linux、ABI、编译器、对象生命周期、对齐和硬件等附加保证。

`std::atomic_ref` 也**不是标准规定的跨进程同步 API**。

## 3. Threads of Execution

线程是从顶层函数调用开始的执行流，包括该调用引发的后续函数执行；它不等同于 CPU core、SMT 硬件线程、协程或任务。

~~~cpp
void f() {}
int main() {
    std::thread t(f);
    t.join();
}
~~~

这里至少有 main 线程与执行 `f` 的线程。后者如何映射到内核线程，由实现决定。

### 3.1 栈对象也可以跨线程访问

~~~cpp
void example() {
    int x = 0;
    std::thread t([&] { x = 42; });
    t.join();
    assert(x == 42);
}
~~~

其正确性取决于生命周期覆盖访问，并且 `join` 提供规定的同步关系，而不是因为对象位于“线程栈”或“共享堆”。

### 3.2 Thread-local storage

~~~cpp
thread_local int counter = 0;
~~~

每个线程具有独立实例；动态初始化的精确时点受到标准规则和实现选择约束，不能无条件认为所有 TLS 都在 OS 线程刚创建时初始化。

## 4. Expressions、Full-expressions、Evaluations

### 4.1 Evaluation 两类组成

- **Value computation**：计算表达式的值、确定 glvalue 所指对象等。
- **Side effect**：修改对象、读取 volatile glvalue、标准 I/O 及其相关调用等。

~~~cpp
int a = 10, b = 20;
a = a + b;   // 读取/加法是值计算，更新 a 是副作用
~~~

普通读取不是仅因为用了机器 load 就变成语言层 side effect。

### 4.2 Full-expression

完整表达式划定求值边界，并经常决定临时对象何时析构。

~~~cpp
struct X { ~X(); };
void use(const X&);

use(X{});  // 临时 X 通常在该完整表达式结束时销毁
next();
~~~

生命周期延长等例外需单独分析；不是所有临时对象都在最近的分号处析构。

### 4.3 Side effect 完成不等于外设完成

`std::cout << "Hello";` 调用完成不保证文字已显示到屏幕；普通 store 已在处理器的架构语义上完成，也不必然已落入 DRAM。两者分别涉及 I/O 与硬件内存模型，不应混作 C++ 的 sequencing 规则。

## 5. Sequenced-before：同一线程的偏序

设两个求值 A、B：

- `A sequenced-before B`：A 必须在语义上先于 B；具传递性。
- **Indeterminately sequenced**：二者执行先后不指定，但不能相互交错。
- **Unsequenced**：不存在二者的先后保证，操作可在允许的语义中交错。

Sequenced-before 是抽象语义偏序，**不是对每条机器指令发出顺序的要求**。

### 5.1 Unsequenced UB

~~~cpp
int i = 0;
int x = i++ + i++;  // UB：同一 memory location 的未定序修改
~~~

### 5.2 Indeterminately Sequenced

从 C++17 起，同一次函数调用中不同实参的求值彼此 indeterminately sequenced：

~~~cpp
int i = 0;
f(i++, i++);  // C++17+ 不因这两个 i++ 而产生未定序 UB
// 两参数值可能分别是 (0,1) 或 (1,0)，i 最终为 2
~~~

这里假设 `f` 接受两个 `int` 参数，且函数本身无额外修改。C++17 之前规则不同。

### 5.3 编译器优化

~~~cpp
int x = 0;
void f() {
    x = 1;
    x = 2;
}
~~~

在不存在影响正确性的其他观察时，编译器可删除第一次 store。语言规定抽象求值关系，不要求每个源码 store 保留。

## 6. 函数、初始化与运算符的顺序

### 6.1 函数实参

~~~cpp
g(a(), b(), c());
~~~

三个实参必须在 `g` 函数体开始前求值完毕，但不能假设左到右。C++17+ 不同实参求值不会相互交错。

如需指定顺序，显式拆分：

~~~cpp
auto x = a();
auto y = b();
auto z = c();
g(x, y, z);
~~~

### 6.2 Braced initializer 左到右

~~~cpp
std::tuple<A, A> t1(A(0), A(1));  // 函数式构造：两实参顺序未指定
std::tuple<A, A> t2{A(0), A(1)};  // 花括号：初始化子句左到右求值
~~~

**注意**：求值顺序与类成员的实际初始化顺序是不同规则，后者取决于基类及成员的声明顺序等。

### 6.3 重要运算符规则

| 形式 | 关键规则 |
|---|---|
| `a && b`, `a || b` | 左侧先于右侧，且短路 |
| `a, b`（内建逗号） | 左侧先于右侧 |
| `a << b`, `a >> b` | C++17+ 左侧先于右侧 |
| `a[b]` | C++17+ 左侧先于右侧 |
| `a = b`, `a += b` | C++17+ 右操作数先于左操作数 |
| `a.*b`, `a->*b` | 左侧先于右侧 |

**纠错**：原 PDF 第 68 页附近关于运算符与操作数 value computation 的箭头方向有误。正确的是：操作数的值计算先于运算符结果的值计算；这并不自动规定操作数间顺序或所有副作用的完成顺序。

## 7. Synchronizes-with：跨线程建立同步

~~~cpp
int data = 0;
std::atomic<bool> ready{false};

// Producer
data = 42;  // A
ready.store(true, std::memory_order_release);  // B

// Consumer
while (!ready.load(std::memory_order_acquire)) {}  // C: 最后一次读到 true
int result = data;  // D
~~~

- A sequenced-before B。
- 如果 C 读到 B 写入的值（或满足 release sequence 等条件），B synchronizes-with C。
- C sequenced-before D。

`release` 与 `acquire` 不是彼此存在就自动同步；读取关系不可省略。

### 7.1 Mutex 也提供同步

~~~cpp
std::mutex m;
int value = 0;

void write() { std::lock_guard lock(m); value = 42; }
int read()  { std::lock_guard lock(m); return value; }
~~~

一次 unlock 与后续成功 lock 满足相应关系时建立同步。无需把每个受 mutex 正确保护的普通成员都改为 atomic。

### 7.2 Relaxed 不发布普通对象

~~~cpp
// Thread A
data = 42;
ready.store(true, std::memory_order_relaxed);

// Thread B
if (ready.load(std::memory_order_relaxed))
    consume(data);
~~~

即使 B 读到 `true`，也未借此建立 `data` 所需的 happens-before。访问 `data` 的分支发生时可能形成 data race；问题不是“偶尔读到旧数据”这么简单。

## 8. Happens-before 与 Data Race

在现代标准常用的基本关系中：

1. A sequenced-before B ⇒ A happens-before B。
2. A synchronizes-with B ⇒ A happens-before B。
3. Happens-before 可传递。

上面的发布示例可严格推导：

~~~
A: data = 42
      │ sequenced-before
      ▼
B: release store
      │ synchronizes-with
      ▼
C: acquire load（读到发布值）
      │ sequenced-before
      ▼
D: read data
~~~

因此 A happens-before D（还需确保没有其他写入影响所读值）。

### 8.1 Happens-before 不等于真实物理先后

- A happens-before B 不要求各自对应的机器指令按单纯的真实时间顺序逐条发生。
- 物理上 A 先运行，不自动建立语言层 happens-before。
- 编译器可合法重排普通操作，只要保留定义良好的程序必须具备的观察语义。

### 8.2 Data race 判断

不同并发执行流在同一 memory location 上发生冲突访问（至少一方修改），其中访问未被适当的 atomic 或 happens-before 规则保护时，可能构成 **data race → Undefined Behavior**。

~~~cpp
int counter = 0;
void worker() { ++counter; } // 多线程并发执行且无同步：data race
~~~

### 8.3 Atomic Modification Order

每个 atomic 对象有自己的修改总序（modification order），不等于所有 relaxed atomic 对象共享一个全局总序。

## 9. Atomic Memory Orders

| Order | 用途 | 重要限制 |
|---|---|---|
| relaxed | 原子计数等 | 无普通数据的 release/acquire 发布保证 |
| acquire | 读取已发布状态 | 同步依赖于读取关系 |
| release | 发布此前写入 | 单独 release 不保证接收端正确 |
| acq_rel | 原子 RMW 双向约束 | 只用于适当的 read-modify-write |
| seq_cst | 更强的原子全序约束 | 不等于所有普通操作的全局物理顺序 |
| consume | 历史数据依赖发布模型 | C++26 弃用，实际代码应避免依赖 |

### 9.1 SPSC Ring Buffer：双向发布

~~~cpp
// Producer
buffer[head] = message;
head_.store(nextHead, std::memory_order_release);

// Consumer
auto h = head_.load(std::memory_order_acquire);
if (tail != h) {
    Message msg = buffer[tail];
    tail_.store(nextTail, std::memory_order_release);
}

// Producer 在复用 slot 前，acquire 读取 tail_。
~~~

两条同步链：

1. Producer 写 slot → release head → acquire head → Consumer 读 slot。
2. Consumer 读完 slot → release tail → acquire tail → Producer 复用 slot。

还需证明索引、容量、溢出、初始化、ownership、异常与对象生命周期均正确。

## 10. Execution Steps

C++ execution step 是用于描述 forward progress 的**语言层活动**，不是一条 CPU 指令。

典型相关活动包括：线程终止、volatile 访问、标准 I/O 完成、同步与原子操作的完成等；C++26 对部分措辞进一步细化。

~~~cpp
std::atomic<bool> stop{false};
while (!stop.load(std::memory_order_relaxed)) {
}
~~~

atomic load 是能够体现持续执行进展的操作。但这不意味着等待条件最终必然满足。

~~~cpp
volatile bool stop = false;
// volatile 不提供跨线程同步，不能替代 atomic。
~~~

## 11. Forward Progress：前进保证

**取得进展 ≠ 操作成功 ≠ 函数最终返回 ≠ 满足实时 deadline。**

### 11.1 Concurrent Forward Progress

执行流可以不依赖其他执行流取得进展而最终继续执行；没有具体的毫秒/微秒时限承诺。

### 11.2 Parallel Forward Progress

在开始执行、取得首个 execution step 之前，不保证何时启动；开始后提供 concurrent 级别的前进保证。某些线程池调度可作为直观例子，但具体性质由运行时规定。

### 11.3 Weakly Parallel Forward Progress

不保证特定执行流最终一定取得进展。某些协作式调度任务可能处于此类。

| 级别 | 对单个执行流的保证 | 典型直觉 |
|---|---|---|
| Concurrent | 独立的最终进展 | 有前进保证的 OS 线程 |
| Parallel | 启动后保证 | 尚在队列中的线程池任务 |
| Weakly Parallel | 不保证 | 无公平性承诺的协作调度 |

`main` 线程和 `std::thread` 所创建线程的相关保证在标准中具有 implementation-defined 成分；不能把 Linux 的常见行为直接视为全平台 ISO C++ 事实。

### 11.4 Deadlock 与进展

~~~cpp
// Thread A: lock(m1); lock(m2);
// Thread B: lock(m2); lock(m1);
~~~

前进保证不替应用消除锁顺序循环依赖。标准库某些阻塞操作被特殊对待，也不意味着所有等待最终都可解除。

## 12. Blocking with Forward Progress Guarantee Delegation

原演讲称为 **Boost Blocking**。核心想法：当一个具有前进保证的执行流阻塞等待另一组工作时，受到此规则支持的阻塞操作需要适当地把前进保证委托给所依赖的工作，让它们有机会运行。

### 12.1 单 Worker 死锁

~~~cpp
ThreadPool pool(1);
pool.submit([&] {
    auto child = pool.submit(taskB);
    child.wait();   // 如果普通 wait 且只有一个 worker，可能死锁
});
~~~

等待者占据唯一 worker，依赖工作没有执行资源。

### 12.2 Helping 思路（示意伪代码）

~~~cpp
void TaskHandle::wait() {
    while (!is_done()) {
        if (auto task = pool.try_pop())
            task(); // 等待过程中帮助执行任务
        else
            pool.wait_for_work_or_completion();
    }
}
~~~

这里省略同步、异常、唤醒和生命周期处理。Helping 是可能的实现策略，不意味着标准规定必须如此实现，也不意味着每个 `wait()` 都提供保证。

保证的委托面向相关依赖链，并不意味着无关任务自动取得相同的进展保证。

## 13. 2019 演讲之后的标准更新

### C++17

- 函数实参间求值变为 indeterminately sequenced。
- 多项运算符求值顺序规则增强。
- 并行算法 execution policies。

### C++20

- `std::atomic::wait/notify`、`std::atomic_ref`。
- `std::jthread`、latch、barrier、semaphore。
- 协程；`std::execution::unseq`。

### C++23

- `std::start_lifetime_as`：对适用 implicit-lifetime 类型显式开始生命周期。

### C++26

- P2809R3：trivial infinite loops 不再因为传统 forward-progress 假设被当作 UB；适用的循环体使用等效 yield 的语义处理（freestanding 有特殊规则）。
- `memory_order_consume` 弃用，并与 acquire 语义对齐。
- Execution / Senders-Receivers 标准化设施。

**重要**：2019 PDF 中出现的 `std::for_loop` 是历史提案/示例背景，不应直接认为它是通用已发布的标准库接口。

## 14. Language Semantics → Compiler Optimization → CPU Microarchitecture

### 14.1 三层分离

1. **Language Semantics**：type access、object lifetime、sequencing、synchronization、forward progress。
2. **Compiler**：alias analysis、constant propagation、CSE、DSE、LICM、vectorization、instruction scheduling。
3. **CPU**：OoO、store buffer、load/store queue、cache coherence、cache line ownership、TLB、NUMA。

### 14.2 Strict Aliasing

~~~cpp
float f = 1.5f;
int* p = reinterpret_cast<int*>(&f); // cast 本身可以形成指针值
int x = *p;                           // 违反类型访问规则，UB
~~~

关闭某些 strict-aliasing 优化只是实现选择，不自动把程序变成 ISO C++ 定义良好。

### 14.3 Lifetime

~~~cpp
void* storage = ::operator new(sizeof(T));
T* obj = ::new (storage) T{};
obj->~T();
::operator delete(storage);
~~~

访问有效性需要同时证明存储、对齐、正确对象生命周期、类型访问和同步规则。仅仅地址有效不够。

### 14.4 Atomic RMW 与 Cache

~~~cpp
counter.fetch_add(1, std::memory_order_relaxed);
~~~

语言层：原子操作及 modification order；某些 x86-64 实现层：可生成 `lock` RMW，引起 cache line 的独占获取与 coherence traffic。语言标准不规定具体 lock 指令或一致性协议。

`relaxed` 也不保证总比 `seq_cst` 更快；某些 RMW 在 x86 上可能生成相同指令。

## 15. HFT 工程实践

| 主题 | 例子 | 主要检查点 |
|---|---|---|
| Storage / Lifetime | Hugepage arena、slab、L3 Order Book | 构造/析构、对齐、reuse |
| Memory Location | 并发状态字段 | data race 与 false sharing 分开论证 |
| Sequencing | Order Entry 状态机、消息 parse | 不能依赖未指定实参顺序 |
| Synchronization | SPSC、行情发布 | release/acquire reads-from |
| Happens-before | Feed → Strategy | 数据发布、读取和 slot 复用 |
| Modification Order | 原子索引、计数 | 单对象顺序与跨对象关系 |
| Forward Progress | Spin/poll、CAS loop | 系统进展、个体饥饿、调度保证 |
| Delegated Blocking | Thread pool、异步子任务 | worker exhaustion 和等待依赖 |
| Microarchitecture | CTPIO、Onload、cache locality | profiler、tail latency 验证 |

### 15.1 Lock-free 与 Wait-free

- **Obstruction-free**：单独运行无争用时操作能完成。
- **Lock-free**：系统整体持续完成操作，不保证某个线程不会反复失败。
- **Wait-free**：每个操作都能在有限的自身执行步骤内完成。

`lock-free != no contention != wait-free != guaranteed low tail latency`。

### 15.2 Busy Poll 分层分析

~~~cpp
while (running.load(std::memory_order_relaxed)) {
    poll();
}
~~~

- 语言：atomic 合法；`poll()` 的其他共享访问仍要正确同步。
- 编译器：不能随意删除必要 atomic 访问。
- CPU：占核、功耗、cache 流量、可能的调度扰动。
- 前进：循环可以持续取得执行步骤，但不保证 `running` 最终变 false，也不保证其他任务的进展。

## 16. 实战审查顺序

1. **Object / Lifetime**：真实对象是否存在？存储和对齐是否正确？
2. **Access / Aliasing**：类型访问是否合法？指针能否互相别名？
3. **Evaluation / Sequencing**：子表达式的先后关系是否满足算法要求？
4. **Ownership**：哪些线程可以触碰哪些 memory locations？
5. **Synchronization**：释放/获取、锁、join 等建立了什么关系？
6. **Happens-before**：是否能给每个冲突访问画出证明链？
7. **Forward Progress**：是否会死锁、饥饿、一直排队或无法终止？
8. **Performance**：最后才检查汇编、CPU cache 和 p99.9 tail latency。

### 核心结论

> **C++ Execution Model 描述语义可接受的执行，而不是物理处理器逐指令执行的时间表。**
>
> **跨线程正确性需要标准定义的同步和 happens-before，而不只是“先运行过”或“x86 cache coherent”。**
>
> **Forward Progress 表示是否取得执行进展，不等于操作成功、任务终止或实时延迟保证。**

## 参考资料

- [Bryce — The C++ Execution Model (Core C++ 2019)](https://corecppil.github.io/CoreCpp2019/Presentations/Bryce_C++_Execution_Model.pdf)
- [C++ Draft: Sequential execution](https://eel.is/c++draft/intro.execution)
- [C++ Draft: Multithreaded executions and data races](https://eel.is/c++draft/intro.races)
- [C++ Draft: Forward progress](https://eel.is/c++draft/intro.progress)
- [cppreference: Order of evaluation](https://en.cppreference.com/w/cpp/language/eval_order)
- [P2809R3: Trivial Infinite Loops Are Not Undefined Behavior](https://www9.open-std.org/JTC1/SC22/WG21/docs/papers/2024/p2809r3.html)
