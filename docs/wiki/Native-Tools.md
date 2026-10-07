<p align="center">
  <img src="images/product/04-tools.png" alt="Native tools view" width="100%">
</p>

# Native Tools

Forge Conductor's project tools are native Windows implementations behind application contracts and workspace authority.

Version 1.3.15 retains the original tools and expands Primary/Fallback to 104 descriptors. Dedicated WinHTTP search/fetch/HTTP, native DOCX/XLSX/PPTX writers, desktop/browser observation/input/capture, PNG drawing/image previews, independent model workers and persistent schedules have explicit native contracts. See [Windows Workflow Capabilities](Windows-workflow-capabilities) for current bounds and [Release 1.3.15](Release-1.3.15) for executed verification.

Host filesystem mode covers ordinary available local volumes using existing native ACL/path/reparse checks; workspace mode retains registered/configured roots. Both keep the selected project directory as the default for relative artifact paths. Models cannot change the owner's filesystem mode. Generative image models and cloud accounts require separately configured providers/accounts or authorized APIs.

## Workspace authority

Before filesystem, search, Git, shell, or document work:

- open and canonicalize the project root
- resolve final paths through handles
- reject reparse-point escape
- reject unsafe alternate data streams and device paths
- reject unapproved UNC roots
- bind the grant to an authority generation

Requested forward and mixed Windows separators are converted to canonical separators before strict resolution. Configured roots are still strict; dot/parent components, device paths, unauthorized UNC forms, and outside-workspace targets remain rejected. Continuity observations are recorded only after authorization.

## Filesystem

The `fs_*` family supports bounded UTF-8 read/write/edit/list/glob/move/delete/mkdir operations. Reads support line-window pagination and oversized-line byte continuation, with final serialized payloads bounded to 32 KiB and accurate UTF-8 offsets/EOF metadata. Writes use the atomic file store where required.

## Search and glob

`search_text` performs bounded recursive text search. `fs_glob` performs case-insensitive Windows pattern matching under the authorized root. File sizes, scan budget, match count, and response size are capped.

## Git

The Git pack discovers an exact `git.exe` path and launches it directly through the Windows process supervisor—not through an ambient shell. Prompts are disabled, output is bounded, and nonzero exit codes are returned as structured process outcomes.

There is no remote `git_push` tool.

## PowerShell

`shell_exec` requires owner-enabled shell policy, project authority, shell permission, execute intent, a bounded command, and a 1–120 second timeout. Forge Conductor prefers PowerShell 7 (`pwsh.exe`) by PATH or the standard Program Files location and falls back to Windows PowerShell only when PowerShell 7 is unavailable. The child process tree is contained in a kill-on-close Job Object.

## PDF

`pdf_write` and `pdf_from_file` use the native Forge PDF engine. They do not require pandoc or a separate runtime.

## Complete results

Large canonical results use ordered bounded JSON text fragments without executing the operation again. `structuredContent` remains complete. Consumers must reconstruct the full sequence before parsing its semantic outcome; see [MCP Protocol](MCP-Protocol#bounded-native-result-delivery).

## Process supervision

Git, PowerShell, and other child work share a native supervisor with explicit executable paths, an explicit bounded Windows tool environment containing system tools, Git, PowerShell 7, `PATHEXT`, and `COMSPEC`, handle inheritance allowlists, redirected bounded pipes, deadlines, cancellation, and deterministic process-tree termination.

**Next:** [Tool Catalog](Tool-Catalog) · [Security](Security) · [How-To Recipes](How-To#invoke-a-native-project-tool)

## Retained evidence and review workflows

In workspace mode, owner-configured `allowed_roots` are candidates, not automatic grants. Call `workspace_authority_bind` for an existing exact entry. Explicit activation is available after continuity recovery and retained on subsequent calls. Rebind after connector/Manager restart. Recovery intersects bindings with the current issuer capability and never restores removed grants. Host mode uses the owner's current ordinary local-volume scope. All shell/process launch cwd checks use the connection's active roots before Manager dispatch. Shell commands and executables run with ordinary account permissions; cwd validation is not an OS sandbox for absolute arguments or later directory changes.

Nested directory creation no longer asks for unrelated child-delete/file-creation rights. Strict sharing, pinned directory handles, and reparse checks remain. Native file operations report `filesystem_access_denied` with path, operation, and Win32 code for OS access denials; policy denial remains separate.

`shell_exec` and `shell_job_start` accept 65,536 UTF-8 command bytes. Scripts use a supervised UTF-8 stdin loader with fixed launch arguments. Explicit exit, native exit failures, nonterminating errors, timeout, cancellation, and process-tree ownership remain observable.

`reviewer_start` requires an `authorization` reference and exactly one `opening_message_path` or `opening_message`. Both sources accept up to 65,536 UTF-8 bytes without NUL. File sources are authorized locally before Manager dispatch; inline openings avoid staging a message in a repository. `receive_timeout_sec` ranges from 1 to 3,600, default 600, and controls a monotonic budget for each provider Responses request from submission through response-body completion, clipped to its transport operation deadline. The asynchronous run is independent of the admitting MCP call’s deadline: start returns an owned run and status polls observe later inference. Default `mode: "tools"` retains read-only tools; `text_only` restricts review to supplied text and identifies missing external evidence as unverified.

Reviewer status exposes timeout, mode, `infrastructure_blocked`, `failure_category`, usage, output pages, and error details. Transport failure is not a review verdict. Completing a run never makes `gate_approved` true. The verified receipt retains the chosen timeout; old receipts without it return null.

## Scheduled model work and notifications

`schedule_create/list/cancel/run_now` use a Manager-owned persistent file, current-policy intersection with frozen roots/grants/tools, bounded provider lifetimes, distinct run/cancellation identities, actual results/history, and no overlap for the same owned task. Manager must stay running. Future triggers restore after restart; uncertain interrupted effects block automatic replay and require a fresh explicit authorization. Reference strings record task scope, while authenticated owner/project routing provides actual authority.

Meaningful changes use the installed Forge Conductor application's local Windows toast channel. `last_notification` retains the bounded actual submission receipt/error; accepted submission leaves `display_confirmed: false`. Existing Windows notification settings remain unchanged, model output/remote content are omitted, and a notification failure does not change or replay the model task.
