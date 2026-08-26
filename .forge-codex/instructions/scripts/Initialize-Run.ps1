[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [switch]$Force
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$state = Join-Path $WorkspaceRoot ".forge-codex\state"
if ($Force -and (Test-Path -LiteralPath $state)) {
    Remove-Item -Recurse -Force -LiteralPath $state
}
New-Item -ItemType Directory -Force -Path $state | Out-Null
foreach ($name in @("decisions","blockers","handoffs","gate-results","phase-results","commands","profiles","installer")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $state $name) | Out-Null
}

$runStatePath = Join-Path $state "run-state.json"
if (-not (Test-Path -LiteralPath $runStatePath)) {
    $phases = Read-JsonFile (Join-Path $WorkspaceRoot ".forge-codex\instructions\plans\phases.json")
    $gates = Read-JsonFile (Join-Path $WorkspaceRoot ".forge-codex\instructions\plans\gates.json")
    $run = [ordered]@{
        schema_version = 1
        run_id = [Guid]::NewGuid().ToString("D").ToLowerInvariant()
        created_at = Get-UtcTimestamp
        updated_at = Get-UtcTimestamp
        status = "in_progress"
        active_role = "architect"
        active_phase = "P00"
        session_number = 0
        phases = @($phases.phases | ForEach-Object { [ordered]@{ id=$_.id; status="not_started"; evidence=@() } })
        gates = @($gates.gates | ForEach-Object { [ordered]@{ id=$_.id; status="not_started"; evidence=@() } })
        latest_handoff = $null
        open_blockers = @()
        notes = @()
    }
    Write-JsonFileAtomic -Path $runStatePath -Value $run
    Write-JsonFileAtomic -Path (Join-Path $state "evidence-index.json") -Value ([ordered]@{
        schema_version=1; created_at=Get-UtcTimestamp; items=@()
    })
    New-Item -ItemType File -Force -Path (Join-Path $state "event-ledger.jsonl") | Out-Null
    Add-LedgerEvent -StateRoot $state -Role "architect" -Phase "P00" -Action "run_initialized" -Data @{
        workspace = $WorkspaceRoot
    } | Out-Null
}
Write-Host "Run state ready: $runStatePath"
