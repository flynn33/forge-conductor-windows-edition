#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Executable,
    [Parameter(Mandatory)]
    [string]$DataRoot,
    [Parameter(Mandatory)]
    [string]$ProjectA,
    [Parameter(Mandatory)]
    [string]$ProjectB
)

$ErrorActionPreference = 'Stop'

function Start-McpSession {
    param([string]$WorkingDirectory)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Resolve-Path -LiteralPath $Executable).Path
    [void]$start.ArgumentList.Add('serve')
    [void]$start.ArgumentList.Add('--home')
    [void]$start.ArgumentList.Add((Resolve-Path -LiteralPath $DataRoot).Path)
    $start.WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'The native MCP process did not start.' }
    $process.StandardInput.NewLine = "`n"
    return $process
}

function Send-Frame {
    param([Diagnostics.Process]$Process, [hashtable]$Frame)
    $Process.StandardInput.WriteLine(($Frame | ConvertTo-Json -Depth 20 -Compress))
    $Process.StandardInput.Flush()
}

function Read-Response {
    param([Diagnostics.Process]$Process, [long]$Id)
    $line = $Process.StandardOutput.ReadLine()
    if ($null -eq $line) {
        $errorText = $Process.StandardError.ReadToEnd()
        throw "MCP stdout ended before response $Id. $errorText"
    }
    $response = $line | ConvertFrom-Json -Depth 30
    if ($response.id -ne $Id) {
        throw "Expected response $Id but received $($response.id). Frame: $line"
    }
    if ($null -ne $response.error) {
        throw "MCP response $Id failed: $($response.error.code): $($response.error.message)"
    }
    return $response
}

function Initialize-Session {
    param([Diagnostics.Process]$Process)
    Send-Frame $Process @{
        jsonrpc='2.0'; id=1; method='initialize'; params=@{
            protocolVersion='2025-11-25'; capabilities=@{};
            clientInfo=@{name='forge-conductor-alpha-smoke';version='1.0'}
        }
    }
    $initialized = Read-Response $Process 1
    if ($initialized.result.serverInfo.version -ne '1.1.20') {
        throw 'The MCP server version was not 1.1.20.'
    }
    Send-Frame $Process @{jsonrpc='2.0';method='notifications/initialized';params=@{}}
    Send-Frame $Process @{jsonrpc='2.0';id=2;method='tools/list';params=@{}}
    $listed = Read-Response $Process 2
    return @($listed.result.tools.name)
}

function Invoke-Tool {
    param(
        [Diagnostics.Process]$Process,
        [long]$Id,
        [string]$Name,
        [hashtable]$Arguments
    )
    Send-Frame $Process @{
        jsonrpc='2.0'; id=$Id; method='tools/call';
        params=@{name=$Name;arguments=$Arguments}
    }
    $response = Read-Response $Process $Id
    if ($response.result.isError) {
        throw "$Name returned a tool error: $($response.result.content[0].text)"
    }
    return $response.result.structuredContent
}

function Stop-McpSession {
    param([Diagnostics.Process]$Process)
    $Process.StandardInput.Close()
    if (-not $Process.WaitForExit(10000)) {
        $Process.Kill($true)
        throw 'The native MCP process did not stop after stdin closed.'
    }
    $stderr = $Process.StandardError.ReadToEnd()
    if ($Process.ExitCode -ne 0) {
        throw "The native MCP process exited $($Process.ExitCode): $stderr"
    }
    $Process.Dispose()
}

function Get-RegisteredProject {
    param([string]$DisplayName)
    $registryPath = Join-Path $DataRoot 'projects\registry.json'
    $registry = Get-Content -Raw -LiteralPath $registryPath | ConvertFrom-Json -Depth 20
    foreach ($project in $registry.projects) {
        if ([string]$project.displayName -eq $DisplayName) { return $project }
    }
    throw "The startup project '$DisplayName' is absent from $registryPath."
}

$requiredTools = @(
    'fs_read','fs_write','git_status','memory_get','memory_set',
    'project_memory.initialize','project_memory.remember','project_memory.search',
    'search_text','shell_exec'
)

$sessionA = Start-McpSession $ProjectA
$tools = Initialize-Session $sessionA
foreach ($tool in $requiredTools) {
    if ($tool -notin $tools) { throw "Required tool is absent: $tool" }
}

$registeredA = Get-RegisteredProject 'project-a'
$projectAResult = Invoke-Tool $sessionA 3 'project_memory.initialize' @{
    project_path=[string]$registeredA.aliases[0]
    project_id=[string]$registeredA.id
    display_name='Alpha Project A'
    idempotency_key='alpha-p2-project-a'
}
$projectAId = [string]$projectAResult.project_id
if ([string]::IsNullOrWhiteSpace($projectAId)) { throw 'Project A did not return an identity.' }

$write = Invoke-Tool $sessionA 4 'fs_write' @{
    path='alpha-tool-proof.txt'; content='native filesystem and search proof'
}
if (-not $write.ok) { throw 'fs_write did not report success.' }
$read = Invoke-Tool $sessionA 5 'fs_read' @{path='alpha-tool-proof.txt'}
if ([string]$read.content -notmatch 'native filesystem') { throw 'fs_read did not return written content.' }
$search = Invoke-Tool $sessionA 6 'search_text' @{pattern='search proof'}
if (-not $search.ok) { throw 'search_text did not report success.' }
$git = Invoke-Tool $sessionA 7 'git_status' @{}
if (-not $git.ok) { throw 'git_status did not report success.' }
$shell = Invoke-Tool $sessionA 8 'shell_exec' @{
    command="Write-Output 'alpha-shell-ok'"
}
if ([string]$shell.stdout -notmatch 'alpha-shell-ok') { throw 'shell_exec did not return real PowerShell output.' }
$legacy = Invoke-Tool $sessionA 9 'memory_set' @{
    key='alpha/p2/restart';body='legacy memory survives process restart';tags=@('alpha','p2')
}
if (-not $legacy.ok) { throw 'memory_set did not report success.' }
$remembered = Invoke-Tool $sessionA 10 'project_memory.remember' @{
    project_id=$projectAId;kind='decision';title='P2 persistence';
    summary='Project A memory survives process restart';body='isolated-A';
    tags=@('alpha','p2');idempotency_key='alpha-p2-memory-a'
}
if (-not $remembered.ok) { throw 'project_memory.remember did not report success.' }
Stop-McpSession $sessionA

$sessionB = Start-McpSession $ProjectB
[void](Initialize-Session $sessionB)
$registeredB = Get-RegisteredProject 'project-b'
$projectBResult = Invoke-Tool $sessionB 3 'project_memory.initialize' @{
    project_path=[string]$registeredB.aliases[0]
    project_id=[string]$registeredB.id
    display_name='Alpha Project B'
    idempotency_key='alpha-p2-project-b'
}
$projectBId = [string]$projectBResult.project_id
if ($projectBId -eq $projectAId) { throw 'Distinct project roots received the same identity.' }
$isolated = Invoke-Tool $sessionB 4 'project_memory.search' @{
    project_id=$projectBId;query='P2 persistence';include_body=$true
}
if (@($isolated.records).Count -ne 0) { throw 'Project A memory leaked into Project B.' }
Stop-McpSession $sessionB

$sessionRestart = Start-McpSession $ProjectA
[void](Initialize-Session $sessionRestart)
$legacyAfterRestart = Invoke-Tool $sessionRestart 3 'memory_get' @{key='alpha/p2/restart'}
if ([string]$legacyAfterRestart.note.body -notmatch 'survives process restart') {
    throw 'Legacy memory did not survive process restart.'
}
$projectAfterRestart = Invoke-Tool $sessionRestart 4 'project_memory.search' @{
    project_id=$projectAId;query='P2 persistence';include_body=$true
}
if (@($projectAfterRestart.records).Count -ne 1) {
    throw 'Project A memory did not survive process restart.'
}
Stop-McpSession $sessionRestart

[ordered]@{
    ok=$true
    tool_count=$tools.Count
    project_a_id=$projectAId
    project_b_id=$projectBId
    projects_isolated=$true
    filesystem_read_write=$true
    text_search=$true
    git_status=$true
    shell_exec=$true
    legacy_memory_restart=$true
    project_memory_restart=$true
    home=(Resolve-Path -LiteralPath $DataRoot).Path
} | ConvertTo-Json -Depth 5
