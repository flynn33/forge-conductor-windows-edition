# Continuity tool contract

Support both compatibility surfaces:

Legacy:

- `session_checkpoint`
- `session_handoff`
- `context_get`
- `context_list`

Lifecycle:

- `continuity.checkpoint`
- `continuity.prepare_handoff`
- `continuity.get_pending_handoff`
- `continuity.acknowledge_handoff`
- `continuity.resume`
- `continuity.status`
- `continuity.request_rollover`

Lifecycle calls are project-scoped and idempotent. `request_rollover` must report whether a real successor was created. It may not claim GUI-chat creation without host evidence. In Forge-native mode it creates a logical successor through the native session host.

Tests cover duplicate operations, crashes at every transition, exact acknowledgment, wrong checksum/session/handoff, concurrent rollover, provider timeout, rate limit, storage limit, cancellation, database busy, restart, and multi-project isolation.
