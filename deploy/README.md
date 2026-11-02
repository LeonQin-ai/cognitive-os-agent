# Docker deployment and workspace isolation

From the repository root, run `docker compose up --build -d`, then open
http://127.0.0.1:18300. Configure the provider in the Models settings.
The initial mock provider permits a smoke test without spending API tokens.
State and workspace use separate persistent named volumes. `docker compose down`
preserves them; **`down -v` deletes them**. Back up the state volume privately:
it includes credentials, conversations and memory. Do not put credentials in the image.

The provided Compose configuration runs UID/GID 10001, drops all capabilities,
disallows privilege escalation, uses a read-only root filesystem, and bounds
memory, CPU, processes and temporary storage. Only state, workspace and temporary
storage are writable. The host port is bound to loopback. Do not expose it publicly
without configuring API authentication and an authenticated TLS reverse proxy.

To use an existing project, replace the workspace volume with an explicit bind
mount, for example `./my-project:/workspace`. Give UID/GID 10001 access on Linux;
avoid making it world-writable. Mount only the intended project. Windows Docker
Desktop can bind a selected host directory through its file-sharing settings.
The directory picker inside the container sees container paths, not the host filesystem.

This isolates the entire Runtime from unmounted host files. Tasks inside one
container share its workspace, state and network: this is **not per-task isolation**.
Use separate Compose projects/volumes for mutually untrusted users or tasks.
Never mount the Docker socket or run privileged. Network access is enabled for
LLM requests; strict network sandboxing requires an external egress policy.
The minimal image includes Python, Git and curl; Node/MCP/browser dependencies
must be installed in a derived image if those tools are required.

CI runs `python3 backend/tests/test_container_smoke.py` against a real Docker
engine, checking startup, non-root identity, root write denial, persistent volumes
and restart. This Windows development host has no Docker engine, so container
acceptance is determined by that CI job, not a local mock.
