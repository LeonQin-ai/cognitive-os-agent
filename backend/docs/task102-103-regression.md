# Task 102/103 regression audit

The latest #232 reports are two distinct outcomes. Task 103 shows legacy
protocol recovery latched, then repeated malformed calls/commentary and failure.
Task 102's exported trace says DONE and contains a long repeated Chinese plan.
The last planner Trace is truncated; it is not a complete final-answer record.
Do not infer a new runtime root cause from that discrepancy alone.

## Coverage gap corrected

The previous degeneration tests exercised the CLI and fixed responses. They did
not check the chat HTTP scheduler path shown by Task 102. The updated
`test_answer_repetition.py` reconstructs its Chinese sentence cycle (including
an interrupted/restarted cycle) and runs both legacy/native protocols through:

- CLI: degenerate output fails, short quotes and distinct analysis still pass.
- Actual HTTP Runtime: create a session, submit code analysis, execute a source
  read, return the repeated model output, poll the scheduler to FAILED, and
  require the failure explanation without delivering the degenerate answer.

All 18 checks passed locally. The HTTP portion uses a deterministic provider,
not an actual model. Existing Linux CI covers this suite; Windows and both macOS
lanes now run it too. No new runtime fix is claimed where current code passes.

## Real legacy integration check

The prior live harness cancelled on the first model failure, so it could not
validate Runtime recovery. `--accept-recovery` is now an explicit, bounded mode:
up to three recorded failures, the unchanged time/request bounds, and all actual
source/artifact/readback/final-answer requirements. Default zero-failure scoring
remains unchanged. Recovered success must never be reported as zero failure.

The actual configured DeepSeek legacy paged-source run passed in 59.882 seconds:
14 requests, 16 tools, **two protocol failures recovered**, 150,237 input + 9,390
output = **159,627 tokens**. Report and actual model-created HTML:
`build/live-legacy-paged-recovery-103.json/.html/.log`. No assistant-authored
deliverable or repeated paid rerun was used for this check.

This does not establish successful execution on the reporter's unavailable
`maas-glm-5.1-zhipu` route. #232 remains open pending matching integration evidence.
CI success proves the tested interfaces/fixtures; it does not prove all provider
responses or real tasks succeed. A different model's success is not a substitute.
