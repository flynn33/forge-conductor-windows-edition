[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)
$ErrorActionPreference='Stop'
$Repository=(Resolve-Path $Repository).Path
$failures=@()
$sourceFiles = @(& git -C $Repository ls-files -co --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Git could not enumerate release source files.' }
$sourceFiles | Where-Object {
    $_ -notmatch '^(?:\.forge-codex|\.forge-qwen|docs|tests|scripts/validation|out|build|artifacts|packages)/'
} | ForEach-Object {
    $_ = Get-Item -LiteralPath (Join-Path $Repository $_) -ErrorAction SilentlyContinue
    if ($null -eq $_ -or $_.PSIsContainer) { return }
    if($_.Extension -in @('.cs','.csproj','.fs','.fsproj','.vb','.vbproj','.java','.jar','.py','.pyw')){$failures+=$_.FullName;return}
    if($_.Length -lt 4MB){$text=Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue;if($text -match '(?i)#\s*include\s*[<"](?:boost/|Qt)|\bboost::|\belectron(?:\.exe)?\b|\bnpm\s+(start|run|install|ci)\b'){$failures+=$_.FullName}}
}
if($failures.Count){$failures|Sort-Object -Unique|ForEach-Object{Write-Error $_};throw 'Native-stack gate failed.'}
Write-Host 'Native-stack gate passed.'
