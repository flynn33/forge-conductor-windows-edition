#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$WorkspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [Parameter(Mandatory)][string]$PreviousCandidate,
    [Parameter(Mandatory)][string]$CurrentCandidate,
    [string]$EvidenceRoot = 'out\validation'
)

$ErrorActionPreference = 'Stop'

function Resolve-WorkspacePath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $WorkspaceRoot $Path)).Path
}

function Assert-Equal {
    param($Actual, $Expected, [string]$Message)
    if ($Actual -ne $Expected) {
        throw "$Message Expected '$Expected', received '$Actual'."
    }
}

function Test-Candidate {
    param([string]$CandidatePath, [string]$ExpectedVersion)

    $metadataPath = Join-Path $CandidatePath 'distribution.json'
    $metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-Json -Depth 20
    Assert-Equal $metadata.package_identity 'ForgeConductor.Windows' 'Unexpected package identity.'
    Assert-Equal $metadata.package_version "$ExpectedVersion.0" 'Unexpected package version.'
    Assert-Equal $metadata.architecture 'x64' 'Unexpected package architecture.'

    $packagePath = Join-Path $CandidatePath $metadata.package
    $packageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $packagePath).Hash.ToLowerInvariant()
    Assert-Equal $packageHash ([string]$metadata.sha256).ToLowerInvariant() 'The package digest does not match distribution.json.'
    Assert-Equal (Get-AuthenticodeSignature -LiteralPath $packagePath).Status 'Valid' 'The package signature is not valid.'

    $manifestPath = Join-Path $CandidatePath 'payload\payload-manifest.json'
    $manifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
    Assert-Equal $manifestHash ([string]$metadata.payload_manifest_sha256).ToLowerInvariant() 'The payload manifest digest does not match distribution.json.'
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json -Depth 20
    foreach ($file in $manifest.files) {
        $payloadFile = Join-Path (Join-Path $CandidatePath 'payload') ([string]$file.path)
        if (-not (Test-Path -LiteralPath $payloadFile -PathType Leaf)) {
            throw "The payload file '$($file.path)' is missing."
        }
        $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadFile).Hash.ToLowerInvariant()
        Assert-Equal $digest ([string]$file.sha256).ToLowerInvariant() "Payload digest mismatch for '$($file.path)'."
    }

    [ordered]@{
        path = $CandidatePath
        version = $ExpectedVersion
        package_sha256 = $packageHash
        payload_manifest_sha256 = $manifestHash
        payload_file_count = @($manifest.files).Count
    }
}

function Assert-DisposableInstallPath {
    param([string]$InstallPath, [string]$RunPath)
    $resolvedInstall = [IO.Path]::GetFullPath($InstallPath).TrimEnd('\')
    $resolvedRun = [IO.Path]::GetFullPath($RunPath).TrimEnd('\')
    if (-not $resolvedInstall.StartsWith($resolvedRun + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace install path outside the disposable run root: $resolvedInstall"
    }
}

function Install-Payload {
    param([string]$CandidatePath, [string]$InstallPath, [string]$RunPath)
    Assert-DisposableInstallPath $InstallPath $RunPath
    if (Test-Path -LiteralPath $InstallPath) {
        Remove-Item -LiteralPath $InstallPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
    Copy-Item -Path (Join-Path $CandidatePath 'payload\*') -Destination $InstallPath -Recurse -Force
    return (Resolve-Path -LiteralPath (Join-Path $InstallPath 'forge-conductor.exe')).Path
}

function Uninstall-Payload {
    param([string]$InstallPath, [string]$RunPath)
    Assert-DisposableInstallPath $InstallPath $RunPath
    if (Test-Path -LiteralPath $InstallPath) {
        Remove-Item -LiteralPath $InstallPath -Recurse -Force
    }
    if (Test-Path -LiteralPath $InstallPath) {
        throw 'The simulated package root still exists after uninstall.'
    }
}

function Test-ExecutableVersion {
    param([string]$Executable, [string]$ExpectedVersion)
    $versionText = (& $Executable version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $versionText -notmatch ([regex]::Escape($ExpectedVersion))) {
        throw "Version command failed for $Executable. Output: $versionText"
    }
    $selfTest = (& $Executable --self-test | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Self-test failed for $Executable. Output: $selfTest"
    }
    return [ordered]@{ version_output = $versionText; self_test_output = $selfTest }
}

function Start-McpSession {
    param([string]$Executable, [string]$DataRoot, [string]$WorkingDirectory)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    [void]$start.ArgumentList.Add('serve')
    [void]$start.ArgumentList.Add('--home')
    [void]$start.ArgumentList.Add($DataRoot)
    $start.WorkingDirectory = $WorkingDirectory
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
    $pendingLine = $Process.StandardOutput.ReadLineAsync()
    if (-not $pendingLine.Wait(30000)) { $Process.Kill($true); throw 'MCP response exceeded 30 seconds.' }
    $line = $pendingLine.Result
    if ($null -eq $line) {
        throw "MCP stdout ended before response $Id. $($Process.StandardError.ReadToEnd())"
    }
    $response = $line | ConvertFrom-Json -Depth 30
    if ($response.id -ne $Id) {
        throw "Expected MCP response $Id but received '$($response.id)'."
    }
    if ($null -ne $response.error) {
        throw "MCP response $Id failed: $($response.error.code): $($response.error.message)"
    }
    return $response
}

function Initialize-Session {
    param([Diagnostics.Process]$Process, [string]$ExpectedVersion)
    Send-Frame $Process @{
        jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{
            protocolVersion = '2025-11-25'; capabilities = @{}
            clientInfo = @{ name = 'forge-conductor-r6-lifecycle'; version = '1.0' }
        }
    }
    $initialized = Read-Response $Process 1
    Assert-Equal $initialized.result.serverInfo.version $ExpectedVersion 'Unexpected MCP server version.'
    Send-Frame $Process @{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} }
    Send-Frame $Process @{ jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{} }
    return @((Read-Response $Process 2).result.tools.name)
}

function Invoke-Tool {
    param([Diagnostics.Process]$Process, [long]$Id, [string]$Name, [hashtable]$Arguments)
    Send-Frame $Process @{
        jsonrpc = '2.0'; id = $Id; method = 'tools/call'
        params = @{ name = $Name; arguments = $Arguments }
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
    param([string]$DataRoot, [string]$DisplayName)
    $registryPath = Join-Path $DataRoot 'projects\registry.json'
    $registry = Get-Content -Raw -LiteralPath $registryPath | ConvertFrom-Json -Depth 20
    foreach ($project in $registry.projects) {
        if ([string]$project.displayName -eq $DisplayName) { return $project }
    }
    throw "The startup project '$DisplayName' is absent from $registryPath."
}

function Get-FileDigest {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$workspace = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$previousPath = Resolve-WorkspacePath $PreviousCandidate
$currentPath = Resolve-WorkspacePath $CurrentCandidate
$evidenceBase = if ([IO.Path]::IsPathRooted($EvidenceRoot)) {
    [IO.Path]::GetFullPath($EvidenceRoot)
} else {
    [IO.Path]::GetFullPath((Join-Path $workspace $EvidenceRoot))
}
New-Item -ItemType Directory -Path $evidenceBase -Force | Out-Null
$runRoot = Join-Path $evidenceBase ("r6-simulated-lifecycle-{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8)))
$installRoot = Join-Path $runRoot 'simulated-package'
$profileRoot = Join-Path $runRoot 'profile'
$projectA = Join-Path $runRoot 'workspaces\project-a'
$projectB = Join-Path $runRoot 'workspaces\project-b'
$foreignRoot = Join-Path $runRoot 'external-lm-studio'
foreach ($directory in @($runRoot, $profileRoot, $projectA, $projectB, $foreignRoot)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

$foreignConfig = Join-Path $foreignRoot 'mcp.json'
$foreignContent = '{"mcpServers":{"owner-managed":{"command":"existing-server.exe"}}}'
Set-Content -LiteralPath $foreignConfig -Value $foreignContent -NoNewline -Encoding utf8
$foreignDigest = Get-FileDigest $foreignConfig

$previousProduct = [string](Get-Content -Raw -LiteralPath (Join-Path $previousPath 'distribution.json') | ConvertFrom-Json).product_version
$currentProduct = [string](Get-Content -Raw -LiteralPath (Join-Path $currentPath 'distribution.json') | ConvertFrom-Json).product_version
$previousValidation = Test-Candidate $previousPath $previousProduct
$currentValidation = Test-Candidate $currentPath $currentProduct
$previousVersion = [version]$previousValidation.version
$currentVersion = [version]$currentValidation.version
if ($currentVersion -le $previousVersion) { throw 'The current candidate is not newer than the retained candidate.' }

$previousExecutable = Install-Payload $previousPath $installRoot $runRoot
$previousExecutableResult = Test-ExecutableVersion $previousExecutable $previousProduct

$sessionA = Start-McpSession $previousExecutable $profileRoot $projectA
$previousTools = Initialize-Session $sessionA $previousProduct
foreach ($requiredTool in @('fs_read', 'fs_write', 'memory_get', 'memory_set', 'project_memory.initialize', 'project_memory.remember', 'project_memory.search')) {
    if ($requiredTool -notin $previousTools) { throw "The 0.9.4 candidate is missing required tool '$requiredTool'." }
}
$registeredA = Get-RegisteredProject $profileRoot 'project-a'
$projectAResult = Invoke-Tool $sessionA 3 'project_memory.initialize' @{
    project_path = [string]$registeredA.aliases[0]; project_id = [string]$registeredA.id
    display_name = 'Lifecycle Project A'; idempotency_key = 'r6-lifecycle-project-a'
}
$projectAId = [string]$projectAResult.project_id
$write = Invoke-Tool $sessionA 4 'fs_write' @{ path = 'lifecycle-proof.txt'; content = 'created by 0.9.4; retained by 0.9.5' }
if (-not $write.ok) { throw 'The 0.9.4 filesystem marker was not written.' }
$legacy = Invoke-Tool $sessionA 5 'memory_set' @{ key = 'r6/lifecycle'; body = 'legacy marker from 0.9.4'; tags = @('r6', 'lifecycle') }
if (-not $legacy.ok) { throw 'The 0.9.4 legacy memory marker was not written.' }
$remember = Invoke-Tool $sessionA 6 'project_memory.remember' @{
    project_id = $projectAId; kind = 'decision'; title = 'R6 lifecycle marker'
    summary = 'project memory created by 0.9.4'; body = 'retained-A'
    tags = @('r6', 'lifecycle'); idempotency_key = 'r6-lifecycle-memory-a'
}
if (-not $remember.ok) { throw 'The 0.9.4 project memory marker was not written.' }
Stop-McpSession $sessionA

$sessionB = Start-McpSession $previousExecutable $profileRoot $projectB
[void](Initialize-Session $sessionB $previousProduct)
$registeredB = Get-RegisteredProject $profileRoot 'project-b'
$projectBResult = Invoke-Tool $sessionB 3 'project_memory.initialize' @{
    project_path = [string]$registeredB.aliases[0]; project_id = [string]$registeredB.id
    display_name = 'Lifecycle Project B'; idempotency_key = 'r6-lifecycle-project-b'
}
$projectBId = [string]$projectBResult.project_id
$isolated = Invoke-Tool $sessionB 4 'project_memory.search' @{ project_id = $projectBId; query = 'R6 lifecycle marker'; include_body = $true }
if (@($isolated.records).Count -ne 0 -or $projectAId -eq $projectBId) { throw 'Project memory isolation failed under 0.9.4.' }
Stop-McpSession $sessionB

$configPath = Join-Path $profileRoot 'config\config.json'
New-Item -ItemType Directory -Path (Split-Path -Parent $configPath) -Force | Out-Null
$configuration = [ordered]@{
    schema_version = 1
    shell = [ordered]@{ enabled = $true; default_timeout_sec = 17 }
    dashboard = [ordered]@{ refresh_interval_sec = 9 }
}
$configuration | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $configPath -NoNewline -Encoding utf8
$settingsDigest = Get-FileDigest $configPath

$currentExecutable = Install-Payload $currentPath $installRoot $runRoot
Assert-Equal (Get-FileDigest $configPath) $settingsDigest 'Settings changed while upgrading the simulated package root.'
Assert-Equal (Get-FileDigest $foreignConfig) $foreignDigest 'The unrelated LM Studio configuration changed during upgrade.'
$currentExecutableResult = Test-ExecutableVersion $currentExecutable $currentProduct

$upgradeSession = Start-McpSession $currentExecutable $profileRoot $projectA
$currentTools = Initialize-Session $upgradeSession $currentProduct
$read = Invoke-Tool $upgradeSession 3 'fs_read' @{ path = 'lifecycle-proof.txt' }
if ([string]$read.content -notmatch 'retained by 0.9.5') { throw 'The workspace marker did not survive upgrade.' }
$legacyAfterUpgrade = Invoke-Tool $upgradeSession 4 'memory_get' @{ key = 'r6/lifecycle' }
if ([string]$legacyAfterUpgrade.note.body -ne 'legacy marker from 0.9.4') { throw 'Legacy memory did not survive upgrade.' }
$projectAfterUpgrade = Invoke-Tool $upgradeSession 5 'project_memory.search' @{ project_id = $projectAId; query = 'R6 lifecycle marker'; include_body = $true }
if (@($projectAfterUpgrade.records).Count -ne 1) { throw 'Project memory did not survive upgrade.' }
$shell = Invoke-Tool $upgradeSession 6 'shell_exec' @{ command = "Write-Output 'r6-upgrade-ok'" }
if ([string]$shell.stdout -notmatch 'r6-upgrade-ok') { throw 'The current candidate did not execute a real shell tool call.' }
Stop-McpSession $upgradeSession
Assert-Equal (Get-FileDigest $configPath) $settingsDigest 'Settings changed after the upgraded candidate ran.'
Assert-Equal (Get-FileDigest $foreignConfig) $foreignDigest 'The unrelated LM Studio configuration changed after upgrade.'

Uninstall-Payload $installRoot $runRoot
Assert-Equal (Get-FileDigest $configPath) $settingsDigest 'Settings changed during simulated uninstall.'
Assert-Equal (Get-FileDigest $foreignConfig) $foreignDigest 'The unrelated LM Studio configuration changed during simulated uninstall.'
if (-not (Test-Path -LiteralPath (Join-Path $projectA 'lifecycle-proof.txt') -PathType Leaf)) {
    throw 'The workspace marker was removed during simulated uninstall.'
}

$reinstalledExecutable = Install-Payload $currentPath $installRoot $runRoot
[void](Test-ExecutableVersion $reinstalledExecutable $currentProduct)
Assert-Equal (Get-FileDigest $configPath) $settingsDigest 'Settings changed during simulated reinstall.'
Assert-Equal (Get-FileDigest $foreignConfig) $foreignDigest 'The unrelated LM Studio configuration changed during simulated reinstall.'
$reinstallSession = Start-McpSession $reinstalledExecutable $profileRoot $projectA
[void](Initialize-Session $reinstallSession $currentProduct)
$legacyAfterReinstall = Invoke-Tool $reinstallSession 3 'memory_get' @{ key = 'r6/lifecycle' }
$projectAfterReinstall = Invoke-Tool $reinstallSession 4 'project_memory.search' @{ project_id = $projectAId; query = 'R6 lifecycle marker'; include_body = $true }
Stop-McpSession $reinstallSession
if ([string]$legacyAfterReinstall.note.body -ne 'legacy marker from 0.9.4' -or @($projectAfterReinstall.records).Count -ne 1) {
    throw 'Persisted memory was not available after simulated reinstall.'
}

$result = [ordered]@{
    ok = $true
    test_kind = 'isolated-simulated-installed-lifecycle'
    previous_candidate = $previousValidation
    current_candidate = $currentValidation
    previous_executable = $previousExecutableResult
    current_executable = $currentExecutableResult
    upgrade = [ordered]@{
        profile_preserved = $true
        settings_preserved = $true
        workspace_preserved = $true
        legacy_memory_preserved = $true
        project_memory_preserved = $true
        project_isolation_preserved = $true
        current_tool_count = $currentTools.Count
    }
    uninstall_reinstall = [ordered]@{
        package_root_removed = $true
        profile_preserved = $true
        settings_preserved = $true
        workspace_preserved = $true
        memory_preserved = $true
    }
    external_lm_studio_configuration_preserved = $true
    project_a_id = $projectAId
    project_b_id = $projectBId
    evidence_root = $runRoot
}
$receiptPath = Join-Path $runRoot 'lifecycle-result.json'
$result | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $receiptPath -NoNewline -Encoding utf8
$result | ConvertTo-Json -Depth 20
