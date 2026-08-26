# P13-001: Bounded Native Tool Boundaries

Status: Accepted

Date: 2026-08-26

## Context

The macOS 0.9.0 tool packs expose seventeen filesystem, Git, search, PDF, and
shell tools. Several source handlers have no input or traversal ceiling, and
the macOS authorization service permits selected reads outside the active
workspace. The Windows architecture instead requires explicit rooted authority,
bounded work, immutable capabilities, cancellation, deadlines, typed errors,
and no platform types in Domain or Contracts.

OWNER-001 and OWNER-002 defer a dedicated hardening campaign but retain rooted
workspace access, reparse containment, disabled-by-default shell execution,
process bounds, cancellation, deadlines, and RAII because those are functional
correctness boundaries for Forge-owned code.

## Decision

- P13 implements typed native services behind application-owned interfaces.
  P14 owns JSON parsing, MCP schemas, routing, audit receipts, and wire payloads.
- G13 qualifies those native service boundaries only. It is not evidence of
  end-to-end MCP registration, schema, dispatch, or response parity. The
  seventeen affected parity rows (`fs_read` through `shell_exec`, indexes 9-25)
  remain open until P14 binds and qualifies their MCP-facing behavior.
- Every path operation receives an immutable `AuthorizedPath`; process services
  additionally receive an immutable `WorkspaceAuthority`. Capability identity,
  project, caller, generation, root, grant, and denial bindings are checked
  before effects.
- Windows does not copy the macOS fallback that allows reads elsewhere in the
  interactive user's home. Every path must be under an explicit configured
  project root.
- Shell execution remains disabled unless the active immutable authority says
  it is enabled. Git does not imply unrestricted shell authority.
- Source-compatible hard ceilings are allocated to their owning boundary: P13
  enforces 2 MiB text files, 1,000 list entries, 500 glob matches, 200 search
  matches, 80,000 stdout bytes, 20,000 stderr bytes, a 30-second default shell
  timeout, and a 120-second shell maximum. P14 owns the observable `fs_read`
  offset/length aliases, 1-based line metadata, and 200-line default window
  because those are MCP request and response semantics layered over the bounded
  P13 byte read.
- Missing macOS ceilings are resolved as follows: file/PDF source text is capped
  at 2 MiB, generated PDF output at 16 MiB, Git log at 200 entries, one process
  argument at 4 KiB, Git add at 200 paths, and every native walker has explicit
  match, response-byte, depth, entry, and scanned-byte admission checks.
- Empty strings, embedded NUL, malformed UTF-8, overflow, cancellation,
  expiration, unsupported path forms, and work after shutdown fail with typed
  errors. Exceptions do not cross service boundaries.

## Consequences

The Windows services may reject extremely large inputs the macOS implementation
would attempt. This is a deliberate resolution of the higher-precedence
no-unbounded-work requirement, not a removed feature. MCP schemas and tool
descriptions in P14 must disclose the effective bounds while retaining the
canonical tool names and compatible argument fields.

A passing G13 therefore advances the implementation evidence for the seventeen
rows without changing their end-to-end parity status. P14 must update those rows
only after the MCP schemas, aliases, defaults, routing, metadata, and response
payloads have their own runtime evidence.

Dedicated adversarial fuzzing and defense-in-depth qualification remain deferred
under OWNER-002. G13 still exercises the negative cases needed to prove these
functional boundaries.

## Evidence

- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/instructions/plans/mcp-tool-parity.json`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/state/decisions/OWNER-001-defer-post-baseline-security-hardening.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- macOS `FilesystemToolPack.swift`, `GitToolPack.swift`, `SearchToolPack.swift`,
  `DocsToolPack.swift`, `ShellToolPack.swift`, and `ToolAuthorizationService.swift`
