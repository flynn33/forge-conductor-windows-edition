[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][string]$Phase,
    [Parameter(Mandatory)][string]$Role,
    [Parameter(Mandatory)][string]$Command,
    [string]$WorkingDirectory = $WorkspaceRoot,
    [int]$TimeoutSeconds = 1800,
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

$start = Get-UtcTimestamp
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command))
$process = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive','-EncodedCommand',$encoded) `
    -WorkingDirectory $WorkingDirectory `
    -RedirectStandardOutput $outPath `
    -RedirectStandardError $errPath `
    -PassThru

$timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
if ($timedOut) {
    try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {}
    $exitCode = 124
} else {
    # Complete redirected stream flushing before reading hashes.
    $process.WaitForExit()
    $exitCode = $process.ExitCode
}
if (-not (Test-Path -LiteralPath $outPath)) { Write-AtomicUtf8 -Path $outPath -Content '' }
if (-not (Test-Path -LiteralPath $errPath)) { Write-AtomicUtf8 -Path $errPath -Content '' }
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
    timed_out = $timedOut
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
