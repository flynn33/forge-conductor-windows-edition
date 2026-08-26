# P03-005: Manifest Capabilities and Runtime Requirements

Status: Accepted

Date: 2026-08-25

## Context

The selected Windows profile requires every schema 1.1 field, and activation must be limited to declared capabilities and runtime requirements. The bootstrap manifest carried template identity values and is superseded by this product decision.

## Decision

The canonical manifest is src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests/ForgeConductorAppModule.json. The top-level manifests/ForsettiManifests/forge-conductor.json file is a byte-equivalent packaging mirror and is not a second module registration.

Canonical identity and compatibility:

- schemaVersion and manifestTemplateVersion: 1.1
- moduleID: com.forsetti.app.forge-conductor-windows
- moduleVersion: 0.9.0
- moduleType: app
- supportedPlatforms: Windows
- minForsettiVersion and maxForsettiVersion: 0.2.0
- iapProductID: null
- entryPoint: ForgeConductorAppModule
- defaultModuleRole: ui

Capabilities are declared in this canonical order: networking, storage, secure_storage, file_export, telemetry, routing_overlay, toolbar_items, view_injection, event_publishing, shared_database, diagnostics, api, security. crypto_utilities is deliberately not requested because the supplied C++/schema/profile contracts disagree about its I/O representation.

The nine required I/O declarations are, in order:

1. forge.network.client - networking/read_write
2. forge.settings.storage - storage/read_write
3. forge.secrets.secure-storage - secure_storage/read_write
4. forge.data.export - file_export/write
5. forge.telemetry.events - telemetry/emit
6. forge.database.access - shared_database/read_write
7. forge.diagnostics.events - diagnostics/emit
8. forge.api.invoke - api/execute
9. forge.security.authorization - security/execute

The UI contract declares control scheme forge-conductor.windows.controls.v1, layout forge-conductor.windows.shell.v1, appShell, the settings toolbar item, settings/reset/purge routes, and the shell/settings/reset/purge view factories. Data isolation is private_to_module with the five stores forge-conductor.central, forge-conductor.project, forge-conductor.configuration, forge-conductor.manager-state, and forge-conductor.session-ledger.

A native contract test must compare the registered module descriptor and UI contribution identifiers against the parsed canonical manifest, and validation must prove the packaging mirror is byte-equivalent. Any capability use not covered by this exact manifest fails closed and requires a superseding ADR plus manifest validation.

## Consequences

Runtime access is auditable before activation, product identity matches the 0.9.0 source contract, and packaging consumes one canonical module descriptor.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Manifest mismatches are corrected in the application manifest or application adapters, never in the framework loader.

## Evidence

- .forge-codex/instructions/governance/PORT_TASK_CONTRACT.json - SHA-256 cd2714169189e274eb51c3b0bb666b2b59a68042f4c01b0a60f7506ab6ad0547
- .forge-codex/instructions/governance/schemas/module-manifest-1.1.schema.json - SHA-256 0d768364790214335ca3b1b585cfd3be2af5e81313b771463f367a61bd0ed90d
- .forge-codex/instructions/governance/source/forsetti-agentic/editions/windows/forsetti-windows-0.2.0.profile.json - SHA-256 4a65c4c986da951cddd2376de39ebc87f1186b21c147c53c122ce0c10ac19c8a
- .forge-inputs/macos/Forge-Conductor-MacOS-main/README.md - SHA-256 a229aca6a04fa4c0e0493a1bbd736fc1691c3a14648293dcc550690ab03eaa7e
