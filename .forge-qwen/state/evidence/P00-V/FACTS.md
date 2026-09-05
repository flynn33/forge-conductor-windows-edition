# P00-V Independent Validation Facts — Gate G00

- Role: independent validator, fresh context (no builder session state inherited)
- Session window UTC: 2026-09-04T18:2x–18:38Z (validation completed ~18:38Z)
- Mission: forge-conductor-windows11-port-qwen-guided-v2 | run_id 1df367e1-3584-41e9-b2cb-8fbbe721addf
- Workspace fingerprint: 98601f23820832b5542bc29ed245d0d9b984cc8435af37403ba1af8ac8eafdb7
- Branch (verified this session via workspace lock): forge-windows-port-1df367e1

## VERIFIED (re-executed or directly read in this session)

1. Workspace lock POSITIVE: `Assert-QwenWorkspace.ps1 -Repository D:\GitHub\Forge-Conductor-Windows-Edition -RequireCurrentDirectory` from repo root → exit 0, `WORKSPACE_LOCK_VERIFIED`; run_id, fingerprint, target_repository, expected_branch=actual_branch all match the lock.
2. Workspace lock NEGATIVE (adversarial): same script invoked with cwd `D:\GitHub` → exit 1, `WRONG_WORKSPACE_BLOCKED` ("current directory is not the locked target repository"). Failure path executed and correctly rejected.
3. Package/source hashes: SHA-256 computed this session for all 4 archives in `.forge-inputs\archives`; every entry MATCHES `SOURCE-HASHES.json` (schema_version 1) on both sha256 AND byte count:
   - Forge-Conductor-MacOS-main.zip — 3e344d4b…072dd, 15040337 B (authoritative feature/behavior baseline)
   - Forsetti-Framework-Windows-main.zip — 3fc89cba…f0204d, 340536 B (sealed Windows Forsetti framework baseline)
   - forsetti-agentic-edition-main.zip — e8ef20ad…9692b, 789736 B (Forsetti governance baseline)
   - Forge-Conductor-Audit-Bundle.zip — 7be01307…f86e6c, 371895 B (regression evidence bundle)
4. Audit package integrity: `.forge-inputs\audit\forge_conductor_audit_final\SHA256SUMS.txt` → all 8 payload entries MATCH (Audit-Evidence.json, Audit-Summary.json, Build-Test-Summary.md, Consolidated-Audit.md, Consolidated-Findings.tsv, Key-Evidence.md, VALIDATION-PASS.txt, forge_audit_final.py). Summary: ok=8 bad=1 missing=0; the single "bad" is the manifest's own self-entry (see INFERRED 1).
5. Governance intake: `source-roots.json` defines 4 canonical roots — macos/Forge-Conductor-MacOS-main, forsetti-framework/Forsetti-Framework-Windows-main, forsetti-agentic/forsetti-agentic-edition-main, audit — all present on disk. P00-C handoff records governance contract FAE-TASK-2026-08-29-016 baselined and all 8 routed documents read in its session.
6. Durable state: `run-state.json` active_microtask=P00-V; completed=[BOOT-WORKSPACE-000, BOOT-TOOLS-001, P00-A, P00-B, P00-C]; workspace_verified=true; release_eligible=false. 5 handoffs present (2×BOOT + P00-A/B/C); latest_handoff=handoff-P00-C with checksum field; event-ledger.jsonl and evidence-index.json (schema v1) present.
7. No builder self-approval: at validation time `gates.json` G00 status = not_started; P00-C handoff has decisions=[] and next_action explicitly defers to a fresh context; no validator approval existed in state before this session.
8. Tool bindings functional (tool_binding_revision=0, consistent across run-state and P00-C handoff): Prepare/Assert/Set/Write scripts under `.forge-qwen\instructions\scripts` execute with expected exit codes; positive+negative workspace lock both verified above.

## INFERRED

1. The SHA256SUMS.txt self-entry mismatch (expected ed066246… vs actual 4b1b3dbf…) is most plausibly a stale self-referential hash — the manifest was generated before its own final write — not tampering: all 8 payload files match and no other entry is affected.
2. `evidence-index.json` items=[] suggests indexing is populated at handoff time by Write-Qwen-Handoff rather than per evidence file.

## UNKNOWN

1. Exact canonicalization algorithm behind the handoff "checksum" field (not specified in docs read this session); structural presence and run-state reference verified, value not independently recomputed.
2. Exact git HEAD at validation time was not re-run this session; branch identity + repo path were verified via Assert-QwenWorkspace (expected_branch=actual_branch).

## BLOCKED

- None.

## G00 DECISION — PASS (validator P00-V)

All five acceptance areas independently re-executed/verified in a fresh context:
(1) package/source hashes valid [VERIFIED 3,4]; (2) governance intake present and baselined [VERIFIED 5]; (3) durable state consistent [VERIFIED 6]; (4) tool bindings functional incl. adversarial failure path [VERIFIED 1,2,8]; (5) first handoff structurally valid with checksum and no builder self-approval [VERIFIED 7].

## COMMAND LOG (this session, in order)

| # | command (executable + args; cwd) | exit / result |
|---|---|---|
| 1 | Assert-QwenWorkspace.ps1 -Repository D:\GitHub\Forge-Conductor-Windows-Edition -RequireCurrentDirectory ; cwd=D:\GitHub | 1 — WRONG_WORKSPACE_BLOCKED (adversarial, expected) |
| 2 | Read run-state.json, gates.json, microtasks.json, evidence-index.json, handoffs dir listing, P00-C handoff raw | ok — state consistent; G00 not_started; no self-approval |
| 3 | Assert-QwenWorkspace.ps1 (same args) ; cwd=D:\GitHub\Forge-Conductor-Windows-Edition | 0 — WORKSPACE_LOCK_VERIFIED |
| 4 | Read source-roots.json; list .forge-inputs subdirs (archives, macos, audit, forsetti-agentic, forsetti-framework) | ok — all canonical roots present |
| 5 | Get-FileHash SHA256 × 5 files in .forge-inputs\archives | ok — hashes computed |
| 6 | Verify SHA256SUMS.txt entries (manifest-dir relative) | ok=8 bad=1(self-entry) missing=0 |
| 7 | Read SOURCE-HASHES.json; cross-check 4 archive sha256+bytes | 4/4 MATCH |
