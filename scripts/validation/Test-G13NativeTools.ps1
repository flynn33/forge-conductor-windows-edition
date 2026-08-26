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
    if (-not $Condition) { throw "G13 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) `
        "$Message (expected '$Expected', found '$Actual')"
}

function Assert-Set {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualValues = @($Actual | ForEach-Object { [string]$_ } |
        Sort-Object -CaseSensitive)
    $expectedValues = @($Expected | ForEach-Object { [string]$_ } |
        Sort-Object -CaseSensitive)
    Assert-Exact $actualValues.Count $expectedValues.Count "$Message count"
    for ($index = 0; $index -lt $expectedValues.Count; $index++) {
        Assert-Exact $actualValues[$index] $expectedValues[$index] `
            "$Message item $index"
    }
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

function Assert-CrlfTextFile {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -gt 0) "$Message is nonempty"
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    Assert-True (-not $hasBom) "$Message has no UTF-8 BOM"
    $bareLf = 0
    $bareCr = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10 -and
            ($index -eq 0 -or $bytes[$index - 1] -ne 13)) { $bareLf++ }
        if ($bytes[$index] -eq 13 -and
            ($index -eq $bytes.Length - 1 -or $bytes[$index + 1] -ne 10)) {
            $bareCr++
        }
    }
    Assert-Exact $bareLf 0 "$Message bare-LF count"
    Assert-Exact $bareCr 0 "$Message bare-CR count"
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

function Assert-X64PortableExecutable {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -ge 0x40) "$Message DOS header length"
    Assert-Exact ([Text.Encoding]::ASCII.GetString($bytes, 0, 2)) 'MZ' `
        "$Message DOS signature"
    $offset = [BitConverter]::ToInt32($bytes, 0x3C)
    Assert-True ($offset -ge 0x40 -and $offset + 6 -le $bytes.Length) `
        "$Message bounded PE offset"
    Assert-Exact ([Text.Encoding]::ASCII.GetString($bytes, $offset, 4)) `
        "PE`0`0" "$Message PE signature"
    Assert-Exact ([BitConverter]::ToUInt16($bytes, $offset + 4)) `
        ([uint16]0x8664) "$Message x64 machine"
}

function Resolve-CtestExecutable {
    $command = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $statePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
    Assert-True (Test-Path -LiteralPath $statePath -PathType Leaf) `
        'toolchain state exists for CTest resolution'
    $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
    $candidate = [string]$state.tools.ctest
    Assert-True (-not [string]::IsNullOrWhiteSpace($candidate) -and
        (Test-Path -LiteralPath $candidate -PathType Leaf)) `
        'CTest executable exists from toolchain state'
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Invoke-RepositoryIntegrityChecks {
    $output = @(& git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('git diff --check: ' + ($output -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti tree hash'

$retained = Join-Path $WorkspaceRoot `
    'scripts\validation\Test-G12NativeSessionHost.ps1'
Assert-True (Test-Path -LiteralPath $retained -PathType Leaf) `
    'retained G12 validator exists'
Write-Host 'G13: running retained G12 static validation without a retained rebuild.'
& $retained -WorkspaceRoot $WorkspaceRoot -Parallel $Parallel -StaticOnly
Assert-True $? 'retained G12 static validation'

& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-Python validation'
& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-attribution validation'

$phases = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\phases.json') | ConvertFrom-Json
$phase = @($phases.phases | Where-Object id -ceq 'P13')
Assert-Exact $phase.Count 1 'single P13 phase plan entry'
Assert-Exact ([string]$phase[0].title) `
    'Filesystem, search, Git, shell, and PDF tools' 'P13 title'
Assert-Set @($phase[0].dependencies) @('P06') 'P13 dependency'
Assert-Set @($phase[0].required_gates) @('G13') 'P13 required gate'
Assert-Set @($phase[0].required_work) @(
    'Port bounded tools with workspace authority',
    'Use CreateProcessW/Job Objects and native search/PDF',
    'Negative security and timeout tests') 'P13 required work'

$gates = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\gates.json') | ConvertFrom-Json
$gate = @($gates.gates | Where-Object id -ceq 'G13')
Assert-Exact $gate.Count 1 'single G13 gate plan entry'
Assert-Exact ([string]$gate[0].title) 'Native tools' 'G13 title'
Assert-Exact ([string]$gate[0].class) 'hard' 'G13 class'
Assert-Exact ([string]$gate[0].acceptance) `
    'Filesystem/search/Git/shell/PDF behavior and security tests pass.' `
    'G13 acceptance'

$expectedToolNames = @(
    'fs_read',
    'fs_write',
    'fs_edit',
    'fs_list',
    'fs_glob',
    'fs_mkdir',
    'fs_delete',
    'fs_move',
    'git_status',
    'git_diff',
    'git_log',
    'git_add',
    'git_commit',
    'search_text',
    'pdf_write',
    'pdf_from_file',
    'shell_exec')
$mcpParity = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\mcp-tool-parity.json') | ConvertFrom-Json
$p13Tools = @($mcpParity.tools | Where-Object {
    [int]$_.index -ge 9 -and [int]$_.index -le 25
} | Sort-Object { [int]$_.index })
Assert-Exact $p13Tools.Count 17 'P13 canonical MCP tool count'
for ($index = 0; $index -lt $expectedToolNames.Count; $index++) {
    Assert-Exact ([int]$p13Tools[$index].index) ($index + 9) `
        "P13 canonical MCP index $index"
    Assert-Exact ([string]$p13Tools[$index].name) $expectedToolNames[$index] `
        "P13 canonical MCP name $index"
}

$featureRows = Get-Content -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\feature-parity-matrix.tsv') |
    ConvertFrom-Csv -Delimiter ([char]9)
$toolFeatures = @($featureRows | Where-Object id -in @(
    'TOOL-001','TOOL-002','TOOL-003','TOOL-004','TOOL-005','TOOL-006'))
Assert-Set @($toolFeatures.id) @(
    'TOOL-001','TOOL-002','TOOL-003','TOOL-004','TOOL-005','TOOL-006') `
    'P13 feature-parity rows'

$semanticInventory = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\state\baseline\p02-mcp-semantic-inventory.json') |
    ConvertFrom-Json
Assert-Exact ([int]$semanticInventory.bounds.text_file_bytes) 2097152 `
    'source text-file byte bound'
Assert-Exact ([int]$semanticInventory.bounds.directory_entries) 1000 `
    'source directory-entry bound'
Assert-Exact ([int]$semanticInventory.bounds.glob_results) 500 `
    'source glob-result bound'
Assert-Exact ([int]$semanticInventory.bounds.search_results) 200 `
    'source search-result bound'
Assert-Exact ([int]$semanticInventory.bounds.shell_timeout_seconds) 120 `
    'source shell timeout maximum'
Assert-Exact ([int]$semanticInventory.bounds.shell_capture_bytes) 100000 `
    'source shell aggregate capture bound'
Assert-Exact ([int]$semanticInventory.bounds.shell_stdout_bytes) 80000 `
    'source shell stdout bound'
Assert-Exact ([int]$semanticInventory.bounds.shell_stderr_bytes) 20000 `
    'source shell stderr bound'

$nativeHeaderRoot = Join-Path $WorkspaceRoot `
    'include\ForgeConductor\NativeTools\Windows'
$nativeSourceRoot = Join-Path $WorkspaceRoot 'src\NativeTools\Windows'
$nativeTestRoot = Join-Path $WorkspaceRoot 'tests\NativeTools'
$expectedNativeHeaders = @(
    'WindowsFileSystem.h',
    'WindowsGitService.h',
    'WindowsPathGlobService.h',
    'WindowsPdfService.h',
    'WindowsShellService.h',
    'WindowsTextSearchService.h')
$expectedNativeSources = @(
    'NativeFileOperations.cpp',
    'NativeFileOperations.h',
    'NativeToolValidation.h',
    'WindowsFileSystem.cpp',
    'WindowsGitService.cpp',
    'WindowsPathGlobService.cpp',
    'WindowsPdfService.cpp',
    'WindowsShellService.cpp',
    'WindowsTextSearchService.cpp')
$expectedNativeTests = @(
    'WindowsFileSystemSearchTests.cpp',
    'WindowsGitShellIntegrationTests.cpp',
    'WindowsGitShellTests.cpp',
    'WindowsPdfServiceTests.cpp',
    'WindowsWorkspaceAuthorityTests.cpp')
Assert-Set @((Get-ChildItem -LiteralPath $nativeHeaderRoot -File).Name) `
    $expectedNativeHeaders 'P13 public native-tools header inventory'
Assert-Set @((Get-ChildItem -LiteralPath $nativeSourceRoot -File).Name) `
    $expectedNativeSources 'P13 native-tools source inventory'
Assert-Set @((Get-ChildItem -LiteralPath $nativeTestRoot -File).Name) `
    $expectedNativeTests 'P13 native-tools test inventory'

$p13Files = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Contracts/IFileSystemServices.h',
    'include/ForgeConductor/Contracts/INativeToolServices.h',
    'include/ForgeConductor/Contracts/IPathGlobService.h',
    'include/ForgeConductor/Domain/Domain.h',
    'include/ForgeConductor/Domain/FileSystemModels.h',
    'include/ForgeConductor/Domain/PdfModels.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h',
    'src/Infrastructure/Windows/Detail/WindowsPathResolver.h',
    'src/Infrastructure/Windows/Detail/WindowsPathResolver.cpp',
    'src/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.cpp',
    'src/Infrastructure/Windows/WindowsWorkspaceAuthority.cpp',
    'tests/Continuity/LegacyContinuityPersistenceWindowsTests.cpp',
    'tests/Contracts/NativeToolBoundaryFakeContractTests.h',
    'tests/Fakes/FileSystemFake.h',
    'tests/Fakes/GitServiceFake.h',
    'tests/Fakes/PdfServiceFake.h',
    '.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md',
    '.forge-codex/state/decisions/P13-001-bounded-native-tool-boundaries.md',
    '.forge-codex/state/decisions/P13-002-handle-aware-filesystem-search-and-glob.md',
    '.forge-codex/state/decisions/P13-003-exact-git-and-powershell-process-adapters.md',
    '.forge-codex/state/decisions/P13-004-deterministic-native-pdf-writer.md',
    '.forge-codex/state/decisions/P13-005-native-services-to-mcp-parity-boundary.md',
    '.forge-codex/state/decisions/P13-006-g13-regression-and-alpha-qualification.md',
    'scripts/validation/Test-G13NativeTools.ps1')
$p13Files += @($expectedNativeHeaders | ForEach-Object {
    'include/ForgeConductor/NativeTools/Windows/' + $_
})
$p13Files += @($expectedNativeSources | ForEach-Object {
    'src/NativeTools/Windows/' + $_
})
$p13Files += @($expectedNativeTests | ForEach-Object {
    'tests/NativeTools/' + $_
})
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
$p13Decisions = @(Get-ChildItem -LiteralPath $decisionRoot -File -Filter 'P13-*.md')
Assert-Set @($p13Decisions.Name) @(
    'P13-001-bounded-native-tool-boundaries.md',
    'P13-002-handle-aware-filesystem-search-and-glob.md',
    'P13-003-exact-git-and-powershell-process-adapters.md',
    'P13-004-deterministic-native-pdf-writer.md',
    'P13-005-native-services-to-mcp-parity-boundary.md',
    'P13-006-g13-regression-and-alpha-qualification.md') `
    'exact P13 decision inventory'
foreach ($relative in @($p13Files | Sort-Object -Unique)) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "P13 file exists: $relative"
    Assert-CrlfTextFile $path "P13 text file $relative"
}

$portablePaths = @(
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Contracts/IFileSystemServices.h',
    'include/ForgeConductor/Contracts/INativeToolServices.h',
    'include/ForgeConductor/Contracts/IPathGlobService.h',
    'include/ForgeConductor/Domain/Domain.h',
    'include/ForgeConductor/Domain/FileSystemModels.h',
    'include/ForgeConductor/Domain/PdfModels.h')
$portableText = ($portablePaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join [Environment]::NewLine
Assert-NoMatch $portableText `
    '(?im)^\s*#\s*include\s*[<"](?:windows[.]h|winrt[/\\]|winsqlite3?[.]h|sqlite3[.]h|nlohmann[/\\]|filesystem[>"])' `
    'P13 Domain and Contracts remain platform neutral'
Assert-NoMatch $portableText `
    '\b(?:HANDLE|HWND|HRESULT|DWORD|sqlite3|nlohmann::|std::filesystem)\b' `
    'P13 portable contracts expose no platform implementation types'

$nativePublicText = ($expectedNativeHeaders | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $nativeHeaderRoot $_)
}) -join [Environment]::NewLine
Assert-NoMatch $nativePublicText `
    '(?im)^\s*#\s*include\s*[<"](?:windows[.]h|winioctl[.]h|winternl[.]h)[>"]' `
    'P13 public native-tool headers hide raw Windows APIs'
Assert-NoMatch $nativePublicText '\b(?:HANDLE|DWORD|NTSTATUS|IO_STATUS_BLOCK)\b' `
    'P13 public native-tool headers hide raw Windows ownership types' `
    -CaseSensitive

$productionPaths = @(
    'include/ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h',
    'src/Infrastructure/Windows/WindowsWorkspaceAuthority.cpp')
$productionPaths += @($expectedNativeHeaders | ForEach-Object {
    'include/ForgeConductor/NativeTools/Windows/' + $_
})
$productionPaths += @($expectedNativeSources | ForEach-Object {
    'src/NativeTools/Windows/' + $_
})
$productionText = ($productionPaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join [Environment]::NewLine
Assert-NoMatch $productionText `
    '(?:\.py\b|python(?:3|\.exe)?\b|pyproject|pip(?:3|\.exe)?\b)' `
    'P13 production contains no Python dependency'
Assert-NoMatch $productionText `
    '(?:\bdotnet\b|System[.]IO|System[.]Diagnostics|Microsoft[.]Win32)' `
    'P13 production contains no .NET dependency'
Assert-NoMatch $productionText `
    '(?:\bnode(?:[.]exe)?\b|\bnpm\b|\bnpx\b|Electron|#\s*include\s*[<"](?:Qt|boost/))' `
    'P13 production contains no forbidden Node, Electron, Qt, or Boost dependency'

$processAdapterText = (Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsGitService.cpp')) +
    [Environment]::NewLine + (Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsShellService.cpp'))
Assert-NoMatch $processAdapterText `
    '\b(?:CreateProcessW|ShellExecute(?:Ex)?W?|SearchPathW|_popen|popen|system)\s*\(' `
    'Git and shell adapters use no ambient or direct process-launch API'
Assert-NoMatch $processAdapterText '\bcmd(?:[.]exe)?\b' `
    'Git and shell adapters do not compose through cmd.exe'
Assert-Match $processAdapterText 'processSupervisor_->run' `
    'Git and shell adapters delegate to the process supervisor' -CaseSensitive

$contractsUmbrella = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Contracts\Contracts.h')
Assert-Match $contractsUmbrella `
    '#include\s+"ForgeConductor/Contracts/IPathGlobService[.]h"' `
    'Contracts umbrella exports P13 glob and edit contracts' -CaseSensitive
$domainUmbrella = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Domain\Domain.h')
Assert-Match $domainUmbrella `
    '#include\s+"ForgeConductor/Domain/PdfModels[.]h"' `
    'Domain umbrella exports the PDF receipt model' -CaseSensitive
$infrastructureUmbrella = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Infrastructure\Windows\InfrastructureWindows.h')
Assert-Match $infrastructureUmbrella `
    '#include\s+"ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority[.]h"' `
    'Windows infrastructure umbrella exports workspace authority' -CaseSensitive

foreach ($className in @(
    'WindowsWorkspaceAuthority',
    'WindowsFileSystem',
    'WindowsGitService',
    'WindowsPathGlobService',
    'WindowsPdfService',
    'WindowsShellService',
    'WindowsTextSearchService')) {
    Assert-Match ($productionText + $nativePublicText) `
        ('class\s+' + [regex]::Escape($className) + '\s+final\b') `
        "P13 concrete class is final: $className" -CaseSensitive
}

$fileSystemHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeHeaderRoot 'WindowsFileSystem.h')
$fileSystemModels = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Domain\FileSystemModels.h')
$fileSystemContracts = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Contracts\IFileSystemServices.h')
Assert-Match $fileSystemModels `
    'struct\s+DirectoryListing\s+final\s*\{[\s\S]*?std::vector<PathText>\s+entries;[\s\S]*?bool\s+truncated' `
    'directory listings expose bounded-prefix truncation metadata' -CaseSensitive
Assert-Match $fileSystemContracts `
    'Result<Domain::DirectoryListing>\s+list\s*\(' `
    'filesystem contract returns a directory-listing value' -CaseSensitive
Assert-Match $fileSystemHeader `
    'Result<Domain::DirectoryListing>\s+list\s*\(' `
    'Windows filesystem implements the directory-listing contract' -CaseSensitive
Assert-Match $fileSystemHeader `
    'MaximumTextFileBytes\s*=\s*2U\s*[*]\s*1024U\s*[*]\s*1024U' `
    'filesystem 2 MiB text bound' -CaseSensitive
Assert-Match $fileSystemHeader 'MaximumListEntries\s*=\s*1''000U' `
    'filesystem 1000-entry bound' -CaseSensitive
$globHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeHeaderRoot 'WindowsPathGlobService.h')
Assert-Match $globHeader 'MaximumMatches\s*=\s*500U' `
    'glob 500-match bound' -CaseSensitive
Assert-Match $globHeader `
    'MaximumResponseBytes\s*=\s*2U\s*[*]\s*1024U\s*[*]\s*1024U' `
    'glob 2 MiB response bound' -CaseSensitive
$searchHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeHeaderRoot 'WindowsTextSearchService.h')
Assert-Match $searchHeader 'MaximumMatches\s*=\s*200U' `
    'search 200-match bound' -CaseSensitive
Assert-Match $searchHeader `
    'MaximumResponseBytes\s*=\s*2U\s*[*]\s*1024U\s*[*]\s*1024U' `
    'search 2 MiB response bound' -CaseSensitive
$searchSource = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsTextSearchService.cpp')
Assert-Match $searchSource 'std::regex_constants::basic' `
    'text search compiles patterns with POSIX basic-regex semantics' -CaseSensitive
Assert-Match $searchSource 'std::regex_search\s*\(' `
    'text search evaluates compiled regular expressions' -CaseSensitive
$gitHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeHeaderRoot 'WindowsGitService.h')
foreach ($entry in @(
    @('MaximumOutputBytes','80''000U'),
    @('MaximumErrorBytes','20''000U'),
    @('MaximumLogEntries','200U'),
    @('MaximumAddPaths','200U'))) {
    Assert-Match $gitHeader `
        ([regex]::Escape($entry[0]) + '\s*=\s*' + [regex]::Escape($entry[1])) `
        "Git bound $($entry[0])" -CaseSensitive
}
Assert-Match $gitHeader `
    'MaximumArgumentBytes\s*=\s*4U\s*[*]\s*1024U' `
    'Git 4 KiB argument bound' -CaseSensitive
$shellHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeHeaderRoot 'WindowsShellService.h')
Assert-Match $shellHeader 'DefaultTimeout\{30''000\}' `
    'shell 30-second default timeout' -CaseSensitive
Assert-Match $shellHeader 'MaximumTimeout\{120''000\}' `
    'shell 120-second maximum timeout' -CaseSensitive
Assert-Match $shellHeader 'MaximumCommandBytes\s*=\s*4U\s*[*]\s*1024U' `
    'shell 4 KiB command bound' -CaseSensitive
Assert-Match $shellHeader 'MaximumOutputBytes\s*=\s*80''000U' `
    'shell 80000-byte stdout bound' -CaseSensitive
Assert-Match $shellHeader 'MaximumErrorBytes\s*=\s*20''000U' `
    'shell 20000-byte stderr bound' -CaseSensitive
$nativeToolContracts = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Contracts\INativeToolServices.h')
$gitProcessResultMethods = [regex]::Matches(
    $nativeToolContracts,
    'Result<Domain::ProcessResult>\s+(?:status|diff|log|add|commit)\s*\(').Count
Assert-Exact $gitProcessResultMethods 5 `
    'all five Git operations preserve structured process outcomes'
Assert-Match $nativeToolContracts 'MaximumTitleBytes\s*=\s*512U' `
    'PDF 512-byte title bound' -CaseSensitive
Assert-Match $nativeToolContracts `
    'MaximumTextBytes\s*=\s*2U\s*[*]\s*1024U\s*[*]\s*1024U' `
    'PDF 2 MiB source-text bound' -CaseSensitive
Assert-Match $nativeToolContracts `
    'MaximumPdfBytes\s*=\s*16U\s*[*]\s*1024U\s*[*]\s*1024U' `
    'PDF 16 MiB output bound' -CaseSensitive
$workspaceHeader = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Infrastructure\Windows\WindowsWorkspaceAuthority.h')
Assert-Match $workspaceHeader 'MaximumPolicies\s*=\s*32U' `
    'workspace authority policy bound' -CaseSensitive
Assert-Match $workspaceHeader 'MaximumTrustedRootsPerPolicy\s*=\s*32U' `
    'workspace authority root bound' -CaseSensitive

$nativeOperations = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'NativeFileOperations.cpp')
$nativeOperationsHeader = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'NativeFileOperations.h')
$fileSystemSource = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsFileSystem.cpp')
Assert-Match $fileSystemSource `
    'Domain::DirectoryListing\s*\{\s*std::move\(result\)\s*,\s*truncated\s*\}' `
    'filesystem returns a sorted bounded prefix with truncation metadata' `
    -CaseSensitive
Assert-Match $nativeOperations 'GetFinalPathNameByHandleW' `
    'opened filesystem handles are canonical-path verified' -CaseSensitive
Assert-Match $nativeOperations 'FILE_FLAG_OPEN_REPARSE_POINT' `
    'filesystem opens reparse points without following them' -CaseSensitive
Assert-Match ($nativeOperations + $nativeOperationsHeader) `
    'FILE_ATTRIBUTE_REPARSE_POINT' 'native traversal identifies reparse points' `
    -CaseSensitive
Assert-Match $nativeOperations 'FileDispositionInfoEx' `
    'filesystem deletion is handle based' -CaseSensitive
Assert-Match $fileSystemSource 'NtSetInformationFile' `
    'filesystem move uses a handle-based native rename boundary' -CaseSensitive
Assert-Match $fileSystemSource 'rejectAuthorityRoot' `
    'destructive filesystem operations protect authority roots' -CaseSensitive
Assert-Match ($nativeOperations + $nativeOperationsHeader) `
    'ensureAuthorizedParentDirectories' `
    'native filesystem owns authorized parent creation' -CaseSensitive
Assert-Match $fileSystemSource `
    'writeFile[\s\S]*?ensureAuthorizedParentDirectories' `
    'filesystem writes create authorized missing parents' -CaseSensitive
Assert-Match $fileSystemSource `
    'move[\s\S]*?ensureAuthorizedParentDirectories' `
    'filesystem moves create authorized missing destination parents' -CaseSensitive
Assert-Match $nativeOperationsHeader 'authorizedPathOwner' `
    'opened native objects retain their anchored authority owner' -CaseSensitive
Assert-Match $nativeOperations 'revalidateDirectoryAnchors' `
    'native filesystem revalidates retained directory anchors' -CaseSensitive
Assert-NoMatch ($nativeOperations + [Environment]::NewLine + $fileSystemSource + `
    [Environment]::NewLine + $searchSource) `
    '\bFILE_SHARE_DELETE\b' `
    'active filesystem operations deny delete-sharing namespace races' `
    -CaseSensitive

$gitSource = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsGitService.cpp')
Assert-Match $gitSource 'GIT_TERMINAL_PROMPT"\s*,\s*"0' `
    'Git disables terminal prompting' -CaseSensitive
foreach ($field in @(
    'stdoutUtf8', 'stderrUtf8', 'stdoutTruncated', 'stderrTruncated')) {
    Assert-Match $gitSource `
        ('enforceOutputBounds[\s\S]*?' + [regex]::Escape($field)) `
        "Git bounds structured process field $field" -CaseSensitive
}
foreach ($fragment in @(
    '"status", "--porcelain=v1", "-b"',
    '"log", "-n"',
    '"commit", "-m"')) {
    Assert-Match $gitSource ([regex]::Escape($fragment)) `
        "Git direct argv fragment $fragment" -CaseSensitive
}
Assert-Match $gitSource `
    'std::vector<std::string>\s+arguments\{"add"\}[\s\S]*?arguments[.]emplace_back\("-A"\)' `
    'Git add-all uses a direct argv vector' -CaseSensitive
$shellSource = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsShellService.cpp')
foreach ($switch in @('-NoLogo','-NoProfile','-NonInteractive','-Command')) {
    Assert-Match $shellSource ([regex]::Escape('"' + $switch + '"')) `
        "PowerShell fixed switch $switch" -CaseSensitive
}
Assert-Match $shellSource 'shellEnabled\(\)' `
    'shell execution checks the immutable authority policy' -CaseSensitive
Assert-Match $shellSource 'MaximumConcurrentOperations' `
    'shell admission is bounded' -CaseSensitive
Assert-Match $shellSource 'processSupervisor->cancel' `
    'shell shutdown and cancellation reach the supervisor' -CaseSensitive
Assert-Match $shellSource 'std::stop_source' `
    'shell owns a local stop source per admitted operation' -CaseSensitive
Assert-Match $shellSource 'std::stop_callback\s+callerCancellationBridge' `
    'shell bridges caller cancellation into local admission state' -CaseSensitive
Assert-Match $shellSource `
    'OperationContext\s+supervisorContext[\s\S]*?operationCancellation->get_token\(\)' `
    'shell passes its durable local stop token to the supervisor' -CaseSensitive
Assert-Match $shellSource 'operationCancellation->request_stop\(\)' `
    'shell cancellation persists before supervisor admission' -CaseSensitive
Assert-NoMatch $shellSource `
    'processSupervisor\s*->\s*(?:cancelAll|shutdown)\s*\(' `
    'shell does not cancel all work or shut down its shared supervisor' `
    -CaseSensitive

$pdfSource = Get-Content -Raw -LiteralPath (
    Join-Path $nativeSourceRoot 'WindowsPdfService.cpp')
foreach ($fragment in @('%PDF-1.4','xref','trailer','startxref','%%EOF')) {
    Assert-Match $pdfSource ([regex]::Escape($fragment)) `
        "native PDF structure $fragment" -CaseSensitive
}
Assert-Match $pdfSource 'MaximumWrappedRows\s*=\s*200''000U' `
    'native PDF wrapped-row bound' -CaseSensitive
Assert-Match $pdfSource 'atomicFileStore[.]replace' `
    'native PDF publishes through the injected atomic store' -CaseSensitive
Assert-Match $pdfSource `
    'wrapLine\([\s\S]*?OperationContext&\s+context\)[\s\S]*?while\s*\([\s\S]*?checkContext\(context' `
    'native PDF wrapping is cancellation and deadline aware' -CaseSensitive
Assert-Match $pdfSource 'ensureAuthorizedParentDirectories' `
    'native PDF publication creates authorized missing parents' -CaseSensitive

$workspaceSource = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Infrastructure\Windows\WindowsWorkspaceAuthority.cpp')
Assert-Match $workspaceSource 'FILE_FLAG_OPEN_REPARSE_POINT' `
    'workspace authority validates roots without following reparses' -CaseSensitive
Assert-Match $workspaceSource 'PathOutsideAuthority' `
    'workspace authority rejects path escape' -CaseSensitive
Assert-Match $workspaceSource 'FILE_CASE_SENSITIVE_INFO' `
    'workspace authority reads directory case-sensitivity state' -CaseSensitive
Assert-Match $workspaceSource 'validateDirectoryCaseSensitivityFlags' `
    'workspace authority rejects unsupported case-sensitive roots' -CaseSensitive
Assert-Match $workspaceSource 'narrowAuthority' `
    'workspace authority narrows immutable capability scope' -CaseSensitive

$fileSystemTests = Get-Content -Raw -LiteralPath (
    Join-Path $nativeTestRoot 'WindowsFileSystemSearchTests.cpp')
foreach ($case in @(
    'native-tools.filesystem-roundtrip-mutations',
    'native-tools.glob-search-bounds-order',
    'native-tools.authority-reparse-root',
    'native-tools.bounds-utf8-context',
    'native-tools.authorized-open-pins-ancestors')) {
    Assert-Match $fileSystemTests ([regex]::Escape($case)) `
        "filesystem/search native case $case" -CaseSensitive
}
Assert-Match $fileSystemTests 'cross-directory handle-relative move' `
    'filesystem tests cover cross-directory moves' -CaseSensitive
Assert-Match $fileSystemTests 'cross-directory directory move failed' `
    'filesystem tests cover cross-directory directory moves' -CaseSensitive
Assert-Match $fileSystemTests 'directory junction' `
    'filesystem tests cover reparse-point denial' -CaseSensitive
Assert-Match $fileSystemTests `
    'text search did not preserve POSIX basic-regex behavior' `
    'filesystem/search tests cover POSIX basic-regex behavior' -CaseSensitive
Assert-Match $fileSystemTests `
    'text search accepted an invalid basic regular expression' `
    'filesystem/search tests cover invalid basic-regex rejection' -CaseSensitive
foreach ($evidence in @(
    'directory listing did not return a sorted truncated prefix',
    'native file creation did not create missing parent directories',
    'native move did not create missing destination parents',
    'an active authorized open allowed an ancestor rename race')) {
    Assert-Match $fileSystemTests ([regex]::Escape($evidence)) `
        "filesystem invariant evidence $evidence" -CaseSensitive
}
$nativeContractTests = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'tests\Contracts\NativeToolBoundaryFakeContractTests.h')
Assert-Match $nativeContractTests `
    ([regex]::Escape(
        'filesystem fake did not preserve sorted-prefix truncation metadata')) `
    'filesystem fake contract preserves listing truncation metadata' `
    -CaseSensitive
Assert-Match $nativeContractTests `
    ([regex]::Escape(
        'git status did not preserve its configured nonzero process result')) `
    'Git fake contract preserves a configured nonzero process result' `
    -CaseSensitive
Assert-Match $nativeContractTests `
    ([regex]::Escape(
        'git fake did not cap structured output and retain process status')) `
    'Git fake contract bounds output without losing process status' `
    -CaseSensitive

$gitShellTests = Get-Content -Raw -LiteralPath (
    Join-Path $nativeTestRoot 'WindowsGitShellTests.cpp')
foreach ($case in @(
    'native_tools.git_direct_argv',
    'native_tools.git_bounds_authority',
    'native_tools.git_process_outcomes',
    'native_tools.shell_fixed_powershell',
    'native_tools.shell_policy_shutdown',
    'native_tools.shell_admission_shutdown_race')) {
    Assert-Match $gitShellTests ([regex]::Escape($case)) `
        "Git/shell adapter case $case" -CaseSensitive
}
$gitShellIntegration = Get-Content -Raw -LiteralPath (
    Join-Path $nativeTestRoot 'WindowsGitShellIntegrationTests.cpp')
Assert-Match $gitShellIntegration 'WindowsProcessSupervisor' `
    'Git/shell integration uses the production process supervisor' -CaseSensitive
foreach ($behavior in @(
    'git.status', 'git.add', 'git.diff', 'git.commit', 'git.log',
    'shell.execute', 'timedOut', 'stdoutTruncated',
    'Git integration discarded a real nonzero process payload')) {
    Assert-Match $gitShellIntegration ([regex]::Escape($behavior)) `
        "Git/shell integration behavior $behavior" -CaseSensitive
}
Assert-Match $gitShellIntegration 'argc\s*==\s*3' `
    'Git/shell integration requires exact injected executable paths' -CaseSensitive

$pdfTests = Get-Content -Raw -LiteralPath (
    Join-Path $nativeTestRoot 'WindowsPdfServiceTests.cpp')
foreach ($case in @(
    'native_tools.pdf_valid_escape_receipt',
    'native_tools.pdf_multipage_conversion',
    'native_tools.pdf_input_bounds',
    'native_tools.pdf_context',
    'native_tools.pdf_maximum_unbroken_deadline',
    'native_tools.pdf_maximum_tab_deadline',
    'native_tools.pdf_windows_runtime_load_render')) {
    Assert-Match $pdfTests ([regex]::Escape($case)) `
        "native PDF case $case" -CaseSensitive
}
Assert-Match $pdfTests 'LoadFromFileAsync' `
    'Windows.Data.Pdf loads the generated document' -CaseSensitive
Assert-Match $pdfTests 'RenderToStreamAsync' `
    'Windows.Data.Pdf renders the generated first page' -CaseSensitive
Assert-Match $pdfTests `
    'root\s*/\s*L"nested"\s*/\s*L"publication"' `
    'native PDF test publishes through missing destination parents' `
    -CaseSensitive
$authorityTests = Get-Content -Raw -LiteralPath (
    Join-Path $nativeTestRoot 'WindowsWorkspaceAuthorityTests.cpp')
foreach ($case in @(
    'workspace_authority.canonical_success',
    'workspace_authority.identity_scope_generation',
    'workspace_authority.narrowing',
    'workspace_authority.hostile_paths',
    'workspace_authority.root_protection',
    'workspace_authority.reparse_case_overlap',
    'workspace_authority.context')) {
    Assert-Match $authorityTests ([regex]::Escape($case)) `
        "workspace authority case $case" -CaseSensitive
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
$nativeSourceList = [regex]::Match(
    $cmake,
    'set\s*\(\s*FORGE_NATIVE_TOOLS_WINDOWS_SOURCES(?<body>.*?)\)',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $nativeSourceList.Success 'CMake P13 native source list'
$cmakeNativeSources = @($nativeSourceList.Groups['body'].Value -split '\r?\n' |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -match '[.]cpp$' })
Assert-Set $cmakeNativeSources @(
    'src/NativeTools/Windows/NativeFileOperations.cpp',
    'src/NativeTools/Windows/WindowsFileSystem.cpp',
    'src/NativeTools/Windows/WindowsGitService.cpp',
    'src/NativeTools/Windows/WindowsPathGlobService.cpp',
    'src/NativeTools/Windows/WindowsPdfService.cpp',
    'src/NativeTools/Windows/WindowsShellService.cpp',
    'src/NativeTools/Windows/WindowsTextSearchService.cpp') `
    'CMake exact P13 implementation source set'
Assert-Match $cmake `
    'add_library\s*\(\s*ForgeConductor[.]NativeTools[.]Windows\s+STATIC\s+[$][{]FORGE_NATIVE_TOOLS_WINDOWS_SOURCES[}]\s*\)' `
    'P13 native tools are a static library' -CaseSensitive
Assert-Match $cmake `
    'forge_add_layer\s*\(\s*ForgeConductor[.]NativeTools[.]Windows\s+ForgeConductor::NativeTools[.]Windows\s+ForgeConductor::Contracts\s+ForgeConductor::Infrastructure[.]Windows\s*\)' `
    'P13 dependency direction terminates at Contracts and Windows infrastructure' `
    -CaseSensitive
Assert-Exact ([regex]::Matches(
    $cmake,
    'src/Infrastructure/Windows/WindowsWorkspaceAuthority[.]cpp',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant).Count) 1 `
    'single Windows workspace-authority production placement'

$g13Tests = @(
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests',
    'ForgeConductor.NativeTools.FileSystemSearchTests',
    'ForgeConductor.NativeTools.GitShellTests',
    'ForgeConductor.NativeTools.GitShellIntegrationTests',
    'ForgeConductor.NativeTools.PdfTests',
    'ForgeConductor.NativeTools.WorkspaceAuthorityTests',
    'ForgeConductor.NativeTools.ContractTests',
    'ForgeConductor.NativeTools.HeaderSelfContainment')
foreach ($test in $g13Tests) {
    Assert-Match $cmake ('add_test\s*\(\s*NAME\s+' + [regex]::Escape($test)) `
        "CMake test registration $test" -CaseSensitive
    Assert-Match $cmake `
        ([regex]::Escape($test) + '[\s\S]*?LABELS\s+"[^"]*G13[^"]*"') `
        "CMake G13 label $test" -CaseSensitive
}
Assert-Match $cmake `
    'ForgeConductor[.]NativeTools[.]ContractTests[\s\S]*?[$]<TARGET_FILE:ForgeConductor[.]Contracts[.]ContractTests>' `
    'G13 reuses the qualified contracts executable' -CaseSensitive
Assert-Match $cmake `
    'ForgeConductor[.]NativeTools[.]GitShellIntegrationTests[\s\S]*?FORGE_P13_GIT_EXECUTABLE[\s\S]*?FORGE_P13_POWERSHELL_EXECUTABLE' `
    'G13 integration receives exact discovered Git and PowerShell paths' `
    -CaseSensitive
Assert-Match $cmake `
    'target_link_libraries\s*\(\s*ForgeConductor[.]NativeTools[.]PdfTests[\s\S]*?\bwindowsapp\b[\s\S]*?\)' `
    'native PDF runtime validation links the Windows app platform library' `
    -CaseSensitive

if ($StaticOnly) {
    $frameworkAfter = Get-TreeSummary $frameworkRoot
    Assert-Exact ([string]$frameworkAfter.sha256) `
        ([string]$frameworkBefore.sha256) 'sealed Forsetti unchanged after static G13'
    Invoke-RepositoryIntegrityChecks
    Write-Host "G13 static native-tools validation passed: $script:AssertionCount assertions."
    return
}

$buildTargets = @(
    'ForgeConductor.Domain.UnitTests',
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.Infrastructure.ShutdownTests',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests',
    'ForgeConductor.NativeTools.FileSystemSearchTests',
    'ForgeConductor.NativeTools.GitShellTests',
    'ForgeConductor.NativeTools.GitShellIntegrationTests',
    'ForgeConductor.NativeTools.PdfTests',
    'ForgeConductor.NativeTools.WorkspaceAuthorityTests',
    'ForgeConductor.NativeTools.HeaderSelfContainment')
$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G13: performing the one authoritative fresh x64 Debug affected-target build.'
& $buildScript -Configuration Debug -Architecture x64 -Target $buildTargets `
    -Parallel $Parallel -Fresh
Assert-True $? 'one authoritative fresh x64 Debug G06/G13 affected-target build'
Write-Host 'G13: running the one authoritative x64 Debug retained G06 plus G13 CTest pass.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel `
    -Label 'G06|G13'
Assert-True $? 'one authoritative x64 Debug retained G06 plus G13 CTest pass'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$ctest = Resolve-CtestExecutable
$ctestJsonText = (& $ctest --test-dir $buildRoot -C Debug -L 'G06|G13' `
    --show-only=json-v1) -join [Environment]::NewLine
Assert-Exact $LASTEXITCODE 0 'G06/G13 CTest JSON inventory command'
try {
    $ctestInventory = $ctestJsonText | ConvertFrom-Json
} catch {
    throw "G13 assertion failed: invalid CTest JSON inventory - $($_.Exception.Message)"
}

$expectedTests = @(
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.Infrastructure.ShutdownTests',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.HeaderSelfContainment') + $g13Tests
Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) $expectedTests `
    'exact retained G06 plus G13 CTest inventory'
Assert-Exact (@($ctestInventory.tests).Count) 12 `
    'retained G06 plus G13 CTest count'

$expectedLabels = [ordered]@{
    'ForgeConductor.Infrastructure.UnitTests' = @('G06','T-SEC','T-UNIT')
    'ForgeConductor.Infrastructure.ShutdownTests' = @('G06','T-STRESS','T-UNIT')
    'ForgeConductor.Infrastructure.ProcessTests' = @('G06','T-PROC','T-SEC')
    'ForgeConductor.Infrastructure.HeaderSelfContainment' = @('G06','T-UNIT')
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests' = @(
        'G11','G13','T-CONT','T-DB','T-SEC','T-STRESS','T-UNIT')
    'ForgeConductor.NativeTools.FileSystemSearchTests' = @(
        'G13','T-INTEGRATION','T-SEC','T-UNIT')
    'ForgeConductor.NativeTools.GitShellTests' = @(
        'G13','T-PROC','T-SEC','T-UNIT')
    'ForgeConductor.NativeTools.GitShellIntegrationTests' = @(
        'G13','T-INTEGRATION','T-PROC','T-SEC')
    'ForgeConductor.NativeTools.PdfTests' = @(
        'G13','T-DOC','T-INTEGRATION','T-SEC','T-UNIT')
    'ForgeConductor.NativeTools.WorkspaceAuthorityTests' = @(
        'G13','T-SEC','T-UNIT')
    'ForgeConductor.NativeTools.ContractTests' = @('G13','T-UNIT')
    'ForgeConductor.NativeTools.HeaderSelfContainment' = @('G13','T-UNIT')
}
$expectedTimeouts = [ordered]@{
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests' = 180.0
    'ForgeConductor.NativeTools.FileSystemSearchTests' = 180.0
    'ForgeConductor.NativeTools.GitShellTests' = 180.0
    'ForgeConductor.NativeTools.GitShellIntegrationTests' = 60.0
    'ForgeConductor.NativeTools.PdfTests' = 180.0
    'ForgeConductor.NativeTools.WorkspaceAuthorityTests' = 180.0
    'ForgeConductor.NativeTools.ContractTests' = 180.0
    'ForgeConductor.NativeTools.HeaderSelfContainment' = 60.0
}
$buildRootForward = $buildRoot.Replace('\', '/')
foreach ($test in @($ctestInventory.tests)) {
    $testName = [string]$test.name
    Assert-Exact ([string]$test.config) 'Debug' "CTest configuration for $testName"
    $labelsProperty = @($test.properties | Where-Object name -ceq 'LABELS')
    Assert-Exact $labelsProperty.Count 1 "CTest label property count for $testName"
    Assert-Set @($labelsProperty[0].value) @($expectedLabels[$testName]) `
        "exact CTest labels for $testName"
    $workingProperty = @($test.properties | Where-Object {
        $_.name -ceq 'WORKING_DIRECTORY'
    })
    Assert-Exact $workingProperty.Count 1 `
        "CTest working-directory property count for $testName"
    Assert-Exact ([string]$workingProperty[0].value) $buildRootForward `
        "CTest working directory for $testName"

    if ($expectedTimeouts.Contains($testName)) {
        $timeoutProperty = @($test.properties | Where-Object name -ceq 'TIMEOUT')
        Assert-Exact $timeoutProperty.Count 1 `
            "CTest timeout property count for $testName"
        Assert-Exact ([double]$timeoutProperty[0].value) `
            ([double]$expectedTimeouts[$testName]) "CTest timeout for $testName"
    }

    $expectedExecutable = "$buildRootForward/bin/Debug/$testName.exe"
    if ($testName -ceq 'ForgeConductor.NativeTools.ContractTests') {
        $expectedExecutable = `
            "$buildRootForward/bin/Debug/ForgeConductor.Contracts.ContractTests.exe"
    }
    Assert-True (@($test.command).Count -ge 1) `
        "CTest command exists for $testName"
    Assert-Exact ([string]$test.command[0]) $expectedExecutable `
        "CTest executable for $testName"
    if ($testName -ceq 'ForgeConductor.Infrastructure.ProcessTests') {
        Assert-Exact (@($test.command).Count) 2 `
            'infrastructure process-test command count'
        Assert-Exact ([string]$test.command[1]) `
            "$buildRootForward/bin/Debug/ForgeConductor.ProcessFixture.exe" `
            'infrastructure process fixture path'
    } elseif ($testName -ceq `
        'ForgeConductor.NativeTools.GitShellIntegrationTests') {
        Assert-Exact (@($test.command).Count) 3 `
            'Git/shell integration command count'
        Assert-True ([IO.Path]::IsPathFullyQualified([string]$test.command[1])) `
            'Git integration executable path is absolute'
        Assert-True ([IO.Path]::IsPathFullyQualified([string]$test.command[2])) `
            'PowerShell integration executable path is absolute'
        Assert-True (Test-Path -LiteralPath ([string]$test.command[1]) -PathType Leaf) `
            'Git integration executable exists'
        Assert-True (Test-Path -LiteralPath ([string]$test.command[2]) -PathType Leaf) `
            'PowerShell integration executable exists'
        Assert-Exact ([IO.Path]::GetFileName([string]$test.command[1])) 'git.exe' `
            'Git integration executable leaf'
        Assert-Exact ([IO.Path]::GetFileName([string]$test.command[2])) 'pwsh.exe' `
            'PowerShell integration executable leaf'
    } else {
        Assert-Exact (@($test.command).Count) 1 `
            "CTest command count for $testName"
    }
}

$generatedHeaderRoot = Join-Path $buildRoot 'generated\p13-header-isolation'
$generatedHeaderSources = @(Get-ChildItem -LiteralPath $generatedHeaderRoot `
    -File -Filter '*.cpp')
Assert-Exact $generatedHeaderSources.Count 6 `
    'generated P13 isolated public-header translation-unit count'
$expectedHeaderSources = [ordered]@{}
foreach ($header in $expectedNativeHeaders) {
    $includePath = "ForgeConductor/NativeTools/Windows/$header"
    $stem = $includePath.Replace('/', '_').Replace('.', '_')
    $expectedHeaderSources["$stem.cpp"] =
        "#include <$includePath>" + [char]10 +
        'int ' + $stem + '_isolated() noexcept { return 0; }'
}
Assert-Set @($generatedHeaderSources.Name) @($expectedHeaderSources.Keys) `
    'exact generated P13 isolated-header translation-unit inventory'
foreach ($source in $generatedHeaderSources) {
    $sourceText = (Get-Content -Raw -LiteralPath $source.FullName).
        Replace([string][char]13 + [char]10, [string][char]10).
        TrimEnd([char[]]@([char]13,[char]10))
    Assert-Exact $sourceText $expectedHeaderSources[$source.Name] `
        "generated isolated-header translation unit $($source.Name)"
}

$artifacts = @(
    'lib/Debug/ForgeConductor.Domain.lib',
    'lib/Debug/ForgeConductor.Infrastructure.Windows.lib',
    'lib/Debug/ForgeConductor.NativeTools.Windows.lib',
    'bin/Debug/ForgeConductor.Domain.UnitTests.exe',
    'bin/Debug/ForgeConductor.Contracts.ContractTests.exe',
    'bin/Debug/ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'bin/Debug/ForgeConductor.Infrastructure.UnitTests.exe',
    'bin/Debug/ForgeConductor.Infrastructure.ShutdownTests.exe',
    'bin/Debug/ForgeConductor.Infrastructure.ProcessTests.exe',
    'bin/Debug/ForgeConductor.Infrastructure.HeaderSelfContainment.exe',
    'bin/Debug/ForgeConductor.ProcessFixture.exe',
    'bin/Debug/ForgeConductor.LegacyContinuityPersistence.WindowsTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.FileSystemSearchTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.GitShellTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.GitShellIntegrationTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.PdfTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.WorkspaceAuthorityTests.exe',
    'bin/Debug/ForgeConductor.NativeTools.HeaderSelfContainment.exe')
Assert-Exact $artifacts.Count 18 'G13 artifact inventory count'
$artifactHashes = [ordered]@{}
foreach ($relative in $artifacts) {
    $path = Join-Path $buildRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G13 artifact exists: $relative"
    Assert-True ([long](Get-Item -LiteralPath $path).Length -gt 0) `
        "G13 artifact nonempty: $relative"
    $hash = Get-FileSha256 $path
    Assert-Match $hash '^[0-9a-f]{64}$' "G13 artifact hash: $relative" `
        -CaseSensitive
    $artifactHashes[$relative] = $hash
    Write-Host "$relative SHA-256: $hash"
    if ($relative.EndsWith('.exe', [StringComparison]::Ordinal)) {
        Assert-X64PortableExecutable $path $relative
    }
}

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after full G13'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after full G13'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after full G13'
Invoke-RepositoryIntegrityChecks
Write-Host "G13 native-tools validation passed: $script:AssertionCount assertions; 4 retained G06 plus 8 G13 CTest registrations, $($artifactHashes.Count) artifact hashes, retained G12 static validation, and the single fresh x64 Debug build/test invocation succeeded."
