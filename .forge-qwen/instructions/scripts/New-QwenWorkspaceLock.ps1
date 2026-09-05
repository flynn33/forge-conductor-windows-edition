[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$SourceRootsPath,
    [Parameter(Mandatory)][string]$ExpectedBranch
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"

$Repository=Get-NormalizedFullPath -Path $Repository
$PackageRoot=Get-NormalizedFullPath -Path $PackageRoot
if ((Test-SamePath -Left $Repository -Right $PackageRoot) -or (Test-PathInside -Path $Repository -Root $PackageRoot)) {
    throw 'The target repository cannot be the instruction package or a child of it.'
}
$statePath=Join-Path $Repository '.forge-qwen\state\run-state.json'
$state=Read-Json $statePath
$roots=Read-Json $SourceRootsPath
foreach($property in $roots.PSObject.Properties){
    $value=[string]$property.Value
    if($value -and (Test-PathInside -Path $Repository -Root $value)){
        throw "The writable target repository cannot be inside source baseline '$value'."
    }
}
$manifest=Join-Path $PackageRoot 'MANIFEST.json'
$sourceHashes=Join-Path $PackageRoot 'inputs\SOURCE-HASHES.json'
$manifestHash=if(Test-Path -LiteralPath $manifest){Get-Sha256 $manifest}else{'0'*64}
$sourceHash=Get-Sha256 $sourceHashes
$mission='forge-conductor-windows11-port-qwen-guided-v2'
$canonical=($mission+"`n"+$state.run_id+"`n"+$Repository.ToLowerInvariant()+"`n"+$manifestHash+"`n"+$sourceHash)
$fingerprint=Get-StringSha256 -Text $canonical
$namespace='forge_windows_port::'+$state.run_id+'::'+$fingerprint.Substring(0,16)
$readOnly=@(
    $PackageRoot,
    (Join-Path $Repository '.forge-inputs'),
    (Join-Path $Repository '.forge-qwen\instructions')
)
$sourceRootValues=@()
foreach($property in $roots.PSObject.Properties){if([string]$property.Value){$sourceRootValues+=@(Get-NormalizedFullPath -Path ([string]$property.Value))}}
$readOnly+=@($sourceRootValues)
$forbidden=@(
    (Join-Path $PackageRoot 'work'),
    (Join-Path $Repository '.forge-inputs'),
    (Join-Path $Repository '.forge-qwen\instructions')
)
foreach($source in $sourceRootValues){
    $forbidden+=@($source,(Join-Path $source '.forge-codex'),(Join-Path $source '.forsetti\remediation'),(Join-Path $source '.forsetti\remediation-v2'),(Join-Path $source '.forsetti\remediation-v3'))
}
$lock=[ordered]@{
    schema_version=1
    mission_id=$mission
    run_id=[string]$state.run_id
    target_os='Windows 11'
    target_language='C++20'
    target_repository=$Repository
    repository_fingerprint=$fingerprint
    expected_branch=$ExpectedBranch
    memory_namespace=$namespace
    bootstrap_completed=$true
    package_root=$PackageRoot
    package_manifest_sha256=$manifestHash
    source_hashes_sha256=$sourceHash
    source_roots=$roots
    read_only_roots=@($readOnly|Select-Object -Unique)
    writable_roots=@($Repository,(Join-Path $Repository '.forge-qwen\state'))
    forbidden_roots=@($forbidden|Select-Object -Unique)
    forbidden_path_markers=@('.forge-codex','.forsetti\remediation','.forsetti/remediation')
    created_utc=Get-UtcNow
}
$path=Join-Path $Repository '.forge-qwen\state\WORKSPACE_LOCK.json'
Write-JsonAtomic -Path $path -Value $lock
Write-AtomicUtf8 -Path (Join-Path $Repository '.forge-qwen\WINDOWS_PORT_TARGET.txt') -Content ("mission_id=$mission`nrun_id=$($state.run_id)`nfingerprint=$fingerprint`ntarget=$Repository`n")
$state.mission_id=$mission
$state.workspace_fingerprint=$fingerprint
$state.target_repository=$Repository
$state.expected_branch=$ExpectedBranch
$state.memory_namespace=$namespace
$state.workspace_verified=$false
$state.active_microtask='BOOT-WORKSPACE-000'
Write-JsonAtomic -Path $statePath -Value $state
$resultPath=Join-Path $Repository '.forge-qwen\state\session-result.json'
if(Test-Path -LiteralPath $resultPath){
    $result=Read-Json $resultPath
    $result.mission_id=$mission
    $result.run_id=[string]$state.run_id
    $result.workspace_fingerprint=$fingerprint
    $result.microtask_id='BOOT-WORKSPACE-000'
    $result.status='in_progress'
    $result.handoff=''
    $result.memory_cursor_file='.forge-qwen/state/MEMORY_CURSOR.json'
    $result.memory_cursor_key=($namespace+'::cursor')
    $result.next_microtask=$null
    $result.summary='Workspace lock created; verify BOOT-WORKSPACE-000.'
    Write-JsonAtomic -Path $resultPath -Value $result
}
Add-QwenLedgerEvent -Repository $Repository -Role 'architect' -Microtask 'BOOT-WORKSPACE-000' -Action 'workspace_lock_created' -Data @{fingerprint=$fingerprint;target=$Repository;memory_namespace=$namespace}|Out-Null
$lock|ConvertTo-Json -Depth 100
