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
    if (-not $Condition) { throw "G10 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) `
        "$Message (expected '$Expected', found '$Actual')"
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
        Assert-Exact $actualSorted[$index] $expectedSorted[$index] `
            "$Message item $index"
    }
}

function Assert-Sequence {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualItems = @($Actual | ForEach-Object { [string]$_ })
    $expectedItems = @($Expected | ForEach-Object { [string]$_ })
    Assert-Exact $actualItems.Count $expectedItems.Count "$Message count"
    for ($index = 0; $index -lt $expectedItems.Count; $index++) {
        Assert-Exact $actualItems[$index] $expectedItems[$index] `
            "$Message item $index"
    }
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
        if ($bytes[$index] -eq 10 -and
            ($index -eq 0 -or $bytes[$index - 1] -ne 13)) {
            $bareLfCount++
        }
        if ($bytes[$index] -eq 13 -and
            ($index -eq $bytes.Length - 1 -or $bytes[$index + 1] -ne 10)) {
            $bareCrCount++
        }
    }
    Assert-Exact $bareLfCount 0 "$Message bare-LF count"
    Assert-Exact $bareCrCount 0 "$Message bare-CR count"
    Assert-True ($bytes.Length -ge 2 -and
        $bytes[$bytes.Length - 2] -eq 13 -and
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
    return @(Get-CMakeTokens $body | Where-Object { $_ -match '^tests/.+[.]cpp$' })
}

function Get-ClassBody {
    param([string]$Text, [string]$Name, [string]$Message)
    $match = [regex]::Match(
        $Text,
        'class\s+' + [regex]::Escape($Name) + '\b[^\{]*\{(?<body>.*?)\r?\n\};',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    Assert-True $match.Success $Message
    return $match.Groups['body'].Value
}

function Assert-X64PortableExecutable {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -ge 0x40) "$Message has a DOS header"
    $dosSignature = [string][char]$bytes[0] + [string][char]$bytes[1]
    Assert-Exact $dosSignature 'MZ' "$Message DOS signature"
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    Assert-True ($peOffset -ge 0x40 -and $peOffset + 6 -le $bytes.Length) `
        "$Message bounded PE header offset"
    Assert-Exact ([Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4)) `
        "PE`0`0" "$Message PE signature"
    Assert-Exact ([BitConverter]::ToUInt16($bytes, $peOffset + 4)) `
        ([uint16]0x8664) "$Message x64 machine"
}

function Invoke-RepositoryIntegrityChecks {
    $gitOutput = @(& git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1)
    $gitExitCode = $LASTEXITCODE
    Assert-Exact $gitExitCode 0 `
        ('git diff --check failed: ' + ($gitOutput -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 `
    'sealed Forsetti file count before retained and G10 validation'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L `
    'sealed Forsetti byte count before retained and G10 validation'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti hash before retained and G10 validation'

$g09Validator = Join-Path $WorkspaceRoot `
    'scripts\validation\Test-G09LegacyMemory.ps1'
Assert-True (Test-Path -LiteralPath $g09Validator -PathType Leaf) `
    'retained G09 validator exists'
$g09Arguments = @{
    WorkspaceRoot = $WorkspaceRoot
    Parallel = $Parallel
    StaticOnly = $true
}
if ($StaticOnly) {
    Write-Host 'G10: running retained G09 static validation before G10 static review.'
} else {
    Write-Host 'G10: running retained G09 static validation without a retained-gate rebuild.'
}
& $g09Validator @g09Arguments
Assert-True $? 'retained G09 static validation'

$catalogFiles = @(
    'include/ForgeConductor/Application/AgentCatalog.h',
    'src/Application/AgentCatalog.cpp')
$serviceFiles = @(
    'include/ForgeConductor/Application/AgentSessionService.h',
    'src/Application/AgentSessionService.cpp')
$repositoryFiles = @(
    'include/ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h',
    'src/Persistence/Windows/WindowsAgentSessionRepository.cpp')
$completionInspectorFiles = @(
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'src/Infrastructure/Windows/WindowsAgentCompletionReportInspector.cpp')
$supportFiles = @(
    'include/ForgeConductor/Contracts/IAgentServices.h',
    'include/ForgeConductor/Domain/AgentModels.h',
    'src/Domain/AgentModels.cpp',
    'include/ForgeConductor/Domain/Error.h',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Domain/Domain.h',
    'include/ForgeConductor/Persistence/Windows/PersistenceWindows.h')
$testFiles = @(
    'tests/Agents/AgentCatalogTests.cpp',
    'tests/Agents/AgentSessionServiceTests.cpp',
    'tests/Agents/AgentSessionRepositoryWindowsTests.cpp',
    'tests/Agents/AgentSessionProcessFixture.cpp')
$resourceNames = @(
    'debug.md',
    'docs.md',
    'explore.md',
    'implement.md',
    'plan.md',
    'precommit-audit.md',
    'research.md',
    'review.md',
    'security.md',
    'test.md')
$resourceFiles = @($resourceNames | ForEach-Object {
    'src/ForgeConductor.Application/Resources/Agents/' + $_
})
$adrFiles = @(
    '.forge-codex/state/decisions/P10-001-bounded-deterministic-agent-catalog.md',
    '.forge-codex/state/decisions/P10-002-atomic-durable-agent-run-lifecycle.md',
    '.forge-codex/state/decisions/P10-003-agent-service-semantics-and-p14-boundary.md',
    '.forge-codex/state/decisions/P10-004-abrupt-crash-and-concurrency-evidence.md')
$requiredFiles = @(
    'CMakeLists.txt',
    'scripts/validation/Test-G09LegacyMemory.ps1',
    'scripts/validation/Test-G10Agents.ps1',
    '.forge-codex/instructions/plans/mcp-tool-parity.json',
    '.forge-codex/instructions/scripts/Verify-Ledger.ps1') +
    $catalogFiles + $serviceFiles + $repositoryFiles +
    $completionInspectorFiles + $supportFiles +
    $testFiles + $resourceFiles + $adrFiles

$sourceAnchors = [ordered]@{
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/AgentCatalog.swift' = @('e5bb41f33660dba2a20fb97da0cb3df9147b6a4d40da0d2a760f650603ad3a48',21945L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/AgentSessionService.swift' = @('9b9dc37ee186e5d195484fbabd1d5cef25f701675d1e60e082e18d06f9d0cb00',19831L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/AgentToolPack.swift' = @('df7e04af91142c8f9d1c9f6a27acc6b7f4ddb44d9c4d1009df98035805ed19bd',4603L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift' = @('a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0',39463L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/CoreTests.swift' = @('574d4a2b3a51d73240da36881ffbd32519b5382fd35c19a11804b0cab7f70f4e',25483L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityTests.swift' = @('698660433a240038b6347258743f6eaca12e2232b5ca3e0f9557b8f2ddbc4d00',82407L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/debug.md' = @('4013e3cfa6a4af1b51d69d4dc8445cc1e0590b0658fb5d250b4a431c70224bef',1789L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/docs.md' = @('e1118265e19562bf5e470ad6de16cd0c4e009b01e55f263049b24f714649ee55',4047L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/explore.md' = @('0cfd24d91a74e24a64a8716e7fb6f57bb4a5a6388f5f0d2d843bbc4775e09b92',2703L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/implement.md' = @('1459a82d77ceebd099a6fce5ef2504bffac451f0bd9e41c96318c3ff85a70142',2130L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/plan.md' = @('89e9ad8543beaa7a681c48a153569b9b340bdf7fd4f34a9164354c5a6874aaff',1670L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/precommit-audit.md' = @('9dc5693035148f124b1b990e8177aa799b5149fd818da7b9a1d447d537db8ea0',1383L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/research.md' = @('e5b0141797297ae5a5f647d9487445cfbffb43956ce1093e2edfad18ce4cb7e5',1301L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/review.md' = @('cfab52c5a3ecb384dba6bfa022db773729a0b48c2828e7716d970b873826a9cc',1523L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/security.md' = @('b7a7bd4a6174499b75ce095f9208cbec26f6e158a0cd7cffd9d204d782340c99',1453L)
    '.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/test.md' = @('bc29c81555acd940eccfe692d4ded50717f43b87943a13644c62b11783d30cc0',1381L)
}

foreach ($relativePath in @($requiredFiles + $sourceAnchors.Keys | Sort-Object -Unique)) {
    $fullPath = Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $fullPath -PathType Leaf) `
        "required P10 file $relativePath"
}

Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Application') -File -Filter '*Agent*' |
    ForEach-Object { $_.Name }) `
    @('AgentCatalog.h','AgentSessionService.h') `
    'exact P10 Application agent-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Application') -File -Filter '*Agent*' |
    ForEach-Object { $_.Name }) `
    @('AgentCatalog.cpp','AgentSessionService.cpp') `
    'exact P10 Application agent-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Persistence\Windows') -File -Filter '*Agent*' |
    ForEach-Object { $_.Name }) `
    @('WindowsAgentSessionRepository.h') `
    'exact P10 persistence agent-header inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Persistence\Windows') -File -Filter '*Agent*' |
    ForEach-Object { $_.Name }) `
    @('WindowsAgentSessionRepository.cpp') `
    'exact P10 persistence agent-source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Infrastructure\Windows') -File -Filter '*AgentCompletionReport*' |
    ForEach-Object { $_.Name }) `
    @('WindowsAgentCompletionReportInspector.cpp') `
    'exact private Windows completion-report inspector source inventory'
Assert-Set @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Infrastructure\Windows') -File `
    -Filter '*AgentCompletionReport*' | ForEach-Object { $_.Name }) `
    @() 'completion-report inspector has no public concrete header'

$agentTestRoot = Join-Path $WorkspaceRoot 'tests\Agents'
Assert-Set @(Get-ChildItem -LiteralPath $agentTestRoot -Force -File |
    ForEach-Object { $_.Name }) `
    @($testFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) `
    'exact P10 four-file native test inventory'
Assert-Set @(Get-ChildItem -LiteralPath $agentTestRoot -Force -Directory |
    ForEach-Object { $_.Name }) @() 'P10 test subdirectory inventory'

$resourceRoot = Join-Path $WorkspaceRoot `
    'src\ForgeConductor.Application\Resources\Agents'
Assert-Set @(Get-ChildItem -LiteralPath $resourceRoot -Force -File |
    ForEach-Object { $_.Name }) $resourceNames `
    'exact P10 ten-resource inventory'
Assert-Set @(Get-ChildItem -LiteralPath $resourceRoot -Force -Directory |
    ForEach-Object { $_.Name }) @() 'P10 resource subdirectory inventory'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot `
    'resources\Agents'))) 'legacy root-level agent resources remain removed'

$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
Assert-Set @(Get-ChildItem -LiteralPath $decisionRoot -Force -File `
    -Filter 'P10-*.md' | ForEach-Object { $_.Name }) `
    @($adrFiles | ForEach-Object { [IO.Path]::GetFileName($_) }) `
    'exact P10 four-ADR inventory'

foreach ($relativePath in $sourceAnchors.Keys) {
    $fullPath = Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')
    Assert-Exact ([long](Get-Item -LiteralPath $fullPath).Length) `
        ([long]$sourceAnchors[$relativePath][1]) `
        "authoritative source byte count $relativePath"
    Assert-Exact (Get-FileSha256 $fullPath) `
        ([string]$sourceAnchors[$relativePath][0]) `
        "authoritative source SHA-256 $relativePath"
}

$tokens = $null
$parseErrors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $PSCommandPath,
    [ref]$tokens,
    [ref]$parseErrors)
Assert-Exact @($parseErrors).Count 0 'G10 validator PowerShell parser-error count'

$crlfFiles = @(
    'CMakeLists.txt',
    'scripts/validation/Test-G10Agents.ps1') +
    $catalogFiles + $serviceFiles + $repositoryFiles +
    $completionInspectorFiles + $supportFiles +
    $testFiles + $resourceFiles + $adrFiles
foreach ($relativePath in @($crlfFiles | Sort-Object -Unique)) {
    Assert-CrlfTextFile `
        (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) `
        $relativePath
}

$textByPath = [ordered]@{}
foreach ($relativePath in @($catalogFiles + $serviceFiles + $repositoryFiles +
    $completionInspectorFiles + $supportFiles + $testFiles + $resourceFiles +
    $adrFiles |
    Sort-Object -Unique)) {
    $textByPath[$relativePath] = Get-Content -Raw -LiteralPath `
        (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\'))
}
$catalogText = @($catalogFiles | ForEach-Object { $textByPath[$_] }) -join `
    [Environment]::NewLine
$serviceText = @($serviceFiles | ForEach-Object { $textByPath[$_] }) -join `
    [Environment]::NewLine
$repositoryText = @($repositoryFiles | ForEach-Object { $textByPath[$_] }) -join `
    [Environment]::NewLine
$completionInspectorText = @($completionInspectorFiles | ForEach-Object {
    $textByPath[$_]
}) -join [Environment]::NewLine
$testText = @($testFiles | ForEach-Object { $textByPath[$_] }) -join `
    [Environment]::NewLine
$resourceText = @($resourceFiles | ForEach-Object { $textByPath[$_] }) -join `
    [Environment]::NewLine
$productAndTestText = $catalogText + [Environment]::NewLine +
    $serviceText + [Environment]::NewLine + $repositoryText +
    [Environment]::NewLine + $completionInspectorText +
    [Environment]::NewLine + $testText + [Environment]::NewLine + $resourceText

foreach ($relativePath in @($catalogFiles + $serviceFiles + $repositoryFiles +
    $completionInspectorFiles + $supportFiles |
    Where-Object { $_ -match '[.]h$' })) {
    Assert-Match $textByPath[$relativePath] '^#pragma once\r?$' `
        "$relativePath uses pragma-once isolation" -CaseSensitive
}
Assert-NoMatch $productAndTestText `
    '(?i)\b(?:Python|PyBind|Boost|Qt|Electron|node_modules|nodejs|npm|npx|gcnew|Xcode|macOS|Darwin|textutil|cupsfilter|pandoc|search_files|python_exec|python_info|pytest)\b|#using|/clr' `
    'P10 code, tests, and resources contain no prohibited runtime or platform term'
$noAttributionPattern = '(?i)generated' + ' by|AI-' + 'generated|created' +
    ' with|co-authored-' + 'by|Chat' + 'GPT|Open' + 'AI|automated-authorship'
Assert-NoMatch $productAndTestText `
    $noAttributionPattern `
    'P10 product and test artifacts contain no prohibited attribution'
$applicationOnlyText = $catalogText + [Environment]::NewLine + $serviceText
Assert-NoMatch $applicationOnlyText `
    '#\s*include\s*[<"](?:Windows[.]h|windows[.]h|winrt/|wil/|winsqlite/|sqlite3[.]h|nlohmann/)' `
    'P10 Application code has no native, database, or JSON implementation include'
Assert-NoMatch $applicationOnlyText `
    '#\s*include\s*[<"]ForgeConductor/(?:Persistence|Infrastructure|UI)/' `
    'P10 Application code preserves inward dependency direction'
Assert-NoMatch $applicationOnlyText `
    '\b(?:sqlite3|sqlite3_stmt|Winsqlite|HANDLE|HKEY|HRESULT|DWORD|LPWSTR|PCWSTR|OVERLAPPED|winrt|nlohmann)\b' `
    'P10 Application code leaks no native, database, or JSON implementation type'

$catalogHeader = $textByPath['include/ForgeConductor/Application/AgentCatalog.h']
$catalogSource = $textByPath['src/Application/AgentCatalog.cpp']
Assert-Match $catalogHeader `
    'class\s+AgentCatalog\s+final\s*:\s*public\s+Contracts::IAgentCatalog' `
    'AgentCatalog is a final IAgentCatalog implementation' -CaseSensitive
foreach ($deletedMember in @(
    'AgentCatalog(const AgentCatalog&) = delete;',
    'AgentCatalog& operator=(const AgentCatalog&) = delete;',
    'AgentCatalog(AgentCatalog&&) = delete;',
    'AgentCatalog& operator=(AgentCatalog&&) = delete;')) {
    Assert-Exact ([regex]::Matches(
        $catalogHeader,
        [regex]::Escape($deletedMember)).Count) 1 `
        "AgentCatalog exact deleted member $deletedMember"
}
$catalogLimits = [ordered]@{
    MaximumEntries = '256U'
    MandatoryEntryCount = '10U'
    MaximumDefinitionDocuments = '1024U'
    MaximumDefinitionBytes = '64U * 1024U'
    MaximumAggregateDefinitionBytes = '16U * 1024U * 1024U'
    MaximumRecommendationTaskBytes = '4U * 1024U'
    MaximumListItems = '64U'
    MaximumSpecItems = '256U'
}
foreach ($limit in $catalogLimits.GetEnumerator()) {
    Assert-Exact ([regex]::Matches(
        $catalogHeader,
        'static\s+constexpr\s+std::size_t\s+' + [regex]::Escape($limit.Key) +
            '\s*=\s*' + [regex]::Escape($limit.Value) + '\s*;').Count) 1 `
        "AgentCatalog exact bound $($limit.Key)"
}
Assert-Match $catalogSource `
    'std::atomic<std::shared_ptr<const Snapshot>>\s+snapshot_' `
    'AgentCatalog owns an atomic immutable snapshot' -CaseSensitive
Assert-Exact ([regex]::Matches($catalogSource,'snapshot_[.]store\(').Count) 2 `
    'AgentCatalog exact initial and reload snapshot publication count'
Assert-Match $catalogSource `
    'MaximumEntries\s*-\s*AgentCatalog::MandatoryEntryCount' `
    'AgentCatalog reserves capacity for every mandatory definition' -CaseSensitive
$catalogParserSlice = Get-SourceSlice $catalogSource `
    '[[nodiscard]] Domain::Result<Domain::AgentSpec> parseDefinitionUnchecked(' `
    '[[nodiscard]] Domain::AgentId requiredAgentId(' `
    'bounded agent definition parser'
Assert-Match $catalogParserSlice `
    'const auto openingFenceEnd\s*=\s*normalized[.]find\(''\\n''\);.*?substr\(0U,\s*openingFenceEnd\)\s*!=\s*"---"' `
    'frontmatter opening fence must be one exact literal line' -CaseSensitive
Assert-Match $catalogParserSlice `
    'substr\(lineStart,\s*lineLength\)\s*==\s*"---"' `
    'frontmatter closing fence must be one exact literal line' -CaseSensitive
Assert-NoMatch $catalogParserSlice `
    '(?:starts_with|ends_with)\s*\(\s*"---"\s*\)' `
    'frontmatter fences are never accepted by prefix or suffix' -CaseSensitive

$catalogParserTestText = $textByPath['tests/Agents/AgentCatalogTests.cpp']
$catalogParserTestSlice = Get-SourceSlice $catalogParserTestText `
    'void testSourceCompatibleParserAndValidation()' `
    'void testParserStructuralBounds()' `
    'catalog parser native test'
foreach ($malformedFenceCase in @(
    '"opening-prefix.md", "----\nid: bad\n---\nbody"',
    '"closing-suffix.md", "---\nid: bad\n---extra\nbody"',
    '"embedded-fence.md", "---\nid: bad\ndescription: alpha---omega\nbody"')) {
    Assert-Match $catalogParserTestSlice ([regex]::Escape($malformedFenceCase)) `
        "catalog parser rejects exact malformed fence case $malformedFenceCase" `
        -CaseSensitive
}
Assert-Match $catalogParserTestSlice `
    'for\s*\(\s*const auto& candidate\s*:\s*malformed\s*\).*?REQUIRE\(!result\);.*?ErrorCodes::InvalidRequest' `
    'every malformed fence case fails with invalid_request' -CaseSensitive

$toolInventoryPath = Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\mcp-tool-parity.json'
try {
    $toolInventory = Get-Content -Raw -LiteralPath $toolInventoryPath |
        ConvertFrom-Json
} catch {
    throw "G10 assertion failed: invalid MCP inventory - $($_.Exception.Message)"
}
Assert-Exact ([int]$toolInventory.schema_version) 1 'MCP inventory schema'
Assert-Exact ([int]$toolInventory.expected_tool_count) 53 `
    'MCP inventory expected tool count'
Assert-Exact @($toolInventory.tools).Count 53 'MCP inventory actual tool count'
$canonicalBlock = [regex]::Match(
    $catalogSource,
    'CanonicalTools\{(?<body>.*?)\};',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $canonicalBlock.Success 'AgentCatalog canonical tool block'
$catalogTools = @([regex]::Matches(
    $canonicalBlock.Groups['body'].Value,
    '"(?<tool>[^"]+)"') | ForEach-Object { $_.Groups['tool'].Value })
Assert-Sequence $catalogTools @($toolInventory.tools | ForEach-Object { $_.name }) `
    'AgentCatalog exact canonical tool inventory and order'

$fallbackBlock = Get-SourceSlice $catalogSource `
    'std::vector<Domain::AgentSpec> mandatoryFallbacks()' `
    'constexpr std::array<std::string_view, AgentCatalog::MandatoryEntryCount>' `
    'embedded mandatory fallbacks'
$fallbackIds = @([regex]::Matches(
    $fallbackBlock,
    'values[.]push_back\(fallback\(\s*"(?<id>[a-z0-9-]+)"') |
    ForEach-Object { $_.Groups['id'].Value })
$mandatoryIds = @($resourceNames | ForEach-Object {
    [IO.Path]::GetFileNameWithoutExtension($_)
})
Assert-Set $fallbackIds $mandatoryIds 'exact ten embedded mandatory fallback ids'

$rulesBlock = [regex]::Match(
    $catalogSource,
    'RecommendationRules\s*=\s*std::to_array<RecommendationRule>\(\{(?<body>.*?)\}\);',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $rulesBlock.Success 'AgentCatalog recommendation-rule block'
$actualRules = @([regex]::Matches(
    $rulesBlock.Groups['body'].Value,
    '\{"(?<agent>[a-z0-9-]+)",\s*"(?<keyword>[^"]+)"\}') |
    ForEach-Object { $_.Groups['agent'].Value + '|' + $_.Groups['keyword'].Value })
$expectedRules = @(
    'precommit-audit|commit','precommit-audit|precommit',
    'precommit-audit|pull request','precommit-audit|pr ',
    'precommit-audit|ok_to_commit','security|security','security|auth',
    'security|secret','security|injection','debug|debug','debug|crash',
    'debug|traceback','debug|exception','debug|failing','test|test',
    'test|ctest','test|coverage','docs|docs','docs|readme','docs|pdf',
    'docs|manual','docs|handbook','docs|runbook','docs|documentation',
    'research|research','research|web search','research|http','review|review',
    'review|critique','plan|plan','plan|design','plan|architecture',
    'explore|explore','explore|map','explore|codebase','explore|structure',
    'explore|overview','explore|unfamiliar','implement|implement',
    'implement|feature','implement|bugfix','implement|write code','implement|edit')
Assert-Sequence $actualRules $expectedRules `
    'exact source-ordered Windows-native recommendation rules'

foreach ($resourceName in $resourceNames) {
    $relativePath = 'src/ForgeConductor.Application/Resources/Agents/' +
        $resourceName
    $text = $textByPath[$relativePath]
    $expectedId = [IO.Path]::GetFileNameWithoutExtension($resourceName)
    Assert-Exact ([regex]::Matches(
        $text,
        '^id:\s*' + [regex]::Escape($expectedId) + '\r?$',
        [Text.RegularExpressions.RegexOptions]::Multiline).Count) 1 `
        "$resourceName exact frontmatter id"
    $toolsMatch = [regex]::Match(
        $text,
        '^tools:\s*\[(?<tools>[^\]]*)\]\r?$',
        [Text.RegularExpressions.RegexOptions]::Multiline)
    Assert-True $toolsMatch.Success "$resourceName flat allowed-tool list"
    $resourceTools = @($toolsMatch.Groups['tools'].Value.Split(',') |
        ForEach-Object { $_.Trim() } | Where-Object { $_ })
    Assert-Exact @($resourceTools | Sort-Object -Unique).Count `
        $resourceTools.Count "$resourceName unique allowed-tool list"
    foreach ($tool in $resourceTools) {
        Assert-True ($tool -cin $catalogTools) `
            "$resourceName allowed tool is canonical: $tool"
    }
    Assert-Match $text '\bagent_run_complete\b' `
        "$resourceName completion requirement" -CaseSensitive
}

$agentContract = $textByPath['include/ForgeConductor/Contracts/IAgentServices.h']
$inspectorPublicHeader = $textByPath[
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h']
$inspectorSource = $textByPath[
    'src/Infrastructure/Windows/WindowsAgentCompletionReportInspector.cpp']
$inspectorContractBody = Get-ClassBody $agentContract `
    'IAgentCompletionReportInspector' `
    'IAgentCompletionReportInspector class block'
Assert-Exact ([regex]::Matches(
    $inspectorContractBody,
    '\binspect\s*\(').Count) 1 `
    'IAgentCompletionReportInspector exposes exactly one inspect operation'
Assert-Match $inspectorContractBody `
    'Domain::Result<std::vector<Domain::AgentReportField>>\s+inspect\s*\(\s*std::string_view\s+canonicalJson\s*,\s*const Domain::OperationContext&\s+context\s*\)\s*noexcept\s*=\s*0\s*;' `
    'completion-report inspector exact transport-neutral contract' -CaseSensitive
Assert-Match $inspectorPublicHeader `
    'std::unique_ptr<Contracts::IAgentCompletionReportInspector>\s+createWindowsAgentCompletionReportInspector\s*\(\s*Contracts::IClock&\s+clock\s*\)\s*;' `
    'Windows infrastructure exposes only the abstract inspector factory' `
    -CaseSensitive
Assert-NoMatch $inspectorPublicHeader `
    '\bclass\s+WindowsAgentCompletionReportInspector\b' `
    'private Windows inspector concrete type is absent from public headers' `
    -CaseSensitive
Assert-Match $inspectorSource `
    'namespace\s*\{.*?class\s+WindowsAgentCompletionReportInspector\s+final\s*:\s*public\s+Contracts::IAgentCompletionReportInspector' `
    'Windows completion-report inspector is final and translation-unit private' `
    -CaseSensitive
foreach ($deletedMemberPattern in @(
    'WindowsAgentCompletionReportInspector\s*\(\s*const WindowsAgentCompletionReportInspector&\s*\)\s*=\s*delete\s*;',
    'WindowsAgentCompletionReportInspector&\s+operator=\s*\(\s*const WindowsAgentCompletionReportInspector&\s*\)\s*=\s*delete\s*;',
    'WindowsAgentCompletionReportInspector\s*\(\s*WindowsAgentCompletionReportInspector&&\s*\)\s*=\s*delete\s*;',
    'WindowsAgentCompletionReportInspector&\s+operator=\s*\(\s*WindowsAgentCompletionReportInspector&&\s*\)\s*=\s*delete\s*;')) {
    Assert-Exact ([regex]::Matches(
        $inspectorSource,
        $deletedMemberPattern).Count) 1 `
        "Windows completion-report inspector exact deleted member $deletedMemberPattern"
}
Assert-Exact ([regex]::Matches(
    $inspectorSource,
    '#include\s+<nlohmann/json[.]hpp>').Count) 1 `
    'private Windows inspector has the one approved JSON implementation include'
Assert-Exact ([regex]::Matches(
    $inspectorSource,
    'Detail::validateOperationContext\s*\(').Count) 2 `
    'inspector checks cancellation and deadline before and after parsing'
Assert-Match $inspectorSource `
    'canonicalJson[.]empty\(\).*?MaximumReportJsonBytes.*?canonicalJson[.]find\(''\\0''\).*?!Domain::isValidUtf8\(canonicalJson\)' `
    'inspector rejects empty oversized NUL and invalid UTF-8 reports' `
    -CaseSensitive
Assert-Match $inspectorSource `
    'std::vector<std::set<std::string>>\s+objectKeys;.*?depth\s*<\s*0\s*\|\|\s*depth\s*>\s*64.*?parse_event_t::object_start.*?objectKeys[.]emplace_back\(\).*?parse_event_t::key.*?objectKeys[.]back\(\)[.]insert\(.*?[.]second.*?parse_event_t::object_end.*?objectKeys[.]pop_back\(\)' `
    'inspector detects duplicate keys at every object depth and caps depth at 64' `
    -CaseSensitive
Assert-Match $inspectorSource `
    'Json::parse\(\s*canonicalJson\s*,\s*callback\s*,\s*false\s*,\s*false\s*\).*?invalidStructure.*?!objectKeys[.]empty\(\).*?document[.]is_discarded\(\).*?!document[.]is_object\(\).*?MaximumReportFields' `
    'inspector parses without exceptions and requires one bounded top-level object' `
    -CaseSensitive
Assert-Match $inspectorSource `
    'document[.]dump\(\s*-1\s*,\s*'' ''\s*,\s*false\s*,\s*Json::error_handler_t::strict\s*\).*?encoded\s*!=\s*canonicalJson' `
    'inspector enforces exact strict canonical JSON encoding' -CaseSensitive
Assert-MarkerOrder $inspectorSource @(
    'for (auto iterator = document.cbegin();',
    'iterator.key().empty()',
    'iterator->is_string()',
    'iterator->is_array()',
    'iterator->is_object()',
    'iterator->is_boolean()',
    'iterator->is_number()',
    'fields.push_back(Domain::AgentReportField{') `
    'inspector independently derives every top-level field kind and logical size'
Assert-Match $inspectorSource `
    'return\s+std::make_unique<WindowsAgentCompletionReportInspector>\(clock\);' `
    'Windows factory returns the private concrete inspector behind its interface' `
    -CaseSensitive
$p10003Text = $textByPath[
    '.forge-codex/state/decisions/P10-003-agent-service-semantics-and-p14-boundary.md']
Assert-MarkerOrder $p10003Text @(
    'Completion never trusts caller-supplied field metadata',
    'IAgentCompletionReportInspector',
    'strict canonical JSON',
    'nlohmann/json dependency below the Application',
    'rejects duplicate object keys at every depth',
    'caps JSON nesting at',
    '64.',
    'P14 still owns wire parsing and schemas',
    'trust-boundary invariant check before durable mutation') `
    'P10-003 records the report-inspection trust boundary and dependency resolution'
Assert-MarkerOrder $p10003Text @(
    'canonical call must contain exactly the requested',
    'A retained P05 run without a project is compatible only when it has a',
    'durable working directory accepted by the injected workspace-authority path',
    'resolver under an immutable trusted root',
    'a lexical prefix check is not',
    'Windows reparse points can redirect a path',
    'Projectless',
    'and pathless runs cannot be transferred') `
    'P10-003 requires injected path resolution and rejects unscoped legacy transfer'

$serviceHeader = $textByPath[
    'include/ForgeConductor/Application/AgentSessionService.h']
$serviceSource = $textByPath['src/Application/AgentSessionService.cpp']
Assert-Match $serviceHeader `
    'class\s+AgentSessionService\s+final\s*:\s*public\s+Contracts::IAgentSessionService' `
    'AgentSessionService is a final IAgentSessionService implementation' `
    -CaseSensitive
Assert-Match $serviceHeader `
    'AgentSessionService\s*\(\s*Contracts::IAgentCatalog&\s+catalog\s*,\s*Contracts::IAgentSessionRepository&\s+repository\s*,\s*Contracts::IAgentCompletionReportInspector&\s+reportInspector\s*,\s*Contracts::IWorkspaceAuthority&\s+workspaceAuthority\s*,\s*Contracts::IClock&\s+clock\s*,\s*Contracts::IUuidGenerator&\s+uuidGenerator' `
    'AgentSessionService constructor injects the report inspector and workspace authority' `
    -CaseSensitive
foreach ($deletedMember in @(
    'AgentSessionService(const AgentSessionService&) = delete;',
    'AgentSessionService& operator=(const AgentSessionService&) = delete;',
    'AgentSessionService(AgentSessionService&&) = delete;',
    'AgentSessionService& operator=(AgentSessionService&&) = delete;')) {
    Assert-Exact ([regex]::Matches(
        $serviceHeader,
        [regex]::Escape($deletedMember)).Count) 1 `
        "AgentSessionService exact deleted member $deletedMember"
}
$serviceInterfaceBody = Get-ClassBody $agentContract 'IAgentSessionService' `
    'IAgentSessionService class block'
$serviceConcreteBody = Get-ClassBody $serviceHeader 'AgentSessionService' `
    'AgentSessionService class block'
Assert-Exact ([regex]::Matches($serviceInterfaceBody,'\bstatus\s*\(').Count) 1 `
    'IAgentSessionService has exactly one legacy status method'
Assert-Exact ([regex]::Matches($serviceConcreteBody,'\bstatus\s*\(').Count) 1 `
    'AgentSessionService has exactly one legacy status method'
Assert-Exact ([regex]::Matches(
    $serviceSource,
    'AgentSessionService::status\s*\(').Count) 1 `
    'AgentSessionService defines exactly one legacy status method'
Assert-Exact ([regex]::Matches($serviceInterfaceBody,'\brunStatus\s*\(').Count) 1 `
    'IAgentSessionService has exactly one rich runStatus method'
Assert-Match $serviceSource `
    'constexpr\s+std::string_view\s+StatusToolName\s*=\s*"agent_run_status"' `
    'status mutation binds the exact agent_run_status tool' -CaseSensitive
Assert-Match $serviceSource `
    'MaximumConsumedAuthorizations\s*=\s*256U' `
    'status replay tracking is bounded' -CaseSensitive
Assert-Match $serviceSource `
    'MaximumMemoryBindings' `
    'in-memory active-binding projection is bounded' -CaseSensitive
Assert-Exact ([regex]::Matches(
    $serviceSource,
    'Contracts::IAgentCompletionReportInspector&\s+reportInspector_;').Count) 1 `
    'AgentSessionService stores exactly one non-owning abstract report inspector'
Assert-Exact ([regex]::Matches(
    $serviceSource,
    'Contracts::IWorkspaceAuthority&\s+workspaceAuthority_;').Count) 1 `
    'AgentSessionService stores exactly one non-owning workspace authority'
Assert-Exact ([regex]::Matches(
    $serviceSource,
    'validateTargetAuthority\s*\(').Count) 4 `
    'workspace target validation has one definition and exactly three operation calls'
$targetAuthoritySlice = Get-SourceSlice $serviceSource `
    'validateTargetAuthority(' `
    'void appendJsonString(' `
    'AgentSessionService durable target-authority validation'
Assert-MarkerOrder $targetAuthoritySlice @(
    'if (run.projectId &&',
    '!authorization.matchesProject(run.projectId.value())',
    'if (!run.projectId && !run.workingDirectory)',
    'A legacy projectless agent run requires an authorized durable working directory.',
    'if (run.workingDirectory)',
    'workspaceAuthority.authorize(',
    '*run.workingDirectory',
    'std::nullopt',
    'Domain::FileAccess::Write',
    'false}',
    'error.code != Domain::ErrorCodes::Unauthorized',
    'error.code != Domain::ErrorCodes::PathOutsideAuthority',
    'ErrorCodes::PathOutsideAuthority',
    'path.authorityId() != authority.authorityId()',
    'path.access() != Domain::FileAccess::Write',
    'authority.trustedRoots().begin()',
    'path.authorityRoot()) == authority.trustedRoots().end()',
    'ErrorCodes::IntegrityFailure') `
    'service resolves every durable path through immutable write authority and fails closed'
$completionCoreSlice = Get-SourceSlice $serviceSource `
    'completeRunCore(' `
    '[[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attach(' `
    'AgentSessionService completion core'
Assert-MarkerOrder $completionCoreSlice @(
    'reportInspector_.inspect(',
    'Domain::AgentCompletionReport verifiedReport{',
    'Domain::validateAgentCompletionReport(verifiedReport)',
    'sameReportFields(request.report.fields, verifiedReport.fields)',
    'Domain::makeAgentCompletionSummary(',
    'repository_.completeRun(',
    'verifiedReport.canonicalJson') `
    'completion verifies independently derived fields before durable mutation'
Assert-Match $serviceSource `
    'std::condition_variable\s+lifecycleChanged_;' `
    'AgentSessionService owns its lifecycle condition variable' -CaseSensitive
Assert-Match $serviceSource `
    'lifecycleChanged_[.]wait\s*\(\s*lock\s*,\s*\[&\]\(\)\s*\{\s*return\s+activeOperations_\s*==\s*0U;\s*\}\s*\);' `
    'shutdown waits for every already-admitted operation to drain' -CaseSensitive
Assert-Match $serviceSource `
    'accepting_\s*=\s*false;.*?activeOperations_\s*==\s*0U;.*?repository_[.]close\s*\(\s*\);.*?shutdownComplete_[.]store\s*\(\s*true\s*,\s*std::memory_order_release\s*\);.*?lifecycleChanged_[.]notify_all\s*\(\s*\);' `
    'shutdown closes admission before drain and publishes completion after close' `
    -CaseSensitive
Assert-NoMatch $serviceText `
    '(?i)\b(?:json[-_]?rpc|toolregistry|tooldispatcher|registeragenttools|toolschema|tooldescriptor)\b' `
    'P14 wire registration remains outside the P10 service slice'

$errorHeader = $textByPath['include/ForgeConductor/Domain/Error.h']
$stableErrors = [ordered]@{
    AgentNotFound = 'agent_not_found'
    SessionNotFound = 'session_not_found'
    OwnershipConflict = 'ownership_conflict'
    Conflict = 'conflict'
    PathOutsideAuthority = 'path_outside_authority'
}
foreach ($errorPair in $stableErrors.GetEnumerator()) {
    Assert-Exact ([regex]::Matches(
        $errorHeader,
        'inline\s+constexpr\s+std::string_view\s+' +
            [regex]::Escape($errorPair.Key) + '\s*=\s*"' +
            [regex]::Escape($errorPair.Value) + '"\s*;').Count) 1 `
        "stable P10 error code $($errorPair.Key)"
}
foreach ($errorName in @(
    'AgentNotFound','SessionNotFound','OwnershipConflict','Conflict',
    'PathOutsideAuthority')) {
    Assert-Match ($serviceText + [Environment]::NewLine + $repositoryText +
        [Environment]::NewLine + $testText) `
        ('ErrorCodes::' + $errorName + '\b') `
        "P10 implementation or tests exercise stable error $errorName" `
        -CaseSensitive
}

$repositoryHeader = $textByPath[
    'include/ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h']
$repositorySource = $textByPath[
    'src/Persistence/Windows/WindowsAgentSessionRepository.cpp']
Assert-Match $repositoryHeader `
    'class\s+WindowsAgentSessionRepository\s+final\s*:\s*public\s+Contracts::IAgentSessionRepository' `
    'WindowsAgentSessionRepository is final' -CaseSensitive
foreach ($deletedMember in @(
    'WindowsAgentSessionRepository(const WindowsAgentSessionRepository&) = delete;',
    'WindowsAgentSessionRepository(WindowsAgentSessionRepository&&) = delete;')) {
    Assert-Match $repositoryHeader ([regex]::Escape($deletedMember)) `
        "WindowsAgentSessionRepository deleted member $deletedMember" `
        -CaseSensitive
}
Assert-Match $repositoryHeader `
    'enum\s+class\s+AgentSessionTransactionCheckpoint\s*\{\s*BeforeCommit\s*,\s*AfterCommit\s*\}' `
    'test seam exposes exact before/after commit checkpoints' -CaseSensitive
Assert-Match $repositoryHeader `
    'class\s+IAgentSessionTransactionObserver\b' `
    'process-crash observer is an injected test seam' -CaseSensitive
Assert-NoMatch $repositorySource `
    '(?i)\b(?:getenv|_wgetenv|GetEnvironmentVariable|SetEnvironmentVariable)\b' `
    'production repository has no environment-controlled crash switch'
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'WinsqliteTransaction::beginImmediate\(').Count) 6 `
    'exact six serialized repository mutation transactions'
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'AgentSessionTransactionCheckpoint::BeforeCommit').Count) 5 `
    'exact five observable pre-commit checkpoints'
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'AgentSessionTransactionCheckpoint::AfterCommit').Count) 5 `
    'exact five observable post-commit checkpoints'

$startSlice = Get-SourceSlice $repositorySource `
    'WindowsAgentSessionRepository::startRun(' `
    'WindowsAgentSessionRepository::getRun(' `
    'atomic start transaction'
Assert-MarkerOrder $startSlice @(
    'WinsqliteTransaction::beginImmediate',
    'closeClientRuns(',
    'INSERT INTO agent_sessions(',
    'writeRunProjection(',
    'writeActiveProjection(',
    'AgentSessionTransactionCheckpoint::BeforeCommit',
    'transaction.commit()',
    'AgentSessionTransactionCheckpoint::AfterCommit') `
    'atomic start row and projection transaction'

$reattachSlice = Get-SourceSlice $repositorySource `
    'WindowsAgentSessionRepository::reattachRun(' `
    'WindowsAgentSessionRepository::completeRun(' `
    'ownership-transfer transaction'
Assert-MarkerOrder $reattachSlice @(
    'WinsqliteTransaction::beginImmediate',
    'existing->session.clientId != mutation.expectedClientId',
    'closeClientRuns(',
    'UPDATE agent_sessions SET client_id=?,updated_at=?',
    'changes(transaction, context) != 1',
    'deleteMatchingActiveProjection(',
    'writeActiveProjection(',
    'AgentSessionTransactionCheckpoint::BeforeCommit',
    'transaction.commit()',
    'AgentSessionTransactionCheckpoint::AfterCommit') `
    'ownership transfer compare-and-swap transaction'
Assert-Match $reattachSlice `
    'WHERE id=\? AND client_id=\? AND.*status IN' `
    'reattach CAS binds the expected durable owner'
Assert-Match $reattachSlice `
    'WHERE id=\? AND client_id IS NULL AND.*status IN' `
    'reattach CAS supports an explicitly unowned durable run'

$completeSlice = Get-SourceSlice $repositorySource `
    'WindowsAgentSessionRepository::completeRun(' `
    'WindowsAgentSessionRepository::touchRun(' `
    'atomic completion transaction'
Assert-MarkerOrder $completeSlice @(
    'WinsqliteTransaction::beginImmediate',
    'existing->session.clientId != mutation.expectedClientId',
    "status='closed',summary=?,report_json=?,updated_at=?",
    'changes(transaction, context) != 1',
    'deleteMatchingActiveProjection(',
    'writeRunProjection(',
    'AgentSessionTransactionCheckpoint::BeforeCommit',
    'transaction.commit()',
    'AgentSessionTransactionCheckpoint::AfterCommit') `
    'completion row report and projection transaction'
Assert-Match $completeSlice `
    "WHERE id=\? AND.*status IN \('open','active','running','started'\)" `
    'completion CAS accepts only open-compatible states'
Assert-NoMatch $completeSlice `
    'existing->session[.]updatedAt\s*==\s*mutation[.]completedAt' `
    'idempotent completion retry does not require a repeated wall-clock value' `
    -CaseSensitive
Assert-Match $repositorySource `
    'Older projections may contain a report copy[.] Validate but ignore it:.*?central-v6 report_json is authoritative' `
    'central-v6 report column remains authoritative without projection duplication' `
    -CaseSensitive
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'ORDER BY julianday\((?:updated_at|created_at)\) DESC').Count) 4 `
    'exact four newest-first chronological SQLite ordering anchors'
Assert-Exact ([regex]::Matches(
    $repositorySource,
    'ORDER BY julianday\(updated_at\) ASC').Count) 1 `
    'exact oldest-first chronological stale-selection ordering anchor'
Assert-Match $repositorySource `
    'julianday\(updated_at\)<julianday\(\?\).*?ORDER BY julianday\(updated_at\) ASC,id ASC LIMIT \?' `
    'stale selection compares and orders timestamps chronologically'
Assert-Match $repositorySource `
    "WHERE id=\? AND julianday\(updated_at\)<julianday\(\?\) AND.*?status IN \('open','active','running','started'\)" `
    'stale close CAS repeats the chronological cutoff and open-state predicate'

$catalogTestText = $textByPath['tests/Agents/AgentCatalogTests.cpp']
$catalogCases = @([regex]::Matches(
    $catalogTestText,
    '^void\s+(?<name>test[A-Za-z0-9_]+)\s*\(',
    [Text.RegularExpressions.RegexOptions]::Multiline) |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Set $catalogCases @(
    'testEmbeddedFallbacksAndResourcesAreEquivalent',
    'testSourceCompatibleParserAndValidation',
    'testParserStructuralBounds',
    'testDeterministicOverridesAndMalformedSkip',
    'testCapacityRetainsMandatoryDefinitions',
    'testRecommendationOrderAndBounds',
    'testCancellationDeadlineAndAtomicSnapshots') `
    'exact AgentCatalog native test-case inventory'
Assert-Match $catalogTestText `
    'argumentCount\s*!=\s*2.*Usage:\s*AgentCatalogTests\s*<Agents-resource-directory>' `
    'AgentCatalog test requires one explicit staged-resource argument' `
    -CaseSensitive

$processFixtureText = $textByPath[
    'tests/Agents/AgentSessionProcessFixture.cpp']
Assert-Match $processFixtureText '#include\s+<Windows[.]h>' `
    'process fixture uses the native Windows process surface' -CaseSensitive
Assert-Match $processFixtureText `
    'class\s+BlockingTransactionObserver\s+final' `
    'process fixture blocks at an injected transaction checkpoint' -CaseSensitive
Assert-Match $processFixtureText `
    'WaitForSingleObject\([^,]+,\s*(?:30|60)[^)]*\)' `
    'process fixture waits with an explicit deadline'
$fixtureModes = @([regex]::Matches(
    $processFixtureText,
    'mode\s*==\s*L"(?<mode>--[a-z-]+)"') |
    ForEach-Object { $_.Groups['mode'].Value })
Assert-Sequence $fixtureModes @(
    '--crash-start-before-commit',
    '--crash-start-after-commit',
    '--crash-complete-before-commit',
    '--crash-complete-after-commit',
    '--crash-reattach-before-commit',
    '--crash-reattach-after-commit',
    '--reattach') 'exact process-fixture mode inventory'

$cmakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$cmake = Get-Content -Raw -LiteralPath $cmakePath
$applicationBody = Get-CMakeInvocationBody $cmake 'set' `
    'FORGE_APPLICATION_SOURCES' 'FORGE_APPLICATION_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $applicationBody | Where-Object {
    $_ -match '^src/Application/Agent(?:Catalog|SessionService)[.]cpp$'
}) @(
    'src/Application/AgentCatalog.cpp',
    'src/Application/AgentSessionService.cpp') `
    'exact P10 Application production source placement'
$persistenceBody = Get-CMakeInvocationBody $cmake 'set' `
    'FORGE_PERSISTENCE_WINDOWS_SOURCES' `
    'FORGE_PERSISTENCE_WINDOWS_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $persistenceBody | Where-Object {
    $_ -match '^src/Persistence/Windows/.*Agent.*[.]cpp$'
}) @('src/Persistence/Windows/WindowsAgentSessionRepository.cpp') `
    'exact P10 persistence production source placement'
$infrastructureBody = Get-CMakeInvocationBody $cmake 'set' `
    'FORGE_INFRASTRUCTURE_WINDOWS_SOURCES' `
    'FORGE_INFRASTRUCTURE_WINDOWS_SOURCES declaration'
Assert-Set @(Get-CMakeTokens $infrastructureBody | Where-Object {
    $_ -match '^src/Infrastructure/Windows/.*AgentCompletionReport.*[.]cpp$'
}) @('src/Infrastructure/Windows/WindowsAgentCompletionReportInspector.cpp') `
    'exact private Windows completion-report inspector production placement'
foreach ($productionSource in @(
    'src/Application/AgentCatalog.cpp',
    'src/Application/AgentSessionService.cpp',
    'src/Persistence/Windows/WindowsAgentSessionRepository.cpp',
    'src/Infrastructure/Windows/WindowsAgentCompletionReportInspector.cpp')) {
    Assert-Exact ([regex]::Matches(
        $cmake,
        [regex]::Escape($productionSource)).Count) 1 `
        "CMake exact single production placement $productionSource"
}
Assert-Match $cmake `
    'forge_add_layer\s*\(\s*ForgeConductor[.]Application\s+ForgeConductor::Application\s+ForgeConductor::Contracts\s*\)' `
    'Application layer depends only on Contracts' -CaseSensitive
Assert-Match $cmake `
    'forge_add_layer\s*\(\s*ForgeConductor[.]Persistence[.]Windows\s+ForgeConductor::Persistence[.]Windows\s+ForgeConductor::Contracts\s+ForgeConductor::Infrastructure[.]Windows\s*\)' `
    'Windows persistence layer retains its approved dependencies' -CaseSensitive
$infrastructureLinks = Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Infrastructure.Windows' `
    'Windows infrastructure link inventory')
Assert-Exact @($infrastructureLinks | Where-Object {
    $_ -ceq 'nlohmann_json::nlohmann_json'
}).Count 1 'Windows infrastructure has exactly one approved JSON implementation link'

$resourceSetBody = Get-CMakeInvocationBody $cmake 'set' `
    'FORGE_AGENT_RESOURCE_FILES' 'FORGE_AGENT_RESOURCE_FILES declaration'
Assert-Sequence (Get-CMakeTokens $resourceSetBody) $resourceFiles `
    'CMake exact ten staged agent resources and order'
Assert-Match $cmake `
    'foreach\s*\(\s*_forge_agent_resource\s+IN\s+LISTS\s+FORGE_AGENT_RESOURCE_FILES\s*\).*?if\s*\(\s*NOT\s+EXISTS\s+"[$][{]PROJECT_SOURCE_DIR[}]/[$][{]_forge_agent_resource[}]"\s*\).*?message\s*\(\s*FATAL_ERROR.*?list\s*\(\s*APPEND\s+FORGE_AGENT_RESOURCE_PATHS' `
    'CMake fails closed while resolving mandatory resource paths' `
    -CaseSensitive
$resourceStage = [regex]::Match(
    $cmake,
    'add_custom_command\s*\(\s*TARGET\s+ForgeConductor[.]Agents[.]CatalogTests\s+POST_BUILD(?<body>.*?)\r?\n\s*\)',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $resourceStage.Success 'AgentCatalog post-build resource staging command'
Assert-Match $resourceStage.Groups['body'].Value `
    'COMMAND\s+"[$][{]CMAKE_COMMAND[}]"\s+-E\s+make_directory\s+"[$]<TARGET_FILE_DIR:ForgeConductor[.]Agents[.]CatalogTests>/Resources/Agents"' `
    'resource staging creates the exact target-relative directory' `
    -CaseSensitive
Assert-Match $resourceStage.Groups['body'].Value `
    'COMMAND\s+"[$][{]CMAKE_COMMAND[}]"\s+-E\s+copy_if_different\s+[$][{]FORGE_AGENT_RESOURCE_PATHS[}]\s+"[$]<TARGET_FILE_DIR:ForgeConductor[.]Agents[.]CatalogTests>/Resources/Agents"' `
    'resource staging copies the explicit bounded list' -CaseSensitive
Assert-Match $resourceStage.Groups['body'].Value `
    'COMMAND_EXPAND_LISTS' 'resource staging expands only the declared list' `
    -CaseSensitive

$expectedTargets = [ordered]@{
    'ForgeConductor.Agents.CatalogTests' = @(
        'tests/Agents/AgentCatalogTests.cpp')
    'ForgeConductor.Agents.SessionServiceTests' = @(
        'tests/Agents/AgentSessionServiceTests.cpp')
    'ForgeConductor.Agents.RepositoryWindowsTests' = @(
        'tests/Agents/AgentSessionRepositoryWindowsTests.cpp')
}
$expectedLabels = [ordered]@{
    'ForgeConductor.Agents.CatalogTests' = @('T-UNIT','T-AGENT','T-SEC','G10')
    'ForgeConductor.Agents.SessionServiceTests' = @(
        'T-UNIT','T-AGENT','T-STRESS','G10')
    'ForgeConductor.Agents.RepositoryWindowsTests' = @(
        'T-UNIT','T-DB','T-AGENT','T-SEC','G10')
}
$expectedTimeouts = [ordered]@{
    'ForgeConductor.Agents.CatalogTests' = 60
    'ForgeConductor.Agents.SessionServiceTests' = 120
    'ForgeConductor.Agents.RepositoryWindowsTests' = 180
}

foreach ($target in $expectedTargets.Keys) {
    Assert-Set (Get-CMakeExecutableSources $cmake $target) `
        $expectedTargets[$target] "$target exact CMake source inventory"
    $propertyBody = Get-CMakeInvocationBody $cmake 'set_tests_properties' `
        $target "$target CMake test properties"
    Assert-Match $propertyBody `
        ('LABELS\s+"' + [regex]::Escape(($expectedLabels[$target] -join ';')) + '"') `
        "$target exact CMake labels" -CaseSensitive
    Assert-Match $propertyBody `
        ('TIMEOUT\s+' + $expectedTimeouts[$target] + '\b') `
        "$target exact CMake timeout" -CaseSensitive
}

Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Agents.CatalogTests' `
    'catalog test link inventory')) `
    @('PRIVATE','ForgeConductor::Application') `
    'catalog test exact Application link layer'
Assert-Match $cmake `
    'forge_configure_standard_target\s*\(\s*ForgeConductor[.]Agents[.]CatalogTests\s*\)' `
    'catalog tests use the standard native C++ target policy' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Agents.SessionServiceTests' `
    'session service test include inventory')) `
    @('PRIVATE','${PROJECT_SOURCE_DIR}/tests') `
    'session service test exact include inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Agents.SessionServiceTests' `
    'session service test link inventory')) `
    @('PRIVATE','ForgeConductor::Application',
        'ForgeConductor::Infrastructure.Windows') `
    'session service test exact Application and Windows inspector link layers'
Assert-Match $cmake `
    'forge_configure_standard_target\s*\(\s*ForgeConductor[.]Agents[.]SessionServiceTests\s*\)' `
    'session service tests use the standard native C++ target policy' `
    -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Agents.RepositoryWindowsTests' `
    'repository test include inventory')) `
    @('PRIVATE','${PROJECT_SOURCE_DIR}/src','${PROJECT_SOURCE_DIR}/tests') `
    'repository test exact include inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Agents.RepositoryWindowsTests' `
    'repository test link inventory')) `
    @('PRIVATE','ForgeConductor::Persistence.Windows',
        'ForgeConductor::Infrastructure.Windows') `
    'repository test exact native link layers'
Assert-Match $cmake `
    'forge_configure_native_target\s*\(\s*ForgeConductor[.]Agents[.]RepositoryWindowsTests\s*\)' `
    'repository tests use the Windows native target policy' -CaseSensitive
Assert-Match $cmake `
    'add_test\s*\(\s*NAME\s+ForgeConductor[.]Agents[.]CatalogTests\s+COMMAND\s+[$]<TARGET_FILE:ForgeConductor[.]Agents[.]CatalogTests>\s+"[$]<TARGET_FILE_DIR:ForgeConductor[.]Agents[.]CatalogTests>/Resources/Agents"\s*\)' `
    'catalog CTest command has the exact staged-resource argument' `
    -CaseSensitive
Assert-Match $cmake `
    'add_test\s*\(\s*NAME\s+ForgeConductor[.]Agents[.]SessionServiceTests\s+COMMAND\s+[$]<TARGET_FILE:ForgeConductor[.]Agents[.]SessionServiceTests>\s*\)' `
    'session service CTest command has no undeclared arguments' -CaseSensitive
Assert-Match $cmake `
    'add_test\s*\(\s*NAME\s+ForgeConductor[.]Agents[.]RepositoryWindowsTests\s+COMMAND\s+[$]<TARGET_FILE:ForgeConductor[.]Agents[.]RepositoryWindowsTests>\s+[$]<TARGET_FILE:ForgeConductor[.]Agents[.]ProcessFixture>\s+"[$][{]PROJECT_SOURCE_DIR[}]/tests/Persistence/Fixtures"\s*\)' `
    'repository CTest command has the exact process and fixture arguments' `
    -CaseSensitive

Assert-Set (Get-CMakeExecutableSources $cmake `
    'ForgeConductor.Agents.ProcessFixture') `
    @('tests/Agents/AgentSessionProcessFixture.cpp') `
    'process fixture exact CMake source inventory'
Assert-Match $cmake `
    'forge_configure_native_target\s*\(\s*ForgeConductor[.]Agents[.]ProcessFixture\s*\)' `
    'process fixture uses the native target policy' -CaseSensitive
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_include_directories' 'ForgeConductor.Agents.ProcessFixture' `
    'process fixture include inventory')) `
    @('PRIVATE','${PROJECT_SOURCE_DIR}/src','${PROJECT_SOURCE_DIR}/tests') `
    'process fixture exact include inventory'
Assert-Sequence (Get-CMakeTokens (Get-CMakeInvocationBody $cmake `
    'target_link_libraries' 'ForgeConductor.Agents.ProcessFixture' `
    'process fixture link inventory')) `
    @('PRIVATE','ForgeConductor::Persistence.Windows',
        'ForgeConductor::Infrastructure.Windows') `
    'process fixture exact native link layers'
Assert-NoMatch $cmake `
    'add_test\s*\(\s*NAME\s+ForgeConductor[.]Agents[.]ProcessFixture\b' `
    'process fixture is not independently registered with CTest' -CaseSensitive
Assert-Match $cmake `
    'add_dependencies\s*\(\s*ForgeConductor[.]Agents[.]RepositoryWindowsTests\s+ForgeConductor[.]Agents[.]ProcessFixture\s*\)' `
    'repository tests have an explicit process-fixture build dependency' `
    -CaseSensitive

$g10PropertyTargets = @([regex]::Matches(
    $cmake,
    'set_tests_properties\s*\(\s*(?<target>[^\s\)]+)\s+PROPERTIES(?<body>.*?)\)',
    [Text.RegularExpressions.RegexOptions]::Singleline) | Where-Object {
        $_.Groups['body'].Value -match '(?:^|;)G10(?:;|"|\s)'
    } | ForEach-Object { $_.Groups['target'].Value })
Assert-Set $g10PropertyTargets @($expectedTargets.Keys) `
    'exact three statically registered G10 CTest targets'

$sessionTestText = $textByPath['tests/Agents/AgentSessionServiceTests.cpp']
$repositoryTestText = $textByPath[
    'tests/Agents/AgentSessionRepositoryWindowsTests.cpp']
$runProjectionHelperSlice = Get-SourceSlice $repositoryTestText `
    'void requireRunProjectionMatches(' `
    'void requireRunProjectionAbsent(' `
    'run projection assertion helper'
Assert-MarkerOrder $runProjectionHelperSlice @(
    '"session_id"',
    '"agent_id"',
    '"status"',
    '"project_id"',
    '"goal"',
    '"cwd"',
    '"output_schema"',
    '"first_moves"',
    'projection.find("\"report\":") == std::string::npos') `
    'run projection helper validates durable content without report duplication'
$activeProjectionHelperSlice = Get-SourceSlice $repositoryTestText `
    'void requireActiveProjectionMatches(' `
    'void requireActiveProjectionAbsent(' `
    'active projection assertion helper'
Assert-MarkerOrder $activeProjectionHelperSlice @(
    '"session_id"',
    '"agent_id"',
    '"goal"',
    '"tools_primary"',
    '"tools_forbidden"',
    '"output_schema"',
    '"done_definition"',
    '"cwd"') `
    'active projection helper validates the complete durable binding content'
$repeatedRecoverySlice = Get-SourceSlice $repositoryTestText `
    'void verifyRepeatedRecoveryIsStableAndNonMutating(' `
    '[[nodiscard]] std::wstring quoteArgument(' `
    'repeated recovery stability test helper'
Assert-MarkerOrder $repeatedRecoverySlice @(
    'constexpr std::size_t RepeatedRecoveryAttemptCount = 64U;',
    'attempt < RepeatedRecoveryAttemptCount',
    'fixture.repository->recoverRun(',
    'recovered.usedActiveProjection',
    '!recovered.projectionNeedsRepair',
    'run.reportJson == expectedRun.reportJson',
    'binding.doneDefinition == expectedBinding.doneDefinition',
    'projectionBody(database, activeKey) == activeBodyBefore',
    'projectionBody(database, runKey) == runBodyBefore',
    'repeated recovery mutated durable row or projection state',
    'quickCheck(') `
    'exact 64 repeated recoveries preserve rows projections metadata and timestamps'
Assert-Exact ([regex]::Matches(
    $repositoryTestText,
    'verifyRepeatedRecoveryIsStableAndNonMutating\s*\(').Count) 2 `
    'repeated recovery helper has exactly one definition and one exercised call'
$repositoryBoundsSlice = Get-SourceSlice $repositoryTestText `
    'void staleCloseCancellationDeadlineAndSharedShutdown()' `
    'void crashRollbackAndCommittedRecovery(' `
    'repository cancellation deadline busy and shutdown test'
Assert-MarkerOrder $repositoryBoundsSlice @(
    'PRAGMA busy_timeout=0; BEGIN IMMEDIATE;',
    'const auto busyStarted = std::chrono::steady_clock::now();',
    'fixture.repository->touchRun(',
    'const auto busyElapsed = std::chrono::steady_clock::now() - busyStarted;',
    'busy.error().code == Domain::ErrorCodes::DatabaseBusy',
    'busyElapsed >= 2500ms && busyElapsed < 4500ms',
    'writeLock.execute("ROLLBACK;")',
    'p10-database-busy-recovered') `
    'actual competing write lock maps database_busy inside the 2.5 to 4.5 second bound'
$sessionCaseNames = @([regex]::Matches(
    $sessionTestText,
    '\{\s*"(?<name>[a-z0-9_]+)"\s*,\s*[A-Za-z][A-Za-z0-9_]*\s*\}') |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Set $sessionCaseNames @(
    'start_prunes_before_atomic_commit_and_uses_stable_errors',
    'status_uses_observed_idle_and_authorization_is_globally_one_use',
    'completion_uses_durable_schema_and_evicts_persisted_owner',
    'rehydrate_repairs_only_when_durable_goal_exists',
    'prune_uses_strict_cutoff_and_evicts_only_closed_bindings',
    'binding_cache_is_bounded_and_deterministically_evicted',
    'concurrent_starts_cannot_reverse_committed_cache_order',
    'admitted_legacy_completion_drains_through_shutdown',
    'cancellation_deadline_and_shutdown_own_their_boundaries') `
    'exact AgentSessionService native test-case inventory'
Assert-Exact $sessionCaseNames.Count 9 `
    'AgentSessionService has exactly nine registered native cases'
Assert-Match $sessionTestText `
    'std::cout\s*<<\s*"SUMMARY passed=".*?return\s+failures\s*==\s*0U' `
    'AgentSessionService test main has a fail-closed dynamic summary'
Assert-Exact ([regex]::Matches(
    $sessionTestText,
    'std::cout\s*<<\s*"PASS\s+"').Count) 1 `
    'AgentSessionService prints one PASS line per registry case'
Assert-Exact ([regex]::Matches(
    $sessionTestText,
    'createWindowsAgentCompletionReportInspector\s*\(').Count) 1 `
    'service tests use exactly one real private Windows report-inspector factory'
$workspaceAuthorityTestSlice = Get-SourceSlice $sessionTestText `
    'class AgentWorkspaceAuthority final' `
    'struct Fixture final {' `
    'service-test workspace-authority fake'
Assert-MarkerOrder $workspaceAuthorityTestSlice @(
    'public Contracts::IWorkspaceAuthority',
    'Domain::Result<Contracts::AuthorizedPath> authorize(',
    '++authorizeCalls_',
    'if (rejectNextResolvedPath_)',
    'ErrorCodes::PathOutsideAuthority',
    'return delegate_.authorize(authority, request, context)',
    'void rejectNextResolvedPath() noexcept',
    'std::size_t authorizeCalls() const noexcept',
    'Fakes::DeterministicWorkspaceAuthority delegate_') `
    'service tests own an injectable resolver that can model a reparse escape'
$serviceFixtureSlice = Get-SourceSlice $sessionTestText `
    'struct Fixture final {' `
    'void startPrunesBeforeAtomicCommitAndUsesStableErrors()' `
    'AgentSessionService native fixture'
Assert-MarkerOrder $serviceFixtureSlice @(
    'createWindowsAgentCompletionReportInspector(clock)',
    'AgentWorkspaceAuthority workspaceAuthority;',
    'Application::AgentSessionService service{',
    '*reportInspector,',
    'workspaceAuthority,',
    'clock,',
    'uuids,') `
    'service-test fixture injects the real inspector and controlled workspace authority'
$statusAuthorityTestSlice = Get-SourceSlice $sessionTestText `
    'void statusUsesObservedIdleAndAuthorizationIsGloballyOneUse()' `
    'void completionUsesDurableSchemaAndEvictsPersistedOwner()' `
    'status workspace-authority native regression'
Assert-MarkerOrder $statusAuthorityTestSlice @(
    'escapedRun.workingDirectory = path("C:/workspace-escape")',
    'const auto escaped = fixture.service.runStatus(',
    'escaped.error().code == Domain::ErrorCodes::PathOutsideAuthority',
    'fixture.repository.snapshot(escapedRun.session.id)',
    'reparseRun.workingDirectory = path("C:/workspace/junction/elsewhere")',
    '"status-reparse-escape"',
    'const auto authorizationCalls = fixture.workspaceAuthority.authorizeCalls();',
    'fixture.workspaceAuthority.rejectNextResolvedPath();',
    'const auto reparseEscape = fixture.service.attach(',
    'reparseEscape.error().code ==',
    'authorizationCalls + 1U',
    'fixture.repository.snapshot(reparseRun.session.id)',
    '"retained P05 projectless start"',
    'path("C:/workspace/legacy")',
    'REQUIRE(legacyProjectless)',
    '"unscoped retained P05 start"',
    '"legacy-unscoped-start"',
    'REQUIRE(!unscopedStatus)',
    'unscopedStatus.error().code == Domain::ErrorCodes::Unauthorized',
    'fixture.repository.snapshot(unscopedLegacy.id)') `
    'status tests reject lexical and reparse escapes without mutation and scope legacy rows'
Assert-Match $statusAuthorityTestSlice `
    'snapshot\(escapedRun[.]session[.]id\).*?session[.]clientId\s*==\s*oldOwner.*?reparseEscape[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::PathOutsideAuthority.*?snapshot\(reparseRun[.]session[.]id\).*?session[.]clientId\s*==\s*oldOwner' `
    'lexical and resolved-path escapes preserve durable ownership' `
    -CaseSensitive
Assert-Match $statusAuthorityTestSlice `
    '"unscoped retained P05 start"\s*,\s*std::nullopt.*?unscopedStatus[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::Unauthorized.*?snapshot\(unscopedLegacy[.]id\).*?session[.]clientId\s*==\s*oldOwner' `
    'projectless pathless legacy runs are rejected without ownership mutation' `
    -CaseSensitive
$completionServiceTestSlice = Get-SourceSlice $sessionTestText `
    'void completionUsesDurableSchemaAndEvictsPersistedOwner()' `
    'void rehydrateRepairsOnlyWhenDurableGoalExists()' `
    'completion service trust-boundary native test'
Assert-MarkerOrder $completionServiceTestSlice @(
    'const Domain::AgentCompletionReport report{',
    'fixture.service.completeRun(',
    'completed.value().report.fields.size() == 6U',
    'completed.value().report.fields[0].key == "details"',
    'completed.value().report.fields[5].key == "number"',
    'const auto assertRejectedReport =',
    'ErrorCodes::InvalidRequest',
    '!durableRun->reportJson',
    '"forged-report-metadata"',
    '"malformed-report-json"',
    '"duplicate-report-key"',
    '"nested-duplicate-report-key"',
    'overNestedReport.append(66U, ''['')',
    'overNestedReport.append(66U, '']'')',
    '"report-nesting-over-64"',
    '"noncanonical-report-json"') `
    'real inspector derives ordered fields and rejects hostile reports before mutation'
Assert-Match $sessionTestText `
    'void\s+admittedLegacyCompletionDrainsThroughShutdown\s*\(\s*\).*?blockNextGetRun\s*\(\s*\).*?service[.]complete\s*\(.*?service[.]shutdown\s*\(\s*\).*?ErrorCodes::Cancelled.*?releaseGetRun\s*\(\s*\).*?completionThread[.]join\s*\(\s*\).*?shutdownThread[.]join\s*\(\s*\).*?completion->value\s*\(\s*\)[.]status\s*==\s*Domain::SessionStatus::Closed.*?repository[.]closeCalled\s*\(\s*\)' `
    'shutdown stress case proves admitted legacy completion drains before close' `
    -CaseSensitive
$repositoryCaseNames = @([regex]::Matches(
    $repositoryTestText,
    '"PASS\s+(?<name>agent_session_repository[.][a-z0-9_]+)\\n"') |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Set $repositoryCaseNames @(
    'agent_session_repository.lifecycle_restart',
    'agent_session_repository.recovery_repair',
    'agent_session_repository.v5_hostile_rows',
    'agent_session_repository.bounds_shutdown',
    'agent_session_repository.timestamp_report_bounds',
    'agent_session_repository.crash_atomicity',
    'agent_session_repository.completion_crash_retry',
    'agent_session_repository.cross_process_cas') `
    'exact repository Windows native test-case inventory'
Assert-Match $repositoryTestText `
    '"SUMMARY passed=8 failed=0\\n"' `
    'repository test main has the exact fail-closed eight-case summary' `
    -CaseSensitive
Assert-Exact ([regex]::Matches(
    $repositoryTestText,
    '::TerminateProcess\s*\(').Count) 3 `
    'repository parent has exact start completion and reattach termination sites'
Assert-Match $repositoryTestText 'TerminateProcess\s*\(' `
    'parent repository tests use abrupt native process termination' `
    -CaseSensitive
Assert-Match $repositoryTestText 'quickCheck\s*\(' `
    'crash/reopen tests run database quick_check' -CaseSensitive
Assert-Match $processFixtureText `
    'attached[.]error\(\)[.]code\s*==\s*Domain::ErrorCodes::OwnershipConflict.*?ExitCode::OwnershipConflict' `
    'concurrent ownership fixture maps the typed losing result to its exact exit' `
    -CaseSensitive
foreach ($fixtureMode in @(
    '--crash-start-before-commit',
    '--crash-start-after-commit',
    '--crash-complete-before-commit',
    '--crash-complete-after-commit',
    '--crash-reattach-before-commit',
    '--crash-reattach-after-commit',
    '--reattach')) {
    Assert-Match $repositoryTestText ([regex]::Escape($fixtureMode)) `
        "repository parent exercises process-fixture mode $fixtureMode" `
        -CaseSensitive
}
$startCrashTestSlice = Get-SourceSlice $repositoryTestText `
    'void crashRollbackAndCommittedRecovery(' `
    'void completionCrashRetryIgnoresNewTimestamp(' `
    'start crash-boundary parent test'
Assert-Exact ([regex]::Matches(
    $startCrashTestSlice,
    'for\s*\(\s*const auto& testCase\s*:\s*cases\s*\)').Count) 1 `
    'start crash parent has one loop over its exact before and after cases'
Assert-Exact ([regex]::Matches(
    $startCrashTestSlice,
    '::TerminateProcess\s*\(').Count) 1 `
    'start crash case loop has one abrupt termination site'
Assert-MarkerOrder $startCrashTestSlice @(
    'L"--crash-start-before-commit"',
    'L"--crash-start-after-commit"',
    'p10-crash-predecessor-seed',
    '::TerminateProcess(',
    'quickCheck(',
    'run.has_value() == testCase.shouldCommit',
    'predecessor.has_value()',
    'const auto expectedSummary = Domain::makeAgentSupersedeSummary(',
    'pre-commit start crash changed its predecessor',
    'const auto expectedActiveSession = testCase.shouldCommit',
    'recovered.run->session.id == expectedActiveSession',
    'requireRunProjectionMatches(database, *predecessor)',
    'requireRunProjectionAbsent(database, sessionId)',
    'requireActiveProjectionMatches(database, clientId, *recovered.binding)') `
    'start parent proves predecessor row and projections are one crash-atomic unit'
$completionCrashTestSlice = Get-SourceSlice $repositoryTestText `
    'void completionCrashRetryIgnoresNewTimestamp(' `
    'void reattachCrashBeforeAndAfter(' `
    'completion crash-boundary parent test'
Assert-Exact ([regex]::Matches(
    $completionCrashTestSlice,
    'for\s*\(\s*const auto& testCase\s*:\s*cases\s*\)').Count) 1 `
    'completion crash parent has one loop over its exact before and after cases'
Assert-Exact ([regex]::Matches(
    $completionCrashTestSlice,
    '::TerminateProcess\s*\(').Count) 1 `
    'completion crash case loop has one abrupt termination site'
Assert-MarkerOrder $completionCrashTestSlice @(
    'L"--crash-complete-before-commit"',
    'L"--crash-complete-after-commit"',
    '::TerminateProcess(',
    'quickCheck(',
    'if (!testCase.shouldCommit)',
    'Domain::SessionStatus::Closed',
    'originalCompletionTime + 5min',
    'retried.run.session.updatedAt == originalCompletionTime',
    '!retried.activeProjectionCleared',
    'requireRunProjectionMatches(database, *durable)',
    'requireActiveProjectionMatches(',
    'requireActiveProjectionAbsent(database, clientId)',
    'completion crash active removal was not atomic') `
    'completion parent proves row report summary and projections at both crash boundaries'
$reattachCrashTestSlice = Get-SourceSlice $repositoryTestText `
    'void reattachCrashBeforeAndAfter(' `
    'void crossProcessReattachCas(' `
    'reattach crash-boundary parent test'
Assert-Exact ([regex]::Matches(
    $reattachCrashTestSlice,
    'for\s*\(\s*const auto& testCase\s*:\s*cases\s*\)').Count) 1 `
    'reattach crash parent has one loop over its exact before and after cases'
Assert-Exact ([regex]::Matches(
    $reattachCrashTestSlice,
    '::TerminateProcess\s*\(').Count) 1 `
    'reattach crash case loop has one abrupt termination site'
Assert-MarkerOrder $reattachCrashTestSlice @(
    'L"--crash-reattach-before-commit"',
    'L"--crash-reattach-after-commit"',
    'p10-reattach-crash-destination-seed',
    '::TerminateProcess(',
    'quickCheck(',
    'const auto expectedOwner = testCase.shouldCommit',
    'p10-reattach-destination-read',
    'post-commit reattach did not supersede the destination run',
    'pre-commit reattach changed the destination run',
    'const auto originalRecovery = take(',
    'const auto newRecovery = take(',
    'post-commit reattach did not replace the new active pointer',
    'pre-commit reattach changed the destination active pointer',
    'requireRunProjectionMatches(database, *durable)',
    'requireRunProjectionMatches(database, *destination)',
    'reattach crash left a partial active projection set') `
    'reattach parent proves destination supersede and both projection sets atomically'
$crossProcessCasSlice = Get-SourceSlice $repositoryTestText `
    'void crossProcessReattachCas(' `
    '} // namespace' `
    'cross-process ownership compare-and-swap test'
Assert-Match $crossProcessCasSlice `
    'std::count\(exits[.]begin\(\),\s*exits[.]end\(\),\s*0U\)\s*==\s*1.*?std::count\(exits[.]begin\(\),\s*exits[.]end\(\),\s*41U\)\s*==\s*1' `
    'cross-process CAS proves exactly one winner and one typed conflict' `
    -CaseSensitive
Assert-MarkerOrder $crossProcessCasSlice @(
    'const auto winningClient = *durable->session.clientId;',
    'const auto losingClient = parse<Domain::ClientId>(',
    'const auto winningRecovery = take(',
    'winningRecovery.run->session.id == sessionId',
    'winningRecovery.binding->sessionId == sessionId',
    'requireActiveProjectionMatches(',
    'requireActiveProjectionAbsent(database, originalClient)',
    'requireActiveProjectionAbsent(database, losingClient)',
    'cross-process CAS left duplicate active projections') `
    'cross-process CAS verifies one winning pointer body and no losing pointers'

if ($StaticOnly) {
    $frameworkAfter = Get-TreeSummary $frameworkRoot
    Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
        'sealed Forsetti file count after static G10 validation'
    Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
        'sealed Forsetti byte count after static G10 validation'
    Assert-Exact ([string]$frameworkAfter.sha256) `
        ([string]$frameworkBefore.sha256) `
        'sealed Forsetti hash after static G10 validation'
    Invoke-RepositoryIntegrityChecks
    Write-Host "G10 static agent validation passed: $script:AssertionCount fail-closed assertions."
    return
}

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
$g10BuildTargets = @(
    'ForgeConductor.Agents.CatalogTests',
    'ForgeConductor.Agents.SessionServiceTests',
    'ForgeConductor.Agents.RepositoryWindowsTests',
    'ForgeConductor.Agents.ProcessFixture')
Write-Host 'G10: performing the one authoritative x64 Debug G10 target rebuild.'
& $buildScript -Configuration Debug -Architecture x64 `
    -Target $g10BuildTargets -Parallel $Parallel -Fresh
Assert-True $? 'one authoritative x64 Debug G10 target rebuild'
Write-Host 'G10: running the one authoritative x64 Debug G10 CTest pass.'
& $testScript -Configuration Debug -Architecture x64 `
    -Parallel $Parallel -Label G10
Assert-True $? 'one authoritative x64 Debug G10 CTest pass'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$artifactTemplates = @(
    'lib/{0}/ForgeConductor.Application.lib',
    'lib/{0}/ForgeConductor.Infrastructure.Windows.lib',
    'lib/{0}/ForgeConductor.Persistence.Windows.lib',
    'bin/{0}/ForgeConductor.Agents.CatalogTests.exe',
    'bin/{0}/ForgeConductor.Agents.SessionServiceTests.exe',
    'bin/{0}/ForgeConductor.Agents.RepositoryWindowsTests.exe',
    'bin/{0}/ForgeConductor.Agents.ProcessFixture.exe')
$artifactHashes = [ordered]@{}
$sourceResourceHashes = [ordered]@{}
foreach ($resourceName in $resourceNames) {
    $sourceResourceHashes[$resourceName] = Get-FileSha256 `
        (Join-Path $resourceRoot $resourceName)
}

foreach ($configuration in @('Debug')) {
    foreach ($artifactTemplate in $artifactTemplates) {
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
        if ($relativeArtifact.EndsWith('.exe', [StringComparison]::Ordinal)) {
            Assert-X64PortableExecutable $artifactPath `
                "$configuration $relativeArtifact"
        }
    }

    $stagedResourceRoot = Join-Path $buildRoot `
        "bin\$configuration\Resources\Agents"
    Assert-True (Test-Path -LiteralPath $stagedResourceRoot -PathType Container) `
        "$configuration staged Agents resource directory"
    Assert-Set @(Get-ChildItem -LiteralPath $stagedResourceRoot -Force -File |
        ForEach-Object { $_.Name }) $resourceNames `
        "$configuration exact staged resource inventory"
    Assert-Set @(Get-ChildItem -LiteralPath $stagedResourceRoot -Force -Directory |
        ForEach-Object { $_.Name }) @() `
        "$configuration staged resource subdirectory inventory"
    foreach ($resourceName in $resourceNames) {
        $stagedResourceHash = Get-FileSha256 `
            (Join-Path $stagedResourceRoot $resourceName)
        Assert-Exact $stagedResourceHash `
            $sourceResourceHashes[$resourceName] `
            "$configuration staged resource hash $resourceName"
        Write-Host "$configuration Resources/Agents/$resourceName SHA-256: $stagedResourceHash"
    }

}

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after full G10 validation'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after full G10 validation'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after full G10 validation'

Invoke-RepositoryIntegrityChecks
Write-Host "G10 agent validation passed: $script:AssertionCount fail-closed assertions; exact ten playbooks, three CTest targets plus one process fixture, $($artifactHashes.Count) Debug artifact hashes, retained G09 static validation, and the one authoritative x64 Debug G10 rebuild/test pass succeeded."
