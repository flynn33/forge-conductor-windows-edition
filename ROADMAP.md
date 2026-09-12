# Windows Alpha delivery roadmap

**Outcome:** an installed Windows 11 x64 native GUI with complete operational visuals, usable Settings, real local tools/project memory, and Manager-owned productive context continuity. Existing working C++/WinUI implementation is retained.

The active plan is `windows-alpha-recovery-2026-09-12`; its machine-readable scope is [docs/alpha-plan.json](docs/alpha-plan.json). The audited main `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b` records merged PR #2 and is a reconciliation observation, not a rollback destination.

| Phase | Milestone and issue | Deliverable | Delivery |
|---|---|---|---|
| [R0](docs/implementation/alpha-recovery/phases/R0.md) | [WA-R0](https://github.com/flynn33/forge-conductor-windows-edition/milestone/1) / [#3](https://github.com/flynn33/forge-conductor-windows-edition/issues/3) | Current baseline, adopted instructions, accountable delivery workflow | Merged through [PR #11](https://github.com/flynn33/forge-conductor-windows-edition/pull/11) and synchronized |
| [R1](docs/implementation/alpha-recovery/phases/R1.md) | [WA-R1](https://github.com/flynn33/forge-conductor-windows-edition/milestone/2) / [#4](https://github.com/flynn33/forge-conductor-windows-edition/issues/4) | Manager-owned ordinary inference and context continuity wiring | Merged through [PR #12](https://github.com/flynn33/forge-conductor-windows-edition/pull/12) and synchronized; native visual control walkthrough remains open |
| [R2](docs/implementation/alpha-recovery/phases/R2.md) | [WA-R2](https://github.com/flynn33/forge-conductor-windows-edition/milestone/3) / [#5](https://github.com/flynn33/forge-conductor-windows-edition/issues/5) | Real native telemetry and visual dashboards | Implementation pushed in draft [PR #13](https://github.com/flynn33/forge-conductor-windows-edition/pull/13); native visual/accessibility walkthroughs blocked by the current control surface |
| [R3](docs/implementation/alpha-recovery/phases/R3.md) | [WA-R3](https://github.com/flynn33/forge-conductor-windows-edition/milestone/4) / [#6](https://github.com/flynn33/forge-conductor-windows-edition/issues/6) | Native operating pages and local workflows | Next independent phase |
| [R4](docs/implementation/alpha-recovery/phases/R4.md) | [WA-R4](https://github.com/flynn33/forge-conductor-windows-edition/milestone/5) / [#7](https://github.com/flynn33/forge-conductor-windows-edition/issues/7) | Accessible settings and scoped maintenance | Planned |
| [R5](docs/implementation/alpha-recovery/phases/R5.md) | [WA-R5](https://github.com/flynn33/forge-conductor-windows-edition/milestone/6) / [#8](https://github.com/flynn33/forge-conductor-windows-edition/issues/8) | Installable signed Windows candidate and data behavior | Planned |
| [R6](docs/implementation/alpha-recovery/phases/R6.md) | [WA-R6](https://github.com/flynn33/forge-conductor-windows-edition/milestone/7) / [#9](https://github.com/flynn33/forge-conductor-windows-edition/issues/9) | Real installed and live-provider Alpha acceptance | Planned |
| [R7](docs/implementation/alpha-recovery/phases/R7.md) | [WA-R7](https://github.com/flynn33/forge-conductor-windows-edition/milestone/8) / [#10](https://github.com/flynn33/forge-conductor-windows-edition/issues/10) | Documented delivery and exact main synchronization | Planned |

## Execution

R0 preserves and reconciles the existing foundation. R1 supplies the typed Manager-owned ordinary Responses/tool loop, authoritative retained-context observations, continuity successor ownership, and native run controls. R2 supplies the native resource collectors, typed operational snapshot, native charts/gauges/status/timeline presentation, drill-down summaries, and refresh lifetime. R3–R5 complete native operating actions, settings/reset, and installer/data behavior. R6 performs the small real installed/live acceptance pass; R7 closes documentation and source synchronization.

Independent R2–R5 slices can continue while a provider, installation permission, migration source, review, or merge dependency is unavailable. Each phase has one primary PR to `main`; implementation, PR delivery, actual merge, and final acceptance remain distinct states.

The prior P0–P6 plan is retained under [historical implementation records](docs/implementation/) and mapped in the adopted [roadmap contract](docs/implementation/alpha-recovery/instructions/ROADMAP.md). It records useful foundation evidence but does not compete with R0–R7.

Full native operational telemetry, all required page functions, context settings, and the Windows installer are part of this Alpha. Optional browser-dashboard parity, advanced analytics, broad connector expansion, additional architectures, public Store/release infrastructure, and unrelated security/stress campaigns remain outside this internal delivery.

<!-- alpha-phase-review:start -->
Phase review: R2 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
