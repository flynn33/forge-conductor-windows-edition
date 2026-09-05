[CmdletBinding()]
param([string]$Repository=(Get-Location).Path,[string]$McpJson)

$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-RepoRoot -Start $Repository
$candidates=@()
if($McpJson){$candidates+=$McpJson}
if($env:LM_STUDIO_MCP_JSON){$candidates+=$env:LM_STUDIO_MCP_JSON}
$candidates+=@((Join-Path $env:USERPROFILE '.lmstudio\mcp.json'),(Join-Path $env:APPDATA 'LM Studio\mcp.json'),(Join-Path $env:LOCALAPPDATA 'LM Studio\mcp.json'))
$candidates=@($candidates|Select-Object -Unique)
$found=$null
foreach($candidate in $candidates){if(Test-Path -LiteralPath $candidate){try{$json=Read-Json $candidate;if($json.mcpServers){$found=$candidate;break}}catch{}}}
$servers=@()
if($found){
    $json=Read-Json $found
    foreach($property in $json.mcpServers.PSObject.Properties){
        $label=$property.Name
        $capabilities=@()
        $patterns=@(
            @('shell','shell|terminal|powershell'),@('filesystem','file.?system|files'),@('github','github'),
            @('rag_v1','rag.?v?1|retriev'),@('js_sandbox','js|javascript.*sandbox|code.?sandbox'),
            @('long_term_memory','long.?term.*memory|ltm'),@('persistent_memory','persistent.*memory')
        )
        foreach($pair in $patterns){if($label -match $pair[1]){$capabilities+=$pair[0]}}
        $servers+=[ordered]@{label=$label;plugin_id=('mcp/'+$label);capability_candidates=$capabilities}
    }
}
$result=[ordered]@{utc=Get-UtcNow;mcp_json=$found;servers=$servers;note='Candidates are unverified until harmless probes in BOOT-TOOLS-001.'}
$path=Join-Path $Repository '.forge-qwen\state\profiles\lmstudio.json'
Write-JsonAtomic -Path $path -Value $result
$result|ConvertTo-Json -Depth 20
