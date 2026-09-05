[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [Parameter(Mandatory)][string]$Microtask,
    [Parameter(Mandatory)][ValidateSet('not_started','in_progress','passed','blocked','failed')][string]$Status,
    [string[]]$Evidence = @(),
    [string]$Message,
    [string]$Role = 'builder'
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"
$Repository = Get-RepoRoot -Start $Repository
$planPath = Join-Path $Repository '.forge-qwen\state\microtasks.json'
$plan = Read-Json $planPath
$item = $plan.microtasks | Where-Object { $_.id -eq $Microtask } | Select-Object -First 1
if (-not $item) { throw "Unknown microtask: $Microtask" }

$item.status = $Status
$item.evidence = @($Evidence)
if (-not ($item.PSObject.Properties.Name -contains 'attempts')) { $item | Add-Member -NotePropertyName attempts -NotePropertyValue 0 }
if ($Status -in @('in_progress','failed')) { $item.attempts = [int]$item.attempts + 1 }
if ($Message) {
    if ($item.PSObject.Properties.Name -contains 'last_message') { $item.last_message = $Message }
    else { $item | Add-Member -NotePropertyName last_message -NotePropertyValue $Message }
}
Write-JsonAtomic -Path $planPath -Value $plan

$statePath = Join-Path $Repository '.forge-qwen\state\run-state.json'
$state = Read-Json $statePath
$state.active_microtask = $Microtask
if ($Status -eq 'passed' -and -not ($state.completed_microtasks -contains $Microtask)) { $state.completed_microtasks += @($Microtask) }
if ($Status -eq 'blocked' -and -not ($state.blocked_microtasks -contains $Microtask)) { $state.blocked_microtasks += @($Microtask) }
if ($Status -eq 'passed') { $state.blocked_microtasks = @($state.blocked_microtasks | Where-Object { $_ -ne $Microtask }) }
Write-JsonAtomic -Path $statePath -Value $state
Add-QwenLedgerEvent -Repository $Repository -Role $Role -Microtask $Microtask -Action 'microtask_status' -Data @{ status=$Status; evidence=$Evidence; message=$Message } | Out-Null
