[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)
$ErrorActionPreference='Stop'
$Repository=(Resolve-Path $Repository).Path
$failures=@()
Get-ChildItem -LiteralPath $Repository -File -Recurse -Force | Where-Object {$_.FullName -notmatch '\\(\.git|\.forge-inputs|\.forge-qwen|build|out|artifacts|packages)\\'} | ForEach-Object {
    if($_.Extension -in @('.cs','.csproj','.fs','.fsproj','.vb','.vbproj','.java','.jar','.py','.pyw')){$failures+=$_.FullName;return}
    if($_.Length -lt 4MB){$text=Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue;if($text -match '(?i)#\s*include\s*[<"](?:boost/|Qt)|\bboost::|\belectron(?:\.exe)?\b|\bnpm\s+(start|run|install|ci)\b'){$failures+=$_.FullName}}
}
if($failures.Count){$failures|Sort-Object -Unique|ForEach-Object{Write-Error $_};throw 'Native-stack gate failed.'}
Write-Host 'Native-stack gate passed.'
