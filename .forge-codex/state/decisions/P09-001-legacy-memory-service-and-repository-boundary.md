# P09-001: Legacy Memory Service and Repository Boundary

Status: Accepted

Date: 2026-08-26

## Context

P09 must preserve the five global legacy-memory capabilities (`memory_set`,
`memory_get`, `memory_list`, `memory_delete`, and `memory_search`) while keeping
transport, application policy, and Winsqlite ownership in their proper layers.
The macOS implementation combines argument aliases, response dictionaries,
application dispatch, and direct `SQLiteStore` calls in one tool pack. That
shape is behavioral evidence, but it conflicts with the Windows interface-first
and one-way-dependency requirements.

P07 already owns the central Winsqlite connection, migration, transaction,
cancellation, deadline, and native-resource machinery. Recreating any of that
inside a memory tool or exposing a database handle through a public contract
would split ownership and violate the sealed persistence boundary.

## Decision

- P09 implements typed legacy-memory behavior only. The application-facing
  `ILegacyMemoryService` exposes set, get, list, delete, search, and the explicit
  destructive purge operation. A narrow `ILegacyMemoryRepository` supplies the
  corresponding central-store persistence operations. Both contracts use only
  domain values, `OperationContext`, and typed `Result` values.
- A final application service owns validation, normalization, compatibility
  policy, and service-level result semantics. It depends on the repository and
  Unicode-canonicalizer abstractions through constructor injection. A final
  Windows repository implements SQL semantics over the P07 central-database
  kernel and owns no process-global state. The final Windows Unicode adapter is
  an infrastructure service; no Windows NLS API leaks into Domain or Contracts.
- The composition root owns the service, repository, and central-database
  lifetimes explicitly. Shutdown is idempotent and prevents new operations
  before waiting for or releasing the owned database lease. No background
  worker, timer, callback backlog, service locator, singleton, or hidden second
  SQLite connection is introduced for legacy memory.
- Every repository call receives the caller's cancellation token and deadline.
  P07's bounded busy and progress callbacks remain authoritative. Exceptions
  are caught at concrete method boundaries and converted to typed errors before
  crossing an application, ABI, process, or later MCP boundary.
- Domain memory objects contain no Win32, WinRT, Winsqlite, JSON, or transport
  types. SQL statements, `sqlite3` objects, transactions, and row conversion
  remain private to `ForgeConductor.Persistence.Windows`.
- P14 owns exact MCP registration, JSON Schemas, input aliases, compatibility
  envelopes, and wire-error mapping for all five names. In particular, P14
  preserves `body` plus the accepted `content` and `value` aliases, and `query`
  plus the accepted `q` and `pattern` aliases. P09 does not register tools or
  claim wire-schema completion.
- P14 also owns tool-call audit serialization. It must redact or omit caller
  values supplied as `body`, `content`, or `value`; P09 never asks the
  persistence layer to inspect an MCP argument dictionary.
- Agent and continuity phases may use the same typed service/repository to
  maintain their legacy pointer keys. They do not gain direct SQL access and do
  not create a dependency between independently registered Forsetti modules.

## Consequences

The Windows implementation can preserve legacy behavior without coupling the
domain to Winsqlite or pre-implementing P14 transport. There is one lifetime
owner and one transactional authority for the central `memory_notes` table.
Later hosts can expose identical MCP behavior by adapting typed results rather
than duplicating persistence or validation logic.

P09 tests prove the service and repository contracts directly. Exact JSON
descriptor and envelope parity remains a named P14 obligation and cannot be
claimed from P09's typed tests alone.

## Evidence basis and source hashes

- `916df67b5ddd32538732cbe82e9c1382e1ddcf2817723055368e54298e325ee7`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift`
- `511d2b1f438ffd223263fff4ed6899cfddb0d7fc768212ee575061ca60141765`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/MCP/MCPServer.swift`
- `a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
- `c7e06d649906d0854f1bc0d4e435219b2036268dae8734618f5dd97c4e42a36f`
  — `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `5f071b8f8178f6c4101698c0e5921f5141823e9be1e1191f7ff781411a44b217`
  — `.forge-codex/instructions/plans/mcp-tool-parity.json` (indices 26 through 30)

