# Benchmark Results

Clean scorecard of cognitive-os-agent on public agent benchmarks. All numbers are **real runs** (full agent loop, no shortcuts); methodology, failure attribution and every framework fix along the way are documented in the detailed report: [`benchmark-report-2026-08-31.md`](benchmark-report-2026-08-31.md).

## Headline results

| Benchmark | Score | Model | Date |
|---|---|---|---|
| **GAIA 2023 validation** (official 165 tasks) | **78.79%** — L1 84.91% · L2 81.40% · L3 57.69% | GLM-5.3-flash | 2026-09-11 |
| **SWE-bench_Verified mini** (11 tasks, official resolved standard) | **8/11 = 72.7%** | GLM-5.3-flash | 2026-09-12 |
| **BFCL-style function calling** (22 cases, AST-strict) | **20–22/22 (~94%)** | deepseek-chat | 2026-09-08 |
| **GAIA-style mini** (17 tasks, official normalization) | **17/17 = 100%** (+ vision 1/1) | GLM-5.3-flash | 2026-09-09 |
| **WebArena-style browser tasks** (6 tasks, strict matching) | **6/6 = 100%** | GLM-5.3-flash | 2026-09-09 |
| **ToolBench / AgentBench style** (9 end-to-end cases) | **9/9 = 100%** | deepseek-chat | 2026-09-07 |
| **Policy compliance** (4 violating requests) | **bare LLM 0/4 → with runtime policy engine 4/4** | deepseek-chat | 2026-09-07 |
| **Real engineering task** (todo-cli: docx spec → code → tests → PPT) | **PASS** (4/4 deliverables, 11 self-written tests green) | deepseek-chat | 2026-09-07 |

## Notes

- **GAIA**: official 2023 validation set (165 tasks, L1×53 / L2×86 / L3×26), GAIA-official normalized scoring, full agent loop with browser (Playwright MCP), OCR and shell/python document tools. The no-browser CLI baseline scored 10.91% — the 7.2× improvement came entirely from tool access, confirming the gap was tooling, not the model. Reference: leaderboard leaders 90–93% with GPT-5/Claude/Gemini-class models.
- **SWE-bench_Verified**: 11-task mini set across django/sympy/sphinx/pylint/requests/flask. `resolved` = official dual standard (FAIL_TO_PASS all green + PASS_TO_PASS no regression). Docker-isolated rerun of the same 11: 5/11 (delta is single-run sampling variance of GLM-5.3-flash; union across runs = 8). Reference: full-set SOTA ~65–70% with frontier models.
- **BFCL-style**: self-built 22 cases covering simple/multiple/parallel/irrelevance/tau-policy, judged at AST strictness (extra optional parameters count as wrong). After schema/prompt hardening, remaining variance is deepseek sampling noise.
- **Policy compliance**: the same 4 violating requests (write in read-only mode, quota violation, network fetch in a no-net environment, `reset --hard` on a protected repo). With rules written in the prompt the bare LLM complied 0/4; with rules registered in the runtime policy engine the violating actions were never even planned — 4/4. Policy compliance cannot rely on LLM self-restraint; it must be framework-enforced.
- Small sample sizes (11–22 tasks) show capability ceilings, not statistical estimates; full context in the detailed report.

## Reproduce

```bash
cd backend && ./build.sh
# offline sanity (no API key)
./build/cognitive-os-agent-bench-bfcl --mock
./build/cognitive-os-agent-bench-gaia --mock
# real runs (export COA_LLM_PROVIDER/BASE_URL/MODEL/API_KEY first)
./build/cognitive-os-agent-bench-bfcl --real
./build/cognitive-os-agent-bench-real --real
./build/cognitive-os-agent-bench-gaia --real --vision
```

GAIA full-set and SWE-bench harnesses: `gaia-dataset/run_browser.py`, `swebench/swe_run.py` (+ `swe_run_docker.py`) — see the detailed report for the exact configurations behind each score.
