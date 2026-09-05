[CmdletBinding()]
param([string]$Repository = (Get-Location).Path)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
$stateDirectory = Join-Path $Repository '.forge-qwen\state'
$state = Read-Json -Path (Join-Path $stateDirectory 'run-state.json')
$previous = '0' * 64
$count = 0
foreach ($line in Get-Content -LiteralPath (Join-Path $stateDirectory 'event-ledger.jsonl')) {
    if (-not $line.Trim()) { continue }
    $count++
    $event = $line | ConvertFrom-Json
    if ($event.previous_hash -ne $previous) { throw "Ledger previous hash mismatch at line $count" }
    $body = [ordered]@{
        event_id = $event.event_id
        timestamp_utc = $event.timestamp_utc
        run_id = $event.run_id
        role = $event.role
        microtask_id = $event.microtask_id
        action = $event.action
        data = $event.data
        previous_hash = $event.previous_hash
    }
    $actual = Get-StringSha256 -Text ($previous + "`n" + ($body | ConvertTo-Json -Depth 100 -Compress))
    if ($actual -ne $event.hash) { throw "Ledger hash mismatch at line $count" }
    $previous = $actual
}
if ($previous -ne $state.last_event_hash) { throw 'Ledger head mismatch.' }
[ordered]@{ valid = $true; events = $count; head = $previous } | ConvertTo-Json
