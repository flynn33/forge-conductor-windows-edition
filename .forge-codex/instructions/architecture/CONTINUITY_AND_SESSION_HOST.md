# Autonomous continuity and session rollover

## Objective

The system must persist project state, create a successor logical session, bootstrap it with an exact handoff, receive an acknowledgment, seal the predecessor, and resume work without operator intervention.

## State machine

Use a durable per-project operation:

```text
idle
→ checkpoint_preparing
→ checkpoint_persisted
→ successor_creating
→ successor_created
→ bootstrap_sending
→ acknowledged
→ predecessor_sealing
→ completed
```

Recoverable branches:

```text
retry_wait
failed_recoverable
cancelling
cancelled
```

Only valid compare-and-set transitions are allowed. Persist every transition and checksum.

## Handoff

Canonical UTF-8 JSON includes:

- schema/version;
- project and operation IDs;
- predecessor/successor logical and provider session IDs;
- model/provider;
- mission and constraints;
- phase/work-item IDs;
- repository root, branch, commit, dirty summary;
- active files;
- completed work, open work, decisions;
- passed/open gates;
- project-memory record IDs and evidence IDs;
- exact next actions;
- creation time;
- content SHA-256.

The first successor exchange must contain the canonical handoff or its lossless representation. Acknowledgment must match the exact handoff ID, successor session ID, and SHA-256.

## Context-budget sources

Use, in precedence order:

1. provider-reported remaining context;
2. provider token-usage metadata;
3. configured model context size minus locally measured usage;
4. conservative byte/token estimate.

Trigger early enough to complete checkpoint and bootstrap. Thresholds are configurable and bounded. Persist checkpoints periodically even before rollover.

## Session-host adapter

Define `ISessionHostAdapter` capabilities for create, bootstrap, acknowledge, cancel, query, and recover.

When LM Studio or another host exposes a supported session API, implement a provider adapter and verify it. When it does not, build `ForgeNativeSessionHostPlugin` and `ForgeConductor.SessionHost.exe`:

- own logical conversation sessions;
- call a configured and verified local model HTTP API;
- execute Forge tools through the application service boundary;
- persist messages and token usage;
- create successor sessions autonomously;
- bootstrap and verify handoffs;
- resume after manager/process restart.

A Forge-owned logical session is acceptable. Falsely claiming that an LM Studio GUI chat was created is not.

## Crash recovery

At startup:

- scan incomplete operations;
- verify checksums and durable handoff;
- query adapter idempotency key;
- resume the next transition;
- avoid duplicate successor sessions;
- cancel orphaned provider operations when safe;
- back off with persisted retry time;
- surface health without blocking unrelated projects.

## Project isolation

Continuity operations, active sessions, handoffs, budgets, and memory references are keyed by explicit project ID. A session cannot adopt another project's handoff without an explicit validated import.
