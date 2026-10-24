# 先完成一个可验证的 Agent 任务 / Your first verifiable task

[返回中文首页](README.zh-CN.md) · [English README](README.md) · [完整配置与 API](backend/README.md)

## 1. 下载并打开

在 [最新版本](https://github.com/LeonQin-ai/cognitive-os-agent/releases/latest) 选择系统对应的文件：Windows 使用安装器，macOS 选择 ARM64 或 Intel 应用包，Linux 使用 x64 便携包。版本页的 `SHA256SUMS` 用于核对下载文件。

Windows / macOS 打开应用后进入控制台。Linux 解压后在包目录运行 `./cognitive-os-agent serve 18300`，浏览器打开 `http://localhost:18300/`。详细构建、启动和平台说明以 [开发文档](backend/README.md) 为准。

## 2. 配置模型与独立工作区

在控制台配置模型提供方、Base URL、模型名与凭据，使用一个专门用于体验的工作区。真实任务会调用你配置的模型并产生相应费用；密钥保存在本地配置，不要放入 Issue、截图或示例代码。

项目支持 OpenAI / Anthropic 协议适配；具体模型的工具与多模态能力需要按提供方配置。离线 mock 是测试工具，不会把任意自然语言请求变为真实模型回答。

## 3. 提交一个文件任务

输入：

> 在当前工作区创建 note.txt，内容为 hello。完成后报告实际写入的路径。

观察执行历史中的模型阶段与工具步骤。完成后打开实际工作区，确认：

1. `note.txt` 出现在预期目录；
2. 文件内容确实是 `hello`；
3. 执行记录中的结果与文件系统一致。

这三个检查把“模型说完成”和“结果可验证”连接起来。失败时先看工具是否真的执行，再检查路径、权限与模型配置。

## 4. 用 API 观察相同的任务

在服务已启动且认证设置允许的本地环境中，可以运行：

```bash
curl -X POST http://localhost:18300/v1/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"Create note.txt with content hello"}'
```

保存返回的任务 ID，然后查询 `GET /v1/tasks/<id>`。不要直接复制固定 ID。Windows PowerShell 可使用 `curl.exe`；启用认证时按开发文档附加认证头。桌面应用和独立 CLI 默认使用相同端口，避免同时启动两份服务。

## 5. 再选一条深入路线

| 想探索的能力 | 下一步 |
| --- | --- |
| 多角色互相讨论 | [讨论的上下文、轮次与接管](https://leonqin-ai.github.io/articles/agent-group-discussion/) |
| 先读证据再执行下一步 | [DAG 依赖与失败传播](https://leonqin-ai.github.io/articles/flow-dependencies/) |
| 长任务为什么漏信息 | [Memory 与 Context 预算](https://leonqin-ai.github.io/articles/memory-and-context/) |
| 中断后怎样确认结果 | [完成证据与恢复](https://leonqin-ai.github.io/articles/reliability-and-recovery/) |

不配置真实模型也可观看 [连续演示录像](backend/docs/runtime-demo.mp4)，或按 README 运行离线回归。录像使用确定性模型 fixture，工具与 Runtime 实际执行。

## English walkthrough

1. Download the matching package from [Releases](https://github.com/LeonQin-ai/cognitive-os-agent/releases/latest) and check `SHA256SUMS`. Open the desktop app, or run the extracted Linux binary with `serve 18300` and visit `http://localhost:18300/`.
2. Configure your provider, endpoint, model and credentials. Choose a dedicated workspace. Real-model tasks use your provider account; offline mocks are deterministic test fixtures.
3. Ask: **Create note.txt in the current workspace with content hello. Report the actual path.**
4. Inspect the task stages and tool results, then open the workspace and verify the file's path and contents independently.
5. To use HTTP, submit the curl request above and query `/v1/tasks/<id>` with the returned ID. Follow your authentication configuration, and do not run the desktop app and a separate server on the same port.

For a bug report, include the version, OS, provider protocol, redacted input, expected behavior and observed result. [Open an issue](https://github.com/LeonQin-ai/cognitive-os-agent/issues/new/choose).
