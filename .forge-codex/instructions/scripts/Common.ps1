Set-StrictMode -Version Latest

function Write-AtomicUtf8 {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Content
    )
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
    $temp = "$Path.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    [System.IO.File]::WriteAllText($temp, $Content, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temp -Destination $Path -Force
}

function Get-FileSha256 {
    param([Parameter(Mandatory)][string]$Path)
    Get-BytesSha256 ([System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path))
}

function Get-BytesSha256 {
    param([Parameter(Mandatory)][AllowEmptyCollection()][byte[]]$Bytes)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $algorithm.ComputeHash($Bytes)
        return ([System.BitConverter]::ToString($hash)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-StringSha256 {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Value)
    Get-BytesSha256 -Bytes ([System.Text.Encoding]::UTF8.GetBytes($Value))
}

function ConvertTo-CompactJson {
    param([Parameter(Mandatory)]$Value)
    $Value | ConvertTo-Json -Depth 100 -Compress
}

function ConvertFrom-JsonPreserveDates {
    param([Parameter(Mandatory)][string]$Value)
    $convertFromJson = Get-Command ConvertFrom-Json -ErrorAction Stop
    if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
        return $Value | ConvertFrom-Json -DateKind String
    }
    # Windows PowerShell 5.1 does not coerce ISO-8601 JSON strings to DateTime.
    return $Value | ConvertFrom-Json
}

function Read-JsonFile {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "JSON file does not exist: $Path"
    }
    ConvertFrom-JsonPreserveDates -Value (Get-Content -Raw -LiteralPath $Path)
}

function Write-JsonFileAtomic {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Value
    )
    Write-AtomicUtf8 -Path $Path -Content (($Value | ConvertTo-Json -Depth 100) + "`n")
}

function Get-UtcTimestamp {
    [DateTimeOffset]::UtcNow.ToString('o')
}

function Get-RelativePathPortable {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )
    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar.ToString())) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)
    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relative = [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString())
    return $relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

function Test-Windows11 {
    if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
        return $false
    }
    $build = [System.Environment]::OSVersion.Version.Build
    try {
        $registry = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction Stop
        if ($registry.CurrentBuildNumber) {
            $build = [int]$registry.CurrentBuildNumber
        }
    }
    catch {
        # OSVersion remains the fallback when registry access is unavailable.
    }
    return $build -ge 22000
}

function Get-VsWherePath {
    $programFilesX86 = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::ProgramFilesX86)
    if ($programFilesX86) {
        $candidate = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $cmd = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Update-ProcessPathFromRegistry {
    if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) { return }
    $machine = [System.Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [System.Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @($machine, $user, $env:Path) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $env:Path = ($parts -join ';')
}

function Add-LedgerEvent {
    param(
        [Parameter(Mandatory)][string]$StateRoot,
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Phase,
        [Parameter(Mandatory)][string]$Action,
        [hashtable]$Data = @{}
    )
    if (-not (Test-Path -LiteralPath $StateRoot)) {
        New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
    }
    $ledger = Join-Path $StateRoot 'event-ledger.jsonl'
    $sequence = 1
    $previousHash = ('0' * 64)
    if (Test-Path -LiteralPath $ledger) {
        $lines = @(Get-Content -LiteralPath $ledger | Where-Object { $_.Trim() })
        if ($lines.Count -gt 0) {
            $last = ConvertFrom-JsonPreserveDates -Value $lines[-1]
            $sequence = [int]$last.sequence + 1
            $previousHash = [string]$last.event_hash
        }
    }

    $body = [ordered]@{
        sequence = $sequence
        utc = Get-UtcTimestamp
        role = $Role
        phase = $Phase
        action = $Action
        data = $Data
        previous_hash = $previousHash
    }
    $eventHash = Get-StringSha256 -Value (ConvertTo-CompactJson $body)
    $event = [ordered]@{}
    foreach ($key in $body.Keys) { $event[$key] = $body[$key] }
    $event['event_hash'] = $eventHash
    $line = (ConvertTo-CompactJson $event) + [Environment]::NewLine
    [System.IO.File]::AppendAllText($ledger, $line, (New-Object System.Text.UTF8Encoding($false)))
    return $event
}
