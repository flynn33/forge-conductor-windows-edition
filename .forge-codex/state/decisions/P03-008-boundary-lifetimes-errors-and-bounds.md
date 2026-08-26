# P03-008: Boundary Lifetimes, Errors, and Bounds

Status: Accepted

Date: 2026-08-25

## Context

Native processes, WinRT/COM, IPC, databases, telemetry, graphics, and tools introduce asynchronous work and native resources. Reliability requires explicit ownership and measurable bounds rather than cleanup conventions.

## Decision

- Each process composition root is the lifecycle owner. It stops admission, signals cancellation, revokes callbacks/event tokens, closes transports/listeners, joins std::jthread workers, flushes bounded durable work, releases view/graphics resources on their required apartment, destroys adapters, and finally releases the root.
- Every asynchronous operation has one lifetime owner, cancellation through std::stop_token or the applicable WinRT cancellation token, an explicit deadline, a bounded input/output queue, and an ordered shutdown path.
- Every HANDLE, HKEY, HINTERNET, SOCKET, COM/WinRT object, process/thread, Job Object, file mapping, pipe, timer, database object, ETW registration, graphics object, callback token, and event revoker is held by a typed RAII owner.
- Background threads use std::jthread; detached threads and ownership cycles in callbacks are prohibited. Long-lived callbacks capture a weak lifetime token and validate it before dispatch. Capturing raw this across an asynchronous boundary is prohibited.
- Member declaration order must support reverse-order destruction. A type whose callback or worker can outlive a dependency owns an explicit stop method and its destructor invokes that stop before dependent members are released.
- Locks are not held across I/O, callbacks, waits, or coroutine suspension.
- Domain and application boundaries return typed errors with stable code, message, retryability, and optional evidence ID. Infrastructure exceptions are caught before ABI, COM, process, named-pipe, HTTP, or MCP boundaries.
- Immutable snapshots cross thread/process boundaries. Telemetry delivery is a latest-value mailbox with capacity one. Every cache, history, diagnostic log, process capture, request, IPC frame, MCP line, and render queue is capped.
- The selected resource profile governs all numerical caps. Across profiles, MCP input is capped at 1,048,576 bytes, pipe frames at 2,097,152 bytes, tool stdout/stderr at 80,000/20,000 bytes, shell execution at 120 seconds, idle GUI+manager CPU at 2 percent, hidden GPU at 1 percent, and handle growth over one hour at 5 percent. Profile-specific memory, thread, history, sampling, log, and open-repository limits remain exactly those in resource-budgets.json.
- A budget change requires a superseding ADR and repeated identical-flow evidence; lower memory alone is not leak proof.

## Consequences

Shutdown, cancellation, timeout, overload, device loss, process failure, and boundary error behavior become deterministic and testable. Resource compliance remains a later runtime gate and is not inferred from this architecture decision.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Application wrappers own consumer-side lifetimes without changing sealed framework ownership semantics.

## Evidence

- .forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md - SHA-256 c7e06d649906d0854f1bc0d4e435219b2036268dae8734618f5dd97c4e42a36f
- .forge-codex/instructions/architecture/RESOURCE_BUDGETS.md - SHA-256 bca7eeff7df38854853399b3669334b0ff3d3c586aa0c5f977baadd6acef6daa
- .forge-codex/instructions/plans/resource-budgets.json - SHA-256 f80c5d57081d47b87ddb77027f843c912bcf3c3e558c7ade42b4db4828760965
- .forge-codex/instructions/architecture/TELEMETRY_AND_RENDERING.md - SHA-256 55c34d93e09e4afba8f148fa28412fabf31b497a956a87d913c948fd40b19270
