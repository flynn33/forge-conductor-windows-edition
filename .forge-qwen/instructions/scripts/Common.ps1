Set-StrictMode -Version Latest

function Write-AtomicUtf8 {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Content
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $temp = "$Path.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    [IO.File]::WriteAllText($temp, $Content, (New-Object Text.UTF8Encoding($false)))
    $stream = [IO.File]::Open($temp, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    Move-Item -LiteralPath $temp -Destination $Path -Force
}

function Write-JsonAtomic {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Value)
    Write-AtomicUtf8 -Path $Path -Content (($Value | ConvertTo-Json -Depth 100) + "`n")
}

function Read-Json {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing JSON: $Path" }
    Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-StringSha256 {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return (([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '')).ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-UtcNow { [DateTime]::UtcNow.ToString('o') }

function Get-RelativePathPortable {
    param([Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$Target)
    $baseFull = [IO.Path]::GetFullPath($Base)
    if (-not $baseFull.EndsWith([IO.Path]::DirectorySeparatorChar.ToString())) {
        $baseFull += [IO.Path]::DirectorySeparatorChar
    }
    $baseUri = New-Object Uri($baseFull)
    $targetUri = New-Object Uri([IO.Path]::GetFullPath($Target))
    [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('/', [IO.Path]::DirectorySeparatorChar)
}

function Test-Windows11 {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { return $false }
    $build = [Environment]::OSVersion.Version.Build
    try {
        $record = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        if ($record.CurrentBuildNumber) { $build = [int]$record.CurrentBuildNumber }
    }
    catch {}
    return $build -ge 22000
}


function Update-ProcessPathFromRegistry {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { return }
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @($machine, $user, $env:Path) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $env:Path = $parts -join ';'
}

function Get-VsWherePath {
    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    if ($programFilesX86) {
        $candidate = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $command = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}


function Get-NormalizedFullPath {
    param([Parameter(Mandatory)][string]$Path)
    [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Test-SamePath {
    param([Parameter(Mandatory)][string]$Left,[Parameter(Mandatory)][string]$Right)
    $a=Get-NormalizedFullPath -Path $Left
    $b=Get-NormalizedFullPath -Path $Right
    return [string]::Equals($a,$b,[StringComparison]::OrdinalIgnoreCase)
}

function Test-PathInside {
    param([Parameter(Mandatory)][string]$Path,[Parameter(Mandatory)][string]$Root)
    $p=Get-NormalizedFullPath -Path $Path
    $r=Get-NormalizedFullPath -Path $Root
    if (Test-SamePath -Left $p -Right $r) { return $true }
    $prefix=$r+[IO.Path]::DirectorySeparatorChar
    return $p.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)
}

function Get-QwenWorkspaceLock {
    param([Parameter(Mandatory)][string]$Repository)
    $root=Get-NormalizedFullPath -Path $Repository
    $path=Join-Path $root '.forge-qwen\state\WORKSPACE_LOCK.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "WORKSPACE_LOCK_MISSING: $path"
    }
    return Read-Json $path
}

function Get-RepoRoot {
    param([string]$Start = (Get-Location).Path)
    $root=Get-NormalizedFullPath -Path $Start
    $lock=Get-QwenWorkspaceLock -Repository $root
    if (-not (Test-SamePath -Left $root -Right ([string]$lock.target_repository))) {
        throw "WRONG_WORKSPACE: current root '$root' does not equal locked target '$($lock.target_repository)'."
    }
    if ([string]$lock.mission_id -ne 'forge-conductor-windows11-port-qwen-guided-v2') {
        throw "WRONG_MISSION: $($lock.mission_id)"
    }
    return $root
}

function Add-QwenLedgerEvent {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Microtask,
        [Parameter(Mandatory)][string]$Action,
        [hashtable]$Data = @{}
    )

    $stateDirectory = Join-Path $Repository '.forge-qwen\state'
    $statePath = Join-Path $stateDirectory 'run-state.json'
    $state = Read-Json $statePath
    $previous = [string]$state.last_event_hash

    $body = [ordered]@{
        event_id = [Guid]::NewGuid().ToString('D')
        timestamp_utc = Get-UtcNow
        run_id = $state.run_id
        role = $Role
        microtask_id = $Microtask
        action = $Action
        data = $Data
        previous_hash = $previous
    }
    $canonical = $body | ConvertTo-Json -Depth 100 -Compress
    $hash = Get-StringSha256 -Text ($previous + "`n" + $canonical)
    $event = [ordered]@{}
    foreach ($key in $body.Keys) { $event[$key] = $body[$key] }
    $event['hash'] = $hash

    $ledgerPath = Join-Path $stateDirectory 'event-ledger.jsonl'
    [IO.File]::AppendAllText($ledgerPath, (($event | ConvertTo-Json -Depth 100 -Compress) + "`n"), (New-Object Text.UTF8Encoding($false)))
    $state.last_event_hash = $hash
    $state.updated_utc = Get-UtcNow
    Write-JsonAtomic -Path $statePath -Value $state
    return $event
}
