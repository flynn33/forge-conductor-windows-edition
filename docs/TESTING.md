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

R2 focused coverage uses the Windows CPU/RAM collector tests, Manager protocol/dispatcher and authenticated named-pipe
tests, and `ForgeConductor.App.TelemetryPresentationTests`. The presentation check preserves stale CPU, unsupported GPU,
Manager-derived context headroom, bounded histories, measured latency, store failures, and page detail projections.
The ignored receipt `out/alpha-evidence/r2/freshness-lifetime.json` records a real isolated Debug Manager/app lifetime
probe and matching Windows process/RAM observations. R6 later completed native keyboard, High Contrast, and 150% Windows text-size inspection on the exact 0.9.2 package.

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

## One installed-app acceptance pass
Use a Windows VM or suitable clean profile without source checkout/Visual Studio. Install the signed MSIX,
launch via Start, exercise the native pages and main workflow, update/reinstall once, then uninstall once with data retained.
Use screenshots or a short operator record for the actual GUI; headless process creation does not prove UI usability.
LM Studio/model installation remains a documented prerequisite, not a hidden embedded model download.

R6 packaging verification used the full x64 Release product build and `scripts/package.ps1 -DevelopmentSigning` at commit `d8a2d68c80f2fd090aa36466a517725a0eb59445`. The package script signed, unpacked, and rehashed the 0.9.2.0 MSIX. The exact unpacked CLI/GUI/Manager path, detach/reattach, telemetry, profile-isolation and accessibility checks passed. A normal 0.9.1.0 installation attempt still returned `0x800B0109` because machine-level trust for the development publisher was absent; keep installed checks open until that explicit trust action succeeds.

## Evidence
For each required result record command/action, commit, configuration, exit code or observed behavior,
and one log/artifact path. A short Markdown acceptance record is sufficient. The absence of a suitable Windows host
or live model is `not run`, not `passed`. Fix required Alpha failures. Defer unrelated old release tests explicitly.
Do not run historical Run-All-Gates, every architecture, lengthy stress tests, or independent validator-agent chains
before each commit. Do not create a fake-green CI check that skips all tests when the filter matches nothing.

<!-- alpha-phase-review:start -->
Phase review: R6 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
