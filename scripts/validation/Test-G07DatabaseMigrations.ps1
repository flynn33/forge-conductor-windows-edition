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
    if (-not $Condition) { throw "G07 assertion failed: $Message" }
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

function Read-Json {
    param([string]$Path)
    try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
    catch { throw "G07 assertion failed: invalid JSON at $Path - $($_.Exception.Message)" }
}

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

function Assert-FileInventory {
    param([string]$Directory, [string[]]$Expected, [string]$Message)
    Assert-True (Test-Path -LiteralPath $Directory -PathType Container) "$Message directory"
    $actual = @(Get-ChildItem -LiteralPath $Directory -Force -File | ForEach-Object { $_.Name })
    Assert-Set $actual $Expected $Message
}

function Assert-DirectoryInventory {
    param([string]$Directory, [string[]]$Expected, [string]$Message)
    Assert-True (Test-Path -LiteralPath $Directory -PathType Container) "$Message parent directory"
    $actual = @(Get-ChildItem -LiteralPath $Directory -Force -Directory |
        ForEach-Object { $_.Name })
    Assert-Set $actual $Expected $Message
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

function Get-MatchingRelativeFiles {
    param([IO.FileInfo[]]$Files, [string]$Pattern)
    $matches = [Collections.Generic.List[string]]::new()
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    foreach ($file in $Files) {
        $text = Get-Content -Raw -LiteralPath $file.FullName
        if ([regex]::IsMatch($text, $Pattern, $options)) {
            $matches.Add((Get-RelativePath $file.FullName))
        }
    }
    return @($matches | ForEach-Object { [string]$_ })
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
    return @(Get-CMakeTokens $body | Where-Object { $_ -match '^(?:tests|src)/.+' })
}

function Assert-MigrationManifest {
    param(
        [string]$Path,
        [string]$Prefix,
        [string[]]$ExpectedRows,
        [string]$Message
    )
    $text = Get-Content -Raw -LiteralPath $Path
    $sqlPattern = 'constexpr\s+std::string_view\s+(?<symbol>' +
        [regex]::Escape($Prefix) + '\d{3})Sql\s*=\s*R"sql\((?<sql>.*?)\)sql";'
    $sqlMatches = @([regex]::Matches(
        $text,
        $sqlPattern,
        [Text.RegularExpressions.RegexOptions]::Singleline))
    Assert-Exact $sqlMatches.Count $ExpectedRows.Count "$Message SQL literal count"
    $sqlHashes = @{}
    foreach ($sqlMatch in $sqlMatches) {
        $symbol = $sqlMatch.Groups['symbol'].Value
        $normalizedSql = $sqlMatch.Groups['sql'].Value.Replace("`r`n", "`n")
        Assert-True (-not $sqlHashes.ContainsKey($symbol)) "$Message duplicate SQL symbol $symbol"
        $sqlHashes[$symbol] = Get-StringSha256 $normalizedSql
    }

    $rowPattern = '\{\s*(?<version>\d+)\s*,\s*"(?<identifier>' +
        [regex]::Escape($Prefix) + '\d{3})"\s*,\s*(?<symbol>' +
        [regex]::Escape($Prefix) + '\d{3})Sql\s*,\s*"(?<checksum>[0-9a-f]{64})"\s*\}'
    $rows = @([regex]::Matches($text, $rowPattern) | ForEach-Object {
        $_.Groups['version'].Value + '|' +
            $_.Groups['identifier'].Value + '|' +
            $_.Groups['symbol'].Value + '|' +
            $_.Groups['checksum'].Value
    })
    Assert-Sequence $rows $ExpectedRows "$Message ordered migration rows"
    foreach ($row in $ExpectedRows) {
        $parts = $row.Split('|')
        Assert-Exact $parts[1] $parts[2] "$Message identifier/SQL symbol $($parts[1])"
        Assert-Exact ([string]$sqlHashes[$parts[2]]) $parts[3] `
            "$Message normalized SQL checksum $($parts[1])"
    }
}

$publicHeaderNames = @(
    'DatabaseModels.h',
    'PersistenceWindows.h',
    'WindowsCentralDatabase.h',
    'WindowsProjectDatabase.h')
$topLevelSourceNames = @(
    'DatabaseBackupCoordinator.cpp',
    'DatabaseBackupCoordinator.h',
    'DatabaseQuarantine.cpp',
    'DatabaseQuarantine.h',
    'WindowsCentralDatabase.cpp',
    'WindowsProjectDatabase.cpp')
$detailHeaderNames = @(
    'AnchoredSqliteVfs.h',
    'DatabaseNamespaceLease.h',
    'WindowsDatabaseStore.h',
    'WinsqliteBackup.h',
    'WinsqliteConnection.h',
    'WinsqliteError.h',
    'WinsqliteOperationGuard.h',
    'WinsqliteStatement.h',
    'WinsqliteTransaction.h')
$detailSourceNames = @(
    'AnchoredSqliteVfs.cpp',
    'DatabaseNamespaceLease.cpp',
    'WindowsDatabaseStore.cpp',
    'WinsqliteBackup.cpp',
    'WinsqliteConnection.cpp',
    'WinsqliteError.cpp',
    'WinsqliteOperationGuard.cpp',
    'WinsqliteStatement.cpp',
    'WinsqliteTransaction.cpp')
$migrationHeaderNames = @(
    'CentralMigrations.h',
    'MigrationManifest.h',
    'ProjectMigrations.h',
    'SchemaMigrator.h')
$migrationSourceNames = @(
    'CentralMigrations.cpp',
    'ProjectMigrations.cpp',
    'SchemaMigrator.cpp')
$testFileNames = @(
    'BackupIntegrityTests.cpp',
    'CentralMigrationTests.cpp',
    'DatabaseAuthorityTests.cpp',
    'DatabaseConcurrencyTests.cpp',
    'DatabaseStoreSerializationTests.cpp',
    'IntegrityRecoveryTests.cpp',
    'PersistenceProcessFixture.cpp',
    'PersistenceTestMain.cpp',
    'PersistenceTestSupport.h',
    'ProjectMigrationTests.cpp',
    'WinsqliteKernelTests.cpp')
$fixtureNames = @(
    'central-v3-minimal.sql',
    'central-v3.sql',
    'central-v5.sql',
    'project-v1.sql',
    'README.md')
$adrNames = @(
    'P07-001-winsqlite-persistence-boundary-and-ownership.md',
    'P07-002-source-compatible-schema-lineage.md',
    'P07-003-anchored-database-namespace-and-online-backups.md')

$publicRoot = Join-Path $WorkspaceRoot 'include\ForgeConductor\Persistence\Windows'
$sourceRoot = Join-Path $WorkspaceRoot 'src\Persistence\Windows'
$detailRoot = Join-Path $sourceRoot 'Detail'
$migrationRoot = Join-Path $sourceRoot 'Migrations'
$testRoot = Join-Path $WorkspaceRoot 'tests\Persistence'
$fixtureRoot = Join-Path $testRoot 'Fixtures'
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
$architectureRoot = Join-Path $WorkspaceRoot 'tests\Architecture'

Assert-FileInventory $publicRoot $publicHeaderNames 'exact P07 public-header inventory'
Assert-DirectoryInventory $publicRoot @() 'P07 public-header subdirectory inventory'
Assert-FileInventory $sourceRoot $topLevelSourceNames 'exact P07 top-level source inventory'
Assert-DirectoryInventory $sourceRoot @('Detail','Migrations') 'P07 source subdirectory inventory'
Assert-FileInventory $detailRoot ($detailHeaderNames + $detailSourceNames) `
    'exact P07 private detail inventory'
Assert-DirectoryInventory $detailRoot @() 'P07 detail subdirectory inventory'
Assert-FileInventory $migrationRoot ($migrationHeaderNames + $migrationSourceNames) `
    'exact P07 migration inventory'
Assert-DirectoryInventory $migrationRoot @() 'P07 migration subdirectory inventory'
Assert-FileInventory $testRoot $testFileNames 'exact P07 persistence-test inventory'
Assert-DirectoryInventory $testRoot @('Fixtures') 'P07 persistence-test subdirectory inventory'
Assert-FileInventory $fixtureRoot $fixtureNames 'exact P07 fixture inventory'
Assert-DirectoryInventory $fixtureRoot @() 'P07 fixture subdirectory inventory'
Assert-Set @(Get-ChildItem -LiteralPath $architectureRoot -File -Filter 'P07*' |
    ForEach-Object { $_.Name }) @('P07HeaderSelfContainmentMain.cpp') `
    'exact P07 architecture-test inventory'
Assert-Set @(Get-ChildItem -LiteralPath $decisionRoot -File -Filter 'P07-*.md' |
    ForEach-Object { $_.Name }) $adrNames 'exact P07 ADR inventory'

$requiredRelativeFiles = @(
    'CMakeLists.txt',
    'Directory.Build.props',
    'scripts/build.ps1',
    'scripts/test.ps1',
    'scripts/validation/Test-G04BuildScaffold.ps1',
    'scripts/validation/Test-G05DomainContracts.ps1',
    'scripts/validation/Test-G06WindowsInfrastructure.ps1',
    'scripts/validation/Test-G07DatabaseMigrations.ps1',
    'tests/Architecture/P07HeaderSelfContainmentMain.cpp')
foreach ($name in $publicHeaderNames) {
    $requiredRelativeFiles += "include/ForgeConductor/Persistence/Windows/$name"
}
foreach ($name in $topLevelSourceNames) {
    $requiredRelativeFiles += "src/Persistence/Windows/$name"
}
foreach ($name in $detailHeaderNames + $detailSourceNames) {
    $requiredRelativeFiles += "src/Persistence/Windows/Detail/$name"
}
foreach ($name in $migrationHeaderNames + $migrationSourceNames) {
    $requiredRelativeFiles += "src/Persistence/Windows/Migrations/$name"
}
foreach ($name in $testFileNames) {
    $requiredRelativeFiles += "tests/Persistence/$name"
}
foreach ($name in $fixtureNames) {
    $requiredRelativeFiles += "tests/Persistence/Fixtures/$name"
}
foreach ($name in $adrNames) {
    $requiredRelativeFiles += ".forge-codex/state/decisions/$name"
}
foreach ($relativePath in $requiredRelativeFiles) {
    Assert-True (Test-Path -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) `
        -PathType Leaf) "required P07 file $relativePath"
}

$tokens = $null
$parseErrors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $PSCommandPath,
    [ref]$tokens,
    [ref]$parseErrors)
Assert-Exact @($parseErrors).Count 0 'G07 validator PowerShell parser-error count'

$crlfFiles = @(
    Get-Item -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt'),
        (Join-Path $WorkspaceRoot 'tests\Architecture\P07HeaderSelfContainmentMain.cpp'),
        $PSCommandPath
) + @(Get-ChildItem -LiteralPath $publicRoot -File) +
    @(Get-ChildItem -LiteralPath $sourceRoot -File) +
    @(Get-ChildItem -LiteralPath $detailRoot -File) +
    @(Get-ChildItem -LiteralPath $migrationRoot -File) +
    @(Get-ChildItem -LiteralPath $testRoot -File) +
    @(Get-ChildItem -LiteralPath $fixtureRoot -File) +
    @(Get-ChildItem -LiteralPath $decisionRoot -File -Filter 'P07-*.md')
foreach ($file in @($crlfFiles | Sort-Object FullName -Unique)) {
    Assert-CrlfTextFile $file.FullName (Get-RelativePath $file.FullName)
}

$publicHeaders = @(Get-ChildItem -LiteralPath $publicRoot -File -Filter '*.h')
$publicText = @($publicHeaders | ForEach-Object {
    "// $($_.Name)" + [Environment]::NewLine + (Get-Content -Raw -LiteralPath $_.FullName)
}) -join [Environment]::NewLine
foreach ($header in $publicHeaders) {
    $headerText = Get-Content -Raw -LiteralPath $header.FullName
    Assert-Match $headerText '^#pragma once\r?$' "$($header.Name) uses pragma-once isolation" -CaseSensitive
    Assert-NoMatch $headerText '#\s*include\s*[<"](?:Windows\.h|windows\.h|winrt/|wil/|winsqlite/|sqlite3\.h|nlohmann/)' `
        "$($header.Name) has no Windows, SQLite, WIL, WinRT, or nlohmann include"
    Assert-NoMatch $headerText '#\s*include\s*[<"](?:\.\./|Detail/|Migrations/)' `
        "$($header.Name) does not include private persistence implementation"
}
Assert-NoMatch $publicText '\b(?:sqlite3|sqlite3_stmt|sqlite3_backup|sqlite3_vfs|sqlite3_file)\b' `
    'public persistence headers leak no SQLite types' -CaseSensitive
Assert-NoMatch $publicText '\b(?:HANDLE|HKEY|HRESULT|DWORD|LPWSTR|LPCWSTR|PCWSTR|OVERLAPPED|SECURITY_ATTRIBUTES|GUID)\b' `
    'public persistence headers leak no Windows ownership or ABI types' -CaseSensitive
Assert-NoMatch $publicText '\bnlohmann\b|\bjson\b' `
    'public persistence headers leak no nlohmann or JSON implementation types'

$umbrellaText = Get-Content -Raw -LiteralPath (Join-Path $publicRoot 'PersistenceWindows.h')
$umbrellaIncludes = @([regex]::Matches(
    $umbrellaText,
    '^#include\s+"(?<path>ForgeConductor/Persistence/Windows/[^"]+)"\r?$',
    [Text.RegularExpressions.RegexOptions]::Multiline) |
        ForEach-Object { $_.Groups['path'].Value })
Assert-Set $umbrellaIncludes @(
    'ForgeConductor/Persistence/Windows/DatabaseModels.h',
    'ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h',
    'ForgeConductor/Persistence/Windows/WindowsProjectDatabase.h') `
    'public persistence umbrella include inventory'
Assert-Match $publicText 'class\s+WindowsCentralDatabase\s+final' `
    'central database public facade is final' -CaseSensitive
Assert-Match $publicText 'class\s+WindowsProjectDatabase\s+final' `
    'project database public facade is final' -CaseSensitive
Assert-Match $publicText 'struct\s+DatabaseSchemaSnapshot\s+final' `
    'schema snapshot public model is final' -CaseSensitive
Assert-Match $publicText 'struct\s+DatabaseBackupReport\s+final' `
    'backup report public model is final' -CaseSensitive
Assert-Match $publicText 'struct\s+WindowsProjectDatabaseOptions\s+final' `
    'project options public model is final' -CaseSensitive
foreach ($facade in @('WindowsCentralDatabase','WindowsProjectDatabase')) {
    Assert-Match $publicText ("~$facade\(\) noexcept") "$facade destructor is noexcept" -CaseSensitive
    Assert-Match $publicText ("$facade\(const $facade&\) = delete") `
        "$facade copy construction is deleted" -CaseSensitive
    Assert-Match $publicText ("$facade\($facade&&\) = delete") `
        "$facade move construction is deleted" -CaseSensitive
    Assert-Match $publicText 'struct\s+Impl;' "$facade remains behind a private implementation" -CaseSensitive
}
Assert-NoMatch $publicText '\b(?:throw|try|catch)\b' `
    'public persistence contract contains no exception transport'
Assert-Exact ([regex]::Matches($publicText, 'Domain::Result<').Count) 10 `
    'public persistence typed-result method count'
Assert-Exact ([regex]::Matches($publicText, '\)\s*(?:const\s+)?noexcept\s*;').Count) 15 `
    'public persistence noexcept declaration count'

$productCppFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'include') -Recurse -File |
        Where-Object { $_.Extension -in @('.h','.hpp','.cpp','.cxx') }
) + @(
    Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src') -Recurse -File |
        Where-Object { $_.Extension -in @('.h','.hpp','.cpp','.cxx') }
)
$winsqliteIncludePattern = '^\s*#\s*include\s*[<"]winsqlite/winsqlite3\.h[>"]'
$winsqliteProductIncludes = Get-MatchingRelativeFiles $productCppFiles $winsqliteIncludePattern
Assert-Set $winsqliteProductIncludes @(
    'src/Persistence/Windows/Detail/AnchoredSqliteVfs.cpp',
    'src/Persistence/Windows/Detail/WinsqliteBackup.cpp',
    'src/Persistence/Windows/Detail/WinsqliteConnection.cpp',
    'src/Persistence/Windows/Detail/WinsqliteError.cpp',
    'src/Persistence/Windows/Detail/WinsqliteOperationGuard.cpp',
    'src/Persistence/Windows/Detail/WinsqliteStatement.cpp',
    'src/Persistence/Windows/Detail/WinsqliteTransaction.cpp') `
    'Winsqlite SDK include confinement to exact private P07 sources'
$standaloneSqliteIncludes = @(Get-MatchingRelativeFiles $productCppFiles `
    '^\s*#\s*include\s*[<"]sqlite3\.h[>"]')
Assert-Set $standaloneSqliteIncludes @() `
    'unapproved standalone sqlite3 header inventory'
foreach ($relativePath in $winsqliteProductIncludes) {
    Assert-Match $relativePath '^src/Persistence/Windows/Detail/[^/]+\.cpp$' `
        "Winsqlite include private-boundary path $relativePath" -CaseSensitive
}

$p07CodeFiles = @(
    @(Get-ChildItem -LiteralPath $publicRoot -File)
    @(Get-ChildItem -LiteralPath $sourceRoot -File)
    @(Get-ChildItem -LiteralPath $detailRoot -File)
    @(Get-ChildItem -LiteralPath $migrationRoot -File)
    @(Get-ChildItem -LiteralPath $testRoot -File)
) | Where-Object { $_.Extension -in @('.h','.hpp','.cpp','.cxx') }
$p07CodeText = @($p07CodeFiles | ForEach-Object {
    "// $(Get-RelativePath $_.FullName)" + [Environment]::NewLine +
        (Get-Content -Raw -LiteralPath $_.FullName)
}) -join [Environment]::NewLine
Assert-NoMatch $p07CodeText '(?:\bpython(?:3)?\b|\.py\b|Py_Initialize|PyObject)' `
    'P07 source and tests contain no Python dependency'
Assert-NoMatch $p07CodeText '(?:\bdotnet\b|System\.Runtime|mscoree|/clr\b|CLRSupport|CSharp)' `
    'P07 source and tests contain no .NET dependency'
Assert-NoMatch $p07CodeText '(?:\bnode(?:\.exe)?\b|\bnpm\b|\bnpx\b|\byarn\b|\bpnpm\b|Electron)' `
    'P07 source and tests contain no Node or Electron dependency'
Assert-NoMatch $p07CodeText '(?:#\s*include\s*[<"](?:Qt|boost/)|\bQApplication\b|\bboost::)' `
    'P07 source and tests contain no Qt or Boost dependency'
$p07ProductionText = @($publicHeaders +
    @(Get-ChildItem -LiteralPath $sourceRoot -File) +
    @(Get-ChildItem -LiteralPath $detailRoot -File) +
    @(Get-ChildItem -LiteralPath $migrationRoot -File) | ForEach-Object {
        "// $(Get-RelativePath $_.FullName)" + [Environment]::NewLine +
            (Get-Content -Raw -LiteralPath $_.FullName)
    }) -join [Environment]::NewLine
Assert-NoMatch $p07ProductionText '(?:std::system\s*\(|\bsystem\s*\(|_popen\s*\(|\bpopen\s*\(|ShellExecute\w*\s*\(|WinExec\s*\(|CreateProcess\w*\s*\(|\bcmd\.exe\b|\bpowershell(?:\.exe)?\b|\bpwsh(?:\.exe)?\b)' `
    'P07 production contains no runtime shell or process-launch path'

$centralHeader = Get-Content -Raw -LiteralPath (Join-Path $migrationRoot 'CentralMigrations.h')
$projectHeader = Get-Content -Raw -LiteralPath (Join-Path $migrationRoot 'ProjectMigrations.h')
Assert-Match $centralHeader 'inline\s+constexpr\s+int\s+CentralSourceVersion\s*=\s*5\s*;' `
    'central source compatibility version pin' -CaseSensitive
Assert-Match $centralHeader 'inline\s+constexpr\s+int\s+CentralPhysicalVersion\s*=\s*6\s*;' `
    'central physical schema version pin' -CaseSensitive
Assert-Match $projectHeader 'inline\s+constexpr\s+int\s+ProjectSourceVersion\s*=\s*1\s*;' `
    'project source compatibility version pin' -CaseSensitive
Assert-Match $projectHeader 'inline\s+constexpr\s+int\s+ProjectPhysicalVersion\s*=\s*2\s*;' `
    'project physical schema version pin' -CaseSensitive

$centralRows = @(
    '1|C001|C001|6d34b6a07a3d74440b598f2ca8b73ce84b615f99b814911b0f23e517e77c3eeb',
    '2|C002|C002|3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315',
    '3|C003|C003|600c16d28acd5f54a53a900d20e9ca51392a764e4bc9cdcb0b0b895a335173d9',
    '4|C004|C004|653de9cd69b5a570b2269304715742375958e80335fead0a708362a134328936',
    '5|C005|C005|e710c085f429574b82013d1bd5d711418147fdb15b91a1de7f74a83e14703cba',
    '6|C006|C006|2f4ebc81ba122ca1a471504ce69fad1b11e7cbeecedd972024a521ebc849c427')
$projectRows = @(
    '1|P001|P001|9c9d3e635b2c75088da271ca773f4cea18aca862c40d77ad13f3d9ea183a514f',
    '2|P002|P002|69fa2b2c63f84903badba580bc0692804a9569a8b2a50b55b919e0c9881b0c08')
Assert-MigrationManifest (Join-Path $migrationRoot 'CentralMigrations.cpp') 'C' `
    $centralRows 'central migration manifest'
Assert-MigrationManifest (Join-Path $migrationRoot 'ProjectMigrations.cpp') 'P' `
    $projectRows 'project migration manifest'

$fixtureHashes = [ordered]@{
    'central-v3-minimal.sql' = 'f67826c542332b85b2d436153679934a61726cef36f9e75a334d1934f705aa20'
    'central-v3.sql' = '879741100764d0aeaaee8cbf0790214fd311b03227d6da4d77ad47c69337f9f5'
    'central-v5.sql' = 'eaa76a623a626cd0d6f8022351e1e94ce64c23e5dbdd84785df8f3a1c1acfb0f'
    'project-v1.sql' = '408dbe2871de9f2ff3a7c927af91a7a3081dd5d2ad9cb64548ef555fd4e006d5'
    'README.md' = 'a7c794591868ab0959e59e76e85fd8d8562f59cd339e6adf9a00fb826a586de3'
}
foreach ($fixtureName in $fixtureHashes.Keys) {
    $fixturePath = Join-Path $fixtureRoot $fixtureName
    Assert-Exact (Get-FileSha256 $fixturePath) $fixtureHashes[$fixtureName] `
        "immutable fixture SHA-256 $fixtureName"
}

$adrTitles = [ordered]@{
    'P07-001-winsqlite-persistence-boundary-and-ownership.md' =
        '# P07-001: Winsqlite Persistence Boundary and Ownership'
    'P07-002-source-compatible-schema-lineage.md' =
        '# P07-002: Source-Compatible Schema Lineage'
    'P07-003-anchored-database-namespace-and-online-backups.md' =
        '# P07-003: Anchored Database Namespace and Online Backups'
}
foreach ($adrName in $adrNames) {
    $adrText = Get-Content -Raw -LiteralPath (Join-Path $decisionRoot $adrName)
    Assert-Match $adrText ('\A' + [regex]::Escape($adrTitles[$adrName]) + '\r?\n') `
        "$adrName exact title" -CaseSensitive
    Assert-Match $adrText '^Status:\s+Accepted\r?$' "$adrName accepted status" -CaseSensitive
    Assert-Match $adrText '^Date:\s+2026-08-25\r?$' "$adrName decision date" -CaseSensitive
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
$persistenceSourceSet = [regex]::Match(
    $cmake,
    'set\s*\(\s*FORGE_PERSISTENCE_WINDOWS_SOURCES(?<body>.*?)\)',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $persistenceSourceSet.Success 'CMake P07 production source set'
$cmakePersistenceSources = @(Get-CMakeTokens $persistenceSourceSet.Groups['body'].Value)
Assert-Sequence $cmakePersistenceSources @(
    'src/Persistence/Windows/DatabaseBackupCoordinator.cpp',
    'src/Persistence/Windows/DatabaseQuarantine.cpp',
    'src/Persistence/Windows/WindowsCentralDatabase.cpp',
    'src/Persistence/Windows/WindowsProjectDatabase.cpp',
    'src/Persistence/Windows/Detail/AnchoredSqliteVfs.cpp',
    'src/Persistence/Windows/Detail/DatabaseNamespaceLease.cpp',
    'src/Persistence/Windows/Detail/WindowsDatabaseStore.cpp',
    'src/Persistence/Windows/Detail/WinsqliteBackup.cpp',
    'src/Persistence/Windows/Detail/WinsqliteConnection.cpp',
    'src/Persistence/Windows/Detail/WinsqliteError.cpp',
    'src/Persistence/Windows/Detail/WinsqliteOperationGuard.cpp',
    'src/Persistence/Windows/Detail/WinsqliteStatement.cpp',
    'src/Persistence/Windows/Detail/WinsqliteTransaction.cpp',
    'src/Persistence/Windows/Migrations/CentralMigrations.cpp',
    'src/Persistence/Windows/Migrations/ProjectMigrations.cpp',
    'src/Persistence/Windows/Migrations/SchemaMigrator.cpp') `
    'CMake ordered P07 production source inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'add_library' `
    'ForgeConductor.Persistence.Windows' 'CMake persistence static library')) @(
        'STATIC','${FORGE_PERSISTENCE_WINDOWS_SOURCES}') `
    'CMake persistence static-library declaration'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'forge_add_layer' `
    'ForgeConductor.Persistence.Windows' 'CMake persistence layer declaration')) @(
        'ForgeConductor::Persistence.Windows',
        'ForgeConductor::Contracts',
        'ForgeConductor::Infrastructure.Windows') `
    'CMake persistence public layer dependencies'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Persistence.Windows' `
    'CMake persistence include directories')) @(
        'PUBLIC','${PROJECT_SOURCE_DIR}/include',
        'PRIVATE','${PROJECT_SOURCE_DIR}/src') `
    'CMake persistence include visibility'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Persistence.Windows' `
    'CMake persistence private links')) @(
        'PRIVATE','nlohmann_json::nlohmann_json','bcrypt','winsqlite3') `
    'CMake persistence exact private-link inventory'
Assert-Match $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Persistence\.Windows\s*\)' `
    'CMake persistence native target configuration' -CaseSensitive

Assert-Sequence (Get-CMakeExecutableSources $cmake `
    'ForgeConductor.Persistence.ProcessFixture') @(
        'tests/Persistence/PersistenceProcessFixture.cpp') `
    'CMake persistence process-fixture source inventory'
Assert-Sequence (Get-CMakeExecutableSources $cmake `
    'ForgeConductor.Persistence.UnitTests') @(
        'tests/Persistence/BackupIntegrityTests.cpp',
        'tests/Persistence/CentralMigrationTests.cpp',
        'tests/Persistence/DatabaseAuthorityTests.cpp',
        'tests/Persistence/DatabaseConcurrencyTests.cpp',
        'tests/Persistence/DatabaseStoreSerializationTests.cpp',
        'tests/Persistence/IntegrityRecoveryTests.cpp',
        'tests/Persistence/PersistenceTestMain.cpp',
        'tests/Persistence/ProjectMigrationTests.cpp',
        'tests/Persistence/WinsqliteKernelTests.cpp') `
    'CMake ordered persistence unit-test source inventory'
Assert-Sequence (Get-CMakeExecutableSources $cmake `
    'ForgeConductor.Persistence.HeaderSelfContainment') @(
        'tests/Architecture/P07HeaderSelfContainmentMain.cpp') `
    'CMake P07 header-test main inventory'
Assert-Match $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Persistence\.ProcessFixture\s*\)' `
    'CMake process fixture native configuration' -CaseSensitive
Assert-Match $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Persistence\.UnitTests\s*\)' `
    'CMake persistence unit-test native configuration' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Persistence.ProcessFixture' `
    'CMake persistence fixture includes')) @(
        'PRIVATE','${PROJECT_SOURCE_DIR}/src','${PROJECT_SOURCE_DIR}/tests') `
    'CMake persistence process-fixture include inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Persistence.ProcessFixture' `
    'CMake persistence fixture links')) @(
        'PRIVATE','ForgeConductor::Persistence.Windows') `
    'CMake persistence process-fixture link inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Persistence.UnitTests' `
    'CMake persistence test includes')) @(
        'PRIVATE','${PROJECT_SOURCE_DIR}/src','${PROJECT_SOURCE_DIR}/tests') `
    'CMake persistence unit-test include inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Persistence.UnitTests' `
    'CMake persistence test links')) @(
        'PRIVATE','ForgeConductor::Persistence.Windows','nlohmann_json::nlohmann_json') `
    'CMake persistence unit-test link inventory'
Assert-Match $cmake 'add_dependencies\s*\(\s*ForgeConductor\.Persistence\.UnitTests\s+ForgeConductor\.Persistence\.ProcessFixture\s*\)' `
    'CMake persistence tests build their process fixture' -CaseSensitive

$cmakeG07Tests = @([regex]::Matches(
    $cmake,
    'add_test\s*\(\s*NAME\s+(?<name>ForgeConductor\.Persistence\.[A-Za-z]+)',
    [Text.RegularExpressions.RegexOptions]::Singleline) |
        ForEach-Object { $_.Groups['name'].Value })
Assert-Set $cmakeG07Tests @(
    'ForgeConductor.Persistence.HeaderSelfContainment',
    'ForgeConductor.Persistence.UnitTests') 'exact two G07 CTest definitions'
Assert-NoMatch $cmake 'add_test\s*\(\s*NAME\s+ForgeConductor\.Persistence\.ProcessFixture\b' `
    'persistence process fixture is build-only, never an independent test' -CaseSensitive
Assert-Match $cmake 'NAME\s+ForgeConductor\.Persistence\.UnitTests\s+COMMAND\s+\$<TARGET_FILE:ForgeConductor\.Persistence\.UnitTests>\s+"\$\{PROJECT_SOURCE_DIR\}/tests/Persistence/Fixtures"\s+"\$<TARGET_FILE:ForgeConductor\.Persistence\.ProcessFixture>"' `
    'P07 unit-test command receives fixture directory and helper executable' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'set_tests_properties' 'ForgeConductor.Persistence.UnitTests' `
    'CMake P07 unit-test properties')) @(
        'PROPERTIES','LABELS','T-UNIT;T-DB;T-SEC;G07','TIMEOUT','240') `
    'CMake P07 unit-test exact labels and timeout'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'set_tests_properties' 'ForgeConductor.Persistence.HeaderSelfContainment' `
    'CMake P07 header-test properties')) @(
        'PROPERTIES','LABELS','T-UNIT;G07','TIMEOUT','60') `
    'CMake P07 header-test exact labels and timeout'
Assert-Match $cmake 'file\s*\(\s*GLOB\s+_forge_persistence_windows_headers\s+CONFIGURE_DEPENDS\s+"\$\{PROJECT_SOURCE_DIR\}/include/ForgeConductor/Persistence/Windows/\*\.h"\s*\)' `
    'P07 public-header isolation glob' -CaseSensitive
Assert-Match $cmake 'generated/p07-header-isolation' `
    'P07 isolated-header generation directory' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake 'add_library' `
    'ForgeConductor.PersistenceWindows.HeaderObjects' `
    'CMake P07 header object target')) @(
        'OBJECT','${_forge_p07_header_isolation_sources}') `
    'CMake P07 header object declaration'
Assert-Match $cmake 'forge_configure_standard_target\s*\(\s*ForgeConductor\.PersistenceWindows\.HeaderObjects\s*\)' `
    'CMake P07 header object standard configuration' -CaseSensitive
Assert-Match $cmake 'forge_configure_standard_target\s*\(\s*ForgeConductor\.Persistence\.HeaderSelfContainment\s*\)' `
    'CMake P07 header executable standard configuration' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.PersistenceWindows.HeaderObjects' `
    'CMake P07 header object links')) @(
        'PRIVATE','ForgeConductor::Persistence.Windows') `
    'CMake P07 header object link inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Persistence.HeaderSelfContainment' `
    'CMake P07 header executable links')) @(
        'PRIVATE','ForgeConductor.PersistenceWindows.HeaderObjects',
        'ForgeConductor::Persistence.Windows') `
    'CMake P07 header executable link inventory'
$persistenceCMakeSurface = @(
    $persistenceSourceSet.Groups['body'].Value,
    (Get-CMakeInvocationBody $cmake 'add_library' `
        'ForgeConductor.Persistence.Windows' 'CMake persistence static library surface'),
    (Get-CMakeInvocationBody $cmake 'forge_add_layer' `
        'ForgeConductor.Persistence.Windows' 'CMake persistence layer surface'),
    (Get-CMakeInvocationBody $cmake 'target_include_directories' `
        'ForgeConductor.Persistence.Windows' 'CMake persistence include surface'),
    (Get-CMakeInvocationBody $cmake 'target_link_libraries' `
        'ForgeConductor.Persistence.Windows' 'CMake persistence link surface')
) -join [Environment]::NewLine
Assert-NoMatch $persistenceCMakeSurface '(?:Boost|Qt|Python|Electron|System\.Runtime|mscoree|\bnode\b)' `
    'CMake P07 target surface has no forbidden dependency'

$testMain = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'PersistenceTestMain.cpp')
$registrationFunctions = @(
    'registerWinsqliteKernelTests',
    'registerCentralMigrationTests',
    'registerProjectMigrationTests',
    'registerBackupIntegrityTests',
    'registerDatabaseConcurrencyTests',
    'registerDatabaseAuthorityTests',
    'registerDatabaseStoreSerializationTests',
    'registerIntegrityRecoveryTests')
foreach ($registrationFunction in $registrationFunctions) {
    Assert-Exact ([regex]::Matches(
        $testMain,
        ('ForgeConductor::Tests::' + [regex]::Escape($registrationFunction) + '\s*\(')).Count) 1 `
        "P07 test main invokes $registrationFunction exactly once"
}
Assert-Match $testMain 'argumentCount\s*!=\s*3' `
    'P07 test main requires exact fixture and process-helper arguments' -CaseSensitive
Assert-Match $testMain 'is_directory\s*\(\s*fixtures[\s\S]*?is_regular_file\s*\(\s*processFixture' `
    'P07 test main validates both external test inputs' -CaseSensitive

$registrationOwners = [ordered]@{
    'WinsqliteKernelTests.cpp' = 'registerWinsqliteKernelTests'
    'CentralMigrationTests.cpp' = 'registerCentralMigrationTests'
    'ProjectMigrationTests.cpp' = 'registerProjectMigrationTests'
    'BackupIntegrityTests.cpp' = 'registerBackupIntegrityTests'
    'DatabaseConcurrencyTests.cpp' = 'registerDatabaseConcurrencyTests'
    'DatabaseAuthorityTests.cpp' = 'registerDatabaseAuthorityTests'
    'DatabaseStoreSerializationTests.cpp' = 'registerDatabaseStoreSerializationTests'
    'IntegrityRecoveryTests.cpp' = 'registerIntegrityRecoveryTests'
}
foreach ($testFileName in $registrationOwners.Keys) {
    $testText = Get-Content -Raw -LiteralPath (Join-Path $testRoot $testFileName)
    Assert-Exact ([regex]::Matches(
        $testText,
        ('void\s+' + [regex]::Escape($registrationOwners[$testFileName]) + '\s*\(')).Count) 1 `
        "$testFileName exact native registration entry point"
    Assert-Match $testText 'addTest\s*\(\s*tests\s*,' `
        "$testFileName registers native tests" -CaseSensitive
}
$kernelTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'WinsqliteKernelTests.cpp')
Assert-Set @([regex]::Matches($kernelTests, '"(?<name>persistence\.kernel\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.kernel.open-modes',
        'persistence.kernel.defensive-settings',
        'persistence.kernel.typed-statements',
        'persistence.kernel.complete-sql-script',
        'persistence.kernel.transactions',
        'persistence.kernel.transaction-concurrency',
        'persistence.kernel.cancellation-deadline',
        'persistence.kernel.busy-timeout',
        'persistence.kernel.online-backup',
        'persistence.kernel.explicit-close',
        'persistence.kernel.quarantine-close-fallback') `
    'exact Winsqlite kernel native-test inventory'
$centralTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'CentralMigrationTests.cpp')
Assert-Set @([regex]::Matches($centralTests, '"(?<name>persistence\.central\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.central.fresh-idempotent',
        'persistence.central.v3-preservation',
        'persistence.central.v3-minimal-preservation',
        'persistence.central.v3-minimal-concurrent-idempotent',
        'persistence.central.v5-preservation',
        'persistence.central.reject-read-only') `
    'exact central migration native-test inventory'
$projectTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'ProjectMigrationTests.cpp')
Assert-Set @([regex]::Matches($projectTests, '"(?<name>persistence\.project\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.project.fresh-idempotent',
        'persistence.project.v1-preservation-fts-backfill',
        'persistence.project.fts-lifecycle-metadata',
        'persistence.project.reject-read-only',
        'persistence.project.reject-comment-split-schema',
        'persistence.project.rollback-integrity',
        'persistence.project.backup-receipt-lease',
        'persistence.project.backup-provenance-rejection',
        'persistence.project.backup-generation') `
    'exact project migration native-test inventory'
$backupTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'BackupIntegrityTests.cpp')
Assert-Set @([regex]::Matches($backupTests, '"(?<name>persistence\.(?:backup|quarantine)\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.backup.wal-online',
        'persistence.backup.operation-collision',
        'persistence.backup.final-sidecar-collision',
        'persistence.backup.same-operation-race',
        'persistence.backup.cancelled-cleanup',
        'persistence.backup.stale-stage-policy',
        'persistence.quarantine.notadb-manifest',
        'persistence.quarantine.corrupt-manifest',
        'persistence.quarantine.no-false-claim',
        'persistence.quarantine.collision-no-overwrite',
        'persistence.quarantine.source-role-injection',
        'persistence.quarantine.evidence-role-injection',
        'persistence.quarantine.manifest-source-pin',
        'persistence.quarantine.hardlink-postcommit',
        'persistence.quarantine.size-preflight') `
    'exact backup and quarantine native-test inventory'
$authorityTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'DatabaseAuthorityTests.cpp')
Assert-Set @([regex]::Matches($authorityTests, '"(?<name>persistence\.authority\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.authority.canonical-paths',
        'persistence.authority.directory-flags',
        'persistence.authority.hard-link',
        'persistence.authority.cohort-identities',
        'persistence.authority.replacement-pins',
        'persistence.authority.vfs-denials',
        'persistence.authority.publication',
        'persistence.authority.publication-lock-waiter',
        'persistence.authority.bounded-enumeration') `
    'exact anchored authority native-test inventory'
$concurrencyTests = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'DatabaseConcurrencyTests.cpp')
Assert-Set @([regex]::Matches($concurrencyTests, '"(?<name>persistence\.concurrency\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.concurrency.migration-lock',
        'persistence.concurrency.migration-lock-waiter-rotation',
        'persistence.concurrency.simultaneous-initializers',
        'persistence.concurrency.wal-visibility',
        'persistence.concurrency.busy-window') `
    'exact multi-process concurrency native-test inventory'
$storeTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'DatabaseStoreSerializationTests.cpp')
Assert-Set @([regex]::Matches($storeTests, '"(?<name>persistence\.store\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.store.admission-context',
        'persistence.store.operation-close-serialization') `
    'exact store serialization native-test inventory'
$recoveryTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'IntegrityRecoveryTests.cpp')
Assert-Set @([regex]::Matches($recoveryTests, '"(?<name>persistence\.recovery\.[^"]+)"') |
    ForEach-Object { $_.Groups['name'].Value }) @(
        'persistence.recovery.original-cancellation',
        'persistence.recovery.original-deadline') `
    'exact integrity recovery native-test inventory'

$frameworkRoot = Join-Path $WorkspaceRoot '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count before P07 builds'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count before P07 builds'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti hash before P07 builds'

if ($StaticOnly) {
    Write-Host "G07 static validation passed: $script:AssertionCount fail-closed assertions."
    return
}

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G07: building complete x64 Debug tree from a fresh build directory.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Assert-True $? 'x64 Debug full build'
foreach ($label in @('G07','G06','G05','G04')) {
    Write-Host "G07: testing x64 Debug retained $label inventory."
    & $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Debug $label tests"
}
Write-Host 'G07: building complete x64 Release tree.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Assert-True $? 'x64 Release full build'
foreach ($label in @('G07','G06','G05','G04')) {
    Write-Host "G07: testing x64 Release retained $label inventory."
    & $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label $label
    Assert-True $? "x64 Release $label tests"
}

$toolchainStatePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
Assert-True (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) `
    'recorded toolchain state exists'
$toolchainState = Read-Json $toolchainStatePath
Assert-True ([bool]$toolchainState.complete) 'recorded toolchain state is complete'
$ctestPath = [string]$toolchainState.tools.ctest
$linkerPath = [string]$toolchainState.tools.link
Assert-True (-not [string]::IsNullOrWhiteSpace($ctestPath) -and
    (Test-Path -LiteralPath $ctestPath -PathType Leaf)) `
    'CTest executable from recorded toolchain state'
Assert-True (-not [string]::IsNullOrWhiteSpace($linkerPath) -and
    (Test-Path -LiteralPath $linkerPath -PathType Leaf)) `
    'MSVC linker executable from recorded toolchain state'
Assert-Exact (Resolve-Path -LiteralPath $linkerPath).Path `
    (Resolve-Path -LiteralPath ([string]$toolchainState.msvc.linker)).Path `
    'recorded tools.link and msvc.linker identity'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$expectedG07Tests = @(
    'ForgeConductor.Persistence.HeaderSelfContainment',
    'ForgeConductor.Persistence.UnitTests')
$expectedArtifacts = @(
    'lib/{0}/ForgeConductor.Domain.lib',
    'lib/{0}/ForgeConductor.Infrastructure.Windows.lib',
    'lib/{0}/ForgeConductor.Persistence.Windows.lib',
    'bin/{0}/ForgeConductor.Contracts.ContractTests.exe',
    'bin/{0}/ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Domain.UnitTests.exe',
    'bin/{0}/ForgeConductor.ForsettiHostSmoke.exe',
    'bin/{0}/ForgeConductor.Infrastructure.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Infrastructure.ProcessTests.exe',
    'bin/{0}/ForgeConductor.Infrastructure.ShutdownTests.exe',
    'bin/{0}/ForgeConductor.Infrastructure.UnitTests.exe',
    'bin/{0}/ForgeConductor.Persistence.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Persistence.ProcessFixture.exe',
    'bin/{0}/ForgeConductor.Persistence.UnitTests.exe',
    'bin/{0}/ForgeConductor.ProcessFixture.exe',
    'bin/{0}/forge-conductor.exe')
$artifactHashes = [ordered]@{}
$peDependencyEvidence = [ordered]@{}
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
    }

    $ctestJsonText = (& $ctestPath --test-dir $buildRoot -C $configuration `
        -L G07 --show-only=json-v1) -join [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 "$configuration G07 CTest JSON inventory command"
    try {
        $ctestInventory = $ctestJsonText | ConvertFrom-Json
    } catch {
        throw "G07 assertion failed: invalid $configuration CTest JSON inventory - $($_.Exception.Message)"
    }
    Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) $expectedG07Tests `
        "$configuration exact G07 CTest inventory"

    $buildRootForward = $buildRoot.Replace('\', '/')
    $workspaceRootForward = $WorkspaceRoot.Replace('\', '/')
    $expectedCTestCommands = [ordered]@{
        'ForgeConductor.Persistence.UnitTests' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Persistence.UnitTests.exe",
            "$workspaceRootForward/tests/Persistence/Fixtures",
            "$buildRootForward/bin/$configuration/ForgeConductor.Persistence.ProcessFixture.exe")
        'ForgeConductor.Persistence.HeaderSelfContainment' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Persistence.HeaderSelfContainment.exe")
    }
    $expectedCTestLabels = [ordered]@{
        'ForgeConductor.Persistence.UnitTests' = @('G07','T-DB','T-SEC','T-UNIT')
        'ForgeConductor.Persistence.HeaderSelfContainment' = @('G07','T-UNIT')
    }
    foreach ($test in @($ctestInventory.tests)) {
        $testName = [string]$test.name
        Assert-Exact ([string]$test.config) $configuration `
            "$configuration CTest configuration for $testName"
        Assert-Set @($test.properties | ForEach-Object { $_.name }) @(
            'LABELS','TIMEOUT','WORKING_DIRECTORY') `
            "$configuration CTest property inventory for $testName"
        $expectedCommand = @($expectedCTestCommands[$testName])
        Assert-Sequence @($test.command) $expectedCommand `
            "$configuration exact CTest command for $testName"
        $labelsProperty = @($test.properties | Where-Object { $_.name -ceq 'LABELS' })
        Assert-Exact $labelsProperty.Count 1 `
            "$configuration CTest label property count for $testName"
        Assert-Set @($labelsProperty[0].value) @($expectedCTestLabels[$testName]) `
            "$configuration exact CTest labels for $testName"
        $timeoutProperty = @($test.properties | Where-Object { $_.name -ceq 'TIMEOUT' })
        Assert-Exact $timeoutProperty.Count 1 `
            "$configuration CTest timeout property count for $testName"
        $expectedTimeout = if ($testName -ceq 'ForgeConductor.Persistence.UnitTests') {
            240.0
        } else {
            60.0
        }
        Assert-Exact ([double]$timeoutProperty[0].value) $expectedTimeout `
            "$configuration exact CTest timeout for $testName"
        $workingDirectoryProperty = @($test.properties | Where-Object {
            $_.name -ceq 'WORKING_DIRECTORY'
        })
        Assert-Exact $workingDirectoryProperty.Count 1 `
            "$configuration CTest working-directory property count for $testName"
        Assert-Exact ([string]$workingDirectoryProperty[0].value) $buildRootForward `
            "$configuration CTest working directory for $testName"
    }

    $unitTestPath = Join-Path $buildRoot `
        "bin\$configuration\ForgeConductor.Persistence.UnitTests.exe"
    $dependencyOutput = (& $linkerPath /dump /dependents $unitTestPath 2>&1) -join `
        [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 `
        "$configuration recorded linker /dump /dependents command"
    Assert-Exact ([regex]::Matches(
        $dependencyOutput,
        '^\s*winsqlite3\.dll\s*$',
        [Text.RegularExpressions.RegexOptions]::Multiline -bor
            [Text.RegularExpressions.RegexOptions]::IgnoreCase).Count) 1 `
        "$configuration PE imports exactly one winsqlite3.dll"
    Assert-NoMatch $dependencyOutput '^\s*sqlite3\.dll\s*$' `
        "$configuration PE does not import a bundled sqlite3.dll"
    $peDependencyEvidence[$configuration] = Get-StringSha256 $dependencyOutput
    Write-Host "$configuration winsqlite3 PE dependency evidence SHA-256: $($peDependencyEvidence[$configuration])"
}

$generatedHeaderRoot = Join-Path $buildRoot 'generated\p07-header-isolation'
$generatedHeaderSources = @(Get-ChildItem -LiteralPath $generatedHeaderRoot -File -Filter '*.cpp')
Assert-Exact $generatedHeaderSources.Count 4 `
    'generated P07 isolated public-header translation-unit count'
$expectedHeaderSources = [ordered]@{}
foreach ($header in $publicHeaders) {
    $includePath = "ForgeConductor/Persistence/Windows/$($header.Name)"
    $stem = $includePath.Replace('/', '_').Replace('.', '_')
    $expectedHeaderSources["$stem.cpp"] =
        "#include <$includePath>" + [char]10 +
        'int ' + $stem + '_isolated() noexcept { return 0; }'
}
Assert-Set @($generatedHeaderSources.Name) @($expectedHeaderSources.Keys) `
    'exact generated P07 isolated-header translation-unit inventory'
foreach ($source in $generatedHeaderSources) {
    $sourceText = (Get-Content -Raw -LiteralPath $source.FullName).
        Replace([string][char]13 + [char]10, [string][char]10).
        TrimEnd([char[]]@([char]13,[char]10))
    Assert-Exact $sourceText $expectedHeaderSources[$source.Name] `
        "generated isolated-header translation unit $($source.Name)"
}

$directoryBuildProps = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'Directory.Build.props')
Assert-Match $directoryBuildProps '<CppLanguageStandard>stdcpp20</CppLanguageStandard>' `
    'repository C++20 pin' -CaseSensitive
Assert-Match $directoryBuildProps '<ConformanceMode>true</ConformanceMode>' `
    'repository MSVC conformance pin' -CaseSensitive
Assert-Match $directoryBuildProps '<TreatWarningAsError>true</TreatWarningAsError>' `
    'repository warnings-as-errors pin' -CaseSensitive
Assert-Match $directoryBuildProps '<PlatformToolset>v143</PlatformToolset>' `
    'repository MSVC v143 pin' -CaseSensitive
Assert-Match $directoryBuildProps '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' `
    'repository Windows SDK pin' -CaseSensitive
Assert-Match $directoryBuildProps '<WindowsTargetPlatformMinVersion>10\.0\.22000\.0</WindowsTargetPlatformMinVersion>' `
    'repository Windows 11 minimum pin' -CaseSensitive

$p07ProjectNames = @(
    'ForgeConductor.Persistence.HeaderSelfContainment',
    'ForgeConductor.Persistence.ProcessFixture',
    'ForgeConductor.Persistence.UnitTests',
    'ForgeConductor.Persistence.Windows',
    'ForgeConductor.PersistenceWindows.HeaderObjects')
foreach ($projectName in $p07ProjectNames) {
    $projectPath = Join-Path $buildRoot "$projectName.vcxproj"
    Assert-True (Test-Path -LiteralPath $projectPath -PathType Leaf) `
        "generated P07 project $projectName"
    $project = Get-Content -Raw -LiteralPath $projectPath
    Assert-Match $project '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' `
        "$projectName SDK 10.0.26100.0" -CaseSensitive
    Assert-Exact ([regex]::Matches($project, '<PlatformToolset>v143</PlatformToolset>').Count) 2 `
        "$projectName Debug/Release v143 toolset count"
    Assert-Exact ([regex]::Matches($project, '<LanguageStandard>stdcpp20</LanguageStandard>').Count) 2 `
        "$projectName Debug/Release C++20 count"
    Assert-Exact ([regex]::Matches($project, '<ConformanceMode>true</ConformanceMode>').Count) 2 `
        "$projectName Debug/Release conformance count"
    Assert-Exact ([regex]::Matches($project, '<WarningLevel>Level4</WarningLevel>').Count) 2 `
        "$projectName Debug/Release warning-level count"
    Assert-Exact ([regex]::Matches($project, '<TreatWarningAsError>true</TreatWarningAsError>').Count) 2 `
        "$projectName Debug/Release warnings-as-errors count"
    Assert-Exact ([regex]::Matches($project, '<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>').Count) 1 `
        "$projectName Debug DLL CRT"
    Assert-Exact ([regex]::Matches($project, '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>').Count) 1 `
        "$projectName Release DLL CRT"
    Assert-NoMatch $project '<CLRSupport>|MultiThreadedDebug</RuntimeLibrary>|MultiThreaded</RuntimeLibrary>' `
        "$projectName must not enable CLR or static CRT" -CaseSensitive
}

$persistenceProjectPath = Join-Path $buildRoot 'ForgeConductor.Persistence.Windows.vcxproj'
$persistenceProject = Get-Content -Raw -LiteralPath $persistenceProjectPath
$persistenceReferences = @([regex]::Matches(
    $persistenceProject,
    '<ProjectReference\s+Include="(?<path>[^"]+)"') |
        ForEach-Object { [IO.Path]::GetFileName($_.Groups['path'].Value) })
Assert-Set $persistenceReferences @(
    'ForgeConductor.Domain.vcxproj',
    'ForgeConductor.Infrastructure.Windows.vcxproj',
    'ZERO_CHECK.vcxproj') `
    'persistence generated-project reference inventory'
$persistenceDependencySurface = @([regex]::Matches(
    $persistenceProject,
    '<(?:AdditionalIncludeDirectories|AdditionalDependencies|AdditionalLibraryDirectories|ForcedIncludeFiles)>[^<]*</(?:AdditionalIncludeDirectories|AdditionalDependencies|AdditionalLibraryDirectories|ForcedIncludeFiles)>|<(?:ClCompile|ProjectReference)\s+Include="[^"]+"') |
        ForEach-Object { $_.Value }) -join [Environment]::NewLine
Assert-Match $persistenceDependencySurface '<AdditionalIncludeDirectories>' `
    'persistence generated-project dependency surface was captured' -CaseSensitive
Assert-NoMatch $persistenceDependencySurface '(?:Forsetti|Boost|Qt|Python|node|Electron|System\.Runtime|mscoree|(?<!win)sqlite3\.lib)' `
    'persistence generated-project forbidden dependency leakage'

# A Visual Studio static-library project has no linker invocation, so CMake
# propagates its PRIVATE native link requirements to final consumers. Prove the
# generated link surface in both persistence executables and the resulting PE
# import above instead of requiring an impossible Link node on the .lib project.
$persistenceConsumerProjectNames = @(
    'ForgeConductor.Persistence.ProcessFixture',
    'ForgeConductor.Persistence.UnitTests')
foreach ($consumerProjectName in $persistenceConsumerProjectNames) {
    $consumerProjectPath = Join-Path $buildRoot "$consumerProjectName.vcxproj"
    $consumerProject = Get-Content -Raw -LiteralPath $consumerProjectPath
    $consumerDependencies = @([regex]::Matches(
        $consumerProject,
        '<AdditionalDependencies>[^<]*</AdditionalDependencies>') |
            ForEach-Object { $_.Value })
    Assert-Exact $consumerDependencies.Count 2 `
        "$consumerProjectName Debug/Release linker-dependency records"
    foreach ($dependencyRecord in $consumerDependencies) {
        Assert-Match $dependencyRecord '(?:>|;)winsqlite3\.lib(?:;|<)' `
            "$consumerProjectName links Windows SDK winsqlite3 import library"
        Assert-Match $dependencyRecord `
            '(?:>|;)(?:[^;<]+[\\/])?ForgeConductor\.Persistence\.Windows\.lib(?:;|<)' `
            "$consumerProjectName links the persistence static library"
        Assert-NoMatch $dependencyRecord `
            '(?:Forsetti|Boost|Qt|Python|node|Electron|System\.Runtime|mscoree|(?<!win)sqlite3\.lib)' `
            "$consumerProjectName generated-link forbidden dependency leakage"
    }
}

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after P07 builds'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti bytes after P07 builds'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after P07 builds'

$gitOutput = & git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1
Assert-Exact $LASTEXITCODE 0 `
    ('git diff --check failed: ' + ($gitOutput -join [Environment]::NewLine))
& (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'governance ledger verification'

$productFileCount = $publicHeaderNames.Count + $topLevelSourceNames.Count +
    $detailHeaderNames.Count + $detailSourceNames.Count +
    $migrationHeaderNames.Count + $migrationSourceNames.Count
Write-Host "G07 database/migration validation passed: $script:AssertionCount fail-closed assertions; $productFileCount product files, 4 isolated headers, $($artifactHashes.Count) binary hashes, two winsqlite3 PE dependency records, and x64 Debug/Release G04+G05+G06+G07 tests passed."
