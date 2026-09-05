# Forsetti module boundaries

Use Windows profile 0.2.0 and manifest 1.1.

Recommended first-party modules:

| Module | Role | Owned data/capabilities |
|---|---|---|
| ForgeAppModule | only app/UI module | windows/navigation/preferences; toolbar/view/routing/telemetry/diagnostics |
| ForgeMemoryModule | service | project registry and project/legacy memory databases |
| ForgeMcpModule | service | stdio client sessions, descriptors, protocol state |
| ForgeContinuityModule | service | continuity database, checkpoints, operations |
| ForgeAgentHostModule | service | provider/session bindings and streams |
| ForgeManagerModule | service | manager settings, lease, loopback HTTP/SSE |
| ForgeTelemetryModule | service | bounded collector state/histories |
| ForgeIntegrationModule | service | LM Studio discovery/deployment/API state |

Modules depend on `ForsettiCore` only and never include/link/instantiate/open private storage from another module. Cross-module communication uses public Forsetti-mediated versioned events/services or bounded JSON contracts with correlation ID, deadline, caller capability context, and stable errors.

The app module owns all WinUI surfaces. Service modules do not inject arbitrary UI controls outside declared Forsetti contributions.
