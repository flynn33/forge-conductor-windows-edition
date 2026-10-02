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
    if (-not $Condition) { throw "G11 assertion failed: $Message" }
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

$retained = Join-Path $WorkspaceRoot 'scripts\validation\Test-G10Agents.ps1'
Assert-True (Test-Path -LiteralPath $retained -PathType Leaf) `
    'retained G10 validator exists'
Write-Host 'G11: running retained G10 static validation without a retained rebuild.'
& $retained -WorkspaceRoot $WorkspaceRoot -Parallel $Parallel -StaticOnly
Assert-True $? 'retained G10 static validation'

& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-Python validation'
& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-attribution validation'

$p11Files = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Domain/ContinuityModels.h',
    'include/ForgeConductor/Domain/LegacyContinuityModels.h',
    'src/Domain/ContinuityModels.cpp',
    'src/Domain/LegacyContinuityModels.cpp',
    'include/ForgeConductor/Contracts/IContinuityCoordinator.h',
    'include/ForgeConductor/Contracts/IContinuityDocumentCodec.h',
    'include/ForgeConductor/Contracts/IContinuityProjectionStore.h',
    'include/ForgeConductor/Contracts/ILegacyContextContinuityService.h',
    'include/ForgeConductor/Contracts/ILegacyContinuityRepository.h',
    'include/ForgeConductor/Contracts/ILegacyContinuitySessionSource.h',
    'include/ForgeConductor/Contracts/IProjectMemoryService.h',
    'include/ForgeConductor/Application/ContinuityCoordinator.h',
    'include/ForgeConductor/Application/LegacyContextContinuityService.h',
    'include/ForgeConductor/Application/ProjectMemoryRepositoryCache.h',
    'src/Application/ContinuityCoordinator.cpp',
    'src/Application/LegacyContextContinuityService.cpp',
    'src/Application/ProjectMemoryRepositoryCache.cpp',
    'include/ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.h',
    'src/Infrastructure/Windows/WindowsContinuityDocumentCodec.cpp',
    'src/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.cpp',
    'include/ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h',
    'include/ForgeConductor/Persistence/Windows/WindowsLegacyContinuityRepository.h',
    'include/ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h',
    'src/Persistence/Windows/WindowsLegacyContinuityRepository.cpp',
    'src/Persistence/Windows/WindowsProjectMemoryRepository.cpp',
    'src/Persistence/Windows/Migrations/ProjectMigrations.cpp',
    'src/Persistence/Windows/Migrations/SchemaMigrator.cpp',
    'tests/Infrastructure/ContinuityDocumentCodecTests.cpp',
    'tests/Application/LegacyContextContinuityServiceTests.cpp',
    'tests/Continuity/ContinuityCoordinatorTests.cpp',
    'tests/Continuity/ContinuityCoordinatorProcessFixture.cpp',
    'tests/Continuity/ContinuityRepositoryWindowsTests.cpp',
    'tests/Continuity/LegacyContinuityPersistenceWindowsTests.cpp',
    'tests/ProjectMemory/ProjectMemoryCacheTests.cpp',
    'tests/Persistence/ProjectMigrationTests.cpp',
    '.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md',
    '.forge-codex/state/decisions/P11-001-dual-continuity-surfaces-and-canonical-documents.md',
    '.forge-codex/state/decisions/P11-002-project-continuity-schema-cas-and-retry-recovery.md',
    '.forge-codex/state/decisions/P11-003-side-effect-intents-idempotent-recovery-and-host-boundaries.md',
    '.forge-codex/state/decisions/P11-004-legacy-cas-projections-reset-and-lifetimes.md',
    '.forge-codex/state/decisions/P11-005-abrupt-process-crash-and-concurrency-evidence.md',
    'scripts/validation/Test-G11Continuity.ps1')
foreach ($relative in $p11Files) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "P11 file exists: $relative"
    Assert-CrlfTextFile $path "P11 text file $relative"
}

$portablePaths = @($p11Files | Where-Object {
    $_ -match '^(?:include/ForgeConductor/(?:Domain|Contracts|Application)|src/(?:Domain|Application))/'
})
$portableText = ($portablePaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join "`n"
Assert-NoMatch $portableText `
    '(?im)^\s*#\s*include\s*[<"][^>"]*(?:windows[.]h|winrt[/\\]|winsqlite|sqlite3|nlohmann|filesystem)' `
    'Domain, Contracts, and Application remain platform neutral'
Assert-NoMatch $portableText `
    '\b(?:HANDLE|HWND|HRESULT|sqlite3|nlohmann::|std::filesystem)\b' `
    'portable continuity layers expose no platform implementation types'

$models = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Domain\ContinuityModels.h')
$modelSource = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Domain\ContinuityModels.cpp')
foreach ($state in @(
    'Idle','CheckpointPreparing','CheckpointPersisted','SuccessorCreating',
    'SuccessorCreated','BootstrapSending','Acknowledged','PredecessorSealing',
    'Completed','RetryWait','FailedRecoverable','Cancelling','Cancelled')) {
    Assert-Match $models ("\b" + $state + "\b") "continuity state $state" -CaseSensitive
}
Assert-Match $models 'retryResumeState' 'durable retry resume state' -CaseSensitive
Assert-Match $modelSource 'validateContinuityOperationRetryState' `
    'retry-state validation implementation' -CaseSensitive

$migration = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Persistence\Windows\Migrations\ProjectMigrations.cpp')
$migrator = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Persistence\Windows\Migrations\SchemaMigrator.cpp')
Assert-Match $migration `
    'P003Sql[\s\S]*?ADD COLUMN retry_resume_state TEXT[\s\S]*?state NOT IN \(''predecessorSealed'',''completed'',''cancelled''\)' `
    'P003 retry column and exact terminal predicate' -CaseSensitive
Assert-Match $migration `
    '6a84a8c63e67ed4760ff589cb7ba96bec3ce25140c8e85c849b28b421f25acb9' `
    'immutable P003 SQL hash' -CaseSensitive
Assert-Match $migrator 'ProjectVersion3Layout[\s\S]*?ProjectPhysicalVersion' `
    'project-v3 physical layout' -CaseSensitive

$coordinatorHeader = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Application\ContinuityCoordinator.h')
$coordinator = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Application\ContinuityCoordinator.cpp')
Assert-Match $coordinatorHeader 'class\s+ContinuityCoordinator\s+final' `
    'coordinator is final' -CaseSensitive
Assert-Match $coordinator 'MaximumRecoveryProjects\s*=\s*128U' `
    'bounded recovery project count' -CaseSensitive
Assert-Match $coordinator 'MaximumStateMachineSteps\s*=\s*32U' `
    'bounded state-machine steps' -CaseSensitive
foreach ($intent in @(
    'checkpoint_intent','host_create_intent','bootstrap_intent',
    'predecessor_seal_intent','active_session_pointer_swapped','retry_resumed')) {
    Assert-Match $coordinator ([regex]::Escape($intent)) `
        "durable coordinator evidence $intent" -CaseSensitive
}
Assert-Match $coordinator `
    'queryByIdempotencyKey[\s\S]*?createSession[\s\S]*?validateHostSessionBinding' `
    'successor reconciliation validates durable bindings' -CaseSensitive
Assert-Match $coordinator `
    'repository->handoff\([\s\S]*?repository->operation\(' `
    'resume resolves terminal operations through their durable handoff' -CaseSensitive
$resumeStart = $coordinator.IndexOf(
    'Domain::Result<Domain::HandoffResumeOutcome> resume(',
    [StringComparison]::Ordinal)
$recoverStart = $coordinator.IndexOf(
    'Domain::Result<Domain::ContinuityRecoveryReport> recover(',
    [Math]::Max(0, $resumeStart),
    [StringComparison]::Ordinal)
Assert-True ($resumeStart -ge 0 -and $recoverStart -gt $resumeStart) `
    'resume implementation has a bounded source region'
$resumeBody = $coordinator.Substring($resumeStart, $recoverStart - $resumeStart)
$resumeReconcile = $resumeBody.IndexOf(
    'reconcileSession(', [StringComparison]::Ordinal)
$resumeTransition = $resumeBody.IndexOf(
    'repository->compareAndSet(', [StringComparison]::Ordinal)
Assert-True ($resumeReconcile -ge 0 -and $resumeTransition -ge 0 -and
    $resumeReconcile -lt $resumeTransition) `
    'resume validates the host successor before durable completion transitions'

$repository = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Persistence\Windows\WindowsProjectMemoryRepository.cpp')
foreach ($table in @(
    'rollover_operations','rollover_transitions','continuity_handoffs',
    'project_active_sessions')) {
    Assert-Match $repository ([regex]::Escape($table)) `
        "project continuity table $table" -CaseSensitive
}
Assert-Match $repository `
    'createOperation\([\s\S]*?beginImmediate[\s\S]*?INSERT INTO rollover_operations[\s\S]*?INSERT INTO continuity_handoffs[\s\S]*?commit' `
    'operation and canonical handoff creation are one transaction' -CaseSensitive
Assert-Match $repository `
    'validateContinuityTransitionLedger[\s\S]*?MaximumContinuityTransitionsPerOperation' `
    'bounded transition-ledger validation' -CaseSensitive
Assert-Match $repository `
    'reset_project_continuity[\s\S]*?DELETE FROM rollover_transitions[\s\S]*?DELETE FROM project_active_sessions' `
    'continuity-only transactional reset' -CaseSensitive

$persistenceDecision = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\state\decisions\P11-002-project-continuity-schema-cas-and-retry-recovery.md')
Assert-Match $persistenceDecision `
    'Initial operation creation and canonical handoff insertion are one[\s\S]*?Checkpoint intent is a subsequent compare-and-swap transition' `
    'P11 persistence decision records the recoverable two-transaction boundary' -CaseSensitive
$hostDecision = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\state\decisions\P11-003-side-effect-intents-idempotent-recovery-and-host-boundaries.md')
Assert-Match $hostDecision `
    'Explicit resume reconciles and validates the acknowledged successor before[\s\S]*?active-session pointer' `
    'P11 host decision records pre-commit resume reconciliation' -CaseSensitive

$legacyRepository = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Persistence\Windows\WindowsLegacyContinuityRepository.cpp')
Assert-Match $legacyRepository `
    'compareExchange\([\s\S]*?beginImmediate[\s\S]*?write_sequence[\s\S]*?WHERE id=\? AND write_sequence=\?' `
    'legacy repository optimistic CAS transaction' -CaseSensitive
Assert-Match $legacyRepository 'continuity/latest' `
    'legacy latest pointer' -CaseSensitive
Assert-Match $legacyRepository 'continuity/resume_ready' `
    'legacy resume-ready pointer' -CaseSensitive
Assert-Match $legacyRepository 'content_sha256' `
    'legacy canonical payload checksum' -CaseSensitive

$projection = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Infrastructure\Windows\WindowsLegacyContinuityProjectionStore.cpp')
Assert-Match $projection 'MaximumSequenceMarkerBytes\s*=\s*512U' `
    'bounded projection sequence marker' -CaseSensitive
Assert-Match $projection 'IAtomicFileStore' `
    'projection uses atomic file-store abstraction' -CaseSensitive
Assert-Match $projection 'MaximumRepairRows' `
    'projection repair is bounded' -CaseSensitive

$codecTest = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'tests\Infrastructure\ContinuityDocumentCodecTests.cpp')
Assert-Match $codecTest `
    'fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f' `
    'project-v1 canonical fixture digest' -CaseSensitive
$hostileCoverage = [ordered]@{
    duplicate = 'duplicate'
    nesting = 'tooDeep'
    'UTF-8' = 'invalidUtf8'
    schema = 'schema_version'
    digest = 'ExpectedDigest'
}
foreach ($hostile in $hostileCoverage.GetEnumerator()) {
    Assert-Match $codecTest $hostile.Value `
        "codec hostile-input coverage $($hostile.Key)" -CaseSensitive
}

$coordinatorTests = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'tests\Continuity\ContinuityCoordinatorTests.cpp')
foreach ($case in @(
    'recover_every_committed_crash_boundary',
    'exact_acknowledgement_and_resume_are_bound_to_the_successor',
    'resume_reconciliation_precedes_mutation_and_supports_idempotent_create',
    'capability_failure_does_not_weaken_the_durable_checkpoint',
    'recoverable_host_failure_resumes_only_after_its_durable_retry_time',
    'cancellation_deadline_recovery_cancel_and_shutdown_own_their_boundaries',
    'concurrent_projects_remain_isolated_and_bounded')) {
    Assert-Match $coordinatorTests ([regex]::Escape($case)) `
        "coordinator native case $case" -CaseSensitive
}

$processFixture = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'tests\Continuity\ContinuityCoordinatorProcessFixture.cpp')
foreach ($boundary in @(
    'CheckpointIntent','CheckpointPersisted','SuccessorCreateIntent',
    'CreateEffectBeforeCommit','SuccessorCommit','BootstrapIntent',
    'BootstrapEffectBeforeAcknowledgement','AcknowledgementCommit',
    'PredecessorSealIntent','CompletedPointerCommit')) {
    Assert-Match $processFixture ("CrashBoundary::" + $boundary) `
        "abrupt process boundary $boundary" -CaseSensitive
}
Assert-Match $processFixture 'CreateProcessW\s*\(' `
    'native child process creation' -CaseSensitive
Assert-Match $processFixture 'TerminateProcess\s*\(' `
    'abrupt native termination' -CaseSensitive
Assert-Match $processFixture 'WindowsProjectMemoryRepository::open' `
    'process fixture uses real Windows project repository' -CaseSensitive
Assert-Match $processFixture 'L"recover"' `
    'process fixture uses a separate recovery child' -CaseSensitive
Assert-Match $processFixture 'transitionCount\([\s\S]*?==\s*9U' `
    'recovery verifies exact transition count' -CaseSensitive

$cmake = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
$g11Tests = @(
    'ForgeConductor.Continuity.DomainUnitTests',
    'ForgeConductor.Continuity.ContractTests',
    'ForgeConductor.Continuity.ProjectMigrationRegressionTests',
    'ForgeConductor.Continuity.RepositoryAggregateCacheTests',
    'ForgeConductor.Continuity.DocumentCodecTests',
    'ForgeConductor.Continuity.LegacyApplicationTests',
    'ForgeConductor.Continuity.CoordinatorTests',
    'ForgeConductor.Continuity.ProcessFixtureTests',
    'ForgeConductor.Continuity.RepositoryWindowsTests',
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests')
foreach ($test in $g11Tests) {
    Assert-Match $cmake `
        ('add_test\s*\(\s*NAME\s+' + [regex]::Escape($test)) `
        "CMake test registration $test" -CaseSensitive
    Assert-Match $cmake `
        ([regex]::Escape($test) + '[\s\S]*?LABELS\s+"[^"]*G11[^"]*"') `
        "CMake G11 label $test" -CaseSensitive
}
foreach ($source in @(
    'src/Domain/LegacyContinuityModels.cpp',
    'src/Application/ContinuityCoordinator.cpp',
    'src/Application/LegacyContextContinuityService.cpp',
    'src/Infrastructure/Windows/WindowsContinuityDocumentCodec.cpp',
    'src/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.cpp',
    'src/Persistence/Windows/WindowsLegacyContinuityRepository.cpp')) {
    Assert-Exact ([regex]::Matches($cmake, [regex]::Escape($source)).Count) 1 `
        "single CMake production placement $source"
}

if ($StaticOnly) {
    $frameworkAfter = Get-TreeSummary $frameworkRoot
    Assert-Exact ([string]$frameworkAfter.sha256) `
        ([string]$frameworkBefore.sha256) 'sealed Forsetti unchanged after static G11'
    Invoke-RepositoryIntegrityChecks
    Write-Host "G11 static continuity validation passed: $script:AssertionCount assertions."
    return
}

$buildTargets = @(
    'ForgeConductor.Domain.UnitTests',
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Persistence.UnitTests',
    'ForgeConductor.ProjectMemory.CacheTests',
    'ForgeConductor.Continuity.DocumentCodecTests',
    'ForgeConductor.Continuity.LegacyApplicationTests',
    'ForgeConductor.Continuity.CoordinatorTests',
    'ForgeConductor.Continuity.ProcessFixtureTests',
    'ForgeConductor.Continuity.RepositoryWindowsTests',
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests')
$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G11: performing the one authoritative fresh x64 Debug target build.'
& $buildScript -Configuration Debug -Architecture x64 -Target $buildTargets `
    -Parallel $Parallel -Fresh
Assert-True $? 'one authoritative fresh x64 Debug G11 target build'
Write-Host 'G11: running the one authoritative x64 Debug G11 CTest pass.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G11
Assert-True $? 'one authoritative x64 Debug G11 CTest pass'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$artifacts = @(
    'lib/Debug/ForgeConductor.Domain.lib',
    'lib/Debug/ForgeConductor.Application.lib',
    'lib/Debug/ForgeConductor.Infrastructure.Windows.lib',
    'lib/Debug/ForgeConductor.Persistence.Windows.lib',
    'bin/Debug/ForgeConductor.Domain.UnitTests.exe',
    'bin/Debug/ForgeConductor.Contracts.ContractTests.exe',
    'bin/Debug/ForgeConductor.Persistence.ProcessFixture.exe',
    'bin/Debug/ForgeConductor.Persistence.UnitTests.exe',
    'bin/Debug/ForgeConductor.ProjectMemory.CacheTests.exe',
    'bin/Debug/ForgeConductor.Continuity.DocumentCodecTests.exe',
    'bin/Debug/ForgeConductor.Continuity.LegacyApplicationTests.exe',
    'bin/Debug/ForgeConductor.Continuity.CoordinatorTests.exe',
    'bin/Debug/ForgeConductor.Continuity.ProcessFixtureTests.exe',
    'bin/Debug/ForgeConductor.Continuity.RepositoryWindowsTests.exe',
    'bin/Debug/ForgeConductor.LegacyContinuityPersistence.WindowsTests.exe')
$artifactHashes = [ordered]@{}
foreach ($relative in $artifacts) {
    $path = Join-Path $buildRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G11 artifact exists: $relative"
    Assert-True ([long](Get-Item -LiteralPath $path).Length -gt 0) `
        "G11 artifact nonempty: $relative"
    $hash = Get-FileSha256 $path
    Assert-Match $hash '^[0-9a-f]{64}$' "G11 artifact hash: $relative" -CaseSensitive
    $artifactHashes[$relative] = $hash
    Write-Host "$relative SHA-256: $hash"
    if ($relative.EndsWith('.exe', [StringComparison]::Ordinal)) {
        Assert-X64PortableExecutable $path $relative
    }
}

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after full G11'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after full G11'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after full G11'
Invoke-RepositoryIntegrityChecks
Write-Host "G11 continuity validation passed: $script:AssertionCount assertions; 10/10 G11 CTest registrations, $($artifactHashes.Count) artifact hashes, retained G10 static validation, and the single fresh x64 Debug build/test invocation succeeded."
