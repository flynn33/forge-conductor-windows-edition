# Forge Conductor Windows — active delivery guidance

## Authority and outcome
The owner's September 12, 2026 replacement assignment and the adopted `windows-alpha-recovery-2026-09-12` package govern this work. Read [execution](docs/implementation/alpha-recovery/instructions/EXECUTION.md), [the roadmap](ROADMAP.md), the active [R phase](docs/implementation/alpha-recovery/phases/), `.forge-alpha/status.json`, and `.forge-alpha/NEXT.md`. Earlier P0–P6 files and stored Qwen/Codex packages are historical evidence, not active selectors.

Deliver a Windows 11 x64 native C++20 and WinUI 3 application with Manager-owned execution, complete operational telemetry, accessible persistent Settings, useful project/MCP/native-tool workflows, productive context-only continuity, and a working signed MSIX installer. Continue the existing implementation and Forsetti public interfaces. Do not replace it with a web, interpreted, or managed application runtime.

## Workspace, preservation, and implementation
Work only in `D:\GitHub\Forge-Conductor-Windows-Edition` against `https://github.com/flynn33/forge-conductor-windows-edition`. Preserve existing code, useful evidence, unrelated dirty work, configured toolchain inputs, and user data. Never hard-reset, force-push, clean away work, modify the live schema-9 database, fabricate C008/C009 migrations, or silently substitute an empty profile. Use the existing `--alpha-root` isolation for disposable checks.

Production code remains modular object-oriented C++20 with WinUI 3/C++/WinRT, MSVC/v143, Windows SDK, native Win32/COM, WinHTTP, Windows SQLite, and the approved JSON dependency. Use constructor injection, explicit composition roots, and RAII. Keep GUI concerns separate from services; the Manager owns runs and services when the GUI closes. PowerShell is for build, test, packaging, and administrative automation.

## Managed runs, continuity, and tools
Ordinary inference must enter through a typed Manager-owned run path that owns provider turns, tool dispatch, telemetry, and continuity. Context consumption alone triggers automatic rollover. Do not introduce per-run tool-call, iteration, progress-count, or timed-session quotas. Legitimate context capacity, reserves, request timeouts, cancellation, and output-size controls remain valid.

Use actual LM Studio provider turns, canonical handoff state, real provider acknowledgment, predecessor fencing, and productive successor work. Fixtures support focused development but cannot satisfy final live or installed acceptance. Shell is enabled on clean profiles while explicit user opt-out remains respected. Preserve existing useful tools and project separation.

## Execution and delivery
Implement one coherent slice, run the smallest relevant native checks, update its evidence, and continue to the next actionable slice. A provider, trust, migration-source, review, or permission blocker leaves only its dependent check open while independent implementation continues. Do not restart completed historical phases or run broad historical gates without a current need.

Every R phase updates README, CHANGELOG, ROADMAP, STATUS, the plan and ledger, the current handoff, and every active first-party document. Update affected content and replace the single phase-review stamp in unchanged current docs. Preserve historical evidence, generated inputs, licenses, and third-party notices without false refreshes.

Each R phase has one primary PR targeting `main`. Push through the verified `flynn33` account using the repository-verified owner identity. Add no assistant/model attribution, generated-by notices, assistant coauthor trailers, or bot authorship. Respect normal reviews and merge permissions. After an actual merge, fetch and fast-forward local `main`, then prove local, tracking, and GitHub main equality without publishing credentials, databases, build output, or unrelated files.

<!-- alpha-phase-review:start -->
Phase review: R0 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
