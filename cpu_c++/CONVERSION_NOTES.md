# PDF → Markdown 转换说明

转换日期：2026-11-06。原始版本：[`efbafaa18043`](https://github.com/wkdadao/cgrepo/commit/efbafaa18043998db0e4fbfa6cbc1f51948dfe64)。

## 范围

共 13 份文档、478 页；转写 285 处表格片段、276 个独立代码/命令块，以及 236 个图示区域。表格按分页片段统计，包含续表与孤立表头；已恢复 5 处续表的表头。原 PDF、已有 Markdown 和图片均保留。

| 文档 | 页数 | 表格片段 | 独立代码块 | 图示 | Mermaid |
| --- | --- | --- | --- | --- | --- |
| [Level1_CPU_Execution_Model_CN.md](Level1_CPU_Execution_Model_CN.md) | 38 | 26 | 25 | 19 | 7 |
| [Level2_CPP_Assembly_CPU_CN.md](Level2_CPP_Assembly_CPU_CN.md) | 33 | 30 | 35 | 17 | 4 |
| [Level3_Memory_Hierarchy_CN.md](Level3_Memory_Hierarchy_CN.md) | 33 | 29 | 18 | 26 | 2 |
| [Level4_CPU_Latency_Pathology_CN.md](Level4_CPU_Latency_Pathology_CN.md) | 31 | 17 | 13 | 19 | 3 |
| [Level5_CPU_Performance_Profiling_CN.md](Level5_CPU_Performance_Profiling_CN.md) | 38 | 22 | 42 | 17 | 4 |
| [Level6_Linux_OS_Runtime_CN.md](Level6_Linux_OS_Runtime_CN.md) | 36 | 37 | 17 | 28 | 5 |
| [Level7_Network_NIC_Kernel_Bypass_CN.md](Level7_Network_NIC_Kernel_Bypass_CN.md) | 39 | 24 | 14 | 18 | 0 |
| [Level8_Low_Latency_CPP_Design_Patterns_CN.md](Level8_Low_Latency_CPP_Design_Patterns_CN.md) | 54 | 10 | 27 | 14 | 3 |
| [Level9_Concurrency_Architecture_CN.md](Level9_Concurrency_Architecture_CN.md) | 33 | 20 | 13 | 16 | 6 |
| [Level10_Compiler_Optimization_CN.md](Level10_Compiler_Optimization_CN.md) | 39 | 13 | 30 | 7 | 2 |
| [Topic_Lock_Free_Programming_CN.md](Topic_Lock_Free_Programming_CN.md) | 32 | 18 | 16 | 21 | 7 |
| [Topic_Low_Latency_System_Design_CN.md](Topic_Low_Latency_System_Design_CN.md) | 43 | 27 | 13 | 18 | 8 |
| [Topic_Memory_Ordering_Atomics_x86_CN.md](Topic_Memory_Ordering_Atomics_x86_CN.md) | 29 | 12 | 13 | 16 | 2 |

## 图示处理

共 53 个 Mermaid 重建图；其余用可编辑比较表、决策层级、步骤或逐行路径转写。每个图示均保留 180 DPI PNG 原图和完整文字转写，放在 `assets/<原文档名>/`。原图包含字形、空间位置与连接线，作为视觉对照。

Mermaid 为结构化语义重建，不承诺与原图逐像素一致，也不自动代表额外的同步保证或精确硬件时序。简单链条使用步骤，明确比较关系使用表格，避免把所有内容硬画成流程图。对原文连线不清的订单状态图已标注解释性补充。

## 完整性与格式检查

- 每页建立了 source span → 输出元素的分配记录：正文 span 全部恰好分配一次，未分配和重复分配均为 0。该检查覆盖文字提取管线；不是对技术正确性的证明。
- 保留封面、使用说明、原目录、章节、附录、代码、表格和来源链接；仅移除重复运行页眉、页脚和目录点引线等版面符号。
- 每页有 `pdf-page-NNN` 锚点和原 PDF 页码链接；跨页示例保持原先的片段顺序。
- 字体丢失 Unicode 映射时，结合上下文恢复项目符号、`µop/µops`、`µs`；`CPU↔memory` 的连接符按语境恢复。英文软换行尽量合并，并保留常见复合词中的连字符。
- 校验所有 Markdown fence、相对资源与页锚点；Mermaid 11.4.1 解析并进行离线 SVG 渲染检查。离线布局使用文本测量近似，GitHub / 阅读器字体与折行可能不同；PNG 与 PDF 图示作视觉对照。

## 补充与勘误

- **实质勘误，高置信度**：Level 8 §4.8 与附录 C.3 的普通 payload seqlock 不能直接作为 portable C++ 正确实现；并发 non-atomic copy 可能已经构成 data race，retry 不消除 UB。正文旁有依据与替代方向。
- **示例边界澄清**：SPSC skeleton 省略容量与安全复用的完整协议；Hazard / EBR、dirty-bit coalescing 代码省略关键同步细节；订单 FSM 为简化业务示意。
- **平台边界补充**：llvm-mca、PMU、NUMA first-touch、THP、NAPI、compiler flags 和 atomic lowering 均注明对应适用条件或官方参考。
- 原文与新增说明分开标注；不把讲义中的经验规则升级为标准保证。

## 校验的实际边界

**覆盖置信度：高**，依据逐页提取与 span 分配检查。**排版及复杂图的语义还原置信度：中**，因为 PDF 没有原生的段落/表格/流程图结构，部分关系依版面推断；原图和原 PDF 链接用于核对。没有把“0 遗漏 span”宣称为所有技术内容已经逐字人工审定或所有示例已编译/并发验证。

完整机器清单（含 PDF/MD SHA-256、图示坐标和文件路径）：[conversion_manifest.json](assets/conversion_manifest.json)。
