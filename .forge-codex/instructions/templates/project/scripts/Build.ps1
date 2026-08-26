[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [ValidateSet('x64','ARM64')][string]$Architecture = 'x64',
    [switch]$Analyze
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$archToken = $Architecture.ToLowerInvariant()
$configurePreset = "windows-msvc-$archToken"
$buildPreset = "windows-msvc-$archToken-$($Configuration.ToLowerInvariant())"

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is required. Run the toolchain bootstrap/discovery phase.'
}
if ((Get-Content -Raw (Join-Path $root 'vcpkg.json')) -match 'REPLACE_WITH_RESOLVED_VCPKG_BASELINE') {
    throw 'Resolve and pin the vcpkg baseline before building.'
}

$extra = @()
if ($Analyze) { $extra += '-DFORGE_ENABLE_ANALYZE=ON' }
cmake --preset $configurePreset @extra
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $Architecture $Configuration." }
cmake --build --preset $buildPreset --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $Architecture $Configuration." }
