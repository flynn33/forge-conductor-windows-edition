# P16-039: Windows PowerShell Evidence-Runner Compatibility

Status: Accepted

Date: 2026-08-30

## Context

The first authoritative G16 record,
`20260830T201710822Z-0c97b118`, completed the configured Release/x64 target
build but exited before the `build_passed` checkpoint. The durable attempt
therefore correctly remained `build_started`; no G16 test invocation occurred.

A Windows PowerShell 5.1 replay of only the read-only post-build evidence path
located the failure in `Get-CtestArtifactEvidence`. The property form of
`Where-Object` had placed the `-CEQ` comparison value on the following line.
PowerShell 7 accepted that form, while Windows PowerShell 5.1 rejected it with
`ValueNotSpecifiedForWhereObject`.

The diagnosis also found that `Sort-Object -CaseSensitive` produced a different
source-inventory order between Windows PowerShell 5.1 and PowerShell 7. The
inventory contained identical files, byte count, and hashes, but the aggregate
source fingerprint differed by host engine.

## Decision

- Express the lifecycle-test selection with the script-block form of
  `Where-Object`, which is valid in both supported PowerShell engines.
- Sort source-fingerprint paths with `StringComparer.Ordinal`, making the
  aggregate independent of PowerShell engine and current culture.
- Treat the failed record as an interrupted authoritative attempt, not a
  passing build and not a test attempt.
- Preserve the canonical `build_started` checkpoint until the next explicit
  normal G16 invocation archives it through the existing hash-linked
  context-change rollover protocol.
- Permit that subsequent invocation to perform the governed replacement build
  and first G16 test execution.

## Evidence

- Failed command record:
  `.forge-codex/state/commands/20260830T201710822Z-0c97b118.json`
- Captured stdout:
  `.forge-codex/state/commands/20260830T201710822Z-0c97b118.stdout.txt`
- Captured stderr:
  `.forge-codex/state/commands/20260830T201710822Z-0c97b118.stderr.txt`
- Failed attempt checkpoint:
  `.forge-codex/state/evidence/P16/g16-attempt.json`
- Windows PowerShell 5.1 post-build replay after the fix:
  `WINDOWS_POWERSHELL_POST_BUILD_EVIDENCE_PASS`
- Cross-engine fingerprint after ordinal sorting:
  `96817f3321489bb7a108413fe3f906db450b3658c8f32aa03d9cff0120ed4f42`

The read-only replay does not qualify G16 and does not promote the interrupted
attempt. Only a successful recorded normal invocation may produce the passing
checkpoint and summary.

## Consequences

- The failed build remains auditable and linked to its replacement.
- A passing replacement does not erase or reinterpret the first record.
- Builder and Validator source fingerprints remain stable across Windows
  PowerShell 5.1 and PowerShell 7.
- The owner's one-pass rule remains intact: no G16 test was run by the failed
  record, and one successful replacement build is sufficient for the gate.

## References

- `.forge-codex/state/decisions/P16-038-hash-bound-g16-qualification-and-independent-finalization.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `scripts/validation/Test-G16ManagerIpcDashboard.ps1`
