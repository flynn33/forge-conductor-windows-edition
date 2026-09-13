# Small, real-product acceptance pass

## What a phase gate means
R0 establishes the workflow. R1–R5 qualify the implementation increment and its focused functional checks. R6 qualifies the actual installed application and live provider; R7 finishes truthful documentation and source synchronization. This separation allows useful code to merge without falsely claiming a finished Alpha. A failed ordinary code test does not become “blocked externally” merely because LM Studio is offline.

Each phase's primary PR includes all its available required evidence and explicitly names remaining R6 conditions. Do not hold telemetry/settings/local workflows idle behind a missing local model or certificate approval. Do not close R6 or label the product a working Alpha while a required real-product check is unavailable or failed. See [Roadmap](ROADMAP.md) and [build/test policy](BUILD-AND-MINIMUM-TESTS.md).

## Preparation without another setup campaign
Use the owner's actual Windows 11 x64 test environment. Preserve the production store. Choose an explicitly disposable Alpha root, two disposable projects A/B, a harmless local test Git repository, and a foreign MCP configuration sentinel. Record the actual candidate package identity/version/hash and the exact product source commit/tree. Identify the actual LM Studio endpoint and loaded tool-capable model from effective settings; do not assume the old localhost refusal remains current.

Choose one coherent signed candidate. Test the installed executables, not Debug children accidentally resolved from D:. The clean-machine portion must not depend on Visual Studio, this repository, or A: instructions. A test profile on a development machine can be useful but is not automatically proof that all runtime prerequisites are bundled. Do not uninstall or overwrite the owner's working installation without the normal authorized test setup.

## A1 — Installation and everyday local work (R6-G1)
Install the signed MSIX through its documented Windows path using the required legitimate publisher trust. Launch from Start. In the native GUI register/select project A, confirm the active project identity and data root, and deploy primary/fallback MCP without altering the foreign entry. Exercise the actual advertised filesystem read/write, search, local Git and shell operations in that disposable repository. Shell must work on a clean install unless the operator explicitly opted out.

Write and retrieve project memory; restart or reconnect through the intended product controls and verify persistence. Register B or inspect its prepared sentinel and demonstrate it is unchanged by A's ordinary work. Inspect the actual installed GUI/Manager/CLI/SessionHost paths. Verify installed MCP stdout carries only its protocol, with diagnostics directed elsewhere. Record one compact action/result table, not an infrastructure-test count.

Failure of registration, real tool effects, installed path resolution, protocol framing or memory persistence is a required functionality defect. Fix the responsible code and rerun that affected step, not the whole historical gate suite.

## A2 — Real context-triggered productive continuation (R6-G2)
Start a real Forge-managed run from the native application using a loaded tool-capable LM Studio model. An independently owned LM Studio desktop chat is not a managed run just because MCP is connected. Record model identity, loaded context information when exposed, configured effective capacity and each reserve. Where loaded capacity is not programmatically available, identify the verified configuration source rather than inventing a value.

In the disposable configuration, choose a valid small effective application threshold that leaves enough space for response/handoff/safety reserves and fits within the real loaded capacity. Have useful ordinary model/tool work consume actual retained context. Do not increment fake usage values, count tools, wait a fixed interval, or invoke manual rollover as a substitute for the automatic threshold path.

Observe this connected sequence in one run: normal provider turns and real tools; authoritative usage observation; canonical handoff with project/run/generation; a fresh provider root that does not carry the predecessor's `previous_response_id`; retrieval of the exact saved context; provider-originated structured acknowledgment matching the binding/checksum; predecessor fencing; and **a useful automatic successor tool effect or task result**. Report actual response and handoff identifiers with secrets/content redacted where necessary. A locally fabricated acknowledgment or fixture cannot satisfy this gate.

Close and reopen the GUI during a safe part of the run. Verify the Manager retains ownership and reconnecting does not create another run/accepted successor. Exercise pause/resume or cancellation once at a safe boundary. Do not deliberately repeat an uncertain external side effect merely to prove retry. Restore the normal disposable test setting afterward; do not alter the owner's unrelated active model/run.

If the provider is unavailable, make the GUI show the real problem and keep A2 not run/blocked. Implement remaining code and finish independent checks. Do not relabel offline fixture success as this live test.

## A3 — Visual GUI and accessible settings (R6-G3)
Walk every row in [the native page contract](PRODUCT-CONTRACT.md). Related views may share navigation, but no required capability may resolve to the old GenericPanel. Show actual live CPU/RAM/Forge-process history, operational health, provider response usage/performance, context headroom, and a continuity/activity timeline. For optional GPU metrics show measured values on supported hardware or a concrete unsupported/unavailable reason. A zero is a measurement, not a placeholder.

Verify a fresh/connected display, an actual disconnected/stale state, and reconnect. Confirm numbers, units, history scales, timestamp and selected project/run scope. A screenshot illustrates appearance; it does not alone prove an action or a collector. Attach a few representative actual screenshots or an equivalent direct interactive observation record, with source/profile context and no secrets.

Operate Settings by keyboard, including a context slider and exact numeric entry. Check visible focus, accessible labels/text equivalents for custom charts, and one enlarged/high-contrast layout. Save valid provider/context/shell/logging or refresh preferences as applicable, read the effective persisted values after reconnect/restart, and try one invalid input without corrupting the saved value. There is no need for a new UI automation or screenshot-comparison framework.

On disposable data only, cancel a project reset first (no change), then reset A's selected memory/continuity scope while B remains intact. Verify a late pre-reset generation cannot restore deleted state. Test all-store maintenance only in a separately explicit disposable profile in which every store is disposable. No source repository is a memory store.

## A4 — Upgrade, uninstall and data compatibility (R6-G1 / R5-G2)
Test one real higher numeric package version under the same identity/publisher, then one uninstall/reinstall as necessary to inspect retention. Confirm the documented retained data and foreign MCP configuration survive. Do not count re-packaging the same numeric version with a newer timestamp as an upgrade. Inspect that old owned processes cannot keep using an uninstalled executable or cross-attach to another profile.

Supported legacy schema fixtures must upgrade correctly. A newer unsupported store must be refused without mutation. The owner has selected the fresh persistent Internal Alpha profile for this release and deferred schema-9/C008/C009 migration. Verify that ordinary installed Start launches use the selected profile, that its new data survives update/uninstall/reinstall as declared, and that the preserved legacy store is never opened or changed. Record legacy migration as deferred, not implemented, tested, or passed.

## Minimum bug triage
Fix a required-path crash, data loss/cross-project write, broken settings, unusable required page, installer/path failure or false continuity success before final acceptance. Add a focused regression where it can reproduce the bug and is not already covered. Retest the affected integration after a shared-interface or lifetime change. Unrelated optional hardware coverage, cosmetic polish beyond usability and historical stress/security matrices may remain deferred with a truthful reason.

All gates need a short outcome and evidence reference. Use the [phase closeout](../templates/PHASE-CLOSEOUT.md) and [acceptance record](../templates/ACCEPTANCE-RESULT.md); no new evidence engine is required. A missing environment is not a bug fix, a mock is not the provider, a signed ZIP is not an installation, and a saved NEXT.md is not a finished assignment.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
