[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)

$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-RepoRoot -Start $Repository
$source=Join-Path $Repository '.forge-qwen\instructions\rag'
$destination=Join-Path $Repository '.forge-qwen\rag'
if(Test-Path -LiteralPath $destination){Remove-Item -LiteralPath $destination -Recurse -Force}
Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
$roots=Read-Json (Join-Path $Repository '.forge-inputs\source-roots.json')
$manifestPath=Join-Path $destination 'RAG_CORPUS_MANIFEST.json'
$manifest=Read-Json $manifestPath
$manifest.external_roots=[ordered]@{macos_source=[ordered]@{path=$roots.macos;index_by_default=$false};forsetti_framework=[ordered]@{path=$roots.forsetti_framework;index_by_default=$false};forsetti_agentic=[ordered]@{path=$roots.forsetti_agentic;index_by_default=$false}}
Write-JsonAtomic -Path $manifestPath -Value $manifest
Write-Host $destination
