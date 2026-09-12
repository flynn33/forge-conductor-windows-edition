#Requires -Version 7.0
[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [int]$Parallel=4)
$ErrorActionPreference='Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installations = (& $vswhere -products '*' -version '[17.0,18.0)' -format json | ConvertFrom-Json)
$msbuild = $null
foreach ($installation in $installations) {
    $toolset = Join-Path $installation.installationPath 'MSBuild/Microsoft/VC/v170/Application Type/Windows Store/10.0/Platforms/x64/PlatformToolsets/v143/Toolset.props'
    if (Test-Path -LiteralPath $toolset) {
        $msbuild = Join-Path $installation.installationPath 'MSBuild/Current/Bin/MSBuild.exe'
        break
    }
}
if (-not $msbuild) { throw 'Install VS 2022 C++ Universal Windows Platform tools (v143) and Windows SDK 26100 for the WinUI XAML build.' }
$project = Join-Path $root 'src/Hosts/App/ForgeConductor.App.vcxproj'
& $msbuild $project /restore "/p:Configuration=$Configuration" /p:Platform=x64 "/m:$Parallel" /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Native app build failed (exit $LASTEXITCODE)." }
$output = Join-Path $root "out/app/x64/$Configuration"
$backend = Join-Path $root "out/build/windows-msvc-x64/bin/$Configuration"
foreach ($name in @('forge-conductor.exe','ForgeConductor.Manager.exe','ForgeConductor.SessionHost.exe')) {
    Copy-Item -LiteralPath (Join-Path $backend $name) -Destination $output -Force
}
$entries = foreach ($name in @('ForgeConductorApp.exe','forge-conductor.exe','ForgeConductor.Manager.exe','ForgeConductor.SessionHost.exe')) {
    $file = Join-Path $output $name
    @{name=$name;path=$file;sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$sourceCommit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $sourceCommit) { throw 'Could not resolve the build source commit.' }
$sourceTree = (& git -C $root rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0 -or -not $sourceTree) { throw 'Could not resolve the build source tree.' }
$sourceInputs = @(
    'CMakeLists.txt','CMakePresets.json','vcpkg.json','vcpkg-configuration.json',
    'include','src')
$sourceDirty = @(& git -C $root status --porcelain=v1 --untracked-files=all -- @sourceInputs)
@{configuration=$Configuration;architecture='x64';app_directory=$output;
  source_commit=$sourceCommit;source_tree=$sourceTree;source_dirty=@($sourceDirty);
  executables=@($entries)} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'staging-manifest.json') -Encoding utf8
Write-Host "Native application and sibling services staged at $output"
