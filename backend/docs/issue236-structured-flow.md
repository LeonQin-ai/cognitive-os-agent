# Issue 236: structured execution control (work in progress)

This branch replaces natural-language keyword checks in runtime completion and
continuation decisions with explicit planner decisions and structured acceptance
conditions. It is a review checkpoint, not a completed migration for main.

The planner distinguishes actions, commentary, task declarations and completion.
Commentary can recover through a subsequent real action; consecutive decisions
without new observations have a bounded budget. Generic acceptance conditions
cover required actions, observations, artifact write/read receipts and complete
file reads. Accepted conditions accumulate and survive task resume.

Compatibility changes:

- Legacy model responses must use explicit completion decisions. Plain text is
  commentary, including when it sounds like a final answer.
- Required artifacts and reads come from structured acceptance supplied by the
  caller or model. The runtime no longer infers them from request wording.
  An under-declared contract cannot prove that every natural-language request
  was fulfilled.
- Tagged tool envelopes must be at the structural response root; free prose
  cannot authorize an embedded tool call.

Focused validation uses `test_structured_task_contract_e2e.py` and the C
`--progress-loop` suite. Existing protocol fixtures and the full CI matrix still
need migration and verification. No live maas-glm-5.1-zhipu endpoint was available
for validation in this environment. Offline mock prompt matching only selects
fixture responses; it does not control the production runtime state machine.

## Regression migration checkpoint

Legacy C suite: 4603 passed, zero failures. Structured contract suite: all 40
legacy/native scenarios passed. Both binaries compile. Fixtures now supply
explicit obligations and valid completion decisions; tool-count assertions are
preserved. Other Python regressions and the full CI matrix still require
verification before merging to main.
