# P10-001: Bounded Deterministic Agent Catalog and Windows Playbooks

Status: Accepted

Date: 2026-08-26

## Context

The macOS catalog loads Markdown playbooks from several bundle layouts, a
source-relative directory, compile-time fallbacks, and a per-user override
directory. It silently skips malformed files and finally keeps the
lexicographically first 256 identifiers. That behavior proves the required
ten playbooks, custom same-id replacement, and deterministic recommendation
rules, but it also performs unbounded discovery before truncation, can evict a
mandatory builtin, holds its reader lock during I/O, and resolves duplicate
custom identifiers in filesystem enumeration order.

Several source playbooks also declare unavailable runtime tools and macOS-only
commands. The owner contract prohibits that runtime in the target repository
and requires native Windows tooling. The canonical MCP inventory provides
native filesystem/search, PDF, shell-policy, CMake, MSBuild, and CTest
replacements without removing the affected product capabilities.

## Decision

- `AgentCatalog` is a final, non-copyable Application service implementing
  `IAgentCatalog`. It owns an immutable sorted snapshot and receives its clock
  and bounded definition documents through constructor injection; it performs
  no Win32 or filesystem work.
- The application module contains ten mandatory Windows playbooks under
  `src/ForgeConductor.Application/Resources/Agents`. The same ten definitions
  exist as compiled fallbacks so catalog availability does not depend on a
  packaging path. Tests require the resource and fallback forms to normalize
  to equivalent `AgentSpec` values.
- The mandatory identifiers are `debug`, `docs`, `explore`, `implement`,
  `plan`, `precommit-audit`, `research`, `review`, `security`, and `test`.
  They are always retained. A valid custom definition replaces a builtin with
  the same exact case-sensitive identifier. Additional custom identifiers are
  admitted in ordinal order until the 256-entry hard limit is reached.
- Parsing implements only the evidenced flat frontmatter profile: literal
  opening and closing fences, scalar keys, comma/bracket/dash lists, folded
  scalar lines, comments, and an uninterpreted Markdown body. Nested YAML is
  not introduced and no YAML dependency is added.
- One document is limited to 64 KiB of validated UTF-8. An agent has at most 64
  values in one list, 256 aggregate list values, and 64 KiB of normalized text.
  Recommendation input is limited to 4 KiB. NUL and disallowed control values
  fail before parsing or matching work.
- Reload constructs and validates a candidate snapshot away from the published
  snapshot, then replaces it under a short lock. Cancellation, deadline, or
  invalid-input failure leaves the last known-good snapshot intact.
- Recommendation preserves the source's ordered, case-insensitive ASCII
  substring rules and resolves the first available match. Unmatched work
  selects `explore`. Custom replacements therefore participate without a
  second rule registry.
- Allowed tool lists must be a subset of the canonical product inventory and
  only narrow global authorization. An empty list means deny-all. A listed
  shell tool does not enable shell execution; the separate global shell policy
  remains disabled by default.
- Windows playbooks preserve the source intent while using approved native
  names and commands. Search uses `fs_glob` and `search_text`, documents use
  `pdf_write` and `pdf_from_file`, and verification uses CMake, MSBuild, CTest,
  native test executables, and the opt-in bounded PowerShell boundary.

## Consequences

The ten product playbooks remain available and source-equivalent at the
behavioral level, custom replacement stays possible, and recommendation stays
compatible. Catalog memory and parsing work are bounded, publication is
atomic, and resource text cannot silently widen runtime authority.

Exact packaged-directory discovery, reparse-point rejection, and user-path
resolution remain responsibilities of a Windows definition-source adapter and
its composition owner. The Application catalog accepts only already-bounded
documents and never treats a source path as authority.

## Rejected alternatives

- Copying the source resources verbatim: rejected because higher-precedence
  Windows rules prohibit their unavailable runtime and platform commands.
- Keeping only compiled definitions: rejected because the project explicitly
  requires ported resources and later packaging/UI surfaces need the playbook
  documents.
- Keeping only external Markdown: rejected because a missing resource copy
  must not remove the builtin catalog.
- Applying a lexical prefix after loading: rejected because it can discard a
  mandatory product agent.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/AgentCatalog.swift`
  — SHA-256 `e5bb41f33660dba2a20fb97da0cb3df9147b6a4d40da0d2a760f650603ad3a48`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/AgentToolPack.swift`
  — SHA-256 `df7e04af91142c8f9d1c9f6a27acc6b7f4ddb44d9c4d1009df98035805ed19bd`
- `.forge-codex/instructions/plans/feature-parity-matrix.tsv` — AGENT-001 and AGENT-002
- `.forge-codex/instructions/plans/mcp-tool-parity.json`
- `.forge-codex/instructions/governance/PORT_TASK_CONTRACT.json`

