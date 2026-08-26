[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][string]$Role,
    [Parameter(Mandatory)][string]$Phase,
    [Parameter(Mandatory)][string]$Summary,
    [string[]]$Completed = @(),
    [string[]]$OpenWork = @(),
    [string[]]$NextActions = @(),
    [string[]]$Evidence = @(),
    [string[]]$Decisions = @(),
    [string[]]$Blockers = @()
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$state = Join-Path $WorkspaceRoot ".forge-codex\state"
$handoffDir = Join-Path $state "handoffs"
New-Item -ItemType Directory -Force -Path $handoffDir | Out-Null
$id = "{0}-{1}" -f ([DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssfffZ")), ([Guid]::NewGuid().ToString("N").Substring(0,8))
$jsonPath = Join-Path $handoffDir "$id.json"
$mdPath = Join-Path $handoffDir "$id.md"

$handoff = [ordered]@{
    schema_version = 1
    handoff_id = $id
    created_at = Get-UtcTimestamp
    role = $Role
    phase = $Phase
    summary = $Summary
    completed = $Completed
    open_work = $OpenWork
    next_actions = $NextActions
    evidence = $Evidence
    decisions = $Decisions
    blockers = $Blockers
    repository_status = (& git -C $WorkspaceRoot status --short --branch 2>$null | Out-String).Trim()
}
$canonical = ConvertTo-CompactJson $handoff
$bytes = [Text.Encoding]::UTF8.GetBytes($canonical)
$handoff["content_sha256"] = Get-BytesSha256 -Bytes $bytes
Write-JsonFileAtomic -Path $jsonPath -Value $handoff

$md = @"
# Continuity handoff $id

- Role: $Role
- Phase: $Phase
- Created: $($handoff.created_at)
- Content SHA-256: $($handoff.content_sha256)

## Summary

$Summary

## Completed

$($Completed | ForEach-Object { "- $_" } | Out-String)

## Open work

$($OpenWork | ForEach-Object { "- $_" } | Out-String)

## Next actions

$($NextActions | ForEach-Object { "- $_" } | Out-String)

## Evidence

$($Evidence | ForEach-Object { "- $_" } | Out-String)

## Decisions

$($Decisions | ForEach-Object { "- $_" } | Out-String)

## Blockers

$($Blockers | ForEach-Object { "- $_" } | Out-String)

## Repository status

~~~
$($handoff.repository_status)
~~~
"@
Write-AtomicUtf8 -Path $mdPath -Content $md

$runPath = Join-Path $state "run-state.json"
$run = Read-JsonFile $runPath
$run.latest_handoff = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $jsonPath
$run.updated_at = Get-UtcTimestamp
Write-JsonFileAtomic -Path $runPath -Value $run
Add-LedgerEvent -StateRoot $state -Role $Role -Phase $Phase -Action "handoff_written" -Data @{
    handoff = $run.latest_handoff
    sha256 = $handoff.content_sha256
} | Out-Null
Write-Host $jsonPath
