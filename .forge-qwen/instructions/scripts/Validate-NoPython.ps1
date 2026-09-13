[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)
$ErrorActionPreference='Stop'
$Repository=(Resolve-Path $Repository).Path
$failures=@()
$sourceFiles = @(& git -C $Repository ls-files -co --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Git could not enumerate release source files.' }
$sourceFiles | Where-Object {
    $_ -notmatch '^(?:\.forge-qwen/state|\.forge-qwen/instructions/inputs|out|build|artifacts|packages)/'
} | ForEach-Object {
    $_ = Get-Item -LiteralPath (Join-Path $Repository $_) -ErrorAction SilentlyContinue
    if ($null -eq $_ -or $_.PSIsContainer) { return }
    if($_.Extension -in @('.py','.pyw','.pyc','.pyo','.whl') -or $_.Name -in @('requirements.txt','Pipfile','pyproject.toml','setup.py')){$failures+=$_.FullName;return}
    if($_.Extension -in @('.ps1','.cmd','.bat','.cmake','.txt','.json','.yml','.yaml','.vcxproj','.props','.targets') -and $_.Length -lt 4MB){$text=Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue;if($text -match '(?im)^\s*#!.*python|\bpython(?:3)?(?:\.exe)?\s+-m\b|\bpip(?:3)?\s+install\b'){$failures+=$_.FullName}}
}
if($failures.Count){$failures|Sort-Object -Unique|ForEach-Object{Write-Error $_};throw 'No-Python gate failed.'}
Write-Host 'No-Python gate passed.'
