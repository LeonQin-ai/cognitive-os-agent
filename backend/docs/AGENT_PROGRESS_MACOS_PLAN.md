# Agent 可观测性、缺陷修复与 macOS 支持计划

日期：2026-09-21。状态：核心实施与跨平台验收已完成；发布签名、公证和安装器仍属于发布阶段工作。

## 目标与范围

用户发送消息后立即得到反馈；执行时能看到当前阶段、简短操作说明和步骤状态；完成时得到结论并保留可展开的执行记录。长等待、失败、取消、重连均可辨识。

“思考中”使用真实阶段和面向用户的简短说明，不直接展示模型内部推理、系统提示或原始 round_log。简单问答无需强行生成计划。

macOS 分两层交付：先实现可验证的 CLI + 本地 Web 控制台，再交付原生桌面 .app 与安装产物。覆盖 Apple Silicon 和 Intel；分别验证后才声明对应架构受支持。

## 扫描发现

以下为首轮扫描时的代码证据。相关核心问题已经实施修复；完成情况见文末验收记录。

| 优先级 | 问题与证据 | 影响与处理 |
| --- | --- | --- |
| P1 | `src/cognition/reasoning.c:1233` 的 run_step_add 扩容 steps 并先递增 n_steps；`:2654` 的 reasoning_steps_json 无锁遍历。round_log_append 与 reasoning_round_log_tail 同样无锁共享可扩容缓冲区。 | 存在数据竞争、读未完成记录及失效内存的风险。引入同步快照；同步写入、重置、读取与生命周期，不能只锁读端。 |
| P1 | `CMakeLists.txt:20` 引用不存在的 src/cognitive-os-agent.c；`:71` 起引用不存在的 src/os/os_*.c；非 Windows 分支只选 linux，遗漏 posix 和 macos 分流。 | CMake 构建入口已失效；对照实际源码补齐新增模块，统一三种构建入口的平台选择。 |
| P2 | `apps/web/index.html:2307` 起按 chatIsBusy 接受全局事件，未校验 task/session；`src/cognitive_os_agent.c:340` 起广播总线事件。 | 多会话时进度串线。事件携带 task_id、session_id、run_id、seq，并按任务归属路由。 |
| P2 | `apps/web/index.html:2830` 附近 showLive 无步骤和日志即返回，初始气泡仅为省略号；步骤仅执行后加入；RUNNING 一律显示思考中。 | 首次模型请求、长工具执行缺乏准确反馈。增加阶段及 step started/finished 生命周期。 |
| P2 | showLive 仅更新 DOM、不更新消息缓存；setBot 最后覆盖执行内容。 | 切页重绘暂时丢失进度，结束后步骤不可回看。消息模型分别保存 progress、steps、answer。 |
| P2 | chatSend 在 POST 返回后才登记 CHAT_TASKS。 | 提交等待窗口内重复触发发送可再次提交。提交前设置会话 submitting 状态，失败时释放，并增加请求幂等标识。 |
| P2 | chatSend 轮询处理 404 和任务终态，但网络 status=0、401、500 均继续每 300ms 重试。 | 断线缺少明确提示且持续高频请求。区分认证失败、暂时断线与终态，退避并支持恢复。 |
| P2 | API 将模型用途的 round_log 作为 process_log 返回，前端存在直接展示回退路径。 | 可能显示内部提示和杂乱日志。改成经过筛选、脱敏的用户事件摘要。 |
| 支持缺口 | Makefile/build.sh 已有 Darwin 分支，macos/http.c 复用 HTTP 实现；package.sh 与 tools/desktop.cc 使用 Windows 专属链路；CI 仅 Windows/Linux。 | macOS 有基础适配，尚不具备完整验证和桌面发布链路。 |

## 实施任务与顺序

### T1：执行链路稳定性（先行）

- 为进度、步骤、日志提供任务级一致快照；明确任务与执行 lane 的绑定和释放时机。
- 修复提交期间重复发送，处理网络错误及取消请求中的中间状态。
- 复核任务终态输出发布、lane 复用与任务回收的并发安全。
- 验收：并发会话不串线；大日志扩容与高频快照压力测试无内存错误；快速连续发送只有一个任务；临时网络中断不清空已有内容。

### T2：结构化执行事件（依赖 T1）

- 统一事件字段：task_id、session_id、run_id、seq、timestamp、stage、step_id、status、summary。
- 阶段：queued、analyzing、planning、executing、summarizing，以及 completed、failed、cancelled、timed_out。
- 模型调用开始、工具调用开始/结束、重试和终态均发事件；长耗时期间仅显示仍在等待与耗时，不伪造进展。
- WebSocket 推送与 HTTP 快照共用数据源；有序去重、断线后快照补齐；持久化最终步骤记录并设置容量上限。
- 展示内容走摘要及脱敏边界，原始工具输出默认不进入主对话。
- 验收：快速连续事件无丢失/重复；工具运行中即可看到步骤；重连不改变终态；执行线程不被慢客户端阻塞。

### T3：对话体验（依赖 T2）

- 发送即显示“已收到，正在准备”；排队时明确显示等待。
- 回复区域分成当前状态、可折叠步骤、最终答案。示例：分析需求 → 检查相关文件 → 执行修改与验证 → 整理结论。
- 步骤显示等待/执行中/成功/失败及耗时；说明使用自然语言，例如“正在检查构建配置”。
- 完成后折叠过程但允许展开；最终结论说明结果、验证和未解决项。
- 按会话缓存状态，切页和刷新后恢复；只在用户位于底部时跟随滚动；状态变化支持辅助阅读和减少动画设置。
- 如接入答案增量输出，仅推送用户可见回答片段；不要把规划 JSON 或内部推理当答案流输出。
- 验收目标：本地交互反馈 100ms 内出现；本地模拟事件产生后 500ms 内显示（均需实测）；慢模型、慢工具、失败、取消和切换会话均有明确状态。

### T4：macOS 核心支持（可与 T2/T3 开发阶段并行）

- 修复 CMake 源文件清单和 APPLE/POSIX 选择，统一构建时 Web UI 生成，检查 Makefile 与 build.sh 的一致性。
- 审核 POSIX 适配：opaque pthread 存储尺寸/对齐增加静态断言；检查 ucontext、socket 断连信号、进程终止/回收、动态 libcurl、路径和临时目录。
- 在真实 macOS runner 上分别编译、执行单元/场景/adapter/E2E 测试；保留 Windows/Linux 回归。
- 验证 HTTPS、shell/git 工具、MCP 子进程、取消、状态持久化与 Web 控制台。
- 验收：两种架构均有原生执行证据；不能用 Windows 静态检查或交叉编译代替 macOS 运行验证。

### T5：macOS 桌面与交付（依赖 T4）

- 为桌面入口增加 Cocoa/WKWebView 宿主或采用仓库现有 webview 抽象的 macOS 实现。
- 完成后端启动、端口冲突处理、就绪检查、退出清理及用户数据目录适配。
- 产出 .app、Info.plist、图标和安装包，补充安装/启动/卸载文档。
- 签名与公证取决于 Apple 开发者凭据，作为发布条件单独记录；不阻塞本地构建与运行验证。
- 验收：干净 macOS 环境可安装启动、完成任务、关闭退出，无残留后端；签名版本另做 Gatekeeper 验证。

## 建议交付拆分

1. PR 1：并发快照、提交与轮询缺陷修复及针对性回归测试。
2. PR 2：任务事件协议、步骤存储与 WebSocket/快照恢复。
3. PR 3：对话过程卡片、状态文案、历史恢复和增量答案。
4. PR 4：构建统一、macOS 核心适配和 CI。
5. PR 5：macOS 桌面与打包文档。

## 实施与验收记录

| 任务 | 状态 | 代码与验证证据 |
| --- | --- | --- |
| T1 执行链路稳定性 | 完成 | 进度、步骤和日志改为同步快照；提交状态、取消、恢复及任务归属已有单元和场景覆盖。 |
| T2 结构化执行事件 | 完成 | 任务事件包含任务、会话、运行序号、阶段和步骤状态；WebSocket 与 HTTP 快照可恢复同一执行状态。 |
| T3 对话体验 | 完成 | 已实现即时接收反馈、阶段状态、步骤卡片、最终答案、历史恢复，以及运行中追加消息并据此调整任务。 |
| T4 macOS 核心支持 | 完成 | CMake/平台源码选择已统一；GitHub Actions 在 `macos-15` 与 `macos-15-intel` 上通过单元、场景、基准、adapter 和 E2E。Windows、Linux、ASAN 同轮通过。 |
| T5 macOS 桌面与交付 | 部分完成 | Cocoa/WKWebView `.app`、Info.plist、后端生命周期、临时签名和架构 ZIP 已在两种 macOS runner 打包并上传。正式图标、安装器、Developer ID 签名、公证及 Gatekeeper 发布验证仍待发布凭据和品牌资产。 |

最近一次完整跨平台门禁：GitHub Actions run `35591167695`，五个作业全部成功。实现提交从 `90f9b02` 开始，跨平台测试修复为 `42847a6`，macOS 打包修复为 `0280c85`。

## 剩余发布任务

- 提供正式 `.icns` 品牌图标并写入 `CFBundleIconFile`。
- 选择 DMG 或 PKG 安装形式，补充可视化安装与卸载验证。
- 使用 Apple Developer ID 对应用签名并提交公证，验证 Gatekeeper 首次启动。
- 在正式发布工作流中保留 SHA256 校验和、签名身份和公证票据。
