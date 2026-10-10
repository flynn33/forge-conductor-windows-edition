# Forge Conductor capabilities for models in LM Studio

This guide inventories **all 125 distinct model-facing tools in Forge Conductor for Windows 1.3.29** (package **1.3.29.0**), plus its ten specialist playbooks and supporting orchestration services. It explains what the model calls, what the operator configures, and how to read the result.

The matching [LM-STUDIO-MODEL-CAPABILITIES.json](LM-STUDIO-MODEL-CAPABILITIES.json) contains every exact advertised `name`, `description` and complete `inputSchema`, alongside role, source and access metadata. This inventory identifies packaged 1.3.29 source `ed101d77cd5adbbf809e7f36c7174d060613e6c4`; the earlier reference was based on source `eb071505f00d0b890ca12240cbfdbcc7594516e4`, inspected 2026-10-09. These are reference documents; loading them does not register tools or enable an integration.

Version 1.3.29 adds 11 general ComfyUI tools plus `desktop_scroll` and `desktop_drag`. See [ComfyUI automation](COMFYUI-AUTOMATION.md) for setup, workflows, preview approval and recovery. [Versioned release verification](validation/RELEASE-1.3.29.md) and [host qualification](COMFYUI_HOST_QUALIFICATION.md) retain separate source/package and productive-inference evidence.

For `comfy_run` and `comfy_validate`, expected output arrays select graph output node IDs. For example, `"expected_outputs": ["25"]` selects output node `25`; set the output file name in that node's inputs. `preview_expected_outputs` and `final_expected_outputs` override the common list for their respective graphs. Omit the lists or use an empty effective list to infer all graph output nodes from their discovered node contracts.

Before selecting creation settings, read `comfy_status` for effective limits and `configuration.quality_preference`; honor the configured quality when the operator omits it. When video duration is omitted, propose a motion draft of about five seconds. Choose playable MP4/H.264 for final video from discovered output-node contracts unless the operator requests a supported alternative format or codec. Save the selected format in the proposed final graph before preview approval; a later format change requires another preview and approval. Preserve supported requested alternatives and avoid universal output-format rewrites of unknown graphs. This guidance is carried in the `comfy_run` tool description as well as bootstrap instructions.

For an image preview, call the existing `image_read` tool with the verified preview image artifact's `path` to display its larger bounded image before requesting approval. The job-status thumbnail alone is insufficient. Both the `comfy_run` description and bootstrap instructions carry this delivery guidance; existing image preview limits and tool arguments are unchanged.

Successful decoding verifies readable media; it does not establish the requested subject or motion quality. Before describing video content or quality, call the existing `image_analyze` tool with the `path` of the published artifact whose `role` is `sampled_video_contact_sheet`, then poll `reviewer_status` for actual findings. Report missing contact sheets and review failures, including returned errors, and limit visual claims to the reviewed sampled frames. Do not infer observed content from the generation prompt or claim motion quality that those frames do not establish. Both the `comfy_run` description and bootstrap instructions carry this review guidance.

For a verified video preview or final artifact with `provider_view_url`, call the existing `browser_open` tool with that exact URL while ComfyUI is running, then use `desktop_read` on the observed browser window to check its actual address. Launch acceptance alone does not verify page loading or playback; report actual browser-launch or observation failures. Present the URL as a copyable reference and the complete `artifacts[].path` in a copyable fenced block. Retain the sampled-frame preview and never abbreviate file paths. The URL uses the provider's existing local `/view` route; it is supplied only for retrieved provider files, not derived local contact sheets. The installed LM Studio 0.4.26+4 ordinary Markdown link path rejects loopback URLs, so a Markdown link does not establish playable delivery. Its classic-chat renderer also strips Windows file protocols and uses an image preview component. Independent sampled-frame review and native user approval remain required. Browser loading and playback need separately observed evidence; see [ComfyUI automation](COMFYUI-AUTOMATION.md) and [host qualification](COMFYUI_HOST_QUALIFICATION.md).

## Connections and model access

| LM Studio integration | Role | Tools | How the model accesses it |
| --- | --- | ---: | --- |
| `forge-conductor` | Primary | 125 | Enable it in the LM Studio chat; call the advertised tools through LM Studio. |
| `forge-conductor-fallback` | Fallback | 125 | Select this independent integration in LM Studio; use the same full catalog. |
| `forge-conductor-clu` | CLU | 5 | Use `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve`, `project_policy.read`. |

The five CLU tools are a subset of the 125, and are also exposed by Primary and Fallback. Fallback does not automatically replay a timed-out Primary call or switch back to Primary. Reconcile uncertain side effects before retrying.

### Owner setup

1. The owner installs/launches Forge Conductor, registers/selects the real project folder, and selects a usable LM Studio provider profile/model.
2. The owner configures filesystem_access (host or workspace), existing additional-root grants and opt-in shell policy; the model reads effective authority in status.
3. The owner adds instruction-package folders in order and selects the project development-policy folder/file. Bind policy after selecting the intended project.
4. Use Forge's LM Studio install/repair action to register forge-conductor, forge-conductor-fallback and forge-conductor-clu for the selected project. Each registration uses the exact integer timeout 180000 milliseconds.
5. In the LM Studio chat, enable the intended Forge integration and load a model that can emit tool calls. LM Studio supplies the registered tool definitions and dispatches the model's calls.
6. Enable Auto Continuity for the selected project/provider when desired; leave a matching Manager running for workers, reviewers, schedules and native automatic handoff.

### Model bootstrap

Read the `initialize` instructions supplied by Forge, then perform these calls on Primary or Fallback. CLU permits only its five governance names.

| Order | Tool and arguments | Purpose |
| --- | --- | --- |
| 1 | `context_get` `{}` | Recover the authorized handoff before starting new work (Primary/Fallback). If initialization identifies a packet, pass its actual handoff_id instead of relying on the latest default. |
| 2 | `get_forge_status` `{}` | Read real workspace.project_root/project_id, active authority, ordered instruction package records and development policy. |
| 3 | `host_capabilities` `{}` | Inspect actually configured services, roots and external prerequisites. |
| 4 | `provider_status` `{}` | Inspect loaded provider/model and configured context reserves. |
| 5 | `project_policy.read` `{}` | Read the pinned policy index, then follow its cursor and document offsets. |
| 6 | `clu.findings` `{}` | Read governance findings and deferred notifications. |

Between the status read and policy reads, consume `instruction_packages.packages` in its declared order using `instruction_package.read` and the returned real `queue_row_id`. Follow every package/policy inventory cursor and document offset. `workspace.project_root` is the project; Forge `home` is application data. If initialization identifies a handoff, use `context_get` with that actual `handoff_id`. Use returned IDs/paths rather than invented values.

Status fields distinguish configuration from execution: `durable_manager` reports Manager availability; `workspace_authority` reports configured/active roots and filesystem mode; `auto_continuity.resume_packet_id`, `resume_packet_ready`, `readback_confirmed` and `readback_scope` describe this client’s packet pickup. `visible_chat_continuity` describes native rollover; `context_telemetry` distinguishes measured provider usage from cached rendered-prompt projection.

### Invocation and transport

LM Studio performs the MCP handshake and supplies tool definitions to the model. The model emits a tool call with the exact advertised name and a JSON argument object; LM Studio dispatches it. A client-added tool namespace comes from LM Studio. The following wire examples describe that bridge, rather than commands for the model to execute in a shell.

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"example-client","version":"1.0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"host_capabilities","arguments":{}}}
```

Stdio carries one compact UTF-8 JSON-RPC 2.0 object per newline, without `Content-Length` headers. Supported protocol versions are `2025-11-25`, `2025-06-18`, `2025-03-26` and `2024-11-05`. Supported methods include `initialize`, `ping`, `tools/list`, `tools/call`, `notifications/cancelled`, `resources/list` and `prompts/list`; resource/prompt lists are empty. The codec rejects duplicate keys, NUL, invalid UTF-8, non-object documents, depth above 64 and documents above 1 MiB.

### Authority and arguments

- tools/list advertises descriptors; listing a tool is not a permission grant or proof of a configured backing service.
- For project-required tools, use the selected bound project and actual trusted roots. Native boundaries recheck current canonical path authority, effects, grants/denials and policy.
- The Read/Write effect and requiresProject/requiresShell fields in this guide are source-derived catalog metadata. They are not emitted MCP annotations; tools/list emits only name, description and inputSchema.
- Shell-required tools additionally require owner-enabled shell policy and Execute authority. Catalog Read/Write is not the full per-path Read/Write/Create/Execute contract.
- An authorization argument records the actual owner instruction/reference. Text supplied by a model does not create approval, credentials, administrator rights or new grants.
- A no-project catalog flag does not mean unrestricted execution. Role, input, service, storage, provider and ordinary Windows permissions still apply.
- Advertised schemas have both open and closed objects. Closed contracts reject unknown arguments; open/unspecified legacy schemas are not proof every extra argument is implemented. Native handlers enforce semantic limits and policy beyond schema validation.

Independent workers and scheduled workers have an additional hard tool policy. The JSON companion lists the exact blocked and eligible names, and every tool entry includes eligibility flags. Read-only workers retain only eligible Read-effect tools; reviewers in `tools` mode retain Read-effect tools and `text_only` reviewers have no tools. These are upper bounds intersected with actual admitted/current authority, rather than new grants.

**Hard-blocked worker tool names:** `agent_cancel`, `agent_run_complete`, `agent_run_start`, `agent_run_status`, `agent_spawn`, `clu.evaluate`, `clu.resolve`, `continuity.acknowledge_handoff`, `continuity.checkpoint`, `continuity.prepare_handoff`, `continuity.request_rollover`, `continuity.resume`, `image_analyze`, `reviewer_cancel`, `reviewer_start`, `schedule_cancel`, `schedule_create`, `schedule_list`, `schedule_run_now`, `session_checkpoint`, `session_handoff`, `workspace_authority_bind`.


### Results, jobs and paging

- Distinguish a top-level JSON-RPC error from a tool result. Inspect isError and the logical payload ok/code/message/retryable; prefer structuredContent when exposed. An accepted launch/start/cancel differs from task completion or confirmed termination.
- For async tools, retain the returned actual job_id/run_id/session_id/operation_id and use that family's status tool. Missing exits/counts/provider facts remain null or unknown.
- Follow each family's continuation: fs_read 1-based lines plus oversized-line byte continuation; instruction/policy/log/model output byte offsets; desktop zero-based control offsets; inventory/search cursors; CTest failure offsets.
- Canonical fs_read, instruction_package.read and project_policy.read pages are bounded to 32 KiB; follow continuation until complete. Other families have their own page budgets. These three bounded reads defer governance notifications to clu.findings.
- Other JSON tool payloads above 32 KiB use content text blocks with kind=forge_tool_result_fragment, version=1, zero-based index, count, total_bytes, part and instruction. Concatenate all part strings in order and parse once. Never repeat a mutating call to obtain later fragments.
- Each fragment part is at most 12 KiB UTF-8, each serialized fragment at most 32 KiB. structuredContent retains the complete logical payload and isError the original outcome; complete JSON-RPC responses remain at most 1 MiB. Preview base64 is emitted as a native image content block with image_content_block=true rather than duplicated in the logical object.
- MCP request cancellation and durable-job cancellation are distinct. To cancel persistent work, use its owning job/run cancel tool and observe the final state.
- An MCP PNG image block/LM Studio preview can be displayed without establishing pixel input to the main chat model. Use image_analyze and reviewer_status for independently returned interpretation.
- Fallback is a separately selected integration. A Primary timeout is not an automatic replay through Fallback, and Fallback selection does not cause an automatic switch back. Reconcile uncertain mutating outcomes before retrying.

## Capability overview

The catalog groups into 27 native tool packs. The access sequences below cover each pack; the complete argument catalog follows later.

| Capability | Tools |
| --- | --- |
| [Host, provider and process inspection](#hostinspectiontoolpack) | `host_capabilities`, `process_status`, `provider_status` |
| [Runtime status and ten specialist playbooks](#agenttoolpack) | `agent_context`, `agent_get`, `agent_list`, `agent_recommend`, `agent_run_complete`, `agent_run_start`, `agent_run_status`, `forge_status`, `get_forge_status` |
| [Ordered project instructions](#instructionpackagetoolpack) | `instruction_package.read` |
| [Read the adopted development policy](#projectpolicytoolpack) | `project_policy.read` |
| [CLU findings and correction evidence](#clugovernancetoolpack) | `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve` |
| [Local files and additional authorized roots](#filesystemtoolpack) | `fs_delete`, `fs_edit`, `fs_glob`, `fs_list`, `fs_mkdir`, `fs_move`, `fs_read`, `fs_write`, `workspace_authority_bind` |
| [Recursive local text search](#searchtoolpack) | `search_text` |
| [Local Git status, history, staging and commits](#gittoolpack) | `git_add`, `git_commit`, `git_diff`, `git_log`, `git_status` |
| [GitHub repository evidence](#githubreadtoolpack) | `github_read` |
| [Public web and explicit remote HTTP APIs](#webaccesstoolpack) | `http_request`, `web_fetch`, `web_search` |
| [PowerShell commands and durable shell jobs](#shelltoolpack) | `shell_exec`, `shell_job_cancel`, `shell_job_list`, `shell_job_start`, `shell_job_status` |
| [Exact executable launches, logs and owned process control](#processtoolpack) | `process_adopt`, `process_kill`, `process_launch`, `process_list`, `process_poll`, `process_read_log`, `process_wait` |
| [Native CMake build and CTest jobs](#cmaketesttoolpack) | `cmake_test_run`, `cmake_test_status` |
| [Pinned external Python verification environments](#verificationtoolpack) | `verification_env_create`, `verification_env_status` |
| [Native PDF creation](#docstoolpack) | `pdf_from_file`, `pdf_write` |
| [Native DOCX, XLSX and PPTX files](#officedocumenttoolpack) | `document_write`, `presentation_write`, `spreadsheet_write` |
| [Windows UI observation, input, capture and browser launch](#desktoptoolpack) | `browser_open`, `desktop_capture`, `desktop_click`, `desktop_key`, `desktop_list`, `desktop_read`, `desktop_type` |
| [Local image decoding, drawing and independent visual analysis](#imagetoolpack) | `image_analyze`, `image_read`, `image_write` |
| [Optional ComfyUI generation, variation and masked edits](#imageprovidertoolpack) | `image_edit`, `image_generate`, `image_job_cancel`, `image_job_resume`, `image_job_status`, `image_provider_status` |
| [Independent Manager-owned model workers](#agentworkertoolpack) | `agent_cancel`, `agent_poll`, `agent_spawn` |
| [Fresh independent read-only review](#reviewertoolpack) | `reviewer_cancel`, `reviewer_start`, `reviewer_status` |
| [Persistent scheduled model tasks](#scheduledtasktoolpack) | `schedule_cancel`, `schedule_create`, `schedule_list`, `schedule_run_now` |
| [Durable local key/value notes](#memorytoolpack) | `memory_delete`, `memory_get`, `memory_list`, `memory_search`, `memory_set` |
| [Project-scoped records, search and typed links](#projectmemorytoolpack) | `project_memory.forget`, `project_memory.get`, `project_memory.initialize`, `project_memory.link`, `project_memory.list_recent`, `project_memory.remember`, `project_memory.remember_batch`, `project_memory.search`, `project_memory.status`, `project_memory.update` |
| [Legacy checkpoints and chat handoff packets](#continuitytoolpack) | `context_get`, `context_list`, `session_checkpoint`, `session_handoff` |
| [Durable project handoff lifecycle](#continuitylifecycletoolpack) | `continuity.acknowledge_handoff`, `continuity.checkpoint`, `continuity.get_pending_handoff`, `continuity.prepare_handoff`, `continuity.request_rollover`, `continuity.resume`, `continuity.status` |
| [SHA-256 capture chains and evidence integrity](#evidencetoolpack) | `evidence_digest`, `evidence_log_read` |

<a id="hostinspectiontoolpack"></a>

### Host, provider and process inspection

**Model access:**

1. Call host_capabilities to discover actual dedicated services, filesystem authority, shell policy and external connection requirements.
2. Call provider_status for actually loaded LM Studio models, context configuration and reserves; call process_status for a bounded Windows process/service snapshot.

**Contracts and prerequisites:**

- Catalog registration and service availability are separate. Missing model-file, version or revision facts remain null with provenance and reasons.
- Process inspection uses the current Windows account without elevation.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp).

<a id="agenttoolpack"></a>

### Runtime status and ten specialist playbooks

**Model access:**

1. Use get_forge_status or forge_status for project root, active authority, ordered instruction packages, policy, tool count and agent count.
2. Use agent_list, agent_recommend(task), and agent_get(agent_id) or its alias agent_context to select and read a playbook.
3. Start agent_run_start with agent_id and goal; follow the returned instructions, first_moves and output_schema in the current model conversation.
4. Retain the returned session_id; inspect agent_run_status, then submit the actual report through agent_run_complete.

**Contracts and prerequisites:**

- A specialist session is durable guidance for the current model. Independent inference uses agent_spawn instead.
- Starting a specialist session supersedes prior open specialist sessions. Status touches ownership and is classified Write.
- Playbook primary/forbidden tool lists are guidance; enforced worker permissions are separate. A report or recommendation does not grant owner authority or CLU approval.

Sources: [AgentCatalog.cpp](../src/Application/AgentCatalog.cpp), [AgentSessionService.cpp](../src/Application/AgentSessionService.cpp), [USER-GUIDE.md](USER-GUIDE.md).

<a id="instructionpackagetoolpack"></a>

### Ordered project instructions

**Model access:**

1. Read instruction_packages.packages in get_forge_status in its declared order.
2. Call instruction_package.read with the actual queue_row_id; follow returned cursor inventory pages and path/next_offset document pages until complete.

**Contracts and prerequisites:**

- The package selection belongs to the owner-configured project. Reads return pinned text and coverage; a model cannot select a different project by inventing a row ID.
- Inventory cursors and document byte offsets are distinct continuation mechanisms. Preserve the pinned identity and restart an inventory when its cursor is invalidated.

Sources: [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp), [USER-GUIDE.md](USER-GUIDE.md), [MCP-Protocol.md](wiki/MCP-Protocol.md).

<a id="projectpolicytoolpack"></a>

### Read the adopted development policy

**Model access:**

1. Call project_policy.read with {} for the adopted index; follow every next_cursor.
2. Read each exact indexed path, supplying next_offset as offset until the document is complete.

**Contracts and prerequisites:**

- This reads the project-bound pinned policy; it cannot adopt policy or approve a review.
- The inventory cursor binds project, revision and complete inspected index hash, including guidance. Changed content invalidates old cursors even if the revision label is unchanged.

Sources: [MCP-Protocol.md](wiki/MCP-Protocol.md), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp).

<a id="clugovernancetoolpack"></a>

### CLU findings and correction evidence

**Model access:**

1. Call clu.findings to read findings, correction requests, notification receipts and coverage state.
2. Submit development evidence through clu.evaluate against the exact bound policy; follow the policy-defined evidence contract.
3. Use clu.resolve with the actual finding_id and correction_evidence; use clu.export_log for the redacted project governance log.

**Contracts and prerequisites:**

- All four CLU tools also exist in Primary and Fallback. Together with project_policy.read they form the five-tool CLU role.
- Policy adoption is an owner action. An independent reviewer, evidence hash or model statement alone cannot approve a policy gate.
- Bounded read results defer governance notifications to clu.findings instead of acknowledging them. The private policy text is excluded from the exported governance log.

Sources: [McpServer.cpp](../src/Mcp/McpServer.cpp), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [Native-Tools.md](wiki/Native-Tools.md).

<a id="filesystemtoolpack"></a>

### Local files and additional authorized roots

**Model access:**

1. Use fs_list/fs_glob to discover actual paths and fs_read to read UTF-8 text.
2. Follow fs_read next_offset for line windows; use returned next_byte_offset/byte_offset continuation for an oversized line.
3. Use fs_write/fs_edit/fs_mkdir/fs_move/fs_delete within the authorized task and current roots.
4. Use workspace_authority_bind only to bind an additional root already present in the owner-configured allowlist.

**Contracts and prerequisites:**

- Relative paths default to the selected project root in both host and workspace modes. Forge application-data home is a different directory.
- The owner selects host or workspace access. A model cannot change that setting, create new grants or bypass native ACL/path checks.
- Native path authorization checks canonical roots and current grants/denials; UNC/device namespaces, alternate streams and reparse traversal remain subject to Windows checks.
- fs_read offset is a 1-based line index; instruction/policy/log/output byte offsets and desktop offsets follow their own contracts. Do not mix them.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp), [McpToolRouter.cpp](../src/Mcp/McpToolRouter.cpp).

<a id="searchtoolpack"></a>

### Recursive local text search

**Model access:**

1. Use search_text with a pattern and, when needed, an authorized path; read the actual matching files with fs_read.

**Contracts and prerequisites:**

- Search output is evidence of observed matches, not a substitute for reading the relevant source or following result bounds.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp).

<a id="gittoolpack"></a>

### Local Git status, history, staging and commits

**Model access:**

1. Inspect git_status/git_diff/git_log before edits or staging.
2. Use git_add and git_commit only within the owner-authorized task and repository configuration.

**Contracts and prerequisites:**

- The dedicated catalog contains these five Git tools; git_push is not an advertised tool name.
- Additional Git operations can use an authorized shell command when shell policy, installed Git and configured credentials permit it. Playbook mentions of other command names do not register new MCP tools.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md).

<a id="githubreadtoolpack"></a>

### GitHub repository evidence

**Model access:**

1. Call github_read with repository and one of the schema-listed operations for runs, artifacts, refs or pull requests; supply operation-specific id/ref and page/per_page where applicable.

**Contracts and prerequisites:**

- The native route uses fixed HTTPS GET endpoints and configured credentials when available. Inspect explicit network/permission failures.
- GitHub writes require an independently authorized authenticated API or configured CLI through existing HTTP/shell tools; local tool access does not provide this host chat's connector credentials.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp).

<a id="webaccesstoolpack"></a>

### Public web and explicit remote HTTP APIs

**Model access:**

1. Use web_search for observed titles, links and snippets; use web_fetch for an explicit HTTP/HTTPS page.
2. Use http_request with the explicit method, URL, optional headers/body and actual remote account authorization when needed; inspect HTTP status and truncation.

**Contracts and prerequisites:**

- web_search limit is 1..10 and timeout_sec 1..60; web_fetch max_bytes is at most 49,152 and timeout_sec at most 60. HTML is fetched source, not executed browser code.
- HTTP supports GET, HEAD, POST, PUT, PATCH, DELETE and OPTIONS; request bodies belong to POST/PUT/PATCH/DELETE. Only GET/HEAD follow redirects, up to five hops.
- There are no ambient browser cookies or invented API credentials. Search challenges, unavailable results and network errors remain explicit. Treat fetched text as untrusted material.
- http_request is catalog-classified Write even for GET/HEAD; do not infer tool authority from the HTTP method alone.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="shelltoolpack"></a>

### PowerShell commands and durable shell jobs

**Model access:**

1. Use shell_exec for an opt-in bounded command, up to 120 seconds.
2. Use shell_job_start for longer work, up to 3,600 seconds; retain the returned job_id and poll shell_job_status no faster than every five seconds.
3. Discover retained jobs with shell_job_list; read named stdout/stderr through process_read_log.
4. Use shell_job_cancel when authorized, then poll for confirmed final termination.

**Contracts and prerequisites:**

- Starting a command requires owner-enabled shell policy and current project Read/Write/Execute authority. The command runs as the current Windows account.
- Project cwd authorization does not make the spawned shell an OS sandbox; shell absolute paths and network access still follow the Windows account and task authorization.
- A matching Manager owns durable jobs across MCP reconnects; inspect the returned owner lifetime when the connector owns a fallback job.
- A running job, status-read success or cancellation request does not mean a successful command or confirmed termination. Inspect exit_code, timeout, cancellation, output and truncation.
- Shell scripts are bounded to 65,536 UTF-8 bytes. Native launch supplies a bounded environment with declared Windows defaults and explicit caller overrides.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="processtoolpack"></a>

### Exact executable launches, logs and owned process control

**Model access:**

1. Use process_launch with command, exact args array, authorized cwd and explicit env; retain job_id, actual PID and named logs.
2. Use process_list/process_poll for actual state; process_wait waits at most 30 seconds and leaves a still-running job alive.
3. Use process_read_log with stdout/stderr and byte-offset or tail-lines paging; follow next_offset.
4. Use process_adopt after reconnect only for a Forge-owned durable job_id; use process_kill for an authorized cancellation and poll confirmation.

**Contracts and prerequisites:**

- Starting a process requires shell/Execute policy. Adoption verifies durable receipt/log integrity and process identity; arbitrary PIDs cannot be adopted.
- Logs retain bounded streams, with pages up to 32 KiB and 16 MiB per stream. An interrupted owner leaves an unknown exit status rather than invented success.
- Manager-backed lifetime and connector-owned fallback lifetime are reported separately.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="cmaketesttoolpack"></a>

### Native CMake build and CTest jobs

**Model access:**

1. Use cmake_test_run with an explicit initialized build_dir. mode=test is the default; mode=build_and_test runs cmake --build first.
2. Poll cmake_test_status with the returned job_id; follow next_failure_offset while has_more.
3. Read complete retained phase logs with process_read_log; use process_wait/process_kill for bounded waiting or authorized cancellation.

**Contracts and prerequisites:**

- CMake/CTest must be available and the tree must already be configured. There is no implicit configure step or clangd service.
- target is allowed only in build_and_test mode. One total deadline covers build/test/report: default 1,800 seconds, maximum 3,600.
- Inspect actual phase exits, timeout/cancellation/termination and validated JUnit counts. Missing phase results/counts remain null; no tests is an error.
- Failure paging defaults to 16, accepts 1..32 and has a 24 KiB encoded budget. Reports are bounded to 8 MiB/100,000 cases, with depth 16 and DTD rejection.
- A report integrity hash is not proof of test-report origin or governance approval.

Sources: [CMAKE-CTEST.md](CMAKE-CTEST.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="verificationtoolpack"></a>

### Pinned external Python verification environments

**Model access:**

1. Use verification_env_create with an authorized external path and an existing python_path; retain the durable process job and inspect its actual completion.
2. Use verification_env_status to read the resulting exact runtime/distribution manifest after successful creation.

**Contracts and prerequisites:**

- This optional workflow requires an installed usable Python executable and shell policy. It does not add a Python runtime requirement to the native Forge product.
- The schema permits only jsonschema==4.25.1 and PyYAML==6.0.3 as optional pinned requirements. It does not grant elevation or edit product source.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md).

<a id="docstoolpack"></a>

### Native PDF creation

**Model access:**

1. Use pdf_write with markdown-ish text and an authorized destination, or pdf_from_file to convert an authorized local Markdown/text file.
2. Inspect the returned artifact path and actual result before reporting success.

**Contracts and prerequisites:**

- The native writer does not require pandoc or an external Python converter. It supports bounded text and simple Markdown structures; full Unicode glyph coverage and HTML/CSS layout are not established.
- Native service limits are title 512 UTF-8 bytes, source text 2 MiB and output 16 MiB; encoded MCP inputs must also fit the 1 MiB transport bound. These tools create PDF; they do not advertise arbitrary PDF editing or layout/rendering inspection.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [PARITY.md](PARITY.md).

<a id="officedocumenttoolpack"></a>

### Native DOCX, XLSX and PPTX files

**Model access:**

1. Use document_write for a title/paragraphs DOCX, spreadsheet_write for typed sheets/rows XLSX, or presentation_write for title/body slides PPTX.
2. Use an authorized matching file extension; inspect canonical path, format, actual bytes_written and content counts in the receipt.

**Contracts and prerequisites:**

- Native OOXML ZIP packages publish atomically; invalid input or failed replacement preserves an existing destination. Office/Python are not required.
- DOCX supports 4,096 paragraphs; text values are bounded to 65,536 UTF-8 bytes. PPTX supports 128 slides and 128 body paragraphs per slide.
- XLSX supports 32 sheets, 10,000 rows per sheet, 256 columns and 100,000 input cell positions. Sheet names are unique case-insensitively, 1..31 UTF-16 units; cell strings at most 32,767 UTF-16 units.
- Cells accept string, number, boolean and null. Strings including a leading = remain literal; use strings for identifiers exceeding 15 integer digits or exact unusual numeric text. Numbers must be zero or finite normal doubles, magnitude at most 1e307; denormals are rejected.
- Native input/output bounds are 2 MiB/16 MiB; the complete encoded MCP request also must fit 1 MiB. There is no formula evaluation, visual rendering, arbitrary existing Office-file editing, macro support or Office application automation in these writers.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="desktoptoolpack"></a>

### Windows UI observation, input, capture and browser launch

**Model access:**

1. Use desktop_list to observe actual visible window_id/PID/title/geometry, then desktop_read for the exact listed target.
2. Follow zero-based next_offset pages; use the current observed tree/coordinates for task-authorized desktop_click, desktop_type or desktop_key.
3. Observe again after input to establish its effect. Use desktop_capture for an authorized PNG of the visible window region.
4. Use browser_open for HTTP/HTTPS launch in the registered browser; observe the actual browser afterward.

**Contracts and prerequisites:**

- These capabilities require an accessible interactive Windows desktop and normal account/integrity permissions. Refresh stale targets and focus observations; covered click points can fail.
- desktop_read defaults offset=0/limit=100; limit accepts 1..300 scanned positions. Each fresh tree can change indices; password text is excluded.
- Accessibility pages retain a 32 KiB aggregate text and 64 KiB encoded pre-framing budget. Capture is visible-screen capture: overlapping windows can appear.
- Input submission or browser launch acceptance does not establish the requested UI task or page load. Preview sizing follows the Image tools contract.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="imagetoolpack"></a>

### Local image decoding, drawing and independent visual analysis

**Model access:**

1. Use image_read for native decoding, original dimensions, preview and optional exact x/y RGBA8 samples.
2. Use image_write for native PNG rectangles, ellipses, lines and Unicode text.
3. Use image_analyze with an authorized path and actual task authorization for fresh independent read-only interpretation; poll reviewer_status with its returned run_id.

**Contracts and prerequisites:**

- Read formats are PNG/JPEG/GIF/BMP/TIFF/ICO, frame zero. Optional samples have 1..64 exact source pixel coordinates; source, decoded frame and emitted preview hashes identify different bytes.
- preview_max_dimension is 128..2,048, default 256; previews do not enlarge the source and adapt to at most 512 KiB of base64-encoded PNG data. Written artifacts keep full dimensions.
- Stock LM Studio preview display alone does not prove its chat model received image pixels. Managed worker/reviewer Responses attach input_text metadata and input_image in the original function_call_output while preserving call_id.
- image_analyze admission/decode success is not completed model analysis and does not freeze later source bytes. Inspect sealed reviewer state/output/usage/errors.
- Drawing is distinct from the optional generative provider. A visual review does not grant policy approval or include executor history.

Sources: [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [MCP-Protocol.md](wiki/MCP-Protocol.md).

<a id="imageprovidertoolpack"></a>

### Optional ComfyUI generation, variation and masked edits

For new image and video requests, read `comfy_status` first. When ComfyUI automation is enabled, use `comfy_run` with explicit preview and final graphs and wait for the operator's native preview approval. This includes ordinary image requests. The following legacy tools remain available for explicitly requested SD1 generation and masked-editing contracts; starting ComfyUI does not select that legacy path.

**Model access:**

1. Ask the owner to configure/start the optional compatible ComfyUI sd1 endpoint/checkpoint; inspect image_provider_status.
2. Use image_generate or image_edit with explicit prompt, seed, absolute authorized destination and dimensions; edits also use an authorized source_path and optional mask_path.
3. Poll image_job_status for actual state/errors/publication/hashes/preview; wait_sec is at most 60.
4. Use image_job_cancel for authorized local publication suppression. Use image_job_resume only for fresh authorized reattachment to an exact existing recovered provider job.

**Contracts and prerequisites:**

- Provider is disabled by default. Forge does not install a model, load it merely by inspecting inventory, or start the provider server.
- Width/height are 64..1,024 and multiples of eight; edit source/output/mask dimensions must match. Mask red=0 preserves exact original RGBA, 255 selects generated pixels, intermediate values blend all channels.
- Public schemas are closed; callers cannot supply arbitrary workflow, provider configuration or a scope envelope. Current grants/settings are revalidated.
- A cancellation can leave remote inference running while suppressing publication. Read-only status cannot resume/publish recovered work; explicit resume never resubmits generation or upload.
- Lost acknowledgements, missing history and uncertain submissions remain unknown; do not replay a generation to make a missing response look complete.

Sources: [IMAGE-PROVIDER.md](IMAGE-PROVIDER.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="agentworkertoolpack"></a>

### Independent Manager-owned model workers

**Model access:**

1. Use agent_spawn with a concrete task and actual owner authorization; optionally select agent_id for a specialist playbook and timeout_sec.
2. Retain run_id and use agent_poll for actual state, usage, sealed receipt and UTF-8 output pages across MCP reconnects.
3. Use agent_cancel with actual authorization, then poll to establish final cancellation.

**Contracts and prerequisites:**

- Requires a running matching Manager and usable configured LM Studio model/provider. Each worker is fresh independent inference rather than the current-model specialist session.
- Workers inherit a frozen upper bound of roots/grants/tool allowlist, intersected with current authority. They cannot grant approvals or spawn recursively.
- Hard policy excludes recursive worker starts/cancels, specialist session mutations, reviewer starts/cancels, image-analysis starts, authority binding, governance/lifecycle mutations, schedules and legacy session_checkpoint/session_handoff. Read-only workers also receive only Read-effect tools.
- Worker timeout_sec is 1..3,600, default 600; service capacity is 16 active workers, with 256 KiB retained output and at most 32 KiB per poll page. Follow next_output_offset. Reconnect durability does not establish survival through Manager crash.
- A usable loaded LM Studio Responses model is required; a worker does not promise a separate model instance. Running or a status read is not task completion.

Sources: [McpAgentWorkerTools.cpp](../src/Mcp/McpAgentWorkerTools.cpp), [ManagedRunWorkerPolicy.h](../include/ForgeConductor/Application/ManagedRunWorkerPolicy.h), [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md).

<a id="reviewertoolpack"></a>

### Fresh independent read-only review

**Model access:**

1. Use reviewer_start with actual review authorization and exactly one of opening_message_path or inline opening_message.
2. Use mode=tools for enforced read-only tool review, or mode=text_only to review only supplied text.
3. Poll reviewer_status for actual state/output/token usage/sealed outcome, following next_output_offset; use reviewer_cancel when authorized.

**Contracts and prerequisites:**

- Requires a matching running Manager and usable LM Studio provider; executor conversation history is not supplied.
- Inline opening text is bounded to 65,536 UTF-8 bytes. receive_timeout_sec is 1..3,600, default 600, for each provider Responses request through the complete body, bounded by the caller deadline.
- The receive budget is distinct from LM Studio's 180-second MCP deadline and from total reviewer lifetime. Start returns a run_id for subsequent status calls.
- Text-only review cannot establish external evidence. A running/failed review or an independent review report is not a CLU-approved gate.

Sources: [USER-GUIDE.md](USER-GUIDE.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp).

<a id="scheduledtasktoolpack"></a>

### Persistent scheduled model tasks

**Model access:**

1. Use schedule_create with name, task, owner_reference, actual authorization, and the supported UTC at_time or interval_sec schedule.
2. Use schedule_list for actual state/attention; for full records follow record_json_page byte paging with its revision and next_offset, restarting at zero on revision conflict.
3. Use schedule_run_now for an explicitly authorized immediate run/retry; schedule_cancel cancels the schedule and requests active-worker cancellation.

**Contracts and prerequisites:**

- A matching Manager must remain running to execute scheduled inference. Persistence alone does not imply a Windows service or unattended execution after Manager exit.
- Creation freezes the authority/tool allowlist upper bound; execution intersects current owner policy, avoids overlapping runs and does not automatically replay uncertain interrupted effects.
- Supply exactly one future UTC at_time or interval_sec (60..31,536,000). Defaults allow_tools=true and read_only_tools=true; at most 32 schedules are supported. Supplied settings cannot expand owner authority or the worker hard allowlist.
- Local toast submission records acceptance and display_confirmed=false; it does not establish that a notification was displayed or seen.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md).

<a id="memorytoolpack"></a>

### Durable local key/value notes

**Model access:**

1. Use memory_set with key/body and optional tags; retrieve with memory_get, search with memory_search or inventory with memory_list.
2. Use memory_delete to delete an authorized note by key.

**Contracts and prerequisites:**

- Notes survive chat sessions in the common Forge-home note store. Default inventories hide internal agent/continuity keys. Runtime limits: key 512 UTF-8 bytes, body 512 KiB, 32 tags; list/search default 50, maximum 200 entries.
- This legacy key/value interface is distinct from the explicitly project-scoped record/graph interface below.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [McpToolPackAdapter.cpp](../src/Mcp/McpToolPackAdapter.cpp).

<a id="projectmemorytoolpack"></a>

### Project-scoped records, search and typed links

**Model access:**

1. Initialize/open the authorized store with project_memory.initialize(project_path); retain its actual project_id and inspect project_memory.status.
2. Use project_memory.remember or remember_batch for redacted deduplicated records; retain stable IDs and versions.
3. Use get/search/list_recent with actual project_id and cursor/limit/response bounds; request bodies only when needed.
4. Use update with optimistic version checking, link for idempotent typed links, and forget for a tombstone.

**Contracts and prerequisites:**

- Project memory uses durable project-scoped SQLite records with redaction, deduplication, transactional batches and bounded deterministic lexical search/paging (FTS5 or bounded SQL fallback). Vector/semantic retrieval is not an advertised capability.
- Default runtime limits include body 256 KiB, batch 50 records/1 MiB, page 100 records/default 20. Resource profiles affect capacities; inspect initialize/status for actual limits rather than assuming a permissive listing schema grants unlimited storage.
- A record or evidence reference does not create authorization, factual truth or governance approval.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [PARITY.md](PARITY.md).

<a id="continuitytoolpack"></a>

### Legacy checkpoints and chat handoff packets

**Model access:**

1. Call context_get first in a new Primary/Fallback chat; use context_list to inspect recent packets.
2. Use session_checkpoint to soft-save and continue; use session_handoff to finalize an actionable packet and receive resume_seed.
3. When using packet_json, put the entire packet and all fields inside that JSON-encoded string; pass only packet_json at the outer level. Alternatively use supported direct fields with packet_json omitted.

**Contracts and prerequisites:**

- Arrays must be actual JSON arrays inside the packet. Preserve exact IDs for an update; do not duplicate goal/handoff_id/etc. both inside and outside packet_json.
- Include current goal/status, decisions, constraints, changed files, verification, blockers and actionable next steps with the observed project cwd.
- Saving a packet does not by itself prove a native successor chat was created or resumed. Owner-enabled native Auto Continuity is a separate orchestration service.

Sources: [USER-GUIDE.md](USER-GUIDE.md), [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [Continuity.md](wiki/Continuity.md).

<a id="continuitylifecycletoolpack"></a>

### Durable project handoff lifecycle

**Model access:**

1. Use continuity.status for actual durable state/active session/retry metadata; continuity.checkpoint saves a project checkpoint.
2. Use continuity.prepare_handoff or request_rollover with actual project_id, predecessor_session_id and mission plus observed handoff context.
3. Fetch get_pending_handoff, acknowledge the exact operation/handoff/successor using acknowledge_handoff, then resume to seal/select the acknowledged successor.

**Contracts and prerequisites:**

- These are project-scoped lifecycle records with exact identities and compare-and-set transitions. Do not invent a successor identity or infer native UI completion from a persisted lifecycle record.
- Auto Continuity readiness and native successor verification come from the Manager/Primary observation service. Unknown interrupted New/Send effects are not automatically replayed.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [Continuity.md](wiki/Continuity.md).

<a id="evidencetoolpack"></a>

### SHA-256 capture chains and evidence integrity

**Model access:**

1. Use evidence_digest to hash authorized binary evidence and append a durable capture-chain record.
2. Use evidence_log_read with bounded paging to verify the project chain; retain head_digest independently.

**Contracts and prerequisites:**

- Digests, byte counts, capture IDs and chain heads identify measured bytes. An unkeyed chain is not an identity signature, a factual endorsement or a governance approval.
- Independent head retention is needed to detect replacement of an entire chain.

Sources: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), [HOST-CAPABILITIES.md](HOST-CAPABILITIES.md).

## Specialist playbooks

Use `agent_list` and `agent_get` for the runtime playbook, and obey the `output_schema` returned by `agent_run_start`. Complete the actual session with `agent_run_complete`. Optional `agent_id` in `agent_spawn` applies a playbook to separate independent inference under worker policy.

| `agent_id` | Purpose |
| --- | --- |
| `debug` | Diagnose failures from logs, stack traces, and failing tests with evidence. |
| `docs` | Write accurate Markdown documentation and native PDF manuals. |
| `explore` | Map a codebase and report structure, entry points, build commands, risks, and the next specialist. |
| `implement` | Implement features and bug fixes with focused, verified code changes. |
| `plan` | Design ordered implementation plans with files, risks, and verification. |
| `precommit-audit` | Gate a commit or change review on a structured OK_TO_COMMIT decision. |
| `research` | Gather facts from authoritative project sources and report evidence-backed findings. |
| `review` | Review changes for correctness, security, tests, and maintainability. |
| `security` | Threat-model changes and identify credentials, injection, and unsafe patterns. |
| `test` | Discover, run, and report verification while identifying coverage gaps. |

The default runtime catalog is defined by [`AgentCatalog.cpp::mandatoryFallbacks`](../src/Application/AgentCatalog.cpp). A playbook’s preferred/forbidden tool names express guidance; they do not add names to the MCP catalog or replace enforced worker permissions.

## Supporting orchestration services

### Owner-enabled native Auto Continuity

**Access:** Owner controls the selected project/provider preference in Forge (enabled when no preference is saved); model supplies a detailed session_handoff when requested and checks Primary get_forge_status.visible_chat_continuity.

- Manager owns the native observer; Primary supplies authorized observations and automatic-resumption proof. Fallback/CLU do not create competing observers.
- Pressure uses actual selected provider usage or a separately labelled model-bound cached rendered-prompt projection; a cached projection is not measured current KV usage.
- At context reserve pressure and a completed authorized tool boundary, Forge requests a handoff, creates/sends a native successor and verifies conversation/message/integrations plus full native Forge context retrieval and a following successful tool result. The released acceptance proof uses Primary; result order does not require separate serialized request steps.
- Explicit terminal recovery is a separate archive-bound model-authored handoff, full context_get result and later successful Primary result; it does not automatically replay old New/Send effects.
- DPAPI-protected durable visible-handoff checkpoints support reconciliation. state_recovery_pending/recovery_pending means incomplete proof, not success; uncertain New/Send effects are not replayed.

Saving memory/handoff or enabling the preference alone does not prove rollover. Current qualification records distinguish tested reserve-pressure cases from unverified physical overflow, crash/UI interruption and running-agent reattachment.

### Project configuration and Manager persistence

**Access:** Owner configures projects/provider profiles/instruction packages/policy/authority/schedules in Forge; models read adopted state through status and relevant tools.

- Manager persists configured project scope, package order, policy binding, memory, preferences and schedules.
- Closing the App is distinct from stopping Manager-owned inference; scheduled execution requires the Manager running.

These owner controls are not additional hidden MCP tool names.

### Transport, deadline and repeated-call supervision

**Access:** Applied by Forge to MCP calls; models read actual error/status and use the supported bounded asynchronous APIs.

- Stdio uses compact newline-delimited UTF-8 JSON-RPC 2.0, response-only flushed stdout, and teardown on EOF/cancellation/broken pipe/parent exit.
- For applicable chat calls, repeated identical non-continuity, non-polling requests receive a warning and eventually a hard block; managed-run-v1 calls use their separate bounded run policy. Context reserve pressure schedules continuity rather than granting extra authority.

Client deadlines and unknown interrupted effects still apply; supervision is not proof a task completed.

## External prerequisites

| Service | When needed | Setup/access |
| --- | --- | --- |
| LM Studio model/provider | independent workers, reviewers, image analysis, scheduled inference, native chat rollover | Owner supplies a usable loaded model/provider profile and running matching Manager; inspect provider_status and service flags. |
| ComfyUI sd1 | image_generate, image_edit, generative job recovery | Owner configures/starts the optional compatible loopback provider and existing checkpoint; inspect image_provider_status. Disabled by default. |
| Git and GitHub credentials/network | local Git operations, private repository reads, authorized remote GitHub writes | Use configured native Git/account credentials when available; HTTP/shell writes require explicit task/account authorization. |
| CMake/CTest and a configured build tree | cmake_test_run | Use an existing initialized build directory and available tools. No implicit configure or language-server startup. |
| Python executable | verification_env_create | Supply an existing authorized python_path; optional pinned dependencies are limited by the schema. Native Forge/Office/image/PDF tools do not acquire a Python runtime dependency. |
| Interactive Windows desktop | desktop observation/input/capture, browser observation, native chat UI rollover | Use an accessible visible desktop under the current Windows account/integrity level. |
| Cloud mail/calendar/chat/storage and other APIs | account-specific remote operations | Supply the owner-authorized service/API connection through existing HTTP, browser or shell capabilities. Forge does not inherit this host's Codex connectors, create accounts or manufacture tokens. |

## Schema-checked call examples

These are illustrative requests, not live-call receipts. Replace example project paths and authorization placeholders with observed authorized values. Use IDs returned by real calls for polling and updates. Each argument object was validated against the exact advertised schema; native semantics, owner authority and service availability still apply.

### Read a project text file

**Prerequisites:** A bound authorized project containing README.md.

```json
{
  "jsonrpc": "2.0",
  "id": 100,
  "method": "tools/call",
  "params": {
    "name": "fs_read",
    "arguments": {
      "path": "README.md",
      "offset": 1,
      "length": 80
    }
  }
}
```

**Next:** Follow next_offset/next_byte_offset if more content is returned.

### Search the public web

**Prerequisites:** Project web access and network availability.

```json
{
  "jsonrpc": "2.0",
  "id": 101,
  "method": "tools/call",
  "params": {
    "name": "web_search",
    "arguments": {
      "query": "CMake CTest documentation",
      "limit": 5,
      "timeout_sec": 30
    }
  }
}
```

**Next:** Read actual URLs/status; fetch chosen pages and cite observed sources.

### Write a Word file

**Prerequisites:** Owner-authorized writable project reports destination.

```json
{
  "jsonrpc": "2.0",
  "id": 102,
  "method": "tools/call",
  "params": {
    "name": "document_write",
    "arguments": {
      "path": "reports/update.docx",
      "title": "Project update",
      "paragraphs": [
        "Verified work",
        "Remaining work"
      ]
    }
  }
}
```

**Next:** Inspect canonical path/bytes_written; native creation does not establish rendered appearance.

### Write an Excel file

**Prerequisites:** Owner-authorized writable destination.

```json
{
  "jsonrpc": "2.0",
  "id": 103,
  "method": "tools/call",
  "params": {
    "name": "spreadsheet_write",
    "arguments": {
      "path": "reports/results.xlsx",
      "sheets": [
        {
          "name": "Results",
          "rows": [
            [
              "Check",
              "Passed"
            ],
            [
              "Build",
              true
            ],
            [
              "Count",
              42
            ]
          ]
        }
      ]
    }
  }
}
```

**Next:** Inspect actual counts and bytes; cell strings are literal, not formulas.

### Write a PowerPoint file

**Prerequisites:** Owner-authorized writable destination.

```json
{
  "jsonrpc": "2.0",
  "id": 104,
  "method": "tools/call",
  "params": {
    "name": "presentation_write",
    "arguments": {
      "path": "reports/briefing.pptx",
      "title": "Briefing",
      "slides": [
        {
          "title": "Progress",
          "body": [
            "Verified result",
            "Next step"
          ]
        }
      ]
    }
  }
}
```

**Next:** Inspect actual artifact receipt; render with a suitable application when visual validation is needed.

### Draw a native PNG diagram

**Prerequisites:** Owner-authorized writable PNG destination.

```json
{
  "jsonrpc": "2.0",
  "id": 105,
  "method": "tools/call",
  "params": {
    "name": "image_write",
    "arguments": {
      "path": "reports/diagram.png",
      "width": 640,
      "height": 360,
      "background": "#FFFFFF",
      "elements": [
        {
          "type": "rectangle",
          "x": 30,
          "y": 30,
          "width": 220,
          "height": 100,
          "color": "#336699"
        },
        {
          "type": "text",
          "x": 35,
          "y": 150,
          "text": "Project status",
          "size": 24,
          "color": "#000000"
        }
      ]
    }
  }
}
```

**Next:** Inspect artifact dimensions/hash/preview; this draws elements rather than starting diffusion.

### Select a specialist for the current model

**Prerequisites:** A bound project and task authorization to inspect it.

```json
{
  "jsonrpc": "2.0",
  "id": 106,
  "method": "tools/call",
  "params": {
    "name": "agent_run_start",
    "arguments": {
      "agent_id": "explore",
      "goal": "Map this authorized repository and identify its build and test entry points."
    }
  }
}
```

**Next:** Retain session_id, follow returned instructions/output_schema, then submit the actual report using agent_run_complete.

### Start an independent worker

**Prerequisites:** Replace authorization with the real owner instruction; matching Manager/provider and inherited authority must be available.

```json
{
  "jsonrpc": "2.0",
  "id": 107,
  "method": "tools/call",
  "params": {
    "name": "agent_spawn",
    "arguments": {
      "task": "Independently inspect this project and report its build and test entry points.",
      "authorization": "<actual owner instruction authorizing this independent task>",
      "agent_id": "explore",
      "timeout_sec": 600
    }
  }
}
```

**Next:** Poll agent_poll with the actual run_id and follow next_output_offset.

### Start a text-only independent reviewer

**Prerequisites:** Replace authorization with the real instruction; matching Manager/provider required.

```json
{
  "jsonrpc": "2.0",
  "id": 108,
  "method": "tools/call",
  "params": {
    "name": "reviewer_start",
    "arguments": {
      "opening_message": "Review the supplied change summary and identify unsupported claims. No external evidence is supplied.",
      "mode": "text_only",
      "authorization": "<actual owner instruction authorizing this review>",
      "receive_timeout_sec": 600
    }
  }
}
```

**Next:** Poll reviewer_status; supplied-text review does not verify external evidence or approve a CLU gate.

### Start a tracked PowerShell job

**Prerequisites:** Owner-enabled shell policy, project Read/Write/Execute authority and authorization for the command.

```json
{
  "jsonrpc": "2.0",
  "id": 109,
  "method": "tools/call",
  "params": {
    "name": "shell_job_start",
    "arguments": {
      "command": "Get-Location",
      "timeout_sec": 300
    }
  }
}
```

**Next:** Retain job_id; poll shell_job_status no faster than every five seconds; use process_read_log for logs.

### Run a CMake build followed by CTest

**Prerequisites:** Replace build_dir with a real initialized authorized build tree; CMake/CTest and shell policy required.

```json
{
  "jsonrpc": "2.0",
  "id": 110,
  "method": "tools/call",
  "params": {
    "name": "cmake_test_run",
    "arguments": {
      "build_dir": "out/build/x64-release",
      "mode": "build_and_test",
      "config": "Release",
      "timeout_sec": 1800
    }
  }
}
```

**Next:** Poll cmake_test_status; inspect phase results/counts and page failures. No configure step is performed.

### Interpret an existing image independently

**Prerequisites:** Replace authorization; use a real authorized image and matching Manager/provider.

```json
{
  "jsonrpc": "2.0",
  "id": 111,
  "method": "tools/call",
  "params": {
    "name": "image_analyze",
    "arguments": {
      "path": "reports/diagram.png",
      "authorization": "<actual owner instruction authorizing visual analysis>",
      "question": "Describe the visible shapes and text.",
      "preview_max_dimension": 512
    }
  }
}
```

**Next:** Poll reviewer_status with the returned run_id; image preview/decode admission is not the completed analysis.

### Start optional ComfyUI generation

**Prerequisites:** Replace absolute path with an authorized destination; owner must have configured/started a compatible provider and authorized generation.

```json
{
  "jsonrpc": "2.0",
  "id": 112,
  "method": "tools/call",
  "params": {
    "name": "image_generate",
    "arguments": {
      "path": "C:\\Work\\ExampleProject\\reports\\generated.png",
      "prompt": "A simple blue geometric landscape",
      "seed": 42,
      "width": 512,
      "height": 512,
      "steps": 20
    }
  }
}
```

**Next:** Poll image_job_status. Do not repeat uncertain submissions; resume exact existing jobs only with fresh authority.

### Keep a durable local note

**Prerequisites:** Authorized local memory use.

```json
{
  "jsonrpc": "2.0",
  "id": 113,
  "method": "tools/call",
  "params": {
    "name": "memory_set",
    "arguments": {
      "key": "project-next-step",
      "body": "Read the actual test entry points before choosing verification.",
      "tags": [
        "planning"
      ]
    }
  }
}
```

**Next:** Retrieve with memory_get or memory_search; use project_memory for explicitly project-scoped structured records.

### Open a project memory store

**Prerequisites:** Replace project_path with the actual authorized selected project.

```json
{
  "jsonrpc": "2.0",
  "id": 114,
  "method": "tools/call",
  "params": {
    "name": "project_memory.initialize",
    "arguments": {
      "project_path": "C:\\Work\\ExampleProject"
    }
  }
}
```

**Next:** Use the returned project_id for project_memory.status and record/search/link operations.

### Soft-save legacy continuity state

**Prerequisites:** Replace cwd with the observed authorized project; report only actual progress.

```json
{
  "jsonrpc": "2.0",
  "id": 115,
  "method": "tools/call",
  "params": {
    "name": "session_checkpoint",
    "arguments": {
      "goal": "Document verified capabilities",
      "summary": "Tool catalog inspected; runtime availability still must be checked.",
      "cwd": "C:\\Work\\ExampleProject"
    }
  }
}
```

**Next:** Continue working; use a complete session_handoff packet for an actionable successor.

### Save an encoded handoff packet

**Prerequisites:** Replace example narrative, decisions, constraints, paths and next steps with actual work. The outer object contains only packet_json.

```json
{
  "jsonrpc": "2.0",
  "id": 116,
  "method": "tools/call",
  "params": {
    "name": "session_handoff",
    "arguments": {
      "packet_json": "{\"goal\":\"Complete the authorized repository task\",\"narrative\":\"Describe verified work and remaining work here.\",\"resume_seed\":\"Retrieve this packet, re-read applicable instructions, and resume the remaining task.\",\"decisions\":[\"Preserve the owner's workspace boundary.\"],\"constraints\":[\"Use only the current owner-authorized scope.\"],\"key_files\":[\"README.md\"],\"next_actions\":[\"Inspect remaining task evidence.\"],\"blockers\":[],\"cwd\":\"C:\\\\Work\\\\ExampleProject\"}"
    }
  }
}
```

**Next:** Retain the actual returned handoff_id; for an update put that ID inside the encoded packet. context_get retrieves the full packet; saving alone does not prove native rollover.

### Read an ordered instruction package

**Prerequisites:** Replace queue_row_id with the selected project status record.

```json
{
  "jsonrpc": "2.0",
  "id": 117,
  "method": "tools/call",
  "params": {
    "name": "instruction_package.read",
    "arguments": {
      "queue_row_id": "<actual returned queue row ID>",
      "offset": 0
    }
  }
}
```

**Next:** Follow next_cursor inventory pages and exact path/next_offset document pages in package order.

### Page independent reviewer output

**Prerequisites:** Replace run_id with the actual reviewer_start/image_analyze receipt.

```json
{
  "jsonrpc": "2.0",
  "id": 118,
  "method": "tools/call",
  "params": {
    "name": "reviewer_status",
    "arguments": {
      "run_id": "<actual returned run ID>",
      "output_offset": 0,
      "max_output_bytes": 32768
    }
  }
}
```

**Next:** Inspect sealed state/error/usage and follow next_output_offset while output_has_more.

### Page a CMake/CTest result

**Prerequisites:** Replace job_id with the actual cmake_test_run receipt.

```json
{
  "jsonrpc": "2.0",
  "id": 119,
  "method": "tools/call",
  "params": {
    "name": "cmake_test_status",
    "arguments": {
      "job_id": "<actual returned job ID>",
      "failure_offset": 0,
      "max_failures": 16
    }
  }
}
```

**Next:** Inspect actual phases and nullable counts; follow next_failure_offset, then read retained logs if needed.

### Read an owned job log

**Prerequisites:** Replace job_id with an actual Forge-owned job receipt.

```json
{
  "jsonrpc": "2.0",
  "id": 120,
  "method": "tools/call",
  "params": {
    "name": "process_read_log",
    "arguments": {
      "job_id": "<actual returned job ID>",
      "stream": "stdout",
      "offset": 0
    }
  }
}
```

**Next:** Follow next_offset and inspect truncation; logs and status are distinct evidence.

## Complete tool argument catalog

The 112 existing tools appear below, grouped by capability. The 13 added tools and schemas are documented in [ComfyUI automation](COMFYUI-AUTOMATION.md) and the matching JSON catalog. Every tool uses `tools/call`; set `params.name` to the exact heading and `params.arguments` to the argument object. Primary/Fallback are available for every entry; CLU availability is shown explicitly.

“Required” follows the advertised schema, and nested rows are required only inside their parent. Conditional root constraints are shown separately. Defaults are listed only when the schema supplies them; service defaults and stronger semantic rules are described in the access sections. The JSON companion retains the full original schemas, including nested arrays, additional-property contracts and `oneOf` branches.

There are 51 catalog Read tools, 61 Write tools, 90 requiring a bound project and five requiring shell policy. These labels describe catalog checks, not a complete OS permission contract. No MCP `annotations` or standard hint flags are emitted by this implementation.

### Host, provider and process inspection — tool arguments

#### `host_capabilities`

Report actual dedicated capabilities, tool names, filesystem mode and root authority, and external services needing a connection. Consult before claiming a capability is absent.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 94. Exact schema: JSON companion `tools` entry named `host_capabilities`.

#### `process_status`

Read a bounded native Windows process and service snapshot as the current user, without elevation.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 120. Exact schema: JSON companion `tools` entry named `process_status`.

#### `provider_status`

Inspect actually loaded LM Studio models and configured context/reserves. Missing file, revision or version facts remain null with provenance and reasons.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 133. Exact schema: JSON companion `tools` entry named `provider_status`.

### Runtime status and ten specialist playbooks — tool arguments

#### `agent_context`

Alias of agent_get — full playbook body.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `agent_id` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 44. Exact schema: JSON companion `tools` entry named `agent_context`.

#### `agent_get`

Get a specialist agent playbook by id.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `agent_id` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 45. Exact schema: JSON companion `tools` entry named `agent_get`.

#### `agent_list`

List specialist agent playbooks.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 46. Exact schema: JSON companion `tools` entry named `agent_list`.

#### `agent_recommend`

Recommend a specialist agent for a task description.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `task` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 48. Exact schema: JSON companion `tools` entry named `agent_recommend`.

#### `agent_run_complete`

Close a session with a report matching output_schema.

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `report` | optional | object |
| `session_id` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 49. Exact schema: JSON companion `tools` entry named `agent_run_complete`.

#### `agent_run_start`

Start a durable specialist session (supersedes prior open sessions).

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `agent_id` | required | string |
| `cwd` | optional | string |
| `goal` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 50. Exact schema: JSON companion `tools` entry named `agent_run_start`.

#### `agent_run_status`

Status of an agent session; reminds host to complete open runs.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `report` | optional | object |
| `session_id` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 51. Exact schema: JSON companion `tools` entry named `agent_run_status`.

#### `forge_status`

Runtime and project context: project folder, ordered instruction-package folders, development-policy source, home, agents, sessions, and tools.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 78. Exact schema: JSON companion `tools` entry named `forge_status`.

#### `get_forge_status`

Report the project folder, ordered instruction packages, development-policy folder, tool count, and agent count.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 87. Exact schema: JSON companion `tools` entry named `get_forge_status`.

### Ordered project instructions — tool arguments

#### `instruction_package.read`

Read a selected instruction package by queue_row_id from get_forge_status. Returns its pinned text and coverage, with cursor and byte-offset paging.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cursor` | optional | string |
| `offset` | optional | integer; minimum=0 |
| `path` | optional | string |
| `queue_row_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 105. Exact schema: JSON companion `tools` entry named `instruction_package.read`.

### Read the adopted development policy — tool arguments

#### `project_policy.read`

Read the adopted policy index or an exact pinned document. Follow next_cursor for every index page; supply path and next_offset as offset for every document part. This tool cannot adopt policy or approve reviews.

**Access:** roles Primary, Fallback, CLU; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cursor` | optional | string |
| `offset` | optional | integer; minimum=0 |
| `path` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 132. Exact schema: JSON companion `tools` entry named `project_policy.read`.

### CLU findings and correction evidence — tool arguments

#### `clu.evaluate`

Evaluate development evidence against the exact bound policy revision and return any CLU finding.

**Access:** roles Primary, Fallback, CLU; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `evidence` | required | object |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 54. Exact schema: JSON companion `tools` entry named `clu.evaluate`.

#### `clu.export_log`

Export the redacted project-bound CLU governance log without full private policy content.

**Access:** roles Primary, Fallback, CLU; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 55. Exact schema: JSON companion `tools` entry named `clu.export_log`.

#### `clu.findings`

List CLU findings, correction requests, notification receipts, and policy coverage state.

**Access:** roles Primary, Fallback, CLU; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 56. Exact schema: JSON companion `tools` entry named `clu.findings`.

#### `clu.resolve`

Attach correction evidence and resolve one exact CLU finding while retaining original evidence.

**Access:** roles Primary, Fallback, CLU; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `correction_evidence` | required | object |
| `finding_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 57. Exact schema: JSON companion `tools` entry named `clu.resolve`.

### Local files and additional authorized roots — tool arguments

#### `fs_delete`

Delete a file or directory.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 79. Exact schema: JSON companion `tools` entry named `fs_delete`.

#### `fs_edit`

Replace occurrences of old with new in a file.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `new` | required | string |
| `old` | required | string |
| `path` | required | string |

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 80. Exact schema: JSON companion `tools` entry named `fs_edit`.

#### `fs_glob`

Find files by name pattern under a path.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | optional | string |
| `pattern` | optional | string |

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 81. Exact schema: JSON companion `tools` entry named `fs_glob`.

#### `fs_list`

List directory entries.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | optional | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 82. Exact schema: JSON companion `tools` entry named `fs_list`.

#### `fs_mkdir`

Create a directory.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 83. Exact schema: JSON companion `tools` entry named `fs_mkdir`.

#### `fs_move`

Move/rename a path.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `dest` | required | string |
| `destination` | optional | string |
| `path` | required | string |
| `source` | optional | string |
| `src` | optional | string |

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 84. Exact schema: JSON companion `tools` entry named `fs_move`.

#### `fs_read`

Read a UTF-8 text file. Optional 1-based line window: offset (start line) + length/limit (line count). Response includes total_lines, start_line, end_line, has_more, next_offset. Do not re-call with the same offset when content was returned.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `byte_offset` | optional | integer; UTF-8 byte offset returned by next_byte_offset for an oversized line |
| `length` | optional | integer; Number of lines to return (alias: limit) |
| `limit` | optional | integer; Alias for length — number of lines to return |
| `offset` | optional | integer; 1-based start line for a partial read |
| `path` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 85. Exact schema: JSON companion `tools` entry named `fs_read`.

#### `fs_write`

Write a UTF-8 text file.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `content` | required | string |
| `path` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 86. Exact schema: JSON companion `tools` entry named `fs_write`.

#### `workspace_authority_bind`

Bind an existing additional root already present in the owner-configured allowlist to this project. Tool arguments cannot add new grants; forge_status reports configured and active roots.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `root` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 154. Exact schema: JSON companion `tools` entry named `workspace_authority_bind`.

### Recursive local text search — tool arguments

#### `search_text`

Recursive text search (grep).

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | optional | string |
| `pattern` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 141. Exact schema: JSON companion `tools` entry named `search_text`.

### Local Git status, history, staging and commits — tool arguments

#### `git_add`

git add path or -A.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 88. Exact schema: JSON companion `tools` entry named `git_add`.

#### `git_commit`

git commit -m message.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 89. Exact schema: JSON companion `tools` entry named `git_commit`.

#### `git_diff`

git diff (optional staged).

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 90. Exact schema: JSON companion `tools` entry named `git_diff`.

#### `git_log`

git log --oneline.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 91. Exact schema: JSON companion `tools` entry named `git_log`.

#### `git_status`

git status --porcelain.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** allowed by advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 92. Exact schema: JSON companion `tools` entry named `git_status`.

### GitHub repository evidence — tool arguments

#### `github_read`

Read GitHub repository runs, artifacts, refs and pull requests via fixed HTTPS GET routes. Uses configured credentials when available; reports permission and network failures explicitly.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `id` | optional | integer; minimum=1 |
| `operation` | required | string; enum=["runs","run","artifacts","artifact","refs","pull_requests","pull_request","pull_request_files"] |
| `page` | optional | integer; minimum=1; maximum=1000 |
| `per_page` | optional | integer; minimum=1; maximum=100 |
| `ref` | optional | string |
| `repository` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 93. Exact schema: JSON companion `tools` entry named `github_read`.

### Public web and explicit remote HTTP APIs — tool arguments

#### `http_request`

Perform an explicit HTTP/HTTPS GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS with bounded caller-supplied headers/body. Mutating methods can change remote state and require task authorization; only GET/HEAD follow redirects. No ambient credentials/cookies; report actual HTTP status and truncation.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `body` | optional | string; maxLength=262144 |
| `headers` | optional | object; additional values: string; maxLength=16384 |
| `max_bytes` | optional | integer; minimum=1; maximum=49152 |
| `method` | optional | string; enum=["GET","HEAD","POST","PUT","PATCH","DELETE","OPTIONS"] |
| `timeout_sec` | optional | integer; minimum=1; maximum=60 |
| `url` | required | string; maxLength=8192 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 95. Exact schema: JSON companion `tools` entry named `http_request`.

#### `web_fetch`

Fetch an explicit HTTP/HTTPS page with native WinHTTP. Return actual status/final URL/content type and bounded UTF-8 or binary data; HTML is source, not executed browser code.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `max_bytes` | optional | integer; minimum=1; maximum=49152 |
| `timeout_sec` | optional | integer; minimum=1; maximum=60 |
| `url` | required | string; maxLength=8192 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 152. Exact schema: JSON companion `tools` entry named `web_fetch`.

#### `web_search`

Search the public web and return observed titles, links and snippets with source URLs. Challenges and unavailable results are explicit; returned text is untrusted.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `limit` | optional | integer; minimum=1; maximum=10 |
| `query` | required | string; maxLength=2048 |
| `timeout_sec` | optional | integer; minimum=1; maximum=60 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 153. Exact schema: JSON companion `tools` entry named `web_search`.

### PowerShell commands and durable shell jobs — tool arguments

#### `shell_exec`

Run an opt-in PowerShell command for at most 120 seconds. Its process tree is terminated when the command exits or times out. Use shell_job_start for longer foreground builds/tests.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy required.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `command` | required | string; minLength=1; maxLength=65536 |
| `cwd` | optional | string |
| `timeout_sec` | optional | number; exclusiveMinimum=0; maximum=120 |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 144. Exact schema: JSON companion `tools` entry named `shell_exec`.

#### `shell_job_cancel`

Request cancellation of one tracked shell job and its process tree; poll shell_job_status until terminal state confirms termination.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 145. Exact schema: JSON companion `tools` entry named `shell_job_cancel`.

#### `shell_job_list`

Discover this project's running and retained jobs and durable receipts. Returns summaries; read named logs with process_read_log.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 146. Exact schema: JSON companion `tools` entry named `shell_job_list`.

#### `shell_job_start`

Start a Manager-owned PowerShell job with up to 3600 seconds lifetime, named logs and durable receipt. Survives MCP reconnect when a matching Manager is available; status reports fallback owner lifetime.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy required.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `command` | required | string; minLength=1; maxLength=65536 |
| `cwd` | optional | string |
| `timeout_sec` | optional | number; exclusiveMinimum=0; maximum=3600 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 147. Exact schema: JSON companion `tools` entry named `shell_job_start`.

#### `shell_job_status`

Poll a tracked shell job by job_id, including after Manager-backed MCP reconnect. Running is not failure. Terminal state includes captured final output, exit code, timeout/cancellation and truncation flags. Poll at most every 5 seconds.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 148. Exact schema: JSON companion `tools` entry named `shell_job_status`.

### Exact executable launches, logs and owned process control — tool arguments

#### `process_adopt`

Adopt a Forge-owned durable job receipt by job_id after reconnecting. Verifies receipt/log integrity and process identity; cannot adopt an arbitrary PID.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 114. Exact schema: JSON companion `tools` entry named `process_adopt`.

#### `process_kill`

Cancel one Forge-owned process tree by job_id; poll until final termination is confirmed.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 115. Exact schema: JSON companion `tools` entry named `process_kill`.

#### `process_launch`

Launch an authorized executable with exact argv, cwd and environment. Returns stable job_id, actual PID and named stdout/stderr logs. A matching Manager owns the job across MCP reconnects; otherwise status reports connector-owned lifetime.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy required.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `args` | optional | array; maxItems=256; items: string; maxLength=4096 |
| `command` | required | string; minLength=1; maxLength=4096 |
| `cwd` | optional | string |
| `env` | optional | object; maxProperties=128; additional values: string; maxLength=4096 |
| `timeout_sec` | optional | number; exclusiveMinimum=0; maximum=3600 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 116. Exact schema: JSON companion `tools` entry named `process_launch`.

#### `process_list`

List this project's active and retained durable jobs. Interrupted broker jobs report unknown exit status rather than invented success.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 117. Exact schema: JSON companion `tools` entry named `process_list`.

#### `process_poll`

Read a durable job's alive/done state, actual PID, elapsed time, exit status and log/receipt paths. Poll no faster than every five seconds.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 118. Exact schema: JSON companion `tools` entry named `process_poll`.

#### `process_read_log`

Read live or final stdout/stderr by job_id with tail_lines or byte offset and bounded paging; returns next_offset and truncation state.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |
| `offset` | optional | integer; minimum=0 |
| `stream` | optional | string; enum=["stdout","stderr"] |
| `tail_lines` | optional | integer; minimum=1; maximum=1000 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 119. Exact schema: JSON companion `tools` entry named `process_read_log`.

#### `process_wait`

Wait up to 30 seconds for a durable job to finish. Timeout returns done=false and leaves the job running; repeat or poll for longer runs.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=1; maxLength=128 |
| `timeout_sec` | optional | integer; minimum=0; maximum=30 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 121. Exact schema: JSON companion `tools` entry named `process_wait`.

### Native CMake build and CTest jobs — tool arguments

#### `cmake_test_run`

Start a durable CTest job in an explicit initialized CMake build directory. mode=build_and_test first runs cmake --build and runs CTest only after a successful build; default mode=test. Returns a job ID; cmake_test_status provides actual phase results, sealed JUnit counts and paged failures. No implicit configure step. Use process_read_log/wait/kill for logs, bounded waiting and cancellation.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy required.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `build_dir` | required | string; minLength=1; maxLength=32768 |
| `config` | optional | string; minLength=1; maxLength=1024 |
| `filter` | optional | string; minLength=1; maxLength=1024 |
| `mode` | optional | string; enum=["test","build_and_test"]; default="test" |
| `target` | optional | string; minLength=1; maxLength=1024 |
| `timeout_sec` | optional | integer; minimum=1; maximum=3600; default=1800 |

**Top-level additional properties:** rejected.

**Additional request constraints:**

```json
{
  "oneOf": [
    {
      "properties": {
        "mode": {
          "const": "build_and_test"
        }
      },
      "required": [
        "mode"
      ]
    },
    {
      "not": {
        "required": [
          "target"
        ]
      },
      "properties": {
        "mode": {
          "const": "test"
        }
      }
    }
  ]
}
```

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 58. Exact schema: JSON companion `tools` entry named `cmake_test_run`.

#### `cmake_test_status`

Read a project-owned CMake/CTest job with actual build/test exit status, sealed JUnit counts and bounded failure paging. A successful status read does not mean the tests passed. Follow next_failure_offset while has_more; absent phase results and counts remain null. Use process_read_log for complete retained logs.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `failure_offset` | optional | integer; minimum=0 |
| `job_id` | required | string; minLength=1; maxLength=128 |
| `max_failures` | optional | integer; minimum=1; maximum=32; default=16 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 59. Exact schema: JSON companion `tools` entry named `cmake_test_status`.

### Pinned external Python verification environments — tool arguments

#### `verification_env_create`

Materialize a pinned Python venv in an authorized external directory as a durable job, recording exact installed versions and a manifest. Does not edit product source or grant elevation.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy required.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string |
| `python_path` | required | string |
| `requirements` | optional | array; maxItems=2; items: string; enum=["jsonschema==4.25.1","PyYAML==6.0.3"] |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 150. Exact schema: JSON companion `tools` entry named `verification_env_create`.

#### `verification_env_status`

Read the pinned verification venv manifest from an authorized directory; reports exact runtime and distributions only after successful creation.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 151. Exact schema: JSON companion `tools` entry named `verification_env_status`.

### Native PDF creation — tool arguments

#### `pdf_from_file`

Convert a local markdown/text file to PDF.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `dest_path` | optional | string |
| `source_path` | required | string |
| `title` | optional | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 111. Exact schema: JSON companion `tools` entry named `pdf_from_file`.

#### `pdf_write`

Write a PDF from markdown-ish text (stdlib, no pandoc).

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `content` | required | string |
| `path` | required | string |
| `title` | optional | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 112. Exact schema: JSON companion `tools` entry named `pdf_write`.

### Native DOCX, XLSX and PPTX files — tool arguments

#### `document_write`

Create a valid native Word DOCX ZIP package from a title and paragraphs. No Office installation or Python required. Does not render or approve its content.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `paragraphs` | required | array; maxItems=4096; items: string; maxLength=65536 |
| `path` | required | string; maxLength=32768 |
| `title` | optional | string; maxLength=65536 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 75. Exact schema: JSON companion `tools` entry named `document_write`.

#### `presentation_write`

Create a valid native PowerPoint PPTX ZIP package with title/body slides, layout, master, theme and relationships. No Office installation or Python required.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string; maxLength=32768 |
| `slides` | required | array; maxItems=128 |
| `slides[].body` | required in parent | array; maxItems=128; items: string; maxLength=65536 |
| `slides[].title` | required in parent | string; maxLength=65536 |
| `title` | optional | string; maxLength=65536 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 113. Exact schema: JSON companion `tools` entry named `presentation_write`.

#### `spreadsheet_write`

Create a valid native Excel XLSX ZIP package from sheets and typed cell rows. Text is literal; formulas are not evaluated. No Office installation or Python required.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string; maxLength=32768 |
| `sheets` | required | array; maxItems=32 |
| `sheets[].name` | required in parent | string; maxLength=128 |
| `sheets[].rows` | required in parent | array; maxItems=10000; items: array; maxItems=256; items: string/number/boolean/null |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 149. Exact schema: JSON companion `tools` entry named `spreadsheet_write`.

### Windows UI observation, input, capture and browser launch — tool arguments

#### `browser_open`

Open an HTTP/HTTPS URL in the user's registered browser. Returns launch acceptance, not proof of page loading; observe the browser with desktop tools.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `url` | required | string; maxLength=16384 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 53. Exact schema: JSON companion `tools` entry named `browser_open`.

#### `desktop_capture`

Capture the visible desktop region of a listed window/PID to an authorized PNG and return an image preview. Optional preview_max_dimension (128..2048, default 256) preserves more detail; adaptive resizing retains the 512 KiB encoded bound. Overlapping windows may appear; this is visible-screen capture.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string; maxLength=32768 |
| `pid` | required | integer; minimum=1; maximum=4294967295 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `window_id` | required | integer; minimum=1; maximum=9223372036854775807 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 69. Exact schema: JSON companion `tools` entry named `desktop_capture`.

#### `desktop_click`

Click a window-relative point in a listed visible window/PID. Requires task authorization and a fresh observation. Returns input submission; observe the resulting UI.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `button` | optional | string; enum=["left","right"] |
| `pid` | required | integer; minimum=1; maximum=4294967295 |
| `window_id` | required | integer; minimum=1; maximum=9223372036854775807 |
| `x` | required | integer; minimum=0; maximum=8192 |
| `y` | required | integer; minimum=0; maximum=8192 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 70. Exact schema: JSON companion `tools` entry named `desktop_click`.

#### `desktop_key`

Submit a named key and optional ctrl/alt/shift modifiers to a listed foreground window/PID. Requires task authorization; observe the resulting UI.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `alt` | optional | boolean |
| `ctrl` | optional | boolean |
| `key` | required | string; maxLength=32 |
| `pid` | required | integer; minimum=1; maximum=4294967295 |
| `shift` | optional | boolean |
| `window_id` | required | integer; minimum=1; maximum=9223372036854775807 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 71. Exact schema: JSON companion `tools` entry named `desktop_key`.

#### `desktop_list`

List visible Windows windows with exact window_id/PID/title and screen geometry under the current Windows account.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `limit` | optional | integer; minimum=1; maximum=500 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 72. Exact schema: JSON companion `tools` entry named `desktop_list`.

#### `desktop_read`

Read bounded Windows accessibility controls for an exact listed visible window/PID, with window-relative rectangles and enabled/offscreen states. Optional zero-based offset defaults to 0; follow next_offset while has_more to reach later controls. Row indices refer to the freshly observed tree and may change when the UI changes.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `limit` | optional | integer; minimum=1; maximum=300 |
| `offset` | optional | integer; minimum=0; maximum=2147483647 |
| `pid` | required | integer; minimum=1; maximum=4294967295 |
| `window_id` | required | integer; minimum=1; maximum=9223372036854775807 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 73. Exact schema: JSON companion `tools` entry named `desktop_read`.

#### `desktop_type`

Submit literal Unicode text to the focused control in an exact listed foreground window/PID. Requires task authorization; observe focus and the resulting UI.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `pid` | required | integer; minimum=1; maximum=4294967295 |
| `text` | required | string; maxLength=65536 |
| `window_id` | required | integer; minimum=1; maximum=9223372036854775807 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 74. Exact schema: JSON companion `tools` entry named `desktop_type`.

### Local image decoding, drawing and independent visual analysis — tool arguments

#### `image_analyze`

Start a fresh independent read-only visual review of an authorized decoded image with explicit authorization. Returns an asynchronous run ID; use reviewer_status for the actual model analysis, usage, sealed outcome and infrastructure errors. Optional preview_max_dimension (128..2048, default 256) requests more image detail within the 512 KiB encoded bound. Use this when LM Studio displays an image preview without supplying pixels to its chat model. No executor history or policy approval is included.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `authorization` | required | string; minLength=1; maxLength=1024 |
| `path` | required | string; minLength=1; maxLength=32768 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `question` | optional | string; minLength=1; maxLength=4096 |
| `receive_timeout_sec` | optional | integer; minimum=1; maximum=3600; default=600 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 96. Exact schema: JSON companion `tools` entry named `image_analyze`.

#### `image_read`

Decode an authorized local PNG/JPEG/GIF/BMP/TIFF/ICO using native Windows codecs and return a PNG preview with original dimensions. Optional samples (1..64 exact source x/y pixel coordinates) return measured RGBA8 values; decoded-frame and emitted-preview SHA-256 identify the actual bytes. Optional preview_max_dimension (128..2048, default 256) requests more detail; adaptive resizing retains the 512 KiB encoded bound. These measurements do not establish the chat model's inference input; use image_analyze and reviewer_status for independent visual interpretation.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `path` | required | string; maxLength=32768 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `samples` | optional | array; minItems=1; maxItems=64 |
| `samples[].x` | required in parent | integer; minimum=0; maximum=4095 |
| `samples[].y` | required in parent | integer; minimum=0; maximum=4095 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 103. Exact schema: JSON companion `tools` entry named `image_read`.

#### `image_write`

Render rectangles, ellipses, lines and Unicode text to an authorized native PNG, returning an image preview. Optional preview_max_dimension (128..2048, default 256) requests more detail within the 512 KiB encoded bound; the written image keeps its full dimensions. Supports diagrams/charts; generative artwork requires a separate image provider.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `background` | optional | string; maxLength=7 |
| `elements` | required | array; maxItems=1000 |
| `elements[].color` | optional in parent | string; maxLength=7 |
| `elements[].height` | optional in parent | integer; minimum=1; maximum=8192 |
| `elements[].size` | optional in parent | integer; minimum=6; maximum=256 |
| `elements[].stroke_width` | optional in parent | integer; minimum=1; maximum=128 |
| `elements[].text` | optional in parent | string; maxLength=16384 |
| `elements[].type` | required in parent | string; enum=["rectangle","ellipse","line","text"] |
| `elements[].width` | optional in parent | integer; minimum=1; maximum=8192 |
| `elements[].x` | optional in parent | integer; minimum=-8192; maximum=8192 |
| `elements[].x2` | optional in parent | integer; minimum=-8192; maximum=8192 |
| `elements[].y` | optional in parent | integer; minimum=-8192; maximum=8192 |
| `elements[].y2` | optional in parent | integer; minimum=-8192; maximum=8192 |
| `height` | optional | integer; minimum=1; maximum=4096 |
| `path` | required | string; maxLength=32768 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `width` | optional | integer; minimum=1; maximum=4096 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 104. Exact schema: JSON companion `tools` entry named `image_write`.

### Optional ComfyUI generation, variation and masked edits — tool arguments

#### `image_edit`

Start a Manager-owned image variation or masked diffusion edit using the explicitly configured optional ComfyUI provider. Requires an absolute authorized source_path, output path, prompt, seed, width and height; dimensions must match the source and optional mask. Mask red values select edits; zero preserves exact original RGBA. Returns a job ID; image_job_status reports actual provider state and published preview. No model installation or server startup.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cfg` | optional | number; minimum=0; maximum=20; default=7 |
| `denoise` | optional | number; minimum=0.05; maximum=1; default=1.0 |
| `height` | required | integer; minimum=64; maximum=1024 |
| `mask_path` | optional | string; minLength=1; maxLength=32768 |
| `negative_prompt` | optional | string; maxLength=4096 |
| `path` | required | string; minLength=1; maxLength=32768 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `prompt` | required | string; minLength=1; maxLength=4096 |
| `seed` | required | integer; minimum=0; maximum=9007199254740991 |
| `source_path` | required | string; minLength=1; maxLength=32768 |
| `steps` | optional | integer; minimum=1; maximum=100; default=20 |
| `timeout_sec` | optional | integer; minimum=1; maximum=3600; default=1800 |
| `width` | required | integer; minimum=64; maximum=1024 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 97. Exact schema: JSON companion `tools` entry named `image_edit`.

#### `image_generate`

Start a Manager-owned image generation using the explicitly configured optional ComfyUI provider. Requires an absolute authorized output path, prompt, seed, width and height. Returns a job ID; image_job_status reports actual provider state and published preview. Provider is disabled by default; no model installation or server startup.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cfg` | optional | number; minimum=0; maximum=20; default=7 |
| `denoise` | optional | number; minimum=0.05; maximum=1; default=1.0 |
| `height` | required | integer; minimum=64; maximum=1024 |
| `negative_prompt` | optional | string; maxLength=4096 |
| `path` | required | string; minLength=1; maxLength=32768 |
| `preview_max_dimension` | optional | integer; minimum=128; maximum=2048; default=256 |
| `prompt` | required | string; minLength=1; maxLength=4096 |
| `seed` | required | integer; minimum=0; maximum=9007199254740991 |
| `steps` | optional | integer; minimum=1; maximum=100; default=20 |
| `timeout_sec` | optional | integer; minimum=1; maximum=3600; default=1800 |
| `width` | required | integer; minimum=64; maximum=1024 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 98. Exact schema: JSON companion `tools` entry named `image_generate`.

#### `image_job_cancel`

Request cancellation of one project-owned image job. Exact pending provider jobs may be removed from the queue; active cancellation suppresses local publication and can leave remote generation running. Read image_job_status for actual cancellation confirmation. Never interrupts a shared provider process.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=36; maxLength=36 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 99. Exact schema: JSON companion `tools` entry named `image_job_cancel`.

#### `image_job_resume`

Reattach fresh caller authority to a recovered project-owned image job before publishing its exact existing provider result. Revalidates scope, provider settings and unchanged output destination. Never resubmits generation; missing or uncertain provider history remains unknown. Read image_job_status for the actual result.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=36; maxLength=36 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 100. Exact schema: JSON companion `tools` entry named `image_job_resume`.

#### `image_job_status`

Read a project-owned Manager image job across MCP reconnects with actual provider state, explicit errors, artifact hashes and a bounded image preview after publication. Optional wait_sec waits at most 60 seconds. Uncertain submissions and recovered unfinished jobs are not resubmitted automatically.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `job_id` | required | string; minLength=36; maxLength=36 |
| `wait_sec` | optional | integer; minimum=0; maximum=60; default=0 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 101. Exact schema: JSON companion `tools` entry named `image_job_status`.

#### `image_provider_status`

Inspect the explicitly configured optional ComfyUI endpoint, node contracts and checkpoint inventory without starting the server or loading a model. Reports disabled, unavailable and incompatible providers explicitly; catalog presence alone does not establish generation availability.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

No named arguments are advertised; use `{}`.

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 102. Exact schema: JSON companion `tools` entry named `image_provider_status`.

### Independent Manager-owned model workers — tool arguments

#### `agent_cancel`

Cancel one independent Manager-owned worker in this project; follow agent_poll to confirm final state.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `authorization` | required | string; maxLength=4096 |
| `run_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 43. Exact schema: JSON companion `tools` entry named `agent_cancel`.

#### `agent_poll`

Read an independent worker's actual state, usage, sealed receipt and UTF-8 output page across MCP reconnects.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `max_output_bytes` | optional | integer; minimum=1; maximum=32768 |
| `output_offset` | optional | integer; minimum=0; maximum=262144 |
| `run_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 47. Exact schema: JSON companion `tools` entry named `agent_poll`.

#### `agent_spawn`

Start a fresh independent Manager-owned model task with explicit task authorization. Inherits current roots/grants and cannot grant policy approvals or spawn recursively; existing specialist sessions remain available.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `agent_id` | optional | string; maxLength=128 |
| `authorization` | required | string; maxLength=4096 |
| `task` | required | string; maxLength=65536 |
| `timeout_sec` | optional | integer; minimum=1; maximum=3600 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 52. Exact schema: JSON companion `tools` entry named `agent_spawn`.

### Fresh independent read-only review — tool arguments

#### `reviewer_cancel`

Cancel one independently started read-only reviewer run for this project.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `run_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 134. Exact schema: JSON companion `tools` entry named `reviewer_cancel`.

#### `reviewer_start`

Start a fresh Manager-owned reviewer with exactly one authorized opening_message_path or bounded inline opening_message and no executor history. Requires explicit review authorization; read-only tools are enforced. receive_timeout_sec budgets each provider Responses request from send through response body, defaults to 600 (1...3600), and remains bounded by its caller deadline; mode=text_only omits tools and reviews only supplied text.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `authorization` | required | string; minLength=1; maxLength=1024 |
| `mode` | optional | string; enum=["tools","text_only"]; default="tools" |
| `opening_message` | optional | string; minLength=1; maxLength=65536; Inline UTF-8 opening message, at most 65536 bytes; mutually exclusive with opening_message_path. |
| `opening_message_path` | optional | string |
| `receive_timeout_sec` | optional | integer; minimum=1; maximum=3600; default=600; Per-provider Responses request budget from send through complete response body; caller deadline may shorten it. This is not the total reviewer lifetime. |
| `task` | optional | string; maxLength=32768 |

**Top-level additional properties:** rejected.

**Additional request constraints:**

```json
{
  "oneOf": [
    {
      "required": [
        "opening_message_path"
      ]
    },
    {
      "required": [
        "opening_message"
      ]
    }
  ]
}
```

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 135. Exact schema: JSON companion `tools` entry named `reviewer_start`.

#### `reviewer_status`

Read the separate reviewer run's actual state, provider response, token usage and bounded UTF-8 output page; follow next_output_offset for more. A running or failed review is not an approved gate.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `max_output_bytes` | optional | integer; minimum=4; maximum=32768 |
| `output_offset` | optional | integer; minimum=0; maximum=262144 |
| `run_id` | required | string; minLength=1; maxLength=128 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 136. Exact schema: JSON companion `tools` entry named `reviewer_status`.

### Persistent scheduled model tasks — tool arguments

#### `schedule_cancel`

Cancel one persistent model-task schedule and request cancellation of its active worker. Requires explicit authorization.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `authorization` | required | string; maxLength=2048 |
| `schedule_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 137. Exact schema: JSON companion `tools` entry named `schedule_cancel`.

#### `schedule_create`

Persist an authorized model task for a UTC time or interval. Freezes current authority/tool allowlist, runs through the Manager, and blocks uncertain interrupted effects from automatic replay.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `allow_tools` | optional | boolean |
| `allowed_tools` | optional | array; maxItems=128; items: string; maxLength=128 |
| `at_time` | optional | string; maxLength=20 |
| `authorization` | required | string; maxLength=2048 |
| `interval_sec` | optional | integer; minimum=60; maximum=31536000 |
| `name` | required | string; maxLength=128 |
| `owner_reference` | required | string; maxLength=512 |
| `read_only_tools` | optional | boolean |
| `task` | required | string; maxLength=65536 |
| `timeout_sec` | optional | integer; minimum=1; maximum=3600 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 138. Exact schema: JSON companion `tools` entry named `schedule_create`.

#### `schedule_list`

Read this project's persistent model schedules, actual run state, and required owner attention.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `max_bytes` | optional | integer; minimum=1; maximum=32768 |
| `offset` | optional | integer; minimum=0; maximum=8388608 |
| `revision` | optional | string; maxLength=20 |
| `schedule_id` | optional | string; maxLength=36 |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 139. Exact schema: JSON companion `tools` entry named `schedule_list`.

#### `schedule_run_now`

Explicitly start a stored task now, including resumption after uncertain interrupted effects. Requires authorization and current owner authority.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `authorization` | required | string; maxLength=2048 |
| `schedule_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 140. Exact schema: JSON companion `tools` entry named `schedule_run_now`.

### Durable local key/value notes — tool arguments

#### `memory_delete`

Delete a durable memory note by key.

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `key` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 106. Exact schema: JSON companion `tools` entry named `memory_delete`.

#### `memory_get`

Read a durable memory note by key.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `key` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 107. Exact schema: JSON companion `tools` entry named `memory_get`.

#### `memory_list`

List durable memory notes (optional prefix/tag; hides internal agent and continuity keys by default).

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `include_body` | optional | boolean |
| `include_system` | optional | boolean |
| `limit` | optional | integer |
| `prefix` | optional | string |
| `tag` | optional | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 108. Exact schema: JSON companion `tools` entry named `memory_list`.

#### `memory_search`

Search durable memory notes by substring in key/body/tags.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `include_body` | optional | boolean |
| `include_system` | optional | boolean |
| `limit` | optional | integer |
| `query` | required | string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 109. Exact schema: JSON companion `tools` entry named `memory_search`.

#### `memory_set`

Store a durable key/value note in Forge local memory (survives chat sessions).

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `body` | required | string |
| `content` | optional | string; Alias of body |
| `key` | required | string |
| `tags` | optional | array; items: string |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 110. Exact schema: JSON companion `tools` entry named `memory_set`.

### Project-scoped records, search and typed links — tool arguments

#### `project_memory.forget`

Tombstone a record in one project.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `id` | required | string |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 122. Exact schema: JSON companion `tools` entry named `project_memory.forget`.

#### `project_memory.get`

Fetch project memory records by stable ID.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `id` | optional | string |
| `ids` | optional | array; items: string |
| `include_body` | optional | boolean |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 123. Exact schema: JSON companion `tools` entry named `project_memory.get`.

#### `project_memory.initialize`

Create or open a durable project-scoped memory store.

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `display_name` | optional | string |
| `idempotency_key` | optional | string |
| `project_id` | optional | string |
| `project_path` | required | string |
| `repository_identity` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 124. Exact schema: JSON companion `tools` entry named `project_memory.initialize`.

#### `project_memory.link`

Create an idempotent typed link between records.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `project_id` | required | string |
| `relation` | required | string |
| `source_id` | required | string |
| `target_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 125. Exact schema: JSON companion `tools` entry named `project_memory.link`.

#### `project_memory.list_recent`

List recent project records with bounded pagination.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cursor` | optional | string |
| `deadline_ms` | optional | integer |
| `include_body` | optional | boolean |
| `kinds` | optional | array; maxItems=100; items: string |
| `limit` | optional | integer |
| `maximum_response_bytes` | optional | integer |
| `project_id` | required | string |
| `session_id` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 126. Exact schema: JSON companion `tools` entry named `project_memory.list_recent`.

#### `project_memory.remember`

Store one redacted, deduplicated project memory record.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `body` | optional | string |
| `confidence` | optional | number |
| `deadline_ms` | optional | integer |
| `expires_at` | optional | string |
| `idempotency_key` | optional | string |
| `importance` | optional | number |
| `kind` | required | string |
| `project_id` | required | string |
| `related_ids` | optional | array; items: string |
| `session_id` | optional | string |
| `source_kind` | optional | string |
| `source_reference` | optional | string |
| `summary` | required | string |
| `tags` | optional | array; items: string |
| `title` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 127. Exact schema: JSON companion `tools` entry named `project_memory.remember`.

#### `project_memory.remember_batch`

Store a bounded batch transactionally.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `items` | required | array; maxItems=50; items: object |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 128. Exact schema: JSON companion `tools` entry named `project_memory.remember_batch`.

#### `project_memory.search`

Search one project with deterministic bounded pagination.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `cursor` | optional | string |
| `deadline_ms` | optional | integer |
| `include_body` | optional | boolean |
| `kinds` | optional | array; maxItems=100; items: string |
| `limit` | optional | integer |
| `maximum_response_bytes` | optional | integer |
| `project_id` | required | string |
| `query` | required | string |
| `session_id` | optional | string |
| `tags` | optional | array; items: string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 129. Exact schema: JSON companion `tools` entry named `project_memory.search`.

#### `project_memory.status`

Report project memory health, sizes, capabilities, and limits.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `deadline_ms` | optional | integer |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 130. Exact schema: JSON companion `tools` entry named `project_memory.status`.

#### `project_memory.update`

Update a record with optimistic version checking.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `body` | optional | string |
| `deadline_ms` | optional | integer |
| `expected_version` | required | integer |
| `id` | required | string |
| `project_id` | required | string |
| `summary` | optional | string |
| `tags` | optional | array; items: string |
| `title` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 131. Exact schema: JSON companion `tools` entry named `project_memory.update`.

### Legacy checkpoints and chat handoff packets — tool arguments

#### `context_get`

Load latest (or id) handoff packet — call first in every new chat bootstrap.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `handoff_id` | optional | string |
| `id` | optional | string |
| `resume_ready` | optional | boolean; Prefer latest resume-ready packet |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 60. Exact schema: JSON companion `tools` entry named `context_get`.

#### `context_list`

List recent context handoff packets.

**Access:** roles Primary, Fallback; catalog effect Read; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `limit` | optional | integer |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 61. Exact schema: JSON companion `tools` entry named `context_list`.

#### `session_checkpoint`

Soft-save context + open agent sessions for continuity (continue working).

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `blockers` | optional | array; items: string |
| `chat_label` | optional | string |
| `cwd` | optional | string |
| `decisions` | optional | array; items: string |
| `goal` | optional | string |
| `handoff_id` | optional | string; Update an existing packet |
| `key_files` | optional | array; items: string |
| `narrative` | optional | string |
| `next_actions` | optional | array; items: string |
| `project_slug` | optional | string |
| `resume_seed` | optional | string |
| `status` | optional | string |
| `summary` | optional | string; Alias for narrative |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 142. Exact schema: JSON companion `tools` entry named `session_checkpoint`.

#### `session_handoff`

Finalize context/agent handoff for a new chat; returns resume_seed. Prefer before context is full.

**Access:** roles Primary, Fallback; catalog effect Write; bound project not required by catalog; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `blockers` | optional | array; items: string |
| `chat_label` | optional | string |
| `cwd` | optional | string |
| `decisions` | optional | array; items: string; For Auto Continuity, record all explicit user constraints as nonblank strings in this nonempty array. |
| `goal` | optional | string |
| `handoff_id` | optional | string; Update an existing packet |
| `key_files` | optional | array; items: string |
| `narrative` | optional | string |
| `next_actions` | optional | array; items: string |
| `packet_json` | optional | string; JSON-string containing the complete model-authored packet object. When using this encoded form, supply ONLY packet_json as the outer argument; put ALL fields, including handoff_id for an update, inside the encoded object. Do not duplicate fields beside packet_json. Include goal, narrative, resume_seed, decisions, key_files and next_actions; collections must be JSON arrays inside the object. Direct field calls remain supported when packet_json is omitted. |
| `project_slug` | optional | string |
| `resume_seed` | optional | string |
| `status` | optional | string |
| `summary` | optional | string; Alias for narrative |

**Top-level additional properties:** unspecified in advertised schema.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 143. Exact schema: JSON companion `tools` entry named `session_handoff`.

### Durable project handoff lifecycle — tool arguments

#### `continuity.acknowledge_handoff`

Compare-and-set acknowledgment for an exact successor and handoff.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `adapter_id` | optional | string |
| `handoff_id` | required | string |
| `operation_id` | required | string |
| `project_id` | required | string |
| `successor_session_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 62. Exact schema: JSON companion `tools` entry named `continuity.acknowledge_handoff`.

#### `continuity.checkpoint`

Persist a compact project checkpoint and rollover operation.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `active_files` | optional | array; maxItems=128; items: string |
| `adapter_id` | optional | string |
| `branch` | optional | string |
| `commit` | optional | string |
| `constraints` | optional | array; maxItems=128; items: string |
| `context_budget_source` | optional | string |
| `decisions` | optional | array; maxItems=128; items: string |
| `dirty_summary` | optional | array; maxItems=128; items: string |
| `evidence_ids` | optional | array; maxItems=128; items: string |
| `handoff_id` | optional | string |
| `idempotency_key` | optional | string |
| `memory_record_ids` | optional | array; maxItems=128; items: string |
| `mission` | required | string |
| `model` | optional | string |
| `next_actions` | optional | array; maxItems=128; items: string |
| `open_gates` | optional | array; maxItems=128; items: string |
| `open_work` | optional | array; maxItems=128; items: string |
| `operation_id` | optional | string |
| `passed_gates` | optional | array; maxItems=128; items: string |
| `phase_id` | optional | string |
| `predecessor_session_id` | required | string |
| `project_id` | required | string |
| `provider_session_id` | optional | string |
| `remaining_budget_estimate` | optional | number |
| `repository_root` | optional | string |
| `summary` | optional | string |
| `work_item_id` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 63. Exact schema: JSON companion `tools` entry named `continuity.checkpoint`.

#### `continuity.get_pending_handoff`

Fetch the latest unsealed project handoff.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 64. Exact schema: JSON companion `tools` entry named `continuity.get_pending_handoff`.

#### `continuity.prepare_handoff`

Build and persist a canonical successor handoff.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `active_files` | optional | array; maxItems=128; items: string |
| `adapter_id` | optional | string |
| `branch` | optional | string |
| `commit` | optional | string |
| `constraints` | optional | array; maxItems=128; items: string |
| `context_budget_source` | optional | string |
| `decisions` | optional | array; maxItems=128; items: string |
| `dirty_summary` | optional | array; maxItems=128; items: string |
| `evidence_ids` | optional | array; maxItems=128; items: string |
| `handoff_id` | optional | string |
| `idempotency_key` | optional | string |
| `memory_record_ids` | optional | array; maxItems=128; items: string |
| `mission` | required | string |
| `model` | optional | string |
| `next_actions` | optional | array; maxItems=128; items: string |
| `open_gates` | optional | array; maxItems=128; items: string |
| `open_work` | optional | array; maxItems=128; items: string |
| `operation_id` | optional | string |
| `passed_gates` | optional | array; maxItems=128; items: string |
| `phase_id` | optional | string |
| `predecessor_session_id` | required | string |
| `project_id` | required | string |
| `provider_session_id` | optional | string |
| `remaining_budget_estimate` | optional | number |
| `repository_root` | optional | string |
| `summary` | optional | string |
| `work_item_id` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 65. Exact schema: JSON companion `tools` entry named `continuity.prepare_handoff`.

#### `continuity.request_rollover`

Prepare rollover; reports memory-only readiness unless a host adapter confirms creation.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `active_files` | optional | array; maxItems=128; items: string |
| `adapter_id` | optional | string |
| `branch` | optional | string |
| `commit` | optional | string |
| `constraints` | optional | array; maxItems=128; items: string |
| `context_budget_source` | optional | string |
| `decisions` | optional | array; maxItems=128; items: string |
| `dirty_summary` | optional | array; maxItems=128; items: string |
| `evidence_ids` | optional | array; maxItems=128; items: string |
| `handoff_id` | optional | string |
| `idempotency_key` | optional | string |
| `memory_record_ids` | optional | array; maxItems=128; items: string |
| `mission` | required | string |
| `model` | optional | string |
| `next_actions` | optional | array; maxItems=128; items: string |
| `open_gates` | optional | array; maxItems=128; items: string |
| `open_work` | optional | array; maxItems=128; items: string |
| `operation_id` | optional | string |
| `passed_gates` | optional | array; maxItems=128; items: string |
| `phase_id` | optional | string |
| `predecessor_session_id` | required | string |
| `project_id` | required | string |
| `provider_session_id` | optional | string |
| `remaining_budget_estimate` | optional | number |
| `repository_root` | optional | string |
| `summary` | optional | string |
| `work_item_id` | optional | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 66. Exact schema: JSON companion `tools` entry named `continuity.request_rollover`.

#### `continuity.resume`

Seal an acknowledged rollover and atomically select the successor.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `operation_id` | required | string |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 67. Exact schema: JSON companion `tools` entry named `continuity.resume`.

#### `continuity.status`

Report durable continuity state, retry metadata, and active session.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `project_id` | required | string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 68. Exact schema: JSON companion `tools` entry named `continuity.status`.

### SHA-256 capture chains and evidence integrity — tool arguments

#### `evidence_digest`

Hash authorized binary evidence and append a durable SHA256 capture chain; returns byte lengths and capture/head identities. This is an integrity record, not an identity signature.

**Access:** roles Primary, Fallback; catalog effect Write; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `paths` | required | array; maxItems=64; items: string |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 76. Exact schema: JSON companion `tools` entry named `evidence_digest`.

#### `evidence_log_read`

Read and verify the project evidence capture chain with bounded record paging. Retain head_digest independently to detect replacement of the complete chain.

**Access:** roles Primary, Fallback; catalog effect Read; bound project required; shell policy not required by catalog.

| Argument | Presence | Advertised type and constraints |
| --- | --- | --- |
| `limit` | optional | integer; minimum=1; maximum=16 |
| `offset` | optional | integer; minimum=0 |
| `verify` | optional | boolean |

**Top-level additional properties:** rejected.

Source: [McpToolCatalog.cpp](../src/Mcp/McpToolCatalog.cpp), `SourceDescriptors`, line 77. Exact schema: JSON companion `tools` entry named `evidence_log_read`.

## Evidence and maintenance

All 125 descriptors copied exactly from the semantic golden fixture and joined by name to current SourceDescriptors metadata; full conditional/nested schemas retained. Listing/role/authority/protocol and specialist sources inspected.

This document is an implementation inventory. It does not establish that every tool was executed in the current LM Studio conversation or that every external provider is configured. Read [1.3.29 release verification](validation/RELEASE-1.3.29.md) and [ComfyUI host qualification](COMFYUI_HOST_QUALIFICATION.md) for actual checks and remaining acceptance limits. The [1.3.28 qualification record](validation/HOST-CAPABILITIES-1.3.28.md) retains its historical artifact.

The authoritative descriptor fixture is [mcp-tools-semantic-golden.json](../tests/fixtures/Mcp/mcp-tools-semantic-golden.json). Its file SHA-256 is `bfaf8164382cbd1f35beb649d9ed2e41c6bb3c1dfc3f9b56040cf1af965d3015`. The separately canonicalized descriptor digest is `9e19a9e3f145166c4803c49d60f9f05c815acde28e00ecc15e4752a06bb75c9e`.

Maintain this pair when the catalog, roles, permissions, playbooks or service contracts change. Check exact descriptor equality, unique name coverage, source metadata, role membership, conditional/nested schemas and the validity of examples. Reference material: [user guide](USER-GUIDE.md), [host workflows](HOST-CAPABILITIES.md), [capability map](PARITY.md), [CMake/CTest](CMAKE-CTEST.md), [image provider](IMAGE-PROVIDER.md).
