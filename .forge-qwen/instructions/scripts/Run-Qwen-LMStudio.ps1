[CmdletBinding()]
param(
    [string]$WorkspaceRoot = (Get-Location).Path,
    [string]$ConfigPath,
    [int]$MaxMicrotasks = 0
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$WorkspaceRoot = Get-RepoRoot -Start $WorkspaceRoot
& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $WorkspaceRoot | Out-Null
$lock = Get-QwenWorkspaceLock -Repository $WorkspaceRoot

if (-not $ConfigPath) { $ConfigPath = Join-Path $WorkspaceRoot '.forge-qwen\config\runner-config.json' }
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw "Runner config missing: $ConfigPath" }
$config = Read-Json -Path $ConfigPath
if ([string]$config.model_id -like 'REPLACE*') { throw 'An exact LM Studio model_id is required.' }
if (@($config.plugin_bindings).Count -eq 0) { throw 'At least one exact capability/plugin binding is required.' }
foreach ($binding in @($config.plugin_bindings)) {
    if ([string]$binding.id -notmatch '^mcp/' -or [string]$binding.id -like '*REPLACE*') {
        throw "Invalid plugin binding for $($binding.capability)."
    }
}

$token = $env:LM_API_TOKEN
if (-not $token) { throw 'LM_API_TOKEN is required for autonomous API mode.' }
$headers = @{ Authorization = ('Bearer ' + $token) }
$limit = if ($MaxMicrotasks -gt 0) { $MaxMicrotasks } else { [int]$config.maximum_microtasks_per_run }
$noProgress = 0

function Get-ResponseText {
    param($Response)
    $parts = @()
    foreach ($output in @($Response.output)) {
        if ($output.content -is [string]) {
            $parts += [string]$output.content
            continue
        }
        foreach ($content in @($output.content)) {
            if ($content.PSObject.Properties.Name -contains 'text') { $parts += [string]$content.text }
            elseif ($content.PSObject.Properties.Name -contains 'content') { $parts += [string]$content.content }
        }
    }
    return ($parts -join "`n")
}

function Get-IntegrationSet {
    param(
        $Microtask,
        $Configuration,
        [string]$Repository
    )
    $required = @($Microtask.required_capabilities)
    $stateBindingsPath = Join-Path $Repository '.forge-qwen\state\tool-bindings.json'
    $verified = @()
    if (Test-Path -LiteralPath $stateBindingsPath) {
        $stateBindings = Read-Json -Path $stateBindingsPath
        $verified = @($stateBindings.bindings | Where-Object { $_.status -eq 'verified' -and $_.plugin_id })
    }

    $result = @()
    $seen = @{}
    foreach ($capability in $required) {
        $candidate = $verified | Where-Object { $_.capability -eq $capability } | Select-Object -First 1
        if ($candidate) {
            $id = [string]$candidate.plugin_id
            $allowed = @($candidate.tool_names)
        }
        else {
            $candidate = @($Configuration.plugin_bindings) | Where-Object { $_.capability -eq $capability } | Select-Object -First 1
            if (-not $candidate) { throw "No plugin binding for capability: $capability" }
            $id = [string]$candidate.id
            $allowed = @($candidate.allowed_tools)
        }

        if (-not $seen.ContainsKey($id)) {
            $integration = [ordered]@{ type = 'plugin'; id = $id }
            if ($allowed.Count) { $integration['allowed_tools'] = $allowed }
            $result += $integration
            $seen[$id] = $integration
        }
        elseif ($allowed.Count) {
            $existing = $seen[$id]
            $existing['allowed_tools'] = @(@($existing.allowed_tools) + $allowed) | Select-Object -Unique
        }
    }
    return @($result)
}

for ($iteration = 1; $iteration -le $limit; $iteration++) {
    & "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $WorkspaceRoot | Out-Null
    $assignment = & "$PSScriptRoot\Prepare-QwenAssignment.ps1" -Repository $WorkspaceRoot | ConvertFrom-Json
    & "$PSScriptRoot\Assert-QwenActiveAssignment.ps1" -Repository $WorkspaceRoot | Out-Null
    $microtask = $assignment.microtask

    & "$PSScriptRoot\Set-Microtask-State.ps1" -Repository $WorkspaceRoot -Microtask $microtask.id -Status in_progress -Role $microtask.role -Message 'Guided LM Studio controller started a fresh lock-bound context.'

    $system = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'QWEN_SYSTEM_PROMPT.txt')
    $prompt = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot '.forge-qwen\NEXT_MESSAGE.txt')
    $prompt += "`n`nACTIVE_ASSIGNMENT_JSON:`n" + ($assignment | ConvertTo-Json -Depth 100) + "`n`nExecute the numbered step card now. Do not select another task."
    $integrations = Get-IntegrationSet -Microtask $microtask -Configuration $config -Repository $WorkspaceRoot
    $microtaskStatePath = Join-Path $WorkspaceRoot '.forge-qwen\state\microtasks.json'
    $before = Get-Sha256 -Path $microtaskStatePath
    $previous = $null
    $completed = $false

    for ($turn = 0; $turn -le [int]$config.maximum_continuations_per_microtask; $turn++) {
        $input = if ($turn -eq 0) {
            $prompt
        }
        else {
            'Re-run the workspace assertion. Re-read ACTIVE_ASSIGNMENT.json and the same step card. Continue only that microtask. Finish evidence, state, handoff, MEMORY_CURSOR.json, persistent cursor replacement when assigned, session result, and marker.'
        }

        $body = [ordered]@{
            model = $config.model_id
            input = $input
            system_prompt = $system
            context_length = [int]$config.context_length
            max_output_tokens = [int]$config.max_output_tokens
            temperature = [double]$config.temperature
            top_p = [double]$config.top_p
            top_k = [int]$config.top_k
            repeat_penalty = [double]$config.repeat_penalty
            reasoning = $config.reasoning
            integrations = $integrations
        }
        if ($previous) { $body['previous_response_id'] = $previous }

        $uri = $config.api_base.TrimEnd('/') + '/api/v1/chat'
        $response = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 100) -TimeoutSec ([int]$config.request_timeout_seconds)
        $raw = Join-Path $WorkspaceRoot ('.forge-qwen\state\responses\' + $microtask.id + '-' + $iteration + '-' + $turn + '.json')
        Write-JsonAtomic -Path $raw -Value $response
        Write-AtomicUtf8 -Path ([IO.Path]::ChangeExtension($raw, '.txt')) -Content (Get-ResponseText -Response $response)
        $previous = $response.response_id

        $result = Read-Json -Path (Join-Path $WorkspaceRoot '.forge-qwen\state\session-result.json')
        if (
            $result.mission_id -ne $lock.mission_id -or
            $result.run_id -ne $lock.run_id -or
            $result.workspace_fingerprint -ne $lock.repository_fingerprint -or
            $result.microtask_id -ne $microtask.id
        ) {
            throw 'Foreign, stale, or wrong-microtask session result detected.'
        }

        $currentPlan = Read-Json -Path $microtaskStatePath
        $current = $currentPlan.microtasks | Where-Object { $_.id -eq $microtask.id } | Select-Object -First 1
        if ($current.status -eq 'passed') { $completed = $true; break }
        if (-not $previous) { break }
    }

    $after = Get-Sha256 -Path $microtaskStatePath
    if ($after -eq $before) { $noProgress++ } else { $noProgress = 0 }
    if ($noProgress -ge 2) { throw 'Two lock-bound sessions produced no durable microtask-state change.' }
    if (-not $completed) { Write-Warning "Microtask $($microtask.id) remains incomplete; a new lock-bound context will resume only that task." }
}
