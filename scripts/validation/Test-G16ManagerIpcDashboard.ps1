[CmdletBinding(DefaultParameterSetName = 'Normal')]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [switch]$Resume,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [string]$PriorPassedEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Resume')]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$PriorPassedEvidenceSha256,

    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0
$script:GitTool = $null
$loadedGateScriptBytes = [Text.UTF8Encoding]::new($false, $true).GetBytes(
    $MyInvocation.MyCommand.ScriptBlock.Ast.Extent.Text)
$script:ExecutingGateScriptSha256 = Get-BytesSha256 $loadedGateScriptBytes
if ((Get-FileSha256 $PSCommandPath) -cne
    $script:ExecutingGateScriptSha256) {
    throw 'Loaded G16 runner bytes do not match the path before validation.'
}
$script:BuildInputPattern =
    '^(?:CMakeLists[.]txt|CMakePresets[.]json|Directory[.]Build[.](?:props|targets)|cmake/|include/|src/|tests/|scripts/|vcpkg(?:-configuration)?[.]json|[.]forge-codex/instructions/|[.]forge-codex/state/decisions/)'
$script:MutableParityLedgerPattern =
    '^[.]forge-codex/instructions/plans/feature-parity-matrix[.](?:json|tsv)$'
$script:ExpectedTests = @(
    'ForgeConductor.Dashboard.AcceptedConnectionHandoffTests',
    'ForgeConductor.Dashboard.AcceptSlotSetTests',
    'ForgeConductor.Dashboard.AcceptSlotTests',
    'ForgeConductor.Dashboard.AdmissionControllerTests',
    'ForgeConductor.Dashboard.ApplicationErrorMapperTests',
    'ForgeConductor.Dashboard.ApplicationJsonCodecTests',
    'ForgeConductor.Dashboard.ConnectionApplicationFactoryTests',
    'ForgeConductor.Dashboard.ConnectionApplicationTests',
    'ForgeConductor.Dashboard.ConnectionEventBridgeTests',
    'ForgeConductor.Dashboard.ConnectionRegistryTests',
    'ForgeConductor.Dashboard.ConnectionResponseCatalogTests',
    'ForgeConductor.Dashboard.ConnectionRuntimeServicesTests',
    'ForgeConductor.Dashboard.ConnectionSocketTests',
    'ForgeConductor.Dashboard.ConnectionStateTests',
    'ForgeConductor.Dashboard.DeadlineIocpBridgeTests',
    'ForgeConductor.Dashboard.DeadlineNotificationMailboxTests',
    'ForgeConductor.Dashboard.DeadlineSchedulerTests',
    'ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests',
    'ForgeConductor.Dashboard.HandlerExecutorTests',
    'ForgeConductor.Dashboard.HandlerOperationsTests',
    'ForgeConductor.Dashboard.HttpParserSessionTests',
    'ForgeConductor.Dashboard.HttpParserTests',
    'ForgeConductor.Dashboard.HttpResponseTests',
    'ForgeConductor.Dashboard.IoCompletionPortTests',
    'ForgeConductor.Dashboard.IocpCompletionRouterTests',
    'ForgeConductor.Dashboard.IocpWorkerKernelTests',
    'ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests',
    'ForgeConductor.Dashboard.ListenerGenerationTests',
    'ForgeConductor.Dashboard.ListeningSocketTests',
    'ForgeConductor.Dashboard.LoopbackEndpointTests',
    'ForgeConductor.Dashboard.ManagerJsonCodecTests',
    'ForgeConductor.Dashboard.OperationalServiceTests',
    'ForgeConductor.Dashboard.OverloadResponderSetTests',
    'ForgeConductor.Dashboard.PreparedExchangeTests',
    'ForgeConductor.Dashboard.RequestPlannerTests',
    'ForgeConductor.Dashboard.RequestPolicyTests',
    'ForgeConductor.Dashboard.ResponseComposerTests',
    'ForgeConductor.Dashboard.RouteCatalogTests',
    'ForgeConductor.Dashboard.SessionCloseRequestDecoderTests',
    'ForgeConductor.Dashboard.ShutdownDrainTests',
    'ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests',
    'ForgeConductor.Dashboard.SseBroadcasterTests',
    'ForgeConductor.Dashboard.StaticAssetStoreTests',
    'ForgeConductor.Dashboard.StaticResourcePathTests',
    'ForgeConductor.Dashboard.StaticShellContractTests',
    'ForgeConductor.Dashboard.StreamQueryDecoderTests',
    'ForgeConductor.Dashboard.TelemetryJsonCodecTests',
    'ForgeConductor.Dashboard.TelemetrySourceTests',
    'ForgeConductor.Dashboard.WindowsBearerTokenTests',
    'ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests',
    'ForgeConductor.Dashboard.WindowsRuntimeTests',
    'ForgeConductor.Dashboard.WindowsStaticAssetBundleTests',
    'ForgeConductor.Dashboard.WinsockExtensionsTests',
    'ForgeConductor.Dashboard.WinsockRuntimeTests',
    'ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests',
    'ForgeConductor.Manager.BrowserLauncherTests',
    'ForgeConductor.Manager.CompositionLifecycleTests',
    'ForgeConductor.Manager.CompositionSupportTests',
    'ForgeConductor.Manager.ControllerTests',
    'ForgeConductor.Manager.DashboardOperationalDataSourceTests',
    'ForgeConductor.Manager.DeadlineMapperTests',
    'ForgeConductor.Manager.DoctorServiceTests',
    'ForgeConductor.Manager.LmStudioCompositionSupportTests',
    'ForgeConductor.Manager.LmStudioReadScopeResolverTests',
    'ForgeConductor.Manager.MaintenanceServiceTests',
    'ForgeConductor.Manager.MaintenanceWorkerTests',
    'ForgeConductor.Manager.NamedPipeRoundTripTests',
    'ForgeConductor.Manager.ProcessArgumentsTests',
    'ForgeConductor.Manager.ProcessEnvironmentRealTests',
    'ForgeConductor.Manager.ProcessEnvironmentTests',
    'ForgeConductor.Manager.ProcessHostTests',
    'ForgeConductor.Manager.ProcessRestartSignalTests',
    'ForgeConductor.Manager.ProcessStopWatcherTests',
    'ForgeConductor.Manager.ProcessWorkerGroupTests',
    'ForgeConductor.Manager.ProtocolTests',
    'ForgeConductor.Manager.RequestDispatcherTests',
    'ForgeConductor.Manager.StartupLifecycleIntegrationTests',
    'ForgeConductor.Manager.StartupTests',
    'ForgeConductor.Manager.TransitionWorkerTests',
    'ForgeConductor.Manager.UnavailableTelemetryServiceTests',
    'ForgeConductor.Manager.WindowsInfrastructureTests',
    'ForgeConductor.Manager.WindowsRuntimeTests',
    'ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests'
)
$script:ExpectedTestArgumentTargets = @{
    'ForgeConductor.Manager.BrowserLauncherTests' = @(
        'ForgeConductor.Cli'
    )
    'ForgeConductor.Manager.CompositionLifecycleTests' = @(
        'ForgeConductor.Manager.CompositionFixture',
        'ForgeConductor.Cli'
    )
    'ForgeConductor.Manager.StartupLifecycleIntegrationTests' = @(
        'ForgeConductor.Manager.StartupLifecycleFixture'
    )
}
$script:ExpectedResources = @(
    [ordered]@{
        id = 301
        macro = 'FORGE_DASHBOARD_INDEX_HTML_RESOURCE_ID'
        file = 'index.html'
        route = '/static/index.html'
    },
    [ordered]@{
        id = 302
        macro = 'FORGE_DASHBOARD_CONTROL_HTML_RESOURCE_ID'
        file = 'control.html'
        route = '/static/control.html'
    },
    [ordered]@{
        id = 303
        macro = 'FORGE_DASHBOARD_CSS_RESOURCE_ID'
        file = 'dashboard.css'
        route = '/static/dashboard.css'
    },
    [ordered]@{
        id = 304
        macro = 'FORGE_DASHBOARD_AUTH_JS_RESOURCE_ID'
        file = 'auth.js'
        route = '/static/auth.js'
    },
    [ordered]@{
        id = 305
        macro = 'FORGE_DASHBOARD_TELEMETRY_JS_RESOURCE_ID'
        file = 'telemetry.js'
        route = '/static/telemetry.js'
    },
    [ordered]@{
        id = 306
        macro = 'FORGE_DASHBOARD_CONTROL_JS_RESOURCE_ID'
        file = 'control.js'
        route = '/static/control.js'
    }
)

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G16 assertion failed: $Message" }
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
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message,
        [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor
            [Text.RegularExpressions.RegexOptions]::IgnoreCase
    }
    Assert-True ([regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-NoMatch {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message,
        [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor
            [Text.RegularExpressions.RegexOptions]::IgnoreCase
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
            ($index -eq 0 -or $bytes[$index - 1] -ne 13)) {
            $bareLf++
        }
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
    Assert-NoMatch $text '[ \t]+\r\n' "$Message trailing whitespace" `
        -CaseSensitive
}

function Copy-FileBytesAtomicExact {
    param([string]$Source, [string]$Destination)
    $directory = Split-Path -Parent $Destination
    Assert-True (Test-Path -LiteralPath $directory -PathType Container) `
        'durable evidence directory exists'
    $temporary = $Destination + '.tmp.' + $PID + '.' +
        [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllBytes(
            $temporary,
            [IO.File]::ReadAllBytes($Source))
        Move-Item -LiteralPath $temporary -Destination $Destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
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

function Resolve-StateTool {
    param([string]$CommandName, [string]$PropertyName)
    $statePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
    Assert-True (Test-Path -LiteralPath $statePath -PathType Leaf) `
        'toolchain state exists'
    $state = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $statePath)
    Assert-True ($null -ne $state.tools) `
        'toolchain state has a tools object'
    $candidate = [string]$state.tools.$PropertyName
    Assert-True (-not [string]::IsNullOrWhiteSpace($candidate)) `
        "$CommandName has an exact toolchain-state selection"
    Assert-True ([IO.Path]::IsPathRooted($candidate)) `
        "$CommandName toolchain-state selection is absolute"
    Assert-True (Test-Path -LiteralPath $candidate -PathType Leaf) `
        "$CommandName toolchain-state selection exists"
    $resolved = (Resolve-Path -LiteralPath $candidate).Path
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($candidate),
        $resolved,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$CommandName toolchain-state selection resolves without redirection"
    return $resolved
}

function Move-G16PriorLastTestLog {
    param([string]$Source, [string]$AttemptId)
    $historyRoot = Join-Path $WorkspaceRoot `
        '.forge-codex\state\evidence\P16\g16-attempt-history'
    if (-not (Test-Path -LiteralPath $historyRoot -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $historyRoot)
    }
    $historyItem = Get-Item -Force -LiteralPath $historyRoot
    Assert-True (($historyItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 attempt-history directory is not a reparse point'
    $destination = Join-Path $historyRoot `
        ($AttemptId + '-pretest-LastTest.log')
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        Assert-True (-not (Test-Path -LiteralPath $Source -PathType Leaf)) `
            'G16 prior LastTest archive and source do not coexist'
        $archived = Get-Item -Force -LiteralPath $destination
        Assert-True (($archived.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'G16 recovered prior LastTest archive is not a reparse point'
        Assert-True ($archived.Length -le 64MB) `
            'G16 recovered prior LastTest archive has bounded bytes'
        return [ordered]@{
            status = 'archived'
            path = (Get-RelativePathPortable `
                -BasePath $WorkspaceRoot `
                -TargetPath $destination).Replace('\', '/')
            bytes = [long]$archived.Length
            sha256 = Get-FileSha256 $destination
        }
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        return [ordered]@{
            status = 'absent'
            path = $null
            bytes = 0L
            sha256 = $null
        }
    }
    $sourceItem = Get-Item -Force -LiteralPath $Source
    Assert-True (($sourceItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 prior LastTest source is not a reparse point'
    $sourceBytes = [long]$sourceItem.Length
    Assert-True ($sourceBytes -le 64MB) `
        'G16 prior LastTest source has bounded bytes'
    $sourceSha256 = Get-FileSha256 $Source
    Move-Item -LiteralPath $Source -Destination $destination
    Assert-True (-not (Test-Path -LiteralPath $Source)) `
        'G16 prior LastTest source was removed by the archive move'
    Assert-Exact ([long](Get-Item -LiteralPath $destination).Length) `
        $sourceBytes 'G16 prior LastTest archive bytes'
    Assert-Exact (Get-FileSha256 $destination) $sourceSha256 `
        'G16 prior LastTest archive SHA-256'
    return [ordered]@{
        status = 'archived'
        path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $destination).Replace('\', '/')
        bytes = $sourceBytes
        sha256 = $sourceSha256
    }
}

function Copy-G16HistoryFile {
    param(
        [string]$Source,
        [string]$Destination,
        [string]$LogicalName)
    $sourceItem = Get-Item -Force -LiteralPath $Source
    Assert-True (($sourceItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "G16 history source is not a reparse point: $LogicalName"
    $sourceBytes = [long]$sourceItem.Length
    $maximumBytes = switch -CaseSensitive ($LogicalName) {
        'attempt' { 16MB; break }
        'summary' { 4MB; break }
        'durable_last_test' { 64MB; break }
        'transient_last_test' { 64MB; break }
        default { throw "Unrecognized G16 history logical name: $LogicalName" }
    }
    Assert-True ($sourceBytes -gt 0 -and $sourceBytes -le $maximumBytes) `
        "G16 history source has bounded bytes: $LogicalName"
    $sourceSha256 = Get-FileSha256 $Source
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        Assert-Exact ([long](Get-Item -LiteralPath $Destination).Length) `
            $sourceBytes "G16 existing history bytes: $LogicalName"
        Assert-Exact (Get-FileSha256 $Destination) $sourceSha256 `
            "G16 existing history SHA-256: $LogicalName"
    } else {
        Copy-FileBytesAtomicExact -Source $Source -Destination $Destination
    }
    return [ordered]@{
        name = $LogicalName
        canonical_path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $Source).Replace('\', '/')
        archive_path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $Destination).Replace('\', '/')
        bytes = $sourceBytes
        sha256 = $sourceSha256
    }
}

function Assert-G16RolloverDeletionPlan {
    param(
        $Manifest,
        $Attempt,
        $CanonicalByName,
        [string]$BundleRoot,
        [switch]$SkipExtantCanonicalCoverage)
    Assert-Exact ([int]$Manifest.schema_version) 1 `
        'G16 rollover manifest schema version'
    Assert-Exact ([string]$Manifest.phase) 'P16' `
        'G16 rollover manifest phase'
    Assert-Exact ([string]$Manifest.gate) 'G16' `
        'G16 rollover manifest gate'
    Assert-True (@(
            'source_or_toolchain_context_changed',
            'interrupted_authoritative_invocation_retry'
        ) -ccontains [string]$Manifest.reason) `
        'G16 rollover manifest reason is recognized'
    Assert-Exact ([string]$Manifest.attempt_id) `
        ([string]$Attempt.attempt_id) 'G16 rollover attempt identifier'
    Assert-Exact ([string]$Manifest.status) ([string]$Attempt.status) `
        'G16 rollover attempt status'
    Assert-Exact ([string]$Manifest.old_context_sha256) `
        ([string]$Attempt.context.sha256) 'G16 rollover old context'
    Assert-Match ([string]$Manifest.new_context_sha256) `
        '^[0-9a-f]{64}$' 'G16 rollover intended context' -CaseSensitive
    if ([string]$Manifest.reason -ceq
        'interrupted_authoritative_invocation_retry') {
        Assert-True ([string]$Attempt.status -cin @(
                'build_started', 'test_started')) `
            'G16 interrupted-retry rollover archives an interrupted status'
        Assert-Exact ([string]$Manifest.new_context_sha256) `
            ([string]$Manifest.old_context_sha256) `
            'G16 interrupted-retry rollover preserves the source/toolchain context'
    }
    $records = @($Manifest.files)
    Assert-True ($records.Count -ge 1 -and
        $records.Count -le $CanonicalByName.Count) `
        'G16 rollover manifest has a bounded file count'
    Assert-Exact @($records | ForEach-Object { [string]$_.name } |
        Sort-Object -Unique -CaseSensitive).Count $records.Count `
        'G16 rollover manifest file names are unique'
    foreach ($record in $records) {
        $name = [string]$record.name
        Assert-True ($CanonicalByName.Contains($name)) `
            "G16 rollover manifest file name is allowed: $name"
        Assert-Match ([string]$record.sha256) '^[0-9a-f]{64}$' `
            "G16 rollover manifest SHA-256 shape: $name" -CaseSensitive
        Assert-True ([long]$record.bytes -ge 0 -and
            [long]$record.bytes -le 512MB) `
            "G16 rollover manifest bounded bytes: $name"
        $expectedCanonical = [IO.Path]::GetFullPath(
            [string]$CanonicalByName[$name])
        $recordedCanonical = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
            ([string]$record.canonical_path).Replace('/', '\')))
        Assert-True ([string]::Equals(
            $recordedCanonical,
            $expectedCanonical,
            [StringComparison]::OrdinalIgnoreCase)) `
            "G16 rollover exact canonical path: $name"
        $expectedArchive = [IO.Path]::GetFullPath((Join-Path $BundleRoot `
            ($name + [IO.Path]::GetExtension($expectedCanonical))))
        $recordedArchive = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
            ([string]$record.archive_path).Replace('/', '\')))
        Assert-True ([string]::Equals(
            $recordedArchive,
            $expectedArchive,
            [StringComparison]::OrdinalIgnoreCase)) `
            "G16 rollover exact archive path: $name"
        Assert-True (Test-Path -LiteralPath $recordedArchive -PathType Leaf) `
            "G16 rollover archive exists: $name"
        $archiveItem = Get-Item -Force -LiteralPath $recordedArchive
        Assert-True (($archiveItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "G16 rollover archive is not a reparse point: $name"
        Assert-Exact ([long]$archiveItem.Length) ([long]$record.bytes) `
            "G16 rollover archive bytes: $name"
        Assert-Exact (Get-FileSha256 $recordedArchive) `
            ([string]$record.sha256) `
            "G16 rollover archive SHA-256: $name"
    }
    Assert-Exact @($records | Where-Object name -CEQ 'attempt').Count 1 `
        'G16 rollover manifest includes the canonical attempt exactly once'
    $summaryRecords = @($records | Where-Object name -CEQ 'summary')
    if ($summaryRecords.Count -gt 0) {
        Assert-Exact ([string]$Attempt.status) 'test_passed' `
            'G16 rollover summary belongs only to a passing attempt'
    }
    if ([string]$Attempt.status -ceq 'test_passed') {
        $attemptRecord = @($records | Where-Object name -CEQ 'attempt')[0]
        $durableRecords = @(
            $records | Where-Object name -CEQ 'durable_last_test')
        $transientRecords = @(
            $records | Where-Object name -CEQ 'transient_last_test')
        Assert-Exact $durableRecords.Count 1 `
            'G16 passing rollover includes the durable CTest log exactly once'
        Assert-True ($transientRecords.Count -le 1) `
            'G16 passing rollover has at most one transient CTest log'
        $passingLogRecords = @($durableRecords[0])
        if ($transientRecords.Count -eq 1) {
            $passingLogRecords += $transientRecords[0]
        }
        foreach ($logRecord in $passingLogRecords) {
            Assert-Exact ([long]$logRecord.bytes) `
                ([long]$Attempt.transient_ctest_log.bytes) `
                "G16 passing rollover log bytes: $($logRecord.name)"
            Assert-Exact ([string]$logRecord.sha256) `
                ([string]$Attempt.transient_ctest_log.sha256) `
                "G16 passing rollover log SHA-256: $($logRecord.name)"
            $archivedLogPath = Join-Path $WorkspaceRoot `
                ([string]$logRecord.archive_path).Replace('/', '\')
            $passingLog = Get-LastTestLogEvidence $archivedLogPath
            Assert-Exact ([long]$passingLog.bytes) ([long]$logRecord.bytes) `
                "G16 passing rollover parsed log bytes: $($logRecord.name)"
            Assert-Exact ([string]$passingLog.sha256) `
                ([string]$logRecord.sha256) `
                "G16 passing rollover parsed log SHA-256: $($logRecord.name)"
        }
        Assert-True ($summaryRecords.Count -le 1) `
            'G16 passing rollover has at most one summary'
        if ($summaryRecords.Count -eq 1) {
            $summaryRecord = $summaryRecords[0]
            $archivedSummaryPath = Join-Path $WorkspaceRoot `
                ([string]$summaryRecord.archive_path).Replace('/', '\')
            $archivedSummary = ConvertFrom-JsonPreserveDates -Value `
                (Get-Content -Raw -LiteralPath $archivedSummaryPath)
            Assert-Exact ([int]$archivedSummary.schema_version) 1 `
                'G16 rollover summary schema version'
            Assert-Exact ([string]$archivedSummary.phase) 'P16' `
                'G16 rollover summary phase'
            Assert-Exact ([string]$archivedSummary.gate) 'G16' `
                'G16 rollover summary gate'
            Assert-Exact ([string]$archivedSummary.status) 'passed' `
                'G16 rollover summary status'
            Assert-Exact ([string]$archivedSummary.attempt_checkpoint.attempt_id) `
                ([string]$Attempt.attempt_id) `
                'G16 rollover summary attempt identifier'
            Assert-Exact ([string]$archivedSummary.attempt_checkpoint.status) `
                'test_passed' 'G16 rollover summary attempt status'
            Assert-Exact ([string]$archivedSummary.attempt_checkpoint.sha256) `
                ([string]$attemptRecord.sha256) `
                'G16 rollover summary binds the archived attempt checkpoint'
            Assert-Exact `
                ([string]$archivedSummary.attempt_checkpoint.context_sha256) `
                ([string]$Attempt.context.sha256) `
                'G16 rollover summary context SHA-256'
            Assert-Exact ([long]$archivedSummary.ctest_log.bytes) `
                ([long]$durableRecords[0].bytes) `
                'G16 rollover summary durable CTest-log bytes'
            Assert-Exact ([string]$archivedSummary.ctest_log.sha256) `
                ([string]$durableRecords[0].sha256) `
                'G16 rollover summary durable CTest-log SHA-256'
            Assert-Exact ([long]$archivedSummary.transient_ctest_log.bytes) `
                ([long]$Attempt.transient_ctest_log.bytes) `
                'G16 rollover summary transient CTest-log bytes'
            Assert-Exact ([string]$archivedSummary.transient_ctest_log.sha256) `
                ([string]$Attempt.transient_ctest_log.sha256) `
                'G16 rollover summary transient CTest-log SHA-256'
            Assert-Exact `
                ([string]$archivedSummary.transient_ctest_log.durable_copy_sha256) `
                ([string]$durableRecords[0].sha256) `
                'G16 rollover summary binds the durable CTest-log copy'
        }
    }
    if (-not $SkipExtantCanonicalCoverage) {
        foreach ($name in $CanonicalByName.Keys) {
            $canonicalPath = [string]$CanonicalByName[$name]
            if (Test-Path -LiteralPath $canonicalPath -PathType Leaf) {
                Assert-Exact @($records | Where-Object name -CEQ $name).Count 1 `
                    "G16 rollover covers each extant canonical file: $name"
            }
        }
    }
}

function Archive-G16CanonicalAttempt {
    param(
        $Attempt,
        [string]$AttemptPath,
        [string]$SummaryPath,
        [string]$DurableLastTestLogPath,
        [string]$TransientLastTestLogPath,
        $NewContext,
        [ValidateSet(
            'source_or_toolchain_context_changed',
            'interrupted_authoritative_invocation_retry')]
        [string]$Reason = 'source_or_toolchain_context_changed')
    $historyRoot = Join-Path $WorkspaceRoot `
        '.forge-codex\state\evidence\P16\g16-attempt-history'
    if (-not (Test-Path -LiteralPath $historyRoot -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $historyRoot)
    }
    $historyRootItem = Get-Item -Force -LiteralPath $historyRoot
    Assert-True (($historyRootItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 attempt-history root is not a reparse point'
    $bundleRoot = Join-Path $historyRoot ([string]$Attempt.attempt_id)
    if (-not (Test-Path -LiteralPath $bundleRoot -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $bundleRoot)
    }
    $bundleItem = Get-Item -Force -LiteralPath $bundleRoot
    Assert-True (($bundleItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 attempt-history bundle is not a reparse point'
    $manifestPath = Join-Path $bundleRoot 'rollover.json'
    $canonicalByName = [ordered]@{
        attempt = $AttemptPath
        summary = $SummaryPath
        durable_last_test = $DurableLastTestLogPath
        transient_last_test = $TransientLastTestLogPath
    }
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifestItem = Get-Item -Force -LiteralPath $manifestPath
        Assert-True (($manifestItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'G16 rollover manifest is not a reparse point'
        Assert-True ($manifestItem.Length -gt 0 -and
            $manifestItem.Length -le 1MB) `
            'G16 rollover manifest has bounded bytes'
        $manifest = ConvertFrom-JsonPreserveDates -Value `
            (Get-Content -Raw -LiteralPath $manifestPath)
    } else {
        $files = [Collections.Generic.List[object]]::new()
        foreach ($name in $canonicalByName.Keys) {
            $source = [string]$canonicalByName[$name]
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                continue
            }
            $extension = [IO.Path]::GetExtension($source)
            $destination = Join-Path $bundleRoot ($name + $extension)
            $files.Add((Copy-G16HistoryFile `
                -Source $source `
                -Destination $destination `
                -LogicalName $name))
        }
        Assert-True (@($files | Where-Object name -CEQ 'attempt').Count -eq 1) `
            'G16 rollover bundle includes the canonical attempt'
        $manifest = [ordered]@{
            schema_version = 1
            phase = 'P16'
            gate = 'G16'
            attempt_id = [string]$Attempt.attempt_id
            status = [string]$Attempt.status
            archived_utc = Get-UtcTimestamp
            reason = $Reason
            old_context_sha256 = [string]$Attempt.context.sha256
            new_context_sha256 = [string]$NewContext.sha256
            files = @($files)
        }
        Write-JsonFileAtomic -Path $manifestPath -Value $manifest
    }
    Assert-G16RolloverDeletionPlan `
        -Manifest $manifest `
        -Attempt $Attempt `
        -CanonicalByName $canonicalByName `
        -BundleRoot $bundleRoot
    $intendedContextSha256 = [string]$manifest.new_context_sha256
    $predecessor = [ordered]@{
        attempt_id = [string]$Attempt.attempt_id
        status = [string]$Attempt.status
        manifest_path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot `
            -TargetPath $manifestPath).Replace('\', '/')
        manifest_sha256 = Get-FileSha256 $manifestPath
        old_context_sha256 = [string]$Attempt.context.sha256
        intended_replacement_context_sha256 = $intendedContextSha256
        actual_replacement_context_sha256 = [string]$NewContext.sha256
    }
    $pendingPath = Join-Path $WorkspaceRoot `
        '.forge-codex\state\evidence\P16\g16-rollover-pending.json'
    $pending = [ordered]@{
        schema_version = 1
        phase = 'P16'
        gate = 'G16'
        new_context_sha256 = $intendedContextSha256
        predecessor = $predecessor
    }
    if (Test-Path -LiteralPath $pendingPath -PathType Leaf) {
        $existingPending = ConvertFrom-JsonPreserveDates -Value `
            (Get-Content -Raw -LiteralPath $pendingPath)
        Assert-Exact ([int]$existingPending.schema_version) 1 `
            'G16 existing pending-rollover schema version'
        Assert-Exact ([string]$existingPending.predecessor.attempt_id) `
            ([string]$predecessor.attempt_id) `
            'G16 existing pending-rollover predecessor attempt'
        Assert-Exact ([string]$existingPending.predecessor.manifest_sha256) `
            ([string]$predecessor.manifest_sha256) `
            'G16 existing pending-rollover predecessor manifest'
        $pending = $existingPending
    } else {
        Write-JsonFileAtomic -Path $pendingPath -Value $pending
    }
    $orderedRemovalRecords = @(
        @($manifest.files | Where-Object name -CNE 'attempt')
        @($manifest.files | Where-Object name -CEQ 'attempt')
    )
    foreach ($record in $orderedRemovalRecords) {
        $canonicalPath = Join-Path $WorkspaceRoot `
            ([string]$record.canonical_path).Replace('/', '\')
        if (Test-Path -LiteralPath $canonicalPath -PathType Leaf) {
            Assert-Exact ([long](Get-Item -LiteralPath $canonicalPath).Length) `
                ([long]$record.bytes) `
                "G16 rollover canonical bytes: $($record.name)"
            Assert-Exact (Get-FileSha256 $canonicalPath) `
                ([string]$record.sha256) `
                "G16 rollover canonical SHA-256: $($record.name)"
            Remove-Item -LiteralPath $canonicalPath
        }
    }
    return [ordered]@{
        attempt_id = [string]$pending.predecessor.attempt_id
        status = [string]$pending.predecessor.status
        manifest_path = [string]$pending.predecessor.manifest_path
        manifest_sha256 = [string]$pending.predecessor.manifest_sha256
        old_context_sha256 =
            [string]$pending.predecessor.old_context_sha256
        intended_replacement_context_sha256 =
            [string]$pending.new_context_sha256
        actual_replacement_context_sha256 = [string]$NewContext.sha256
    }
}

function Read-G16PendingRollover {
    param([string]$Path, $CurrentContext)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    $item = Get-Item -Force -LiteralPath $Path
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 pending-rollover marker is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 1MB) `
        'G16 pending-rollover marker has bounded bytes'
    $pending = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $Path)
    Assert-Exact ([int]$pending.schema_version) 1 `
        'G16 pending-rollover schema version'
    Assert-Exact ([string]$pending.phase) 'P16' `
        'G16 pending-rollover phase'
    Assert-Exact ([string]$pending.gate) 'G16' `
        'G16 pending-rollover gate'
    Assert-Match ([string]$pending.new_context_sha256) '^[0-9a-f]{64}$' `
        'G16 pending-rollover intended context' -CaseSensitive
    Assert-Match ([string]$pending.predecessor.attempt_id) `
        '^[0-9a-f]{32}$' 'G16 pending-rollover attempt identifier' `
        -CaseSensitive
    Assert-Match ([string]$pending.predecessor.manifest_sha256) `
        '^[0-9a-f]{64}$' 'G16 pending-rollover manifest SHA-256 shape' `
        -CaseSensitive
    $expectedManifestPath = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
        ('.forge-codex\state\evidence\P16\g16-attempt-history\' +
            [string]$pending.predecessor.attempt_id + '\rollover.json')))
    $recordedManifestPath = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
        ([string]$pending.predecessor.manifest_path).Replace('/', '\')))
    Assert-True ([string]::Equals(
        $recordedManifestPath,
        $expectedManifestPath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'G16 pending-rollover exact manifest path'
    $manifestPath = $expectedManifestPath
    Assert-True (Test-Path -LiteralPath $manifestPath -PathType Leaf) `
        'G16 pending-rollover manifest exists'
    $manifestItem = Get-Item -Force -LiteralPath $manifestPath
    Assert-True (($manifestItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 pending-rollover manifest is not a reparse point'
    Assert-True ($manifestItem.Length -gt 0 -and
        $manifestItem.Length -le 1MB) `
        'G16 pending-rollover manifest has bounded bytes'
    Assert-Exact (Get-FileSha256 $manifestPath) `
        ([string]$pending.predecessor.manifest_sha256) `
        'G16 pending-rollover manifest SHA-256'
    return [ordered]@{
        attempt_id = [string]$pending.predecessor.attempt_id
        status = [string]$pending.predecessor.status
        manifest_path = [string]$pending.predecessor.manifest_path
        manifest_sha256 = [string]$pending.predecessor.manifest_sha256
        old_context_sha256 =
            [string]$pending.predecessor.old_context_sha256
        intended_replacement_context_sha256 =
            [string]$pending.new_context_sha256
        actual_replacement_context_sha256 =
            [string]$CurrentContext.sha256
    }
}

function Assert-G16PredecessorLineage {
    param(
        $Predecessor,
        [string]$AttemptPath,
        [string]$SummaryPath,
        [string]$DurableLastTestLogPath,
        [string]$TransientLastTestLogPath)
    if ($null -eq $Predecessor) {
        return
    }
    Assert-Match ([string]$Predecessor.attempt_id) '^[0-9a-f]{32}$' `
        'G16 predecessor lineage attempt identifier' -CaseSensitive
    $historyRoot = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
        '.forge-codex\state\evidence\P16\g16-attempt-history'))
    $bundleRoot = [IO.Path]::GetFullPath((Join-Path $historyRoot `
        ([string]$Predecessor.attempt_id)))
    $manifestPath = [IO.Path]::GetFullPath((Join-Path $bundleRoot `
        'rollover.json'))
    $recordedManifestPath = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
        ([string]$Predecessor.manifest_path).Replace('/', '\')))
    Assert-True ([string]::Equals(
        $recordedManifestPath,
        $manifestPath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'G16 predecessor lineage exact manifest path'
    Assert-True (Test-Path -LiteralPath $manifestPath -PathType Leaf) `
        'G16 predecessor lineage manifest exists'
    $manifestItem = Get-Item -Force -LiteralPath $manifestPath
    Assert-True (($manifestItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 predecessor lineage manifest is not a reparse point'
    Assert-True ($manifestItem.Length -gt 0 -and
        $manifestItem.Length -le 1MB) `
        'G16 predecessor lineage manifest has bounded bytes'
    Assert-Exact (Get-FileSha256 $manifestPath) `
        ([string]$Predecessor.manifest_sha256) `
        'G16 predecessor lineage manifest SHA-256'
    $manifest = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $manifestPath)
    Assert-Exact ([string]$manifest.new_context_sha256) `
        ([string]$Predecessor.intended_replacement_context_sha256) `
        'G16 predecessor lineage intended replacement context'
    $attemptRecord = @($manifest.files | Where-Object name -CEQ 'attempt')
    Assert-Exact $attemptRecord.Count 1 `
        'G16 predecessor lineage has one archived attempt'
    $archivedAttemptPath = Join-Path $WorkspaceRoot `
        ([string]$attemptRecord[0].archive_path).Replace('/', '\')
    $archivedAttempt = Read-G16AttemptCheckpoint $archivedAttemptPath
    Assert-Exact ([string]$archivedAttempt.attempt_id) `
        ([string]$Predecessor.attempt_id) `
        'G16 predecessor lineage archived attempt identifier'
    Assert-Exact ([string]$archivedAttempt.status) `
        ([string]$Predecessor.status) `
        'G16 predecessor lineage archived attempt status'
    Assert-Exact ([string]$archivedAttempt.context.sha256) `
        ([string]$Predecessor.old_context_sha256) `
        'G16 predecessor lineage archived context'
    $canonicalByName = [ordered]@{
        attempt = $AttemptPath
        summary = $SummaryPath
        durable_last_test = $DurableLastTestLogPath
        transient_last_test = $TransientLastTestLogPath
    }
    Assert-G16RolloverDeletionPlan `
        -Manifest $manifest `
        -Attempt $archivedAttempt `
        -CanonicalByName $canonicalByName `
        -BundleRoot $bundleRoot `
        -SkipExtantCanonicalCoverage
}

function Assert-PathToolIdentity {
    param([string]$CommandName, [string]$ExpectedPath)
    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        Assert-True $true `
            "$CommandName may use the exact toolchain-state fallback"
        return
    }
    Assert-True ([string]::Equals(
        (Resolve-Path -LiteralPath $command.Source).Path,
        $ExpectedPath,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$CommandName PATH identity matches the selected toolchain"
}

function Get-VcpkgEvidence {
    param($ToolchainState, [string]$GitPath)
    $root = (Resolve-Path -LiteralPath `
        ([string]$ToolchainState.vcpkg.root)).Path
    $executable = (Resolve-Path -LiteralPath `
        ([string]$ToolchainState.vcpkg.executable)).Path
    $toolchainFile = (Resolve-Path -LiteralPath (Join-Path $root `
        'scripts\buildsystems\vcpkg.cmake')).Path
    Assert-True ([string]::Equals(
        (Resolve-Path -LiteralPath `
            ([string]$ToolchainState.tools.vcpkg)).Path,
        $executable,
        [StringComparison]::OrdinalIgnoreCase)) `
        'toolchain-state vcpkg executable identities agree'
    Assert-True ([string]::Equals(
        $executable,
        (Join-Path $root 'vcpkg.exe'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'selected vcpkg executable is the exact file invoked by build.ps1'

    $headOutput = @(& $GitPath -C $root rev-parse HEAD 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('selected vcpkg checkout HEAD query: ' + ($headOutput -join ' '))
    $head = ($headOutput -join '').Trim()
    Assert-Match $head '^[0-9a-f]{40}$' `
        'selected vcpkg checkout HEAD shape' -CaseSensitive
    Assert-Exact $head ([string]$ToolchainState.vcpkg.checkout_head) `
        'selected vcpkg checkout HEAD matches toolchain state'
    Assert-Exact $head ([string]$ToolchainState.vcpkg.builtin_baseline) `
        'selected vcpkg checkout HEAD matches the governed baseline'
    $dirtyOutput = @(& $GitPath -C $root status --porcelain=v1 2>&1)
    Assert-Exact $LASTEXITCODE 0 'selected vcpkg checkout status query'
    Assert-Exact $dirtyOutput.Count 0 `
        ('selected vcpkg checkout is clean: ' +
            ($dirtyOutput -join ', '))

    return [ordered]@{
        root = $root
        executable = $executable
        executable_sha256 = Get-FileSha256 $executable
        checkout_head = $head
        builtin_baseline = [string]$ToolchainState.vcpkg.builtin_baseline
        toolchain_file = $toolchainFile
        toolchain_file_bytes = [long](Get-Item -LiteralPath $toolchainFile).Length
        toolchain_file_sha256 = Get-FileSha256 $toolchainFile
        git_path = $GitPath
        git_sha256 = Get-FileSha256 $GitPath
    }
}

function New-G16ToolchainIdentity {
    param(
        [string]$ToolchainStateSha256,
        [string]$CmakePath,
        [string]$CtestPath,
        [string]$CompilerPath,
        [string]$LinkerPath,
        [string]$MsbuildPath,
        [string]$GitPath,
        $VcpkgEvidence)
    return [ordered]@{
        state_sha256 = $ToolchainStateSha256
        cmake = [ordered]@{
            path = $CmakePath
            sha256 = Get-FileSha256 $CmakePath
        }
        ctest = [ordered]@{
            path = $CtestPath
            sha256 = Get-FileSha256 $CtestPath
        }
        compiler = [ordered]@{
            path = $CompilerPath
            sha256 = Get-FileSha256 $CompilerPath
        }
        linker = [ordered]@{
            path = $LinkerPath
            sha256 = Get-FileSha256 $LinkerPath
        }
        msbuild = [ordered]@{
            path = $MsbuildPath
            sha256 = Get-FileSha256 $MsbuildPath
        }
        git = [ordered]@{
            path = $GitPath
            sha256 = Get-FileSha256 $GitPath
        }
        vcpkg = [ordered]@{
            root = [string]$VcpkgEvidence.root
            executable = [string]$VcpkgEvidence.executable
            executable_sha256 = [string]$VcpkgEvidence.executable_sha256
            checkout_head = [string]$VcpkgEvidence.checkout_head
            builtin_baseline = [string]$VcpkgEvidence.builtin_baseline
            toolchain_file = [string]$VcpkgEvidence.toolchain_file
            toolchain_file_sha256 =
                [string]$VcpkgEvidence.toolchain_file_sha256
        }
    }
}

function Assert-G16LiveToolchainSnapshot {
    param(
        [string]$ToolchainStatePath,
        [string]$ExpectedToolchainStateSha256,
        $ExpectedIdentity,
        [switch]$SkipPathSelection)
    Assert-Exact (Get-FileSha256 $ToolchainStatePath) `
        $ExpectedToolchainStateSha256 `
        'live toolchain-state SHA-256 before helper invocation'
    $liveState = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $ToolchainStatePath)
    $liveCmake = Resolve-StateTool 'cmake.exe' 'cmake'
    $liveCtest = Resolve-StateTool 'ctest.exe' 'ctest'
    $liveCompiler = Resolve-StateTool 'cl.exe' 'cl'
    $liveLinker = Resolve-StateTool 'link.exe' 'link'
    $liveMsbuild = Resolve-StateTool 'MSBuild.exe' 'msbuild'
    $liveGit = Resolve-StateTool 'git.exe' 'git'
    if (-not $SkipPathSelection) {
        Assert-PathToolIdentity 'cmake.exe' $liveCmake
        Assert-PathToolIdentity 'ctest.exe' $liveCtest
    }
    $liveVcpkg = Get-VcpkgEvidence `
        -ToolchainState $liveState `
        -GitPath $liveGit
    $liveIdentity = New-G16ToolchainIdentity `
        -ToolchainStateSha256 $ExpectedToolchainStateSha256 `
        -CmakePath $liveCmake `
        -CtestPath $liveCtest `
        -CompilerPath $liveCompiler `
        -LinkerPath $liveLinker `
        -MsbuildPath $liveMsbuild `
        -GitPath $liveGit `
        -VcpkgEvidence $liveVcpkg
    Assert-JsonObjectExact $ExpectedIdentity $liveIdentity `
        'live helper toolchain identity'
    Assert-Exact (Get-FileSha256 $ToolchainStatePath) `
        $ExpectedToolchainStateSha256 `
        'live toolchain-state SHA-256 after helper preflight'
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
        $length = [long](Get-Item -LiteralPath $fullPath).Length
        $bytes += $length
        $rows.Add($path + [char]9 + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join [char]10)
    }
}

function Invoke-RepositoryIntegrityChecks {
    Assert-True (-not [string]::IsNullOrWhiteSpace($script:GitTool)) `
        'repository integrity uses the selected Git executable'
    $output = @(& $script:GitTool -c core.safecrlf=false -C $WorkspaceRoot `
        diff --check 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('git diff --check: ' + ($output -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

function Test-BuildInputPath {
    param([string]$Path)
    return $Path.Replace('\', '/') -match $script:BuildInputPattern
}

function Assert-TrackedBuildInputsClean {
    $working = @(& $script:GitTool -C $WorkspaceRoot diff --name-only --)
    Assert-Exact $LASTEXITCODE 0 'tracked working-tree inventory exit code'
    $workingInputs = @($working | Where-Object { Test-BuildInputPath $_ })
    Assert-Exact $workingInputs.Count 0 `
        ('tracked build inputs are clean: ' + ($workingInputs -join ', '))

    $staged = @(& $script:GitTool -C $WorkspaceRoot diff --cached --name-only --)
    Assert-Exact $LASTEXITCODE 0 'tracked index inventory exit code'
    $stagedInputs = @($staged | Where-Object { Test-BuildInputPath $_ })
    Assert-Exact $stagedInputs.Count 0 `
        ('staged build inputs are clean: ' + ($stagedInputs -join ', '))
}

function Assert-NoUntrackedBuildInputs {
    $untracked = @(& $script:GitTool -C $WorkspaceRoot ls-files `
        --others --exclude-standard --)
    Assert-Exact $LASTEXITCODE 0 'untracked-file inventory exit code'
    $inputs = @($untracked | Where-Object { Test-BuildInputPath $_ })
    Assert-Exact $inputs.Count 0 `
        ('untracked source or build-input paths: ' + ($inputs -join ', '))
}

function Get-TrackedSourceFingerprint {
    $tracked = @(& $script:GitTool -C $WorkspaceRoot ls-files --)
    Assert-Exact $LASTEXITCODE 0 'tracked source inventory exit code'
    $excludedLedgerList = [Collections.Generic.List[string]]::new()
    foreach ($path in @($tracked | Where-Object {
                $_.Replace('\', '/') -cmatch
                    $script:MutableParityLedgerPattern
            })) {
        $excludedLedgerList.Add([string]$path)
    }
    $excludedLedgerList.Sort([StringComparer]::Ordinal)
    $excludedLedgers = @($excludedLedgerList)
    Assert-Set $excludedLedgers @(
        '.forge-codex/instructions/plans/feature-parity-matrix.json',
        '.forge-codex/instructions/plans/feature-parity-matrix.tsv'
    ) 'exact mutable parity-ledger fingerprint exclusions'
    $pathList = [Collections.Generic.List[string]]::new()
    foreach ($path in @($tracked | Where-Object {
                (Test-BuildInputPath $_) -and
                $_.Replace('\', '/') -cnotmatch
                    $script:MutableParityLedgerPattern
            })) {
        $pathList.Add([string]$path)
    }
    $pathList.Sort([StringComparer]::Ordinal)
    $paths = @($pathList)
    Assert-True ($paths.Count -gt 0) 'tracked source fingerprint is nonempty'
    $rows = [Collections.Generic.List[string]]::new()
    $bytes = 0L
    foreach ($path in $paths) {
        $fullPath = Join-Path $WorkspaceRoot $path.Replace('/', '\')
        Assert-True (Test-Path -LiteralPath $fullPath -PathType Leaf) `
            "tracked source fingerprint path exists: $path"
        $length = [long](Get-Item -LiteralPath $fullPath).Length
        $bytes += $length
        $rows.Add($path + [char]9 + $length + [char]9 +
            (Get-FileSha256 $fullPath))
    }
    $head = (& $script:GitTool -C $WorkspaceRoot rev-parse HEAD).Trim()
    Assert-Exact $LASTEXITCODE 0 'git HEAD resolution exit code'
    Assert-Match $head '^[0-9a-f]{40}$' 'git HEAD shape' -CaseSensitive
    return [ordered]@{
        git_head = $head
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join [char]10)
        excluded_mutable_governance_ledgers = $excludedLedgers
    }
}

function Assert-SourceFingerprint {
    param($Actual, $Expected, [string]$Message)
    Assert-Match ([string]$Actual.git_head) '^[0-9a-f]{40}$' `
        "$Message current Git HEAD shape" -CaseSensitive
    Assert-Match ([string]$Expected.git_head) '^[0-9a-f]{40}$' `
        "$Message recorded Git HEAD shape" -CaseSensitive
    Assert-Exact ([int]$Actual.files) ([int]$Expected.files) `
        "$Message file count"
    Assert-Exact ([long]$Actual.bytes) ([long]$Expected.bytes) `
        "$Message byte count"
    Assert-Exact ([string]$Actual.sha256) ([string]$Expected.sha256) `
        "$Message aggregate SHA-256"
    Assert-Set @($Actual.excluded_mutable_governance_ledgers) `
        @($Expected.excluded_mutable_governance_ledgers) `
        "$Message mutable-governance-ledger exclusions"
}

function Get-CmakeTestLabels {
    param([string]$CmakeText)
    $labelsByTest = @{}
    $options = [Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    foreach ($match in [regex]::Matches(
            $CmakeText,
            'set_tests_properties\s*\((?<body>.*?)\)',
            $options)) {
        $parts = [regex]::Match(
            $match.Groups['body'].Value,
            '^(?<names>.*?)\bPROPERTIES\b(?<properties>.*)$',
            $options)
        if (-not $parts.Success) { continue }
        $label = [regex]::Match(
            $parts.Groups['properties'].Value,
            '\bLABELS\s+"(?<value>[^"]*)"',
            $options)
        if (-not $label.Success) { continue }
        $labels = @($label.Groups['value'].Value.Split(
            ';', [StringSplitOptions]::RemoveEmptyEntries))
        $names = @([regex]::Matches(
            $parts.Groups['names'].Value,
            '[A-Za-z0-9_.+-]+') | ForEach-Object { $_.Value })
        foreach ($name in $names) {
            Assert-True (-not $labelsByTest.ContainsKey($name)) `
                "CTest labels are declared once for $name"
            $labelsByTest[$name] = $labels
        }
    }
    return $labelsByTest
}

function Assert-CmakeTestCommandTargets {
    param([string]$CmakeText)
    foreach ($name in $script:ExpectedTests) {
        $pattern = '(?ms)add_test\s*\(\s*NAME\s+' +
            [regex]::Escape($name) + '\s+COMMAND(?<command>.*?)\)'
        $blocks = [regex]::Matches($CmakeText, $pattern)
        Assert-Exact $blocks.Count 1 `
            "single exact add_test command block: $name"
        $commandText = $blocks[0].Groups['command'].Value
        $targetMatches = [regex]::Matches(
            $commandText,
            '[$]<TARGET_FILE:(?<target>[A-Za-z0-9_.+-]+)>')
        $actualTargets = @($targetMatches | ForEach-Object {
                $_.Groups['target'].Value
            })
        $expectedTargets = @($name)
        if ($script:ExpectedTestArgumentTargets.ContainsKey($name)) {
            $expectedTargets += @($script:ExpectedTestArgumentTargets[$name])
        }
        Assert-Set $actualTargets $expectedTargets `
            "add_test target set: $name"
        Assert-Exact ($actualTargets -join [char]9) `
            ($expectedTargets -join [char]9) `
            "add_test ordered target sequence: $name"
        $residue = $commandText
        foreach ($targetMatch in $targetMatches) {
            $residue = $residue.Replace($targetMatch.Value, '')
        }
        $residue = $residue.Replace('"', '').Replace("'", '')
        Assert-True ([string]::IsNullOrWhiteSpace($residue)) `
            "add_test command has no unpinned arguments: $name"
    }
}

function Get-CtestPropertyValues {
    param($Test, [string]$Name)
    $property = @($Test.properties | Where-Object name -CEQ $Name)
    Assert-Exact $property.Count 1 `
        "CTest $($Test.name) has one $Name property"
    return @($property[0].value | ForEach-Object { [string]$_ })
}

function Get-CmakeCacheValue {
    param([string]$Text, [string]$Name)
    $matches = [regex]::Matches(
        $Text,
        '(?m)^' + [regex]::Escape($Name) +
            ':[A-Za-z_]+=(?<value>[^\r\n]*)$')
    Assert-Exact $matches.Count 1 "single CMake cache value: $Name"
    return $matches[0].Groups['value'].Value
}

function Get-CmakeCacheEvidence {
    param([string]$Path, [string]$ExpectedLinker, $ToolchainState)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) `
        'post-build CMake cache exists'
    $item = Get-Item -Force -LiteralPath $Path
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'post-build CMake cache is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 4MB) `
        'post-build CMake cache has bounded bytes'
    $text = (Get-Content -Raw -LiteralPath $Path).
        Replace("`r`n", "`n").Replace("`r", "`n")
    $generator = Get-CmakeCacheValue $text 'CMAKE_GENERATOR'
    $generatorInstance = Get-CmakeCacheValue `
        $text 'CMAKE_GENERATOR_INSTANCE'
    $generatorPlatform = Get-CmakeCacheValue `
        $text 'CMAKE_GENERATOR_PLATFORM'
    $generatorToolset = Get-CmakeCacheValue `
        $text 'CMAKE_GENERATOR_TOOLSET'
    $systemVersion = Get-CmakeCacheValue $text 'CMAKE_SYSTEM_VERSION'
    $hostTriplet = Get-CmakeCacheValue $text 'VCPKG_HOST_TRIPLET'
    $targetTriplet = Get-CmakeCacheValue $text 'VCPKG_TARGET_TRIPLET'
    $cacheToolchainFile = Get-CmakeCacheValue $text 'CMAKE_TOOLCHAIN_FILE'
    $cacheLinker = Get-CmakeCacheValue $text 'CMAKE_LINKER'
    $selectedGeneratorInstance =
        [string]$ToolchainState.visual_studio.resolvedInstallationPath
    $selectedGeneratorInstance = $selectedGeneratorInstance.TrimEnd('\')

    Assert-Exact $generator 'Visual Studio 17 2022' `
        'post-build CMake generator'
    Assert-Exact $generatorPlatform 'x64' `
        'post-build CMake generator platform'
    Assert-Exact $generatorToolset 'v143' `
        'post-build CMake generator toolset'
    Assert-Exact $systemVersion '10.0.26100.0' `
        'post-build Windows SDK system version'
    Assert-Exact $hostTriplet 'x64-windows' `
        'post-build vcpkg host triplet'
    Assert-Exact $targetTriplet 'x64-windows' `
        'post-build vcpkg target triplet'
    Assert-True ([string]::Equals(
        $generatorInstance.Replace('/', '\').TrimEnd('\'),
        $selectedGeneratorInstance,
        [StringComparison]::OrdinalIgnoreCase)) `
        'post-build CMake generator instance is the selected BuildTools instance'
    Assert-True ([string]::Equals(
        $cacheLinker.Replace('/', '\'),
        $ExpectedLinker,
        [StringComparison]::OrdinalIgnoreCase)) `
        'post-build CMake linker is the selected 14.44 x64 linker'
    $selectedToolchainFile = (Resolve-Path -LiteralPath (Join-Path `
        ([string]$ToolchainState.vcpkg.root) `
        'scripts\buildsystems\vcpkg.cmake')).Path
    Assert-True ([string]::Equals(
        $cacheToolchainFile.Replace('/', '\'),
        $selectedToolchainFile,
        [StringComparison]::OrdinalIgnoreCase)) `
        'post-build CMake toolchain file is the selected vcpkg checkout'

    return [ordered]@{
        path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $Path).Replace('\', '/')
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $Path
        generator = $generator
        generator_instance = $generatorInstance
        generator_platform = $generatorPlatform
        generator_toolset = $generatorToolset
        system_version = $systemVersion
        vcpkg_host_triplet = $hostTriplet
        vcpkg_target_triplet = $targetTriplet
        toolchain_file = $cacheToolchainFile
        toolchain_file_sha256 = Get-FileSha256 $selectedToolchainFile
        linker = $cacheLinker
    }
}

function Get-JsonObjectSha256 {
    param($Value)
    $json = $Value | ConvertTo-Json -Depth 100 -Compress
    return Get-StringSha256 $json
}

function Assert-JsonObjectExact {
    param($Actual, $Expected, [string]$Message)
    Assert-Exact (Get-JsonObjectSha256 $Actual) `
        (Get-JsonObjectSha256 $Expected) "$Message object SHA-256"
}

function Get-CmakeCompilerEvidence {
    param(
        [string]$BuildRoot,
        [string]$ExpectedCompiler,
        [string]$ExpectedLinker,
        $ToolchainState)
    $path = Join-Path $BuildRoot (
        'CMakeFiles\' + [string]$ToolchainState.tool_versions.cmake +
        '\CMakeCXXCompiler.cmake')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        'configured CMake C++ compiler evidence exists'
    $item = Get-Item -Force -LiteralPath $path
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'configured CMake C++ compiler evidence is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 1MB) `
        'configured CMake C++ compiler evidence has bounded bytes'
    $text = (Get-Content -Raw -LiteralPath $path).
        Replace("`r`n", "`n").Replace("`r", "`n")
    $values = [ordered]@{}
    foreach ($name in @(
            'CMAKE_CXX_COMPILER',
            'CMAKE_CXX_COMPILER_ID',
            'CMAKE_CXX_COMPILER_VERSION',
            'CMAKE_CXX_COMPILER_ARCHITECTURE_ID',
            'CMAKE_LINKER')) {
        $matches = [regex]::Matches(
            $text,
            '(?m)^set\(' + [regex]::Escape($name) +
                ' "(?<value>[^"\r\n]*)"\)$')
        Assert-Exact $matches.Count 1 `
            "single configured CMake compiler value: $name"
        $values[$name] = $matches[0].Groups['value'].Value
    }
    Assert-True ([string]::Equals(
        ([string]$values.CMAKE_CXX_COMPILER).Replace('/', '\'),
        $ExpectedCompiler,
        [StringComparison]::OrdinalIgnoreCase)) `
        'configured CMake C++ compiler is the selected compiler'
    Assert-Exact ([string]$values.CMAKE_CXX_COMPILER_ID) 'MSVC' `
        'configured CMake C++ compiler identity'
    Assert-Exact ([string]$values.CMAKE_CXX_COMPILER_VERSION) `
        ([string]$ToolchainState.tool_versions.cl) `
        'configured CMake C++ compiler version'
    Assert-Exact ([string]$values.CMAKE_CXX_COMPILER_ARCHITECTURE_ID) `
        'x64' 'configured CMake C++ compiler architecture'
    Assert-True ([string]::Equals(
        ([string]$values.CMAKE_LINKER).Replace('/', '\'),
        $ExpectedLinker,
        [StringComparison]::OrdinalIgnoreCase)) `
        'configured CMake linker is the selected linker'

    return [ordered]@{
        path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $path).Replace('\', '/')
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $path
        compiler = [string]$values.CMAKE_CXX_COMPILER
        compiler_id = [string]$values.CMAKE_CXX_COMPILER_ID
        compiler_version = [string]$values.CMAKE_CXX_COMPILER_VERSION
        architecture = [string]$values.CMAKE_CXX_COMPILER_ARCHITECTURE_ID
        linker = [string]$values.CMAKE_LINKER
    }
}

function Get-RcDataResources {
    param([string]$Path)
    if (-not ('ForgeConductor.Validation.RcDataReader' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;

namespace ForgeConductor.Validation
{
    public static class RcDataReader
    {
        private const uint LoadLibraryAsImageResource = 0x00000020;
        private static readonly IntPtr RcDataType = new IntPtr(10);

        private delegate bool EnumResourceNameProcedure(
            IntPtr module,
            IntPtr type,
            IntPtr name,
            IntPtr parameter);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern IntPtr LoadLibraryExW(
            string fileName,
            IntPtr file,
            uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FreeLibrary(IntPtr module);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern bool EnumResourceNamesW(
            IntPtr module,
            IntPtr type,
            EnumResourceNameProcedure procedure,
            IntPtr parameter);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern IntPtr FindResourceW(
            IntPtr module,
            IntPtr name,
            IntPtr type);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(
            IntPtr module,
            IntPtr resource);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(
            IntPtr module,
            IntPtr resource);

        [DllImport("kernel32.dll")]
        private static extern IntPtr LockResource(IntPtr resourceData);

        public static Dictionary<int, byte[]> ReadAll(string path)
        {
            IntPtr module = LoadLibraryExW(
                path,
                IntPtr.Zero,
                LoadLibraryAsImageResource);
            if (module == IntPtr.Zero)
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "The PE image could not be opened as a resource image.");
            }

            try
            {
                List<int> identifiers = new List<int>();
                bool nonIntegerName = false;
                EnumResourceNameProcedure callback = delegate(
                    IntPtr callbackModule,
                    IntPtr callbackType,
                    IntPtr name,
                    IntPtr parameter)
                {
                    ulong raw = unchecked((ulong)name.ToInt64());
                    if ((raw >> 16) != 0)
                    {
                        nonIntegerName = true;
                        return false;
                    }
                    identifiers.Add((int)(raw & 0xffff));
                    return true;
                };

                bool enumerated = EnumResourceNamesW(
                    module,
                    RcDataType,
                    callback,
                    IntPtr.Zero);
                GC.KeepAlive(callback);
                if (nonIntegerName)
                {
                    throw new InvalidDataException(
                        "The image contains a named RCDATA resource.");
                }
                if (!enumerated)
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "RCDATA enumeration failed.");
                }

                Dictionary<int, byte[]> result =
                    new Dictionary<int, byte[]>();
                foreach (int identifier in identifiers)
                {
                    IntPtr resource = FindResourceW(
                        module,
                        new IntPtr(identifier),
                        RcDataType);
                    if (resource == IntPtr.Zero)
                    {
                        throw new Win32Exception(
                            Marshal.GetLastWin32Error(),
                            "An enumerated RCDATA resource is missing.");
                    }
                    uint size = SizeofResource(module, resource);
                    if (size == 0)
                    {
                        throw new InvalidDataException(
                            "An RCDATA resource is empty.");
                    }
                    IntPtr loaded = LoadResource(module, resource);
                    if (loaded == IntPtr.Zero)
                    {
                        throw new Win32Exception(
                            Marshal.GetLastWin32Error(),
                            "An RCDATA resource could not be loaded.");
                    }
                    IntPtr bytes = LockResource(loaded);
                    if (bytes == IntPtr.Zero)
                    {
                        throw new InvalidDataException(
                            "An RCDATA resource could not be locked.");
                    }
                    byte[] copy = new byte[checked((int)size)];
                    Marshal.Copy(bytes, copy, 0, copy.Length);
                    result.Add(identifier, copy);
                }
                return result;
            }
            finally
            {
                if (!FreeLibrary(module))
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "The PE resource image could not be released.");
                }
            }
        }
    }
}
'@
    }

    $resources = [ForgeConductor.Validation.RcDataReader]::ReadAll(
        (Resolve-Path -LiteralPath $Path).Path)
    return [pscustomobject]@{ Items = $resources }
}

function Get-EmbeddedResourceEvidence {
    param([string]$ManagerPath)
    $read = Get-RcDataResources $ManagerPath
    $actualIdentifiers = @($read.Items.Keys | ForEach-Object { [int]$_ })
    Assert-Set $actualIdentifiers @($script:ExpectedResources.id) `
        'production Manager exact embedded RCDATA identifiers'

    $resourceRoot = Join-Path $WorkspaceRoot `
        'src\Hosts\Manager\Resources\Dashboard'
    $evidence = [Collections.Generic.List[object]]::new()
    foreach ($expected in $script:ExpectedResources) {
        $sourcePath = Join-Path $resourceRoot ([string]$expected.file)
        Assert-True (Test-Path -LiteralPath $sourcePath -PathType Leaf) `
            "dashboard source asset exists: $($expected.file)"
        $sourceBytes = [IO.File]::ReadAllBytes($sourcePath)
        $embeddedBytes = [byte[]]$read.Items[[int]$expected.id]
        Assert-True ($embeddedBytes.Length -gt 0) `
            "embedded dashboard resource $($expected.id) is nonempty"
        Assert-Exact $embeddedBytes.Length $sourceBytes.Length `
            "embedded dashboard resource $($expected.id) byte count"
        $sourceHash = Get-BytesSha256 $sourceBytes
        $embeddedHash = Get-BytesSha256 $embeddedBytes
        Assert-Exact $embeddedHash $sourceHash `
            "embedded dashboard resource $($expected.id) SHA-256"
        $evidence.Add([ordered]@{
            id = [int]$expected.id
            file = [string]$expected.file
            route = [string]$expected.route
            bytes = $embeddedBytes.Length
            source_sha256 = $sourceHash
            embedded_sha256 = $embeddedHash
        })
    }
    return @($evidence)
}

function Get-ProductionBinaryPolicyEvidence {
    param([string]$Path, [string]$Linker)
    $item = Get-Item -LiteralPath $Path
    Assert-True ($item.Length -gt 0 -and $item.Length -le 256MB) `
        "production artifact has bounded bytes: $($item.Name)"
    $dependencyText = @(& $Linker /dump /dependents $Path 2>&1) -join "`n"
    Assert-Exact $LASTEXITCODE 0 `
        "linker dependency inspection exit code: $($item.Name)"

    $languageStem = 'py' + 'thon'
    $languageImagePattern = '(?i)\b' + $languageStem +
        '(?:3(?:[.][0-9]+)?)?[.](?:exe|dll)\b'
    $languageModulePattern = '(?i)\b' + $languageStem +
        '(?:3(?:[.][0-9]+)?)?\s+-m\b'
    Assert-NoMatch $dependencyText $languageImagePattern `
        "production dependency list excludes the forbidden runtime: $($item.Name)"

    $bytes = [IO.File]::ReadAllBytes($Path)
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [Text.Encoding]::Unicode.GetString($bytes)
    $unicodeOdd = if ($bytes.Length -gt 1) {
        [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
    } else {
        ''
    }
    foreach ($text in @($ascii, $unicode, $unicodeOdd)) {
        Assert-NoMatch $text $languageImagePattern `
            "production artifact excludes forbidden runtime images: $($item.Name)"
        Assert-NoMatch $text $languageModulePattern `
            "production artifact excludes forbidden module launch: $($item.Name)"
    }

    $aiWord = 'A' + 'I'
    $generatedWord = 'gen' + 'erated'
    $createdWord = 'cre' + 'ated'
    $authorshipWord = 'author' + 'ship'
    $automatedWord = 'auto' + 'mated'
    $vendorWords = '(?:Chat' + 'GPT|Open' + 'AI|Cod' + 'ex|' +
        'an ' + $aiWord + '|a model)'
    $attributionPatterns = @(
        '(?i)\b' + $aiWord + '[- ]' + $generatedWord + '\b',
        '(?i)\b' + $generatedWord + ' by ' + $vendorWords + '\b',
        '(?i)\b' + $createdWord + ' (?:with|by) ' +
            '(?:Chat' + 'GPT|Open' + 'AI|Cod' + 'ex|' + $aiWord + ')\b',
        '(?i)\b' + $automatedWord + ' ' + $authorshipWord + '\b'
    )
    foreach ($pattern in $attributionPatterns) {
        Assert-NoMatch $ascii $pattern `
            "production artifact ASCII attribution exclusion: $($item.Name)"
        Assert-NoMatch $unicode $pattern `
            "production artifact aligned UTF-16 attribution exclusion: $($item.Name)"
        Assert-NoMatch $unicodeOdd $pattern `
            "production artifact unaligned UTF-16 attribution exclusion: $($item.Name)"
    }

    $dependencyNames = @([regex]::Matches(
        $dependencyText,
        '(?im)^\s*(?<name>[A-Za-z0-9_.+-]+[.]dll)\s*$') |
        ForEach-Object { $_.Groups['name'].Value.ToLowerInvariant() } |
        Sort-Object -Unique -CaseSensitive)
    Assert-True ($dependencyNames.Count -gt 0) `
        "production dependency inventory is nonempty: $($item.Name)"
    return [ordered]@{
        dependencies = $dependencyNames
        dependency_inventory_sha256 = Get-StringSha256 `
            ($dependencyNames -join [char]10)
        forbidden_runtime_scan = 'passed'
        attribution_scan = 'passed'
    }
}

function Get-ArtifactEvidence {
    param([string]$Path, [string]$Linker, [switch]$ProductionPolicy)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) `
        "artifact exists: $Path"
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    Assert-X64PortableExecutable $resolved "artifact $resolved"
    $item = Get-Item -LiteralPath $resolved
    $evidence = [ordered]@{
        path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $resolved).Replace('\', '/')
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $resolved
    }
    if ($ProductionPolicy) {
        $evidence['policy'] = Get-ProductionBinaryPolicyEvidence `
            -Path $resolved -Linker $Linker
    }
    return $evidence
}

function Get-CtestArtifactEvidence {
    param($Inventory)
    $buildRoot = (Resolve-Path -LiteralPath (Join-Path $WorkspaceRoot `
        'out\build\windows-msvc-x64')).Path
    $binRoot = (Resolve-Path -LiteralPath (Join-Path $WorkspaceRoot `
        'out\build\windows-msvc-x64\bin\Release')).Path
    $results = [Collections.Generic.List[object]]::new()
    foreach ($name in $script:ExpectedTests) {
        $test = @($Inventory.tests | Where-Object name -CEQ $name)
        Assert-Exact $test.Count 1 "single CTest inventory entry: $name"
        Assert-True ($null -ne $test[0].PSObject.Properties['command']) `
            "CTest inventory exposes the built command: $name"
        $command = @($test[0].command)
        Assert-True ($command.Count -ge 1) "CTest command is nonempty: $name"
        $expectedCommand = @()
        if ($name -ceq
            'ForgeConductor.Manager.ProcessEnvironmentRealTests') {
            $expectedCommand += Join-Path $buildRoot `
                'tests\manager-process-environment-real\Release\ForgeConductor.Manager.exe'
        } else {
            $expectedCommand += Join-Path $binRoot ($name + '.exe')
        }
        if ($script:ExpectedTestArgumentTargets.ContainsKey($name)) {
            foreach ($target in $script:ExpectedTestArgumentTargets[$name]) {
                switch -CaseSensitive ($target) {
                    'ForgeConductor.Cli' {
                        $expectedCommand += Join-Path $binRoot 'forge-conductor.exe'
                    }
                    'ForgeConductor.Manager.CompositionFixture' {
                        $expectedCommand += Join-Path $buildRoot `
                            'tests\manager-composition-fixture\Release\ForgeConductor.Manager.exe'
                    }
                    'ForgeConductor.Manager.StartupLifecycleFixture' {
                        $expectedCommand += Join-Path $binRoot `
                            'ForgeConductor.Manager.StartupLifecycleFixture.exe'
                    }
                    default {
                        throw "G16 has no output-path mapping for target $target."
                    }
                }
            }
        }
        Assert-Exact $command.Count $expectedCommand.Count `
            "CTest exact command element count: $name"
        $resolvedCommand = [Collections.Generic.List[string]]::new()
        for ($index = 0; $index -lt $command.Count; $index++) {
            $element = [string]$command[$index]
            Assert-True (Test-Path -LiteralPath $element -PathType Leaf) `
                "CTest command element exists: $name index $index"
            $resolvedElement = (Resolve-Path -LiteralPath $element).Path
            Assert-True ($resolvedElement.StartsWith(
                $buildRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) `
                "CTest command element remains inside the x64 build root: $name index $index"
            Assert-True ([string]::Equals(
                $resolvedElement,
                [IO.Path]::GetFullPath([string]$expectedCommand[$index]),
                [StringComparison]::OrdinalIgnoreCase)) `
                "CTest exact command target path: $name index $index"
            Assert-X64PortableExecutable $resolvedElement `
                "CTest command artifact $name index $index"
            $resolvedCommand.Add($resolvedElement)
        }
        $resolved = $resolvedCommand[0]
        $labels = Get-CtestPropertyValues $test[0] 'LABELS'
        Assert-True ($labels -ccontains 'G16') "$name carries G16"
        if ($name.StartsWith(
                'ForgeConductor.Manager.',
                [StringComparison]::Ordinal)) {
            Assert-True ($labels -ccontains 'T-MGR') "$name carries T-MGR"
        }
        $item = Get-Item -LiteralPath $resolved
        $commandEvidence = @($resolvedCommand | ForEach-Object {
                $commandItem = Get-Item -LiteralPath $_
                [ordered]@{
                    path = (Get-RelativePathPortable `
                        -BasePath $WorkspaceRoot `
                        -TargetPath $_).Replace('\', '/')
                    bytes = [long]$commandItem.Length
                    sha256 = Get-FileSha256 $_
                }
            })
        $results.Add([ordered]@{
            test = $name
            path = (Get-RelativePathPortable `
                -BasePath $WorkspaceRoot -TargetPath $resolved).Replace('\', '/')
            bytes = [long]$item.Length
            sha256 = Get-FileSha256 $resolved
            command = $commandEvidence
            command_sha256 = Get-JsonObjectSha256 $commandEvidence
            labels = @($labels | Sort-Object -CaseSensitive)
        })
    }
    $lifecycle = @($Inventory.tests | Where-Object {
            [string]$_.name -ceq
                'ForgeConductor.Manager.CompositionLifecycleTests'
        })
    Assert-Exact $lifecycle.Count 1 'controlled composition lifecycle CTest exists'
    $runSerial = Get-CtestPropertyValues $lifecycle[0] 'RUN_SERIAL'
    Assert-Set $runSerial @('True') `
        'controlled composition lifecycle CTest runs serially'
    return @($results)
}

function Get-G16CtestInventory {
    param([string]$CtestPath)
    Push-Location $WorkspaceRoot
    try {
        $inventoryText = @(& $CtestPath `
            --preset windows-msvc-x64-release `
            --show-only=json-v1 `
            --no-tests=error `
            --label-regex G16 2>&1) -join "`n"
        $inventoryExitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    Assert-Exact $inventoryExitCode 0 'G16 CTest inventory query exit code'
    $inventory = ConvertFrom-JsonPreserveDates -Value $inventoryText
    Assert-Set @($inventory.tests | ForEach-Object name) `
        $script:ExpectedTests 'runtime exact G16 CTest inventory'
    Assert-Exact @($inventory.tests).Count 83 `
        'runtime G16 CTest inventory count'
    return $inventory
}

function Get-G16CtestInventoryPreservingLastTest {
    param(
        [string]$CtestPath,
        [string]$LastTestPath)
    $priorBytes = $null
    if (Test-Path -LiteralPath $LastTestPath -PathType Leaf) {
        $priorItem = Get-Item -Force -LiteralPath $LastTestPath
        Assert-True (($priorItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'pre-inventory LastTest.log is not a reparse point'
        Assert-True ($priorItem.Length -le 64MB) `
            'pre-inventory LastTest.log has bounded bytes'
        $priorBytes = [IO.File]::ReadAllBytes($LastTestPath)
    }
    try {
        return Get-G16CtestInventory $CtestPath
    }
    finally {
        if ($null -ne $priorBytes) {
            $directory = Split-Path -Parent $LastTestPath
            $temporary = Join-Path $directory `
                ('LastTest.restore.' + $PID + '.' +
                    [Guid]::NewGuid().ToString('N') + '.log')
            try {
                [IO.File]::WriteAllBytes($temporary, $priorBytes)
                Move-Item -LiteralPath $temporary `
                    -Destination $LastTestPath -Force
            }
            finally {
                if (Test-Path -LiteralPath $temporary) {
                    Remove-Item -LiteralPath $temporary
                }
            }
            Assert-Exact (Get-FileSha256 $LastTestPath) `
                (Get-BytesSha256 $priorBytes) `
                'CTest inventory preserves the prior LastTest.log exactly'
        } elseif (Test-Path -LiteralPath $LastTestPath -PathType Leaf) {
            Remove-Item -LiteralPath $LastTestPath
            Assert-True (-not (Test-Path -LiteralPath $LastTestPath)) `
                'CTest inventory removes its zero-test LastTest.log side effect'
        }
    }
}

function Assert-TestArtifactEvidenceCurrent {
    param([object[]]$Artifacts, [string]$Message)
    Assert-Exact @($Artifacts).Count 83 "$Message artifact count"
    Assert-Set @($Artifacts | ForEach-Object test) `
        $script:ExpectedTests "$Message artifact names"
    foreach ($recorded in $Artifacts) {
        $artifactPath = Join-Path $WorkspaceRoot `
            ([string]$recorded.path).Replace('/', '\')
        Assert-True (Test-Path -LiteralPath $artifactPath -PathType Leaf) `
            "$Message artifact exists: $($recorded.test)"
        Assert-X64PortableExecutable $artifactPath `
            "$Message artifact $($recorded.test)"
        Assert-Exact ([long](Get-Item -LiteralPath $artifactPath).Length) `
            ([long]$recorded.bytes) `
            "$Message artifact bytes: $($recorded.test)"
        Assert-Exact (Get-FileSha256 $artifactPath) `
            ([string]$recorded.sha256) `
            "$Message artifact SHA-256: $($recorded.test)"
        Assert-True ($null -ne $recorded.PSObject.Properties['command']) `
            "$Message command evidence exists: $($recorded.test)"
        Assert-True (@($recorded.command).Count -ge 1) `
            "$Message command evidence is nonempty: $($recorded.test)"
        foreach ($commandElement in @($recorded.command)) {
            $commandPath = Join-Path $WorkspaceRoot `
                ([string]$commandElement.path).Replace('/', '\')
            Assert-True (Test-Path -LiteralPath $commandPath -PathType Leaf) `
                "$Message command element exists: $($recorded.test)"
            Assert-Exact ([long](Get-Item -LiteralPath $commandPath).Length) `
                ([long]$commandElement.bytes) `
                "$Message command element bytes: $($recorded.test)"
            Assert-Exact (Get-FileSha256 $commandPath) `
                ([string]$commandElement.sha256) `
                "$Message command element SHA-256: $($recorded.test)"
        }
        Assert-Exact (Get-JsonObjectSha256 @($recorded.command)) `
            ([string]$recorded.command_sha256) `
            "$Message command evidence aggregate: $($recorded.test)"
    }
}

function Get-G16ProductionArtifactEvidence {
    param(
        [string]$ManagerPath,
        [string]$CliPath,
        [string]$FixturePath,
        [string]$FixtureCliPath,
        [string]$Linker)
    $artifacts = [ordered]@{
        manager = Get-ArtifactEvidence `
            -Path $ManagerPath -Linker $Linker -ProductionPolicy
        cli = Get-ArtifactEvidence `
            -Path $CliPath -Linker $Linker -ProductionPolicy
        composition_fixture = Get-ArtifactEvidence `
            -Path $FixturePath -Linker $Linker
        composition_fixture_cli = Get-ArtifactEvidence `
            -Path $FixtureCliPath -Linker $Linker
    }
    Assert-Exact ([long]$artifacts.composition_fixture_cli.bytes) `
        ([long]$artifacts.cli.bytes) `
        'staged fixture CLI byte length equals the canonical CLI'
    Assert-Exact ([string]$artifacts.composition_fixture_cli.sha256) `
        ([string]$artifacts.cli.sha256) `
        'staged fixture CLI SHA-256 equals the canonical CLI'
    return $artifacts
}

function Get-G16BuildEvidenceBundle {
    param(
        $Inventory,
        $CmakeCache,
        $CmakeCompiler,
        [string]$ManagerPath,
        [string]$CliPath,
        [string]$FixturePath,
        [string]$FixtureCliPath,
        [string]$Linker)
    $testArtifacts = Get-CtestArtifactEvidence $Inventory
    $productionArtifacts = Get-G16ProductionArtifactEvidence `
        -ManagerPath $ManagerPath `
        -CliPath $CliPath `
        -FixturePath $FixturePath `
        -FixtureCliPath $FixtureCliPath `
        -Linker $Linker
    $embeddedResources = Get-EmbeddedResourceEvidence $ManagerPath
    $bundle = [ordered]@{
        cmake_cache = $CmakeCache
        cmake_compiler = $CmakeCompiler
        test_artifacts = $testArtifacts
        production_artifacts = $productionArtifacts
        embedded_dashboard_resources = $embeddedResources
    }
    return [ordered]@{
        sha256 = Get-JsonObjectSha256 $bundle
        evidence = $bundle
    }
}

function Assert-G16BuildEvidenceBundleCurrent {
    param($Recorded, $Current, [string]$Message)
    Assert-Exact ([string]$Current.sha256) ([string]$Recorded.sha256) `
        "$Message aggregate SHA-256"
    Assert-JsonObjectExact $Current.evidence $Recorded.evidence `
        "$Message evidence"
}

function Get-LastTestLogEvidence {
    param([string]$Path)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) `
        'CTest LastTest.log exists'
    $item = Get-Item -Force -LiteralPath $Path
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'CTest LastTest.log is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 64MB) `
        'CTest LastTest.log has bounded bytes'
    $text = Get-Content -Raw -LiteralPath $Path
    $normalizedText = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    Assert-Match $normalizedText '(?m)^Start testing: ' 'CTest log start marker' `
        -CaseSensitive
    Assert-Match $normalizedText '(?m)^End testing: ' 'CTest log end marker' `
        -CaseSensitive
    Assert-NoMatch $normalizedText '(?m)^Test Failed[.]$' `
        'CTest log contains no failed test' -CaseSensitive
    $testingMatches = [regex]::Matches(
        $normalizedText,
        '(?m)^(?<ordinal>[1-9][0-9]*)/(?<total>[1-9][0-9]*) Testing: (?<name>[^\r\n]+)$')
    Assert-Exact $testingMatches.Count 83 'CTest log exact Testing-line count'
    $selectedOrdinals = @($testingMatches | ForEach-Object {
            [int]$_.Groups['ordinal'].Value
        })
    Assert-Exact @($selectedOrdinals | Sort-Object -Unique).Count 83 `
        'CTest log selected ordinals are unique'
    $registeredTotals = @($testingMatches | ForEach-Object {
            $_.Groups['total'].Value
        } | Sort-Object -Unique -CaseSensitive)
    Assert-Exact $registeredTotals.Count 1 `
        'CTest log uses one registered-test denominator'
    $registeredTotal = 0
    Assert-True ([int]::TryParse(
        $registeredTotals[0], [ref]$registeredTotal)) `
        'CTest log registered-test denominator is numeric'
    Assert-True ($registeredTotal -ge 83 -and $registeredTotal -le 10000) `
        'CTest log registered-test denominator is bounded and covers G16'
    foreach ($ordinal in $selectedOrdinals) {
        Assert-True ($ordinal -ge 1 -and $ordinal -le $registeredTotal) `
            "CTest log selected ordinal is within the registered inventory: $ordinal"
    }
    Assert-Set @($testingMatches | ForEach-Object {
            $_.Groups['name'].Value
        }) $script:ExpectedTests 'CTest log exact G16 execution inventory'
    Assert-Exact ([regex]::Matches(
        $normalizedText,
        '(?m)^Test Passed[.]$').Count) 83 `
        'CTest log exact passed-test count'
    foreach ($name in $script:ExpectedTests) {
        Assert-Exact ([regex]::Matches(
            $normalizedText,
            '(?m)^"' + [regex]::Escape($name) +
                '" time elapsed: ').Count) 1 `
            "CTest log completed exactly once: $name"
    }
    return [ordered]@{
        path = (Get-RelativePathPortable `
            -BasePath $WorkspaceRoot -TargetPath $Path).Replace('\', '/')
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $Path
        executed_tests = 83
        passed_tests = 83
    }
}

function Resolve-CanonicalPriorSummary {
    param([string]$Path)
    Assert-True (-not [string]::IsNullOrWhiteSpace($Path)) `
        'resume evidence path is supplied'
    Assert-True ([IO.Path]::IsPathRooted($Path)) `
        'resume evidence path is absolute'
    $fullPath = [IO.Path]::GetFullPath($Path)
    Assert-True ([string]::Equals(
        $Path,
        $fullPath,
        [StringComparison]::OrdinalIgnoreCase)) `
        'resume evidence path is canonical'
    $expected = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
        '.forge-codex\state\evidence\P16\g16-validation-summary.json'))
    Assert-True ([string]::Equals(
        $fullPath,
        $expected,
        [StringComparison]::OrdinalIgnoreCase)) `
        'resume evidence is the canonical P16/G16 summary'
    Assert-True (Test-Path -LiteralPath $fullPath -PathType Leaf) `
        'resume evidence exists'
    $resolved = (Resolve-Path -LiteralPath $fullPath).Path
    Assert-True ([string]::Equals(
        $resolved,
        $expected,
        [StringComparison]::OrdinalIgnoreCase)) `
        'resume evidence resolves without redirection'
    $item = Get-Item -Force -LiteralPath $resolved
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'resume evidence is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 4MB) `
        'resume evidence has bounded bytes'
    return $resolved
}

function New-G16ContextFingerprint {
    param($SourceFingerprint, $ToolchainIdentity)
    $stableSource = [ordered]@{
        files = [int]$SourceFingerprint.files
        bytes = [long]$SourceFingerprint.bytes
        sha256 = [string]$SourceFingerprint.sha256
        excluded_mutable_governance_ledgers =
            @($SourceFingerprint.excluded_mutable_governance_ledgers)
    }
    $evidence = [ordered]@{
        working_directory = $WorkspaceRoot
        source = $stableSource
        toolchain = $ToolchainIdentity
    }
    return [ordered]@{
        sha256 = Get-JsonObjectSha256 $evidence
        evidence = $evidence
    }
}

function Get-G16StableInvocationEvidence {
    param($InvocationEvidence)
    return [ordered]@{
        configuration = [string]$InvocationEvidence.configuration
        architecture = [string]$InvocationEvidence.architecture
        build_targets = @($InvocationEvidence.build_targets)
        ctest_preset = [string]$InvocationEvidence.ctest_preset
        ctest_label = [string]$InvocationEvidence.ctest_label
        build_script = $InvocationEvidence.build_script
        test_script = $InvocationEvidence.test_script
        gate_script = $InvocationEvidence.gate_script
        cmake = $InvocationEvidence.cmake
        ctest = $InvocationEvidence.ctest
    }
}

function Read-G16AttemptCheckpoint {
    param([string]$Path)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) `
        'G16 attempt checkpoint exists'
    $item = Get-Item -Force -LiteralPath $Path
    Assert-True (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 attempt checkpoint is not a reparse point'
    Assert-True ($item.Length -gt 0 -and $item.Length -le 16MB) `
        'G16 attempt checkpoint has bounded bytes'
    $attempt = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $Path)
    Assert-Exact ([int]$attempt.schema_version) 2 `
        'G16 attempt checkpoint schema version'
    Assert-Exact ([string]$attempt.phase) 'P16' `
        'G16 attempt checkpoint phase'
    Assert-Exact ([string]$attempt.gate) 'G16' `
        'G16 attempt checkpoint gate'
    Assert-Match ([string]$attempt.attempt_id) '^[0-9a-f]{32}$' `
        'G16 attempt identifier shape' -CaseSensitive
    Assert-Match ([string]$attempt.started_utc) `
        '^[0-9]{4}-[0-9]{2}-[0-9]{2}T' `
        'G16 attempt start timestamp' -CaseSensitive
    Assert-Match ([string]$attempt.updated_utc) `
        '^[0-9]{4}-[0-9]{2}-[0-9]{2}T' `
        'G16 attempt update timestamp' -CaseSensitive
    Assert-True (@(
            'prepared',
            'build_started',
            'build_passed',
            'test_started',
            'test_passed'
        ) -ccontains [string]$attempt.status) `
        'G16 attempt checkpoint status is recognized'
    Assert-Exact ([string]$attempt.context.sha256) `
        (Get-JsonObjectSha256 $attempt.context.evidence) `
        'G16 attempt context aggregate'
    Assert-Exact ([string]$attempt.invocation.sha256) `
        (Get-JsonObjectSha256 $attempt.invocation.evidence) `
        'G16 attempt invocation aggregate'
    if ($null -ne $attempt.predecessor) {
        Assert-Match ([string]$attempt.predecessor.attempt_id) `
            '^[0-9a-f]{32}$' 'G16 predecessor attempt identifier' `
            -CaseSensitive
        Assert-Match ([string]$attempt.predecessor.manifest_sha256) `
            '^[0-9a-f]{64}$' 'G16 predecessor manifest SHA-256' `
            -CaseSensitive
        Assert-Exact `
            ([string]$attempt.predecessor.actual_replacement_context_sha256) `
            ([string]$attempt.context.sha256) `
            'G16 predecessor replacement context'
    }

    $expectedBuildInvocations = if (
        [string]$attempt.status -ceq 'prepared') { 0 } else { 1 }
    $expectedTestInvocations = if (
        [string]$attempt.status -cin @('test_started', 'test_passed')) {
        1
    } else {
        0
    }
    Assert-Exact ([int]$attempt.build_invocations) $expectedBuildInvocations `
        'G16 attempt status/build-invocation invariant'
    Assert-Exact ([int]$attempt.test_invocations) $expectedTestInvocations `
        'G16 attempt status/test-invocation invariant'

    if ([string]$attempt.status -cin @(
            'build_passed', 'test_started', 'test_passed')) {
        Assert-True ($null -ne $attempt.build_evidence) `
            'G16 passed-build checkpoint includes build evidence'
        Assert-Exact ([string]$attempt.build_evidence.sha256) `
            (Get-JsonObjectSha256 $attempt.build_evidence.evidence) `
            'G16 attempt build-evidence aggregate'
    } else {
        Assert-True ($null -eq $attempt.build_evidence) `
            'G16 pre-pass build checkpoint has no build evidence'
    }
    if ([string]$attempt.status -cin @('test_started', 'test_passed')) {
        Assert-Match ([string]$attempt.test_started_utc) `
            '^[0-9]{4}-[0-9]{2}-[0-9]{2}T' `
            'G16 attempt test-start timestamp' -CaseSensitive
        Assert-True ($null -ne $attempt.prior_last_test_log) `
            'G16 test checkpoint records the prior LastTest disposition'
        Assert-True (@('absent', 'archived') -ccontains
            [string]$attempt.prior_last_test_log.status) `
            'G16 prior LastTest disposition is recognized'
        if ([string]$attempt.prior_last_test_log.status -ceq 'archived') {
            Assert-Match ([string]$attempt.prior_last_test_log.sha256) `
                '^[0-9a-f]{64}$' `
                'G16 archived prior LastTest SHA-256' -CaseSensitive
            $expectedPriorPath = Join-Path $WorkspaceRoot `
                ('.forge-codex\state\evidence\P16\g16-attempt-history\' +
                    [string]$attempt.attempt_id + '-pretest-LastTest.log')
            $recordedPriorPath = Join-Path $WorkspaceRoot `
                ([string]$attempt.prior_last_test_log.path).Replace('/', '\')
            Assert-True ([string]::Equals(
                [IO.Path]::GetFullPath($recordedPriorPath),
                [IO.Path]::GetFullPath($expectedPriorPath),
                [StringComparison]::OrdinalIgnoreCase)) `
                'G16 archived prior LastTest exact path'
            Assert-True (Test-Path -LiteralPath $recordedPriorPath -PathType Leaf) `
                'G16 archived prior LastTest exists'
            $priorItem = Get-Item -Force -LiteralPath $recordedPriorPath
            Assert-True (($priorItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
                'G16 archived prior LastTest is not a reparse point'
            Assert-True ($priorItem.Length -le 64MB) `
                'G16 archived prior LastTest has bounded bytes'
            Assert-Exact ([long]$priorItem.Length) `
                ([long]$attempt.prior_last_test_log.bytes) `
                'G16 archived prior LastTest bytes'
            Assert-Exact (Get-FileSha256 $recordedPriorPath) `
                ([string]$attempt.prior_last_test_log.sha256) `
                'G16 archived prior LastTest SHA-256 identity'
        } else {
            Assert-True ($null -eq $attempt.prior_last_test_log.path) `
                'G16 absent prior LastTest has no path'
            Assert-Exact ([long]$attempt.prior_last_test_log.bytes) 0L `
                'G16 absent prior LastTest has zero bytes'
            Assert-True ($null -eq $attempt.prior_last_test_log.sha256) `
                'G16 absent prior LastTest has no SHA-256'
        }
    } else {
        Assert-True ($null -eq $attempt.test_started_utc) `
            'G16 pre-test checkpoint has no test-start timestamp'
        Assert-True ($null -eq $attempt.prior_last_test_log) `
            'G16 pre-test checkpoint has no prior LastTest disposition'
    }
    if ([string]$attempt.status -ceq 'test_passed') {
        Assert-True ($null -ne $attempt.transient_ctest_log) `
            'G16 test-pass checkpoint includes the CTest log'
        Assert-Match ([string]$attempt.transient_ctest_log.sha256) `
            '^[0-9a-f]{64}$' 'G16 attempt CTest-log SHA-256' -CaseSensitive
    } else {
        Assert-True ($null -eq $attempt.transient_ctest_log) `
            'G16 pre-test-pass checkpoint has no passing CTest log'
    }
    return $attempt
}

function Set-G16AttemptCheckpoint {
    param(
        [string]$Path,
        [ValidateSet(
            'prepared',
            'build_started',
            'build_passed',
            'test_started',
            'test_passed')]
        [string]$Status,
        $Context,
        $SourceFingerprint,
        $ToolchainIdentity,
        $Invocation,
        $Existing,
        $BuildEvidence,
        [string]$TestStartedUtc,
        $PriorLastTestLog,
        $TransientCtestLog,
        $Predecessor)
    $attemptId = if ($null -eq $Existing) {
        [Guid]::NewGuid().ToString('N')
    } else {
        [string]$Existing.attempt_id
    }
    $startedUtc = if ($null -eq $Existing) {
        Get-UtcTimestamp
    } else {
        [string]$Existing.started_utc
    }
    if ($null -ne $Existing) {
        $allowedTransition = [ordered]@{
            prepared = 'build_started'
            build_started = 'build_passed'
            build_passed = 'test_started'
            test_started = 'test_passed'
        }
        Assert-True ($allowedTransition.Contains([string]$Existing.status)) `
            'G16 attempt can transition from its current status'
        Assert-Exact $Status $allowedTransition[[string]$Existing.status] `
            'G16 attempt status transition'
        $Context = $Existing.context
        $SourceFingerprint = $Existing.source_fingerprint
        $ToolchainIdentity = $Existing.toolchain_identity
        $Invocation = $Existing.invocation
        $Predecessor = $Existing.predecessor
    }
    $persistedBuildEvidence = if (
        $PSBoundParameters.ContainsKey('BuildEvidence')) {
        $BuildEvidence
    } elseif ($null -ne $Existing) {
        $Existing.build_evidence
    } else {
        $null
    }
    $persistedTestStartedUtc = if (
        $PSBoundParameters.ContainsKey('TestStartedUtc')) {
        $TestStartedUtc
    } elseif ($null -ne $Existing) {
        $Existing.test_started_utc
    } else {
        $null
    }
    $persistedTransientLog = if (
        $PSBoundParameters.ContainsKey('TransientCtestLog')) {
        $TransientCtestLog
    } elseif ($null -ne $Existing) {
        $Existing.transient_ctest_log
    } else {
        $null
    }
    $persistedPriorLastTestLog = if (
        $PSBoundParameters.ContainsKey('PriorLastTestLog')) {
        $PriorLastTestLog
    } elseif ($null -ne $Existing) {
        $Existing.prior_last_test_log
    } else {
        $null
    }
    $checkpoint = [ordered]@{
        schema_version = 2
        phase = 'P16'
        gate = 'G16'
        attempt_id = $attemptId
        status = $Status
        started_utc = $startedUtc
        updated_utc = Get-UtcTimestamp
        working_directory = $WorkspaceRoot
        context = $Context
        source_fingerprint = $SourceFingerprint
        toolchain_identity = $ToolchainIdentity
        invocation = $Invocation
        predecessor = $Predecessor
        build_invocations = if ($Status -ceq 'prepared') { 0 } else { 1 }
        test_invocations = if ($Status -cin @(
                'test_started', 'test_passed')) { 1 } else { 0 }
        build_evidence = $persistedBuildEvidence
        test_started_utc = $persistedTestStartedUtc
        prior_last_test_log = $persistedPriorLastTestLog
        transient_ctest_log = $persistedTransientLog
    }
    $candidatePath = $Path + '.candidate.' + $PID + '.' +
        [Guid]::NewGuid().ToString('N')
    try {
        Write-JsonFileAtomic -Path $candidatePath -Value $checkpoint
        $validatedCandidate = Read-G16AttemptCheckpoint $candidatePath
        Assert-Exact ([string]$validatedCandidate.attempt_id) $attemptId `
            'G16 candidate checkpoint attempt identifier'
        Assert-Exact ([string]$validatedCandidate.status) $Status `
            'G16 candidate checkpoint status'
        Move-Item -LiteralPath $candidatePath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $candidatePath) {
            Remove-Item -LiteralPath $candidatePath -Force
        }
    }
    return Read-G16AttemptCheckpoint $Path
}

function Assert-G16AttemptContext {
    param(
        $Attempt,
        $Context,
        $SourceFingerprint,
        $ToolchainIdentity,
        $Invocation,
        [switch]$IgnoreCurrentParallel)
    Assert-True ([string]::Equals(
        [string]$Attempt.working_directory,
        $WorkspaceRoot,
        [StringComparison]::OrdinalIgnoreCase)) `
        'G16 attempt checkpoint working directory'
    Assert-Exact ([string]$Attempt.context.sha256) ([string]$Context.sha256) `
        'G16 attempt context SHA-256'
    Assert-SourceFingerprint $SourceFingerprint $Attempt.source_fingerprint `
        'G16 attempt source fingerprint'
    Assert-JsonObjectExact $ToolchainIdentity $Attempt.toolchain_identity `
        'G16 attempt live toolchain identity'
    if ($IgnoreCurrentParallel) {
        Assert-True ([int]$Attempt.invocation.evidence.parallel -ge 1 -and
            [int]$Attempt.invocation.evidence.parallel -le 256) `
            'G16 recorded invocation has bounded parallelism'
        Assert-JsonObjectExact `
            (Get-G16StableInvocationEvidence $Invocation.evidence) `
            (Get-G16StableInvocationEvidence $Attempt.invocation.evidence) `
            'G16 attempt stable original invocation'
    } else {
        Assert-JsonObjectExact $Invocation $Attempt.invocation `
            'G16 attempt original invocation'
    }
}

$script:GitTool = Resolve-StateTool 'git.exe' 'git'
$staticSourceFingerprint = Get-TrackedSourceFingerprint
Assert-Exact (Get-FileSha256 $PSCommandPath) `
    $script:ExecutingGateScriptSha256 `
    'executing G16 runner bytes remain unchanged before static validation'
Assert-Exact $script:ExpectedTests.Count 83 'pinned G16 test count'
Assert-Exact @($script:ExpectedTests | Sort-Object -Unique -CaseSensitive).Count `
    83 'pinned G16 tests are unique'
Assert-Exact $script:ExpectedResources.Count 6 `
    'pinned dashboard resource count'

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L `
    'sealed Forsetti byte count'
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

$phases = ConvertFrom-JsonPreserveDates -Value `
    (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\plans\phases.json'))
$phase = @($phases.phases | Where-Object id -CEQ 'P16')
Assert-Exact $phase.Count 1 'single P16 phase plan entry'
Assert-Exact ([string]$phase[0].title) `
    'Manager process, startup, IPC, and dashboard' 'P16 title'
Assert-Set @($phase[0].dependencies) @('P06', 'P07') 'P16 dependencies'
Assert-Set @($phase[0].required_gates) @('G16') 'P16 required gate'
Assert-Set @($phase[0].required_work) @(
    'Single-instance manager',
    'Authenticated named pipe and loopback dashboard',
    'Watchdog/startup/restart/port tests') 'P16 required work'

$gates = ConvertFrom-JsonPreserveDates -Value `
    (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\plans\gates.json'))
$gate = @($gates.gates | Where-Object id -CEQ 'G16')
Assert-Exact $gate.Count 1 'single G16 gate plan entry'
Assert-Exact ([string]$gate[0].title) 'Manager/IPC/dashboard' 'G16 title'
Assert-Exact ([string]$gate[0].class) 'hard' 'G16 class'
Assert-Exact ([string]$gate[0].acceptance) `
    'Single ownership, authenticated IPC, startup, restart, and shutdown pass.' `
    'G16 acceptance'

$testMatrix = ConvertFrom-JsonPreserveDates -Value `
    (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\plans\test-matrix.json'))
$managerSuite = @($testMatrix.suites | Where-Object id -CEQ 'T-MGR')
Assert-Exact $managerSuite.Count 1 'single T-MGR suite entry'
Assert-Exact ([string]$managerSuite[0].scope) `
    'Single manager, named pipe ACL/auth, dashboard, startup, recovery' `
    'T-MGR scope'
Assert-True ([bool]$managerSuite[0].required) 'T-MGR remains required'

$taskContract = ConvertFrom-JsonPreserveDates -Value `
    (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\governance\PORT_TASK_CONTRACT.json'))
Assert-Exact ([string]$taskContract.schema_version) '1.0' `
    'task-contract schema version'
Assert-Exact ([string]$taskContract.task_id) `
    'FAE-TASK-2026-08-24-016' 'task-contract identifier'
Assert-Exact ([string]$taskContract.forsetti_project_context.target_platform) `
    'Windows' 'task-contract target platform'
Assert-Exact ([string]$taskContract.forsetti_project_context.framework_version) `
    '0.2.0' 'task-contract Forsetti version'
Assert-True ([bool]$taskContract.forsetti_project_context.uses_public_api_only) `
    'task contract retains public Forsetti API use only'
Assert-Exact ([bool]$taskContract.forsetti_project_context.touches_framework_internals) `
    $false 'task contract keeps Forsetti internals sealed'
Assert-True (@($taskContract.scope.in_scope) -ccontains
    'native C++20 domain, application, infrastructure, CLI, manager, MCP, session host, installer, tests, and WinUI 3 UI') `
    'task contract includes the native Manager and CLI output'
Assert-True (@($taskContract.scope.out_of_scope) -ccontains
    'adding Python to the Windows product') `
    'task contract excludes the forbidden runtime'
Assert-True (@($taskContract.scope.out_of_scope) -ccontains
    'GUI automation of LM Studio or another model host') `
    'task contract excludes external model-host GUI automation'
Assert-True (@($taskContract.validation_requirements) -ccontains
    'native tests pass') 'task contract requires native tests'
Assert-True (@($taskContract.validation_requirements) -ccontains
    'no-Python and no-attribution scans pass') `
    'task contract requires repository policy scans'
Assert-True (@($taskContract.validation_requirements) -ccontains
    'independent validator session passes') `
    'task contract retains independent evidence review'
Assert-True (@($taskContract.evidence_requirements) -ccontains
    'command logs with exit codes and hashes') `
    'task contract requires command evidence'
Assert-True (@($taskContract.evidence_requirements) -ccontains
    'test reports') 'task contract requires test evidence'
Assert-True (@($taskContract.evidence_requirements) -ccontains
    'binary and package hashes') 'task contract requires binary hashes'

$ownerDecisionPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\decisions\OWNER-002-alpha-release-qualification-scope.md'
$ownerDecision = Get-Content -Raw -LiteralPath $ownerDecisionPath
Assert-Match $ownerDecision 'Status:\s+Accepted owner override' `
    'OWNER-002 remains accepted'
Assert-Match $ownerDecision `
    'Each remaining alpha gate receives one authoritative gate invocation[.]' `
    'OWNER-002 one authoritative invocation rule'
Assert-Match $ownerDecision `
    'If it succeeds, the gate passes for alpha[.]' `
    'OWNER-002 one-pass gate result rule'
Assert-Match $ownerDecision `
    'P22/G22 and all additional security-only review, hardening, fuzzing, adversarial\s+qualification, and defense-in-depth work are deferred until after alpha[.]' `
    'OWNER-002 security-hardening deferral'
Assert-Match $ownerDecision `
    'Already implemented controls are not removed, and controls inseparable from\s+correct product behavior remain in their owning feature slices[.]' `
    'OWNER-002 retains functional controls'
Assert-Match $ownerDecision `
    'Alpha installer qualification is limited to this machine' `
    'OWNER-002 current-machine qualification'
Assert-Match $ownerDecision `
    'Cross-machine, virtual\s+machine, clean-profile, clean-OS, and alternate-hardware qualification is\s+deferred until after alpha[.]' `
    'OWNER-002 clean-environment deferral'
Assert-Match $ownerDecision `
    'Custom styling, decorative treatments,\s+micro-animations, pixel-level visual refinement, and other bespoke polish are\s+deferred until after alpha[.]' `
    'OWNER-002 bespoke UI-polish deferral'
Assert-Match $ownerDecision `
    'Functional UI automation remains in the alpha scope at the complete planned\s+coverage[.]' `
    'OWNER-002 retains full functional UI automation'
Assert-Match $ownerDecision `
    'This changes evidence repetition, not the no-feature-loss requirement[.]' `
    'OWNER-002 retains no feature loss'

$definitionOfDone = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\docs\DEFINITION_OF_DONE.md')
Assert-Match $definitionOfDone `
    'Manager is single-owner, restartable, and auto-start capable[.]' `
    'definition of done retains Manager lifecycle behavior'
Assert-Match $definitionOfDone `
    'Native unit/integration/protocol/UI/stress/fault tests pass[.]' `
    'definition of done retains native behavioral tests'
Assert-Match $definitionOfDone 'No unbounded ownership issue remains[.]' `
    'definition of done retains bounded ownership'
Assert-Match $definitionOfDone `
    'No Python or forbidden runtime is present[.]' `
    'definition of done retains runtime exclusion'
Assert-Match $definitionOfDone `
    'No automated-authorship attribution is present[.]' `
    'definition of done retains attribution exclusion'

$parityRows = Import-Csv -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\feature-parity-matrix.tsv') `
    -Delimiter "`t"
$expectedParityRows = @(
    [ordered]@{
        id = 'PROC-003'; category = 'Process'; feature = 'Manager run mode'
        behavior = 'manager run owns dashboard lifecycle'
        acceptance = 'Single-owner manager'
    },
    [ordered]@{
        id = 'MGR-001'; category = 'Manager'; feature = 'Single dashboard owner'
        behavior = 'GUI attaches, manager listens'
        acceptance = 'Port collision tests'
    },
    [ordered]@{
        id = 'MGR-002'; category = 'Manager'; feature = 'Startup installation'
        behavior = 'Per-user automatic manager start'
        acceptance = 'Task registration tests'
    },
    [ordered]@{
        id = 'MGR-003'; category = 'Manager'; feature = 'Watchdog/restart'
        behavior = 'Health, recovery, bounded restart'
        acceptance = 'Fault injection'
    },
    [ordered]@{
        id = 'MGR-004'; category = 'Manager'; feature = 'Typed client'
        behavior = 'GUI manager client'
        acceptance = 'IPC compatibility'
    },
    [ordered]@{
        id = 'DASH-001'; category = 'Dashboard'; feature = 'Loopback dashboard'
        behavior = 'Operational/manager/telemetry routes'
        acceptance = 'Token-auth loopback tests'
    },
    [ordered]@{
        id = 'DIAG-002'; category = 'Diagnostics'
        feature = 'Runtime ownership snapshot'
        behavior = 'Tasks/timers/processes/queues/databases'
        acceptance = 'Shutdown zero-owner gate'
    },
    [ordered]@{
        id = 'SEC-003'; category = 'Security'; feature = 'Authenticated IPC'
        behavior = 'Current-user named pipe and dashboard token'
        acceptance = 'Cross-user denial tests'
    },
    [ordered]@{
        id = 'PKG-006'; category = 'Packaging'
        feature = 'No Python/no attribution'
        behavior = 'Scanners pass'
        acceptance = 'Hard gate'
    }
)
foreach ($expectedRow in $expectedParityRows) {
    $row = @($parityRows | Where-Object id -CEQ $expectedRow.id)
    Assert-Exact $row.Count 1 "single parity row $($expectedRow.id)"
    Assert-Exact ([string]$row[0].category) ([string]$expectedRow.category) `
        "$($expectedRow.id) category"
    Assert-Exact ([string]$row[0].feature) ([string]$expectedRow.feature) `
        "$($expectedRow.id) feature"
    Assert-Exact ([string]$row[0].behavior) ([string]$expectedRow.behavior) `
        "$($expectedRow.id) behavior"
    Assert-Exact ([string]$row[0].windows_acceptance) `
        ([string]$expectedRow.acceptance) "$($expectedRow.id) acceptance"
}

$g16TextFiles = @(
    'CMakeLists.txt',
    'src/Hosts/Manager/ManagerCompositionRoot.cpp',
    'src/Hosts/Manager/ManagerCompositionRoot.h',
    'src/Hosts/Manager/ManagerProcessArguments.cpp',
    'src/Hosts/Manager/ManagerProcessArguments.h',
    'src/Hosts/Manager/main.cpp',
    'src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc',
    'src/Hosts/Manager/Resources/Dashboard/DashboardResourceIds.h',
    'src/Hosts/Manager/Resources/Dashboard/index.html',
    'src/Hosts/Manager/Resources/Dashboard/control.html',
    'src/Hosts/Manager/Resources/Dashboard/dashboard.css',
    'src/Hosts/Manager/Resources/Dashboard/auth.js',
    'src/Hosts/Manager/Resources/Dashboard/telemetry.js',
    'src/Hosts/Manager/Resources/Dashboard/control.js',
    'src/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.cpp',
    'tests/Manager/ManagerCompositionFixture.cpp',
    'tests/Manager/WindowsManagerCompositionLifecycleTests.cpp',
    'scripts/validation/Test-G16ManagerIpcDashboard.ps1',
    '.forge-codex/state/decisions/P16-038-hash-bound-g16-qualification-and-independent-finalization.md',
    '.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md'
)
foreach ($relative in $g16TextFiles) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G16 governed file exists: $relative"
    Assert-CrlfTextFile $path "G16 governed text file $relative"
}

$productionFiles = @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'src') `
    -Recurse -File -Include '*.cpp', '*.h')
$productionText = ($productionFiles | ForEach-Object {
    Get-Content -Raw -LiteralPath $_.FullName
}) -join "`n"
Assert-NoMatch $productionText 'C:\\Users\\' `
    'production code contains no machine-specific user path' -CaseSensitive
Assert-NoMatch $productionText `
    '\b(?:SendInput|mouse_event|keybd_event|IUIAutomation|SetCursorPos|FindWindowW)\b' `
    'production code contains no external application GUI automation'

$cmakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$cmakeText = Get-Content -Raw -LiteralPath $cmakePath
$labelsByTest = Get-CmakeTestLabels $cmakeText
Assert-CmakeTestCommandTargets $cmakeText
$cmakeG16Tests = @($labelsByTest.Keys | Where-Object {
    @($labelsByTest[$_]) -ccontains 'G16'
})
Assert-Set $cmakeG16Tests $script:ExpectedTests `
    'static exact G16 CTest inventory'
Assert-Exact @($cmakeG16Tests | Where-Object {
        $_.StartsWith('ForgeConductor.Manager.', [StringComparison]::Ordinal)
    }).Count 27 'static G16 Manager test count'
Assert-Exact @($cmakeG16Tests | Where-Object {
        $_.StartsWith('ForgeConductor.Dashboard.', [StringComparison]::Ordinal)
    }).Count 54 'static G16 Dashboard test count'
foreach ($name in $script:ExpectedTests) {
    Assert-Match $cmakeText `
        ('add_test\s*\(\s*(?:NAME\s+)?' + [regex]::Escape($name) + '\b') `
        "CTest registration exists: $name" -CaseSensitive
    Assert-Match $cmakeText `
        ('add_executable\s*\(\s*' + [regex]::Escape($name) + '\b') `
        "CTest target exists: $name" -CaseSensitive
    if ($name.StartsWith(
            'ForgeConductor.Manager.',
            [StringComparison]::Ordinal)) {
        Assert-True (@($labelsByTest[$name]) -ccontains 'T-MGR') `
            "static Manager test carries T-MGR: $name"
    }
}
Assert-Match $cmakeText `
    'add_library\s*\(ForgeConductor[.]Manager[.]CompositionRoot[.]Windows\s+STATIC[\s\S]*?ManagerCompositionRoot[.]cpp[\s\S]*?ManagerCompositionRoot[.]h\s*\)' `
    'shared Manager composition-root library is declared' -CaseSensitive
Assert-Match $cmakeText `
    'add_executable\s*\(ForgeConductor[.]Manager\s+[\s\S]*?DashboardAssets[.]rc[\s\S]*?src/Hosts/Manager/main[.]cpp\s*\)' `
    'production Manager target embeds the dashboard resources' -CaseSensitive
Assert-Match $cmakeText `
    'target_link_libraries\s*\(ForgeConductor[.]Manager\s+PRIVATE\s+ForgeConductor::Manager[.]CompositionRoot[.]Windows\s*\)' `
    'production Manager links the shared composition root' -CaseSensitive
Assert-Match $cmakeText `
    'set_target_properties\s*\(ForgeConductor[.]Manager\s+PROPERTIES\s+OUTPUT_NAME\s+"ForgeConductor[.]Manager"\s*\)' `
    'production Manager output name is exact' -CaseSensitive
Assert-Match $cmakeText `
    'add_dependencies\s*\(ForgeConductor[.]Manager\s+ForgeConductor[.]Cli\s*\)' `
    'production Manager depends on the exact CLI' -CaseSensitive
Assert-Match $cmakeText `
    'add_custom_command\s*\(TARGET\s+ForgeConductor[.]Manager\s+POST_BUILD[\s\S]*?TARGET_FILE:ForgeConductor[.]Cli[\s\S]*?TARGET_FILE_DIR:ForgeConductor[.]Manager' `
    'production Manager stages the exact sibling CLI' -CaseSensitive
Assert-Match $cmakeText `
    'add_executable\s*\(ForgeConductor[.]Manager[.]CompositionFixture[\s\S]*?ManagerCompositionFixture[.]cpp[\s\S]*?DashboardAssets[.]rc\s*\)' `
    'controlled composition fixture embeds the production resources' `
    -CaseSensitive
Assert-NoMatch $cmakeText `
    'add_test\s*\(\s*(?:NAME\s+)?ForgeConductor[.]Manager[.]CompositionFixture\b' `
    'long-lived Manager composition fixture is not a CTest' -CaseSensitive
Assert-Match $cmakeText `
    'add_custom_target\s*\(\s*ForgeConductor[.]Manager[.]CompositionFixture[.]CliStage[\s\S]*?make_directory[\s\S]*?manager-composition-fixture/[$]<CONFIG>[\s\S]*?copy_if_different[\s\S]*?TARGET_FILE:ForgeConductor[.]Cli[\s\S]*?manager-composition-fixture/[$]<CONFIG>/[$]<TARGET_FILE_NAME:ForgeConductor[.]Cli>[\s\S]*?\)' `
    'controlled fixture always stages the exact current CLI' -CaseSensitive
Assert-Match $cmakeText `
    'add_dependencies\s*\(\s*ForgeConductor[.]Manager[.]CompositionFixture[.]CliStage\s+ForgeConductor[.]Cli\s*\)' `
    'controlled fixture CLI stage depends on the canonical CLI' -CaseSensitive
Assert-Match $cmakeText `
    'add_dependencies\s*\(\s*ForgeConductor[.]Manager[.]CompositionFixture\s+ForgeConductor[.]Manager[.]CompositionFixture[.]CliStage\s*\)' `
    'controlled fixture depends on its always-run CLI stage' -CaseSensitive
Assert-Match $cmakeText `
    'add_dependencies\s*\(\s*ForgeConductor[.]Manager[.]CompositionLifecycleTests\s+ForgeConductor[.]Manager[.]CompositionFixture\s*\)' `
    'controlled lifecycle test stages the composition fixture by dependency' `
    -CaseSensitive
Assert-Match $cmakeText `
    'add_test\s*\(\s*NAME\s+ForgeConductor[.]Manager[.]CompositionLifecycleTests\s+COMMAND\s+[$]<TARGET_FILE:ForgeConductor[.]Manager[.]CompositionLifecycleTests>\s+[$]<TARGET_FILE:ForgeConductor[.]Manager[.]CompositionFixture>\s+[$]<TARGET_FILE:ForgeConductor[.]Cli>\s*\)' `
    'controlled lifecycle CTest receives the exact fixture and canonical CLI paths' `
    -CaseSensitive
Assert-Match $cmakeText `
    'ForgeConductor[.]Manager[.]CompositionLifecycleTests\s+PROPERTIES[\s\S]*?LABELS\s+"G16;T-MGR;T-PROC;T-SEC;T-INTEGRATION;T-STRESS"[\s\S]*?RUN_SERIAL\s+TRUE[\s\S]*?TIMEOUT\s+480\s*\)' `
    'controlled lifecycle CTest has exact labels, serialization, and bound' `
    -CaseSensitive

$resourceRoot = Join-Path $WorkspaceRoot `
    'src\Hosts\Manager\Resources\Dashboard'
$resourceHeader = Get-Content -Raw -LiteralPath (Join-Path $resourceRoot `
    'DashboardResourceIds.h')
$resourceRc = Get-Content -Raw -LiteralPath (Join-Path $resourceRoot `
    'DashboardAssets.rc')
$resourceBundle = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Infrastructure\Windows\WindowsDashboardStaticAssetBundle.cpp')
$definedResourcePairs = [Collections.Generic.List[string]]::new()
foreach ($match in [regex]::Matches(
        $resourceHeader,
        '(?m)^#define\s+(?<macro>FORGE_DASHBOARD_[A-Z_]+_RESOURCE_ID)\s+(?<id>[0-9]+)\s*$')) {
    $definedResourcePairs.Add(
        $match.Groups['macro'].Value + '=' + $match.Groups['id'].Value)
}
Assert-Set @($definedResourcePairs) @($script:ExpectedResources |
    ForEach-Object { $_.macro + '=' + $_.id }) `
    'exact dashboard resource identifier definitions'
$rcResourcePairs = [Collections.Generic.List[string]]::new()
foreach ($match in [regex]::Matches(
        $resourceRc,
        '(?m)^(?<macro>FORGE_DASHBOARD_[A-Z_]+_RESOURCE_ID)\s+RCDATA\s+"(?<file>[^"]+)"\s*$')) {
    $rcResourcePairs.Add(
        $match.Groups['macro'].Value + '=' + $match.Groups['file'].Value)
}
Assert-Set @($rcResourcePairs) @($script:ExpectedResources |
    ForEach-Object { $_.macro + '=' + $_.file }) `
    'exact dashboard RCDATA source mappings'
$bundleResourcePairs = [Collections.Generic.List[string]]::new()
foreach ($match in [regex]::Matches(
        $resourceBundle,
        '\{(?<macro>FORGE_DASHBOARD_[A-Z_]+_RESOURCE_ID),\s*"(?<route>/static/[^"]+)"\}')) {
    $bundleResourcePairs.Add(
        $match.Groups['macro'].Value + '=' + $match.Groups['route'].Value)
}
Assert-Set @($bundleResourcePairs) @($script:ExpectedResources |
    ForEach-Object { $_.macro + '=' + $_.route }) `
    'exact dashboard runtime route mappings'

Invoke-RepositoryIntegrityChecks
$frameworkStaticAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkStaticAfter.files) `
    ([int]$frameworkBefore.files) 'sealed Forsetti file count after static checks'
Assert-Exact ([long]$frameworkStaticAfter.bytes) `
    ([long]$frameworkBefore.bytes) 'sealed Forsetti bytes after static checks'
Assert-Exact ([string]$frameworkStaticAfter.sha256) `
    ([string]$frameworkBefore.sha256) 'sealed Forsetti hash after static checks'
$staticSourceAfter = Get-TrackedSourceFingerprint
Assert-SourceFingerprint $staticSourceAfter $staticSourceFingerprint `
    'static validation source fingerprint remains unchanged'
Assert-Exact (Get-FileSha256 $PSCommandPath) `
    $script:ExecutingGateScriptSha256 `
    'executing G16 runner bytes remain unchanged after static validation'

if ($StaticOnly -and -not $Resume) {
    Write-Host "G16 static validation passed ($script:AssertionCount assertions)."
    return
}

$script:G16RunnerLock = $null
trap {
    if ($null -ne $script:G16RunnerLock) {
        $script:G16RunnerLock.Dispose()
        $script:G16RunnerLock = $null
    }
    throw
}
$runnerLockPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P16\g16-runner.lock'
if (Test-Path -LiteralPath $runnerLockPath) {
    $runnerLockItem = Get-Item -Force -LiteralPath $runnerLockPath
    Assert-True (($runnerLockItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'G16 runner lock is not a reparse point'
}
try {
    $script:G16RunnerLock = [IO.File]::Open(
        $runnerLockPath,
        [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
} catch {
    throw 'Another G16 qualification or evidence review owns the workspace lock.'
}
Assert-True ($null -ne $script:G16RunnerLock) `
    'G16 workspace runner lock is held exclusively'

Assert-True (Test-Windows11) 'qualification host is Windows 11'
$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$binRoot = Join-Path $buildRoot 'bin\Release'
$managerPath = Join-Path $binRoot 'ForgeConductor.Manager.exe'
$cliPath = Join-Path $binRoot 'forge-conductor.exe'
$fixturePath = Join-Path $buildRoot `
    'tests\manager-composition-fixture\Release\ForgeConductor.Manager.exe'
$fixtureCliPath = Join-Path (Split-Path -Parent $fixturePath) `
    'forge-conductor.exe'
$transientLastTestLogPath = Join-Path $buildRoot `
    'Testing\Temporary\LastTest.log'
$durableLastTestLogPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P16\g16-LastTest.log'
$summaryPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P16\g16-validation-summary.json'
$attemptPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P16\g16-attempt.json'
$pendingRolloverPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P16\g16-rollover-pending.json'
$toolchainStatePath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\toolchain.json'
$toolchainStateBytes = [IO.File]::ReadAllBytes($toolchainStatePath)
$toolchainStateSha256 = Get-BytesSha256 $toolchainStateBytes
$toolchainStateText = [Text.UTF8Encoding]::new($false, $true).GetString(
    $toolchainStateBytes)
$toolchainState = ConvertFrom-JsonPreserveDates -Value `
    $toolchainStateText
Assert-Exact (Get-FileSha256 $toolchainStatePath) $toolchainStateSha256 `
    'toolchain state remains unchanged after its exact-byte parse'
$ctest = Resolve-StateTool 'ctest.exe' 'ctest'
$linker = Resolve-StateTool 'link.exe' 'link'
$cmake = Resolve-StateTool 'cmake.exe' 'cmake'
$compiler = Resolve-StateTool 'cl.exe' 'cl'
$msbuild = Resolve-StateTool 'MSBuild.exe' 'msbuild'
$gitTool = $script:GitTool
if (-not $Resume) {
    Assert-PathToolIdentity 'cmake.exe' $cmake
    Assert-PathToolIdentity 'ctest.exe' $ctest
}
$vcpkgEvidence = Get-VcpkgEvidence `
    -ToolchainState $toolchainState `
    -GitPath $gitTool
$env:VCPKG_ROOT = [string]$vcpkgEvidence.root
Assert-Exact ([string]$toolchainState.tool_versions.cl) '19.44.35223.0' `
    'selected compiler version'
Assert-Exact ([string]$toolchainState.tool_versions.msbuild) `
    '17.14.40.60911' 'selected MSBuild version'
Assert-Exact ([string]$toolchainState.msvc.toolset_version) `
    '14.44.35207' 'selected MSVC toolset version'
Assert-True ([string]::Equals(
    (Resolve-Path -LiteralPath ([string]$toolchainState.msvc.compiler)).Path,
    $compiler,
    [StringComparison]::OrdinalIgnoreCase)) `
    'toolchain state compiler identity is exact'
Assert-True ([string]::Equals(
    (Resolve-Path -LiteralPath ([string]$toolchainState.msvc.linker)).Path,
    $linker,
    [StringComparison]::OrdinalIgnoreCase)) `
    'toolchain state linker identity is exact'
$buildTargets = @(
    'ForgeConductor.Manager',
    'ForgeConductor.Manager.CompositionFixture'
) + $script:ExpectedTests

$sourceFingerprint = Get-TrackedSourceFingerprint
Assert-SourceFingerprint $sourceFingerprint $staticSourceFingerprint `
    'runtime source fingerprint matches the statically validated source'
Assert-Exact (Get-FileSha256 $PSCommandPath) `
    $script:ExecutingGateScriptSha256 `
    'executing G16 runner bytes remain unchanged before runtime validation'
$toolchainIdentity = New-G16ToolchainIdentity `
    -ToolchainStateSha256 $toolchainStateSha256 `
    -CmakePath $cmake `
    -CtestPath $ctest `
    -CompilerPath $compiler `
    -LinkerPath $linker `
    -MsbuildPath $msbuild `
    -GitPath $gitTool `
    -VcpkgEvidence $vcpkgEvidence
$contextFingerprint = New-G16ContextFingerprint `
    -SourceFingerprint $sourceFingerprint `
    -ToolchainIdentity $toolchainIdentity
$invocationEvidence = [ordered]@{
    configuration = 'Release'
    architecture = 'x64'
    parallel = $Parallel
    build_targets = $buildTargets
    ctest_preset = 'windows-msvc-x64-release'
    ctest_label = 'G16'
    build_script = [ordered]@{
        path = 'scripts/build.ps1'
        sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot `
            'scripts\build.ps1')
    }
    test_script = [ordered]@{
        path = 'scripts/test.ps1'
        sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot `
            'scripts\test.ps1')
    }
    gate_script = [ordered]@{
        path = 'scripts/validation/Test-G16ManagerIpcDashboard.ps1'
        sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot `
            'scripts\validation\Test-G16ManagerIpcDashboard.ps1')
    }
    cmake = $toolchainIdentity.cmake
    ctest = $toolchainIdentity.ctest
}
$invocationIdentity = [ordered]@{
    sha256 = Get-JsonObjectSha256 $invocationEvidence
    evidence = $invocationEvidence
}
$productionArtifacts = $null
$testArtifacts = $null
$embeddedResources = $null
$lastTestLog = $null
$inventory = $null
$summary = $null
$cacheEvidence = $null
$compilerEvidence = $null
$buildEvidence = $null

if ($Resume) {
    Write-Host 'G16: validating the hash-bound prior pass without rebuilding or rerunning tests.'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    $resolvedPriorSummary = Resolve-CanonicalPriorSummary `
        $PriorPassedEvidencePath
    Assert-Exact (Get-FileSha256 $resolvedPriorSummary) `
        $PriorPassedEvidenceSha256 'resume evidence SHA-256'
    $summary = ConvertFrom-JsonPreserveDates -Value `
        (Get-Content -Raw -LiteralPath $resolvedPriorSummary)
    Assert-Exact ([int]$summary.schema_version) 1 `
        'resume summary schema version'
    Assert-Exact ([string]$summary.phase) 'P16' 'resume summary phase'
    Assert-Exact ([string]$summary.gate) 'G16' 'resume summary gate'
    Assert-Exact ([string]$summary.status) 'passed' 'resume summary status'
    Assert-True ([string]::Equals(
        [string]$summary.working_directory,
        $WorkspaceRoot,
        [StringComparison]::OrdinalIgnoreCase)) `
        'resume summary working directory'
    Assert-Exact ([string]$summary.scope.configuration) 'Release' `
        'resume summary configuration'
    Assert-Exact ([string]$summary.scope.architecture) 'x64' `
        'resume summary architecture'
    Assert-True ([bool]$summary.scope.machine_local) `
        'resume summary is machine-local'
    Assert-True ([bool]$summary.scope.clean_environment_deferred) `
        'resume summary records clean-environment deferral'
    Assert-True ([bool]$summary.scope.security_hardening_deferred) `
        'resume summary records security-hardening deferral'
    Assert-True ([bool]$summary.scope.bespoke_ui_polish_deferred) `
        'resume summary records bespoke UI-polish deferral'
    Assert-True ([bool]$summary.scope.functional_ui_automation_retained) `
        'resume summary retains functional UI automation'
    Assert-Exact ([int]$summary.scope.authoritative_build_invocations) 1 `
        'resume summary build invocation count'
    Assert-Exact ([int]$summary.scope.authoritative_test_invocations) 1 `
        'resume summary test invocation count'
    Assert-Exact ([int]$summary.deterministic_test_count) 83 `
        'resume summary deterministic test count'
    Assert-Set @($summary.deterministic_tests) $script:ExpectedTests `
        'resume summary exact deterministic tests'
    Assert-SourceFingerprint $sourceFingerprint $summary.source_fingerprint `
        'resume source fingerprint'
    $attempt = Read-G16AttemptCheckpoint $attemptPath
    Assert-G16AttemptContext `
        -Attempt $attempt `
        -Context $contextFingerprint `
        -SourceFingerprint $sourceFingerprint `
        -ToolchainIdentity $toolchainIdentity `
        -Invocation $invocationIdentity `
        -IgnoreCurrentParallel
    Assert-True (-not (Test-Path -LiteralPath $pendingRolloverPath)) `
        'resume has no unresolved G16 pending rollover'
    Assert-G16PredecessorLineage `
        -Predecessor $attempt.predecessor `
        -AttemptPath $attemptPath `
        -SummaryPath $summaryPath `
        -DurableLastTestLogPath $durableLastTestLogPath `
        -TransientLastTestLogPath $transientLastTestLogPath
    Assert-Exact ([string]$attempt.status) 'test_passed' `
        'resume G16 attempt reached the durable test-pass checkpoint'
    Assert-Exact ([int]$attempt.build_invocations) 1 `
        'resume G16 attempt build invocation count'
    Assert-Exact ([int]$attempt.test_invocations) 1 `
        'resume G16 attempt test invocation count'
    Assert-Exact ([string]$summary.attempt_checkpoint.path) `
        '.forge-codex/state/evidence/P16/g16-attempt.json' `
        'resume G16 attempt checkpoint path'
    Assert-Exact ([string]$summary.attempt_checkpoint.status) 'test_passed' `
        'resume G16 attempt checkpoint summary status'
    Assert-Exact ([string]$summary.attempt_checkpoint.attempt_id) `
        ([string]$attempt.attempt_id) `
        'resume G16 attempt checkpoint identifier'
    Assert-Exact ([string]$summary.attempt_checkpoint.context_sha256) `
        ([string]$attempt.context.sha256) `
        'resume G16 attempt context SHA-256'
    Assert-Exact ([string]$summary.attempt_checkpoint.invocation_sha256) `
        ([string]$attempt.invocation.sha256) `
        'resume G16 attempt invocation SHA-256'
    Assert-Exact ([string]$summary.attempt_checkpoint.build_evidence_sha256) `
        ([string]$attempt.build_evidence.sha256) `
        'resume G16 attempt build-evidence SHA-256'
    Assert-JsonObjectExact $summary.attempt_checkpoint.predecessor `
        $attempt.predecessor 'resume G16 attempt predecessor'
    Assert-Exact ([string]$summary.attempt_checkpoint.sha256) `
        (Get-FileSha256 $attemptPath) `
        'resume G16 attempt checkpoint SHA-256'
    Assert-Exact ([string]$summary.toolchain.state.sha256) `
        $toolchainStateSha256 'resume toolchain-state SHA-256'
    Assert-Exact ([string]$summary.toolchain.state.path) `
        '.forge-codex/state/toolchain.json' 'resume toolchain-state path'
    Assert-Exact ([string]$summary.toolchain.build_script_sha256) `
        ([string]$attempt.invocation.evidence.build_script.sha256) `
        'resume summary build-script SHA-256 is attempt-bound'
    Assert-Exact ([string]$summary.toolchain.test_script_sha256) `
        ([string]$attempt.invocation.evidence.test_script.sha256) `
        'resume summary test-script SHA-256 is attempt-bound'
    Assert-Exact ([string]$summary.toolchain.gate_script_sha256) `
        ([string]$attempt.invocation.evidence.gate_script.sha256) `
        'resume summary gate-script SHA-256 is attempt-bound'
    Assert-Exact ([string]$summary.toolchain.cmake.path) $cmake `
        'resume CMake path'
    Assert-Exact ([string]$summary.toolchain.cmake.version) `
        ([string]$toolchainState.tool_versions.cmake) `
        'resume CMake version'
    Assert-Exact ([string]$summary.toolchain.cmake.sha256) `
        (Get-FileSha256 $cmake) 'resume CMake SHA-256'
    Assert-Exact ([string]$summary.toolchain.ctest.path) $ctest `
        'resume CTest path'
    Assert-Exact ([string]$summary.toolchain.ctest.version) `
        ([string]$toolchainState.tool_versions.ctest) `
        'resume CTest version'
    Assert-Exact ([string]$summary.toolchain.ctest.sha256) `
        (Get-FileSha256 $ctest) 'resume CTest SHA-256'
    Assert-Exact ([string]$summary.toolchain.compiler.path) $compiler `
        'resume compiler path'
    Assert-Exact ([string]$summary.toolchain.compiler.version) `
        ([string]$toolchainState.tool_versions.cl) `
        'resume compiler version'
    Assert-Exact ([string]$summary.toolchain.compiler.sha256) `
        (Get-FileSha256 $compiler) 'resume compiler SHA-256'
    Assert-Exact ([string]$summary.toolchain.msbuild.path) $msbuild `
        'resume MSBuild path'
    Assert-Exact ([string]$summary.toolchain.msbuild.version) `
        ([string]$toolchainState.tool_versions.msbuild) `
        'resume MSBuild version'
    Assert-Exact ([string]$summary.toolchain.msbuild.sha256) `
        (Get-FileSha256 $msbuild) 'resume MSBuild SHA-256'
    Assert-Exact ([string]$summary.toolchain.linker.path) $linker `
        'resume linker path'
    Assert-Exact ([string]$summary.toolchain.linker.toolset_version) `
        ([string]$toolchainState.msvc.toolset_version) `
        'resume linker toolset version'
    Assert-Exact ([string]$summary.toolchain.linker.sha256) `
        (Get-FileSha256 $linker) 'resume linker SHA-256'
    foreach ($property in @(
            'root',
            'executable',
            'executable_sha256',
            'checkout_head',
            'builtin_baseline',
            'toolchain_file',
            'toolchain_file_bytes',
            'toolchain_file_sha256',
            'git_path',
            'git_sha256')) {
        Assert-Exact $vcpkgEvidence[$property] `
            $summary.toolchain.vcpkg.$property `
            "resume vcpkg $property"
    }
    $cacheEvidence = Get-CmakeCacheEvidence `
        -Path (Join-Path $buildRoot 'CMakeCache.txt') `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $compilerEvidence = Get-CmakeCompilerEvidence `
        -BuildRoot $buildRoot `
        -ExpectedCompiler $compiler `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    foreach ($property in @(
            'path',
            'bytes',
            'sha256',
            'generator',
            'generator_instance',
            'generator_platform',
            'generator_toolset',
            'system_version',
            'vcpkg_host_triplet',
            'vcpkg_target_triplet',
            'toolchain_file',
            'toolchain_file_sha256',
            'linker')) {
        Assert-Exact $cacheEvidence[$property] `
            $summary.toolchain.cmake_cache.$property `
            "resume CMake cache $property"
    }
    foreach ($property in @(
            'path',
            'bytes',
            'sha256',
            'compiler',
            'compiler_id',
            'compiler_version',
            'architecture',
            'linker')) {
        Assert-Exact $compilerEvidence[$property] `
            $summary.toolchain.cmake_compiler.$property `
            "resume configured CMake compiler $property"
    }

    $inventory = Get-G16CtestInventoryPreservingLastTest `
        -CtestPath $ctest `
        -LastTestPath $transientLastTestLogPath
    $resumeBuildEvidence = Get-G16BuildEvidenceBundle `
        -Inventory $inventory `
        -CmakeCache $cacheEvidence `
        -CmakeCompiler $compilerEvidence `
        -ManagerPath $managerPath `
        -CliPath $cliPath `
        -FixturePath $fixturePath `
        -FixtureCliPath $fixtureCliPath `
        -Linker $linker
    Assert-G16BuildEvidenceBundleCurrent `
        -Recorded $attempt.build_evidence `
        -Current $resumeBuildEvidence `
        -Message 'resume authoritative build'

    $productionArtifacts = [ordered]@{
        manager = Get-ArtifactEvidence `
            -Path $managerPath -Linker $linker -ProductionPolicy
        cli = Get-ArtifactEvidence `
            -Path $cliPath -Linker $linker -ProductionPolicy
        composition_fixture = Get-ArtifactEvidence `
            -Path $fixturePath -Linker $linker
        composition_fixture_cli = Get-ArtifactEvidence `
            -Path $fixtureCliPath -Linker $linker
    }
    Assert-Exact ([long]$productionArtifacts.composition_fixture_cli.bytes) `
        ([long]$productionArtifacts.cli.bytes) `
        'resume staged fixture CLI byte length equals the canonical CLI'
    Assert-Exact ([string]$productionArtifacts.composition_fixture_cli.sha256) `
        ([string]$productionArtifacts.cli.sha256) `
        'resume staged fixture CLI SHA-256 equals the canonical CLI'
    foreach ($name in @(
            'manager',
            'cli',
            'composition_fixture',
            'composition_fixture_cli')) {
        $current = $productionArtifacts[$name]
        $recorded = $summary.production_artifacts.$name
        Assert-Exact ([string]$current.path) ([string]$recorded.path) `
            "resume $name artifact path"
        Assert-Exact ([long]$current.bytes) ([long]$recorded.bytes) `
            "resume $name artifact bytes"
        Assert-Exact ([string]$current.sha256) ([string]$recorded.sha256) `
            "resume $name artifact SHA-256"
    }

    $testArtifacts = @($summary.test_artifacts)
    Assert-TestArtifactEvidenceCurrent `
        -Artifacts $testArtifacts `
        -Message 'resume test'

    $embeddedResources = Get-EmbeddedResourceEvidence $managerPath
    Assert-Exact @($summary.embedded_dashboard_resources).Count 6 `
        'resume summary embedded-resource count'
    foreach ($current in $embeddedResources) {
        $recorded = @($summary.embedded_dashboard_resources |
            Where-Object id -CEQ $current.id)
        Assert-Exact $recorded.Count 1 `
            "resume embedded resource $($current.id) record"
        foreach ($property in @(
                'file',
                'route',
                'bytes',
                'source_sha256',
                'embedded_sha256')) {
            Assert-Exact $current[$property] $recorded[0].$property `
                "resume embedded resource $($current.id) $property"
        }
    }

    $lastTestLog = Get-LastTestLogEvidence $durableLastTestLogPath
    Assert-Exact ([string]$lastTestLog.path) `
        ([string]$summary.ctest_log.path) 'resume CTest log path'
    Assert-Exact ([long]$lastTestLog.bytes) `
        ([long]$summary.ctest_log.bytes) 'resume CTest log bytes'
    Assert-Exact ([string]$lastTestLog.sha256) `
        ([string]$summary.ctest_log.sha256) 'resume CTest log SHA-256'
    Assert-Exact ([long]$attempt.transient_ctest_log.bytes) `
        ([long]$lastTestLog.bytes) `
        'resume attempt CTest log bytes'
    Assert-Exact ([string]$attempt.transient_ctest_log.sha256) `
        ([string]$lastTestLog.sha256) `
        'resume attempt CTest log SHA-256'
    Assert-Exact ([string]$summary.acceptance) ([string]$gate[0].acceptance) `
        'resume G16 acceptance'
    Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
        $sourceFingerprint 'resume final source fingerprint closure'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    Assert-Exact (Get-FileSha256 $PSCommandPath) `
        $script:ExecutingGateScriptSha256 `
        'resume final executing-runner SHA-256 closure'
    Assert-G16LiveToolchainSnapshot `
        -ToolchainStatePath $toolchainStatePath `
        -ExpectedToolchainStateSha256 $toolchainStateSha256 `
        -ExpectedIdentity $toolchainIdentity `
        -SkipPathSelection
    Assert-Exact (Get-FileSha256 $resolvedPriorSummary) `
        $PriorPassedEvidenceSha256 'resume final summary SHA-256 closure'
    Assert-Exact (Get-FileSha256 $attemptPath) `
        ([string]$summary.attempt_checkpoint.sha256) `
        'resume final attempt-checkpoint SHA-256 closure'
    $finalResumeLog = Get-LastTestLogEvidence $durableLastTestLogPath
    Assert-Exact ([string]$finalResumeLog.sha256) `
        ([string]$summary.ctest_log.sha256) `
        'resume final durable CTest-log SHA-256 closure'
    $terminalResumeCache = Get-CmakeCacheEvidence `
        -Path (Join-Path $buildRoot 'CMakeCache.txt') `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $terminalResumeCompiler = Get-CmakeCompilerEvidence `
        -BuildRoot $buildRoot `
        -ExpectedCompiler $compiler `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $terminalResumeInventory = Get-G16CtestInventoryPreservingLastTest `
        -CtestPath $ctest `
        -LastTestPath $transientLastTestLogPath
    $terminalResumeBuildEvidence = Get-G16BuildEvidenceBundle `
        -Inventory $terminalResumeInventory `
        -CmakeCache $terminalResumeCache `
        -CmakeCompiler $terminalResumeCompiler `
        -ManagerPath $managerPath `
        -CliPath $cliPath `
        -FixturePath $fixturePath `
        -FixtureCliPath $fixtureCliPath `
        -Linker $linker
    Assert-G16BuildEvidenceBundleCurrent `
        -Recorded $attempt.build_evidence `
        -Current $terminalResumeBuildEvidence `
        -Message 'resume terminal complete build-evidence closure'
    Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
        $sourceFingerprint 'resume terminal source fingerprint closure'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    Assert-Exact (Get-FileSha256 $PSCommandPath) `
        $script:ExecutingGateScriptSha256 `
        'resume terminal executing-runner SHA-256 closure'
    Assert-G16LiveToolchainSnapshot `
        -ToolchainStatePath $toolchainStatePath `
        -ExpectedToolchainStateSha256 $toolchainStateSha256 `
        -ExpectedIdentity $toolchainIdentity `
        -SkipPathSelection
} else {
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    $predecessor = Read-G16PendingRollover `
        -Path $pendingRolloverPath `
        -CurrentContext $contextFingerprint
    $recoveringAttempt = Test-Path -LiteralPath $attemptPath -PathType Leaf
    if ($recoveringAttempt) {
        $attempt = Read-G16AttemptCheckpoint $attemptPath
        if ($null -ne $predecessor -and
            $null -ne $attempt.predecessor -and
            [string]$attempt.predecessor.manifest_sha256 -ceq
                [string]$predecessor.manifest_sha256) {
            Assert-Exact `
                ([string]$attempt.predecessor.actual_replacement_context_sha256) `
                ([string]$attempt.context.sha256) `
                'G16 recovered attempt consumed its pending rollover context'
            Remove-Item -LiteralPath $pendingRolloverPath
            $predecessor = $null
        } elseif ($null -ne $predecessor -and
            [string]$predecessor.attempt_id -ceq
                [string]$attempt.attempt_id) {
            $predecessor = Archive-G16CanonicalAttempt `
                -Attempt $attempt `
                -AttemptPath $attemptPath `
                -SummaryPath $summaryPath `
                -DurableLastTestLogPath $durableLastTestLogPath `
                -TransientLastTestLogPath $transientLastTestLogPath `
                -NewContext $contextFingerprint
            $recoveringAttempt = $false
            $attempt = $null
            Write-Host 'G16: completed the pending prior-attempt archive before replacement.'
        }
        if ($recoveringAttempt -and
            [string]$attempt.context.sha256 -cne
            [string]$contextFingerprint.sha256) {
            $predecessor = Archive-G16CanonicalAttempt `
                -Attempt $attempt `
                -AttemptPath $attemptPath `
                -SummaryPath $summaryPath `
                -DurableLastTestLogPath $durableLastTestLogPath `
                -TransientLastTestLogPath $transientLastTestLogPath `
                -NewContext $contextFingerprint
            $recoveringAttempt = $false
            $attempt = $null
            Write-Host 'G16: archived the prior context before starting its one replacement invocation.'
        }
        if ($recoveringAttempt -and
            [string]$attempt.status -cin @(
                'build_started', 'test_started')) {
            $predecessor = Archive-G16CanonicalAttempt `
                -Attempt $attempt `
                -AttemptPath $attemptPath `
                -SummaryPath $summaryPath `
                -DurableLastTestLogPath $durableLastTestLogPath `
                -TransientLastTestLogPath $transientLastTestLogPath `
                -NewContext $contextFingerprint `
                -Reason interrupted_authoritative_invocation_retry
            $recoveringAttempt = $false
            $attempt = $null
            Write-Host (
                'G16: archived the interrupted attempt before the explicitly ' +
                'invoked same-context retry.')
        }
    }
    Assert-True (-not (Test-Path -LiteralPath $summaryPath)) `
        'canonical G16 pass summary does not preexist the authoritative invocation'
    if ($recoveringAttempt) {
        if ($null -ne $predecessor) {
            Assert-JsonObjectExact $attempt.predecessor $predecessor `
                'G16 recovered attempt predecessor matches pending rollover'
            Remove-Item -LiteralPath $pendingRolloverPath
        }
        Assert-G16AttemptContext `
            -Attempt $attempt `
            -Context $contextFingerprint `
            -SourceFingerprint $sourceFingerprint `
            -ToolchainIdentity $toolchainIdentity `
            -Invocation $invocationIdentity
        Write-Host (
            'G16: recovering durable attempt state without repeating a passed build or test: ' +
            [string]$attempt.status)
    } else {
        Assert-True (-not (Test-Path -LiteralPath $durableLastTestLogPath)) `
            'canonical durable G16 CTest log does not preexist a new authoritative attempt'
        Assert-G16PredecessorLineage `
            -Predecessor $predecessor `
            -AttemptPath $attemptPath `
            -SummaryPath $summaryPath `
            -DurableLastTestLogPath $durableLastTestLogPath `
            -TransientLastTestLogPath $transientLastTestLogPath
        $attempt = Set-G16AttemptCheckpoint `
            -Path $attemptPath `
            -Status prepared `
            -Context $contextFingerprint `
            -SourceFingerprint $sourceFingerprint `
            -ToolchainIdentity $toolchainIdentity `
            -Invocation $invocationIdentity `
            -Predecessor $predecessor
        if (Test-Path -LiteralPath $pendingRolloverPath -PathType Leaf) {
            Assert-JsonObjectExact $attempt.predecessor $predecessor `
                'G16 new attempt persisted the pending predecessor'
            Remove-Item -LiteralPath $pendingRolloverPath
        }
    }

    Assert-True (-not (Test-Path -LiteralPath $pendingRolloverPath)) `
        'G16 attempt has no unresolved pending rollover'
    Assert-G16PredecessorLineage `
        -Predecessor $attempt.predecessor `
        -AttemptPath $attemptPath `
        -SummaryPath $summaryPath `
        -DurableLastTestLogPath $durableLastTestLogPath `
        -TransientLastTestLogPath $transientLastTestLogPath

    if ([string]$attempt.status -ceq 'prepared') {
        Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
            $sourceFingerprint `
            'source fingerprint immediately before the build helper'
        Assert-Exact (Get-FileSha256 $PSCommandPath) `
            $script:ExecutingGateScriptSha256 `
            'executing G16 runner bytes immediately before the build helper'
        Assert-G16LiveToolchainSnapshot `
            -ToolchainStatePath $toolchainStatePath `
            -ExpectedToolchainStateSha256 $toolchainStateSha256 `
            -ExpectedIdentity $toolchainIdentity
        $attempt = Set-G16AttemptCheckpoint `
            -Path $attemptPath `
            -Status build_started `
            -Existing $attempt
        Write-Host 'G16: running the single authoritative affected-target Release x64 build.'
        & (Join-Path $WorkspaceRoot 'scripts\build.ps1') `
            -Configuration Release `
            -Architecture x64 `
            -Target $buildTargets `
            -Parallel $Parallel
        $buildHelperSucceeded = $?
        Assert-G16LiveToolchainSnapshot `
            -ToolchainStatePath $toolchainStatePath `
            -ExpectedToolchainStateSha256 $toolchainStateSha256 `
            -ExpectedIdentity $toolchainIdentity
        Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
            $sourceFingerprint `
            'source fingerprint immediately after the build helper'
        Assert-Exact (Get-FileSha256 $PSCommandPath) `
            $script:ExecutingGateScriptSha256 `
            'executing G16 runner bytes immediately after the build helper'
        Assert-True $buildHelperSucceeded `
            'single authoritative G16 affected-target build'
    } elseif ([string]$attempt.status -ceq 'build_started') {
        throw (
            'The authoritative G16 build was interrupted before a durable pass ' +
            'checkpoint. Refusing to infer success from mutable build artifacts.')
    } elseif ([string]$attempt.status -ceq 'test_started') {
        throw (
            'The authoritative G16 test was interrupted before a durable pass ' +
            'checkpoint. Refusing to infer success from a mutable LastTest.log.')
    }

    $cacheEvidence = Get-CmakeCacheEvidence `
        -Path (Join-Path $buildRoot 'CMakeCache.txt') `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $compilerEvidence = Get-CmakeCompilerEvidence `
        -BuildRoot $buildRoot `
        -ExpectedCompiler $compiler `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $inventory = Get-G16CtestInventoryPreservingLastTest `
        -CtestPath $ctest `
        -LastTestPath $transientLastTestLogPath
    $currentBuildEvidence = Get-G16BuildEvidenceBundle `
        -Inventory $inventory `
        -CmakeCache $cacheEvidence `
        -CmakeCompiler $compilerEvidence `
        -ManagerPath $managerPath `
        -CliPath $cliPath `
        -FixturePath $fixturePath `
        -FixtureCliPath $fixtureCliPath `
        -Linker $linker
    if ([string]$attempt.status -ceq 'build_started') {
        $attempt = Set-G16AttemptCheckpoint `
            -Path $attemptPath `
            -Status build_passed `
            -Existing $attempt `
            -BuildEvidence $currentBuildEvidence
    } else {
        Assert-G16BuildEvidenceBundleCurrent `
            -Recorded $attempt.build_evidence `
            -Current $currentBuildEvidence `
            -Message 'recovered authoritative build'
    }

    $testAlreadyPassed = [string]$attempt.status -ceq 'test_passed'
    if ($testAlreadyPassed) {
        $lastTestLog = Get-LastTestLogEvidence $durableLastTestLogPath
        Assert-Exact ([long]$attempt.transient_ctest_log.bytes) `
            ([long]$lastTestLog.bytes) `
            'recovered test-pass checkpoint CTest log byte length'
        Assert-Exact ([string]$attempt.transient_ctest_log.sha256) `
            ([string]$lastTestLog.sha256) `
            'recovered test-pass checkpoint CTest log SHA-256'
        $transientLastTestLog = [ordered]@{
            path = [string]$attempt.transient_ctest_log.path
            bytes = [long]$attempt.transient_ctest_log.bytes
            sha256 = [string]$attempt.transient_ctest_log.sha256
        }
    }
    if (-not $testAlreadyPassed) {
        Assert-Exact ([string]$attempt.status) 'build_passed' `
            'G16 build checkpoint precedes the authoritative test'
        $priorLastTestLog = Move-G16PriorLastTestLog `
            -Source $transientLastTestLogPath `
            -AttemptId ([string]$attempt.attempt_id)
        Assert-True (-not (Test-Path -LiteralPath $transientLastTestLogPath)) `
            'G16 transient LastTest path is absent before test_started'
        $testStartedUtc = Get-UtcTimestamp
        Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
            $sourceFingerprint `
            'source fingerprint immediately before the test helper'
        Assert-Exact (Get-FileSha256 $PSCommandPath) `
            $script:ExecutingGateScriptSha256 `
            'executing G16 runner bytes immediately before the test helper'
        Assert-G16LiveToolchainSnapshot `
            -ToolchainStatePath $toolchainStatePath `
            -ExpectedToolchainStateSha256 $toolchainStateSha256 `
            -ExpectedIdentity $toolchainIdentity
        $attempt = Set-G16AttemptCheckpoint `
            -Path $attemptPath `
            -Status test_started `
            -Existing $attempt `
            -TestStartedUtc $testStartedUtc `
            -PriorLastTestLog $priorLastTestLog
        Write-Host 'G16: running the exact G16 CTest label once.'
        & (Join-Path $WorkspaceRoot 'scripts\test.ps1') `
            -Configuration Release `
            -Architecture x64 `
            -Parallel $Parallel `
            -Label G16
        $testHelperSucceeded = $?
        Assert-G16LiveToolchainSnapshot `
            -ToolchainStatePath $toolchainStatePath `
            -ExpectedToolchainStateSha256 $toolchainStateSha256 `
            -ExpectedIdentity $toolchainIdentity
        Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
            $sourceFingerprint `
            'source fingerprint immediately after the test helper'
        Assert-Exact (Get-FileSha256 $PSCommandPath) `
            $script:ExecutingGateScriptSha256 `
            'executing G16 runner bytes immediately after the test helper'
        Assert-True $testHelperSucceeded `
            'single authoritative G16 CTest execution'

        $transientLastTestLog = Get-LastTestLogEvidence `
            $transientLastTestLogPath
        Copy-FileBytesAtomicExact `
            -Source $transientLastTestLogPath `
            -Destination $durableLastTestLogPath
        $lastTestLog = Get-LastTestLogEvidence $durableLastTestLogPath
        Assert-Exact ([string]$lastTestLog.sha256) `
            ([string]$transientLastTestLog.sha256) `
            'durable G16 CTest log exactly matches the transient CTest log'
        $postTestCacheEvidence = Get-CmakeCacheEvidence `
            -Path (Join-Path $buildRoot 'CMakeCache.txt') `
            -ExpectedLinker $linker `
            -ToolchainState $toolchainState
        $postTestCompilerEvidence = Get-CmakeCompilerEvidence `
            -BuildRoot $buildRoot `
            -ExpectedCompiler $compiler `
            -ExpectedLinker $linker `
            -ToolchainState $toolchainState
        $postTestInventory = Get-G16CtestInventoryPreservingLastTest `
            -CtestPath $ctest `
            -LastTestPath $transientLastTestLogPath
        $postTestBuildEvidence = Get-G16BuildEvidenceBundle `
            -Inventory $postTestInventory `
            -CmakeCache $postTestCacheEvidence `
            -CmakeCompiler $postTestCompilerEvidence `
            -ManagerPath $managerPath `
            -CliPath $cliPath `
            -FixturePath $fixturePath `
            -FixtureCliPath $fixtureCliPath `
            -Linker $linker
        Assert-G16BuildEvidenceBundleCurrent `
            -Recorded $attempt.build_evidence `
            -Current $postTestBuildEvidence `
            -Message 'post-CTest authoritative build'
        $attempt = Set-G16AttemptCheckpoint `
            -Path $attemptPath `
            -Status test_passed `
            -Existing $attempt `
            -TransientCtestLog $transientLastTestLog
    }

    $buildEvidence = $attempt.build_evidence
    $cacheEvidence = $buildEvidence.evidence.cmake_cache
    $compilerEvidence = $buildEvidence.evidence.cmake_compiler
    $testArtifacts = @($buildEvidence.evidence.test_artifacts)
    $productionArtifacts = $buildEvidence.evidence.production_artifacts
    $embeddedResources = @(
        $buildEvidence.evidence.embedded_dashboard_resources)

    $sourceAfter = Get-TrackedSourceFingerprint
    Assert-SourceFingerprint $sourceAfter $sourceFingerprint `
        'authoritative invocation source fingerprint remains unchanged'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs

    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'post-build repository no-Python validation'
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'post-build repository no-attribution validation'
    Invoke-RepositoryIntegrityChecks

    $frameworkAfter = Get-TreeSummary $frameworkRoot
    Assert-Exact ([int]$frameworkAfter.files) `
        ([int]$frameworkBefore.files) `
        'sealed Forsetti file count remains unchanged'
    Assert-Exact ([long]$frameworkAfter.bytes) `
        ([long]$frameworkBefore.bytes) `
        'sealed Forsetti byte count remains unchanged'
    Assert-Exact ([string]$frameworkAfter.sha256) `
        ([string]$frameworkBefore.sha256) `
        'sealed Forsetti tree hash remains unchanged'
    Assert-Exact ([string]$attempt.status) 'test_passed' `
        'G16 attempt reached the durable passing-test checkpoint'
    Assert-Exact ([int]$attempt.build_invocations) 1 `
        'G16 attempt exact build invocation count'
    Assert-Exact ([int]$attempt.test_invocations) 1 `
        'G16 attempt exact test invocation count'

    $summary = [ordered]@{
        schema_version = 1
        phase = 'P16'
        gate = 'G16'
        status = 'passed'
        recorded_utc = Get-UtcTimestamp
        working_directory = $WorkspaceRoot
        scope = [ordered]@{
            operating_system = 'Windows 11'
            architecture = 'x64'
            configuration = 'Release'
            machine_local = $true
            clean_environment_deferred = $true
            security_hardening_deferred = $true
            bespoke_ui_polish_deferred = $true
            functional_ui_automation_retained = $true
            authoritative_build_invocations = 1
            authoritative_test_invocations = 1
            validator_rebuild_required = $false
            validator_test_rerun_required = $false
            recovered_finalization = [bool]$recoveringAttempt
        }
        invocations = [ordered]@{
            build = [ordered]@{
                script = 'scripts/build.ps1'
                configuration = 'Release'
                architecture = 'x64'
                parallel = $Parallel
                targets = $buildTargets
                exit_code = 0
            }
            ctest_inventory = [ordered]@{
                executable = $ctest
                preset = 'windows-msvc-x64-release'
                label = 'G16'
                exit_code = 0
            }
            test = [ordered]@{
                script = 'scripts/test.ps1'
                configuration = 'Release'
                architecture = 'x64'
                parallel = $Parallel
                label = 'G16'
                exit_code = 0
            }
        }
        toolchain = [ordered]@{
            state = [ordered]@{
                path = '.forge-codex/state/toolchain.json'
                sha256 = $toolchainStateSha256
            }
            cmake = [ordered]@{
                path = $cmake
                version = [string]$toolchainState.tool_versions.cmake
                sha256 = Get-FileSha256 $cmake
            }
            ctest = [ordered]@{
                path = $ctest
                version = [string]$toolchainState.tool_versions.ctest
                sha256 = Get-FileSha256 $ctest
            }
            compiler = [ordered]@{
                path = $compiler
                version = [string]$toolchainState.tool_versions.cl
                toolset_version = [string]$toolchainState.msvc.toolset_version
                sha256 = Get-FileSha256 $compiler
            }
            linker = [ordered]@{
                path = $linker
                toolset_version = [string]$toolchainState.msvc.toolset_version
                sha256 = Get-FileSha256 $linker
            }
            msbuild = [ordered]@{
                path = $msbuild
                version = [string]$toolchainState.tool_versions.msbuild
                visual_studio_instance =
                    [string]$toolchainState.visual_studio.instanceId
                visual_studio_installation =
                    [string]$toolchainState.visual_studio.resolvedInstallationPath
                sha256 = Get-FileSha256 $msbuild
            }
            vcpkg = $vcpkgEvidence
            cmake_cache = $cacheEvidence
            cmake_compiler = $compilerEvidence
            build_script_sha256 =
                [string]$attempt.invocation.evidence.build_script.sha256
            test_script_sha256 =
                [string]$attempt.invocation.evidence.test_script.sha256
            gate_script_sha256 =
                [string]$attempt.invocation.evidence.gate_script.sha256
        }
        source_fingerprint = $sourceFingerprint
        attempt_checkpoint = [ordered]@{
            path = '.forge-codex/state/evidence/P16/g16-attempt.json'
            attempt_id = [string]$attempt.attempt_id
            status = [string]$attempt.status
            sha256 = Get-FileSha256 $attemptPath
            context_sha256 = [string]$attempt.context.sha256
            invocation_sha256 = [string]$attempt.invocation.sha256
            build_evidence_sha256 = [string]$attempt.build_evidence.sha256
            predecessor = $attempt.predecessor
        }
        deterministic_tests = $script:ExpectedTests
        deterministic_test_count = $script:ExpectedTests.Count
        test_artifacts = $testArtifacts
        ctest_log = $lastTestLog
        transient_ctest_log = [ordered]@{
            path = (Get-RelativePathPortable `
                -BasePath $WorkspaceRoot `
                -TargetPath $transientLastTestLogPath).Replace('\', '/')
            bytes = [long]$transientLastTestLog.bytes
            sha256 = [string]$transientLastTestLog.sha256
            durable_copy_sha256 = [string]$lastTestLog.sha256
        }
        production_artifacts = $productionArtifacts
        embedded_dashboard_resources = $embeddedResources
        assertions = $script:AssertionCount
        acceptance = [string]$gate[0].acceptance
        terminal_state_transition =
            'deferred_to_independent_validator_after_outer_command_record'
        remaining_limitations = @(
            'This qualifies only P16/G16 on the owner current Windows 11 x64 machine in Release configuration.',
            'It does not qualify another machine, a clean environment, ARM64, UI parity, packaging, or the complete alpha.',
            'Security-only hardening remains deferred until after alpha under OWNER-002.',
            'Bespoke UI polish remains deferred; complete functional UI automation remains required by its later gate.',
            'An independent Validator must review this hash-bound evidence without rebuilding or rerunning G16.'
        )
    }
    Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
        $sourceFingerprint 'normal final source fingerprint closure'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    Assert-Exact (Get-FileSha256 $PSCommandPath) `
        $script:ExecutingGateScriptSha256 `
        'normal final executing-runner SHA-256 closure'
    Assert-G16LiveToolchainSnapshot `
        -ToolchainStatePath $toolchainStatePath `
        -ExpectedToolchainStateSha256 $toolchainStateSha256 `
        -ExpectedIdentity $toolchainIdentity
    Assert-Exact (Get-FileSha256 $attemptPath) `
        ([string]$summary.attempt_checkpoint.sha256) `
        'normal final attempt-checkpoint SHA-256 closure'
    $finalNormalLog = Get-LastTestLogEvidence $durableLastTestLogPath
    Assert-Exact ([string]$finalNormalLog.sha256) `
        ([string]$summary.ctest_log.sha256) `
        'normal final durable CTest-log SHA-256 closure'
    $terminalNormalCache = Get-CmakeCacheEvidence `
        -Path (Join-Path $buildRoot 'CMakeCache.txt') `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $terminalNormalCompiler = Get-CmakeCompilerEvidence `
        -BuildRoot $buildRoot `
        -ExpectedCompiler $compiler `
        -ExpectedLinker $linker `
        -ToolchainState $toolchainState
    $terminalNormalInventory = Get-G16CtestInventoryPreservingLastTest `
        -CtestPath $ctest `
        -LastTestPath $transientLastTestLogPath
    $terminalNormalBuildEvidence = Get-G16BuildEvidenceBundle `
        -Inventory $terminalNormalInventory `
        -CmakeCache $terminalNormalCache `
        -CmakeCompiler $terminalNormalCompiler `
        -ManagerPath $managerPath `
        -CliPath $cliPath `
        -FixturePath $fixturePath `
        -FixtureCliPath $fixtureCliPath `
        -Linker $linker
    Assert-G16BuildEvidenceBundleCurrent `
        -Recorded $attempt.build_evidence `
        -Current $terminalNormalBuildEvidence `
        -Message 'normal terminal complete build-evidence closure'
    Assert-SourceFingerprint (Get-TrackedSourceFingerprint) `
        $sourceFingerprint 'normal terminal source fingerprint closure'
    Assert-TrackedBuildInputsClean
    Assert-NoUntrackedBuildInputs
    Assert-Exact (Get-FileSha256 $PSCommandPath) `
        $script:ExecutingGateScriptSha256 `
        'normal terminal executing-runner SHA-256 closure'
    Assert-G16LiveToolchainSnapshot `
        -ToolchainStatePath $toolchainStatePath `
        -ExpectedToolchainStateSha256 $toolchainStateSha256 `
        -ExpectedIdentity $toolchainIdentity
    $summary.assertions = $script:AssertionCount
    Write-JsonFileAtomic -Path $summaryPath -Value $summary
    Assert-True (Test-Path -LiteralPath $summaryPath -PathType Leaf) `
        'G16 validation summary was written after the pass'
}

if ($StaticOnly) {
    $script:G16RunnerLock.Dispose()
    $script:G16RunnerLock = $null
    Write-Host "G16 hash-bound resume validation passed without a build, test, or state update ($script:AssertionCount assertions)."
    return
}

$summaryHash = Get-FileSha256 $summaryPath
$script:G16RunnerLock.Dispose()
$script:G16RunnerLock = $null
Write-Host "G16 Manager/IPC/dashboard validation passed ($script:AssertionCount assertions)."
Write-Host "Validation summary: $summaryPath"
Write-Host "Validation summary SHA-256: $summaryHash"
Write-Host 'Terminal G16/P16 state remains deferred to the independent Validator.'
