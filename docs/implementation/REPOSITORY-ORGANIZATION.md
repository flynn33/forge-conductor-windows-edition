# Organize once, then build
## Preserve the actual work
Start with git status, current branch/commit and a comparison against the audit SHA.
Do not force-reset main, discard local changes, close unrelated pull requests or recreate a second repository.
Use a focused branch and the configured Git identity. Preserve existing license/notice decisions;
this package does not grant new third-party redistribution rights or direct a public release.

## One active instruction authority
Adopt root AGENTS.md, README.md, ROADMAP.md, docs/ALPHA-SCOPE.md and docs/STATUS.md with a reviewed diff.
The owner scope supersedes old Qwen/Codex gate policies. Remove auto-loaded duplicate/nested agent rules,
old quota controls and stale selectors from the active workflow; replace their entry points with short links
that say superseded by root Alpha instructions. Keep old content in Git history or a clearly non-executable
historical archive, not in deployed Resources, prompts, CLI help, current docs or the build input search path.

Do not move/delete .forge-codex/state/toolchain.json while current build/test scripts still read it.
Extract needed immutable source-lock/baseline records into a small docs/reference area, preserve their provenance,
and update consumers before pruning old generated ledgers. Reuse historical inventories; do not rescan everything.
Update references in AGENTS, prompts, .vscode tasks, README and scripts so no entry point quietly invokes the old all-gates program.

## Clean the tracked tree
Root CMakeCache.txt, CMakeFiles and .tmp-blob/.tmp-lf check artifacts are generated/machine-specific.
First confirm exact tracked paths and no source content, then remove only those from tracking using reviewed
`git rm --cached` paths. Keep the working build if useful. Ignore out/build, staging/dist logs, local state,
PFX/private signing material, credentials and restored dependency payloads. Do not ignore source CMake files or
hide failing tests. Do not commit another vendor archive without checking the existing accepted distribution policy.

Keep src/include/tests organization largely intact. Do not refactor a 150 KB CMake file solely for tidiness;
make a small app/integration include and move larger test lists later only if it improves the current build.
Keep backend, GUI and packaging versions aligned through one version source.
Check `git check-ignore` for the new .forge-alpha files; deliberately track only the small next-action/status records,
not transient logs, local credentials or a recreated evidence tree.

## GitHub plan
Read existing labels, milestones, issues and open PRs before changes. Use github/plan.json and
Sync-GitHubPlan.ps1 in preview mode first; then apply creation/reconciliation for this package's markers.
Reuse matching human-owned milestones, preserve their descriptions/status/dates, and report title conflicts.
Do not reset closed issues, overwrite discussion, or close unrelated milestones. No due dates are fabricated.

Sixteen prepared issues map to seven Alpha milestones. Each issue has scope, paths, a done condition,
a small verification expectation and dependencies. Use labels for Alpha/deferred, priority and area.
The script preserves existing issue bodies/status and prints matches; update an existing issue deliberately after review.
Do not make GitHub Projects v2, org permissions, branch-protection edits or admin APIs prerequisites to implementing code.
An optional board can simply show Backlog/In progress/Blocked/Done using these issues.

Create a lightweight Windows CI workflow only when current repository delivery needs it and clean-clone restoration is reproducible:
checkout -> verified dependency restore/cache -> build selected production targets -> selected existing fast tests ->
upload failure logs. Use a documented Windows runner/toolchain, no success-on-skip filtering and no all-gates/ARM64 matrix.
Add packaging as a manual or release workflow once signing is available. Live model/interactive GUI acceptance is a
separate actual-host result, not something falsely certified by a headless hosted runner.
Do not check in a workflow that references unimplemented scripts; finish the corresponding entry points first.

## Public-facing docs
README: truthful status, screenshot only after actual GUI exists, install artifact link only after it is built,
quick start, development commands and roadmap. STATUS: concise current facts and blockers. Roadmap: milestones.
User guide: real project/provider/deploy/run/reset workflow. Build/Test/Install: exact executed commands.
Release notes: known limitations and actual version. Documentation must not label planned capabilities completed.

GitHub write permission failure is an administration blocker, not permission to stop implementing the Alpha.
Retain the local plan and list pending remote updates in status; do not invent a successful remote change.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
