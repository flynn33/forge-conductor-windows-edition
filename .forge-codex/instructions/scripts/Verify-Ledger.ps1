[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$ledger = Join-Path $WorkspaceRoot ".forge-codex\state\event-ledger.jsonl"
$previous = ("0" * 64)
$expectedSequence = 1
foreach ($line in Get-Content -LiteralPath $ledger) {
    if (-not $line.Trim()) { continue }
    $event = ConvertFrom-JsonPreserveDates -Value $line
    if ([int]$event.sequence -ne $expectedSequence) { throw "Sequence mismatch at $expectedSequence." }
    if ([string]$event.previous_hash -ne $previous) { throw "Previous hash mismatch at $expectedSequence." }
    $body = [ordered]@{
        sequence = $event.sequence
        utc = $event.utc
        role = $event.role
        phase = $event.phase
        action = $event.action
        data = $event.data
        previous_hash = $event.previous_hash
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-CompactJson $body))
    $actual = Get-BytesSha256 -Bytes $bytes
    if ($actual -ne [string]$event.event_hash) { throw "Event hash mismatch at $expectedSequence." }
    $previous = $actual
    $expectedSequence++
}
Write-Host "Ledger validation passed: $($expectedSequence - 1) event(s)."
