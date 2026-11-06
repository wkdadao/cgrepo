# C++ / CPU 讲义

本目录包含 13 份 PDF 的完整 Markdown 转写，共 478 页。PDF 保留；MD 与对应 PDF 同名。

## 阅读目录

| 层次 / 专题 | Markdown | PDF 页数 |
| --- | --- | --- |
| CPU Execution Model | [Level1_CPU_Execution_Model_CN.md](Level1_CPU_Execution_Model_CN.md) | 38 |
| C++ → Assembly → CPU | [Level2_CPP_Assembly_CPU_CN.md](Level2_CPP_Assembly_CPU_CN.md) | 33 |
| Memory Hierarchy | [Level3_Memory_Hierarchy_CN.md](Level3_Memory_Hierarchy_CN.md) | 33 |
| CPU Latency Pathology | [Level4_CPU_Latency_Pathology_CN.md](Level4_CPU_Latency_Pathology_CN.md) | 31 |
| CPU Performance Profiling | [Level5_CPU_Performance_Profiling_CN.md](Level5_CPU_Performance_Profiling_CN.md) | 38 |
| Linux OS Runtime | [Level6_Linux_OS_Runtime_CN.md](Level6_Linux_OS_Runtime_CN.md) | 36 |
| Network / NIC / Kernel Bypass | [Level7_Network_NIC_Kernel_Bypass_CN.md](Level7_Network_NIC_Kernel_Bypass_CN.md) | 39 |
| Low-Latency C++ Design Patterns | [Level8_Low_Latency_CPP_Design_Patterns_CN.md](Level8_Low_Latency_CPP_Design_Patterns_CN.md) | 54 |
| Concurrency Architecture | [Level9_Concurrency_Architecture_CN.md](Level9_Concurrency_Architecture_CN.md) | 33 |
| Compiler Optimization | [Level10_Compiler_Optimization_CN.md](Level10_Compiler_Optimization_CN.md) | 39 |
| Lock-Free Programming | [Topic_Lock_Free_Programming_CN.md](Topic_Lock_Free_Programming_CN.md) | 32 |
| Low-Latency System Design | [Topic_Low_Latency_System_Design_CN.md](Topic_Low_Latency_System_Design_CN.md) | 43 |
| Memory Ordering &amp; Atomics on x86 | [Topic_Memory_Ordering_Atomics_x86_CN.md](Topic_Memory_Ordering_Atomics_x86_CN.md) | 29 |

## 阅读与核对

- 每个 MD 保留原章节顺序、原文目录和逐页定位链接。
- 表格改为 Markdown；代码和命令使用独立代码块。
- 图示用 Mermaid、比较表或步骤表达；展开“查看原图与完整文字转写”可查看原始图形。
- 新增技术说明明确标注“转换补充 / 转换勘误”；建议先阅读 Level 8 中关于 seqlock 示例的勘误。
- 支持 Mermaid 的阅读器会渲染图形；不支持时仍可查看 PNG 原图。

[查看转换范围、校验结果与局限](CONVERSION_NOTES.md)

结构校验：在仓库根目录运行 `python3 cpu_c++/validate_conversion.py`；安装 PyMuPDF 后可加 `--pdf-text` 核对逐页文本。[复核修复与校验边界](CONVERSION_NOTES.md)。
