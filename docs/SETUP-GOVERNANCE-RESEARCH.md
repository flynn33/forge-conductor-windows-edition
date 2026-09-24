# Easier setup, policy adoption, and release evidence

Research date: September 23, 2026. Product baseline: `a3d6092afd737f52f0e720ee1f0b249da63ff969` (1.1.43). This document separates findings, implemented first fixes, and proposed work. It is not a release acceptance record.

Implementation update: version 1.2.1 adds the missing automatic plugin deployment and synchronization, visible persistent setup entry and ordered policy/task sections to the preparation path and native policy intake/review gate. The 1.2.0 setup path omitted plugin deployment and is superseded. See [the current user guide](USER-GUIDE.md). The recommendations below retain their original research scope; semantic validator parity and operating-system command sandboxing are not implied by import.

## Recommended product change

Make the default starting experience **Choose your folder → Prepare automatically → Describe your task**. Keep one visible next action, preserve progress, and put optional instructions, memory, and technical settings behind expandable sections. Setup should perform preparation instead of sending the user around the application to do it manually.

Add a separate **Development policy** feature with repository intake, a pinned revision, explicit adoption, machine-checkable rules, review obligations, and enforcement at Manager execution boundaries. The existing instruction package feature is useful guidance but is not a substitute for governance enforcement.

Make release readiness a report backed by native results and test artifacts. A build, a model response, or a passing unit suite must never turn the whole product green.

## Findings from the current implementation

| Finding | Source | Consequence |
|---|---|---|
| Nine persisted guide steps span Projects, Provider, and Autonomy. | `MainWindow.xaml.cpp`, `RenderGuidedMode`, guide event handlers | Users must understand internal concepts before accomplishing their first task. |
| The opening folder button only changed page and step. | `GuidedModePrimaryClicked` | A button named Choose my project folder did not actually open a picker. Fixed in this working tree. |
| The guide can finish without starting a run. | `GuidedModeSecondaryClicked` | Guide completion is not readiness verification. The final title now describes guide completion rather than claiming full setup readiness. |
| Native tools default to off. | `RunAllowNativeTools` | Users can receive a textual answer without actual development work. Keep explicit authorization, but explain the practical consequence before Start. |
| Provider discovery already exists. | `ProviderModels` action and `ManagerConnection` | Reuse it in automatic preparation; do not build a second discovery implementation. |
| Instruction import accepts local folders, recursively scans supported text, and caps input at 32 files / 212 KiB. | `ManagerRequestDispatcher::scanInstructionPackage` | A full policy repository with wiki and archive copies is not a valid intake strategy. |
| Run instructions are assembled within a fixed task budget; files that do not fit may be omitted. | `managedRunTaskWithInstructions` | Mandatory policy reading cannot be inferred from activation or prompt assembly. |
| Generic instructions become part of the provider task. | `managedRunTaskWithInstructions` | Prose inclusion does not enforce review, allowed dependencies, or release authority. |
| Existing release status names unfinished installed, accessibility, and continuity checks. | `docs/STATUS.md` | The repository already distinguishes implementation from qualification; retain that distinction. |
| A real-process lifecycle test hard-coded port 7788. | `WindowsManagerCompositionLifecycleTests.cpp` | Running tests while the installed application owns that port caused a failure. The fixture now chooses an ephemeral loopback port and writes only its isolated configuration. |

## Research and its application

Microsoft recommends contextual, easily navigated help that follows the application's accessibility and usability standards. Apply this through offline articles, field-level explanations, actionable errors, and links to the relevant control. Keep help available after onboarding. [Microsoft: in-app help](https://learn.microsoft.com/en-us/windows/apps/design/in-app-help/guidelines-for-app-help)

Nielsen Norman Group's onboarding guidance supports teaching in the context of a task. Use a short first-project workflow and explanations at the moment a decision is needed, rather than requiring a tutorial before work begins. [Onboarding and contextual help](https://www.nngroup.com/articles/onboarding-tutorials/)

Progressive disclosure supports moving infrequent, complex choices away from the primary path. Preserve advanced controls while making folder, readiness, and task the default view. This is a design recommendation, not evidence that this specific interface has passed a usability study. [Progressive disclosure](https://www.nngroup.com/articles/progressive-disclosure/)

LM Studio exposes supported server, model-listing, and model-loading operations. A native adapter can automate discovery and preparation using the supported CLI or HTTP interface. Detect installed versions and capabilities; do not assume every endpoint exists. Model installation and download remain explicit choices because they consume resources and may involve license decisions. [LM Studio CLI](https://lmstudio.ai/docs/cli), [REST quickstart](https://lmstudio.ai/docs/developer/rest/quickstart)

## Proposed first-project workflow

1. **Choose a folder.** Open the picker immediately. Support creating a folder there. Derive a friendly display name from the folder, allow editing it, and explain that registration grants access to this folder. Detect an already registered folder and reuse its project identity.
2. **Prepare automatically.** Attach to/start the Manager, register and read back the project, check storage, discover LM Studio, inspect its local server and loaded models, and validate the selected model's required capabilities. Show each check as Checking, Ready, Needs action, or Failed with a reason. Never convert an unavailable observation into Ready.
3. **Describe the task.** Keep the selected project and model visible. Explain text-only versus tool-enabled work in plain language. Start only when the required readiness snapshot still matches the project, provider configuration, and policy revision. Show the actual run identifier and state after Manager readback.

Optional **Development policy** and **Project context** sections belong before the first run, but do not require empty notes or placeholder records. Once a policy is adopted, its required checks are mandatory rather than optional setup steps.

Automatic safe operations: inspection, existing project matching, display-name suggestion, connection checks, capability detection, progress persistence, and retry after a transient read failure. User decisions: authorized folder, policy adoption/update, model download, tool permission, destructive reset, and release acceptance. Avoid repeated decisions when the existing scope already authorizes the operation.

The setup service must live behind the Manager API, not only in button handlers. Persist a resumable preparation record. Derive readiness from current evidence, invalidate affected checks when inputs change, and retain completed unrelated checks. GUI closure or cancellation must not leave duplicate projects or half-activated policies.

Proposed acceptance targets: a first project in three primary screens, no manually typed UUIDs or JSON, an already-ready host reaches the task form without visiting Settings, and every failed check offers a specific recovery action. Measure actual completion time and error rate with a fresh-profile installed walkthrough; these targets are not measured results yet.

## Development policy intake and enforcement

The reviewed Raven Forge Development source is version **0.6.2**, commit **`ed0028a46bac9c5b92876a6ad6589ca421fd9499`**. A research checkout was obtained in ignored `out/research/raven-forge-development`. It is reference material, not an application dependency or an adopted project contract.

The repository distinguishes pinned policy adoption, project records, primary-system contracts, separate governance contracts, and contract/preflight/delivery validation. Its validator reports specific structural and repository conditions; it does not replace semantic review or native behavior tests. Preserve those boundaries in the product. [Pinned policy source](https://github.com/flynn33/raven-forge-development/tree/ed0028a46bac9c5b92876a6ad6589ca421fd9499)

### User-facing flow

**Projects → Development policy → Import repository** accepts a repository URL or an existing local checkout. For the supplied URL, resolve the selected branch/tag to an immutable commit, display the source, version, selected active documents, and checks, then offer **Adopt for this project**. Default to the existing adopted revision on resume. An update is a separate action showing a diff and the checks it invalidates.

Intake states are Downloading, Verifying, Ready to review, Adopted, Needs review, and Failed. A failed download must leave the previously adopted policy intact. Support cancellation and retries. A private repository needs an explicitly configured authenticated route; never place access tokens in source URLs, logs, or records.

### Repository processing

- Keep the snapshot in Manager-owned storage, separate from the product repository and ordinary project memory edits.
- Record normalized URL, immutable commit, document paths, content hashes, intake time, selected profile identifiers, and source roles. Reject traversal, links/reparse escapes, oversized input, malformed encodings, incomplete manifests, and inconsistent revisions.
- Read the repository's active entry points and mandatory references. For Raven Forge, begin with its README and Project Start reading list. Exclude `archive/` and duplicated wiki content from active authority unless explicitly required. Show excluded material and unresolved references.
- Preserve complete mandatory source content. Chunk and index it for retrieval rather than silently dropping files to fit a prompt. Record delivery/read coverage separately from understanding and review. An access receipt cannot prove comprehension.
- Never run repository scripts just because they were imported. The product uses native C++20 services. The policy repository's Python validator is not a production runtime dependency; native enforcement needs defined rule parity and test fixtures, or an explicitly separate externally managed validator integration.

### Enforcement model

| Requirement | Enforcement mechanism | Evidence |
|---|---|---|
| Exact policy revision and complete required documents | Native manifest/hash verification; fail closed on missing or changed mandatory input | Intake manifest, content hashes, failure reason |
| Correct project and work boundary | Manager authorization tied to project identity and authorized roots | Project and operation IDs, allow/deny decision |
| Required preflight before dependent edits | Native gate bound to current source and policy identities | Check ID, inputs, result, timestamp |
| Dependency and prohibited-import rules | Native repository/dependency checks against an explicit ruleset | Findings with paths and selected profile |
| Review and approval requirements | Separate review records and authorized decisions | Reviewer identity, scope, evidence binding; no invented approval |
| Narrative standards and design intent | Review-required obligations, never a fabricated automatic pass | Review conclusion and supporting evidence |
| Delivery qualification | Required checks for the exact candidate and input identities | Build/test/package hashes and coverage report |

Enforce at **all** relevant entry points: managed runs, direct native tool invocation, MCP, shell/Git dispatch, continuation, and release/export actions. A UI-only disabled button is insufficient. Unknown mandatory rules remain unresolved. Do not label unsupported requirements Enforced.

Shell access is particularly important: arbitrary shell execution can bypass individual filesystem or Git checks. Either constrain it with the same verifiable boundary or require a policy-authorized execution profile. Do not advertise comprehensive enforcement while leaving an unrestricted alternate path.

A policy change must not silently change a running task. Pin its adopted revision for the run; require explicit transition/revalidation when an update must affect ongoing work. Invalidating preflight after source changes must be selective but deterministic. Retain decision history and the previous adopted revision for recovery.

## Knowledge base

The initial implementation adds 20 searchable offline articles within Guided setup, covering first projects, new folders, existing projects, Manager behavior, provider/model recovery, tool permissions, instructions and their limits, memory, run controls, continuity, MCP, telemetry, evidence, settings, and problem reports.

Next, turn this into a versioned help catalog with article IDs, keywords, applicability, related controls, error-code mappings, and review dates. Add a dedicated Help navigation entry and contextual links from failures. Keep application help separate from imported project policy. Policy search results should show their source revision; application help should show the product version it describes.

Help acceptance includes offline operation, keyboard navigation, screen-reader labels, High Contrast, search with no results, narrow windows, long text, and links that take the user to the correct control without losing project selection. Do not require model inference just to explain a button or an error.

## Implementation order

| Order | Deliverable | Required completion evidence |
|---|---|---|
| 1 | Manager setup coordinator and three-stage default UI | Fresh-profile first project, interruption/retry, existing-project reuse, provider failures |
| 2 | Policy source service, snapshot store, and import/adoption UI | Supplied Raven repository, local/private/offline cases, pinning, exclusions, changed revision, failed-download rollback |
| 3 | Native policy rule model and common authorization gates | Denial through every entry point, source drift, unsupported rules, approval binding, shell bypass attempts |
| 4 | Contextual knowledge catalog and recovery actions | Search, offline rendering, accessibility, correct navigation and preserved state |
| 5 | Complete feature/control coverage and candidate qualification | Exact-source automated report, installed end-to-end evidence, signed lifecycle, owner acceptance |

The small fixes in this change are groundwork. The Manager setup coordinator and policy importer/enforcement layer are **not implemented by this research change**. They must not be represented as shipping capabilities.

## Verification performed and remaining proof

The initial Release run had **150/151 passing** tests. `ForgeConductor.Manager.CompositionLifecycleTests` refused to use the occupied production dashboard port. After isolating that port and its fixture configuration, the full Release suite passed **151/151** in **29.91 seconds**. Logs are in `out/baseline-validation-20260923.log` and `out/setup-governance-validation-20260923.log`.

The native app and sibling services built successfully after the initial help and button changes. Final consolidated verification is recorded separately by `scripts/validation/Invoke-ReadinessEvidence.ps1`. That command builds Release, runs every CTest entry, runs static gates, checks package persistence, and writes logs and a machine-readable summary. It deliberately reports `shippable: false` because these checks do not cover the full acceptance boundary.

### Coverage required before shipping

| Surface | Existing automated evidence to retain | Additional proof required |
|---|---|---|
| First launch and setup | Startup, project registry, Manager connection tests | Installed clean-profile folder → ready provider → actual task; cancellation/restart/no duplicates |
| Policy import/governance | No complete feature exists yet | All intake/enforcement cases above, supplied repository acceptance, review and update flows |
| Autonomy | ManagedRunService tests | Real provider with native tool output, pause/resume/stop, GUI close/reopen, exact-project reattach |
| Project and legacy memory | Repository, application, archive, migration tests | Installed CRUD/search/import/export, isolation, corruption and recovery |
| All 57 tools | MCP tool contract matrix plus native service integration tests | Map every current catalog entry to success, invalid input, denied scope, cancellation, and actual output evidence; record legitimate platform-specific limitations |
| MCP roles | Catalog, transport, router, LM Studio deployment tests | Real primary/fallback/CLU registration and activation, foreign-entry preservation, desktop-source handoff |
| Continuity | Coordinator, repository, native-session tests | Provider acknowledgment, fencing, useful successor work, recovery and retained context |
| Telemetry | CPU/RAM collectors and presentation tests | Independent CPU/RAM/GPU/disk/process comparisons, unavailable counters, stale/disconnected views, histories and sampling timestamps |
| Settings and maintenance | Config, DPAPI, startup and maintenance tests | Every save/revert/restart/reset control and scoped confirmation; persistence after reopening |
| GUI controls | Scheduler/presentation tests | Inventory all XAML controls, map events to expected effects, then exercise keyboard, focus, screen reader, DPI, High Contrast, resize and error states |
| Data compatibility | Immutable migration and backup/integrity tests | Disposable copies of supported released stores; no live database mutation |
| Packaging/lifecycle | Package persistence/static checks | Clean-source signed MSIX, payload verification, installed upgrade/repair/uninstall/reinstall and retained data; replace historical Alpha-only lifecycle assumptions |

For each acceptance case record the case ID, source identity, binary hash, profile/fixture, steps, expected and actual result, timestamps, exit/result code, and evidence paths. Count **passed, failed, not run, and not applicable** separately. A missing case or unsupported measurement remains visible. Final release acceptance is a separate decision after those records are reviewed.
