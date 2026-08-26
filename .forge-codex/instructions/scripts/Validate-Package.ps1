[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$manifestPath = Join-Path $PackageRoot 'MANIFEST.json'
$manifest = Read-JsonFile $manifestPath
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
    $length = (Get-Item -LiteralPath $path).Length
    if ($length -ne [int64]$entry.bytes) {
        $failures.Add("size mismatch: $($entry.path)")
    }
    $hash = Get-FileSha256 $path
    if ($hash -ne [string]$entry.sha256) {
        $failures.Add("hash mismatch: $($entry.path)")
    }
}

$allowedMetadata = @('manifest.json','sha256sums.txt')
Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | ForEach-Object {
    $relative = Get-RelativePathPortable -BasePath $PackageRoot -TargetPath $_.FullName
    $key = $relative.ToLowerInvariant()
    if (-not $expected.ContainsKey($key) -and $key -notin $allowedMetadata) {
        $failures.Add("unexpected file: $relative")
    }
}

$sourceHashPath = Join-Path $PackageRoot 'inputs\SOURCE-HASHES.json'
$sourceHashes = Read-JsonFile $sourceHashPath
foreach ($entry in $sourceHashes.files) {
    $path = Join-Path (Join-Path $PackageRoot 'inputs') ([string]$entry.file)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $failures.Add("missing immutable source: $($entry.file)")
        continue
    }
    if ((Get-Item -LiteralPath $path).Length -ne [int64]$entry.bytes) {
        $failures.Add("immutable source size mismatch: $($entry.file)")
    }
    if ((Get-FileSha256 $path) -ne [string]$entry.sha256) {
        $failures.Add("immutable source hash mismatch: $($entry.file)")
    }
}

if ($failures.Count -gt 0) {
    $failures | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
    throw "Package validation failed with $($failures.Count) error(s)."
}

Write-Host "Package validation passed: $($manifest.files.Count) manifested files and $($sourceHashes.files.Count) immutable sources."
