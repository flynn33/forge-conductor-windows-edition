[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G08 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) "$Message (expected '$Expected', found '$Actual')"
}

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message, [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor [Text.RegularExpressions.RegexOptions]::IgnoreCase
    }
    Assert-True ([regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-NoMatch {
    param([string]$Text, [string]$Pattern, [string]$Message, [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor [Text.RegularExpressions.RegexOptions]::IgnoreCase
    }
    Assert-True (-not [regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-Set {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualSorted = @($Actual | ForEach-Object { [string]$_ } | Sort-Object)
    $expectedSorted = @($Expected | ForEach-Object { [string]$_ } | Sort-Object)
    Assert-Exact $actualSorted.Count $expectedSorted.Count "$Message count"
    for ($index = 0; $index -lt $expectedSorted.Count; $index++) {
        Assert-Exact $actualSorted[$index] $expectedSorted[$index] "$Message item $index"
    }
}

function Assert-Sequence {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualItems = @($Actual | ForEach-Object { [string]$_ })
    $expectedItems = @($Expected | ForEach-Object { [string]$_ })
    Assert-Exact $actualItems.Count $expectedItems.Count "$Message count"
    for ($index = 0; $index -lt $expectedItems.Count; $index++) {
        Assert-Exact $actualItems[$index] $expectedItems[$index] "$Message item $index"
    }
}

function Split-MsvcOptionTokens {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return @()
    }

    $tokens = [Collections.Generic.List[string]]::new()
    $token = [Text.StringBuilder]::new()
    $quote = [char]0
    foreach ($character in $Text.ToCharArray()) {
        if ($character -eq '"' -or $character -eq "'") {
            if ($quote -eq [char]0) {
                $quote = $character
            } elseif ($quote -eq $character) {
                $quote = [char]0
            } else {
                [void]$token.Append($character)
            }
            continue
        }
        if ([char]::IsWhiteSpace($character) -and $quote -eq [char]0) {
            if ($token.Length -gt 0) {
                $tokens.Add($token.ToString())
                [void]$token.Clear()
            }
            continue
        }
        [void]$token.Append($character)
    }
    if ($token.Length -gt 0) {
        $tokens.Add($token.ToString())
    }
    return $tokens.ToArray()
}

function Get-ManagedCompilerSwitches {
    param([AllowEmptyString()][string]$Options)
    $findings = [Collections.Generic.List[string]]::new()
    $regexOptions = [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::CultureInvariant
    foreach ($rawToken in @(Split-MsvcOptionTokens $Options)) {
        $token = $rawToken.Trim()
        if ([regex]::IsMatch($token, '^[/-]clr(?:[:=].*)?$', $regexOptions)) {
            $findings.Add($rawToken)
            continue
        }
        if ([regex]::IsMatch(
            $token,
            '^[/-]FUNCTIONPADMIN(?:[:=].*)?$',
            $regexOptions)) {
            continue
        }
        if ([regex]::IsMatch($token, '^[/-]FU.*$', $regexOptions)) {
            $findings.Add($rawToken)
        }
    }
    return $findings.ToArray()
}

function Get-ResponseFileReferences {
    param([AllowEmptyString()][string]$Options)
    $findings = [Collections.Generic.List[string]]::new()
    # MSBuild item-list metadata uses semicolons while AdditionalOptions uses
    # command-line whitespace. Treat both as boundaries so a response file
    # cannot hide behind an earlier library or inherited-list placeholder.
    foreach ($rawToken in @(Split-MsvcOptionTokens $Options.Replace(';', ' '))) {
        $token = $rawToken.Trim()
        if ($token.StartsWith('@', [StringComparison]::Ordinal)) {
            $findings.Add($rawToken)
        }
    }
    return $findings.ToArray()
}

$script:ForbiddenEvaluatedDependencyPattern =
    '(?i)(?<![A-Za-z0-9_])(?:Forsetti[A-Za-z0-9_.-]*|(?:lib)?Boost[A-Za-z0-9_.-]*|Qt[A-Za-z0-9_.-]*|(?:lib)?Python[A-Za-z0-9_.-]*|Electron[A-Za-z0-9_.-]*|[.]NET|dotnet|System[.]Runtime|mscoree|mscorlib|node_modules|nodejs|node(?:[._-]?runtime|[.](?:exe|dll|lib))?|npm(?:[.](?:cmd|exe))?|npx(?:[.](?:cmd|exe))?)(?![A-Za-z0-9_])'

function Get-ForbiddenEvaluatedDependencyMatches {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return @()
    }
    return @([regex]::Matches(
        $Text,
        $script:ForbiddenEvaluatedDependencyPattern,
        [Text.RegularExpressions.RegexOptions]::CultureInvariant) |
        ForEach-Object { $_.Value } |
        Sort-Object -Unique)
}

function Get-MSBuildItemField {
    param($Item, [string]$Name)
    if ($null -eq $Item) {
        return ''
    }
    $property = $Item.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return ''
    }
    return [string]$property.Value
}

function Get-MSBuildItems {
    param($Evaluation, [string]$ItemType)
    if ($null -eq $Evaluation -or $null -eq $Evaluation.Items) {
        return @()
    }
    $property = $Evaluation.Items.PSObject.Properties[$ItemType]
    if ($null -eq $property -or $null -eq $property.Value) {
        return @()
    }
    return @($property.Value)
}

function Assert-NativeAdditionalOptions {
    param([AllowEmptyString()][string]$Options, [string]$Context)
    $managedSwitches = @(Get-ManagedCompilerSwitches $Options)
    Assert-Exact $managedSwitches.Count 0 `
        ("$Context has managed compiler switch(es): " + ($managedSwitches -join ', '))
    $responseFiles = @(Get-ResponseFileReferences $Options)
    Assert-Exact $responseFiles.Count 0 `
        ("$Context has forbidden response-file indirection: " +
            ($responseFiles -join ', '))
}

function Assert-NoResponseFileIndirection {
    param([AllowEmptyString()][string]$Options, [string]$Context)
    $responseFiles = @(Get-ResponseFileReferences $Options)
    Assert-Exact $responseFiles.Count 0 `
        ("$Context has forbidden response-file indirection: " +
            ($responseFiles -join ', '))
}

function Assert-NoForbiddenEvaluatedDependency {
    param([AllowEmptyString()][string]$Value, [string]$Context)
    $matches = @(Get-ForbiddenEvaluatedDependencyMatches $Value)
    Assert-Exact $matches.Count 0 `
        ("$Context has forbidden dependency token(s): " + ($matches -join ', '))
}

function Invoke-EvaluatedMSBuildQuery {
    param(
        [string]$MSBuildPath,
        [string]$ProjectPath,
        [string]$Configuration,
        [string]$InspectionTargetsPath
    )
    $arguments = @(
        $ProjectPath,
        '/nologo',
        '/v:q',
        "/p:Configuration=$Configuration",
        '/p:Platform=x64',
        "/p:ForceImportAfterCppTargets=$InspectionTargetsPath",
        '/getProperty:Configuration,Platform,WindowsTargetPlatformVersion,PlatformToolset,CLRSupport,ConfigurationType',
        '/getItem:ClCompile,ResourceCompile,Link,Lib,ProjectReference,Reference,COMReference,PackageReference,FrameworkReference,NativeReference')
    $output = @(& $MSBuildPath @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    Assert-Exact $exitCode 0 `
        ("$Configuration evaluated MSBuild query for $ProjectPath" +
            [Environment]::NewLine + ($output -join [Environment]::NewLine))
    try {
        return (($output -join [Environment]::NewLine) | ConvertFrom-Json)
    } catch {
        throw "G08 assertion failed: invalid evaluated MSBuild JSON for $Configuration $ProjectPath - $($_.Exception.Message)"
    }
}

foreach ($managedOptionCase in @(
    '/clr',
    '-clr',
    '"/clr"',
    '/clr:pure',
    "'-clr:safe'",
    '/FU mscorlib.dll',
    '"/FU:C:\managed\System.Runtime.dll"',
    '/FU"C:\managed assemblies\bridge.dll"',
    '-FUbridge.dll')) {
    Assert-True (@(Get-ManagedCompilerSwitches $managedOptionCase).Count -gt 0) `
        "managed compiler-option adversarial case: $managedOptionCase"
}
foreach ($nativeOptionCase in @(
    '',
    '/FUNCTIONPADMIN',
    '/FUNCTIONPADMIN:6',
    '/NODEFAULTLIB',
    '/NODEFAULTLIB:msvcrt.lib',
    '/O2 /GS /guard:cf',
    '/DVALUE=/clr')) {
    Assert-Exact (@(Get-ManagedCompilerSwitches $nativeOptionCase).Count) 0 `
        "native compiler-option adversarial case: $nativeOptionCase"
}
foreach ($responseFileCase in @(
    '@managed.rsp',
    '"@C:\managed options\compile.rsp"',
    '@"C:\managed options\link.rsp"',
    '/O2 @nested.rsp',
    '/link @''C:\managed options\link.rsp''',
    'kernel32.lib;@hidden-link.rsp;%(AdditionalDependencies)')) {
    Assert-True (@(Get-ResponseFileReferences $responseFileCase).Count -gt 0) `
        "response-file adversarial case: $responseFileCase"
}
foreach ($directOptionCase in @(
    '',
    '/DADDRESS=user@example.com',
    '/DVALUE="@not-a-response-token"',
    'C:\objects\literal@name.obj')) {
    Assert-Exact (@(Get-ResponseFileReferences $directOptionCase).Count) 0 `
        "direct-option adversarial case: $directOptionCase"
}
foreach ($forbiddenDependencyCase in @(
    'C:\sdk\ForsettiCore.lib',
    'libboost_filesystem-vc143.lib',
    'Qt6Core.lib',
    'python311.lib',
    'electron.exe',
    'System.Runtime.dll',
    'mscoree.lib',
    'mscorlib.dll',
    'C:\runtime\node.exe',
    'node_modules\package',
    'npm.cmd',
    'npx.exe')) {
    Assert-True (@(Get-ForbiddenEvaluatedDependencyMatches $forbiddenDependencyCase).Count -gt 0) `
        "forbidden evaluated-dependency adversarial case: $forbiddenDependencyCase"
}
foreach ($allowedDependencyCase in @(
    '/FUNCTIONPADMIN',
    '/NODEFAULTLIB:libcmt.lib',
    'D:\cmake\ForgeForsettiExternal.cmake',
    'D:\sdk\nlohmann_json.lib')) {
    Assert-Exact (@(Get-ForbiddenEvaluatedDependencyMatches $allowedDependencyCase).Count) 0 `
        "allowed evaluated-dependency adversarial case: $allowedDependencyCase"
}
$evaluatedDependencyMetadataNames = @(
    'AdditionalIncludeDirectories',
    'AdditionalUsingDirectories',
    'ForcedUsingFiles',
    'AdditionalOptions',
    'AdditionalLibraryDirectories',
    'AdditionalDependencies',
    'DelayLoadDLLs')
Assert-True ('AdditionalInputs' -cnotin $evaluatedDependencyMetadataNames) `
    'harmless CMake AdditionalInputs are excluded from dependency evaluation'

function Get-RelativePath {
    param([string]$Path)
    return [IO.Path]::GetRelativePath($WorkspaceRoot, $Path).Replace('\', '/')
}

function Assert-CrlfTextFile {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -gt 0) "$Message is nonempty"
    $hasUtf8Bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    Assert-True (-not $hasUtf8Bom) "$Message UTF-8 BOM is forbidden"
    $bareLfCount = 0
    $bareCrCount = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10 -and ($index -eq 0 -or $bytes[$index - 1] -ne 13)) {
            $bareLfCount++
        }
        if ($bytes[$index] -eq 13 -and
            ($index -eq $bytes.Length - 1 -or $bytes[$index + 1] -ne 10)) {
            $bareCrCount++
        }
    }
    Assert-Exact $bareLfCount 0 "$Message bare-LF count"
    Assert-Exact $bareCrCount 0 "$Message bare-CR count"
    Assert-True ($bytes.Length -ge 2 -and $bytes[$bytes.Length - 2] -eq 13 -and
        $bytes[$bytes.Length - 1] -eq 10) "$Message final CRLF"
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    Assert-NoMatch $text '[ \t]+\r\n' "$Message trailing whitespace" -CaseSensitive
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
        $rows.Add($path + [char]9 + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join [char]10)
    }
}

function Get-CMakeInvocationBody {
    param([string]$Text, [string]$Command, [string]$Target, [string]$Message)
    $pattern = [regex]::Escape($Command) + '\s*\(\s*' +
        [regex]::Escape($Target) + '(?<body>.*?)\)'
    $match = [regex]::Match(
        $Text,
        $pattern,
        [Text.RegularExpressions.RegexOptions]::Singleline)
    Assert-True $match.Success $Message
    return $match.Groups['body'].Value
}

function Get-CMakeTokens {
    param([string]$Body)
    return @([regex]::Matches($Body, '"(?<quoted>[^"]*)"|(?<bare>\S+)') |
        ForEach-Object {
            if ($_.Groups['quoted'].Success) { $_.Groups['quoted'].Value }
            else { $_.Groups['bare'].Value }
        })
}

function Get-CMakeExecutableSources {
    param([string]$Text, [string]$Target)
    $body = Get-CMakeInvocationBody $Text 'add_executable' $Target `
        "CMake executable target $Target"
    return @(Get-CMakeTokens $body | Where-Object { $_ -match '^tests/.+\.cpp$' })
}

function Assert-TestCaseInventory {
    param(
        [string]$Path,
        [string[]]$ExpectedNames,
        [int]$ExpectedPassed,
        [string]$Message
    )
    $text = Get-Content -Raw -LiteralPath $Path
    $actualNames = @([regex]::Matches($text, '"PASS\s+(?<name>[^"\\]+)\\n"') |
        ForEach-Object { $_.Groups['name'].Value })
    Assert-Set $actualNames $ExpectedNames "$Message exact PASS-case inventory"
    Assert-Exact ([regex]::Matches(
        $text,
        '"SUMMARY passed=' + $ExpectedPassed + ' failed=0\\n"').Count) 1 `
        "$Message exact success summary"
    Assert-Exact $actualNames.Count $ExpectedPassed "$Message PASS/summary count agreement"
}

$domainFiles = @(
    'include/ForgeConductor/Domain/ProjectMemoryModels.h',
    'src/Domain/ProjectMemoryModels.cpp')
$contractHeaders = @(
    'include/ForgeConductor/Contracts/IProjectMemoryArtifactStore.h',
    'include/ForgeConductor/Contracts/IProjectMemoryService.h')
$applicationFiles = @(
    'include/ForgeConductor/Application/ProjectMemoryRepositoryCache.h',
    'include/ForgeConductor/Application/ProjectMemoryService.h',
    'src/Application/ProjectMemoryRepositoryCache.cpp',
    'src/Application/ProjectMemoryService.cpp')
$persistenceHeaders = @(
    'include/ForgeConductor/Persistence/Windows/WindowsProjectMemoryArtifactStore.h',
    'include/ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h',
    'include/ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepositoryOpener.h',
    'include/ForgeConductor/Persistence/Windows/WindowsProjectRegistryRepository.h')
$persistenceSources = @(
    'src/Persistence/Windows/WindowsProjectMemoryArtifactStore.cpp',
    'src/Persistence/Windows/WindowsProjectMemoryRepository.cpp',
    'src/Persistence/Windows/WindowsProjectMemoryRepositoryOpener.cpp',
    'src/Persistence/Windows/WindowsProjectRegistryRepository.cpp')
$testFiles = @(
    'tests/ProjectMemory/ProjectMemoryApplicationTests.cpp',
    'tests/ProjectMemory/ProjectMemoryArtifactWindowsTests.cpp',
    'tests/ProjectMemory/ProjectMemoryCacheTests.cpp',
    'tests/ProjectMemory/ProjectMemoryRepositoryWindowsTests.cpp',
    'tests/ProjectMemory/ProjectRegistryWindowsTests.cpp')
$fixtureFiles = @(
    'tests/ProjectMemory/Fixtures/macos-0.9.0-project-memory-export-v1.json')
$adrFiles = @(
    '.forge-codex/state/decisions/P08-001-project-identity-and-registry-authority.md',
    '.forge-codex/state/decisions/P08-002-project-repository-cache-and-operation-ownership.md',
    '.forge-codex/state/decisions/P08-003-project-memory-observable-semantics-and-artifacts.md')
$requiredFiles = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Domain/ResourcePolicy.h',
    'include/ForgeConductor/Persistence/Windows/PersistenceWindows.h',
    'scripts/build.ps1',
    'scripts/test.ps1',
    'scripts/validation/Test-G04BuildScaffold.ps1',
    'scripts/validation/Test-G05DomainContracts.ps1',
    'scripts/validation/Test-G06WindowsInfrastructure.ps1',
    'scripts/validation/Test-G07DatabaseMigrations.ps1',
    'scripts/validation/Test-G08ProjectMemory.ps1',
    'src/Domain/ResourcePolicy.cpp') + $domainFiles + $contractHeaders +
    $applicationFiles + $persistenceHeaders + $persistenceSources + $testFiles +
    $fixtureFiles + $adrFiles

foreach ($relativePath in $requiredFiles) {
    Assert-True (Test-Path -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) `
        -PathType Leaf) "required P08 file $relativePath"
}

$projectMemoryTestRoot = Join-Path $WorkspaceRoot 'tests\ProjectMemory'
Assert-Set @(Get-ChildItem -LiteralPath $projectMemoryTestRoot -Force -File |
    ForEach-Object { $_.Name }) @($testFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) `
    'exact P08 test-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath $projectMemoryTestRoot -Force -Directory |
    ForEach-Object { $_.Name }) @('Fixtures') 'P08 test subdirectory inventory'
$fixtureRoot = Join-Path $projectMemoryTestRoot 'Fixtures'
Assert-Set @(Get-ChildItem -LiteralPath $fixtureRoot -Force -File |
    ForEach-Object { $_.Name }) @($fixtureFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) `
    'exact P08 golden-fixture inventory'
Assert-Set @(Get-ChildItem -LiteralPath $fixtureRoot -Force -Directory |
    ForEach-Object { $_.Name }) @() 'P08 golden-fixture subdirectory inventory'
$macosGoldenPath = Join-Path $WorkspaceRoot $fixtureFiles[0].Replace('/', '\')
$macosGoldenBytes = [IO.File]::ReadAllBytes($macosGoldenPath)
Assert-Exact $macosGoldenBytes.Length 928 'macOS 0.9.0 project-memory golden byte count'
Assert-Exact (Get-FileSha256 $macosGoldenPath) `
    '516427f69516bf12aae9b570eb2fc2a964847b93fa6f99d46e487edc5e8e8a11' `
    'macOS 0.9.0 project-memory golden SHA-256' -CaseSensitive
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
Assert-Set @(Get-ChildItem -LiteralPath $decisionRoot -Force -File -Filter 'P08-*.md' |
    ForEach-Object { $_.Name }) @($adrFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) `
    'exact P08 ADR inventory'

$tokens = $null
$parseErrors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $PSCommandPath,
    [ref]$tokens,
    [ref]$parseErrors)
Assert-Exact @($parseErrors).Count 0 'G08 validator PowerShell parser-error count'

$crlfFiles = @('CMakeLists.txt', 'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Persistence/Windows/PersistenceWindows.h',
    'scripts/validation/Test-G08ProjectMemory.ps1') + $domainFiles + $contractHeaders +
    $applicationFiles + $persistenceHeaders + $persistenceSources + $testFiles + $adrFiles
foreach ($relativePath in @($crlfFiles | Sort-Object -Unique)) {
    $fullPath = Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')
    Assert-CrlfTextFile $fullPath $relativePath
}

$contractText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts\IProjectMemoryService.h')
$artifactContractText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts\IProjectMemoryArtifactStore.h')
$serviceMatch = [regex]::Match(
    $contractText,
    'class\s+IProjectMemoryService\s*\{(?<body>.*?)\r?\n\};',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $serviceMatch.Success 'IProjectMemoryService class block'
$serviceText = $serviceMatch.Groups['body'].Value
$toolResultTypes = 'ProjectInitialization|MemoryWriteOutcome|MemoryBatchOutcome|' +
    'MemoryPage|MemoryRecords|ProjectMemoryRecord|ForgetOutcome|LinkOutcome|' +
    'ProjectMemoryExport|ProjectMemoryImport|ProjectMemoryStatus'
$toolMethods = @([regex]::Matches(
    $serviceText,
    'Domain::Result<Domain::(?:' + $toolResultTypes +
        ')>\s+(?<name>[A-Za-z][A-Za-z0-9_]*)\s*\(') |
    ForEach-Object { $_.Groups['name'].Value })
$expectedToolMethods = @(
    'initialize',
    'remember',
    'rememberBatch',
    'search',
    'get',
    'update',
    'forget',
    'listRecent',
    'link',
    'exportMemory',
    'importMemory',
    'status')
Assert-Set $toolMethods $expectedToolMethods `
    'exact twelve typed IProjectMemoryService tool methods'
Assert-Exact $toolMethods.Count 12 'IProjectMemoryService tool-method count'
$managementMethods = @('closeProject','resetProjectMemory','resetAllProjectMemory','shutdown')
foreach ($method in $expectedToolMethods + $managementMethods) {
    Assert-Exact ([regex]::Matches($serviceText, '\b' + $method + '\s*\(').Count) 1 `
        "IProjectMemoryService exact declaration count for $method"
    Assert-Match $serviceText ('\b' + $method + '\s*\(.*?\)\s*noexcept\s*(?:=\s*0)?\s*;') `
        "IProjectMemoryService $method is noexcept"
}
Assert-Exact ([regex]::Matches($serviceText, '\bvirtual\b').Count) 17 `
    'IProjectMemoryService exact virtual member count including destructor'

$interfaceNames = @(
    'IProjectRegistryRepository',
    'IProjectMemoryRepository',
    'IProjectMemoryRepositoryFactory',
    'IProjectMemoryRepositoryOpener')
foreach ($interfaceName in $interfaceNames) {
    Assert-Match $contractText ('class\s+' + $interfaceName + '\s*\{') `
        "$interfaceName public interface declaration" -CaseSensitive
    Assert-Match $contractText ('virtual\s+~' + $interfaceName + '\(\)\s*=\s*default') `
        "$interfaceName virtual destructor" -CaseSensitive
}
Assert-Match $artifactContractText 'class\s+IProjectMemoryArtifactStore\s*\{' `
    'IProjectMemoryArtifactStore public interface declaration' -CaseSensitive
Assert-Match $artifactContractText `
    'virtual\s+~IProjectMemoryArtifactStore\(\)\s*=\s*default' `
    'IProjectMemoryArtifactStore virtual destructor' -CaseSensitive
Assert-Exact ([regex]::Matches(
    $artifactContractText,
    'virtual\s+Domain::Result<Domain::PathText>\s+publish\s*\(').Count) 1 `
    'artifact store exact publish declaration count'
Assert-Exact ([regex]::Matches(
    $artifactContractText,
    'virtual\s+Domain::Result<Domain::ProjectMemoryArtifactDocument>\s+read\s*\(').Count) 1 `
    'artifact store exact read declaration count'
Assert-Exact ([regex]::Matches(
    $artifactContractText,
    'virtual\s+Domain::Result<Domain::PathText>\s+quarantineOversized\s*\(').Count) 1 `
    'artifact store exact oversized-quarantine declaration count'
Assert-Exact ([regex]::Matches(
    $artifactContractText,
    'virtual\s+Domain::Result<Domain::PathText>\s+quarantineCorrupt\s*\(').Count) 1 `
    'artifact store exact quarantine declaration count'
foreach ($method in @('publish','read','quarantineOversized','quarantineCorrupt')) {
    Assert-Match $artifactContractText ('\b' + $method + '\s*\(.*?\)\s*noexcept\s*=\s*0\s*;') `
        "artifact store $method is a noexcept pure virtual boundary"
}
Assert-Match $artifactContractText `
    'MaximumOwnedArtifactFilesPerProject\s*=\s*256U' `
    'artifact store exposes the exact 256-file per-project hard quota' -CaseSensitive
$pureVirtualDeclarations = @([regex]::Matches(
    $contractText + [Environment]::NewLine + $artifactContractText,
    '(?<declaration>[^;{}]*=\s*0\s*;)',
    [Text.RegularExpressions.RegexOptions]::Singleline) |
    ForEach-Object { $_.Groups['declaration'].Value })
Assert-True ($pureVirtualDeclarations.Count -gt 0) `
    'P08 contract pure-virtual declaration inventory is nonempty'
foreach ($declaration in $pureVirtualDeclarations) {
    Assert-Match $declaration '\bnoexcept\b' `
        'every P08 pure-virtual method boundary is noexcept' -CaseSensitive
}

$publicHeaders = @($contractHeaders +
    ($applicationFiles | Where-Object { $_ -match '\.h$' }) + $persistenceHeaders)
$publicText = @($publicHeaders | ForEach-Object {
    $path = Join-Path $WorkspaceRoot $_.Replace('/', '\')
    "// $_" + [Environment]::NewLine + (Get-Content -Raw -LiteralPath $path)
}) -join [Environment]::NewLine
foreach ($relativePath in $publicHeaders) {
    $headerText = Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))
    Assert-Match $headerText '^#pragma once\r?$' "$relativePath uses pragma-once isolation" `
        -CaseSensitive
    Assert-NoMatch $headerText `
        '#\s*include\s*[<"](?:Windows\.h|windows\.h|winrt/|wil/|winsqlite/|sqlite3\.h|nlohmann/)' `
        "$relativePath has no Windows, SQLite, WIL, WinRT, or nlohmann include"
    Assert-NoMatch $headerText '\b(?:throw|try|catch)\b' `
        "$relativePath has no exception transport"
}
Assert-NoMatch $publicText `
    '\b(?:sqlite3|sqlite3_stmt|sqlite3_backup|sqlite3_vfs|sqlite3_file)\b' `
    'P08 public headers leak no SQLite types' -CaseSensitive
Assert-NoMatch $publicText `
    '\b(?:HANDLE|HKEY|HRESULT|DWORD|LPWSTR|LPCWSTR|PCWSTR|OVERLAPPED|SECURITY_ATTRIBUTES)\b' `
    'P08 public headers leak no Windows ownership or ABI types' -CaseSensitive
Assert-NoMatch $publicText `
    '\bnlohmann\b|basic_json|ordered_json|json_pointer|json_sax' `
    'P08 public headers leak no JSON implementation types'
$nonPersistencePublicText = @($contractHeaders +
    ($applicationFiles | Where-Object { $_ -match '\.h$' }) | ForEach-Object {
        Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
    }) -join [Environment]::NewLine
Assert-NoMatch $nonPersistencePublicText '#\s*include\s*[<"]ForgeConductor/Persistence/' `
    'P08 application and contracts do not depend on persistence implementation'

$finalClasses = @(
    'ProjectMemoryRepositoryCache',
    'ProjectMemoryService',
    'WindowsProjectMemoryArtifactStore',
    'WindowsProjectMemoryRepository',
    'WindowsProjectMemoryRepositoryOpener',
    'WindowsProjectRegistryRepository')
foreach ($className in $finalClasses) {
    Assert-Match $publicText ('class\s+' + $className + '\s+final\b') `
        "$className is final" -CaseSensitive
    Assert-Match $publicText `
        ('~' + $className + '\(\)\s*(?:noexcept\s*)?override\s*;') `
        "$className destructor overrides the no-throw interface boundary" -CaseSensitive
    Assert-Match $publicText `
        ($className + '\(\s*const\s+' + $className + '&\)\s*=\s*delete') `
        "$className copy construction is deleted" -CaseSensitive
}
$concreteOverrideDeclarations = @([regex]::Matches(
    $publicText,
    '(?<declaration>[^;{}]*\boverride\s*;)',
    [Text.RegularExpressions.RegexOptions]::Singleline) |
    ForEach-Object { $_.Groups['declaration'].Value } |
    Where-Object { $_ -notmatch '~[A-Za-z][A-Za-z0-9_]*\s*\(' })
Assert-True ($concreteOverrideDeclarations.Count -gt 0) `
    'P08 concrete override declaration inventory is nonempty'
foreach ($declaration in $concreteOverrideDeclarations) {
    Assert-Match $declaration '\bnoexcept\b' `
        'every non-destructor P08 concrete override is noexcept' -CaseSensitive
}
foreach ($modelName in @('ProjectMemoryLimits','ProjectMemoryDescriptor',
    'ProjectMemoryRecord','ProjectMemoryWrite','ProjectInitialization',
    'ProjectMemoryArtifactDocument','ProjectMemoryStatus')) {
    $domainHeaderText = Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain\ProjectMemoryModels.h')
    Assert-Match $domainHeaderText ('struct\s+' + $modelName + '\s+final\b') `
        "$modelName domain model is final" -CaseSensitive
}

$contractsUmbrella = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts\Contracts.h')
foreach ($include in @(
    'ForgeConductor/Contracts/IProjectMemoryArtifactStore.h',
    'ForgeConductor/Contracts/IProjectMemoryService.h')) {
    Assert-Exact ([regex]::Matches(
        $contractsUmbrella,
        '^#include\s+"' + [regex]::Escape($include) + '"\r?$',
        [Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 `
        "contracts umbrella exact include $include"
}
$persistenceUmbrella = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Persistence\Windows\PersistenceWindows.h')
foreach ($relativePath in $persistenceHeaders) {
    $include = $relativePath.Substring('include/'.Length)
    Assert-Exact ([regex]::Matches(
        $persistenceUmbrella,
        '^#include\s+"' + [regex]::Escape($include) + '"\r?$',
        [Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 `
        "persistence umbrella exact include $include"
}

$domainHeader = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain\ProjectMemoryModels.h')
foreach ($limit in ([ordered]@{
    maximumTitleBytes = '512'
    maximumSummaryBytes = '4\s*\*\s*1024'
    maximumBodyBytes = '256\s*\*\s*1024'
    maximumSourceReferenceBytes = '2\s*\*\s*1024'
    maximumTagCount = '32'
    maximumTagBytes = '128'
    maximumRelatedIdCount = '32'
    maximumBatchCount = '50'
    maximumBatchBytes = '1024\s*\*\s*1024'
    maximumQueryBytes = '4\s*\*\s*1024'
    maximumPageCount = '100'
    defaultPageCount = '20'
    maximumResponseBytes = '256\s*\*\s*1024'
    defaultResponseBytes = '64\s*\*\s*1024'
    maximumOpenProjects = '8'
    maximumArtifactRecords = "10'000"
    maximumArtifactBytes = '32\s*\*\s*1024\s*\*\s*1024'
}).GetEnumerator()) {
    Assert-Match $domainHeader `
        ('std::size_t\s+' + $limit.Key + '\s*\{\s*' + $limit.Value + '\s*\}\s*;') `
        "exact project-memory limit $($limit.Key)" -CaseSensitive
}
Assert-Match $domainHeader `
    'MinimumProjectMemoryDeadline\s*\{\s*1\s*\}' `
    'minimum project-memory deadline is 1 ms' -CaseSensitive
Assert-Match $domainHeader `
    "MaximumProjectMemoryDeadline\s*\{\s*60'000\s*\}" `
    'maximum project-memory deadline is 60000 ms' -CaseSensitive
$domainSource = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Domain\ProjectMemoryModels.cpp')
Assert-Match $domainSource `
    'limits\.maximumOpenProjects\s*=\s*budgetsForProfile\(profile\)\.openProjectRepositoriesMaximum' `
    'project-memory cache bound derives from the resource profile' -CaseSensitive
Assert-Match $domainSource `
    'deadline\s*<\s*MinimumProjectMemoryDeadline\s*\|\|\s*deadline\s*>\s*MaximumProjectMemoryDeadline' `
    'project-memory deadline validation is closed at both bounds' -CaseSensitive
$repositorySource = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Persistence\Windows\WindowsProjectMemoryRepository.cpp')
Assert-Match $repositorySource `
    "MaximumRetainedEventRows\s*=\s*10'000" `
    'project-memory event journal has the exact 10,000-row retention ceiling' -CaseSensitive
Assert-Match $repositorySource `
    'class\s+ArtifactSaxHandler\s+final\s*:\s*public\s+Json::json_sax_t' `
    'artifact import uses a private final public-API SAX state machine' -CaseSensitive
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'Json::sax_parse\s*\(').Count) 2 `
    'artifact import has exactly two direct SAX parse paths for schema probing and current-schema parsing'
Assert-Exact ([regex]::Matches(
    $repositorySource,
    '\bparseArtifact\s*\(').Count) 3 `
    'artifact import uses one bounded parser definition for validation and ingestion'
Assert-Match $repositorySource `
    'enum\s+class\s+ArtifactFailureProvenance\s*\{\s*ArtifactValidation\s*,\s*Policy\s*,\s*Dependency\s*\}' `
    'artifact quarantine uses an explicit closed failure-provenance vocabulary' -CaseSensitive
Assert-Match $repositorySource `
    'class\s+ArtifactValidationHasher\s+final\s*:\s*public\s+Contracts::IHasher' `
    'artifact validation wraps injected hash failures as dependency provenance' -CaseSensitive
Assert-Match $repositorySource `
    'class\s+ArtifactValidationRedactor\s+final\s*:\s*public\s+Contracts::IRedactor' `
    'artifact validation distinguishes redaction verdicts from dependency failures' -CaseSensitive
Assert-Match $repositorySource `
    'validationFailureProvenance\s*==\s*ArtifactFailureProvenance::ArtifactValidation' `
    'committed import quarantines only explicitly proven artifact-validation failures' -CaseSensitive
Assert-NoMatch $repositorySource `
    'shouldQuarantineCorruptImport' `
    'artifact quarantine does not infer provenance from public error codes' -CaseSensitive
Assert-NoMatch $repositorySource `
    'std::vector\s*<\s*PreparedWrite\s*>\s+writes\b' `
    'artifact import does not retain an envelope-sized prepared-write vector' -CaseSensitive
Assert-Match $repositorySource `
    'sourceRecordIds_\.reserve\(limits_\.maximumArtifactRecords\)' `
    'streaming import duplicate-id state is reserved at the exact record bound' -CaseSensitive
Assert-Match $repositorySource `
    'content\.reserve\(limits\.maximumArtifactBytes\)' `
    'streaming export owns one final vector reserved at the exact byte bound' -CaseSensitive
Assert-Match $repositorySource `
    'IncrementalHashChunkBytes\s*=\s*1024U\s*\*\s*1024U' `
    'incremental snapshot hashing has an exact 1 MiB cancellation chunk' -CaseSensitive
$artifactStoreSource = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Persistence\Windows\WindowsProjectMemoryArtifactStore.cpp')
Assert-Match $artifactStoreSource `
    'MaximumIoChunkBytes\s*=\s*1024U\s*\*\s*1024U' `
    'artifact I/O and corrupt comparison use an exact 1 MiB chunk ceiling' -CaseSensitive
Assert-Match $artifactStoreSource `
    'MaximumRetainedOversizedArtifactsPerStripe\s*=\s*16U' `
    'oversized artifact retained-handle catalog is bounded per stripe' -CaseSensitive
Assert-Match $artifactStoreSource `
    'MaximumDirectoryEntriesPerScan\s*=\s*1024U' `
    'artifact directory inventory scan is bounded to 1,024 entries' -CaseSensitive
$cacheTests = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectMemoryCacheTests.cpp')
Assert-Match $cacheTests "constexpr\s+std::size_t\s+bounds\[\]\s*\{\s*4U\s*,\s*8U\s*,\s*16U\s*\}" `
    'cache profiles test exact 4/8/16 bounds' -CaseSensitive
$cacheSource = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Application\ProjectMemoryRepositoryCache.cpp')
Assert-Match $cacheSource `
    'entries_\.size\(\)\s*\+\s*pending_\.size\(\)\s*>=\s*maximumOpenRepositories_' `
    'pending repository opens consume bounded cache capacity' -CaseSensitive
Assert-Match $cacheSource `
    'if\s*\(containsPending\(projectId\)\)' `
    'repository close recognizes an in-flight open reservation' -CaseSensitive
$domainTests = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Domain\DomainTests.cpp')
foreach ($profileExpectation in @(
    'projectMemoryLimitsForProfile\(\s*Domain::ResourceProfile::Constrained8GiB\).*?limits\.maximumOpenProjects\s*==\s*4U',
    'Standard16GiB\)\s*\.maximumOpenProjects\s*==\s*8U',
    'Expanded32GiBPlus\)\s*\.maximumOpenProjects\s*==\s*16U')) {
    Assert-Match $domainTests $profileExpectation `
        "project-memory resource-profile expectation $profileExpectation" -CaseSensitive
}

foreach ($relativePath in $adrFiles) {
    $adrText = Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))
    Assert-Match $adrText '^#\s+P08-00[1-3]:' "$relativePath ADR identifier"
    Assert-Exact ([regex]::Matches(
        $adrText,
        '^Status:\s+Accepted\r?$',
        [Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 `
        "$relativePath accepted status"
    Assert-Match $adrText '^##\s+Decision\r?$' "$relativePath decision section"
    Assert-Match $adrText '^##\s+Consequences\r?$' "$relativePath consequences section"
}

Assert-TestCaseInventory `
    (Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectMemoryApplicationTests.cpp') @(
        'project_memory_application.twelve_operations',
        'project_memory_application.security_scope',
        'project_memory_application.reset',
        'project_memory_application.shutdown') 4 'application tests'
Assert-TestCaseInventory `
    (Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectMemoryCacheTests.cpp') @(
        'project_memory_cache.deterministic_lru',
        'project_memory_cache.active_pin',
        'project_memory_cache.profile_bounds_shutdown',
        'project_memory_cache.cancelled_admission',
        'project_memory_cache.concurrent_reservations') 5 'cache tests'
Assert-TestCaseInventory `
    (Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectMemoryRepositoryWindowsTests.cpp') @(
        'project_memory_repository.crud_search_reset',
        'project_memory_repository.atomicity_bounds_redaction',
        'project_memory_repository.isolation_scope',
        'project_memory_repository.ranking_pagination_recent',
        'project_memory_repository.batch_transaction_rollback',
        'project_memory_repository.get_order_duplicates',
        'project_memory_repository.first_row_response_cap',
        'project_memory_repository.post_close',
        'project_memory_repository.restart_durability',
        'project_memory_repository.context_guard',
        'project_memory_repository.persisted_hostile_rows',
        'project_memory_repository.incomplete_fts_unavailable',
        'project_memory_repository.cursor_int64_boundary',
        'project_memory_repository.update_normalization_empty',
        'project_memory_repository.cursor_scalar_exact_envelope',
        'project_memory_repository.tombstone_hash_collision',
        'project_memory_repository.event_journal_retention') 17 'repository Windows tests'
Assert-TestCaseInventory `
    (Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectRegistryWindowsTests.cpp') @(
        'project_registry.persistence_detach',
        'project_registry.conflicting_evidence',
        'project_registry.non_directory',
        'project_registry.hostile_document_recovery',
        'project_registry.unsafe_paths') 5 'registry Windows tests'

Assert-TestCaseInventory `
    (Join-Path $WorkspaceRoot 'tests\ProjectMemory\ProjectMemoryArtifactWindowsTests.cpp') @(
        'project_memory_artifact.store_boundary',
        'project_memory_artifact.oversized_retained_handle',
        'project_memory_artifact.owned_file_quota',
        'project_memory_artifact.quarantine_quota_edge',
        'project_memory_artifact.roundtrip_security_rollback',
        'project_memory_artifact.valid_artifact_database_failure',
        'project_memory_artifact.dependency_failure_provenance',
        'project_memory_artifact.streaming_strictness',
        'project_memory_artifact.future_schema_extensions',
        'project_memory_artifact.macos_golden_compatibility',
        'project_memory_artifact.exact_snapshot_ceilings') 11 'artifact Windows tests'

$cmakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$cmake = Get-Content -Raw -LiteralPath $cmakePath
$domainBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_DOMAIN_SOURCES' `
    'FORGE_DOMAIN_SOURCES declaration'
Assert-Exact @(Get-CMakeTokens $domainBody |
    Where-Object { $_ -ceq 'src/Domain/ProjectMemoryModels.cpp' }).Count 1 `
    'CMake domain project-memory source count'
$applicationBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_APPLICATION_SOURCES' `
    'FORGE_APPLICATION_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $applicationBody | Where-Object { $_ -match '^src/.+\.cpp$' }) @(
    'src/Application/ProjectMemoryRepositoryCache.cpp',
    'src/Application/ProjectMemoryService.cpp') 'CMake exact P08 application source inventory'
$persistenceBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_PERSISTENCE_WINDOWS_SOURCES' `
    'FORGE_PERSISTENCE_WINDOWS_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $persistenceBody | Where-Object {
    $_ -match '^src/Persistence/Windows/(?:WindowsProjectMemory|WindowsProjectRegistry).+\.cpp$'
}) $persistenceSources 'CMake exact P08 persistence source inventory'

$expectedTargets = [ordered]@{
    'ForgeConductor.ProjectMemory.CacheTests' = @(
        'tests/ProjectMemory/ProjectMemoryCacheTests.cpp')
    'ForgeConductor.ProjectMemory.ApplicationTests' = @(
        'tests/ProjectMemory/ProjectMemoryApplicationTests.cpp')
    'ForgeConductor.ProjectMemory.RepositoryWindowsTests' = @(
        'tests/ProjectMemory/ProjectMemoryRepositoryWindowsTests.cpp')
    'ForgeConductor.ProjectMemory.RegistryWindowsTests' = @(
        'tests/ProjectMemory/ProjectRegistryWindowsTests.cpp')
    'ForgeConductor.ProjectMemory.ArtifactWindowsTests' = @(
        'tests/ProjectMemory/ProjectMemoryArtifactWindowsTests.cpp')
}
$expectedLabels = [ordered]@{
    'ForgeConductor.ProjectMemory.CacheTests' = @('T-UNIT','T-MEM','T-STRESS','G08')
    'ForgeConductor.ProjectMemory.ApplicationTests' = @('T-UNIT','T-MEM','T-SEC','G08')
    'ForgeConductor.ProjectMemory.RepositoryWindowsTests' = @(
        'T-UNIT','T-DB','T-MEM','T-SEC','G08')
    'ForgeConductor.ProjectMemory.RegistryWindowsTests' = @(
        'T-UNIT','T-DB','T-MEM','T-SEC','G08')
    'ForgeConductor.ProjectMemory.ArtifactWindowsTests' = @(
        'T-UNIT','T-DB','T-MEM','T-SEC','G08')
}
$expectedTimeouts = [ordered]@{
    'ForgeConductor.ProjectMemory.CacheTests' = 60
    'ForgeConductor.ProjectMemory.ApplicationTests' = 60
    'ForgeConductor.ProjectMemory.RepositoryWindowsTests' = 120
    'ForgeConductor.ProjectMemory.RegistryWindowsTests' = 120
    'ForgeConductor.ProjectMemory.ArtifactWindowsTests' = 180
}
foreach ($target in $expectedTargets.Keys) {
    Assert-Set (Get-CMakeExecutableSources $cmake $target) $expectedTargets[$target] `
        "$target exact CMake source inventory"
    $testArguments = if ($target -ceq 'ForgeConductor.ProjectMemory.ArtifactWindowsTests') {
        '\s+"\$\{PROJECT_SOURCE_DIR\}/tests/ProjectMemory/Fixtures"'
    } else {
        ''
    }
    Assert-Match $cmake `
        ('add_test\s*\(\s*NAME\s+' + [regex]::Escape($target) +
            '\s+COMMAND\s+\$<TARGET_FILE:' + [regex]::Escape($target) + '>' +
            $testArguments + '\s*\)') `
        "$target exact CMake test command" -CaseSensitive
    $propertyBody = Get-CMakeInvocationBody $cmake 'set_tests_properties' $target `
        "$target CMake test properties"
    $labelText = ($expectedLabels[$target] -join ';')
    Assert-Match $propertyBody ('LABELS\s+"' + [regex]::Escape($labelText) + '"') `
        "$target exact CMake labels" -CaseSensitive
    Assert-Match $propertyBody ('TIMEOUT\s+' + $expectedTimeouts[$target] + '\b') `
        "$target exact CMake timeout" -CaseSensitive
}
Assert-Match $cmake `
    'forge_add_layer\s*\(\s*ForgeConductor\.Application\s+ForgeConductor::Application\s+ForgeConductor::Contracts\s*\)' `
    'application layer depends only on contracts' -CaseSensitive
Assert-Match $cmake `
    'forge_add_layer\s*\(\s*ForgeConductor\.Persistence\.Windows\s+ForgeConductor::Persistence\.Windows\s+ForgeConductor::Contracts\s+ForgeConductor::Infrastructure\.Windows\s*\)' `
    'persistence layer depends only on contracts and Windows infrastructure' -CaseSensitive

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count before P08 builds'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L `
    'sealed Forsetti byte count before P08 builds'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti hash before P08 builds'

if ($StaticOnly) {
    Write-Host "G08 static validation passed: $script:AssertionCount fail-closed assertions."
    return
}

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G08: building complete x64 Debug tree from a fresh build directory.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Assert-True $? 'x64 Debug full fresh build'
foreach ($label in @('G08','G07','G06','G05','G04')) {
    Write-Host "G08: testing x64 Debug retained $label inventory."
    & $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Debug $label tests"
}
Write-Host 'G08: building complete x64 Release tree.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Assert-True $? 'x64 Release full build'
foreach ($label in @('G08','G07','G06','G05','G04')) {
    Write-Host "G08: testing x64 Release retained $label inventory."
    & $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Release $label tests"
}

$toolchainStatePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
Assert-True (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) `
    'recorded toolchain state exists'
try {
    $toolchainState = Get-Content -Raw -LiteralPath $toolchainStatePath | ConvertFrom-Json
} catch {
    throw "G08 assertion failed: invalid toolchain state - $($_.Exception.Message)"
}
Assert-True ([bool]$toolchainState.complete) 'recorded toolchain state is complete'
$ctestPath = [string]$toolchainState.tools.ctest
Assert-True (-not [string]::IsNullOrWhiteSpace($ctestPath) -and
    (Test-Path -LiteralPath $ctestPath -PathType Leaf)) `
    'CTest executable from recorded toolchain state'
$msbuildPath = [string]$toolchainState.tools.msbuild
Assert-True (-not [string]::IsNullOrWhiteSpace($msbuildPath) -and
    (Test-Path -LiteralPath $msbuildPath -PathType Leaf)) `
    'MSBuild executable from recorded toolchain state'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$expectedArtifacts = @(
    'lib/{0}/ForgeConductor.Domain.lib',
    'lib/{0}/ForgeConductor.Application.lib',
    'lib/{0}/ForgeConductor.Persistence.Windows.lib',
    'bin/{0}/ForgeConductor.Contracts.ContractTests.exe',
    'bin/{0}/ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Domain.UnitTests.exe',
    'bin/{0}/ForgeConductor.Persistence.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.ProjectMemory.ApplicationTests.exe',
    'bin/{0}/ForgeConductor.ProjectMemory.ArtifactWindowsTests.exe',
    'bin/{0}/ForgeConductor.ProjectMemory.CacheTests.exe',
    'bin/{0}/ForgeConductor.ProjectMemory.RepositoryWindowsTests.exe',
    'bin/{0}/ForgeConductor.ProjectMemory.RegistryWindowsTests.exe')
$artifactHashes = [ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    foreach ($artifactTemplate in $expectedArtifacts) {
        $relativeArtifact = $artifactTemplate -f $configuration
        $artifactPath = Join-Path $buildRoot $relativeArtifact.Replace('/', '\')
        Assert-True (Test-Path -LiteralPath $artifactPath -PathType Leaf) `
            "$configuration artifact missing: $relativeArtifact"
        Assert-True ([long](Get-Item -LiteralPath $artifactPath).Length -gt 0) `
            "$configuration artifact is empty: $relativeArtifact"
        $hash = Get-FileSha256 $artifactPath
        Assert-Match $hash '^[0-9a-f]{64}$' `
            "$configuration artifact hash: $relativeArtifact" -CaseSensitive
        $artifactHashes[$configuration + '/' + $relativeArtifact] = $hash
        Write-Host "$configuration $relativeArtifact SHA-256: $hash"
    }

    $ctestJsonText = (& $ctestPath --test-dir $buildRoot -C $configuration `
        -L G08 --show-only=json-v1) -join [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 "$configuration G08 CTest JSON inventory command"
    try {
        $ctestInventory = $ctestJsonText | ConvertFrom-Json
    } catch {
        throw "G08 assertion failed: invalid $configuration CTest JSON inventory - $($_.Exception.Message)"
    }
    Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) `
        @($expectedTargets.Keys) "$configuration exact G08 CTest inventory"

    $buildRootForward = $buildRoot.Replace('\', '/')
    foreach ($test in @($ctestInventory.tests)) {
        $testName = [string]$test.name
        Assert-Exact ([string]$test.config) $configuration `
            "$configuration CTest configuration for $testName"
        $expectedCommand = @(
            "$buildRootForward/bin/$configuration/$testName.exe")
        if ($testName -ceq 'ForgeConductor.ProjectMemory.ArtifactWindowsTests') {
            $expectedCommand +=
                "$($WorkspaceRoot.Replace('\', '/'))/tests/ProjectMemory/Fixtures"
        }
        Assert-Sequence @($test.command) $expectedCommand `
            "$configuration exact CTest command for $testName"
        Assert-Set @($test.properties | ForEach-Object { $_.name }) @(
            'LABELS','TIMEOUT','WORKING_DIRECTORY') `
            "$configuration CTest property inventory for $testName"
        $labelsProperty = @($test.properties | Where-Object { $_.name -ceq 'LABELS' })
        Assert-Exact $labelsProperty.Count 1 `
            "$configuration CTest label-property count for $testName"
        Assert-Set @($labelsProperty[0].value) @($expectedLabels[$testName]) `
            "$configuration exact CTest labels for $testName"
        $timeoutProperty = @($test.properties | Where-Object { $_.name -ceq 'TIMEOUT' })
        Assert-Exact $timeoutProperty.Count 1 `
            "$configuration CTest timeout-property count for $testName"
        Assert-Exact ([double]$timeoutProperty[0].value) `
            ([double]$expectedTimeouts[$testName]) `
            "$configuration exact CTest timeout for $testName"
        $workingDirectoryProperty = @($test.properties | Where-Object {
            $_.name -ceq 'WORKING_DIRECTORY'
        })
        Assert-Exact $workingDirectoryProperty.Count 1 `
            "$configuration CTest working-directory property count for $testName"
        Assert-Exact ([string]$workingDirectoryProperty[0].value) $buildRootForward `
            "$configuration CTest working directory for $testName"
    }
}

$projectNames = @(
    'ForgeConductor.Domain',
    'ForgeConductor.Application',
    'ForgeConductor.Infrastructure.Windows',
    'ForgeConductor.Persistence.Windows',
    'ForgeConductor.ProjectMemory.ApplicationTests',
    'ForgeConductor.ProjectMemory.ArtifactWindowsTests',
    'ForgeConductor.ProjectMemory.CacheTests',
    'ForgeConductor.ProjectMemory.RepositoryWindowsTests',
    'ForgeConductor.ProjectMemory.RegistryWindowsTests')
$inspectionTargetsPath = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    'ForgeConductor-G08-' + [guid]::NewGuid().ToString('N') + '.targets')
$inspectionTargets = @(
    '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
    '  <ItemGroup>',
    '    <Link Include="__forge_g08_link_probe__" />',
    '    <Lib Include="__forge_g08_lib_probe__" />',
    '  </ItemGroup>',
    '</Project>',
    '') -join [Environment]::NewLine
[IO.File]::WriteAllText(
    $inspectionTargetsPath,
    $inspectionTargets,
    [Text.UTF8Encoding]::new($false))
$evaluatedProjectConfigurationCount = 0
try {
    foreach ($projectName in $projectNames) {
        $projectPath = Join-Path $buildRoot "$projectName.vcxproj"
        Assert-True (Test-Path -LiteralPath $projectPath -PathType Leaf) `
            "generated P08 project $projectName"
        foreach ($configuration in @('Debug','Release')) {
            $evaluation = Invoke-EvaluatedMSBuildQuery `
                $msbuildPath $projectPath $configuration $inspectionTargetsPath
            $evaluatedProjectConfigurationCount++
            Assert-Exact (Get-MSBuildItemField $evaluation.Properties 'Configuration') `
                $configuration "$projectName $configuration evaluated configuration"
            Assert-Exact (Get-MSBuildItemField $evaluation.Properties 'Platform') 'x64' `
                "$projectName $configuration evaluated platform"
            Assert-Exact (Get-MSBuildItemField `
                $evaluation.Properties 'WindowsTargetPlatformVersion') '10.0.26100.0' `
                "$projectName $configuration evaluated SDK"
            Assert-Exact (Get-MSBuildItemField $evaluation.Properties 'PlatformToolset') `
                'v143' "$projectName $configuration evaluated toolset"

            $clrSupport = Get-MSBuildItemField $evaluation.Properties 'CLRSupport'
            Assert-True ([string]::IsNullOrEmpty($clrSupport) -or
                $clrSupport -ceq 'false') `
                "$projectName $configuration effective CLRSupport must be empty or exact false"

            $compileItems = @(Get-MSBuildItems $evaluation 'ClCompile')
            Assert-True ($compileItems.Count -gt 0) `
                "$projectName $configuration has evaluated C++ compile inputs"
            for ($compileIndex = 0; $compileIndex -lt $compileItems.Count; $compileIndex++) {
                $compileItem = $compileItems[$compileIndex]
                $compileContext = "$projectName $configuration ClCompile[$compileIndex]"
                Assert-Exact (Get-MSBuildItemField $compileItem 'LanguageStandard') `
                    'stdcpp20' "$compileContext evaluated C++ language standard"
                Assert-Exact (Get-MSBuildItemField $compileItem 'TreatWarningAsError') `
                    'true' "$compileContext evaluated warnings-as-errors"
                $compileAsManaged = Get-MSBuildItemField $compileItem 'CompileAsManaged'
                Assert-True ([string]::IsNullOrEmpty($compileAsManaged) -or
                    $compileAsManaged -ceq 'false') `
                    "$compileContext effective CompileAsManaged must be empty or exact false"
                $forcedUsingFiles = Get-MSBuildItemField $compileItem 'ForcedUsingFiles'
                Assert-Exact $forcedUsingFiles '' `
                    "$compileContext effective ForcedUsingFiles must be empty"
                $compileOptions = Get-MSBuildItemField $compileItem 'AdditionalOptions'
                Assert-NativeAdditionalOptions $compileOptions `
                    "$compileContext effective AdditionalOptions"
                foreach ($metadataName in @(
                    'AdditionalIncludeDirectories',
                    'AdditionalUsingDirectories',
                    'ForcedUsingFiles',
                    'AdditionalOptions')) {
                    Assert-NoForbiddenEvaluatedDependency `
                        (Get-MSBuildItemField $compileItem $metadataName) `
                        "$compileContext effective $metadataName"
                }
            }

            foreach ($resourceItem in @(Get-MSBuildItems $evaluation 'ResourceCompile')) {
                foreach ($metadataName in @('AdditionalIncludeDirectories','AdditionalOptions')) {
                    $resourceMetadata = Get-MSBuildItemField $resourceItem $metadataName
                    Assert-NoForbiddenEvaluatedDependency $resourceMetadata `
                        "$projectName $configuration ResourceCompile effective $metadataName"
                    if ($metadataName -ceq 'AdditionalOptions') {
                        Assert-NoResponseFileIndirection $resourceMetadata `
                            "$projectName $configuration ResourceCompile effective $metadataName"
                    }
                }
            }

            foreach ($binaryItemType in @('Link','Lib')) {
                $binaryItems = @(Get-MSBuildItems $evaluation $binaryItemType)
                $probeIdentity = "__forge_g08_$($binaryItemType.ToLowerInvariant())_probe__"
                Assert-Exact (@($binaryItems | Where-Object {
                    (Get-MSBuildItemField $_ 'Identity') -ceq $probeIdentity
                }).Count) 1 "$projectName $configuration evaluated $binaryItemType probe count"
                for ($binaryIndex = 0; $binaryIndex -lt $binaryItems.Count; $binaryIndex++) {
                    $binaryItem = $binaryItems[$binaryIndex]
                    $binaryContext =
                        "$projectName $configuration $binaryItemType[$binaryIndex]"
                    $binaryOptions = Get-MSBuildItemField $binaryItem 'AdditionalOptions'
                    Assert-NativeAdditionalOptions $binaryOptions `
                        "$binaryContext effective AdditionalOptions"
                    Assert-NoResponseFileIndirection `
                        (Get-MSBuildItemField $binaryItem 'AdditionalDependencies') `
                        "$binaryContext effective AdditionalDependencies"
                    foreach ($metadataName in @(
                        'AdditionalLibraryDirectories',
                        'AdditionalDependencies',
                        'DelayLoadDLLs',
                        'AdditionalOptions')) {
                        Assert-NoForbiddenEvaluatedDependency `
                            (Get-MSBuildItemField $binaryItem $metadataName) `
                            "$binaryContext effective $metadataName"
                    }
                }
            }

            foreach ($referenceItemType in @(
                'ProjectReference',
                'Reference',
                'COMReference',
                'PackageReference',
                'FrameworkReference',
                'NativeReference')) {
                $referenceItems = @(Get-MSBuildItems $evaluation $referenceItemType)
                for ($referenceIndex = 0;
                    $referenceIndex -lt $referenceItems.Count;
                    $referenceIndex++) {
                    $referenceItem = $referenceItems[$referenceIndex]
                    foreach ($fieldName in @(
                        'Identity',
                        'FullPath',
                        'HintPath',
                        'Name',
                        'FusionName',
                        'Version',
                        'WrapperTool')) {
                        Assert-NoForbiddenEvaluatedDependency `
                            (Get-MSBuildItemField $referenceItem $fieldName) `
                            "$projectName $configuration $referenceItemType[$referenceIndex] $fieldName"
                    }
                }
            }
        }
    }
} finally {
    [IO.File]::Delete($inspectionTargetsPath)
}
Assert-Exact $evaluatedProjectConfigurationCount 18 `
    'exact Debug/Release evaluated MSBuild inspection count for nine P08 projects'
Assert-True (-not (Test-Path -LiteralPath $inspectionTargetsPath)) `
    'evaluated MSBuild inspection targets cleanup'

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after P08 builds'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after P08 builds'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after P08 builds'

$gitOutput = & git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1
Assert-Exact $LASTEXITCODE 0 `
    ('git diff --check failed: ' + ($gitOutput -join [Environment]::NewLine))
& (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'governance ledger verification'

Write-Host "G08 project-memory validation passed: $script:AssertionCount fail-closed assertions; exact 12-operation service surface, five native test targets, $($artifactHashes.Count) Debug/Release output hashes, and x64 Debug/Release G04+G05+G06+G07+G08 tests passed."
