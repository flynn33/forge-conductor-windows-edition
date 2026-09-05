[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [switch]$Force,
    [switch]$AutonomousApi,
    [switch]$SkipToolchainRepair
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
& "$PSScriptRoot\Validate-Package.ps1" -PackageRoot $PackageRoot
if(-not(Test-Windows11)){throw 'Windows 11 build 22000 or newer is required.'}
$PackageRoot=Get-NormalizedFullPath -Path $PackageRoot
$WorkspaceRoot=Get-NormalizedFullPath -Path $WorkspaceRoot
if((Test-SamePath -Left $WorkspaceRoot -Right $PackageRoot) -or (Test-PathInside -Path $WorkspaceRoot -Root $PackageRoot)){
    $WorkspaceRoot=Join-Path (Split-Path -Parent $PackageRoot) ('Forge-Conductor-Windows-'+(Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss'))
}
if(Test-Path -LiteralPath $WorkspaceRoot){
    $existingLock=Join-Path $WorkspaceRoot '.forge-qwen\state\WORKSPACE_LOCK.json'
    $nonEmpty=@(Get-ChildItem -LiteralPath $WorkspaceRoot -Force -ErrorAction SilentlyContinue).Count -gt 0
    if($nonEmpty -and -not(Test-Path -LiteralPath $existingLock)){
        $WorkspaceRoot=$WorkspaceRoot+'-guided-'+(Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
        Write-Warning "Requested workspace was nonempty and not lock-bound. A safe new target was selected: $WorkspaceRoot"
    }
    elseif(Test-Path -LiteralPath $existingLock){
        $existing=Read-Json $existingLock
        if($existing.mission_id -ne 'forge-conductor-windows11-port-qwen-guided-v2' -or -not (Test-SamePath -Left ([string]$existing.target_repository) -Right $WorkspaceRoot)){
            $WorkspaceRoot=$WorkspaceRoot+'-guided-'+(Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
            Write-Warning "Requested workspace belonged to a different mission/run. A safe new target was selected: $WorkspaceRoot"
        }
    }
}
New-Item -ItemType Directory -Force -Path $WorkspaceRoot|Out-Null
$instructionRoot=Join-Path $WorkspaceRoot '.forge-qwen\instructions'
$inputRoot=Join-Path $WorkspaceRoot '.forge-inputs'
New-Item -ItemType Directory -Force -Path $instructionRoot,$inputRoot|Out-Null
foreach($file in @('AGENTS.md','QWEN_SYSTEM_PROMPT.txt','READ-THIS-FIRST-QWEN.txt','RECOVER-QWEN-NOW.txt')){
    Copy-Item -LiteralPath (Join-Path $PackageRoot $file) -Destination (Join-Path $WorkspaceRoot $file) -Force
}
Copy-Item -LiteralPath (Join-Path $PackageRoot 'QWEN_EXECUTION_PROMPT.md') -Destination (Join-Path $WorkspaceRoot '.forge-qwen\QWEN_EXECUTION_PROMPT.md') -Force
foreach($file in @('AGENTS.md','QWEN_SYSTEM_PROMPT.txt','QWEN_EXECUTION_PROMPT.md','READ-THIS-FIRST-QWEN.txt','RECOVER-QWEN-NOW.txt','START-HERE.md','README.md','GUIDED-V2-CHANGES.md')){Copy-Item -LiteralPath (Join-Path $PackageRoot $file) -Destination (Join-Path $instructionRoot $file) -Force}
foreach($name in @('qwen','lmstudio','architecture','docs','governance','plans','schemas','scripts','rag','templates','qwen-skills','evidence')){
    $source=Join-Path $PackageRoot $name;$destination=Join-Path $instructionRoot $name
    if(Test-Path -LiteralPath $destination){Remove-Item -LiteralPath $destination -Recurse -Force}
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}
$archives=Join-Path $inputRoot 'archives';New-Item -ItemType Directory -Force -Path $archives|Out-Null
foreach($name in @('Forge-Conductor-MacOS-main.zip','Forsetti-Framework-Windows-main.zip','forsetti-agentic-edition-main.zip','Forge-Conductor-Audit-Bundle.zip')){
    $source=Join-Path $PackageRoot ('inputs\'+$name);if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination (Join-Path $archives $name) -Force}
}
Copy-Item -LiteralPath (Join-Path $PackageRoot 'inputs\SOURCE-HASHES.json') -Destination (Join-Path $archives 'SOURCE-HASHES.json') -Force
$extracts=@(@('Forge-Conductor-MacOS-main.zip','macos'),@('Forsetti-Framework-Windows-main.zip','forsetti-framework'),@('forsetti-agentic-edition-main.zip','forsetti-agentic'),@('Forge-Conductor-Audit-Bundle.zip','audit'))
foreach($entry in $extracts){
    $archive=Join-Path $archives $entry[0];if(-not(Test-Path -LiteralPath $archive)){continue}
    $destination=Join-Path $inputRoot $entry[1]
    if($Force -and (Test-Path -LiteralPath $destination)){Remove-Item -LiteralPath $destination -Recurse -Force}
    if(-not(Test-Path -LiteralPath $destination)){Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force}
}
function Resolve-ExtractedRoot([string]$Path){$root=Get-ChildItem -LiteralPath $Path -Directory|Where-Object{$_.Name -ne '__MACOSX'}|Select-Object -First 1;if($root){return $root.FullName};return $Path}
$roots=[ordered]@{macos=(Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'macos'));forsetti_framework=(Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'forsetti-framework'));forsetti_agentic=(Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'forsetti-agentic'));audit=(Join-Path $inputRoot 'audit')}
$rootsPath=Join-Path $inputRoot 'source-roots.json';Write-JsonAtomic -Path $rootsPath -Value $roots
$template=Join-Path $instructionRoot 'templates\project'
Get-ChildItem -LiteralPath $template -File -Recurse|ForEach-Object{$relative=Get-RelativePathPortable -Base $template -Target $_.FullName;$destination=Join-Path $WorkspaceRoot $relative;if(-not(Test-Path -LiteralPath $destination)){$parent=Split-Path -Parent $destination;if($parent){New-Item -ItemType Directory -Force -Path $parent|Out-Null};Copy-Item -LiteralPath $_.FullName -Destination $destination}}
New-Item -ItemType Directory -Force -Path (Join-Path $WorkspaceRoot '.forge-qwen\config')|Out-Null
$runnerConfig=Join-Path $WorkspaceRoot '.forge-qwen\config\runner-config.json'
if(-not(Test-Path -LiteralPath $runnerConfig)){Copy-Item -LiteralPath (Join-Path $PackageRoot 'lmstudio\runner-config.template.json') -Destination $runnerConfig}
& "$PSScriptRoot\Initialize-Qwen-State.ps1" -Repository $WorkspaceRoot -PackageRoot $PackageRoot -Force:$Force|Out-Null
$state=Read-Json (Join-Path $WorkspaceRoot '.forge-qwen\state\run-state.json')
$branch='forge-windows-port-'+([string]$state.run_id).Substring(0,8)
$git=Get-Command git.exe -ErrorAction SilentlyContinue;if(-not $git){$git=Get-Command git -ErrorAction SilentlyContinue}
if(-not $git -and -not $SkipToolchainRepair){try{& "$PSScriptRoot\Provision-Toolchain.ps1";Update-ProcessPathFromRegistry;$git=Get-Command git.exe -ErrorAction SilentlyContinue}catch{}}
if(-not $git){throw 'Git is required to create the isolated Windows target repository.'}
if(-not(Test-Path -LiteralPath (Join-Path $WorkspaceRoot '.git'))){
    & $git.Source -C $WorkspaceRoot init -b $branch 2>$null
    if($LASTEXITCODE -ne 0){& $git.Source -C $WorkspaceRoot init|Out-Null;& $git.Source -C $WorkspaceRoot checkout -B $branch|Out-Null}
}else{
    $current=(& $git.Source -C $WorkspaceRoot branch --show-current 2>$null|Out-String).Trim()
    $oldLock=Join-Path $WorkspaceRoot '.forge-qwen\state\WORKSPACE_LOCK.json'
    if(Test-Path -LiteralPath $oldLock){$branch=[string](Read-Json $oldLock).expected_branch}
    if($current -ne $branch){& $git.Source -C $WorkspaceRoot checkout -B $branch|Out-Null}
}
& "$PSScriptRoot\New-QwenWorkspaceLock.ps1" -Repository $WorkspaceRoot -PackageRoot $PackageRoot -SourceRootsPath $rootsPath -ExpectedBranch $branch|Out-Null
& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $WorkspaceRoot|Out-Null
& "$PSScriptRoot\Prepare-QwenAssignment.ps1" -Repository $WorkspaceRoot|Out-Null
if(-not $SkipToolchainRepair){
    try{
        $vswhere=Get-VsWherePath;$hasCpp=$false
        if($vswhere){$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath;$hasCpp = -not [string]::IsNullOrWhiteSpace(($installation|Out-String).Trim())}
        $vcpkgReady=$env:VCPKG_ROOT -and (Test-Path -LiteralPath (Join-Path ([string]$env:VCPKG_ROOT) 'vcpkg.exe'))
        $needsRepair=-not(Get-Command cmake.exe -ErrorAction SilentlyContinue) -or -not $vcpkgReady -or -not $hasCpp
        if($needsRepair){& "$PSScriptRoot\Provision-Toolchain.ps1" -IncludeVisualStudioBuildTools:(-not $hasCpp);Update-ProcessPathFromRegistry}
    }catch{
        $blocker=Join-Path $WorkspaceRoot '.forge-qwen\state\blockers\toolchain-bootstrap.json'
        Write-JsonAtomic -Path $blocker -Value ([ordered]@{utc=Get-UtcNow;category='environment';message=$_.Exception.Message;retry_condition='Network, winget, privilege, or installer availability changes.'})
        Add-QwenLedgerEvent -Repository $WorkspaceRoot -Role 'architect' -Microtask 'P01-B' -Action 'toolchain_bootstrap_blocked' -Data @{evidence=(Get-RelativePathPortable -Base $WorkspaceRoot -Target $blocker)}|Out-Null
        Write-Warning "Toolchain repair blocked; source/governance work remains available. $blocker"
    }
}
& "$PSScriptRoot\Discover-Toolchain.ps1" -Repository $WorkspaceRoot|Out-Null
& "$PSScriptRoot\Discover-LMStudio.ps1" -Repository $WorkspaceRoot|Out-Null
$apiReady=[bool]$AutonomousApi
if($AutonomousApi){try{& "$PSScriptRoot\Resolve-LMStudioRunnerConfig.ps1" -Repository $WorkspaceRoot|Out-Null}catch{$apiReady=$false;$blocker=Join-Path $WorkspaceRoot '.forge-qwen\state\blockers\lmstudio-api-bootstrap.json';Write-JsonAtomic -Path $blocker -Value ([ordered]@{utc=Get-UtcNow;category='external_integration';message=$_.Exception.Message;retry_condition='LM Studio API, token, model, or plugin bindings become uniquely resolvable.'});Write-Warning "Autonomous API mode blocked: $blocker"}}
& "$PSScriptRoot\Prepare-RAG-Corpus.ps1" -Repository $WorkspaceRoot|Out-Null
Copy-Item -LiteralPath (Join-Path $WorkspaceRoot '.forge-qwen\NEXT_MESSAGE.txt') -Destination (Join-Path $WorkspaceRoot '.forge-qwen\FIRST_MESSAGE.txt') -Force
Write-Host "LOCKED WINDOWS TARGET: $WorkspaceRoot"
Write-Host "FIRST MESSAGE: $(Join-Path $WorkspaceRoot '.forge-qwen\FIRST_MESSAGE.txt')"
if($apiReady){& "$PSScriptRoot\Run-Qwen-LMStudio.ps1" -WorkspaceRoot $WorkspaceRoot}
