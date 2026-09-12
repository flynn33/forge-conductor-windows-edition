#Requires -Version 5.1
[CmdletBinding()]
param([string]$Distribution=$PSScriptRoot,[switch]$TrustDevelopmentPublisher)
$ErrorActionPreference='Stop'
$metadata=Get-Content -LiteralPath (Join-Path $Distribution 'distribution.json') -Raw | ConvertFrom-Json
$package=Join-Path $Distribution ([IO.Path]::GetFileName($metadata.package))
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne $metadata.sha256) {
    throw 'Package SHA-256 does not match distribution.json.'
}
$certificatePath=Join-Path $Distribution 'Publisher.cer'
if ((Get-FileHash -LiteralPath $certificatePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
    $metadata.certificate_sha256) {
    throw 'Public certificate SHA-256 does not match distribution.json.'
}
$certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
if ($certificate.Thumbprint -ne $metadata.certificate_thumbprint -or
    $certificate.Subject -ne $metadata.publisher) {
    throw 'Publisher certificate differs from distribution.json.'
}
$signature=Get-AuthenticodeSignature -LiteralPath $package
if (-not $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint -or
    $signature.Status -in @('NotSigned','HashMismatch')) {
    throw "Package signature does not match the published certificate ($($signature.Status))."
}
if ($TrustDevelopmentPublisher) {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=[Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Open PowerShell as Administrator to trust this development publisher, then run this same command.'
    }
    Import-Certificate -FilePath $certificatePath -CertStoreLocation Cert:/LocalMachine/TrustedPeople | Out-Null
}
$installedBefore=Get-AppxPackage -Name $metadata.package_identity -ErrorAction SilentlyContinue |
    Sort-Object Version -Descending | Select-Object -First 1
if ($installedBefore -and [version]$installedBefore.Version -ge [version]$metadata.package_version) {
    throw "Installed version $($installedBefore.Version) must be lower than candidate $($metadata.package_version) for an update, or removed before a fresh install."
}
Add-AppxPackage -Path $package
$installedAfter=Get-AppxPackage -Name $metadata.package_identity -ErrorAction Stop |
    Sort-Object Version -Descending | Select-Object -First 1
if (-not $installedAfter -or
    [string]$installedAfter.Version -ne [string]$metadata.package_version -or
    $installedAfter.Publisher -ne $metadata.publisher) {
    throw 'Windows did not register the expected package identity, version, and publisher.'
}
@{
    installed_at_utc=[DateTime]::UtcNow.ToString('o')
    package_identity=$installedAfter.Name
    package_version=[string]$installedAfter.Version
    publisher=$installedAfter.Publisher
    install_location=$installedAfter.InstallLocation
    prior_version=if ($installedBefore) { [string]$installedBefore.Version } else { $null }
} | ConvertTo-Json | Set-Content -LiteralPath (
    Join-Path $Distribution 'install-result.json') -Encoding utf8
Write-Host "Forge Conductor $($installedAfter.Version) installed. Launch Forge Conductor from Start."
