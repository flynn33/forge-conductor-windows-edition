[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][ValidatePattern("^P\d{2}$")][string]$Phase,
    [Parameter(Mandatory)][ValidateSet("not_started","in_progress","passed","failed","blocked_external")][string]$Status,
    [string[]]$Evidence = @(),
    [string]$Role = "builder"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$stateRoot = Join-Path $WorkspaceRoot ".forge-codex\state"
$path = Join-Path $stateRoot "run-state.json"
$run = Read-JsonFile $path
$entry = $run.phases | Where-Object id -eq $Phase | Select-Object -First 1
if (-not $entry) { throw "Unknown phase: $Phase" }
$entry.status = $Status
$entry.evidence = @($Evidence)
$run.active_phase = $Phase
$run.active_role = $Role
$run.updated_at = Get-UtcTimestamp
Write-JsonFileAtomic -Path $path -Value $run
Add-LedgerEvent -StateRoot $stateRoot -Role $Role -Phase $Phase -Action "phase_status" -Data @{
    status=$Status; evidence=$Evidence
} | Out-Null
