[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [switch]$Autonomous,
    [int]$MaxIterations = 80,
    [switch]$ForceReinitialize
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

& (Join-Path $PSScriptRoot 'Validate-Package.ps1') -PackageRoot $PackageRoot
if (-not (Test-Windows11)) { throw 'This package must be bootstrapped on Windows 11 build 22000 or newer.' }

$vswhere = Get-VsWherePath
$hasCppBuildTools = $false
if ($vswhere) {
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $hasCppBuildTools = -not [string]::IsNullOrWhiteSpace(($installation | Out-String).Trim())
}
$needsBaseToolchain = `
    -not (Get-Command git.exe -ErrorAction SilentlyContinue) -or `
    -not (Get-Command cmake.exe -ErrorAction SilentlyContinue) -or `
    -not (Get-Command pwsh.exe -ErrorAction SilentlyContinue) -or `
    [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT) -or `
    -not (Test-Path -LiteralPath (Join-Path ([string]$env:VCPKG_ROOT) 'vcpkg.exe'))
if ($needsBaseToolchain -or -not $hasCppBuildTools) {
    & (Join-Path $PSScriptRoot 'Provision-Toolchain.ps1') -IncludeVisualStudioBuildTools:(-not $hasCppBuildTools)
    Update-ProcessPathFromRegistry
}
$git = Get-Command git.exe -ErrorAction Stop

New-Item -ItemType Directory -Force -Path $WorkspaceRoot | Out-Null
$codexRoot = Join-Path $WorkspaceRoot '.forge-codex'
$instructionRoot = Join-Path $codexRoot 'instructions'
$inputRoot = Join-Path $WorkspaceRoot '.forge-inputs'
New-Item -ItemType Directory -Force -Path $instructionRoot,$inputRoot | Out-Null

Copy-Item -LiteralPath (Join-Path $PackageRoot 'AGENTS.md') -Destination (Join-Path $WorkspaceRoot 'AGENTS.md') -Force
Copy-Item -LiteralPath (Join-Path $PackageRoot 'CODEX_EXECUTION_PROMPT.md') -Destination (Join-Path $codexRoot 'CODEX_EXECUTION_PROMPT.md') -Force

foreach ($name in @('architecture','docs','governance','plans','schemas','specifications','templates','codex-skills','scripts')) {
    $source = Join-Path $PackageRoot $name
    $destination = Join-Path $instructionRoot $name
    if (Test-Path -LiteralPath $destination) { Remove-Item -Recurse -Force -LiteralPath $destination }
    Copy-Item -Recurse -Force -LiteralPath $source -Destination $destination
}

$archives = Join-Path $inputRoot 'archives'
New-Item -ItemType Directory -Force -Path $archives | Out-Null
foreach ($archiveName in @(
    'Forge-Conductor-MacOS-main.zip',
    'Forsetti-Framework-Windows-main.zip',
    'forsetti-agentic-edition-main.zip',
    'Forge-Conductor-Audit-Bundle.zip'
)) {
    Copy-Item -LiteralPath (Join-Path $PackageRoot "inputs\$archiveName") -Destination (Join-Path $archives $archiveName) -Force
}
Copy-Item -LiteralPath (Join-Path $PackageRoot 'inputs\SOURCE-HASHES.json') -Destination (Join-Path $archives 'SOURCE-HASHES.json') -Force

$extracts = @(
    @{Archive='Forge-Conductor-MacOS-main.zip'; Folder='macos'},
    @{Archive='Forsetti-Framework-Windows-main.zip'; Folder='forsetti-framework'},
    @{Archive='forsetti-agentic-edition-main.zip'; Folder='forsetti-agentic'},
    @{Archive='Forge-Conductor-Audit-Bundle.zip'; Folder='macos-audit'}
)
foreach ($item in $extracts) {
    $destination = Join-Path $inputRoot $item.Folder
    if (-not (Test-Path -LiteralPath $destination) -or $ForceReinitialize) {
        if (Test-Path -LiteralPath $destination) { Remove-Item -Recurse -Force -LiteralPath $destination }
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Expand-Archive -LiteralPath (Join-Path $archives $item.Archive) -DestinationPath $destination -Force
    }
}

function Resolve-ExtractedRoot {
    param([Parameter(Mandatory)][string]$Path)
    $candidate = Get-ChildItem -LiteralPath $Path -Directory | Where-Object { $_.Name -ne '__MACOSX' } | Select-Object -First 1
    if (-not $candidate) { throw "No extracted source root found under $Path" }
    return $candidate.FullName
}
$roots = [ordered]@{
    macos = (Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'macos'))
    forsetti_framework = (Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'forsetti-framework'))
    forsetti_agentic = (Resolve-ExtractedRoot -Path (Join-Path $inputRoot 'forsetti-agentic'))
    audit = (Join-Path $inputRoot 'macos-audit')
}
Write-JsonFileAtomic -Path (Join-Path $inputRoot 'source-roots.json') -Value $roots

$template = Join-Path $instructionRoot 'templates\project'
Get-ChildItem -LiteralPath $template -Recurse -File | ForEach-Object {
    $relative = Get-RelativePathPortable -BasePath $template -TargetPath $_.FullName
    $destination = Join-Path $WorkspaceRoot $relative
    if (-not (Test-Path -LiteralPath $destination)) {
        $parent = Split-Path -Parent $destination
        if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot '.git'))) {
    & $git.Source -C $WorkspaceRoot init
    if ($LASTEXITCODE -ne 0) { throw 'git init failed.' }
}
& (Join-Path $instructionRoot 'scripts\Initialize-Run.ps1') -WorkspaceRoot $WorkspaceRoot -Force:$ForceReinitialize
& (Join-Path $instructionRoot 'scripts\Discover-Toolchain.ps1') -WorkspaceRoot $WorkspaceRoot

$compatMac = Join-Path $inputRoot 'Forge-Conductor-MacOS-main'
if (-not (Test-Path -LiteralPath $compatMac)) {
    New-Item -ItemType Junction -Path $compatMac -Target ([string]$roots.macos) | Out-Null
}
& (Join-Path $instructionRoot 'scripts\Discover-Source.ps1') -WorkspaceRoot $WorkspaceRoot

$stateRoot = Join-Path $WorkspaceRoot '.forge-codex\state'
$run = Read-JsonFile (Join-Path $stateRoot 'run-state.json')
if (-not $run.latest_handoff) {
    & (Join-Path $instructionRoot 'scripts\Write-Handoff.ps1') `
        -WorkspaceRoot $WorkspaceRoot -Role 'architect' -Phase 'P00' `
        -Summary 'Package integrity, immutable source extraction, repository initialization, toolchain discovery, and source-baseline generation completed.' `
        -Completed @('Validated package manifest and immutable source hashes','Installed governing instructions','Initialized durable state and Git repository','Generated initial source and MCP baselines') `
        -NextActions @('Execute P01 toolchain provisioning and pin exact versions','Execute P02 source archaeology and expand parity evidence') `
        -Evidence @('.forge-inputs/source-roots.json','.forge-codex/state/toolchain.json','.forge-codex/state/baseline/macos-swift-source-index.json')
}
& (Join-Path $instructionRoot 'scripts\Set-GateResult.ps1') `
    -WorkspaceRoot $WorkspaceRoot -Gate 'G00' -Status 'passed' -Phase 'P00' `
    -Acceptance 'Package, immutable source hashes, governing intake, durable state, and initial handoff validated.' `
    -Evidence @('.forge-inputs/archives/SOURCE-HASHES.json','.forge-codex/state/baseline/macos-swift-source-index.json')
& (Join-Path $instructionRoot 'scripts\Set-PhaseStatus.ps1') `
    -WorkspaceRoot $WorkspaceRoot -Phase 'P00' -Status 'passed' -Role 'architect' `
    -Evidence @('.forge-codex/state/gate-results/G00.json','.forge-codex/state/handoffs')

Write-Host "Forge Conductor Windows workspace ready: $WorkspaceRoot"
if ($Autonomous) {
    & (Join-Path $instructionRoot 'scripts\Run-Codex-Autonomously.ps1') -WorkspaceRoot $WorkspaceRoot -MaxIterations $MaxIterations
}
