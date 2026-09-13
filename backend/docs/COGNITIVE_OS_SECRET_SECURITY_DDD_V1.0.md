# Cognitive OS Secret Security Plane Detailed Design Document v1.0

> Status: Architecture Baseline / Implementation Guide  
> Scope: Personal Edition + Enterprise Edition common security plane  
> Primary language: C11  
> Optional implementation: Rust/WASM for selected untrusted protocol/sandbox components

---

## 1. Overview

### 1.1 Background

Cognitive OS agents can access terminals, filesystems, APIs, SSH, browsers, MCP servers and remote execution environments. A normal Agent workflow therefore inevitably encounters credentials and other sensitive information:

- usernames and passwords;
- API keys;
- OAuth access/refresh tokens;
- SSH private keys;
- TLS client certificates;
- session cookies;
- cloud credentials;
- database connection secrets;
- enterprise service-account credentials.

The security boundary must not depend on the LLM behaving correctly. A credential accidentally entering model context can also enter prompt logs, traces, memory, RAG indexes, embeddings or external model providers.

The design therefore introduces a dedicated **Secret Security Plane**.

### 1.2 Core objective

The security invariant is:

```text
Secret material must not enter LLM context.
```

A second invariant is:

```text
Secret material should be plaintext only inside the shortest possible
trusted execution boundary.
```

A third invariant is:

```text
Adding secret protection must not break existing login workflows.
```

### 1.3 Design goals

P0:

- detect likely secrets at input/egress boundaries;
- redact or tokenize secrets before LLM, Memory, RAG, Logs and Traces;
- store managed secrets encrypted at rest;
- expose only opaque `credential_ref` / `session_ref` handles to Agent/LLM;
- inject secrets directly into the target Tool/Executor boundary;
- support existing plaintext login flows in compatibility mode;
- provide capability and policy checks before secret use;
- zeroize temporary plaintext buffers where practical;
- audit access without recording secret values.

P1:

- enterprise Vault/KMS/HSM adapters;
- short-lived credentials;
- secret rotation;
- session reuse;
- browser/login integration;
- remote-node secret execution;
- secret-use anomaly detection.

P2:

- hardware-backed key isolation;
- attestation for trusted execution environments;
- dynamic credential issuance instead of long-lived static passwords;
- cross-node delegated secret broker.

### 1.4 Non-goals

This component is not:

- a replacement for enterprise identity management;
- a general password manager UI;
- a new cryptographic library;
- a guarantee that a compromised host kernel cannot read process memory;
- a reason to send plaintext credentials to an Agent process.

### 1.5 Initial sizing targets

These are engineering targets for benchmarking, not measured production claims.

| Metric | Personal | Enterprise target |
|---|---:|---:|
| Secret metadata records | 10^4 | 10^6+ |
| Secret lookup p95 | < 5 ms | < 10 ms |
| Policy decision p95 | < 1 ms | < 3 ms |
| Redaction throughput | > 50 MB/s | > 200 MB/s/node |
| Broker request rate | 100/s | 2,000/s/node |
| Secret plaintext residency | < 1 s preferred | < 1 s preferred |
| Audit event latency | < 50 ms | < 100 ms |

The critical performance principle is that detection, policy and reference resolution must be cheaper than an LLM call and must never invoke the LLM merely to detect a normal password/token pattern.

---

# 2. Architecture

## 2.1 System-level architecture

```text
                              USER / EVENT
                                   |
                                   v
                         +---------------------+
                         |   Input Gateway     |
                         +----------+----------+
                                    |
                                    v
                         +---------------------+
                         |  Secret Interceptor  |
                         +----------+----------+
                                    |
                      +-------------+-------------+
                      |                           |
                normal data                 secret material
                      |                           |
                      v                           v
                  Agent/LLM                Secret Broker
                                                  |
                            +---------------------+--------------------+
                            |                     |                    |
                            v                     v                    v
                       Secret Store          Policy Engine         Audit
                            |
                     encrypted at rest

Agent / LLM flow:

User -> Interceptor -> Redacted/Tokenized Context -> Agent -> LLM
                                                   |
                                                   v
                                                Tool Call
                                                   |
                                             Capability Check
                                                   |
                                             Secret Broker
                                                   |
                                            Secure Injection
                                                   |
                                                   v
                                         Local / Sandbox / VM / Remote
```

## 2.2 Secret Security Plane

```text
security/
├── interceptor
│   ├── ingress
│   └── egress
├── detector
│   ├── pattern
│   ├── entropy
│   └── classifier
├── redactor
├── broker
├── injector
├── session
├── policy
├── lifecycle
├── audit
└── provider
    ├── local
    ├── vault
    └── kms
```

### Component responsibilities

| Component | Responsibility |
|---|---|
| `SecretInterceptor` | Intercept data before it crosses a trust boundary |
| `SecretDetector` | Identify likely sensitive fields/material |
| `SecretRedactor` | Replace plaintext with redaction or opaque references |
| `SecretBroker` | Authorize, resolve and mediate secret use |
| `SecretProvider` | Retrieve encrypted secret material |
| `SecretInjector` | Deliver secret directly to the execution target |
| `SessionBroker` | Maintain opaque authenticated session handles |
| `SecretPolicy` | Decide whether use is allowed |
| `SecretLifecycle` | Expiration, rotation, revocation, zeroization |
| `SecretAudit` | Record metadata-only access events |

---

# 3. Trust Boundaries

## 3.1 Trust domains

```text
T0: Untrusted external input
T1: Cognitive Runtime / Agent
T2: LLM provider
T3: Secret Broker
T4: Trusted execution target
T5: Secret Provider / KMS
```

Secrets must never flow:

```text
T0 -> T2
T1 -> T2
T5 -> T1
T5 -> logs
T5 -> vector index
```

Preferred flow:

```text
T0 -> Interceptor -> T3
T3 -> T4
T3 -> metadata-only T1
```

## 3.2 Threat model

Primary threats:

1. accidental inclusion of secrets in prompts;
2. secret leakage through tool results;
3. secret leakage through logs/traces;
4. secret leakage through long-term Memory or RAG indexes;
5. generated-plugin exfiltration;
6. plaintext command-line arguments visible through process inspection;
7. unauthorized Agent access to another Agent's credentials;
8. replay of stale session handles;
9. credential exposure during remote execution;
10. compatibility regressions that cause operators to bypass the protection.

---

# 4. Credential Model

## 4.1 Secret types

```c
typedef enum {
    SECRET_USERNAME = 1,
    SECRET_PASSWORD,
    SECRET_API_KEY,
    SECRET_ACCESS_TOKEN,
    SECRET_REFRESH_TOKEN,
    SECRET_SSH_PRIVATE_KEY,
    SECRET_TLS_CERTIFICATE,
    SECRET_COOKIE,
    SECRET_DATABASE_CREDENTIAL,
    SECRET_GENERIC
} secret_type_t;
```

## 4.2 Secret reference

The Agent receives only a reference:

```c
typedef struct {
    uint64_t secret_id;
    uint32_t version;
    secret_type_t type;

    char reference[256];
    char owner_scope[128];

    uint64_t created_at;
    uint64_t expires_at;

    uint32_t policy_id;
    uint32_t flags;
} secret_ref_t;
```

Example:

```text
cred://project/cognitive-os/test-server/login
```

The reference must not contain the secret itself.

## 4.3 Secret payload

Plaintext secret material must never be stored in `secret_ref_t` or ordinary Agent state.

```c
typedef struct {
    uint8_t *data;
    size_t len;

    uint64_t expires_at;
    uint32_t flags;
} secret_buffer_t;
```

`secret_buffer_t` is an ephemeral object owned by the Secret Broker/Injector path only.

---

# 5. Compatibility Modes

Backward compatibility is a hard requirement.

## 5.1 `passthrough`

For existing integrations during migration:

```text
Agent -> login_tool -> legacy credential -> target
```

The interceptor may audit/redact copied data, but does not block an existing valid login path unless a higher-level policy explicitly denies it.

Use cases:

- migration;
- personal developer environments;
- legacy tools not yet Secret Broker aware.

## 5.2 `managed`

Preferred default for new integrations:

```text
Agent -> credential_ref -> Secret Broker -> injection -> target
```

The Agent never receives the actual secret.

## 5.3 `strict`

Enterprise high-security mode:

```text
plain credential detected
        |
        v
      BLOCK
        |
        v
credential_ref required
```

The mode is configurable per project, Agent, tool or environment.

### Policy

```c
typedef enum {
    SECRET_MODE_PASSTHROUGH,
    SECRET_MODE_MANAGED,
    SECRET_MODE_STRICT
} secret_mode_t;
```

---

# 6. End-to-End Login Flow

## 6.1 Managed login

```mermaid
sequenceDiagram
    participant U as User
    participant I as Interceptor
    participant A as Agent
    participant L as LLM
    participant T as Login Tool
    participant P as Policy
    participant B as Secret Broker
    participant S as Secret Store
    participant E as Executor

    U->>I: login request
    I->>B: store/resolve credential
    B->>S: decrypt metadata / retrieve secret
    S-->>B: encrypted payload
    B-->>I: credential_ref
    I->>A: sanitized request
    A->>L: prompt with credential_ref
    L-->>A: invoke login_tool(credential_ref)
    A->>T: login request
    T->>P: authorize(secret_ref)
    P-->>T: allow
    T->>B: request injection
    B->>S: retrieve secret
    S-->>B: secret payload
    B->>E: inject secret
    E-->>T: authenticated
    B->>B: zeroize plaintext
    T-->>A: session_ref
    A-->>L: authenticated=true
```

The LLM sees:

```json
{
  "credential_ref": "cred://project/test-server/login"
}
```

It never sees:

```json
{
  "password": "actual-secret"
}
```

## 6.2 Existing legacy login

```text
Agent
  |
  v
login_tool(username,password)
  |
Compatibility Policy
  |
  +-- allowed --> target login
  |
  +-- managed migration --> Secret Broker
  |
  +-- strict --> deny
```

The compatibility path is intentionally retained to prevent sudden behavior regressions.

---

# 7. Secret Interceptor

## 7.1 Interception points

The interceptor is mandatory at these boundaries:

```text
UI input
CLI input
IM input
Tool arguments
Tool results
MCP request
MCP response
LLM input
LLM output
Memory write
RAG ingestion
Logging
Tracing
Audit serialization
```

## 7.2 Detection pipeline

```text
Input
  |
  v
Pattern Detector
  |
  v
Entropy Detector
  |
  v
Field/Context Classification
  |
  v
Confidence
  |
  +---- low ------> allow
  |
  +---- medium ---> redact/audit
  |
  +---- high -----> tokenize/block
```

Rules must be deterministic first. ML classification is optional and should not be required for basic operation.

## 7.3 Examples

High-confidence patterns:

```text
Authorization: Bearer ...
-----BEGIN OPENSSH PRIVATE KEY-----
password=...
api_key=...
AWS-like access key patterns
JWT-like tokens
```

False-positive examples:

```text
password field containing placeholder text
non-secret IDs
public keys
example documentation values
```

Applications should be able to provide field-level annotations to suppress known false positives.

---

# 8. Redaction and Tokenization

## 8.1 Redaction formats

Default:

```text
[REDACTED:secret]
```

For a managed secret:

```text
<SECRET_REF:cred://project/test-server/login>
```

References must remain opaque and non-reversible.

## 8.2 Egress policy

If LLM output contains a secret-looking value:

```text
LLM Output
   |
Secret Detector
   |
 +-- no secret --> deliver
 |
 +-- secret --> redact
              |
              +-- policy ALLOW_REDACT --> deliver redacted
              |
              +-- policy BLOCK --> reject output
```

The original LLM response must not be persisted to ordinary logs before redaction.

---

# 9. Secret Broker

## 9.1 Responsibilities

The broker is the only supported service for managed secret resolution.

```c
int secret_resolve(
    const secret_ref_t *ref,
    const secret_access_context_t *ctx,
    secret_buffer_t *out
);

int secret_inject(
    const secret_buffer_t *secret,
    const injection_target_t *target
);

int secret_revoke(
    const secret_ref_t *ref
);
```

## 9.2 Access context

```c
typedef struct {
    uint64_t agent_id;
    uint64_t task_id;

    char capability[128];
    char target[256];

    uint64_t request_id;
    uint64_t timestamp_ns;
} secret_access_context_t;
```

Policy must evaluate the whole context, not only the secret ID.

Example:

```text
Agent A
+ capability secret.use
+ target test-server
+ project cognitive-os
=> ALLOW

Agent A
+ capability secret.use
+ target production-db
=> DENY
```

---

# 10. Secure Injection

## 10.1 Principles

Never inject credentials through a channel that is unnecessarily observable.

Avoid by default:

```text
argv
plain log output
shared files
LLM context
ordinary environment dumps
```

Prefer where supported:

```text
stdin
pipe
protected FD
memfd
OS credential APIs
short-lived session handles
```

## 10.2 Injection interface

```c
typedef enum {
    INJECT_STDIN,
    INJECT_FD,
    INJECT_ENV,
    INJECT_FILE,
    INJECT_BROWSER_SESSION,
    INJECT_CUSTOM
} injection_mode_t;

typedef struct {
    injection_mode_t mode;
    int fd;
    char path[256];
    char env_name[128];
    uint64_t ttl_ms;
} injection_target_t;
```

The executor should choose the least observable injection mode supported by the target.

---

# 11. Session Broker

A successful authentication should not require repeatedly obtaining a password.

```text
credential_ref
      |
      v
    login
      |
      v
session_ref
```

Example:

```text
session://project/test-server/7a5d...
```

The Agent receives the session reference only.

Subsequent tool calls:

```text
Agent
  |
  v
session_ref
  |
Session Broker
  |
  v
authenticated channel
```

Session properties:

- TTL;
- target binding;
- Agent binding;
- project binding;
- revocation status;
- generation number.

---

# 12. Memory / RAG Protection

Secret protection must exist before persistent memory and retrieval.

```text
Raw data
   |
Secret Interceptor
   |
 +-------------------------+
 |                         |
 normal                  secret
 |                         |
 v                         v
Memory / RAG            Secret Broker
                           |
                    reference only
```

Long-term Markdown example:

```markdown
---
type: credential_reference
sensitivity: restricted
secret_ref: cred://project/test-server/login
---

# Test Server Credential

Managed by Secret Broker.
The actual secret must never be stored in this file.
```

Rules:

- no plaintext secrets in Markdown memory;
- no secret embedding vectors;
- no secret values in graph properties;
- no secret values in memory summaries L0/L1/L2;
- secret references may appear only where policy permits.

---

# 13. LLM Boundary Protection

The LLM-facing representation of a secret-bearing action should be:

```json
{
  "tool": "ssh_login",
  "target": "test-server",
  "credential_ref": "cred://project/test-server/login"
}
```

The runtime should reject attempts to place a managed secret into:

- system prompt;
- user prompt;
- tool result text;
- memory write payload;
- RAG document;
- trace annotation.

The runtime should also sanitize model output before displaying or storing it.

---

# 14. Plugin / MCP Integration

Generated plugins are a high-risk boundary.

Plugin request:

```text
Plugin
 |
 +-- capability: secret.use
 +-- reference: cred://...
 |
 v
Secret Broker
```

A plugin must never receive a wildcard capability such as:

```text
secret.read_all
```

Recommended capability forms:

```text
secret.use:cred://project/test-server/login
secret.use:session://project/test-server/*
```

A plugin that only needs authenticated access should receive `session.use` rather than raw credential access.

---

# 15. Data Model

## 15.1 `secret_metadata`

| Field | Type | Constraint |
|---|---|---|
| `secret_id` | BIGINT | PK |
| `reference` | VARCHAR(512) | UNIQUE |
| `secret_type` | SMALLINT | NOT NULL |
| `scope` | VARCHAR(256) | INDEX |
| `provider` | VARCHAR(64) | NOT NULL |
| `version` | INTEGER | NOT NULL |
| `status` | SMALLINT | INDEX |
| `created_at` | TIMESTAMP | NOT NULL |
| `expires_at` | TIMESTAMP | INDEX |
| `last_used_at` | TIMESTAMP | INDEX |
| `owner_agent_id` | BIGINT | INDEXABLE |
| `policy_id` | BIGINT | INDEX |

The database contains metadata, not plaintext secret values.

## 15.2 `secret_audit`

| Field | Type |
|---|---|
| `audit_id` | BIGINT PK |
| `secret_id` | BIGINT |
| `agent_id` | BIGINT |
| `task_id` | BIGINT |
| `action` | SMALLINT |
| `decision` | SMALLINT |
| `target` | VARCHAR(256) |
| `created_at` | TIMESTAMP |
| `trace_id` | BIGINT |
| `error_code` | INTEGER |

Never store the secret value, password length when unnecessary, or raw token.

## 15.3 Indexes

```sql
CREATE UNIQUE INDEX idx_secret_reference
ON secret_metadata(reference);

CREATE INDEX idx_secret_scope_status
ON secret_metadata(scope, status);

CREATE INDEX idx_secret_expiry
ON secret_metadata(expires_at);

CREATE INDEX idx_secret_audit_agent_time
ON secret_audit(agent_id, created_at);
```

---

# 16. Encryption Design

## 16.1 Envelope encryption

Recommended architecture:

```text
Application Secret
      |
      v
Data Encryption Key (DEK)
      |
      v
Encrypted Secret Blob
      |
      v
Key Encryption Key (KEK)
      |
      v
KMS / Vault / HSM
```

For Personal Edition, a local encrypted store may use a user-protected root key. Enterprise should integrate an external Vault/KMS/HSM rather than inventing an enterprise key-management service.

## 16.2 Cryptographic rules

- authenticated encryption only;
- unique nonce/IV per encryption operation;
- key versioning;
- key rotation support;
- no custom cryptography;
- no plaintext backup files;
- no secrets in crash dumps where feasible.

The crypto implementation should use a maintained cryptographic library appropriate to the target platform.

---

# 17. Lifecycle

```mermaid
stateDiagram-v2
    [*] --> DISCOVERED
    DISCOVERED --> CLASSIFIED
    CLASSIFIED --> STORED
    STORED --> ACTIVE
    ACTIVE --> IN_USE
    IN_USE --> ACTIVE
    ACTIVE --> EXPIRED
    ACTIVE --> REVOKED
    EXPIRED --> ARCHIVED
    REVOKED --> ARCHIVED
    ARCHIVED --> [*]
```

Temporary plaintext buffers have a separate lifecycle:

```text
ALLOCATED
   ↓
FILLED
   ↓
USED
   ↓
ZEROIZED
   ↓
RELEASED
```

---

# 18. Error Handling

| Code | Meaning | Action |
|---|---|---|
| `SEC-1001` | Invalid reference | Reject |
| `SEC-1002` | Secret not found | Reject |
| `SEC-1003` | Secret expired | Reject / rotate |
| `SEC-1004` | Secret revoked | Reject |
| `SEC-1005` | Capability denied | Reject |
| `SEC-1006` | Policy denied | Reject |
| `SEC-1007` | Provider unavailable | Retry if safe |
| `SEC-1008` | Decryption failed | Reject; audit |
| `SEC-1009` | Injection failed | Reject; cleanup |
| `SEC-1010` | Secret detected in prohibited channel | Redact/block |
| `SEC-1011` | Session expired | Re-authenticate |
| `SEC-1012` | Session target mismatch | Reject |
| `SEC-1013` | Compatibility policy denied | Reject |
| `SEC-1014` | Secure zeroization failed | Raise high-severity audit |

### Retry rules

- Secret provider network read: up to 3 bounded retries with exponential backoff.
- Decryption failure: **do not retry blindly**; likely key/version/configuration error.
- Policy denial: no retry.
- Injection failure: cleanup then fail.
- Login failure: retry only according to tool-specific authentication policy; do not repeatedly brute-force credentials.

---

# 19. Concurrency

## 19.1 Local ownership

Prefer immutable secret metadata and per-request secret buffers.

Do not expose mutable plaintext buffers across unrelated threads.

## 19.2 Provider concurrency

Use connection pools for external Vault/KMS providers, but never cache plaintext secrets in a global process-wide cache.

Allowed cache:

```text
reference -> encrypted blob / metadata
```

Disallowed long-lived cache:

```text
reference -> plaintext password
```

## 19.3 Secret rotation race

Use `(secret_id, version)` as the compare-and-check identity.

If a caller has version 3 but current version is 4:

```text
resolve v3
    |
    v
version mismatch
    |
 policy: reject / refresh
```

Do not silently use an arbitrary newer secret when the target protocol is version-sensitive.

---

# 20. API Design

## 20.1 Store secret

```http
POST /api/v1/secrets
Idempotency-Key: <uuid>
```

Request example:

```json
{
  "type": "ssh_credential",
  "scope": "project/cognitive-os",
  "name": "test-server-login",
  "provider": "local"
}
```

The actual secret value should be transmitted through a protected credential-input path rather than normal chat/message fields.

Response:

```json
{
  "reference": "cred://project/cognitive-os/test-server-login",
  "version": 1,
  "status": "ACTIVE"
}
```

## 20.2 Test credential

```http
POST /api/v1/secrets/{reference}/test
```

Response:

```json
{
  "reachable": true,
  "authenticated": true,
  "sessionRef": "session://project/cognitive-os/test-server/7a5d"
}
```

## 20.3 Revoke

```http
POST /api/v1/secrets/{reference}/revoke
```

## 20.4 Internal broker API

```c
int secret_broker_resolve(...);
int secret_broker_authorize(...);
int secret_broker_inject(...);
int secret_broker_revoke(...);
int secret_broker_rotate(...);
int secret_broker_session_create(...);
int secret_broker_session_revoke(...);
```

---

# 21. Idempotency and Rate Limiting

Idempotency is required for:

```text
secret create
secret rotate
secret revoke
session create
```

Rate limits:

| Operation | Personal | Enterprise default |
|---|---:|---:|
| secret create | 20/min | 100/min/tenant |
| resolve | 300/min | 3,000/min/tenant |
| session create | 30/min | 300/min/tenant |
| rotate | 10/min | 100/min/tenant |
| revoke | 30/min | 300/min/tenant |

Actual limits should be configurable.

---

# 22. Observability

## Metrics

```text
secret_detected_total
secret_redacted_total
secret_blocked_total
secret_resolve_total
secret_resolve_denied_total
secret_injection_total
secret_injection_failure_total
secret_provider_latency
secret_provider_error_total
secret_rotation_total
session_create_total
session_expired_total
```

## Audit example

```json
{
  "event": "secret.use",
  "secretRef": "cred://project/test-server/login",
  "agentId": "agt_123",
  "taskId": "task_456",
  "target": "test-server",
  "decision": "ALLOW",
  "traceId": "tr_789",
  "timestamp": "2026-09-13T14:00:00Z"
}
```

No secret value should appear in the audit record.

---

# 23. Failure Scenarios

## 23.1 Secret provider unavailable

```text
Agent
 ↓
Tool
 ↓
Secret Broker
 ↓
Provider timeout
 ↓
bounded retry
 ↓
if still unavailable -> TOOL_AUTH_UNAVAILABLE
```

Do not expose provider credentials to the Agent as a fallback.

## 23.2 Secret detected in LLM prompt

```text
Detector
 ↓
high confidence
 ↓
Managed mode: tokenize
Strict mode: block
```

## 23.3 Secret detected in tool result

The result is redacted before reaching the Agent/LLM and before ordinary persistence.

## 23.4 Existing login stops working

The compatibility policy must check:

```text
Is this an existing registered tool?
Is passthrough allowed?
Is the credential format still valid?
Does the tool return a valid session?
```

The rollout must not globally enable strict mode without migration validation.

## 23.5 Rollback of a login task

If a transaction rolls back code/files, an authenticated remote session may still exist.

Therefore transaction rollback does **not** imply credential/session rollback.

The system must separately revoke temporary sessions when the policy requires it.

---

# 24. Security Rules for Logs, Memory and RAG

The following is mandatory:

```text
                         Secret
                           |
        +------------------+------------------+
        |                  |                  |
       LLM               Memory              RAG
        |                  |                  |
       DENY               DENY               DENY

        +------------------+------------------+
                           |
                         Logs
                           |
                          DENY
```

Safe substitutes:

```text
[REDACTED]
credential_ref
session_ref
secret_type
policy_id
access outcome
```

---

# 25. Plugin Security

Generated plugins must pass:

```text
Generate
 ↓
Static analysis
 ↓
Capability extraction
 ↓
Build
 ↓
Sandbox test
 ↓
Secret-access review
 ↓
Package signing
 ↓
Registry
 ↓
Install
```

A plugin that requests a new secret capability should trigger a policy review before installation.

---

# 26. Personal vs Enterprise

## Personal

Default:

```text
managed = recommended
passthrough = allowed
strict = optional
```

Provider:

```text
local encrypted store
```

## Enterprise

Default:

```text
managed
```

Preferred provider:

```text
Vault / KMS / HSM / enterprise secret manager
```

Additional controls:

- tenant isolation;
- RBAC/ABAC;
- approval workflows;
- audit retention;
- credential rotation;
- node-local execution broker;
- secret-use anomaly detection.

---

# 27. Code Architecture

```text
backend/src/security/
│
├── interceptor/
│   ├── secret_ingress.c
│   ├── secret_egress.c
│   └── secret_boundary.c
│
├── detector/
│   ├── secret_detector.c
│   ├── pattern_detector.c
│   ├── entropy_detector.c
│   └── field_classifier.c
│
├── redactor/
│   ├── secret_redactor.c
│   └── tokenization.c
│
├── broker/
│   ├── secret_broker.c
│   ├── secret_access.c
│   └── secret_resolver.c
│
├── injector/
│   ├── injector.c
│   ├── stdin_injector.c
│   ├── fd_injector.c
│   └── session_injector.c
│
├── session/
│   ├── session_broker.c
│   ├── session_store.c
│   └── session_policy.c
│
├── policy/
│   ├── secret_policy.c
│   └── capability.c
│
├── lifecycle/
│   ├── expiry.c
│   ├── rotation.c
│   ├── revoke.c
│   └── zeroize.c
│
├── audit/
│   └── secret_audit.c
│
└── provider/
    ├── provider.c
    ├── local_provider.c
    ├── vault_provider.c
    └── kms_provider.c
```

Public headers:

```text
include/cognitive-os/security/
├── secret.h
├── secret_broker.h
├── secret_policy.h
├── secret_detector.h
├── secret_injector.h
└── session.h
```

---

# 28. Integration with Existing Cognitive OS Layers

```text
                         UI / CLI / IM
                               |
                               v
                       Secret Interceptor
                               |
                         API / Agent
                               |
          +--------------------+--------------------+
          |                    |                    |
          v                    v                    v
       Memory                Context              LLM
       Guard                  MMU                Guard
          |                    |                    |
          +--------------------+--------------------+
                               |
                              Agent
                               |
                         Action / Tool
                               |
                       Policy / Capability
                               |
                        Secret Broker
                               |
                         Transaction
                               |
                           Executor
```

Important boundary:

```text
Memory/Context/LLM never own secret plaintext.
Execution boundary may temporarily own plaintext.
```

---

# 29. Test Plan

## 29.1 Unit tests

- detector high-confidence patterns;
- detector false positives;
- redaction correctness;
- reference validation;
- policy allow/deny;
- expiry checks;
- version checks;
- session target binding;
- zeroization API behavior.

## 29.2 Integration tests

### Login

```text
managed credential -> login succeeds
managed credential -> wrong password -> failure without leak
legacy plaintext -> compatibility login succeeds
strict + plaintext -> blocked
session reuse -> no second credential retrieval
expired session -> re-auth required
```

### LLM

```text
credential input -> prompt contains only credential_ref
LLM output secret-like data -> redacted
tool result secret-like data -> redacted
memory write secret -> blocked/redacted
RAG ingestion secret -> blocked/redacted
```

### Failure injection

```text
Vault timeout
KMS timeout
decryption failure
provider unavailable
injection failure
disk full
concurrent rotation
session revoke during tool call
```

## 29.3 Security tests

- process listing does not expose password through argv;
- ordinary logs contain no plaintext credentials;
- memory dump analysis where feasible;
- plugin cannot access another Agent's credential scope;
- strict policy cannot be bypassed through MCP;
- secret references cannot be guessed into valid credentials.

---

# 30. Rollout Plan

The rollout must preserve existing logins.

## Phase 0 — Observe

```text
Detect -> Audit -> Do not block
```

Collect false-positive and compatibility statistics.

## Phase 1 — Tokenize managed credentials

```text
New integrations -> managed
Existing integrations -> passthrough
```

## Phase 2 — Default managed mode

New tools and plugins require managed credentials.

Existing tools remain compatibility-enabled during migration.

## Phase 3 — Enterprise strict

Selected high-risk environments:

```text
production
regulated data
privileged infrastructure
```

require `credential_ref` and deny plaintext credential input.

---

# 31. Design Advantages

1. **No secret in LLM context.** This is stronger than relying on prompt instructions.
2. **Compatible migration.** Existing login flows can remain functional while security is introduced incrementally.
3. **Execution-bound decryption.** Plaintext is created only where it is needed.
4. **Agent isolation.** Secret access follows Agent/Project/Tenant capability boundaries.
5. **Memory/RAG safe by construction.** Secret material is removed before durable knowledge pipelines.
6. **Provider abstraction.** Local encrypted storage and enterprise Vault/KMS can share one Broker interface.
7. **Session reuse.** Successful login can become an opaque session reference instead of repeatedly exposing credentials.
8. **Plugin-aware.** Generated capabilities must request explicit secret permissions.

---

# 32. Design Risks

## Risk 1: Over-aggressive detection

Mitigation: compatibility mode, field annotations, confidence thresholds, observe-first rollout.

## Risk 2: Plaintext process exposure

Mitigation: avoid argv, use stdin/FD/memfd where possible, shorten residency, zeroize buffers.

## Risk 3: Host compromise

The Broker cannot guarantee secrecy from a fully compromised host kernel. Enterprise deployments should combine it with sandboxing, VM isolation or hardware-backed trust where required.

## Risk 4: Provider dependency

Mitigation: Provider interface + local fallback where policy allows.

## Risk 5: Security component becomes a bottleneck

Mitigation: metadata/index caching, asynchronous audit, provider connection pooling, no LLM in hot detection path.

## Risk 6: Login behavior regression

Mitigation: three-mode compatibility design and phased rollout.

---

# 33. Final Design Principles

```text
1. Secrets are references in cognition, not data in cognition.

2. The LLM may request a credential operation,
   but must never receive the credential value.

3. Secret plaintext exists only at the execution boundary.

4. Memory and RAG receive references or redacted content only.

5. Policy and capability checks happen before every managed secret use.

6. Existing login paths remain available through explicit compatibility policy.

7. Security is deterministic code, not a prompt instruction.
```

Final data flow:

```text
User Input
   ↓
Secret Detection
   ↓
Redaction / Credential Reference
   ↓
Agent
   ↓
LLM
   ↓
Tool Request
   ↓
Capability + Policy
   ↓
Secret Broker
   ↓
Decrypt
   ↓
Secure Injection
   ↓
Execution
   ↓
Session Reference / Result
   ↓
Redaction
   ↓
Memory / Logs / UI
```

**Cognitive OS Secret Security Plane is therefore a security boundary between cognition and execution: the Agent can request the use of a credential without ever possessing the credential itself.**

---

# 34. Architecture Decision Records

## ADR-SEC-001 — Secret Broker is a first-class subsystem

Decision: Accepted.

Reason: credentials cross UI, Agent, Tool, Execution and external-provider boundaries and require centralized policy.

## ADR-SEC-002 — Managed secrets are references in Agent context

Decision: Accepted.

Reason: prevents credential values from entering model context and durable cognitive state.

## ADR-SEC-003 — Compatibility mode is mandatory

Decision: Accepted.

Reason: security deployment must not cause existing successful authentication paths to fail without migration.

## ADR-SEC-004 — LLM is never the credential resolver

Decision: Accepted.

Reason: credential resolution is a deterministic security operation and must remain outside model control.

## ADR-SEC-005 — Plaintext decryption occurs at execution boundary

Decision: Accepted.

Reason: minimizes plaintext residency and avoids returning secrets to Agent memory.

## ADR-SEC-006 — L0/L1/L2 and HOT/WARM/COLD remain independent

Decision: Accepted.

Reason: L0/L1/L2 describe memory representation; HOT/WARM/COLD describe runtime cache residency. Secret security must operate before both persistent memory and context cache ingestion.
