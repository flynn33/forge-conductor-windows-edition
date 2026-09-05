# P00-B facts — Read governing rules and task contract

Run: 1df367e1-3584-41e9-b2cb-8fbbe721addf
Fingerprint: 98601f23820832b5542bc29ed245d0d9b984cc8435af37403ba1af8ac8eafdb7
Branch: forge-windows-port-1df367e1
Role: builder | Phase: P00 | Microtask: P00-B

## VERIFIED

- Workspace lock verified in prior session (step-card steps 1–6): current directory printed, WORKSPACE_LOCK.json read, Assert-QwenWorkspace.ps1 -RequireCurrentDirectory passed with WORKSPACE_LOCK_VERIFIED; assignment mission/run/fingerprint/target/branch match the lock.
- ACTIVE_ASSIGNMENT.json microtask is P00-B ("Read governing rules and task contract"), role builder, phase P00, depends on P00-A; budgets: 5 files / 400 lines / 18 tool calls / 3 attempts; required capabilities include rag_v1 and persistent_memory.
- All eight routed documents read directly this session (direct filesystem verification per RAG_PLAYBOOK.md):
  - .forge-qwen/instructions/AGENTS.md (read in prior session, confirmed by handoff)
  - .forge-qwen/instructions/qwen/MICROTASK_PROTOCOL.md — one objective; step card order; default budgets 5 files / 400 lines / 18 tool calls; end-of-task state/evidence/handoff/cursor/session-result then marker.
  - .forge-qwen/instructions/qwen/TOOL_ROUTING.md — lock-first capability gating; Forge Conductor's own memory_*/context_*/session_* tools are the product under test and must not drive Qwen's build session; failure policy: retry once, equivalent bound capability, then blocker.
  - .forge-qwen/instructions/qwen/RAG_PLAYBOOK.md — RAG after lock only; at most three focused queries before direct inspection; store query + document IDs/paths + concise findings; current verified files win over stale retrieval.
  - .forge-qwen/instructions/governance/PORT_TASK_CONTRACT.json — task FAE-TASK-2026-08-29-016: port Forge Conductor 0.9.0 to native Windows 11; C++20 OOP + WinUI 3 C++/WinRT; Forsetti public APIs only (profile 0.2.0, manifest 1.1); no Python anywhere; required outputs include GUI/CLI/manager/MCP/session host, MSIX + native setup, tests/docs/evidence; validation gates include all 53 MCP tools, parity rows, memory/continuity crash recovery, installer lifecycle, resource budgets, no-Python/no-attribution scans, independent validator.
  - .forge-qwen/instructions/architecture/TARGET_ARCHITECTURE.md — installed product binaries (ForgeConductor.App.exe, forge-conductor.exe, ForgeConductor.Manager.exe, ForgeConductor.SessionHost.exe, ForgeConductor.Setup.exe); library set Domain/Contracts/Application/Infrastructure.Windows/Mcp/Memory/Continuity/AgentHost/Manager/Telemetry.Windows/Rendering.Windows/Presentation.WinUI/ForsettiModule; dependency direction Domain <- Contracts <- Application <- adapters/presentation/hosts; no mutable global service state.
  - .forge-qwen/instructions/architecture/FORSETTI_MODULES.md — eight first-party modules (ForgeAppModule owns all WinUI surfaces; ForgeMemoryModule, ForgeMcpModule, ForgeContinuityModule, ForgeAgentHostModule, ForgeManagerModule, ForgeTelemetryModule, ForgeIntegrationModule); modules depend on ForsettiCore only; cross-module via public versioned events/services or bounded JSON contracts with correlation ID/deadline/caller capability/stable errors.
  - .forge-qwen/instructions/docs/WINDOWS_TOOLCHAIN.md — discover actual toolchain (VS/MSVC, CMake/CTest, Windows SDK, Winsqlite3, MakeAppx, SignTool, vcpkg, App SDK restore, LM Studio); provision first-party supported tooling noninteractively; pin resolved versions.
- RAG corpus manifest (.forge-qwen/rag/RAG_CORPUS_MANIFEST.json): 19 indexed documents (governance/architecture/audit); external roots (macOS source, Forsetti framework/agentic) index_by_default=false — direct assignment-routed reads required for exact symbols; denylist present.
- RAG query executed this session: "Forge Conductor P00 Package integrity and governing intake Read governing rules and task contract macOS source Forsetti Windows target audit evidence". Focused searches (2 of max 3): literal "P00" -> 0 corpus hits; literal "governing" -> 1 hit, corpus/governance/AGENTS.md line 65 ("RAG-v1: retrieve governing/source context ... verify with filesystem"). No stale retrieval conflicts found; all material facts above verified by direct file read.

## INFERRED

- P00-B is a source-only/read microtask: the objective is comprehension and evidence capture (this FACTS.md), not code changes; no implementation output is declared, so acceptance "implementation or declared source-only output complete" is satisfied by the fact table plus verified governance/contract reads.
- The remaining close steps (state transitions via Set-Microtask-State.ps1, Assert-QwenWritePaths.ps1 on .forge-qwen/state/evidence/P00-B/FACTS.md, Write-Qwen-Handoff.ps1, namespaced cursor replacement) are mechanical and fully specified by the step card templates.

## UNKNOWN

- Current recorded status of P00-B in microtasks.json / run-state.json (not re-read this session; ACTIVE_ASSIGNMENT.json shows "not_started").
- Whether Set-Microtask-State.ps1 was already executed for P00-B in a prior attempt before the forced handoff.

## BLOCKED

- B-P00B-001: Conductor tool-call budget exhausted (auto-handoff imminent) after step-card steps 7–9 completed; steps 10–20 (UNKNOWN resolution pass, in_progress transition, focused validation, write-path assertion, final state, checksummed handoff, MEMORY_CURSOR.json generation, namespaced cursor replacement, FORGE_QWEN_RESULT marker) not yet executed. Resume from this packet and continue at step-card step 10/11 using the exact control-command templates on the step card.
