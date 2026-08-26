[CmdletBinding()]
param(
    [switch]$IncludeVisualStudioBuildTools,
    [string]$VcpkgRoot = (Join-Path $env:LOCALAPPDATA 'ForgeConductor\Toolchain\vcpkg')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$winget = Get-Command winget.exe -ErrorAction SilentlyContinue
if (-not $winget) {
    throw 'winget is unavailable. Preserve the blocker with toolchain evidence; do not substitute an unapproved compiler/runtime.'
}

function Install-WingetPackage {
    param([Parameter(Mandatory)][string]$Id, [string[]]$Extra = @())
    & $winget.Source install --id $Id --exact --silent --disable-interactivity --accept-source-agreements --accept-package-agreements @Extra
    if ($LASTEXITCODE -ne 0) { throw "winget failed for $Id with exit code $LASTEXITCODE." }
    Update-ProcessPathFromRegistry
}

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Install-WingetPackage 'Git.Git' }
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { Install-WingetPackage 'Kitware.CMake' }
if (-not (Get-Command pwsh.exe -ErrorAction SilentlyContinue)) { Install-WingetPackage 'Microsoft.PowerShell' }

$vswhere = Get-VsWherePath
$hasCppBuildTools = $false
if ($vswhere) {
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $hasCppBuildTools = -not [string]::IsNullOrWhiteSpace(($installation | Out-String).Trim())
}
if ($IncludeVisualStudioBuildTools -and -not $hasCppBuildTools) {
    Install-WingetPackage 'Microsoft.VisualStudio.2022.BuildTools' @(
        '--override',
        '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended'
    )
}

if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git'))) {
    $git = Get-Command git.exe -ErrorAction Stop
    $parent = Split-Path -Parent $VcpkgRoot
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    & $git.Source clone --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg clone failed.' }
}
$bootstrap = Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    & $bootstrap -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed.' }
}
[System.Environment]::SetEnvironmentVariable('VCPKG_ROOT', $VcpkgRoot, 'User')
$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "Toolchain provisioning completed. VCPKG_ROOT=$VcpkgRoot. Run Discover-Toolchain.ps1 to prove installed components."
