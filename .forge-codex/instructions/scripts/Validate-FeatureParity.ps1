[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$matrixPath = Join-Path $WorkspaceRoot ".forge-codex\instructions\plans\feature-parity-matrix.json"
$matrix = Read-JsonFile $matrixPath
$invalid = @($matrix.features | Where-Object { $_.status -ne "passed" })
if ($invalid.Count -gt 0) {
    $invalid | ForEach-Object { Write-Error "$($_.id) $($_.feature): $($_.status)" }
    throw "$($invalid.Count) feature-parity row(s) are not passed."
}
Write-Host "Feature parity passed: $($matrix.features.Count) rows."
