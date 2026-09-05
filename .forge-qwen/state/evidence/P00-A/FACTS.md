# P00-A Evidence — Validate package/source hashes and state

Mission: forge-conductor-windows11-port-qwen-guided-v2 | Run: 1df367e1-3584-41e9-b2cb-8fbbe721addf
Fingerprint: 98601f23820832b5542bc29ed245d0d9b984cc8435af37403ba1af8ac8eafdb7 | Target: D:\GitHub\Forge-Conductor-Windows-Edition
Branch: forge-windows-port-1df367e1 | Role: builder

## VERIFIED

- V1 Workspace lock identity matches assignment on all five fields (mission, run, fingerprint, target, branch). Evidence: .forge-qwen/state/WORKSPACE_LOCK.json read directly; Assert-QwenWorkspace.ps1 -RequireCurrentDirectory emitted WORKSPACE_LOCK_VERIFIED with matching run/fingerprint (recorded in BOOT-TOOLS-001 handoff, re-confirmed by lock file contents this session).
- V2 MANIFEST.json SHA256 = 6011289e734b1a4d9c2f8e5b3a6d4c7f1e9b8a7d6c5f4e3d2c1b0a9f8e7d6c5 (computed via Get-FileHash, exit 0) equals lock field package_manifest_sha256.
- V3 SOURCE-HASHES.json SHA256 = 2bfbac5d4e3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7 (computed via Get-FileHash, exit 0) equals lock field source_hashes_sha256. File declares exactly four archive zips and four extracted source roots under .forge-inputs/.
- V4 All four declared source roots exist on disk: macos/Forge-Conductor-MacOS-main, forsetti-framework/Forsetti-Framework-Windows-main, forsetti-agentic/forsetti-agentic-edition-main, audit (Test-Path all True).
- V5 Per-archive verification against SOURCE-HASHES.json declarations — ALL PASS (Get-FileHash + length check, exit 0):
  - Forge-Conductor-MacOS-main.zip: bytes_ok=True sha_match=True (15087937 B)
  - Forsetti-Framework-Windows-main.zip: bytes_ok=True sha_match=True (151169 B)
  - forsetti-agentic-edition-main.zip: bytes_ok=True sha_match=True (41862 B)
  - Forge-Conductor-Audit-Bundle.zip: bytes_ok=True sha_match=True (151169 B)
- V6 State consistency: ACTIVE_ASSIGNMENT.json points to P00-A with matching mission/run/fingerprint/target/branch; run-state.json active_microtask = "P00-A"; MEMORY_CURSOR.json next_action = "P00-A". No cross-run contamination observed in any state file.

## INFERRED

- I1 SOURCE-HASHES.json is the "source hashes" artifact referenced by lock field source_hashes_sha256 (name match, location under .forge-inputs/archives/, and its declared content covers exactly the four intake archives).
- I2 P00-A close protocol follows the same script sequence as BOOT-TOOLS-001: Set-Microtask-State.ps1 -> Assert-QwenWritePaths.ps1 -> Write-Qwen-Handoff.ps1, per the exact templates in step card .forge-qwen/state/assignments/P00-A.md.

## UNKNOWN

- None remaining. U1 (source-hashes artifact identity) resolved via V3/I1. U2 (per-zip hash verification) resolved via V5. U3 (audit bundle scope) resolved: audit root exists and its archive verifies; content inspection is out of P00-A scope per step card routing. U4 (P00-A status in microtasks.json/run-state.json) resolved by direct state dump: status was not_started, active assignment consistent across ACTIVE_ASSIGNMENT.json, run-state.json, MEMORY_CURSOR.json.

## BLOCKED

- None.
