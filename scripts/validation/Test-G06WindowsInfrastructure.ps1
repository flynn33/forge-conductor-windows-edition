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
    if (-not $Condition) { throw "G06 assertion failed: $Message" }
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

function Assert-PatternOrder {
    param(
        [string]$Text,
        [string]$EarlierPattern,
        [string]$LaterPattern,
        [string]$Message
    )
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    $earlier = [regex]::Match($Text, $EarlierPattern, $options)
    $later = [regex]::Match($Text, $LaterPattern, $options)
    Assert-True $earlier.Success "$Message earlier anchor"
    Assert-True $later.Success "$Message later anchor"
    Assert-True ($earlier.Index -lt $later.Index) $Message
}

function Read-Json {
    param([string]$Path)
    try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
    catch { throw "G06 assertion failed: invalid JSON at $Path - $($_.Exception.Message)" }
}

function Assert-CrlfTextFile {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bareLfCount = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10 -and ($index -eq 0 -or $bytes[$index - 1] -ne 13)) {
            $bareLfCount++
        }
    }
    Assert-Exact $bareLfCount 0 "$Message bare-LF count"
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

function Get-MatchingRelativeFiles {
    param(
        [IO.FileInfo[]]$Files,
        [string]$Pattern
    )
    $matches = [Collections.Generic.List[string]]::new()
    foreach ($file in $Files) {
        $text = Get-Content -Raw -LiteralPath $file.FullName
        if ([regex]::IsMatch(
                $text,
                $Pattern,
                [Text.RegularExpressions.RegexOptions]::Multiline -bor
                    [Text.RegularExpressions.RegexOptions]::Singleline)) {
            $matches.Add(
                $file.FullName.Substring($WorkspaceRoot.Length + 1).Replace('\', '/'))
        }
    }
    return @($matches)
}

$requiredFiles = @(
    'CMakeLists.txt',
    'Directory.Build.props',
    'include/ForgeConductor/Domain/DiagnosticsModels.h',
    'include/ForgeConductor/Domain/ProcessModels.h',
    'include/ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h',
    'include/ForgeConductor/Infrastructure/Windows/DeadlineScheduler.h',
    'include/ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'include/ForgeConductor/Infrastructure/Windows/SecretRedactor.h',
    'include/ForgeConductor/Infrastructure/Windows/SystemClock.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h',
    'src/Infrastructure/Windows/Detail/DiagnosticDirectoryTree.h',
    'src/Infrastructure/Windows/Detail/DiagnosticRotationPublishObserver.h',
    'src/Infrastructure/Windows/Detail/ProcessLaunchObserver.h',
    'src/Infrastructure/Windows/Detail/RelativeFileOperations.h',
    'src/Infrastructure/Windows/Detail/RelativeFileOperations.cpp',
    'tests/Infrastructure/FoundationWindowsTests.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/ProcessIntegrationTestMain.cpp',
    'tests/Infrastructure/ShutdownWindowsTests.cpp',
    'tests/Infrastructure/StorageWindowsTests.cpp',
    'tests/Infrastructure/TestSupport.h',
    'tests/Infrastructure/WindowsDiagnosticSinkTests.cpp',
    'tests/Infrastructure/Windows/OverlappedPipeReaderTests.cpp',
    'tests/Infrastructure/Windows/ProcessFixture.cpp',
    'tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp',
    'tests/Architecture/P06HeaderSelfContainmentMain.cpp',
    '.forge-codex/state/decisions/P06-001-windows-root-layout-and-infrastructure-bounds.md',
    '.forge-codex/state/decisions/P06-002-versioned-config-atomic-replace-and-recovery.md',
    '.forge-codex/state/decisions/P06-003-current-user-dpapi-private-store.md',
    '.forge-codex/state/decisions/P06-004-diagnostics-etw-redaction-and-runtime-ownership.md',
    '.forge-codex/state/decisions/P06-005-job-object-process-cancellation-and-shutdown.md',
    '.forge-codex/state/decisions/P06-006-handle-relative-atomic-publish.md',
    '.forge-codex/state/decisions/P06-007-narrow-native-relative-file-wrapper.md',
    '.forge-codex/state/decisions/P06-008-handle-relative-diagnostic-rotation.md',
    '.forge-codex/state/decisions/P06-009-same-token-hard-link-publication-boundary.md',
    '.forge-codex/state/probes/p06-source-oplock-hardlink.cpp',
    '.forge-codex/state/probes/p06-source-oplock-hardlink.exe',
    '.forge-codex/state/probes/p06-posix-unlinked-link-ex.cpp',
    '.forge-codex/state/probes/p06-posix-unlinked-link-ex.exe',
    '.forge-codex/state/commands/20260826T005107667Z-cf49e950.json',
    '.forge-codex/state/commands/20260826T005120986Z-83e45bff.json',
    '.forge-codex/state/commands/20260826T005120986Z-83e45bff.stdout.txt',
    '.forge-codex/state/commands/20260826T010656506Z-af022f23.json',
    '.forge-codex/state/commands/20260826T010700102Z-b88fffe8.json',
    '.forge-codex/state/commands/20260826T010700102Z-b88fffe8.stdout.txt',
    '.forge-codex/instructions/plans/resource-budgets.json',
    'scripts/build.ps1',
    'scripts/test.ps1'
)
foreach ($relativePath in $requiredFiles) {
    Assert-True (Test-Path -LiteralPath (
        Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) -PathType Leaf) "required P06 file is missing: $relativePath"
}

$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile(
    $PSCommandPath,
    [ref]$tokens,
    [ref]$errors) | Out-Null
Assert-Exact $errors.Count 0 'G06 validator PowerShell parser errors'

$publicRoot = Join-Path $WorkspaceRoot 'include\ForgeConductor\Infrastructure\Windows'
$sourceRoot = Join-Path $WorkspaceRoot 'src\Infrastructure\Windows'
$detailRoot = Join-Path $sourceRoot 'Detail'
$testRoot = Join-Path $WorkspaceRoot 'tests\Infrastructure'
$testWindowsRoot = Join-Path $testRoot 'Windows'

$publicHeaders = @(Get-ChildItem -LiteralPath $publicRoot -File -Filter '*.h' | Sort-Object Name)
$topLevelSources = @(Get-ChildItem -LiteralPath $sourceRoot -File -Filter '*.cpp' | Sort-Object Name)
$detailHeaders = @(Get-ChildItem -LiteralPath $detailRoot -File -Filter '*.h' | Sort-Object Name)
$detailSources = @(Get-ChildItem -LiteralPath $detailRoot -File -Filter '*.cpp' | Sort-Object Name)
$testRootFiles = @(Get-ChildItem -LiteralPath $testRoot -File | Sort-Object Name)
$testWindowsFiles = @(Get-ChildItem -LiteralPath $testWindowsRoot -File | Sort-Object Name)

Assert-Set @($publicHeaders.Name) @(
    'BCryptSha256Hasher.h',
    'DeadlineScheduler.h',
    'DpapiSecureStorage.h',
    'InfrastructureWindows.h',
    'SecretRedactor.h',
    'SystemClock.h',
    'WindowsApplicationPaths.h',
    'WindowsAtomicFileStore.h',
    'WindowsConfigurationStore.h',
    'WindowsDiagnosticSink.h',
    'WindowsProcessSupervisor.h',
    'WindowsRuntimeDiagnostics.h',
    'WindowsUuidGenerator.h') 'P06 public Windows infrastructure header inventory'
Assert-Set @($topLevelSources.Name) @(
    'BCryptSha256Hasher.cpp',
    'DeadlineScheduler.cpp',
    'DpapiSecureStorage.cpp',
    'SecretRedactor.cpp',
    'SystemClock.cpp',
    'WindowsApplicationPaths.cpp',
    'WindowsAtomicFileStore.cpp',
    'WindowsConfigurationStore.cpp',
    'WindowsDiagnosticSink.cpp',
    'WindowsProcessSupervisor.cpp',
    'WindowsRuntimeDiagnostics.cpp',
    'WindowsUuidGenerator.cpp') 'P06 top-level implementation source inventory'
Assert-Set @($detailHeaders.Name) @(
    'AtomicReplaceEngine.h',
    'BoundedSerialExecutor.h',
    'CommandLineBuilder.h',
    'DiagnosticDirectoryTree.h',
    'DiagnosticRotationPublishObserver.h',
    'EtwProvider.h',
    'JobObject.h',
    'OperationContextGuard.h',
    'OperationGuard.h',
    'OverlappedPipeReader.h',
    'ProcessLaunchObserver.h',
    'RelativeFileOperations.h',
    'SecureBuffer.h',
    'UniqueBCryptHandle.h',
    'UniqueCoTaskMemAllocation.h',
    'UniqueHandle.h',
    'UniqueLocalAllocation.h',
    'UniqueRegistryKey.h',
    'UtfConversion.h',
    'Win32Error.h',
    'WindowsPathResolver.h') 'P06 private Detail header inventory'
Assert-Set @($detailSources.Name) @(
    'AtomicReplaceEngine.cpp',
    'BoundedSerialExecutor.cpp',
    'CommandLineBuilder.cpp',
    'EtwProvider.cpp',
    'JobObject.cpp',
    'OperationContextGuard.cpp',
    'OperationGuard.cpp',
    'OverlappedPipeReader.cpp',
    'RelativeFileOperations.cpp',
    'UniqueRegistryKey.cpp',
    'UtfConversion.cpp',
    'Win32Error.cpp',
    'WindowsPathResolver.cpp') 'P06 private Detail implementation inventory'
Assert-Set @((Get-ChildItem -LiteralPath $sourceRoot -Directory).Name) @('Detail') 'P06 source subdirectory inventory'
Assert-Set @($testRootFiles.Name) @(
    'FoundationWindowsTests.cpp',
    'InfrastructureTestMain.cpp',
    'ProcessIntegrationTestMain.cpp',
    'ShutdownWindowsTests.cpp',
    'StorageWindowsTests.cpp',
    'TestSupport.h',
    'WindowsDiagnosticSinkTests.cpp') 'P06 infrastructure test-root inventory'
Assert-Set @((Get-ChildItem -LiteralPath $testRoot -Directory).Name) @('Windows') 'P06 infrastructure test subdirectory inventory'
Assert-Set @($testWindowsFiles.Name) @(
    'OverlappedPipeReaderTests.cpp',
    'ProcessFixture.cpp',
    'WindowsProcessSupervisorTests.cpp') 'P06 Windows integration-test inventory'

$p06CrlfFiles = @(
    Get-Item -LiteralPath (Join-Path $WorkspaceRoot '.editorconfig'),
        (Join-Path $WorkspaceRoot 'CMakeLists.txt'),
        $PSCommandPath,
        (Join-Path $WorkspaceRoot 'tests\Architecture\P06HeaderSelfContainmentMain.cpp')
) + $publicHeaders + $topLevelSources + $detailHeaders + $detailSources +
    $testRootFiles + $testWindowsFiles +
    @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot '.forge-codex\state\decisions') -File -Filter 'P06-*.md')
foreach ($file in @($p06CrlfFiles | Sort-Object FullName -Unique)) {
    $relativePath = $file.FullName.Substring($WorkspaceRoot.Length + 1).Replace('\', '/')
    Assert-CrlfTextFile $file.FullName "P06 CRLF policy: $relativePath"
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
$sourceListMatch = [regex]::Match(
    $cmake,
    'set\s*\(\s*FORGE_INFRASTRUCTURE_WINDOWS_SOURCES(?<body>.*?)\)',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $sourceListMatch.Success 'CMake exact P06 source list'
$cmakeSources = @(
    $sourceListMatch.Groups['body'].Value -split '\r?\n' |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '\.cpp$' })
$expectedCmakeSources = @($topLevelSources | ForEach-Object {
    'src/Infrastructure/Windows/' + $_.Name
}) + @($detailSources | ForEach-Object {
    'src/Infrastructure/Windows/Detail/' + $_.Name
})
Assert-Set $cmakeSources $expectedCmakeSources 'CMake P06 implementation inventory'
Assert-Match $cmake 'add_library\s*\(\s*ForgeConductor\.Infrastructure\.Windows\s+STATIC\s+\$\{FORGE_INFRASTRUCTURE_WINDOWS_SOURCES\}\s*\)' 'P06 infrastructure is a static library' -CaseSensitive
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor\.Infrastructure\.Windows\s+ForgeConductor::Infrastructure\.Windows\s+ForgeConductor::Contracts\s*\)' 'P06 dependency direction terminates at Contracts' -CaseSensitive
Assert-Match $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Infrastructure\.Windows\s*\)' 'P06 native target policy' -CaseSensitive
$linkMatch = [regex]::Match(
    $cmake,
    'target_link_libraries\s*\(\s*ForgeConductor\.Infrastructure\.Windows\s+PRIVATE(?<body>.*?)\)',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $linkMatch.Success 'P06 exact native dependency block'
Assert-Set @($linkMatch.Groups['body'].Value -split '\s+' | Where-Object { $_ }) @(
    'nlohmann_json::nlohmann_json',
    'advapi32',
    'bcrypt',
    'crypt32',
    'ntdll',
    'ole32',
    'shell32') 'P06 exact approved native dependencies'

$publicText = ($publicHeaders | ForEach-Object {
    [Environment]::NewLine + '/* FILE: ' + $_.FullName + ' */' +
        [Environment]::NewLine + (Get-Content -Raw -LiteralPath $_.FullName)
}) -join [Environment]::NewLine
$implementationFiles = @($topLevelSources) + @($detailHeaders) + @($detailSources)
$implementationText = ($implementationFiles | ForEach-Object {
    [Environment]::NewLine + '/* FILE: ' + $_.FullName + ' */' +
        [Environment]::NewLine + (Get-Content -Raw -LiteralPath $_.FullName)
}) -join [Environment]::NewLine

Assert-NoMatch $publicText '(?im)^\s*#\s*include\s*[<"](?:windows\.h|wincrypt\.h|bcrypt\.h|evntprov\.h|evntrace\.h|shlobj\.h|processthreadsapi\.h|jobapi2?\.h)[>"]' 'native header leakage through P06 public headers'
Assert-NoMatch $publicText '(?im)^\s*#\s*include\s*[<"](?:[^>"]*[\\/])?(?:nlohmann|forsetti|boost|qt)(?:[\\/][^>"]*)?[>"]' 'third-party header leakage through P06 public headers'
Assert-NoMatch $publicText '\b(?:HANDLE|HKEY|REGHANDLE|HRESULT|DWORD|DATA_BLOB|PROCESS_INFORMATION|STARTUPINFOEXW|OVERLAPPED)\b' 'raw Windows ownership type leakage through P06 public headers' -CaseSensitive
Assert-NoMatch $publicText 'std::(?:filesystem|fstream|wstream)' 'platform path or stream leakage through P06 public headers' -CaseSensitive
$expectedPublicClasses = @(
    'BCryptSha256Hasher',
    'DeadlineScheduler',
    'DpapiSecureStorage',
    'SecretRedactor',
    'SystemClock',
    'WindowsApplicationPaths',
    'WindowsAtomicFileStore',
    'WindowsConfigurationStore',
    'WindowsDiagnosticSink',
    'WindowsProcessSupervisor',
    'WindowsRuntimeDiagnostics',
    'WindowsUuidGenerator')
$publicClassMatches = @([regex]::Matches(
    $publicText,
    '(?m)^\s*class\s+(?<name>[A-Za-z_]\w*)\s+final\b'))
Assert-Set @($publicClassMatches | ForEach-Object { $_.Groups['name'].Value }) $expectedPublicClasses 'P06 final public concrete class inventory'
foreach ($className in $expectedPublicClasses) {
    Assert-Match $publicText ('\bclass\s+' + [regex]::Escape($className) + '\s+final\b') "public concrete class $className is final" -CaseSensitive
}
Assert-NoMatch $publicText '(?:ServiceLocator|getInstance\s*\(|singleton)' 'hidden service locator or singleton public surface'
Assert-NoMatch $implementationText '(?:boost::|Qt::|QApplication|System::|gcnew|Python\.h|pybind|node_api|Electron)' 'unapproved infrastructure runtime dependency'
Assert-NoMatch $implementationText '\b(?:system|_wsystem|popen|_popen|ShellExecuteW?|WinExec|CreateProcessA|SearchPathA|SearchPathW)\s*\(' 'ambient shell or executable-search API'
Assert-NoMatch $implementationText '\b(?:cmd\.exe|powershell(?:\.exe)?)\b' 'ambient command interpreter dependency'
Assert-NoMatch $implementationText '\bCRYPTPROTECT_LOCAL_MACHINE\b' 'machine-scope DPAPI is prohibited' -CaseSensitive
Assert-NoMatch $implementationText '\bHKEY_LOCAL_MACHINE\b' 'machine-scope registry ownership is prohibited' -CaseSensitive
Assert-NoMatch $implementationText '\bstd::filesystem\b' 'production infrastructure must use explicit Windows path authority' -CaseSensitive
Assert-NoMatch $implementationText '(?m)^\s*(?:inline\s+)?static\s+(?!constexpr\b|const\b)[^(\r\n]*[;=]' 'process-wide mutable static infrastructure state'

$umbrella = Get-Content -Raw -LiteralPath (Join-Path $publicRoot 'InfrastructureWindows.h')
foreach ($header in @($publicHeaders | Where-Object Name -ne 'InfrastructureWindows.h')) {
    Assert-Match $umbrella ('#include\s+"ForgeConductor/Infrastructure/Windows/' +
        [regex]::Escape($header.Name) + '"') "P06 umbrella missing $($header.Name)" -CaseSensitive
}

Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bCloseHandle\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueHandle.h') 'CloseHandle RAII owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bRegCloseKey\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueRegistryKey.cpp') 'RegCloseKey RAII owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bCoTaskMemFree\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueCoTaskMemAllocation.h') 'CoTaskMemFree RAII owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bLocalFree\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueLocalAllocation.h',
    'src/Infrastructure/Windows/DpapiSecureStorage.cpp') 'LocalFree typed owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bBCryptCloseAlgorithmProvider\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueBCryptHandle.h',
    'src/Infrastructure/Windows/DpapiSecureStorage.cpp') 'BCrypt algorithm owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bBCryptDestroyHash\s*\(') @(
    'src/Infrastructure/Windows/Detail/UniqueBCryptHandle.h') 'BCrypt hash owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bDeleteProcThreadAttributeList\s*\(') @(
    'src/Infrastructure/Windows/WindowsProcessSupervisor.cpp') 'STARTUPINFOEX attribute-list owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bEventUnregister\s*\(') @(
    'src/Infrastructure/Windows/Detail/EtwProvider.cpp') 'ETW registration owner inventory'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bCreateProcessW\s*\(') @(
    'src/Infrastructure/Windows/WindowsProcessSupervisor.cpp') 'production process-creation boundary inventory'
Assert-Exact ([regex]::Matches(
    (Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'WindowsProcessSupervisor.cpp')),
    '\bCreateProcessW\s*\(').Count) 1 'one production CreateProcessW call'


$decisionSpecifications = @(
    [ordered]@{
        file = 'P06-001-windows-root-layout-and-infrastructure-bounds.md'
        anchors = @(
            '%LOCALAPPDATA%\Forge Conductor',
            'allowEnvironmentOverride',
            'Configuration documents are schema 1, at most 2 MiB, and at most 32 JSON levels deep',
            'Atomic-file payloads are at most 32 MiB',
            'Secure storage admits 128-byte keys, 64 KiB secrets, and 128 entries',
            'Diagnostic rings retain at most 4,000 records',
            'Process supervision admits 64 concurrent operations')
    },
    [ordered]@{
        file = 'P06-002-versioned-config-atomic-replace-and-recovery.md'
        anchors = @(
            'Parsing rejects duplicate keys',
            'A corrupt primary recovers only from a valid backup',
            'only then publishes the immutable in-memory snapshot',
            'CREATE_NEW',
            'FlushFileBuffers',
            'P06-006-handle-relative-atomic-publish.md',
            'Cancellation observed after successful replacement returns success')
    },
    [ordered]@{
        file = 'P06-003-current-user-dpapi-private-store.md'
        anchors = @(
            'CryptProtectData',
            'CryptUnprotectData',
            'CRYPTPROTECT_UI_FORBIDDEN',
            '`CRYPTPROTECT_LOCAL_MACHINE` is prohibited',
            'application-owned HKCU subkey',
            'SHA-256 hashed for value names',
            'SecureZeroMemory')
    },
    [ordered]@{
        file = 'P06-004-diagnostics-etw-redaction-and-runtime-ownership.md'
        anchors = @(
            'redacted before it enters memory, JSONL, export, or ETW-derived data',
            'EventRegister',
            'EventUnregister',
            'emits fixed numeric severity/category/process/timestamp descriptors only',
            'active file counts toward the exact 5/8/10 file total',
            'deterministic JSON and Markdown artifacts',
            'weak control state',
            'process and reader capacities are 64 and 128')
    },
    [ordered]@{
        file = 'P06-005-job-object-process-cancellation-and-shutdown.md'
        anchors = @(
            'at most 64 concurrent operations',
            'non-null absolute',
            'lpApplicationName',
            'PROC_THREAD_ATTRIBUTE_HANDLE_LIST',
            'CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT',
            'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE',
            '80,000/20,000 bytes',
            'TerminateJobObject',
            'process_termination_unconfirmed',
            'adopted into typed RAII owners immediately after `CreateProcessW` returns',
            'Observer injection is available only through a private Detail test-access factory')
    },
    [ordered]@{
        file = 'P06-006-handle-relative-atomic-publish.md'
        anchors = @(
            'Status: Accepted',
            'NtSetInformationFile',
            'FileRenameInformationEx',
            'FILE_RENAME_FLAG_POSIX_SEMANTICS',
            'FILE_RENAME_FLAG_REPLACE_IF_EXISTS',
            'FILE_SUPPORTS_POSIX_UNLINK_RENAME',
            '`RootDirectory == nullptr`',
            'read-and-delete sharing',
            'byte-for-byte recovery copy',
            'DACL semantics must be preserved',
            'p06-posix-unlinked-link-ex.cpp',
            '`SetFileInformationByHandle(FileRenameInfo/FileRenameInfoEx)`, `ReplaceFileW`,',
            'Handle-bound `FileBasicInfo` and',
            '`FileDispositionInfo` remain permitted')
    },
    [ordered]@{
        file = 'P06-007-narrow-native-relative-file-wrapper.md'
        anchors = @(
            'Windows 11 from build 22000',
            'require Windows 11 version 24H2',
            '`Infrastructure::Windows::Detail::openRelative`',
            'Link `ntdll` privately',
            'accepts exactly one validated path component',
            'Every open forces `FILE_OPEN_REPARSE_POINT`',
            'Full-path creation is')
    },
    [ordered]@{
        file = 'P06-008-handle-relative-diagnostic-rotation.md'
        anchors = @(
            'final diagnostic root',
            '`FILE_SHARE_READ`',
            'bounded 64 KiB',
            'one of sixteen reserved',
            'native class-65',
            'flags `0`',
            'Any handled failure disposition-deletes',
            'leaves the source intact',
            'more than one hard link or delete-pending state',
            'startup-cleaned',
            'P06-009-same-token-hard-link-publication-boundary.md')
    },
    [ordered]@{
        file = 'P06-009-same-token-hard-link-publication-boundary.md'
        anchors = @(
            'Risk disposition: Accepted; architectural mitigation deferred to P22',
            'RWH oplock',
            'POSIX-unlinked',
            'outside the Forge service-authority boundary',
            'No claim of containment',
            'P22',
            '.forge-codex/state/probes/p06-source-oplock-hardlink.cpp',
            '.forge-codex/state/commands/20260826T005120986Z-83e45bff.json',
            '.forge-codex/state/probes/p06-posix-unlinked-link-ex.cpp',
            '.forge-codex/state/commands/20260826T010700102Z-b88fffe8.json')
    })
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
foreach ($specification in $decisionSpecifications) {
    $decisionPath = Join-Path $decisionRoot $specification.file
    $decision = Get-Content -Raw -LiteralPath $decisionPath
    Assert-Match $decision '(?m)^Status:\s+Accepted\s*$' "$($specification.file) is accepted" -CaseSensitive
    Assert-Match $decision '(?m)^Date:\s+2026-08-25\s*$' "$($specification.file) decision date" -CaseSensitive
    foreach ($anchor in $specification.anchors) {
        Assert-Match $decision ([regex]::Escape($anchor)) "$($specification.file) decision anchor: $anchor" -CaseSensitive
    }
}

$hardLinkProbeRoot = Join-Path $WorkspaceRoot '.forge-codex\state\probes'
$hardLinkCommandRoot = Join-Path $WorkspaceRoot '.forge-codex\state\commands'
Assert-Exact (Get-FileSha256 (
        Join-Path $hardLinkProbeRoot 'p06-source-oplock-hardlink.cpp')) `
    '7078b221c84bc9a06824e28efb0c7db2ef5a7894edcf3ff124cee0821341304a' `
    'P06 source-oplock probe source hash'
Assert-Exact (Get-FileSha256 (
        Join-Path $hardLinkProbeRoot 'p06-source-oplock-hardlink.exe')) `
    'c25753c46288c0cd59067b27e25843921cafeb5eaccb3cb837a58644b7a575dd' `
    'P06 source-oplock probe binary hash'
$hardLinkBuildRecord = Read-Json (
    Join-Path $hardLinkCommandRoot '20260826T005107667Z-cf49e950.json')
$hardLinkRunRecord = Read-Json (
    Join-Path $hardLinkCommandRoot '20260826T005120986Z-83e45bff.json')
Assert-Exact ([int]$hardLinkBuildRecord.exit_code) 0 'P06 source-oplock probe build exit code'
Assert-Exact ([int]$hardLinkRunRecord.exit_code) 0 'P06 source-oplock probe run exit code'
Assert-Exact ([string]$hardLinkBuildRecord.working_directory) $WorkspaceRoot `
    'P06 source-oplock probe build working directory'
Assert-Exact ([string]$hardLinkRunRecord.working_directory) $WorkspaceRoot `
    'P06 source-oplock probe run working directory'
$hardLinkProbeOutputPath = Join-Path $hardLinkCommandRoot `
    '20260826T005120986Z-83e45bff.stdout.txt'
Assert-Exact (Get-FileSha256 $hardLinkProbeOutputPath) `
    'd28c2d102509437fca597cc4101c7f6ce4c62ecfeed425a3097cdd3b7fd325f1' `
    'P06 source-oplock probe output hash'
$hardLinkProbeOutput = Get-Content -Raw -LiteralPath $hardLinkProbeOutputPath
Assert-Match $hardLinkProbeOutput 'OPLOCK request=ERROR_IO_PENDING link_wait=0 link_succeeded=1 link_error=0 oplock_wait=258' 'P06 probe proves a source oplock does not serialize a fresh hard link' -CaseSensitive
Assert-Match $hardLinkProbeOutput 'POSIX_RENAME name_removed=1 clear_succeeded=0 clear_error=5 rename_succeeded=0 rename_error=5' 'P06 probe rejects rename publication from a POSIX-unlinked surviving handle' -CaseSensitive
Assert-Match $hardLinkProbeOutput 'POSIX_LINK name_removed=1 link_status=0xC0000022 destination_exists=0' 'P06 probe rejects relinking a POSIX-unlinked surviving handle' -CaseSensitive

Assert-Exact (Get-FileSha256 (
        Join-Path $hardLinkProbeRoot 'p06-posix-unlinked-link-ex.cpp')) `
    '6e7978b22d3ca1d0422c10e3cb461a22d265e5a05165c6ab4ec5a4acfaaa399e' `
    'P06 class-72 POSIX-unlink probe source hash'
Assert-Exact (Get-FileSha256 (
        Join-Path $hardLinkProbeRoot 'p06-posix-unlinked-link-ex.exe')) `
    'ab579cc420b619f009b0274648f99707d61ffa70e727d7e4aeaa7263faadcda6' `
    'P06 class-72 POSIX-unlink probe binary hash'
$linkExBuildRecord = Read-Json (
    Join-Path $hardLinkCommandRoot '20260826T010656506Z-af022f23.json')
$linkExRunRecord = Read-Json (
    Join-Path $hardLinkCommandRoot '20260826T010700102Z-b88fffe8.json')
Assert-Exact ([int]$linkExBuildRecord.exit_code) 0 'P06 class-72 probe build exit code'
Assert-Exact ([int]$linkExRunRecord.exit_code) 0 'P06 class-72 probe run exit code'
Assert-Exact ([string]$linkExBuildRecord.working_directory) $WorkspaceRoot `
    'P06 class-72 probe build working directory'
Assert-Exact ([string]$linkExRunRecord.working_directory) $WorkspaceRoot `
    'P06 class-72 probe run working directory'
$linkExProbeOutputPath = Join-Path $hardLinkCommandRoot `
    '20260826T010700102Z-b88fffe8.stdout.txt'
Assert-Exact (Get-FileSha256 $linkExProbeOutputPath) `
    '3b69a8d53c0305d93b0a28ace79e8a3419132ba932156e88a00acce3073bf4f3' `
    'P06 class-72 probe output hash'
$linkExProbeOutput = Get-Content -Raw -LiteralPath $linkExProbeOutputPath
Assert-Match $linkExProbeOutput 'flags-zero[\s\S]*?link_status=0xC0000022[\s\S]*?clear_succeeded=0 clear_error=5' 'P06 class-72 flags-zero relink is rejected after POSIX unlink' -CaseSensitive
Assert-Match $linkExProbeOutput 'flags-posix[\s\S]*?link_status=0xC0000022[\s\S]*?clear_succeeded=0 clear_error=5' 'P06 class-72 POSIX relink is rejected after POSIX unlink' -CaseSensitive

$applicationPathsHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsApplicationPaths.h')
$applicationPathsSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsApplicationPaths.cpp')
$atomicHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsAtomicFileStore.h')
$atomicSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'AtomicReplaceEngine.cpp')
$atomicEngineHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'AtomicReplaceEngine.h')
$pathResolverHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'WindowsPathResolver.h')
$pathResolverSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'WindowsPathResolver.cpp')
$relativeOperationsHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'RelativeFileOperations.h')
$relativeOperationsSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'RelativeFileOperations.cpp')
$configurationHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsConfigurationStore.h')
$configurationSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsConfigurationStore.cpp')
$dpapiHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'DpapiSecureStorage.h')
$dpapiSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'DpapiSecureStorage.cpp')
$registrySource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'UniqueRegistryKey.cpp')
$diagnosticHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsDiagnosticSink.h')
$runtimeHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsRuntimeDiagnostics.h')
$processModels = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain\ProcessModels.h')
$diagnosticModels = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain\DiagnosticsModels.h')
$secretRedactorHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'SecretRedactor.h')
$executorHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'BoundedSerialExecutor.h')

Assert-Match $atomicHeader 'MaximumContentBytes\s*=\s*32U\s*\*\s*1024U\s*\*\s*1024U\s*;' 'atomic payload hard bound is 32 MiB' -CaseSensitive
Assert-Match $configurationHeader 'MaximumDocumentBytes\s*=\s*2U\s*\*\s*1024U\s*\*\s*1024U\s*;' 'configuration byte hard bound is 2 MiB' -CaseSensitive
Assert-Match $configurationHeader 'MaximumJsonDepth\s*=\s*32U\s*;' 'configuration JSON depth hard bound is 32' -CaseSensitive
Assert-Match $configurationHeader 'SchemaVersion\s*=\s*1U\s*;' 'configuration schema is version 1' -CaseSensitive
Assert-Match $dpapiHeader 'MaximumKeyBytes\s*=\s*128U\s*;' 'DPAPI key hard bound is 128 bytes' -CaseSensitive
Assert-Match $dpapiHeader 'MaximumSecretBytes\s*=\s*64U\s*\*\s*1024U\s*;' 'DPAPI secret hard bound is 64 KiB' -CaseSensitive
Assert-Match $dpapiHeader 'MaximumEntryCount\s*=\s*128U\s*;' 'DPAPI entry hard bound is 128' -CaseSensitive
Assert-Match $secretRedactorHeader 'MaximumInputBytes\s*=\s*256U\s*\*\s*1024U\s*;' 'redactor input hard bound is 256 KiB' -CaseSensitive
Assert-Match $diagnosticHeader 'MaximumExportBasenameBytes\s*=\s*128U\s*;' 'diagnostic export basename hard bound' -CaseSensitive
Assert-Match $diagnosticHeader 'MaximumRetainedLogFiles\s*=\s*10U\s*;' 'diagnostic retained-file hard bound' -CaseSensitive
Assert-Match $diagnosticHeader 'MaximumRetainedRecords\s*=\s*Domain::MaximumDiagnosticRingRecords\s*;' 'diagnostic ring reuses Domain bound' -CaseSensitive
Assert-Match $runtimeHeader 'MaximumGenericOwnershipCount\s*=\s*4''096U\s*;' 'runtime generic-owner hard bound' -CaseSensitive
Assert-Match $runtimeHeader 'MaximumProcessReaderCount\s*=\s*2U\s*\*\s*Domain::MaximumConcurrentProcessOperations\s*;' 'runtime process-reader bound derives from process cap' -CaseSensitive
Assert-Match $executorHeader 'MaximumPendingOperationCount\s*=\s*16U\s*;' 'bounded serial executor admission cap' -CaseSensitive
Assert-Match $processModels 'MaximumConcurrentProcessOperations\s*=\s*64U\s*;' 'Domain process admission cap is 64' -CaseSensitive
Assert-Match $processModels 'MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator\s*=\s*32''767U\s*;' 'native command-line cap includes terminator' -CaseSensitive
Assert-Match $processModels 'MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators\s*=\s*32''767U\s*;' 'Forge environment-block cap includes terminators' -CaseSensitive
Assert-Match $processModels 'bool\s+inheritEnvironment\s*\{\s*false\s*\}\s*;' 'ambient process environment inheritance is disabled by default' -CaseSensitive
Assert-Match $diagnosticModels 'MaximumDiagnosticFlattenedFieldBytes\s*=\s*512U\s*;' 'diagnostic flattened-field cap is 512' -CaseSensitive
Assert-Match $diagnosticModels 'MaximumDiagnosticRingRecords\s*=\s*4''000U\s*;' 'diagnostic ring cap is 4000' -CaseSensitive

$budgetPath = Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\resource-budgets.json'
Assert-Exact (Get-FileSha256 $budgetPath) 'f80c5d57081d47b87ddb77027f843c912bcf3c3e558c7ade42b4db4828760965' 'authoritative resource-budget hash'
$budgets = Read-Json $budgetPath
$budgetExpectations = @(
    [ordered]@{ name = 'constrained_8gb'; logs = 5; bytes = 4194304; repositories = 4; threads = 32 },
    [ordered]@{ name = 'standard_16gb'; logs = 8; bytes = 8388608; repositories = 8; threads = 40 },
    [ordered]@{ name = 'expanded_32gb_plus'; logs = 10; bytes = 10485760; repositories = 16; threads = 48 })
foreach ($expectation in $budgetExpectations) {
    $profile = $budgets.profiles.($expectation.name)
    Assert-Exact ([int]$profile.diagnostic_log_files_max) ([int]$expectation.logs) "$($expectation.name) diagnostic file cap"
    Assert-Exact ([int]$profile.diagnostic_log_file_bytes_max) ([int]$expectation.bytes) "$($expectation.name) diagnostic byte cap"
    Assert-Exact ([int]$profile.open_project_repositories_max) ([int]$expectation.repositories) "$($expectation.name) repository cap"
    Assert-Exact ([int]$profile.manager_threads_max) ([int]$expectation.threads) "$($expectation.name) manager thread cap"
    Assert-Exact ([int]$profile.telemetry_pending_snapshots_max) 1 "$($expectation.name) telemetry mailbox cap"
    Assert-Exact ([int]$profile.tool_stdout_bytes_max) 80000 "$($expectation.name) stdout cap"
    Assert-Exact ([int]$profile.tool_stderr_bytes_max) 20000 "$($expectation.name) stderr cap"
}

Assert-Match $applicationPathsHeader 'std::optional\s*<\s*Domain::PathText\s*>\s+explicitDataRoot\s*;' 'explicit injected application root seam' -CaseSensitive
Assert-Match $applicationPathsHeader 'bool\s+allowEnvironmentOverride\s*\{\s*\}\s*;' 'opt-in environment override seam' -CaseSensitive
Assert-Match $applicationPathsSource 'if\s*\(\s*options\.explicitDataRoot\.has_value\s*\(\s*\)\s*\)' 'explicit root has first precedence' -CaseSensitive
Assert-Match $applicationPathsSource 'if\s*\(\s*options\.allowEnvironmentOverride\s*\)\s*\{[\s\S]*?environmentOverride\s*\(\s*\)' 'environment root is sampled only behind the injected opt-in' -CaseSensitive
Assert-Match $applicationPathsSource 'SHGetKnownFolderPath\s*\(\s*FOLDERID_LocalAppData' 'default root uses FOLDERID_LocalAppData' -CaseSensitive
Assert-Match $applicationPathsSource 'UniqueCoTaskMemAllocation\s*<\s*wchar_t\s*>\s+localAppData' 'known-folder allocation has typed RAII ownership' -CaseSensitive
Assert-Match $applicationPathsSource 'ProductDirectoryName\[\]\s*=\s*L"Forge Conductor"' 'default per-user product directory' -CaseSensitive
Assert-Match $applicationPathsSource 'WindowsApplicationPaths::configurationRoot[\s\S]*?return\s+childFor\s*\(\s*context\s*,\s*L"config"' 'configuration root is the owned config child' -CaseSensitive
Assert-Match $applicationPathsSource 'WindowsApplicationPaths::diagnosticsRoot[\s\S]*?return\s+childFor\s*\(\s*context\s*,\s*L"logs"' 'diagnostic root is the owned logs child' -CaseSensitive
Assert-Match $applicationPathsSource 'std::wstring\s+relative\s*\{\s*L"projects\\\\"\s*\}' 'project roots are owned children' -CaseSensitive

Assert-NoMatch $relativeOperationsHeader '\b(?:NTSTATUS|OBJECT_ATTRIBUTES|IO_STATUS_BLOCK|UNICODE_STRING)\b|winternl\.h' 'native NT types do not leak through the relative-file wrapper header' -CaseSensitive
Assert-Match $relativeOperationsHeader 'enum\s+class\s+RelativeOpenDisposition[\s\S]*?OpenExisting[\s\S]*?CreateNew[\s\S]*?OpenOrCreate' 'relative-file wrapper exposes bounded dispositions' -CaseSensitive
Assert-Match $relativeOperationsHeader 'enum\s+class\s+RelativeObjectType[\s\S]*?File[\s\S]*?Directory' 'relative-file wrapper exposes bounded object types' -CaseSensitive
Assert-Match $relativeOperationsSource '#include\s*<winternl\.h>' 'native relative-file API is isolated to one private translation unit' -CaseSensitive
Assert-Exact ([regex]::Matches($relativeOperationsSource, '\bNtCreateFile\s*\(').Count) 1 'one native relative create/open boundary'
Assert-Exact ([regex]::Matches($relativeOperationsSource, '\bRtlNtStatusToDosError\s*\(').Count) 1 'one native-status conversion boundary'
Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bNtCreateFile\s*\(') @(
    'src/Infrastructure/Windows/Detail/RelativeFileOperations.cpp') 'NtCreateFile private-wrapper inventory'
Assert-Match $relativeOperationsSource 'validOneComponentName[\s\S]*?value\s*==\s*L"\."[\s\S]*?value\s*==\s*L"\.\."[\s\S]*?L''\\\\''[\s\S]*?L''/''[\s\S]*?L'':''[\s\S]*?L''\*''' 'relative-file wrapper rejects traversal, separators, streams, and wildcards' -CaseSensitive
Assert-Match $relativeOperationsSource 'equals\s*\(\s*L"CON"\s*\)[\s\S]*?equals\s*\(\s*L"CONOUT\$"\s*\)[\s\S]*?L"COM"[\s\S]*?L"LPT"' 'relative-file wrapper rejects reserved DOS device basenames' -CaseSensitive
Assert-Match $relativeOperationsSource 'InitializeObjectAttributes\s*\([\s\S]*?rootDirectory[\s\S]*?NtCreateFile\s*\(' 'relative-file wrapper binds the child to the retained root handle' -CaseSensitive
Assert-Match $relativeOperationsSource 'FILE_OPEN_REPARSE_POINT\s*\|\s*FILE_SYNCHRONOUS_IO_NONALERT' 'every relative open targets the final reparse object synchronously' -CaseSensitive
Assert-NoMatch $implementationText '\bFILE_OPEN_FOR_BACKUP_INTENT\b' 'relative namespace callers do not request backup-intent privilege semantics' -CaseSensitive

Assert-Match $atomicSource 'class\s+PendingTemporaryFile\s+final' 'atomic temporary file has a typed RAII owner' -CaseSensitive
Assert-Match $atomicSource '~PendingTemporaryFile\s*\(\s*\)[\s\S]*?cleanupRequired_[\s\S]*?SetFileInformationByHandle\s*\([\s\S]*?FileDispositionInfo' 'uncommitted temporary files are disposition-deleted through their retained handles' -CaseSensitive
Assert-NoMatch $atomicSource '\b(?:DeleteFileW|MoveFileExW|ReplaceFileW)\s*\(' 'atomic namespace mutation never reopens a path' -CaseSensitive
Assert-Match $atomicSource 'createTemporaryFile[\s\S]*?options\.shareAccess\s*=\s*0U\s*;[\s\S]*?RelativeOpenDisposition::CreateNew[\s\S]*?options\.writeThrough\s*=\s*true\s*;[\s\S]*?openRelative\s*\(\s*parentDirectory\s*,\s*name\s*,\s*options\s*\)' 'same-directory atomic temporary uses exclusive handle-relative CREATE_NEW and write-through' -CaseSensitive
Assert-Match $atomicSource 'writeBytes[\s\S]*?WriteFile[\s\S]*?FlushFileBuffers\s*\(\s*destination\s*\)' 'replacement content is flushed before commit' -CaseSensitive
Assert-Match $atomicSource 'copyAndFlush[\s\S]*?ReadFile[\s\S]*?WriteFile[\s\S]*?FlushFileBuffers\s*\(\s*destination\s*\)' 'recovery-backup content is bounded-copied and flushed before publication' -CaseSensitive
Assert-Match $atomicSource 'constexpr\s+ULONG\s+NativeFileRenameInformationEx\s*=\s*65U\s*;' 'native publish uses the documented FileRenameInformationEx class' -CaseSensitive
Assert-Exact ([regex]::Matches($atomicSource, '\bntSetInformationFile\s*\(').Count) 1 'one native atomic rename call boundary'
Assert-Match $atomicSource 'ntSetInformationFile\s*\([\s\S]*?NativeFileRenameInformationEx\s*\)' 'native publish always requests FileRenameInformationEx' -CaseSensitive
Assert-Match $atomicSource 'information->Flags\s*=\s*replaceExisting\s*\?\s*FILE_RENAME_FLAG_REPLACE_IF_EXISTS\s*\|\s*FILE_RENAME_FLAG_POSIX_SEMANTICS\s*:\s*0U\s*;' 'replace and create publication use exact native rename flags' -CaseSensitive
Assert-Match $atomicSource 'information->RootDirectory\s*=\s*nullptr\s*;[\s\S]*?information->FileNameLength[\s\S]*?destinationName' 'same-directory native publish uses a simple name with null RootDirectory' -CaseSensitive
Assert-Match $atomicSource 'publishTemporary[\s\S]*?Verify atomic staged identity[\s\S]*?verifyCurrentIdentity[\s\S]*?beforePublish\s*\(\s*destinationName\s*\)[\s\S]*?validateOperationContext[\s\S]*?renameFile\s*\(\s*temporary\.handle\s*\(\s*\)' 'atomic publish rechecks stage, destination, and operation context immediately before native rename' -CaseSensitive
Assert-Match $atomicSource 'beforePublish\s*\(\s*destinationName\s*\)[\s\S]*?readAtomicFileIdentity\s*\(\s*temporary\.handle\s*\(\s*\)[\s\S]*?Final atomic staged identity check[\s\S]*?renameFile\s*\(\s*temporary\.handle\s*\(\s*\)' 'atomic publish repeats complete staged-object validation after its deterministic hook and before native rename' -CaseSensitive
Assert-PatternOrder $atomicSource 'const\s+auto\s+renamed\s*=\s*nativeOperations\.renameFile\s*\(' 'temporary\.markCommitted\s*\(\s*\)' 'native publication precedes temporary ownership release'
Assert-Match $atomicSource '!existing\s*&&\s*path\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Write[\s\S]*?RecordNotFound' 'Write authority cannot create a missing target' -CaseSensitive
Assert-Match $atomicSource 'existing\s*&&\s*path\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Create[\s\S]*?Unauthorized' 'Create authority cannot replace an existing target' -CaseSensitive
Assert-Match $atomicSource 'GetFinalPathNameByHandleW' 'atomic target is verified by opened final path' -CaseSensitive
Assert-Match $atomicSource 'openRelative\s*\(' 'atomic file operations use the shared final-reparse relative-open boundary' -CaseSensitive
Assert-Match $atomicSource 'openReplacementTarget[\s\S]*?options\.shareAccess\s*=\s*FILE_SHARE_READ\s*\|\s*FILE_SHARE_DELETE\s*;' 'retained target and backup handles permit POSIX replacement while denying writers' -CaseSensitive
Assert-Match $atomicSource 'constexpr\s+DWORD\s+RejectedAttributes\s*=\s*FILE_ATTRIBUTE_REPARSE_POINT[\s\S]*?FILE_ATTRIBUTE_COMPRESSED[\s\S]*?FILE_ATTRIBUTE_ENCRYPTED' 'atomic metadata policy rejects reparse, compressed, and encrypted files' -CaseSensitive
Assert-Match $atomicSource 'validateDefaultDataStreamOnly[\s\S]*?FileStreamInfo[\s\S]*?rejects files with alternate data streams' 'atomic metadata policy rejects alternate data streams' -CaseSensitive
Assert-Match $atomicSource 'applyAndVerifyMetadata[\s\S]*?SetSecurityInfo[\s\S]*?FileBasicInfo[\s\S]*?metadata\.equivalentTo' 'atomic replacement applies and verifies DACL and creation time' -CaseSensitive
Assert-PatternOrder $atomicSource 'const\s+auto\s+feature\s*=\s*nativeOperations_->queryPosixRenameSupport' 'std::optional\s*<\s*PendingTemporaryFile\s*>\s+backupTemporary' 'POSIX replacement capability is checked before any replacement staging'
Assert-PatternOrder $atomicSource 'auto\s+publishedBackup\s*=\s*publishTemporary\s*\(' 'auto\s+publishedTarget\s*=\s*publishTemporary\s*\(' 'recovery backup publishes before target linearization'
Assert-Match $atomicSource 'beforePublish\s*\(\s*destinationName\s*\)[\s\S]*?validateOperationContext[\s\S]*?renameFile' 'cancellation and deadline are honored at the final prepublish boundary' -CaseSensitive
Assert-Match $atomicSource 'handle-relative rename is the linearization point[\s\S]*?Cancellation[\s\S]*?racing after[\s\S]*?returns success' 'late cancellation preserves committed atomic success' -CaseSensitive
Assert-Match $atomicEngineHeader 'MaximumTemporaryNameAttempts\s*=\s*32U\s*;' 'atomic temporary collision retry bound is exactly 32' -CaseSensitive
Assert-Match $atomicSource 'BCryptGenRandom\s*\([\s\S]*?BCRYPT_USE_SYSTEM_PREFERRED_RNG' 'atomic temporary names use the system CSPRNG' -CaseSensitive
Assert-Match $atomicSource 'createTemporaryFile[\s\S]*?MaximumTemporaryNameAttempts[\s\S]*?randomBytes\s*\([\s\S]*?RelativeOpenDisposition::CreateNew[\s\S]*?verifyOpenedFile\s*\(' 'atomic temporary creation retries fresh entropy and verifies the opened final path' -CaseSensitive
Assert-Match $pathResolverHeader 'class\s+AnchoredAuthorizedPath\s+final[\s\S]*?AnchoredAuthorizedPath\s*\(\s*const\s+AnchoredAuthorizedPath\s*&\s*\)\s*=\s*delete[\s\S]*?std::vector\s*<\s*UniqueHandle\s*>\s+directoryAnchors_' 'authorized atomic ancestry is retained by a move-only handle owner' -CaseSensitive
Assert-Match $pathResolverSource 'openDirectoryAnchor[\s\S]*?FILE_LIST_DIRECTORY\s*\|\s*FILE_TRAVERSE\s*\|\s*FILE_READ_ATTRIBUTES[\s\S]*?FILE_ADD_FILE\s*\|\s*FILE_DELETE_CHILD[\s\S]*?FILE_SHARE_READ\s*,\s*nullptr[\s\S]*?FILE_FLAG_BACKUP_SEMANTICS\s*\|\s*FILE_FLAG_OPEN_REPARSE_POINT' 'atomic ancestry anchors retain strong traversal and final-parent child-mutation rights' -CaseSensitive
Assert-NoMatch ([regex]::Match($pathResolverSource, 'openDirectoryAnchor[\s\S]*?anchorExistingParentDirectories').Value) 'FILE_SHARE_(?:WRITE|DELETE)' 'atomic ancestry anchors prohibit mutation and delete sharing' -CaseSensitive
Assert-Match $pathResolverSource 'resolveAnchoredAuthorizedPath[\s\S]*?resolveAuthorizedPath[\s\S]*?anchorExistingParentDirectories[\s\S]*?verifyExistingAncestry' 'authorized atomic ancestry is revalidated after parent handles are pinned' -CaseSensitive
Assert-Match $atomicSource 'AtomicReplaceEngine::replace[\s\S]*?resolveAnchoredAuthorizedPath[\s\S]*?resolved\.value\s*\(\s*\)\.canonicalPath[\s\S]*?resolved\.value\s*\(\s*\)\.parentDirectoryHandle[\s\S]*?publishTemporary\s*\(' 'atomic replacement retains anchored authorization through its commit' -CaseSensitive

Assert-Match $configurationHeader 'WindowsConfigurationStore\s*\(\s*Contracts::IAtomicFileStore&\s+atomicFileStore\s*,\s*Contracts::AuthorizedPath\s+readPath\s*,\s*Contracts::AuthorizedPath\s+writePath\s*,\s*Contracts::AuthorizedPath\s+createPath\s*,\s*Contracts::AuthorizedPath\s+backupReadPath\s*\)' 'configuration constructor receives primary read/write/create and sibling-backup read capabilities' -CaseSensitive
Assert-Match $configurationSource 'readPath\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Read[\s\S]*?writePath\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Write[\s\S]*?createPath\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Create[\s\S]*?backupReadPath\.access\s*\(\s*\)\s*==\s*Domain::FileAccess::Read' 'configuration enforces exact primary and backup access capabilities' -CaseSensitive
Assert-Match $configurationSource 'backup\.size\s*\(\s*\)\s*==\s*primary\.size\s*\(\s*\)\s*\+\s*4U[\s\S]*?backup\.starts_with\s*\(\s*primary\s*\)[\s\S]*?backup\.ends_with\s*\(\s*"\.bak"\s*\)' 'configuration backup capability binds the exact sibling .bak path' -CaseSensitive
Assert-Match $configurationSource 'readPath\.canonicalPath\s*\(\s*\)\s*==\s*writePath\.canonicalPath\s*\(\s*\)[\s\S]*?readPath\.canonicalPath\s*\(\s*\)\s*==\s*createPath\.canonicalPath\s*\(\s*\)' 'configuration capabilities bind the exact same canonical path' -CaseSensitive
Assert-Match $configurationSource 'Json::parse\s*\(\s*text\s*,\s*callback\s*,\s*true\s*,\s*false\s*\)' 'configuration uses strict JSON parsing without ignored comments' -CaseSensitive
Assert-Match $configurationSource 'parse_event_t::key[\s\S]*?!objectKeys\.back\s*\(\s*\)\.insert[\s\S]*?duplicate object key' 'configuration rejects duplicate object keys' -CaseSensitive
Assert-Match $configurationSource 'static_cast\s*<\s*std::size_t\s*>\s*\(\s*depth\s*\)\s*>\s*WindowsConfigurationStore::MaximumJsonDepth' 'configuration rejects excessive JSON depth' -CaseSensitive
Assert-Match $configurationSource 'schemaVersion[\s\S]*?WindowsConfigurationStore::SchemaVersion' 'configuration validates schema version 1' -CaseSensitive
Assert-Match $configurationSource 'auto\s+candidateDocument\s*=\s*document_\s*;' 'configuration updates preserve unknown fields in a complete candidate' -CaseSensitive
Assert-PatternOrder $configurationSource 'atomicFileStore_\.replace\s*\(' 'document_\s*=\s*std::move\s*\(\s*candidateDocument\s*\)' 'configuration publishes document only after durable commit'
Assert-PatternOrder $configurationSource 'atomicFileStore_\.replace\s*\(' 'snapshot_\s*=\s*std::move\s*\(\s*updated\s*\)' 'configuration publishes snapshot only after durable commit'
Assert-Match $configurationSource 'targetExists_\s*\?\s*writePath_\s*:\s*createPath_' 'configuration transitions from Create to Write authority' -CaseSensitive
Assert-Match $configurationSource 'RecordNotFound[\s\S]*?targetExists_\s*=\s*false[\s\S]*?defaultDocument\s*\(\s*\)' 'missing configuration creates defaults without masking corruption' -CaseSensitive
Assert-Match $configurationSource 'loadFromDisk[\s\S]*?recoverFromBackup[\s\S]*?backupReadPath_[\s\S]*?invalidRecoveryPair[\s\S]*?IntegrityFailure' 'configuration recovers corrupt primary only through the explicit valid sibling backup' -CaseSensitive
Assert-Match $configurationSource 'IntegrityFailure' 'configuration maps hostile documents to integrity failure' -CaseSensitive
Assert-Match $configurationSource '(?:sensitive|secret)[\s\S]*?(?:api_key|refresh_token)' 'configuration rejects secret-bearing unknown fields'

Assert-Match $dpapiHeader 'DefaultRegistrySubkey\s*=\s*L"Software\\\\Forge Conductor\\\\SecureStorage"' 'secure storage default is app-owned HKCU scope' -CaseSensitive
Assert-Match $dpapiSource 'StoredEnvelopePrefix[\s\S]*?std::byte\{1\}' 'DPAPI ciphertext envelope is versioned' -CaseSensitive
Assert-Match $dpapiSource 'PlainEnvelopePrefix[\s\S]*?std::byte\{1\}' 'DPAPI plaintext envelope is versioned' -CaseSensitive
Assert-Match $dpapiSource 'sha256\s*\(\s*const\s+std::string_view\s+value\s*\)' 'logical secure-storage keys are SHA-256 hashed' -CaseSensitive
Assert-Match $dpapiSource 'std::wstring\s+valueName\s*\([\s\S]*?L"v1_"' 'registry value names expose only versioned digests' -CaseSensitive
Assert-Match $dpapiSource 'entropyBlob\s*\(\s*digest\s*\)' 'key digest is bound as DPAPI optional entropy' -CaseSensitive
Assert-Match $dpapiSource 'CryptProtectData\s*\([\s\S]*?&entropy[\s\S]*?CRYPTPROTECT_UI_FORBIDDEN[\s\S]*?protectedBlob\.put\s*\(\s*\)' 'DPAPI protection is current-user and UI-forbidden' -CaseSensitive
Assert-Match $dpapiSource 'CryptUnprotectData\s*\([\s\S]*?&entropy[\s\S]*?CRYPTPROTECT_UI_FORBIDDEN[\s\S]*?plain\.put\s*\(\s*\)' 'DPAPI unprotection reuses key-bound entropy' -CaseSensitive
Assert-Exact ([regex]::Matches($dpapiSource, '\bCryptProtectData\s*\(').Count) 1 'one DPAPI protection boundary'
Assert-Exact ([regex]::Matches($dpapiSource, '\bCryptUnprotectData\s*\(').Count) 1 'one DPAPI unprotection boundary'
Assert-Match $registrySource 'RegCreateKeyExW\s*\(\s*HKEY_CURRENT_USER' 'secure storage creates only beneath HKCU' -CaseSensitive
Assert-Match $registrySource 'RegOpenKeyExW\s*\(\s*HKEY_CURRENT_USER' 'secure storage opens only beneath HKCU' -CaseSensitive
Assert-Match $registrySource 'RequiredPrefix\s*=\s*L"Software\\\\Forge Conductor\\\\"' 'registry owner rejects foreign subkeys' -CaseSensitive
Assert-Match $dpapiSource 'type\s*!=\s*REG_BINARY' 'secure storage rejects unexpected registry value types' -CaseSensitive
Assert-Match $dpapiSource 'catalog\.value\s*\(\s*\)\.entryCount\s*>=\s*MaximumEntryCount' 'secure storage rejects a distinct 129th key' -CaseSensitive
Assert-Match $dpapiSource 'SecureZeroMemory' 'secure storage zeroes sensitive plaintext and entropy buffers' -CaseSensitive
Assert-Match $dpapiSource 'class\s+SensitiveDigest\s+final' 'DPAPI SHA-256 entropy is owned by a final sensitive RAII type'
Assert-Match $dpapiSource 'SensitiveDigest\s*\(\s*const\s+SensitiveDigest\s*&\s*\)\s*=\s*delete\s*;' 'sensitive digest copy construction is forbidden'
Assert-Match $dpapiSource 'operator=\s*\(\s*const\s+SensitiveDigest\s*&\s*\)\s*=\s*delete\s*;' 'sensitive digest copy assignment is forbidden'
Assert-Match $dpapiSource '(?s)SensitiveDigest\s*\(\s*SensitiveDigest&&.*?SecureZeroMemory\s*\(\s*other\.bytes_\.data\(\s*\)\s*,\s*other\.bytes_\.size\(\s*\)\s*\)' 'sensitive digest move construction clears the source'
Assert-Match $dpapiSource '(?s)operator=\s*\(\s*SensitiveDigest&&.*?SecureZeroMemory\s*\(\s*bytes_\.data\(\s*\)\s*,\s*bytes_\.size\(\s*\)\s*\).*?SecureZeroMemory\s*\(\s*other\.bytes_\.data\(\s*\)\s*,\s*other\.bytes_\.size\(\s*\)\s*\)' 'sensitive digest move assignment clears destination and source'
Assert-Match $dpapiSource '(?s)~SensitiveDigest\s*\(\s*\).*?SecureZeroMemory\s*\(\s*bytes_\.data\(\s*\)\s*,\s*bytes_\.size\(\s*\)\s*\)' 'sensitive digest destructor clears entropy bytes'
Assert-Match $dpapiSource 'Domain::Result\s*<\s*SensitiveDigest\s*>\s+sha256\s*\(' 'SHA-256 helper returns the sensitive RAII digest'
Assert-Match $dpapiSource '(?s)valueName\s*\(\s*digest\.value\(\)\.get\(\)\s*\).*?protectSecret\s*\(\s*secret\s*,\s*digest\.value\(\)\.get\(\)\s*\)' 'secure put keeps entropy inside the sensitive digest owner'
Assert-Match $dpapiSource 'unprotectSecret\s*\([\s\S]*?digest\.value\(\)\.get\(\)' 'secure get keeps entropy inside the sensitive digest owner'
Assert-Match $dpapiSource 'void\s+shutdown\s*\(\s*\)\s*noexcept\s*\{\s*executor_\.shutdown\s*\(\s*\)' 'secure storage fails closed through bounded executor shutdown' -CaseSensitive
$diagnosticSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsDiagnosticSink.cpp')
$diagnosticTreeHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'DiagnosticDirectoryTree.h')
$diagnosticRotationObserverHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'DiagnosticRotationPublishObserver.h')
$etwHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'EtwProvider.h')
$etwSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'EtwProvider.cpp')

Assert-Match $diagnosticHeader 'struct\s+WindowsDiagnosticSinkOptions\s+final\s*\{[\s\S]*?Domain::PathText\s+diagnosticsRoot\s*;[\s\S]*?Domain::PathText\s+exportRoot\s*;[\s\S]*?Domain::ResourceBudgets\s+budgets\s*;[\s\S]*?bool\s+enableEtw\s*\{\s*true\s*\}\s*;' 'diagnostic roots and budgets are explicitly injected' -CaseSensitive
Assert-Match $diagnosticHeader 'MasterLogName\s*\{\s*"forge-diagnostics\.jsonl"\s*\}' 'canonical master diagnostic filename' -CaseSensitive
Assert-Match $diagnosticSource 'diagnosticLogFilesMaximum\s*>\s*WindowsDiagnosticSink::MaximumRetainedLogFiles' 'diagnostic profile count cannot exceed the hard cap' -CaseSensitive
Assert-Match $diagnosticSource 'diagnosticLogFileBytesMaximum\s*==\s*0U[\s\S]*?diagnosticLogFilesMaximum\s*==\s*0U' 'zero diagnostic budgets fail closed' -CaseSensitive

$privateNameMatch = [regex]::Match(
    $diagnosticSource,
    'static\s+const\s+std::set\s*<[^;{]+>\s+exact\s*\{(?<body>.*?)\};',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $privateNameMatch.Success 'diagnostic exact private-field set'
$privateNames = @([regex]::Matches(
    $privateNameMatch.Groups['body'].Value,
    '"(?<name>[^"]+)"') | ForEach-Object { $_.Groups['name'].Value })
Assert-Set $privateNames @(
    'access_key',
    'access_key_id',
    'api_key',
    'apikey',
    'api_secret',
    'auth',
    'authorization',
    'bearer',
    'body',
    'client_secret',
    'command',
    'content',
    'cookie',
    'credential',
    'credentials',
    'cwd',
    'error',
    'goal',
    'home',
    'json',
    'markdown',
    'narrative',
    'password',
    'path',
    'private_key',
    'refresh_token',
    'prompt',
    'query',
    'secret',
    'session_cookie',
    'summary',
    'token') 'diagnostic exact private-field inventory'
foreach ($sensitiveName in @(
    'accesskey',
    'apikey',
    'auth',
    'bearer',
    'cookie',
    'credential',
    'password',
    'passwd',
    'privatekey',
    'secret',
    'token')) {
    Assert-Match $diagnosticSource ('"' + [regex]::Escape($sensitiveName) + '"') "diagnostic compact private-name fragment $sensitiveName" -CaseSensitive
}
Assert-Match $diagnosticSource 'looksLikePath[\s\S]*?value\.starts_with\s*\(\s*"/"\s*\)[\s\S]*?value\[1\]\s*==\s*'':''[\s\S]*?value\.find\s*\(\s*"\\\\Users\\\\"' 'diagnostic values use structural path detection' -CaseSensitive
Assert-Match $diagnosticSource 'flattenedBytes\s*>\s*Domain::MaximumDiagnosticFlattenedFieldBytes' 'diagnostic flattened fields enforce 512-byte bound' -CaseSensitive
Assert-Match $diagnosticSource 'redactText[\s\S]*?redactor\.redact\s*\(\s*value\s*\)[\s\S]*?redactText\s*\(\s*redactor\s*,\s*event\.fields\[index\]\.value' 'every arbitrary diagnostic field value crosses the redactor' -CaseSensitive
Assert-Match $diagnosticSource 'isPrivateFieldName\s*\(\s*event\.fields\[index\]\.name\s*\)[\s\S]*?output\.fields\[index\]\.value[\s\S]*?<redacted:' 'private diagnostic names replace values before retention' -CaseSensitive
Assert-Match $diagnosticSource 'auto\s+redacted\s*=\s*redactEnvelope\s*\(\s*event\s*,\s*\*redactor_\s*\)[\s\S]*?encodeJsonLine\s*\(\s*redacted\.value\s*\(\s*\)\s*\)[\s\S]*?appendJsonLine' 'redaction precedes JSONL persistence' -CaseSensitive
Assert-Match $diagnosticSource 'retained_\.push_back\s*\(\s*RetainedDiagnostic[\s\S]*?appendJsonLine[\s\S]*?if\s*\(\s*!persisted\s*\)[\s\S]*?retained_\.pop_back' 'allocating ring insertion is staged and rolled back if durable append fails' -CaseSensitive
Assert-Match $diagnosticSource 'appendJsonLine[\s\S]*?ringBytes_\s*\+=' 'durable append precedes no-fail ring publication' -CaseSensitive
Assert-Match $diagnosticSource 'retained_\.push_back[\s\S]*?etw_->write\s*\(\s*retained_\.back\s*\(\s*\)\.envelope\s*\)' 'ETW receives only the retained redacted envelope' -CaseSensitive
Assert-Match $diagnosticSource 'retained_\.size\s*\(\s*\)\s*>\s*WindowsDiagnosticSink::MaximumRetainedRecords[\s\S]*?ringBytes_\s*>\s*budgets_\.diagnosticLogFileBytesMaximum' 'diagnostic ring is bounded by count and encoded bytes' -CaseSensitive

$creationAnchorMatch = [regex]::Match(
    $diagnosticSource,
    'ensureDiagnosticsRoot\s*\([^)]*?retainedAnchors[^)]*?\)\s*noexcept\s*(?<body>\{[\s\S]*?)\r?\n\}\s*\r?\n\s*using\s+AnchoredDirectoryTree',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $creationAnchorMatch.Success 'diagnostic incremental creation-anchor implementation'
$creationAnchorText = $creationAnchorMatch.Groups['body'].Value
Assert-Match $diagnosticTreeHeader 'struct\s+AnchoredDiagnosticDirectoryTree\s+final\s*\{[\s\S]*?std::wstring\s+root\s*;[\s\S]*?std::vector\s*<\s*UniqueHandle\s*>\s+handles\s*;' 'diagnostic transaction owns the complete directory-handle chain' -CaseSensitive
Assert-Match $creationAnchorText 'std::vector\s*<\s*Detail::UniqueHandle\s*>\s+creationAnchors' 'diagnostic creation retains each verified ancestor' -CaseSensitive
Assert-Match $creationAnchorText 'std::wstring\s+current\s*=\s*root\.value\s*\(\s*\)\.substr\s*\(\s*0U\s*,\s*3U\s*\)' 'diagnostic creation anchors the drive root before descendants' -CaseSensitive
Assert-Match $creationAnchorText 'CreateFileW\s*\([\s\S]*?FILE_READ_ATTRIBUTES\s*\|\s*FILE_LIST_DIRECTORY\s*\|\s*FILE_TRAVERSE\s*,[\s\S]*?FILE_SHARE_READ\s*,\s*nullptr[\s\S]*?FILE_FLAG_BACKUP_SEMANTICS\s*\|\s*FILE_FLAG_OPEN_REPARSE_POINT' 'diagnostic drive anchor uses traverse-only strong sharing and reparse inspection' -CaseSensitive
Assert-NoMatch $creationAnchorText 'FILE_SHARE_(?:WRITE|DELETE)|FILE_OPEN_FOR_BACKUP_INTENT' 'diagnostic creation anchors deny mutation sharing and backup-intent semantics' -CaseSensitive
Assert-NoMatch $creationAnchorText 'CreateDirectoryW|creationAnchors\.back\s*\(\s*\)\.reset|upgrade the diagnostics' 'diagnostic creation never releases or path-reopens a pinned parent' -CaseSensitive
Assert-Match $creationAnchorText 'openRelative\s*\(\s*creationAnchors\.back\s*\(\s*\)\.get\s*\(\s*\)\s*,\s*component\s*,\s*childOptions\s*\)[\s\S]*?RelativeOpenDisposition::OpenOrCreate[\s\S]*?openRelative\s*\(\s*creationAnchors\.back\s*\(\s*\)\.get\s*\(\s*\)\s*,\s*component\s*,\s*childOptions\s*\)' 'diagnostic child open/create remains relative to the same retained parent' -CaseSensitive
Assert-Match $creationAnchorText 'while\s*\(\s*!child\s*&&\s*child\.win32Error\s*==\s*ERROR_SHARING_VIOLATION\s*\)[\s\S]*?boundedWaitMilliseconds\s*\(\s*context\.deadline\s*\)' 'diagnostic directory contention is deadline and shutdown bounded' -CaseSensitive
Assert-PatternOrder $creationAnchorText 'verifyOpenedDirectory\s*\(\s*child\.handle\.get\s*\(\s*\)\s*,\s*childPath\s*\)' 'creationAnchors\.push_back\s*\(\s*std::move\s*\(\s*child\.handle\s*\)\s*\)' 'diagnostic component is verified before its handle is retained'
Assert-PatternOrder $creationAnchorText 'creationAnchors\.push_back\s*\(' 'current\s*=\s*childPath' 'diagnostic parent handle is retained before the next child path is selected'
Assert-Match $creationAnchorText '\*retainedAnchors\s*=\s*std::move\s*\(\s*creationAnchors\s*\)' 'diagnostic transaction receives the complete retained anchor chain' -CaseSensitive

Assert-Match $diagnosticSource 'class\s+DiagnosticFileLock\s+final' 'diagnostic interprocess lock has a typed RAII owner' -CaseSensitive
Assert-Match $diagnosticSource 'L"\.forge-diagnostics\.lock"' 'diagnostic lock uses a fixed app-owned sibling' -CaseSensitive
Assert-Match $diagnosticSource 'lockOptions\.desiredAccess\s*=\s*GENERIC_READ\s*\|\s*GENERIC_WRITE[\s\S]*?lockOptions\.shareAccess\s*=\s*FILE_SHARE_READ\s*\|\s*FILE_SHARE_WRITE[\s\S]*?RelativeOpenDisposition::OpenOrCreate[\s\S]*?openRelative\s*\(\s*anchoredRoot\.handles\.back\s*\(\s*\)\.get\s*\(\s*\)' 'diagnostic lock opens relative to the retained root and excludes delete sharing' -CaseSensitive
Assert-Match $diagnosticSource 'verifyOpenedFile\s*\(\s*openedLock\.handle\.get\s*\(\s*\)\s*,\s*path\s*\)' 'diagnostic lock file is verified after relative open' -CaseSensitive
Assert-Match $diagnosticSource 'LockFileEx\s*\(\s*handle\.get\s*\(\s*\)\s*,\s*LOCKFILE_EXCLUSIVE_LOCK\s*\|\s*LOCKFILE_FAIL_IMMEDIATELY\s*,\s*0U\s*,\s*1U\s*,\s*0U\s*,\s*&operation\s*\)' 'diagnostic lock polls exactly one byte without pending kernel storage' -CaseSensitive
Assert-Match $diagnosticSource 'std::stop_callback\s+cancellationWake' 'diagnostic lock wait is cancellation-aware' -CaseSensitive
Assert-Match $diagnosticSource 'std::array\s*<\s*HANDLE\s*,\s*2U\s*>\s+waitHandles' 'diagnostic lock poll wait observes cancellation and shutdown' -CaseSensitive
Assert-Match $diagnosticSource 'WaitForMultipleObjects[\s\S]*?boundedWaitMilliseconds\s*\(\s*context\.deadline\s*\)' 'diagnostic lock wait honors the operation deadline' -CaseSensitive
Assert-Match $diagnosticSource 'MaximumPollMilliseconds\s*=\s*25LL[\s\S]*?WAIT_TIMEOUT[\s\S]*?continue' 'diagnostic lock polling wakes at most every 25 milliseconds' -CaseSensitive
Assert-NoMatch $diagnosticSource 'FILE_FLAG_OVERLAPPED[\s\S]*?LockFileEx|CancelIoEx\s*\([^)]*operation' 'diagnostic lock polling has no pending OVERLAPPED ownership' -CaseSensitive
Assert-Match $diagnosticSource 'UnlockFileEx\s*\(\s*handle_\.get\s*\(\s*\)\s*,\s*0U\s*,\s*1U\s*,\s*0U' 'diagnostic lock releases the exact owned byte' -CaseSensitive
Assert-Match $diagnosticSource 'struct\s+DiagnosticTransaction\s+final\s*\{\s*AnchoredDirectoryTree\s+anchoredRoot\s*;\s*DiagnosticFileLock\s+lock\s*;\s*std::wstring\s+masterPath\s*;' 'diagnostic transaction retains anchors and lock together' -CaseSensitive
Assert-Match $diagnosticSource 'prepareDiagnosticTransaction[\s\S]*?prepareAnchoredDiagnosticsRoot[\s\S]*?DiagnosticFileLock::acquire' 'diagnostic transaction anchors directories before locking' -CaseSensitive
Assert-NoMatch $diagnosticSource '\b(?:CreateDirectoryW|DeleteFileW|MoveFileExW|ReplaceFileW)\s*\(' 'diagnostic namespace mutations are handle-relative' -CaseSensitive
Assert-Match $diagnosticSource 'inspectRelativeFile[\s\S]*?openRelative[\s\S]*?deleteIfExists[\s\S]*?openRelative[\s\S]*?FileDispositionInfo[\s\S]*?moveIfExists[\s\S]*?openRelative[\s\S]*?openAppendHandle[\s\S]*?openRelative' 'diagnostic inspect, delete, rotation, and append operations stay beneath the retained root handle' -CaseSensitive
Assert-Match $diagnosticSource 'verifyOpenedPath[\s\S]*?noexcept\s*\{\s*try\s*\{[\s\S]*?FileStandardInfo[\s\S]*?NumberOfLinks\s*!=\s*1U\s*\|\|\s*standard\.DeletePending\s*!=\s*FALSE' 'diagnostic no-throw verifier rejects multiply linked and delete-pending leaves' -CaseSensitive
Assert-Match $diagnosticSource 'validateDiagnosticDirectoryCaseSensitivity[\s\S]*?FILE_CS_FLAG_CASE_SENSITIVE_DIR[\s\S]*?verifyOpenedPath[\s\S]*?FileCaseSensitiveInfo[\s\S]*?validateDiagnosticDirectoryCaseSensitivity' 'diagnostic directory verification fails closed for case-sensitive namespaces' -CaseSensitive

Assert-Match $diagnosticSource 'DiagnosticRotationTemporarySlots\s*=\s*16U\s*;' 'diagnostic rotation staging namespace has exactly sixteen slots' -CaseSensitive
Assert-Match $diagnosticSource 'DiagnosticRotationTemporaryPrefix\s*=\s*L"\.forge-diagnostics-rotation-"' 'diagnostic rotation uses its fixed reserved staging prefix' -CaseSensitive
Assert-Match $diagnosticSource 'class\s+PendingDiagnosticRotationFile\s+final[\s\S]*?~PendingDiagnosticRotationFile\s*\(\s*\)\s*noexcept[\s\S]*?FileDispositionInfo[\s\S]*?markCommitted\s*\(\s*\)\s*noexcept' 'diagnostic rotation temporary has exact-handle RAII cleanup and an explicit commit transition' -CaseSensitive
Assert-Match $diagnosticSource 'createDiagnosticRotationTemporary[\s\S]*?attempt\s*<\s*DiagnosticRotationTemporarySlots[\s\S]*?shareAccess\s*=\s*0U[\s\S]*?RelativeOpenDisposition::CreateNew[\s\S]*?writeThrough\s*=\s*true[\s\S]*?openRelative\s*\(\s*anchoredRoot' 'diagnostic rotation creates a bounded exclusive write-through stage relative to the retained root' -CaseSensitive
Assert-Match $diagnosticSource 'publishDiagnosticRotationTemporary[\s\S]*?information->Flags\s*=\s*0U[\s\S]*?information->RootDirectory\s*=\s*nullptr[\s\S]*?NtSetInformationFile[\s\S]*?NativeFileRenameInformationEx' 'diagnostic rotation publishes by native class 65 with null root and create-new semantics' -CaseSensitive
Assert-Match $diagnosticRotationObserverHeader 'class\s+IDiagnosticRotationPublishObserver[\s\S]*?beforeStagedFileValidation\s*\(\s*std::wstring_view\s+stagedPath\s*\)\s*noexcept' 'diagnostic rotation race seam is private and typed' -CaseSensitive
$diagnosticPublicSection = [regex]::Match(
    $diagnosticHeader,
    'class\s+WindowsDiagnosticSink\s+final[\s\S]*?public\s*:\s*(?<body>[\s\S]*?)\s*private\s*:',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $diagnosticPublicSection.Success 'diagnostic facade exposes a structurally bounded public section'
Assert-NoMatch $diagnosticPublicSection.Groups['body'].Value 'IDiagnosticRotationPublishObserver|WindowsDiagnosticSinkTestAccess' 'diagnostic production API cannot inject or invoke the blocking rotation observer' -CaseSensitive
Assert-Match $diagnosticHeader 'private\s*:[\s\S]*?friend\s+struct\s+Detail::WindowsDiagnosticSinkTestAccess\s*;[\s\S]*?WindowsDiagnosticSink\s*\([\s\S]*?std::shared_ptr\s*<\s*Detail::IDiagnosticRotationPublishObserver\s*>\s+rotationPublishObserver\s*\)' 'diagnostic observer constructor is private and restricted to the test-access friend' -CaseSensitive
Assert-Match $diagnosticRotationObserverHeader 'struct\s+WindowsDiagnosticSinkTestAccess\s+final[\s\S]*?static\s+std::unique_ptr\s*<\s*WindowsDiagnosticSink\s*>\s+create\s*\([\s\S]*?std::shared_ptr\s*<\s*IDiagnosticRotationPublishObserver\s*>\s+rotationPublishObserver\s*\)' 'private diagnostic test access owns the sole observer-injection factory' -CaseSensitive
Assert-Match $diagnosticSource 'WindowsDiagnosticSinkTestAccess::create\s*\([\s\S]*?new\s+WindowsDiagnosticSink\s*\{' 'diagnostic test factory alone invokes the private observer constructor' -CaseSensitive
Assert-Match $diagnosticSource 'verifyOpenedPath[\s\S]*?FileStandardInfo[\s\S]*?NumberOfLinks\s*!=\s*1U[\s\S]*?DeletePending\s*!=\s*FALSE[\s\S]*?GetFinalPathNameByHandleW' 'diagnostic file verification rejects hard links and delete-pending state before final-path identity is accepted' -CaseSensitive
Assert-Match $diagnosticSource 'beforeStagedFileValidation\s*\(\s*temporary\.path\s*\(\s*\)\s*\)[\s\S]*?verifyOpenedFile\s*\(\s*temporary\.handle\s*\(\s*\)\s*,\s*temporary\.path\s*\(\s*\)\s*\)[\s\S]*?ntSetInformationFile\s*\(\s*temporary\.handle\s*\(\s*\)' 'diagnostic publish repeats exact staged-handle validation after the deterministic hook and immediately before native publication' -CaseSensitive
Assert-Match $diagnosticSource 'pruneStaleDiagnosticRotationTemporaries[\s\S]*?slot\s*<\s*DiagnosticRotationTemporarySlots[\s\S]*?deleteIfExists' 'diagnostic rotation stale-stage recovery is fixed-slot and handle-relative' -CaseSensitive
Assert-Match $diagnosticSource 'pruneStaleArchives[\s\S]*?pruneStaleDiagnosticRotationTemporaries[\s\S]*?for\s*\(\s*std::size_t\s+generation' 'diagnostic stale stages are pruned before canonical archive access' -CaseSensitive
Assert-Match $diagnosticSource 'moveIfExists[\s\S]*?std::array\s*<\s*std::byte\s*,\s*64U\s*\*\s*1024U\s*>[\s\S]*?validateDiagnosticTransactionStart[\s\S]*?ReadFile[\s\S]*?WriteFile[\s\S]*?FlushFileBuffers[\s\S]*?publishDiagnosticRotationTemporary[\s\S]*?FileDispositionInfo[\s\S]*?markCommitted' 'diagnostic rotation copies in bounded cancellable chunks, flushes, atomically publishes, then retires the source' -CaseSensitive

Assert-Match $diagnosticSource 'for\s*\(\s*std::size_t\s+generation\s*=\s*maximumFiles\s*;\s*generation\s*<\s*WindowsDiagnosticSink::MaximumRetainedLogFiles' 'profile downshift prunes every stale archive generation' -CaseSensitive
Assert-Match $diagnosticSource 'appendWouldExceed[\s\S]*?rotateLog[\s\S]*?openAppendHandle[\s\S]*?openedFileSize[\s\S]*?appendWouldExceed[\s\S]*?rotateLog' 'diagnostic append rechecks capacity after opening under the lock' -CaseSensitive
Assert-Match $diagnosticSource 'WriteFile[\s\S]*?FlushFileBuffers[\s\S]*?completed flush is the durability linearization point' 'diagnostic append flushes before reporting durable success' -CaseSensitive
Assert-Match $diagnosticSource 'CreateEventW\s*\(\s*nullptr\s*,\s*TRUE\s*,\s*FALSE\s*,\s*nullptr\s*\)' 'diagnostic sink owns a manual-reset shutdown event' -CaseSensitive
Assert-Match $diagnosticSource 'shutdownRequested_\.exchange[\s\S]*?SetEvent\s*\(\s*shutdownEvent_\.get\s*\(\s*\)\s*\)[\s\S]*?executor_\.beginShutdown\s*\(\s*\)[\s\S]*?executor_\.waitUntilIdle\s*\(\s*DiagnosticShutdownDrainTimeout\s*\)[\s\S]*?etw_->shutdown' 'diagnostic shutdown wakes lock waits before bounded owned-work drain' -CaseSensitive
foreach ($dependency in @('IClock', 'IRedactor', 'IHasher', 'IWorkspaceAuthority', 'IAtomicFileStore')) {
    Assert-Match $diagnosticHeader ('std::shared_ptr\s*<\s*Contracts::' + $dependency + '\s*>') "diagnostic facade requires shared ownership for $dependency" -CaseSensitive
    Assert-Match $diagnosticSource ('const\s+std::shared_ptr\s*<\s*Contracts::' + $dependency + '\s*>') "diagnostic implementation retains shared ownership for $dependency" -CaseSensitive
}
Assert-Match $diagnosticSource '!clock_\s*\|\|\s*!redactor_\s*\|\|\s*!hasher_\s*\|\|\s*!workspaceAuthority_[\s\S]*?!atomicFileStore_[\s\S]*?invalid_argument' 'diagnostic construction rejects missing dependency owners' -CaseSensitive
Assert-Match $diagnosticSource 'WindowsDiagnosticSink::~WindowsDiagnosticSink[\s\S]*?std::move\s*\(\s*implementation_\s*\)[\s\S]*?implementation->shutdown' 'diagnostic destructor transfers shared implementation ownership before bounded shutdown' -CaseSensitive
Assert-Match $diagnosticSource 'WindowsDiagnosticSink::record[\s\S]*?const\s+auto\s+implementation\s*=\s*implementation_[\s\S]*?implementation->record[\s\S]*?WindowsDiagnosticSink::recent[\s\S]*?const\s+auto\s+implementation\s*=\s*implementation_[\s\S]*?implementation->recent[\s\S]*?WindowsDiagnosticSink::exportData[\s\S]*?const\s+auto\s+implementation\s*=\s*implementation_[\s\S]*?implementation->exportData' 'active diagnostic calls retain shared implementation ownership' -CaseSensitive

Assert-Match $etwHeader 'std::atomic\s*<\s*REGHANDLE\s*>\s+registration_' 'ETW registration has one typed atomic owner' -CaseSensitive
Assert-Match $etwSource 'EventRegister\s*\(' 'ETW provider registration boundary' -CaseSensitive
Assert-Match $etwSource 'EventUnregister\s*\(' 'ETW provider unregistration boundary' -CaseSensitive
Assert-Match $etwSource 'std::array\s*<\s*EVENT_DATA_DESCRIPTOR\s*,\s*4U\s*>\s+data' 'ETW payload has exactly four fixed numeric descriptors' -CaseSensitive
Assert-Exact ([regex]::Matches($etwSource, 'EventDataDescCreate\s*\(').Count) 4 'ETW exact descriptor count'
Assert-Match $etwSource 'EventWriteTransfer\s*\(' 'ETW emits through the application-owned provider' -CaseSensitive
Assert-NoMatch $etwSource 'envelope\.(?:event|role|fields)' 'ETW must not receive arbitrary strings' -CaseSensitive
Assert-Match $diagnosticSource 'MaximumPersistedExportRecords\s*=\s*50''000U\s*;' 'persisted diagnostic export record cap' -CaseSensitive
Assert-Match $diagnosticSource 'MaximumPersistedJsonLineBytes\s*=\s*16U\s*\*\s*1024U\s*;' 'persisted diagnostic JSONL line cap' -CaseSensitive
Assert-Match $diagnosticSource 'loadPersistedDiagnostics[\s\S]*?prepareDiagnosticTransaction[\s\S]*?pruneStaleArchives[\s\S]*?diagnosticLogFileBytesMaximum' 'persisted export reads the bounded active master under the interprocess transaction' -CaseSensitive
Assert-Match $diagnosticSource 'content\.back\s*\(\s*\)\s*!=\s*''\\n''[\s\S]*?content\.find\s*\(\s*''\\0''\s*\)[\s\S]*?content\.find\s*\(\s*''\\r''\s*\)' 'persisted export accepts only complete canonical JSONL' -CaseSensitive
Assert-Match $diagnosticSource 'Json::parse\s*\(\s*line\s*,[\s\S]*?parse_event_t::key[\s\S]*?Duplicate persisted diagnostic JSON key' 'persisted export rejects duplicate JSON keys' -CaseSensitive
Assert-Match $diagnosticSource 'requiredKeys[\s\S]*?allowedKeys[\s\S]*?contains an unknown member' 'persisted export rejects missing and unknown members' -CaseSensitive
Assert-Match $diagnosticSource 'validateDiagnosticEnvelope\s*\(\s*envelope\s*\)[\s\S]*?redactEnvelope\s*\(\s*envelope\s*,\s*redactor\s*\)' 'hostile persisted records are domain-validated and re-redacted' -CaseSensitive
Assert-Match $diagnosticSource 'parseTimestamp[\s\S]*?timestampText\s*\(\s*timestamp\s*\)\s*!=\s*text' 'persisted UTC timestamps round-trip canonically' -CaseSensitive
Assert-Match $diagnosticSource 'std::sort\s*\(\s*persisted\.begin\s*\(\s*\)\s*,\s*persisted\.end\s*\(\s*\)\s*,\s*diagnosticEnvelopeLess\s*\)[\s\S]*?std::unique[\s\S]*?sameDiagnosticEnvelope' 'persisted and live diagnostics are deterministically sorted and deduplicated' -CaseSensitive
Assert-Match $diagnosticSource 'planExportPaths\s*\(\s*request\s*,\s*exportRoot_\s*,\s*exportedAt\s*\)' 'default diagnostic export uses the explicit export root' -CaseSensitive
Assert-Match $diagnosticSource 'loadPersistedDiagnostics\s*\(\s*diagnosticsRoot_\s*,\s*budgets_\s*,\s*\*redactor_\s*,\s*context\s*,\s*shutdownEvent_\.get\s*\(\s*\)\s*\)' 'diagnostic export loads shared durable records with cancellation and shutdown ownership' -CaseSensitive
Assert-Match $diagnosticSource 'buildExportDocuments\s*\([\s\S]*?"schema_version"\s*,\s*1[\s\S]*?Json::error_handler_t::strict' 'diagnostic JSON export is deterministic strict schema 1' -CaseSensitive
Assert-Match $diagnosticSource 'Forge Conductor Diagnostic Export[\s\S]*?Summary by severity[\s\S]*?Summary by category[\s\S]*?Timeline' 'diagnostic Markdown export has deterministic sections' -CaseSensitive
Assert-Exact ([regex]::Matches($diagnosticSource, 'hasher_->sha256\s*\(').Count) 2 'independent JSON and Markdown export checksums'
Assert-Exact ([regex]::Matches($diagnosticSource, 'atomicFileStore_->replace\s*\(').Count) 2 'exact JSON and Markdown atomic export writes'
Assert-Match $diagnosticSource 'preauthorizeReplacement[\s\S]*?Domain::FileAccess::Create[\s\S]*?Domain::FileAccess::Write' 'diagnostic export requests both Create and Write authority' -CaseSensitive
Assert-Match $diagnosticSource 'planExportPaths[\s\S]*?preauthorizeReplacement\s*\(\s*plan\.value\s*\(\s*\)\.json[\s\S]*?preauthorizeReplacement\s*\(\s*plan\.value\s*\(\s*\)\.markdown[\s\S]*?resolveExportPaths' 'both diagnostic artifacts are preauthorized before filesystem resolution or durable-log reads' -CaseSensitive
Assert-Match $diagnosticSource 'validBasename[\s\S]*?!value\.empty\s*\(\s*\)[\s\S]*?value\.size\s*\(\s*\)\s*<=\s*WindowsDiagnosticSink::MaximumExportBasenameBytes[\s\S]*?std::all_of[\s\S]*?character\s*>=\s*''a''[\s\S]*?character\s*<=\s*''z''[\s\S]*?character\s*>=\s*''A''[\s\S]*?character\s*<=\s*''Z''[\s\S]*?character\s*>=\s*''0''[\s\S]*?character\s*<=\s*''9''[\s\S]*?character\s*==\s*''-''[\s\S]*?character\s*==\s*''_''' 'diagnostic export basename is bounded and traversal-safe' -CaseSensitive
$processHeader = Get-Content -Raw -LiteralPath (
    Join-Path $publicRoot 'WindowsProcessSupervisor.h')
$processSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsProcessSupervisor.cpp')
$processLaunchObserverHeader = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'ProcessLaunchObserver.h')
$commandLineSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'CommandLineBuilder.cpp')
$jobSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'JobObject.cpp')
$operationSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'OperationGuard.cpp')
$pipeSource = Get-Content -Raw -LiteralPath (
    Join-Path $detailRoot 'OverlappedPipeReader.cpp')
$runtimeSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsRuntimeDiagnostics.cpp')

Assert-Set (Get-MatchingRelativeFiles $implementationFiles '\bFreeEnvironmentStringsW\s*\(') @(
    'src/Infrastructure/Windows/Detail/CommandLineBuilder.cpp') 'inherited environment RAII owner inventory'
Assert-Match $commandLineSource 'class\s+EnvironmentStrings\s+final[\s\S]*?~EnvironmentStrings[\s\S]*?FreeEnvironmentStringsW' 'inherited environment has typed RAII ownership' -CaseSensitive
Assert-Match $commandLineSource 'MultiByteToWideChar\s*\(\s*CP_UTF8\s*,\s*MB_ERR_INVALID_CHARS' 'process input rejects malformed UTF-8' -CaseSensitive
Assert-Match $commandLineSource 'quoteArgument[\s\S]*?\(?\s*backslashes\s*\*\s*2U\s*\)?\s*\+\s*1U' 'process arguments use exact Windows backslash/quote escaping' -CaseSensitive
Assert-Match $commandLineSource 'commandLine\.size\s*\(\s*\)\s*\+\s*1U\s*>\s*Domain::MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator' 'final command line enforces the native cap including terminator' -CaseSensitive
Assert-Match $commandLineSource 'std::map\s*<\s*std::wstring\s*,\s*std::wstring\s*,\s*EnvironmentNameLess\s*>\s+entries' 'environment names are case-insensitively unique and ordered' -CaseSensitive
Assert-Match $commandLineSource 'CompareStringOrdinal[\s\S]*?TRUE' 'environment name comparison is ordinal case-insensitive' -CaseSensitive
Assert-Match $commandLineSource 'block\.push_back\s*\(\s*L''\\0''\s*\)[\s\S]*?block\.push_back\s*\(\s*L''\\0''\s*\)' 'Unicode environment block contains final double terminators' -CaseSensitive
Assert-Match $commandLineSource 'MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators' 'final Unicode environment block reuses the Forge bound' -CaseSensitive
Assert-Match $commandLineSource 'AllowedInheritedEnvironmentNames\s*\{[\s\S]*?L"SystemRoot"\s*,\s*L"WINDIR"\s*,\s*L"TEMP"\s*,\s*L"TMP"\s*\}' 'opt-in ambient inheritance uses the exact four-name allowlist' -CaseSensitive
Assert-NoMatch $commandLineSource 'AllowedInheritedEnvironmentNames\s*\{[^}]*L"PATH"' 'ambient PATH inheritance is prohibited' -CaseSensitive
Assert-Match $commandLineSource 'CredentialEnvironmentMarkers[\s\S]*?L"API_KEY"[\s\S]*?L"PRIVATE_KEY"[\s\S]*?L"SECRET"[\s\S]*?L"TOKEN"' 'ambient credential-bearing names are denied before inheritance' -CaseSensitive
Assert-Match $commandLineSource 'readCurrentEnvironment[\s\S]*?isSafeInheritedEnvironmentName\s*\(\s*name\s*\)[\s\S]*?continue' 'ambient environment is filtered while enumerating the parent block' -CaseSensitive
Assert-Match $commandLineSource 'if\s*\(\s*request\.inheritEnvironment\s*\)[\s\S]*?isSafeInheritedEnvironmentName\s*\(\s*entry\.name\s*\)[\s\S]*?insert_or_assign' 'provided inherited entries are defensively filtered again' -CaseSensitive

Assert-Match $processSource 'authority\.shellEnabled\s*\(\s*\)' 'process execution requires shell-enabled authority' -CaseSensitive
Assert-Match $processSource 'containsAccess\s*\(\s*authority\.grants\s*\(\s*\)\s*,\s*Domain::FileAccess::Execute\s*\)' 'process execution requires an Execute grant' -CaseSensitive
Assert-Match $processSource 'containsAccess\s*\(\s*authority\.denials\s*\(\s*\)\s*,\s*Domain::FileAccess::Execute\s*\)' 'process execution honors Execute denials' -CaseSensitive
Assert-Match $processSource 'GetFinalPathNameByHandleW' 'authorized executable and working directory are checked by handle' -CaseSensitive
Assert-Match $processSource 'struct\s+OpenedPath\s+final\s*\{[\s\S]*?std::wstring\s+finalPath\s*;[\s\S]*?std::vector\s*<\s*UniqueHandle\s*>\s+anchors\s*;[\s\S]*?std::vector\s*<\s*std::wstring\s*>\s+expectedPaths\s*;' 'canonical launch paths retain the complete typed ancestry-handle chain' -CaseSensitive
Assert-Match $processSource 'openAnchoredPath[\s\S]*?CreateFileW\s*\([\s\S]*?FILE_LIST_DIRECTORY\s*\|\s*FILE_TRAVERSE\s*\|\s*FILE_READ_ATTRIBUTES[\s\S]*?FILE_SHARE_READ[\s\S]*?FILE_FLAG_BACKUP_SEMANTICS\s*\|\s*FILE_FLAG_OPEN_REPARSE_POINT[\s\S]*?openRelative\s*\(\s*anchors\.back\s*\(\s*\)\.get\s*\(\s*\)' 'process launch opens the drive and every descendant relative to retained no-mutation-share parents' -CaseSensitive
Assert-Match $processSource 'verifyLaunchPathHandle[\s\S]*?FileAttributeTagInfo[\s\S]*?FILE_ATTRIBUTE_REPARSE_POINT[\s\S]*?FileStandardInfo[\s\S]*?DeletePending[\s\S]*?NumberOfLinks\s*!=\s*1U[\s\S]*?FileCaseSensitiveInfo[\s\S]*?FILE_CS_FLAG_CASE_SENSITIVE_DIR' 'process path verification rejects reparse, delete-pending, hard-linked executable, and case-sensitive-directory ambiguity' -CaseSensitive
Assert-Match $processSource 'revalidateAnchoredPath[\s\S]*?path\.anchors\.size\s*\(\s*\)\s*!=\s*path\.expectedPaths\.size\s*\(\s*\)[\s\S]*?verifyLaunchPathHandle' 'process launch revalidates every retained anchor' -CaseSensitive
Assert-Match $processSource 'executableStillAuthorized\s*=\s*revalidateAnchoredPath[\s\S]*?workingDirectoryStillAuthorized\s*=\s*revalidateAnchoredPath[\s\S]*?CreateProcessW' 'executable and working-directory ancestry are revalidated immediately before native launch' -CaseSensitive
Assert-Match $processLaunchObserverHeader 'class\s+IProcessLaunchObserver[\s\S]*?beforeCreateProcess\s*\(\s*\)\s*noexcept[\s\S]*?afterCreateProcess\s*\(\s*\)\s*noexcept' 'process launch race seam brackets the native process-creation boundary' -CaseSensitive
$processPublicSection = [regex]::Match(
    $processHeader,
    'class\s+WindowsProcessSupervisor\s+final[\s\S]*?public\s*:\s*(?<body>[\s\S]*?)\s*private\s*:',
    [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $processPublicSection.Success 'process facade exposes a structurally bounded public section'
Assert-NoMatch $processPublicSection.Groups['body'].Value 'IProcessLaunchObserver|ProcessSupervisorTestAccess' 'process production API cannot inject or invoke the blocking launch observer' -CaseSensitive
Assert-Match $processHeader 'private\s*:[\s\S]*?friend\s+struct\s+Detail::ProcessSupervisorTestAccess\s*;[\s\S]*?WindowsProcessSupervisor\s*\([\s\S]*?std::shared_ptr\s*<\s*Detail::IProcessLaunchObserver\s*>\s+launchObserver\s*\)' 'process observer constructor is private and restricted to the test-access friend' -CaseSensitive
Assert-Match $processLaunchObserverHeader 'struct\s+ProcessSupervisorTestAccess\s+final[\s\S]*?static\s+std::unique_ptr\s*<\s*WindowsProcessSupervisor\s*>[\s\S]*?create\s*\([\s\S]*?std::shared_ptr\s*<\s*IProcessLaunchObserver\s*>\s+launchObserver\s*\)[\s\S]*?new\s+WindowsProcessSupervisor' 'private process test access owns the sole observer-injection factory' -CaseSensitive
Assert-PatternOrder $processSource 'launchObserver_->beforeCreateProcess\s*\(\s*\)' 'CreateProcessW\s*\(' 'launch observer reaches the final boundary before native path resolution'
Assert-Match $processSource 'PROC_THREAD_ATTRIBUTE_HANDLE_LIST' 'process inheritance is restricted by STARTUPINFOEX handle list' -CaseSensitive
Assert-Match $processSource 'std::array\s*<\s*HANDLE\s*,\s*3\s*>\s+inheritedHandles' 'only stdin, stdout, and stderr are inherited' -CaseSensitive
Assert-Match $processSource 'creationFlags\s*=\s*CREATE_UNICODE_ENVIRONMENT\s*\|\s*EXTENDED_STARTUPINFO_PRESENT\s*\|\s*CREATE_SUSPENDED\s*\|\s*CREATE_NO_WINDOW' 'process launch uses exact suspended Unicode no-window flags' -CaseSensitive
Assert-Match $processSource 'CreateProcessW\s*\(\s*launchPaths\.application\.finalPath\.c_str\s*\(\s*\)\s*,\s*commandLine\.value\s*\(\s*\)\.data\s*\(\s*\)' 'CreateProcessW receives non-null anchored absolute lpApplicationName and mutable command line' -CaseSensitive
Assert-Match $processSource 'CreateProcessW\s*\([\s\S]*?launchError\s*=\s*created\s*\?\s*ERROR_SUCCESS\s*:\s*::GetLastError\s*\(\s*\)[\s\S]*?UniqueHandle\s+process\s*\{\s*processInformation\.hProcess\s*\}[\s\S]*?UniqueHandle\s+primaryThread\s*\{\s*processInformation\.hThread\s*\}[\s\S]*?launchPaths\.application\.anchors\.clear' 'native process and thread handles enter RAII ownership before launch anchors release' -CaseSensitive
Assert-Match $processSource 'launchPaths\.application\.anchors\.clear\s*\(\s*\)[\s\S]*?launchPaths\.workingDirectory->anchors\.clear\s*\(\s*\)[\s\S]*?launchObserver_->afterCreateProcess\s*\(\s*\)' 'process authority anchors release immediately after native launch and before the post-launch seam' -CaseSensitive
Assert-PatternOrder $processSource 'CreateProcessW\s*\(' 'launchPaths.application.anchors.clear\s*\(\s*\)' 'process ancestry remains pinned through CreateProcessW'
Assert-PatternOrder $processSource 'launchPaths.application.anchors.clear\s*\(\s*\)' 'operationJob->assign\s*\(' 'process ancestry leases do not extend across the child lifetime'
Assert-PatternOrder $processSource 'operationJob->assign\s*\(' 'state->resumePrimaryThread\s*\(' 'child is assigned to its Job before serialized resume'
Assert-Match $operationSource 'resumePrimaryThread[\s\S]*?scoped_lock\s+lock\s*\{\s*mutex_\s*\}[\s\S]*?reason\s*\(\s*\)[\s\S]*?ResumeThread' 'resume and cancellation serialize through the operation-state mutex' -CaseSensitive
Assert-Match $jobSource 'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE' 'Job Object kills the entire process tree on owner close' -CaseSensitive
Assert-NoMatch ($jobSource + $processSource) '\bJOB_OBJECT_LIMIT_(?:BREAKAWAY_OK|SILENT_BREAKAWAY_OK)\b' 'Job Object breakaway flags are prohibited' -CaseSensitive
Assert-Match $jobSource 'TerminateJobObject\s*\(' 'process cancellation terminates the whole Job' -CaseSensitive
Assert-Match $operationSource 'active_\.size\s*\(\s*\)\s*>=\s*Domain::MaximumConcurrentProcessOperations[\s\S]*?LimitExceeded[\s\S]*?no work was queued' 'process admission is exactly 64 with no waiting queue' -CaseSensitive
Assert-Match $operationSource 'cancel\s*\([\s\S]*?active_\.find\s*\(\s*operationId\.value\s*\(\s*\)\s*\)[\s\S]*?requestTermination' 'targeted cancellation is isolated by OperationId' -CaseSensitive
Assert-Match $pipeSource 'CreateNamedPipeW[\s\S]*?FILE_FLAG_OVERLAPPED[\s\S]*?PIPE_REJECT_REMOTE_CLIENTS' 'process output uses local overlapped pipes' -CaseSensitive
Assert-Match $pipeSource 'maximumCaptureBytes_\s*>\s*captured_\.size\s*\(\s*\)[\s\S]*?captured_\.append[\s\S]*?issueReadLocked' 'output retention caps do not stop pipe draining' -CaseSensitive
Assert-Match $pipeSource 'PostQueuedCompletionStatus[\s\S]*?worker_\.join\s*\(\s*\)' 'shared completion reader has an owned shutdown wake and join' -CaseSensitive
Assert-NoMatch ($processSource + $pipeSource + $operationSource) '\.detach\s*\(' 'process supervision must not detach threads' -CaseSensitive
Assert-Match $pipeSource 'cancelAndReapConnect[\s\S]*?CancelIoEx\s*\(\s*server\s*,\s*&connection\s*\)[\s\S]*?WaitForSingleObject\s*\(\s*connection\.hEvent\s*,\s*NativeIoReapTimeoutMilliseconds\s*\)[\s\S]*?GetOverlappedResult\s*\(\s*server\s*,\s*&connection[\s\S]*?FALSE\s*\)[\s\S]*?ERROR_IO_INCOMPLETE[\s\S]*?std::terminate' 'pending pipe connections use a bounded fail-closed synchronous reap' -CaseSensitive
Assert-Match $pipeSource 'cancelAndWait\s*\(\s*\)\s*noexcept[\s\S]*?cancelPendingRead[\s\S]*?idleCondition_\.wait_for\s*\(\s*lock\s*,\s*NativeIoReapTimeout[\s\S]*?!pending_[\s\S]*?std::terminate' 'reader cancellation uses a bounded fail-closed OVERLAPPED reap' -CaseSensitive
Assert-Match $pipeSource 'IoCompletionPort::shutdown[\s\S]*?reader->cancelAndWait\s*\(\s*\)[\s\S]*?waitForWorkerExit\s*\(\s*NativeIoReapTimeout\s*\)[\s\S]*?std::terminate[\s\S]*?worker_\.join' 'completion-port shutdown acknowledges its worker within the native reap bound' -CaseSensitive
Assert-Match $pipeSource 'availableBytesLocked[\s\S]*?PeekNamedPipe[\s\S]*?PipeAvailability\s*\{\s*0U\s*,\s*error\s*\}[\s\S]*?finishAvailable[\s\S]*?recordReadErrorLocked' 'PeekNamedPipe failures are preserved as process-output read errors' -CaseSensitive
Assert-Match $processSource 'ProcessTerminationUnconfirmed|process_termination_unconfirmed' 'unconfirmed tree death returns the dedicated typed error'

Assert-Match $processHeader 'std::shared_ptr\s*<\s*Contracts::IRuntimeDiagnostics\s*>' 'process facade requires shared runtime-diagnostics ownership' -CaseSensitive
Assert-Match $processSource 'Impl\s*\([\s\S]*?std::shared_ptr\s*<\s*Contracts::IRuntimeDiagnostics\s*>\s+runtimeDiagnostics[\s\S]*?runtimeDiagnostics_\s*\{\s*std::move\s*\(\s*runtimeDiagnostics\s*\)\s*\}[\s\S]*?!runtimeDiagnostics_[\s\S]*?InvalidRequest' 'process implementation validates injected runtime-diagnostics ownership' -CaseSensitive
Assert-Match $processSource 'std::shared_ptr\s*<\s*Contracts::IRuntimeDiagnostics\s*>\s+runtimeDiagnostics_\s*;' 'process implementation retains runtime-diagnostics ownership' -CaseSensitive
Assert-Match $processSource 'runtimeDiagnostics_->acquire\s*\([\s\S]*?RuntimeOwnerKind::ChildProcess[\s\S]*?runtimeDiagnostics_->acquire\s*\([\s\S]*?RuntimeOwnerKind::ProcessReader[\s\S]*?runtimeDiagnostics_->acquire\s*\([\s\S]*?RuntimeOwnerKind::ProcessReader' 'process composition acquires one child and two reader ownership leases' -CaseSensitive
Assert-Match $operationSource 'setRuntimeOwnership[\s\S]*?childProcessOwnership_\.emplace[\s\S]*?stdoutReaderOwnership_\.emplace[\s\S]*?stderrReaderOwnership_\.emplace' 'operation state owns all process runtime leases' -CaseSensitive
Assert-PatternOrder $operationSource 'releaseResources\s*\(' 'registry_->release\s*\(' 'native process resources and leases release before operation deregistration'
Assert-Match $processHeader 'std::shared_ptr\s*<\s*Impl\s*>\s+implementation_' 'process facade shares implementation lifetime with active calls' -CaseSensitive
Assert-Match $processSource 'WindowsProcessSupervisor::~WindowsProcessSupervisor[\s\S]*?std::move\s*\(\s*implementation_\s*\)[\s\S]*?implementation->shutdown' 'process destructor transfers and shuts down shared implementation ownership' -CaseSensitive
Assert-Match $processSource 'WindowsProcessSupervisor::run[\s\S]*?const\s+auto\s+implementation\s*=\s*implementation_[\s\S]*?implementation->run' 'active process calls retain shared implementation ownership' -CaseSensitive
Assert-Match $processSource 'stdoutCapture\.readError\s*!=\s*ERROR_SUCCESS[\s\S]*?stderrCapture\.readError\s*!=\s*ERROR_SUCCESS' 'stdout and stderr asynchronous read failures are propagated' -CaseSensitive
Assert-Match $processSource 'isLocalDriveAbsolutePath[\s\S]*?GetDriveTypeW[\s\S]*?case\s+DRIVE_REMOVABLE[\s\S]*?case\s+DRIVE_FIXED[\s\S]*?case\s+DRIVE_CDROM[\s\S]*?case\s+DRIVE_RAMDISK[\s\S]*?return\s+true[\s\S]*?default:[\s\S]*?return\s+false' 'process preflight allows only explicit local drive types' -CaseSensitive
Assert-NoMatch $processSource 'case\s+DRIVE_REMOTE[\s\S]*?return\s+true' 'process preflight must reject remote drives' -CaseSensitive
Assert-Match $processSource 'validateLocalRequestPaths\s*\(\s*request\s*\)[\s\S]*?operations_\.admit[\s\S]*?authorizeLaunchPaths' 'local-path preflight occurs before admission and synchronous path authorization' -CaseSensitive
Assert-Match $processSource 'finalPathForHandle[\s\S]*?GetFinalPathNameByHandleW' 'opened process paths resolve their final handle identity' -CaseSensitive
Assert-Match $processSource 'authorizedNormalizedPath[\s\S]*?absolutePath\s*\(\s*requested\s*\)[\s\S]*?isWithinRoot\s*\(\s*candidate\.value\s*\(\s*\)\s*,\s*normalizedRoot\.value\s*\(\s*\)\s*\)' 'process path authority is checked lexically before opening the authorized namespace' -CaseSensitive

$ownerKinds = @(
    'OwnedOperation',
    'PendingCallback',
    'BackgroundThread',
    'OpenRepository',
    'TelemetryPendingSnapshot',
    'ActiveTimer',
    'ChildProcess',
    'ProcessReader',
    'OpenDatabase')
Assert-Match $runtimeSource 'OwnerKindCount\s*=\s*9U\s*;' 'runtime diagnostics has exactly nine owner counters' -CaseSensitive
foreach ($ownerKind in $ownerKinds) {
    Assert-Match $runtimeSource ('RuntimeOwnerKind::' + $ownerKind) "runtime owner counter $ownerKind" -CaseSensitive
}
Assert-Match $runtimeSource 'std::weak_ptr\s*<\s*Control\s*>\s+weakControl\s*\{\s*control_\s*\}' 'ownership leases cannot prolong the runtime registry' -CaseSensitive
Assert-Match $runtimeSource 'if\s*\(\s*control->counts\[index\]\s*>\s*0U\s*\)\s*\{\s*--control->counts\[index\]' 'runtime lease release decrements exactly once without underflow' -CaseSensitive
Assert-Match $runtimeSource 'TelemetryPendingSnapshot:\s*return\s+budgets_\.telemetryPendingSnapshotsMaximum' 'telemetry capacity comes from the exact-one profile budget' -CaseSensitive
Assert-Match $runtimeSource 'ChildProcess:\s*return\s+Domain::MaximumConcurrentProcessOperations' 'runtime child-process capacity is 64' -CaseSensitive
Assert-Match $runtimeSource 'ProcessReader:\s*return\s+MaximumProcessReaderCount' 'runtime process-reader capacity is 128' -CaseSensitive
Assert-Match $runtimeSource 'control_->shutdown\s*=\s*true\s*;' 'runtime diagnostics fails closed after shutdown' -CaseSensitive
Assert-Match $runtimeSource 'counts\s*\[\s*ownerIndex\s*\(\s*Contracts::RuntimeOwnerKind::OpenDatabase\s*\)\s*\]' 'runtime snapshot exposes the ninth counter' -CaseSensitive
function Get-RegisteredTestNames {
    param([string]$Text)
    return @([regex]::Matches(
        $Text,
        'addTest\s*\(\s*tests\s*,\s*"(?<name>[^"]+)"',
        [Text.RegularExpressions.RegexOptions]::Singleline) |
            ForEach-Object { $_.Groups['name'].Value })
}

$foundationTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'FoundationWindowsTests.cpp')
$storageTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'StorageWindowsTests.cpp')
$diagnosticTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'WindowsDiagnosticSinkTests.cpp')
$processTests = Get-Content -Raw -LiteralPath (
    Join-Path $testWindowsRoot 'WindowsProcessSupervisorTests.cpp')
$pipeReaderTests = Get-Content -Raw -LiteralPath (
    Join-Path $testWindowsRoot 'OverlappedPipeReaderTests.cpp')
$shutdownTests = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'ShutdownWindowsTests.cpp')
$infrastructureMain = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'InfrastructureTestMain.cpp')
$processMain = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'ProcessIntegrationTestMain.cpp')
$testSupport = Get-Content -Raw -LiteralPath (
    Join-Path $testRoot 'TestSupport.h')

Assert-Set (Get-RegisteredTestNames $foundationTests) @(
    'foundation.application_paths_and_override_policy',
    'foundation.clocks_and_deadline_gate',
    'foundation.path_resolver_rejects_unsafe_forms',
    'foundation.secret_redaction',
    'foundation.utf_and_handle_owners',
    'foundation.uuid_and_sha256') 'foundation native test inventory'
Assert-Set (Get-RegisteredTestNames $storageTests) @(
    'storage.atomic.backup-before-target-failure',
    'storage.atomic.backup-identity-swap',
    'storage.atomic.entropy-collision-bound',
    'storage.atomic.final-leaf-swap-containment',
    'storage.atomic.final-stage-hard-link-rejection',
    'storage.atomic.metadata-policy',
    'storage.atomic.old-handle-new-identity',
    'storage.atomic.parent-anchor',
    'storage.atomic.read-parent-anchor',
    'storage.atomic.case-sensitive-directory-policy',
    'storage.atomic.crash-recovery-child',
    'storage.atomic.crash-restart-recovery',
    'storage.atomic.parent-reparse-no-stage',
    'storage.atomic.reader-without-delete-share',
    'storage.atomic.roundtrip-backup-bounds',
    'storage.atomic.security-identity-ea-policy',
    'storage.atomic.security-cancellation',
    'storage.atomic.hard-link-authority',
    'storage.atomic.stale-temp-recovery',
    'storage.atomic.target-prepublish-cancellation',
    'storage.atomic.target-reparse-swap',
    'storage.atomic.unsupported-posix-no-stage',
    'storage.config.bounds-defaults-shutdown',
    'storage.config.commit-before-publish',
    'storage.config.hostile-json',
    'storage.config.unknown-field-preservation',
    'storage.config.valid-backup-recovery',
    'storage.dpapi.catalog-integrity',
    'storage.dpapi.different-sid-denial',
    'storage.dpapi.entry-cap-corruption',
    'storage.dpapi.roundtrip-bounds-shutdown') 'storage native test inventory'
Assert-Set (Get-RegisteredTestNames $diagnosticTests) @(
    'diagnostics.active-destructor-dependency-lifetime',
    'diagnostics.bounds-cancel-shutdown',
    'diagnostics.case-sensitive-root-rejection',
    'diagnostics.concurrent-rotation-downshift',
    'diagnostics.directory-creation-parent-anchor',
    'diagnostics.final-root-protected-transaction',
    'diagnostics.hard-link-leaf-rejection',
    'diagnostics.redaction-all-surfaces',
    'diagnostics.relative-file-operations',
    'diagnostics.restart-export-create-write',
    'diagnostics.rotation-crash-child',
    'diagnostics.rotation-crash-restart',
    'diagnostics.rotation-exclusive-cancellation',
    'diagnostics.rotation-stage-hard-link-rejection') 'diagnostics native test inventory'
Assert-Set (Get-RegisteredTestNames $processTests) @(
    'process.authority_and_launch_failures',
    'process.command_line_quoting',
    'process.concurrent_admission_64_65',
    'process.active_destructor_lifetime',
    'process.descendant_job_termination',
    'process.environment_and_working_directory',
    'process.final_native_text_bounds',
    'process.handle_ownership',
    'process.independent_output_caps',
    'process.launch-hard-link-denial',
    'process.launch-path-ancestry',
    'process.malformed_utf8_replacement',
    'process.local_path_preflight',
    'process.runtime_ownership',
    'process.shutdown',
    'process.targeted_cancellation',
    'process.timeout_and_exit_status') 'process native integration-test inventory'
Assert-Set (Get-RegisteredTestNames $pipeReaderTests) @(
    'pipe_reader.broken_pipe_is_clean_eof',
    'pipe_reader.completion_port_shutdown_is_bounded',
    'pipe_reader.peek_failure_propagates',
    'pipe_reader.pending_read_cancellation_is_bounded') 'pipe-reader native negative-path test inventory'
$shutdownNames = @([regex]::Matches(
    $shutdownTests,
    '\{\s*"(?<name>runtime\.[^"]+)"\s*,') |
        ForEach-Object { $_.Groups['name'].Value })
Assert-Set $shutdownNames @(
    'runtime.capacities-and-shutdown',
    'runtime.moves-and-zero',
    'runtime.weak-lifetime') 'runtime shutdown-test inventory'

foreach ($registration in @(
    'registerFoundationWindowsTests\s*\(\s*tests\s*\)',
    'registerStorageWindowsTests\s*\(\s*tests\s*\)',
    'registerDiagnosticWindowsTests\s*\(\s*tests\s*\)')) {
    Assert-Match $infrastructureMain $registration "infrastructure test entry registration $registration" -CaseSensitive
}
Assert-Match $processMain 'registerProcessWindowsTests\s*\(\s*tests\s*,\s*std::wstring\s*\{\s*arguments\[1\]\s*\}\s*\)' 'process test executable receives the built fixture path' -CaseSensitive
Assert-Match $processMain 'argumentCount\s*!=\s*2' 'process test executable fails closed without exactly one fixture path' -CaseSensitive
Assert-Match $testSupport '#include\s*<chrono>' 'shared test context owns its chrono dependency' -CaseSensitive
Assert-Match $processMain 'registerOverlappedPipeReaderTests\s*\(\s*tests\s*\)' 'process test executable registers pipe-reader negative-path tests' -CaseSensitive
Assert-Match $testSupport 'now\s*\{\s*std::chrono::steady_clock::now\s*\(\s*\)\s*\}' 'shared test context samples one coherent monotonic instant' -CaseSensitive

foreach ($raiiType in @(
    'Detail::UniqueHandle',
    'Detail::UniqueBCryptAlgorithmHandle',
    'Detail::SecureBuffer')) {
    Assert-Match $foundationTests ('static_assert\s*\(\s*!std::is_copy_constructible_v\s*<\s*' +
        [regex]::Escape($raiiType) + '\s*>\s*\)') "noncopyable RAII proof for $raiiType" -CaseSensitive
    Assert-Match $foundationTests ('static_assert\s*\(\s*std::is_nothrow_move_constructible_v\s*<\s*' +
        [regex]::Escape($raiiType) + '\s*>\s*\)') "nothrow-movable RAII proof for $raiiType" -CaseSensitive
}
Assert-Match $foundationTests 'production-default path provider consumed FORGE_CONDUCTOR_HOME[\s\S]*?injected development override was not honored[\s\S]*?environment overrode an explicitly injected app root' 'application-root precedence and opt-in override test'
Assert-Match $foundationTests 'CreateSymbolicLinkW[\s\S]*?reparse point was accepted' 'application path reparse rejection test'
Assert-Match $foundationTests 'MaximumInputBytes[\s\S]*?exact redaction byte limit[\s\S]*?byte limit plus one' 'redactor exact-cap and cap-plus-one test'
Assert-Match $foundationTests 'UniqueHandle move did not transfer sole ownership[\s\S]*?close its native handle exactly once' 'native handle closes exactly once test'

Assert-Match $storageTests 'MaximumContentBytes\s*\+\s*1U' 'atomic payload cap-plus-one test' -CaseSensitive
Assert-Match $storageTests 'FileAccess::Create[\s\S]*?FileAccess::Read[\s\S]*?FileAccess::Write' 'atomic/config tests exercise distinct capabilities' -CaseSensitive
Assert-Match $storageTests 'atomic replacement must retain the prior file as \.bak' 'atomic sibling-backup test'
Assert-Match $storageTests 'pre-cancelled atomic replacement must not create a file' 'atomic precommit cancellation test'
Assert-Match $storageTests 'atomic target parent was renameable after authority validation' 'atomic parent ancestry anchor race test'
Assert-Match $storageTests 'atomic replacement must reject a reparse parent before staging[\s\S]*?parent-reparse rejection generated a temporary filename' 'atomic parent-reparse fail-before-stage test'
Assert-Match $storageTests 'the old handle must continue to expose the replaced file bytes[\s\S]*?a new open of the target name must expose the replacement bytes' 'atomic old-reader/new-reader identity test'
Assert-Match $storageTests 'a reader without delete sharing must reject handle-relative[\s\S]*?replacement[\s\S]*?a reader sharing conflict must preserve the target name and old bytes' 'atomic incompatible-reader conflict test'
Assert-Match $storageTests 'exclusive staging did not deny a concurrent writer[\s\S]*?handle-relative publish must contain a final leaf-name swap[\s\S]*?target name must resolve to the exact staged bytes' 'atomic final-leaf substitution and staged-file tamper containment test'
Assert-Match $storageTests 'NativeFileLinkInformation\s*=\s*11U[\s\S]*?the final-stage hard-link injection must execute[\s\S]*?a detected final-stage hard link must prevent native publication[\s\S]*?final-stage hard-link rejection modified the original target' 'atomic deterministic prepublication hard-link rejection test' -CaseSensitive
Assert-Match $storageTests 'atomicCrashChildRequested\s*\(\s*\)[\s\S]*?tests\.clear\s*\(\s*\)[\s\S]*?storage\.atomic\.crash-recovery-child' 'atomic crash-child mode isolates the native residue fixture' -CaseSensitive
Assert-Match $storageTests 'openAtomicCrashParent[\s\S]*?FILE_LIST_DIRECTORY[\s\S]*?FILE_ADD_FILE[\s\S]*?FILE_DELETE_CHILD[\s\S]*?FILE_SHARE_READ[\s\S]*?createAtomicCrashStage[\s\S]*?RelativeOpenDisposition::CreateNew[\s\S]*?FILE_ATTRIBUTE_TEMPORARY[\s\S]*?writeThrough\s*=\s*true[\s\S]*?openRelative' 'atomic crash fixture creates production-shaped stages beneath a strong parent handle' -CaseSensitive
Assert-Match $storageTests 'class\s+AtomicCrashChildProcess\s+final[\s\S]*?~AtomicCrashChildProcess[\s\S]*?TerminateProcess[\s\S]*?WaitForSingleObject[\s\S]*?terminateAndClose' 'atomic crash child process has bounded RAII termination and handle ownership' -CaseSensitive
Assert-Match $storageTests 'atomicCrashRestartReclaimsClosedStagesAndProtectsLiveStage[\s\S]*?CreateProcessW[\s\S]*?WaitForMultipleObjects[\s\S]*?child\.terminateAndClose[\s\S]*?forced process death must leave both closed crash-stage names[\s\S]*?restartedEngine\.replace[\s\S]*?restart recovery must delete both exact closed crash-stage names[\s\S]*?restart recovery must preserve the exact identity of an active live stage' 'atomic restart test force-terminates a ready child, reclaims pre/post-flush residues, and preserves a live stage' -CaseSensitive
Assert-Match $storageTests 'immediate identity recheck must reject a precommit reparse[\s\S]*?substitution[\s\S]*?precommit reparse substitution must not reach native publication' 'atomic target-reparse substitution rejection test'
Assert-Match $storageTests 'immediate backup identity recheck must reject substitution[\s\S]*?precommit backup identity substitution[\s\S]*?must not reach native publication' 'atomic backup-identity substitution rejection test'
Assert-Match $storageTests 'unsupported-volume rejection must precede all staging and publication' 'atomic unsupported-POSIX fail-before-stage test'
Assert-Match $storageTests 'preserve the exact DACL[\s\S]*?protection bit[\s\S]*?preserve the target creation time[\s\S]*?reject an existing alternate data stream[\s\S]*?reject an existing compressed file' 'atomic metadata preservation and unsupported-metadata rejection test'
Assert-Match $storageTests 'atomic temporary-name collision exhaustion must fail closed' 'atomic CSPRNG collision retry-bound test'
Assert-Match $storageTests 'recovery backup must publish before the target rename is attempted[\s\S]*?published recovery backup must contain the byte-for-byte old target' 'atomic backup-before-target failure recovery test'
Assert-Match $storageTests 'target prepublish cancellation hook must[\s\S]*?reach the final linearization boundary[\s\S]*?prepublish cancellation must allow only the earlier recovery-backup[\s\S]*?publication[\s\S]*?target prepublish cancellation must preserve the old target bytes' 'atomic final-prepublish cancellation test'
Assert-Match $storageTests 'configurationRecoversOnlyFromValidBackup[\s\S]*?corrupt primary[\s\S]*?valid sibling backup[\s\S]*?corrupt backup[\s\S]*?IntegrityFailure' 'configuration valid-backup-only recovery test'
Assert-Match $storageTests 'preserve unknown top-level fields[\s\S]*?preserve unknown nested fields' 'configuration forward-field preservation test'
Assert-Match $storageTests 'duplicate configuration keys[\s\S]*?depth cap\+1[\s\S]*?secret-bearing configuration' 'configuration hostile JSON tests'
Assert-Match $storageTests 'failed durable replacement must not publish[\s\S]*?cancellation after durable linearization must still return success' 'configuration commit-before-publish and late-cancellation test'
Assert-Match $storageTests 'MaximumKeyBytes[\s\S]*?MaximumSecretBytes\s*\+\s*1U[\s\S]*?MaximumEntryCount' 'DPAPI exact key/secret/entry bound tests' -CaseSensitive
Assert-Match $storageTests 'DPAPI corrupt binary envelope[\s\S]*?integrity_failure' 'DPAPI tamper test'
Assert-Match $storageTests 'Domain::ErrorCodes::IntegrityFailure[\s\S]*?non-binary registry value' 'DPAPI hostile registry type test' -CaseSensitive
Assert-Match $storageTests 'void\s+dpapiRejectsDifferentEffectiveSid\s*\(' 'DPAPI different-effective-SID denial test exists'
Assert-Match $storageTests 'addTest\s*\(\s*tests\s*,\s*"storage\.dpapi\.different-sid-denial"\s*,\s*dpapiRejectsDifferentEffectiveSid\s*\)' 'different-SID test has exact G06 registration'
Assert-Match $storageTests 'S-1-5-7' 'different-SID test uses the fixed effective alternate SID'
Assert-Match $storageTests '\[EVIDENCE\]\s+dpapi_owner_sid=' 'different-SID test emits the owner SID evidence field'
Assert-Match $storageTests 'alternate_sid=' 'different-SID test emits the alternate SID evidence field'
Assert-Match $storageTests 'unprotect_error=' 'different-SID test emits the CryptUnprotectData error evidence field'
Assert-Match $storageTests '(?s)unprotectError.*?ERROR_ACCESS_DENIED' 'different-SID test requires CryptUnprotectData error 5 access denial'
Assert-Match $diagnosticTests 'directory creation must anchor the parent before the child[\s\S]*?diagnostic parent writes must fail because its no-write handle[\s\S]*?an anchored diagnostic parent must reject replacement during child' 'diagnostic parent-junction substitution race test'
Assert-Match $storageTests 'calls after shutdown must fail closed' 'storage services reject post-shutdown work'

foreach ($canary in @(
    'COOKIE_CANARY_4317',
    'AUTH_CANARY_4317',
    'BEARER_CANARY_4317',
    'PRIVATE_KEY_CANARY_4317',
    'API_KEY_CANARY_4317',
    'CLIENT_SECRET_CANARY_4317',
    'PROMPT_CANARY_4317',
    'PATH_CANARY_4317')) {
    Assert-Match $diagnosticTests ([regex]::Escape($canary)) "diagnostic privacy canary $canary" -CaseSensitive
}
foreach ($surface in @(
    'recent ring',
    'JSONL',
    'JSON export',
    'Markdown export')) {
    Assert-Match $diagnosticTests ([regex]::Escape($surface)) "diagnostic canary scan surface $surface" -CaseSensitive
}
Assert-Match $diagnosticTests 'default diagnostic export must use the distinct exports root' 'diagnostic default export-root test'
Assert-Match $diagnosticTests 'restart export must merge durable and local records without duplicates' 'diagnostic restart/shared-durable export test'
Assert-Match $diagnosticTests 'durable export must use deterministic timestamp ordering' 'diagnostic export deterministic ordering test'
Assert-Match $diagnosticTests 'second export must replace through Write authority deterministically' 'diagnostic export Create-to-Write transition test'
Assert-Match $diagnosticTests 'std::jthread\s+left[\s\S]*?std::jthread\s+right[\s\S]*?concurrent JSONL records must never interleave' 'cross-instance diagnostic serialization test' -CaseSensitive
Assert-Match $diagnosticTests 'rotation must retain no more than the configured file count[\s\S]*?active file byte cap' 'diagnostic exact rotation count and byte-cap test'
Assert-Match $diagnosticTests 'profile downshift must prune stale higher generations' 'diagnostic profile-downshift self-healing test'
Assert-Match $diagnosticTests 'flattened diagnostic fields must enforce the 512-byte cap[\s\S]*?basename must reject traversal[\s\S]*?basename must enforce its byte cap' 'diagnostic boundary tests'
Assert-Match $diagnosticTests 'cancelled diagnostic append must fail before mutation[\s\S]*?reject records after shutdown[\s\S]*?reject exports after shutdown' 'diagnostic cancellation and shutdown tests'
Assert-Match $diagnosticTests 'sink\.reset\s*\(\s*\)[\s\S]*?active diagnostic call must retain every injected dependency[\s\S]*?diagnostic dependencies must release with the final Impl owner' 'active diagnostic facade destruction retains dependency lifetimes through the call' -CaseSensitive
Assert-Match $diagnosticTests 'CreateHardLinkW[\s\S]*?multiply linked diagnostic leaf must fail closed[\s\S]*?hard-link rejection must not mutate the outside canary' 'diagnostic hard-link authority test' -CaseSensitive
Assert-Match $diagnosticTests 'FileCaseSensitiveInfo[\s\S]*?case-sensitive directory must retain case-twin diagnostic leaves[\s\S]*?case-sensitive diagnostic directory must be rejected before' 'diagnostic case-sensitive-directory integration test' -CaseSensitive
Assert-Match $diagnosticTests 'partial rotation must not publish its canonical archive name[\s\S]*?in-progress diagnostic rotation temporary must deny external[\s\S]*?cancellation during rotation copy must fail before publication[\s\S]*?colliding diagnostic rotation must not replace its destination canary' 'diagnostic exclusive-stage cancellation and collision tests'
Assert-Match $diagnosticTests 'native FileLinkInformation must add a link through the pre-opened[\s\S]*?PathOutsideAuthority[\s\S]*?preserve its complete source[\s\S]*?must not publish a canonical archive[\s\S]*?same-token residual boundary' 'diagnostic prepublication hard-link rejection preserves source and does not claim alias authority' -CaseSensitive
Assert-Match $diagnosticTests 'preCrashSource\s*=\s*readText\s*\(\s*master\s*\)[\s\S]*?CreateProcessW[\s\S]*?crash child must reach its noncanonical staging copy[\s\S]*?process termination during rotation must preserve the exact source bytes[\s\S]*?process termination during rotation must not leave a partial[\s\S]*?restart must publish the complete pre-crash source as the first[\s\S]*?restart must publish the exact pre-crash source bytes as the first archive' 'diagnostic crash-restart subprocess proves byte-exact source preservation and archive recovery' -CaseSensitive

Assert-Match $processTests 'command-line quoting did not preserve argv' 'CreateProcessW quoting integration test'
Assert-Match $processTests 'explicit Unicode environment block did not reach the child[\s\S]*?authorized working directory did not reach CreateProcessW' 'process environment and working-directory tests'
Assert-Match $processTests 'process launch observer did not reach the pre-CreateProcess boundary[\s\S]*?executable ancestry was renameable before CreateProcessW returned[\s\S]*?working-directory ancestry was renameable before CreateProcessW returned[\s\S]*?process launch observer did not reach the post-CreateProcess boundary[\s\S]*?executable ancestry lease remained open after CreateProcessW returned[\s\S]*?working-directory ancestry lease remained open after CreateProcessW returned' 'process executable and working-directory ancestry launch-boundary test'
Assert-Match $processTests 'CreateHardLinkW[\s\S]*?hard-linked executable escaped process authority[\s\S]*?hard-link rejection modified the out-of-authority executable' 'process hard-link authority-denial test' -CaseSensitive
Assert-Match $processTests 'stdout did not retain its independent exact cap[\s\S]*?stderr did not retain its independent exact cap' 'independent stdout/stderr cap tests'
Assert-Match $processTests 'operation deadline exceeded its bounded termination path' 'process deadline test'
Assert-Match $processTests 'targeted cancellation terminated an unrelated operation' 'targeted cancellation isolation test'
Assert-Match $processTests 'default process launch leaked the ambient SystemRoot variable[\s\S]*?non-allowlisted PATH[\s\S]*?credential marker[\s\S]*?case-insensitive explicit environment override' 'process environment isolation and sanitization tests'
Assert-Match $processTests 'KILL_ON_JOB_CLOSE did not terminate the descendant tree' 'descendant Job termination test'
Assert-Match $processTests 'shell-disabled authority launched a process[\s\S]*?missing absolute executable' 'process authority and launch-failure tests'
Assert-Match $processTests 'exact native command-line ceiling was rejected[\s\S]*?cap-plus-one was accepted' 'process command-line exact/cap-plus-one tests'
Assert-Match $processTests 'exact Forge environment-block ceiling was rejected[\s\S]*?environment-block cap-plus-one was accepted' 'process environment exact/cap-plus-one tests'
Assert-Match $processTests 'testConcurrentAdmissionIsExactly64[\s\S]*?MaximumConcurrentProcessOperations[\s\S]*?65th process operation was queued or admitted' 'process exact 64/65 admission test' -CaseSensitive
Assert-Match $processTests 'repeated process runs retained native handles' 'process handle ownership test'
Assert-Match $processTests 'childProcesses\s*==\s*1U[\s\S]*?processReaders\s*==\s*2U[\s\S]*?childProcesses\s*==\s*0U[\s\S]*?processReaders\s*==\s*0U' 'process runtime ownership composition and release test' -CaseSensitive
Assert-Match $processTests 'supervisor\.reset\s*\(\s*\)[\s\S]*?supervisor destruction did not cancel active work safely' 'active process facade destruction lifetime test'
Assert-Match $processTests 'UNC executable reached synchronous path authorization[\s\S]*?device-namespace working directory reached synchronous path authorization[\s\S]*?UNC preflight did not precede authority evaluation' 'UNC and device paths fail in local preflight before authorization' -CaseSensitive
Assert-Match $pipeReaderTests 'PeekNamedPipe failure was not preserved[\s\S]*?pending pipe cancellation exceeded its bounded reap path[\s\S]*?completion-port shutdown exceeded its bounded worker acknowledgement path[\s\S]*?normal broken-pipe EOF surfaced as an output error' 'pipe-reader error, bounded-cancellation, shutdown, and EOF tests' -CaseSensitive

foreach ($ownerKind in $ownerKinds) {
    Assert-Match $shutdownTests ('RuntimeOwnerKind::' + $ownerKind) "shutdown test covers runtime owner $ownerKind" -CaseSensitive
}
Assert-Match $shutdownTests 'TelemetryPendingSnapshot[\s\S]*?LimitExceeded' 'telemetry latest-value capacity-one test'
Assert-Match $shutdownTests 'MaximumConcurrentProcessOperations[\s\S]*?ChildProcess[\s\S]*?LimitExceeded' 'runtime child-process 64/65 capacity test'
Assert-Match $shutdownTests 'diagnostics\.shutdown\s*\(\s*\)[\s\S]*?admitted ownership after shutdown[\s\S]*?exposed a snapshot after shutdown' 'runtime diagnostics post-shutdown failure test'
Assert-Match $shutdownTests 'weak-lifetime|leasesNeverProlongRegistryLifetime' 'runtime lease weak-lifetime test'
function Get-CMakeExecutableSources {
    param([string]$Target)
    $match = [regex]::Match(
        $cmake,
        ('add_executable\s*\(\s*' + [regex]::Escape($Target) +
         '(?<body>.*?)\)'),
        [Text.RegularExpressions.RegexOptions]::Singleline)
    Assert-True $match.Success "CMake executable target $Target"
    return @($match.Groups['body'].Value -split '\s+' |
        Where-Object { $_ -match '^(?:tests|src)/.+' })
}

Assert-Set (Get-CMakeExecutableSources 'ForgeConductor.Infrastructure.UnitTests') @(
    'tests/Infrastructure/FoundationWindowsTests.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/StorageWindowsTests.cpp',
    'tests/Infrastructure/WindowsDiagnosticSinkTests.cpp') 'CMake infrastructure unit-test source inventory'
Assert-Set (Get-CMakeExecutableSources 'ForgeConductor.Infrastructure.ShutdownTests') @(
    'tests/Infrastructure/ShutdownWindowsTests.cpp') 'CMake infrastructure shutdown-test source inventory'
Assert-Set (Get-CMakeExecutableSources 'ForgeConductor.ProcessFixture') @(
    'tests/Infrastructure/Windows/ProcessFixture.cpp') 'CMake process-fixture source inventory'
Assert-Set (Get-CMakeExecutableSources 'ForgeConductor.Infrastructure.ProcessTests') @(
    'tests/Infrastructure/ProcessIntegrationTestMain.cpp',
    'tests/Infrastructure/Windows/OverlappedPipeReaderTests.cpp',
    'tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp') 'CMake process-test source inventory'
Assert-Set (Get-CMakeExecutableSources 'ForgeConductor.Infrastructure.HeaderSelfContainment') @(
    'tests/Architecture/P06HeaderSelfContainmentMain.cpp') 'CMake P06 header-test main inventory'

$cmakeG06Tests = @([regex]::Matches(
    $cmake,
    'add_test\s*\(\s*NAME\s+(?<name>ForgeConductor\.Infrastructure\.[A-Za-z]+)',
    [Text.RegularExpressions.RegexOptions]::Singleline) |
        ForEach-Object { $_.Groups['name'].Value })
Assert-Set $cmakeG06Tests @(
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.ShutdownTests',
    'ForgeConductor.Infrastructure.UnitTests') 'exact four G06 CTest definitions'
Assert-NoMatch $cmake 'add_test\s*\(\s*NAME\s+ForgeConductor\.ProcessFixture\b' 'process fixture is build-only, never an independent test' -CaseSensitive
Assert-Match $cmake 'add_dependencies\s*\(\s*ForgeConductor\.Infrastructure\.ProcessTests\s+ForgeConductor\.ProcessFixture\s*\)' 'process tests build their exact fixture' -CaseSensitive
Assert-Match $cmake 'NAME\s+ForgeConductor\.Infrastructure\.ProcessTests\s+COMMAND\s+\$<TARGET_FILE:ForgeConductor\.Infrastructure\.ProcessTests>\s+"\$<TARGET_FILE:ForgeConductor\.ProcessFixture>"' 'process CTest command receives the generated fixture path' -CaseSensitive
Assert-Match $cmake 'ForgeConductor\.Infrastructure\.UnitTests\s+PROPERTIES\s+LABELS\s+"T-UNIT;T-SEC;G06"' 'unit-test exact G06 labels' -CaseSensitive
Assert-Match $cmake 'ForgeConductor\.Infrastructure\.ShutdownTests\s+PROPERTIES\s+LABELS\s+"T-UNIT;T-STRESS;G06"' 'shutdown-test exact G06 labels' -CaseSensitive
Assert-Match $cmake 'ForgeConductor\.Infrastructure\.ProcessTests\s+PROPERTIES\s+LABELS\s+"T-PROC;T-SEC;G06"' 'process-test exact G06 labels' -CaseSensitive
Assert-Match $cmake 'ForgeConductor\.Infrastructure\.HeaderSelfContainment\s+PROPERTIES\s+LABELS\s+"T-UNIT;G06"' 'header-test exact G06 labels' -CaseSensitive
Assert-Match $cmake 'file\s*\(\s*GLOB\s+_forge_infrastructure_windows_headers\s+CONFIGURE_DEPENDS\s+"\$\{PROJECT_SOURCE_DIR\}/include/ForgeConductor/Infrastructure/Windows/\*\.h"\s*\)' 'P06 public-header isolation glob' -CaseSensitive
Assert-Match $cmake 'generated/p06-header-isolation' 'P06 isolated-header generation directory' -CaseSensitive
Assert-Match $cmake 'add_library\s*\(\s*ForgeConductor\.InfrastructureWindows\.HeaderObjects\s+OBJECT' 'P06 headers compile as independent object translation units' -CaseSensitive

$frameworkRoot = Join-Path $WorkspaceRoot '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count before P06 builds'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count before P06 builds'
Assert-Exact ([string]$frameworkBefore.sha256) 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' 'sealed Forsetti hash before P06 builds'

if ($StaticOnly) {
    Write-Host "G06 static validation passed: $script:AssertionCount assertions."
    return
}

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G06: building complete x64 Debug tree from a fresh build directory.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Write-Host 'G06: testing x64 Debug Windows infrastructure.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G06
Write-Host 'G06: testing x64 Debug P05 regression.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G05
Write-Host 'G06: testing x64 Debug P04 regression.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G04
Write-Host 'G06: building complete x64 Release tree.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Write-Host 'G06: testing x64 Release Windows infrastructure.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G06
Write-Host 'G06: testing x64 Release P05 regression.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G05
Write-Host 'G06: testing x64 Release P04 regression.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G04

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if ($ctestCommand) {
    $ctestPath = $ctestCommand.Source
} else {
    $toolchainStatePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
    Assert-True (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) 'toolchain state for CTest resolution'
    $toolchainState = Read-Json $toolchainStatePath
    $ctestCandidate = [string]$toolchainState.tools.ctest
    Assert-True (-not [string]::IsNullOrWhiteSpace($ctestCandidate) -and
        (Test-Path -LiteralPath $ctestCandidate -PathType Leaf)) 'CTest executable from toolchain state'
    $ctestPath = (Resolve-Path -LiteralPath $ctestCandidate).Path
}

$expectedG06Tests = @(
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.ShutdownTests',
    'ForgeConductor.Infrastructure.UnitTests')
$expectedArtifacts = @(
    'lib/{0}/ForgeConductor.Domain.lib',
    'lib/{0}/ForgeConductor.Infrastructure.Windows.lib',
    'bin/{0}/ForgeConductor.Contracts.ContractTests.exe',
    'bin/{0}/ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Domain.UnitTests.exe',
    'bin/{0}/ForgeConductor.ForsettiHostSmoke.exe',
    'bin/{0}/ForgeConductor.Infrastructure.HeaderSelfContainment.exe',
    'bin/{0}/ForgeConductor.Infrastructure.ProcessTests.exe',
    'bin/{0}/ForgeConductor.Infrastructure.ShutdownTests.exe',
    'bin/{0}/ForgeConductor.Infrastructure.UnitTests.exe',
    'bin/{0}/ForgeConductor.ProcessFixture.exe',
    'bin/{0}/forge-conductor.exe')
$artifactHashes = [ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    foreach ($artifactTemplate in $expectedArtifacts) {
        $relativeArtifact = $artifactTemplate -f $configuration
        $artifactPath = Join-Path $buildRoot $relativeArtifact.Replace('/', '\')
        Assert-True (Test-Path -LiteralPath $artifactPath -PathType Leaf) "$configuration artifact missing: $relativeArtifact"
        Assert-True ([long](Get-Item -LiteralPath $artifactPath).Length -gt 0) "$configuration artifact is empty: $relativeArtifact"
        $hash = Get-FileSha256 $artifactPath
        Assert-Match $hash '^[0-9a-f]{64}$' "$configuration artifact hash: $relativeArtifact" -CaseSensitive
        $artifactHashes[$configuration + '/' + $relativeArtifact] = $hash
    }

    $ctestJsonText = (& $ctestPath --test-dir $buildRoot -C $configuration -L G06 --show-only=json-v1) -join [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 "$configuration G06 CTest JSON inventory command"
    try {
        $ctestInventory = $ctestJsonText | ConvertFrom-Json
    } catch {
        throw "G06 assertion failed: invalid $configuration CTest JSON inventory - $($_.Exception.Message)"
    }
    Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) $expectedG06Tests "$configuration exact G06 CTest inventory"

    $buildRootForward = $buildRoot.Replace('\', '/')
    $expectedCTestCommands = [ordered]@{
        'ForgeConductor.Infrastructure.UnitTests' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Infrastructure.UnitTests.exe")
        'ForgeConductor.Infrastructure.ShutdownTests' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Infrastructure.ShutdownTests.exe")
        'ForgeConductor.Infrastructure.ProcessTests' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Infrastructure.ProcessTests.exe",
            "$buildRootForward/bin/$configuration/ForgeConductor.ProcessFixture.exe")
        'ForgeConductor.Infrastructure.HeaderSelfContainment' = @(
            "$buildRootForward/bin/$configuration/ForgeConductor.Infrastructure.HeaderSelfContainment.exe")
    }
    $expectedCTestLabels = [ordered]@{
        'ForgeConductor.Infrastructure.UnitTests' = @('G06','T-SEC','T-UNIT')
        'ForgeConductor.Infrastructure.ShutdownTests' = @('G06','T-STRESS','T-UNIT')
        'ForgeConductor.Infrastructure.ProcessTests' = @('G06','T-PROC','T-SEC')
        'ForgeConductor.Infrastructure.HeaderSelfContainment' = @('G06','T-UNIT')
    }
    foreach ($test in @($ctestInventory.tests)) {
        $testName = [string]$test.name
        Assert-Exact ([string]$test.config) $configuration "$configuration CTest configuration for $testName"
        Assert-Set @($test.properties | ForEach-Object { $_.name }) @(
            'LABELS',
            'WORKING_DIRECTORY') "$configuration CTest property inventory for $testName"

        $expectedCommand = @($expectedCTestCommands[$testName])
        Assert-Exact @($test.command).Count $expectedCommand.Count "$configuration CTest command count for $testName"
        for ($commandIndex = 0; $commandIndex -lt $expectedCommand.Count; $commandIndex++) {
            Assert-Exact ([string]$test.command[$commandIndex]) $expectedCommand[$commandIndex] "$configuration CTest command item $commandIndex for $testName"
        }

        $labelsProperty = @($test.properties | Where-Object { $_.name -ceq 'LABELS' })
        Assert-Exact $labelsProperty.Count 1 "$configuration CTest label property count for $testName"
        Assert-Set @($labelsProperty[0].value) @($expectedCTestLabels[$testName]) "$configuration exact CTest labels for $testName"

        $workingDirectoryProperty = @($test.properties | Where-Object {
            $_.name -ceq 'WORKING_DIRECTORY'
        })
        Assert-Exact $workingDirectoryProperty.Count 1 "$configuration CTest working-directory property count for $testName"
        Assert-Exact ([string]$workingDirectoryProperty[0].value) $buildRootForward "$configuration CTest working directory for $testName"
    }
}

$generatedHeaderRoot = Join-Path $buildRoot 'generated\p06-header-isolation'
$generatedHeaderSources = @(Get-ChildItem -LiteralPath $generatedHeaderRoot -File -Filter '*.cpp')
Assert-Exact $generatedHeaderSources.Count 13 'generated P06 isolated public-header translation-unit count'
$expectedHeaderSources = [ordered]@{}
foreach ($header in $publicHeaders) {
    $includePath = "ForgeConductor/Infrastructure/Windows/$($header.Name)"
    $stem = $includePath.Replace('/', '_').Replace('.', '_')
    $expectedHeaderSources["$stem.cpp"] =
        "#include <$includePath>" + [char]10 +
        'int ' + $stem + '_isolated() noexcept { return 0; }'
}
Assert-Set @($generatedHeaderSources.Name) @($expectedHeaderSources.Keys) 'exact generated P06 isolated-header translation-unit inventory'
foreach ($source in $generatedHeaderSources) {
    $sourceText = (Get-Content -Raw -LiteralPath $source.FullName).
        Replace([string][char]13 + [char]10, [string][char]10).
        TrimEnd([char[]]@([char]13,[char]10))
    Assert-Exact $sourceText $expectedHeaderSources[$source.Name] "generated isolated-header translation unit $($source.Name)"
}
$directoryBuildProps = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'Directory.Build.props')
Assert-Match $directoryBuildProps '<CppLanguageStandard>stdcpp20</CppLanguageStandard>' 'repository C++20 pin' -CaseSensitive
Assert-Match $directoryBuildProps '<ConformanceMode>true</ConformanceMode>' 'repository MSVC conformance pin' -CaseSensitive
Assert-Match $directoryBuildProps '<TreatWarningAsError>true</TreatWarningAsError>' 'repository warnings-as-errors pin' -CaseSensitive
Assert-Match $directoryBuildProps '<PlatformToolset>v143</PlatformToolset>' 'repository MSVC v143 pin' -CaseSensitive
Assert-Match $directoryBuildProps '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' 'repository Windows SDK pin' -CaseSensitive
Assert-Match $directoryBuildProps '<WindowsTargetPlatformMinVersion>10\.0\.22000\.0</WindowsTargetPlatformMinVersion>' 'repository Windows 11 minimum pin' -CaseSensitive

$p06ProjectNames = @(
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.ShutdownTests',
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.Infrastructure.Windows',
    'ForgeConductor.InfrastructureWindows.HeaderObjects',
    'ForgeConductor.ProcessFixture')
foreach ($projectName in $p06ProjectNames) {
    $projectPath = Join-Path $buildRoot "$projectName.vcxproj"
    Assert-True (Test-Path -LiteralPath $projectPath -PathType Leaf) "generated P06 project $projectName"
    $project = Get-Content -Raw -LiteralPath $projectPath
    Assert-Match $project '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' "$projectName SDK 10.0.26100.0" -CaseSensitive
    Assert-Exact ([regex]::Matches($project, '<PlatformToolset>v143</PlatformToolset>').Count) 2 "$projectName Debug/Release v143 toolset count"
    Assert-Exact ([regex]::Matches($project, '<LanguageStandard>stdcpp20</LanguageStandard>').Count) 2 "$projectName Debug/Release C++20 count"
    Assert-Exact ([regex]::Matches($project, '<ConformanceMode>true</ConformanceMode>').Count) 2 "$projectName Debug/Release conformance count"
    Assert-Exact ([regex]::Matches($project, '<WarningLevel>Level4</WarningLevel>').Count) 2 "$projectName Debug/Release warning-level count"
    Assert-Exact ([regex]::Matches($project, '<TreatWarningAsError>true</TreatWarningAsError>').Count) 2 "$projectName Debug/Release warnings-as-errors count"
    Assert-Exact ([regex]::Matches($project, '<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>').Count) 1 "$projectName Debug DLL CRT"
    Assert-Exact ([regex]::Matches($project, '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>').Count) 1 "$projectName Release DLL CRT"
    Assert-NoMatch $project '<CLRSupport>|MultiThreadedDebug</RuntimeLibrary>|MultiThreaded</RuntimeLibrary>' "$projectName must not enable CLR or static CRT" -CaseSensitive
}

$infrastructureProjectPath = Join-Path $buildRoot 'ForgeConductor.Infrastructure.Windows.vcxproj'
$infrastructureProject = Get-Content -Raw -LiteralPath $infrastructureProjectPath
$infrastructureReferences = @([regex]::Matches(
    $infrastructureProject,
    '<ProjectReference\s+Include="(?<path>[^"]+)"') |
        ForEach-Object { [IO.Path]::GetFileName($_.Groups['path'].Value) })
Assert-Set $infrastructureReferences @(
    'ForgeConductor.Domain.vcxproj',
    'ZERO_CHECK.vcxproj') 'infrastructure generated-project reference inventory'
$infrastructureDependencySurface = @([regex]::Matches(
    $infrastructureProject,
    '<(?:AdditionalIncludeDirectories|AdditionalDependencies|AdditionalLibraryDirectories|ForcedIncludeFiles)>[^<]*</(?:AdditionalIncludeDirectories|AdditionalDependencies|AdditionalLibraryDirectories|ForcedIncludeFiles)>|<(?:ClCompile|ProjectReference)\s+Include="[^"]+"') |
        ForEach-Object { $_.Value }) -join [Environment]::NewLine
Assert-Match $infrastructureDependencySurface '<AdditionalIncludeDirectories>' 'infrastructure generated-project dependency surface was captured' -CaseSensitive
Assert-NoMatch $infrastructureDependencySurface '(?:Forsetti|Boost|Qt|Python|node|Electron|System\.Runtime|mscoree)' 'infrastructure project dependency leakage'

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) 'sealed Forsetti file count after P06 builds'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) 'sealed Forsetti bytes after P06 builds'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) 'sealed Forsetti hash after P06 builds'

$gitOutput = & git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1
Assert-Exact $LASTEXITCODE 0 ('git diff --check failed: ' + ($gitOutput -join [Environment]::NewLine))
& (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Verify-Ledger.ps1') -WorkspaceRoot $WorkspaceRoot

$productFileCount = $publicHeaders.Count + $topLevelSources.Count +
    $detailHeaders.Count + $detailSources.Count
Write-Host "G06 Windows infrastructure validation passed: $script:AssertionCount fail-closed assertions; $productFileCount product files, 13 isolated headers, $($artifactHashes.Count) binary hashes, and x64 Debug/Release G04+G05+G06 tests passed."
