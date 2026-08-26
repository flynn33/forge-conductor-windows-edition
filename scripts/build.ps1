[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64', 'ARM64')]
    [string]$Architecture = 'x64',

    [string[]]$Target = @(),

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [switch]$Analyze,
    [switch]$Fresh,
    [switch]$SkipClassicPreinstall
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$workspaceRoot = Split-Path -Parent $PSScriptRoot
$toolchainStatePath = Join-Path $workspaceRoot '.forge-codex\state\toolchain.json'
$triplet = if ($Architecture -ceq 'ARM64') { 'arm64-windows' } else { 'x64-windows' }
$architectureToken = $Architecture.ToLowerInvariant()
$configurePreset = "windows-msvc-$architectureToken"
$buildPreset = "windows-msvc-$architectureToken-$($Configuration.ToLowerInvariant())"

$toolchainState = $null
if (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) {
    $toolchainState = Get-Content -Raw -LiteralPath $toolchainStatePath | ConvertFrom-Json
}

function Resolve-RequiredTool {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [AllowNull()]
        [string]$StatePath
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    if (-not [string]::IsNullOrWhiteSpace($StatePath) -and
        (Test-Path -LiteralPath $StatePath -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $StatePath).Path
    }
    throw "Required tool '$Name' was not found on PATH or in .forge-codex/state/toolchain.json."
}

$vcpkgRoot = $null
if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
    if (-not (Test-Path -LiteralPath $env:VCPKG_ROOT -PathType Container)) {
        throw "VCPKG_ROOT does not name a directory: $env:VCPKG_ROOT"
    }
    $vcpkgRoot = (Resolve-Path -LiteralPath $env:VCPKG_ROOT).Path
}
elseif ($null -ne $toolchainState -and $null -ne $toolchainState.vcpkg -and
    -not [string]::IsNullOrWhiteSpace([string]$toolchainState.vcpkg.root)) {
    $vcpkgRoot = (Resolve-Path -LiteralPath ([string]$toolchainState.vcpkg.root)).Path
}
elseif ($null -ne $toolchainState -and $null -ne $toolchainState.tools -and
    -not [string]::IsNullOrWhiteSpace([string]$toolchainState.tools.vcpkg)) {
    $vcpkgRoot = Split-Path -Parent (Resolve-Path -LiteralPath ([string]$toolchainState.tools.vcpkg)).Path
}
else {
    throw 'VCPKG_ROOT is not set and the toolchain state has no vcpkg root.'
}

$vcpkgExecutable = Join-Path $vcpkgRoot 'vcpkg.exe'
$vcpkgToolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path -LiteralPath $vcpkgExecutable -PathType Leaf)) {
    throw "vcpkg.exe is missing under VCPKG_ROOT: $vcpkgExecutable"
}
if (-not (Test-Path -LiteralPath $vcpkgToolchain -PathType Leaf)) {
    throw "The vcpkg CMake toolchain is missing: $vcpkgToolchain"
}
$env:VCPKG_ROOT = $vcpkgRoot

$cmakeStatePath = if ($null -ne $toolchainState -and $null -ne $toolchainState.tools) {
    [string]$toolchainState.tools.cmake
}
else {
    $null
}
$cmake = Resolve-RequiredTool -Name 'cmake.exe' -StatePath $cmakeStatePath

if (-not $SkipClassicPreinstall) {
    $classicConfig = Join-Path $vcpkgRoot "installed\$triplet\share\nlohmann_json\nlohmann_jsonConfig.cmake"
    if (-not (Test-Path -LiteralPath $classicConfig -PathType Leaf)) {
        Write-Host "Preinstalling nlohmann-json:$triplet in vcpkg classic mode for the sealed Forsetti build."
        & $vcpkgExecutable install "nlohmann-json:$triplet" --classic
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg classic-mode preinstall failed for nlohmann-json:$triplet (exit $LASTEXITCODE)."
        }
        if (-not (Test-Path -LiteralPath $classicConfig -PathType Leaf)) {
            throw "vcpkg reported success but the classic package config is missing: $classicConfig"
        }
    }
}

$configureArguments = @('--preset', $configurePreset)
if ($Fresh) {
    $configureArguments += '--fresh'
}
if ($Analyze) {
    $configureArguments += '-DFORGE_ENABLE_ANALYZE=ON'
}

Push-Location $workspaceRoot
try {
    Write-Host "Configuring $Architecture $Configuration with preset $configurePreset."
    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for $Architecture $Configuration (exit $LASTEXITCODE)."
    }

    $buildArguments = @('--build', '--preset', $buildPreset, '--parallel', [string]$Parallel)
    if ($Target.Count -gt 0) {
        $buildArguments += '--target'
        $buildArguments += $Target
    }

    Write-Host "Building $Architecture $Configuration with preset $buildPreset."
    & $cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for $Architecture $Configuration (exit $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
