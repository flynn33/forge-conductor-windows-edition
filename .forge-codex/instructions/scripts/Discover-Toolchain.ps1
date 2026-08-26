[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

Update-ProcessPathFromRegistry

function Resolve-CommandPath {
    param([Parameter(Mandatory)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

function Resolve-ExistingPath {
    param([AllowNull()][string[]]$Candidates)
    foreach ($candidate in @($Candidates)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate)) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Get-ExecutableVersion {
    param([AllowNull()][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path)) {
        return $null
    }
    return [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion
}

$vswhere = Get-VsWherePath
$vsInstances = @()
if ($vswhere) {
    $discovered = & $vswhere -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -format json | ConvertFrom-Json
    $vsInstances = @($discovered |
        Sort-Object { [version]$_.installationVersion } -Descending)
}

# CMakePresets.json targets the Visual Studio 2022 generator, so prefer VS 17.
$visualStudio = $vsInstances |
    Where-Object { ([version]$_.installationVersion).Major -eq 17 } |
    Select-Object -First 1
if (-not $visualStudio) {
    $visualStudio = $vsInstances | Select-Object -First 1
}
$vsRoot = if ($visualStudio) { [string]$visualStudio.installationPath } else { $null }

$msbuild = Resolve-ExistingPath @(
    $(if ($vsRoot) { Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe' }),
    (Resolve-CommandPath 'msbuild.exe')
)
$cmake = Resolve-ExistingPath @(
    (Resolve-CommandPath 'cmake.exe'),
    $(if ($vsRoot) {
        Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    })
)
$ctest = Resolve-ExistingPath @(
    (Resolve-CommandPath 'ctest.exe'),
    $(if ($cmake) { Join-Path (Split-Path -Parent $cmake) 'ctest.exe' })
)

$vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'Process')
if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
    $vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
}
if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
    $vcpkgRoot = Join-Path $env:LOCALAPPDATA 'ForgeConductor\Toolchain\vcpkg'
}
$vcpkg = Resolve-ExistingPath @(
    (Resolve-CommandPath 'vcpkg.exe'),
    (Join-Path $vcpkgRoot 'vcpkg.exe')
)

$msvcRoot = if ($vsRoot) { Join-Path $vsRoot 'VC\Tools\MSVC' } else { $null }
$msvcToolset = $null
if ($msvcRoot -and (Test-Path -LiteralPath $msvcRoot)) {
    $msvcToolset = Get-ChildItem -LiteralPath $msvcRoot -Directory |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}
$cl = Resolve-ExistingPath @(
    $(if ($msvcToolset) { Join-Path $msvcToolset.FullName 'bin\Hostx64\x64\cl.exe' }),
    (Resolve-CommandPath 'cl.exe')
)
$link = Resolve-ExistingPath @(
    $(if ($msvcToolset) { Join-Path $msvcToolset.FullName 'bin\Hostx64\x64\link.exe' }),
    (Resolve-CommandPath 'link.exe')
)

$kitsRoot = $null
try {
    $kitsRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction Stop).KitsRoot10
} catch {}
if ([string]::IsNullOrWhiteSpace($kitsRoot)) {
    $kitsRoot = 'C:\Program Files (x86)\Windows Kits\10'
}
$sdkVersions = @()
$sdkIncludeRoot = Join-Path $kitsRoot 'Include'
if (Test-Path -LiteralPath $sdkIncludeRoot) {
    $sdkVersions = @(Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory |
        Where-Object {
            $_.Name -match '^10\.\d+\.\d+\.\d+$' -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h'))
        } |
        Sort-Object { [version]$_.Name } -Descending)
}
$sdkVersion = if ($sdkVersions.Count -gt 0) { $sdkVersions[0].Name } else { $null }
$sdkBin = if ($sdkVersion) {
    Join-Path (Join-Path $kitsRoot 'bin') (Join-Path $sdkVersion 'x64')
} else {
    $null
}
$cppwinrtTool = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'cppwinrt.exe' }))
$makeAppx = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'makeappx.exe' }))
$signTool = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'signtool.exe' }))

$propsPath = Join-Path $WorkspaceRoot 'Directory.Build.props'
$pinnedWindowsSdkVersion = $null
$windowsAppSdkVersion = $null
$cppWinRtVersion = $null
if (Test-Path -LiteralPath $propsPath) {
    [xml]$props = Get-Content -LiteralPath $propsPath -Raw
    $pinnedWindowsSdkVersion = [string]$props.Project.PropertyGroup.WindowsTargetPlatformVersion
    $windowsAppSdkVersion = [string]$props.Project.PropertyGroup.WindowsAppSdkVersion
    $cppWinRtVersion = [string]$props.Project.PropertyGroup.CppWinRTVersion
}
$matchingSdk = $sdkVersions |
    Where-Object { $_.Name -eq $pinnedWindowsSdkVersion } |
    Select-Object -First 1
$sdkVersion = if ($matchingSdk) { $matchingSdk.Name } else { $null }
$sdkBin = if ($sdkVersion) {
    Join-Path (Join-Path $kitsRoot 'bin') (Join-Path $sdkVersion 'x64')
} else {
    $null
}
$cppwinrtTool = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'cppwinrt.exe' }))
$makeAppx = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'makeappx.exe' }))
$signTool = Resolve-ExistingPath @($(if ($sdkBin) { Join-Path $sdkBin 'signtool.exe' }))
$nugetRoot = [Environment]::GetEnvironmentVariable('NUGET_PACKAGES', 'Process')
if ([string]::IsNullOrWhiteSpace($nugetRoot)) {
    $nugetRoot = [Environment]::GetEnvironmentVariable('NUGET_PACKAGES', 'User')
}
if ([string]::IsNullOrWhiteSpace($nugetRoot)) {
    $nugetRoot = Join-Path $env:USERPROFILE '.nuget\packages'
}
$windowsAppSdkCache = if ($windowsAppSdkVersion) {
    Join-Path $nugetRoot (Join-Path 'microsoft.windowsappsdk' $windowsAppSdkVersion)
} else {
    $null
}
$cppWinRtCache = if ($cppWinRtVersion) {
    Join-Path $nugetRoot (Join-Path 'microsoft.windows.cppwinrt' $cppWinRtVersion)
} else {
    $null
}

$vcpkgBaseline = $null
$vcpkgManifest = Join-Path $WorkspaceRoot 'vcpkg.json'
if (Test-Path -LiteralPath $vcpkgManifest) {
    $vcpkgBaseline = [string]((Get-Content -LiteralPath $vcpkgManifest -Raw |
        ConvertFrom-Json).'builtin-baseline')
}
$vcpkgCheckoutHead = $null
$gitForVcpkg = Resolve-CommandPath 'git.exe'
if ($gitForVcpkg -and (Test-Path -LiteralPath (Join-Path $vcpkgRoot '.git'))) {
    $resolvedHead = & $gitForVcpkg -C $vcpkgRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) {
        $vcpkgCheckoutHead = [string]($resolvedHead | Select-Object -First 1)
    }
}

$tools = [ordered]@{
    git = Resolve-CommandPath 'git.exe'
    cmake = $cmake
    ctest = $ctest
    msbuild = $msbuild
    vcpkg = $vcpkg
    winget = Resolve-CommandPath 'winget.exe'
    pwsh = Resolve-CommandPath 'pwsh.exe'
    codex = Resolve-CommandPath 'codex.exe'
    code = Resolve-CommandPath 'code.cmd'
    cl = $cl
    link = $link
    cppwinrt = $cppwinrtTool
    makeappx = $makeAppx
    signtool = $signTool
}

$missing = @()
$windows11 = Test-Windows11
if (-not $windows11) { $missing += 'windows_11' }
foreach ($requiredTool in @(
    'git','cmake','ctest','msbuild','vcpkg','pwsh',
    'cl','link','cppwinrt','makeappx','signtool'
)) {
    if ([string]::IsNullOrWhiteSpace([string]$tools[$requiredTool])) {
        $missing += $requiredTool
    }
}
if (-not $visualStudio) { $missing += 'visual_studio_cpp' }
if (-not $sdkVersion) { $missing += 'windows_sdk' }
if (-not $windowsAppSdkVersion) { $missing += 'windows_app_sdk_pin' }
if (-not ($windowsAppSdkCache -and (Test-Path -LiteralPath $windowsAppSdkCache))) {
    $missing += 'windows_app_sdk_package'
}
if (-not $cppWinRtVersion) { $missing += 'cppwinrt_package_pin' }
if (-not ($cppWinRtCache -and (Test-Path -LiteralPath $cppWinRtCache))) {
    $missing += 'cppwinrt_package'
}
if ([string]::IsNullOrWhiteSpace($vcpkgBaseline) -or
    [string]::IsNullOrWhiteSpace($vcpkgCheckoutHead) -or
    $vcpkgBaseline -ne $vcpkgCheckoutHead) {
    $missing += 'vcpkg_baseline_checkout_mismatch'
}

$result = [ordered]@{
    schema_version = 1
    utc = Get-UtcTimestamp
    windows_11 = $windows11
    os = [Environment]::OSVersion.VersionString
    architecture = $env:PROCESSOR_ARCHITECTURE
    powershell = $PSVersionTable.PSVersion.ToString()
    complete = ($missing.Count -eq 0)
    missing = $missing
    tools = $tools
    tool_versions = [ordered]@{
        cmake = Get-ExecutableVersion $cmake
        ctest = Get-ExecutableVersion $ctest
        msbuild = Get-ExecutableVersion $msbuild
        vcpkg = Get-ExecutableVersion $vcpkg
        cl = Get-ExecutableVersion $cl
        cppwinrt = Get-ExecutableVersion $cppwinrtTool
        makeappx = Get-ExecutableVersion $makeAppx
        signtool = Get-ExecutableVersion $signTool
    }
    visual_studio = $visualStudio
    visual_studio_instances = $vsInstances
    msvc = [ordered]@{
        toolset_version = if ($msvcToolset) { $msvcToolset.Name } else { $null }
        root = if ($msvcToolset) { $msvcToolset.FullName } else { $null }
        compiler = $cl
        linker = $link
    }
    windows_sdk = [ordered]@{
        root = $kitsRoot
        installed_versions = @($sdkVersions | ForEach-Object { $_.Name })
        pinned_version = $pinnedWindowsSdkVersion
        selected_version = $sdkVersion
        cppwinrt = $cppwinrtTool
        makeappx = $makeAppx
        signtool = $signTool
    }
    vcpkg = [ordered]@{
        root = $vcpkgRoot
        executable = $vcpkg
        builtin_baseline = $vcpkgBaseline
        checkout_head = $vcpkgCheckoutHead
    }
    windows_app_sdk = [ordered]@{
        package = 'Microsoft.WindowsAppSDK'
        version = $windowsAppSdkVersion
        cache_path = $windowsAppSdkCache
        cached = [bool]($windowsAppSdkCache -and (Test-Path -LiteralPath $windowsAppSdkCache))
    }
    cppwinrt_package = [ordered]@{
        package = 'Microsoft.Windows.CppWinRT'
        version = $cppWinRtVersion
        cache_path = $cppWinRtCache
        cached = [bool]($cppWinRtCache -and (Test-Path -LiteralPath $cppWinRtCache))
    }
}

$out = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
Write-JsonFileAtomic -Path $out -Value $result
if (-not $result.windows_11) {
    throw 'Windows 11 build 22000 or newer is required.'
}
Write-Host $out
