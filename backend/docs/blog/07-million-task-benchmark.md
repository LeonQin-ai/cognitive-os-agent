# 我测了百万任务，但没有测百万次模型推理：Cognitive OS 的 Runtime 实测

> “百万任务”只有与工作负载一起解释才有意义。这次我测的是实际 Scheduler、AgentPool 和本地确定性任务的执行路径，目的是观察 Runtime 的规模行为，而不是宣称远程模型具有同样的吞吐。

这是 [Cognitive OS](https://github.com/LeonQin-ai/cognitive-os-agent) 技术系列第 7 篇。数据来自仓库保存的扩展基准，以下按原报告口径解读。

## 测试到底运行了什么

机器为 Windows 11 build 26200、Intel Family 6 Model 142，约 8 GiB 内存，使用 Zig cc 0.14.1、`-O1` 构建。规模负载使用 8 个工作线程，每任务执行 64 次 Context 探测，聚合上下文缓冲为 8 MiB；原始 JSON 还记录了 8 个空闲的辅助 Runtime Worker，不能遗漏这部分进程开销。

Agent 数量指注册的角色数。1000 个角色不等于 1000 个进程，也不等于同时向模型发起 1000 次请求。任务主体使用本地确定性工作负载，不包含生产模型推理与网络延迟。

每个规模组合执行 3 次独立运行；耗时、吞吐和各次运行的延迟分位数按报告取中位值，峰值 RSS 使用组内最大值。

## 把数字与范围一起展示

| 注册角色 | 完成任务 | 耗时中位值 | 吞吐 | 排队 P99 | 峰值 RSS |
| --- | --- | --- | --- | --- | --- |
| 100 | 100,000 | 5748.495 ms | 17,396 task/s | 0.422 ms | 22.48 MiB |
| 1000 | 100,000 | 5771.132 ms | 17,328 task/s | 0.376 ms | 22.57 MiB |
| 100 | 1,000,000 | 65971.943 ms | 15,158 task/s | 2.039 ms | 78.65 MiB |
| 1000 | 1,000,000 | 72279.054 ms | 13,835 task/s | 14.790 ms | 77.92 MiB |

百万任务扩展包含 100/1000 角色各 3 轮，共实际完成 600 万项任务。报告记录所有提交任务被接纳并完成，失败与拒绝为零，结束时活动数和队列数为零；完成历史保留上限为 64 条。

RSS 还包含基准自身保留的验证数组，不能把它全部归因于调度器，也不能根据长任务测试的 RSS 增加就直接判断历史泄漏。

## 平均吞吐不能盖住排队突增

1000 角色下，百万任务的吞吐约比已有十万任务基准下降 20%，排队 P99 上升到 14.790 ms，采样队列峰值达到 6201。这个结果值得展示，因为它明确告诉我们：长时间运行时，平均值之外还存在突发排队。

这组数据不能证明线性扩展，也没有建立生产延迟 SLO。若只保留最好的吞吐数字，读者反而无法判断系统在较长负载下的行为。

“累计完成百万任务”也不等于“百万任务同时积压”。项目另外设置 backlog gate，在十万任务提交完成前阻塞 Worker，观察到 99,992 条真实排队任务。这是独立负载，不能与普通吞吐结果混为一个场景。

## 如何复现，而不是只看表格

先按照仓库说明构建 `runtime-bench`：Windows 使用 `build.ps1 -Target runtime-bench`，POSIX 可使用 `make build/bench-runtime`。进入 backend 后：

```bash
python tests/test_runtime_benchmark.py
python tests/run_runtime_measurements.py --million \
  --compiler "实际编译器/版本/参数" --output docs
```

第一条是小规模正确性 smoke，覆盖任务完成、MMU、积压与回滚。第二条重新运行百万任务组合并保存数据；它需要实际承担测试负载，结果会随机器、编译参数和系统负载变化。

本篇引用的是已保存的历史测量，不代表每次 CI 都重新执行百万任务，也不代表后续每个版本都已经重新建立同样的性能基线。

## 这些测试对项目有什么帮助

它们把“Agent 很多时会不会出问题”拆成可测的问题：注册表查找是否退化？排队延迟如何变化？历史保留是否有界？结束时任务数量是否守恒？积压放行之后能否排空？

这是一组系统工程证据。对于实际业务 Agent，还要继续测模型延迟、限流、工具 I/O、外部失败和具体任务完成率。

数据：[完整报告](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/docs/runtime-benchmark-expanded.md)、[百万任务原始 JSON](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/docs/runtime-million-2026-10-14.json)。源码：[基准程序](https://github.com/LeonQin-ai/cognitive-os-agent/blob/main/backend/tests/bench_runtime.c)。

上一篇：[可靠性与恢复](06-reliability-and-recovery.md)；下一篇：[三个上手实验](08-getting-started-and-contributing.md)。欢迎提交带机器、编译参数和原始结果的复测，让不同平台的数据能在相同口径下讨论。
