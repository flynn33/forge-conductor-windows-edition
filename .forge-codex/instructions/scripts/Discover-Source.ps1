[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$inputs = Join-Path $WorkspaceRoot ".forge-inputs"
$mac = Join-Path $inputs "Forge-Conductor-MacOS-main"
$evidence = Join-Path $WorkspaceRoot ".forge-codex\state\baseline"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

$swift = Get-ChildItem -LiteralPath $mac -Recurse -File -Filter *.swift
$sourceIndex = @($swift | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = Get-RelativePathPortable -BasePath $mac -TargetPath $_.FullName
        bytes = $_.Length
        sha256 = Get-FileSha256 $_.FullName
    }
})
Write-JsonFileAtomic -Path (Join-Path $evidence "macos-swift-source-index.json") -Value ([ordered]@{
    schema_version=1; count=$sourceIndex.Count; files=$sourceIndex
})

$mcpFile = Join-Path $WorkspaceRoot ".forge-codex\instructions\plans\mcp-tool-parity.json"
$mcp = Read-JsonFile $mcpFile
Write-JsonFileAtomic -Path (Join-Path $evidence "mcp-tool-baseline.json") -Value $mcp
Write-Host "Source baseline generated: $evidence"
