# DEFECTS — Canonical Defect Ledger

Scope: Forge Conductor Windows 11 guided port
Mission: forge-conductor-windows11-port-qwen-guided-v2
Run: 1df367e1-3584-41e9-b2cb-8fbbe721addf
Repository: D:\GitHub\Forge-Conductor-Windows-Edition

This file is the canonical ledger of defects discovered during guided execution. Each entry records the defect, root cause with evidence, and the governing decision. Entries are append-only; decisions recorded here bind all subsequent microtasks unless superseded by an operator decision recorded in this same file.

---

## DEFECT-P00A-JSON-ESCAPE — ConvertTo-Json `<>&` escaping differs between PS 5.1 and pwsh 7, making cross-version handoff verification non-deterministic

Status: RESOLVED (policy defect; standardization decision below is binding)
Recorded: 2026-09-04 (UTC), microtask P00-A closeout
Severity: High (blocks deterministic handoff verification across PowerShell versions)

### Symptom
P00-A reported "Handoff checksum mismatch" for `.forge-qwen/state/handoffs/handoff-P00-A-20260904T121711714Z.json`: the recomputed SHA256 of the ordered body did not match the stored `checksum` field when verification was attempted under a different PowerShell implementation than the one that wrote the file.

### Root cause
NOT tampering. PowerShell-version serialization difference in `ConvertTo-Json -Depth 100 -Compress`:
- Windows PowerShell 5.1 (`powershell.exe`) escapes `<`, `>`, `&` as `\u003c`, `\u003e`, `\u0026`.
- PowerShell 7 (`pwsh.exe`) does not escape these characters.

The P00-A handoff body contains a literal `>` in `commands[0]`, so the two versions serialize different byte strings and therefore produce different SHA256 digests for identical logical content. The stored checksum is the pwsh-canonical value; the file was written by pwsh and verifies cleanly under pwsh.

### Evidence
- `where.exe pwsh` → `C:\Program Files\PowerShell\7\pwsh.exe` present on this machine (2026-09-04).
- Verify-Handoff.ps1 run under pwsh 7 — all three handoffs exit 0 / valid=true:
  - `handoff-P00-A-20260904T121711714Z.json` → checksum `6904c5f9766ccf511b850dbc71b1f928205ef8aa663bae2541e69b4be2b4eac0` (matches stored value — non-tampering confirmed)
  - `handoff-BOOT-WORKSPACE-000-20260903T232854369Z.json` → `738673b44d9da78ff6650cc818d5e943ee50217fdbb36fdb0e32ee531a9d9ea0`
  - `handoff-BOOT-TOOLS-001-20260904T112534478Z.json` → `8ded29319109f7a155b9ecdaae2ac9c147eb64e2c2d26de600aa1102dae70571`
- BOOT-WORKSPACE-000 / BOOT-TOOLS-001 bodies contain no `<`, `>`, or `&` characters → they verify identically under both PS 5.1 and pwsh 7, consistent with the escaping-difference root cause (only P00-A's body is version-sensitive).
- MEMORY_CURSOR.json `latest_handoff_checksum` = `6904c5f9...` (already pwsh-canonical; no rewrite required or performed).

### Decision — STANDARDIZE (binding)
All Qwen handoff write and verify operations MUST use a single PowerShell implementation:
1. Preferred runtime: pwsh 7 at `C:\Program Files\PowerShell\7\pwsh.exe` (verified present on this machine).
2. Canonical invocation from the cmd.exe outer shell — no `&` call operator; inline `powershell -Command` quoting is mangled by the outer shell:
   `"C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -ExecutionPolicy Bypass -File <script>`
3. Never mix PS 5.1 and pwsh 7 for write/verify of the same handoff artifact; checksums are only comparable within one PowerShell version's serialization.

### Notes
- No script edits were required or permitted to resolve this defect (write scope restricted to `.forge-qwen/state/` + evidence directories).
- Reusable diagnostic left at `.forge-qwen/state/_diag-roundtrip.ps1` (safe to delete once P00 closeout is complete).
