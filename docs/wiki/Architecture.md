# Architecture

The native dependency direction remains Domain → Contracts → Application → Infrastructure/Persistence/MCP/Manager/Presentation/Hosts. WinUI issues typed commands to the Manager; existing services own project access, memory, settings, policy, deployment, and telemetry.

## Native chat continuity ownership

`McpServeCompositionRoot` constructs and starts `WindowsLMStudioChatContinuity` only for Primary. Authorized adapter calls report the actual workspace binding. `WindowsLMStudioConversationReader` reads the selected native conversation's ID, generation usage/capacity, active-tool state, enabled integrations, and correlated request/results. It reassembles complete canonical per-call fragment sequences before consuming large result evidence; incomplete or inconsistent sequences do not establish semantic success.

At reserve pressure the worker waits for completed-tool evidence, then `WindowsLMStudioChatControl::pauseAtToolBoundary` uses Windows UI Automation without interrupting an active Forge call. The loaded model saves a detailed `session_handoff`. The adapter publishes `continuity/project/<project-id>`; all three connector bootstrap paths can carry it.

The same product controller invokes New chat and Send, checks a fresh native successor ID and the exact persisted handed message, and retains the loaded model plus all three integrations. Native `context_get` and a following successful Forge result establish `resumed`. Fallback and CLU do not start competing chat creators. Sessions are LM Studio chats; no run-manager replacement, fourth plugin, credential store, or login step is added.

Historical Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Product interfaces

Workspace, Rig, Continuity, Activity, and Settings retain the existing native design. Package order and policy binding are project-scoped. App/Manager carry the selected registered project into all three deployment roles; implicit continuity pickup uses the current authorized project/client. `get_forge_status` discloses authoritative context; `instruction_package.read` exposes selected-package content; CLU notifications return findings to the model and findings UI/Activity.

Existing persistence migrations and stable MSIX identity remain. Historical Manager-owned Responses run records describe earlier versions, not the current native chat handoff guarantee.

## Dedicated host workflows in 1.3.26

The current source catalog contains 112 Primary/Fallback tools. Native web, Office and desktop/image services reuse operation contexts, project authority issuers and atomic storage. Office writers complete OOXML before publication; PNG drawing uses Windows codecs. Production adds no Office/Python runtime. Owner-selected host/workspace modes retain the registered project as the relative-path default. Integrated 1.3.26 source, package, installed Manager/LM Studio and native-model qualification remain pending. Historical [published 1.3.21 qualification](Release-1.3.21) identifies its own source and binaries. See [1.3.26 source notes](Release-1.3.26).

Native CMake/CTest jobs reuse the shell-service owner and typed durable job receipts. An explicitly initialized build tree is authorized before launch; sequential build/test/report phases share one deadline. Status retains actual nullable phase outcomes and validated JUnit counts with bounded UTF-8 failure paging. Report path/length/SHA-256 checks detect changes against the receipt; they do not authenticate producer text. Crash recovery preserves unknown outcomes without automatic replay. Existing named log/wait/kill routes remain. See [CMake/CTest source guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/CMAKE-CTEST.md).

Native shell and executable jobs explicitly supply bounded `SystemDrive`, `ProgramFiles`, `ProgramFiles(x86)` and `ProgramData` defaults from the Windows host. Case-insensitive explicit caller overrides remain authoritative; absent or oversized defaults are omitted. `SystemDrive` supports Windows/.NET known-folder resolution used by MSBuild; arbitrary host environment variables remain excluded.

Accessibility paging observes a fresh tree for each call and retains zero-based offsets, row indices, 32 KiB text and 64 KiB native JSON bounds. Image previews optionally request 128–2,048 pixels, default 256, and adaptively downscale to the 512 KiB base64 bound with actual dimension/reduction metadata. Filled rectangles honor exact requested pixel width/height, including one-pixel shapes; ellipse behavior remains. Higher resolution does not establish exact OCR or change LM Studio's stock metadata-only image result.

Independent workers use a separate managed-run receipt store with fresh provider histories, frozen scope and bounded total lifetimes. The Manager-owned scheduler persists unique admission before inference, intersects frozen/current owner policy and prevents overlap of the same owned task. It restores future triggers and blocks uncertain-effect replay. The notification callback persists actual submission receipts/errors; SDK Windows toast resolves the installed package identity/root and leaves display unconfirmed. These services retain existing native chat, specialist, reviewer, process, memory, policy and continuity contracts.

## Optional image-provider ownership

The Manager composition owns `WindowsImageProviderService`, native WinHTTP transport and private `image-jobs` receipts, and shuts down local image workers before service destruction. All six MCP routes use the durable broker; authenticated dispatch intersects the internal caller envelope with current project roots/grants/denials. The provider is optional and disabled by default. Exact prompt/workflow identity is sealed before one generation POST; reconstructed read-only status observes without publication, while explicit fresh-authority resume retrieves existing history without generation/upload replay. Native WIC compositing retains exact original RGBA under red-zero masks. Provider effects, file publication and actual model quality are distinct observations. See [local image-provider guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md).

## 1.3.26 observer lifetime

The 1.3.26 candidate uses `ManagerVisibleChatContinuity` as an `IManagerTransitionWorker`. `ManagerProcessWorkerGroup` starts it after controller initialization and joins it before memory, configuration and repository services are destroyed. Internal `visible_chat.observe` and `visible_chat.status` use the existing authenticated same-Windows-user nonce pipe and 2 MiB frame bound; JSON object bodies are limited to 1 MiB with closed-field, duplicate-field and depth checks. The Primary CLI callback carries the authorized project/root, tool name, success and adapter canonical result before client-local guard annotations. Fallback and CLU do not submit competing observations. Route validation uses the verified sibling CLI from `ManagerProcessEnvironment`, rather than treating Manager.exe as the deployed MCP executable. The public catalog, schemas and 112/112/5 source inventory are unchanged.

Manager lifetime does not grant saved authority or confirm an uncertain UI effect. Current project/provider/route validation, the OS-held single-writer lease and selected-native evidence still gate control dispatch. An empty or unrelated third chat does not establish the successor. Explicit route migration requires a fresh authorized Primary callback, matching actual native packet/recovery evidence and exact preservation of the old encrypted checkpoint. Uncertain New chat or Send is reconciled without automatic replay. Completion requires the actual handed message, successor `context_get` and a following successful Forge tool result. Installed automatic delivery remains unqualified.
