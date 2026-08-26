[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][ValidatePattern("^G\d{2}$")][string]$Gate,
    [Parameter(Mandatory)][ValidateSet("passed","failed","blocked_external")][string]$Status,
    [Parameter(Mandatory)][string]$Acceptance,
    [string[]]$Evidence = @(),
    [string[]]$Failures = @(),
    [string]$Role = "validator",
    [string]$Phase = "P00"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$stateRoot = Join-Path $WorkspaceRoot ".forge-codex\state"
$result = [ordered]@{
    gate_id=$Gate
    status=$Status
    evaluated_at=Get-UtcTimestamp
    role=$Role
    acceptance=$Acceptance
    evidence=$Evidence
    failures=$Failures
}
Write-JsonFileAtomic -Path (Join-Path $stateRoot "gate-results\$Gate.json") -Value $result

$runPath = Join-Path $stateRoot "run-state.json"
$run = Read-JsonFile $runPath
$entry = $run.gates | Where-Object id -eq $Gate | Select-Object -First 1
if (-not $entry) { throw "Unknown gate: $Gate" }
$entry.status = $Status
$entry.evidence = @($Evidence)
$run.updated_at = Get-UtcTimestamp
Write-JsonFileAtomic -Path $runPath -Value $run
Add-LedgerEvent -StateRoot $stateRoot -Role $Role -Phase $Phase -Action "gate_result" -Data @{
    gate=$Gate; status=$Status; evidence=$Evidence; failures=$Failures
} | Out-Null
