# P08-001: Project Identity and Registry Authority

Status: Accepted

Date: 2026-08-26

## Context

Project memory must remain explicitly scoped when several repositories are open.
The macOS source identifies projects by a generated UUID while retaining a
canonical path alias and an optional Git-remote-derived repository identity.
The Windows security policy additionally requires handle-derived canonical
paths, rejection of reparse traversal and normalization ambiguity, bounded
state, and durable cross-process updates.

The public registry contract accepts `project_path`; the source handler also
accepted an undocumented `path` alias that was absent from its closed schema.
The declared schema is the observable contract.

## Decision

- `project_memory.initialize` accepts only the declared `project_path` spelling.
  MCP decoding remains P14 work; P08 implements the typed request behavior.
- Resolve an existing local directory by opening it with
  `FILE_FLAG_OPEN_REPARSE_POINT`, rejecting reparse points, case-sensitive
  directories, UNC/device/alternate-stream forms, and non-directories, then
  obtain the normalized DOS path with `GetFinalPathNameByHandleW`. The retained
  handle is the authority for inspection during that resolve operation.
- Derive optional Git identity from the canonical project's own `.git/config`
  only after handle-relative validation. Hash the sorted normalized remote URL
  lines and store `git:<sha256>`. A caller-supplied repository identity is
  normalized and compared rather than silently replacing a conflicting value.
- Match in this order: explicitly requested UUID, repository identity, canonical
  alias. A requested UUID must already exist. Any disagreement among supplied
  identity evidence returns `project_scope_mismatch`; selection is never based
  on recency.
- Store registry schema version 1 separately from project databases as a bounded
  JSON record beneath the application data root. Cap it at 2 MiB, 1,024
  projects, and 32 aliases per project. Overflow fails before mutation.
- Serialize read-modify-publish across processes with an application-owned lock.
  Persist by the existing handle-anchored atomic replacement boundary and retain
  a sibling backup. Publish is the registry update linearization point.
- Project IDs are lowercase UUIDs. Aliases compare using Windows ordinal
  case-insensitive semantics and are stored in deterministic order.
- Detaching an alias never deletes the project database. Destructive database
  removal is a separate, confirmed memory-reset operation.

## Consequences

Registry size and lookup work are bounded, aliases cannot silently cross project
scope, and concurrent processes cannot lose each other's registry changes.
Projects on unsupported namespace forms fail closed. P08 proves memory reset and
alias detach; continuity reset and combined Settings workflows remain P11 and
P20 respectively.

## Evidence basis

- `.forge-codex/instructions/architecture/PROJECT_MEMORY.md`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ProjectMemoryService.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/ProjectMemoryToolPack.swift`
- `.forge-codex/instructions/plans/feature-parity-matrix.tsv` (`PMEM-001`, `PMEM-002`)

