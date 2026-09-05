[CmdletBinding()]
param([Parameter(Mandatory)][string]$Repository)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-NormalizedFullPath -Path $Repository
& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $Repository | Out-Null
$lock = Get-QwenWorkspaceLock -Repository $Repository
$next = & "$PSScriptRoot\Select-Next-Microtask.ps1" -Repository $Repository | ConvertFrom-Json
if (-not $next.id) { throw "No ready microtask: $($next.reason)" }

$plan = Read-Json -Path (Join-Path $Repository '.forge-qwen\state\microtasks.json')
$microtask = $plan.microtasks | Where-Object { $_.id -eq $next.id } | Select-Object -First 1
if (-not $microtask) { throw "Selected microtask missing from plan: $($next.id)" }

foreach ($route in @($microtask.document_routes)) {
    $document = Join-Path $Repository ('.forge-qwen\instructions\' + [string]$route)
    if (-not (Test-Path -LiteralPath $document -PathType Leaf)) { throw "Routed document missing: $route ($document)" }
}

$stepCard = & "$PSScriptRoot\New-QwenStepCard.ps1" -Repository $Repository -Microtask $microtask
$assignment = [ordered]@{
    schema_version = 1
    mission_id = $lock.mission_id
    run_id = $lock.run_id
    repository_fingerprint = $lock.repository_fingerprint
    target_repository = $Repository
    expected_branch = $lock.expected_branch
    memory_namespace = $lock.memory_namespace
    memory_cursor_key = ([string]$lock.memory_namespace + '::cursor')
    microtask = $microtask
    step_card = $stepCard
    preflight_steps = @(
        'Print the current directory with a tool; do not run Git first.',
        'Read WORKSPACE_LOCK.json from this exact directory.',
        'Run Assert-QwenWorkspace.ps1 with RequireCurrentDirectory.',
        'Confirm assignment mission, run ID, fingerprint, target, and branch match the lock.',
        'Retrieve the exact cursor only when persistent_memory is required; otherwise do not call memory.'
    )
    execution_steps = @(
        'Read the generated step card.',
        'Read only routed documents.',
        'Run only routed RAG queries and directly verify every material fact.',
        'Write VERIFIED, INFERRED, UNKNOWN, and BLOCKED facts.',
        'Set the microtask in progress.',
        'Execute each numbered step in order.',
        'Reread writes and run focused tests.',
        'Record commands, exit codes, evidence, and acceptance results.'
    )
    close_steps = @(
        'Validate every changed path against allowed write roots.',
        'Update microtask and run state.',
        'Write a lock-bound checksummed handoff.',
        'Generate MEMORY_CURSOR.json.',
        'Replace only the exact namespaced persistent-memory cursor when required.',
        'Write session-result.json.',
        'Emit FORGE_QWEN_RESULT and end the chat.'
    )
    read_only_roots = $lock.read_only_roots
    allowed_write_roots = $microtask.allowed_write_roots
    forbidden_roots = $lock.forbidden_roots
    required_final_marker = 'FORGE_QWEN_RESULT'
    created_utc = Get-UtcNow
}
$path = Join-Path $Repository '.forge-qwen\state\ACTIVE_ASSIGNMENT.json'
Write-JsonAtomic -Path $path -Value $assignment

$template = Get-Content -Raw -LiteralPath (Join-Path $Repository '.forge-qwen\instructions\lmstudio\LOCK_FIRST_MESSAGE_TEMPLATE.txt')
$message = $template.Replace('{{TARGET_REPOSITORY}}', $Repository).Replace('{{MEMORY_NAMESPACE}}', [string]$lock.memory_namespace).Replace('{{RUN_ID}}', [string]$lock.run_id).Replace('{{WORKSPACE_FINGERPRINT}}', [string]$lock.repository_fingerprint)
$message += "`nACTIVE MICROTASK: $($microtask.id)`nSTEP CARD: $stepCard`nMEMORY CURSOR KEY: $($assignment.memory_cursor_key)`n"
Write-AtomicUtf8 -Path (Join-Path $Repository '.forge-qwen\NEXT_MESSAGE.txt') -Content $message
Write-JsonAtomic -Path (Join-Path $Repository '.forge-qwen\state\session-result.json') -Value ([ordered]@{
    schema_version = 1
    mission_id = $lock.mission_id
    run_id = $lock.run_id
    workspace_fingerprint = $lock.repository_fingerprint
    microtask_id = [string]$microtask.id
    status = 'in_progress'
    handoff = ''
    memory_cursor_file = '.forge-qwen/state/MEMORY_CURSOR.json'
    memory_cursor_key = $assignment.memory_cursor_key
    next_microtask = $null
    summary = 'Assignment prepared; no completion result has been accepted.'
})

$statePath = Join-Path $Repository '.forge-qwen\state\run-state.json'
$state = Read-Json -Path $statePath
$state.active_microtask = [string]$microtask.id
$state.active_assignment = Get-RelativePathPortable -Base $Repository -Target $path
$state.updated_utc = Get-UtcNow
Write-JsonAtomic -Path $statePath -Value $state
& "$PSScriptRoot\Assert-QwenActiveAssignment.ps1" -Repository $Repository | Out-Null
Add-QwenLedgerEvent -Repository $Repository -Role ([string]$microtask.role) -Microtask ([string]$microtask.id) -Action 'assignment_prepared' -Data @{ assignment = $state.active_assignment; step_card = $stepCard; fingerprint = $lock.repository_fingerprint } | Out-Null
$assignment | ConvertTo-Json -Depth 100
