# Setup research and initial fixes: verification record

Date: September 23, 2026. Baseline commit: `a3d6092afd737f52f0e720ee1f0b249da63ff969`. Source changes are uncommitted. This is development-build evidence, not installed-release qualification.

## Automated results

The consolidated runner is `scripts/validation/Invoke-ReadinessEvidence.ps1`.

Run directory: `out/validation/readiness-20260923-210744-4addfd91/`.

| Check | Result | Evidence |
|---|---|---|
| Complete native Release app and sibling services | Passed, exit 0 | `release-app.log` |
| Complete Release CTest suite | 151/151 passed, exit 0 | `release-tests.log`, `LastTest.log` |
| No-Python, native-stack, no-attribution static gates | Passed, exit 0 | `static-gates.log` |
| Package persistence contract | Passed, exit 0 | `package-persistence.log` |
| Tracked diff unchanged during consolidated checks | Passed | `summary.json`, source patches |
| Modified source/test whitespace check | Passed | `git diff --check -- src tests` |

The original run failed 1 of 151 tests because the lifecycle fixture requested the installed Manager's occupied dashboard port. The fix selects an OS-assigned loopback port and seeds only the fixture's isolated `config/config.json`. It retains the real startup, process identity, forced termination, successor, authenticated shutdown, and cleanup assertions. The installed Manager was not stopped to make the test pass.

## Native UI observations

The built executable `out/app/x64/Release/ForgeConductorApp.exe` was launched with `--alpha-root` pointing to `out/validation/setup-ui-20260923`, using a separate dashboard port. Its UI explicitly reported **Isolated profile**. The existing installed application's window remained separate.

| Case | Observed result |
|---|---|
| Open Guided setup | Navigation reached Guided setup and displayed the new collapsed help section. |
| Expand offline help | Twenty article headings and the search field were present. |
| Search `policy` | Three matching help articles were reported. |
| Expand governance article | Displayed the distinction between instruction guidance and policy enforcement, including the current local-folder limitation. |
| Search `zz-no-such-article-zz` | Displayed the no-match recovery message with suggested search terms. |
| Choose my project folder from the opening guide | Opened the native Choose an authorized project folder dialog immediately. |
| Cancel picker using Escape | Returned to guide step 2 with Browse for my folder; no folder was selected or project registered by this action. |
| Stop isolated service | Navigation changed to Manager unavailable. Previously loaded detail cards continued showing the earlier service-active snapshot; stale-detail presentation needs explicit follow-up. |

These checks were performed through the native window accessibility interface and screenshot observation. They are bounded manual automation observations, not a checked-in repeatable GUI test suite. They do not prove every interactive control, accessibility mode, or complete first-run project workflow.

## Acceptance still open

- Automatic Manager-owned preparation and the proposed shorter default workflow.
- Policy repository intake, adoption, updates, native rules, review obligations, and enforcement across all execution routes.
- Comprehensive interactive control coverage, keyboard-only and screen-reader use, High Contrast, DPI, narrow-window layout, and error recovery.
- Independent telemetry comparisons and complete live provider/tool/continuity workflows.
- Clean exact-commit packaging, signing, installed lifecycle tests, and owner acceptance.

The readiness summary intentionally records `shippable: false`. No new package was installed, no release was published, and no review or release approval is implied.

The disposable GUI closed normally. The isolated Manager process remained after the UI stop request; its exact executable path and isolated-profile command line were checked before terminating that test process. This shutdown behavior requires investigation. The production Manager was left untouched. Source files (including the new help header) and the tested app/Manager binaries are hashed in `change-and-binary-hashes.json` in the run directory.
