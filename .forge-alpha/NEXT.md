# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12` plus owner telemetry-parity correction
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R2.5 — native telemetry parity closeout
**Current branch:** `alpha/r2-telemetry-parity`, based on synchronized main `56e5f55a2439fd5c6f1e2ce5fe1ca5d54f5c4517`

## Verified state

- PR #23 merged and main/origin/main/GitHub main were observed at `56e5f55a2439fd5c6f1e2ce5fe1ca5d54f5c4517` before this follow-up. Draft PR #24 targets main and carries the reopened R2 telemetry work.
- Product source commit `27c26e1401da72241cff68012ece2a8645f4e752` adds native logical-CPU/frequency, GPU-engine, disk/volume, and relevant-process measurements with typed Manager transport and integrated WinUI presentation. Product identity is 0.9.5.
- The Manager owns a 250 ms base sample, tiers GPU/disk near one second and process/volume work near five seconds, publishes measured cadence/timestamps/sample age, and retains bounded time-aware histories. The GUI does not own a second collector.
- Debug and Release products built. Seven affected CTest targets passed after rebuilding: Telemetry Windows, Manager protocol/dispatcher, App presentation, MCP protocol/serve snapshot, and Infrastructure unit tests.
- Final signed candidate `out/dist/candidate-0.9.5.0-20260913-135703/ForgeConductor-0.9.5.0-x64.msix` has SHA-256 `78683d3cef440a190932b8f8cb0fe53b39a6a1ac2940a80c7e4da9e4309a8f22`; ZIP SHA-256 is `6bb46f8992599a4d7da5729570011c76cf6cd0657857505ffbb19ca4737b8e91`. It is bound to commit `27c26e1401da72241cff68012ece2a8645f4e752` and tree `e9995ac4ca6de0043fb1f293091f23720a434acd`.
- Exact packaged GUI/Manager bytes ran under disposable root `out/validation/telemetry-parity-alpha`. The native Rig rendered target/measured cadence, all 32 logical CPUs at positive frequency, RTX 4090 engine utilization, explicit current-process DXGI memory scope, live disk throughput/IOPS, A:/C:/D: capacity, relevant LM Studio/Forge process rows, workflow inventory, and growing histories.
- The exact candidate also rendered an honest disconnected state and reconnected after the native **Start manager** control launched packaged Manager PID 43400. UI Automation exposed values and keyboard focus. Actual High Contrast rendered strong panel boundaries. An initial 150% text pass found a clipped horizontal status banner; commit `5b4c1cb` changed it to a vertical layout, and a rebuilt exact-candidate top-to-bottom 150% pass showed all telemetry headings, values, workflow inventory, history, and Manager controls without clipping. High Contrast and text scale were restored, exact packaged processes were stopped, and unrelated Manager PID 37832 was preserved.
- Prior exact 0.9.4 LM Studio rollover evidence remains valid. The live owner database and unrelated `.forge-qwen/state/**` changes/evidence were untouched and excluded from commits.

## Next exact action

Commit and push this documentation/ledger closeout to draft PR #24, update its description, mark it ready, complete normal review/merge under existing owner authority, then preserve and retire only `alpha/r2-telemetry-parity` with an exact-head lease. The machine-trusted installed lifecycle remains a separate open acceptance action.

## Open dependencies

- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.
- `B-R6-INSTALL-TRUST`: an authorized administrator must import the public development certificate into Local Machine Trusted People and run the documented 0.9.4→0.9.5 install/update/uninstall/reinstall workflow in dedicated account `.\ForgeAlphaTest`. Do not use the owner profile.

## Phase closeout obligations

R2-G1, R2-G2, and R2-G3 pass for the parity follow-up. R6-G1 remains blocked and is not waived. Keep implementation, individual checks, review readiness, merge, and acceptance distinct; report Git ref equality separately from the intentionally dirty unrelated `.forge-qwen` working tree.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
