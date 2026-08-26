# P03-006: Host Activation, Service, and UI Composition

Status: Accepted

Date: 2026-08-25

## Context

The shipped WinUI host needs a production activation path that honors compatibility, entitlement, capability, service, and UI-surface policies. Demo, permissive, no-op, and immediate-dispatch providers are not production implementations.

## Decision

The GUI composition root constructs explicit production implementations for activation storage, entitlement evaluation, capability policy, diagnostics/logging, telemetry, Windows UI dispatch, view factories, overlay routing, module communication, and the application service graph. It supplies every dependency through ForsettiHostBootstrapConfiguration and registers exactly one ForgeConductorAppModule factory and descriptor.

Production composition expressly forbids AllowAllEntitlementProvider, AllowAllCapabilityPolicy, NoopTelemetryService, NoopOverlayRouter, ImmediateWindowsDispatcher, and DefaultForsettiPlatformServices::registerAll. Test doubles may use permissive behavior only inside deterministic test processes.

The host boots with ForsettiHostLaunchStrategy::explicitModuleIDs containing exactly com.forsetti.app.forge-conductor-windows. It does not use the default restoreOnly strategy or ActivateAllEligibleForDevelopment. Persisted activation is advisory recovery state and cannot activate a different module identity.

Activation follows discover, schema/profile validation, compatibility validation, entitlement evaluation, descriptor-to-manifest identity validation, scoped service construction, activation, and UI contribution publication. Every step fails closed. ForgeConductorAppModule implements the public app-module contract and delegates commands to application services.

UI contributions are immutable descriptors. Presentation.WinUI owns registered view factories and creates WinUI views from injected immutable view-model snapshots and commands. Views perform no filesystem, database, process, registry, networking, or secret operations. Toolbar items, view injections, overlay routes, and event publication are accepted only when the declared manifest capability and identifier match.

## Consequences

Production activation is policy-governed and testable without UI-side service lookup. An entitlement, compatibility, identity, capability, route, dispatcher, or factory mismatch prevents activation rather than degrading behavior silently.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Host composition uses IForsettiHostBootstrap, IForsettiHostController, public module registration, public service interfaces, and public view-factory contracts only.

## Evidence

- .forge-codex/instructions/governance/source/forsetti-windows/framework-policy.json - SHA-256 2331dffd25a17356457cc64f9fce8fd4b8a8e92f758cea29a13dec44cac9e150
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiHostTemplate/ForsettiHostController.h - SHA-256 e2e39b80a0fc65839d269f9d3fa0b4fec96474d5ed8a4b89171dcd75ab3d8b4c
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiHostTemplate/ForsettiHostBootstrap.h - SHA-256 83083eded3387541fbff847ebf3c26e54087f3c807f580ca7ae6710a5b6fc965
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiCore/ModuleRegistration.h - SHA-256 4317f66e9f96c97a4ca641f8cb76f6cfd27047716d057b554557bcfa76a8c1d3
- .forge-codex/instructions/architecture/WINDOWS_NATIVE_UI.md - SHA-256 6b8440e67f857692d716a1c4ebc2fff8da5366874c7e53e572cd087bb1f72c49
