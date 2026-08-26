[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount)
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G04 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) "$Message (expected '$Expected', found '$Actual')"
}

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message)
    $options = [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    Assert-True ([regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-NoMatch {
    param([string]$Text, [string]$Pattern, [string]$Message)
    $options = [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    Assert-True (-not [regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-Sequence {
    param([AllowEmptyCollection()][object[]]$Actual,
          [AllowEmptyCollection()][object[]]$Expected,
          [string]$Message)
    Assert-Exact $Actual.Count $Expected.Count "$Message count"
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        Assert-Exact ([string]$Actual[$index]) ([string]$Expected[$index]) "$Message item $index"
    }
}

function Assert-Set {
    param([AllowEmptyCollection()][object[]]$Actual,
          [AllowEmptyCollection()][object[]]$Expected,
          [string]$Message)
    $actualSorted = @($Actual | ForEach-Object { [string]$_ } | Sort-Object)
    $expectedSorted = @($Expected | ForEach-Object { [string]$_ } | Sort-Object)
    Assert-Sequence $actualSorted $expectedSorted $Message
}

function Read-Json {
    param([string]$Path)
    try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
    catch { throw "G04 assertion failed: invalid JSON at $Path - $($_.Exception.Message)" }
}

function Get-TreeSummary {
    param([string]$Root)
    $rootFull = (Resolve-Path -LiteralPath $Root).Path
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File)) {
        $paths.Add($file.FullName.Substring($rootFull.Length + 1).Replace('\', '/'))
    }
    $paths.Sort([StringComparer]::Ordinal)
    $rows = [Collections.Generic.List[string]]::new()
    $bytes = 0L
    foreach ($path in $paths) {
        $fullPath = Join-Path $rootFull $path.Replace('/', '\')
        $bytes += [long](Get-Item -LiteralPath $fullPath).Length
        $rows.Add($path + "`t" + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join "`n")
    }
}

function Get-LinkItems {
    param([string]$CMakeText, [string]$Target, [string]$Visibility = 'PRIVATE')
    $pattern = 'target_link_libraries\s*\(\s*' + [regex]::Escape($Target) +
        '\s+' + [regex]::Escape($Visibility) + '\s+(?<body>[^\)]*)\)'
    $match = [regex]::Match($CMakeText, $pattern,
        [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::Singleline)
    Assert-True $match.Success "missing $Visibility link declaration for $Target"
    return @($match.Groups['body'].Value -split '\s+' | Where-Object { $_ })
}

function Get-LayerGraph {
    param([string]$CMakeText)
    $graph = [ordered]@{}
    foreach ($match in [regex]::Matches($CMakeText, 'forge_add_layer\s*\((?<body>[^\)]*)\)',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::Singleline)) {
        $tokens = @($match.Groups['body'].Value -split '\s+' | Where-Object { $_ })
        Assert-True ($tokens.Count -ge 2) 'invalid forge_add_layer declaration'
        Assert-True (-not $graph.Contains([string]$tokens[0])) "duplicate layer target $($tokens[0])"
        $graph[[string]$tokens[0]] = [ordered]@{
            alias = [string]$tokens[1]
            dependencies = @($tokens | Select-Object -Skip 2)
        }
    }
    return $graph
}

$requiredFiles = @(
    'CMakeLists.txt',
    'CMakePresets.json',
    'vcpkg.json',
    'cmake/ForgeForsettiExternal.cmake',
    'cmake/ForsettiExternalPolicy.cmake',
    'scripts/build.ps1',
    'scripts/test.ps1',
    '.vscode/tasks.json',
    '.vscode/launch.json',
    '.vscode/settings.json',
    'src/ForsettiModule/ForgeConductorAppModule.cpp',
    'src/Hosts/Cli/main.cpp',
    'tests/ForsettiHostSmoke/main.cpp',
    'src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests/ForgeConductorAppModule.json'
)
foreach ($relativePath in $requiredFiles) {
    Assert-True (Test-Path -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) -PathType Leaf) "required P04 file is missing: $relativePath"
}

foreach ($relativePath in @('scripts/build.ps1', 'scripts/test.ps1')) {
    $tokens = $null
    $errors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))).Path,
        [ref]$tokens,
        [ref]$errors) | Out-Null
    Assert-Exact $errors.Count 0 "$relativePath PowerShell parser errors"
}

$rootCMakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$externalCMakePath = Join-Path $WorkspaceRoot 'cmake\ForgeForsettiExternal.cmake'
$policyCMakePath = Join-Path $WorkspaceRoot 'cmake\ForsettiExternalPolicy.cmake'
$rootCMake = Get-Content -Raw -LiteralPath $rootCMakePath
$externalCMake = Get-Content -Raw -LiteralPath $externalCMakePath
$policyCMake = Get-Content -Raw -LiteralPath $policyCMakePath

Assert-Match $rootCMake 'cmake_minimum_required\s*\(\s*VERSION\s+3\.28\s*\)' 'root CMake minimum version'
Assert-Match $rootCMake 'VERSION\s+0\.9\.0' 'project version 0.9.0'
Assert-Match $rootCMake 'Visual Studio 17 2022' 'VS17 generator pin'
Assert-Match $rootCMake '\bv143\b' 'v143 toolset pin'
Assert-Match $rootCMake '10\.0\.26100\.0' 'Windows SDK pin'
Assert-Match $rootCMake 'CMAKE_CXX_STANDARD\s+20' 'C++20 standard pin'
Assert-Match $rootCMake 'MultiThreaded\$<\$<CONFIG:Debug>:Debug>DLL' 'DLL CRT selection'
Assert-NoMatch $rootCMake 'add_subdirectory\s*\([^\)]*(?:Forsetti|\.forge-inputs)' 'consumer root must not add_subdirectory sealed Forsetti'
Assert-NoMatch $rootCMake '(?:ForsettiCore|ForsettiPlatform|ForsettiHostTemplate)[^\r\n]*\.cpp' 'consumer root must not compile framework internals'
Assert-NoMatch $rootCMake 'ForsettiExample|ForsettiDemo' 'consumer target graph must not reference examples'
Assert-Match $rootCMake 'add_library\s*\(\s*ForgeConductor\.ForsettiModule\s+STATIC\s+src/ForsettiModule/ForgeConductorAppModule\.cpp\s*\)' 'one-source static Forsetti module target'
Assert-NoMatch $rootCMake 'ModuleRegistration\.cpp|ForsettiHostSmoke\.cpp' 'obsolete P04 source placeholders'
Assert-Match $rootCMake 'add_executable\s*\(\s*ForgeConductor\.Cli\s+src/Hosts/Cli/main\.cpp\s*\)' 'CLI target source'
Assert-Match $rootCMake 'OUTPUT_NAME\s+"forge-conductor"' 'CLI output name'
Assert-Match $rootCMake 'add_executable\s*\(\s*ForgeConductor\.ForsettiHostSmoke\s+tests/ForsettiHostSmoke/main\.cpp\s*\)' 'Forsetti smoke target source'

Assert-Sequence (Get-LinkItems $rootCMake 'ForgeConductor.ForsettiModule' 'PUBLIC') @('Forsetti::Core', 'ForgeConductor::Contracts') 'Forsetti module links'
Assert-Sequence (Get-LinkItems $rootCMake 'ForgeConductor.ForsettiHostSmoke') @('ForgeConductor::ForsettiModule', 'Forsetti::HostTemplate') 'Forsetti smoke links'
Assert-Sequence (Get-LinkItems $rootCMake 'ForgeConductor.Cli') @('ForgeConductor::Application') 'CLI links'

$expectedLayerGraph = [ordered]@{
    'ForgeConductor.Domain' = [ordered]@{ alias = 'ForgeConductor::Domain'; dependencies = @() }
    'ForgeConductor.Contracts' = [ordered]@{ alias = 'ForgeConductor::Contracts'; dependencies = @('ForgeConductor::Domain') }
    'ForgeConductor.Application' = [ordered]@{ alias = 'ForgeConductor::Application'; dependencies = @('ForgeConductor::Contracts') }
    'ForgeConductor.Infrastructure.Windows' = [ordered]@{ alias = 'ForgeConductor::Infrastructure.Windows'; dependencies = @('ForgeConductor::Contracts') }
    'ForgeConductor.Persistence.Windows' = [ordered]@{ alias = 'ForgeConductor::Persistence.Windows'; dependencies = @('ForgeConductor::Contracts', 'ForgeConductor::Infrastructure.Windows') }
    'ForgeConductor.NativeTools.Windows' = [ordered]@{ alias = 'ForgeConductor::NativeTools.Windows'; dependencies = @('ForgeConductor::Contracts', 'ForgeConductor::Infrastructure.Windows') }
    'ForgeConductor.SessionHost.Core' = [ordered]@{ alias = 'ForgeConductor::SessionHost.Core'; dependencies = @('ForgeConductor::Application', 'ForgeConductor::Contracts') }
    'ForgeConductor.Mcp' = [ordered]@{ alias = 'ForgeConductor::Mcp'; dependencies = @('ForgeConductor::Application', 'ForgeConductor::Contracts') }
    'ForgeConductor.Composition.Windows' = [ordered]@{ alias = 'ForgeConductor::Composition.Windows'; dependencies = @('ForgeConductor::Application', 'ForgeConductor::Infrastructure.Windows', 'ForgeConductor::Persistence.Windows', 'ForgeConductor::NativeTools.Windows', 'ForgeConductor::SessionHost.Core', 'ForgeConductor::Mcp', 'Forsetti::Platform', 'Forsetti::HostTemplate') }
}
$actualLayerGraph = Get-LayerGraph $rootCMake
Assert-Set @($actualLayerGraph.Keys) @($expectedLayerGraph.Keys) 'layer target set'
foreach ($layerName in $expectedLayerGraph.Keys) {
    Assert-Exact ([string]$actualLayerGraph[$layerName].alias) ([string]$expectedLayerGraph[$layerName].alias) "$layerName alias"
    Assert-Sequence @($actualLayerGraph[$layerName].dependencies) @($expectedLayerGraph[$layerName].dependencies) "$layerName dependencies"
}
Assert-Match $rootCMake 'target_include_directories\s*\(\s*ForgeConductor\.Contracts\s+INTERFACE\s+"\$\{PROJECT_SOURCE_DIR\}/include"\s*\)' 'Contracts public include root'

Assert-Match $rootCMake 'ForsettiManifests/ForgeConductorAppModule\.json' 'canonical manifest stage destination'
Assert-Match $rootCMake 'COMMAND\s+\$<TARGET_FILE:ForgeConductor\.ForsettiHostSmoke>\s+"\$<TARGET_FILE_DIR:ForgeConductor\.ForsettiHostSmoke>/ForsettiManifests"' 'smoke test manifest-directory argv'
Assert-Match $rootCMake 'ForgeConductor\.Cli\.SelfTest[\s\S]*LABELS\s+"T-UNIT;G04"' 'CLI CTest labels'
Assert-Match $rootCMake 'ForgeConductor\.ForsettiHostSmoke[\s\S]*LABELS\s+"T-UNIT;G04"' 'smoke CTest labels'

Assert-Match $externalCMake 'ExternalProject_Add\s*\(\s*forsetti_external' 'Forsetti standalone ExternalProject'
Assert-Match $externalCMake 'SOURCE_DIR\s+"\$\{FORGE_FORSETTI_SOURCE_DIR\}"' 'ExternalProject immutable source dir'
Assert-Match $externalCMake '\.forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main' 'exact pinned Forsetti path'
Assert-Match $externalCMake 'FORGE_FORSETTI_VERSION\s+"0\.2\.0"' 'Forsetti version pin'
Assert-Match $externalCMake '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d' 'Forsetti archive SHA pin'
Assert-Match $externalCMake 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' 'Forsetti tree SHA pin'
Assert-Match $externalCMake 'VCPKG_MANIFEST_MODE:BOOL=OFF' 'UP-014 external manifest-mode workaround'
Assert-Match $externalCMake 'FORSETTI_BUILD_HOST_TEMPLATE:BOOL=ON' 'external HostTemplate selection'
Assert-Match $externalCMake 'FORSETTI_BUILD_SAMPLES:BOOL=OFF' 'external sample suppression'
Assert-Match $externalCMake 'BUILD_TESTING:BOOL=OFF' 'external test suppression'
Assert-Match $externalCMake '--target\s+ForsettiHostTemplate\s+--parallel' 'target-specific external build command'
Assert-Match $externalCMake 'INSTALL_COMMAND\s+""' 'external install disabled'
Assert-Match $externalCMake 'TEST_COMMAND\s+""' 'external tests disabled'
Assert-NoMatch $externalCMake 'PATCH_COMMAND|add_subdirectory' 'external orchestration must not patch or merge framework source'
Assert-NoMatch $externalCMake 'ForsettiExample|ForsettiDemo' 'consumer external graph must not name examples'
foreach ($alias in @('Forsetti::Core', 'Forsetti::Platform', 'Forsetti::HostTemplate')) {
    Assert-Match $externalCMake ('add_library\s*\(\s*' + [regex]::Escape($alias) + '\s+ALIAS') "missing imported public alias $alias"
}
Assert-Match $externalCMake 'Forsetti::Core;advapi32;bcrypt;crypt32;winhttp' 'Platform link closure'
Assert-Match $externalCMake 'Forsetti::Core;Forsetti::Platform' 'HostTemplate link closure'
Assert-Match $policyCMake 'CMP0091\s+NEW' 'external DLL CRT policy'
Assert-Match $policyCMake 'MultiThreaded\$<\$<CONFIG:Debug>:Debug>DLL' 'external DLL CRT value'
Assert-Match $policyCMake 'VCPKG_MANIFEST_MODE' 'external manifest-mode fail-closed policy'

$presets = Read-Json (Join-Path $WorkspaceRoot 'CMakePresets.json')
Assert-Exact ([int]$presets.version) 6 'CMake preset schema version'
$basePreset = @($presets.configurePresets | Where-Object name -ceq 'windows-msvc-base')
Assert-Exact $basePreset.Count 1 'base configure preset count'
Assert-Exact ([string]$basePreset[0].generator) 'Visual Studio 17 2022' 'preset generator'
Assert-Exact ([string]$basePreset[0].toolset.value) 'v143' 'preset toolset'
Assert-Exact ([string]$basePreset[0].cacheVariables.CMAKE_SYSTEM_VERSION) '10.0.26100.0' 'preset SDK'
Assert-Exact ([string]$basePreset[0].cacheVariables.CMAKE_MSVC_RUNTIME_LIBRARY) 'MultiThreaded$<$<CONFIG:Debug>:Debug>DLL' 'preset DLL CRT'
Assert-Exact ([string]$basePreset[0].cacheVariables.VCPKG_MANIFEST_MODE) 'ON' 'consumer manifest mode'
Assert-Exact ([string]$basePreset[0].cacheVariables.BUILD_TESTING) 'ON' 'consumer test registration'
Assert-Set @($presets.configurePresets.name) @('windows-msvc-base', 'windows-msvc-x64', 'windows-msvc-arm64') 'configure preset set'
Assert-Set @($presets.buildPresets.name) @('windows-msvc-x64-debug', 'windows-msvc-x64-release', 'windows-msvc-arm64-debug', 'windows-msvc-arm64-release') 'build preset set'
Assert-Set @($presets.testPresets.name) @('windows-msvc-x64-debug', 'windows-msvc-x64-release', 'windows-msvc-arm64-debug', 'windows-msvc-arm64-release') 'test preset set'

$consumerVcpkg = Read-Json (Join-Path $WorkspaceRoot 'vcpkg.json')
Assert-Exact ([string]$consumerVcpkg.name) 'forge-conductor-windows' 'consumer vcpkg manifest name'
Assert-Exact ([string]$consumerVcpkg.'builtin-baseline') '00c5775211f45cd08b37fce0484b4cb940e422ab' 'current consumer vcpkg baseline'
Assert-Sequence @($consumerVcpkg.dependencies) @('nlohmann-json') 'consumer vcpkg dependencies'

$buildText = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'scripts\build.ps1')
$testText = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'scripts\test.ps1')
Assert-Match $buildText '\$env:VCPKG_ROOT' 'build script environment discovery'
Assert-Match $buildText '\.forge-codex\\state\\toolchain\.json' 'build script toolchain-state discovery'
Assert-Match $buildText 'nlohmann-json:\$triplet"\s+--classic' 'build script classic dependency preinstall'
Assert-Match $buildText '\[string\[\]\]\$Target' 'build script target selection'
Assert-Match $buildText '--parallel' 'build script parallel selection'
Assert-Match $testText '--preset' 'test script configuration-aware preset'
Assert-Match $testText '--no-tests=error' 'test script no-test failure mode'

foreach ($jsonPath in @('.vscode/tasks.json', '.vscode/launch.json', '.vscode/settings.json')) {
    [void](Read-Json (Join-Path $WorkspaceRoot $jsonPath.Replace('/', '\')))
}

$frameworkRoot = Join-Path $WorkspaceRoot '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$treeBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$treeBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$treeBefore.bytes) 723455L 'sealed Forsetti byte count'
Assert-Exact ([string]$treeBefore.sha256) 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' 'sealed Forsetti tree SHA-256'

$toolchain = Read-Json (Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json')
Assert-True ([bool]$toolchain.complete) 'toolchain discovery is incomplete'
Assert-Exact ([string]$toolchain.windows_sdk.selected_version) '10.0.26100.0' 'discovered Windows SDK'
Assert-Match ([string]$toolchain.visual_studio.installationVersion) '^17\.' 'discovered Visual Studio 17'
Assert-Match ([string]$toolchain.msvc.toolset_version) '^14\.' 'discovered v143 MSVC toolset'

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G04: building x64 Debug.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Write-Host 'G04: testing x64 Debug.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G04
Write-Host 'G04: building x64 Release.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Write-Host 'G04: testing x64 Release.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G04

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$canonicalManifest = Join-Path $WorkspaceRoot 'src\ForgeConductor.ForsettiModule\Resources\ForsettiManifests\ForgeConductorAppModule.json'
$canonicalManifestHash = Get-FileSha256 $canonicalManifest
foreach ($configuration in @('Debug', 'Release')) {
    $binRoot = Join-Path $buildRoot "bin\$configuration"
    $artifactPaths = @(
        (Join-Path $binRoot 'forge-conductor.exe'),
        (Join-Path $binRoot 'ForgeConductor.ForsettiHostSmoke.exe'),
        (Join-Path $buildRoot "lib\$configuration\ForgeConductor.ForsettiModule.lib"),
        (Join-Path $buildRoot "_deps\forsetti-framework\src\ForsettiCore\$configuration\ForsettiCore.lib"),
        (Join-Path $buildRoot "_deps\forsetti-framework\src\ForsettiPlatform\$configuration\ForsettiPlatform.lib"),
        (Join-Path $buildRoot "_deps\forsetti-framework\src\ForsettiHostTemplate\$configuration\ForsettiHostTemplate.lib")
    )
    foreach ($artifactPath in $artifactPaths) {
        Assert-True (Test-Path -LiteralPath $artifactPath -PathType Leaf) "$configuration artifact missing: $artifactPath"
        Assert-True ([long](Get-Item -LiteralPath $artifactPath).Length -gt 0L) "$configuration artifact is empty: $artifactPath"
    }
    $stagedManifestRoot = Join-Path $binRoot 'ForsettiManifests'
    $stagedJson = @(Get-ChildItem -LiteralPath $stagedManifestRoot -File -Filter '*.json')
    Assert-Exact $stagedJson.Count 1 "$configuration staged manifest count"
    Assert-Exact $stagedJson[0].Name 'ForgeConductorAppModule.json' "$configuration staged manifest name"
    Assert-Exact (Get-FileSha256 $stagedJson[0].FullName) $canonicalManifestHash "$configuration canonical manifest equality"
}

$rootProjects = @(Get-ChildItem -LiteralPath $buildRoot -File -Filter '*.vcxproj')
Assert-True ($rootProjects.Count -gt 0) 'Visual Studio projects were not generated'
$allRootProjectText = ($rootProjects | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
Assert-Match $allRootProjectText '<PlatformToolset>v143</PlatformToolset>' 'generated v143 projects'
Assert-Match $allRootProjectText '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' 'generated SDK selection'
Assert-Match $allRootProjectText '<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>' 'generated Debug DLL CRT'
Assert-Match $allRootProjectText '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>' 'generated Release DLL CRT'
Assert-NoMatch $allRootProjectText 'ForsettiExample|ForsettiDemo' 'consumer projects must not register or link examples'

$treeAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$treeAfter.files) ([int]$treeBefore.files) 'sealed Forsetti file count after builds'
Assert-Exact ([long]$treeAfter.bytes) ([long]$treeBefore.bytes) 'sealed Forsetti bytes after builds'
Assert-Exact ([string]$treeAfter.sha256) ([string]$treeBefore.sha256) 'sealed Forsetti tree hash after builds'

Write-Host "G04 build scaffold validation passed: $script:AssertionCount fail-closed assertions; x64 Debug and Release built and passed both G04 tests."
