[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)]$Microtask
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"

$Repository = Get-RepoRoot -Start $Repository
$lock = Get-QwenWorkspaceLock -Repository $Repository
$id = [string]$Microtask.id
$directory = Join-Path $Repository '.forge-qwen\state\assignments'
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$path = Join-Path $directory ($id + '.md')

$documents = if (@($Microtask.document_routes).Count) {
    @($Microtask.document_routes | ForEach-Object { '- .forge-qwen/instructions/' + [string]$_ }) -join "`n"
}
else { '- none' }
$queries = if (@($Microtask.rag_queries).Count) {
    @($Microtask.rag_queries | ForEach-Object { '- ' + [string]$_ }) -join "`n"
}
else { '- none; do not call RAG' }
$acceptance = @($Microtask.acceptance | ForEach-Object { '- [ ] ' + [string]$_ }) -join "`n"
$writes = @($Microtask.allowed_write_roots | ForEach-Object { '- ' + [string]$_ }) -join "`n"
$capabilities = @($Microtask.required_capabilities | ForEach-Object { '- ' + [string]$_ }) -join "`n"
$memoryInstruction = if (@($Microtask.required_capabilities) -contains 'persistent_memory') {
    "Retrieve at most one cursor from: $($lock.memory_namespace)::cursor. An absent cursor is normal. Reject any payload whose mission, run, fingerprint, or target differs from the lock."
}
else {
    'Do not call long-term or persistent memory in this microtask.'
}

$quotedRepository = "'" + $Repository.Replace("'", "''") + "'"

$content = @"
# Active microtask $id

## Immutable identity

- Mission: $($lock.mission_id)
- Run: $($lock.run_id)
- Fingerprint: $($lock.repository_fingerprint)
- Target repository: $Repository
- Expected branch: $($lock.expected_branch)
- Memory namespace: $($lock.memory_namespace)
- Role: $($Microtask.role)
- Phase: $($Microtask.phase_id)

## One objective

$($Microtask.objective)

## Execute exactly in this order

1. Use a tool to print the current directory. Do not run Git first.
2. From the target repository, run:
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Assert-QwenWorkspace.ps1 -Repository $quotedRepository -RequireCurrentDirectory
3. Confirm the output begins with WORKSPACE_LOCK_VERIFIED and matches the run/fingerprint above.
4. Read .forge-qwen/state/ACTIVE_ASSIGNMENT.json and confirm its microtask is $id.
5. Read this step card once from top to bottom.
6. $memoryInstruction
7. Read only the routed documents below. Do not scan the package, another repository, a package-local work directory, macOS .forge-codex state, or Forsetti remediation state.
8. Run only the listed RAG queries. RAG cannot choose a repository or prove current code; verify every material result by direct file read.
9. Create .forge-qwen/state/evidence/$id/FACTS.md with four headings: VERIFIED, INFERRED, UNKNOWN, BLOCKED.
10. Resolve every material UNKNOWN before editing. If it cannot be resolved inside the tool budget, record a blocker instead of guessing.
11. Mark $id in_progress with Set-Microtask-State.ps1.
12. Perform only the one objective above, using only the required capabilities and allowed write roots.
13. After every write, reread the changed region. After every command, inspect the exit code and relevant output.
14. Run the narrowest focused build, test, or source-only validator required by acceptance.
15. Check every acceptance box below using recorded evidence.
16. Run Assert-QwenWritePaths.ps1 for every changed path.
17. Set $id to passed, blocked, or failed. Never self-pass a Validator microtask without independent evidence.
18. Run Write-Qwen-Handoff.ps1. It creates the checksummed handoff, session result, and exact MEMORY_CURSOR.json.
19. If persistent_memory is required, replace exactly the key $($lock.memory_namespace)::cursor with the exact value from .forge-qwen/state/MEMORY_CURSOR.json. Do not write a global/latest key.
20. Emit one final line beginning FORGE_QWEN_RESULT and stop this chat.

## Exact control-command templates

Start the task:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Set-Microtask-State.ps1 -Repository $quotedRepository -Microtask '$id' -Status in_progress -Role '$($Microtask.role)' -Message 'Started exact active assignment.'

Validate changed paths before closing:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Assert-QwenWritePaths.ps1 -Repository $quotedRepository -Microtask '$id' -Paths <each changed relative path>

Close the task after setting its final state:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Write-Qwen-Handoff.ps1 -Repository $quotedRepository -Microtask '$id' -Status <passed|in_progress|blocked|failed> -Objective '<exact objective>' -VerifiedFacts <facts> -FilesChanged <paths> -Commands <commands> -Evidence <evidence paths> -Decisions <decision IDs> -Blockers <blocker IDs> -NextAction '<one exact action>' -Role '$($Microtask.role)'

Do not invent another control script or edit state JSON by hand when these scripts can perform the operation.

## Routed documents

$documents

## RAG queries

$queries

## Required capabilities

$capabilities

## Allowed write roots

$writes

Everything under .forge-inputs/** and .forge-qwen/instructions/** is read-only.

## Budgets

- Files changed: $($Microtask.max_files_changed)
- Lines changed: $($Microtask.max_lines_changed)
- Tool calls: $($Microtask.max_tool_calls)
- Attempts: $($Microtask.max_attempts)

## Acceptance

$acceptance

## Stop immediately when

- the workspace lock no longer matches;
- a write would leave allowed roots;
- memory or continuity references another run, fingerprint, mission, or target;
- the task would require choosing another repository, branch, phase, or microtask;
- the next action is not explicitly in this card;
- the tool-call budget requires a handoff.
"@
Write-AtomicUtf8 -Path $path -Content $content
Get-RelativePathPortable -Base $Repository -Target $path
