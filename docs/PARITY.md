# Windows Alpha capability and parity map

This map records current Windows source capability against the internal Alpha contract. Earlier Mac and P0–P6 inventories remain useful references, but R0–R7 own the remaining Windows delivery.

| Capability | Current Windows state | Alpha requirement | Owner |
|---|---|---|---|
| Native desktop shell | C++20/WinUI 3 host, Manager attachment, navigation and Provider controls are merged | Complete every required destination with real data/actions | R2–R4 |
| Manager lifecycle | Per-user native Manager and GUI attach/detach path exist | Manager continues owning ordinary runs and services after GUI close | R1, R6 |
| Provider integration | Real loopback model discovery and Responses bootstrap transport exist; endpoint/model/capacity/reserves persist | Typed ordinary run service, visible errors, live model validation | R1, R4, R6 |
| Managed inference and continuity | Response IDs, correlated tool bootstrap, usage and context-only automation fixtures exist | Ordinary turns, tool dispatch and usage feed the Manager-owned controller; productive live successor is proven | R1, R6 |
| Context policy | Count/time rollover behavior is removed | Context consumption alone triggers rollover; capacity/reserves stay configurable | R1, R4, R6 |
| Projects and memory | Native services and disposable two-project isolation evidence exist | Full native registration/selection/memory workflow and installed persistence | R3, R6 |
| MCP deployment and tools | Existing services passed disposable MCP, file/search/Git/shell/memory effects and foreign-entry preservation | Useful native deploy/inspect/repair and tool actions; shell clean-profile default with opt-out | R3, R6 |
| Operational telemetry | Typed/backend telemetry foundations exist; required native visual experience is incomplete | Real CPU/RAM/process history, supported GPU state or reason, provider/context/continuity timelines and accessible values | R2, R6 |
| Agents, Feed, Runtimes, Events, Diagnostics | Navigation exists; several destinations still reuse generic or Rig content | Feature-specific data, scope and useful actions with recoverable failures | R3, R6 |
| Settings and maintenance | Provider settings persist; full Settings/reset surface is incomplete | Discoverable accessible settings, exact context controls, save/revert/readback and safe scoped/all-store reset | R4, R6 |
| Signed installer | Signed x64 engineering MSIX/ZIP creation exists | Stable complete candidate, provenance, trust/onboarding, Start launch, update/uninstall and data preservation | R5, R6 |
| Existing data | Source has central migrations C001–C007; prior evidence reports a preserved newer schema-9 owner store | Authentic compatibility or explicit non-mutating newer-store rejection; no fabricated migrations | R5, R6 |

## Tool inventory

Reuse `.forge-codex/state/baseline/mcp-tool-baseline.json` and `p02-mcp-semantic-inventory.json` only as historical starting points. R3 compares the current production `tools/list` output and actual native handlers, then records project scope, primary/fallback exposure, availability and a representative real effect. A catalog name alone does not prove a useful tool.

Owner-directed removal of count/time-triggered run controls is intentional. Manager-owned continuity does not imply control of ordinary unowned LM Studio desktop chats. Optional browser dashboards, advanced analytics/connectors, additional architectures and public distribution infrastructure remain outside this internal Alpha; core native telemetry and settings are included.

<!-- alpha-phase-review:start -->
Phase review: R0 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
