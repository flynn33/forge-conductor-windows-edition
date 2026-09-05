[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [Parameter(Mandatory)][string]$Microtask,
    [Parameter(Mandatory)][ValidateSet('passed','in_progress','blocked','failed')][string]$Status,
    [Parameter(Mandatory)][string]$Objective,
    [string[]]$VerifiedFacts = @(),
    [string[]]$FilesChanged = @(),
    [string[]]$Commands = @(),
    [string[]]$Evidence = @(),
    [string[]]$Decisions = @(),
    [string[]]$Blockers = @(),
    [Parameter(Mandatory)][string]$NextAction,
    [string]$NextMicrotask,
    [string]$Role = 'builder'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $Repository | Out-Null
$lock = Get-QwenWorkspaceLock -Repository $Repository
$assignmentResult = & "$PSScriptRoot\Assert-QwenActiveAssignment.ps1" -Repository $Repository | ConvertFrom-Json
if ([string]$assignmentResult.microtask_id -ne $Microtask) { throw "Active assignment is $($assignmentResult.microtask_id), not $Microtask." }
if (@($FilesChanged).Count -gt 0) {
    & "$PSScriptRoot\Assert-QwenWritePaths.ps1" -Repository $Repository -Microtask $Microtask -Paths $FilesChanged | Out-Null
}

$statePath = Join-Path $Repository '.forge-qwen\state\run-state.json'
$state = Read-Json -Path $statePath
$id = 'handoff-' + $Microtask + '-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
$head = ''
$statusText = ''
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
if ($git) {
    $head = (& $git.Source -C $Repository rev-parse HEAD 2>$null | Out-String).Trim()
    $statusText = (& $git.Source -C $Repository status --short --branch 2>$null | Out-String).Trim()
}

$body = [ordered]@{
    schema_version = 1
    mission_id = $lock.mission_id
    handoff_id = $id
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    target_repository = $Repository
    memory_namespace = $lock.memory_namespace
    assignment_id = $state.active_assignment
    microtask_id = $Microtask
    created_utc = Get-UtcNow
    status = $Status
    objective = $Objective
    verified_facts = $VerifiedFacts
    files_changed = $FilesChanged
    commands = $Commands
    evidence = $Evidence
    decisions = $Decisions
    blockers = $Blockers
    next_action = $NextAction
    git_state = [ordered]@{ head = $head; status = $statusText; branch = $lock.expected_branch }
    tool_binding_revision = $state.tool_binding_revision
    ledger_head = $state.last_event_hash
}
$body['checksum'] = Get-StringSha256 -Text ($body | ConvertTo-Json -Depth 100 -Compress)
$path = Join-Path $Repository ('.forge-qwen\state\handoffs\' + $id + '.json')
Write-JsonAtomic -Path $path -Value $body

$state.latest_handoff = Get-RelativePathPortable -Base $Repository -Target $path
$state.updated_utc = Get-UtcNow
Write-JsonAtomic -Path $statePath -Value $state
Add-QwenLedgerEvent -Repository $Repository -Role $Role -Microtask $Microtask -Action 'handoff_created' -Data @{ path = $state.latest_handoff; checksum = $body.checksum; fingerprint = $lock.repository_fingerprint } | Out-Null

$cursor = & "$PSScriptRoot\Write-QwenMemoryCursor.ps1" -Repository $Repository -Microtask $Microtask -HandoffPath $state.latest_handoff -HandoffChecksum $body.checksum -Blockers $Blockers -NextAction $NextAction | ConvertFrom-Json
$resolvedNext = if ($NextMicrotask) { $NextMicrotask } else { $null }
$result = [ordered]@{
    schema_version = 1
    mission_id = $lock.mission_id
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    microtask_id = $Microtask
    status = $Status
    handoff = $state.latest_handoff
    memory_cursor_file = '.forge-qwen/state/MEMORY_CURSOR.json'
    memory_cursor_key = $cursor.key
    next_microtask = $resolvedNext
    summary = $NextAction
}
Write-JsonAtomic -Path (Join-Path $Repository '.forge-qwen\state\session-result.json') -Value $result
$result | ConvertTo-Json -Depth 30
