# Continuity and native session host

Durable state machine:

```text
active -> checkpointing -> handoff_prepared -> successor_creating
-> successor_bootstrapping -> awaiting_exact_ack -> predecessor_sealed -> successor_active
```

Every transition is compare-and-set with project/operation/predecessor/successor/handoff/checksum/attempt/deadline/error. Retries after crash are idempotent. Startup reconciliation completes or safely returns to the last valid state.

Handoffs are bounded and reference large project-memory records. Include mission, constraints, phase/work, repository/commit/dirty summary, active files, completed/open work, decisions, gate/evidence IDs, blockers, next actions, context source/budget, and SHA-256. A successor becomes active only after exact handoff/session/checksum acknowledgment.

`ISessionHostAdapter` supports capabilities, create, bootstrap, acknowledge, cancel, query, and recover. Use a documented external host API when proven. Otherwise build `ForgeConductor.SessionHost.exe` and a first-party adapter that owns logical sessions and uses a supported local model API. Never automate a private GUI or claim an external GUI chat was created without evidence.
