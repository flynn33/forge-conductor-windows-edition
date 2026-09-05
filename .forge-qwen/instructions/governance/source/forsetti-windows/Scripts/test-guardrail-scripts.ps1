# Forsetti Framework - Guardrail Script Regression Tests
# Copyright (c) 2026 James Daley. All Rights Reserved.
#
# Exercises script-only guardrail behavior that is difficult to cover from
# native C++ tests.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$violations = @()

Write-Host "=== Guardrail Script Regression Tests ===" -ForegroundColor Cyan

function New-TestRepoRoot {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("forsetti-script-guardrail-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path (Join-Path $path "src\TestModule\ForsettiManifests") -Force | Out-Null
    return $path
}

function New-ManifestJson {
    param(
        [string[]]$SupportedPlatforms,
        [string[]]$Capabilities = @("storage")
    )

    return [ordered]@{
        schemaVersion = "1.0"
        moduleID = "com.test.module"
        displayName = "Test Module"
        moduleVersion = [ordered]@{
            major = 1
            minor = 0
            patch = 0
            prerelease = $null
        }
        moduleType = "service"
        supportedPlatforms = $SupportedPlatforms
        minForsettiVersion = [ordered]@{
            major = 0
            minor = 1
            patch = 0
            prerelease = $null
        }
        maxForsettiVersion = $null
        capabilitiesRequested = $Capabilities
        iapProductID = $null
        entryPoint = "TestModule"
    } | ConvertTo-Json -Depth 8
}

function Invoke-ManifestCheck {
    param(
        [string]$TestRepoRoot
    )

    $scriptPath = Join-Path $repoRoot "Scripts\check-manifests.ps1"
    $output = & pwsh -NoProfile -ExecutionPolicy Bypass -File $scriptPath -RepoRoot $TestRepoRoot 2>&1
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = ($output | Out-String)
    }
}

function Invoke-ManifestCase {
    param(
        [string]$Name,
        [string[]]$SupportedPlatforms,
        [string[]]$Capabilities,
        [int]$ExpectedExitCode
    )

    $testRoot = New-TestRepoRoot
    try {
        $manifestPath = Join-Path $testRoot "src\TestModule\ForsettiManifests\TestModule.json"
        New-ManifestJson -SupportedPlatforms $SupportedPlatforms -Capabilities $Capabilities |
            Set-Content -Path $manifestPath -Encoding UTF8

        $result = Invoke-ManifestCheck -TestRepoRoot $testRoot
        if ($result.ExitCode -ne $ExpectedExitCode) {
            $script:violations += "$Name - expected exit code $ExpectedExitCode, got $($result.ExitCode). Output: $($result.Output)"
        }
    } finally {
        Remove-Item -Path $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Invoke-ManifestCase `
    -Name "Exact Windows casing passes" `
    -SupportedPlatforms @("Windows") `
    -Capabilities @("storage") `
    -ExpectedExitCode 0

Invoke-ManifestCase `
    -Name "Lowercase windows fails" `
    -SupportedPlatforms @("windows") `
    -Capabilities @("storage") `
    -ExpectedExitCode 1

Invoke-ManifestCase `
    -Name "Mixed unsupported platform fails" `
    -SupportedPlatforms @("Windows", "iOS") `
    -Capabilities @("storage") `
    -ExpectedExitCode 1

Invoke-ManifestCase `
    -Name "Capability casing is exact" `
    -SupportedPlatforms @("Windows") `
    -Capabilities @("Storage") `
    -ExpectedExitCode 1

if ($violations.Count -eq 0) {
    Write-Host "All guardrail script regression tests passed." -ForegroundColor Green
    exit 0
}

Write-Host "Guardrail script regression failures:" -ForegroundColor Red
foreach ($violation in $violations) {
    Write-Host "  - $violation" -ForegroundColor Red
}
exit 1
