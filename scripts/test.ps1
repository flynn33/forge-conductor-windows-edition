param([string]$Configuration = "Debug")
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "out\$Configuration\x64\ForgeConductor.Tests\ForgeConductor.Tests.exe"
$app = Join-Path $root "out\$Configuration\x64\ForgeConductorApp\ForgeConductor.exe"
if (-not (Test-Path $app)) {
    $app = Join-Path $root "out\$Configuration\x64\ForgeConductorApp\ForgeConductorApp.exe"
}
$smoke = Join-Path $root "out\$Configuration\x64\ForgeConductor.McpSmoke\ForgeConductor.McpSmoke.exe"
if (-not (Test-Path $exe)) { throw "Tests not built: $exe" }
Push-Location $root
& $exe
$code = $LASTEXITCODE
if (Test-Path $smoke) {
    & $smoke $app
    if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE }
}
Pop-Location
exit $code
