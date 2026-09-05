# Forge Conductor Windows Port — Qwen guided execution

## Absolute session-zero rule

Before reading memory, continuity, Git status, source, RAG, or a prior handoff, verify the exact current directory against `.forge-qwen/state/WORKSPACE_LOCK.json` and run `Assert-QwenWorkspace.ps1 -RequireCurrentDirectory`.

- Memory never selects the workspace.
- Continuity never grants workspace authority.
- The instruction package, `.forge-inputs`, `.forge-qwen/instructions`, macOS `.forge-codex`, Forsetti remediation directories, and any foreign `work/` directory are not the writable target.
- A `main` branch and uncommitted modifications are not evidence of the target repository.
- If the lock is absent or mismatched, emit `WRONG_WORKSPACE_BLOCKED` and perform no Git/source writes.
- After the lock passes, execute only `.forge-qwen/state/ACTIVE_ASSIGNMENT.json` and its generated step card.

This section overrides any lower instruction that says to retrieve memory or adopt a continuity workspace first.

## Mission

Build an installable Windows 11 Forge Conductor with complete macOS 0.9.0 feature parity, native object-oriented C++20, WinUI 3/C++/WinRT, Forsetti Framework Windows 0.2.0 compliance, project-scoped memory, autonomous continuity, bounded resource use, native installer, full tests, and independent validation.

## Instruction precedence

1. The owner request embodied by this package.
2. `governance/PORT_TASK_CONTRACT.json`.
3. Forsetti Agentic Edition and Windows profile.
4. Sealed Forsetti Framework public contracts.
5. Attached macOS source/tests for observable behavior.
6. This package’s architecture, plans, gates, and Qwen execution rules.
7. Local implementation preference.

A conflict is recorded in an ADR. Never weaken a higher rule silently.

## Qwen operating mode

- Execute **one microtask per fresh context**.
- Read only routed documents and exact source symbols needed for the microtask.
- Use RAG for recall, then verify every material claim by direct file read or command output.
- Keep a four-column fact table: `claim`, `status`, `evidence`, `action`; status is one of `VERIFIED`, `INFERRED`, `UNKNOWN`, `BLOCKED`.
- Never edit from an `INFERRED` or `UNKNOWN` claim when source inspection can resolve it.
- Default edit budget: at most 5 files and 400 changed lines in one microtask. A microtask may declare a different limit.
- Default tool budget: 18 calls. Checkpoint at 8 calls; handoff by 16 calls or 65% reported context; stop by 22 calls or 75% context.
- After each write, reread the edited region. After each command, inspect exit code and relevant output.
- At most two tool calls may be issued in parallel. Never launch a large speculative tool batch.
- A microtask is not complete without focused build/test evidence or a source-only acceptance explicitly declared by that microtask.

## Canonical state

Repository files are the only source of truth:

```text
.forge-qwen/state/run-state.json
.forge-qwen/state/microtasks.json
.forge-qwen/state/evidence-index.json
.forge-qwen/state/event-ledger.jsonl
.forge-qwen/state/decisions/
.forge-qwen/state/blockers/
.forge-qwen/state/handoffs/
```

Long-term memory and persistent memory are indexes, not authoritative state. On every session start, verify `WORKSPACE_LOCK.json` first. Retrieve memory only when the active assignment lists the corresponding capability. Persistent memory uses only the assignment's exact `memory_cursor_key`; long-term memory uses only a run-scoped ADR/blocker index. Ignore all generic, global, latest, or mismatched memory. Repository state wins.

## Tool rules

- **Filesystem:** primary method for precise reads/writes. Read before write; atomic replace; reread after write.
- **Shell:** build, test, Git, PowerShell, CMake, MSBuild, diagnostics. Set explicit working directory. Prefer one command per call. Capture exit code.
- **RAG-v1:** retrieve governing/source context with explicit symbols, paths, and expected terminology. It may locate evidence but cannot prove exact current code; verify with filesystem.
- **GitHub:** remote repository, issue, PR, branch, and review operations. Do not push, merge, close, or rewrite remote history before local gates pass. Never add automated-authorship attribution.
- **JavaScript sandbox:** pure JSON/schema/table/diff calculations only. It is not a production runtime and must not add Node to the Windows product.
- **Long-term memory:** only the compact cursor under the lock's exact run namespace. It never selects a path or task.
- **Persistent memory:** only run ID, workspace fingerprint, exact handoff pointer, decision IDs, blockers, and last validated commit under that namespace. Never store secrets or large source text.

Discover exact tool names and verify harmless calls in microtask `BOOT-TOOLS-001`. Record bindings in `.forge-qwen/state/tool-bindings.json`.

## Native implementation rules

- Production code: C++20 with MSVC.
- GUI: native WinUI 3 C++/WinRT and Windows App SDK.
- Build: CMake/CTest and MSBuild where WinUI/MSIX requires it.
- Persistence: Windows SDK Winsqlite3 behind RAII repositories.
- Rendering: one shared D3D11/D2D/DirectWrite resource graph; no per-gauge device/queue/swapchain.
- Telemetry: native documented Windows APIs, explicit unavailable/stale states, capacity-one latest-value mailbox.
- Processes: `CreateProcessW`, restricted handle inheritance, bounded pipes, Job Objects, cancellation, deadlines.
- Secrets: DPAPI CurrentUser, redacted logs/exports/handoffs.
- Manager/dashboard: current-user ownership, authenticated named pipe, loopback-only bounded HTTP/SSE.
- Installer: self-contained x64 and supported ARM64 MSIX plus native C++ setup bootstrapper.
- PowerShell is allowed for development/build/install automation and the opt-in shell tool.
- Python is forbidden in target source, scripts, tests, generators, build, packaging, installer, and runtime.
- Do not introduce Electron, Qt, Java, Boost, a .NET application layer, or an installed Node runtime.

## Forsetti rules

- Treat `ForsettiCore`, `ForsettiPlatform`, and `ForsettiHostTemplate` as sealed.
- Use public headers/interfaces only; never patch framework internals.
- Consumer modules depend on `ForsettiCore` only; product composition may use the public host/platform products.
- Use Windows profile 0.2.0 and manifest schema/template 1.1.
- Declare exact capabilities, runtime requirements, I/O, data isolation, UI contributions, and roles.
- Use interface-first OOP, constructor injection, `final` concrete classes, explicit RAII ownership, one-way dependencies, and no mutable global service state.
- Builder and Validator are separate fresh Qwen contexts.

## Feature preservation

- Preserve all seven GUI surfaces: Forge Rig, LM Studio MCP, Agents, Tools, Feed, Diagnostics, Manager.
- Preserve CLI/process modes, manager routes, data semantics, migration behavior, configuration, telemetry fields, and all 53 MCP tools.
- Keep legacy memory and continuity tools while implementing project-scoped memory and lifecycle continuity.
- Do not remove, rename, hide, stub, or silently weaken a feature.
- Unsupported external host session creation must use the Forge-native logical session host; never fake GUI-chat creation or automate private UI.

## Known defects not to port

- Unbounded telemetry-to-UI work or captured snapshots.
- Per-gauge native resources or independent recurring clocks.
- Unbounded histories, queues, logs, caches, subprocess output, requests, or retries.
- Blocking I/O on the UI thread.
- Detached/unowned tasks, timers, callbacks, pipes, processes, or delegates.
- Constant/duplicated telemetry mappings presented as real metrics.
- Leak claims based only on lower working set.

## Build-test-debug-fix

1. Capture exact failure command, cwd, versions, exit code, stdout/stderr, and artifact paths.
2. Classify the failure.
3. Reduce it to the smallest deterministic reproduction.
4. Identify the first product-owned failing boundary or retaining edge.
5. Make the smallest architecture-compliant repair.
6. Rerun the exact reproduction.
7. Run adjacent regression tests.
8. Record evidence and update the microtask.

Two failed attempts require diagnostic mode. Three no-progress attempts require a blocker record and selection of independent ready work. Never suppress tests, raise limits, add sleeps, or claim success without evidence.

## Session completion contract

Before ending every context:

1. update run/microtask state atomically;
2. append evidence and event ledger;
3. write a checksum-bearing handoff;
4. update long-term and persistent memory with only the compact cursor;
5. output exactly one final marker:

```text
FORGE_QWEN_RESULT {"microtask_id":"...","status":"passed|in_progress|blocked|failed","handoff":"relative/path","next_microtask":"...|null"}
```

Do not expose hidden reasoning. Store only concise evidence-based rationale and decisions.

## Completion prohibition

Do not claim completion because a build, window, tool list, or installer exists. Completion requires every hard gate, every parity row, all 53 tools, clean install lifecycle, resource qualification, independent Validator/Documentation/Release decisions, no Python, no prohibited stack, no secrets, and no automated-authorship attribution.