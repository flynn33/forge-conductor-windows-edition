# Measured host workflow qualification for 1.3.19

Product **1.3.19**, Windows MSIX **1.3.19.0**, Release x64. Both native WinHTTP timeout diagnostics consistently report the selected receive budget; timeout, deadline and cancellation behavior remain unchanged. The managed-context fix is retained: recovered-context legacy bookkeeping and its continuity-state lease requirement apply only to legacy-protocol calls. Manager-owned recovery retains the recovered packet; client ownership and canonical request/receipt validation remain unconditional. Primary/Fallback retain **104 tools each**, all original 80 tools, ten specialist playbooks and CLU's five tools.

| Bound fact | Observed result |
|---|---|
| Immutable product commit | `f93b0cd76126720b9316a7d4dc143e6520c31972` |
| Immutable product tree | `815d7a31eed46d75e0ead2f6bcd832bfd9fea383` |
| Documentation input commit | `f93b0cd76126720b9316a7d4dc143e6520c31972`; this measured update is a later docs-only descendant |
| MSIX SHA-256 | `ca59499a1e11e1905b06087d9c78f449770065a47eeb2c10b7f30ecc8bec3047` |
| Payload-manifest SHA-256 | `3de9b393dbe495b77a25a72c5ff8962788a9f50d2fc1da9fe41afd6894283b82` |
| Full local tests | 162/162, exit 0, 74.37 seconds |
| Windows CI | [37666107046](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37666107046), 162/162, 126.04 seconds |
| Other source checks | Product All; No-Python, Native-stack and No-attribution; package-persistence contract |
| Direct native fixture | 10 cases, 63 MCP exchanges, 5 connector sessions; owned loopback provider |
| Original shell/authority fixture | 7 required cases against final binaries; no model inference |
| Current selected chat | 9 cases, 22/22 successful actual calls; 0 unsuccessful calls retained |
| Native connector provenance | 7 observed CLI births/paths/hashes; exact installed CLI and Primary traces |
| Blind recognition | 3 properties in actual sealed independent read-only image-analysis output |

All four installed executables matched staging and actual signed MSIX streams:

| Executable | SHA-256 |
|---|---|
| ForgeConductorApp.exe | `05a3b2fac0683238c785abe6df214dd371ea4099ea202e648a0b8c87539c5f7e` |
| forge-conductor.exe | `e73fec5aab5bf73ebc5168d05ce591cdf01fddb3e07cd8be5735ce96ab2bb435` |
| ForgeConductor.Manager.exe | `ab1a780293a97b9c3fc67bafe5586f49c9cfef3c8bffbb3615dfc3d4a255ab8a` |
| ForgeConductor.SessionHost.exe | `8d97567ec6514db32785442fd468ca37c34bab9616d7e2198e9494b92ab40e3e` |

The three baseline reviewer cases preserve infrastructure deadline failure at one second and completed text/tool reviews at the default 600-second provider budget. Their sealed receipts and paged reconnect output were verified. The separate extended review requested approximately 3,500 words and a 1,800-second provider budget; it completed in 877.656 seconds including polling, with 28,046 logical MCP UTF-8 bytes, 4,294 words, 10,940 input tokens and 13,534 output tokens. Output exceeded 20,000 bytes without whole-output truncation. Logical output is distinct from physical CRLF text-file bytes.

The existing selected conversation and its earlier message prefix were preserved. The owner explicitly selected Host access from Workspace access; all other owner configuration keys were verified unchanged. Current cases cover status, the previously denied same-file Host read, owned write/readback, exact native DOCX/XLSX/PPTX artifacts, desktop observation/capture, native PNG bytes and fresh blind `image_analyze`, an independent mutable worker, a separate read-only reviewer, and a completed then cancelled schedule. Three current run receipts retain their canonical seals; reviewers retain `gate_approved: false`. The last recorded generation ended with EOS. The 22 recorded generations total 4,108 generated tokens and 2036.828 seconds summed generation time, distinct from end-to-end time and independent reviewer usage.

The separate installed-model managed-context regression captured 2 actual native calls in order (`context_get`, `fs_read`) across 3 provider turns through an owned transparent loopback relay. Native continuation results returned `found: true`, the exact seeded packet and complete unseen UTF-8 fixture. The checkpoint projection remained byte-exact and the managed result contained no `context_budget_cleared` annotation. Fresh read-only execution completed with verified sealed evidence, 259 UTF-8 output bytes and exact output after reconnect. Validation re-read actual MCP/relay bytes, seed/fixture/native receipt and owned process cleanup; no model self-report substitutes for the native result. Forced legacy hard-loop state preservation remains the separately passing source unit regression, not a live-model claim.

The first dedicated 1.3.18 managed-context attempt remains unqualified. Its captured native recovery and file-read results matched the seed and fixture, but the final response indented the two narrative lines and failed the strict exact-text check. That original failure is retained; this qualification requires a distinct fresh passing proof and does not normalize its output to turn the earlier attempt into a pass.

A historical 1.3.17 image-analysis run completed but did not pass the required literal property check. Its sealed result and source image remain retained under their original 1.3.17 identities, together with the distinct passing 1.3.17 retry. Neither attempt substitutes for fresh 1.3.19 blind-image qualification.

Isolated catalog, direct native, original-shell and reviewer probes preserved live LM Studio MCP bytes and complete inventories of all three owned plugins. The private blind-fixture creator preserved the same routes and retained the existing production Manager. Installation preservation, subsequent route readback and the owner setting change have separate scope. Registration does not establish three simultaneous live connections.

The tag-triggered [signing workflow](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37679861714) failed at Require signing secrets: the required signing secrets were not available to the job. Its downstream build/test/package/upload steps were skipped. The separately qualified development-signed package is the release artifact.

The artifact uses the existing Forge Conductor Development certificate. Exact package/signature/source binding and all four payload hashes were verified; no private signing key is distributed. Native Office/PNG operations require neither Office nor Python. Workers and read-only reviewers each admit at most sixteen simultaneous runs; terminal sealed receipts remain available by ID. Raw output is bounded to 256 KiB and serialized receipts to 4 MiB. Schedules require Manager and explicit authorization after uncertain interrupted effects. Historical 1.3.17 checks retained blocked notification admission: the direct fixture recorded `host_capability_unavailable` with `notification_setting=disabled_for_user` and no submission receipt, and current-chat schedule results recorded the same unavailable capability without accepted-submission or confirmed-display flags. Those actual 1.3.17 receipts remain preserved under their original identities. The native workflow qualification does not claim Windows toast submission acceptance or banner display; existing Windows notification settings are preserved.

Qualification does not execute or approve the original project gate, establish a new live rollover or physical context exhaustion, exhaustive GUI/every-feature coverage, or cloud connector parity. Blind recognition uses the fresh independent read-only `image_analyze` route; stock preview display alone does not qualify pixel interpretation. Its inner provider image request body is not retained in the native run record and was not directly captured. Shell cwd admission is not an OS sandbox. Earlier 1.3.14–1.3.18 attempts retain their own results and identities and do not substitute for these final binaries.

Publication is pending; no published download is asserted.

[Release notes](../releases/1.3.19.md).
