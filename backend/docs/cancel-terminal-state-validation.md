# Cancellation terminal status publication

The macOS gate observed an HTTP-cancelled task with cancellation output and
stage, but `status: FAILED`. The ordinary task runner published FAILED when the
interrupted provider returned a nonzero result. The scheduler then overrode that
with CANCELLED after the runner (including trace finalization) returned. A poll
between those writes saw FAILED as the first terminal outcome and stopped.

The runtime runner now records `execution_failed` without publishing a terminal
status. The scheduler commits the terminal status after execution returns, using
its existing timeout/cancellation precedence before the execution failure. Normal
failure still becomes FAILED; normal success still becomes DONE. Legacy embedded
runners that explicitly assign status retain compatibility. No provider retries,
longer cancellation budget, or polling grace period hides the intermediate state.

A deterministic scheduler test pauses a runner after it records its execution
result and before return. Both successful and failed results remain RUNNING while
paused; cancellation accepted in that window becomes CANCELLED as the first
terminal status. Uncancelled controls become DONE and FAILED respectively.

The actual HTTP suite repeats all five silent headers/body, regular/streaming,
and orchestrator cases three times (15 cancellations). It checks the first
terminal result, cancellation output, stable subsequent state, exactly one model
request per cancellation, and a three-second bound including the DELETE request.
The agent is registered once before the repeated cases.

Local validation is performed on Linux; Windows/macOS behavior requires the CI
matrix. The reported macOS failure is a real status publication race, not a
reason to weaken the expected CANCELLED assertion.

Local results: 4543 unit checks, all 15 HTTP cancellation cases normally and
under AddressSanitizer (`detect_leaks=0`), all 12 Flow scenarios, and all 20
completion-review scenarios passed.
