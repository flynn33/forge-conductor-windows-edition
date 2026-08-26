# P05-004: Interface-Issued Capability Values in Contracts

Status: Accepted

Date: 2026-08-25

## Context

P03 keeps the Contracts layer limited to abstract boundaries and places behavior in later application or Windows-adapter layers. P05 also requires filesystem and tool authorization to be represented by values that callers cannot construct or mutate. Keeping those values in Domain would either couple Domain to an issuing service or expose public constructors that turn authorization evidence into forgeable data.

## Decision

- Contracts may define a narrow class of immutable capability values whose private constructors are accessible only to the abstract interface that issues them.
- `WorkspaceAuthority` and `AuthorizedPath` are issued only through `IWorkspaceAuthority`. `AuthorizedToolCall` is issued only through `IToolAuthorizer` after binding the exact request metadata, canonical arguments, effect, project, authority identity and generation, and operation correlation.
- Capability values expose const observations, delete assignment, contain no ambient lookup, and do not own platform resources or service lifetimes.
- Domain remains dependency-free and carries only serializable request data and authority references. Platform implementations, policy decisions, composition, and global access remain outside Contracts.
- Sensitive service boundaries consume the issued capability together with the matching workspace authority and `OperationContext`; mutable audit records are not substitutes for authorization.
- A legitimately projectless tool call remains projectless in its capability. Every project-sensitive boundary must additionally call `matchesProject`; authority identity alone never supplies an absent project binding.
- The application tool handler is the single parser of canonical protocol arguments. Typed LM Studio and installer services are not MCP handlers and therefore do not invent a second JSON canonicalizer or compare ad-hoc MCP tool names; their boundary checks bind authority identity/generation, caller, correlation, explicit project, required intent, and operation effect to the already-parsed typed request. Operation routing and replay prevention remain responsibilities of the application handler/router that performs the one canonical parse.

## Consequences

The type system prevents ordinary callers from fabricating successful authorization by filling a public struct, while deterministic fakes can issue real capabilities through the same protected interface path. The exception does not authorize concrete services, service location, platform headers, or executable ownership in Contracts.

`AuthorizedToolCall` is proof of an authorized canonical tool request, not a license for a typed service to reinterpret its JSON. Composition must route the capability and the typed value produced from that same canonical request as one operation. P05 fakes verify this boundary split; later application phases own the concrete parser and one-use dispatch table.

## Rejected alternatives

- Public aggregate authorization records: rejected because they are forgeable and can be replayed with mismatched requests or authority generations.
- Domain-owned capability constructors: rejected because Domain must not depend on service boundaries and cannot prove that a policy service issued a value.
- Opaque booleans or mutable audit decisions: rejected because they lose request, caller, project, effect, correlation, and generation binding.

## Evidence

- `include/ForgeConductor/Contracts/AuthorityCapabilities.h`
- `include/ForgeConductor/Contracts/AuthorizedToolCall.h`
- `include/ForgeConductor/Contracts/IFileSystemServices.h`
- `include/ForgeConductor/Contracts/IToolServices.h`
- `include/ForgeConductor/Domain/ToolModels.h`
- `tests/Fakes/DeterministicWorkspaceAuthority.h`
- `tests/Fakes/ToolServiceFakes.h`
- `tests/Contracts/main.cpp`
