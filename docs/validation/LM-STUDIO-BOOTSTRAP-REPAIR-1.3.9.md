# Installed bootstrap repair: Forge Conductor 1.3.9

On October 5, 2026, the authorized host repair installed and exercised Forge Conductor 1.3.9.0. The deployed product was built from clean source commit `a136ff7fb6b4fb9f130dc7f4b0e41ab2fd25538a`. This documentation update does not change that immutable artifact's provenance.

## Evidence and mechanisms

The real packaged desktop preference selected YWE, ID `2f9a1172-3309-4efb-a94a-ed2f56575438`, while all three LM Studio server registrations launched `D:\GitHub\Forge-Conductor-Windows-Edition`. The saved active policy and current queued package belonged to YWE. The Forge project instead contained a legacy package and no active policy. Those are separate project stores; the repair did not transfer policy between them.

`McpToolPackAdapter::Impl::workspaceContext` formerly read only version-2 package queue rows; legacy conversion happened in Manager's queue-listing path. The shared migration now makes stored legacy instructions visible during MCP bootstrap, using bounded reads/writes, stable row identities, durable empty-queue suppression, and explicit missing-content coverage. Persisted cursor/order/state is retained when deduplicating a migration write.

The old `fs_edit` tool schema advertised no arguments while its handler required `path`, `old`, and `new`. Historical audit records contained eight empty calls. The repaired schema advertises those fields; loaded-Qwen edit/readback passed. The evidence establishes the contract mismatch and successful repair, without claiming to reconstruct every earlier model decision.

Project binding now carries the selected registered ID and canonical root to Primary, Fallback, and CLU. Prepare reuses exact, uniquely registered aliases after native Read authorization and integrity checks. New-folder initialization retains its repository identity conflict guard. This resolves existing YWE preparation without rewriting its historical repository identity.

Specialist start/status catalog capabilities now require the project expected by the strict session service. Current/project continuity selection excludes an unrelated or pathless global packet from implicit recovery; explicit packet lookup remains available. Budget checkpoints retain authorized workspace and recent-file evidence. Missing goals in historical packets cannot be reconstructed.

## Build, package and installation

- `scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4`: 153/153, exit 0, 38.84 seconds after the final Prepare change.
- `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4`: exit 0, including native WinUI/App `ManagerConnection.cpp` compilation.
- `scripts/Run-Static-Gates.ps1`: all three gates passed.
- `scripts/validation/Test-PackagePersistenceContract.ps1`: passed, preserving the ordinary Forge AppData profile exclusion.
- `scripts/package.ps1 -DevelopmentSigning`: valid signed MSIX from clean product inputs and exact staging provenance.
- `scripts/alpha/Install-Engineering.ps1 -PreflightOnly`, then the installer: passed; 1.3.8.0 upgraded to 1.3.9.0 with the existing trusted development publisher. No new certificate trust was installed.

Distribution: `out/dist/release-1.3.9.0-20261005-165459`.

MSIX SHA-256: `c435bdd70a4c7102cbc87ac76bcac3225e5115793d81fc0b69a2cd0874a996ef`.

CLI SHA-256: `8df3503a2fa311a60201b481da2556816903161cd54da7649aea4ee6a9ce54ef`. The staged CLI, frozen model-probed copy and installed CLI match. App, Manager and SessionHost installed hashes also match staging.

The production deployment service committed revision `b10fd863-56af-48cf-a2c9-d6852ae33015`. All three registrations have the installed 1.3.9 command, `serve --project-id 2f9a1172-3309-4efb-a94a-ed2f56575438`, `cwd=D:\GitHub\YWE`, `FORGE_CONDUCTOR_HOME=C:\Users\james\AppData\Local\Forge Conductor`, and timeout 180000 ms. Native host activation confirmed exact hot synchronization with `restarted=false`. Foreign configuration semantics and foreign plugin-tree content were preserved.

## Runtime acceptance

The actual installed CLI passed 31 MCP calls in a disposable profile/project: filesystem operations, text search, PDF file creation, Git status, shell, legacy/project memory, specialist start/status/completion, checkpoint/handoff, and exact recovery by a fresh process. This did not mutate YWE project files.

Final frozen 1.3.9 API probes with loaded `qwen/qwen3.8-27b` passed model-generated filesystem edit/readback and YWE instruction/policy retrieval. The actual 58-tool catalog fingerprint matched the production deployment verifier. These probes preserved the native chat.

The product's supported native-chat controller then created a separate verification conversation, `Forge-Conductor-Windows-Edition/1791219582785.conversation.json`. Actual Qwen tool requests and matching successful results through `mcp/forge-conductor`, plus primary PID 35116's independent trace, confirm:

- `context_get` called once, followed by status, package read, policy index, policy document, and final status: six successful calls.
- Runtime 1.3.9, 58 tools and 10 agents, explicitly bound to YWE at `D:\GitHub\YWE`.
- One ready package, `YWE-M3-SOURCE-CLOSURE-001-r2-QWEN-FORGE`, revision `33c4fca4fc16fb9de2119c542421bc35f49c67fb8da4009d7532ed0303ec05aa`; returned instruction text complete at 1,808 bytes.
- Active/enforcing development policy from `A:/raven-forge-development-main`, revision `e5145d9b23b1f98bf61c83d0dd5cf32ea3428b84fdfddbb7ad6983c40d11af75`; `AGENTS.md` complete at 14,019 bytes. The saved policy reports 42 coverage gaps: 40 directory inventory entries and two opaque files.
- Auto Continuity enabled and unblocked; native visible-chat state `observing`. Availability remains false before a completed rollover.

The original `1791193442765.conversation.json` remains byte-identical to its pre-upgrade backup, SHA-256 `a3287c58f25435c3b26c49e3d014601c8db5d4706a87db87629be16aa93bc5ae`. Forge App/Manager and LM Studio remain open.

Detailed host evidence is retained in `C:\Users\james\Documents\ChatGPT\Forge Conductor Windows Edition`, including package/install logs, installed hashes, production deployment/activation JSON, model traces, the installed feature smoke, native conversation snapshots and primary trace results. A consistent SQLite/profile snapshot and original LM Studio configuration/conversation backup were taken before installation.

## Native index defect discovered during acceptance

Independent comparison found the native policy index was incomplete: the full primary result contained 67,664 characters, while the saved LM Studio text was exactly its first 50,000 characters followed by `... (truncated)` (50,015 total). That is not valid complete JSON. The installed LM Studio 0.4.25+1 `resources/app/.webpack/lib/mcpbridgeworker.js` helper hardcodes that per-text-result threshold without a configuration parameter. The full policy document and package text returned completely, but the index requires the subsequent 1.3.10 MCP projection repair. Successful tool status does not prove a complete index.

## Verification limits

These checks qualify the observed bootstrap, retrieval, binding, filesystem, specialist-session and packet-recovery failures. They do not establish every GUI action or physical context exhaustion. Native automatic rollover was not reserve-triggered again for this artifact, and in-flight native rollover phase recovery after idle-process eviction remains an in-process limitation. Unit/stdio packet recovery and historical native rollover evidence are distinct from that limitation.
