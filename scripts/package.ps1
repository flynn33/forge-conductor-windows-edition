#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Release')][string]$Configuration='Release',
    [ValidateSet('x64')][string]$Architecture='x64',
    [switch]$DevelopmentSigning,
    [string]$PfxPath=$env:FORGE_SIGNING_PFX,
    [string]$PfxPassword=$env:FORGE_SIGNING_PASSWORD
)
$ErrorActionPreference='Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root "out/app/$Architecture/$Configuration"
$stagingManifest = Join-Path $app 'staging-manifest.json'
if (-not (Test-Path -LiteralPath $stagingManifest)) { throw 'Build Release with scripts/build.ps1 -Product All before packaging.' }
$staging = Get-Content -LiteralPath $stagingManifest -Raw | ConvertFrom-Json
foreach ($entry in $staging.executables) {
    if ((Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw "Staged executable changed since build: $($entry.name)"
    }
}
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?s)project\(\s*ForgeConductorWindows\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'Product version missing from CMake.' }
$version = $Matches[1] + '.0'
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/10.0.26100.0/x64'
$makeappx = Join-Path $sdk 'makeappx.exe'
$signtool = Join-Path $sdk 'signtool.exe'
foreach ($tool in @($makeappx,$signtool)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Required SDK tool missing: $tool" } }
$distribution = Join-Path $root ("out/dist/engineering-$version-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
$payload = Join-Path $distribution 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $app -Force) {
    if ($file.Extension -in @('.pdb','.lib','.exp') -or $file.Name -eq 'staging-manifest.json') { continue }
    Copy-Item -LiteralPath $file.FullName -Destination $payload -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $root 'packaging/Assets') -Destination $payload -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'src/ForgeConductor.Application/Resources') -Destination $payload -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests') -Destination (Join-Path $payload 'Resources') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination $payload
# Include the redistributable release CRT, never the developer Debug CRT.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installations = & $vswhere -products '*' -version '[17.0,18.0)' -format json | ConvertFrom-Json
$crt = $null
foreach ($installation in $installations) {
    $redist = Join-Path $installation.installationPath 'VC/Redist/MSVC'
    if (-not (Test-Path -LiteralPath $redist)) { continue }
    foreach ($directory in Get-ChildItem -LiteralPath $redist -Directory) {
        $candidate = Join-Path $directory.FullName 'x64/Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath (Join-Path $candidate 'vcruntime140.dll')) { $crt = $candidate }
    }
}
if (-not $crt) { throw 'VS 2022 x64 redistributable release CRT was not found.' }
Copy-Item -Path (Join-Path $crt '*.dll') -Destination $payload
$certificate = $null
if ($DevelopmentSigning) {
    if ($PfxPath) { throw 'Choose DevelopmentSigning or PFX signing, not both.' }
    $subject = 'CN=Forge Conductor Alpha Development'
    foreach ($candidate in Get-ChildItem Cert:/CurrentUser/My) {
        if ($candidate.Subject -eq $subject -and $candidate.HasPrivateKey -and $candidate.NotAfter -gt (Get-Date).AddDays(7)) { $certificate=$candidate; break }
    }
    if (-not $certificate) {
        $certificate=New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:/CurrentUser/My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(1)
    }
} elseif ($PfxPath) {
    $certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($PfxPath,$PfxPassword)
    $subject=$certificate.Subject
} else { throw 'Supply -DevelopmentSigning or FORGE_SIGNING_PFX.' }
$manifest = Get-Content -LiteralPath (Join-Path $root 'packaging/Package.appxmanifest.template') -Raw
$manifest=$manifest.Replace('REPLACE_PACKAGE_IDENTITY','ForgeConductor.Windows.Alpha').Replace('REPLACE_CERTIFICATE_SUBJECT',[Security.SecurityElement]::Escape($subject)).Replace('REPLACE_PRODUCT_VERSION',$version)
Set-Content -LiteralPath (Join-Path $payload 'AppxManifest.xml') -Value $manifest -Encoding utf8
$package = Join-Path $distribution "ForgeConductor-$version-x64-engineering.msix"
& $makeappx pack /d $payload /p $package /o
if ($LASTEXITCODE -ne 0) { throw "MSIX packing failed (exit $LASTEXITCODE)." }
if ($DevelopmentSigning) {
    & $signtool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $package
} else {
    & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $package
}
if ($LASTEXITCODE -ne 0) { throw "MSIX signing failed (exit $LASTEXITCODE)." }
[IO.File]::WriteAllBytes((Join-Path $distribution 'Publisher.cer'),$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'alpha/Install-Engineering.ps1') -Destination $distribution
@"
Forge Conductor engineering checkpoint $version (x64 Release)

This is not the accepted Alpha. Native GUI build/launch has been tested; complete
workflow, live provider continuity and clean-machine acceptance remain pending.
An existing database newer than this checkout is refused without modification.

Install from an Administrator PowerShell in this distribution directory:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Engineering.ps1 -TrustDevelopmentPublisher
For a publisher already trusted on the machine, omit the switch.
Use a disposable Windows account for acceptance.
The package includes Windows App SDK runtime and the redistributable release CRT.
Public certificate only: no private signing key is included in this distribution.
"@ | Set-Content -LiteralPath (Join-Path $distribution 'README.txt') -Encoding utf8
@{version=$version;configuration=$Configuration;architecture=$Architecture;package=$package;
  sha256=(Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant();
  publisher=$subject;certificate_thumbprint=$certificate.Thumbprint;alpha_accepted=$false} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $distribution 'distribution.json') -Encoding utf8
Write-Host "Signed engineering distribution: $distribution"
$bundleFiles = @($package,(Join-Path $distribution 'Publisher.cer'),(Join-Path $distribution 'Install-Engineering.ps1'),(Join-Path $distribution 'README.txt'),(Join-Path $distribution 'distribution.json'))
Compress-Archive -LiteralPath $bundleFiles -DestinationPath (Join-Path $distribution "ForgeConductor-$version-x64-engineering.zip")
