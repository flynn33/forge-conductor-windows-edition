# P03-004: Dependency Direction and Composition Roots

Status: Accepted

Date: 2026-08-25

## Context

The Windows port must preserve strict object orientation, test seams, Forsetti module isolation, and one-way dependencies while supporting several native process hosts.

## Decision

Dependencies point inward:

- ForgeConductor.Domain depends only on the C++20 standard library.
- ForgeConductor.Contracts may use Domain value types and contains abstract interfaces only.
- ForgeConductor.Application depends on Domain and Contracts and contains use cases and orchestration.
- Infrastructure.Windows, Mcp, Telemetry.Windows, Rendering.Windows, and Presentation.WinUI implement Contracts and may depend inward on Application, Contracts, and Domain. They do not use lateral dependencies for service lookup.
- ForgeConductor.ForsettiModule is a thin adapter from public ForsettiCore contracts to the application composition root and links only the public ForsettiCore target.
- Executable hosts are the outermost layer and are the only production composition roots. ForgeConductor.App composition may link the public ForsettiHostTemplate and ForsettiPlatform targets in addition to the app module and Forge libraries.

Every required dependency is passed through constructors. Concrete implementations are final unless an accepted extension ADR says otherwise. Mutable product state has no process-wide accessor, hidden service locator, or static owner. Independently registered Forsetti modules never depend directly on one another; framework-mediated public services/events are the only module communication mechanism.

The build graph and include graph must both obey the same direction. A link that is not represented by an allowed architectural dependency is rejected even when compilation succeeds.

## Consequences

Domain and application tests remain platform-independent, adapter replacement is deterministic, and process-specific lifetimes are visible in one composition root per executable.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Consumer composition may depend on their public targets and interfaces only.

## Evidence

- .forge-codex/instructions/architecture/TARGET_ARCHITECTURE.md - SHA-256 6271806ac05ec06b638b04d9cf1a9a0c6123f7a32bc4ecbc74227cc3c603a88d
- .forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md - SHA-256 c7e06d649906d0854f1bc0d4e435219b2036268dae8734618f5dd97c4e42a36f
- .forge-codex/instructions/governance/source/forsetti-windows/framework-policy.json - SHA-256 2331dffd25a17356457cc64f9fce8fd4b8a8e92f758cea29a13dec44cac9e150
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiCore/ForsettiRuntime.h - SHA-256 bd81a9866817624c85e41427be52c0d5ac5fda345083b10e81912d63a6e991fd
