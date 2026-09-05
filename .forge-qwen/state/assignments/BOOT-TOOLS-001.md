# Active microtask BOOT-TOOLS-001

## Immutable identity

- Mission: forge-conductor-windows11-port-qwen-guided-v2
- Run: 1df367e1-3584-41e9-b2cb-8fbbe721addf
- Fingerprint: 98601f23820832b5542bc29ed245d0d9b984cc8435af37403ba1af8ac8eafdb7
- Target repository: D:\GitHub\Forge-Conductor-Windows-Edition
- Expected branch: forge-windows-port-1df367e1
- Memory namespace: forge_windows_port::1df367e1-3584-41e9-b2cb-8fbbe721addf::98601f23820832b5
- Role: builder
- Phase: P00

## One objective

Bind exact LM Studio tool/plugin/function names and argument shapes, run harmless probes, and persist verified bindings.

## Execute exactly in this order

1. Use a tool to print the current directory. Do not run Git first.
2. From the target repository, run:
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Assert-QwenWorkspace.ps1 -Repository 'D:\GitHub\Forge-Conductor-Windows-Edition' -RequireCurrentDirectory
3. Confirm the output begins with WORKSPACE_LOCK_VERIFIED and matches the run/fingerprint above.
4. Read .forge-qwen/state/ACTIVE_ASSIGNMENT.json and confirm its microtask is BOOT-TOOLS-001.
5. Read this step card once from top to bottom.
6. Retrieve at most one cursor from: forge_windows_port::1df367e1-3584-41e9-b2cb-8fbbe721addf::98601f23820832b5::cursor. An absent cursor is normal. Reject any payload whose mission, run, fingerprint, or target differs from the lock.
7. Read only the routed documents below. Do not scan the package, another repository, a package-local work directory, macOS .forge-codex state, or Forsetti remediation state.
8. Run only the listed RAG queries. RAG cannot choose a repository or prove current code; verify every material result by direct file read.
9. Create .forge-qwen/state/evidence/BOOT-TOOLS-001/FACTS.md with four headings: VERIFIED, INFERRED, UNKNOWN, BLOCKED.
10. Resolve every material UNKNOWN before editing. If it cannot be resolved inside the tool budget, record a blocker instead of guessing.
11. Mark BOOT-TOOLS-001 in_progress with Set-Microtask-State.ps1.
12. Perform only the one objective above, using only the required capabilities and allowed write roots.
13. After every write, reread the changed region. After every command, inspect the exit code and relevant output.
14. Run the narrowest focused build, test, or source-only validator required by acceptance.
15. Check every acceptance box below using recorded evidence.
16. Run Assert-QwenWritePaths.ps1 for every changed path.
17. Set BOOT-TOOLS-001 to passed, blocked, or failed. Never self-pass a Validator microtask without independent evidence.
18. Run Write-Qwen-Handoff.ps1. It creates the checksummed handoff, session result, and exact MEMORY_CURSOR.json.
19. If persistent_memory is required, replace exactly the key forge_windows_port::1df367e1-3584-41e9-b2cb-8fbbe721addf::98601f23820832b5::cursor with the exact value from .forge-qwen/state/MEMORY_CURSOR.json. Do not write a global/latest key.
20. Emit one final line beginning FORGE_QWEN_RESULT and stop this chat.

## Exact control-command templates

Start the task:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Set-Microtask-State.ps1 -Repository 'D:\GitHub\Forge-Conductor-Windows-Edition' -Microtask 'BOOT-TOOLS-001' -Status in_progress -Role 'builder' -Message 'Started exact active assignment.'

Validate changed paths before closing:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Assert-QwenWritePaths.ps1 -Repository 'D:\GitHub\Forge-Conductor-Windows-Edition' -Microtask 'BOOT-TOOLS-001' -Paths <each changed relative path>

Close the task after setting its final state:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge-qwen/instructions/scripts/Write-Qwen-Handoff.ps1 -Repository 'D:\GitHub\Forge-Conductor-Windows-Edition' -Microtask 'BOOT-TOOLS-001' -Status <passed|in_progress|blocked|failed> -Objective '<exact objective>' -VerifiedFacts <facts> -FilesChanged <paths> -Commands <commands> -Evidence <evidence paths> -Decisions <decision IDs> -Blockers <blocker IDs> -NextAction '<one exact action>' -Role 'builder'

Do not invent another control script or edit state JSON by hand when these scripts can perform the operation.

## Routed documents

- .forge-qwen/instructions/AGENTS.md
- .forge-qwen/instructions/qwen/TOOL_DISCOVERY.md
- .forge-qwen/instructions/qwen/TOOL_CAPABILITY_MAP.json
- .forge-qwen/instructions/qwen/TOOL_ROUTING.md

## RAG queries

- Forge Qwen tool discovery capability bindings shell filesystem github rag-v1 js sandbox long-term memory persistent memory

## Required capabilities

- shell
- filesystem
- github
- rag_v1
- js_sandbox
- long_term_memory
- persistent_memory

## Allowed write roots

- .forge-qwen/state/**

Everything under .forge-inputs/** and .forge-qwen/instructions/** is read-only.

## Budgets

- Files changed: 4
- Lines changed: 250
- Tool calls: 18
- Attempts: 3

## Acceptance

- [ ] all visible tools enumerated
- [ ] seven capabilities bound or exact missing blockers recorded
- [ ] harmless probes evidenced
- [ ] tool-bindings schema valid

## Stop immediately when

- the workspace lock no longer matches;
- a write would leave allowed roots;
- memory or continuity references another run, fingerprint, mission, or target;
- the task would require choosing another repository, branch, phase, or microtask;
- the next action is not explicitly in this card;
- the tool-call budget requires a handoff.