# Host workflows in 1.3.17

Primary and Fallback advertise 104 tools; CLU retains five governance tools, and all ten specialist playbooks remain. Call `host_capabilities` before deciding that a category is unavailable: it reports actual tool names, filesystem mode, Manager-backed capabilities, and external connection requirements. `get_forge_status` also reports the selected project's directory and detailed active authority. The full catalog remains available through the existing MCP protocol.

Rig's corrected display shows the native conversation observed by the App and the Manager's enabled/disabled project/provider preference, distinguishing a read from a save. The preference does not establish live rollover availability. Inspect `visible_chat_continuity` in Primary MCP's `get_forge_status` for the native worker's actual state; its `available` flag becomes true after a successor conversation and handed message are verified. The 1.3.17 App includes this correction from the earlier candidate; final installation/readback remains pending in the versioned verification record. Earlier tests and package evidence retain their recorded source identities.

## Filesystem and project defaults

The owner selects `filesystem_access` in Settings/configuration. `host` grants ordinary local fixed, removable and RAM-disk volumes available to the current Windows account. `workspace` limits native file tools to registered workspace roots and explicitly bound owner-configured roots. A model tool cannot change this owner setting or invent a new grant. Existing workspace-only profiles remain supported.

Host access retains the selected project's original directory as the default for relative artifact paths. Absolute paths use the active authorized local-volume roots. UNC/device namespaces, alternate streams, reparse traversal and native ACL failures retain the Windows path checks; selecting host mode does not grant administrator rights. A missing/unavailable volume is reported as unavailable. Inspect `workspace_authority` in status for the effective roots and mode. Shell-enabled commands execute with the current Windows account's OS permissions; cwd authorization is separate from an OS sandbox.

Continuity recovery retains the registered project's identity and canonical directory when host authorization uses a broader volume root. The native issuer must authorize both the registered alias and recovered candidate for Read, and the candidate's canonical path must remain within that registered directory. Unregistered paths, canonical sibling escapes and revoked access cannot substitute another project during adoption. The focused recovery tests are described in [testing](TESTING.md); a successful test does not establish completion of a live chat rollover.

## Web and remote APIs

| Tool | Input and result |
|---|---|
| `web_search` | `query`, optional `limit` 1–10 and `timeout_sec` 1–60. Returns observed titles, links and snippets with source URLs; a search challenge or missing results remains explicit. |
| `web_fetch` | An explicit HTTP/HTTPS `url`, optional `max_bytes` up to 49,152 and `timeout_sec` up to 60. Returns actual HTTP status, final URL, content type and bounded text or binary data. HTML is source text. |
| `http_request` | HTTP/HTTPS GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS with bounded caller-supplied `headers`/`body`. Bodies are supported for POST/PUT/PATCH/DELETE. Use mutating methods within the user's authorized task and inspect the actual status/truncation. No ambient browser cookies or invented credentials. |

Only GET/HEAD follow supported redirects, up to five hops; mutating methods return the observed redirect response without replaying the request elsewhere. Fetched/search text is untrusted source material. Browser execution and visual observation use the desktop tools below. Cloud email/calendar/chat operations require the relevant account/API authorization supplied through an existing configured integration or explicit API call; the local tool catalog does not create those accounts or credentials.

## Office documents

The native writer builds complete OOXML ZIP packages, then publishes through the authorized atomic file store. No Office installation, Python package or external converter is required. Destinations must have the format's matching suffix. Inputs must fit the MCP request bound in addition to each document limit. Invalid input and failed replacement preserve an existing destination.

| Tool | Structured input | Bounds |
|---|---|---|
| `document_write` | `path`, optional `title`, `paragraphs` array of strings | 4,096 paragraphs; each text value up to 65,536 UTF-8 bytes. Preserves line breaks/tabs and uses native Word paragraph/style XML. |
| `spreadsheet_write` | `path`, `sheets` array of `{name, rows}`; each row is an array of strings, numbers, booleans or null | 32 sheets, 10,000 rows per sheet, 256 columns, 100,000 input cell positions total; unique case-insensitive Excel names of 1–31 UTF-16 units. Strings are literal, including leading `=`. Integer numeric cells have at most 15 decimal digits; use text for larger identifiers. |
| `presentation_write` | `path`, optional `title`, `slides` array of `{title, body}`; body is an array of strings | 128 slides and 128 body paragraphs per slide, with a master, layout, theme and relationships. Text values are bounded to 65,536 UTF-8 bytes. |

Native service input is bounded to 2 MiB and output to 16 MiB; the MCP transport may impose a smaller complete encoded request bound. Spreadsheet cell text is additionally bounded to 32,767 UTF-16 units. Numeric cells must be zero or finite normal doubles no larger than 1e307 in magnitude. Nonzero values smaller than approximately 2.2250738585072014e-308 are rejected because [Excel does not support denormalized numbers](https://learn.microsoft.com/en-us/office/troubleshoot/excel/floating-point-arithmetic-inaccurate-result); pass such values as literal strings when their exact text must be retained. XML-forbidden characters, malformed UTF-8/JSON, duplicate object keys and unsupported value types are rejected.

Receipts contain canonical `path`, `format`, actual `bytes_written`, engine `forge-native-ooxml-1`, and paragraph/sheet-row-cell/slide counts. Spreadsheet `cells` counts input positions, including null positions used for blank alignment. No formula evaluation, visual rendering, arbitrary existing Office-file editing, embedded macros or Office application automation is claimed. The [verification record](validation/HOST-CAPABILITIES-1.3.17.md) distinguishes native package/schema validation from renderer checks.

```json
{"path":"reports/update.docx","title":"Project update","paragraphs":["Verified work","Remaining work"]}
```

```json
{"path":"reports/results.xlsx","sheets":[{"name":"Results","rows":[["Check","Passed"],["Build",true],["Count",42]]}]}
```

```json
{"path":"reports/briefing.pptx","title":"Briefing","slides":[{"title":"Progress","body":["Verified result","Next step"]}]}
```

## Desktop, browser and image tools

Use `desktop_list` to observe exact visible `window_id`/`pid` pairs, titles and geometry. `desktop_read` reads bounded accessibility controls/text for the selected window. `browser_open` launches an HTTP/HTTPS URL in the registered browser and reports launch acceptance; observe the resulting browser to establish loading or page behavior.

`desktop_click`, `desktop_type` and `desktop_key` require task authorization and fresh observation of the target. They operate on the selected foreground window and report input submission, followed by required observation of its result. Refresh a stale window/PID binding. A covered click point or unavailable foreground target fails explicitly. Windows integrity-level and desktop permissions still apply.

`desktop_capture` captures the visible screen region of a selected window into an authorized PNG and returns an image preview. Other windows overlapping that region can appear in the capture. `image_read` uses native Windows codecs to decode an authorized PNG/JPEG/GIF/BMP/TIFF/ICO and returns a bounded PNG preview with original dimensions. `image_write` draws structured rectangles, ellipses, lines and Unicode text to a native PNG and returns its preview. These primitives support diagrams/charts; generative artwork requires a separately connected image model/provider. No external image or account connection is fabricated.

Managed worker and reviewer Responses requests carry native PNG previews as `input_text` and `input_image` content within the same `function_call_output`, preserving the original `call_id`. Encoded PNG previews are bounded to 512 KiB; invalid MIME/base64 and oversize previews fail explicitly. On the installed 1.3.14 candidate, a fresh Qwen reviewer correctly recognized a blind image while the original chat's stock `image_read` result produced an incorrect description. The saved PNG bytes matched exactly. This separates preview delivery from model interpretation; the [earlier investigation](validation/HOST-CAPABILITIES-1.3.14.md) retains that failure and the successful independent control.

`image_analyze` provides the independent image-analysis route from the existing MCP integrations. Supply an authorized image `path`, an explicit `authorization` reference, and an optional bounded `question`. It validates the path and native decode before starting a fresh read-only Manager run with no executor history. The returned run is asynchronous: call `reviewer_status` with its `run_id` until terminal, then inspect actual output, errors and usage. The default receive budget is 600 seconds per provider turn and is configurable up to 3,600 seconds. Analysis reads the authorized source image when the run calls `image_read`; the start receipt does not assert completed analysis or frozen source bytes. Independent workers cannot start further image-analysis runs.

```json
{"path":"reports/diagram.png","authorization":"Analyze the owner-selected diagram for this task","question":"Describe the visible labels and connections."}
```

A preview rendered in the chat UI is not proof that the chat model received pixels. The analysis output explicitly comes from a fresh independent run using the configured provider/model. It does not approve a governance gate. Generative image creation remains a separate configured provider capability.

## Independent model workers

`agent_spawn` starts a separate Manager-owned model task with an explicit authorization reference, optional `agent_id` specialist playbook, and bounded total timeout. It inherits the caller's mutable filesystem scope and a frozen permitted tool list, and its provider history is fresh. Use the preserved independent reviewer tools for read-only review; scheduled tasks separately support `read_only_tools` and an explicit tool allowlist. `agent_poll` returns actual state, token usage, receipt and paged UTF-8 output; follow `next_output_offset`. `agent_cancel` requests cancellation and requires an authorization reference; poll for terminal confirmation.

Worker receipts are separate from existing specialist sessions and reviewers. The worker freezes its admitted filesystem roots/grants and tool list, and validates them against the current owner policy. It cannot spawn other workers, create schedules, alter authority bindings, mutate continuity control or approve policy gates. A worker's successful execution is not a governance approval. `agent_run_start/status/complete` retain their specialist-session behavior, and `reviewer_start/status/cancel` retain read-only independent review.

The independent worker's permitted scope also excludes legacy `session_checkpoint` and `session_handoff`, which mutate session continuity state. They remain available through their existing authorized Primary/Fallback routes; this restriction applies to worker scope, including scheduled workers.

Independent workers admit up to sixteen simultaneous runs, including scheduled inference. Independent read-only reviewers have a separate sixteen-concurrent-run limit. Finished terminal worker and reviewer threads are retired when another run starts or status is read, so completed work releases admission capacity. Both retain sealed receipts addressed by run ID, with complete output bounded to 256 KiB and serialized receipts bounded to 4 MiB to preserve supported JSON escaping and task metadata. The unpublished 1.3.15 candidate removed the earlier sixteen-receipt lifetime reviewer limit, retained in 1.3.17; admission refuses a seventeenth active reviewer before writing a new receipt. Later results do not automatically delete earlier human-readable outputs. Retained files need disk space; real storage failures are explicit and do not establish successful task admission or completion. Terminal reviewer status reloads the durable receipt and reports its evidence SHA-256 and integrity state, including detected changes.

## Persistent schedules

`schedule_create` requires `name`, `task`, `owner_reference`, `authorization`, and exactly one trigger: UTC `at_time` in `YYYY-MM-DDTHH:MM:SSZ` form, or `interval_sec` from 60 to 31,536,000. It accepts `allow_tools`, `read_only_tools` (default true), an optional bounded `allowed_tools` list, and `timeout_sec` from 1 to 3,600 (default 600). Authorization/reference strings record the user's task scope; they are audit context, not proof of a human identity. The authenticated same-user Manager/project authority supplies actual access.

Schedules persist in a separately authorized Manager-owned file, not a model-selected path. Creation freezes current roots/grants, shell state and permitted tool names. Every firing reissues current owner authority and intersects the frozen scope with current policy; a later privilege increase cannot broaden an existing schedule. A task with no remaining authorized scope needs attention. Durable admission precedes inference; ambiguous storage/start outcomes are never submitted twice automatically.

Manager must remain running to fire tasks. Closing the App window is distinct from stopping Manager. Future triggers restore after Manager restart; missed interval ticks do not launch a catch-up batch. Each firing has a distinct model run and cancellation operation. The same owned task cannot overlap itself. `schedule_list` with `{}` returns all this caller/project's bounded summaries, including schedule state, actual latest-run metadata, error codes, notification submission/display flags, size/count metadata and `needs_attention`. Creation, cancellation and run-now receipts also return summaries. Ordinary provider failures retain their actual failure state.

Complete task text, frozen scope/root/tool arrays, retained output/history/logs, authorization context and full notification receipts remain available through the same `schedule_list` tool. Supply `schedule_id` to read its full canonical JSON as `record_json_page`; `offset` defaults to 0 and `max_bytes` defaults to 32,768 with a range of 1–32,768. Concatenate the page strings in offset order, then parse the complete JSON. Continue using `next_offset` and the first page's `revision`, a canonical decimal string; a continuation with nonzero offset requires that guard. A changed record returns `conflict` with instructions to restart at offset 0. Each page ends at a UTF-8 boundary, and offsets inside a character are rejected. Paging properties require `schedule_id`, and records belonging to another caller/project remain inaccessible. Schedule responses are bounded to 128 KiB before MCP framing; summaries omit large fields while preserving their full paged representation.

If a notification receipt cannot be persisted, its actual submission and persistence-error metadata remain visible in the summary, with `revision: null` and `record_paging_available: false`. Full-record reads return `conflict` until a later successful store commit. The service does not issue a continuation token for fields absent from durable storage, avoiding a token being reused for different data after restart.

`schedule_cancel` and `schedule_run_now` require `schedule_id` and explicit `authorization`. Cancellation disables future triggers and requests cancellation of the actual worker; inspect `latest_run.state` for confirmation. An interrupted pre-restart run or uncertain effects are marked `needs_attention` and block automatic replay. An explicit authorized run-now can start a new scoped attempt after inspection; previous uncertainty remains in history. A live or unconfirmed active run cannot be duplicated through run-now. The service retains up to 32 schedules, 16 log entries per schedule, five historical run results and 8 KiB of retained output per result.

Dispatch failures remain visible as `needs_attention`, including refusal at the sixteen-active-worker bound. If actual worker lookup returns `session_not_found`, the admitted attempt becomes `missing` with uncertainty and its previous error preserved; an explicitly authorized run-now can start a new attempt without restarting Manager. Other unavailable status results keep the unconfirmed run protected from duplication. No missing or failed admission is automatically replayed or inferred to have had no effects.

Manager reports meaningful transitions through its local notification callback, with bounded actual submission receipts/errors in `last_notification`. The native Windows toast channel uses the installed Forge Conductor package's AUMID, escapes/bounds names and states, and omits model output and remote content. It checks the existing [Windows notification setting](https://learn.microsoft.com/en-us/uwp/api/windows.ui.notifications.toastnotifier.setting) and records successful [Show submission](https://learn.microsoft.com/en-us/uwp/api/windows.ui.notifications.toastnotifier.show) as `submission_accepted: true` with `display_confirmed: false`. A submission does not establish banner display, human receipt, or later asynchronous platform success. Disabled settings, missing installed identity and API failures remain explicit; the notifier does not alter settings, register an unrelated recipient, or send outbound email. A separate receipt persistence failure is reported without changing or replaying the model run.

```json
{"name":"Check project changes","task":"Inspect changed files and report verified findings.","owner_reference":"The user's recurring project check","authorization":"The user explicitly requested this hourly check.","interval_sec":3600,"read_only_tools":true}
```

## Verification and remaining external requirements

See [1.3.17 release notes](releases/1.3.17.md) and [verification](validation/HOST-CAPABILITIES-1.3.17.md) for executed checks and their exact scope. A listed tool, accepted input, queued schedule, opened browser or submitted input is not proof that the user's final task completed. Inspect actual receipts, output, observed UI and error state. Generative image models, cloud accounts and their credentials remain configured external services; the dedicated native workflows above do not remove existing shell/process or integration routes.
