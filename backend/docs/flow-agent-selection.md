# Flow agent selection

Flow generation and automatic execution offer two roster modes:

- Automatic (default): the LLM selects only necessary roles from the registry.
  A single node is valid; registering ten roles does not require ten nodes.
- Manual: select one or more allowed registered roles. The planner receives
  only that roster. Unselected roles in a model plan reject the whole plan;
  a restricted task never falls back to an unrestricted single-agent run.

`POST /v1/flows/decompose` and `POST /v1/orchestrate` accept optional `agents`,
an array of registered names. Omit it for automatic selection. Empty, non-array
or unknown selections return 400 before a provider call. Selecting a role makes
it available; the model can still omit it if unnecessary.

Automatic task records retain the roster as an internal versioned task envelope,
so restart and resume use the same constraints. The UI displays the original task
and preserves that envelope when editing its text. Removing a selected role
before a later run makes the restricted plan fail instead of broadening its roster.
Explicit hand-edited DAG execution remains an independent user action.

Validation: `tests/test_flow_agent_selection.py` runs an actual private HTTP
Runtime and deterministic provider with ten registered roles. It checks one-role
plans, restricted provider context, atomic out-of-scope rejection, asynchronous
execution and resume, persisted selection, and invalid admission without model
calls. Added to all platform CI lanes. Local C11 CLI build and regression pass;
web script syntax is checked. No paid model call is claimed for these tests.
