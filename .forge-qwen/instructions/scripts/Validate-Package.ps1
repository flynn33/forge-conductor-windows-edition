[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$manifest = Read-Json (Join-Path $PackageRoot 'MANIFEST.json')
$failures = [System.Collections.Generic.List[string]]::new()
$expected = @{}

foreach ($entry in $manifest.files) {
    $relative = ([string]$entry.path).Replace('/', [IO.Path]::DirectorySeparatorChar)
    $expected[$relative.ToLowerInvariant()] = $true
    $path = Join-Path $PackageRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $failures.Add("missing: $($entry.path)")
        continue
    }
    if ((Get-Item -LiteralPath $path).Length -ne [int64]$entry.bytes) { $failures.Add("size: $($entry.path)") }
    if ((Get-Sha256 $path) -ne [string]$entry.sha256) { $failures.Add("hash: $($entry.path)") }
}

Get-ChildItem -LiteralPath $PackageRoot -File -Recurse | ForEach-Object {
    $relative = Get-RelativePathPortable -Base $PackageRoot -Target $_.FullName
    $key = $relative.ToLowerInvariant()
    if (-not $expected.ContainsKey($key) -and $key -notin @('manifest.json', 'sha256sums.txt')) {
        $failures.Add("unexpected: $relative")
    }
}

$sources = Read-Json (Join-Path $PackageRoot 'inputs\SOURCE-HASHES.json')
foreach ($entry in $sources.files) {
    $path = Join-Path $PackageRoot ("inputs\" + [string]$entry.file)
    if (-not (Test-Path -LiteralPath $path) -or (Get-Sha256 $path) -ne [string]$entry.sha256) {
        $failures.Add("source: $($entry.file)")
    }
}

if ($failures.Count -gt 0) {
    $failures | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
    throw 'Package validation failed.'
}
Write-Host "Package validation passed: $($manifest.files.Count) files."
