[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Microtask,
    [Parameter(Mandatory)][string]$HandoffPath,
    [Parameter(Mandatory)][string]$HandoffChecksum,
    [string[]]$Blockers = @(),
    [string]$NextAction = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
$lock = Get-QwenWorkspaceLock -Repository $Repository
$state = Read-Json -Path (Join-Path $Repository '.forge-qwen\state\run-state.json')
$cursorValue = [ordered]@{
    schema_version = 1
    mission_id = $lock.mission_id
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    target_repository = $Repository
    expected_branch = $lock.expected_branch
    active_microtask = $Microtask
    latest_handoff = $HandoffPath
    latest_handoff_checksum = $HandoffChecksum
    last_validated_commit = $state.last_validated_commit
    blocker_ids = @($Blockers)
    next_action = $NextAction
    updated_utc = Get-UtcNow
}
$canonical = $cursorValue | ConvertTo-Json -Depth 100 -Compress
$cursor = [ordered]@{
    schema_version = 1
    key = ([string]$lock.memory_namespace + '::cursor')
    value = $cursorValue
    value_sha256 = Get-StringSha256 -Text $canonical
    replacement_mode = 'replace_exact_key_only'
}
$path = Join-Path $Repository '.forge-qwen\state\MEMORY_CURSOR.json'
Write-JsonAtomic -Path $path -Value $cursor
$cursor | ConvertTo-Json -Depth 100
