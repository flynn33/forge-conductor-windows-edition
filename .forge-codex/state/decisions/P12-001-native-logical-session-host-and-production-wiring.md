# P12-001: Native Logical Session Host and Production Wiring

Status: Accepted

Date: 2026-08-26

## Context

The macOS source registers `forge.native-session-host`, but no production GUI,
CLI, Manager, or context-budget path resolves that adapter and invokes the
durable coordinator. Its default transport also returns a synthetic
acknowledgement without proving that a successor consumed the handoff. G12
requires a successor to be created, bootstrapped, acknowledged, sealed, and
resumed without operator action.

## Decision

- `ContinuityAutomation final` is the application-owned trigger. One budget or
  progress observation synchronously invokes the existing durable checkpoint,
  rollover, and idempotent resume operations. The caller never invokes the
  coordinator between those steps.
- `ForgeNativeSessionHostAdapter final` is the concrete adapter. It depends on
  typed ledger, provider transport, canonical handoff codec, UUID, and clock
  interfaces supplied by a composition root. It uses no global registry or
  service locator.
- Provider creation is replayed only through a transport contract that requires
  the supplied idempotency key to be honored. Project, operation, predecessor,
  logical session, and key bindings are checked before a replay is returned.
- Explicit resume replays the exact bootstrap through the adapter before the
  active pointer is published or reaffirmed. The native adapter first probes a
  durably Ready provider, recreates a missing provider only with the same
  idempotency key and physical ID, and performs a distinct bootstrap effect only
  when transport state actually requires reconstruction.
- The bootstrap transport receives the exact canonical handoff. The Forge-owned
  logical transport decodes it, validates project and operation identity, and
  schedules the first bounded next action before returning an acknowledgement.
  Therefore acknowledgement means the logical successor accepted continuation
  state; it is not a fabricated claim about another application's GUI.
- The generic adapter remains transport-neutral. A Windows WinHTTP transport is
  used for configured local-model providers, and the Forge logical transport is
  a supported native fallback. P15 remains responsible for LM Studio discovery
  and deployment.
- The concrete core is exposed through a versioned, `noexcept` DLL factory ABI
  and a separately owned session-host executable. Manager IPC is added in P16,
  so P12 does not introduce a competing transport.

## Consequences

The Windows product has one explicit automation-to-coordinator-to-host chain.
It can recover idempotent provider effects after process loss and cannot report
native parity from registration alone. External hosts without supported APIs
remain on the truthful manual/memory-only path; no GUI automation is used to
simulate a session API.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContinuityCoordinator.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContinuityAutomation.swift`
- `architecture/PROCESS_MODEL.md`
- `architecture/COMPONENT_MODEL.md`
- `.forge-codex/instructions/plans/gates.json`
