# Forge Conductor Windows — Alpha execution instructions

## Authority and outcome
The owner's September 10, 2026 request governs this Alpha. This file and `docs/ALPHA-SCOPE.md`
replace conflicting historical port-package instructions, gates, and Qwen/Codex workflow selectors.
`ROADMAP.md` defines the sequence; `docs/STATUS.md` records current product evidence.
Do not reactivate old package gate runners or enforce their completion policies as Alpha criteria.

Deliver a working Windows 11 x64 native GUI application and signed MSIX installer distribution.
Preserve and connect the existing C++20 backend; do not restart the port. Do not stop at a plan,
compilation, an empty window, a fake handoff, or a package containing placeholder executable paths.

## Implementation
Production code is modular object-oriented C++20. GUI is WinUI 3 with C++/WinRT.
Use MSVC/v143, Windows SDK, native Win32/COM, WinHTTP, Windows SQLite, and existing approved JSON dependency.
Use constructor injection, explicit composition roots and RAII. Keep the GUI separate from business logic;
the manager owns runs and services when the GUI closes. Use Forsetti public interfaces; do not patch its sealed core.
PowerShell is for build/test/package/admin automation. Do not add Swift, Python, Node, Java, .NET application,
Electron, Qt or an interpreted application runtime. Preserve existing notices and configured authorship.

## Context continuity and useful tools
No per-run tool-call quota, iteration limit, progress-count rollover, or timed session rollover.
Remove these policies from execution, settings, emitted configuration, prompts, tests and current documentation;
do not hide them or set them to a huge number. Context consumption alone triggers automatic rollover.
Retain sensible request timeouts, process cancellation, resource capacities and output-size controls: they are
not limits on how many tools the model may use across a run.
Use actual LM Studio provider turns, canonical handoffs, real model acknowledgments and predecessor fencing.
Never substitute a synthetic logical transport or local acknowledgment for live-provider completion.
Enable shell on clean installs; honor an explicit user opt-out. Keep existing tools available and functional.

## Completion, testing and focus
Follow P0–P6. Read only the active phase and required source, not all historical evidence.
Build changed targets once per coherent slice. Run the smallest relevant existing native tests.
Documentation-only changes need no application build. Full legacy gates, ARM64, stress campaigns,
security-hardening work, independent validator-agent chains and broad redesign are not Alpha requirements.
Do not delete useful protections or tests indiscriminately. Update obsolete count-policy assertions.
Required before Alpha: actual native GUI execution, usable tools, real context-forced rollover,
and an installed signed x64 build verified without the development checkout.
Record blockers honestly and continue independent work; never mark unavailable execution as passed.
After every coherent slice, continue with the next feasible P0-P6 action. A build, package, phase boundary,
or saved checkpoint does not end the assignment while required implementation or verification remains.

## Work preservation and continuity
Inspect git status first. Do not reset, clean, overwrite or discard unrelated work. Check newer changes against the audit;
never roll the repository back to its pinned audit commit. Keep `.forge-alpha/NEXT.md` and `status.json` concise.
At a context transition, persist current branch/commit, dirty files, last result, next command and unresolved blockers.
Do not require a new agent session merely to validate a slice. Do not create another microtask ledger.
No automated-author/model attribution in product files or commit trailers. Do not change global Git identity.
