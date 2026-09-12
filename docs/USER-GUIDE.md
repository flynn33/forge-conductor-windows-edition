# Operator guide — Alpha workflow
This is the intended Alpha workflow. Update screenshots, actual labels and artifact links after implementation;
do not publish this draft as evidence that the current application already performs every step.

## Start a project
Install and launch from Start. Connect/start the manager, choose a project folder and give it a display name.
The application retains a stable project identity; reopening or relinking its folder must not silently create
another project's memory. The source folder is never deleted by a memory reset.

For the internal 0.9.1.0 candidate, verify the package and certificate hashes in `distribution.json`. In Administrator PowerShell, run the bundled `Install-Engineering.ps1 -TrustDevelopmentPublisher` only after reviewing the named development publisher. The helper verifies identity, signer, version, and Windows registration; it never distributes a private key. A Windows `0x800B0109` result means the publisher is not yet trusted at the required machine scope.

## Two LM Studio modes
**Desktop MCP mode:** use LM Studio MCP -> Deploy to register the installed Forge primary/fallback stdio servers.
The application preserves unrelated MCP entries and verifies the actual installed executable. Use the tools from
LM Studio normally. Connecting a desktop chat does not itself activate manager-owned automatic rollover.

**Forge-managed mode:** configure the provider endpoint/model, test the connection, select the project and start a task
in Autonomy. The manager makes the provider requests and executes the tools, so it can measure context and carry the
task into a fresh session automatically. Closing the GUI leaves that manager-owned task running; reopen to attach.

## Context and continuity
Settings lets you select the effective model context capacity. The UI shows used context, measurement source and
reserved space for completion/handoff. Automatic handoff starts before physical overflow. It is not based on elapsed
time or how many tools were used. A transition is complete only when the successor has read/acknowledged the saved
handoff and resumed useful work. Errors show the retained handoff and a retry/reconcile action.

## Rig and operational telemetry

Rig refreshes from the Manager every two seconds. It shows Manager, provider, continuity, and store health; current
CPU, RAM, GPU capability, and retained context; recent CPU/RAM and activity-latency history; and a readable activity
timeline. The text next to each visual is the accessible authoritative value. Unavailable or stale measurements show
a reason rather than zero. Projects, Tools, Feed, Runtimes, Provider, Events & Evidence, Diagnostics, Manager, and
Settings reuse the same snapshot for their current detail summary. The last selected page is restored on next launch.

## Tools and shell
Tools shows actual availability. Clean installs allow the native shell tool; an explicit opt-out is preserved.
PowerShell is the native Windows shell route. Missing optional command-line tools should not prevent the whole app
from starting. Diagnostics identifies them and the appropriate recovery. Do not label Windows PowerShell semantics
as identical to macOS bash; document differing command syntax and exit/output behavior.

## Memory maintenance
Select the exact project on Projects, then open Settings and choose memory, continuity, or combined project data.
Type the confirmation text shown for that project ID and choose **Run confirmed reset**. The Manager reports affected
projects, records, links, and events and closes the old repository generation. **All registered project data** uses the
separate `RESET ALL PROJECT DATA` confirmation. These operations do not delete source repositories or silently remove
provider settings or foreign MCP servers. Run them on disposable projects during acceptance.

## Settings
Use **Load effective settings** before editing. **Save and read back** validates and persists through the Manager,
then reloads the effective values. **Revert pending edits** restores the last readback. Provider model text may stay
blank for automatic selection of the first loaded LM Studio model; **Test LM Studio** reports the discovered model.
Shell-backed tools, logging, telemetry refresh, Manager watchdog/restart/startup behavior, session retention, and the
context threshold/reserves are all available here. Each context slider has an adjacent exact token input.

## Troubleshooting
For a disconnected manager, use reconnect/start and inspect Diagnostics. For a provider failure, save settings first,
then test the real endpoint and loaded model. For a continuity failure, inspect the handoff and last error rather than
starting a duplicate writer. For an installer failure, retain the deployment error and package/certificate identity.
Use Events & Evidence to open relevant local logs/results. Report the app version, action, error and reproduction steps;
never include an access token or signing private key in an issue.

<!-- alpha-phase-review:start -->
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
