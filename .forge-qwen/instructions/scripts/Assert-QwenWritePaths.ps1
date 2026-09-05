[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Microtask,
    [Parameter(Mandatory)][string[]]$Paths
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
$assignment = Read-Json -Path (Join-Path $Repository '.forge-qwen\state\ACTIVE_ASSIGNMENT.json')
if ([string]$assignment.microtask.id -ne $Microtask) { throw "Assignment microtask mismatch: $($assignment.microtask.id)" }

function Convert-GlobToRegex([string]$Pattern) {
    $normalized = $Pattern.Replace('\\', '/')
    $escaped = [Regex]::Escape($normalized)
    $escaped = $escaped.Replace('\*\*', '§DOUBLESTAR§').Replace('\*', '[^/]*').Replace('§DOUBLESTAR§', '.*')
    return '^' + $escaped + '$'
}

$patterns = @($assignment.allowed_write_roots | ForEach-Object { Convert-GlobToRegex -Pattern ([string]$_) })
$violations = [System.Collections.Generic.List[string]]::new()
foreach ($path in $Paths) {
    if ([string]::IsNullOrWhiteSpace($path)) { continue }
    if ([IO.Path]::IsPathRooted($path)) {
        $full = Get-NormalizedFullPath -Path $path
        if (-not (Test-PathInside -Path $full -Root $Repository)) { $violations.Add("outside target: $path"); continue }
        $relative = Get-RelativePathPortable -Base $Repository -Target $full
    }
    else { $relative = $path }

    $relative = $relative.Replace('\', '/')
    while ($relative.StartsWith('./')) { $relative = $relative.Substring(2) }
    while ($relative.StartsWith('/')) { $relative = $relative.Substring(1) }
    if ($relative -like '.forge-inputs/*' -or $relative -like '.forge-qwen/instructions/*') { $violations.Add("read-only root: $relative"); continue }
    if ($relative -like '*.pfx' -or $relative -like '*.key' -or $relative -like '*.pem') { $violations.Add("sensitive artifact path: $relative"); continue }

    $matched = $false
    foreach ($pattern in $patterns) {
        if ($relative -match $pattern) { $matched = $true; break }
    }
    if (-not $matched) { $violations.Add("not allowed by assignment: $relative") }
}
if ($violations.Count) { throw ('WRITE_SCOPE_VIOLATION: ' + ($violations -join '; ')) }
[ordered]@{ valid = $true; microtask_id = $Microtask; paths = @($Paths) } | ConvertTo-Json -Depth 20
