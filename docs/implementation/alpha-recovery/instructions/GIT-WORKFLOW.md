# Owner-authenticated phase PRs and exact source synchronization

## The required transaction
Every R phase has one primary phase branch and one PR **targeting `main` in `flynn33/forge-conductor-windows-edition`**. Open that PR explicitly as a draft after the first verified slice so the active work is visible, then update the same PR through the phase boundary. Finish the phase's functional checks and documentation sweep before marking it ready, and record the real URL. Reuse an existing PR for the same phase; do not open duplicates on resume. If the primary PR merges before the scoped phase work is complete, preserve that PR's history and use one narrowly scoped continuation PR for the missing work. Reuse an existing continuation branch or PR before creating one.

**Merge authorization:** the owner explicitly requested phase PRs. Follow any already explicit owner authorization for Codex to merge passing PRs. Without such standing authorization, leave the PR ready for the owner's merge; do not infer permission to self-approve, bypass review or silently merge from the instruction to open a PR. A pending merge is a delivery dependency, not permission to stop independent implementation. When the owner or an authorized normal merge completes, fetch and synchronize local main immediately. Never enable auto-merge or change branch protection solely to avoid waiting. This package does not authorize public releases.

Implementation progress, each functional check, draft PR visibility, review readiness, actual merge, synchronization, and product acceptance are recorded separately. Phase delivery is complete only after its actual merge and synchronization. GitHub's actual PR state is the merge authority. Never mark a planned merge as done. E07–E10 in [Sources](../audit/SOURCES.md) document the command behavior; the repository's current settings must be rechecked.

## 1. Observe without disturbing
Run commands from the supplied D: checkout. Check exit status after each native command; a failed fetch/auth/PR command is not a receipt.

```powershell
Set-Location -LiteralPath 'D:\GitHub\Forge-Conductor-Windows-Edition'
git rev-parse --show-toplevel
git status --short --branch
git remote -v
git branch --show-current
git log -5 --format='%h %an <%ae> %s'
git diff --stat
git diff --cached --stat
git ls-files --others --exclude-standard
git fetch origin --prune
```

Confirm that origin's read and push destinations resolve to this repository; HTTPS and SSH are both acceptable. Do not rewrite a working remote unnecessarily. The audited branch `alpha/native-desktop` had already merged as PR #2, but the local checkout may still contain that branch, new changes, or a newer main. Determine actual ancestry. Do not attempt to reopen the merged PR or recreate its old commits.

Before branch changes, classify in-scope modified source, unrelated user edits, generated output and sensitive files. Preserve every class appropriately. Do not `git add -A` over an unreviewed dirty tree. Do not use blanket stashing as an invisible way to make user work disappear. A necessary stash/backup must be named, documented, non-destructive and restored or explicitly retained. Do not publish unrelated or sensitive files merely to make Git status empty. If their presence prevents final equality, report the specific unresolved state rather than claiming synchronization.

## 2. Verify the owner's actual account and author identity

```powershell
gh auth status --hostname github.com
gh api user --jq .login
git config --show-origin --get user.name
git config --show-origin --get user.email
git var GIT_AUTHOR_IDENT
git var GIT_COMMITTER_IDENT
```

The authenticated account for phase pushes/PRs must be `flynn33`, not a bot, another user, or a fork. Git author configuration alone does not authenticate a push. Prefer the user's already configured owner identity. The audited merge records `Jim Daley <94642455+flynn33@users.noreply.github.com>` as the author (S03/S23). Once the owner account is verified, that repository-verified identity is a fallback for missing or automated repository author settings; use **repository-local** config only. Do not guess an email, copy a third party's identity, change global Git identity, print tokens, or claim a bot-authenticated write is a user push. Required account access is an external blocker, not a reason to manufacture authentication.

No assistant/model signatures, generated-by notices, automated-author trailers or `Co-authored-by` entries for an assistant in new product files, commits, PRs or release notes. Keep genuine existing human attribution, licenses and third-party notices. Do not rewrite existing public history to remove old authors. Ordinary GitHub merge/signature metadata is platform metadata, not an added model attribution.

## 3. Start and preserve phase branches
Use `alpha/r0-reconcile`, `alpha/r1-managed-runs`, and the remaining slugs from the plan. Reuse an existing matching branch after validating it. Base new phases on freshly synchronized main when dependencies have merged. A branch must not be deleted merely because its original remote ref was removed after a merge.

When a PR is pending, work on an independent phase from current main. For genuinely dependent work, a successor branch may start from the reviewed parent phase head; record the dependency and do not merge it ahead of the parent. Its PR still targets main. After the parent merge, merge the refreshed main into the successor without force-pushing, resolve the resulting real conflicts, and verify its diff contains only the intended remaining work. This exception prevents an idle wait; it is not a mandate for stacked-branch infrastructure or new worktrees.

Keep shared plan/status/document updates coherent when switching phase branches. Merge the latest accepted ledger rather than overwriting another phase's progress with a stale copy.

At every phase boundary, choose from the observed state rather than waiting by default:

- Merged parent: fetch, preserve local edits, fast-forward main where possible, verify the integrated source, and begin the next slice from it.
- Pending parent with independent next work: branch from current main and implement.
- Pending parent with dependent next work: branch from the exact reviewed parent head, record its PR and head, keep the successor PR based on main with a cumulative-diff warning, and implement without merging it ahead of the parent.

After a parent lands, merge refreshed main into a dependent branch without force-pushing and verify the remaining phase diff. A pending PR is a delivery state, not an unconditional engineering stop.

## 4. Phase closeout before the PR
Run only the phase's required functional checks. Inspect the actual diff. Update the mandatory root docs and all active docs as specified in [Tracking](TRACKING-AND-DOCS.md). Record concise gate evidence with source/configuration and actual outcomes. Review exactly what will be staged, including deletion and rename effects.

Commit using the verified identity and an ordinary descriptive subject such as `feat(telemetry): add native resource sampling and Rig charts`. No attribution trailers. Use an explicit staged-path selection. Do not commit logs, user stores, signed binaries, private certificates or credentials into source history.

Push the phase branch using the owner's existing credential path. Confirm successful return and that GitHub holds the intended head before creating the PR. Example after the branch/body file and title have been set from actual phase state:

```powershell
$repository = 'flynn33/forge-conductor-windows-edition'
$branch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($branch) -or $branch -eq 'main') {
    throw 'A verified non-main phase branch is required.'
}
git push --set-upstream origin $branch
if ($LASTEXITCODE -ne 0) { throw 'Phase push failed.' }
gh pr list --repo $repository --head $branch --base main --state all
# Reuse an existing phase PR; otherwise create one with a prepared, reviewed body:
gh pr create --repo $repository --base main --head $branch `
    --title $PhaseTitle --body-file $PrBodyPath
if ($LASTEXITCODE -ne 0) { throw 'Inspect the result; do not claim a PR was created.' }
```

`$PhaseTitle` and `$PrBodyPath` are the current phase's real prepared values, not literal placeholders to run. See the [PR body template](../templates/PR-BODY.md). Set the matching phase issue/milestone and labels if those records exist. Attach concise actual GUI evidence where relevant; do not claim a screenshot was taken when only a process launch occurred.

Once the PR number is known, add it to the phase record/status and push the documentation-only metadata change to the same PR. This does not require a new application build. Link the exact final head and applicable code test results in the PR. Required regression failures prevent ready-for-merge status; a blocked external final-acceptance requirement is carried openly only where the plan explicitly reserves it for R6.

## 5. Normal merge, not a bypass
Read current required checks and review rules. Fix relevant failures. Do not disable a check merely to merge, invent a passing check, invoke a broad historical suite unless it is actually required, self-approve, or use administrative bypass. The package audit did not qualify a CI workflow; a missing CI run is not a passed run. Local Windows evidence may satisfy this plan when no repository-required remote check exists.

Where standing owner merge authority exists, use the normal GitHub merge mechanism and the permitted merge strategy. Prefer a merge commit where available to preserve phase branch history. Match the reviewed head SHA so a newly pushed, untested change cannot be accidentally merged:

```powershell
# Only after explicit standing merge authorization and all required checks/reviews.
gh pr merge $PrNumber --repo $repository --merge --match-head-commit $ReviewedHead
```

Adapt to a repository-required strategy rather than changing its settings. Never add `--admin`. Without merge authority or with a required human review pending, record `awaiting_merge`, continue independent slices, and recheck on a meaningful state change rather than polling endlessly. A final delivery cannot be declared synchronized with main while its PR is still open.

## 6. Synchronize after the actual GitHub merge
Confirm the PR is actually merged and capture its merge commit. Preserve/commit or safely segregate current work before switching. Then fetch and fast-forward; do not pull with an accidental merge or reset a divergent local main:

```powershell
git fetch origin --prune
if ($LASTEXITCODE -ne 0) { throw 'Fetch failed.' }
git switch main
if ($LASTEXITCODE -ne 0) { throw 'Preserve work and resolve the branch state.' }
git merge --ff-only origin/main
if ($LASTEXITCODE -ne 0) { throw 'Local main diverged; preserve and reconcile, never hard-reset.' }
```

A divergent main needs investigation: retain a named preservation branch for local-only commits, determine whether they are in-scope, and deliver them through the appropriate PR. Do not erase them to satisfy the equality check. Resolve conflicts by intent and rerun only the affected functionality.

## 7. Prove exact tracked-source equality
The owner wants local source and GitHub source exactly aligned. Report ref equality and working-tree cleanliness as separate observations: equality means the selected local, tracking, and GitHub refs identify the same commit/tree, while cleanliness describes tracked, staged, and untracked local files. Unrelated preserved work can make the tree dirty without disproving ref equality. Do not upload ignored build output, move local credentials, or publish live application databases to make the status empty. Respect `.gitattributes` normalization rather than demanding matching filesystem timestamps/CRLF bytes outside Git's tracked content model.

After a successful final fetch, verify the checked-out branch is `main` and compare **three independently obtained** main SHAs:

```powershell
$currentBranch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or $currentBranch -ne 'main') {
    throw 'Final synchronization requires main checked out, not merely a matching main ref.'
}
$localMain = (git rev-parse refs/heads/main).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read local main.' }
$trackingMain = (git rev-parse refs/remotes/origin/main).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read origin/main.' }
$githubMain = (gh api 'repos/flynn33/forge-conductor-windows-edition/git/ref/heads/main' --jq '.object.sha').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot independently read GitHub main.' }
if ($localMain -ne $trackingMain -or $localMain -ne $githubMain) {
    throw 'Main is not synchronized; fetch/reconcile the actual change.'
}
git diff --exit-code HEAD --
if ($LASTEXITCODE -ne 0) { throw 'Tracked working tree differs from HEAD.' }
git diff --cached --exit-code
if ($LASTEXITCODE -ne 0) { throw 'The index contains uncommitted changes.' }
git status --porcelain=v1 --untracked-files=normal
git rev-list --left-right --count main...origin/main
```

The last count must be `0 0`. Inspect all reported untracked files: no unpublished source/doc/config file is allowed at final completion. Ignored `out/` outputs and intentionally local secrets/data are excluded and preserved. At an open phase PR, compare its branch against the actual GitHub branch instead; do not misleadingly compare a feature branch to main and declare it unsynchronized simply because the PR is pending.

Record the final SHA, tree, PR URL/merge SHA, clean-state results and UTC observation time in a GitHub PR comment and an ignored local evidence receipt, then in the final report. Do not make a new tracked edit merely to embed that commit's own SHA into itself. The next phase's normal commit can incorporate the previous phase's now-known merge receipt. This avoids endless metadata commits and leaves local/main/remote identical.

<!-- alpha-phase-review:start -->
Phase review: R6 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
