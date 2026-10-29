# Physical HTML acceptance for #232

The new report shows DONE after directory listings and source reads only, with a
final work announcement and no HTML write. A fixed-response fixture reproduces
false DONE with the prior binary: two actual source reads followed by repeated
`complete_task(complete)` claims, without any artifact.

For explicit Chinese output-HTML requests, completion now requires physical
receipts from the current run: a successful built-in file_write of a .html file,
a successful later file_read of the same canonical target after its latest
write, and a currently existing nonempty file. This applies to native and legacy
completion, including the final fallback boundary. Model wording, a completion
declaration, unrelated mutations, or a preexisting file cannot satisfy it.

Canonical evidence paths are captured from actual tool arguments separately
from display-limited argument strings, so a large HTML body cannot destroy the
receipt by truncating its displayed JSON. Receipt ownership follows run steps.
A missing receipt rejects candidate completion, allows bounded continuation, and
returns a concrete artifact acceptance error if work remains undelivered.
Actual new observations reset the candidate-rejection count; budgets are not
expanded. This changes execution acceptance, not system prompt text.

Fixed-wire tests cover native/legacy false completion, preexisting files,
write-only output, obsolete readback followed by a newer write, and actual
write/readback success. The fixture never inspects prompt/recovery wording.
The old HTML177 fixture now really reads back its generated file; its earlier
write-only success is no longer valid acceptance. A stalled no-write control
now fails after three rejected declarations rather than consuming six rounds.

Scope: this verifies built-in file-tool receipts, not the semantic quality of
an analysis. Shell/plugin-only generation does not yet supply this receipt and
cannot pass this gate without an explicit built-in write/readback. The format
request is recognized from the original instruction, not from final-answer
phrases. The private GLM endpoint and Windows sources are not available here;
#232 stays open until actual deployment delivery is verified.

Local validation: 4,588 C unit checks passed. All 18 fixed-response cases pass
normally and under AddressSanitizer (`detect_leaks=0`), including native/legacy
HTML bodies exceeding the 64-KiB display-argument cap. Six recovery cases,
20 completion-review cases, the compatibility recovery suite, 12 Flow cases,
and native literal/budget-boundary deliveries pass. Existing CI executes the
expanded fixed-response suite on every platform and ASAN.
