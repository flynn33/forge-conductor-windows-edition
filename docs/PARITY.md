# Windows Alpha capability and parity map

This map records current Windows source capability against the internal Alpha contract. Earlier Mac and P0–P6 inventories remain useful references, but R0–R7 own the remaining Windows delivery.

| Capability | Current Windows state | Alpha requirement | Owner |
|---|---|---|---|
| Native desktop shell | C++20/WinUI 3 host, Manager attachment, managed-run controls, native Rig visuals, typed detail summaries, isolated profile view state, all required destinations, native accessibility walkthroughs, registered WindowsApps Start/Manager identity, and owner-approved simulated lifecycle exist | Accepted for Internal Alpha; an actual second-machine lifecycle is optional follow-up | R3–R4, R6 |
| Manager lifecycle | Per-user native Manager and GUI attach/detach path exist | Manager continues owning ordinary runs and services after GUI close | R1, R6 |
| Provider integration | Real loopback model discovery and Responses transport passed against `qwen3-coder-30b`; endpoint/model/capacity/reserves persist | Typed ordinary run service, visible errors, live model validation | R1, R4, R6 |
| Managed inference and continuity | Response IDs, correlated tool bootstrap, usage and context-only automation fixtures exist | Ordinary turns, tool dispatch and usage feed the Manager-owned controller; productive live successor is proven in the R6 continuation | R1, R6 |
| Context policy | Count/time rollover behavior is removed | Context consumption alone triggers rollover; capacity/reserves stay configurable | R1, R4, R6 |
| Projects and memory | Native services, disposable two-project isolation, and registered durable-profile restart evidence exist | Preserve declared markers through the disposable upgrade/reinstall lifecycle | R3, R6 |
| MCP deployment and tools | Registered payload passed the 53-tool catalog, project isolation, filesystem, search, Git, shell, and memory effects | Preserve the foreign MCP entry through the disposable upgrade/reinstall lifecycle | R3, R6 |
| Operational telemetry | Manager-owned 250 ms CPU/RAM sampling, native per-logical CPU utilization/frequency, one-second PDH GPU-engine and physical-disk rates, five-second volumes/relevant processes, scoped DXGI memory, typed timestamps/freshness, bounded histories, and integrated WinUI instruments now exist. Exact 0.9.5 candidate idle/disconnect/reconnect rendering, UI Automation values, High Contrast, and repaired 150% text layout were inspected. | Complete registered-package acceptance | R2, R6 |
| Agents, Feed, Runtimes, Events, Diagnostics | Typed operational summaries and Manager-owned agent/session actions replace placeholder text; recoverable failures remain visible | Repeat representative actions through the registered package | R3, R6 |
| Settings and maintenance | Discoverable labeled settings, exact context controls, save/revert/readback, safe scoped/all-store reset, keyboard operation, High Contrast and enlarged text evidence exist | Repeat the confirmed reset through the registered package | R4, R6 |
| Signed installer | Stable 0.9.5.0 identity, complete x64 Release payload, embedded provenance/hashes, development signature, public certificate, install helper, machine-trusted registration/Start launch, and narrow durable-profile unvirtualization pass | Disposable 0.9.4→0.9.5 update/uninstall/reinstall and full marker preservation | R6 |
| Existing data | Ordinary first-Alpha launches use the durable `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile with a distinct Manager/IPC identity. A disposable schema-9 probe still proves non-mutating refusal. | Legacy schema-9/C008/C009 migration is owner-deferred, not implemented or passed; preserve the old store untouched. | Deferred B3 |

## Tool inventory

Reuse `.forge-codex/state/baseline/mcp-tool-baseline.json` and `p02-mcp-semantic-inventory.json` only as historical starting points. R3 compares the current production `tools/list` output and actual native handlers, then records project scope, primary/fallback exposure, availability and a representative real effect. A catalog name alone does not prove a useful tool.

Owner-directed removal of count/time-triggered run controls is intentional. Manager-owned continuity does not imply control of ordinary unowned LM Studio desktop chats. Optional browser dashboards, advanced analytics/connectors, additional architectures and public distribution infrastructure remain outside this internal Alpha; core native telemetry and settings are included.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
