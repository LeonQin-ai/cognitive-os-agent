# Cognitive OS：项目介绍与演示素材

## 一句话介绍

Cognitive OS 是一个用 C11 编写的本地 Agent 运行时，把任务调度、工具执行、记忆、上下文预算与恢复记录放进可检查的系统。

**English:** Cognitive OS is a C11 agent runtime for inspectable tool execution, task scheduling, memory, context budgets and recovery.

## 项目入口

- 源码：https://github.com/LeonQin-ai/cognitive-os-agent
- 下载：https://github.com/LeonQin-ai/cognitive-os-agent/releases/latest
- 首次任务：https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/GETTING_STARTED.md
- 源码与实测文章：https://leonqin-ai.github.io/
- 控制台截图：[执行过程](screenshots/execution.png)、[聊天界面](screenshots/ui.png)
- [项目分享图](screenshots/project-card.png)
- [连续运行录像](backend/docs/runtime-demo.mp4)及[验证数据](backend/docs/runtime-demo-evidence.json)

## 中文社区介绍

**标题：我用 C11 写了一个 Agent Runtime：从工具调用到可验证的任务交付**

我在做 Cognitive OS，一个用 C11 实现的本地 Agent 运行时。想解决的问题很具体：模型说“完成了”以后，系统怎样证明文件真的生成了？多个角色之间有先后依赖时，谁负责等待？执行中断后，哪些结果还能被信任？

项目把调度、状态、记忆、上下文预算、工具执行和恢复记录放进 Runtime，提供桌面 / Web 控制台与 HTTP API。可以在执行历史里查看模型与工具阶段、文件变化和任务结果，也可以用 Flow DAG 声明角色之间的依赖。

第一次体验可以先创建一个内容为 hello 的 note.txt，然后同时检查执行记录和实际文件。我整理了安装与首次任务指南，以及 8 篇从源码到实验的中文文章。

仓库保留了本地确定性负载的百万任务调度数据。它测量 Runtime，不是百万次模型调用；连续演示也使用确定性模型 fixture，工具和文件操作实际执行。真实模型任务需要配置自己的提供方。项目仍在演进，特别希望收到安装体验、协议兼容性与任务完成边界方面的可复现反馈。

源码：https://github.com/LeonQin-ai/cognitive-os-agent

体验指南：https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/GETTING_STARTED.md

## English introduction

**Title: Show HN: Cognitive OS — a C11 runtime for inspectable AI agent execution**

I'm building Cognitive OS, a local agent runtime written in C11. The question behind it is what happens between a model proposing a tool call and a task actually being complete.

The runtime manages task scheduling, memory, context budgets, tool execution and recovery records. It includes a desktop/web console, an HTTP API, and DAG-based multi-agent flows. A small first task is to create a file, inspect the tool steps, and verify the actual result in the workspace.

Windows, Linux and macOS packages are available. Real-model tasks require your own configured provider. The continuous demo uses a deterministic model fixture with real Runtime and tool execution. Published million-task measurements exercise local deterministic runtime workloads, not a million model requests. Recovery tests cover specific file-operation boundaries rather than universal exactly-once execution.

I'd appreciate reproducible feedback on setup, provider compatibility, and cases where the reported task state disagrees with the actual artifact.

Source: https://github.com/LeonQin-ai/cognitive-os-agent

First task: https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/GETTING_STARTED.md

## 60 秒演示提纲

| 时间 | 画面 | 要展示的事实 |
| --- | --- | --- |
| 0–10 秒 | 控制台与一个文件任务 | 用户目标是什么 |
| 10–30 秒 | 模型阶段、工具与路径 | 动作怎样被执行和记录 |
| 30–45 秒 | 实际工作区文件 | 内容与目标是否一致 |
| 45–60 秒 | 依赖流程或执行历史 | 系统层的价值，以及源码 / 下载入口 |

已有录像为原速连续记录。剪辑或重新录制时注明模型来源，使用实际界面，避免将 fixture 画面说成真实模型能力展示。

## 衡量传播结果

先记录发布前的 GitHub views / unique visitors、clones / unique cloners、Stars、Release 下载量，以及可复现问题的数量。后续按相同窗口复查，关注访问是否转为安装与反馈。平台通常只提供有限的统计窗口；不要把缺失值解释为零，也不要把下载次数等同于活跃用户。

发布材料应围绕一个实际问题或一个可复现实验展开，例如“如何判断文件任务真的完成”或“DAG 如何阻断缺失上游结果”。更新时核对 Release、截图、指南与测量版本，避免只重复功能列表。
