# Roadmap

The measurement block below records historical 1.3.19 qualification; it does not qualify the current 1.3.21 implementation.

<!-- measured-qualification-1.3.19 -->

Both native WinHTTP timeout diagnostics consistently report the selected receive budget, with unchanged timeout, deadline and cancellation behavior. The managed-context legacy-lease fix is retained. The exact 1.3.19 source passed 162/162 Release tests in 74.37 seconds, Product All, all three static gates and package persistence. Windows CI separately passed 162/162 in 126.04 seconds. Installed/staging/MSIX hashes matched all four executables, and actual catalogs returned 104/104/5. The existing selected Qwen conversation passed 9 bounded cases, including the formerly denied Host read and fresh sealed blind image analysis. The dedicated real-model managed recovery returned the exact seeded packet and unseen file in 2 captured native calls with verified sealed output. [Published release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.19) contains seven assets verified by size and SHA-256. [Windows package](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.msix) and [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.zip).

<!-- /measured-qualification-1.3.19 -->

## Current source version: 1.3.21

Version 1.3.21 implements 106 Primary/Fallback tools while preserving all original tools, ten specialist playbooks and five CLU tools. Native CMake/CTest run/status jobs, paged accessibility reads and optional higher-resolution bounded image previews extend dedicated web, native Office documents, desktop/browser, PNG/image analysis, independent managed workers and persistent schedules. Existing shell/process jobs, reviews, memory, policy, deployment and continuity remain. Clean-source, signed-package, installed-catalog and measured native-model qualification passed; the 1.3.21 verification record states the exact scope and limits. See the [capability guide](docs/HOST-CAPABILITIES.md), [CMake/CTest guide](docs/CMAKE-CTEST.md), [implementation notes](docs/releases/1.3.21.md) and [measured verification](docs/validation/HOST-CAPABILITIES-1.3.21.md).

The 1.3.21 source persists the interrupted native handoff phase in a bounded, current-user DPAPI checkpoint. A single writer owns native controls; reconstruction requires fresh project/provider/route confirmation and selected-chat evidence. Uncertain New chat or Send effects remain `recovery_pending` and are not replayed. The 118-group Infrastructure suite passed, including the private same-PID observer reconstruction regression and 20 durability cases. Installed observer/UI interruption or connector rollover recovery remains unverified; physical context exhaustion and already-running agent reattachment were not exercised. Historical 1.3.20 checkpoint-process primitive evidence retains its original source identity and is not a current installed qualification. See the [continuity implementation](docs/implementation/CONTEXT-CONTINUITY.md).

The following limitation records the historical qualification through 1.3.19, before the 1.3.21 checkpoint implementation:

Historical Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Qualify the implemented durable native handoff after an interrupted installed Primary connector is evicted/restarted, including uncertain-effect reconciliation without duplicate New chat or Send.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
