# GitHub tracking adoption

`plan.json` provides desired phase milestones/issues and stable markers. It does not claim that any was created. Enumerate actual repository records through available authenticated GitHub tools/CLI, compare markers/titles and reuse existing compatible records. Preserve other issues, milestones and labels. Add missing records only once. Record actual returned IDs and URLs in the existing repository ledger/plan; never guess numbers or use a failed API response as evidence of absence.

Use one milestone and one phase issue per R phase, with the slice checklist in the issue. Do not create one issue per gate or hundreds of microtasks. A real independent bug may have its own issue linked to the appropriate phase. Before closing the milestone/issue, reconcile the functional gates and actual PR delivery state; an issue closed by a PR keyword does not by itself prove acceptance.

Every phase PR targets main and is pushed with the verified owner's account/author identity. GitHub administrative tracking access, Git transport authentication and commit author strings are separate checks. Use existing credentials without exposing them. Lack of a milestone-writing route should be recorded specifically; it does not stop local source edits or turn uncreated milestones into completed work.

The normative workflow is [owner-authenticated Git](../instructions/GIT-WORKFLOW.md), including normal review/merge authority, preserving dirty work and post-merge tracked-source equality. The package does not grant permission to bypass branch rules or publish a public Release. The connected review observed the merged foundation PR but did not create or edit any remote record.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
