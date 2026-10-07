# Forge Conductor Windows architecture
## Ownership
`WinUI views -> view models -> typed manager client -> per-user manager -> application services -> domain/contracts`.
Windows infrastructure implements filesystem, process, SQLite, HTTP and IPC contracts. Constructors receive dependencies;
views never spawn processes, read databases or rewrite LM Studio configuration directly.

The Manager owns project access, persistent memory/settings/policy, deployment, telemetry, and long-running services. Sessions are LM Studio chats. The primary stdio MCP composition owns the native Auto Continuity worker; closing Forge's GUI does not cancel LM Studio chat work.
The CLI remains a separate native executable with `serve` as the external stdio MCP entry point.
The session-host adapter is an execution component, not proof that a real provider session exists.

Normal desktop navigation is intentionally limited to Workspace, Rig, Continuity, Activity, and Settings. Workspace composes
project/provider identity, an ordered project-scoped instruction queue, optional CLU policy governance, the
per-project/provider automatic-continuity preference, and readiness. The older wizard and mission entry are not
separate operating surfaces.

Instruction intake is an inventory pipeline rather than a text-file admission gate. The Manager streams hashes,
persists revision identities and deterministic entry records, records directories/reparse points/read failures, and
derives bounded text only when the bytes can be represented safely. Protocol paging uses revision-bound cursors.
Initialization and `get_forge_status` report the bound project and package queue in order. `instruction_package.read` returns the selected row and paged entry content; opaque coverage remains explicit.

CLU is composed through `IProjectPolicyService` as optional, nonblocking development governance. Its state contains
the bound source/revision, coverage, findings, notification receipts, correction evidence, and redacted history.
Its MCP role has no continuity operations. Pending findings are attached to non-CLU tool results as `clu_governance_notifications`; the UI renders findings through Inspect findings and Activity. Selecting a policy folder immediately dispatches PolicyBind.

Tracked work is implemented by the native shell-service instance and existing process supervisor. The stdio adapter brokers shell-job, executable-process, reviewer, and verification operations to the persistent Manager when available; standalone work remains connector-owned. Job IDs, actual PID/creation identity, stdout/stderr files, and an integrity-checked receipt support readback/adoption without claiming an unknown interrupted exit status. Jobs have a 3,600-second maximum, two active slots, sixteen retained results in service memory, and up to thirty-two persisted jobs per project. Owner shutdown cancels/drains active workers. Account/profile values and Python UTF-8 defaults are explicitly constructed rather than inheriting arbitrary secrets.

`WindowsProjectWorkspaceAuthority` merges exact roots already in the owner's saved allowlist after checking existing directories and the full original capability. Binding is project-scoped, process-local, and mirrored to the Manager for brokered work. Native authorization retains namespace/traversal/overlap/reparse checks. `WindowsEvidenceService` hashes binary files through anchored read handles and serializes atomic durable project-log appends under a named cross-process mutex. The SHA-256 capture chain is unkeyed; complete replacement is detectable only against an independently retained head.

The reviewer route starts a new Manager-owned provider context and enforces read tools without executor chat history. Its dedicated `WindowsReviewerRunStore` preserves full tasks/reports and pending-call evidence in atomic SHA-256-verified receipts; it admits sixteen records, refuses full capacity, caps reports at 256 KiB and encoded receipts at 4 MiB, and marks interrupted work failed/unknown without replay. A caller-provided authorization reference is not proof of a human grant. The verification-environment route starts an owned Python process, limits selected requirements to qualified exact pins, refuses an existing destination, writes dependencies below a bound additional root, and publishes observed runtime/distribution versions only after success. Native host/provider inspection and GitHub reads expose facts or explicit unknown/failure states through existing injected services.

## Native chat rollover boundary

`McpServeCompositionRoot` starts `WindowsLMStudioChatContinuity` only for Primary and shuts it down with the MCP host. The worker receives authorized workspace binding from the adapter, reads selected native conversation statistics, and waits for completed-tool evidence before pausing generation. It requests a model-written packet through the existing `session_handoff` tool, then uses `WindowsLMStudioChatControl` to create a native successor and send the full packet. Persisted message/native-ID/integration checks precede availability; correlated native packet retrieval and a following successful Forge call precede `resumed`.

The three existing plugins remain the integration surface. Primary owns rollover, Fallback supplies the general catalog, and CLU supplies governance and shared bootstrap packet context. No renderer credential, token store, fourth plugin, or replacement run manager is introduced.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Build boundaries
Retain the current CMake targets for backend libraries, manager, CLI, session host and native tests.
Add a normal Microsoft WinUI C++/WinRT MSBuild project for XAML compilation and packaging.
Use explicit paths to the CMake output libraries through generated local build properties or a small staging manifest.
Do not hand-port XAML build machinery into thousands of new CMake rules. Match architecture, CRT and configuration.
The canonical build script orchestrates both toolchains and stages the actual runtime payload.

Use the existing Forsetti app-module composition where appropriate. A GUI entry point does not justify changing
sealed framework source or duplicating manager-owned services inside a singleton UI model.

## Product truth
Maintain a compact manager snapshot containing connection state, project identity, active run/session,
context usage/source/threshold, handoff status, tool availability and recent events.
It must distinguish disconnected, unavailable, pending, failed, paused, and completed. Do not display green
connected/autonomous status from the existence of a configuration file or a locally generated session ID.

Default local endpoint is LM Studio on 127.0.0.1:1234. The selected provider endpoint and model belong to
persistent configuration, not constants copied into separate pages. Manager IPC stays per-user/local.
Add support for an explicitly configured LAN model endpoint only through the same provider configuration/transport;
network placement does not change project identity, cancellation or data-safety behavior.

Forge writes an exact integer `timeout: 180000` to the Primary, Fallback, and CLU LM Studio registrations. The configuration codec, deployed bridge state, and host-synchronization acknowledgment validate the same value. LM Studio owns this outer request deadline; Forge `shell_exec` requests remain capped at 120 seconds. `FallbackPromoted` is derived health/status, not automatic call routing. A timed-out request is never replayed across roles because a mutating call can have an ambiguous completion state.

The MCP composition root resolves an explicit registered project or a current-directory startup default; authorized client workspace adoption can then select the actual bound project. It projects that authoritative binding into both the protocol-level `initialize.instructions` field and `get_forge_status`. The projection also reads the ordered instruction-package queue from project memory and the active policy source/revision from `IProjectPolicyService`. Handoff paths and continuity implicit roots are evidence carried by those subsystems, not substitutes for the registered project root. The LM Studio serve verifier treats missing or malformed bootstrap instructions as deployment drift.

R2 implements that snapshot as `ManagerTelemetrySnapshot`. The Manager joins its existing telemetry service with
status, settings, selected-run context, continuity identity, runtime diagnostics, project/tool catalogs, audit events,
and store-read health before encoding one authenticated pipe response. The WinUI process renders the typed values and
never opens telemetry collectors or recomputes retained-context headroom. Native PDH queries remain open in the Manager for per-logical CPU, GPU-engine, and physical-disk counters; Toolhelp/process APIs and volume APIs supply the heavier process and capacity tiers. The 250 ms base sampler refreshes GPU/disk near one second and processes/volumes near five seconds, publishes actual cadence and timestamps, and retains bounded histories. One window-owned timer requests fresh
snapshots; closing the window stops the timer and cancels in-flight UI work without stopping the Manager.

R3 and R4 extend the same pipe boundary with typed project, LM Studio, tool, operational, settings, and maintenance requests. Settings never opens configuration JSON or project databases. Record deletion reuses the project-bound `project_memory.forget` tool and refreshes authoritative Manager state; it does not reload or reset a workspace. Reset dispatch remains separate, validates an exact project/profile confirmation, invokes the existing transactional memory and continuity owners, and closes the affected repository generation so a stale owner cannot continue writing after a committed reset.

## Telemetry refresh layout

The 1.3.6 repair keeps the shared diagnostic `ManagerState` TextBlock collapsed and excludes `Action::Refresh` from generic connecting-progress text in `MainWindow::RunAction`. The existing background timer and telemetry values continue updating without allocating/removing the status row in the page stack. An actual WinUI in-process observer with an injected Manager verified stable Rig geometry; this is separate from installed-window visual acceptance.

## Changes deliberately avoided
No new general plugin bus, migration to a web GUI, remote orchestration server, replacement database,
new governance engine, multi-agent validator pipeline, or generalized cross-platform source rewrite.
The Mac Swift source is behavioral evidence only. No Swift binaries belong in the Windows build or installer.

## Product and package identity

`ForgeConductor::Domain::ProductIdentity` is the single native product-version source consumed by the Manager, CLI/MCP host, LM Studio transports, and diagnostics. CMake and packaging validate the same `1.3.16` value; the stable MSIX identity is `ForgeConductor.Windows` with numeric version `1.3.16.0`. The packaged GUI displays its actual installed package version in the navigation footer, operational context, and locally exported diagnostic context. Release staging records the commit, tree, configuration, architecture, and hashes of all four product executables before packaging. The Manager uses a dedicated process exit code for an unsupported newer central store so the GUI can explain the non-destructive failure and the explicit disposable `--alpha-root` compatibility option. Production view state retains stable registry names; isolated profiles derive deterministic per-profile names and validate a saved project ID against the authoritative Manager snapshot before use.

## Dedicated host workflows

The 1.3.16 catalog contains 104 Primary/Fallback tools. Native WebAccess, ArtifactDocument and DesktopArtifact services provide WinHTTP, stored-ZIP OOXML, Windows accessibility/input/capture, PNG drawing and image-codec previews. They use existing operation contexts, authority issuers and atomic storage. Dedicated Office documents build complete packages in memory before publication; production adds no Office/Python runtime or document-generation dependency.

The owner-selected filesystem mode distinguishes ordinary local-volume host access from workspace/configured-root access. The registered project remains the default directory for relative artifact destinations even when authorized host roots are drive roots. Path authorization, anchored native operations, reparse/namespace checks and OS ACLs continue to govern native file access. Host selection does not issue administrator rights.

Independent workers reuse managed provider inference through a separate receipt store, preserving existing specialist-session and reviewer contracts. Worker scopes freeze admitted roots, grants, denials, shell state, tool names and total lifetime; current issuer checks cannot broaden that snapshot. Worker tools exclude recursive spawn/scheduling, authority mutation, continuity control and governance approval. Recovery marks uncertain effects explicitly and retains actual errors/output.

Independent-worker and read-only reviewer services each admit up to sixteen simultaneous runs. A finished terminal thread is removed on subsequent start/status and joined outside the service mutex, leaving its durable result available by run ID. Completed receipts do not consume a lifetime admission slot; the unpublished 1.3.15 candidate removed the earlier reviewer sixteen-record lifetime gate, retained in 1.3.16. Terminal reviewer status reloads its durable receipt and reports actual evidence integrity and SHA-256. Per-file serialized receipts stay bounded to 4 MiB, including up to 256 KiB of raw output; existing results are not automatically removed, and ordinary storage exhaustion remains an actual failure.

The Manager owns one persistent schedule writer and dispatch thread. Storage callbacks supply separate authorized Read/Write/Create capabilities for one application-owned file. Schedules durably admit a unique run before starting provider inference, use a separate cancellation operation per firing, reconcile actual worker snapshots and prevent overlap for an owned task. UTC/interval trigger state restores after restart; uncertain previous effects require explicit run-now authorization. Startup reconciles before starting the loop, and shutdown stops the loop/cancels active workers before managed-run shutdown. Reference strings record task authorization context, while current authenticated project/owner authority supplies access.

The schedule notification callback returns an actual bounded submission receipt/error persisted separately as `last_notification`, without changing provider state. Native Windows toast uses existing SDK C++/WinRT projections and the installed package AUMID. A directly launched installed executable without process package identity resolves its immediate package directory through `PackageFamilyNameFromFullName` and verifies the exact registered path with `GetPackagePathByFullName`. Submission checks notification settings and preserves unconfirmed display; it adds no notification registration, settings mutation, network content or external dependency.

The capability receipt distinguishes these native workflows from external generative image providers and cloud email/calendar/chat accounts, which require separately configured connections or explicit authorized API credentials. Desktop/browser input reports submission and requires observation; HTTP status, provider state, schedule admission and packet persistence are not invented task-completion evidence. See [capability contracts](HOST-CAPABILITIES.md).

Continuity recovery retains its selected project root and the intersection of explicitly bound additional roots with the current issuer's authority. Binding asks the owning issuer to activate only a configured exact root; deliberate grant narrowing remains enforced. The MCP adapter validates process and tracked-shell cwd before forwarding to the Manager, matching synchronous shell authority. Supervised PowerShell uses a fixed UTF-8 stdin loader rather than passing the full script as a Windows process argument. Filesystem parent creation asks for directory creation rights without unrelated child-delete rights and keeps directory anchors pinned against rename/reparse substitution.

Independent reviewer requests retain a per-turn receive timeout in their immutable run identity and sealed receipt. The Responses transport owns asynchronous request callback state and buffers through `HANDLE_CLOSING`, serializes API calls with handle closing, and applies a monotonic budget from request submission through response-body completion, clips it to the transport operation deadline, and closes the exact owned request when that budget expires. It also checks deadline and cancellation after each WinHTTP completion so a late success cannot pass. Asynchronous admission does not impose the admitting MCP request’s deadline on the reviewer lifetime. Inline openings and optional text-only review share the existing read-only reviewer service and persistence; neither introduces a new tool or makes tool completion approve a governance gate.

Historical retained backend managed-run summaries carry an unkeyed SHA-256 consistency seal over the persisted summary and immutable run/project/client/task identities. The native run store recalculates it on read and reports verified, mismatch, or legacy-unsealed status. This detects accidental or partial alteration, but it is not an authenticity signature against an actor able to rewrite both the record and seal. The exact-project Events & Evidence projection reads this durable store independently of the live run cache and exposes response provenance, token counts, and redacted task/stored-output digests. The local export excludes task and model text. Record integrity and a completed provider response are not task-outcome verification; until an approved native task check is attached and executed, the result is explicitly unverified.

Ordinary startup selects `%LOCALAPPDATA%\Forge Conductor`. The MSIX manifest exempts only that directory from AppData write virtualization through `unvirtualizedResources`, keeping project, settings, memory, and continuity stores outside package-private data that Windows removes on uninstall. A focused manifest contract test rejects a broader exclusion or any different profile path. Central schema versions 3, 5, 6, 7, 8, 9, and 10 are handled explicitly; version 9 receives a guarded, backed-up C010 upgrade and unsupported future versions are refused without mutation. Newly recorded native MCP outcomes retain exact role/deployment provenance; earlier audit rows remain unqualified.

Historical phase records are retained under `docs/implementation/alpha-recovery/`; [Product status](STATUS.md) is authoritative for the current release.
