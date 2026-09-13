# V3.1 架构升级追踪（Detailed Design v1.1）

> 基线：`docs/COGNITIVE_OS_DETAILED_DESIGN_V3.1.md`（DDD v1.1）
> 状态：核心三件套已落地（record / promotion gate / Context MMU），双平台全量验证通过

## 已完成

### 1. Memory Record V3.1（DDD §7.2/§7.3/§8.1）
- `memory/record.h` + `src/memory/record.c`
- 六类语义类型 `mem_type_t`（MEMR_* 前缀，避免与 service.h 四态枚举撞名）
- 七态生命周期 `mem_status_t`：PROVISIONAL → TRUSTED → VERIFIED；DEPRECATED / CONTRADICTED / ARCHIVED 为出口
- 记录属性齐全：title/scope/importance/confidence/utility/emotion(valence,arousal,salience)/source/origin_id/outcome/evidence/version/access
- **Markdown 为权威存储**：YAML frontmatter（受控子集：扁平 key:value + emotion:/source: 嵌套块）+ 正文；派生索引全部可从 Markdown 重建
- 派生向量镜像 `"r:<id>"`（幂等 rebuild，非出口态才镜像）

### 2. Promotion Gate（DDD §8.1/§8.3）
- `memory/promotion.h` + `src/memory/promotion.c`
- 确定性门禁（零 LLM 调用）：CANDIDATE → score → verify → PROMOTE | MERGE | REJECT
- 复合分 = 0.40*importance + 0.30*confidence + 0.20*outcome_weight + 0.10*salience
- **无证据的模型结论被封顶**（no_evidence_cap=0.50 < promote 阈值 0.55），落实 §8.3 "model-only assertion 最低置信"
- FAILURE 证据一等公民：procedural 类负向记忆权重 0.9（§8.2 负向经验保留）
- 生命周期成熟化：PROMOTE 入库即 PROVISIONAL；有证据 MERGE → TRUSTED；强证据类（SUCCESS/FAILURE）MERGE → VERIFIED
- 去重：按 origin_id 匹配活跃记录合并；无 origin 候选按确定性内容 id 合并；归档/矛盾记录不阻塞新晋升
- MERGE 升 version/confidence/utility，同级强证据以最新为准

### 3. Context MMU（DDD §9 + §7.4/§7.5/§7.7）
- `context/mmu.h` + `src/context/mmu.c`
- **L0/L1/L2 与 HOT/WARM/COLD 正交**（DDR-003）：L0/L1/L2 是表示粒度（摘要/概览/全文），HOT/WARM/COLD 是驻留态（L2 常驻/L1 常驻/仅元数据）
- L0/L1/L2 确定性派生 + content-hash 缓存（§7.4：内容不变不重摘要），UTF-8 安全截断
- 召回/晋升流（§9.4）：向量候选(L0) → §7.7 排序 → WARM+L1 → 重排 → TOP-K HOT+L2 → 组装上下文
- §7.7 多因子评分：relevance × importance × confidence × utility × recency × salience × scope（权重可配），逐条输出 factors 日志（可解释检索）
- 逐出（§9.5）：keep_score = relevance+importance+utility+access_freq+salience/2 − token_cost_penalty，最低分先逐出；**pinned 永不逐出**
- token 预算：chars/4 启发式估算，页只按当前驻留层级计费；`ctx_mmu_stats_json` 暴露 used/budget/generation

### 4. 测试与验证
- `tests/test_all.c` 新增 `test_record_v3` / `test_promotion_v3` / `test_mmu_v3`（+84 断言）
- Windows（zig cc）：unit **1475/0**、scenario **85/0**、E2E **PASS**、adapters **PASS**
- Linux（gcc，101.43.16.207）：unit **1450/0**（含全部 V3 分节）
- 真实 agent 功能抽查：GAIA mini 真跑 **17/17 (100%)**；SWE-bench docker 抽查 **django__django-17087 resolved=True**（f2p 1/1，p2p 17/20）+ **pallets__flask-5014 resolved=True**（f2p 1/1，p2p 20/20），与 v0.2.0 基线一致 — V3.1 改动未影响真实 agent 功能

## 与 DDD 的已知偏差（有意为之）

| DDD 条目 | 现状 | 理由 |
|---|---|---|
| §7.3 `memory/{user,agent,project,global}/` 目录分域 | 扁平 `records/*.md` + scope 字段 | 单进程个人版扁平存储足够；scope 已是 frontmatter 属性，目录分域可后置 |
| §7.2 `uri/title/source_uri/supersedes_uri` | id 即 uri；title 已加；supersedes 暂缺 | 冲突消解（supersedes）属于 CONTRADICTED 流程，待后续 |
| §7.3 JSONL journal 前置层 | 事件仍走 episodic 环 + 巩固管道 | episodic 已承担 append-oriented 层职责 |
| §9.6 Prefetch | 未实现 | 需任务 DAG 依赖暴露，后置 |
| §20.3 SQLite metadata 索引 | 向量镜像 + 内存页表 | 10^4 规模内无需 SQLite |

## 安全平面（SECRET_SECURITY_DDD_V1.0）核心切片已落地

### 5. Secret Security Plane（安全 DDD §5/§7/§8/§12/§13/§22）
- `include/security/secret.h` + `src/security/secret_detector.c` / `secret_boundary.c`
- **确定性检测管道**（§7.2，零 LLM 调用）：模式检测（PEM 私钥 / Bearer / Basic / JWT / AWS AKIA / sk-、ghp_、xox、AIza 固定前缀 / password=、api_key= 等 kv 赋值）→ 熵分类器（≥28 字节且 ≥4.5 bits/byte → MEDIUM）→ 置信分级 LOW/MEDIUM/HIGH
- **误报控制**（§7.3）：placeholder 词表（changeme/${...}/<...>/example/your- 等）+ 长度 + 熵下限；重叠匹配取最强
- **脱敏**（§8.1）：MEDIUM/HIGH → `[REDACTED:secret]`，LOW 放行
- **LLM 边界**（§13）：`llm_chat`/`llm_stream` 入口统一拦截——输入扫描（审计），输出整段脱敏，流式输出用 hold-back 窗口（保留 95 字节防跨 delta 泄漏，free 时冲刷）
- **内存写入防护**（§12）：`mem_records_put` 持久化前脱敏（Markdown 权威存储永不落明文密钥）
- **兼容模式**（§5）：`passthrough`（默认，只审计+脱敏不阻断）/ `strict`（HIGH 置信输入直接拒绝）；`security.mode` 配置持久化，运行时 `POST /v1/config/security` 切换
- **审计与观测**（§22）：检测命中逐条写 `<state_root>/security_audit.jsonl`；累计计数器暴露 `GET /v1/security/stats`，可挂 metrics 计数器
- 单测 `test_security`（+38 断言）：检测命中/误报/脱敏正确性/passthrough 不阻断/strict 阻断/流式跨 delta 泄漏防护/内存写入脱敏/stats 形状

### 已知偏差（安全平面）
| DDD 条目 | 现状 | 理由 |
|---|---|---|
| §27 完整目录（broker/injector/session/provider/lifecycle） | 检测+脱敏+边界+模式+审计 | Phase 0/1 先行（observe → tokenize）；broker/注入/会话代理随 managed 凭据功能后置 |
| 流式跨 delta 检测 | 95 字节 hold-back | JWT 最长 2048 字节超出窗口时可能漏检前缀；整段（非流式）路径无此限制 |
| §16 信封加密 / §24 全日志防护 | 审计 JSONL 为明文元数据（不含密钥内容） | 密钥本体从不入审计；加密存储随 Secret Broker 后置 |

## 后续（本任务范围外）
- 记忆门禁接入 memory facade 的 consolidation 管道（reflection 产出 → mem_candidate）
- Context MMU 接入 reasoning context 组装路径（替换 HOT/WARM/COLD 旧预算代码）
- §8.6 Experience → Skill → Plugin 的生成门（build+test+security check）
- Secret Broker / managed 凭据 / Secure Injection / Session Broker（安全 DDD §9/§10/§11）
- 检测边界扩展到 Tool args/results、MCP request/response、日志/trace 序列化点（安全 DDD §7.1 剩余清单）

| DDD 条目 | 现状 | 理由 |
|---|---|---|
| §7.3 `memory/{user,agent,project,global}/` 目录分域 | 扁平 `records/*.md` + scope 字段 | 单进程个人版扁平存储足够；scope 已是 frontmatter 属性，目录分域可后置 |
| §7.2 `uri/title/source_uri/supersedes_uri` | id 即 uri；title 已加；supersedes 暂缺 | 冲突消解（supersedes）属于 CONTRADICTED 流程，待后续 |
| §7.3 JSONL journal 前置层 | 事件仍走 episodic 环 + 巩固管道 | episodic 已承担 append-oriented 层职责 |
| §9.6 Prefetch | 未实现 | 需任务 DAG 依赖暴露，后置 |
| §20.3 SQLite metadata 索引 | 向量镜像 + 内存页表 | 10^4 规模内无需 SQLite |

## 后续（本任务范围外）
- 记忆门禁接入 memory facade 的 consolidation 管道（reflection 产出 → mem_candidate）
- Context MMU 接入 reasoning context 组装路径（替换 HOT/WARM/COLD 旧预算代码）
- §8.6 Experience → Skill → Plugin 的生成门（build+test+security check）
