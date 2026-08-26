[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Binary,
    [ValidateSet('primary','fallback')][string]$Role = 'primary',
    [int]$TimeoutSeconds = 10
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) { throw "Binary not found: $Binary" }
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $Binary
$psi.Arguments = 'serve'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.EnvironmentVariables['FORGE_MCP_ROLE'] = $Role

$process = New-Object Diagnostics.Process
$process.StartInfo = $psi
if (-not $process.Start()) { throw 'Failed to start MCP server.' }

$initialize = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"forge-smoke","version":"1"}}}'
$initialized = '{"jsonrpc":"2.0","method":"notifications/initialized"}'
$list = '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
$process.StandardInput.WriteLine($initialize)
$process.StandardInput.WriteLine($initialized)
$process.StandardInput.WriteLine($list)
$process.StandardInput.Flush()

try {
    $readTask = $process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($TimeoutSeconds * 1000)) {
        try { $process.Kill() } catch {}
        throw 'MCP initialize response timed out.'
    }
    $first = $readTask.Result
    $secondTask = $process.StandardOutput.ReadLineAsync()
    if (-not $secondTask.Wait($TimeoutSeconds * 1000)) {
        try { $process.Kill() } catch {}
        throw 'MCP tools/list response timed out.'
    }
    $second = $secondTask.Result
    if ($first -match 'Content-Length' -or $second -match 'Content-Length') {
        throw 'MCP output used prohibited Content-Length framing.'
    }
    $initObject = $first | ConvertFrom-Json
    $listObject = $second | ConvertFrom-Json
}
finally {
    if (-not $process.HasExited) { try { $process.Kill() } catch {} }
    $process.Dispose()
}
$tools = @($listObject.result.tools)
if ($tools.Count -ne 53) { throw "Expected 53 tools, got $($tools.Count)." }
[ordered]@{
    role=$Role
    server_name=$initObject.result.serverInfo.name
    tool_count=$tools.Count
    tools=$tools
} | ConvertTo-Json -Depth 100
