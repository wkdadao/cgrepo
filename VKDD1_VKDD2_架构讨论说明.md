# VKDD1 与 VKDD2 —— 架构讨论说明

## 1. 背景

现代企业软件通常不存在一个唯一、权威的业务意图表达。

知识可能分散存在于：

- 源代码
- 配置
- Excel
- 数据库规则
- 文档
- Ticket
- 邮件
- 法规文件
- 业务人员的经验
- 测试用例
- 生产系统实际行为

这些不同表示随着时间推移可能逐渐不一致。

AI Coding Agent 的出现进一步放大了这个问题。

一个 AI Agent 可以读取代码和文档，但它通常无法可靠判断：

- 哪个来源是权威的；
- 文档是否已经过期；
- 当前生产行为究竟是有意设计还是历史遗留；
- 某条业务规则适用于什么范围；
- 某个实现背后有哪些假设；
- 修改一个实现是否需要同步修改其他实现；
- 如何证明修改后的系统仍满足真正的业务意图。

VKDD 的目标，是建立以下几者之间的系统化关系：

**业务知识、可执行语义、实现、证据和验证。**

目前考虑两个架构版本。

---

## 2. VKDD1 —— 以“经过验证的知识”为中心

### 2.1 核心思想

VKDD1 把 **Validated Knowledge，经过验证的知识** 视为系统的核心资产。

系统尝试从异构 Evidence 中提取结构化 Claim，把 Claim 进一步形式化成可执行 Specification，再建立它们与实际实现之间的映射，并持续记录验证结果。

概念流程：

```text
Evidence
   ↓
Claim
   ↓
Executable / Formal Specification
   ↓
Realization
   ↓
Validation
   ↓
Validation Ledger
   ↓
Confidence / Knowledge Status
```

VKDD1 主要试图回答：

> 关于这个系统，我们知道什么？
>
> 为什么相信这些知识？
>
> 这些知识在哪里被实现？
>
> 这些实现被验证到了什么程度？

---

## 3. VKDD1 的核心对象

### 3.1 Evidence

Evidence 是任何可能支持或者反驳某个 Claim 的来源。

例如：

```text
业务文档
法规
交易所规范
邮件批准
Trader 确认
生产日志
现有源代码
测试用例
历史交易
Excel 模型
Ticket
会议决策
```

Evidence 本身不自动等于事实。

它只是推断和判断的依据。

例如：

```yaml
evidence:
  id: EV-1042
  type: business_policy
  source: PricingPolicy-2026-v18
  section: 4.3
```

### 3.2 Claim

Claim 表示关于系统或者业务领域的一条语义陈述。

例如：

```text
Tier-A EURUSD Spot 客户使用 1.2bp markup。
```

Claim 应尽可能明确 Scope。

```yaml
claim:
  id: FX.MARKUP.017

  statement:
    Tier-A EURUSD Spot 客户使用 1.2bp markup。

  scope:
    product: FX Spot
    currency_pair: EURUSD
    client_tier: A
    region: APAC

  status: validated
```

Claim 在具体实现之上建立了一层人类可以理解的语义层。

### 3.3 PSL / 可执行 Specification

VKDD1 原始设计中，非常强调一层中间的可执行 Specification。

它可以使用：

- DSL
- Specification Language
- 某种统一的语义表示

概念上：

```text
Human Claim
      ↓
Executable Specification
      ↓
Implementation
```

例如：

```text
markup(
    pair = EURUSD,
    clientTier = A,
    product = SPOT
) = 1.2bp
```

Specification 理想情况下可以：

- 生成测试；
- 计算期望行为；
- 比较多个实现；
- 表达和解释业务语义；
- 发现 implementation drift。

这层最终可以发展成专门的 DSL，例如：

**PSL — Programming / Policy / Product Specification Language。**

---

## 4. VKDD1 中的 Realization

Realization 表示知识的具体实现形式。

例如：

```text
C++ 代码
Java 代码
Python 模型
Excel 公式
SQL Procedure
配置
数据库规则
Runtime Service
```

一份 Specification 可以对应多个 Realization。

```text
Claim
  ↓
PSL Specification
  ├── C++ pricing engine
  ├── Excel pricing model
  ├── Python simulation
  └── regression tests
```

VKDD 会显式记录这些关系。

---

## 5. VKDD1 中的 Validation

Validation 用来判断某个 Realization 是否符合 Specification 或 Claim。

例如：

```text
Specification vs C++ Implementation
Specification vs Excel
Specification vs Production Replay
Specification vs Historical Trades
Specification vs Regression Tests
```

结果存入 **Validation Ledger**。

```yaml
validation:
  claim: FX.MARKUP.017
  specification: FX.MARKUP.SPEC.017
  realization: pricing-engine@abc183

  checks:
    unit_tests: PASS
    regression_tests: PASS
    excel_comparison: PASS
    production_replay: PASS

  timestamp: 2026-09-22
```

---

## 6. VKDD1 的 Confidence Model

由于 Evidence 可能不完整，甚至相互冲突，VKDD1 可以为知识维护 Confidence 或 Trust Status。

概念上：

```text
Claim
 ├── supporting evidence
 ├── contradicting evidence
 ├── validation history
 └── confidence
```

例如：

```text
Confidence: 0.94
```

或者使用状态：

```text
discovered
proposed
reviewed
validated
disputed
deprecated
```

这里存在一个重要设计问题：

> 数值型 Confidence 是否真的提供了有效信息，还是制造了一种虚假的精确感？

---

## 7. VKDD1 的 Knowledge Graph

VKDD1 很自然地可以用 Graph 作为内部表示。

```text
Evidence
   ↓ supports
Claim
   ↓ formalized-by
Specification
   ↓ realized-by
Implementation
   ↓ validated-by
Validation
```

还可以有：

```text
contradicts
depends-on
supersedes
derived-from
scoped-by
validated-against
```

最终形成一个跨越：

**业务语义 → 软件实现 → 验证结果**

的可追踪知识网络。

---

## 8. VKDD1 中 AI 的角色

AI 主要用于帮助构建和维护知识系统。

可能有：

```text
Discovery Agent
    ↓
发现候选知识

Claim Agent
    ↓
生成结构化 Claim

Specification Agent
    ↓
把 Claim 形式化

Implementation Agent
    ↓
把 Claim 映射到代码

Validation Agent
    ↓
比较不同实现

Knowledge Agent
    ↓
发现冲突和缺失知识
```

关键原则：

```text
AI 可以提出知识。

Validation 和/或有权限的人决定知识是否被接受。
```

因此 VKDD1 的主要价值主张是：

> 为软件系统建立一个持续经过验证的语义表示。

---

## 9. VKDD1 的优势

VKDD1 特别适合：

- 知识发现；
- 业务规则整理；
- Traceability；
- Legacy System 理解；
- 强监管环境；
- 同一逻辑存在多个实现的系统；
- 不一致检测；
- 新员工 Onboarding；
- AI Context Generation；
- Explainability。

它试图回答：

```text
为什么系统存在这个行为？
这个要求来自哪里？
它被实现在哪些地方？
实际实现是否符合预期语义？
```

---

## 10. VKDD1 的主要风险

VKDD1 很容易变成一个“知识模型驱动”的系统。

团队最终可能花大量精力维护：

```text
Claims
Graph
Confidence Score
Formal Specification
Ontology
Relationships
```

但这些工作未必直接改善开发者日常的软件开发流程。

最大的风险是：

> VKDD 最终变成一个复杂的 Knowledge Management System，
> 而不是开发过程中不可缺少的工具。

---

## 11. VKDD2 —— 以软件变更为中心的 Validated Development

VKDD2 保留 VKDD1 的大部分语义基础，但改变了架构中心。

VKDD1 首先问：

> 系统里存在什么知识？

VKDD2 首先问：

> 我们要做什么改动？
>
> 它会影响什么？
>
> 怎么证明改完以后系统仍然正确？

核心流程变成：

```text
Change Intent
      ↓
Affected Claims
      ↓
Executable Contracts
      ↓
Affected Realizations
      ↓
Implementation
      ↓
Validation
      ↓
Accepted Change
```

此时：

**Validated Knowledge 不再是产品本身的最终目的，而是支持安全软件变更的基础设施。**

---

## 12. VKDD2 的核心哲学

VKDD2 中最重要的对象不再是 Knowledge Graph，而是：

**Change。**

所有开发活动，本质上都是：

```text
Current System State
        ↓
    Change Intent
        ↓
   Desired State
```

VKDD2 试图回答五个实际问题：

```text
1. 我们究竟要改变什么？
2. 为什么要改？
3. 这个改动会影响什么？
4. 改完以后哪些东西必须继续成立？
5. 什么证据可以证明这次修改是正确的？
```

---

## 13. VKDD2 的核心对象

VKDD2 把核心概念收缩到大约四个：

```text
Claim
Contract
Change
Validation
```

Evidence 和 Realization 为它们提供支持。

更完整的关系：

```text
Evidence
   ↓
Claim
   ↓
Contract
   ↓
Realization

Change 会影响其中的一部分或全部。

Validation 用于证明修改后的 Realization
仍然满足要求的 Contract。
```

---

## 14. VKDD2 中的 Claim

Claim 仍然是人类可读的语义陈述。

```text
Tier-A EURUSD Spot 客户使用 1.2bp markup。
```

Scope 仍然非常重要：

```yaml
claim:
  id: FX.MARKUP.017

  statement:
    Tier-A EURUSD Spot 客户使用 1.2bp markup。

  scope:
    product: FX Spot
    pair: EURUSD
    client_tier: A

  status: accepted
```

不同之处在于：

VKDD2 并不是为了构建一个完整 Knowledge Graph 才创建 Claim。

Claim 的存在主要服务于：

**软件行为理解和变更影响分析。**

---

## 15. VKDD2 中的 Contract

VKDD2 不再强依赖专用 PSL，而是采用一个更宽的概念：

**Executable Contract。**

Contract 把语义意图转换成机器可以判断的东西。

```python
def expected_markup(pair, client_tier):
    if pair == "EURUSD" and client_tier == "A":
        return 1.2
```

Contract 不要求统一语言。

它可以是：

```text
Python Function
Property-Based Test
State-Machine Invariant
JSON Schema
SQL Constraint
OpenAPI Schema
Benchmark Threshold
Model Checker
现有的 Executable Test
Formal Specification
```

因此：

```text
Claim
=
人类可理解的预期行为

Contract
=
机器可判断的预期行为
```

以后仍然可以增加专用 VKDD DSL，但 DSL 不再是架构成立的前提。

---

## 16. Invariant

VKDD2 明确把 Invariant 作为重要概念。

很多重要的软件性质，并不是普通业务 Requirement。

例如：

```text
Pricing Hot Path 不允许 Dynamic Allocation。
每个 Order 只能有一个 Owning Writer。
Execution Sequence Number 必须单调递增。
Filled Order 不允许重新进入 Working。
Application p99.9 latency 必须低于 20µs。
```

这些都可以转换成 Machine-Verifiable Contract。

```text
Functional Contract       PASS
State-Machine Invariant   PASS
Memory-Safety Contract    PASS
Latency Invariant         FAIL
```

此时 Change 就不应该被接受。

对于 AI 生成代码，这尤其重要。

---

## 17. Change 成为 First-Class Object

VKDD2 明确把 Change 作为持久化的一等对象。

```yaml
change:
  id: CHG-1029

  intent:
    将 Tier-A EURUSD markup
    从 1.2bp 调整到 1.3bp。

  reason:
    Pricing Policy 更新。

  affected_claims:
    - FX.MARKUP.017

  affected_contracts:
    - MARKUP.CONTRACT.017

  affected_realizations:
    - pricing-engine
    - excel-pricing-model
    - regression-suite
```

系统因此可以维护一个完整生命周期：

```text
Change Request
      ↓
Impact Analysis
      ↓
Claim Revision
      ↓
Contract Revision
      ↓
Implementation
      ↓
Validation
      ↓
Approval
      ↓
Accepted System State
```

---

## 18. Semantic Impact Analysis

VKDD2 的一个主要目标，是让 Impact Analysis 从“代码依赖分析”升级到“语义影响分析”。

传统 Coding Agent 经常做：

```text
grep
code search
symbol search
dependency analysis
```

VKDD2 可以从语义层开始：

```text
Change Intent
     ↓
Affected Claim
     ↓
Affected Contract
     ↓
Realization Index
     ↓
Affected Artifacts
```

例如：

```text
Change:
EURUSD Tier-A markup
1.2 → 1.3 bp

Affected Semantic Object:
FX.MARKUP.017

Known Realizations:
- pricing.cpp
- pricing.xlsx
- client-config.xml
- test_markup.cpp
- regression scenario #32
```

这能发现普通 Code Dependency Analysis 很难捕捉的影响关系。

---

## 19. VKDD2 中的 Realization

VKDD2 明确把 Realization 定义得很宽。

可以是：

```text
C++
Java
Python
Excel
Configuration
SQL
Database State
Runtime Process
Manual Procedure
External Service
```

核心区分是：

```text
Contract 描述 WHAT must be true。

Realization 描述 HOW the system achieves it。
```

一个 Contract 可以有多个 Realization。

---

## 20. Validation 成为架构中心

VKDD2 中，Validation 的地位比 VKDD1 更高。

最核心的问题是：

> 在指定 Scope 下，这个 Realization 是否满足这个 Contract？

```yaml
validation:
  id: VAL-3831

  change: CHG-1029

  contract:
    MARKUP.CONTRACT.017

  realization:
    pricing-engine@a813fc

  environment:
    production-config-2026-09-22

  results:
    functional: PASS
    regression: PASS
    excel_consistency: PASS
    performance: PASS
```

Validation 产生的是明确 Evidence，而不是简单地：

```text
Confidence +0.03
```

---

## 21. Status 取代 Numeric Confidence 的核心地位

VKDD2 弱化 Numeric Confidence。

相比：

```text
Confidence = 0.93
```

它更倾向：

```text
proposed
accepted
verified
disputed
superseded
deprecated
```

再配合：

- Evidence；
- Validation History；
- Authority；
- 时间信息。

原因是：

```text
0.93
```

看起来非常精确，但通常并不能告诉用户：

> 不确定性究竟来自哪里？

而 Evidence-backed Status 更容易理解和审计。

---

## 22. 冲突知识

VKDD2 不假设系统一定能立刻得到一个“唯一真相”。

```text
Business Documentation 说 X。
Production Code 实现 Y。
Historical Behavior 表明 Y。
Trader 说 Z。
```

VKDD 应该允许：

```text
Claim A
supported by Evidence 1

Claim B
supported by Evidence 2

Conflict:
A != B

Resolution:
pending
```

也就是说：

VKDD 表示的是：

**Evidence-backed Knowledge State**

而不是强行假设：

```text
Everything has one known truth.
```

---

## 23. VKDD2 中 AI 的角色

AI 成为 VKDD 的 Operator，而不是 Truth Source。

可能包括：

```text
Discovery Agent
Implementation Agent
Validation Agent
Impact Analysis Agent
Review Agent
```

例如：

```text
Developer:

将 EURUSD Tier-A markup
从 1.2bp 改成 1.3bp。

Agent:

1. 找到受影响的 Claim
2. 找到支持它的 Evidence
3. 找到 Executable Contract
4. 找到所有已知 Realization
5. 提议 Claim Revision
6. 修改 Implementation
7. 更新 Tests
8. 执行 Validation
9. 报告剩余 Conflict
```

非常重要的架构规则是：

```text
Agent proposes.
Contract constrains.
Validation demonstrates.
Authority decides.
```

AI 不应该静默修改已经被接受的业务 Truth。

---

## 24. Agent Context

VKDD2 可以向 Coding Agent 提供结构化 Context。

不是简单把整个 Repo 丢给 Agent，而是给：

```text
Change Intent
Relevant Claims
Relevant Evidence
Executable Contracts
Affected Realizations
Required Invariants
Required Validation
```

例如：

```bash
vkdd context CHG-1029
```

输出一份紧凑的 Semantic Context Package。

这可以：

- 减少 Context 消耗；
- 降低 AI 自行猜测；
- 减少搜索范围；
- 提高 Change 的可解释性。

---

## 25. Knowledge Drift

VKDD2 可以持续检查 Semantic Drift。

```text
                Contract
              /    |     \
             /     |      \
          C++    Excel    Runtime
```

一次 Validation 可能得到：

```text
Contract → C++       VALID
Contract → Excel     INVALID
Contract → Runtime   VALID
Contract → Tests     VALID
```

这样就可以明确发现：

> Excel Realization 已经偏离了系统预期语义。

这种信息比普通 Test Coverage 更有意义。

---

## 26. Knowledge Health

可以从系统状态推导出一个 **Knowledge Health** 概念。
```text
Validated Claims:        94%
Validated Contracts:     91%
Stale Validations:        7%
Unresolved Conflicts:     3
Unknown Realizations:     5
```

这里更推荐展示具体指标，而不是再合成一个不透明的总分。

---

## 27. VKDD2 的开发者工作流

主要 UI 应围绕 Change，而不是 Knowledge Graph。

```text
Change CHG-1029

Intent
------
EURUSD Tier-A markup
1.2bp → 1.3bp

Reason
------
Pricing Policy Revision

Affected Claims
---------------
1

Affected Contracts
------------------
1

Affected Realizations
---------------------
5

Validation
----------
18 / 18 PASS

Unresolved Conflicts
--------------------
0

Status
------
Ready for Approval
```

用户进一步可以查看：

```text
WHY?
↓
Evidence

WHAT?
↓
Claim / Contract

HOW?
↓
Realization

PROOF?
↓
Validation
```

Graph 仍然存在，但它不一定是主要用户界面。

---

## 28. 一个可能的 VKDD2 Repository 结构

可以保持非常简单并且 Git-native：

```text
.vkdd/

  claims/
      FX.MARKUP.017.yaml

  contracts/
      markup.py

  evidence/
      PricingPolicy-2026-v18.md

  changes/
      CHG-1029.yaml

  validations/
      CHG-1029/
          functional.json
          regression.json
          performance.json
```

CLI 可能包括：

```bash
vkdd explain FX.MARKUP.017

vkdd impact CHG-1029

vkdd context CHG-1029

vkdd validate CHG-1029

vkdd conflicts

vkdd status
```

---

## 29. VKDD2 End-to-End 示例

假设 Requirement 是：

```text
把 Tier-A EURUSD markup
从 1.2bp 改成 1.3bp。
```

完整流程：

```text
1. 创建 Change CHG-1029。

2. 找到受影响 Claim：
   FX.MARKUP.017。

3. 获取支持该 Claim 的 Evidence。

4. 找到相关 Contract。

5. 找出所有已知 Realization：
   - C++ pricing engine
   - Excel model
   - configuration
   - unit test
   - regression test

6. 提议更新 Claim。

7. 有权限的用户批准 Semantic Change。

8. 更新 Executable Contract。

9. Coding Agent 修改 C++ Implementation。

10. 其他 Agent 修改：
    configuration / Excel / tests。

11. 执行 Validation。

12. 检查 Contract Violation。

13. 保存 Validation Evidence。

14. Accept 或 Reject Change。

15. 记录新的 System State。
```

最终结果：

```text
CHG-1029

Claim:
FX.MARKUP.017 v4

Affected Realizations:
5

Validation:
✓ functional
✓ regression
✓ Excel consistency
✓ configuration consistency
✓ performance invariant

Unresolved Conflicts:
0

Result:
VERIFIED
```

---

## 30. 两个版本最根本的区别

### VKDD1

可以概括为：

```text
Validated Knowledge System
```

主要关心：

```text
这个系统意味着什么？
我们为什么相信这些知识？
```

架构中心：

```text
Evidence
   ↓
Claim
   ↓
Specification
   ↓
Realization
   ↓
Validation
```

**Knowledge Model 是主要资产。**

### VKDD2

可以概括为：

```text
Change Assurance System
backed by Validated Knowledge
```

主要关心：

```text
我们要改什么？
它会影响什么？
我们如何证明修改后的系统仍然正确？
```

架构中心：

```text
Change
  ↓
Claim
  ↓
Contract
  ↓
Realization
  ↓
Validation
```

**安全、可解释、可证明的软件变更是主要资产。**

---

## 31. 关键架构差异

| 维度 | VKDD1 | VKDD2 |
|---|---|---|
| Primary Abstraction | Knowledge | Change |
| 主要目标 | 建立经过验证的系统知识 | 安全执行并验证软件修改 |
| 起点 | Evidence / Claim | Change Intent |
| Semantic Layer | Claim | Claim |
| Executable Layer | PSL / Formal Spec | General Executable Contract |
| DSL 重要性 | 可能是核心 | Optional |
| Graph 重要性 | 高 | 更偏内部基础设施 |
| Validation | 验证知识 | Gate Change |
| Confidence | 可能是核心概念 | 明显弱化 |
| Status | 次要 | 明确 Lifecycle |
| AI 角色 | 知识发现和形式化 | Change Execution 和 Validation |
| 用户工作流 | 理解和探索系统知识 | 完成并验证 Change |
| 核心问题 | “什么是真的？” | “这次修改能否被证明正确？” |

---

## 32. VKDD1 与 VKDD2 的关系

VKDD2 并不一定否定 VKDD1。

一种可能的理解是：

```text
VKDD1
=
Knowledge Foundation

VKDD2
=
Development Workflow
built on top of that foundation
```

也就是说：

```text
VKDD Knowledge Layer

Evidence
Claims
Contracts
Relationships
Validation History

        ↓

VKDD Change Layer

Change
Impact Analysis
Agent Actions
Validation Gate
Approval
```

因此最终可能不是：

```text
VKDD1 vs VKDD2
```

而是：

```text
VKDD1 + VKDD2
```

成为同一个平台的两个 Layer。

真正需要决定的是：

> 哪一层应该定义 VKDD 的产品身份？

VKDD1 把 **Validated Knowledge** 作为 Primary Abstraction。

VKDD2 把 **Validated Change** 作为 Primary Abstraction。

---

## 33. 希望重点讨论的问题

请从第一性原理出发批判性分析 VKDD1 和 VKDD2，不要默认 VKDD2 更优。

重点讨论以下问题：

1. VKDD1 与 VKDD2 的区别是否真的属于架构层面的区别，还是只是同一系统的两种视图？
2. Change 是否真的应该成为 Top-Level Domain Object？
3. `Claim → Contract → Realization` 是否是一个合理、稳定的抽象？
4. Claim 和 Contract 的区别是否足够清晰？
5. VKDD2 中 Evidence 应该继续作为 First-Class Object，还是只是 Claim 的 Metadata？
6. 用 General Executable Contract 取代 PSL，是否会牺牲过多语义一致性？
7. 长期来看，是否仍然需要一个专门的 Specification DSL？
8. Numeric Confidence 是否应该完全删除，保留在内部，还是只用于 AI 自动发现的 Candidate Knowledge？
9. 当系统存在多个互相冲突的 Claim 时，Authority 和 Approval 应如何建模？
10. 时间语义应该如何表示：valid from、valid until、superseded by、historical behavior？
11. 如何表达 Scope，而又不让系统演变成复杂 Ontology？
12. 当源代码持续变化时，Realization Mapping 如何保持准确？
13. Semantic Impact Analysis 与传统 Dependency Analysis 的本质区别应该是什么？
14. 什么样的 Minimum Viable Implementation 能真正证明或者证伪 VKDD 这个概念？
15. VKDD 与哪些已有领域高度重叠：Requirements Traceability、Executable Specification、Digital Thread、Provenance System、Knowledge Graph、Policy-as-Code、Design-by-Contract、Formal Methods、Software Supply-Chain Attestation、AI Agent Memory？
16. VKDD 真正的新颖性在哪里？是否存在真正 Novel 的部分？
17. 怎样才能让开发人员主动愿意使用 VKDD，而不是把它看成额外 Governance Overhead？
18. 哪一种架构更适合 Autonomous Coding Agent？
19. 哪些 Failure Mode 会导致 VKDD 自己也包含过期或错误知识？
20. VKDD 如何避免自己变成一个新的“第二套 Source of Truth”，最终反过来也和实际系统产生 Drift？

请同时分析 VKDD1 和 VKDD2 的优势、弱点、隐含假设和长期可扩展性，而不是只选择一个版本。