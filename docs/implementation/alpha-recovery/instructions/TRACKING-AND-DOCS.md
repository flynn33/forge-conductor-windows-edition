# Tracking and documentation at every phase

## One scope plan, one execution ledger
Use the existing repository structure rather than another workflow engine:

| Record | Ownership |
|---|---|
| `ROADMAP.md` | Human roadmap: R phases, delivered behavior, current work, real issue/PR links. |
| `docs/alpha-plan.json` | Stable scope/dependencies/slices/gates from this package; changes are reviewed, not casual deferrals. |
| `.forge-alpha/status.json` | Current execution, per-phase/slice state, evidence references, blocker dependencies and real remote IDs. |
| `.forge-alpha/NEXT.md` | Compact operational cursor: current exact edit/command and facts to revalidate on resume. |
| `.forge-alpha/phases/Rn.md` | One concise phase closeout, functional checks, docs sweep and PR reference. |
| `docs/DOCUMENTATION-INDEX.md` | Complete inventory of active vs historical/reference docs and the latest review coverage. |
| `docs/STATUS.md` | Product-level implemented/verified/blocked status, not a duplicate task ledger. |
| GitHub phase issue/milestone/PR | Publicly inspectable progress and actual delivery state. |

At adoption preserve the old status/handoff as a dated historical record if needed, then migrate fields and retain relevant evidence references. Do not replace real entries with the blank template. The old P0–P6 crosswalk in [Roadmap](ROADMAP.md) prevents completed work from being lost. Do not maintain a separate mutable A: package progress copy.

## State semantics
For slices use `not_started`, `in_progress`, `blocked`, `verified`. A verified slice cites its actual implementation and minimal relevant check. Under `phase_status[Rn]`, use `implementation`, `delivery`, `slices`, `gates`, `documentation_review` and actual `pr_number`/`pr_url`, exactly as in the template/schema. Implementation values are `not_started`, `in_progress`, `verified`, `blocked`; delivery values are `not_started`, `branch_pushed`, `pr_open`, `awaiting_merge`, `merged`, `synced`, `blocked`. Do not create synonymous competing field sets. A phase can have verified implementation while its PR is pending; never equate that with merged delivery or final working Alpha.

Each gate result is `not_run`, `passed`, `failed`, or `blocked`, with the tested source/build configuration and actual receipt. Unsupported optional hardware is a capability result with a reason, not a test failure automatically waived by the model. Missing implementation of required CPU/RAM telemetry is a real failure, not optional hardware unavailability.

For blockers record `id`, exact failure, affected slice/gate, dependency type, last observed time, necessary external action, and next independent work. Prefer a short stable list in the existing ledger. Stop carrying a blocker after it is verified resolved; retain its resolution evidence. A historic endpoint refusal, PID, dirty-file count or CLI authentication report is not a current machine fact.

## Update while working
After each coherent slice, update its result, current cursor and relevant changed product documentation. Do not commit after every command or maintain command-by-command transcripts. One concise gate receipt may cover multiple relevant cases. Record source state accurately when evidence was obtained before a documentation-only commit: identify the tested code commit or source-tree hash and the later nonfunctional delta rather than claiming the test ran at a future SHA.

On compaction/restart read root AGENTS, NEXT, the current status row and current phase. Refresh Git/PR/process facts needed to proceed. Do not repeatedly reload the entire repository, old package corpus, all instructions and logs.

## Mandatory all-document update at EVERY phase completion
This is a required delivery step, not optional cleanup after the final phase.

1. **Update `ROADMAP.md`, `CHANGELOG.md`, `README.md`, `docs/STATUS.md`, the plan's `last_reviewed_phase`/`last_reviewed_on` metadata (and scope where genuinely changed), and the execution/cursor records.** Record actual delivered behavior, verification, limitations and the next phase. The changelog is append-preserving: update Unreleased/phase entries with implemented changes; do not keep everything “planned” after implementation. A new phase is not automatically a new product version.
2. **Inventory and review every active first-party documentation file**, including root docs, user/developer guides, current implementation/architecture/config/API/installer/testing/parity docs, adopted phase instructions and relevant GitHub issue/PR templates. Include new/renamed docs. This is the meaning of “all docs”—not only the files Codex happened to remember.
3. **Update affected content and examples** to match implementation. For every other active first-party doc, refresh a single concise existing phase-review block (phase/date/status link) rather than creating meaningless prose changes. All active docs receive an actual phase review update; a shared index alone is not a substitute for the user's requirement. Preserve the document's substantive instructions where still correct.
4. Check working links, commands, settings names, page names, data paths, version distinctions and acceptance wording. Correct old statements that richer functional native telemetry is deferred. Do not fabricate a passing feature or unsupported installer download link.
5. Record coverage in the phase receipt and `DOCUMENTATION-INDEX.md`: file, `content_updated` or `review_stamp_updated`, and a short reason. Include an exclusion section for immutable historical evidence, archived superseded plans, vendored third-party docs, legal notices and dependency licenses. **Do not rewrite historical evidence, change third-party notices, or churn immutable snapshots.** Mark their containing index as historical/reference instead.

Suggested single block, adjusted to valid relative links for the file's directory:

```markdown
<!-- alpha-phase-review:start -->
Phase review: R2 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
```

Use the actual completion date, not the example date. A historical PR merge timestamp or test receipt must never be changed by this block. Machine-readable operational documentation is updated through its appropriate fields, not by injecting Markdown comments into JSON. Do not refresh a source-code file merely to stamp documentation.

## Keep active and historic guidance from competing
Adopt the package's current docs under `docs/implementation/alpha-recovery/`. Update current root links and governing AGENTS to use them. Old `.forge-codex/instructions`, prior P-phase instructions and stored Qwen packages are history unless a specific technical reference is useful. Add a concise supersession pointer to their active entry/index when necessary; do not delete the working build helper's `.forge-codex/state/toolchain.json` input merely because the old plan is retired. Update or retire conflicting current `docs/PARITY.md`, `docs/DEFERRED.md`, `docs/implementation/NATIVE-GUI.md`, `CONTEXT-CONTINUITY.md` and `REPOSITORY-ORGANIZATION.md` so they cannot silently reactivate old scope.

## GitHub tracking without issue sprawl
Start with one milestone and one issue per R phase. Put the slices as checklists in that issue. Reuse existing equivalent issues/milestones and add the stable plan/phase marker; do not create hundreds of microtickets. Create a separate bug issue only for a real defect needing separate ownership or a blocker that cannot fit its phase issue. No manufactured due dates or unsupported time estimates. A Projects board is optional, not a prerequisite for coding.

At every phase completion synchronize issue checkboxes with verified slices, post concise test/docs evidence, link the phase PR, and leave blocked acceptance in its actual owning issue. Close the phase issue/milestone only when its gates and normal merge requirements are satisfied. R6 remains open for missing real installation or live continuity; do not close it because earlier fixture tests passed. Remote failure leaves a specific pending synchronization entry, not a fictional GitHub ID.

## No self-referential commit loop
Before merging a phase, the committed record may say implementation verified and link the live PR. It must not cache “merged” in advance. GitHub's PR state is authoritative for that phase's delivery. After actual merge, record the immutable merge/sync receipt in its PR comment and ignored local evidence; incorporate it into repository tracking at the next ordinary phase commit. At R7, the final PR comment and final report are the post-merge receipt. Do not open another PR or dirty main solely to record its own final SHA. Status and README can truthfully say product acceptance is verified while referring to the actual PR for delivery state.
