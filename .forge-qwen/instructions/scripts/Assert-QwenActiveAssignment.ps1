[CmdletBinding()]
param([Parameter(Mandatory)][string]$Repository)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
$lock = Get-QwenWorkspaceLock -Repository $Repository
$path = Join-Path $Repository '.forge-qwen\state\ACTIVE_ASSIGNMENT.json'
$assignment = Read-Json -Path $path
$errors = [System.Collections.Generic.List[string]]::new()

if ([string]$assignment.mission_id -ne [string]$lock.mission_id) { $errors.Add('mission mismatch') }
if ([string]$assignment.run_id -ne [string]$lock.run_id) { $errors.Add('run mismatch') }
if ([string]$assignment.repository_fingerprint -ne [string]$lock.repository_fingerprint) { $errors.Add('fingerprint mismatch') }
if (-not (Test-SamePath -Left ([string]$assignment.target_repository) -Right $Repository)) { $errors.Add('target mismatch') }
if ([string]$assignment.expected_branch -ne [string]$lock.expected_branch) { $errors.Add('branch mismatch') }
if ([string]$assignment.memory_namespace -ne [string]$lock.memory_namespace) { $errors.Add('memory namespace mismatch') }
if ([string]$assignment.memory_cursor_key -ne ([string]$lock.memory_namespace + '::cursor')) { $errors.Add('memory cursor key mismatch') }

$card = if ([IO.Path]::IsPathRooted([string]$assignment.step_card)) { [string]$assignment.step_card } else { Join-Path $Repository ([string]$assignment.step_card) }
if (-not (Test-Path -LiteralPath $card -PathType Leaf)) { $errors.Add('step card missing') }

$plan = Read-Json -Path (Join-Path $Repository '.forge-qwen\state\microtasks.json')
$item = $plan.microtasks | Where-Object { $_.id -eq $assignment.microtask.id } | Select-Object -First 1
if (-not $item) { $errors.Add('microtask missing from plan') }
elseif ([string]$item.status -eq 'passed') { $errors.Add('assignment points to an already-passed microtask') }

if ($errors.Count) { throw ('ACTIVE_ASSIGNMENT_INVALID: ' + ($errors -join '; ')) }
[ordered]@{
    valid = $true
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    microtask_id = $assignment.microtask.id
    step_card = $assignment.step_card
    memory_namespace = $lock.memory_namespace
    memory_cursor_key = $assignment.memory_cursor_key
} | ConvertTo-Json -Depth 20
