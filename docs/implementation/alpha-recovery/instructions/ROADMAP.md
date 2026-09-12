# Windows Alpha delivery roadmap

**Outcome:** an installed Windows 11 x64 native GUI with complete operational visuals, usable Settings, real local tools/project memory and Manager-owned productive context continuity. Existing working C++/WinUI implementation is retained.

**Plan:** `windows-alpha-recovery-2026-09-12`. Audited current main: `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`. This hash identifies the review, not a rollback destination. Baseline evidence and remaining gaps are in [Snapshot](../audit/SNAPSHOT.md). Active live state remains `.forge-alpha/status.json` and `.forge-alpha/NEXT.md` in the source repository. GitHub milestones/issues/PRs mirror that work and report their actual delivery state.

## Phases and milestones

| Phase | Deliverable | Slices | GitHub milestone | Required implementation dependencies |
|---|---|---:|---|---|
| [R0](../phases/R0.md) | Reconcile the repository and establish the delivery workflow | 4 | WA-R0 — Current baseline and accountable delivery | None |
| [R1](../phases/R1.md) | Complete Manager-owned inference and context continuity wiring | 5 | WA-R1 — Real managed execution path | R0 |
| [R2](../phases/R2.md) | Deliver real native telemetry and visual dashboards | 5 | WA-R2 — Live visual operations | R0 |
| [R3](../phases/R3.md) | Finish the native operating pages and local workflows | 5 | WA-R3 — Usable native workflows | R0 |
| [R4](../phases/R4.md) | Complete accessible settings and scoped maintenance | 5 | WA-R4 — Accessible controls and safe reset | R0 |
| [R5](../phases/R5.md) | Finish the Windows installer and existing-data behavior | 5 | WA-R5 — Installable Windows candidate | R0 |
| [R6](../phases/R6.md) | Run the small real-product acceptance pass and fix blockers | 4 | WA-R6 — Working Alpha proven | R1, R2, R3, R4, R5 |
| [R7](../phases/R7.md) | Close documentation, merge the delivery PR and synchronize main | 3 | WA-R7 — Documented and synchronized delivery | R6 |

There are 36 coherent implementation/tracking slices, not 36 separate PRs or mandatory test suites. Each R phase has one primary PR targeting main and one issue whose checklist records the slices. Reuse actual existing records by phase identifier; don't invent milestone numbers or create duplicate issues after a context transition. No artificial due dates, hour estimates, percentage-complete claims or tool budgets are imposed.

## Execution order and real dependencies
Start R0, then make the concrete R1 ordinary-run change identified by the current repository handoff. Do not reimplement the already-merged WinUI project, real Responses bootstrap transport, `--alpha-root`, Forsetti recovery or engineering packaging. Verify applicable work and extend it.

R2 telemetry collection/visual controls, R3 local workflow pages, R4 settings/reset and R5 installer work have independent portions once R0 is adopted. They may progress when R1 live/provider or review dependencies are unavailable. Integrations of run controls/context/successor status into R2/R3/R4 depend on R1's real contracts; do not invent substitute services merely to close a phase early. If that precise interface is missing, implement an independent slice and leave the dependent gate open. R5 package staging may use the current implemented product while final R6 acceptance waits for all capabilities.

Use one active code slice at a time and preserve shared ledger state across branches. Passing a phase's implementation gates permits normal submission of that increment, **not final Alpha acceptance**. An actual code failure in that increment must be fixed before its PR is ready. Only checks explicitly assigned to R6 may remain as named final-acceptance dependencies on otherwise verified R1–R5 work. The phase file and PR must say so plainly.

R6 requires the actual installed GUI, actual live provider productive rollover, actual settings/reset and lifecycle checks. It cannot be completed using fixtures or unavailable-environment notes. R7 cannot finish until the final PR is actually merged and local/main/GitHub source equality is verified. While an owner merge/review is pending, continue independent work under [Git workflow](GIT-WORKFLOW.md), not a polling loop or a status-only stop.

## Common phase completion transaction
For **every phase**, including R0 and R7: finish its coherent scope; pass its listed minimal gate checks; update source and tests where required; update README, CHANGELOG, ROADMAP, STATUS, plan/ledger, current handoff and every active first-party document; commit reviewed files as the owner; push and open that phase's PR to main. Read actual required reviews/checks and follow normal merge authority. After an actual merge synchronize local main and record a real receipt. See [tracking/docs](TRACKING-AND-DOCS.md) for all-doc review and [Git](GIT-WORKFLOW.md) for the distinction between PR submission and authorized merge.

Do not create an extra native rebuild for a documentation-only closeout. Do not create endless commits to insert a commit's own SHA into itself. Keep the committed record factual at submission; actual merge/sync receipts are recorded on the PR and folded into the next normal phase update.

## Preserve and map the previous plan

| Earlier phase | Existing evidence / reuse | New work ownership |
|---|---|---|
| P0 foundation | Restore/build/documentation foundation is already in merged main; records are stale | R0 reconciles truth and tracking only; no restart |
| P1 native desktop | WinUI project, isolated Manager attach and engineering MSIX exist | R2/R3/R4 finish native capability; R5/R6 installed proof |
| P2 local workflow | Disposable CLI/MCP/tool/memory workflow is reported passed | R3 connects usable GUI workflow; R6 reuses it from installed paths |
| P3 provider/continuity | Real bootstrap transport, context-only policy, Manager automation lifetime and fixtures exist | R1 supplies ordinary run/usage/productive continuation; R6 proves live behavior |
| P4 native GUI | Provider page and navigation exist; required pages are not all implemented | R2 visuals, R3 operations, R4 Settings/maintenance |
| P5 installer | Signed engineering packaging exists; installed trust/lifecycle unresolved | R5 candidate mechanism/data behavior; R6 real installed lifecycle |
| P6 acceptance | Required live/installed GUI completion remains open | R6 actual acceptance and R7 truthful delivery |

Do not relabel old reported passes as newly executed R-phase gates without checking their applicability. Keep original phase receipts immutable historical evidence, not active selectors.

## After this assignment
Full **native operational telemetry, all required page functions, context sliders/settings and the Windows installer are not deferred**. Optional browser-dashboard parity, advanced analytics, broad connector expansion, GPU artwork, additional architectures, public Store/release infrastructure and unrelated security/stress campaigns remain outside this internal Alpha assignment. Preserve useful existing implementations and protections; do not use deferral as permission to remove features.

The final result is a working internal candidate and synchronized source/documentation, not permission to publish a public release. Absence of a model, an approval or an authentic migration contract is recorded precisely; it cannot be silently changed into final success.

<!-- alpha-phase-review:start -->
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
