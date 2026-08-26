[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputEtl,
    [int]$DurationSeconds = 60,
    [string]$Profile = "GeneralProfile"
)
$ErrorActionPreference = "Stop"
$wpr = Get-Command wpr.exe -ErrorAction Stop
& $wpr.Source -cancel 2>$null | Out-Null
& $wpr.Source -start $Profile -filemode
if ($LASTEXITCODE -ne 0) { throw "WPR start failed." }
try {
    Start-Sleep -Seconds $DurationSeconds
} finally {
    & $wpr.Source -stop $OutputEtl
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $OutputEtl)) {
    throw "WPR capture failed."
}
Write-Host $OutputEtl
