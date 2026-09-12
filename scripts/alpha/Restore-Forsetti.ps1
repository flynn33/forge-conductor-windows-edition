#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName='Archive')]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(ParameterSetName='Archive')][string]$Archive,
    [Parameter(Mandatory,ParameterSetName='Directory')][string]$SourceDirectory,
    [Parameter(Mandatory,ParameterSetName='Fetch')][switch]$FetchKnownCandidate
)
$ErrorActionPreference = 'Stop'
$archiveHash = '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d'
$treeHash = 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28'
$root = (Resolve-Path -LiteralPath $Repository).Path
if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt'))) { throw 'Expected the actual Windows repository root.' }
$parent = Join-Path $root '.forge-inputs\forsetti-framework'
$target = Join-Path $parent 'Forsetti-Framework-Windows-main'
function Assert-ChildPath([string]$Path,[string]$AllowedParent) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $boundary = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing filesystem operation outside $AllowedParent : $resolved"
    }
}
function Get-SourceTreeHash([string]$Directory) {
    $base = (Resolve-Path -LiteralPath $Directory).Path
    $map = [System.Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    foreach ($f in Get-ChildItem -LiteralPath $base -File -Recurse -Force) {
        $relative = [IO.Path]::GetRelativePath($base,$f.FullName).Replace('\','/')
        $map.Add($relative,(Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant())
    }
    [string[]]$names = @($map.Keys)
    [Array]::Sort($names,[StringComparer]::Ordinal)
    $lines = foreach ($name in $names) { "$name`t$($map[$name])" }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($sha.ComputeHash($bytes)).ToLowerInvariant() }
    finally { $sha.Dispose() }
}
if (Test-Path -LiteralPath $target) {
    if ((Get-SourceTreeHash $target) -ne $treeHash) { throw 'Existing Forsetti tree differs. It has not been modified; reconcile it explicitly.' }
    Write-Host "Verified existing exact source: $target"
    return
}
$temp = Join-Path ([IO.Path]::GetTempPath()) ('forge-source-' + [Guid]::NewGuid().ToString('N'))
$stage = $null
try {
    if ($PSCmdlet.ParameterSetName -in @('Archive','Fetch')) {
        New-Item -ItemType Directory -Path $temp | Out-Null
        $extract = Join-Path $temp 'source'
        if ($PSCmdlet.ParameterSetName -eq 'Fetch') {
            if (-not $FetchKnownCandidate) { throw 'FetchKnownCandidate must be explicitly enabled.' }
            $Archive = Join-Path $temp 'candidate.zip'
            $candidateUrl = 'https://github.com/flynn33/Forsetti-Framework-Windows/archive/63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5.zip'
            Write-Host 'Downloading the fixed upstream candidate. The complete source tree must still match the recorded pin.'
            Invoke-WebRequest -Uri $candidateUrl -OutFile $Archive -ErrorAction Stop
        } else {
            if ([string]::IsNullOrWhiteSpace($Archive)) { $Archive = Join-Path $root '.forge-inputs\archives\Forsetti-Framework-Windows-main.zip' }
            if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) { throw 'Original archive not found. Supply -Archive, -SourceDirectory or -FetchKnownCandidate; see docs/BUILD.md.' }
            if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash) { throw 'Archive does not match the recorded original attachment. No source was restored.' }
        }
        $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
        try {
            foreach ($entry in $zip.Entries) {
                # The upstream Git tree has a case collision absent from the original Windows archive.
                # Windows extraction retained the first filename's casing and the last entry's bytes.
                # Reproduce that original archive tree; the complete digest still must match.
                if ($PSCmdlet.ParameterSetName -eq 'Fetch' -and
                    $entry.FullName -ceq 'Forsetti-Framework-Windows-63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5/.github/PULL_REQUEST_TEMPLATE.md') { continue }
                $entryPath = $entry.FullName
                if ($PSCmdlet.ParameterSetName -eq 'Fetch' -and
                    $entryPath -ceq 'Forsetti-Framework-Windows-63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5/.github/pull_request_template.md') {
                    $entryPath = $entryPath.Replace('pull_request_template.md','PULL_REQUEST_TEMPLATE.md')
                }
                $destination = Join-Path $extract $entryPath
                Assert-ChildPath $destination $extract
                if (-not $entry.Name) {
                    [IO.Directory]::CreateDirectory($destination) | Out-Null
                    continue
                }
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$destination,$false)
            }
        } finally { $zip.Dispose() }
        $folders = @(Get-ChildItem -LiteralPath $extract -Directory -Force)
        if ($folders.Count -ne 1) { throw 'Expected exactly one extracted source root.' }
        $SourceDirectory = $folders[0].FullName
    }
    if (-not (Test-Path -LiteralPath $SourceDirectory -PathType Container)) { throw 'Source directory does not exist.' }
    if ((Get-SourceTreeHash $SourceDirectory) -ne $treeHash) { throw 'Full source tree differs from the recorded pin. Do not bypass the check; reconcile source provenance.' }
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $stage = Join-Path $parent ('.restore-' + [Guid]::NewGuid().ToString('N'))
    Copy-Item -LiteralPath $SourceDirectory -Destination $stage -Recurse -Force
    if ((Get-SourceTreeHash $stage) -ne $treeHash) { throw 'Staged source verification failed.' }
    if (Test-Path -LiteralPath $target) { throw 'Target appeared during restore; preserving it.' }
    Assert-ChildPath $stage $parent
    Assert-ChildPath $target $parent
    Move-Item -LiteralPath $stage -Destination $target
    $stage = $null
    Write-Host "Restored verified source: $target"
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        Assert-ChildPath $stage $parent
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Continue
    }
    if (Test-Path -LiteralPath $temp) {
        Assert-ChildPath $temp ([IO.Path]::GetTempPath())
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction Continue
    }
}
