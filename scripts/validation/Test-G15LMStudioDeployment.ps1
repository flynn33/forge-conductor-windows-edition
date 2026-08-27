[CmdletBinding(DefaultParameterSetName = 'Normal')]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [string]$LMStudioConfigurationPath,

    [string]$RealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [switch]$Resume,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$PriorFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$PriorFailedRealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$CorrectiveCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$SecondFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$SecondFailedRealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$RunnerCorrectiveCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$ThirdFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$ThirdFailedRealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$DeploymentCorrectiveCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$FourthFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$FourthFailedRealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$ProcessCorrectiveBuildCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$ProcessCorrectiveTestCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$FifthFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$FifthFailedRealHostEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$McpCorrectiveFailedCommandRecordPath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$McpCorrectiveCommandRecordPath,

    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0
$realHostTimeoutMilliseconds = 540000
$maximumCommandRecordBytes = 1MB
$maximumCommandStdoutBytes = 64MB
$maximumCommandStderrBytes = 16MB
$maximumRealHostEvidenceBytes = 1MB
$emptySha256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G15 assertion failed: $Message" }
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

function Invoke-RepositoryIntegrityChecks {
    $output = @(& git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('git diff --check: ' + ($output -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

function Assert-TrackedTreeClean {
    & git -C $WorkspaceRoot diff --quiet --
    Assert-Exact $LASTEXITCODE 0 'tracked working tree is clean'
    & git -C $WorkspaceRoot diff --cached --quiet --
    Assert-Exact $LASTEXITCODE 0 'tracked index is clean'
}

function Assert-NoUntrackedBuildInputs {
    $untracked = @(& git -C $WorkspaceRoot ls-files --others --exclude-standard --)
    Assert-Exact $LASTEXITCODE 0 'untracked-file inventory exit code'
    $buildInputs = @($untracked | Where-Object {
        $portable = ([string]$_).Replace('\\', '/')
        $portable -match '^(?:CMakeLists[.]txt|CMakePresets[.]json|cmake/|include/|src/|tests/|scripts/|vcpkg(?:-configuration|[.]json)|[.]forge-codex/instructions/|[.]forge-codex/state/decisions/)'
    })
    Assert-Exact $buildInputs.Count 0 `
        ('untracked source or build-input paths: ' + ($buildInputs -join ', '))
}

function Assert-DirectoryChainNoReparsePoint {
    param([string]$Path, [string]$Message)
    $resolvedRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    Assert-True ($resolvedPath.StartsWith(
        $resolvedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) "$Message remains inside the workspace"
    $relative = $resolvedPath.Substring($resolvedRoot.Length + 1)
    $current = $resolvedRoot
    foreach ($segment in $relative.Split(
                 [IO.Path]::DirectorySeparatorChar,
                 [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $segment
        $item = Get-Item -Force -LiteralPath $current
        Assert-True (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Message component is not a reparse point: $segment"
    }
}

function Assert-LiteralOccurrenceCount {
    param(
        [string]$Text,
        [string]$Literal,
        [int]$Expected,
        [string]$Message)
    $count = [regex]::Matches($Text, [regex]::Escape($Literal)).Count
    Assert-Exact $count $Expected "$Message occurrence count"
}

function Resolve-CanonicalStateFile {
    param([string]$Path, [string]$Message, [long]$MaximumBytes)
    Assert-True (-not [string]::IsNullOrWhiteSpace($Path)) "$Message path is supplied"
    Assert-True ([IO.Path]::IsPathRooted($Path)) "$Message path is absolute"
    $fullPath = [IO.Path]::GetFullPath($Path)
    Assert-True ([string]::Equals(
        $Path,
        $fullPath,
        [StringComparison]::OrdinalIgnoreCase)) "$Message path is canonical"
    Assert-True (Test-Path -LiteralPath $fullPath -PathType Leaf) "$Message exists"
    $resolvedPath = (Resolve-Path -LiteralPath $fullPath).Path
    Assert-True ([string]::Equals(
        $fullPath,
        $resolvedPath,
        [StringComparison]::OrdinalIgnoreCase)) "$Message resolves without redirection"
    $stateRoot = (Resolve-Path -LiteralPath (Join-Path $WorkspaceRoot `
        '.forge-codex\state')).Path
    Assert-True ($resolvedPath.StartsWith(
        $stateRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) "$Message remains under .forge-codex/state"
    Assert-DirectoryChainNoReparsePoint $resolvedPath $Message
    $item = Get-Item -Force -LiteralPath $resolvedPath
    Assert-True ($item.Length -le $MaximumBytes) "$Message has bounded bytes"
    return $resolvedPath
}

function Resolve-CommandStreamFile {
    param(
        [string]$Reference,
        [string]$ExpectedLeaf,
        [long]$MaximumBytes,
        [string]$Message)
    Assert-True (-not [IO.Path]::IsPathRooted($Reference)) `
        "$Message record reference is workspace-relative"
    $candidate = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot $Reference))
    $resolved = Resolve-CanonicalStateFile $candidate $Message $MaximumBytes
    Assert-Exact ([IO.Path]::GetFileName($resolved)) $ExpectedLeaf `
        "$Message canonical leaf"
    Assert-Exact ((Get-RelativePathPortable `
        -BasePath $WorkspaceRoot -TargetPath $resolved).Replace('\', '/')) `
        $Reference.Replace('\', '/') "$Message canonical record reference"
    return $resolved
}

function Read-ValidatedCommandRecord {
    param(
        [string]$Path,
        [string]$ExpectedCommand,
        [int]$ExpectedExitCode,
        [string]$Message)
    $recordPath = Resolve-CanonicalStateFile `
        $Path $Message $maximumCommandRecordBytes
    Assert-True ([string]::Equals(
        (Split-Path -Parent $recordPath),
        (Join-Path $WorkspaceRoot '.forge-codex\state\commands'),
        [StringComparison]::OrdinalIgnoreCase)) "$Message is in the command-record directory"
    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    Assert-Set @($record.PSObject.Properties.Name) @(
        'schema_version',
        'id',
        'phase',
        'role',
        'command',
        'working_directory',
        'start_utc',
        'end_utc',
        'exit_code',
        'timed_out',
        'stdout',
        'stderr',
        'stdout_sha256',
        'stderr_sha256') "$Message schema"
    Assert-Exact ([int]$record.schema_version) 1 "$Message schema version"
    Assert-Match ([string]$record.id) `
        '^[0-9]{8}T[0-9]{9}Z-[0-9a-f]{8}$' "$Message ID" -CaseSensitive
    Assert-Exact ([IO.Path]::GetFileName($recordPath)) `
        (([string]$record.id) + '.json') "$Message record leaf"
    Assert-Exact ([string]$record.phase) 'P15' "$Message phase"
    Assert-Exact ([string]$record.role) 'builder' "$Message role"
    Assert-Exact ([string]$record.command) $ExpectedCommand "$Message command"
    Assert-True ([string]::Equals(
        [string]$record.working_directory,
        $WorkspaceRoot,
        [StringComparison]::OrdinalIgnoreCase)) "$Message working directory"
    Assert-Exact ([int]$record.exit_code) $ExpectedExitCode "$Message exit code"
    Assert-Exact ([bool]$record.timed_out) $false "$Message did not time out"
    $start = [DateTimeOffset]::MinValue
    $end = [DateTimeOffset]::MinValue
    Assert-True ([DateTimeOffset]::TryParse(
        [string]$record.start_utc, [ref]$start)) "$Message start timestamp"
    Assert-True ([DateTimeOffset]::TryParse(
        [string]$record.end_utc, [ref]$end)) "$Message end timestamp"
    Assert-True ($end -ge $start) "$Message timestamps are ordered"
    Assert-Match ([string]$record.stdout_sha256) `
        '^[0-9a-f]{64}$' "$Message stdout SHA-256 shape" -CaseSensitive
    Assert-Match ([string]$record.stderr_sha256) `
        '^[0-9a-f]{64}$' "$Message stderr SHA-256 shape" -CaseSensitive
    $stdoutPath = Resolve-CommandStreamFile `
        ([string]$record.stdout) (([string]$record.id) + '.stdout.txt') `
        $maximumCommandStdoutBytes "$Message stdout"
    $stderrPath = Resolve-CommandStreamFile `
        ([string]$record.stderr) (([string]$record.id) + '.stderr.txt') `
        $maximumCommandStderrBytes "$Message stderr"
    Assert-Exact (Get-FileSha256 $stdoutPath) `
        ([string]$record.stdout_sha256) "$Message stdout SHA-256"
    Assert-Exact (Get-FileSha256 $stderrPath) `
        ([string]$record.stderr_sha256) "$Message stderr SHA-256"
    return [pscustomobject]@{
        Record = $record
        RecordPath = $recordPath
        RecordSha256 = Get-FileSha256 $recordPath
        StdoutPath = $stdoutPath
        StdoutSha256 = [string]$record.stdout_sha256
        StdoutText = Get-Content -Raw -LiteralPath $stdoutPath
        StderrPath = $stderrPath
        StderrSha256 = [string]$record.stderr_sha256
        StderrBytes = [long](Get-Item -LiteralPath $stderrPath).Length
    }
}

function Assert-CommandLedgerEvent {
    param(
        [string]$RecordPath,
        [int]$ExpectedExitCode,
        [string]$Message)
    $relativeRecord = (Get-RelativePathPortable `
        -BasePath $WorkspaceRoot -TargetPath $RecordPath).Replace('\', '/')
    $matches = @()
    foreach ($line in Get-Content -LiteralPath (Join-Path $WorkspaceRoot `
            '.forge-codex\state\event-ledger.jsonl')) {
        $event = $line | ConvertFrom-Json
        if ([string]$event.action -ceq 'command_completed' -and
            ([string]$event.data.record).Replace('\', '/') -ceq $relativeRecord) {
            $matches += $event
        }
    }
    Assert-Exact $matches.Count 1 "$Message ledger event count"
    Assert-Exact ([string]$matches[0].phase) 'P15' "$Message ledger phase"
    Assert-Exact ([string]$matches[0].role) 'builder' "$Message ledger role"
    Assert-Exact ([int]$matches[0].data.exit_code) $ExpectedExitCode `
        "$Message ledger exit code"
    Assert-Exact ([bool]$matches[0].data.timed_out) $false `
        "$Message ledger did not time out"
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti tree hash'

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
$phase = @($phases.phases | Where-Object id -ceq 'P15')
Assert-Exact $phase.Count 1 'single P15 phase plan entry'
Assert-Exact ([string]$phase[0].title) `
    'Windows LM Studio environment and deployment' 'P15 title'
Assert-Set @($phase[0].dependencies) @('P14') 'P15 dependency'
Assert-Set @($phase[0].required_gates) @('G15') 'P15 required gate'
Assert-Set @($phase[0].required_work) @(
    'Evidence-based path/config discovery',
    'Transactional primary/fallback deploy',
    'Smoke, rollback, drift, host-sync tests') 'P15 required work'

$gates = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\gates.json') | ConvertFrom-Json
$gate = @($gates.gates | Where-Object id -ceq 'G15')
Assert-Exact $gate.Count 1 'single G15 gate plan entry'
Assert-Exact ([string]$gate[0].title) 'LM Studio deployment' 'G15 title'
Assert-Exact ([string]$gate[0].class) 'hard' 'G15 class'
Assert-Exact ([string]$gate[0].acceptance) `
    'Transactional deploy/smoke/rollback/drift pass; real-host evidence required when available.' `
    'G15 acceptance'

$matrix = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\test-matrix.json') | ConvertFrom-Json
$suite = @($matrix.suites | Where-Object id -ceq 'T-LMS')
Assert-Exact $suite.Count 1 'single T-LMS suite entry'
Assert-Exact ([string]$suite[0].scope) `
    'LM Studio discovery/deploy/rollback/smoke/host synchronization' `
    'T-LMS scope'
Assert-True ([bool]$suite[0].required) 'T-LMS remains required'

$p15Files = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Contracts/ILMStudioHostActivator.h',
    'include/ForgeConductor/Contracts/ILMStudioServeVerifier.h',
    'include/ForgeConductor/Domain/EnvironmentModels.h',
    'include/ForgeConductor/Domain/ProcessModels.h',
    'include/ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'src/Domain/EnvironmentModels.cpp',
    'src/Domain/ProcessModels.cpp',
    'src/Infrastructure/Windows/LMStudioConfigurationCodec.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioDeploymentService.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioEnvironment.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioHostActivator.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioServeVerifier.cpp',
    'src/Infrastructure/Windows/WindowsProcessSupervisor.cpp',
    'tests/Application/LMStudioDeploymentServiceTests.cpp',
    'tests/Contracts/main.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/LMStudioConfigurationCodecTests.cpp',
    'tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp',
    'tests/Infrastructure/WindowsLMStudioEnvironmentTests.cpp',
    'tests/Infrastructure/WindowsLMStudioHostActivatorTests.cpp',
    'tests/Infrastructure/WindowsLMStudioRealHostTests.cpp',
    'tests/Infrastructure/WindowsLMStudioServeVerifierTests.cpp',
    '.forge-codex/state/decisions/P15-001-evidence-based-lm-studio-deployment-and-maintenance-authority.md',
    'scripts/validation/Test-G15LMStudioDeployment.ps1')
foreach ($relative in $p15Files) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "P15 file exists: $relative"
    Assert-CrlfTextFile $path "P15 text file $relative"
}

$productionPaths = @($p15Files | Where-Object {
    $_ -match '^(?:include|src)/'
})
$productionText = ($productionPaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join "`n"
Assert-NoMatch $productionText 'C:\\Users\\' `
    'production code contains no machine-specific user path' -CaseSensitive
Assert-NoMatch $productionText `
    '\b(?:SendInput|mouse_event|keybd_event|IUIAutomation|SetCursorPos|FindWindowW)\b' `
    'LM Studio integration contains no external GUI automation'

$cmakeText = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
Assert-Match $cmakeText `
    'add_executable\(ForgeConductor[.]LMStudio[.]RealHostTests[\s\S]*?WindowsLMStudioRealHostTests[.]cpp\)' `
    'real-host runner target is declared' -CaseSensitive
Assert-NoMatch $cmakeText `
    'add_test\(\s*(?:NAME\s+)?ForgeConductor[.]LMStudio[.]RealHostTests' `
    'mutating real-host runner is not registered with CTest' -CaseSensitive
Assert-Match $cmakeText `
    'ForgeConductor[.]Contracts[.]ContractTests\s+PROPERTIES[\s\S]*?LABELS\s+"[^"]*G15' `
    'contract tests carry G15 label' -CaseSensitive
Assert-Match $cmakeText `
    'ForgeConductor[.]Infrastructure[.]ProcessTests\s+PROPERTIES[\s\S]*?LABELS\s+"[^"]*G15' `
    'process tests carry G15 label' -CaseSensitive

$adr = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\state\decisions\P15-001-evidence-based-lm-studio-deployment-and-maintenance-authority.md')
Assert-Match $adr 'one authoritative G15 rebuild/test invocation' `
    'owner-adjusted one-pass G15 scope is recorded'
Assert-Match $adr 'P15 does not click, type into,\s+inspect, or otherwise automate' `
    'external LM Studio GUI automation prohibition is recorded'
Assert-Match $adr 'Foreign server\s+entries and unknown root and per-server fields survive' `
    'foreign configuration preservation is recorded'
Assert-Match $adr 'restores the\s+exact original configuration bytes' `
    'exact rollback requirement is recorded'

Invoke-RepositoryIntegrityChecks

if ($StaticOnly -and -not $Resume) {
    Write-Host "G15 static validation passed ($script:AssertionCount assertions)."
    return
}

if ([string]::IsNullOrWhiteSpace($LMStudioConfigurationPath)) {
    $LMStudioConfigurationPath = Join-Path $env:USERPROFILE '.lmstudio\mcp.json'
}
$LMStudioConfigurationPath = (Resolve-Path -LiteralPath $LMStudioConfigurationPath).Path
$canonicalRealHostEvidencePath = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\windows-lm-studio-real-host.json'))
if ([string]::IsNullOrWhiteSpace($RealHostEvidencePath)) {
    $RealHostEvidencePath = $canonicalRealHostEvidencePath
} else {
    $RealHostEvidencePath = [IO.Path]::GetFullPath($RealHostEvidencePath)
}
Assert-True ([string]::Equals(
    $RealHostEvidencePath,
    $canonicalRealHostEvidencePath,
    [StringComparison]::OrdinalIgnoreCase)) `
    'real-host evidence uses the one canonical P15 evidence file'
$realHostEvidenceDirectory = Split-Path -Parent $RealHostEvidencePath
Assert-True (Test-Path -LiteralPath $realHostEvidenceDirectory -PathType Container) `
    'canonical P15 evidence directory already exists'
Assert-DirectoryChainNoReparsePoint $realHostEvidenceDirectory `
    'canonical P15 evidence directory'
Assert-True (-not (Test-Path -LiteralPath $RealHostEvidencePath)) `
    'canonical real-host evidence does not already exist before its one invocation'

$baselinePath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\windows-lm-studio-host-baseline.json'
$baseline = Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json
Assert-True (Test-Windows11) 'qualification host is Windows 11'
Assert-Exact ([int]$baseline.schema_version) 1 'real-host baseline schema version'
Assert-Exact ([string]$baseline.phase) 'P15' 'real-host baseline phase'
Assert-Exact ([string]$baseline.checkpoint) 'windows_lm_studio_host_baseline' `
    'real-host baseline checkpoint'
Assert-Exact ([string]$baseline.status) 'passed' 'real-host baseline status'
Assert-Exact ([string]$baseline.gate) 'G15' 'real-host baseline gate'
Assert-Exact ([string]$baseline.observation_mode) 'read_only' `
    'real-host baseline observation mode'
Assert-Exact ([int]$baseline.effective_exit_code) 0 `
    'real-host baseline command exit code'
Assert-Exact ([bool]$baseline.timed_out) $false `
    'real-host baseline command did not time out'
Assert-True ([bool]$baseline.lm_studio.executable_exists) `
    'baseline records an installed LM Studio executable'
$hostExecutable = [string]$baseline.lm_studio.executable
Assert-True (Test-Path -LiteralPath $hostExecutable -PathType Leaf) `
    'baseline LM Studio executable still exists'
Assert-Exact (Get-FileSha256 $hostExecutable) `
    ([string]$baseline.lm_studio.sha256) `
    'LM Studio executable still matches the captured host baseline'
Assert-Exact $LMStudioConfigurationPath `
    ([string]$baseline.configuration.path) `
    'qualification uses the captured LM Studio configuration path'
Assert-True ([bool]$baseline.configuration.valid_json) `
    'baseline LM Studio configuration was valid JSON'
Assert-Exact ([bool]$baseline.configuration.reparse_point) $false `
    'baseline LM Studio configuration was not a reparse point'
Assert-True (Test-Path -LiteralPath $LMStudioConfigurationPath -PathType Leaf) `
    'baseline LM Studio configuration still exists'
$configurationItem = Get-Item -Force -LiteralPath $LMStudioConfigurationPath
Assert-Exact (($configurationItem.Attributes -band `
        [IO.FileAttributes]::ReparsePoint) -ne 0) $false `
    'live LM Studio configuration is not a reparse point'

Assert-TrackedTreeClean
Assert-NoUntrackedBuildInputs

$buildTargets = @(
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.LMStudio.RealHostTests')
$expectedTests = @(
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.UnitTests')

$priorCommandEvidence = $null
$correctiveCommandEvidence = $null
$priorFailedEvidenceCanonicalPath = $null
$priorFailedEvidenceSha256 = $null
$secondFailedCommandEvidence = $null
$secondFailedEvidenceCanonicalPath = $null
$secondFailedEvidenceSha256 = $null
$runnerCorrectiveCommandEvidence = $null
$thirdFailedCommandEvidence = $null
$thirdFailedEvidenceCanonicalPath = $null
$thirdFailedEvidenceSha256 = $null
$deploymentCorrectiveCommandEvidence = $null
$fourthFailedCommandEvidence = $null
$fourthFailedEvidenceCanonicalPath = $null
$fourthFailedEvidenceSha256 = $null
$processCorrectiveBuildCommandEvidence = $null
$processCorrectiveTestCommandEvidence = $null
$fifthFailedCommandEvidence = $null
$fifthFailedEvidenceCanonicalPath = $null
$fifthFailedEvidenceSha256 = $null
$mcpCorrectiveFailedCommandEvidence = $null
$mcpCorrectiveCommandEvidence = $null
if ($Resume) {
    Write-Host 'G15: validating exact prior and corrective evidence for fail-closed resume.'
    Assert-True (-not [string]::Equals(
        $PriorFailedCommandRecordPath,
        $CorrectiveCommandRecordPath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'resume command-record paths are distinct'
    Assert-True (-not [string]::Equals(
        $PriorFailedRealHostEvidencePath,
        $RealHostEvidencePath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved failed evidence is distinct from the second-attempt evidence target'

    $gateScriptPath = Join-Path $WorkspaceRoot `
        'scripts\validation\Test-G15LMStudioDeployment.ps1'
    $expectedPriorCommand = "& '" + $gateScriptPath +
        "' -WorkspaceRoot '" + $WorkspaceRoot + "' -Parallel " + $Parallel
    $expectedCorrectiveCommand =
        "cmake --build 'out/build/windows-msvc-x64' --config Debug --target " +
        'ForgeConductor.Infrastructure.UnitTests ' +
        'ForgeConductor.LMStudio.RealHostTests --parallel ' + $Parallel +
        '; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ' +
        "& 'out/build/windows-msvc-x64/bin/Debug/" +
        "ForgeConductor.Infrastructure.UnitTests.exe'; exit `$LASTEXITCODE"
    $expectedSecondFailedCommand = $expectedPriorCommand + ' -Resume' +
        " -PriorFailedCommandRecordPath '" + $PriorFailedCommandRecordPath + "'" +
        " -PriorFailedRealHostEvidencePath '" + $PriorFailedRealHostEvidencePath + "'" +
        " -CorrectiveCommandRecordPath '" + $CorrectiveCommandRecordPath + "'"
    $expectedRunnerCorrectiveCommand =
        "cmake --build 'out/build/windows-msvc-x64' --config Debug --target " +
        'ForgeConductor.LMStudio.RealHostTests --parallel ' + $Parallel +
        '; exit $LASTEXITCODE'
    $expectedThirdFailedCommand = $expectedSecondFailedCommand +
        " -SecondFailedCommandRecordPath '" + $SecondFailedCommandRecordPath + "'" +
        " -SecondFailedRealHostEvidencePath '" + $SecondFailedRealHostEvidencePath + "'" +
        " -RunnerCorrectiveCommandRecordPath '" + $RunnerCorrectiveCommandRecordPath + "'"
    $expectedFourthFailedCommand = $expectedThirdFailedCommand +
        " -ThirdFailedCommandRecordPath '" + $ThirdFailedCommandRecordPath + "'" +
        " -ThirdFailedRealHostEvidencePath '" + $ThirdFailedRealHostEvidencePath + "'" +
        " -DeploymentCorrectiveCommandRecordPath '" +
        $DeploymentCorrectiveCommandRecordPath + "'"
    $expectedProcessCorrectiveBuildCommand =
        "cmake --build 'out/build/windows-msvc-x64' --config Debug --target " +
        'ForgeConductor.Infrastructure.ProcessTests ' +
        'ForgeConductor.LMStudio.RealHostTests --parallel ' + $Parallel +
        '; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ' +
        "ctest --test-dir 'out/build/windows-msvc-x64' -C Debug " +
        "--output-on-failure -R '^Infrastructure[.]ProcessTests$'; exit `$LASTEXITCODE"
    $expectedProcessCorrectiveTestCommand =
        "ctest --test-dir 'out/build/windows-msvc-x64' -C Debug " +
        "--output-on-failure -R '^ForgeConductor[.]Infrastructure[.]ProcessTests$'; " +
        'exit $LASTEXITCODE'
    $expectedFifthFailedCommand = $expectedFourthFailedCommand +
        " -FourthFailedCommandRecordPath '" + $FourthFailedCommandRecordPath + "'" +
        " -FourthFailedRealHostEvidencePath '" + $FourthFailedRealHostEvidencePath + "'" +
        " -ProcessCorrectiveBuildCommandRecordPath '" +
        $ProcessCorrectiveBuildCommandRecordPath + "'" +
        " -ProcessCorrectiveTestCommandRecordPath '" +
        $ProcessCorrectiveTestCommandRecordPath + "'"
    $expectedMcpCorrectiveFailedCommand =
        "cmake --build 'out/build/windows-msvc-x64' --config Debug --target " +
        'ForgeConductor.Infrastructure.UnitTests ' +
        'ForgeConductor.Persistence.UnitTests ' +
        'ForgeConductor.Mcp.ServeProcessSnapshotTests ' +
        'ForgeConductor.ProjectMemory.RegistryWindowsTests ' +
        'ForgeConductor.LMStudio.RealHostTests --parallel ' + $Parallel +
        '; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ' +
        "ctest --test-dir 'out/build/windows-msvc-x64' -C Debug " +
        "--output-on-failure -R '^(ForgeConductor[.]Infrastructure[.]UnitTests|" +
        'ForgeConductor[.]Persistence[.]UnitTests|' +
        'ForgeConductor[.]Mcp[.]ServeProcessSnapshotTests|' +
        "ForgeConductor[.]ProjectMemory[.]RegistryWindowsTests)$'; " +
        'exit $LASTEXITCODE'
    $expectedMcpCorrectiveCommand =
        "cmake --build 'out/build/windows-msvc-x64' --config Debug --target " +
        'ForgeConductor.Mcp.ServeProcessSnapshotTests --parallel ' + $Parallel +
        '; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ' +
        "ctest --test-dir 'out/build/windows-msvc-x64' -C Debug " +
        "--output-on-failure -R '^ForgeConductor[.]Mcp[.]ServeProcessSnapshotTests$'; " +
        'exit $LASTEXITCODE'
    Assert-True ([string]::Equals(
        $SecondFailedCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T143842697Z-c3dff953.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'second failed command uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $RunnerCorrectiveCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T144053220Z-295171a5.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'runner correction uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $ThirdFailedCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T144836198Z-72acd98d.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'third failed command uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $DeploymentCorrectiveCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T145523645Z-f71473e0.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'deployment correction uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $FourthFailedCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T150124952Z-5cedbb6e.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'fourth failed command uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $ProcessCorrectiveBuildCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T151026597Z-92e2bf74.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'process corrective build uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $ProcessCorrectiveTestCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T151128087Z-39498297.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'process corrective test uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $FifthFailedCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T152436935Z-5a68dfe2.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'fifth failed command uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $McpCorrectiveFailedCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T154643696Z-7eedb3e9.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'failed MCP correction uses its exact canonical record path'
    Assert-True ([string]::Equals(
        $McpCorrectiveCommandRecordPath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\commands\20260827T155313063Z-971b814b.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'successful MCP correction uses its exact canonical record path'
    $priorCommandEvidence = Read-ValidatedCommandRecord `
        -Path $PriorFailedCommandRecordPath `
        -ExpectedCommand $expectedPriorCommand `
        -ExpectedExitCode 1 `
        -Message 'prior failed full G15 command record'
    $correctiveCommandEvidence = Read-ValidatedCommandRecord `
        -Path $CorrectiveCommandRecordPath `
        -ExpectedCommand $expectedCorrectiveCommand `
        -ExpectedExitCode 0 `
        -Message 'corrective focused command record'
    $secondFailedCommandEvidence = Read-ValidatedCommandRecord `
        -Path $SecondFailedCommandRecordPath `
        -ExpectedCommand $expectedSecondFailedCommand `
        -ExpectedExitCode 1 `
        -Message 'second failed resumed G15 command record'
    $runnerCorrectiveCommandEvidence = Read-ValidatedCommandRecord `
        -Path $RunnerCorrectiveCommandRecordPath `
        -ExpectedCommand $expectedRunnerCorrectiveCommand `
        -ExpectedExitCode 0 `
        -Message 'runner-only corrective command record'
    $thirdFailedCommandEvidence = Read-ValidatedCommandRecord `
        -Path $ThirdFailedCommandRecordPath `
        -ExpectedCommand $expectedThirdFailedCommand `
        -ExpectedExitCode 1 `
        -Message 'third failed resumed G15 command record'
    $deploymentCorrectiveCommandEvidence = Read-ValidatedCommandRecord `
        -Path $DeploymentCorrectiveCommandRecordPath `
        -ExpectedCommand $expectedCorrectiveCommand `
        -ExpectedExitCode 0 `
        -Message 'deployment corrective command record'
    $fourthFailedCommandEvidence = Read-ValidatedCommandRecord `
        -Path $FourthFailedCommandRecordPath `
        -ExpectedCommand $expectedFourthFailedCommand `
        -ExpectedExitCode 1 `
        -Message 'fourth failed resumed G15 command record'
    $processCorrectiveBuildCommandEvidence = Read-ValidatedCommandRecord `
        -Path $ProcessCorrectiveBuildCommandRecordPath `
        -ExpectedCommand $expectedProcessCorrectiveBuildCommand `
        -ExpectedExitCode 0 `
        -Message 'process corrective build command record'
    $processCorrectiveTestCommandEvidence = Read-ValidatedCommandRecord `
        -Path $ProcessCorrectiveTestCommandRecordPath `
        -ExpectedCommand $expectedProcessCorrectiveTestCommand `
        -ExpectedExitCode 0 `
        -Message 'process corrective test command record'
    $fifthFailedCommandEvidence = Read-ValidatedCommandRecord `
        -Path $FifthFailedCommandRecordPath `
        -ExpectedCommand $expectedFifthFailedCommand `
        -ExpectedExitCode 1 `
        -Message 'fifth failed resumed G15 command record'
    $mcpCorrectiveFailedCommandEvidence = Read-ValidatedCommandRecord `
        -Path $McpCorrectiveFailedCommandRecordPath `
        -ExpectedCommand $expectedMcpCorrectiveFailedCommand `
        -ExpectedExitCode 8 `
        -Message 'failed MCP corrective command record'
    $mcpCorrectiveCommandEvidence = Read-ValidatedCommandRecord `
        -Path $McpCorrectiveCommandRecordPath `
        -ExpectedCommand $expectedMcpCorrectiveCommand `
        -ExpectedExitCode 0 `
        -Message 'successful MCP corrective command record'
    Assert-CommandLedgerEvent `
        -RecordPath $priorCommandEvidence.RecordPath `
        -ExpectedExitCode 1 `
        -Message 'prior failed full G15 command'
    Assert-CommandLedgerEvent `
        -RecordPath $correctiveCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'corrective focused command'
    Assert-CommandLedgerEvent `
        -RecordPath $secondFailedCommandEvidence.RecordPath `
        -ExpectedExitCode 1 `
        -Message 'second failed resumed G15 command'
    Assert-CommandLedgerEvent `
        -RecordPath $runnerCorrectiveCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'runner-only corrective command'
    Assert-CommandLedgerEvent `
        -RecordPath $thirdFailedCommandEvidence.RecordPath `
        -ExpectedExitCode 1 `
        -Message 'third failed resumed G15 command'
    Assert-CommandLedgerEvent `
        -RecordPath $deploymentCorrectiveCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'deployment corrective command'
    Assert-CommandLedgerEvent `
        -RecordPath $fourthFailedCommandEvidence.RecordPath `
        -ExpectedExitCode 1 `
        -Message 'fourth failed resumed G15 command'
    Assert-CommandLedgerEvent `
        -RecordPath $processCorrectiveBuildCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'process corrective build command'
    Assert-CommandLedgerEvent `
        -RecordPath $processCorrectiveTestCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'process corrective test command'
    Assert-CommandLedgerEvent `
        -RecordPath $fifthFailedCommandEvidence.RecordPath `
        -ExpectedExitCode 1 `
        -Message 'fifth failed resumed G15 command'
    Assert-CommandLedgerEvent `
        -RecordPath $mcpCorrectiveFailedCommandEvidence.RecordPath `
        -ExpectedExitCode 8 `
        -Message 'failed MCP corrective command'
    Assert-CommandLedgerEvent `
        -RecordPath $mcpCorrectiveCommandEvidence.RecordPath `
        -ExpectedExitCode 0 `
        -Message 'successful MCP corrective command'

    $priorFailedEvidenceCanonicalPath = Resolve-CanonicalStateFile `
        $PriorFailedRealHostEvidencePath `
        'preserved first-attempt real-host evidence' `
        $maximumRealHostEvidenceBytes
    Assert-True ([string]::Equals(
        (Split-Path -Parent $priorFailedEvidenceCanonicalPath),
        (Join-Path $WorkspaceRoot '.forge-codex\state\evidence\P15'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved first-attempt evidence is in the P15 evidence directory'
    Assert-Exact ([IO.Path]::GetFileName($priorFailedEvidenceCanonicalPath)) `
        'windows-lm-studio-real-host-attempt-1-failed.json' `
        'preserved first-attempt evidence canonical leaf'
    Assert-True ((Get-Item -LiteralPath `
        $priorFailedEvidenceCanonicalPath).Length -gt 0) `
        'preserved first-attempt real-host evidence is nonempty'
    $priorFailedEvidence = Get-Content -Raw -LiteralPath `
        $priorFailedEvidenceCanonicalPath | ConvertFrom-Json
    Assert-Set @($priorFailedEvidence.PSObject.Properties.Name) @(
        'schema_version',
        'bounded',
        'gate',
        'phase',
        'result',
        'runner',
        'sanitized') 'preserved failed real-host evidence schema'
    Assert-Set @($priorFailedEvidence.result.PSObject.Properties.Name) @(
        'deployment_left_installed',
        'error',
        'error_code',
        'retryable',
        'rollback_requested',
        'stage',
        'status') 'preserved failed real-host result schema'
    Assert-Exact ([int]$priorFailedEvidence.schema_version) 1 `
        'preserved failed evidence schema version'
    Assert-Exact ([string]$priorFailedEvidence.phase) 'P15' `
        'preserved failed evidence phase'
    Assert-Exact ([string]$priorFailedEvidence.gate) 'G15' `
        'preserved failed evidence gate'
    Assert-Exact ([string]$priorFailedEvidence.runner) `
        'ForgeConductor.LMStudio.RealHostTests' `
        'preserved failed evidence runner'
    Assert-True ([bool]$priorFailedEvidence.bounded) `
        'preserved failed evidence remained bounded'
    Assert-True ([bool]$priorFailedEvidence.sanitized) `
        'preserved failed evidence remained sanitized'
    Assert-Exact ([string]$priorFailedEvidence.result.status) 'failed' `
        'preserved first-attempt result status'
    Assert-Exact ([string]$priorFailedEvidence.result.stage) `
        'inspect_lmstudio_environment' 'preserved first-attempt failure stage'
    Assert-Exact ([string]$priorFailedEvidence.result.error_code) `
        'limit_exceeded' 'preserved first-attempt failure code'
    Assert-Exact ([bool]$priorFailedEvidence.result.retryable) $false `
        'preserved first-attempt failure is not retryable without correction'
    Assert-Exact ([bool]$priorFailedEvidence.result.deployment_left_installed) `
        $false 'first attempt performed no deployment mutation'
    Assert-Exact ([bool]$priorFailedEvidence.result.rollback_requested) $false `
        'first attempt required no rollback'
    Assert-True (-not [string]::IsNullOrWhiteSpace(
        [string]$priorFailedEvidence.result.error) -and
        ([string]$priorFailedEvidence.result.error).Length -le 1024) `
        'preserved first-attempt error is bounded and nonempty'
    $priorFailedEvidenceSha256 = Get-FileSha256 `
        $priorFailedEvidenceCanonicalPath

    Assert-True ([string]::Equals(
        $SecondFailedRealHostEvidencePath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\evidence\P15\windows-lm-studio-real-host-attempt-2-failed.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'second failed evidence uses its exact canonical path'
    Assert-True (-not [string]::Equals(
        $SecondFailedRealHostEvidencePath,
        $RealHostEvidencePath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved second failed evidence is distinct from the next evidence target'
    $secondFailedEvidenceCanonicalPath = Resolve-CanonicalStateFile `
        $SecondFailedRealHostEvidencePath `
        'preserved second-attempt real-host evidence' `
        $maximumRealHostEvidenceBytes
    Assert-True ([string]::Equals(
        (Split-Path -Parent $secondFailedEvidenceCanonicalPath),
        (Join-Path $WorkspaceRoot '.forge-codex\state\evidence\P15'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved second-attempt evidence is in the P15 evidence directory'
    Assert-Exact ([IO.Path]::GetFileName($secondFailedEvidenceCanonicalPath)) `
        'windows-lm-studio-real-host-attempt-2-failed.json' `
        'preserved second-attempt evidence canonical leaf'
    $secondFailedEvidence = Get-Content -Raw -LiteralPath `
        $secondFailedEvidenceCanonicalPath | ConvertFrom-Json
    Assert-Set @($secondFailedEvidence.PSObject.Properties.Name) @(
        'schema_version', 'bounded', 'gate', 'phase', 'result', 'runner', 'sanitized') `
        'preserved second failed real-host evidence schema'
    Assert-Set @($secondFailedEvidence.result.PSObject.Properties.Name) @(
        'deployment_left_installed', 'error', 'error_code', 'retryable',
        'rollback_requested', 'stage', 'status') `
        'preserved second failed real-host result schema'
    Assert-Exact ([int]$secondFailedEvidence.schema_version) 1 `
        'preserved second failed evidence schema version'
    Assert-Exact ([string]$secondFailedEvidence.phase) 'P15' `
        'preserved second failed evidence phase'
    Assert-Exact ([string]$secondFailedEvidence.gate) 'G15' `
        'preserved second failed evidence gate'
    Assert-Exact ([string]$secondFailedEvidence.runner) `
        'ForgeConductor.LMStudio.RealHostTests' `
        'preserved second failed evidence runner'
    Assert-True ([bool]$secondFailedEvidence.bounded) `
        'preserved second failed evidence remained bounded'
    Assert-True ([bool]$secondFailedEvidence.sanitized) `
        'preserved second failed evidence remained sanitized'
    Assert-Exact ([string]$secondFailedEvidence.result.status) 'failed' `
        'preserved second-attempt result status'
    Assert-Exact ([string]$secondFailedEvidence.result.stage) 'snapshot_file' `
        'preserved second-attempt failure stage'
    Assert-Exact ([string]$secondFailedEvidence.result.error_code) 'limit_exceeded' `
        'preserved second-attempt failure code'
    Assert-Exact ([bool]$secondFailedEvidence.result.retryable) $false `
        'preserved second-attempt failure retryability'
    Assert-Exact ([bool]$secondFailedEvidence.result.deployment_left_installed) $false `
        'second attempt performed no deployment mutation'
    Assert-Exact ([bool]$secondFailedEvidence.result.rollback_requested) $false `
        'second attempt required no rollback'
    Assert-True (-not [string]::IsNullOrWhiteSpace(
        [string]$secondFailedEvidence.result.error) -and
        ([string]$secondFailedEvidence.result.error).Length -le 1024) `
        'preserved second-attempt error is bounded and nonempty'
    $secondFailedEvidenceSha256 = Get-FileSha256 `
        $secondFailedEvidenceCanonicalPath

    Assert-True ([string]::Equals(
        $ThirdFailedRealHostEvidencePath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\evidence\P15\windows-lm-studio-real-host-attempt-3-failed.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'third failed evidence uses its exact canonical path'
    Assert-True (-not [string]::Equals(
        $ThirdFailedRealHostEvidencePath,
        $RealHostEvidencePath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved third failed evidence is distinct from the next evidence target'
    $thirdFailedEvidenceCanonicalPath = Resolve-CanonicalStateFile `
        $ThirdFailedRealHostEvidencePath `
        'preserved third-attempt real-host evidence' `
        $maximumRealHostEvidenceBytes
    Assert-Exact ([IO.Path]::GetFileName($thirdFailedEvidenceCanonicalPath)) `
        'windows-lm-studio-real-host-attempt-3-failed.json' `
        'preserved third-attempt evidence canonical leaf'
    $thirdFailedEvidence = Get-Content -Raw -LiteralPath `
        $thirdFailedEvidenceCanonicalPath | ConvertFrom-Json
    Assert-Set @($thirdFailedEvidence.PSObject.Properties.Name) @(
        'authority', 'before', 'binary', 'bounded', 'gate', 'host',
        'host_observations', 'phase', 'result', 'runner', 'sanitized',
        'schema_version') 'preserved third failed real-host evidence schema'
    Assert-Set @($thirdFailedEvidence.result.PSObject.Properties.Name) @(
        'deployment_left_installed', 'error', 'error_code', 'retryable',
        'rollback_requested', 'stage', 'status') `
        'preserved third failed real-host result schema'
    Assert-Exact ([int]$thirdFailedEvidence.schema_version) 1 `
        'preserved third failed evidence schema version'
    Assert-Exact ([string]$thirdFailedEvidence.phase) 'P15' `
        'preserved third failed evidence phase'
    Assert-Exact ([string]$thirdFailedEvidence.gate) 'G15' `
        'preserved third failed evidence gate'
    Assert-Exact ([string]$thirdFailedEvidence.runner) `
        'ForgeConductor.LMStudio.RealHostTests' `
        'preserved third failed evidence runner'
    Assert-True ([bool]$thirdFailedEvidence.bounded) `
        'preserved third failed evidence remained bounded'
    Assert-True ([bool]$thirdFailedEvidence.sanitized) `
        'preserved third failed evidence remained sanitized'
    Assert-Exact ([string]$thirdFailedEvidence.result.status) 'failed' `
        'preserved third-attempt result status'
    Assert-Exact ([string]$thirdFailedEvidence.result.stage) `
        'deploy_lmstudio_plugins' 'preserved third-attempt failure stage'
    Assert-Exact ([string]$thirdFailedEvidence.result.error_code) `
        'path_outside_authority' 'preserved third-attempt failure code'
    Assert-Exact ([bool]$thirdFailedEvidence.result.retryable) $false `
        'preserved third-attempt failure retryability'
    Assert-Exact ([bool]$thirdFailedEvidence.result.deployment_left_installed) `
        $false 'third attempt performed no deployment mutation'
    Assert-Exact ([bool]$thirdFailedEvidence.result.rollback_requested) $false `
        'third attempt required no rollback'
    $thirdFailedEvidenceSha256 = Get-FileSha256 `
        $thirdFailedEvidenceCanonicalPath

    Assert-True ([string]::Equals(
        $FourthFailedRealHostEvidencePath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\evidence\P15\windows-lm-studio-real-host-attempt-4-failed.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'fourth failed evidence uses its exact canonical path'
    Assert-True (-not [string]::Equals(
        $FourthFailedRealHostEvidencePath,
        $RealHostEvidencePath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved fourth failed evidence is distinct from the next evidence target'
    $fourthFailedEvidenceCanonicalPath = Resolve-CanonicalStateFile `
        $FourthFailedRealHostEvidencePath `
        'preserved fourth-attempt real-host evidence' `
        $maximumRealHostEvidenceBytes
    Assert-Exact ([IO.Path]::GetFileName($fourthFailedEvidenceCanonicalPath)) `
        'windows-lm-studio-real-host-attempt-4-failed.json' `
        'preserved fourth-attempt evidence canonical leaf'
    $fourthFailedEvidence = Get-Content -Raw -LiteralPath `
        $fourthFailedEvidenceCanonicalPath | ConvertFrom-Json
    Assert-Set @($fourthFailedEvidence.PSObject.Properties.Name) @(
        'authority', 'before', 'binary', 'bounded', 'gate', 'host',
        'host_observations', 'phase', 'result', 'runner', 'sanitized',
        'schema_version') 'preserved fourth failed real-host evidence schema'
    Assert-Set @($fourthFailedEvidence.result.PSObject.Properties.Name) @(
        'deployment_left_installed', 'error', 'error_code', 'retryable',
        'rollback_requested', 'stage', 'status') `
        'preserved fourth failed real-host result schema'
    Assert-Exact ([int]$fourthFailedEvidence.schema_version) 1 `
        'preserved fourth failed evidence schema version'
    Assert-Exact ([string]$fourthFailedEvidence.phase) 'P15' `
        'preserved fourth failed evidence phase'
    Assert-Exact ([string]$fourthFailedEvidence.gate) 'G15' `
        'preserved fourth failed evidence gate'
    Assert-Exact ([string]$fourthFailedEvidence.runner) `
        'ForgeConductor.LMStudio.RealHostTests' `
        'preserved fourth failed evidence runner'
    Assert-True ([bool]$fourthFailedEvidence.bounded) `
        'preserved fourth failed evidence remained bounded'
    Assert-True ([bool]$fourthFailedEvidence.sanitized) `
        'preserved fourth failed evidence remained sanitized'
    Assert-Exact ([string]$fourthFailedEvidence.result.status) 'failed' `
        'preserved fourth-attempt result status'
    Assert-Exact ([string]$fourthFailedEvidence.result.stage) `
        'deploy_lmstudio_plugins' 'preserved fourth-attempt failure stage'
    Assert-Exact ([string]$fourthFailedEvidence.result.error_code) `
        'process_launch_failed' 'preserved fourth-attempt failure code'
    Assert-Exact ([bool]$fourthFailedEvidence.result.retryable) $false `
        'preserved fourth-attempt failure retryability'
    Assert-Exact ([bool]$fourthFailedEvidence.result.deployment_left_installed) `
        $false 'fourth attempt performed no deployment mutation'
    Assert-Exact ([bool]$fourthFailedEvidence.result.rollback_requested) $false `
        'fourth attempt required no rollback'
    $fourthFailedEvidenceSha256 = Get-FileSha256 `
        $fourthFailedEvidenceCanonicalPath

    Assert-True ([string]::Equals(
        $FifthFailedRealHostEvidencePath,
        (Join-Path $WorkspaceRoot `
            '.forge-codex\state\evidence\P15\windows-lm-studio-real-host-attempt-5-failed.json'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'fifth failed evidence uses its exact canonical path'
    Assert-True (-not [string]::Equals(
        $FifthFailedRealHostEvidencePath,
        $RealHostEvidencePath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'preserved fifth failed evidence is distinct from the next evidence target'
    $fifthFailedEvidenceCanonicalPath = Resolve-CanonicalStateFile `
        $FifthFailedRealHostEvidencePath `
        'preserved fifth-attempt real-host evidence' `
        $maximumRealHostEvidenceBytes
    Assert-Exact ([IO.Path]::GetFileName($fifthFailedEvidenceCanonicalPath)) `
        'windows-lm-studio-real-host-attempt-5-failed.json' `
        'preserved fifth-attempt evidence canonical leaf'
    $fifthFailedEvidence = Get-Content -Raw -LiteralPath `
        $fifthFailedEvidenceCanonicalPath | ConvertFrom-Json
    Assert-Set @($fifthFailedEvidence.PSObject.Properties.Name) @(
        'authority', 'before', 'binary', 'bounded', 'gate', 'host',
        'host_observations', 'phase', 'result', 'runner', 'sanitized',
        'schema_version') 'preserved fifth failed real-host evidence schema'
    Assert-Set @($fifthFailedEvidence.result.PSObject.Properties.Name) @(
        'deployment_left_installed', 'error', 'error_code', 'retryable',
        'rollback_requested', 'stage', 'status') `
        'preserved fifth failed real-host result schema'
    Assert-Exact ([int]$fifthFailedEvidence.schema_version) 1 `
        'preserved fifth failed evidence schema version'
    Assert-Exact ([string]$fifthFailedEvidence.phase) 'P15' `
        'preserved fifth failed evidence phase'
    Assert-Exact ([string]$fifthFailedEvidence.gate) 'G15' `
        'preserved fifth failed evidence gate'
    Assert-Exact ([string]$fifthFailedEvidence.runner) `
        'ForgeConductor.LMStudio.RealHostTests' `
        'preserved fifth failed evidence runner'
    Assert-True ([bool]$fifthFailedEvidence.bounded) `
        'preserved fifth failed evidence remained bounded'
    Assert-True ([bool]$fifthFailedEvidence.sanitized) `
        'preserved fifth failed evidence remained sanitized'
    Assert-Exact ([string]$fifthFailedEvidence.result.status) 'failed' `
        'preserved fifth-attempt result status'
    Assert-Exact ([string]$fifthFailedEvidence.result.stage) `
        'deploy_lmstudio_plugins' 'preserved fifth-attempt failure stage'
    Assert-Exact ([string]$fifthFailedEvidence.result.error_code) `
        'process_exit_nonzero' 'preserved fifth-attempt failure code'
    Assert-Exact ([string]$fifthFailedEvidence.result.error) `
        'The MCP serve smoke process returned exit code 1.' `
        'preserved fifth-attempt bounded error'
    Assert-Exact ([bool]$fifthFailedEvidence.result.retryable) $false `
        'preserved fifth-attempt failure retryability'
    Assert-Exact ([bool]$fifthFailedEvidence.result.deployment_left_installed) `
        $false 'fifth attempt performed no installed deployment mutation'
    Assert-Exact ([bool]$fifthFailedEvidence.result.rollback_requested) $false `
        'fifth attempt required no rollback'
    $fifthFailedEvidenceSha256 = Get-FileSha256 `
        $fifthFailedEvidenceCanonicalPath

    $buildMarker = 'G15: running the single authoritative affected-target Debug rebuild.'
    $testMarker = 'G15: running the deterministic G15 CTest suite once.'
    $runnerMarker = 'G15: launching LM Studio through its supported activation path; no GUI automation is used.'
    Assert-LiteralOccurrenceCount $priorCommandEvidence.StdoutText `
        $buildMarker 1 'prior full G15 build marker'
    Assert-LiteralOccurrenceCount $priorCommandEvidence.StdoutText `
        $testMarker 1 'prior G15 CTest marker'
    Assert-LiteralOccurrenceCount $priorCommandEvidence.StdoutText `
        $runnerMarker 1 'prior real-host runner marker'
    $buildMarkerIndex = $priorCommandEvidence.StdoutText.IndexOf(
        $buildMarker, [StringComparison]::Ordinal)
    $testMarkerIndex = $priorCommandEvidence.StdoutText.IndexOf(
        $testMarker, [StringComparison]::Ordinal)
    $runnerMarkerIndex = $priorCommandEvidence.StdoutText.IndexOf(
        $runnerMarker, [StringComparison]::Ordinal)
    Assert-True ($buildMarkerIndex -ge 0 -and
        $testMarkerIndex -gt $buildMarkerIndex -and
        $runnerMarkerIndex -gt $testMarkerIndex) `
        'prior command ordered build, CTest, then real-host attempt'
    $priorBuildText = $priorCommandEvidence.StdoutText.Substring(
        $buildMarkerIndex, $testMarkerIndex - $buildMarkerIndex)
    foreach ($target in $buildTargets) {
        Assert-LiteralOccurrenceCount $priorBuildText `
            ($target + '.vcxproj ->') 1 `
            "prior full build completed target $target"
    }
    $priorTestText = $priorCommandEvidence.StdoutText.Substring(
        $testMarkerIndex, $runnerMarkerIndex - $testMarkerIndex)
    $passedCTestLines = [regex]::Matches(
        $priorTestText,
        '(?m)^[1-5]/5 Test #[0-9]+: .+ Passed\s+[0-9]+[.][0-9]+ sec\s*$')
    Assert-Exact $passedCTestLines.Count 5 `
        'prior full command exact passed CTest result count'
    Assert-LiteralOccurrenceCount $priorTestText `
        '100% tests passed out of 5' 1 'prior CTest complete success summary'
    foreach ($testName in $expectedTests) {
        Assert-Match $priorTestText `
            ('(?m)^[1-5]/5 Test #[0-9]+: ' + [regex]::Escape($testName) +
                '\s+[.]+\s+Passed\s+[0-9]+[.][0-9]+ sec\s*$') `
            "prior CTest passed $testName" -CaseSensitive
    }
    Assert-NoMatch $priorTestText '[1-9][0-9]*% tests failed' `
        'prior CTest section reports no failures' -CaseSensitive

    Assert-Exact $correctiveCommandEvidence.StderrBytes 0L `
        'corrective focused command stderr bytes'
    Assert-Exact $correctiveCommandEvidence.StderrSha256 $emptySha256 `
        'corrective focused command empty stderr SHA-256'
    Assert-LiteralOccurrenceCount $correctiveCommandEvidence.StdoutText `
        'ForgeConductor.Infrastructure.UnitTests.vcxproj ->' 1 `
        'corrective focused unit-test build completion'
    Assert-LiteralOccurrenceCount $correctiveCommandEvidence.StdoutText `
        'ForgeConductor.LMStudio.RealHostTests.vcxproj ->' 1 `
        'corrective focused real-host build completion'
    Assert-Exact ([regex]::Matches(
        $correctiveCommandEvidence.StdoutText,
        '(?m)^\[RUN\] .+$').Count) 88 `
        'corrective focused unit-test run count'
    Assert-Exact ([regex]::Matches(
        $correctiveCommandEvidence.StdoutText,
        '(?m)^\[PASS\] .+$').Count) 88 `
        'corrective focused unit-test pass count'
    Assert-LiteralOccurrenceCount $correctiveCommandEvidence.StdoutText `
        '88/88 Windows infrastructure unit tests passed.' 1 `
        'corrective focused unit-test success summary'
    Assert-NoMatch $correctiveCommandEvidence.StdoutText `
        '(?m)^\[FAIL\]' 'corrective focused command contains no failed unit test' `
        -CaseSensitive

    Assert-LiteralOccurrenceCount $secondFailedCommandEvidence.StdoutText `
        'G15: validating exact prior and corrective evidence for fail-closed resume.' 1 `
        'second attempt prior/corrective evidence validation marker'
    Assert-LiteralOccurrenceCount $secondFailedCommandEvidence.StdoutText `
        'G15: prior full build/CTest and corrective focused pass validated; execution is not repeated.' 1 `
        'second attempt no-repeat confirmation marker'
    Assert-LiteralOccurrenceCount $secondFailedCommandEvidence.StdoutText `
        $buildMarker 0 'second attempt full-build marker exclusion'
    Assert-LiteralOccurrenceCount $secondFailedCommandEvidence.StdoutText `
        $testMarker 0 'second attempt full-CTest marker exclusion'
    Assert-LiteralOccurrenceCount $secondFailedCommandEvidence.StdoutText `
        $runnerMarker 1 'second real-host runner marker'

    Assert-Exact $runnerCorrectiveCommandEvidence.StderrBytes 0L `
        'runner-only correction stderr bytes'
    Assert-Exact $runnerCorrectiveCommandEvidence.StderrSha256 $emptySha256 `
        'runner-only correction empty stderr SHA-256'
    Assert-LiteralOccurrenceCount $runnerCorrectiveCommandEvidence.StdoutText `
        'ForgeConductor.LMStudio.RealHostTests.vcxproj ->' 1 `
        'runner-only correction target completion'

    Assert-LiteralOccurrenceCount $thirdFailedCommandEvidence.StdoutText `
        'G15: validating exact prior and corrective evidence for fail-closed resume.' 1 `
        'third attempt resume validation marker'
    Assert-LiteralOccurrenceCount $thirdFailedCommandEvidence.StdoutText `
        'G15: prior full build/CTest and corrective focused pass validated; execution is not repeated.' 1 `
        'third attempt no-repeat confirmation marker'
    Assert-LiteralOccurrenceCount $thirdFailedCommandEvidence.StdoutText `
        $buildMarker 0 'third attempt full-build marker exclusion'
    Assert-LiteralOccurrenceCount $thirdFailedCommandEvidence.StdoutText `
        $testMarker 0 'third attempt full-CTest marker exclusion'
    Assert-LiteralOccurrenceCount $thirdFailedCommandEvidence.StdoutText `
        $runnerMarker 1 'third real-host runner marker'

    Assert-Exact $deploymentCorrectiveCommandEvidence.StderrBytes 0L `
        'deployment correction stderr bytes'
    Assert-Exact $deploymentCorrectiveCommandEvidence.StderrSha256 $emptySha256 `
        'deployment correction empty stderr SHA-256'
    Assert-LiteralOccurrenceCount $deploymentCorrectiveCommandEvidence.StdoutText `
        'ForgeConductor.Infrastructure.UnitTests.vcxproj ->' 1 `
        'deployment correction unit-test target completion'
    Assert-LiteralOccurrenceCount $deploymentCorrectiveCommandEvidence.StdoutText `
        'ForgeConductor.LMStudio.RealHostTests.vcxproj ->' 1 `
        'deployment correction real-host target completion'
    Assert-Exact ([regex]::Matches(
        $deploymentCorrectiveCommandEvidence.StdoutText,
        '(?m)^\[RUN\] .+$').Count) 88 `
        'deployment correction unit-test run count'
    Assert-Exact ([regex]::Matches(
        $deploymentCorrectiveCommandEvidence.StdoutText,
        '(?m)^\[PASS\] .+$').Count) 88 `
        'deployment correction unit-test pass count'
    Assert-LiteralOccurrenceCount $deploymentCorrectiveCommandEvidence.StdoutText `
        '88/88 Windows infrastructure unit tests passed.' 1 `
        'deployment correction unit-test success summary'

    Assert-LiteralOccurrenceCount $fourthFailedCommandEvidence.StdoutText `
        'G15: validating exact prior and corrective evidence for fail-closed resume.' 1 `
        'fourth attempt resume validation marker'
    Assert-LiteralOccurrenceCount $fourthFailedCommandEvidence.StdoutText `
        'G15: prior full build/CTest and corrective focused pass validated; execution is not repeated.' 1 `
        'fourth attempt no-repeat confirmation marker'
    Assert-LiteralOccurrenceCount $fourthFailedCommandEvidence.StdoutText `
        $buildMarker 0 'fourth attempt full-build marker exclusion'
    Assert-LiteralOccurrenceCount $fourthFailedCommandEvidence.StdoutText `
        $testMarker 0 'fourth attempt full-CTest marker exclusion'
    Assert-LiteralOccurrenceCount $fourthFailedCommandEvidence.StdoutText `
        $runnerMarker 1 'fourth real-host runner marker'

    Assert-LiteralOccurrenceCount `
        $processCorrectiveBuildCommandEvidence.StdoutText `
        'ForgeConductor.Infrastructure.ProcessTests.vcxproj ->' 1 `
        'process correction process-test target completion'
    Assert-LiteralOccurrenceCount `
        $processCorrectiveBuildCommandEvidence.StdoutText `
        'ForgeConductor.LMStudio.RealHostTests.vcxproj ->' 1 `
        'process correction real-host target completion'
    Assert-LiteralOccurrenceCount `
        $processCorrectiveBuildCommandEvidence.StdoutText `
        'Test project D:/GitHub/Forge-Conductor-Windows-Edition/out/build/windows-msvc-x64' `
        1 'process correction zero-test CTest project marker'
    Assert-Exact ((Get-Content -Raw -LiteralPath `
        $processCorrectiveBuildCommandEvidence.StderrPath).Trim()) `
        'No tests were found!!!' `
        'process correction zero-test filter stderr'
    Assert-Exact ([regex]::Matches(
        $processCorrectiveBuildCommandEvidence.StdoutText,
        '(?m)^\s*Start [0-9]+:').Count) 0 `
        'process correction zero-test filter started no tests'
    Assert-Exact ([regex]::Matches(
        $processCorrectiveBuildCommandEvidence.StdoutText,
        '(?m)^[0-9]+/[0-9]+ Test #').Count) 0 `
        'process correction zero-test filter executed no tests'
    Assert-NoMatch $processCorrectiveBuildCommandEvidence.StdoutText `
        '[0-9]+% tests passed out of' `
        'process correction zero-test filter is not pass evidence' `
        -CaseSensitive

    Assert-Exact $processCorrectiveTestCommandEvidence.StderrBytes 0L `
        'process corrective test stderr bytes'
    Assert-Exact $processCorrectiveTestCommandEvidence.StderrSha256 `
        $emptySha256 'process corrective test empty stderr SHA-256'
    Assert-LiteralOccurrenceCount `
        $processCorrectiveTestCommandEvidence.StdoutText `
        'Test project D:/GitHub/Forge-Conductor-Windows-Edition/out/build/windows-msvc-x64' `
        1 'process corrective test CTest project marker'
    Assert-LiteralOccurrenceCount `
        $processCorrectiveTestCommandEvidence.StdoutText `
        'Start 21: ForgeConductor.Infrastructure.ProcessTests' 1 `
        'process corrective test exact start'
    Assert-Exact ([regex]::Matches(
        $processCorrectiveTestCommandEvidence.StdoutText,
        '(?m)^1/1 Test #[0-9]+: ForgeConductor[.]Infrastructure[.]ProcessTests\s+[.]+\s+Passed\s+[0-9]+[.][0-9]+ sec\s*$').Count) 1 `
        'process corrective test exact passed result'
    Assert-LiteralOccurrenceCount `
        $processCorrectiveTestCommandEvidence.StdoutText `
        '100% tests passed out of 1' 1 `
        'process corrective test complete success summary'

    Assert-LiteralOccurrenceCount $fifthFailedCommandEvidence.StdoutText `
        'G15: validating exact prior and corrective evidence for fail-closed resume.' 1 `
        'fifth attempt resume validation marker'
    Assert-LiteralOccurrenceCount $fifthFailedCommandEvidence.StdoutText `
        'G15: prior full build/CTest and corrective focused pass validated; execution is not repeated.' 1 `
        'fifth attempt no-repeat confirmation marker'
    Assert-LiteralOccurrenceCount $fifthFailedCommandEvidence.StdoutText `
        $buildMarker 0 'fifth attempt full-build marker exclusion'
    Assert-LiteralOccurrenceCount $fifthFailedCommandEvidence.StdoutText `
        $testMarker 0 'fifth attempt full-CTest marker exclusion'
    Assert-LiteralOccurrenceCount $fifthFailedCommandEvidence.StdoutText `
        $runnerMarker 1 'fifth real-host runner marker'

    foreach ($target in @(
        'ForgeConductor.Infrastructure.UnitTests',
        'ForgeConductor.Persistence.UnitTests',
        'ForgeConductor.Mcp.ServeProcessSnapshotTests',
        'ForgeConductor.ProjectMemory.RegistryWindowsTests',
        'ForgeConductor.LMStudio.RealHostTests')) {
        Assert-LiteralOccurrenceCount `
            $mcpCorrectiveFailedCommandEvidence.StdoutText `
            ($target + '.vcxproj ->') 1 `
            "failed MCP correction completed requested target $target"
    }
    Assert-LiteralOccurrenceCount `
        $mcpCorrectiveFailedCommandEvidence.StdoutText `
        'Test project D:/GitHub/Forge-Conductor-Windows-Edition/out/build/windows-msvc-x64' `
        1 'failed MCP correction CTest project marker'
    Assert-Exact ([regex]::Matches(
        $mcpCorrectiveFailedCommandEvidence.StdoutText,
        '(?m)^\s*Start\s+[0-9]+:').Count) 4 `
        'failed MCP correction exact started-test count'
    Assert-Exact ([regex]::Matches(
        $mcpCorrectiveFailedCommandEvidence.StdoutText,
        '(?m)^[1-4]/4 Test\s+#[0-9]+:').Count) 4 `
        'failed MCP correction exact result count'
    Assert-Exact ([regex]::Matches(
        $mcpCorrectiveFailedCommandEvidence.StdoutText,
        '(?m)^[1-4]/4 Test\s+#[0-9]+: .+\s+Passed\s+[0-9]+[.][0-9]+ sec\s*$').Count) 3 `
        'failed MCP correction passed-test count'
    Assert-Exact ([regex]::Matches(
        $mcpCorrectiveFailedCommandEvidence.StdoutText,
        '(?m)^1/4 Test\s+#4: ForgeConductor[.]Mcp[.]ServeProcessSnapshotTests\s+[.]+[*][*][*]Failed\s+[0-9]+[.][0-9]+ sec\s*$').Count) 1 `
        'failed MCP correction exact failed test'
    Assert-LiteralOccurrenceCount `
        $mcpCorrectiveFailedCommandEvidence.StdoutText `
        'invalid_request: A Windows path could not be converted to UTF-8.' 1 `
        'failed MCP correction exact boundary diagnostic'
    Assert-LiteralOccurrenceCount `
        $mcpCorrectiveFailedCommandEvidence.StdoutText `
        '75% tests passed, 1 tests failed out of 4' 1 `
        'failed MCP correction failure summary'
    Assert-LiteralOccurrenceCount `
        $mcpCorrectiveFailedCommandEvidence.StdoutText `
        '100% tests passed' 0 `
        'failed MCP correction is not pass evidence'
    Assert-Exact ((Get-Content -Raw -LiteralPath `
        $mcpCorrectiveFailedCommandEvidence.StderrPath).Trim()) `
        'Errors while running CTest' `
        'failed MCP correction stderr'

    Assert-Exact $mcpCorrectiveCommandEvidence.StderrBytes 0L `
        'successful MCP correction stderr bytes'
    Assert-Exact $mcpCorrectiveCommandEvidence.StderrSha256 $emptySha256 `
        'successful MCP correction empty stderr SHA-256'
    Assert-LiteralOccurrenceCount $mcpCorrectiveCommandEvidence.StdoutText `
        'ForgeConductor.Cli.vcxproj ->' 1 `
        'successful MCP correction CLI target completion'
    Assert-LiteralOccurrenceCount $mcpCorrectiveCommandEvidence.StdoutText `
        'ForgeConductor.Mcp.ServeProcessSnapshotTests.vcxproj ->' 1 `
        'successful MCP correction test target completion'
    Assert-LiteralOccurrenceCount `
        $mcpCorrectiveCommandEvidence.StdoutText `
        'Test project D:/GitHub/Forge-Conductor-Windows-Edition/out/build/windows-msvc-x64' `
        1 'successful MCP correction CTest project marker'
    Assert-LiteralOccurrenceCount $mcpCorrectiveCommandEvidence.StdoutText `
        'Start 4: ForgeConductor.Mcp.ServeProcessSnapshotTests' 1 `
        'successful MCP correction exact start'
    Assert-Exact ([regex]::Matches(
        $mcpCorrectiveCommandEvidence.StdoutText,
        '(?m)^1/1 Test #4: ForgeConductor[.]Mcp[.]ServeProcessSnapshotTests\s+[.]+\s+Passed\s+[0-9]+[.][0-9]+ sec\s*$').Count) 1 `
        'successful MCP correction exact passed result'
    Assert-LiteralOccurrenceCount $mcpCorrectiveCommandEvidence.StdoutText `
        '100% tests passed out of 1' 1 `
        'successful MCP correction complete success summary'
    Assert-NoMatch $mcpCorrectiveCommandEvidence.StdoutText `
        '[*][*][*]Failed' 'successful MCP correction contains no failed test' `
        -CaseSensitive
} else {
    $buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
    Write-Host 'G15: running the single authoritative affected-target Debug rebuild.'
    & $buildScript -Configuration Debug -Architecture x64 `
        -Target $buildTargets -Parallel $Parallel
    Assert-True $? 'single authoritative G15 affected-target build'
}

$ctest = Resolve-CtestExecutable
Push-Location $WorkspaceRoot
try {
    $inventoryText = @(& $ctest --preset windows-msvc-x64-debug `
        --show-only=json-v1 --no-tests=error --label-regex G15 2>&1) -join "`n"
    $inventoryExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
Assert-Exact $inventoryExitCode 0 'G15 CTest inventory query exit code'
$inventory = $inventoryText | ConvertFrom-Json
Assert-Set @($inventory.tests | ForEach-Object name) $expectedTests `
    'G15 deterministic CTest inventory'

if ($Resume) {
    Write-Host 'G15: prior full build/CTest and corrective focused pass validated; execution is not repeated.'
} else {
    $testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
    Write-Host 'G15: running the deterministic G15 CTest suite once.'
    & $testScript -Configuration Debug -Architecture x64 `
        -Parallel $Parallel -Label G15
    Assert-True $? 'single deterministic G15 CTest execution'
}
Assert-TrackedTreeClean
if ($Resume) {
    Assert-NoUntrackedBuildInputs
}

if ($StaticOnly) {
    Write-Host "G15 resume evidence validation passed without build, test, artifact, or live-host execution ($script:AssertionCount assertions)."
    return
}

$binRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64\bin\Debug'
$artifactNames = @(
    'forge-conductor.exe',
    'ForgeConductor.Contracts.ContractTests.exe',
    'ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'ForgeConductor.Infrastructure.HeaderSelfContainment.exe',
    'ForgeConductor.Infrastructure.ProcessTests.exe',
    'ForgeConductor.Infrastructure.UnitTests.exe',
    'ForgeConductor.LMStudio.RealHostTests.exe',
    'ForgeConductor.ProcessFixture.exe')
$artifacts = [Collections.Generic.List[object]]::new()
foreach ($name in $artifactNames) {
    $path = Join-Path $binRoot $name
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G15 artifact exists: $name"
    Assert-X64PortableExecutable $path "G15 artifact $name"
    $item = Get-Item -LiteralPath $path
    $artifacts.Add([ordered]@{
        path = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $path
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $path
    })
}

$forgeBinary = Join-Path $binRoot 'forge-conductor.exe'
$realHostBinary = Join-Path $binRoot 'ForgeConductor.LMStudio.RealHostTests.exe'
Write-Host 'G15: launching LM Studio through its supported activation path; no GUI automation is used.'
$runnerStart = [Diagnostics.ProcessStartInfo]::new()
$runnerStart.FileName = $realHostBinary
$runnerStart.UseShellExecute = $false
$runnerStart.CreateNoWindow = $true
$runnerStart.Arguments = '"' + $forgeBinary + '" "' + `
    $RealHostEvidencePath + '" "' + $LMStudioConfigurationPath + '" "' + `
    $hostExecutable + '"'
$runnerProcess = [Diagnostics.Process]::new()
$runnerProcess.StartInfo = $runnerStart
try {
    Assert-True ($runnerProcess.Start()) 'single real-host runner process started'
    $runnerExited = $runnerProcess.WaitForExit($realHostTimeoutMilliseconds)
    if (-not $runnerExited) {
        try { $runnerProcess.Kill() } catch { Stop-Process -Id $runnerProcess.Id -Force }
        $runnerProcess.WaitForExit()
    }
    Assert-True $runnerExited `
        'single real-host runner remained within its 540-second outer deadline'
    Assert-Exact $runnerProcess.ExitCode 0 `
        'single real-host LM Studio qualification exit code'
}
finally {
    $runnerProcess.Dispose()
}
Assert-True (Test-Path -LiteralPath $RealHostEvidencePath -PathType Leaf) `
    'real-host evidence exists'
$realHost = Get-Content -Raw -LiteralPath $RealHostEvidencePath | ConvertFrom-Json
Assert-Exact ([string]$realHost.phase) 'P15' 'real-host evidence phase'
Assert-Exact ([string]$realHost.gate) 'G15' 'real-host evidence gate'
Assert-Exact ([string]$realHost.result.status) 'passed' `
    'real-host qualification status'
Assert-True ([bool]$realHost.host.expected_application_bound) `
    'runtime discovery remained bound to the captured LM Studio executable'
Assert-True ([bool]$realHost.authority.selected_roots_only) `
    'real-host mutation authority remained narrowed to selected roots'
Assert-True ([bool]$realHost.result.deployment_left_installed) `
    'successful real-host deployment remains installed'
Assert-True ([bool]$realHost.host_observations.preflight.inspection_succeeded) `
    'expected LM Studio image was inspectable at preflight'
Assert-Exact ([bool]$realHost.host_observations.preflight.running) $false `
    'expected LM Studio image was stopped at preflight'
Assert-True ([bool]$realHost.host_observations.before_activation.inspection_succeeded) `
    'expected LM Studio image was inspectable before activation'
Assert-Exact ([bool]$realHost.host_observations.before_activation.running) $false `
    'expected LM Studio image remained stopped before supported activation'
Assert-True ([bool]$realHost.host_observations.after_activation_attempt.inspection_succeeded) `
    'expected LM Studio image was inspectable after activation'
Assert-True ([bool]$realHost.host_observations.after_activation_attempt.running) `
    'expected LM Studio image was running after supported activation'
Assert-Exact ([bool]$realHost.activation.running_before_deploy) $false `
    'LM Studio was stopped immediately before supported activation'
Assert-Exact ([bool]$realHost.activation.launched) $true `
    'LM Studio was launched through the supported activation path'
Assert-Exact ([bool]$realHost.activation.restarted) $false `
    'LM Studio was not killed or restarted'
Assert-Exact ([bool]$realHost.activation.configuration_synchronized) $true `
    'supported activation synchronized the exact configuration'
Assert-True ([bool]$realHost.activation.process_inspection_succeeded) `
    'activation observed the expected LM Studio process image'
Assert-True ([bool]$realHost.activation.host_running_after_attempt) `
    'activation left the expected LM Studio process running'
Assert-True ([bool]$realHost.after_deployment.synchronized_state_byte_identical_before_activation) `
    'deployment did not preemptively alter host synchronization state'
Assert-True ([bool]$realHost.after_deployment.foreign_configuration_semantics_preserved) `
    'deployment preserved foreign configuration semantics'
Assert-True ([bool]$realHost.after_deployment.foreign_plugin_tree_bytes_preserved) `
    'deployment preserved foreign plugin-tree bytes'
Assert-True ([bool]$realHost.after_deployment.no_new_transaction_roots) `
    'deployment left no new transaction roots'
Assert-True ([bool]$realHost.after_activation.exact_synchronized_revision) `
    'host synchronized the exact deployment revision'
Assert-True ([bool]$realHost.after_activation.complete_role_objects_equal) `
    'live and synchronized primary/fallback role objects match completely'
Assert-True ([bool]$realHost.after_activation.live_codec_registered) `
    'live configuration passes strict codec registration checks'
Assert-True ([bool]$realHost.after_activation.synchronized_codec_registered) `
    'synchronized configuration passes strict codec registration checks'
Assert-True ([bool]$realHost.after_activation.foreign_configuration_semantics_preserved) `
    'foreign LM Studio configuration semantics remained preserved'
Assert-True ([bool]$realHost.after_activation.foreign_plugin_tree_bytes_preserved) `
    'foreign LM Studio plugin-tree bytes remained preserved'
Assert-True ([bool]$realHost.after_activation.no_new_transaction_roots) `
    'activation left no new transaction roots'
Assert-Exact ([string]$realHost.before.foreign_configuration.sha256) `
    ([string]$realHost.after_activation.foreign_configuration.sha256) `
    'foreign configuration canonical semantic digest'
Assert-Exact ([long]$realHost.before.foreign_configuration.server_count) `
    ([long]$realHost.after_activation.foreign_configuration.server_count) `
    'foreign configuration server count'
Assert-Exact ([string]$realHost.before.foreign_plugins.sha256) `
    ([string]$realHost.after_activation.foreign_plugins.sha256) `
    'foreign plugin-tree byte digest'
Assert-Exact ([long]$realHost.before.foreign_plugins.root_entries) `
    ([long]$realHost.after_activation.foreign_plugins.root_entries) `
    'foreign plugin root-entry count'
Assert-Exact ([long]$realHost.before.foreign_plugins.tree_entries) `
    ([long]$realHost.after_activation.foreign_plugins.tree_entries) `
    'foreign plugin tree-entry count'
Assert-Exact ([long]$realHost.before.foreign_plugins.file_bytes) `
    ([long]$realHost.after_activation.foreign_plugins.file_bytes) `
    'foreign plugin total byte count'
Assert-Exact ([string]$realHost.deployment.deployment_id) `
    ([string]$realHost.activation.deployment_id) `
    'activation acknowledges the deployed revision'
Assert-TrackedTreeClean

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count remains unchanged'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count remains unchanged'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti tree hash remains unchanged'
Invoke-RepositoryIntegrityChecks

$summaryPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\g15-validation-summary.json'
$summaryScope = [ordered]@{
    operating_system = 'Windows 11'
    architecture = 'x64'
    configuration = 'Debug'
    machine_local = $true
    clean_environment_deferred = $true
    security_hardening_deferred = $true
    authoritative_build_invocations = 1
    authoritative_test_invocations = 1
    real_host_invocations = if ($Resume) { 6 } else { 1 }
    gui_automation_used_against_lm_studio = $false
}
if ($Resume) {
    $summaryScope['resumed'] = $true
    $summaryScope['corrective_focused_build_invocations'] = 6
    $summaryScope['corrective_focused_test_command_invocations'] = 6
    $summaryScope['corrective_focused_test_executions'] = 5
    $summaryScope['corrective_zero_test_filter_invocations'] = 1
    $summaryScope['corrective_focused_test_invocations'] = 6
    $summaryScope['total_real_host_attempts'] = 6
    $summaryScope['successful_real_host_qualifications'] = 1
}
$summary = [ordered]@{
    schema_version = 1
    phase = 'P15'
    gate = 'G15'
    status = 'passed'
    recorded_utc = Get-UtcTimestamp
    working_directory = $WorkspaceRoot
    scope = $summaryScope
    assertions = $script:AssertionCount
    deterministic_tests = $expectedTests
    deterministic_test_count = $expectedTests.Count
    artifacts = $artifacts
    lm_studio_host = [ordered]@{
        executable = [string]$baseline.lm_studio.executable
        bytes = [long](Get-Item -LiteralPath $hostExecutable).Length
        sha256 = [string]$baseline.lm_studio.sha256
        discovery_bound_to_baseline = [bool]$realHost.host.expected_application_bound
        launched = [bool]$realHost.activation.launched
        configuration_synchronized = [bool]$realHost.activation.configuration_synchronized
    }
    real_host_evidence = Get-RelativePathPortable `
        -BasePath $WorkspaceRoot -TargetPath $RealHostEvidencePath
    real_host_evidence_sha256 = Get-FileSha256 $RealHostEvidencePath
    acceptance = [string]$gate[0].acceptance
    remaining_limitations = @(
        'This qualifies P15/G15 only on the owner current Windows 11 x64 machine in Debug configuration.',
        'It does not qualify another machine, a clean environment, Release, ARM64, UI parity, CLI parity, packaging, or the complete alpha.',
        'Security-only hardening is deferred until after alpha under OWNER-002.'
    )
}
if ($Resume) {
    $summary['resume_evidence'] = [ordered]@{
        prior_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $priorCommandEvidence.RecordPath
        prior_failed_command_record_sha256 = $priorCommandEvidence.RecordSha256
        prior_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $priorCommandEvidence.StdoutPath
        prior_failed_command_stdout_sha256 = $priorCommandEvidence.StdoutSha256
        prior_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $priorCommandEvidence.StderrPath
        prior_failed_command_stderr_sha256 = $priorCommandEvidence.StderrSha256
        prior_failed_real_host_evidence = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $priorFailedEvidenceCanonicalPath
        prior_failed_real_host_evidence_sha256 = $priorFailedEvidenceSha256
        corrective_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $correctiveCommandEvidence.RecordPath
        corrective_command_record_sha256 = $correctiveCommandEvidence.RecordSha256
        corrective_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $correctiveCommandEvidence.StdoutPath
        corrective_command_stdout_sha256 = $correctiveCommandEvidence.StdoutSha256
        corrective_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $correctiveCommandEvidence.StderrPath
        corrective_command_stderr_sha256 = $correctiveCommandEvidence.StderrSha256
        second_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $secondFailedCommandEvidence.RecordPath
        second_failed_command_record_sha256 = $secondFailedCommandEvidence.RecordSha256
        second_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $secondFailedCommandEvidence.StdoutPath
        second_failed_command_stdout_sha256 = $secondFailedCommandEvidence.StdoutSha256
        second_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $secondFailedCommandEvidence.StderrPath
        second_failed_command_stderr_sha256 = $secondFailedCommandEvidence.StderrSha256
        second_failed_real_host_evidence = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $secondFailedEvidenceCanonicalPath
        second_failed_real_host_evidence_sha256 = $secondFailedEvidenceSha256
        runner_corrective_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $runnerCorrectiveCommandEvidence.RecordPath
        runner_corrective_command_record_sha256 = `
            $runnerCorrectiveCommandEvidence.RecordSha256
        runner_corrective_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $runnerCorrectiveCommandEvidence.StdoutPath
        runner_corrective_command_stdout_sha256 = `
            $runnerCorrectiveCommandEvidence.StdoutSha256
        runner_corrective_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $runnerCorrectiveCommandEvidence.StderrPath
        runner_corrective_command_stderr_sha256 = `
            $runnerCorrectiveCommandEvidence.StderrSha256
        third_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $thirdFailedCommandEvidence.RecordPath
        third_failed_command_record_sha256 = $thirdFailedCommandEvidence.RecordSha256
        third_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $thirdFailedCommandEvidence.StdoutPath
        third_failed_command_stdout_sha256 = $thirdFailedCommandEvidence.StdoutSha256
        third_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $thirdFailedCommandEvidence.StderrPath
        third_failed_command_stderr_sha256 = $thirdFailedCommandEvidence.StderrSha256
        third_failed_real_host_evidence = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $thirdFailedEvidenceCanonicalPath
        third_failed_real_host_evidence_sha256 = $thirdFailedEvidenceSha256
        deployment_corrective_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $deploymentCorrectiveCommandEvidence.RecordPath
        deployment_corrective_command_record_sha256 = `
            $deploymentCorrectiveCommandEvidence.RecordSha256
        deployment_corrective_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $deploymentCorrectiveCommandEvidence.StdoutPath
        deployment_corrective_command_stdout_sha256 = `
            $deploymentCorrectiveCommandEvidence.StdoutSha256
        deployment_corrective_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $deploymentCorrectiveCommandEvidence.StderrPath
        deployment_corrective_command_stderr_sha256 = `
            $deploymentCorrectiveCommandEvidence.StderrSha256
        fourth_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fourthFailedCommandEvidence.RecordPath
        fourth_failed_command_record_sha256 = $fourthFailedCommandEvidence.RecordSha256
        fourth_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fourthFailedCommandEvidence.StdoutPath
        fourth_failed_command_stdout_sha256 = $fourthFailedCommandEvidence.StdoutSha256
        fourth_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fourthFailedCommandEvidence.StderrPath
        fourth_failed_command_stderr_sha256 = $fourthFailedCommandEvidence.StderrSha256
        fourth_failed_real_host_evidence = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fourthFailedEvidenceCanonicalPath
        fourth_failed_real_host_evidence_sha256 = $fourthFailedEvidenceSha256
        process_corrective_build_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveBuildCommandEvidence.RecordPath
        process_corrective_build_command_record_sha256 = `
            $processCorrectiveBuildCommandEvidence.RecordSha256
        process_corrective_build_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveBuildCommandEvidence.StdoutPath
        process_corrective_build_command_stdout_sha256 = `
            $processCorrectiveBuildCommandEvidence.StdoutSha256
        process_corrective_build_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveBuildCommandEvidence.StderrPath
        process_corrective_build_command_stderr_sha256 = `
            $processCorrectiveBuildCommandEvidence.StderrSha256
        process_corrective_test_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveTestCommandEvidence.RecordPath
        process_corrective_test_command_record_sha256 = `
            $processCorrectiveTestCommandEvidence.RecordSha256
        process_corrective_test_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveTestCommandEvidence.StdoutPath
        process_corrective_test_command_stdout_sha256 = `
            $processCorrectiveTestCommandEvidence.StdoutSha256
        process_corrective_test_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $processCorrectiveTestCommandEvidence.StderrPath
        process_corrective_test_command_stderr_sha256 = `
            $processCorrectiveTestCommandEvidence.StderrSha256
        fifth_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fifthFailedCommandEvidence.RecordPath
        fifth_failed_command_record_sha256 = $fifthFailedCommandEvidence.RecordSha256
        fifth_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fifthFailedCommandEvidence.StdoutPath
        fifth_failed_command_stdout_sha256 = $fifthFailedCommandEvidence.StdoutSha256
        fifth_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fifthFailedCommandEvidence.StderrPath
        fifth_failed_command_stderr_sha256 = $fifthFailedCommandEvidence.StderrSha256
        fifth_failed_real_host_evidence = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $fifthFailedEvidenceCanonicalPath
        fifth_failed_real_host_evidence_sha256 = $fifthFailedEvidenceSha256
        mcp_corrective_failed_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $mcpCorrectiveFailedCommandEvidence.RecordPath
        mcp_corrective_failed_command_record_sha256 = `
            $mcpCorrectiveFailedCommandEvidence.RecordSha256
        mcp_corrective_failed_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $mcpCorrectiveFailedCommandEvidence.StdoutPath
        mcp_corrective_failed_command_stdout_sha256 = `
            $mcpCorrectiveFailedCommandEvidence.StdoutSha256
        mcp_corrective_failed_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $mcpCorrectiveFailedCommandEvidence.StderrPath
        mcp_corrective_failed_command_stderr_sha256 = `
            $mcpCorrectiveFailedCommandEvidence.StderrSha256
        mcp_corrective_command_record = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $mcpCorrectiveCommandEvidence.RecordPath
        mcp_corrective_command_record_sha256 = $mcpCorrectiveCommandEvidence.RecordSha256
        mcp_corrective_command_stdout = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $mcpCorrectiveCommandEvidence.StdoutPath
        mcp_corrective_command_stdout_sha256 = $mcpCorrectiveCommandEvidence.StdoutSha256
        mcp_corrective_command_stderr = Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $mcpCorrectiveCommandEvidence.StderrPath
        mcp_corrective_command_stderr_sha256 = $mcpCorrectiveCommandEvidence.StderrSha256
    }
}
Write-JsonFileAtomic -Path $summaryPath -Value $summary

Write-Host "G15 LM Studio deployment validation passed ($script:AssertionCount assertions)."
Write-Host "Real-host evidence: $RealHostEvidencePath"
Write-Host "Validation summary: $summaryPath"
