[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [switch]$RequireCurrentDirectory
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-NormalizedFullPath -Path $Repository
$lock = Get-QwenWorkspaceLock -Repository $Repository
$errors = [System.Collections.Generic.List[string]]::new()

if ([string]$lock.mission_id -ne 'forge-conductor-windows11-port-qwen-guided-v2') { $errors.Add('mission_id mismatch') }
if ([string]$lock.target_os -ne 'Windows 11') { $errors.Add('target_os mismatch') }
if ([string]$lock.target_language -ne 'C++20') { $errors.Add('target_language mismatch') }
if (-not [bool]$lock.bootstrap_completed) { $errors.Add('bootstrap_completed is false') }
if (-not (Test-SamePath -Left $Repository -Right ([string]$lock.target_repository))) { $errors.Add('target_repository mismatch') }
if ($RequireCurrentDirectory -and -not (Test-SamePath -Left (Get-Location).Path -Right $Repository)) { $errors.Add('current directory is not the locked target repository') }

foreach ($required in @(
    'AGENTS.md',
    'QWEN_SYSTEM_PROMPT.txt',
    '.forge-qwen\WINDOWS_PORT_TARGET.txt',
    '.forge-qwen\state\run-state.json',
    '.forge-qwen\state\microtasks.json'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $Repository $required))) { $errors.Add("required target file missing: $required") }
}

if (Test-PathInside -Path $Repository -Root ([string]$lock.package_root)) { $errors.Add('target is inside package root') }
foreach ($root in @($lock.source_roots.PSObject.Properties.Value)) {
    if ($root -and (Test-PathInside -Path $Repository -Root ([string]$root))) { $errors.Add("target is inside source root: $root") }
}

$sourceHashCopy = Join-Path $Repository '.forge-inputs\archives\SOURCE-HASHES.json'
if (-not (Test-Path -LiteralPath $sourceHashCopy -PathType Leaf)) {
    $errors.Add('target source hash record missing')
}
elseif ((Get-Sha256 -Path $sourceHashCopy) -ne [string]$lock.source_hashes_sha256) {
    $errors.Add('target source hash record differs from workspace lock')
}

$canonical = ([string]$lock.mission_id + "`n" + [string]$lock.run_id + "`n" + $Repository.ToLowerInvariant() + "`n" + [string]$lock.package_manifest_sha256 + "`n" + [string]$lock.source_hashes_sha256)
$expected = Get-StringSha256 -Text $canonical
if ($expected -ne [string]$lock.repository_fingerprint) { $errors.Add('repository fingerprint mismatch') }
if ([string]$lock.memory_namespace -ne ('forge_windows_port::' + [string]$lock.run_id + '::' + [string]$lock.repository_fingerprint.Substring(0, 16))) { $errors.Add('memory namespace mismatch') }

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
$branch = ''
if ($git -and (Test-Path -LiteralPath (Join-Path $Repository '.git'))) {
    $branch = (& $git.Source -C $Repository branch --show-current 2>$null | Out-String).Trim()
    if ($branch -and $branch -ne [string]$lock.expected_branch) { $errors.Add("branch mismatch: $branch") }
}

if ($errors.Count) {
    $result = [ordered]@{
        valid = $false
        status = 'WRONG_WORKSPACE_BLOCKED'
        errors = @($errors)
        current_directory = (Get-Location).Path
        requested_repository = $Repository
    }
    $result | ConvertTo-Json -Depth 30
    throw ('Workspace lock validation failed: ' + ($errors -join '; '))
}

$statePath = Join-Path $Repository '.forge-qwen\state\run-state.json'
$state = Read-Json -Path $statePath
if ([string]$state.run_id -ne [string]$lock.run_id) { throw 'Run-state run ID does not match workspace lock.' }
if ([string]$state.workspace_fingerprint -ne [string]$lock.repository_fingerprint) { throw 'Run-state fingerprint does not match workspace lock.' }
$state.workspace_verified = $true
$state.updated_utc = Get-UtcNow
Write-JsonAtomic -Path $statePath -Value $state

[ordered]@{
    valid = $true
    status = 'WORKSPACE_LOCK_VERIFIED'
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    target_repository = $Repository
    expected_branch = $lock.expected_branch
    actual_branch = $branch
    memory_namespace = $lock.memory_namespace
} | ConvertTo-Json -Depth 30
