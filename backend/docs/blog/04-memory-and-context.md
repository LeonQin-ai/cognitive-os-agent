# 记忆与上下文分开管理：Cognitive OS 的 Memory Service 与 Context MMU

> 一段信息值得长期保留，并不代表每次请求都应把它加载进模型窗口。做 Agent 时，我发现“存什么”和“当前读什么”需要两套相互配合的机制。

这是 [Cognitive OS](https://github.com/LeonQin-ai/cognitive-os-agent) 技术系列第 4 篇。本文关注任务之外仍然存在的知识，以及一次推理实际使用的工作集。

## Memory 回答“保留什么”

项目通过 Memory Service 接口访问四类记忆：

| 类型 | 用途 |
| --- | --- |
| Working | 当前短期工作信息 |
| Episodic | 任务与结果形成的经验 |
| Semantic | 长期事实 |
| Procedural | 操作方法与过程知识 |

接口暴露 remember、forget、按键和按查询 recall 等操作，底层通过 vtable 接入实现。当前默认后端委托给项目自己的 memory facade；接口留下了替换空间，但不能把这种设计可能性写成已经完成的所有数据库集成。

更重要的是写入条件。一个任务只输出了计划、卡在工具失败或被取消，其内容不应该被当成成功经验共享给其他会话。Runtime 将完成证据和长期记忆更新联系起来，避免“我准备创建文件”逐渐变成系统记忆中的“文件已创建”。

## Context 回答“这次加载什么”

模型上下文的容量与使用成本有限。即使长期记录全部保存，当前推理也只能读取一个经过选择的集合。

Context MMU 中有两个互相独立的维度：

| 维度 | 取值 | 含义 |
| --- | --- | --- |
| 表示粒度 | L0 / L1 / L2 | 摘要索引、概览、细节 |
| 驻留状态 | HOT / WARM / COLD | 细节已加载、概览已加载、主要保留元信息 |

因此，“冷”不等于“内容被永久删掉”，“概览”也不等于“原文”。原始规范记录由记录存储保留，当前活动表示可以按预算被驱逐或重新物化。

## 一次召回的路径

```text
查询
 → 候选记录
 → 相关性及其他因素排序
 → WARM / L1 概览
 → 再排序
 → 部分记录提升到 HOT / L2
 → 超预算时淘汰
```

排序会记录 relevance、importance、confidence、utility、recency、salience、scope 等因素。与只输出一个“相关度分数”相比，这能帮助定位为什么某条记录进入或离开工作集。

表示通过内容 hash 缓存，正文变化后才需要重新派生。被选中的页面并不都能无限提升到 HOT，预算约束需要同时覆盖活动表示和缓存，避免某个未计费的细节副本绕过管理。

## “Context 预算”为什么不能等同于进程内存

这里很容易产生宣传上的误解。MMU 管理的 token 预算，针对的是被物化的上下文表示；整个进程还可能持有规范原文、索引、序列化缓冲、调度对象和系统分配器空间。

所以一个 1 GiB 原文压力实验，可能占用更多 RSS。项目保存的 768/128/128 MiB 布局实验中，峰值 RSS 约为 3197.01 MiB。这说明测试确实有额外内存成本，不能只展示缩小后的概览字节数，就把它描述为整个系统的常驻内存。

概览是有损的。测试核对的“完整恢复”，指从规范记录重新加载正文后，原始长度和内容 hash 一致；它不表示可以从有损概览单独重建全部原文，也不代表向远程模型发送过 GiB 级输入。

## 怎样检查这套实现

先构建 `runtime-bench`，再运行：

```bash
cd backend
python tests/test_runtime_benchmark.py
```

其中的小规模测试会执行真实 MMU 的 HOT 分配、驱逐、概览加载、重新物化和磁盘校验。CI 使用较小的数据检查正确性；历史大内存数据与运行条件保存在基准报告中。

源码：[Memory Service](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/include/memory/service.h)、[Context MMU](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/src/context/mmu.c)、[记录存储](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/src/memory/record.c)。数据：[扩展基准报告](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/docs/runtime-benchmark-expanded.md)。

上一篇：[Flow 依赖](03-flow-dependencies.md)；下一篇：[工具协议与执行证据](05-tool-protocol-and-evidence.md)。如果你正在处理长任务和大型代码库，欢迎从“规范内容与活动表示分离”这个边界一起讨论。
