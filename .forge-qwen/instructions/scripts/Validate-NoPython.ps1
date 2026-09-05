[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)
$ErrorActionPreference='Stop'
$Repository=(Resolve-Path $Repository).Path
$failures=@()
Get-ChildItem -LiteralPath $Repository -File -Recurse -Force | Where-Object {$_.FullName -notmatch '\\(\.git|\.forge-inputs|\.forge-qwen\\instructions\\inputs|build|out|artifacts|packages)\\'} | ForEach-Object {
    if($_.Extension -in @('.py','.pyw','.pyc','.pyo','.whl') -or $_.Name -in @('requirements.txt','Pipfile','pyproject.toml','setup.py')){$failures+=$_.FullName;return}
    if($_.Extension -in @('.ps1','.cmd','.bat','.cmake','.txt','.json','.yml','.yaml','.vcxproj','.props','.targets') -and $_.Length -lt 4MB){$text=Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue;if($text -match '(?im)^\s*#!.*python|\bpython(?:3)?(?:\.exe)?\s+-m\b|\bpip(?:3)?\s+install\b'){$failures+=$_.FullName}}
}
if($failures.Count){$failures|Sort-Object -Unique|ForEach-Object{Write-Error $_};throw 'No-Python gate failed.'}
Write-Host 'No-Python gate passed.'
