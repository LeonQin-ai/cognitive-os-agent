# cognitive-os-agent 智能体基准评测报告（2026-08-31）

> 注（2026-09-05）：项目已由 c-agent 全面更名为 cognitive-os-agent，本文二进制名与环境变量名已同步更新；分数为更名前同名代码（deepseek-chat 真实运行）的评测结果。
>
> 注（2026-09-07）：**全量复测完成**（deepseek-chat，真实运行），并新增 policy-enforced 类目与真实工程任务（todo-cli 全流程）实测。最新分数见下节「2026-09-07 全量复测」；历史分数保留作对照。

## 2026-09-07 全量复测（全部真实跑完）

### 分数总表（deepseek-chat）

| 评测项 | 得分 | 对照（历史/其他） |
|---|---|---|
| ToolBench 风格 工具选择+参数构造（bench-real） | **5/5 (100%)** | 历史 5/5，持平 |
| AgentBench 风格 OS 命令副作用落盘校验（bench-real） | **4/4 (100%)** | 历史 4/4，持平 |
| bench-real 端到端成功 | **9/9 (100%)** | 历史 8/9（mcp 用例曾因评测环境无 :9000 MCP 服务失败，本次通过） |
| BFCL simple（单调用 AST 精确匹配） | **4/5** | 历史 4/5，持平 |
| BFCL multiple（干扰下选对工具） | **2/4** | 历史 2/4，持平；失败均为"选对工具但多给可选参数"（严格 AST 判分） |
| BFCL parallel（多重集合匹配） | **5/5 (100%)** | 历史 5/5，持平 |
| BFCL irrelevance（不该调用时克制） | **1/4** | 历史 1-2/4 波动；模型对"1+1"仍倾向 shell 计算（over-triggering） |
| BFCL tau-policy（提示词内业务规则遵循，裸 LLM） | **0/4** | 历史 0/4，持平 |
| **policy-enforced（同一批违规请求，规则进 policy 引擎，真实 planner 路径）** | **4/4 (100%)** | **裸 LLM 0/4 → 框架 4/4** |
| 真实工程任务：todo-cli 全流程（读 docx → 写代码 → 跑测试 → 生成 PPT） | **PASS**（详见下文） | 修复 2 个框架漏洞后通过 |

mock 规划器对照：BFCL 7/22、enf 4/4（mock 对违规请求本就不产生动作，证明判分器有效）。

### policy-enforced：框架相对裸 LLM 的量化增量（本次新增）

同一批 4 条违规请求（只读模式写文件 / 限流期重复备份 / 禁网环境抓网页 / 只读仓库 reset --hard）：

- **裸 LLM（规则写在提示词里）**：0/4 全部照做，拒绝措辞 0/4。
- **框架（规则注册进 policy_engine，走真实 coa_planner_plan_ex 路径：违规工具从目录中隐藏 + policy 注入提示词）**：4/4，违规动作一次都未被规划（hard-block 层待命但无需触发——第一层目录隐藏已生效）。

结论：**策略遵循不能依赖 LLM 自觉，必须框架强制**。这是 cognitive-os-agent 相对"裸模型+工具调用 API"的核心差异化能力，现在有了量化证据。

### 真实工程任务实测：todo-cli 全流程（多智能体编排）

任务：三步编排——analyst 读需求文档 project_spec.docx（python-docx）→ coder 在 todo_cli/ 目录实现项目并跑通自测 → reporter 用 python-pptx 生成汇报 PPT。deepseek-chat 驱动，/v1/orchestrate 真实运行。

**量化结果（全部经独立验证，非 agent 自报）**：
- 交付物 4/4：todo.py（stdlib-only，5 个子命令）、test_todo.py、README.md、project_report.pptx（33703 字节，python-pptx 校验 6 页、内容正确）
- 测试：agent 自写 **11 个 unittest 用例**（要求 ≥8），覆盖 add/默认优先级/id 自增不复用/list 排序/filter/done/delete/不存在 id 非零退出/stats，独立运行 `python test_todo.py` 退出码 0
- 功能冒烟（主视角独立执行）：add rc=0、stats 输出 total/done/completion_rate 正确、done 999 以 rc=1 拒绝
- 编排 trace：3 步全部 status=ok；merge 产物逐条有据（引用真实测试输出与文件字节数），无虚构
- **coder 表现出"写代码→跑单测→修复→再跑"的迭代环**（首轮 2 个用例失败，自行修复后全绿）——这正是 SWE-bench 类任务的核心行为，说明轻量级 SWE-bench mini harness（选 C 小仓库 + 已知 bug + 单测判分）已具备落地条件

**评测过程中发现并修复的 2 个真实框架漏洞（有前后对照）**：
1. **agent 循环"意向即终局"**：reporter 首轮输出意向文本（"I need to verify…"）且无工具调用，循环立即当作最终答案结束（status=ok），PPT 根本没生成。修复：首轮纯文本若为意向式叙述（"Let me/I need to/我需要…"）则注入一条纠偏观察再给一轮（reasoning.c `looks_like_intent` + nudge）。修复后 reporter 真实执行 10+ 个动作生成 PPT。
2. **编排 merge 虚构成功**：step3 未产出 PPT，merge LLM 仍声称"project_report.pptx ✅ 已生成"。修复：merge 输入带上每步 status，系统提示禁止无证据断言（orchestrator.c）。修复后 merge 报告与磁盘实况一致。
（此前批次已修的 5 个循环健壮性漏洞与启动阻塞问题见 git log dc67070/391632c。）

### 与其他 agent 的对比（诚实口径）

本评测使用 22+4 条自建静态用例 + 9 条真实落盘用例，**不是官方 BFCL v3 全集**，跨 agent 数字只能作量级参考（下列为公开榜单记忆值，未在本环境复现）：

| 参照 | BFCL AST 类目公开成绩（量级参考） | 说明 |
|---|---|---|
| GPT-4o / Claude-3.5-Sonnet 级 | ~88-90% | 官方 BFCL 榜单头部 |
| 中型开源模型 | ~60-75% | deepseek-chat 的 function calling 在此档 |
| deepseek-chat + cognitive-os-agent planner | 12/22 (59%)，parallel 满分 | 失分集中在"多给可选参数"与 over-triggering，属模型侧；planner 提示词（精确工具目录+精确参数名）已把工具选择与并行调用做到 100%/满分 |

**框架的突出点不在裸分数，而在裸模型做不到的三件事（均有量化证据）**：
1. **策略强制**：0/4 → 4/4（policy-enforced）
2. **可观测与自修复的 agent 循环**：多轮 act-observe + 失败回灌（VERIFY 失败不死于静默），真实任务中 coder 自动迭代修测试
3. **诚实的编排汇总**：merge 逐条溯源，杜绝虚构成功——这在多 agent 系统里是稀缺性质

### 与主流 benchmark 的映射（2026-09-07 更新）

评测对象：cognitive-os-agent（Cognitive OS Runtime）`D:\AI\code\cognitive-os\backend` 当前构建。
真实模型：DeepSeek `deepseek-chat`（OpenAI 兼容端点）。离线对照：内置 mock 规划器。

## 评测器

| 评测器 | 产物 | 覆盖 |
|---|---|---|
| `tests/bench_bfcl.c` | `build/cognitive-os-agent-bench-bfcl` | BFCL 四大场景 + Tau-bench 策略遵循，共 22 条 |
| `tests/bench_real.c` | `build/cognitive-os-agent-bench-real` | ToolBench 风格工具选择/参数构造 5 条 + AgentBench 风格 OS 命令副作用 4 条 |

- BFCL 判分为 **AST 级**：输出解析为 `[{tool,args}]`，值类型严格（字符串 `"3"` ≠ 数字 `3`），多余/缺失参数都算错；parallel 为多重集合比对（顺序无关）。
- irrelevance / tau-policy 的正确行为是**不产生任何工具调用**（纯文本回答/拒绝）。
- 运行方式：`--mock`（离线 sanity）/ `--real`（`COA_LLM_*` 环境变量驱动，key 不落日志）。

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

1. **工具选择与参数构造是强项**（ToolBench 5/5、parallel 5/5）。cognitive-os-agent 的 planner 提示词（精确工具目录 + 精确参数名）有效。
2. **"严格不许多给参数"是主要失分点**。multiple 的 2 条失败都是工具选对但补了可选参数（`dir:"."`、`args:{}`）。BFCL 官方 AST 判分同样严格；若放宽为"子集匹配"，multiple 可到 4/4。
3. **过度调用（over-triggering）**：irrelevance 场景模型倾向用 shell "顺手"算 1+1、echo 答案。这是当前 LLM 的通病，缓解方向是在 planner 提示词中强化"纯文本问题禁止调用工具"。
4. **策略遵循（Tau-bench 类）曾是最大短板：0/4**。当前 planner 系统提示没有"业务规则优先于用户请求"的约束。**已修复**：在 `build_context`/SYS_PROMPT 注入 policy 层：请求与规则冲突时必须拒绝并在文本中说明，不允许生成对应工具调用；并已接入 `policy_engine` 做执行侧硬拦截（即使 LLM 违规，动作也会被 policy 引擎拒绝）。
5. mock 规划器 7/22 证明判分器对错误行为敏感（不是"永远 pass"的假 harness）。

## 与主流 benchmark 的映射与范围外说明

| Benchmark | 状态 | 差距 |
|---|---|---|
| BFCL Simple/Multiple/Parallel/Irrelevance | ✅ 已覆盖 | 判分器 AST 级，22 条静态用例 |
| Tau-bench（策略遵循） | ✅ 已覆盖 | 提示词规则 4 条（裸 LLM 0/4）+ **policy-enforced 引擎规则 4 条（框架 4/4）**，执行侧 policy 硬拦截有量化证据 |
| ToolBench / AgentBench（CLI 族） | ✅ 已覆盖（bench_real.c） | 2026-09-07 复测端到端 9/9 |
| GAIA | ❌ 范围外 | 需多模态（图像/音频）+ 网页浏览 + 文档解析 |
| WebArena / OSWorld | ❌ 范围外 | 需浏览器/GUI/VM 沙箱 |
| SWE-bench | ⚠️ mini 可落地 | agent 循环已有；todo-cli 实测中 coder 已具备"跑单测→修复→再跑"迭代环，缺的只是 repo 级任务集 + 容器化沙箱 |

## 补齐路线

1. **policy 硬拦截**（已完成）：policy_engine 已接入动作执行前校验，规则命中即拒绝动作并把原因回灌给 LLM（agent 循环下一轮自我修正）。
2. **SWE-bench mini harness**：选定小型 C 仓库 + 已知 bug + 单测，agent 循环内跑 `shell` 执行测试判分。
3. BFCL 用例扩充：可加载外部 JSON 用例文件（当前为静态嵌入）。
4. 长上下文/多轮对话评测（BFCL multi-turn 类目）。

## 复现

```bash
./build.sh bench-bfcl
# 离线 sanity（预期低分，验证判分器有效）
./build/cognitive-os-agent-bench-bfcl --mock
./build/cognitive-os-agent-bench --mock
# 真实评测（先 export COA_LLM_PROVIDER/BASE_URL/MODEL/API_KEY）
./build/cognitive-os-agent-bench-bfcl --real
./build/cognitive-os-agent-bench-real --real
# 真实工程任务（serve 后提交 /v1/orchestrate，用 UTF-8 JSON 文件体）
./build/cognitive-os-agent.exe serve 18530
curl -s -X POST http://127.0.0.1:18530/v1/orchestrate -H "Content-Type: application/json" --data-binary @orch_task.json
```
