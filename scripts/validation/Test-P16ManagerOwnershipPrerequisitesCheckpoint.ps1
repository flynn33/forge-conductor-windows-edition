[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot
)

$global:LASTEXITCODE = 0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Checkpoint {
    param(
        [Parameter(Mandatory)][bool]$Condition,
        [Parameter(Mandatory)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-CheckpointSha256 {
    param([Parameter(Mandatory)][string]$Path)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead(
        (Resolve-Path -LiteralPath $Path).Path)
    try {
        $hash = $algorithm.ComputeHash($stream)
        return ([BitConverter]::ToString($hash)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Assert-CrlfUtf8NoBom {
    param([Parameter(Mandatory)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes(
        (Resolve-Path -LiteralPath $Path))
    $hasBom = $bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and
        $bytes[2] -eq 0xBF
    Assert-Checkpoint (-not $hasBom) "$Path has a UTF-8 BOM"

    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10) {
            Assert-Checkpoint (
                $index -gt 0 -and $bytes[$index - 1] -eq 13) `
                "$Path contains an LF not preceded by CR"
        }
        if ($bytes[$index] -eq 13) {
            Assert-Checkpoint (
                $index + 1 -lt $bytes.Length -and
                    $bytes[$index + 1] -eq 10) `
                "$Path contains a CR not followed by LF"
        }
    }

    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $lines = $text.Split(
        [string[]]@([Environment]::NewLine),
        [StringSplitOptions]::None)
    foreach ($line in $lines) {
        Assert-Checkpoint (
            -not $line.EndsWith(' ') -and
                -not $line.EndsWith([string][char]9)) `
            "$Path contains trailing whitespace"
    }
}

$workspace = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$evidencePath = Join-Path $workspace `
    '.forge-codex\state\evidence\P16\manager-production-ownership-prerequisites-checkpoint.json'
$runStatePath = Join-Path $workspace '.forge-codex\state\run-state.json'
$evidence = Get-Content -Raw -LiteralPath $evidencePath | ConvertFrom-Json
$runState = Get-Content -Raw -LiteralPath $runStatePath | ConvertFrom-Json

Assert-Checkpoint ($evidence.schema_version -eq 1) `
    'checkpoint schema mismatch'
Assert-Checkpoint ($evidence.phase -eq 'P16') `
    'checkpoint phase mismatch'
Assert-Checkpoint ($evidence.status -eq 'passed') `
    'checkpoint is not passed'
Assert-Checkpoint (
    $evidence.gate_status -eq 'focused-development-checkpoint-not-G16') `
    'checkpoint gate scope mismatch'

$sourceMatched = 0
foreach ($property in $evidence.source_sha256.PSObject.Properties) {
    $path = Join-Path $workspace $property.Name
    Assert-Checkpoint (Test-Path -LiteralPath $path -PathType Leaf) `
        "missing source evidence path: $($property.Name)"
    Assert-Checkpoint (
        (Get-CheckpointSha256 $path) -eq [string]$property.Value) `
        "source hash mismatch: $($property.Name)"
    $sourceMatched++
}
Assert-Checkpoint ($sourceMatched -eq 48) `
    "expected 48 source hashes, found $sourceMatched"

$binaryMatched = 0
foreach ($property in $evidence.binary_sha256.PSObject.Properties) {
    $path = Join-Path $workspace $property.Name
    Assert-Checkpoint (Test-Path -LiteralPath $path -PathType Leaf) `
        "missing binary evidence path: $($property.Name)"
    Assert-Checkpoint (
        (Get-CheckpointSha256 $path) -eq [string]$property.Value) `
        "binary hash mismatch: $($property.Name)"
    $binaryMatched++
}
Assert-Checkpoint ($binaryMatched -eq 19) `
    "expected 19 binary hashes, found $binaryMatched"

$recordBindings = @(
    [pscustomobject]@{
        Name = 'build'
        Value = $evidence.recorded_build
    },
    [pscustomobject]@{
        Name = 'focused'
        Value = $evidence.recorded_validation
    },
    [pscustomobject]@{
        Name = 'stress'
        Value = $evidence.recorded_stress_validation
    }
)
$commandRecordsMatched = 0
foreach ($binding in $recordBindings) {
    $value = $binding.Value
    $recordPath = Join-Path $workspace ([string]$value.command_record)
    Assert-Checkpoint (Test-Path -LiteralPath $recordPath -PathType Leaf) `
        "missing $($binding.Name) command record"
    Assert-Checkpoint (
        (Get-CheckpointSha256 $recordPath) -eq
            [string]$value.command_record_sha256) `
        "$($binding.Name) command record hash mismatch"

    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    Assert-Checkpoint ($record.phase -eq 'P16') `
        "$($binding.Name) phase mismatch"
    Assert-Checkpoint ($record.working_directory -eq $workspace) `
        "$($binding.Name) working-directory mismatch"
    Assert-Checkpoint ($record.exit_code -eq 0) `
        "$($binding.Name) exit code is not zero"
    Assert-Checkpoint (-not $record.timed_out) `
        "$($binding.Name) timed out"
    Assert-Checkpoint (-not $record.output_limit_exceeded) `
        "$($binding.Name) exceeded output limit"
    Assert-Checkpoint (-not $record.stdout_truncated) `
        "$($binding.Name) stdout truncated"
    Assert-Checkpoint (-not $record.stderr_truncated) `
        "$($binding.Name) stderr truncated"

    foreach ($streamName in @('stdout', 'stderr')) {
        $streamPath = Join-Path $workspace ([string]$record.$streamName)
        Assert-Checkpoint (
            Test-Path -LiteralPath $streamPath -PathType Leaf) `
            "missing $($binding.Name) $streamName"
        $hashField = $streamName + '_sha256'
        Assert-Checkpoint (
            (Get-CheckpointSha256 $streamPath) -eq
                [string]$record.$hashField) `
            "$($binding.Name) $streamName record hash mismatch"
        Assert-Checkpoint (
            [string]$record.$hashField -eq [string]$value.$hashField) `
            "$($binding.Name) $streamName checkpoint hash mismatch"
    }

    Assert-Checkpoint (
        $record.command -notmatch `
            "(^|\s)(-L|--label-regex)(=|\s+)G16") `
        "$($binding.Name) command invoked the G16 label gate"
    $commandRecordsMatched++
}
Assert-Checkpoint ($commandRecordsMatched -eq 3) `
    'expected three command records'

$buildRecord = Get-Content -Raw -LiteralPath (
    Join-Path $workspace ([string]$evidence.recorded_build.command_record)) |
    ConvertFrom-Json
Assert-Checkpoint ($buildRecord.role -eq 'Implementer') `
    'build role mismatch'
Assert-Checkpoint ($buildRecord.command -match '^cmake --build ') `
    'build command mismatch'
Assert-Checkpoint ($buildRecord.command -match '--config Release') `
    'build configuration mismatch'
Assert-Checkpoint ($buildRecord.command -match '--parallel 2') `
    'build parallel bound mismatch'
foreach ($target in $evidence.recorded_build.targets) {
    Assert-Checkpoint ($buildRecord.command.Contains([string]$target)) `
        "build target missing from recorded command: $target"
}

$focusedRecord = Get-Content -Raw -LiteralPath (
    Join-Path $workspace (
        [string]$evidence.recorded_validation.command_record)) |
    ConvertFrom-Json
Assert-Checkpoint ($focusedRecord.role -eq 'Implementer') `
    'focused validation role mismatch'
Assert-Checkpoint ($focusedRecord.command -match '^ctest ') `
    'focused CTest command mismatch'
Assert-Checkpoint ($focusedRecord.command -match ' -R ') `
    'focused selection is not regex-bound'
Assert-Checkpoint ($focusedRecord.command -notmatch '--repeat') `
    'focused selection unexpectedly repeated tests'
$focusedOutput = Get-Content -Raw -LiteralPath (
    Join-Path $workspace ([string]$focusedRecord.stdout))
Assert-Checkpoint (
    $focusedOutput.Contains('100% tests passed out of 13')) `
    'focused CTest summary mismatch'
$focusedPassedLines = ([regex]::Matches(
    $focusedOutput,
    '(?m)^\s*\d+/13 Test\s+#[^\r\n]+Passed\s+')).Count
Assert-Checkpoint ($focusedPassedLines -eq 13) `
    "expected 13 focused Passed lines, found $focusedPassedLines"

$stressRecord = Get-Content -Raw -LiteralPath (
    Join-Path $workspace (
        [string]$evidence.recorded_stress_validation.command_record)) |
    ConvertFrom-Json
Assert-Checkpoint ($stressRecord.role -eq 'Validator') `
    'stress validation role mismatch'
Assert-Checkpoint (
    $stressRecord.command -match '--repeat until-fail:50') `
    'stress repetition mismatch'
Assert-Checkpoint ($stressRecord.command -match '--parallel 7') `
    'stress parallel bound mismatch'
Assert-Checkpoint ($stressRecord.command -match ' -R ') `
    'stress selection is not regex-bound'
$stressOutput = Get-Content -Raw -LiteralPath (
    Join-Path $workspace ([string]$stressRecord.stdout))
Assert-Checkpoint (
    $stressOutput.Contains('100% tests passed out of 7')) `
    'stress CTest summary mismatch'
$stressPassedLines = ([regex]::Matches(
    $stressOutput,
    '(?m)^\s*(?:\d+/7 )?Test\s+#[^\r\n]+Passed\s+')).Count
Assert-Checkpoint ($stressPassedLines -eq 350) `
    "expected 350 stress Passed lines, found $stressPassedLines"

$p16 = @($runState.phases | Where-Object id -eq 'P16')
$g16 = @($runState.gates | Where-Object id -eq 'G16')
Assert-Checkpoint ($p16.Count -eq 1) `
    'P16 durable state is missing or duplicated'
Assert-Checkpoint ($g16.Count -eq 1) `
    'G16 durable state is missing or duplicated'
Assert-Checkpoint ($p16[0].status -eq 'in_progress') `
    'P16 must remain in progress'
Assert-Checkpoint ($g16[0].status -eq 'not_started') `
    'G16 must remain not started'
Assert-Checkpoint (@($g16[0].evidence).Count -eq 0) `
    'G16 evidence must remain empty'
Assert-Checkpoint ($runState.active_role -eq 'Implementer') `
    'active role mismatch'
Assert-Checkpoint ($runState.active_phase -eq 'P16') `
    'active phase mismatch'

$requiredP16Evidence = @(
    '.forge-codex/state/decisions/P16-032-lease-ordered-manager-process-environment-and-terminal-ownership.md',
    '.forge-codex/state/decisions/P16-033-post-listener-browser-and-event-driven-stop-edge.md',
    '.forge-codex/state/decisions/P16-034-authority-bound-lm-studio-selected-read-scope.md',
    '.forge-codex/state/decisions/P16-035-capacity-one-manager-maintenance-reconciliation.md',
    '.forge-codex/state/evidence/P16/manager-production-ownership-prerequisites-checkpoint.json',
    '.forge-codex/state/commands/20260830T165116831Z-885aeb41.json',
    '.forge-codex/state/commands/20260830T165303729Z-2f441ec2.json',
    '.forge-codex/state/commands/20260830T165326310Z-97faef7a.json'
)
foreach ($required in $requiredP16Evidence) {
    Assert-Checkpoint (@($p16[0].evidence) -contains $required) `
        "P16 durable evidence is missing $required"
}

$lineEndingPaths = @(
    @($evidence.source_sha256.PSObject.Properties |
        ForEach-Object { $_.Name })
    '.forge-codex/state/evidence/P16/manager-production-ownership-prerequisites-checkpoint.json'
    '.forge-codex/state/run-state.json'
)
foreach ($path in $lineEndingPaths) {
    Assert-CrlfUtf8NoBom -Path (Join-Path $workspace $path)
}

$workingDiffOutput = @(
    & git -c core.safecrlf=false -C $workspace diff --check -- . `
        ':(exclude).forge-codex/state/commands/**' 2>&1)
$workingDiffExitCode = $LASTEXITCODE
Assert-Checkpoint ($workingDiffExitCode -eq 0) (
    'governed worktree git diff --check failed: ' +
        ($workingDiffOutput -join [Environment]::NewLine))

$stagedDiffOutput = @(
    & git -c core.safecrlf=false -C $workspace diff --cached --check -- . `
        ':(exclude).forge-codex/state/commands/**' 2>&1)
$stagedDiffExitCode = $LASTEXITCODE
Assert-Checkpoint ($stagedDiffExitCode -eq 0) (
    'governed staged git diff --check failed: ' +
        ($stagedDiffOutput -join [Environment]::NewLine))

& (Join-Path $workspace `
    '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
    -WorkspaceRoot $workspace
Assert-Checkpoint ($?) 'no-Python validation failed'

& (Join-Path $workspace `
    '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
    -WorkspaceRoot $workspace
Assert-Checkpoint ($?) 'no-attribution validation failed'

& (Join-Path $workspace `
    '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
    -WorkspaceRoot $workspace
Assert-Checkpoint ($?) 'ledger validation failed'

Write-Output (
    "P16 Manager ownership checkpoint policy passed: $sourceMatched " +
        "source hashes, $binaryMatched binary hashes, " +
        "$commandRecordsMatched command records, 13 focused tests, and " +
        "$stressPassedLines repeated lifecycle executions, and both " +
        'worktree and staged governed text passed diff checks. P16 remains ' +
        'in progress; G16 remains not started.')
