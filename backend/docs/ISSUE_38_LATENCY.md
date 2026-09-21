# Issue 38: response latency investigation

## Confirmed from the request path

- `src/cognition/reasoning.c:h_reason` prepares context with `build_context`, then calls `planner_plan_ex` synchronously. Previously both appeared as "planning". They now publish distinct stages.
- `src/cognition/planner.c:plan_with` calls `llm_chat`, which waits for a complete reply. The OpenAI adapter in `src/llm/openai.c:openai_chat` uses `http_post`, not its streaming implementation. No partial answer is shown during this call.
- Planner repair can make additional model calls. The `llm_ms` counter surrounds the entire planner invocation; `llm_calls` counts planner invocations, not every underlying HTTP request. Neither counter updates while that invocation is still in flight.
- Tool rounds can return to the model repeatedly. Fast individual tools therefore do not imply a fast overall task.
- The chat card timer is elapsed time since task start, not the current model request duration. It now explicitly says total elapsed time. The planning label also includes the round number.

## What remains unproven

The reported 8-second greeting and 837-second tool task have no captured request timings attached to the issue. Static inspection cannot distinguish provider generation time, network wait, prompt size, repair requests, or repeated rounds for those executions. This change improves status accuracy; it does not claim to reduce model latency or solve repeated tool failures (#40).

To attribute a reproduction, capture task progress snapshots with timestamps and correlate the context-preparation, planning and execution transitions with provider request start/end times. Use a fresh session for the greeting and compare its planner duration with a direct request to the same model. Do not include credentials or private prompts in reports.

## Validation

All inline JavaScript parses. Six label cases cover context preparation, model wait, round display, legacy snapshots without a round, tool execution, failure and unknown stages. The changed C translation unit compiles with Zig C on Windows. Full backend and live-provider latency tests were not run.
