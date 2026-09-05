# FACTS — BOOT-TOOLS-001 tool-binding verification

Run namespace: `forge_windows_port::1df367e1-3584-41e9-b2cb-8fbbe721addf::98601f23820832b5`
Workspace fingerprint: `98601f23820832b5` (target repo exact match, lock verified)
Machine-readable bindings: `.forge-qwen/state/tool-bindings.json`

## VERIFIED

- **shell** — bound to `shell_exec{command, cwd?, timeout_sec?}`. Evidence: `git --version` → git 2.53.0.windows.1 exit 0; Assert-QwenWorkspace.ps1 exit 0 (lock + fingerprint); Set-Microtask-State.ps1 -Status in_progress exit 0.
- **filesystem** — bound to `fs_read / fs_write / fs_list / fs_edit / fs_mkdir / fs_move / fs_delete / fs_glob`. Evidence: root listing ok:true; all 4 routed docs read ok:true.
- **github (local)** — bound to `git_status / git_log / git_diff / git_add / git_commit` (cwd-scoped, commit does not push). Evidence: git 2.53.0.windows.1 exit 0; local repo state readable. No remote configured (`git remote -v` empty) — local-only scope verified.
- **rag_v1 (corpus retrieval)** — bound to `search_text{path, query}` + `memory_search{query}`. Evidence: exact-phrase query hit at `qwen/TOOL_DISCOVERY.md:17` (count=1); memory_search shape accepted (0 results for routed query).
- **js_sandbox** — bound via verified shell capability to node v24.13.1 (`node -e <pure JSON expression>`; Node never added to product). Evidence: `node --version` exit 0; JSON round-trip probe completed on resume → stdout `ROUNDTRIP_OK`, exit 0 (was pending at handoff due to harness budget wall, not a tool failure).
- **long_term_memory** — bound to `memory_set / memory_get / memory_list / memory_search / memory_delete`. Evidence: namespaced probe key set ok:true then get returned exact body; delete confirmed on resume.
- **persistent_memory** — same MCP `memory_*` surface with shared-store discipline (only the exact run-scoped cursor key from `.forge-qwen/state/MEMORY_CURSOR.json`). Evidence: namespaced set→get cycle under the exact run namespace read back exactly.

## INFERRED

- None remaining. (js_sandbox round-trip was inferred at handoff; probe passed on resume and it is now VERIFIED.)

## UNKNOWN

- None material.

## BLOCKED

- **github remote / PR / issues ops** — exact blocker: no git remote origin configured AND no GitHub API tool on the MCP surface. Local git_* bindings verified; remote collaboration unavailable until a remote exists or an API tool is added.
- **semantic RAG** — literal text retrieval only (`search_text`); no semantic/embedding RAG tool on the surface.

## Notes

- Harness context-budget wall intermittently rejected calls mid-task (memory_* passed while fs_write/shell_exec were blocked). State was safely persisted to disk at handoff; all pending steps completed on resume.
- Script quirk: multi-word `-Message` values break PowerShell argument binding in the Qwen scripts — use single-token or omit.
