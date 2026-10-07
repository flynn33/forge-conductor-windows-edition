# Workspace guide

Forge Conductor opens to Workspace. There is no required guided wizard or Managed Run form.

1. Register/select the project folder and choose its provider profile.
2. Add instruction-package folders and arrange their execution order.
3. Choose a CLU development-policy folder; selection binds it immediately.
4. Set Auto Continuity for this project/provider and review readiness.
5. Use LM Studio chats with the existing Forge integrations.

## Connect LM Studio

**Install or repair all three plugins** remains one action for `forge-conductor`, `forge-conductor-fallback`, and `forge-conductor-clu`. Repair passes the currently selected registered project ID/root into all three roles and preserves foreign integrations and unknown fields. After activation, inspect `get_forge_status` and confirm the project ID/root, ordered packages, and policy source/revision match that selection. The process startup directory is not a substitute for this binding. The exact outer MCP deadline remains 180 seconds; Forge shell calls remain capped at 120 seconds. Fallback is independently selected and does not replay ambiguous timed-out mutations automatically.

Primary owns native chat rollover; all three integrations remain enabled in the verified successor. No fourth plugin, token store, or login step is introduced. The local model connection retains the existing unauthenticated same-host loopback contract.

## Where to look

Workspace contains project/package/policy/continuity readiness. Rig shows native observations. Continuity lists packets and provides delete-selection/clear. Activity shows operational/governance history. Settings keeps load/save/readback/revert/test/restart and selectable saved records with delete buttons.

Historical rollover qualification used reserve pressure while the primary worker stayed alive. The 1.3.11 checks verify native observing state and project-scoped pickup; no new rollover or physical-exhaustion run was performed. Interrupted UI handoff recovery remains process-local. See [Continuity](Continuity).

Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`; disposable verification uses another home. See [Product Surfaces](Product-Surfaces), [Projects & Instructions](Project-Instructions), and [Release 1.3.16](Release-1.3.16).

In 1.3.16, use Settings to select the owner's host/workspace filesystem mode, then inspect `host_capabilities` and the selected project in `get_forge_status`. Dedicated Office, web, desktop/image, worker and schedule contracts are in [Windows Workflow Capabilities](Windows-workflow-capabilities). Leave Manager running for future triggers, inspect actual worker/notification receipts, and explicitly authorize another attempt only after reviewing uncertain interrupted effects.
