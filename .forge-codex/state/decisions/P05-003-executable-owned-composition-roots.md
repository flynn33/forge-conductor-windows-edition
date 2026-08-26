# P05-003: Executable-Owned Composition Roots

Status: Accepted

Date: 2026-08-25

## Context

P05 requires both abstract interfaces and composition roots. P03-004 already limits production composition roots to executable hosts, while most Windows service implementations do not exist until later phases. Leaving every composition target as an interface placeholder would not establish ownership, shutdown, or construction semantics and would defer a literal P05 requirement.

## Decision

- Each production executable owns one final, non-copyable composition-root object. No library, Forsetti module, global accessor, or service container is a production composition root.
- The current CLI host constructs CliCompositionRoot in main. That root creates and uniquely owns the current CLI application object and delegates all command execution through it.
- Required dependencies are constructor arguments when they first exist. A host root may construct a dependency-free application object in P05; later phases expand the same explicit ownership graph as concrete services become available.
- ForgeConductor.Composition.Windows may provide assembly helpers later, but it does not own process lifetime and is not a substitute for an executable-owned root.
- The Forsetti module remains a thin adapter receiving an already-composed application lifecycle. It does not create or locate application services.
- CLI behavior and the G04 self-test remain stable. P05 evidence proves root ownership and construction shape; it does not claim sealed Forsetti activation succeeds.

## Consequences

The first production host now has a concrete ownership boundary without fabricating unfinished platform services. Later UI, manager, MCP, worker, and setup executables must establish their own roots under the same rule. The build and validator can reject a return to direct logic in main, hidden lookup, or an interface-only composition claim.

## Rejected alternatives

- Treating the ForgeConductor.Composition.Windows INTERFACE target as a completed composition root: rejected because it constructs and owns nothing.
- Creating the application graph inside ForgeConductorAppModule: rejected because it would invert the host/module boundary.
- Adding fake Windows services to a production root merely to satisfy P05: rejected because deterministic fakes belong to tests and would misrepresent runtime readiness.
- Deferring every root until P06: rejected because P05 explicitly requires composition roots.

## Evidence

- .forge-codex/state/decisions/P03-004-dependency-direction-and-composition-roots.md
- .forge-codex/instructions/plans/phases.json
- .forge-codex/instructions/architecture/TARGET_ARCHITECTURE.md
- src/Hosts/Cli/CliCompositionRoot.h
- src/Hosts/Cli/CliCompositionRoot.cpp
- src/Hosts/Cli/main.cpp
