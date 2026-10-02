[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = "Stop"

$excluded = @("\.git\","\.forge-inputs\","\.forge-codex\instructions\inputs\","\build\","\out\","\artifacts\","\packages\")
$violations = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $WorkspaceRoot -Recurse -Force -File | ForEach-Object {
    $full = $_.FullName
    foreach ($fragment in $excluded) {
        if ($full.IndexOf($fragment, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return }
    }
    if ($_.Extension -in @(".py",".pyw",".pyc",".pyo")) {
        $violations.Add("Python file: $full")
        return
    }
    if ($_.Length -gt 4MB) { return }
    if ($_.Extension -in @(".ps1",".cmd",".bat",".cmake",".txt",".md",".json",".yml",".yaml",".cpp",".cxx",".cc",".h",".hpp",".vcxproj",".props",".targets")) {
        $text = Get-Content -Raw -LiteralPath $full -ErrorAction SilentlyContinue
        if ($text -match '(?im)^\s*#!.*python|\bpython(?:3(?:\.\d+)?)?\.exe\b|\bpython(?:3(?:\.\d+)?)?\s+-m\b') {
            $violations.Add("Python invocation/reference: $full")
        }
    }
}
if ($violations.Count) {
    $violations | ForEach-Object { Write-Error $_ }
    throw "No-Python gate failed."
}
Write-Host "No-Python gate passed."
