# c-agent 智能体基准评测报告（2026-08-31）

评测对象：c-agent（Cognitive OS Runtime）`D:\AI\code\cognitive-os\backend` 当前构建。
真实模型：DeepSeek `deepseek-chat`（OpenAI 兼容端点）。离线对照：内置 mock 规划器。

## 评测器

| 评测器 | 产物 | 覆盖 |
|---|---|---|
| `tests/bench_bfcl.c` | `build/cagent-bench-bfcl` | BFCL 四大场景 + Tau-bench 策略遵循，共 22 条 |
| `tests/bench_real.c` | `build/cagent-bench-real` | ToolBench 风格工具选择/参数构造 5 条 + AgentBench 风格 OS 命令副作用 4 条 |

- BFCL 判分为 **AST 级**：输出解析为 `[{tool,args}]`，值类型严格（字符串 `"3"` ≠ 数字 `3`），多余/缺失参数都算错；parallel 为多重集合比对（顺序无关）。
- irrelevance / tau-policy 的正确行为是**不产生任何工具调用**（纯文本回答/拒绝）。
- 运行方式：`--mock`（离线 sanity）/ `--real`（`CA_LLM_*` 环境变量驱动，key 不落日志）。

## 分数

### BFCL 风格（22 条，deepseek-chat）

| 场景 | 得分 | 说明 |
|---|---|---|
| simple（单调用精确匹配） | **4/5** | 唯一失败也是"选对工具、多给了可选参数"级别的问题 |
| multiple（干扰下选对工具） | **2/4** | 2 条失败均为**工具选对了但多给参数**（git 多给 `dir:"."`、mcp 多给空 `args:{}`）——严格 AST 判分下不通过 |
| parallel（并行调用，顺序无关） | **5/5** | 满分；模型能正确一次发出多个独立调用 |
| irrelevance（不该调工具） | **2/4** | 模型对"解释递归"“1+1"也去调 shell——**过度调用**倾向 |
| tau-policy（业务规则遵循） | **0/4** | 四条违规请求全部照做，拒绝措辞 0/4——**无策略遵循能力** |
| **总计** | **13/22 (59%)** | |

对照 mock 规划器：7/22（harness 正确识别错误行为，判分有效）。

### ToolBench / AgentBench 风格（9 条，deepseek-chat）

| 族 | 得分 |
|---|---|
| ToolBench 工具选择+参数构造 | **5/5** |
| AgentBench OS 命令副作用（真实落盘校验） | **4/4** |
| 端到端成功 | **8/9**（唯一失败：mcp 端到端，因评测环境没有真实的 :9000 MCP 服务） |

## 关键发现

1. **工具选择与参数构造是强项**（ToolBench 5/5、parallel 5/5）。c-agent 的 planner 提示词（精确工具目录 + 精确参数名）有效。
2. **"严格不许多给参数"是主要失分点**。multiple 的 2 条失败都是工具选对但补了可选参数（`dir:"."`、`args:{}`）。BFCL 官方 AST 判分同样严格；若放宽为"子集匹配"，multiple 可到 4/4。
3. **过度调用（over-triggering）**：irrelevance 场景模型倾向用 shell "顺手"算 1+1、echo 答案。这是当前 LLM 的通病，缓解方向是在 planner 提示词中强化"纯文本问题禁止调用工具"。
4. **策略遵循（Tau-bench 类）是最大短板：0/4**。当前 planner 系统提示没有"业务规则优先于用户请求"的约束。**修复建议（已排期）**：在 `build_context`/SYS_PROMPT 注入 policy 层：请求与规则冲突时必须拒绝并在文本中说明，不允许生成对应工具调用；进一步可接 `policy_engine` 做执行侧硬拦截（即使 LLM 违规，动作也被 policy 引擎拒绝）。
5. mock 规划器 7/22 证明判分器对错误行为敏感（不是"永远 pass"的假 harness）。

## 与主流 benchmark 的映射与范围外说明

| Benchmark | 状态 | 差距 |
|---|---|---|
| BFCL Simple/Multiple/Parallel/Irrelevance | ✅ 已覆盖 | 判分器 AST 级，22 条静态用例 |
| Tau-bench（策略遵循） | ✅ 已覆盖 | 4 条规则冲突用例；**执行侧 policy 硬拦截待做** |
| ToolBench / AgentBench（CLI 族） | ✅ 已覆盖（bench_real.c） | — |
| GAIA | ❌ 范围外 | 需多模态（图像/音频）+ 网页浏览 + 文档解析 |
| WebArena / OSWorld | ❌ 范围外 | 需浏览器/GUI/VM 沙箱 |
| SWE-bench | ❌ 范围外（暂） | agent 循环已有（多轮 act-observe），缺"跑单测判分"的 sandbox 化 harness |

## 补齐路线

1. **policy 硬拦截**（高优先）：policy_engine 接入动作执行前校验，规则命中即拒绝动作并把原因回灌给 LLM（agent 循环下一轮自我修正）。
2. **SWE-bench mini harness**：选定小型 C 仓库 + 已知 bug + 单测，agent 循环内跑 `shell` 执行测试判分。
3. BFCL 用例扩充：可加载外部 JSON 用例文件（当前为静态嵌入）。
4. 长上下文/多轮对话评测（BFCL multi-turn 类目）。

## 复现

```bash
./build.sh bench-bfcl
# 离线 sanity
./build/cagent-bench-bfcl --mock
# 真实评测（先 export CA_LLM_PROVIDER/BASE_URL/MODEL/API_KEY）
./build/cagent-bench-bfcl --real
./build/cagent-bench-real --real
```
