[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',[ValidateSet('x64','ARM64')][string]$Architecture='x64',[switch]$Analyze)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$arch=$Architecture.ToLowerInvariant()
if (-not $env:VCPKG_ROOT){throw 'VCPKG_ROOT is required.'}
if((Get-Content -Raw (Join-Path $root 'vcpkg.json')) -match 'REPLACE_WITH_RESOLVED_VCPKG_BASELINE'){throw 'Pin the resolved vcpkg baseline first.'}
$args=@();if($Analyze){$args+='-DFORGE_ENABLE_ANALYZE=ON'}
cmake --preset "windows-msvc-$arch" @args
if($LASTEXITCODE -ne 0){throw 'CMake configure failed.'}
cmake --build --preset "windows-msvc-$arch-$($Configuration.ToLowerInvariant())" --parallel
if($LASTEXITCODE -ne 0){throw 'CMake build failed.'}
