$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot\build.ps1" -Configuration Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$payload = Join-Path $root "out\Release\x64\ForgeConductorApp"
New-Item -ItemType Directory -Force -Path "$payload\Agents" | Out-Null
New-Item -ItemType Directory -Force -Path "$payload\ForsettiManifests" | Out-Null
Copy-Item "$root\resources\Agents\*" "$payload\Agents" -Force
Copy-Item "$root\resources\Manifests\*" "$payload\ForsettiManifests" -Force

Push-Location (Join-Path $root "installer\ForgeConductor.Setup")
dotnet build -c Release
Pop-Location

$built = Get-ChildItem (Join-Path $root "installer\ForgeConductor.Setup") -Recurse -Filter "ForgeConductor-*-win-x64.msi" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($built) {
    $dest = Join-Path $dist $built.Name
    if ($built.FullName -ne $dest) {
        Copy-Item $built.FullName $dest -Force
    }
    Write-Host "MSI: $dest"
} elseif (-not (Get-ChildItem $dist -Filter *.msi -ErrorAction SilentlyContinue)) {
    Write-Host "WiX MSI not produced; payload is at $payload"
}
exit 0
