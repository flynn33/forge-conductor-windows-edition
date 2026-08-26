# Autonomous operation

## Durable state

The target repository stores orchestration state under `.forge-codex/state/`:

```text
run-state.json
event-ledger.jsonl
evidence-index.json
decisions/
blockers/
handoffs/
gate-results/
phase-results/
```

Updates use write-to-temp, flush, and atomic replace. Every event contains sequence number, UTC time, role, phase, action, input hash, output hash, previous event hash, and event hash.

## Roles without operator intervention

Codex executes bounded sessions under one role at a time:

- Architect: contract, scope, architecture, ADRs.
- Builder: implementation only.
- Validator: fresh session; may add tests/evidence but does not alter production behavior to make validation pass.
- Documentation Manager: synchronizes documentation.
- Release Manager: packaging and release classification.

The autonomous driver starts a fresh session when the current one approaches its context limit or after a bounded work unit. It passes only the governing prompt and repository handoff.

## Work selection

1. Read run state and latest valid handoff.
2. Select the earliest incomplete phase whose dependencies passed.
3. Select the smallest deliverable slice.
4. implement;
5. build;
6. run focused tests;
7. repair failures;
8. run phase gates;
9. persist evidence and state;
10. continue or create handoff.

## No-progress protection

If two consecutive sessions produce no source, test, documentation, decision, or evidence change:

- classify the blocker;
- inspect logs and environment;
- choose an independent ready phase if possible;
- create a focused recovery task;
- never repeat the identical command indefinitely.

## External blockers

Missing network, SDK download, model host, signing secret, or hardware capability is not permission to discard work. Produce the maximum locally verifiable implementation, use deterministic fakes, retain a replayable validation script, and continue independent work. The final completion gate remains open until required real evidence exists.
