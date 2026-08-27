# P14-008: Source-Compatible MCP Semantics and Qualified Windows Extensions

Status: Accepted

Date: 2026-08-27

## Context

P14 exposes the 53-tool MCP catalog through typed Windows services. Exact tool
names and advertised schemas are necessary but are not sufficient for feature
parity: the macOS 0.9.0 handlers also define observable aliases, JSON
coercions, defaults, error codes, and response envelopes. Several of those
behaviors are more permissive than the schemas they advertise.

P08-001 previously selected only the declared `project_path` spelling for
`project_memory.initialize`, because its closed schema omits the macOS
handler's undocumented `path` alias. That implementation choice conflicts with
the attached source evidence, the no-feature-loss requirement, and the owner's
direct requirement to preserve functionality and features. P14 must therefore
preserve source-compatible call behavior without weakening the advertised
closed schema or the typed authority boundary.

Windows also has deliberate platform behavior that cannot be represented as a
byte-for-byte copy of the macOS implementation. Those differences must remain
qualified extensions rather than silent parity drift.

## Decision

- The Windows adapter preserves the exact legacy aliases used by the attached
  handlers. `project_memory.initialize.path` is pre-normalized to
  `project_path`, with `project_path` winning when both are supplied. Legacy
  filesystem move source precedence is `path`, then `src`, then `source`, and
  destination precedence is `dest`, then `destination`. Existing legacy
  memory and continuity aliases, including `body`/`content`/`value`,
  `query`/`q`/`pattern`, `project_slug`/`project`, `chat_label`/`chat`,
  `narrative`/`summary`, and `handoff_id`/`id`, remain observable.
- Source-compatible normalization is allowlisted per closed project-memory and
  continuity-lifecycle field before the unchanged schema validator runs.
  String fields accept JSON strings and source-equivalent JSON numbers, then
  trim the resulting text. Integer fields accept integers, exact integral
  finite numbers, and complete base-10 integer strings. Invalid optional
  booleans are treated as absent so the handler default applies.
- Project-memory string-list fields accept one string as a one-item list;
  arrays retain string members and discard non-string members. Continuity list
  fields accept arrays only, retain string members, discard non-string
  members, and normalize every other supplied shape to an empty list. The same
  project-memory rules apply recursively to every
  `project_memory.remember_batch` item. Unknown closed-schema properties remain
  rejected, and normalization does not bypass domain validation, authority,
  item limits, or response limits.
- Empty lifecycle `next_actions` produces the source default `Continue current
  work`. An omitted Git commit message produces
  `chore: forge-conductor commit`. An omitted or empty `dest_path` for
  `pdf_from_file` derives the destination by replacing the source extension
  with `.pdf`. Shell calls without `timeout_seconds` use the injected
  application configuration rather than an adapter-local constant or ambient
  process discovery.
- Legacy failures retain their source-visible codes at the individual tool
  boundary. These include `agent_not_found`, `missing_session_id`,
  `missing_path`, `missing_args`, `file_too_large`, `not_found`, `no_match`,
  `missing_pattern`, `missing_source`, `missing_command`, and
  `invalid_timeout`. Tool-specific remapping never converts an authority
  failure and never changes `unauthorized`. Unexpected service, store, OS, or
  process exceptions remain the bounded retryable `tool_exception` envelope.
- Process completion with a nonzero exit code remains a successful tool
  dispatch carrying `ok: false`, the exit fields, stdout, and stderr. It does
  not synthesize a legacy payload `error`; the typed receipt may retain
  `process_exit_nonzero` as Windows internal metadata.
- Source response shapes are preserved where they affect callers.
  `fs_delete` returns `deleted: true`. A committed
  `project_memory.import` emits the canonical remember-batch envelope with
  `ok`, `project_id`, `count`, `results`, `schema_version`, and
  `capability_version`; preview keeps its import-preview envelope.
  `continuity.status` without an operation reports `state: active`.
- Recursive glob patterns without a directory separator, such as `*.txt` and
  `*`, match basenames at every traversed depth, preserving `find -name`
  behavior. Patterns containing `/` or `\` retain the component-aware
  root-relative `*`, `?`, and `**` semantics selected by P13-002.
- A deterministic semantic matrix covers exactly 53 catalog tools and four
  categories per tool: `valid`, `invalid`, `dependency_error`, and `boundary`.
  The resulting 212 cases verify catalog membership, payload subsets, typed
  effects, dependency behavior, and focused observations without replacing
  the existing integration tests for authority, context recovery, or real
  process composition.

## Qualified Windows extensions

- `project_memory.initialize` honors `idempotency_key`; the attached macOS
  handler accepted the field but did not use it.
- `continuity.acknowledge_handoff` inherits the pending handoff's adapter when
  `adapter_id` is omitted, preserving the native session-host chain instead of
  forcing `external-mcp`.
- `continuity.request_rollover` invokes the real host rollover service after
  preparation rather than stopping at a memory-only handoff.
- `fs_read` normalizes text to CRLF and pages MCP text at 96 KiB instead of
  returning one full response up to the source's 2 MiB file limit.
- The current continuity status model cannot derive the source repository
  pointer for `active_session_id`. It reports the acknowledged session when
  available and retains additive operation, count, retry, and recovery fields;
  this limitation is not claimed as final parity.
- `fs_edit` may add `bytes_written`, `fs_mkdir` may add `created`, and bounded
  Git/shell process payloads may add cancellation, termination, elapsed-time,
  and receipt metadata.
- PDF creation uses the qualified Windows platform PDF engine rather than the
  macOS engine while retaining the source request defaults and response
  contract.

## Consequences

Callers can continue using the macOS-compatible legacy surface while the
catalog still advertises the stricter, preferred Windows spelling and closed
schemas. The normalization boundary is explicit and allowlisted, so source
compatibility does not become a general schema bypass.

The matrix makes semantic drift visible at one bounded gate, consistent with
the owner's reduced-rebuild alpha scope. This decision does not assert that the
matrix or any runtime gate has passed.

G14 remains in progress. Dynamic multi-project workspace authority and
project-scoped Git/shell execution are still required. P16 manager IPC is also
still required before the final cross-process ownership architecture can be
claimed.

## Conflict resolution

Direct owner requirements and the no-feature-loss contract outrank P08-001's
local implementation choice to accept only `project_path`. This decision
supersedes only that sentence of P08-001. The advertised schema continues to
declare `project_path`; the exact `path` alias is removed by pre-normalization
before closed-schema validation. All remaining P08-001 project identity,
canonicalization, registry, and authority decisions remain in force.

## Evidence basis

- `.forge-codex/state/decisions/P08-001-project-identity-and-registry-authority.md`
- `.forge-codex/state/decisions/P13-002-handle-aware-filesystem-search-and-glob.md`
- `.forge-codex/state/decisions/P14-001-mcp-catalog-codec-and-wire-precedence.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/instructions/plans/mcp-tool-parity.json`
- `.forge-codex/state/baseline/p02-mcp-semantic-inventory.json`
- macOS `AgentToolPack.swift`, `ContinuityToolPack.swift`,
  `ContinuityLifecycleToolPack.swift`, `FilesystemToolPack.swift`,
  `GitToolPack.swift`, `MemoryToolPack.swift`, `PdfToolPack.swift`,
  `ProjectMemoryToolPack.swift`, `SearchToolPack.swift`, and
  `ShellToolPack.swift`
