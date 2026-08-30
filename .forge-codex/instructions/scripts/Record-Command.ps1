[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][string]$Phase,
    [Parameter(Mandatory)][string]$Role,
    [Parameter(Mandatory)][string]$Command,
    [string]$WorkingDirectory = $WorkspaceRoot,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 1800,
    [ValidateRange(1024, 1073741824)]
    [long]$MaximumOutputBytesPerStream = 67108864,
    [switch]$AllowFailure
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$state = Join-Path $WorkspaceRoot '.forge-codex\state'
$commandDir = Join-Path $state 'commands'
New-Item -ItemType Directory -Force -Path $commandDir | Out-Null
$id = '{0}-{1}' -f ([DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')), ([Guid]::NewGuid().ToString('N').Substring(0,8))
$outPath = Join-Path $commandDir "$id.stdout.txt"
$errPath = Join-Path $commandDir "$id.stderr.txt"
$recordPath = Join-Path $commandDir "$id.json"
$outTempPath = "$outPath.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
$errTempPath = "$errPath.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
$exitPath = Join-Path $commandDir "$id.exit.txt"
$escapedExitPath = $exitPath.Replace("'", "''")

$start = Get-UtcTimestamp
$wrappedCommand = @"
`$ErrorActionPreference = 'Stop'
try {
    & {
        try {
$Command
        } finally {
            `$script:forgeRecordedCommandSucceeded = `$?
            `$script:forgeRecordedNativeExitCode = `$LASTEXITCODE
        }
    }
    if (`$script:forgeRecordedCommandSucceeded) {
        `$forgeRecordedExitCode = 0
    } elseif (`$null -ne `$script:forgeRecordedNativeExitCode -and
              [int]`$script:forgeRecordedNativeExitCode -ne 0) {
        `$forgeRecordedExitCode = [int]`$script:forgeRecordedNativeExitCode
    } else {
        `$forgeRecordedExitCode = 1
    }
} catch {
    Write-Error -ErrorRecord `$_ -ErrorAction Continue
    `$forgeRecordedExitCode = 1
}
[System.IO.File]::WriteAllText(
    '$escapedExitPath',
    [string]`$forgeRecordedExitCode,
    (New-Object System.Text.UTF8Encoding(`$false)))
exit `$forgeRecordedExitCode
"@
$encoded = [Convert]::ToBase64String(
    [Text.Encoding]::Unicode.GetBytes($wrappedCommand))
$forgeRecordedProcess = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList @(
        '-NoLogo',
        '-NoProfile',
        '-NonInteractive',
        '-EncodedCommand',
        $encoded) `
    -WorkingDirectory $WorkingDirectory `
    -RedirectStandardOutput $outTempPath `
    -RedirectStandardError $errTempPath `
    -PassThru

function Get-RecordedProcessTreeIds {
    param([Parameter(Mandatory)][int]$RootProcessId)

    $known = @{}
    $known[$RootProcessId] = $true
    try {
        $snapshot = @(
            Get-CimInstance Win32_Process -OperationTimeoutSec 2)
        do {
            $added = $false
            foreach ($candidate in $snapshot) {
                $candidateId = [int]$candidate.ProcessId
                $parentId = [int]$candidate.ParentProcessId
                if ($known.ContainsKey($parentId) -and
                    -not $known.ContainsKey($candidateId)) {
                    $known[$candidateId] = $true
                    $added = $true
                }
            }
        } while ($added)
    } catch {
        # taskkill /T remains the authoritative whole-tree operation.
    }
    return @($known.Keys | ForEach-Object { [int]$_ })
}

function Stop-RecordedProcessTree {
    param(
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$Process
    )

    $treeIds = @(Get-RecordedProcessTreeIds -RootProcessId $Process.Id)
    if (-not $Process.HasExited) {
        $taskKill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
        $taskKillStart = New-Object System.Diagnostics.ProcessStartInfo
        $taskKillStart.FileName = $taskKill
        $taskKillStart.Arguments = "/PID $($Process.Id) /T /F"
        $taskKillStart.UseShellExecute = $false
        $taskKillStart.CreateNoWindow = $true
        $taskKillStart.RedirectStandardOutput = $true
        $taskKillStart.RedirectStandardError = $true
        $taskKillProcess = New-Object System.Diagnostics.Process
        $taskKillProcess.StartInfo = $taskKillStart
        try {
            if ($taskKillProcess.Start()) {
                if (-not $taskKillProcess.WaitForExit(3000)) {
                    try { $taskKillProcess.Kill() } catch {}
                    try { $taskKillProcess.WaitForExit(1000) } catch {}
                }
            }
        } catch {
            # The bounded captured-lineage fallback below remains authoritative.
        } finally {
            $taskKillProcess.Dispose()
        }
    }

    # A parent can exit between the poll and taskkill while a descendant
    # remains. Refresh the recorded lineage and force-stop every survivor;
    # repeat a bounded number of times so a late child cannot escape.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $treeIds = @(
            $treeIds
            Get-RecordedProcessTreeIds -RootProcessId $Process.Id
        ) | Sort-Object -Unique
        $liveIds = @($treeIds | Where-Object {
            $null -ne (Get-Process -Id $_ -ErrorAction SilentlyContinue)
        })
        if ($liveIds.Count -eq 0) { break }
        $orderedIds = @($liveIds | Sort-Object {
            if ($_ -eq $Process.Id) { 1 } else { 0 }
        })
        foreach ($processId in $orderedIds) {
            try {
                Stop-Process -Id $processId -Force -ErrorAction Stop
            } catch {
                if ($null -ne (Get-Process -Id $processId `
                        -ErrorAction SilentlyContinue)) {
                    throw
                }
            }
        }
        Start-Sleep -Milliseconds 100
    }

    $parentStopped = $Process.HasExited -or $Process.WaitForExit(1000)
    $treeIds = @(
        $treeIds
        Get-RecordedProcessTreeIds -RootProcessId $Process.Id
    ) | Sort-Object -Unique
    $survivors = @($treeIds | Where-Object {
        $null -ne (Get-Process -Id $_ -ErrorAction SilentlyContinue)
    })
    if (-not $parentStopped -or $survivors.Count -ne 0) {
        throw 'The recorded command process tree did not stop within the bounded termination window.'
    }
}

function Get-RecordedFileLength {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return 0L }
    return [long](Get-Item -LiteralPath $Path).Length
}

function Limit-RecordedFileLength {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][long]$MaximumBytes
    )

    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::Read)
    try {
        if ($stream.Length -le $MaximumBytes) { return $false }
        $stream.SetLength($MaximumBytes)
        return $true
    } finally {
        $stream.Dispose()
    }
}

$timedOut = $false
$outputLimitExceeded = $false
$deadlineUtc = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$processExited = $false
while (-not $processExited) {
    $processExited = $forgeRecordedProcess.WaitForExit(250)
    $stdoutLength = Get-RecordedFileLength -Path $outTempPath
    $stderrLength = Get-RecordedFileLength -Path $errTempPath
    if ($stdoutLength -gt $MaximumOutputBytesPerStream -or
        $stderrLength -gt $MaximumOutputBytesPerStream) {
        $outputLimitExceeded = $true
        break
    }
    if (-not $processExited -and [DateTime]::UtcNow -ge $deadlineUtc) {
        $timedOut = $true
        break
    }
}
if (-not $processExited -and ($timedOut -or $outputLimitExceeded)) {
    Stop-RecordedProcessTree -Process $forgeRecordedProcess
    $processExited = $true
}
if ($processExited) {
    $forgeRecordedProcess.WaitForExit()
}

$stdoutLength = Get-RecordedFileLength -Path $outTempPath
$stderrLength = Get-RecordedFileLength -Path $errTempPath
if ($stdoutLength -gt $MaximumOutputBytesPerStream -or
    $stderrLength -gt $MaximumOutputBytesPerStream) {
    $outputLimitExceeded = $true
}
$stdoutTruncated = Limit-RecordedFileLength `
    -Path $outTempPath `
    -MaximumBytes $MaximumOutputBytesPerStream
$stderrTruncated = Limit-RecordedFileLength `
    -Path $errTempPath `
    -MaximumBytes $MaximumOutputBytesPerStream

if ($outputLimitExceeded) {
    $exitCode = 125
    $exitCodeSource = 'output-limit'
} elseif ($timedOut) {
    $exitCode = 124
    $exitCodeSource = 'timeout'
} else {
    if (Test-Path -LiteralPath $exitPath) {
        $exitText = [System.IO.File]::ReadAllText($exitPath).Trim()
        $parsedExitCode = 0
        if (-not [int]::TryParse($exitText, [ref]$parsedExitCode)) {
            throw 'The recorded command wrote an invalid exit-code sidecar.'
        }
        $exitCode = $parsedExitCode
        $exitCodeSource = 'sidecar'
    } else {
        $forgeRecordedProcess.Refresh()
        $publishedProcessExitCode = $forgeRecordedProcess.ExitCode
        if ($null -ne $publishedProcessExitCode) {
            $exitCode = [int]$publishedProcessExitCode
            $exitCodeSource = 'process'
        } else {
            $exitCode = 125
            $exitCodeSource = 'unavailable'
        }
    }
}
if (-not (Test-Path -LiteralPath $outTempPath)) {
    Write-AtomicUtf8 -Path $outTempPath -Content ''
}
if (-not (Test-Path -LiteralPath $errTempPath)) {
    Write-AtomicUtf8 -Path $errTempPath -Content ''
}
Move-Item -LiteralPath $outTempPath -Destination $outPath -Force
Move-Item -LiteralPath $errTempPath -Destination $errPath -Force
if (Test-Path -LiteralPath $exitPath) {
    Remove-Item -LiteralPath $exitPath -Force
}
$end = Get-UtcTimestamp

$record = [ordered]@{
    schema_version = 1
    id = $id
    phase = $Phase
    role = $Role
    command = $Command
    working_directory = $WorkingDirectory
    start_utc = $start
    end_utc = $end
    exit_code = $exitCode
    exit_code_source = $exitCodeSource
    timed_out = $timedOut
    maximum_output_bytes_per_stream = $MaximumOutputBytesPerStream
    output_limit_exceeded = $outputLimitExceeded
    stdout_truncated = $stdoutTruncated
    stderr_truncated = $stderrTruncated
    stdout = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $outPath
    stderr = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $errPath
    stdout_sha256 = Get-FileSha256 $outPath
    stderr_sha256 = Get-FileSha256 $errPath
}
Write-JsonFileAtomic -Path $recordPath -Value $record
Add-LedgerEvent -StateRoot $state -Role $Role -Phase $Phase -Action 'command_completed' -Data @{
    record = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $recordPath
    exit_code = $exitCode
    timed_out = $timedOut
} | Out-Null

$record | ConvertTo-Json -Depth 20
if ($exitCode -ne 0 -and -not $AllowFailure) {
    throw "Command failed with exit code $exitCode. Evidence: $recordPath"
}
