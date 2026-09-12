# Required Windows product

## Definition
Deliver a Windows 11 x64 native desktop application using the existing object-oriented C++20 backend and WinUI 3/C++/WinRT GUI. The operator must install it, start it through Start, configure it and perform useful work without a developer shell. PowerShell build/install helpers are tooling, not the application's runtime or a substitute for settings. The signed MSIX is the Windows installer; a ZIP is only a distribution container.

The Manager owns running services and work when the GUI closes. The GUI uses typed services and feature-specific native view models with explicit composition and RAII; it is not a second scheduler. Extend existing interfaces instead of replacing stable backend modules or embedding orchestration in window event handlers. Proposed new service/control names in the phase plans are illustrative roles. Preserve Forsetti's public interface boundary and existing notices/dependencies.

## Required native destinations and actions

| Destination | Minimum real behavior |
|---|---|
| Rig | Live visual operational overview: resource trends, Manager/provider health, selected work and context headroom with drill-down. |
| LM Studio MCP | Inspect and deploy/repair primary and fallback registrations; show actual path/profile/connection state without destroying foreign entries. |
| Agents | Actual catalog/session/run identities, effective role/tool information, active state and supported controls. Do not imply multiple inference workers from playbook records. |
| Tools | Effective catalog and availability, recent outcomes, supported invocation/actions scoped to the exact project. |
| Feed | Real recent activity with project/run/tool identity, timestamp, outcome and navigable details. |
| Projects | Register/select projects and authorized roots, inspect durable binding/memory/continuity health, use relevant project operations. |
| Autonomy | Enter/select the task or supported package assignment, bind a project, start/pause/resume/stop runs, view progress and actionable failures. No new package-queue platform is required unless needed by an already supported workflow. |
| Continuity | Authoritative context gauge, reserves/threshold and measurement source; handoff state, predecessor/successor and acknowledgment/useful-resume evidence. |
| Runtimes | Actual runtime availability and job/process state, existing supported output/cancel controls and honest optional-runtime absence. |
| Provider | Endpoint/model/credential settings, discovery/test, inference state and actual response/token measurements. |
| Events & Evidence | Correlated events, gate/run receipts and bounded navigable log/details; no raw-dump-only primary interface. |
| Diagnostics | Health/failure/freshness overview and specific recovery actions with useful error details. |
| Manager | Actual connection/version/PID/profile status and explicit lifecycle controls. Closing the GUI is not a service-stop command. |
| Settings | Obvious access to persistent provider/context/shell/logging/telemetry/startup preferences and carefully scoped reset controls. |

Related capabilities may share native tabs or reusable controls; no required capability may be silently removed, represented solely by a generic Manager refresh panel, or hidden behind an unimplemented button.

## Non-negotiable behaviors
Project identity is exact and durable across GUI selection, runs, tool invocations and store operations. Native filesystem/search/Git/shell and project memory work through the existing real backend. MCP deployment preserves unrelated entries. Memory and settings persist. Reset is scoped and protects unrelated projects/source folders.

Ordinary managed inference must actually use LM Studio, correlate function calls and results, account for context and continue through a fresh successor at the effective context threshold. A transport object that only bootstraps continuity is not the ordinary run controller. Use real response IDs and returned acknowledgment; do not synthesize accepted provider sessions. No tool-call allowance or count/time session rollover is permitted. Meaningful per-request timeouts and output/context reserves remain.

Normal externally owned LM Studio desktop chats with MCP and Forge-managed API runs are different modes. Show “MCP connected” separately from “managed continuity active.” Do not claim an existing desktop chat was enrolled/reset merely because MCP tools are connected. Do not implement undocumented desktop-tab automation to disguise the distinction. [S16, E01]

## Acceptance and unsupported capabilities
Full **functional native telemetry visuals** are in Alpha, including supported resource collectors and real operational state. They are not deferred as “richer telemetry.” Optional specialized hardware metrics may be unavailable with a real capability reason; core supported CPU/RAM/process integration and required product state may not be waved away as unsupported.

A failing required workflow or existing-data compatibility issue stays explicit until repaired or the owner explicitly changes scope. This package does not authorize a model-generated waiver. A clean isolated install does not prove schema-9 migration. A fixture does not prove live rollover. A build does not prove an interactive GUI. A signed package does not prove installation.

## Deliberately outside this delivery
Do not add a new application language/runtime, a new broad security/governance engine, Mac filesystem qualification, all-hardware/ARM64 testing, a browser dashboard replacement, enterprise distribution service, Store submission, new auto-update infrastructure, elaborate GPU artwork or another installer technology. Preserve useful existing capabilities without expanding them into unrelated completion gates. Support the named Alpha behaviors and the prior telemetry/settings clarification in full.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
