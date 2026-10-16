# 我为什么用 C11 写一个 Agent Runtime

> 当一个 Agent 开始读文件、运行命令和协调其他角色，问题就从“模型怎么回答”扩展为“系统怎么执行”。Cognitive OS 是我沿着这条边界做的一个开源项目：把任务状态、调度、记忆、上下文和执行放进可检查的 Runtime。

项目地址：[cognitive-os-agent](https://github.com/LeonQin-ai/cognitive-os-agent)。本文是源码与实测系列的第 1 篇。

## 从“创建 note.txt”看问题

用户输入“创建 note.txt，内容为 hello”，一个有用的系统至少要回答这些问题：工作目录在哪里？模型返回的工具参数是否有效？写入失败时保留什么？前端显示完成，是否真的代表文件存在？进程重启后还能不能找到执行记录？

这些问题分布在同一条链路上，却不能只靠一段提示词解决。模型可以提出 `file_write` 动作；动作解析、策略检查、实际文件操作和状态记录仍需要确定性的程序负责。

在 Cognitive OS 中，这条链路大致是：

```mermaid
flowchart LR
    U[用户请求] --> A[HTTP / CLI]
    A --> S[任务调度器]
    S --> R[Reasoning Loop]
    R --> L[模型提出计划]
    L --> P[计划解析与策略]
    P --> T[工具与事务]
    T --> V[观察与完成校验]
    V --> R
    V --> J[任务记录与界面]
```

模型会多次参与推理，Runtime 则持续掌握任务的生命周期。一次调用返回了自然语言，并不意味着整项工作已经完成。

## C11 的选择意味着什么

我希望尽量直接地控制对象所有权、线程、文件和跨平台接口，因此用 C11 实现核心。项目通过 OS 抽象层连接线程、协程、网络、进程和文件能力；上层的调度、推理和工具接口复用这些基础设施。

这种选择会增加内存管理、错误路径和平台适配的工作量。它的价值在于系统行为比较容易从调用链追踪：谁创建任务、谁持有引用、谁写终态、哪个边界检查取消，都可以落到具体函数。

这并不自动带来更好的推理质量，也不能仅凭实现语言推导性能优势。模型质量取决于选用的模型、输入证据和任务；Runtime 的性能需要用实际负载测量。

## 把职责拆开之后

目前的核心可以从四个方向阅读：

| 层次 | 负责的事 | 入口 |
| --- | --- | --- |
| 调度与生命周期 | 排队、执行、取消、完成历史 | `runtime/scheduler.c` |
| 推理循环 | 规划、工具观察、重试与完成判断 | `cognition/reasoning.c` |
| 知识与上下文 | 记忆访问、召回、活动上下文预算 | `memory/`、`context/` |
| 动作与交互 | 工具、事务、HTTP、Web 界面 | `action/`、`tx/`、`api/` |

例如多 Agent 不必都占用一个独立进程。角色由 AgentPool 注册，任务由调度器管理；角色数量、排队任务数量和实际工作线程数量是不同指标。后面讨论百万任务测试时，这个区别非常重要。

## 一个可以核对的入口

按照仓库构建说明启动服务，默认示例端口使用 `18300`。在独立测试 workspace 中，可以发送：

```bash
curl -X POST http://localhost:18300/v1/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"创建 note.txt，内容为 hello","session":"blog-demo"}'
```

接口返回任务 `id`。随后读取 `/v1/tasks/<id>`，核对终态和工具步骤，再检查工作目录中的文件。`/v1/tasks/journal` 用于查看持久任务历史。这样的演示把“模型说完成”和“可以检查的结果”连起来了。

工具执行使用运行账户拥有的权限。策略、事务与审计可以帮助约束和追踪操作，但它们不构成对任意外部命令的安全保证；适合先在明确范围的测试工作区里体验。

## 为什么值得继续看

Cognitive OS 最吸引我的部分，是把 Agent 问题拆成可验证的系统问题：讨论如何停止、依赖何时满足、上下文为什么被淘汰、恢复后能否继续执行。这个项目仍在演进，适合对 Agent 基础设施、C 系统设计和可靠执行感兴趣的开发者一起研究。

源码：[Runtime 装配](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/src/cognitive_os_agent.c)、[Scheduler](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/src/runtime/scheduler.c)、[Reasoning](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/src/cognition/reasoning.c)。

下一篇：[让多个 Agent 真正讨论起来](02-agent-group-discussion.md)。欢迎运行项目、提出可复现的问题，或从一个小模块开始参与。
