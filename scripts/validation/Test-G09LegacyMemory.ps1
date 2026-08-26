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
    if (-not $Condition) { throw "G09 assertion failed: $Message" }
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

function Get-SourceSlice {
    param(
        [string]$Text,
        [string]$StartMarker,
        [string]$EndMarker,
        [string]$Message
    )
    $start = $Text.IndexOf($StartMarker, [StringComparison]::Ordinal)
    Assert-True ($start -ge 0) "$Message start marker"
    $end = $Text.IndexOf(
        $EndMarker,
        $start + $StartMarker.Length,
        [StringComparison]::Ordinal)
    Assert-True ($end -gt $start) "$Message end marker"
    return $Text.Substring($start, $end - $start)
}

function Assert-MarkerOrder {
    param([string]$Text, [string[]]$Markers, [string]$Message)
    $previous = -1
    for ($index = 0; $index -lt $Markers.Count; $index++) {
        $position = $Text.IndexOf($Markers[$index], [StringComparison]::Ordinal)
        Assert-True ($position -ge 0) "$Message marker $index"
        Assert-True ($position -gt $previous) "$Message order $index"
        $previous = $position
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
        throw "G09 assertion failed: invalid evaluated MSBuild JSON for $Configuration $ProjectPath - $($_.Exception.Message)"
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
    'include/ForgeConductor/Domain/LegacyMemoryModels.h',
    'src/Domain/LegacyMemoryModels.cpp')
$contractHeaders = @(
    'include/ForgeConductor/Contracts/ILegacyMemoryRepository.h',
    'include/ForgeConductor/Contracts/ILegacyMemoryService.h',
    'include/ForgeConductor/Contracts/IUnicodeCanonicalizer.h')
$applicationFiles = @(
    'include/ForgeConductor/Application/LegacyMemoryService.h',
    'src/Application/LegacyMemoryService.cpp')
$unicodeInfrastructureFiles = @(
    'include/ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h',
    'src/Infrastructure/Windows/WindowsUnicodeCanonicalizer.cpp')
$persistenceFiles = @(
    'include/ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h',
    'src/Persistence/Windows/WindowsLegacyMemoryRepository.cpp')
$testFiles = @(
    'tests/LegacyMemory/LegacyMemoryApplicationTests.cpp',
    'tests/LegacyMemory/LegacyMemoryRepositoryWindowsTests.cpp')
$unicodeTestFiles = @(
    'tests/Infrastructure/FoundationWindowsTests.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/StorageWindowsTests.cpp',
    'tests/Infrastructure/WindowsDiagnosticSinkTests.cpp',
    'tests/Infrastructure/WindowsUnicodeCanonicalizerTests.cpp')
$sharedFakeFiles = @(
    'tests/Fakes/FoundationFakes.h',
    'tests/Fakes/ProjectRepositoryFakes.h')
$adrFiles = @(
    '.forge-codex/state/decisions/P09-001-legacy-memory-service-and-repository-boundary.md',
    '.forge-codex/state/decisions/P09-002-legacy-memory-observable-semantics-and-bounds.md',
    '.forge-codex/state/decisions/P09-003-central-store-ownership-migration-and-purge.md',
    '.forge-codex/state/decisions/P09-004-unicode-canonical-tag-semantics.md')
$centralSeamFiles = @(
    'include/ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h',
    'src/Persistence/Windows/WindowsCentralDatabase.cpp')
$umbrellaFiles = @(
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Domain/Domain.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'include/ForgeConductor/Persistence/Windows/PersistenceWindows.h')
$requiredFiles = @(
    '.forge-inputs/archives/SOURCE-HASHES.json',
    '.forge-inputs/archives/Forge-Conductor-MacOS-main.zip',
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Package.swift',
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift',
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift',
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/MemoryToolTests.swift',
    'CMakeLists.txt',
    'scripts/build.ps1',
    'scripts/test.ps1',
    'scripts/validation/Test-G04BuildScaffold.ps1',
    'scripts/validation/Test-G05DomainContracts.ps1',
    'scripts/validation/Test-G06WindowsInfrastructure.ps1',
    'scripts/validation/Test-G07DatabaseMigrations.ps1',
    'scripts/validation/Test-G08ProjectMemory.ps1',
    'scripts/validation/Test-G09LegacyMemory.ps1',
    'src/Persistence/Windows/Migrations/CentralMigrations.cpp',
    'src/Persistence/Windows/Migrations/SchemaMigrator.cpp',
    'tests/Persistence/Fixtures/central-v5.sql') +
    $domainFiles + $contractHeaders + $applicationFiles + $persistenceFiles +
    $unicodeInfrastructureFiles + $testFiles + $unicodeTestFiles +
    $sharedFakeFiles + $adrFiles + $centralSeamFiles + $umbrellaFiles

foreach ($relativePath in @($requiredFiles | Sort-Object -Unique)) {
    $fullPath = Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $fullPath -PathType Leaf) "required P09 file $relativePath"
}

Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('LegacyMemoryModels.h') 'exact P09 domain-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src\Domain') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('LegacyMemoryModels.cpp') 'exact P09 domain-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('ILegacyMemoryRepository.h','ILegacyMemoryService.h') 'exact P09 contract inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Application') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('LegacyMemoryService.h') 'exact P09 application-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src\Application') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('LegacyMemoryService.cpp') 'exact P09 application-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Persistence\Windows') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('WindowsLegacyMemoryRepository.h') 'exact P09 persistence-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src\Persistence\Windows') -File -Filter '*LegacyMemory*' | ForEach-Object { $_.Name }) @('WindowsLegacyMemoryRepository.cpp') 'exact P09 persistence-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts') -File -Filter '*UnicodeCanonicalizer*' | ForEach-Object { $_.Name }) @('IUnicodeCanonicalizer.h') 'exact P09 Unicode contract inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include\ForgeConductor\Infrastructure\Windows') -File -Filter '*UnicodeCanonicalizer*' | ForEach-Object { $_.Name }) @('WindowsUnicodeCanonicalizer.h') 'exact P09 Unicode infrastructure-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src\Infrastructure\Windows') -File -Filter '*UnicodeCanonicalizer*' | ForEach-Object { $_.Name }) @('WindowsUnicodeCanonicalizer.cpp') 'exact P09 Unicode infrastructure-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'tests\Infrastructure') -File -Filter '*UnicodeCanonicalizer*' | ForEach-Object { $_.Name }) @('WindowsUnicodeCanonicalizerTests.cpp') 'exact P09 Unicode infrastructure-test inventory'

$legacyTestRoot = Join-Path $WorkspaceRoot 'tests\LegacyMemory'
Assert-Set @(Get-ChildItem -LiteralPath $legacyTestRoot -Force -File | ForEach-Object { $_.Name }) @($testFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) 'exact P09 two-file test inventory'
Assert-Set @(Get-ChildItem -LiteralPath $legacyTestRoot -Force -Directory | ForEach-Object { $_.Name }) @() 'P09 test subdirectory inventory'
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
Assert-Set @(Get-ChildItem -LiteralPath $decisionRoot -Force -File -Filter 'P09-*.md' | ForEach-Object { $_.Name }) @($adrFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) 'exact P09 four-ADR inventory'

$sourceHashManifestPath = Join-Path $WorkspaceRoot '.forge-inputs\archives\SOURCE-HASHES.json'
Assert-Exact ([long](Get-Item -LiteralPath $sourceHashManifestPath).Length) 723L 'source-hash manifest byte count'
Assert-Exact (Get-FileSha256 $sourceHashManifestPath) '1032838a2da517f391693bef862167bdb7cf434520ef42e314c2983bc2195cd3' 'source-hash manifest SHA-256'
try {
    $sourceHashManifest = Get-Content -Raw -LiteralPath $sourceHashManifestPath | ConvertFrom-Json
} catch {
    throw "G09 assertion failed: invalid source-hash manifest - $($_.Exception.Message)"
}
Assert-Exact ([int]$sourceHashManifest.schema_version) 1 'source-hash manifest schema'
Assert-Exact @($sourceHashManifest.files).Count 4 'source-hash manifest exact archive count'
$macArchiveEntry = @($sourceHashManifest.files | Where-Object { $_.file -ceq 'Forge-Conductor-MacOS-main.zip' })
Assert-Exact $macArchiveEntry.Count 1 'source-hash manifest exact macOS archive entry'
Assert-Exact ([long]$macArchiveEntry[0].bytes) 15040337L 'macOS source archive manifest byte count'
Assert-Exact ([string]$macArchiveEntry[0].sha256) '3e344d4b3bb0fff80487f99a7c69e7ceadf22aa1e64da3a6f2640ea2fa0072dd' 'macOS source archive manifest SHA-256'
$macArchivePath = Join-Path $WorkspaceRoot '.forge-inputs\archives\Forge-Conductor-MacOS-main.zip'
Assert-Exact ([long](Get-Item -LiteralPath $macArchivePath).Length) 15040337L 'macOS source archive actual byte count'
Assert-Exact (Get-FileSha256 $macArchivePath) '3e344d4b3bb0fff80487f99a7c69e7ceadf22aa1e64da3a6f2640ea2fa0072dd' 'macOS source archive actual SHA-256'

$sourceAnchors = [ordered]@{
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Package.swift' = @('2b9da6f8c1debce8fcf55ad647f6efb209a8b8b73e0fe11778feb9362bcbd146',2032L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift' = @('916df67b5ddd32538732cbe82e9c1382e1ddcf2817723055368e54298e325ee7',8116L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift' = @('a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0',39463L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/MemoryToolTests.swift' = @('cc6fb5cce18ac243fae179f66535bfc6ffc173860e3935d240aa64fa815a821a',12488L)
}
foreach ($relativePath in $sourceAnchors.Keys) {
    $fullPath = Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')
    Assert-Exact ([long](Get-Item -LiteralPath $fullPath).Length) ([long]$sourceAnchors[$relativePath][1]) "source anchor byte count $relativePath"
    Assert-Exact (Get-FileSha256 $fullPath) ([string]$sourceAnchors[$relativePath][0]) "source anchor SHA-256 $relativePath"
}
$swiftPackage = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot '.forge-inputs\macos\Forge-Conductor-MacOS-main\Package.swift')
Assert-Exact ([regex]::Matches($swiftPackage,'^//\s*swift-tools-version:\s*6[.]2\s*$',[Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 'source package pins Swift 6.2 canonical-string evidence'

$tokens = $null
$parseErrors = $null
[void][Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$parseErrors)
Assert-Exact @($parseErrors).Count 0 'G09 validator PowerShell parser-error count'

$crlfFiles = @(
    'CMakeLists.txt',
    'scripts/validation/Test-G09LegacyMemory.ps1',
    'src/Persistence/Windows/Migrations/CentralMigrations.cpp',
    'src/Persistence/Windows/Migrations/SchemaMigrator.cpp',
    'tests/Persistence/Fixtures/central-v5.sql') +
    $domainFiles + $contractHeaders + $applicationFiles + $persistenceFiles +
    $unicodeInfrastructureFiles + $testFiles + $unicodeTestFiles +
    $sharedFakeFiles + $adrFiles + $centralSeamFiles + $umbrellaFiles
foreach ($relativePath in @($crlfFiles | Sort-Object -Unique)) {
    Assert-CrlfTextFile (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) $relativePath
}

$textByPath = [ordered]@{}
foreach ($relativePath in @($domainFiles + $contractHeaders + $applicationFiles + $unicodeInfrastructureFiles + $persistenceFiles + $testFiles + $unicodeTestFiles + $sharedFakeFiles + $adrFiles + $centralSeamFiles + $umbrellaFiles | Sort-Object -Unique)) {
    $textByPath[$relativePath] = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))
}
$domainText = @($domainFiles | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$contractText = @($contractHeaders | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$applicationText = @($applicationFiles | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$unicodeInfrastructureText = @($unicodeInfrastructureFiles | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$persistenceText = @($persistenceFiles | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$productText = $domainText + [Environment]::NewLine + $contractText + [Environment]::NewLine + $applicationText + [Environment]::NewLine + $unicodeInfrastructureText + [Environment]::NewLine + $persistenceText
$testText = @($testFiles + $unicodeTestFiles + $sharedFakeFiles | ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine

$p09PublicHeaders = @($domainFiles + $contractHeaders + $applicationFiles + $unicodeInfrastructureFiles + $persistenceFiles | Where-Object { $_ -match '[.]h$' })
foreach ($relativePath in $p09PublicHeaders) {
    Assert-Match $textByPath[$relativePath] '^#pragma once\r?$' "$relativePath uses pragma-once isolation" -CaseSensitive
}
foreach ($layerText in @($domainText,$contractText)) {
    Assert-NoMatch $layerText '#\s*include\s*[<"](?:Windows[.]h|windows[.]h|winrt/|wil/|winsqlite/|sqlite3[.]h|nlohmann/)' 'Domain/Contracts have no Windows, WinUI, WIL, SQLite, or nlohmann include'
    Assert-NoMatch $layerText '\b(?:sqlite3|sqlite3_stmt|sqlite3_backup|sqlite3_vfs|sqlite3_file|HANDLE|HKEY|HRESULT|DWORD|LPWSTR|LPCWSTR|PCWSTR|OVERLAPPED|SECURITY_ATTRIBUTES|winrt)\b' 'Domain/Contracts leak no native platform or database type'
    Assert-NoMatch $layerText '\bnlohmann\b|basic_json|ordered_json|json_pointer|json_sax' 'Domain/Contracts leak no JSON implementation type'
}
Assert-NoMatch $domainText '#\s*include\s*[<"]ForgeConductor/(?:Contracts|Application|Infrastructure|Persistence|UI)/' 'Domain has no outward layer dependency'
Assert-NoMatch $contractText '#\s*include\s*[<"]ForgeConductor/(?:Application|Infrastructure|Persistence|UI)/' 'Contracts have no implementation-layer dependency'
Assert-NoMatch $applicationText '#\s*include\s*[<"]ForgeConductor/Persistence/' 'Application has no persistence dependency'
Assert-NoMatch $applicationText '\b(?:sqlite3|Winsqlite|nlohmann)\b' 'Application contains no database or JSON implementation'
Assert-NoMatch ($productText + [Environment]::NewLine + $testText) '(?i)\b(?:Python|PyBind|Boost|Qt|Electron|node_modules|nodejs|npm|npx|gcnew|System::Runtime)\b|#using|/clr' 'P09 code contains no forbidden runtime or managed dependency'
$noAttributionPattern = '(?i)generated' + ' by|AI-' + 'generated|created' +
    ' with|co-authored-' + 'by|Chat' + 'GPT|Open' + 'AI|automated-authorship'
Assert-NoMatch ($productText + [Environment]::NewLine + $testText) `
    $noAttributionPattern `
    'P09 product and test artifacts contain no prohibited attribution'
Assert-NoMatch $productText '(?i)\b(?:mcp|json[-_]?rpc|toolregistry|tooldispatcher|registerlegacymemorytools)\b' 'P14 wire registration remains explicitly outside the G09 product slice'

$serviceHeader = $textByPath['include/ForgeConductor/Contracts/ILegacyMemoryService.h']
$repositoryHeader = $textByPath['include/ForgeConductor/Contracts/ILegacyMemoryRepository.h']
$serviceMatch = [regex]::Match($serviceHeader,'class\s+ILegacyMemoryService\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
$repositoryMatch = [regex]::Match($repositoryHeader,'class\s+ILegacyMemoryRepository\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $serviceMatch.Success 'ILegacyMemoryService class block'
Assert-True $repositoryMatch.Success 'ILegacyMemoryRepository class block'
$serviceBody = $serviceMatch.Groups['body'].Value
$repositoryBody = $repositoryMatch.Groups['body'].Value
$serviceResultMethods = @([regex]::Matches($serviceBody,'Domain::Result<[^;]+?>\s+(?<name>[A-Za-z][A-Za-z0-9_]*)\s*\(') | ForEach-Object { $_.Groups['name'].Value })
Assert-Set @($serviceResultMethods | Where-Object { $_ -cin @('set','get','list','remove','search') }) @('set','get','list','remove','search') 'exact five typed legacy-memory service operations'
Assert-Set @($serviceResultMethods | Where-Object { $_ -cin @('purge','quickCheck') }) @('purge','quickCheck') 'exact typed legacy-memory management operations'
Assert-Set $serviceResultMethods @('set','get','list','remove','search','purge','quickCheck') 'exact service Result method inventory'
Assert-Exact ([regex]::Matches($serviceBody,'\bvirtual\b').Count) 9 'service exact virtual member count including destructor and shutdown'
foreach ($method in @('set','get','list','remove','search','purge','quickCheck','shutdown')) {
    Assert-Exact ([regex]::Matches($serviceBody,'\b' + $method + '\s*\(').Count) 1 "service exact declaration count for $method"
    Assert-Match $serviceBody ('\b' + $method + '\s*\(.*?\)\s*noexcept\s*=\s*0\s*;') "service $method is a noexcept pure virtual"
}
Assert-NoMatch $serviceBody '\b(?:upsert|open|create|close)\s*\(' 'service does not expose repository lifecycle or SQL vocabulary'

$repositoryResultMethods = @([regex]::Matches($repositoryBody,'Domain::Result<[^;]+?>\s+(?<name>[A-Za-z][A-Za-z0-9_]*)\s*\(') | ForEach-Object { $_.Groups['name'].Value })
Assert-Set $repositoryResultMethods @('upsert','get','list','remove','search','purge','quickCheck') 'exact repository Result method inventory'
Assert-Exact ([regex]::Matches($repositoryBody,'\bvirtual\b').Count) 9 'repository exact virtual member count including destructor and close'
foreach ($method in @('upsert','get','list','remove','search','purge','quickCheck','close')) {
    Assert-Exact ([regex]::Matches($repositoryBody,'\b' + $method + '\s*\(').Count) 1 "repository exact declaration count for $method"
    Assert-Match $repositoryBody ('\b' + $method + '\s*\(.*?\)\s*noexcept\s*=\s*0\s*;') "repository $method is a noexcept pure virtual"
}
foreach ($declaration in @([regex]::Matches($serviceBody + [Environment]::NewLine + $repositoryBody,'(?<declaration>[^;{}]*=\s*0\s*;)',[Text.RegularExpressions.RegexOptions]::Singleline) | ForEach-Object { $_.Groups['declaration'].Value })) {
    Assert-Match $declaration '\bnoexcept\b' 'every P09 pure-virtual method boundary is noexcept' -CaseSensitive
}
Assert-True ([regex]::Matches($serviceBody + $repositoryBody,'const\s+Domain::OperationContext&\s+context').Count -ge 14) 'service/repository operations carry OperationContext'
Assert-NoMatch $serviceHeader 'ILegacyMemoryRepository' 'service contract is separate from repository contract'
Assert-NoMatch $repositoryHeader 'ILegacyMemoryService' 'repository contract is separate from service contract'

$unicodeContractHeader = $textByPath['include/ForgeConductor/Contracts/IUnicodeCanonicalizer.h']
$unicodeLimitsMatch = [regex]::Match($unicodeContractHeader,'struct\s+UnicodeCanonicalizationLimits\s+final\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
$unicodeKeyMatch = [regex]::Match($unicodeContractHeader,'class\s+NfcUtf8Key\s+final\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
$unicodeContractMatch = [regex]::Match($unicodeContractHeader,'class\s+IUnicodeCanonicalizer\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $unicodeLimitsMatch.Success 'Unicode canonicalization limit block'
Assert-True $unicodeKeyMatch.Success 'immutable ordered NfcUtf8Key class block'
Assert-True $unicodeContractMatch.Success 'IUnicodeCanonicalizer class block'
$unicodeLimitsBody = $unicodeLimitsMatch.Groups['body'].Value
$unicodeKeyBody = $unicodeKeyMatch.Groups['body'].Value
$unicodeContractBody = $unicodeContractMatch.Groups['body'].Value
Assert-Match $unicodeLimitsBody 'MaximumInputBytes\s*=\s*1024U\s*\*\s*1024U\s*;' 'Unicode canonicalization exact 1 MiB input bound' -CaseSensitive
Assert-Match $unicodeLimitsBody 'MaximumKeyBytes\s*=\s*3U\s*\*\s*MaximumInputBytes\s*;' 'Unicode canonicalization exact 3 MiB key bound' -CaseSensitive
Assert-MarkerOrder $unicodeKeyBody @(
    'static Domain::Result<NfcUtf8Key> create(',
    'if (value.size() >',
    'UnicodeCanonicalizationLimits::MaximumKeyBytes',
    '!Domain::isValidUtf8(value)',
    'Domain::ErrorCodes::IntegrityFailure',
    'NfcUtf8Key{PrivateTag{}, std::move(value)}') `
    'NfcUtf8Key validating factory bounds and validates before private construction'
$unicodeKeyPrivate = $unicodeKeyBody.IndexOf('private:', [StringComparison]::Ordinal)
Assert-True ($unicodeKeyPrivate -gt 0) 'NfcUtf8Key has a private construction boundary'
$unicodeKeyPublicBody = $unicodeKeyBody.Substring(0, $unicodeKeyPrivate)
Assert-NoMatch $unicodeKeyPublicBody '\bNfcUtf8Key\(\s*(?:PrivateTag|std::string)' 'NfcUtf8Key exposes no directly constructible byte path' -CaseSensitive
Assert-Match $unicodeKeyBody 'private:\s*struct\s+PrivateTag\s+final\s*\{\s*\};\s*NfcUtf8Key\(PrivateTag,\s*std::string\s+value\)\s+noexcept.*?std::string\s+value_\s*;' 'NfcUtf8Key keeps its validating construction token and bytes private' -CaseSensitive
Assert-Exact ([regex]::Matches($unicodeKeyBody,'const\s+std::string&\s+value\(\)\s+const\s+noexcept').Count) 1 'NfcUtf8Key exposes exactly one immutable value accessor'
Assert-Exact ([regex]::Matches($unicodeKeyBody,'static_cast<unsigned char>\(').Count) 2 'NfcUtf8Key orders both operands as unsigned UTF-8 bytes'
Assert-Match $unicodeKeyBody 'return\s+value_[.]size\(\)\s*<=>\s*other[.]value_[.]size\(\)\s*;' 'NfcUtf8Key uses deterministic length tie-breaking' -CaseSensitive
Assert-Match $unicodeContractBody 'MaximumInputBytes\s*=\s*UnicodeCanonicalizationLimits::MaximumInputBytes\s*;' 'Unicode canonicalizer exposes the shared exact input bound' -CaseSensitive
Assert-Match $unicodeContractBody 'MaximumKeyBytes\s*=\s*UnicodeCanonicalizationLimits::MaximumKeyBytes\s*;' 'Unicode canonicalizer exposes the shared exact key bound' -CaseSensitive
Assert-Exact ([regex]::Matches($unicodeContractBody,'\bvirtual\b').Count) 2 'Unicode canonicalizer exact virtual inventory including destructor'
Assert-Match $unicodeContractBody 'virtual\s+Domain::Result<NfcUtf8Key>\s+nfcKey\(\s*std::string_view\s+value\s*\)\s+const\s+noexcept\s*=\s*0\s*;' 'Unicode canonicalizer has one typed noexcept operation' -CaseSensitive

$publicConcreteText = $textByPath['include/ForgeConductor/Application/LegacyMemoryService.h'] + [Environment]::NewLine + $textByPath['include/ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h']
foreach ($className in @('LegacyMemoryService','WindowsLegacyMemoryRepository')) {
    Assert-Match $publicConcreteText ('class\s+' + $className + '\s+final\b') "$className is final" -CaseSensitive
    Assert-Match $publicConcreteText ('~' + $className + '\(\)\s+noexcept\s+override\s*;') "$className destructor is explicit noexcept override" -CaseSensitive
    Assert-Match $publicConcreteText ($className + '\(\s*const\s+' + $className + '&\)\s*=\s*delete\s*;') "$className copy construction is deleted" -CaseSensitive
    Assert-Match $publicConcreteText ('operator=\(\s*const\s+' + $className + '&\)\s*=\s*delete\s*;') "$className copy assignment is deleted" -CaseSensitive
    Assert-Match $publicConcreteText ($className + '\(\s*' + $className + '&&\)\s*=\s*delete\s*;') "$className move construction is deleted" -CaseSensitive
    Assert-Match $publicConcreteText ('operator=\(\s*' + $className + '&&\)\s*=\s*delete\s*;') "$className move assignment is deleted" -CaseSensitive
}
Assert-Match $textByPath['include/ForgeConductor/Application/LegacyMemoryService.h'] 'class\s+Impl\s*;\s*std::unique_ptr<Impl>\s+implementation_\s*;' 'application service has private PImpl ownership' -CaseSensitive
Assert-Match $textByPath['include/ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h'] 'struct\s+Impl\s*;.*?std::unique_ptr<Impl>\s+implementation_\s*;' 'Windows repository has private PImpl ownership'
Assert-Match $textByPath['include/ForgeConductor/Application/LegacyMemoryService.h'] 'LegacyMemoryService\(\s*Contracts::ILegacyMemoryRepository&\s+repository\s*,\s*std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*\)\s*;' 'application service constructor-injects shared immutable Unicode canonicalizer ownership' -CaseSensitive
Assert-Match $textByPath['include/ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h'] 'open\(.*?std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*,\s*const\s+Domain::OperationContext&\s+context\s*\)\s+noexcept\s*;' 'repository open injects shared immutable Unicode canonicalizer ownership'
Assert-Match $textByPath['include/ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h'] 'create\(.*?std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*\)\s+noexcept\s*;' 'repository test seam injects shared immutable Unicode canonicalizer ownership'
foreach ($declaration in @([regex]::Matches($publicConcreteText,'(?<declaration>[^;{}]*\boverride\s*;)',[Text.RegularExpressions.RegexOptions]::Singleline) | ForEach-Object { $_.Groups['declaration'].Value })) {
    Assert-Match $declaration '\bnoexcept\b' 'every concrete P09 override is noexcept' -CaseSensitive
}

$unicodeInfrastructureHeader = $textByPath['include/ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h']
$unicodeInfrastructureSource = $textByPath['src/Infrastructure/Windows/WindowsUnicodeCanonicalizer.cpp']
Assert-Match $unicodeInfrastructureHeader 'class\s+WindowsUnicodeCanonicalizer\s+final\s*:\s*public\s+Contracts::IUnicodeCanonicalizer' 'Windows Unicode canonicalizer is a final contract adapter' -CaseSensitive
Assert-Match $unicodeInfrastructureHeader 'Domain::Result<Contracts::NfcUtf8Key>\s+nfcKey\(\s*std::string_view\s+value\s*\)\s+const\s+noexcept\s+override\s*;' 'Windows Unicode adapter has an exact typed noexcept override' -CaseSensitive
Assert-Exact ([regex]::Matches($unicodeInfrastructureSource,'::NormalizeString\(').Count) 2 'Windows Unicode adapter exact NormalizeString call-site count'
Assert-Match $unicodeInfrastructureSource 'MaximumNormalizedUtf16CodeUnits\s*=\s*Contracts::IUnicodeCanonicalizer::MaximumInputBytes\s*;' 'UTF-16 normalization allocation uses the public input bound' -CaseSensitive
Assert-Match $unicodeInfrastructureSource 'MaximumWriteAttempts\s*=\s*2U\s*;' 'Unicode normalization has exactly two bounded write attempts' -CaseSensitive
$unicodeConversion = Get-SourceSlice $unicodeInfrastructureSource `
    'WindowsUnicodeCanonicalizer::nfcKey(' `
    '} // namespace ForgeConductor::Infrastructure::Windows' `
    'Windows Unicode NFC conversion'
Assert-MarkerOrder $unicodeConversion @(
    'if (value.size() > MaximumInputBytes)',
    'auto utf16 = Detail::strictUtf8ToUtf16(value)',
    'const int estimated = ::NormalizeString(',
    'if (static_cast<std::size_t>(estimated) >',
    'for (std::size_t attempt = 0; attempt < MaximumWriteAttempts; ++attempt)',
    'normalized.assign(capacity, L''\0'')',
    'const int written = ::NormalizeString(',
    'auto utf8 = Detail::strictUtf16ToUtf8(normalized)',
    'if (utf8.value().size() > MaximumKeyBytes)',
    'return Contracts::NfcUtf8Key::create(std::move(utf8).value())') `
    'Unicode conversion bounds input, estimate, write attempts, UTF-8 output, and validates its typed key'
Assert-Match $unicodeConversion 'const\s+int\s+estimated\s*=\s*::NormalizeString\(\s*NormalizationC\s*,\s*utf16[.]value\(\)[.]data\(\)\s*,\s*inputLength\s*,\s*nullptr\s*,\s*0\s*\)' 'first NFC pass obtains an explicitly bounded output estimate'
Assert-Match $unicodeConversion 'const\s+int\s+written\s*=\s*::NormalizeString\(\s*NormalizationC\s*,\s*utf16[.]value\(\)[.]data\(\)\s*,\s*inputLength\s*,\s*normalized[.]data\(\)\s*,\s*static_cast<int>\(normalized[.]size\(\)\)\s*\)' 'second NFC pass uses explicit input and output lengths'
Assert-MarkerOrder $unicodeConversion @(
    'if (suggested == 0U ||',
    'suggested > MaximumNormalizedUtf16CodeUnits)',
    'capacity = static_cast<std::size_t>(suggested)') `
    'Unicode retry capacity is validated before reuse'
Assert-NoMatch $unicodeConversion 'NormalizeString\(.*?\b-1\b' 'Unicode normalization never uses NUL-terminated input length'
Assert-Match $unicodeConversion 'catch\s*\(\.\.\.\).*?ErrorCodes::InternalFailure' 'Unicode adapter converts allocation exceptions to a typed result'

$domainHeader = $textByPath['include/ForgeConductor/Domain/LegacyMemoryModels.h']
foreach ($modelName in @(
    'LegacyMemoryLimits','MemoryNote','LegacyMemorySetRequest','LegacyMemoryUpsert',
    'LegacyMemoryGetRequest','LegacyMemoryListRequest','LegacyMemoryListQuery',
    'LegacyMemoryRemoveRequest','LegacyMemorySearchRequest','LegacyMemorySearchQuery',
    'LegacyMemoryNoteProjection','LegacyMemorySetOutcome','LegacyMemoryGetOutcome',
    'LegacyMemoryListOutcome','LegacyMemoryDeleteOutcome','LegacyMemorySearchOutcome',
    'LegacyMemoryPurgeOutcome')) {
    Assert-Match $domainHeader ('struct\s+' + $modelName + '\s+final\b') "$modelName is final" -CaseSensitive
}
$limitPatterns = [ordered]@{
    MaximumKeyBytes = '512U'
    MaximumBodyBytes = '512U\s*\*\s*1024U'
    MaximumTagCount = '32U'
    MaximumTagBytes = '128U'
    MaximumFilterBytes = '512U'
    MaximumQueryBytes = '4U\s*\*\s*1024U'
    DefaultQueryLimit = '50U'
    MaximumQueryLimit = '200U'
}
foreach ($limit in $limitPatterns.GetEnumerator()) {
    Assert-Match $domainHeader ('static\s+constexpr\s+std::size_t\s+' + $limit.Key + '\s*=\s*' + $limit.Value + '\s*;') "exact legacy-memory limit $($limit.Key)" -CaseSensitive
}
$errorConstants = [ordered]@{
    InvalidKey = 'invalid_key'
    MissingBody = 'missing_body'
    BodyTooLarge = 'body_too_large'
    MissingQuery = 'missing_query'
    EmptyQuery = 'empty_query'
    StoreError = 'store_error'
}
foreach ($constant in $errorConstants.GetEnumerator()) {
    Assert-Exact ([regex]::Matches($domainHeader,'inline\s+constexpr\s+std::string_view\s+' + $constant.Key + '\s*=\s*"' + $constant.Value + '"\s*;').Count) 1 "source-visible error constant $($constant.Key)"
}
$domainSource = $textByPath['src/Domain/LegacyMemoryModels.cpp']
$applicationSource = $textByPath['src/Application/LegacyMemoryService.cpp']
$applicationTests = $textByPath['tests/LegacyMemory/LegacyMemoryApplicationTests.cpp']
$repositoryTests = $textByPath['tests/LegacyMemory/LegacyMemoryRepositoryWindowsTests.cpp']
$foundationFakes = $textByPath['tests/Fakes/FoundationFakes.h']
$projectRepositoryFakes = $textByPath['tests/Fakes/ProjectRepositoryFakes.h']
$semanticsAdr = $textByPath['.forge-codex/state/decisions/P09-002-legacy-memory-observable-semantics-and-bounds.md']
Assert-Match $domainSource 'requested\s*<\s*1.*?return\s+1U\s*;.*?requested\s*>\s*maximum.*?return\s+LegacyMemoryLimits::MaximumQueryLimit\s*;.*?return\s+static_cast<std::size_t>\(requested\)\s*;' 'limit clamp is exactly inclusive 1 through 200'

$systemResponsePredicate = Get-SourceSlice $domainSource `
    'bool isSystemMemoryKey(' 'bool isHiddenLegacyMemoryKey(' `
    'case-sensitive response system-key predicate'
$hiddenVisibilityPredicate = Get-SourceSlice $domainSource `
    'bool isHiddenLegacyMemoryKey(' 'bool isValidUtf8(' `
    'SQLite-compatible hidden-key visibility predicate'
Assert-Exact ([regex]::Matches($systemResponsePredicate,'key[.]starts_with\("agent_run/"\)|key[.]starts_with\("agent_active/"\)|key[.]starts_with\("continuity/"\)').Count) 3 'exact three case-sensitive response system-prefix predicates'
foreach ($prefix in @('agent_run/','agent_active/','continuity/')) {
    Assert-Exact ([regex]::Matches($systemResponsePredicate,'key[.]starts_with\("' + [regex]::Escape($prefix) + '"\)').Count) 1 "exact case-sensitive response system prefix $prefix"
    Assert-Exact ([regex]::Matches($hiddenVisibilityPredicate,'asciiCaseInsensitivePrefix\(key,\s*"' + [regex]::Escape($prefix) + '"\)').Count) 1 "exact ASCII-insensitive visibility prefix $prefix"
}
Assert-NoMatch $systemResponsePredicate 'asciiCaseInsensitivePrefix|foldAscii|tolower|toupper' 'response system-key predicate performs no case folding' -CaseSensitive
Assert-NoMatch $hiddenVisibilityPredicate '[.]starts_with\(' 'hidden-key visibility does not reuse the case-sensitive response predicate' -CaseSensitive
Assert-Exact ([regex]::Matches($domainHeader,'isSystemMemoryKey\(std::string_view\s+key\)\s+noexcept\s*;').Count) 1 'domain declares the case-sensitive response system-key predicate'
Assert-Exact ([regex]::Matches($domainHeader,'isHiddenLegacyMemoryKey\(std::string_view\s+key\)\s+noexcept\s*;').Count) 1 'domain declares the ASCII-insensitive visibility predicate'

$nullAdmissionHelper = Get-SourceSlice $domainSource `
    '[[nodiscard]] bool containsEmbeddedNull(' `
    '[[nodiscard]] unsigned char foldAscii(' `
    'embedded-NUL admission helper'
Assert-Match $nullAdmissionHelper 'return\s+value[.]find\(''\\0''\)\s*!=\s*std::string_view::npos\s*;' 'embedded-NUL helper rejects the U+0000 byte' -CaseSensitive

$filterNormalization = Get-SourceSlice $domainSource `
    '[[nodiscard]] Result<std::optional<std::string>> normalizeFilter(' `
    '[[nodiscard]] Result<void> validateStoredTags(' `
    'legacy-memory filter normalization'
Assert-MarkerOrder $filterNormalization @(
    'if (filter->size() > LegacyMemoryLimits::MaximumFilterBytes)',
    'if (containsEmbeddedNull(*filter))',
    'if (!isValidUtf8(*filter))') `
    'raw filter cap precedes NUL and UTF-8 scans'
Assert-Match $filterNormalization 'containsEmbeddedNull\(\*filter\).*?ErrorCodes::InvalidRequest.*?U\+0000' 'prefix and tag filters reject U+0000 before repository admission' -CaseSensitive

$keyNormalization = Get-SourceSlice $domainSource `
    'Result<std::string> normalizeLegacyMemoryKey(' `
    'Result<std::string> normalizeLegacyMemoryBody(' `
    'legacy-memory key normalization'
Assert-MarkerOrder $keyNormalization @(
    'if (key.size() > LegacyMemoryLimits::MaximumKeyBytes)',
    'auto normalized = trimUtf8(key, "Memory key")',
    'while (offset < value.size())') `
    'raw key cap precedes trim and scalar iteration'

$tagNormalization = Get-SourceSlice $domainSource `
    'Result<std::vector<std::string>> prepareLegacyMemoryTags(' `
    'Result<std::string> normalizeLegacyMemoryGetRequest(' `
    'legacy-memory raw tag preparation'
Assert-MarkerOrder $tagNormalization @(
    'if (tags.size() > LegacyMemoryLimits::MaximumTagCount)',
    'for (const auto& raw : tags)',
    'if (raw.size() > LegacyMemoryLimits::MaximumTagBytes)',
    'if (containsEmbeddedNull(raw))',
    'auto normalized = trimUtf8(raw, "Memory tag")') `
    'raw tag count and per-tag cap precede iteration, NUL scan, and trim'
Assert-Match $tagNormalization 'containsEmbeddedNull\(raw\).*?ErrorCodes::InvalidRequest.*?U\+0000' 'write tags reject U+0000 before normalization' -CaseSensitive
Assert-NoMatch ($domainHeader + [Environment]::NewLine + $domainSource) '\b(?:normalizeLegacyMemoryTags|normalizeLegacyMemorySetRequest)\b' 'Domain exposes no stale pre-canonicalization tag or set normalizer' -CaseSensitive

$bodyNormalization = Get-SourceSlice $domainSource `
    'Result<std::string> normalizeLegacyMemoryBody(' `
    'Result<std::vector<std::string>> prepareLegacyMemoryTags(' `
    'legacy-memory body normalization'
Assert-Match $bodyNormalization 'if\s*\(\s*!body\s*\).*?ErrorCodes::MissingBody' 'missing body uses the source-visible error'
Assert-Match $bodyNormalization 'body->size\(\)\s*>\s*LegacyMemoryLimits::MaximumBodyBytes.*?ErrorCodes::BodyTooLarge' 'body byte ceiling uses the source-visible error'
Assert-MarkerOrder $bodyNormalization @(
    'if (body->size() > LegacyMemoryLimits::MaximumBodyBytes)',
    'if (containsEmbeddedNull(*body))',
    'if (!isValidUtf8(*body))',
    'return Result<std::string>::success(*body)') `
    'raw body cap precedes NUL and UTF-8 scans'
Assert-Match $bodyNormalization 'containsEmbeddedNull\(\*body\).*?ErrorCodes::InvalidRequest.*?U\+0000' 'memory body rejects U+0000 before repository admission' -CaseSensitive
Assert-NoMatch $bodyNormalization 'body\s*(?:->|[.])\s*(?:append|assign|clear|erase|insert|pop_back|push_back|replace|resize)\s*\(|(?:substr|trimUtf8)\s*\(\s*\*body' 'memory body has no direct trim, truncation, or mutation expression' -CaseSensitive
Assert-Match $bodyNormalization 'Result<std::string>::success\(\*body\)' 'memory body is copied byte-for-byte into the normalized value' -CaseSensitive

$applicationTagCanonicalization = Get-SourceSlice $applicationSource `
    '[[nodiscard]] Domain::Result<std::vector<std::string>> canonicalizeTags(' `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemoryUpsert> normalizeSetRequest(' `
    'application NFC tag canonicalization'
Assert-MarkerOrder $applicationTagCanonicalization @(
    'auto prepared = Domain::prepareLegacyMemoryTags(tags)',
    'std::map<Contracts::NfcUtf8Key, std::string> unique',
    'for (auto& tag : prepared.value())',
    'auto key = canonicalizer.nfcKey(tag)',
    'unique.try_emplace(',
    'for (auto& [key, original] : unique)',
    'result.push_back(std::move(original))') `
    'application deduplicates and orders by NFC key while retaining the first original spelling'
Assert-NoMatch $applicationTagCanonicalization 'insert_or_assign|unique\s*\[' 'canonical duplicate admission never replaces the first original spelling' -CaseSensitive

$applicationSetNormalization = Get-SourceSlice $applicationSource `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemoryUpsert> normalizeSetRequest(' `
    '[[nodiscard]] Domain::Result<bool> containsCanonicalTag(' `
    'application set normalization'
Assert-MarkerOrder $applicationSetNormalization @(
    'Domain::normalizeLegacyMemoryKey(request.key)',
    'Domain::normalizeLegacyMemoryBody(request.body)',
    'canonicalizeTags(request.tags, canonicalizer)',
    'Domain::LegacyMemoryUpsert{') `
    'production set normalization validates exact body bytes before NFC tag canonicalization'
Assert-NoMatch $applicationSource 'Domain::normalizeLegacyMemorySetRequest\(' 'production service does not bypass injected NFC tag canonicalization' -CaseSensitive

$applicationCanonicalFilter = Get-SourceSlice $applicationSource `
    '[[nodiscard]] Domain::Result<bool> containsCanonicalTag(' `
    '[[nodiscard]] Domain::Result<void> validateProjectionPage(' `
    'application NFC tag-filter predicate'
Assert-MarkerOrder $applicationCanonicalFilter @(
    'const Contracts::NfcUtf8Key& candidate',
    'for (const auto& tag : tags)',
    'auto tagKey = canonicalizer.nfcKey(tag)',
    'if (tagKey.value() == candidate)') `
    'application postcondition compares stored tags to one precomputed NFC filter key'

$searchNormalization = Get-SourceSlice $domainSource `
    'Result<LegacyMemorySearchQuery> normalizeLegacyMemorySearchRequest(' `
    'Result<void> validateMemoryNote(' `
    'legacy-memory search normalization'
Assert-MarkerOrder $searchNormalization @(
    'if (request.query->size() > LegacyMemoryLimits::MaximumQueryBytes)',
    'if (containsEmbeddedNull(*request.query))',
    'auto query = trimUtf8(*request.query, "Memory search query")') `
    'raw query cap precedes NUL scan, trim, and scalar decode'
Assert-Match $searchNormalization 'containsEmbeddedNull\(\*request[.]query\).*?ErrorCodes::InvalidRequest.*?U\+0000' 'search query rejects U+0000 before repository admission' -CaseSensitive

Assert-NoMatch $tagNormalization '\b(?:std::set|std::map)\b|\bsort\s*\(' 'Domain tag preparation does not own canonical dedupe or ordering' -CaseSensitive
Assert-Match $domainSource 'if\s*\(\s*requested\s*<\s*1\s*\)' 'non-positive limits clamp to one' -CaseSensitive
Assert-Match $domainSource 'if\s*\(\s*requested\s*>\s*maximum\s*\)' 'over-maximum limits clamp to 200' -CaseSensitive

$projectionValidation = Get-SourceSlice $applicationSource `
    '[[nodiscard]] Domain::Result<void> validateProjectionPage(' `
    '} // namespace' `
    'application projection validation'
Assert-Exact ([regex]::Matches($projectionValidation,'Domain::isHiddenLegacyMemoryKey\(note[.]key\)').Count) 1 'application validates default visibility with the ASCII-insensitive predicate'
Assert-MarkerOrder $projectionValidation @(
    'std::optional<Contracts::NfcUtf8Key> tagKey',
    'auto normalizedTag = canonicalizer.nfcKey(*tag)',
    'tagKey.emplace(std::move(normalizedTag).value())',
    'for (const auto& note : notes)',
    'note.tags, *tagKey, canonicalizer') `
    'application computes one NFC filter key before validating the bounded dependency page'
Assert-Match $applicationSource 'Impl\(\s*Contracts::ILegacyMemoryRepository&\s+repository\s*,\s*std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*\)\s+noexcept\s*:\s*repository_\s*\{\s*repository\s*\}\s*,\s*unicodeCanonicalizer_\s*\{\s*std::move\(unicodeCanonicalizer\)\s*\}' 'application PImpl retains repository authority and shared immutable canonicalizer ownership' -CaseSensitive
Assert-Match $applicationSource 'std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer_\s*;' 'application PImpl stores shared immutable canonicalizer ownership' -CaseSensitive
$serviceConstruction = Get-SourceSlice $applicationSource `
    'LegacyMemoryService::LegacyMemoryService(' `
    'LegacyMemoryService::~LegacyMemoryService() noexcept' `
    'public legacy-memory service construction'
Assert-MarkerOrder $serviceConstruction @(
    'std::shared_ptr<const Contracts::IUnicodeCanonicalizer>',
    'if (!unicodeCanonicalizer)',
    'throw std::invalid_argument{',
    'return std::make_unique<Impl>(',
    'repository, std::move(unicodeCanonicalizer)') `
    'public service rejects a null canonicalizer then transfers shared ownership into its PImpl'

$removeOperation = Get-SourceSlice $applicationSource `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(' `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(' `
    'application delete operation'
Assert-MarkerOrder $removeOperation @(
    'repository_.remove(key.value(), context)',
    'value.deleted != value.existed',
    'value.systemKey != Domain::isSystemMemoryKey(key.value())') `
    'delete validates deleted equals existed and exact response classification'
Assert-Match $removeOperation 'value[.]deleted\s*!=\s*value[.]existed.*?dependencyIntegrityFailure<Domain::LegacyMemoryDeleteOutcome>' 'delete flag mismatch is a typed integrity failure' -CaseSensitive

Assert-Match $applicationTests 'std::string\(Domain::LegacyMemoryLimits::MaximumKeyBytes,\s*'' ''\)\s*\+\s*"k".*?paddedOversizedKey[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidKey' 'application test rejects a trim-reducible oversized raw key' -CaseSensitive
Assert-Match $applicationTests 'emptyTagFlood[.]tags[.]assign\(\s*Domain::LegacyMemoryLimits::MaximumTagCount\s*\+\s*1U,\s*""\).*?emptyTags[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::LimitExceeded' 'application test caps raw tag count before dropping empty tags' -CaseSensitive
Assert-Match $applicationTests 'duplicateTagFlood[.]tags[.]assign\(\s*Domain::LegacyMemoryLimits::MaximumTagCount\s*\+\s*1U,\s*"duplicate"\).*?duplicateTags[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::LimitExceeded' 'application test caps raw tag count before deduplication' -CaseSensitive
Assert-Match $applicationTests 'std::string\(Domain::LegacyMemoryLimits::MaximumTagBytes,\s*'' ''\)\s*\+\s*"t".*?oversizedRawTag[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' 'application test caps raw tag bytes before trimming' -CaseSensitive
Assert-Match $applicationTests 'std::string\(Domain::LegacyMemoryLimits::MaximumFilterBytes,\s*'' ''\)\s*\+\s*"p".*?oversizedRawFilter[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' 'application test caps raw filter bytes before admission' -CaseSensitive
Assert-Match $applicationTests 'std::string\(Domain::LegacyMemoryLimits::MaximumQueryBytes,\s*'' ''\)\s*\+\s*"q".*?paddedLongQuery[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' 'application test caps raw query bytes before trimming' -CaseSensitive
Assert-Match $applicationTests 'std::string\s*\{\s*"body\\0tail"\s*,\s*9\s*\}.*?nullBody[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest' 'application test rejects U+0000 in a body' -CaseSensitive
Assert-Match $applicationTests 'std::string\s*\{\s*"tag\\0tail"\s*,\s*8\s*\}.*?embeddedNullTag[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest' 'application test rejects U+0000 in a write tag' -CaseSensitive
Assert-Match $applicationTests 'std::string\s*\{\s*"pre\\0fix"\s*,\s*7\s*\}.*?nullPrefix[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest' 'application test rejects U+0000 in a prefix filter' -CaseSensitive
Assert-Match $applicationTests 'std::string\s*\{\s*"tag\\0filter"\s*,\s*10\s*\}.*?nullTagFilter[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest' 'application test rejects U+0000 in a tag filter' -CaseSensitive
Assert-Match $applicationTests 'std::string\s*\{\s*"que\\0ry"\s*,\s*6\s*\}.*?nullQuery[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest' 'application test rejects U+0000 in a search query' -CaseSensitive
Assert-Match $applicationTests 'nullQuery[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::InvalidRequest.*?fixture[.]repository[.]calls\s*==\s*callsBeforeValidation' 'all bounded-admission and NUL failures occur before repository dispatch' -CaseSensitive
Assert-Match $applicationTests 'Corruption::RemoveFlags.*?const\s+auto\s+corruptFlags\s*=.*?REQUIRE\(!corruptFlags\).*?corruptFlags[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::IntegrityFailure' 'application test rejects deleted/existed disagreement' -CaseSensitive
Assert-Exact ([regex]::Matches($applicationTests,'^#include\s+"Fakes/FoundationFakes[.]h"\r?$',[Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 'application test imports the shared deterministic foundation fakes exactly once'
Assert-NoMatch $applicationTests 'class\s+[A-Za-z0-9_]*UnicodeCanonicalizer[A-Za-z0-9_]*\s+final\s*:\s*public\s+Contracts::IUnicodeCanonicalizer' 'application test defines no duplicate local Unicode canonicalizer fake' -CaseSensitive
Assert-Match $applicationTests 'makeUnicodeCanonicalizerFake\(\).*?std::make_shared<Fakes::UnicodeCanonicalizerFake>\(.*?Fakes::UnicodeCanonicalizerFake::Mapping.*?\{\s*decomposedEAcute\s*,\s*composedEAcute\s*\}.*?\{\s*composedEAcute\s*,\s*composedEAcute\s*\}' 'application test configures canonical-equivalence mappings through the shared Unicode fake' -CaseSensitive
Assert-Match $applicationTests 'std::shared_ptr<Fakes::UnicodeCanonicalizerFake>\s+unicodeCanonicalizer\s*\{\s*makeUnicodeCanonicalizerFake\(\)\s*\}\s*;.*?ScriptedLegacyMemoryRepository\s+repository\s*\{\s*\*unicodeCanonicalizer\s*\}\s*;.*?LegacyMemoryService\s+service\s*\{\s*repository\s*,\s*unicodeCanonicalizer\s*\}' 'application fixture shares one canonicalizer across its repository double and production service' -CaseSensitive

$applicationOperationsTest = Get-SourceSlice $applicationTests `
    'void fiveOperationsNormalizeAndProject()' `
    'void validationAndLimitErrors()' `
    'application five-operation parity test'
Assert-MarkerOrder $applicationOperationsTest @(
    'noteRequest("Agent_run/case-variant", "hidden note")',
    'const auto defaultVisibilityList = fixture.service.list(',
    'return note.key == "Agent_run/case-variant"',
    'const auto defaultVisibilitySearch = fixture.service.search(',
    'REQUIRE(defaultVisibilitySearch.value().notes.empty())',
    'const auto removedCaseVariantSystem = fixture.service.remove(',
    'REQUIRE(removedCaseVariantSystem.value().deleted)',
    'REQUIRE(removedCaseVariantSystem.value().existed)',
    'REQUIRE(!removedCaseVariantSystem.value().systemKey)') `
    'application test distinguishes ASCII-insensitive visibility from case-sensitive delete response classification'
Assert-Match $applicationTests 'Corruption::ListSystemKey.*?"Agent_run/latest"' 'application fake can expose a mixed-case SQLite-visible system row' -CaseSensitive
Assert-Match $applicationTests 'fixture[.]repository[.]corruption\s*=\s*Corruption::ListSystemKey.*?REQUIRE\(!hiddenSystem\).*?hiddenSystem[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::IntegrityFailure' 'application test rejects a mixed-case hidden-row leak' -CaseSensitive

Assert-Match $applicationOperationsTest 'const\s+std::string\s+decomposedEAcute\s*\{\s*"e\\xCC\\x81"\s*,\s*3U\s*\}.*?const\s+std::string\s+composedEAcute\s*\{\s*"\\xC3\\xA9"\s*,\s*2U\s*\}' 'application test supplies canonically equivalent decomposed and composed tags' -CaseSensitive
Assert-Match $applicationOperationsTest 'const\s+std::string\s+bmpPrivateUse\s*\{\s*"\\xEE\\x80\\x80"\s*,\s*3U\s*\}.*?const\s+std::string\s+supplementary\s*\{\s*"\\xF0\\x90\\x80\\x80"\s*,\s*4U\s*\}' 'application test supplies BMP and supplementary scalar-order boundaries' -CaseSensitive
Assert-Match $applicationOperationsTest 'unicodeSet[.]value\(\)[.]note[.]tags\s*==\s*std::vector<std::string>\s*\(\s*\{\s*decomposedEAcute\s*,\s*bmpPrivateUse\s*,\s*supplementary\s*\}\s*\)' 'application test pins NFC dedupe, first-spelling retention, and scalar ordering' -CaseSensitive
Assert-Match $applicationOperationsTest 'LegacyMemoryListRequest\s*\{\s*"unicode/"\s*,\s*composedEAcute.*?unicodeList[.]value\(\)[.]notes[.]front\(\)[.]tags[.]front\(\)\s*==\s*decomposedEAcute' 'application test pins cross-form NFC tag filtering without rewriting visible bytes' -CaseSensitive

$sharedUnicodeFake = Get-SourceSlice $foundationFakes `
    'class UnicodeCanonicalizerFake final' `
    'class FakeClock final' `
    'shared deterministic Unicode canonicalizer fake'
Assert-Match $sharedUnicodeFake ':\s*public\s+Contracts::IUnicodeCanonicalizer' 'shared Unicode fake implements the canonicalizer contract' -CaseSensitive
Assert-MarkerOrder $sharedUnicodeFake @(
    'if (value.size() > MaximumInputBytes)',
    'for (const auto& [input, key] : mappings_)',
    'return Contracts::NfcUtf8Key::create(key)',
    'return Contracts::NfcUtf8Key::create(std::string{value})') `
    'shared Unicode fake bounds admission and routes mapped and identity keys through the validating factory'
Assert-NoMatch $sharedUnicodeFake 'NfcUtf8Key\s*\{' 'shared Unicode fake cannot bypass typed-key validation' -CaseSensitive

$sharedLegacyFake = Get-SourceSlice $projectRepositoryFakes `
    'class LegacyMemoryServiceFake final' `
    '} // namespace ForgeConductor::Tests::Fakes' `
    'shared deterministic legacy-memory fake'
Assert-Match $sharedLegacyFake 'std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*=\s*std::make_shared<UnicodeCanonicalizerFake>\(\)' 'shared legacy-memory fake constructor owns an immutable canonicalizer by default' -CaseSensitive
Assert-Match $sharedLegacyFake 'std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer_\s*;' 'shared legacy-memory fake retains immutable canonicalizer ownership' -CaseSensitive
$sharedFakeSetNormalization = Get-SourceSlice $sharedLegacyFake `
    'normalizeSetRequest(const Domain::LegacyMemorySetRequest& request) const' `
    '[[nodiscard]] Domain::Result<bool> containsCanonicalTag(' `
    'shared fake set normalization'
Assert-MarkerOrder $sharedFakeSetNormalization @(
    'Domain::normalizeLegacyMemoryKey(request.key)',
    'Domain::normalizeLegacyMemoryBody(request.body)',
    'Domain::prepareLegacyMemoryTags(request.tags)',
    'std::map<Contracts::NfcUtf8Key, std::string> unique',
    'unicodeCanonicalizer_->nfcKey(tag)',
    'unique.try_emplace(',
    'tags.push_back(std::move(original))') `
    'shared fake applies bounded Domain admission then NFC dedupe/order with first-original retention'
Assert-NoMatch $sharedFakeSetNormalization 'insert_or_assign|unique\s*\[' 'shared fake canonical duplicates do not replace the first original spelling' -CaseSensitive
$sharedFakeCanonicalFilter = Get-SourceSlice $sharedLegacyFake `
    '[[nodiscard]] Domain::Result<bool> containsCanonicalTag(' `
    '[[nodiscard]] static unsigned char foldAscii(' `
    'shared fake canonical tag filter'
Assert-MarkerOrder $sharedFakeCanonicalFilter @(
    'auto candidateKey = unicodeCanonicalizer_->nfcKey(candidate)',
    'for (const auto& tag : tags)',
    'auto tagKey = unicodeCanonicalizer_->nfcKey(tag)',
    'if (tagKey.value() == candidateKey.value())') `
    'shared fake compares list filters and retained tags by NFC key'
$sharedFakeList = Get-SourceSlice $sharedLegacyFake `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(' `
    '[[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(' `
    'shared fake list operation'
Assert-MarkerOrder $sharedFakeList @(
    'std::size_t selectedCandidates{}',
    'selectedCandidates >= query.value().limit',
    '++selectedCandidates',
    'containsCanonicalTag(') `
    'shared fake preserves bounded pre-filter selection before NFC tag matching'
Assert-NoMatch $sharedLegacyFake '\b(?:normalizeLegacyMemoryTags|normalizeLegacyMemorySetRequest)\b' 'shared fake uses no removed Domain canonicalization helper' -CaseSensitive

$unicodeInfrastructureTests = $textByPath['tests/Infrastructure/WindowsUnicodeCanonicalizerTests.cpp']
$unicodeInfrastructureCaseNames = @([regex]::Matches(
    $unicodeInfrastructureTests,
    'addTest\(\s*tests\s*,\s*"(?<name>foundation[.]unicode_[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Set $unicodeInfrastructureCaseNames @(
    'foundation.unicode_canonical_equivalence_and_typed_ordering',
    'foundation.unicode_nfc_without_compatibility_folding',
    'foundation.unicode_invalid_and_oversized_input_fails_typed') `
    'exact Unicode infrastructure case inventory'
Assert-Match $unicodeInfrastructureTests 'canonicalizer[.]nfcKey\("Caf\\xC3\\xA9"\).*?canonicalizer[.]nfcKey\("Cafe\\xCC\\x81"\).*?composed\s*==\s*decomposed' 'Windows adapter test pins composed/decomposed NFC equivalence' -CaseSensitive
Assert-Match $unicodeInfrastructureTests 'canonicalizer[.]nfcKey\("\\xEE\\x80\\x80"\).*?canonicalizer[.]nfcKey\("\\xF0\\x90\\x80\\x80"\).*?bmp\s*<=>\s*supplementary\)\s*==\s*std::strong_ordering::less' 'Windows adapter test pins BMP before supplementary scalar ordering' -CaseSensitive
Assert-Match $unicodeInfrastructureTests 'MaximumInputBytes\s*\+\s*1U.*?ErrorCodes::PayloadTooLarge' 'Windows adapter test pins the public input allocation bound' -CaseSensitive
$infrastructureRegistrationFiles = @(
    'tests/Infrastructure/FoundationWindowsTests.cpp',
    'tests/Infrastructure/StorageWindowsTests.cpp',
    'tests/Infrastructure/WindowsDiagnosticSinkTests.cpp',
    'tests/Infrastructure/WindowsUnicodeCanonicalizerTests.cpp')
$infrastructureRegistrationText = @($infrastructureRegistrationFiles |
    ForEach-Object { $textByPath[$_] }) -join [Environment]::NewLine
$infrastructureCaseNames = @([regex]::Matches(
    $infrastructureRegistrationText,
    'addTest\(\s*tests\s*,\s*"(?<name>[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Exact $infrastructureCaseNames.Count 54 'infrastructure registry exact total including two command-line child cases'
Assert-Exact @($infrastructureCaseNames | Sort-Object -Unique).Count 54 'infrastructure registry case names are unique'
Assert-Set @($infrastructureCaseNames | Where-Object { $_ -like '*-child' }) @(
    'diagnostics.rotation-crash-child',
    'storage.atomic.crash-recovery-child') `
    'infrastructure command-line-only child case inventory'
Assert-Exact @($infrastructureCaseNames | Where-Object { $_ -notlike '*-child' }).Count 52 'retained Infrastructure.UnitTests default suite exact case count'
$infrastructureTestMain = $textByPath['tests/Infrastructure/InfrastructureTestMain.cpp']
Assert-Sequence @([regex]::Matches(
    $infrastructureTestMain,
    'ForgeConductor::Tests::(?<name>register[A-Za-z]+WindowsTests)\(tests\)') |
    ForEach-Object { $_.Groups['name'].Value }) @(
    'registerFoundationWindowsTests',
    'registerStorageWindowsTests',
    'registerDiagnosticWindowsTests',
    'registerUnicodeCanonicalizerWindowsTests') `
    'Infrastructure.UnitTests exact registration order including Unicode parity'
Assert-Match $infrastructureTestMain 'for\s*\(\s*const\s+auto&\s*\[name,\s*run\]\s*:\s*tests\s*\).*?passed\s*<<\s*''/''\s*<<\s*tests[.]size\(\).*?return\s+0\s*;' 'Infrastructure.UnitTests executes and reports its dynamic 52-case registry' -CaseSensitive

Assert-Match $semanticsAdr 'Delete''s\s+returned\s+`system_key`\s+predicate\s+is\s+exactly\s+the\s+case-sensitive\s+prefixes.*?agent_run/.*?agent_active/.*?continuity/' 'P09 semantics ADR records exact case-sensitive response classification'
Assert-Match $semanticsAdr 'default\s+`LIKE`\s+behavior\s+supplies\s+the\s+same\s+case-insensitive\s+ASCII\s+matching' 'P09 semantics ADR records SQLite ASCII-insensitive visibility'
Assert-Match $semanticsAdr 'Delete\s+derives\s+its\s+boolean\s+result\s+from\s+the\s+same\s+SQL\s+mutation.*?`deleted`\s+and\s+`existed`\s+agree' 'P09 semantics ADR records the delete flag invariant'
$unicodeAdr = $textByPath['.forge-codex/state/decisions/P09-004-unicode-canonical-tag-semantics.md']
Assert-Match $unicodeAdr 'NFC\s+key\s+for\s+equality\s+and\s+ordering\s+while\s+retaining\s+the\s+first\s+trimmed\s+original\s+string' 'P09 Unicode ADR records NFC-key ordering with first-original retention'
Assert-Match $unicodeAdr 'post-SQL-limit\s+tag\s+filter\s+use\s+NFC\s+keys' 'P09 Unicode ADR records post-limit NFC tag matching'
Assert-Match $unicodeAdr 'composed/decomposed\s+collapse.*?ordering\s+across\s+BMP\s+and\s+supplementary\s+scalars' 'P09 Unicode ADR records canonical-equivalence and scalar-order test evidence'

$repositorySource = $textByPath['src/Persistence/Windows/WindowsLegacyMemoryRepository.cpp']
foreach ($include in @('Detail/WindowsDatabaseStore.h','Detail/WinsqliteConnection.h','Detail/WinsqliteStatement.h','Detail/WinsqliteTransaction.h')) {
    Assert-Exact ([regex]::Matches($repositorySource,'^#include\s+"' + [regex]::Escape($include) + '"\r?$',[Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 "repository exact private RAII include $include"
}
Assert-NoMatch $repositorySource '\bsqlite3(?:_[A-Za-z0-9_]+)?\b' 'production repository uses no raw SQLite API'
Assert-True ([regex]::Matches($repositorySource,'[.]prepare\(').Count -ge 10) 'repository has a nontrivial prepared-statement inventory'
Assert-True ([regex]::Matches($repositorySource,'[.]bind(?:Text|Int64)\(').Count -ge 12) 'repository binds all variable SQL values'
Assert-Match $repositorySource 'WinsqliteTransaction::beginImmediate\(' 'mutations use Winsqlite immediate transactions' -CaseSensitive
Assert-Match $repositorySource 'WindowsCentralDatabase::open\(' 'repository opens only through the central database owner' -CaseSensitive
Assert-NoMatch $repositorySource 'WinsqliteConnection::open\(|WindowsDatabaseStore::open\(' 'repository creates no hidden second database connection'
Assert-NoMatch $repositorySource '\bCREATE\s+TABLE\b|\bALTER\s+TABLE\b|\bDROP\s+TABLE\b' 'P09 repository owns no schema migration'
Assert-Match $repositorySource 'struct\s+WindowsLegacyMemoryRepository::Impl\s+final.*?std::shared_ptr<const\s+Contracts::IUnicodeCanonicalizer>\s+unicodeCanonicalizer\s*;' 'repository PImpl owns its injected immutable Unicode canonicalizer'

$repositoryCanonicalWrite = Get-SourceSlice $repositorySource `
    '[[nodiscard]] std::vector<std::string> canonicalizeWriteTags(' `
    '[[nodiscard]] bool containsCanonicalTag(' `
    'repository canonical-write tag guard'
Assert-MarkerOrder $repositoryCanonicalWrite @(
    'Domain::prepareLegacyMemoryTags(tags)',
    'std::map<Contracts::NfcUtf8Key, std::string> unique',
    'canonicalizer.nfcKey(tag)',
    'unique.try_emplace(std::move(key), std::move(tag))',
    'for (auto& [key, original] : unique)',
    'result.push_back(std::move(original))') `
    'repository canonical-write guard uses NFC dedupe/order with first-original retention'

$repositoryCanonicalFilter = Get-SourceSlice $repositorySource `
    '[[nodiscard]] bool containsCanonicalTag(' `
    '[[nodiscard]] Domain::MemoryNote readNote(' `
    'repository canonical tag-filter predicate'
Assert-Match $repositoryCanonicalFilter 'canonicalizer[.]nfcKey\(tag\)\)\s*==\s*candidate' 'repository compares stored tags to the filter NFC key' -CaseSensitive

$repositoryUpsert = Get-SourceSlice $repositorySource `
    'WindowsLegacyMemoryRepository::upsert(' `
    'WindowsLegacyMemoryRepository::get(' `
    'repository legacy-memory upsert'
Assert-MarkerOrder $repositoryUpsert @(
    'const auto canonicalTags = canonicalizeWriteTags(',
    'if (canonicalTags != request.tags)',
    'const auto tagsJson = encodeTags(request.tags)',
    'runOnStore<Domain::LegacyMemorySetOutcome>') `
    'repository rejects noncanonical direct writes before database dispatch'

$repositoryList = Get-SourceSlice $repositorySource `
    'WindowsLegacyMemoryRepository::list(' `
    'WindowsLegacyMemoryRepository::remove(' `
    'repository legacy-memory list'
Assert-MarkerOrder $repositoryList @(
    'tagKey.emplace(take(',
    'sql += " ORDER BY updated_at DESC,key ASC LIMIT ?"',
    'take(statement.bindInt64(',
    'for (;;)',
    'auto projection = readProjection(',
    'if (tagKey && !containsCanonicalTag(') `
    'repository applies the bounded SQL row limit before NFC tag filtering'
Assert-Exact ([regex]::Matches($repositorySource,'VisibleMemoryPredicate\s*=').Count) 1 'single private system-visibility SQL predicate'
foreach ($prefix in @('agent\\_run/%','agent\\_active/%','continuity/%')) {
    Assert-Exact ([regex]::Matches($repositorySource,[regex]::Escape($prefix)).Count) 1 "exact escaped SQL system prefix $prefix"
}
Assert-NoMatch $repositorySource '\bPRAGMA\s+case_sensitive_like\b' 'repository preserves SQLite default ASCII-insensitive LIKE visibility'
Assert-Match $repositorySource 'LegacyMemoryDeleteOutcome\s*\{\s*std::string\s*\{\s*key\s*\}\s*,\s*existed\s*,\s*existed\s*,\s*Domain::isSystemMemoryKey\(key\)\s*\}' 'repository delete emits equal flags and exact case-sensitive response classification' -CaseSensitive
$repositoryListCompatibility = Get-SourceSlice $repositoryTests `
    'void listCompatibilityFilteringAndOrdering()' `
    'void searchCompatibilityAndBodyProjection()' `
    'repository list compatibility test'
Assert-Match $repositoryListCompatibility '!containsKey\(hidden[.]notes,\s*"Agent_run/visible-case"\)' 'repository test pins SQLite ASCII-insensitive hiding of a mixed-case system prefix' -CaseSensitive
Assert-Match $repositoryListCompatibility 'containsKey\(hidden[.]notes,\s*"agent-run/visible-punctuation"\)' 'repository test distinguishes punctuation from an ASCII case variant' -CaseSensitive
Assert-Match $repositoryListCompatibility 'containsKey\(shown[.]notes,\s*"Agent_run/visible-case"\)' 'repository test exposes the mixed-case row only when includeSystem is true' -CaseSensitive
$repositoryPersistedTagsTest = Get-SourceSlice $repositoryTests `
    'void malformedAndOversizedPersistedTags()' `
    'void cancellationDeadlineCloseAndQuickCheck()' `
    'repository persisted-tag compatibility test'
Assert-Match $repositoryPersistedTagsTest 'decomposedEAcute.*?composedEAcute.*?canonical[.]note[.]tags\s*==\s*std::vector<std::string>\s*\(\s*\{\s*decomposedEAcute\s*\}\s*\)' 'repository test retains the original decomposed spelling on a canonical write' -CaseSensitive
Assert-Match $repositoryPersistedTagsTest 'LegacyMemoryListQuery\s*\{\s*std::string\s*\{\s*"tags/canonical-equivalence"\s*\}\s*,\s*composedEAcute.*?crossFormFilter[.]notes[.]size\(\)\s*==\s*1U.*?tags[.]front\(\)\s*==\s*decomposedEAcute' 'repository test pins post-limit cross-form NFC filtering' -CaseSensitive
Assert-Match $repositoryPersistedTagsTest '\{\s*decomposedEAcute\s*,\s*composedEAcute\s*\}.*?REQUIRE\(!canonicalDuplicate\).*?ErrorCodes::InvalidRequest.*?REQUIRE\(!absentDuplicate[.]note[.]has_value\(\)\)' 'repository test rejects canonical duplicate direct writes before persistence' -CaseSensitive
Assert-MarkerOrder $repositoryPersistedTagsTest @(
    'Application::LegacyMemoryService service{',
    '*fixture.repository, fixture.unicodeCanonicalizer',
    'const auto stored = take(service.set(',
    '"tags/service-repository-integration"',
    '"p09-service-repository-canonical-write"',
    'REQUIRE(stored.note.tags ==',
    'const auto matched = take(service.list(',
    'Domain::LegacyMemoryListRequest{',
    '"p09-service-repository-canonical-filter"',
    'REQUIRE(matched.notes.size() == 1U)') `
    'repository test crosses the real Application service and Windows repository with canonical write and filter forms'
Assert-Match $repositoryPersistedTagsTest 'service[.]set\(.*?Domain::LegacyMemorySetRequest\s*\{\s*"tags/service-repository-integration"\s*,\s*std::string\s*\{\s*"body"\s*\}\s*,\s*\{\s*decomposedEAcute\s*,\s*composedEAcute\s*\}' 'real service-to-repository integration writes canonically equivalent tag forms through Application' -CaseSensitive
Assert-Match $repositoryPersistedTagsTest 'stored[.]note[.]tags\s*==\s*std::vector<std::string>\s*\(\s*\{\s*decomposedEAcute\s*\}\s*\)' 'real service-to-repository integration collapses canonical duplicates before persistence' -CaseSensitive
Assert-Match $repositoryPersistedTagsTest 'service[.]list\(.*?LegacyMemoryListRequest\s*\{\s*std::string\s*\{\s*"tags/service-repository-integration"\s*\}\s*,\s*composedEAcute' 'real service-to-repository integration filters by the opposite canonical form' -CaseSensitive
Assert-Match $repositoryPersistedTagsTest 'matched[.]notes[.]front\(\)[.]tags\s*==\s*std::vector<std::string>\s*\(\s*\{\s*decomposedEAcute\s*\}\s*\)' 'real service-to-repository integration retains the first original spelling' -CaseSensitive
$escapeLikeBlock = [regex]::Match($repositorySource,'std::string\s+escapeLikeLiteral\(.*?\)\s*\{(?<body>.*?)\r?\n\}',[Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $escapeLikeBlock.Success 'private LIKE-literal escape function block'
foreach ($literal in @("character == '\\'","character == '%'","character == '_'")) {
    Assert-True ($escapeLikeBlock.Groups['body'].Value.IndexOf($literal,[StringComparison]::Ordinal) -ge 0) "LIKE escape branch includes $literal"
}
Assert-True ($escapeLikeBlock.Groups['body'].Value.IndexOf("escaped.push_back('\\')",[StringComparison]::Ordinal) -ge 0) 'LIKE escape branch prefixes metacharacters with backslash'
Assert-Match $repositorySource 'ORDER BY updated_at DESC,key ASC LIMIT \?' 'list/search use deterministic newest-first ordering' -CaseSensitive
Assert-Match $repositoryList 'containsCanonicalTag\(\s*projection[.]tags\s*,\s*\*tagKey\s*,\s*\*implementation_->unicodeCanonicalizer\)' 'post-limit tag filter is NFC-aware' -CaseSensitive
Assert-Match $repositorySource 'SELECT COUNT[(][*][)] FROM memory_notes WHERE 1=1.*?if\s*\(\s*!request[.]includeSystem\s*\).*?countSql\s*\+=\s*VisibleMemoryPredicate' 'list total counts only by system visibility'
Assert-Match $repositorySource 'SummaryProjectionColumns\s*=\s*"key,NULL,length\(CAST\(body AS BLOB\)\),tags_json,created_at,updated_at"' 'summary projection avoids body materialization' -CaseSensitive
Assert-Match $repositorySource 'DELETE FROM memory_notes' 'confirmed management purge deletes the complete legacy table' -CaseSensitive
Assert-Match $repositorySource 'WinsqliteTransaction::beginImmediate\(connection,\s*context\).*?DELETE FROM memory_notes.*?SELECT COUNT[(][*][)] FROM memory_notes.*?INSERT INTO audit_events.*?[.]commit\(' 'purge delete, verification, sanitized audit, and commit share one immediate transaction'
$auditArguments = [regex]::Match($repositorySource,'constexpr\s+std::string_view\s+Arguments\s*=\s*"(?<value>[^"\r\n]*(?:\\"[^"\r\n]*)*)"\s*;')
Assert-True $auditArguments.Success 'purge has one private sanitized audit-arguments constant'
Assert-Exact $auditArguments.Groups['value'].Value '{\"scope\":\"legacy-global-memory\"}' 'purge audit arguments contain only the non-secret scope'
Assert-NoMatch $auditArguments.Groups['value'].Value 'PURGE LEGACY GLOBAL MEMORY|token|key|body|tags' 'purge audit arguments omit confirmation and deleted note content'
Assert-NoMatch $repositorySource '\b(?:OutputDebugString[A-W]?|TraceLoggingWrite|EventWrite|std::clog|std::cerr)\b' 'repository performs no direct logging of memory content'

$centralHeader = $textByPath['include/ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h']
$centralSource = $textByPath['src/Persistence/Windows/WindowsCentralDatabase.cpp']
$centralClass = [regex]::Match($centralHeader,'class\s+WindowsCentralDatabase\s+final\s*\{(?<body>.*?)\r?\n\};',[Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $centralClass.Success 'WindowsCentralDatabase class block'
$centralPrivateIndex = $centralClass.Groups['body'].Value.IndexOf('private:',[StringComparison]::Ordinal)
Assert-True ($centralPrivateIndex -ge 0) 'WindowsCentralDatabase explicit private section'
$centralPublicBody = $centralClass.Groups['body'].Value.Substring(0,$centralPrivateIndex)
$centralPrivateBody = $centralClass.Groups['body'].Value.Substring($centralPrivateIndex)
Assert-NoMatch $centralPublicBody '\brepositoryStore\s*\(' 'central repository seam is not public'
Assert-Exact ([regex]::Matches($centralPrivateBody,'friend\s+class\s+WindowsLegacyMemoryRepository\s*;').Count) 1 'central seam exact friend declaration'
Assert-Exact ([regex]::Matches($centralPrivateBody,'Detail::WindowsDatabaseStore[*]\s+repositoryStore\(\)\s+noexcept\s*;').Count) 1 'central seam exact private store accessor'
Assert-Exact ([regex]::Matches($centralSource,'WindowsCentralDatabase::repositoryStore\(\)\s+noexcept').Count) 1 'central seam exact implementation count'
Assert-True ([regex]::Matches($repositorySource,'repositoryStore\(\)').Count -ge 5) 'repository consistently uses the private central seam'

$centralMigrationsPath = Join-Path $WorkspaceRoot 'src\Persistence\Windows\Migrations\CentralMigrations.cpp'
$centralMigrations = Get-Content -Raw -LiteralPath $centralMigrationsPath
Assert-Exact ([regex]::Matches($centralMigrations,'\{2,\s*"C002",\s*C002Sql,\s*"3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315"\}').Count) 1 'unchanged C002 content hash'
$expectedMemorySchema = 'CREATE TABLE memory_notes \(\s*key TEXT PRIMARY KEY,\s*body TEXT NOT NULL,\s*tags_json TEXT NOT NULL DEFAULT ''\[\]'',\s*created_at TEXT NOT NULL,\s*updated_at TEXT NOT NULL\s*\);'
Assert-Exact ([regex]::Matches($centralMigrations,$expectedMemorySchema,[Text.RegularExpressions.RegexOptions]::Singleline).Count) 1 'C002 exact unchanged memory_notes schema'
Assert-NoMatch $centralMigrations '(?i)ALTER\s+TABLE\s+memory_notes|DROP\s+TABLE\s+memory_notes' 'central migrations never reshape or drop memory_notes'
$schemaMigrator = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'src\Persistence\Windows\Migrations\SchemaMigrator.cpp')
Assert-Match $schemaMigrator 'constexpr\s+std::array\s+MemoryNoteColumns\s*\{\s*column\("key",\s*"TEXT",\s*false,\s*1\),\s*column\("body",\s*"TEXT",\s*true\),\s*columnWithDefault\("tags_json",\s*"TEXT",\s*true,\s*"''\[\]''"\),\s*column\("created_at",\s*"TEXT",\s*true\),\s*column\("updated_at",\s*"TEXT",\s*true\),\s*\};' 'schema admission retains exact five memory_notes columns'
Assert-Exact ([regex]::Matches($schemaMigrator,'TableSpec\{"memory_notes",\s*MemoryNoteColumns,\s*\{\}\}').Count) 3 'central v3, v5, and v6 share the unchanged memory_notes table spec'

$fixturePath = Join-Path $WorkspaceRoot 'tests\Persistence\Fixtures\central-v5.sql'
$fixtureText = Get-Content -Raw -LiteralPath $fixturePath
Assert-Exact ([long](Get-Item -LiteralPath $fixturePath).Length) 3913L 'immutable central-v5 fixture byte count'
Assert-Exact (Get-FileSha256 $fixturePath) 'eaa76a623a626cd0d6f8022351e1e94ce64c23e5dbdd84785df8f3a1c1acfb0f' 'immutable central-v5 fixture SHA-256'
foreach ($sentinel in @("'legacy/note'","'central-v5-preservation-sentinel'",('''["legacy","migration"]'''),"'2025-01-02T03:04:05Z'","'2025-01-02T03:04:06Z'")) {
    Assert-Exact ([regex]::Matches($fixtureText,[regex]::Escape($sentinel)).Count) 1 "central-v5 legacy-memory migration sentinel $sentinel"
}
Assert-Exact ([regex]::Matches($fixtureText,'INSERT\s+INTO\s+schema_version\(version\)\s+VALUES\s*\(5\)').Count) 1 'central-v5 fixture exact source schema sentinel'

foreach ($relativePath in $adrFiles) {
    $adrText = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))
    Assert-Match $adrText '^#\s+P09-00[1-4]:' "$relativePath ADR identifier"
    Assert-Exact ([regex]::Matches($adrText,'^Status:\s+Accepted\r?$',[Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 "$relativePath accepted status"
    Assert-Match $adrText '^##\s+Decision\r?$' "$relativePath decision section"
    Assert-Match $adrText '^##\s+Consequences\r?$' "$relativePath consequences section"
}

function Assert-RegistryTestCaseInventory {
    param([string]$Path,[string[]]$ExpectedNames,[int]$ExpectedPassed,[string]$Message)
    $text = Get-Content -Raw -LiteralPath $Path
    $actualNames = @([regex]::Matches($text,'\{\s*"(?<name>[a-z0-9_]+)"\s*,\s*[A-Za-z][A-Za-z0-9_]*\s*\}') | ForEach-Object { $_.Groups['name'].Value })
    Assert-Set $actualNames $ExpectedNames "$Message exact registry-case inventory"
    Assert-Exact $actualNames.Count $ExpectedPassed "$Message registry count"
    Assert-Match $text 'std::cout\s*<<\s*"SUMMARY passed="\s*<<\s*\(tests[.]size\(\)\s*-\s*failures\).*?return\s+failures\s*==\s*0U\s*\?\s*EXIT_SUCCESS\s*:\s*EXIT_FAILURE\s*;' "$Message fail-closed dynamic summary"
}
Assert-RegistryTestCaseInventory (Join-Path $WorkspaceRoot 'tests\LegacyMemory\LegacyMemoryApplicationTests.cpp') @(
    'five_operations_normalize_and_project',
    'validation_and_limit_errors',
    'dependency_failures_and_postconditions',
    'cancellation_deadline_and_shutdown',
    'owner_purge_confirmation_forwarding') 5 'legacy-memory application tests'
Assert-TestCaseInventory (Join-Path $WorkspaceRoot 'tests\LegacyMemory\LegacyMemoryRepositoryWindowsTests.cpp') @(
    'legacy_memory_repository.roundtrip_restart',
    'legacy_memory_repository.upsert_timestamps',
    'legacy_memory_repository.list_compatibility',
    'legacy_memory_repository.search_compatibility',
    'legacy_memory_repository.central_v5_migration',
    'legacy_memory_repository.persisted_tags',
    'legacy_memory_repository.context_close_quick_check',
    'legacy_memory_repository.purge_audit_isolation',
    'legacy_memory_repository.concurrent_serialization') 9 'legacy-memory repository Windows tests'

$cmakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$cmake = Get-Content -Raw -LiteralPath $cmakePath
$domainBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_DOMAIN_SOURCES' 'FORGE_DOMAIN_SOURCES declaration'
Assert-Exact @(Get-CMakeTokens $domainBody | Where-Object { $_ -ceq 'src/Domain/LegacyMemoryModels.cpp' }).Count 1 'CMake domain legacy-memory source count'
$applicationBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_APPLICATION_SOURCES' 'FORGE_APPLICATION_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $applicationBody | Where-Object { $_ -match '^src/Application/.*LegacyMemory.*[.]cpp$' }) @('src/Application/LegacyMemoryService.cpp') 'CMake exact P09 application source placement'
$infrastructureBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_INFRASTRUCTURE_WINDOWS_SOURCES' 'FORGE_INFRASTRUCTURE_WINDOWS_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $infrastructureBody | Where-Object { $_ -match '^src/Infrastructure/Windows/.*UnicodeCanonicalizer.*[.]cpp$' }) @('src/Infrastructure/Windows/WindowsUnicodeCanonicalizer.cpp') 'CMake exact P09 Unicode infrastructure source placement'
$persistenceBody = Get-CMakeInvocationBody $cmake 'set' 'FORGE_PERSISTENCE_WINDOWS_SOURCES' 'FORGE_PERSISTENCE_WINDOWS_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $persistenceBody | Where-Object { $_ -match '^src/Persistence/Windows/.*LegacyMemory.*[.]cpp$' }) @('src/Persistence/Windows/WindowsLegacyMemoryRepository.cpp') 'CMake exact P09 persistence source placement'
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor[.]Application\s+ForgeConductor::Application\s+ForgeConductor::Contracts\s*\)' 'application layer depends only on contracts' -CaseSensitive
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor[.]Infrastructure[.]Windows\s+ForgeConductor::Infrastructure[.]Windows\s+ForgeConductor::Contracts\s*\)' 'Windows Unicode infrastructure remains below the contracts layer' -CaseSensitive
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor[.]Persistence[.]Windows\s+ForgeConductor::Persistence[.]Windows\s+ForgeConductor::Contracts\s+ForgeConductor::Infrastructure[.]Windows\s*\)' 'persistence layer depends only on contracts and Windows infrastructure' -CaseSensitive
$infrastructureLinks = Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Infrastructure.Windows' `
    'Windows infrastructure link inventory')
Assert-Exact @($infrastructureLinks | Where-Object { $_ -ceq 'normaliz' }).Count 1 'Windows infrastructure links the exact Windows NFC import library once'
Assert-Exact ([regex]::Matches($cmake,'\bnormaliz\b').Count) 1 'normaliz appears only at the approved Windows infrastructure boundary'
Assert-Set (Get-CMakeExecutableSources $cmake 'ForgeConductor.Infrastructure.UnitTests') @(
    'tests/Infrastructure/FoundationWindowsTests.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/StorageWindowsTests.cpp',
    'tests/Infrastructure/WindowsDiagnosticSinkTests.cpp',
    'tests/Infrastructure/WindowsUnicodeCanonicalizerTests.cpp') `
    'CMake exact retained 52-case infrastructure unit-test source inventory'

$expectedTargets = [ordered]@{
    'ForgeConductor.LegacyMemory.ApplicationTests' = @('tests/LegacyMemory/LegacyMemoryApplicationTests.cpp')
    'ForgeConductor.LegacyMemory.RepositoryWindowsTests' = @('tests/LegacyMemory/LegacyMemoryRepositoryWindowsTests.cpp')
}
$expectedLabels = [ordered]@{
    'ForgeConductor.LegacyMemory.ApplicationTests' = @('T-UNIT','T-MEM','T-SEC','G09')
    'ForgeConductor.LegacyMemory.RepositoryWindowsTests' = @('T-UNIT','T-DB','T-MEM','T-SEC','G09')
}
$expectedTimeouts = [ordered]@{
    'ForgeConductor.LegacyMemory.ApplicationTests' = 60
    'ForgeConductor.LegacyMemory.RepositoryWindowsTests' = 180
}
foreach ($target in $expectedTargets.Keys) {
    Assert-Set (Get-CMakeExecutableSources $cmake $target) $expectedTargets[$target] "$target exact CMake source inventory"
    $fixtureArgument = if ($target -ceq 'ForgeConductor.LegacyMemory.RepositoryWindowsTests') { '\s+"[$][{]PROJECT_SOURCE_DIR[}]/tests/Persistence/Fixtures"' } else { '' }
    Assert-Match $cmake ('add_test\s*\(\s*NAME\s+' + [regex]::Escape($target) + '\s+COMMAND\s+[$]<TARGET_FILE:' + [regex]::Escape($target) + '>' + $fixtureArgument + '\s*\)') "$target exact CMake test command" -CaseSensitive
    $propertyBody = Get-CMakeInvocationBody $cmake 'set_tests_properties' $target "$target CMake test properties"
    Assert-Match $propertyBody ('LABELS\s+"' + [regex]::Escape(($expectedLabels[$target] -join ';')) + '"') "$target exact CMake labels" -CaseSensitive
    Assert-Match $propertyBody ('TIMEOUT\s+' + $expectedTimeouts[$target] + '\b') "$target exact CMake timeout" -CaseSensitive
}
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'target_include_directories' 'ForgeConductor.LegacyMemory.ApplicationTests' 'application test include inventory')) @('PRIVATE','${PROJECT_SOURCE_DIR}/tests') 'application test exact includes'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'target_link_libraries' 'ForgeConductor.LegacyMemory.ApplicationTests' 'application test link inventory')) @('PRIVATE','ForgeConductor::Application') 'application test exact link layer'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'target_include_directories' 'ForgeConductor.LegacyMemory.RepositoryWindowsTests' 'repository test include inventory')) @('PRIVATE','${PROJECT_SOURCE_DIR}/src','${PROJECT_SOURCE_DIR}/tests') 'repository test exact includes'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'target_link_libraries' 'ForgeConductor.LegacyMemory.RepositoryWindowsTests' 'repository test link inventory')) @('PRIVATE','ForgeConductor::Application','ForgeConductor::Persistence.Windows','nlohmann_json::nlohmann_json') 'repository test exact real Application-to-Windows-repository integration link layers'
Assert-Match $cmake 'forge_configure_standard_target\s*\(\s*ForgeConductor[.]LegacyMemory[.]ApplicationTests\s*\)' 'application test uses standard native C++ target policy' -CaseSensitive
Assert-Match $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor[.]LegacyMemory[.]RepositoryWindowsTests\s*\)' 'repository test uses Windows native target policy' -CaseSensitive

$domainUmbrella = $textByPath['include/ForgeConductor/Domain/Domain.h']
$contractsUmbrella = $textByPath['include/ForgeConductor/Contracts/Contracts.h']
$persistenceUmbrella = $textByPath['include/ForgeConductor/Persistence/Windows/PersistenceWindows.h']
foreach ($pair in @(
    @($domainUmbrella,'ForgeConductor/Domain/LegacyMemoryModels.h'),
    @($contractsUmbrella,'ForgeConductor/Contracts/ILegacyMemoryRepository.h'),
    @($contractsUmbrella,'ForgeConductor/Contracts/ILegacyMemoryService.h'),
    @($contractsUmbrella,'ForgeConductor/Contracts/IUnicodeCanonicalizer.h'),
    @($textByPath['include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h'],'ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h'),
    @($persistenceUmbrella,'ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h'))) {
    Assert-Exact ([regex]::Matches($pair[0],'^#include\s+"' + [regex]::Escape($pair[1]) + '"\r?$',[Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 "umbrella exact include $($pair[1])"
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count before P09 builds'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L `
    'sealed Forsetti byte count before P09 builds'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti hash before P09 builds'

if ($StaticOnly) {
    Write-Host "G09 static validation passed: $script:AssertionCount fail-closed assertions."
    return
}

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G09: building complete x64 Debug tree from a fresh build directory.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Assert-True $? 'x64 Debug full fresh build'
foreach ($label in @('G09','G08','G07','G06','G05','G04')) {
    Write-Host "G09: testing x64 Debug retained $label inventory."
    & $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Debug $label tests"
}
Write-Host 'G09: building complete x64 Release tree.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Assert-True $? 'x64 Release full build'
foreach ($label in @('G09','G08','G07','G06','G05','G04')) {
    Write-Host "G09: testing x64 Release retained $label inventory."
    & $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Release $label tests"
}

$toolchainStatePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
Assert-True (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) `
    'recorded toolchain state exists'
try {
    $toolchainState = Get-Content -Raw -LiteralPath $toolchainStatePath | ConvertFrom-Json
} catch {
    throw "G09 assertion failed: invalid toolchain state - $($_.Exception.Message)"
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
    'lib/{0}/ForgeConductor.Infrastructure.Windows.lib',
    'lib/{0}/ForgeConductor.Persistence.Windows.lib',
    'bin/{0}/ForgeConductor.Infrastructure.UnitTests.exe',
    'bin/{0}/ForgeConductor.LegacyMemory.ApplicationTests.exe',
    'bin/{0}/ForgeConductor.LegacyMemory.RepositoryWindowsTests.exe')
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
        -L G09 --show-only=json-v1) -join [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 "$configuration G09 CTest JSON inventory command"
    try {
        $ctestInventory = $ctestJsonText | ConvertFrom-Json
    } catch {
        throw "G09 assertion failed: invalid $configuration CTest JSON inventory - $($_.Exception.Message)"
    }
    Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) `
        @($expectedTargets.Keys) "$configuration exact G09 CTest inventory"

    $buildRootForward = $buildRoot.Replace('\', '/')
    foreach ($test in @($ctestInventory.tests)) {
        $testName = [string]$test.name
        Assert-Exact ([string]$test.config) $configuration `
            "$configuration CTest configuration for $testName"
        $expectedCommand = @(
            "$buildRootForward/bin/$configuration/$testName.exe")
        if ($testName -ceq 'ForgeConductor.LegacyMemory.RepositoryWindowsTests') {
            $expectedCommand +=
                "$($WorkspaceRoot.Replace('\', '/'))/tests/Persistence/Fixtures"
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
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.LegacyMemory.ApplicationTests',
    'ForgeConductor.LegacyMemory.RepositoryWindowsTests')
$inspectionTargetsPath = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    'ForgeConductor-G09-' + [guid]::NewGuid().ToString('N') + '.targets')
$inspectionTargets = @(
    '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
    '  <ItemGroup>',
    '    <Link Include="__forge_g09_link_probe__" />',
    '    <Lib Include="__forge_g09_lib_probe__" />',
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
            "generated P09 project $projectName"
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
                $probeIdentity = "__forge_g09_$($binaryItemType.ToLowerInvariant())_probe__"
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
Assert-Exact $evaluatedProjectConfigurationCount 14 `
    'exact Debug/Release evaluated MSBuild inspection count for seven P09 projects'
Assert-True (-not (Test-Path -LiteralPath $inspectionTargetsPath)) `
    'evaluated MSBuild inspection targets cleanup'

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after P09 builds'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after P09 builds'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after P09 builds'

$gitOutput = & git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1
Assert-Exact $LASTEXITCODE 0 `
    ('git diff --check failed: ' + ($gitOutput -join [Environment]::NewLine))
& (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'governance ledger verification'

Write-Host "G09 legacy-memory validation passed: $script:AssertionCount fail-closed assertions; exact five typed operations plus three management methods, two native G09 targets (5 application and 9 repository cases), retained 52-case infrastructure suite, $($artifactHashes.Count) Debug/Release output hashes, and x64 Debug/Release G04+G05+G06+G07+G08+G09 tests passed."
