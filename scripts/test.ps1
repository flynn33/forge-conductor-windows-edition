[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64', 'ARM64')]
    [string]$Architecture = 'x64',

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [string]$Label
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$workspaceRoot = Split-Path -Parent $PSScriptRoot
$architectureToken = $Architecture.ToLowerInvariant()
$preset = "windows-msvc-$architectureToken-$($Configuration.ToLowerInvariant())"
$buildRoot = Join-Path $workspaceRoot "out\build\windows-msvc-$architectureToken"
$toolchainStatePath = Join-Path $workspaceRoot '.forge-codex\state\toolchain.json'
$toolchainState = $null
if (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) {
    $toolchainState = Get-Content -Raw -LiteralPath $toolchainStatePath | ConvertFrom-Json
}

$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if ($ctestCommand) {
    $ctest = $ctestCommand.Source
}
elseif ($null -ne $toolchainState -and $null -ne $toolchainState.tools -and
    -not [string]::IsNullOrWhiteSpace([string]$toolchainState.tools.ctest) -and
    (Test-Path -LiteralPath ([string]$toolchainState.tools.ctest) -PathType Leaf)) {
    $ctest = (Resolve-Path -LiteralPath ([string]$toolchainState.tools.ctest)).Path
}
else {
    throw 'ctest.exe was not found on PATH or in .forge-codex/state/toolchain.json.'
}

if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -PathType Leaf)) {
    throw "The $Architecture build tree is not configured. Run scripts/build.ps1 first."
}

$arguments = @(
    '--preset', $preset,
    '--output-on-failure',
    '--no-tests=error',
    '--parallel', [string]$Parallel
)
if (-not [string]::IsNullOrWhiteSpace($Label)) {
    $arguments += '--label-regex'
    $arguments += $Label
}

Push-Location $workspaceRoot
try {
    Write-Host "Testing $Architecture $Configuration with preset $preset."
    & $ctest @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed for $Architecture $Configuration (exit $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
