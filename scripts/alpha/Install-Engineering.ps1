#Requires -Version 5.1
[CmdletBinding()]
param([string]$Distribution=$PSScriptRoot,[switch]$TrustDevelopmentPublisher)
$ErrorActionPreference='Stop'
$metadata=Get-Content -LiteralPath (Join-Path $Distribution 'distribution.json') -Raw | ConvertFrom-Json
$package=Join-Path $Distribution ([IO.Path]::GetFileName($metadata.package))
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne $metadata.sha256) {
    throw 'Package SHA-256 does not match distribution.json.'
}
if ($TrustDevelopmentPublisher) {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=[Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Open PowerShell as Administrator to trust this development publisher, then run this same command.'
    }
    $path=Join-Path $Distribution 'Publisher.cer'
    $certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($path)
    if ($certificate.Thumbprint -ne $metadata.certificate_thumbprint -or $certificate.Subject -ne $metadata.publisher) {
        throw 'Publisher certificate differs from distribution.json.'
    }
    Import-Certificate -FilePath $path -CertStoreLocation Cert:/LocalMachine/TrustedPeople | Out-Null
}
Add-AppxPackage -Path $package
Write-Host 'Engineering package installed. Launch Forge Conductor from Start. Alpha acceptance is still pending.'
