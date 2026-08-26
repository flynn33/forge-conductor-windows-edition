[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = 'Stop'

$violations = [System.Collections.Generic.List[string]]::new()
$excludedPattern = '\\.git\\|\\.forge-inputs\\|\\.forge-codex\\|\\build\\|\\out\\|\\artifacts\\|\\packages\\'
$sourcePatterns = @(
    '(?i)#\s*include\s*[<"]boost/',
    '(?i)#\s*include\s*[<"]Qt',
    '(?i)\bboost::',
    '(?i)\bQApplication\b|\bQWidget\b',
    '(?i)\bSystem\.Windows\.Forms\b|\bMicrosoft\.UI\.Xaml\b.*\.cs'
)
$scriptPatterns = @(
    '(?im)^\s*(?:&\s*)?(?:node|node\.exe|java|java\.exe|dotnet)(?:\s|$)',
    '(?i)\belectron(?:\.exe)?\b',
    '(?i)\bnpm\s+(?:start|run|install|ci)\b'
)

Get-ChildItem -LiteralPath $WorkspaceRoot -Recurse -Force -File | ForEach-Object {
    if ($_.FullName -match $excludedPattern) { return }
    if ($_.Length -gt 4MB) { return }
    $extension = $_.Extension.ToLowerInvariant()
    $text = Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue
    if ($null -eq $text) { return }
    $patterns = if ($extension -in @('.h','.hpp','.hh','.cpp','.cxx','.cc','.ixx','.cppm')) {
        $sourcePatterns
    } elseif ($extension -in @('.ps1','.cmd','.bat','.cmake','.json','.yml','.yaml','.props','.targets','.vcxproj')) {
        $sourcePatterns + $scriptPatterns
    } else {
        @()
    }
    foreach ($pattern in $patterns) {
        if ($text -match $pattern) { $violations.Add("$($_.FullName): $pattern") }
    }
    if ($extension -in @('.cs','.csproj','.fs','.fsproj','.vb','.vbproj','.java','.jar')) {
        $violations.Add("Forbidden managed/cross-platform source artifact: $($_.FullName)")
    }
}
if ($violations.Count) {
    $violations | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
    throw 'Native-stack gate failed.'
}
Write-Host 'Native-stack gate passed.'
