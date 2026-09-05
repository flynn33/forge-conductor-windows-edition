[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [Parameter(Mandatory)][string]$Microtask,
    [Parameter(Mandatory)][string]$Command,
    [string]$WorkingDirectory,
    [int]$TimeoutSeconds = 1800,
    [switch]$AllowFailure,
    [string]$Role = 'builder'
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"
$Repository = Get-RepoRoot -Start $Repository
if (-not $WorkingDirectory) { $WorkingDirectory = $Repository }
$directory = Join-Path $Repository '.forge-qwen\state\evidence\commands'
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$id = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$stdout = Join-Path $directory "$id.stdout.txt"
$stderr = Join-Path $directory "$id.stderr.txt"
$recordPath = Join-Path $directory "$id.json"
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command))
$start = Get-UtcNow
$process = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive','-EncodedCommand',$encoded) -WorkingDirectory $WorkingDirectory -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
if ($timedOut) {
    try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {}
    $exitCode = 124
}
else {
    $process.WaitForExit()
    $exitCode = $process.ExitCode
}
if (-not (Test-Path -LiteralPath $stdout)) { Write-AtomicUtf8 -Path $stdout -Content '' }
if (-not (Test-Path -LiteralPath $stderr)) { Write-AtomicUtf8 -Path $stderr -Content '' }
$record = [ordered]@{
    id=$id; microtask_id=$Microtask; role=$Role; command=$Command; working_directory=$WorkingDirectory
    start_utc=$start; end_utc=Get-UtcNow; exit_code=$exitCode; timed_out=$timedOut
    stdout=(Get-RelativePathPortable -Base $Repository -Target $stdout)
    stderr=(Get-RelativePathPortable -Base $Repository -Target $stderr)
    stdout_sha256=Get-Sha256 $stdout; stderr_sha256=Get-Sha256 $stderr
}
Write-JsonAtomic -Path $recordPath -Value $record
Add-QwenLedgerEvent -Repository $Repository -Role $Role -Microtask $Microtask -Action 'command_completed' -Data @{ record=(Get-RelativePathPortable -Base $Repository -Target $recordPath); exit_code=$exitCode } | Out-Null
$record | ConvertTo-Json -Depth 20
if ($exitCode -ne 0 -and -not $AllowFailure) { throw "Command failed. Evidence: $recordPath" }
