[CmdletBinding()]
param(
    [string]$WorkspaceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

$ErrorActionPreference = 'Stop'
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$installer = Join-Path $WorkspaceRoot 'scripts\alpha\Install-Engineering.ps1'

$script:AssertionCount = 0
$script:ImportCertificateCalls = 0
$script:AddAppxPackageCalls = 0

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    if ($Actual -cne $Expected) {
        throw "Install preflight assertion failed: $Message (expected '$Expected', found '$Actual')"
    }
    $script:AssertionCount++
}

function Import-Certificate {
    param([string]$FilePath, [string]$CertStoreLocation)
    $script:ImportCertificateCalls++
}

function Add-AppxPackage {
    param([string]$Path)
    $script:AddAppxPackageCalls++
}

$conflictMessage = $null
try {
    & $installer `
        -Distribution (Join-Path $WorkspaceRoot 'out\validation\missing-install-distribution') `
        -PreflightOnly `
        -TrustDevelopmentPublisher
}
catch {
    $conflictMessage = $_.Exception.Message
}

Assert-Exact $conflictMessage `
    'PreflightOnly cannot be combined with TrustDevelopmentPublisher.' `
    'conflicting switches are rejected before distribution access'
Assert-Exact $script:ImportCertificateCalls 0 `
    'conflicting preflight does not import a certificate'
Assert-Exact $script:AddAppxPackageCalls 0 `
    'conflicting preflight does not deploy a package'

[ordered]@{
    test = 'InstallEngineeringPreflight'
    assertions = $script:AssertionCount
    conflicting_invocation_rejected = $true
    import_certificate_calls = $script:ImportCertificateCalls
    add_appx_package_calls = $script:AddAppxPackageCalls
} | ConvertTo-Json
