# Focused tests, not another gate program
## Routine loop
Read the current failure, change a coherent slice, build its targets, run the relevant existing test and stop rerunning
it once the defect is corrected. A documentation-only edit needs no native build. Do not compile the entire test graph
by default. Preserve old regression tests as opt-in coverage; do not delete them just to hide failures.

The existing baseline test is `ForgeConductor.Continuity.AutomationTests`, with implementation at
`tests/Continuity/ContinuityAutomationTests.cpp`. Reuse it for the context-policy changes.

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Continuity.AutomationTests
ctest --test-dir out/build/windows-msvc-x64 -C Debug `
  -R '^ForgeConductor\.Continuity\.AutomationTests$' --output-on-failure --no-tests=error
```
Verify CTest names with `ctest --test-dir out/build/windows-msvc-x64 -C Debug -N` before selecting other tests.
Do not confuse a build fixture target with a registered test. The supplied `Invoke-FocusedTests.ps1` helper
builds explicitly named targets and runs explicitly named registered tests; it does not guess the whole suite.
The existing `scripts/test.ps1 -Label <label>` is also available, but broad inherited labels may select stress tests.

## Small regression requirements
**Context policy:** many ordinary calls and a long elapsed interval below threshold do not roll over;
reaching the effective context threshold does; output/resume reserves and overflow handling are consistent;
legacy count/time settings cannot reactivate the policy; duplicate observations do not create duplicate successors.
These can be cases in the existing automation test, not separate frameworks or a new test matrix.

**Provider seam:** a compact recorded-response fixture exercises a normal function call/result, response-ID chaining,
actual usage parsing, and a rejected wrong-handoff acknowledgment. Fixture success is not the live-provider acceptance.
Never have the production transport default to these fixtures or a local fake-ack transport.

**Data/settings:** one disposable project reset preserves another project's data; restart retains settings and memory;
MCP deployment preserves an unrelated server entry. Reuse existing repository tests where they already cover this.

R1 controlled transport coverage proves ordinary response/tool correlation, offline and malformed responses,
retained-context deduplication, continuity observation, pause/resume/cancel boundaries, and recovered uncertain-work
failure. It does not replace the R6 live provider acknowledgment and productive successor check.

R2 focused coverage uses the Windows CPU/RAM/PDH collector tests, Manager protocol/dispatcher and authenticated named-pipe
tests, and `ForgeConductor.App.TelemetryPresentationTests`. The presentation check covers logical CPUs/frequency, GPU adapters/engines and memory scope, disk rates/IOPS, volumes, relevant processes, cadence/freshness, Manager-derived context headroom, bounded histories, measured latency, store failures, disconnected values, and page detail projections.
The ignored receipt `out/alpha-evidence/r2/freshness-lifetime.json` records a real isolated Debug Manager/app lifetime
probe and matching Windows process/RAM observations. The 0.9.5 parity follow-up built Debug and Release products, passed the seven affected CTest targets, and then ran exact packaged bytes under `out/validation/telemetry-parity-alpha`. The native Rig exposed all 32 logical CPUs at positive frequency, RTX 4090 engine activity with explicit current-process DXGI memory scope, live disk/volume and relevant-process rows, 250 ms target/measured cadence, sample ages, growing histories, a real disconnected state, and successful Manager-start reconnect. UI Automation exposed the values and keyboard focus. Actual High Contrast rendered strong boundaries across the new panels. An initial 150% text pass exposed status-banner clipping; after the vertical-layout fix and rebuild, the exact replacement candidate passed a top-to-bottom 150% walkthrough without clipped telemetry labels, and both host settings were restored.

R4 focused coverage uses the Manager controller settings groups, protocol codec, dispatcher maintenance group, project-memory application/cache groups, and Windows memory/continuity repository groups. Together they cover exact confirmation rejection without service calls, typed combined-reset aggregation, transactional reset, project isolation, post-close rejection, restart durability, and continuity reset preservation. R6 completed the separate native keyboard, focus, High Contrast, and 150% text-size walkthrough.

## One live provider/continuity smoke
Use a disposable profile/project and a real locally loaded tool-capable model. Record actual model ID, LM Studio version,
loaded context capacity, configured effective capacity and usage source. Select a smaller valid effective context target
for the test, then restore the normal setting. Do not trigger the test by counting tools or elapsed time.
Have a multi-step task consume enough actual context to cross the threshold. The transition must include a fresh provider
root without a predecessor response ID, retrieval of the saved handoff, a provider-originated acknowledgment, fencing,
and a useful subsequent tool action or task result. Record the before/after response IDs, handoff ID and tool effect.
Close the GUI during the run and reattach once to verify that ownership is in the manager.
An unrecorded inference or ambiguous external tool effect after interruption must not be blindly repeated.
A small targeted interruption exercise suffices; do not build an adversarial crash campaign for Alpha.

## Live-provider result

The R6 continuation ran real LM Studio model `qwen3-coder-30b` with authoritative usage. The earlier run `786d0672-6231-4202-a46c-401732ade283` completed a real rollover and useful successor effects, then hit Forge's legacy desktop-chat repetition block; `PACKAGE_OK` came from a separate 0.9.3 exact-package run. After the guard repair, exact 0.9.4 run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed canonical rollover to `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed both requested effects, and returned `DONE`. R6-G2 passes; the installed pass below remains open.

## One installed-app acceptance pass
Use a Windows VM or suitable clean profile without source checkout/Visual Studio. Install the signed MSIX,
launch via Start, exercise the native pages and main workflow, update/reinstall once, then uninstall once with data retained.
Use screenshots or a short operator record for the actual GUI; headless process creation does not prove UI usability.
LM Studio/model installation remains a documented prerequisite, not a hidden embedded model download.

The 0.9.5 telemetry change invalidated the affected collector, Manager protocol/projection, presentation, version identity, Release build, and package checks. All seven focused targets passed; the signed package was unpacked and all 324 payload hashes were rechecked. Earlier managed-run/live-provider evidence remains applicable. G04's historical aggregate wrapper overflow was not rerun. Registered installation remains blocked without machine-level publisher trust.

## Evidence
For each required result record command/action, commit, configuration, exit code or observed behavior,
and one log/artifact path. A short Markdown acceptance record is sufficient. The absence of a suitable Windows host
or live model is `not run`, not `passed`. Fix required Alpha failures. Defer unrelated old release tests explicitly.
Do not run historical Run-All-Gates, every architecture, lengthy stress tests, or independent validator-agent chains
before each commit. Do not create a fake-green CI check that skips all tests when the filter matches nothing.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
