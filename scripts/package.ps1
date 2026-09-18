#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Release')][string]$Configuration='Release',
    [ValidateSet('x64')][string]$Architecture='x64',
    [switch]$DevelopmentSigning,
    [string]$PfxPath=$env:FORGE_SIGNING_PFX,
    [string]$PfxPassword=$env:FORGE_SIGNING_PASSWORD,
    [string]$UpdateBaseUri
)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root = Split-Path -Parent $PSScriptRoot
$identity = 'ForgeConductor.Windows'

function Invoke-GitScalar {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $value = (& git -C $root @Arguments)
    if ($LASTEXITCODE -ne 0 -or -not $value) {
        throw "Git failed while resolving release provenance: $($Arguments -join ' ')"
    }
    return ([string]$value).Trim()
}

$sourceCommit = Invoke-GitScalar @('rev-parse','HEAD')
$sourceTree = Invoke-GitScalar @('rev-parse','HEAD^{tree}')
$releaseInputs = @(
    'CMakeLists.txt','CMakePresets.json','vcpkg.json','vcpkg-configuration.json',
    'include','src','packaging','scripts/build.ps1','scripts/alpha/Build-App.ps1',
    'scripts/alpha/Install-Engineering.ps1','scripts/package.ps1','THIRD-PARTY-NOTICES.md')
$releaseDirty = @(& git -C $root status --porcelain=v1 --untracked-files=all -- @releaseInputs)
if ($LASTEXITCODE -ne 0) { throw 'Git could not inspect release inputs.' }
if ($releaseDirty.Count -ne 0) {
    throw "Commit all product and packaging inputs before creating a release:`n$($releaseDirty -join "`n")"
}

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?s)project\(\s*ForgeConductorWindows\s+VERSION\s+(\d+\.\d+\.\d+)') {
    throw 'Product version missing from CMake.'
}
$productVersion = $Matches[1]
$version = $productVersion + '.0'
$productIdentity = Get-Content -LiteralPath (
    Join-Path $root 'include/ForgeConductor/Domain/ProductIdentity.h') -Raw
if ($productIdentity -notmatch ('ProductVersion\{"' + [regex]::Escape($productVersion) + '"\}')) {
    throw 'CMake and runtime product versions do not match.'
}

$app = Join-Path $root "out/app/$Architecture/$Configuration"
$stagingManifest = Join-Path $app 'staging-manifest.json'
if (-not (Test-Path -LiteralPath $stagingManifest -PathType Leaf)) {
    throw 'Build Release with scripts/build.ps1 -Product All before packaging.'
}
$staging = Get-Content -LiteralPath $stagingManifest -Raw | ConvertFrom-Json
if ($staging.configuration -cne $Configuration -or $staging.architecture -cne $Architecture) {
    throw 'The staged build configuration or architecture does not match the release.'
}
if ($staging.source_commit -cne $sourceCommit -or $staging.source_tree -cne $sourceTree) {
    throw 'The staged products were not built from the current release commit and tree.'
}
if (@($staging.source_dirty).Count -ne 0) {
    throw 'The staged products were built while product source was dirty.'
}
$requiredExecutables = @(
    'ForgeConductorApp.exe','forge-conductor.exe',
    'ForgeConductor.Manager.exe','ForgeConductor.SessionHost.exe')
$stagedNames = @($staging.executables | ForEach-Object { [string]$_.name })
if (@($stagedNames | Sort-Object -Unique).Count -ne $requiredExecutables.Count -or
    @($requiredExecutables | Where-Object { $_ -cnotin $stagedNames }).Count -ne 0) {
    throw 'The staging manifest does not contain exactly the required product executables.'
}
foreach ($entry in $staging.executables) {
    $expectedPath = Join-Path $app ([string]$entry.name)
    if (-not [IO.Path]::GetFullPath([string]$entry.path).Equals(
            [IO.Path]::GetFullPath($expectedPath), [StringComparison]::OrdinalIgnoreCase)) {
        throw "A staged executable path escaped the release directory: $($entry.name)"
    }
    if ((Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
        [string]$entry.sha256) {
        throw "Staged executable changed since build: $($entry.name)"
    }
}

$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/10.0.26100.0/x64'
$makeappx = Join-Path $sdk 'makeappx.exe'
$signtool = Join-Path $sdk 'signtool.exe'
foreach ($tool in @($makeappx,$signtool)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required SDK tool missing: $tool"
    }
}

$certificate = $null
if ($DevelopmentSigning) {
    if ($PfxPath) { throw 'Choose DevelopmentSigning or PFX signing, not both.' }
    $subject = 'CN=Forge Conductor Development'
    foreach ($candidate in Get-ChildItem Cert:/CurrentUser/My) {
        if ($candidate.Subject -eq $subject -and $candidate.HasPrivateKey -and
            $candidate.NotAfter -gt (Get-Date).AddDays(7)) {
            $certificate=$candidate
            break
        }
    }
    if (-not $certificate) {
        $certificate=New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
            -CertStoreLocation Cert:/CurrentUser/My -KeyAlgorithm RSA -KeyLength 2048 `
            -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(1)
    }
} elseif ($PfxPath) {
    $certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $PfxPath,$PfxPassword)
    $subject=$certificate.Subject
} else {
    throw 'Supply -DevelopmentSigning or FORGE_SIGNING_PFX.'
}

$distribution = Join-Path $root (
    "out/dist/release-$version-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
$payload = Join-Path $distribution 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $app -Force) {
    if ($file.Extension -in @('.pdb','.lib','.exp') -or
        $file.Name -eq 'staging-manifest.json') { continue }
    Copy-Item -LiteralPath $file.FullName -Destination $payload -Recurse -Force
}
$payloadAssets = Join-Path $payload 'Assets'
New-Item -ItemType Directory -Path $payloadAssets -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'packaging/Assets') -File |
    Where-Object { $_.Extension -in @('.png','.ico') } |
    Copy-Item -Destination $payloadAssets -Force
Copy-Item -LiteralPath (Join-Path $root 'src/ForgeConductor.Application/Resources') `
    -Destination $payload -Recurse -Force
$forsettiDestination = Join-Path $payload 'Resources/ForsettiManifests'
New-Item -ItemType Directory -Path $forsettiDestination -Force | Out-Null
Copy-Item -Path (Join-Path $root 'src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests/*') `
    -Destination $forsettiDestination -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination $payload

# Include the redistributable release CRT, never the developer Debug CRT.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installations = & $vswhere -products '*' -version '[17.0,18.0)' -format json |
    ConvertFrom-Json
$crt = $null
foreach ($installation in $installations) {
    $redist = Join-Path $installation.installationPath 'VC/Redist/MSVC'
    if (-not (Test-Path -LiteralPath $redist -PathType Container)) { continue }
    foreach ($directory in Get-ChildItem -LiteralPath $redist -Directory |
        Sort-Object Name -Descending) {
        $candidate = Join-Path $directory.FullName 'x64/Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath (Join-Path $candidate 'vcruntime140.dll')) {
            $crt = $candidate
            break
        }
    }
    if ($crt) { break }
}
if (-not $crt) { throw 'VS 2022 x64 redistributable release CRT was not found.' }
Copy-Item -Path (Join-Path $crt '*.dll') -Destination $payload

$manifest = Get-Content -LiteralPath (
    Join-Path $root 'packaging/Package.appxmanifest.template') -Raw
$manifest=$manifest.Replace('REPLACE_PACKAGE_IDENTITY',$identity).
    Replace('REPLACE_CERTIFICATE_SUBJECT',[Security.SecurityElement]::Escape($subject)).
    Replace('REPLACE_PRODUCT_VERSION',$version)
if ($manifest -match 'REPLACE_[A-Z_]+') { throw 'The package manifest retains a placeholder.' }
Set-Content -LiteralPath (Join-Path $payload 'AppxManifest.xml') `
    -Value $manifest -Encoding utf8

$requiredPayload = @(
    'ForgeConductorApp.exe','forge-conductor.exe','ForgeConductor.Manager.exe',
    'ForgeConductor.SessionHost.exe','App.xbf','MainWindow.xbf','ForgeConductorApp.pri',
    'Microsoft.WindowsAppRuntime.Bootstrap.dll','Microsoft.ui.xaml.dll',
    'Assets/StoreLogo.png','Assets/Square44x44Logo.png',
    'Assets/Square150x150Logo.png','Assets/Wide310x150Logo.png',
    'Resources/Agents/implement.md',
    'Resources/ForsettiManifests/ForgeConductorAppModule.json',
    'vcruntime140.dll','msvcp140.dll','THIRD-PARTY-NOTICES.md','AppxManifest.xml')
foreach ($relative in $requiredPayload) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $relative) -PathType Leaf)) {
        throw "Required package payload is missing: $relative"
    }
}
$mainWindowXbf = Join-Path $payload 'MainWindow.xbf'
$mainWindowText = [Text.Encoding]::Unicode.GetString(
    [IO.File]::ReadAllBytes($mainWindowXbf))
$guidedModeMarkers = @(
    'Guided setup',
    'Open Guided Mode setup',
    'Guided Mode on home',
    'Guided Mode setting',
    'GUIDED MODE · STEP 1 OF 9',
    'Restart from the beginning',
    'The full path',
    'Version loading · Windows 11 · x64')
foreach ($marker in $guidedModeMarkers) {
    if (-not $mainWindowText.Contains($marker, [StringComparison]::Ordinal)) {
        throw "Compiled application surface is missing required Guided Mode marker: $marker"
    }
}
foreach ($debugRuntime in @('vcruntime140d.dll','msvcp140d.dll','ucrtbased.dll')) {
    if (Test-Path -LiteralPath (Join-Path $payload $debugRuntime)) {
        throw "Debug runtime must not ship: $debugRuntime"
    }
}

$buildExecutables = @($staging.executables | ForEach-Object {
    [ordered]@{name=[string]$_.name;sha256=[string]$_.sha256}
})
$provenance = [ordered]@{
    schema_version=1
    created_at_utc=[DateTime]::UtcNow.ToString('o')
    source_commit=$sourceCommit
    source_tree=$sourceTree
    source_dirty=@()
    product_version=$productVersion
    package_identity=$identity
    package_version=$version
    publisher=$subject
    configuration=$Configuration
    architecture=$Architecture
    staged_executables=$buildExecutables
    release_channel='stable'
    development_signed=[bool]$DevelopmentSigning
}
$provenance | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $payload 'release-provenance.json') -Encoding utf8

$payloadEntries = @(
    Get-ChildItem -LiteralPath $payload -File -Recurse | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($payload,$_.FullName).Replace('\','/')
        [ordered]@{
            path=$relative
            bytes=$_.Length
            sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } | Sort-Object path)
[ordered]@{schema_version=1;files=$payloadEntries} | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $payload 'payload-manifest.json') -Encoding utf8

$repositoryNeedles = @($root, 'A:\\Codex\\Projects', 'D:\\GitHub')
foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse |
    Where-Object { $_.Extension -in @('.json','.md','.txt','.xml') }) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($needle in $repositoryNeedles) {
        if ($text.Contains($needle,[StringComparison]::OrdinalIgnoreCase)) {
            throw "Package text contains a build/source drive dependency: $($file.FullName)"
        }
    }
}

$packageName = "ForgeConductor-$version-x64.msix"
$package = Join-Path $distribution $packageName
& $makeappx pack /d $payload /p $package /o
if ($LASTEXITCODE -ne 0) { throw "MSIX packing failed (exit $LASTEXITCODE)." }
if ($DevelopmentSigning) {
    & $signtool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $package
} else {
    & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $package
}
if ($LASTEXITCODE -ne 0) { throw "MSIX signing failed (exit $LASTEXITCODE)." }
$signature = Get-AuthenticodeSignature -LiteralPath $package
if (-not $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint -or
    $signature.Status -in @('NotSigned','HashMismatch')) {
    throw "The signed package does not carry the selected certificate ($($signature.Status))."
}

$verificationRoot = Join-Path $distribution 'verified-payload'
& $makeappx unpack /p $package /d $verificationRoot /o
if ($LASTEXITCODE -ne 0) { throw "MSIX verification unpack failed (exit $LASTEXITCODE)." }
$verifiedManifest = Get-Content -LiteralPath (
    Join-Path $verificationRoot 'payload-manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $verifiedManifest.files) {
    $verifiedFile = Join-Path $verificationRoot ([string]$entry.path)
    if (-not (Test-Path -LiteralPath $verifiedFile -PathType Leaf) -or
        (Get-FileHash -LiteralPath $verifiedFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne
            [string]$entry.sha256) {
        throw "Packed payload verification failed: $($entry.path)"
    }
}
Remove-Item -LiteralPath $verificationRoot -Recurse -Force

$certificatePath = Join-Path $distribution 'Publisher.cer'
[IO.File]::WriteAllBytes($certificatePath,$certificate.Export(
    [Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'alpha/Install-Engineering.ps1') `
    -Destination (Join-Path $distribution 'Install.ps1')
@"
Forge Conductor $version for Windows 11 x64

Verify distribution.json, then run Install.ps1 -PreflightOnly. If it reports
ready_for_install true, run Install.ps1. Development-signed builds additionally
require an authorized administrator to trust Publisher.cer in Local Machine
Trusted People. Each successful install or update writes a timestamped receipt.

Ordinary Start-menu launches use the production profile at
%LOCALAPPDATA%\Forge Conductor. Existing schema-9 data is supported in place.
For isolated testing, launch ForgeConductorApp.exe from a terminal with
--alpha-root and an absolute empty folder.

The package is self-contained for the Windows App SDK and release Visual C++
runtime. LM Studio and a loaded model remain local runtime prerequisites for
inference. Only the public publisher certificate is included; no private key or
password is present in this distribution.
"@ | Set-Content -LiteralPath (Join-Path $distribution 'README.txt') -Encoding utf8

$packageHash=(Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
$certificateHash=(Get-FileHash -LiteralPath $certificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
$helperHash=(Get-FileHash -LiteralPath (
    Join-Path $distribution 'Install.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
$readmeHash=(Get-FileHash -LiteralPath (
    Join-Path $distribution 'README.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
$distributionMetadata = [ordered]@{
    schema_version=2
    created_at_utc=[DateTime]::UtcNow.ToString('o')
    source_commit=$sourceCommit
    source_tree=$sourceTree
    distribution_source_commit=$sourceCommit
    distribution_source_tree=$sourceTree
    distribution_refresh='full_release'
    source_dirty=@()
    product_version=$productVersion
    package_identity=$identity
    package_version=$version
    configuration=$Configuration
    architecture=$Architecture
    package=$packageName
    sha256=$packageHash
    publisher=$subject
    certificate_thumbprint=$certificate.Thumbprint
    certificate_sha256=$certificateHash
    install_helper_sha256=$helperHash
    readme_sha256=$readmeHash
    signature_status=[string]$signature.Status
    payload_manifest_sha256=(Get-FileHash -LiteralPath (
        Join-Path $payload 'payload-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    release_channel='stable'
    development_signed=[bool]$DevelopmentSigning
}
$distributionMetadata | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $distribution 'distribution.json') -Encoding utf8

$appInstallerPath = $null
if ($UpdateBaseUri) {
    $baseUri = $UpdateBaseUri.TrimEnd('/')
    if (-not [Uri]::IsWellFormedUriString($baseUri, [UriKind]::Absolute) -or
        ([Uri]$baseUri).Scheme -cne 'https') {
        throw 'UpdateBaseUri must be an absolute HTTPS URI.'
    }
    $appInstaller = Get-Content -LiteralPath (
        Join-Path $root 'packaging/ForgeConductor.appinstaller.template') -Raw
    $appInstaller = $appInstaller.Replace('REPLACE_PACKAGE_IDENTITY',$identity).
        Replace('REPLACE_CERTIFICATE_SUBJECT',[Security.SecurityElement]::Escape($subject)).
        Replace('REPLACE_PRODUCT_VERSION',$version).
        Replace('REPLACE_UPDATE_BASE_URI',[Security.SecurityElement]::Escape($baseUri)).
        Replace('REPLACE_PACKAGE_FILENAME',$packageName)
    if ($appInstaller -match 'REPLACE_[A-Z_]+') {
        throw 'The App Installer manifest retains a placeholder.'
    }
    $appInstallerPath = Join-Path $distribution 'ForgeConductor.appinstaller'
    Set-Content -LiteralPath $appInstallerPath -Value $appInstaller -Encoding utf8
    $distributionMetadata['appinstaller'] = [IO.Path]::GetFileName($appInstallerPath)
    $distributionMetadata['appinstaller_sha256'] = (
        Get-FileHash -LiteralPath $appInstallerPath -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $distributionMetadata | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $distribution 'distribution.json') -Encoding utf8
}

$zip = Join-Path $distribution "ForgeConductor-$version-x64.zip"
$bundleFiles = @(
    $package,$certificatePath,(Join-Path $distribution 'Install.ps1'),
    (Join-Path $distribution 'README.txt'),(Join-Path $distribution 'distribution.json'))
if ($appInstallerPath) { $bundleFiles += $appInstallerPath }
Compress-Archive -LiteralPath $bundleFiles -DestinationPath $zip
$zipHash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$zipHash  $([IO.Path]::GetFileName($zip))" |
    Set-Content -LiteralPath (Join-Path $distribution 'bundle-sha256.txt') -Encoding ascii

Write-Host "Signed Forge Conductor release: $distribution"
[ordered]@{
    distribution=$distribution
    package=$package
    package_sha256=$packageHash
    bundle=$zip
    bundle_sha256=$zipHash
    source_commit=$sourceCommit
    source_tree=$sourceTree
    package_version=$version
    signature_status=[string]$signature.Status
} | ConvertTo-Json
