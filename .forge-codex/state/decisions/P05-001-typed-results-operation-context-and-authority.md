# P05-001: Typed Results, Operation Context, and Authority-Bearing Contracts

Status: Accepted

Date: 2026-08-25

## Context

The supplied C++ templates are deliberately incomplete scaffolds. They represent successful void operations as `Result<void*>`, bind MCP directly to standard streams, give process execution only a relative timeout and global cancellation, expose only part of project memory, and omit required session-host query, recovery, and checksum acknowledgment operations. Copying those shapes would weaken the direct Windows ownership, security, and continuity requirements.

## Decision

- `Result<T>` has exactly one value or typed `Error`; `Result<void>` is a real specialization. `Result<void*>` is prohibited.
- Errors carry a stable code, redacted message, retryability, and optional evidence ID. Application-facing operations are `noexcept`; infrastructure exceptions are contained at their adapter boundary.
- Every cancellable or time-bounded operation receives an `OperationContext` containing a strong operation ID, correlation ID, absolute monotonic deadline, and `std::stop_token`.
- File and process operations also receive explicit immutable authority values issued only through `IWorkspaceAuthority`. Authority identifies the project, trusted roots, access intent, caller, grants, denials, and generation. A caller may narrow but never widen a resource policy or authority grant.
- `IProcessSupervisor` receives a process request, execution authority, and operation context; it supports operation-targeted cancellation as well as orderly global shutdown.
- MCP uses an injected `IMcpTransport`. The server never owns raw `std::istream` or `std::ostream` contracts, and clean EOF is distinct from framing, size, cancellation, and transport errors.
- LM Studio discovery/deployment, graphics device/rendering, and installer deployment are separate abstract interfaces with virtual destructors, typed results, operation-targeted cancellation where applicable, orderly shutdown, and bounded deterministic fakes.
- Side-effecting `project_memory.export`, `agent_run_status`, LM Studio deployment/activation, and installer execution receive explicit `WorkspaceAuthority`, an immutable `AuthorizedToolCall` issued by `IToolAuthorizer`, and the complete `OperationContext`. The capability binds the exact canonical call, effect, caller, project, correlation, and authority generation; mutable audit data cannot stand in for authorization.
- `IProjectMemoryService` exposes all twelve current project-memory operations—initialize, remember, rememberBatch, search, get, update, forget, listRecent, link, export, import, and status—plus explicit close, reset, and shutdown operations. Every post-initialize call carries an explicit project ID.
- `ISessionHostAdapter` exposes capability discovery, create, query by idempotency key, bootstrap, exact acknowledgment, session query, recovery, cancel, and shutdown. Host sessions bind project, continuity operation, predecessor, and idempotency identities. Bootstrap and acknowledgment reject cross-project or cross-operation substitution; a valid acknowledgment also matches handoff ID, successor logical session ID, adapter ID, and canonical handoff SHA-256.
- `IForgeApplicationLifecycle` returns typed `Result<void>` from `noexcept` start/stop operations. The Forsetti adapter remains the exception-containment and logging boundary.
- Abstract interfaces and deterministic fakes are not composition roots. Executable hosts remain the only production composition roots under P03-004.

## Consequences

Deadlines, cancellation, authorization, transport behavior, lifecycle failure, and continuity evidence become explicit and testable. Later adapters may translate to Win32, HTTP, named pipes, SQLite, or Forsetti only outside Domain and Contracts. The broader interfaces do not claim their later implementations are complete.

## Rejected alternatives

- Retaining `Result<void*>` because it appears in a template: rejected because it permits meaningless pointer states and evades the typed-void contract.
- Treating relative timeouts or `cancelAll()` as sufficient: rejected because concurrent operations require an absolute deadline and targeted cancellation.
- Treating a provider session ID alone as rollover success: rejected because it does not prove exact handoff delivery and acknowledgment.
- Adding a service container in P05: rejected because it would violate the accepted executable-host composition boundary.

## Evidence

- .forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/Result.hpp - SHA-256 9143dbae542788688f2d4b0d6532c2b38ef7029ec2933f7c24dd6e61d014cc8d
- .forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/IProjectMemoryService.hpp - SHA-256 b3bd7c00c796a4f0b7104f6cdbd9951bd99e7b4529e9325470c9f9c5123aac81
- .forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/ISessionHostAdapter.hpp - SHA-256 54d2141726dec238c683fbcfd809f499477eaae5b1225eed58bdebd01606e4a0
- .forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/IProcessSupervisor.hpp - SHA-256 7b0a378551c4d91d9e3cffd497e16e46115bdf3c87f918fef70984b613eff2da
- .forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/IMcpServer.hpp - SHA-256 ab09349c1f93d05fcb705ce3835a386af0c8d2c7a9269bd240a9ead1232a3a1b
- .forge-codex/instructions/architecture/CONTINUITY_AND_SESSION_HOST.md - SHA-256 c6819e807940e108eaf46c1c81d0f092fdff8e0b66d79ed570eedfdbe8de856a
- .forge-codex/state/decisions/P03-004-dependency-direction-and-composition-roots.md
- .forge-codex/state/decisions/P03-008-boundary-lifetimes-errors-and-bounds.md
- .forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md

