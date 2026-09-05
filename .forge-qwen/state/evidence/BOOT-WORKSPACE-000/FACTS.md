# FACTS — BOOT-WORKSPACE-000

Mission: forge-conductor-windows11-port-qwen-guided-v2
Run: 1df367e1-3584-41e9-b2cb-8fbbe721addf
Fingerprint: 98601f23820832b5542bc29ed245d0d9b984cc8435af37403ba1af8ac8eafdb7

## VERIFIED

- CWD printed via tool before any Git command: D:\GitHub\Forge-Conductor-Windows-Edition
- Exact root contains all four required items (Test-Path True): AGENTS.md, QWEN_SYSTEM_PROMPT.txt, .forge-qwen/state/WORKSPACE_LOCK.json, .forge-qwen/state/run-state.json
- WORKSPACE_LOCK.json exact facts: mission_id=forge-conductor-windows11-port-qwen-guided-v2; target_os="Windows 11"; target_language=C++20; bootstrap_completed=true; target_repository=D:\GitHub\Forge-Conductor-Windows-Edition (equals CWD)
- Assert-QwenWorkspace.ps1 -RequireCurrentDirectory: exit 0, status=WORKSPACE_LOCK_VERIFIED, run_id and fingerprint match lock, expected_branch=actual_branch=forge-windows-port-1df367e1
- ACTIVE_ASSIGNMENT.json microtask=BOOT-WORKSPACE-000; mission/run/fingerprint/target/branch all equal the lock values (no FOREIGN_STALE_CONTEXT)
- No persistent memory or continuity packet was retrieved in this microtask (step 6); no stale context encountered to classify
- Read-only roots exist and are untouched: package root A:\Qwen-Projects\Forge-Conductor-Windows11-Qwen3.8-27B-4bit-Guided-v2-Instructions; .forge-inputs/{macos,forsetti-framework,forsetti-agentic,audit}; .forge-qwen/instructions (bootstrap: "Package validation passed: 276 files")
- Routed documents read in full: READ-THIS-FIRST-QWEN.txt, qwen/WORKSPACE_AUTHORITY.md, qwen/WRONG_WORKSPACE_RECOVERY.md, qwen/STEP_BY_STEP_EXECUTION.md
- Resume session (this chat): Assert-QwenWorkspace.ps1 re-run from exact root with -Repository parameter: exit 0, WORKSPACE_LOCK_VERIFIED, run_id=1df367e1-3584-41e9-b2cb-8fbbe721addf, fingerprint match, expected_branch=actual_branch=forge-windows-port-1df367e1
- Defect root-caused (prior session): Assert-QwenWritePaths.ps1 line `$relative = $relative.Replace('\','/').TrimStart([char[]]'./')` stripped ALL leading dots/slashes; '.forge-qwen/state/**' became 'forge-qwen/state/**', could never match allowed root '.forge-qwen/state/**', and also bypassed the read-only `-like` guards. Observed: WRITE_SCOPE_VIOLATION: not allowed by assignment: forge-qwen/state/evidence/BOOT-WORKSPACE-000/FACTS.md
- Operator decision (this chat, OP-DECISION-001): fix the package script rather than accept a documented exception; defect was genuine and not caused by hitting tool limits
- Fix applied to .forge-qwen/instructions/scripts/Assert-QwenWritePaths.ps1 (two latent defects on one line): (a) TrimStart([char[]]'./') stripped ALL leading dots/slashes, so dot-directory paths could never match allowed roots and also bypassed the read-only -like guards — replaced with loops stripping only leading './' sequences and leading '/' characters; (b) Replace('\\','/') used a two-backslash literal that never normalized single-backslash Windows paths — fixed to Replace('\','/')
- Post-fix verification 1 (in-scope path, forward slashes): Assert-QwenWritePaths.ps1 -Microtask BOOT-WORKSPACE-000 -Paths ".forge-qwen/state/evidence/BOOT-WORKSPACE-000/FACTS.md" → exit 0, valid=true (previously failed)
- Post-fix verification 2 (guard regression, forward slashes): same script with -Paths ".forge-qwen/instructions/scripts/Common.ps1" → WRITE_SCOPE_VIOLATION: read-only root (read-only guard intact and correctly attributed; previously this class of path slipped past the `-like` check due to defect (a))
- Post-fix verification 3 (Windows-native backslash forms): in-scope ".forge-qwen\state\evidence\BOOT-WORKSPACE-000\FACTS.md" → valid=true; read-only ".forge-qwen\instructions\scripts\Common.ps1" → WRITE_SCOPE_VIOLATION: read-only root (correctly normalized to forward slashes and attributed)

## INFERRED

- Closing this microtask with Write-Qwen-Handoff.ps1 will regenerate the next ACTIVE_ASSIGNMENT.json for the same locked run (per bootstrap flow and step-card close sequence)
- The operator-directed package repair is an authorized exception to this microtask's allowed write roots (.forge-qwen/state/**); it was directed by the operator above the assignment, not initiated by Qwen

## UNKNOWN

- none material; nothing unresolved that blocks this objective

## BLOCKED

- none. Prior blocker (WRITE_SCOPE_VIOLATION on in-scope dot-directory path) root-caused to a package-script defect, fixed per OP-DECISION-001, and verified resolved (see VERIFIED).
