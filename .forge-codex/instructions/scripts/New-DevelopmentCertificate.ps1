[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Subject = 'CN=Forge Conductor Development'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -FriendlyName 'Forge Conductor Development' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3') `
    -NotAfter (Get-Date).AddYears(2)
$passwordText = [Guid]::NewGuid().ToString('N')
$password = ConvertTo-SecureString $passwordText -AsPlainText -Force
$pfx = Join-Path $OutputDirectory 'ForgeConductor-Development.pfx'
$cer = Join-Path $OutputDirectory 'ForgeConductor-Development.cer'
$credentialFile = Join-Path $OutputDirectory 'ForgeConductor-Development.credential.clixml'
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $password | Out-Null
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
(New-Object Management.Automation.PSCredential('ForgeConductorDevelopment', $password)) | Export-Clixml -LiteralPath $credentialFile
[ordered]@{
    subject=$cert.Subject
    thumbprint=$cert.Thumbprint
    pfx=$pfx
    cer=$cer
    credential_file=$credentialFile
    protection='DPAPI current-user via Export-Clixml'
} | ConvertTo-Json
