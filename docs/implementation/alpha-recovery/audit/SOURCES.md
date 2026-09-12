# Evidence and primary-source register

Repository files below pin the review commit. External documentation supports implementation details; it is not substituted for the owner's requested scope. New R phases and explicit telemetry/PR/documentation requirements are this replacement design, not claims of existing implementation.

## S01 — Repository metadata

[Repository metadata](https://api.github.com/repos/flynn33/forge-conductor-windows-edition)

**Evidence:** `metadata`. **Observed:** 2026-09-12.

Repository identity/default branch/public visibility observed. Permissions here do not establish Codex local CLI authentication.

## S02 — Branch snapshot

[Branch snapshot](https://api.github.com/repos/flynn33/forge-conductor-windows-edition/branches?per_page=100)

**Evidence:** `metadata`. **Observed:** 2026-09-12.

Returned main at the audited commit. Refresh actual branches during adoption; this is not a rollback instruction.

## S03 — Merged native desktop foundation PR #2

[Merged native desktop foundation PR #2](https://github.com/flynn33/forge-conductor-windows-edition/pull/2)

**Evidence:** `metadata_and_report`. **Observed:** 2026-09-12.

Merged at 2026-09-12T14:15:52Z into the audited main commit. PR test/build statements are author-reported, not rerun for this package.

## S04 — .forge-alpha/NEXT.md

[.forge-alpha/NEXT.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/.forge-alpha/NEXT.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Recorded ordinary run/usage integration gap; contains stale old-branch/uncommitted/auth/PID statements.

## S05 — docs/STATUS.md

[docs/STATUS.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/docs/STATUS.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Historical Windows build/test/installer/provider/database results and explicitly incomplete capabilities. Runtime observations not independently repeated.

## S06 — Root AGENTS.md

[Root AGENTS.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/AGENTS.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Current native architecture, no quota policy, preserve-work and concise testing guidance; new owner workflow expands the documentation/PR requirements.

## S07 — CHANGELOG.md

[CHANGELOG.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/CHANGELOG.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Still describes work as planned, despite the foundation merge; needs factual reconciliation.

## S08 — Native MainWindow XAML

[Native MainWindow XAML](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/src/Hosts/App/MainWindow.xaml)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Fourteen named destinations but only Rig, Provider and GenericPanel implementations in the inspected file.

## S09 — Native MainWindow handlers

[Native MainWindow handlers](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/src/Hosts/App/MainWindow.xaml.cpp)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Routes several destinations to Rig and the others to generic status; real typed provider form actions exist.

## S10 — Manager composition root

[Manager composition root](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/src/Hosts/Manager/ManagerCompositionRoot.cpp)

**Evidence:** `source_selected`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Includes/members and selected search excerpts show native provider/continuity composition and UnavailableTelemetryService. Large full-file response was truncated; no claim that every line was audited.

## S11 — Unavailable telemetry implementation

[Unavailable telemetry implementation](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/src/Composition/Windows/UnavailableTelemetryService.cpp)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

sample returns HostCapabilityUnavailable; health/latest do not provide real native measurements.

## S12 — Current roadmap

[Current roadmap](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/ROADMAP.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Old P0–P6 plan; later-parity wording defers richer telemetry. This replacement brings functional native operational visuals explicitly into required Alpha scope.

## S13 — Native Responses transport public contract

[Native Responses transport public contract](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/include/ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Non-streaming INativeSessionTransport bootstrap/session contract, real /v1 Responses configuration; not an ordinary run service interface.

## S14 — Current Alpha status JSON

[Current Alpha status JSON](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/.forge-alpha/status.json)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Old P-phase state, prior base/branch and reported evidence; preserve provenance but refresh current facts.

## S15 — Current Alpha scope

[Current Alpha scope](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/docs/ALPHA-SCOPE.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Windows 11 x64 native GUI, named capabilities, settings/reset, real tools/live continuity and signed installed acceptance.

## S16 — Current product README

[Current product README](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/README.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Current documented build stack and engineering status. Does not claim accepted Alpha.

## S17 — Current testing guidance

[Current testing guidance](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/docs/TESTING.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Existing focused native tests, CTest no-empty-selection rule and separate fixture/live/installed acceptance.

## S18 — Existing package script

[Existing package script](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/scripts/package.ps1)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Signed MSIX/public CER/engineering ZIP, source staging checks, fixed identity, four-part version and alpha_accepted=false. No actual package was created in this review.

## S19 — Historical P1 result

[Historical P1 result](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/.forge-alpha/P1-result.md)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Reports real --alpha-root lease/pipe/DPAPI scoping, isolated GUI attach/detach, signing and trust/database limitations. Preserved as historical report, not freshly reverified runtime.

## S20 — Central migrations implementation

[Central migrations implementation](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/src/Persistence/Windows/Migrations/CentralMigrations.cpp)

**Evidence:** `source_selected`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Pinned search excerpts include C007 SQL/array entry, corroborating records that available migrations reach7. Complete migration history and live schema 9 were not fetched/reconstructed.

## S21 — Native build entry point

[Native build entry point](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/scripts/build.ps1)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Backend/App/All build flow, dependency restore, existing toolchain input, target/configuration and staging behavior.

## S22 — Native WinUI build/staging helper

[Native WinUI build/staging helper](https://github.com/flynn33/forge-conductor-windows-edition/blob/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/scripts/alpha/Build-App.ps1)

**Evidence:** `source_full`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

MSBuild C++ project, actual executable names and output paths; preserves C++/WinRT native implementation.

## S23 — Audited merge commit metadata

[Audited merge commit metadata](https://api.github.com/repos/flynn33/forge-conductor-windows-edition/git/commits/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b)

**Evidence:** `metadata`. **Observed:** 2026-09-12.

Author Jim Daley with owner GitHub noreply email; GitHub committer is normal platform metadata. Exact tree and both merge parents observed.

## S24 — Alpha helper directory inventory

[Alpha helper directory inventory](https://github.com/flynn33/forge-conductor-windows-edition/tree/68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b/scripts/alpha)

**Evidence:** `directory_metadata`. **Observed:** 2026-09-12. **Pinned revision:** `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`.

Build-App, Install-Engineering, Invoke-P2WorkflowSmoke and Restore-Forsetti helpers listed. Inventory is not full code review of every helper.

## E01 — LM Studio Responses endpoint

[LM Studio Responses endpoint](https://lmstudio.ai/docs/developer/openai-compat/responses)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Responses state/previous_response_id and supported API behavior. Verify installed server behavior and configured endpoint; documentation is not a live-provider test.

## E02 — Windows app accessibility overview

[Windows app accessibility overview](https://learn.microsoft.com/en-us/windows/apps/design/accessibility/accessibility-overview)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Native labels, keyboard/focus and accessibility design; applied to practical Windows GUI checks.

## E03 — Windows GetSystemTimes

[Windows GetSystemTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Native CPU accounting API and scope considerations; use valid counter deltas and supported topology behavior.

## E04 — Windows GetProcessMemoryInfo

[Windows GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Process memory query, not a substitute for global physical-memory or GPU metrics.

## E05 — MSIX signing overview

[MSIX signing overview](https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Package signing requirements and trust; actual installability must be tested on the target environment.

## E06 — MSIX development signing certificate

[MSIX development signing certificate](https://learn.microsoft.com/en-gb/windows/msix/package/create-certificate-package-signing)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Certificate subject/publisher and legitimate development trust procedure; no bypass permission.

## E07 — GitHub CLI pull request create

[GitHub CLI pull request create](https://cli.github.com/manual/gh_pr_create)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Explicit head/base/body-file workflow.

## E08 — GitHub CLI pull request merge

[GitHub CLI pull request merge](https://cli.github.com/manual/gh_pr_merge)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Normal merge methods/head-match guard; authorization and branch rules still govern execution.

## E09 — Git pull documentation

[Git pull documentation](https://git-scm.com/docs/git-pull)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Fetch/integration behavior; package chooses explicit fetch plus fast-forward-only integration to avoid destructive synchronization.

## E10 — Git diff documentation

[Git diff documentation](https://git-scm.com/docs/git-diff)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Tracked/index comparisons and exit-code evidence; untracked files also require separate inspection.

## E11 — Codex AGENTS.md guidance

[Codex AGENTS.md guidance](https://developers.openai.com/codex/guides/agents-md/)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Repository instruction discovery. Current redirect points into official learn.chatgpt.com agent configuration documentation. This package does not override higher-priority host rules.

## E12 — Windows system information API reference

[Windows system information API reference](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/)

**Evidence:** `official_external_documentation`. **Observed:** 2026-09-12.

Supported system-memory/platform API discovery. Collector details must match the actual native toolchain.

## U01 — Original attached Windows Alpha Codex instruction ZIP

Original attached Windows Alpha Codex instruction ZIP (supplied in the conversation)

**Evidence:** `owner_supplied_material`. **Observed:** 2026-09-12.

Supplied Windows package basis, not a current repository snapshot or proof of runtime completion.

## U02 — Continuation directive and Codex checkpoint reports

Continuation directive and Codex checkpoint reports (supplied in the conversation)

**Evidence:** `owner_supplied_material`. **Observed:** 2026-09-12.

Owner conversation describes progress/obstacles and requests continued work. Source review supersedes stale local-status narration, without assuming access to D:.

## U03 — Windows GUI/telemetry/settings/installer clarification

Windows GUI/telemetry/settings/installer clarification (supplied in the conversation)

**Evidence:** `owner_supplied_material`. **Observed:** 2026-09-12.

Owner requires Windows 11 native GUI with full functional operational visuals, accessible settings and Windows installer; incorporated, not left optional.

## U04 — September12 replacement-package request

September12 replacement-package request (supplied in the conversation)

**Evidence:** `owner_supplied_material`. **Observed:** 2026-09-12.

Exact owner paths; phase/slice/milestone tracking; all-doc updates each phase; phase PR to main; owner pushes/no attribution; local/GitHub sync; minimum useful tests.

## S25 — Final main branch recheck

[Final branch metadata](https://api.github.com/repos/flynn33/forge-conductor-windows-edition/branches/main)

Rechecked before packaging: main remains at the audited commit/tree. Response reports protected=false and no branch-required status-check contexts at this observation; actual future review/check/merge authorization must still be read, not assumed.
