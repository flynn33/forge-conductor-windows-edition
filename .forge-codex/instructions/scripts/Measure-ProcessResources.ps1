[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProcessName,
    [int]$DurationSeconds = 300,
    [int]$IntervalMilliseconds = 1000,
    [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = "Stop"
$rows = [System.Collections.Generic.List[object]]::new()
$end = [DateTimeOffset]::UtcNow.AddSeconds($DurationSeconds)
while ([DateTimeOffset]::UtcNow -lt $end) {
    $processes = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    foreach ($process in $processes) {
        $rows.Add([ordered]@{
            utc=[DateTimeOffset]::UtcNow.ToString("o")
            id=$process.Id
            private_bytes=$process.PrivateMemorySize64
            working_set=$process.WorkingSet64
            handles=$process.HandleCount
            threads=$process.Threads.Count
            cpu_seconds=$process.CPU
        })
    }
    Start-Sleep -Milliseconds $IntervalMilliseconds
}
$directory = Split-Path -Parent $OutputPath
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$rows | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 -LiteralPath $OutputPath
Write-Host $OutputPath
