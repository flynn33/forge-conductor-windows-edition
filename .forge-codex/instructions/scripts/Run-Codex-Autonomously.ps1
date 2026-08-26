[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WorkspaceRoot,
    [int]$MaxIterations = 80,
    [string]$CodexCommand = $env:CODEX_COMMAND
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Common.ps1")

if (-not $CodexCommand) {
    $cmd = Get-Command codex -ErrorAction SilentlyContinue
    if ($cmd) { $CodexCommand = $cmd.Source }
}
if (-not $CodexCommand) {
    $next = Join-Path $WorkspaceRoot ".forge-codex\NEXT_PROMPT.md"
    Copy-Item -LiteralPath (Join-Path $WorkspaceRoot ".forge-codex\CODEX_EXECUTION_PROMPT.md") -Destination $next -Force
    Write-Warning "Codex CLI not found. Workspace is prepared at $WorkspaceRoot. Start Codex in that repository; it will read AGENTS.md."
    return
}

$promptPath = Join-Path $WorkspaceRoot ".forge-codex\CODEX_EXECUTION_PROMPT.md"
$statePath = Join-Path $WorkspaceRoot ".forge-codex\state\run-state.json"
$noProgress = 0
$lastHead = (& git -C $WorkspaceRoot rev-parse HEAD 2>$null | Out-String).Trim()
$lastStateHash = if (Test-Path $statePath) { Get-FileSha256 $statePath } else { "" }

for ($iteration = 1; $iteration -le $MaxIterations; $iteration++) {
    $state = Read-JsonFile $statePath
    if ($state.status -eq "completed") {
        Write-Host "Run state is completed."
        break
    }

    $prompt = Get-Content -Raw -LiteralPath $promptPath
    $boundedPrompt = @"
$prompt

This is autonomous bounded session $iteration of $MaxIterations.
Resume from repository state. Execute real work now. Before exit, write a durable handoff.
"@

    # CLI syntax may evolve. Override CODEX_COMMAND with a wrapper when needed.
    & $CodexCommand exec --full-auto -C $WorkspaceRoot $boundedPrompt
    $exit = $LASTEXITCODE

    $head = (& git -C $WorkspaceRoot rev-parse HEAD 2>$null | Out-String).Trim()
    $stateHash = if (Test-Path $statePath) { Get-FileSha256 $statePath } else { "" }
    if ($head -eq $lastHead -and $stateHash -eq $lastStateHash) { $noProgress++ } else { $noProgress = 0 }
    $lastHead = $head
    $lastStateHash = $stateHash

    if ($noProgress -ge 2) {
        throw "Autonomous driver detected two no-progress sessions. Inspect blocker and handoff state."
    }
    if ($exit -ne 0) {
        Add-LedgerEvent -StateRoot (Join-Path $WorkspaceRoot ".forge-codex\state") `
            -Role "builder" -Phase ([string]$state.active_phase) -Action "codex_session_nonzero" `
            -Data @{ iteration=$iteration; exit_code=$exit } | Out-Null
    }
}
