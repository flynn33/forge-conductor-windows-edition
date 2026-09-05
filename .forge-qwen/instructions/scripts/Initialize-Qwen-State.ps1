[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$PackageRoot,
    [switch]$Force
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-NormalizedFullPath -Path $Repository
$stateDirectory=Join-Path $Repository '.forge-qwen\state'
if($Force -and (Test-Path -LiteralPath $stateDirectory)){Remove-Item -LiteralPath $stateDirectory -Recurse -Force}
New-Item -ItemType Directory -Force -Path $stateDirectory|Out-Null
foreach($name in @('decisions','blockers','handoffs','evidence','responses','profiles','installer','validation','assignments')){New-Item -ItemType Directory -Force -Path (Join-Path $stateDirectory $name)|Out-Null}
foreach($name in @('run-state.json','tool-bindings.json','evidence-index.json','session-result.json')){
    $destination=Join-Path $stateDirectory $name
    if(-not(Test-Path -LiteralPath $destination)){Copy-Item -LiteralPath (Join-Path $PackageRoot ('state\templates\'+$name)) -Destination $destination}
}
$microtasks=Join-Path $stateDirectory 'microtasks.json'
if(-not(Test-Path -LiteralPath $microtasks)){Copy-Item -LiteralPath (Join-Path $PackageRoot 'plans\qwen-microtasks.json') -Destination $microtasks}
$ledger=Join-Path $stateDirectory 'event-ledger.jsonl'
if(-not(Test-Path -LiteralPath $ledger)){Write-AtomicUtf8 -Path $ledger -Content ''}
$statePath=Join-Path $stateDirectory 'run-state.json'
$state=Read-Json $statePath
if($state.run_id -eq 'created-at-bootstrap'){
    $state.run_id=[Guid]::NewGuid().ToString('D')
    $state.created_utc=Get-UtcNow
    $state.updated_utc=$state.created_utc
    $state.status='running'
    $state.active_microtask='BOOT-WORKSPACE-000'
    Write-JsonAtomic -Path $statePath -Value $state
    Add-QwenLedgerEvent -Repository $Repository -Role 'architect' -Microtask 'BOOT-WORKSPACE-000' -Action 'state_initialized' -Data @{repository=$Repository;mission_id=$state.mission_id}|Out-Null
}
Write-Host $stateDirectory
