$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot\build.ps1" -Configuration Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$payload = Join-Path $root "out\Release\x64\ForgeConductorApp"
Copy-Item "$root\resources\Agents" "$payload\Agents" -Recurse -Force
Copy-Item "$root\resources\Manifests" "$payload\ForsettiManifests" -Recurse -Force

Push-Location (Join-Path $root "installer\ForgeConductor.Setup")
dotnet build -c Release
Pop-Location

Get-ChildItem $root -Recurse -Filter "ForgeConductor-0.8.0-win-x64.msi" | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $dist $_.Name) -Force
    Write-Host "MSI: $($_.FullName)"
}
if (-not (Get-ChildItem $dist -Filter *.msi)) {
    Write-Host "WiX MSI not produced; payload is at $payload"
}
exit 0
