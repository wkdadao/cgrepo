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
16. 实战审查顺序与总结
17. GCC 优化：从 C++ 语义到汇编
18. C++ Atomic 到 GCC x86-64 指令
19. x86-TSO、Store Buffer 与 Litmus Tests
20. Compiler Barrier、CPU Fence、Atomic Fence
21. Cache Coherence、RFO、False Sharing
22. GCC + x86 实验与验证矩阵
23. HFT 场景中的六个故障模式

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


---

## 17. GCC 优化：从 C++ 语义到汇编

> 本章开始属于**新增实现层内容**。以下讲的是常见 GNU/Linux + GCC + x86-64 行为，不是 ISO C++ 对每个平台的保证。所有汇编示例都应在目标编译器上复验。

### 17.1 as-if rule 与 GCC 优化的合法边界

编译器能够消除或重排某些操作，并不是因为“CPU 足够强”，而是因为**语言语义允许在保留必须观察到的行为前提下这么做**。

常见优化：

| 优化 | 英文 | 例子 | 前提/风险 |
|---|---|---|---|
| 删除死写入 | Dead Store Elimination (DSE) | 连续写同一普通变量，前一次结果未被使用 | volatile、atomic、别名及可观察副作用会限制它 |
| 公共子表达式消除 | CSE / GVN | 同值表达式复用结果 | 可能的写入、别名、函数副作用限制复用 |
| 循环不变量外提 | LICM | 把循环中恒定的计算移到外部 | 需证明对所有定义良好的执行等价 |
| 标量替换 | Scalar Replacement / SRA | 拆开结构体，字段留在寄存器 | 地址逃逸、别名和 ABI 边界可能阻碍 |
| 向量化 | Vectorization | 使用 SIMD 处理多个数组元素 | 依赖、对齐、别名、浮点语义会限制 |
| 常量传播 | Constant Propagation | 直接替换常量结果 | 必须遵守执行语义 |

**实验 A：DSE**

~~~cpp
// opt.cpp
int global_value;

void f() {
    global_value = 1;
    global_value = 2;
}
~~~

~~~bash
g++ -std=c++20 -O0 -S -masm=intel opt.cpp -o opt_O0.s
g++ -std=c++20 -O3 -S -masm=intel opt.cpp -o opt_O3.s
grep -A12 '_Z1fv:' opt_O3.s
~~~

优化后通常只剩下写入 2 的机器指令。观察到的汇编是**具体实现结果**；合法性仍需从 C++ 语义分析。

**实验 B：Alias Analysis 会阻止某些优化**

~~~cpp
int sum_no_restrict(int* p, int* q) {
    int before = *p;
    *q = 9;
    return before + *p;
}
~~~

这里 p、q 可能相同。编译器不能在不知道关系的情况下，把第二次 *p 当作必然等于第一次。

与此不同：

~~~cpp
int sum_copy(int* p) {
    int before = *p;
    return before + before;
}
~~~

只要没有额外副作用，后者明显更容易产生单次 load。

**工程提醒：** GCC 的 -fno-strict-aliasing 是特定优化行为的控制选项，不会自动将违反 ISO C++ 类型访问规则的代码变成标准定义良好。应优先修复类型与生命周期语义，而不是依赖关闭优化掩盖问题。

### 17.2 循环优化与 busy poll

~~~cpp
// 错误的跨线程通信
bool ready = false;
void wait_bad() {
    while (!ready) {}
}
~~~

如果企图由其他线程无同步地写入 ready，会形成 data race。编译器无需按“另一个线程可能随时改掉普通 bool”的假设生成代码。

正确的通信方案：

~~~cpp
#include <atomic>
extern std::atomic<bool> ready;

void wait_good() {
    while (!ready.load(std::memory_order_acquire)) {
        // 可在适合的低延迟场景中加入架构相关 pause
    }
}
~~~

注意：atomic load 保证合法的并发访问；但并不保证另一线程会写 true，也不保证任何规定的完成时限。

### 17.3 检查编译器优化过程

~~~bash
g++ --version
g++ -std=c++20 -O3 -S -masm=intel opt.cpp -o opt.s
g++ -std=c++20 -O3 -fdump-tree-optimized opt.cpp -c
g++ -std=c++20 -O3 -fopt-info-vec-optimized -c vector.cpp
g++ -Q -O3 --help=optimizers
~~~

- -S：查看最终汇编。
- -fdump-tree-optimized：查看优化后的 GCC 中间表示（文件名中编号随版本变化）。
- -fopt-info-vec-optimized：报告成功的向量化；也可使用 -fopt-info-vec-missed 查找未向量化原因。
- -Q --help=optimizers：检查当前 GCC 构建的具体优化选项。
- -fno-inline、-fno-tree-vectorize 等适合实验隔离变量，**不应被当作最终性能建议**。

官方资料：[GCC Optimize Options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)、[GCC Developer Options](https://gcc.gnu.org/onlinedocs/gcc/Developer-Options.html)。

## 18. C++ Atomic → GCC x86-64 指令

### 18.1 一张关键映射表

对**适当对齐、自然大小、可内联实现的 std::atomic<int>**，典型 GCC x86-64 编译结果：

| C++ 操作 | 常见汇编 | 说明 |
|---|---|---|
| load(relaxed) | mov reg, [mem] | 普通原子 load 指令 |
| load(acquire) | mov reg, [mem] | x86 普通 load 已具备实现所需的 acquire 排序 |
| load(seq_cst) | mov reg, [mem] | SC 的全局约束由整体代码生成策略满足 |
| store(relaxed) | mov [mem], reg | 普通原子 store 指令 |
| store(release) | mov [mem], reg | x86 普通 store 已满足所需 release 排序 |
| store(seq_cst) | xchg [mem], reg | 常见 GCC 策略；也可能选择其他等价序列 |
| fetch_add（返回值未使用） | lock add [mem], imm | 避免计算旧值的额外开销 |
| fetch_add（返回旧值） | lock xadd [mem], reg | 获取旧值并原子相加 |
| compare_exchange | lock cmpxchg [mem], reg | 原子比较并交换 |
| exchange | xchg [mem], reg | 内存操作数的 xchg 隐含锁定语义 |

**重要限制：**

1. 上表是常见实现选择，不是 C++ 标准规定必须生成的指令。
2. 对象大小、对齐、CPU target、GCC 版本、编译选项、调用上下文会影响结果。
3. 某些 atomic 类型不具备 lock-free 实现，可能调用 libatomic 或内部使用锁。
4. 单看某一条指令不足以证明完整算法的 happens-before、ownership 与生命周期正确性。

### 18.2 可以直接编译的映射实验

~~~cpp
// atomic_mapping.cpp
#include <atomic>

extern std::atomic<int> counter;

int load_relaxed() {
    return counter.load(std::memory_order_relaxed);
}
int load_acquire() {
    return counter.load(std::memory_order_acquire);
}
int load_seq() {
    return counter.load(std::memory_order_seq_cst);
}
void store_relaxed(int x) {
    counter.store(x, std::memory_order_relaxed);
}
void store_release(int x) {
    counter.store(x, std::memory_order_release);
}
void store_seq(int x) {
    counter.store(x, std::memory_order_seq_cst);
}
void add_unused() {
    counter.fetch_add(1, std::memory_order_relaxed);
}
int add_used() {
    return counter.fetch_add(1, std::memory_order_relaxed);
}
bool cas(int& expected, int desired) {
    return counter.compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}
~~~

~~~bash
g++ -std=c++20 -O3 -S -masm=intel atomic_mapping.cpp -o atomic_mapping.s
less atomic_mapping.s

# 或检查目标文件
g++ -std=c++20 -O3 -c atomic_mapping.cpp -o atomic_mapping.o
objdump -drwC -Mintel atomic_mapping.o
~~~

**本次实际验证：** Debian GCC 14.2.0，-std=c++20 -O3 -masm=intel，使用等价的 std::atomic<int> 测试时，得到了：

~~~asm
; load relaxed / acquire / seq_cst
mov eax, DWORD PTR counter[rip]

; store relaxed / release
mov DWORD PTR counter[rip], edi

; store seq_cst
xchg edi, DWORD PTR counter[rip]

; fetch_add，结果未使用
lock add DWORD PTR counter[rip], 1

; fetch_add，结果被使用
mov eax, 1
lock xadd DWORD PTR counter[rip], eax

; compare_exchange
lock cmpxchg DWORD PTR counter[rip], esi
~~~

汇编片段为说明指令形态而简化了函数标签和一些寄存器传递细节，并不代表固定 ABI 序列。

### 18.3 为什么 acquire/release 不需要额外 mfence？

在普通的 x86-64、WB cacheable 内存场景下，x86-TSO 对 load-load、load-store、store-store 提供较强顺序约束。release-store/acquire-load 所需的硬件排序通常已由普通 mov 具备。

但是：

- **编译器仍必须理解 acquire/release 的语义约束**，防止不合法的编译器优化。
- 没有额外 fence 指令，**不代表 acquire/release 没有意义**。
- 换到 ARM 等较弱内存模型，可能需要专门的 load-acquire / store-release 指令。
- cache coherence 自身也不能替代对象生命周期、类型访问和所有跨线程同步证明。

### 18.4 为什么 seq_cst store 常用 xchg？

x86 的普通 store 虽然具备 release 顺序，但 store buffer 允许先前的 store 对其他 core 尚未可见时，执行随后对另一地址的 load。单纯全部使用 mov 不必然满足某些跨对象的 SC 约束。

典型 GCC 策略对 SC store 使用具有锁定语义的 xchg，形成所需的附加排序；另一类合法编译策略可以使用其他 locked RMW 或栅栏组合。

**并非 seq_cst 的每一次 load 都必须附带 mfence。** 原子操作的序列一致性是整段程序的约束，不能逐条映射成“每条 atomic 都插入一次硬件 fence”。

## 19. x86-TSO、Store Buffer 与 Litmus Tests

### 19.1 x86-TSO 的核心模型

TSO = Total Store Order，不能误解为 Sequential Consistency。

对于普通 WB cacheable 内存，可使用下面的近似模型：

~~~
Core 0                         Core 1
  Store Buffer（FIFO）             Store Buffer（FIFO）
       │                               │
       └────── coherent shared memory ┘
~~~

- 本 core 的 store 先进入本地 store buffer，之后成为全局可观察的写入。
- load 可以读取自身 store buffer 的数据（store-to-load forwarding）。
- 不同地址的较早 store 可能尚未对其他 core 可见，而同 core 随后的 load 已读取另一个地址。
- 一般普通内存访问按 TSO 规则维持 load-load、load-store、store-store 顺序。
- 一个重要的可观察松弛是 **Store→Load**（对不同地址）。

这是可理解的架构级近似模型，不表示 x86 所有 memory types、所有指令（例如 non-temporal store）都满足相同简单规则。

### 19.2 Store Buffering（SB）实验

初始：x = 0，y = 0。

~~~
Thread A                    Thread B
x.store(1, relaxed)         y.store(1, relaxed)
r1 = y.load(relaxed)        r2 = x.load(relaxed)
~~~

可能出现：

| r1 | r2 | relaxed C++ | x86-TSO（对应 mov） |
|---:|---:|---|---|
| 0 | 0 | 允许 | 允许 |
| 0 | 1 | 允许 | 允许 |
| 1 | 0 | 允许 | 允许 |
| 1 | 1 | 允许 | 允许 |

**为什么 (0,0) 合法？**

两个核心各自将 store 放入本地 store buffer，在本次 store 对另一个核心可见之前，双方都执行另一地址的 load，从而都读到旧值 0。它不需要 CPU 将两条指令在单核上简单地“交换位置”；这是存储可见性的顺序差异。

### 19.3 完整可执行 SB Test（C++20）

~~~cpp
// store_buffering.cpp
#include <atomic>
#include <barrier>
#include <iostream>
#include <thread>

int main() {
    constexpr int N = 200000;
    // 改为 seq_cst，比较结果。
    constexpr auto order = std::memory_order_relaxed;

    std::atomic<int> x{0}, y{0};
    int r1 = -1;
    int r2 = -1;
    int zero_zero = 0;

    // 两个 worker + main，按轮次同步。
    std::barrier start(3), done(3);

    std::thread a([&] {
        for (int i = 0; i < N; ++i) {
            start.arrive_and_wait();
            x.store(1, order);
            r1 = y.load(order);
            done.arrive_and_wait();
        }
    });

    std::thread b([&] {
        for (int i = 0; i < N; ++i) {
            start.arrive_and_wait();
            y.store(1, order);
            r2 = x.load(order);
            done.arrive_and_wait();
        }
    });

    for (int i = 0; i < N; ++i) {
        x.store(0, std::memory_order_relaxed);
        y.store(0, std::memory_order_relaxed);

        start.arrive_and_wait();
        done.arrive_and_wait();

        if (r1 == 0 && r2 == 0) {
            ++zero_zero;
        }
    }

    a.join();
    b.join();
    std::cout << "(r1,r2)=(0,0): "
              << zero_zero << " / " << N << '\n';
}
~~~

~~~bash
g++ -std=c++20 -O2 -pthread store_buffering.cpp -o sb
./sb
~~~

**当前实验环境实际执行结果**（GCC 14.2.0，x86-64）：

~~~text
(r1,r2)=(0,0): 2918 / 200000
~~~

该计数只是一组调度、机器、编译器及负载条件下的观测值，**不是可移植的概率或延迟数据**。有些运行可能一次 (0,0) 都没有，不能据此证明该结果不可能。

这里每一轮通过 std::barrier 明确分开重置与测试阶段；r1/r2 只在其所属 worker 中写入，main 在同步完成后读取，避免把实验自身变成 UB。

### 19.4 如果换成 seq_cst 会怎样？

在两个 worker 中把 order 改为 std::memory_order_seq_cst（重置的 relaxed store 仍由 barrier 同步隔开）：

~~~
Thread A                 Thread B
SC store x=1             SC store y=1
SC load y                SC load x
~~~

假设两个 load 都读 0。根据每个线程的顺序约束及单一 SC 顺序，就要求产生一个不可能成立的环：

~~~
SC store x < SC load y < SC store y < SC load x < SC store x
~~~

因此 **两者同时读 0 被禁止**。这里的证明基于 C++ SC 原子规则，而不是“x86 运行通常不会发生”。

### 19.5 Message Passing（MP）与 SPSC 的关系

~~~
Producer                     Consumer
payload = value              if (flag.load(acquire))
flag.store(1, release)           use(payload)
~~~

C++ 层正确性由 release/acquire 及读取关系建立。x86-TSO 的强 load/load 与 store/store 约束，有助于高效实现该语义；但硬件层的 MP 直觉**不允许**直接把原子 flag 换成没有同步的普通 bool。

对于 SPSC，另一个非常重要的方向是：consumer 释放 slot → producer 获取 tail → producer 才复用 slot。只证明发布，不证明复用安全，队列仍可能错误。

扩展阅读：[Hardware Memory Models / x86-TSO](https://research.swtch.com/hwmm)、[Linux Kernel Memory Model Explanation](https://docs.kernel.org/dev-tools/lkmm/docs/explanation.html)。

## 20. Compiler Barrier、CPU Fence、Atomic Fence

### 20.1 它们不是同一种东西

| 工具 | 主要作用层面 | 是否自动建立跨线程 synchronizes-with |
|---|---|---|
| GCC asm volatile("" ::: "memory") | GCC 编译器内存屏障 | 否 |
| std::atomic_signal_fence(order) | 编译器级信号处理顺序约束 | 否，不是线程同步机制 |
| std::atomic_thread_fence(order) | C++ 原子 fence 语义；可能生成硬件指令 | **有条件**，需与 atomic 操作配合 |
| x86 mfence | 硬件内存排序 | 单独的汇编指令不自动使无同步的 C++ 普通访问合法 |
| x86 lfence / sfence | 架构定义的对应排序约束 | 同上 |
| acquire/release atomic | C++ 语言级 | 满足匹配读取关系时可建立同步 |

### 20.2 GCC Compiler Barrier

~~~cpp
void compiler_barrier() {
    asm volatile("" ::: "memory");
}
~~~

其中 "memory" clobber 告诉 GCC：这段 asm 可能读写没有列出的内存，因此编译器需要在前后采取保守的内存值假设。

- 通常不生成任何机器指令。
- 它**不能**阻止 CPU 在硬件层进行内存访问重排序或推测执行。
- 不能替代 C++ acquire/release 或 mutex 来使有 data race 的程序变安全。
- 它是 GCC 扩展，不是 ISO C++ 通用语法。

[GCC Extended Asm 官方文档](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html) 特别说明 memory clobber 是**编译器屏障，不是处理器级推测屏障**。

### 20.3 atomic_signal_fence

~~~cpp
std::atomic_signal_fence(std::memory_order_seq_cst);
~~~

用于编译器与同一线程异步信号处理场景的相关顺序约束；通常不需要发出 CPU fence。不应把它作为跨线程消息发布手段。

### 20.4 atomic_thread_fence：不要把 fence 想成独立同步魔法

Release fence + relaxed atomic store 与 relaxed atomic load + acquire fence 可以形成同步，但有前提：

~~~cpp
// Thread A
payload = 42;
std::atomic_thread_fence(std::memory_order_release);
flag.store(1, std::memory_order_relaxed);

// Thread B
int f = flag.load(std::memory_order_relaxed);
if (f == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    use(payload);
}
~~~

假设 B 的 load 读到 A 对 flag 发布的值，并满足相关 fence-atomic 同步规则，那么普通 payload 读写可由同步关系保护。

但如果 B 没有读取到该发布值、读错了对象，或者把 fence 放到了不正确的位置，单纯“代码里有 fence”仍然不能证明正确。

### 20.5 x86 硬件 fence 与性能

- **MFENCE**：建立规范要求的 load/store 排序；某些 seq_cst fence 实现可能使用它或 locked 指令。
- **LFENCE**：具备架构规定的 load 排序及一些实现相关的执行/推测限制；不应简单等同于所有场景的“全面序列化”。
- **SFENCE**：在 non-temporal / write-combining store 等场景特别重要。
- **Locked RMW**：通常同时提供原子读改写及强排序行为，但有性能成本。

对于普通 WB 内存，C++ release/acquire 访问往往仅用 mov 就足够，**没有必要在每次 SPSC 发布后手动加 mfence**。额外 fence 可能显著增加 hot-path 延迟，必须有语义需求和实际测量支持。

## 21. Cache Coherence、RFO、False Sharing 与锁定原子操作

### 21.1 Coherence ≠ Consistency ≠ Happens-before

- **Coherence（一致性协议）**：处理多个 core 对同一 cache line 的持有与修改权限。
- **Memory consistency（硬件内存顺序）**：不同地址 load/store 的可观察顺序，如 x86-TSO。
- **C++ happens-before**：语言级别访问合法性、同步与先行关系。

MESI/MOESI 是常见的理解模型，但 Intel 各代微架构不要求与一张固定 MESI 状态图逐项对应。

### 21.2 RFO（Read For Ownership）

对一个还没有独占修改权限的 cache line 发起写入时，通常需要通过 coherence 机制取得相应写权限。

跨 core 高频写同一 cache line 会造成 ownership 迁移与缓存一致性流量。

~~~cpp
struct Counters {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};
~~~

即使线程 A 只写 a、线程 B 只写 b，不存在 data race，也可能由于它们共享 cache line 而产生严重的 false sharing。

### 21.3 Padding：消除 False Sharing 的实验

~~~cpp
#include <atomic>
#include <cstdint>

struct CountersPacked {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};

// 演示用：假设目标机器常见的 destructive cache-line 大小为 64B。
struct CountersPadded {
    alignas(64) std::atomic<std::uint64_t> a{0};
    alignas(64) std::atomic<std::uint64_t> b{0};
};
~~~

64B 不是跨所有架构的标准 cache line 大小。也可以研究 C++17 的 std::hardware_destructive_interference_size，但要注意实现提供的常量、跨编译单元 ABI 及目标硬件配置。

测量时固定线程亲和性、保持每线程总操作数一致，比较 packed/padded 的 throughput 与 latency，不能只比 sizeof。

### 21.4 LOCK 前缀是否“锁住整个总线”？

对现代 Intel 处理器、普通 write-back cacheable 且访问不跨 cache line 的内存，locked RMW **通常由缓存一致性协议上的 cache locking 完成**，不需要为每次操作锁住整个内存总线。

但以下情况可能使用更昂贵的 bus lock：

1. Split lock：被锁定的操作数横跨两个 cache lines。
2. 对不适合正常 cache locking 的非-WB 内存进行 locked 操作。

因此 atomic 的对齐不只是标准正确性和性能问题，还可能涉及非常严重的平台性能代价；部分 Linux/CPU 配置还能检测甚至禁止 split lock。

资料：[Intel Architecture Programming Reference](https://www.intel.com/content/dam/develop/external/us/en/documents/architecture-instruction-set-extensions-programming-reference-737410.pdf)、[Linux Bus Lock Detection](https://www.kernel.org/doc/html/latest/arch/x86/buslock.html)。

### 21.5 观察 Coherence 性能

~~~bash
# 权限和硬件 PMU 支持满足条件时
perf stat -e cycles,instructions,cache-misses ./your_benchmark
perf c2c record -- ./your_benchmark
perf c2c report
~~~

perf c2c 主要用于分析 cache-to-cache、HITM 和潜在 false sharing 热点；并不是所有 CPU/虚拟化环境都支持同样事件。准确诊断还需结合 CPU 型号、NUMA、线程绑定、变量地址和源码访问模式。

官方资料：[Linux kernel: False Sharing](https://docs.kernel.org/kernel-hacking/false-sharing.html)、[perf-c2c manual](https://man7.org/linux/man-pages/man1/perf-c2c.1.html)。

## 22. GCC + x86 实验与验证矩阵

### 22.1 实验流程

对每个要验证的结论，严格分为五步：

1. **写下语言级假设**：例如“relaxed 不建立同步”“seq_cst SB 不允许 (0,0)”。
2. **写小型可编译程序**：尽量限制类型、函数边界、编译器可见信息。
3. **比较编译产物**：记录 GCC/Clang 版本、-O、-march、架构、汇编。
4. **运行行为实验**：使用正确同步的 litmus harness，不以 UB 代码验证预期行为。
5. **测量真实性能**：在目标生产级 CPU 使用 perf、CPU pinning、预热与重复采样，观察尾延迟而非只看一次平均值。

### 22.2 建议的实验清单

| 实验 | 可验证的现象 | 易犯的错误 |
|---|---|---|
| DSE | 普通死 store 被删除 | 把语言抽象的顺序当作机器 store 顺序 |
| Alias Analysis | 两指针可能别名时读不可随意复用 | 以为传两个指针参数就天然 no-alias |
| Atomic Mapping | relaxed/acquire/release 常用 mov | 把 GCC 选择说成 C++ 标准要求 |
| seq_cst Store | 常见 xchg | 认为每个 seq_cst 访问都必须 mfence |
| Store Buffering | relaxed 出现 (0,0) | 用普通共享 int 做多线程实验，制造 UB |
| Fence | 空 asm 没有 CPU 指令 | 用 compiler barrier 证明线程同步 |
| False Sharing | padding 影响争用成本 | 把 false sharing 当作 data race |
| CAS Contention | 系统前进不保证个体低延迟 | 把 lock-free 当成 wait-free |

### 22.3 面向 HFT 的验证建议

- 用 Compiler Explorer 快速比较 GCC / Clang 和编译选项。
- 用 objdump --disassemble / -drwC 查看真正部署的二进制。
- 用 llvm-mca（若安装）分析静态指令吞吐量的可能瓶颈，但它不直接模拟多核 cache coherence 与真实 tail latency。
- 用 perf stat / perf record、perf c2c 检查分支、cache、HITM 和同步热点。
- 使用 ThreadSanitizer 排查其能识别的动态数据竞争；**TSan 未报告不代表已证明没有 data race**。
- 使用 ASan/UBSan 检查适用的内存/UB 问题；它们同样不能证明整个并发模型正确。
- 对 SPSC、Order Book、Market Data Gateway 建立完整的 ownership 与 happens-before 推导，再做 benchmarking。
- 记录 CPU 型号、微码、内核、NUMA 配置、编译器、构建 flags、affinity、power-state 和预热条件。

### 22.4 正确性的四层证明

~~~text
1. C++ 类型 / 对象生命周期 / strict aliasing 正确
                      ↓
2. Sequencing / Atomic / Happens-before / Forward Progress 正确
                      ↓
3. GCC 对具体代码的优化和指令实现满足语言契约
                      ↓
4. x86 内存顺序与微架构行为满足预期，性能经实测验证
~~~

**不要颠倒这个顺序：** 一段 UB 程序在 x86 上“看起来跑通”不构成合法性证明；一段标准合法的 C++ 程序是否在 x86 上足够快，也必须实测。

## 23. HFT 场景中的六个故障模式

### 23.1 “x86 很强，所以普通 bool 可作为跨线程 flag”

**错误。** x86 硬件性质不能让有 data race 的 C++ 代码变成定义良好。使用 atomic 与适当 memory order。

### 23.2 “ready 使用 relaxed，消费者读到 true，所以 payload 已发布”

**错误。** relaxed 只提供原子性和单对象修改顺序，不构成需要的发布同步。除非另有合法的同步机制，否则使用 release/acquire。

### 23.3 “head 采用 release/acquire，SPSC 就一定正确”

**不完整。** 还必须证明 tail 方向的 slot 复用同步、队列满/空判定、对象生命周期，以及读写所有权。

### 23.4 “使用 memory clobber 就能阻止 CPU 重排序”

**错误。** GCC memory clobber 只是编译器侧约束，并不直接产生 CPU fence 或跨线程 happens-before。

### 23.5 “x86 的 lock add 一定锁总线”

**不准确。** 普通对齐 WB 内存上的 locked RMW 通常靠 cache locking；split lock / 非 WB 情况可能使用昂贵的 bus lock。

### 23.6 “程序 lock-free，所以 p99.9 一定低”

**错误。** Lock-free 保障系统范围的进展，不保障单线程无饥饿、无争用、cache miss 很少或 tail latency 低。

---

### 扩展部分的参考资料（标准、GCC、x86、Linux）

1. [GCC Extended Asm: memory clobber](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html)
2. [GCC Optimize Options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)
3. [GCC Developer Options](https://gcc.gnu.org/onlinedocs/gcc/Developer-Options.html)
4. [GCC __atomic Builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
5. [Hardware Memory Models, Russ Cox](https://research.swtch.com/hwmm)
6. [Linux Kernel Memory Model Explanation](https://docs.kernel.org/dev-tools/lkmm/docs/explanation.html)
7. [Intel 64 and IA-32 Architecture Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
8. [Linux: Bus lock detection](https://www.kernel.org/doc/html/latest/arch/x86/buslock.html)
9. [Linux: False sharing](https://docs.kernel.org/kernel-hacking/false-sharing.html)
10. [perf-c2c Linux Manual](https://man7.org/linux/man-pages/man1/perf-c2c.1.html)


## 参考资料

- [Bryce — The C++ Execution Model (Core C++ 2019)](https://corecppil.github.io/CoreCpp2019/Presentations/Bryce_C++_Execution_Model.pdf)
- [C++ Draft: Sequential execution](https://eel.is/c++draft/intro.execution)
- [C++ Draft: Multithreaded executions and data races](https://eel.is/c++draft/intro.races)
- [C++ Draft: Forward progress](https://eel.is/c++draft/intro.progress)
- [cppreference: Order of evaluation](https://en.cppreference.com/w/cpp/language/eval_order)
- [P2809R3: Trivial Infinite Loops Are Not Undefined Behavior](https://www9.open-std.org/JTC1/SC22/WG21/docs/papers/2024/p2809r3.html)
