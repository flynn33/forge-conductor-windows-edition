# Architecture

The native dependency direction remains Domain → Contracts → Application → Infrastructure/Persistence/MCP/Manager/Presentation/Hosts. WinUI issues typed commands to the Manager; existing services own project access, memory, settings, policy, deployment, and telemetry.

## Native chat continuity ownership

`McpServeCompositionRoot` constructs and starts `WindowsLMStudioChatContinuity` only for Primary. Authorized adapter calls report the actual workspace binding. `WindowsLMStudioConversationReader` reads the selected native conversation's ID, generation usage/capacity, active-tool state, enabled integrations, and correlated request/results. It reassembles complete canonical per-call fragment sequences before consuming large result evidence; incomplete or inconsistent sequences do not establish semantic success.

At reserve pressure the worker waits for completed-tool evidence, then `WindowsLMStudioChatControl::pauseAtToolBoundary` uses Windows UI Automation without interrupting an active Forge call. The loaded model saves a detailed `session_handoff`. The adapter publishes `continuity/project/<project-id>`; all three connector bootstrap paths can carry it.

The same product controller invokes New chat and Send, checks a fresh native successor ID and the exact persisted handed message, and retains the loaded model plus all three integrations. Native `context_get` and a following successful Forge result establish `resumed`. Fallback and CLU do not start competing chat creators. Sessions are LM Studio chats; no run-manager replacement, fourth plugin, credential store, or login step is added.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Product interfaces

Workspace, Rig, Continuity, Activity, and Settings retain the existing native design. Package order and policy binding are project-scoped. App/Manager carry the selected registered project into all three deployment roles; implicit continuity pickup uses the current authorized project/client. `get_forge_status` discloses authoritative context; `instruction_package.read` exposes selected-package content; CLU notifications return findings to the model and findings UI/Activity.

Existing persistence migrations and stable MSIX identity remain. Historical Manager-owned Responses run records describe earlier versions, not the current native chat handoff guarantee.

## Dedicated host workflows in 1.3.17

Native web, Office and desktop/image services reuse operation contexts, project authority issuers and atomic storage. Office writers build complete OOXML packages before publication; PNG drawing uses Windows primitives/codecs. Production adds no Office/Python runtime. Owner-selected host/workspace filesystem modes retain the registered project as the relative-path default.

Independent workers use a separate managed-run receipt store with fresh provider histories, frozen scope and bounded total lifetimes. The Manager-owned scheduler persists unique admission before inference, intersects frozen/current owner policy and prevents overlap of the same owned task. It restores future triggers and blocks uncertain-effect replay. The notification callback persists actual submission receipts/errors; SDK Windows toast resolves the installed package identity/root and leaves display unconfirmed. These services retain existing native chat, specialist, reviewer, process, memory, policy and continuity contracts.
