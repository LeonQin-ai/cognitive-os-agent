# Issue #121 当前源码核对与修复（2026-10-13）

核对以当前源码为准。原 issue 的评估包含旧版本缺陷、设计限制和实际问题，不能将全部表格直接当成仍然存在的 Bug。

| 原编号 | 核对结论 / 处理 |
| --- | --- |
| 1 | auth_generate_token 已使用 Windows BCryptGenRandom / POSIX urandom；不再使用 LCG。 |
| 2 | strbuf/strmap 分配失败返回；没有 exit(1)。 |
| 3 | WebSocket 已限制消息和帧尺寸。 |
| 4 | kvstore_get 已返回线程局部副本，避免并发写入释放内部值。副本只在同线程下一次 get 前有效。 |
| 5 | 随机源无 seeded；secret_boundary 初始化已采用原子状态。 |
| 6 | 本次修复：认证密钥补齐至固定 512 字节；比较循环不再依赖秘密长度，遍历全部密钥。超过授权头容量的密钥拒绝注册，补边界测试。 |
| 7 | WebSocket 发送缓冲已动态分配；超过旧 16KB 限制可发送。 |
| 8 | shell 是明确授权的命令执行工具，整段命令是其接口；不能把 shell 语法全部消毒而破坏用途。调用经过 policy、sandbox 和事务层；SSH 参数另有引用保护。 |
| 9 | worker 使用 &&。 |
| 10 | Flow answer 正常释放、合并失败时保留；#116 已修复上游失败传播。 |
| 11 | trace 正常及失败路径均释放。 |
| 12 | 已有 flow_answer 回退；本次补充最终答案分配失败时返回错误，不能返回成功和 NULL。 |
| 13 | 本次修复：task_bytes 的 progress/trace 读取持 progress_mtx；运行中不读取 runner 正在修改的 output，完成输出由 scheduler 锁保护。 |
| 14 | worker 在完成回调结束后设置 completed 并 broadcast；不是只依靠 finalize_task。 |
| 15 | queue_insert 分配失败会回退新任务记录，释放自有资源。 |
| 16 | path_join 使用动态临时缓冲，输出缓冲不足会报错；不是固定 2048 截断。 |
| 17 | 本次修复：JSON 中 base_url 和 base.url 保持独立；环境覆盖独立保存。双下划线表示点，字段内部单下划线保留；兼容 LLM_BASE_URL。环境布尔/整数解析也补齐。 |
| 18 | ws_client.closed 已为 _Atomic。 |
| 19 | strbuf 扩容和追加已有 SIZE_MAX 检查。 |
| 20 | 内置服务使用 HTTP，远程 HTTPS 通过反向代理部署。这是明确的部署边界，不宣称已增加原生 TLS。 |
| 21 | ConvertThreadToFiber 已检查失败并处理 ERROR_ALREADY_FIBER。 |
| 22 | 已限制每次排空至 4096，避免活锁；本次修复释放调度权后的 pop/reinsert 丢失竞态，改为不移除元素的就绪观察；新增可观测 overflow 计数和有界 flush。 |
| 23 | #116 支持 blocked；本次补齐父任务取消/超时向活动和排队节点传播，独立绑定取消上下文，不消耗父任务的消息；分别返回 cancelled / timeout。 |

其余工程描述也已有实现：HTTP 连接并发上限16、MCP/HTTP/WS回归和跨平台CI、WebSocket分片重组。路由仍采用按序前缀匹配，注册应先具体后通用，当前注册已遵循这一规则。

## 验证

- `cognitive-os-agent-test --audit`：配置键不碰撞、真实环境变量覆盖及类型解析、4线程共20000事件守恒（收到+明确丢弃=提交）、认证511/512字节边界、调度器和Fiber。
- `test_flow_dependencies_e2e.py`：真实HTTP+调度器+本地模型端点；先前依赖/失败/总结4个场景以及父任务取消场景。取消发生在真实模型请求等待期间，放行请求后所有节点状态为cancelled，后继没有创建文件。
- 完整单元、场景和跨平台CI用于覆盖上述“当前已具备”的能力；报告不将压力计数或本地模型fixture误称为生产LLM性能。

取消为协作取消：已在途的网络调用或外部命令需返回或超时后才能终止，运行期间的后续工具动作会被阻止。
