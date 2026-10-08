<p align="center">
  <img src="images/product/04-tools.png" alt="Native tools view" width="100%">
</p>

# Native Tools

Forge Conductor's project tools are native Windows implementations behind application contracts and workspace authority.

The current 1.3.26 source retains every original tool and provides 112 Primary/Fallback descriptors. Optional ComfyUI generation/edit/job routes join native CMake/CTest, WinHTTP search/fetch/HTTP, DOCX/XLSX/PPTX, desktop/browser input/capture, PNG drawing/image analysis, independent model workers and persistent schedules. Integrated 1.3.26 source, package, installed Manager/LM Studio and native-model qualification remain pending. Historical [published 1.3.21 qualification](Release-1.3.21) identifies its own source and binaries. See [Windows Workflow Capabilities](Windows-workflow-capabilities) and [1.3.26 source notes](Release-1.3.26).

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

## CMake and CTest

`cmake_test_run` requires an explicit initialized build tree and enabled shell policy with current Read/Write/Execute authority. Default `mode: "test"` runs CTest; `build_and_test` first runs an optional target and starts testing only after confirmed uninterrupted build success. Configuration and test-name filter are separate. One shared timeout covers build, test and report parsing. `cmake_test_status` exposes actual phase results, nullable validated JUnit counts and UTF-8 failure pages; outer `ok` means retrieval. Use the job ID with existing log/wait/kill tools. Report seals detect changes against the receipt without authenticating producer text; unknown crash/cancellation outcomes are not promoted to counts. The tools do not configure a project implicitly or add clangd. See [CMake/CTest source guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/CMAKE-CTEST.md).

## Desktop and image detail

`desktop_read` accepts zero-based `offset` (default 0) and `limit` 1–300 (default 100 scanned positions). Follow `next_offset` while `has_more`; row `index` values belong to each freshly observed tree. Pages retain 32 KiB aggregate text and 64 KiB encoded native JSON bounds, exclude password text and do not activate the target window. `preview_max_dimension` 128–2,048 (default 256) applies to `image_read`, `image_write`, `desktop_capture` and `image_analyze`. Adaptive downscaling respects the 512 KiB base64 bound and reports actual dimensions and reduction metadata. Invalid values are rejected before image-write/capture effects. Filled rectangles use exact requested pixel width/height, including one-pixel shapes and canvas clipping; ellipse behavior remains. Larger previews do not fix the stock LM Studio metadata boundary or establish exact OCR.

`image_read` accepts optional `samples` with 1–64 integer source-coordinate pairs. Its `pixel_samples` return measured RGBA8 channels in request order; that field is absent by default. `decoded_rgba8_sha256` identifies the tightly packed, top-to-bottom WIC frame-0 RGBA8 buffer, while `preview_png_sha256` identifies the emitted preview PNG bytes. Decoded dimensions, format and stride are explicit. Source alpha is measured separately from the existing opaque preview. These native measurements do not establish the stock chat model's inference input. See the [source capability guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/HOST-CAPABILITIES.md) for the complete receipt contract.

## Optional local generation and editing

The optional ComfyUI `sd1` provider is disabled by default and requires an owner-configured loopback endpoint plus an existing compatible checkpoint. `image_provider_status` reads current configuration and core-node/checkpoint inventory; inventory availability does not establish a loaded model or image quality. `image_generate` and `image_edit` return durable jobs. Poll `image_job_status` for actual state, error and publication measurements. `image_job_cancel` suppresses local publication; running remote work may continue. `image_job_resume` requires fresh current authorization and retrieves the existing exact prompt/workflow without a new generation or upload POST. Read-only status cannot resume or publish. Required generation/edit arguments include an absolute `.png` destination, prompt, explicit seed and dimensions. Sources/masks are authorized snapshots; edit dimensions must match. Bounds include 64–1024 pixels in multiples of eight, 1–100 steps, denoise 0.05–1 and 16 MiB input/output images. Invalid arguments fail before upload/generation effects. Native red-channel compositing preserves all original RGBA bytes under red 0, including RGB beneath alpha.

Receipts retain exact prompt/workflow IDs and artifact byte/pixel hashes. Saved scope and unkeyed seals do not authorize new effects. Cancellation does not confirm remote termination, and Forge does not call global interrupt/queue-clearing or process-kill endpoints. An isolated direct-native ComfyUI smoke run passed six cases, including generation, unmasked editing, exact masked RGBA preservation, observed running cancellation and explicit recovery after a controlled lost acknowledgement. The helper deliberately withheld an observed matching HTTP 200; it did not exercise a real network outage. These checks used private fixture-issued scopes and do not qualify integrated Manager IPC, LM Studio/Qwen delivery, semantic instruction following or model quality. See [local image-provider guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md).

## PDF

`pdf_write` and `pdf_from_file` use the native Forge PDF engine. They do not require pandoc or a separate runtime.

## Complete results

Large canonical results use ordered bounded JSON text fragments without executing the operation again. `structuredContent` remains complete. Consumers must reconstruct the full sequence before parsing its semantic outcome; see [MCP Protocol](MCP-Protocol#bounded-native-result-delivery).

## Process supervision

Git, PowerShell, and other child work share a native supervisor with explicit executable paths, an explicit bounded Windows tool environment containing system tools, Git, PowerShell 7, `PATHEXT`, and `COMSPEC`, handle inheritance allowlists, redirected bounded pipes, deadlines, cancellation, and deterministic process-tree termination.

Native shell and executable jobs explicitly supply bounded `SystemDrive`, `ProgramFiles`, `ProgramFiles(x86)` and `ProgramData` defaults from the Windows host. Case-insensitive explicit caller overrides remain authoritative; absent or oversized defaults are omitted. `SystemDrive` supports Windows/.NET known-folder resolution used by MSBuild; arbitrary host environment variables remain excluded.

**Next:** [Tool Catalog](Tool-Catalog) · [Security](Security) · [How-To Recipes](How-To)

## Retained evidence and review workflows

In workspace mode, owner-configured `allowed_roots` are candidates, not automatic grants. Call `workspace_authority_bind` for an existing exact entry. Explicit activation is available after continuity recovery and retained on subsequent calls. Rebind after connector/Manager restart. Recovery intersects bindings with the current issuer capability and never restores removed grants. Host mode uses the owner's current ordinary local-volume scope. All shell/process launch cwd checks use the connection's active roots before Manager dispatch. Shell commands and executables run with ordinary account permissions; cwd validation is not an OS sandbox for absolute arguments or later directory changes.

Nested directory creation no longer asks for unrelated child-delete/file-creation rights. Strict sharing, pinned directory handles, and reparse checks remain. Native file operations report `filesystem_access_denied` with path, operation, and Win32 code for OS access denials; policy denial remains separate.

`shell_exec` and `shell_job_start` accept 65,536 UTF-8 command bytes. Scripts use a supervised UTF-8 stdin loader with fixed launch arguments. Explicit exit, native exit failures, nonterminating errors, timeout, cancellation, and process-tree ownership remain observable.

`reviewer_start` requires an `authorization` reference and exactly one `opening_message_path` or `opening_message`. Both sources accept up to 65,536 UTF-8 bytes without NUL. File sources are authorized locally before Manager dispatch; inline openings avoid staging a message in a repository. `receive_timeout_sec` ranges from 1 to 3,600, default 600, and controls a monotonic budget for each provider Responses request from submission through response-body completion, clipped to its transport operation deadline. The asynchronous run is independent of the admitting MCP call’s deadline: start returns an owned run and status polls observe later inference. Default `mode: "tools"` retains read-only tools; `text_only` restricts review to supplied text and identifies missing external evidence as unverified.

Reviewer status exposes timeout, mode, `infrastructure_blocked`, `failure_category`, usage, output pages, and error details. Transport failure is not a review verdict. Completing a run never makes `gate_approved` true. The verified receipt retains the chosen timeout; old receipts without it return null.

## Scheduled model work and notifications

`schedule_create/list/cancel/run_now` use a Manager-owned persistent file, current-policy intersection with frozen roots/grants/tools, bounded provider lifetimes, distinct run/cancellation identities, actual results/history, and no overlap for the same owned task. Manager must stay running. Future triggers restore after restart; uncertain interrupted effects block automatic replay and require a fresh explicit authorization. Reference strings record task scope, while authenticated owner/project routing provides actual authority.

Meaningful changes use the installed Forge Conductor application's local Windows toast channel. `last_notification` retains the bounded actual submission receipt/error; accepted submission leaves `display_confirmed: false`. Existing Windows notification settings remain unchanged, model output/remote content are omitted, and a notification failure does not change or replay the model task.
