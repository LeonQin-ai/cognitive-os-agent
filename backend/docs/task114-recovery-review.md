# Task 114: recover output truncation without weakening delivery checks

## Observed failure

The supplied task 114 trace contains 35 model requests and 69 tool actions in
546,479 ms. The last request returned `finish_reason=length` with private
reasoning but no visible answer or executable tool arguments. The configured
million-round ceiling was not reached. The visual-error branch terminated the
run before the ordinary truncated-response recovery branch could run.

All five recorded file writes targeted helper scripts under `.tool-results`;
the trace does not show a modification to the requested application source.
Three attempted reads targeted helper output files that did not exist, and one
Python console read failed on GBK output encoding. Recovering the last model
response does not by itself prove the original UI task has been delivered.

## Runtime changes

- HTTP-success response-stage truncation shares the existing two-regeneration
  allowance across visual and text requests. Incomplete calls execute no tools.
  Network errors, HTTP failures, and non-truncated invalid visual replies do not
  enter that retry branch. A final truncated response stays failed.
- Visual truncation diagnostics identify the output-token limit rather than
  suggesting that a 120-second HTTP wait expired.
- Artifact baselines reset at each run, including reused session lanes. Full
  reads do not consume the separate 64-entry artifact baseline table.
- Artifact production/readback receipts must match the current file version
  and their actual execution order. An earlier file_write cannot vouch for a
  later version; declared shell production followed by readback remains valid.
- Shell/Git cache exemptions are exact conservative forms, rather than whole
  subcommand prefixes. Mutating branch/tag commands, arbitrary env programs,
  options such as git log --output, newline chaining, and declared outputs
  invalidate unversioned searches. Versioned reads retain their existing
  version checks.

The configured default/million-round budget and structural completion protocol
are unchanged. New helper files are not automatically classified as successful
delivery of an undeclared user objective; semantic intent cannot be established
from a successful file_write receipt alone.

## Regression evidence

The new same-process/two-task test fails against the original local binary:
the second task only cats and reads the first task's artifact, yet returns DONE.
The original binary also reuses an empty glob result after creating a Git branch.
Both tests pass against the isolated fixed build.

Automated coverage includes text/visual reasoning-only length recovery, repeated
length exhaustion, invalid visual responses without length, actual file_write
then file_read after visual recovery, silent/trickling provider timeouts, reused
session baselines, 64 full-read obligations plus artifact acceptance, current
version/producer ordering, valid script rewrites, and Git search invalidation.
The structured contract suite is included in platform CI and ASAN jobs.

## Real configured API run

An isolated native DeepSeek v4.1 Flash run uploaded a synthetic PNG, read two
synthetic source files with random proof markers, wrote comparison.html, and
read the HTML back. The test independently checked both random markers, the
source facts, final answer, and write/readback order. The assistant did not
generate the delivered HTML.

- Result: DONE; 10 model requests, 6 tools, 0 model failures; 29.971 seconds.
- Delivered artifact: 6,804 bytes.
- Actual usage: 80,115 prompt + 5,652 visible completion + 984 reasoning =
  **86,751 tokens**. No failed requests occurred in this live run. Monetary
  cost is not computed because no verified route pricing is configured.
- Local evidence: build/task114-live-visual.json, .log, and .html (ignored).

This validates the configured provider's visual tool/artifact path, not an
externally forced provider truncation. Fixed-response HTTP tests exercise the
specific truncation and exhaustion branches deterministically. It does not
claim that arbitrary long UI modification tasks or all providers are proven.
