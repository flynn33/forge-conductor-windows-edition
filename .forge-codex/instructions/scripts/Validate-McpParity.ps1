[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [Parameter(Mandatory)][string]$ActualToolSnapshot
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

$expected = Read-JsonFile (Join-Path $WorkspaceRoot ".forge-codex\instructions\plans\mcp-tool-parity.json")
$actual = Read-JsonFile $ActualToolSnapshot
$expectedNames = @($expected.tools | ForEach-Object name)
$actualNames = @($actual.tools | ForEach-Object name)
if ($actualNames.Count -ne $expected.expected_tool_count) {
    throw "Expected $($expected.expected_tool_count) tools, got $($actualNames.Count)."
}
$missing = @($expectedNames | Where-Object { $_ -notin $actualNames })
$extra = @($actualNames | Where-Object { $_ -notin $expectedNames })
if ($missing.Count -or $extra.Count) {
    throw "MCP tool mismatch. Missing: $($missing -join ', '). Extra: $($extra -join ', ')."
}
Write-Host "MCP name parity passed: $($actualNames.Count) tools. Schema semantic comparison must also pass in native tests."
