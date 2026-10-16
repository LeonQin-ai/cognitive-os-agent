# 用三个小实验体验 Cognitive OS：文件执行、角色讨论与故障边界

> 介绍一个 Agent 项目，最有说服力的方式是给读者一条能跑、能观察、能核对的路径。这篇用三个小实验，把前面讨论的 Runtime 能力串起来。

这是 [Cognitive OS](https://github.com/LeonQin-ai/cognitive-os-agent) 技术系列第 8 篇，也是整组文章的实践入口。

## 准备一个独立工作区

按照仓库构建文档生成 CLI。Windows 示例：

```powershell
cd backend
.\build.ps1 -Target cli
New-Item -ItemType Directory -Force state-blog-demo,workspace-blog-demo | Out-Null
.\build\cognitive-os-agent.exe --state state-blog-demo --workspace workspace-blog-demo serve 18300
```

在启动前创建测试目录，并在所选 state 中配置模型；也可以先用项目的 mock 模式理解调用链。mock 用于确定性离线验证，不能用它的固定回答评价真实模型的理解质量。

服务启动后打开 `http://localhost:18300`，在模型页面配置你使用的接口和模型；配置项与本地模型接入方式见文末运行文档。密钥保留在本地配置中，不要放进准备公开的文章、截图或 issue。

Linux/macOS 按仓库 `make` 或 CMake 构建说明执行，对应二进制通常不带 `.exe`。下面的 HTTP 示例使用同一个 `18300` 端口。

## 实验一：让文件结果说话

向 `POST /v1/chat` 发送：

```json
{"message":"创建 note.txt，内容为 hello","session":"blog-files"}
```

拿到 `id` 后，查询 `GET /v1/tasks/<id>`。不要只停在最终自然语言答案：检查任务状态、工具步骤、测试 workspace 中的文件和实际内容。

再读取 `/v1/tasks/journal`，观察持久任务记录。这一步把“可以执行动作”的演示升级成“可以核对动作与记录”的演示。

如果所用模型不按工具协议提出动作，Runtime 可能要求继续规划或报告未完成。遇到失败应保留模型协议、错误和步骤，而不是为了演示好看把失败当成成功。

## 实验二：让架构师与评审互相回答

先注册两位角色：

```bash
curl -X POST http://localhost:18300/v1/agents \
  -H 'Content-Type: application/json' \
  -d '{"name":"architect","role":"提出方案，并回应评审的问题"}'
curl -X POST http://localhost:18300/v1/agents \
  -H 'Content-Type: application/json' \
  -d '{"name":"reviewer","role":"检查边界条件，提出具体问题并核对回答"}'
```

创建群聊并记录返回的房间 `id`：

```json
{
  "name":"方案讨论",
  "kind":"group",
  "members":["architect","reviewer"],
  "discussion_rounds":3
}
```

向 `POST /v1/im/sessions/<id>/messages` 提交：

```json
{"content":"讨论如何给源码阅读任务设计完整性验证。请互相提问，并说明各自观点需要哪些证据。"}
```

重点观察第二轮：架构师是否真的回答了评审的具体问题？评审是否针对回答更新意见？单纯重复角色介绍不算有深度的讨论。轮数和输入管道由 Runtime 提供，观点质量仍需读者判断。

界面左侧可以看到当前角色和轮次，也可以停止。讨论期间发送新的用户消息，旧任务会被接管，迟到回复不能继续进入房间。尝试一次这样的补充要求，比只看一段顺畅的最终回答更能理解调度边界。

## 实验三：用测试观察失败

构建 CLI 与测试程序后，另开终端，从仓库根目录运行以下回归：

```bash
cd backend
python tests/test_im_discussion_e2e.py
python tests/test_completion_recovery_e2e.py
python tests/test_crash_recovery_e2e.py
```

它们使用自己的临时状态与测试进程：讨论测试检查互相引用、停止、接管和失败；阅读测试检查未到 EOF 的任务如何继续；崩溃测试终止其创建的 Runtime，核对中断识别与显式恢复。

这些用例使用本地 fixture，便于反复复现控制流。它们不会证明所有生产模型行为和所有外部命令都已被覆盖。

## 可以从哪里参与

如果你喜欢 C 系统编程，可以阅读调度器的生命周期、引用和取消边界；如果关注 Agent 行为，可以增加真实失败的最小回归；如果熟悉文档、多模态或不同模型协议，可以提供带脱敏输入、格式与错误阶段的兼容性案例。

一个有帮助的 issue 通常包含：使用版本、操作系统、模型与接口类型、最小触发步骤、预期行为、实际任务状态、展开后的工具结果。不要提交密钥、SSH 密码或不希望公开的文件。

项目仍有很多值得探索的地方，例如多轮讨论的质量评估、更细的 Context 选择、外部动作的幂等设计和跨平台性能复测。欢迎先跑一个小实验，再带着观察参与讨论。

项目：[GitHub 仓库](https://github.com/LeonQin-ai/cognitive-os-agent)、[中文 README](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/README.zh-CN.md)、[运行与 API 文档](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/README.md)。演示：[真实操作录像](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/docs/runtime-demo.mp4)。

上一篇：[百万任务实测](07-million-task-benchmark.md)；返回[系列目录](README.md)。如果这些工程问题也是你正在解决的，欢迎关注、试用或贡献 Cognitive OS。
