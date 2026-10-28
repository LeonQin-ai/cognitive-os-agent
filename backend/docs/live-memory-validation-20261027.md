# Configured API memory validation — 2026-10-27

Provider model: `deepseek-v4.1-flash`. All calls went through freshly built
Cognitive OS Agent Runtime, using private temporary states and the configured
API credentials. Production tasks were untouched. Random fictional facts and
exact output checks prevent credit for guessing. No direct provider calls or
assistant-authored answers were used.

| Experiment | Measured result |
|---|---|
| Three seeds, three memory-off controls, three restarted memory-on recalls | 9/9 exact answers, 9 model requests, 45,258 input + 1,921 output tokens |
| Additional seed/control/recall, superseding update, restarted updated recall, unknown fact | 6/6 exact answers, 7 model requests, 36,307 input + 34,273 output tokens |
| Aggregate | 15/15 answers, 16 requests, 81,565 input + 36,194 output = 117,759 tokens |

Each write/query used a new session. The on/off comparison copied identical
persisted seed states before either branch ran. Memory-off returned UNKNOWN;
memory-on returned the exact random value in all four pairs. The update case
returned the new value after another restart. This is evidence for retained
episode use in these recall tasks, not general semantic retrieval quality or
structured fact extraction. It does not prove code-repair benefits.

## Efficiency failure retained

The unknown-fact case returned UNKNOWN correctly but took 77.968 seconds,
two requests, one recorded model failure, 10,859 input and 32,927 output tokens.
The initial report did not retain the failure diagnostic, so its cause and exact
failed-request token split are unverified. Do not label this zero failure cost.
Subsequent harness runs retain the task's `model_failure` metadata. The report
adds a separate efficiency check for these one-answer tasks: one model request,
no failure, at most 2,048 output tokens (a test criterion, not a Runtime output
limit). Under this criterion the measured run is 14/15, despite 15/15 answers.

Local raw reports: `build/live-memory-report.json` and
`build/live-memory-extended-report.json`. The test deliberately performs no
automatic rerun to obtain a better score. Private state and credential copies
were removed when each experiment ended.
