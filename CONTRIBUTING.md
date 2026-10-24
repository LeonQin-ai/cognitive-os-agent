# Contributing to Cognitive OS

Start with one reproducible behavior: a task state, tool result, dependency edge, context-budget decision or recovery boundary. A small change with clear evidence is easier to review than a broad rewrite.

## Report a problem

Use the [issue templates](https://github.com/LeonQin-ai/cognitive-os-agent/issues/new/choose). Include the release or commit, operating system, provider protocol and model, minimal redacted input, expected behavior and actual result. For a generated file, distinguish task status from whether the artifact exists and is usable. Never attach API keys, private workspace files or unredacted configuration.

## Choose a contribution

| Area | A useful small contribution | Starting point |
| --- | --- | --- |
| Documentation | Reproduce one setup step on your OS and fix a concrete mismatch | [First task](GETTING_STARTED.md), [developer docs](backend/README.md) |
| Tools and providers | Add one protocol edge case with a deterministic fixture | `backend/tests/`, `backend/src/action/` |
| Scheduler and lifecycle | Demonstrate a cancellation, ownership or terminal-state boundary | `backend/src/runtime/` |
| Memory and Context | Check one promotion, eviction or reload invariant | `backend/src/context/`, `backend/src/memory/` |
| Multi-agent flows | Show that a downstream task waits or becomes blocked correctly | Flow dependency tests in `backend/tests/` |
| Console | Fix a reproducible interaction or mobile layout issue | `backend/apps/web/` |

These are contribution areas, not a list of confirmed open bugs. Check existing issues and keep the proposed scope specific.

## Build and validate

Follow the platform instructions in [README](README.md#quick-start). Linux/macOS builds use `make` in `backend`; Windows supports `build.ps1 all`. Run the targeted tests for the changed behavior. For C runtime changes, also run the relevant unit/scenario tests; CI covers platform builds and AddressSanitizer. Documentation-only changes need link and example checks rather than model calls.

Use deterministic providers for control-flow tests. If a real-model test is needed, state its provider/model, task scope and limitations. Do not turn a small fixture or benchmark subset into a general capability claim.

## Submit a pull request

Explain the concrete trigger, the old behavior, the new behavior and how you verified it. Include an adjacent failure case when changing completion, recovery, policy or parsing. Keep runtime changes separate from unrelated formatting. If configuration or an API changes, update the corresponding documentation in the same PR.

Contributions are made under the repository's [Apache-2.0 license](LICENSE). Vendored dependencies retain their own licenses. There is no promised review SLA; small, reproducible contributions are welcome.

## 中文说明

欢迎从一个可复现的小问题开始：补齐某个平台的安装步骤、增加协议边界 fixture、验证取消与回收、检查上下文驱逐，或修复控制台交互。

Issue 请提供版本、环境、脱敏输入、预期与实际结果；PR 请说明问题触发方式、最终行为与验证方法。涉及执行完成、恢复、权限或解析时，补充一个相邻反例。真实模型测试与 mock 控制流测试分别说明，避免把特定样本结果推广成通用承诺。
