# Changelog

## 1.3.28 — Continuity recovery and current connector presence (source candidate; qualification pending)

The **1.3.28 source candidate / Windows package 1.3.28.0** retains 112 Primary/Fallback tool names, five CLU tools, ten specialist playbooks and every existing feature. Automatic handoff and repair instructions ask the model to send only the outer `packet_json` string argument, with all packet fields inside its encoded JSON object. The same instructions ask the model to record each fact once and use an advisory output target only when every constraint, exact value, task fact and path fits. Direct-field calls and conflicting-field refusal remain supported. The only catalog amendment is the `session_handoff` packet_json description; every other descriptor field remains exact. Manager ownership, acknowledgement resets, guarded legacy recovery, cached-pressure evidence and the separate 25-second dispatch context are retained. The source now supports explicit recovery from the exhausted, acknowledged three-repair waiting state without a saved packet. A fresh native Primary session_handoff must save a complete model-authored packet with a new sequence and the current project pointer; an exact context_get result and a later successful Primary tool result authorize recovery in the selected successor. The original encrypted checkpoint is archived exactly, historical acknowledgements are retained, and this recovery issues no automatic New chat or Send replay. Installed native verification of this branch remains open. Final clean source/build, signature/install, catalogs, installed native handoff, CI and publication require new evidence; no earlier 1.3.27.0 result qualifies these binaries.

The focused working-source build and three-target check passed: 3/3 targets in 147.40 seconds, 141 Infrastructure groups in 147.11 seconds, all 40 terminal-recovery cases, 1,042 catalog assertions and 2,090 adapter assertions. The terminal cases cover unchanged and changed routes, refusal of incomplete or mismatched native evidence, and sequence/pointer changes before publication. These are source regressions; the complete release graph, signed installation, actual Qwen recovery, automatic New/Send, exact-source CI and publication require separate current-artifact evidence.

The Manager presence projection now selects connectors within the existing 25-second heartbeat window before applying the collection bound. Expired connector records remain stored and the repository's default full-history read is retained. A current collection exceeding its bound still fails explicitly. This corrects the observed installed 1.3.27 runtime-inspection failure after 271 expired presence records accumulated.

The new regression first reproduced `limit_exceeded: The presence dashboard projection exceeds its bound.` against the previous implementation. After the correction, both affected native targets passed (2/2 in 3.80 seconds), including retained history, inclusive timestamp boundaries, future heartbeats, current overflow and cancellation. Fresh complete-source and installed-Manager verification remain pending.

## Superseded 1.3.27 installed investigation

The superseded, unpublished **1.3.27 / package 1.3.27.0** artifact used source `a78987745f6926b16f9bfdbfe10c263f8791c10e`, tree `57539f81f820eabc4661fe9bcb662b89b302c9c3`. Product All, three static gates and package persistence passed; its complete Release graph passed **167/167 in 173.57 seconds**. The signed installation matched all four executable images and **323 payload files**, preserving **8,623 profile files / 426,385,142 bytes** plus four protected snapshots before launch. Installed catalogs returned **112/112/5** with the then-current descriptors. Exact-source CI separately passed **167/167 in 173.22 seconds**. Original authority, fresh CLI and public image-provider checks retained their separately sealed local scopes. One automatic rollover recorded native New, packet delivery-Send, full Primary packet recovery and a later successful status result; the context/status requests were recorded together, with their results ordered. Later in a different chat, a handoff call duplicated outer and encoded packet fields and received `InvalidRequest`; that native generation reached the context limit. The prior calls and failed attempts remain under their original source, package and binary identities. No 1.3.27 release was published.

## Continuity state and guarded legacy recovery

A new request cycle must begin with its own delivery and context-recovery acknowledgements cleared. The planned legacy recovery admission is limited to an inherited, undispatched request with no packet or effect, and requires fresh Primary handoff observations plus the actual selected native packet and fresh scope. Retained or uncertain effects, unrelated chats, weak packet content, changed authority and stale observations keep their existing refusal requirements. These are candidate changes until their narrow regression and actual installed automatic sequence are verified.

The following 1.3.26 entry preserves its initial candidate contract; the actual historical outcome is recorded after it.

## 1.3.26 - CI recovery and native pressure evidence (source candidate; qualification pending)

- The accessibility change retries `ElementFromHandle` once on exact `UIA_E_TIMEOUT`, rechecking the operation deadline/cancellation and selected visible window/PID. Input actions are not retried. The diagnostic test helper gives each invocation a fresh 30-second deadline while keeping deterministic UTC timestamps; a missing checkpoint retains its original bound and now reports the joined writer's actual result. Private controlled regressions passed, but the hosted CI's exact scheduling trigger and original writer result remain unknown. New integrated 1.3.26 tests and CI are pending.
- The pressure traces add `generation_evidence`, parsed from the reader's existing selected native generation evidence, to both `context_pressure_detected` and `context_pressure_pause`. Each record retains the actual message index, selected version, step index and full `genInfo`, including model, provider usage, loaded capacity and unknown native fields. Pause evidence comes from the fresh read after a confirmed pause. The separately cached `tokenCount` still has no generation timestamp and does not represent current KV usage. No pressure decision, public tool schema, checkpoint source contract, route authority or uncertain-effect replay rule is changed. Integrated checks are pending.
- Preserve all existing feature, effect, route and packet contracts. Integrated dispatch-context and final source/package/install/native/CI/release checks remain pending. See [candidate notes](docs/releases/1.3.26.md).

### Bounded dispatch context and guarded timing

The 1.3.26 candidate gives each continuity control dispatch a fresh **25-second** operation context, separate from the **20-second** observation tick, and uses that same dispatch context for receipt callbacks, fresh authority/configuration checks and checkpoint persistence. It preserves the cancellation token, revalidates the current authority before effects, and retains the existing controller's 25-second cap and queries. No deadline extension revives cancelled work, no permission or route check is relaxed, and uncertain New chat/Send remains unreplayed. Integrated source checks and actual installed automatic delivery remain pending.

Guarded timing used the same C570 controller and protected native inputs. With the original **20-second** caller budget, Send preparation returned `deadline_exceeded` after **20,375.0247 ms**, before the receipt callback. With a fresh **25-second** caller budget, it reached `BeforeDispatch` after **20,363.4627 ms** and the private callback deliberately refused dispatch with `conflict` at **20,367.1673 ms**, leaving **4,632.8327 ms**. Both captures preserved the protected native inputs; neither invoked Send or New chat. This demonstrates the caller-budget boundary on this measured path, not completed native submission or a general latency guarantee.

### Actual historical 1.3.26 outcome — unpublished

The installed, unpublished **1.3.26 / Windows package 1.3.26.0** artifact at source `d0da197db770f26122c82e38435610caf130b10c`, tree `698e1af6e5d6881c2a97f75df81f5bd6e1a280c3`, passed Product All, all three static gates, package persistence and **167/167 local Release tests in 148.63 seconds**. Exact-source Windows CI run **37774964651** passed **167/167 in 143.93 seconds**, including the static gates and staged-product upload. Its development-signed installation matched all four executable images and **323 payload files**, preserving **8,589 profile files / 424,655,103 bytes** and all four protected snapshots before launch. Installed catalogs returned **112/112/5**, retaining every prior 106 descriptor. These checks do not establish automatic continuity or qualify 1.3.27. Original authority passed **7 cases / 63 requests**, fresh isolated CLI acceptance passed **8 cases / 38 requests**, and a separate fresh blind-fixture decoder passed **1 case / 4 requests**. Those isolated scopes made zero model-inference requests and their owned CLI/private Manager processes exited normally with code `0` while retaining protected project, configuration and routing evidence. Public MCP/Manager image-provider acceptance passed **7 cases / 46 requests** with four owned CLI sessions and two graceful private Manager exits, preserving owner routing/configuration evidence. It exercised retained status, explicit masking, running-job local cancellation and exact-job resume after Manager reconstruction without another generation POST. It did not qualify actual Qwen delivery, image-model quality or a real crash/network outage.

The selected Qwen recovery capture contained **24 messages**. Two actual Primary calls returned saved context and status reporting **1.3.26 / 112 tools**; the response reached `eosFound`. Manager remained `recovery_pending` at inherited checkpoint revision **14**: `WaitingPacket`, no packet or successor, no effect, and false packet-request acknowledgement, while delivery acknowledgement and context recovery remained true from the older cycle. The current source begins a new packet-request cycle without clearing those two flags. Fresh native readback did not satisfy the existing successor-and-packet recovery eligibility. Automatic New chat/Send and successor continuation were not established, and no 1.3.26 release was published. The subsequent 1.3.27 changes and their checks have separate inputs. The independently retained native process handles recorded all three predecessor connector CLIs and bridge processes exiting with code `1` while Manager PID `35288` remained alive. This is separate from the normal code `0` isolated processes and does not prove automatic handoff. Selected native five-group and native image-provider scopes were not qualified by these captures.

## 1.3.25 - Superseded installed investigation

The installed, unpublished **1.3.25 / 1.3.25.0** artifact at source `5708cb9c52ed2f2dd10b3a500b57c7ffff6c6f7b`, tree `fcd027b9bf82bc88673a3ee43069973c1600a831`, passed Product All, all three static gates, package persistence and **167/167 local Release tests in 146.24 seconds**. Its signed installation verified all four executable images and **323 payload files**, preserving **8,553 profile files / 422,592,105 bytes** and the four protected snapshots before launch. Exact-source Windows CI run 37763301666 failed **165/167 in 147.41 seconds** at DesktopArtifact accessibility connection and Infrastructure diagnostic rotation checks; the later CI static-gate/upload steps were skipped. Local success does not replace that failed CI result. Actual native assisted readback reported 1.3.25 and 112 tools; it did not establish all-tool runtime qualification or automatic New chat/Send. Full controller diagnosis remained open, and no 1.3.25 release was published. These observations do not qualify 1.3.26.

The captured installed automatic attempt preserved every field of the original **86 messages** in the **59,661,455-byte** native conversation. An **18,697-character** unsent draft exactly matched the traced automatic packet request. Checkpoint revision **14** retained `WaitingPacket`, `effect:null` and a false packet-request acknowledgement. The capture reported no confirmed request Send, no New chat dispatch and no new native tool dispatch for this attempt. It does not establish EOS, a saved new model packet, a successor or automatic delivery. The preserved failure receipt has SHA-256 `8075fd1adeb47c96709d644ac08927dba8074e904e2147537b185ac8c038f6fb`; the native file has SHA-256 `ced8d7316db1ea04bde709cb53cf8424afbb7140dccbac120fd95fa329463547`. This capture does not determine the deadline failure by itself; the later guarded timing comparison below diagnoses that boundary. No query optimization is integrated.

Private read-only comparisons retained their C570 source and copied-library identities. The earlier baseline idle calls measured **1,841.9938–3,157.5090 ms**; typed-cache searches measured **6,473.7840–7,603.1633 ms**, and reversed-condition searches measured **1,838.8690–3,247.9454 ms**. All calls returned idle without input actions or owner writes. Neither candidate supported an idle latency improvement, and neither is integrated. These sequential private comparisons are not a Send profile or automatic-rollover result; neither is a completed Send result; the later guarded comparison diagnoses the caller-budget boundary.

## 1.3.24 - Superseded installed investigation

The installed, unpublished **1.3.24 / 1.3.24.0** artifact at source `83d133df8aeb3801c6813be57d0dd81dfe56b6ea`, tree `3c38cb4390605c6f581debe7da9f27609daad3da`, passed Product All, all three static gates, package persistence and **167/167 Release tests in 96.38 seconds**. Exact-source Windows CI run 37745306780 passed **167/167 in 135.32 seconds**. Its development-signed installation matched all four executable images and all **323 payload files**, preserving **8,505 profile files / 414,180,807 bytes** and four protected snapshots before launch. Installed catalogs returned **112/112/5**, retaining every prior 106 descriptor. Original authority and fresh CLI checks passed **7 cases / 63 requests** and **8 cases / 38 requests**. Public image-provider acceptance passed **7 cases / 47 requests**; selected Qwen image-provider acceptance verified **16 actual pairs across all six image tools**, and native acceptance verified **8 calls across five groups**. These eleven completed scopes did not establish automatic continuity, and no 1.3.24 release was published. They do not qualify the 1.3.25 candidate.

The frozen active 86-message conversation stored a cached full rendered prompt count of **264,415**, with loaded capacity **262,144**, while its latest selected provider generation reported **130,969** total tokens (130,895 prompt plus 74 predicted) under `rollingWindow`. LM Studio's shipped writer counts the complete rendered selected history, system prompt and tool definitions into `tokenCount`; its overflow-retry path can retain a smaller history suffix. Forge 1.3.24 based pressure on latest provider usage and ignored that cached full-prompt projection. This source gap prevented the cached projection from triggering rollover. The individual backend overflow-error invocation and exact discarded prefix were not captured. The cached count has no generation freshness timestamp and is distinct from current KV-cache usage. Automatic native New/Send remained without qualification.

## 1.3.23 - Manager lifetime for native chat continuity (installed, unpublished, superseded)

- Added the Manager transition-worker observer and internal authenticated bounded observe/status bridge; preserved public 112/112/5 catalog and all existing features.
- Source b42a80df7db637337ea60737ef8e1049159cf448 / tree a475d6d04e5a70b32bb72390afc7347d2d97ff65 passed Product All, three static gates, persistence and 167/167 tests in 95.75 seconds. Valid signed installation matched four executables and 323 payload files; prelaunch profile comparison preserved 8,471 files / 412,433,227 bytes and four snapshots.
- Two actual Primary assisted calls returned the retained packet and 1.3.23/112 status; final Qwen generation reached eosFound. Recovery stayed pending because the delivered context_get result added context_budget_cleared:false after the Manager callback. No new 1.3.23 recovery archive, automatic New/Send qualification or release publication was established. See [retained 1.3.23 measurements](docs/validation/HOST-CAPABILITIES-1.3.23.md).

## 1.3.22 - Optional local image-provider jobs (unpublished, superseded)

- Add six optional disabled-by-default ComfyUI tools: image_provider_status, image_generate, image_edit, image_job_status, image_job_cancel and image_job_resume. Retain every previous descriptor, raising Primary/Fallback from 106 to 112 while preserving five CLU tools and ten specialist playbooks.
- Require explicit owner-selected loopback endpoint, sd1 profile, compatible checkpoint, seed, authorized paths and bounded dimensions. Reuse native WinHTTP/WIC and Manager-issued authority; no model installation, provider startup, paid account, global interrupt or new runtime dependency.
- Persist exact prompt/workflow and destination/source seals before dispatch. Preserve ambiguous outcomes without resubmission; keep status read-only and require fresh authority for explicit exact-job resume. Cancellation suppresses local publication without claiming remote termination.
- Preserve exact original RGBA where mask red is zero, including nonopaque alpha. Protect active workers and in-flight API borrowers during bounded cache/evidence retirement; retain published user artifacts and normalize equivalent job UUIDs.
- Exempt image_job_status from identical-call loop handoff and spell all six image-provider names in initialization/help. Drawing and read-only review do not start generative jobs.
- Align the shared native chat-file snapshot reader from 32 MiB to the ordinary reader's 64 MiB bound, retaining identity/revision/context checks. Plugin cleanup, loaded-model acknowledgement and completed-tool-boundary corroboration share this helper. The exact live failing caller and installed recovery remain unverified.
- Add explicit route-upgrade recovery through the existing authorized Primary session_handoff. Require matching native Primary results, a new complete stored model packet/current project pointer and fresh validation of all three routes. Archive the original encrypted checkpoint before promoting only an undispatched waiting request; retain false old acknowledgement and default scope rejection/no replay for uncertain or confirmed effects.
- Preserve 1.3.21 SystemDrive/known-folder handling and all existing desktop, image, CMake/CTest, worker, schedule and continuity contracts.
- Record the isolated ten-target pass and root-owned six-case real ComfyUI smoke within their exact source/native scope; do not treat them as integrated Manager/LM Studio or host-model-quality qualification.
- Preserved pre-final check record: source/package identities were 1.3.22 / 1.3.22.0. Earlier clean source 5b8c524 passed Product All, three static gates and persistence; its complete Release graph passed 165/166 entries in 110.15 seconds with one retained CMake parser cancellation-fixture failure. The test-only cancellation handshake and explicit route-recovery source passed two focused targets, including 125 Infrastructure groups and 37 route-recovery cases, in 64.45 seconds. Fresh complete source qualification remains pending. Package/installed/native/publication qualification remains pending; see [candidate notes](docs/releases/1.3.22.md), [provider guide](docs/IMAGE-PROVIDER.md) and [qualification boundary](docs/validation/HOST-CAPABILITIES-1.3.22.md).

- Final clean source `9178abdf12998af56df4860a56b25dccfcc30e10` / tree `b5e9fdedc9cb6b7ea0f97f9d93ef2e2d2f313d64` passed Product All, three static gates, persistence and 166/166 Release tests in 97.71 seconds. Signed installation matched all four executables and preserved 8,431 files / 410,168,202 bytes and four snapshots before launch.
- Actual selected Qwen Primary status reported 1.3.22/112 tools; successful model handoff sequence 8 preserved encrypted checkpoint revision 2 and entered Creating revision 3. Revision 4 retained uncertain New chat, the new selected chat had zero messages, and all three CLI/bridge pairs exited with code 1. No automatic packet delivery or completed rollover was established. Catalog 112/112/5 and integrated public/native provider acceptance remained unqualified; candidate superseded for observer lifetime.

- Later manually seeded Primary context_get/get_forge_status readback advanced the same packet to checkpoint revision 6/resumed, with confirmed New chat and delivery/context-recovery flags. This assisted recovery does not establish automatic packet Send; the passive watch did not capture revision 5.

## 1.3.21 - Durable CMake/CTest jobs and richer desktop/image tools (published)

- Clear stale context telemetry after the selected native LM Studio chat is cleared, while preserving handoff and workspace-binding state. Refresh preferences and clear recovered read errors only after successful observation; a real selected empty chat retains its identity.
- Add an atomic, Windows-user-protected checkpoint for native visible-chat handoff phases. Fresh project/provider/route authority and exact native message or packet-recovery evidence are required after observer reconstruction. Record Send and New chat before dispatch, retain uncertain outcomes without replay, and expose `recovery_pending` when reconciliation is incomplete. The 118-group Infrastructure suite passed, including the private same-PID observer reconstruction regression and 20 durability cases. Installed observer/UI interruption or connector rollover recovery remains unverified; physical context exhaustion and already-running agent reattachment were not exercised. Historical 1.3.20 checkpoint-process primitive evidence retains its original source identity and is not a current installed qualification.
- Add `cmake_test_run` and `cmake_test_status`, increasing Primary/Fallback to 106 tools while retaining every previous tool, ten specialist playbooks and five CLU tools. Reuse durable jobs for sequential build/test phases, actual phase results, validated JUnit counts, paged failures, logs and cancellation. Explicit initialized build directories and current local Read/Write/Execute authority remain required.
- Add accessibility `offset` paging, actual row indices and continuation metadata to `desktop_read`, with bounded text and encoded JSON pages from a fresh UI tree.
- Add optional 128–2048 pixel preview requests to image read/write, desktop capture and independent image analysis. Preserve the default 256-pixel preview and 512 KiB base64-encoded transport bound with adaptive resizing.
- Add optional source-coordinate samples to `image_read`, returning measured RGBA8 values and hashes of the canonical decoded frame and emitted preview PNG. Source alpha measurements remain distinct from the existing opaque preview and do not establish the stock chat's inference input.
- Supply bounded `SystemDrive`, `ProgramFiles`, `ProgramFiles(x86)` and `ProgramData` defaults for native shell/process jobs. This corrects the installed 1.3.20 MSBuild known-folder initialization failure while preserving case-insensitive caller overrides, omission of missing/oversized defaults and the limited environment. Add real Windows PowerShell 5.1 known-folder, repeated-build and intended-failure-marker regression coverage.
- Correct filled rectangle dimensions, including one-pixel shapes and canvas clipping. Requested width and height now include the previously omitted right/bottom edge.
- Clear recovered LM Studio read-pipeline errors only after successful complete observation; preserve operational handoff and workspace-binding errors.
- Align native, CMake, packaging and application source identities to 1.3.21 / 1.3.21.0. Clean-source, signed-package, installed-catalog and measured native-model qualification passed; the 1.3.21 verification record states the exact scope and limits. Publication readback remains a separate check; [the verification record](docs/validation/HOST-CAPABILITIES-1.3.21.md) retains those distinctions.

## 1.3.19 - Provider timeout diagnostic consistency (published)

- Retain all 104 Primary/Fallback tools, ten specialist playbooks and five CLU tools, including native web/Office/desktop/image analysis, workers, schedules and the original workflows.
- Report the selected receive budget consistently in both native WinHTTP timeout diagnostics, while timeout, deadline and cancellation behavior remain unchanged. Retain legacy-only managed-context completion bookkeeping and its lease requirement; recovered packets, client ownership and canonical request/receipt validation remain preserved. The packaged source completed the required source and native qualification recorded above.
- Preserve alternate-profile automatic-deployment isolation; live MCP and all three plugin inventories remain unchanged across final isolated probes.
- Both native WinHTTP timeout diagnostics consistently report the selected receive budget, with unchanged timeout, deadline and cancellation behavior. The managed-context legacy-lease fix is retained. The exact 1.3.19 source passed 162/162 Release tests in 74.37 seconds, Product All, all three static gates and package persistence. Windows CI separately passed 162/162 in 126.04 seconds. Installed/staging/MSIX hashes matched all four executables, and actual catalogs returned 104/104/5. The existing selected Qwen conversation passed 9 bounded cases, including the formerly denied Host read and fresh sealed blind image analysis. The dedicated real-model managed recovery returned the exact seeded packet and unseen file in 2 captured native calls with verified sealed output. [Published release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.19) contains seven assets verified by size and SHA-256. [Windows package](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.msix) and [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.zip).

## 1.3.18 - Managed context-recovery completion (unpublished, superseded)

- Restrict recovered-context legacy bookkeeping and its lease requirement to legacy-protocol calls. Managed-run `context_get` retains the recovered packet without clearing or annotating unrelated pending legacy state; client ownership and canonical request/receipt validation remain unconditional.
- The original missing-lease regression failed before the one-condition completion fix; the existing InvocationGuard CTest entry then passed 1/1 in 0.09 seconds (0.10 seconds total).
- Source `ffd10dce9125bf700ad5efbfd1228475704cc792` / tree `2781ec95847694ff4225471a8655fa64632bb11e` passed 162/162 local Release tests in 78.61 seconds, Product All, all three static gates and package persistence. Windows CI 37657943054 failed 161/162 in 115.37 seconds on the selected timeout-message assertion; its actual returned message was not printed. Twenty unchanged-source isolated repetitions passed without reproducing that CI mismatch.
- Signed 1.3.18.0 installation matched all four payloads and preserved 8,256 profile files / 401,730,716 bytes. Eleven current-chat native calls succeeded, but complete nine-case acceptance remained unfinished. The first independent managed-recovery probe returned the actual packet and file contents, then failed its final narrative-format assertion and remains unqualified.
- Final preservation verified the 1,199-file original workspace, prior conversation prefix, owner configuration/selection, foreign MCP semantics and all 730 foreign plugin files; only verified policy evaluation history appended. The original project gate was not executed or approved. No final release qualification, tag or publication followed.

## 1.3.17 - Cold Manager startup reporting (unpublished, superseded)

- Retain the exact owned Manager startup process handle and report its exit promptly when no competing profile owner exists, preserving authenticated sibling/profile admission and the isolated automatic-deployment boundary.
- Source `34e6e4c3f418c52522e4db33bf1d1dbb917abc46` / tree `c9a899ed0318af0d25d87c35f5c984c5b13eaa17` passed 162/162 local Release tests in 84.41 seconds; [Windows CI 37646332085](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37646332085) passed 162/162 in 91.69 seconds. Product All, static gates, package persistence and signed packaging passed.
- Installed 1.3.17.0 payloads matched all four staging/MSIX hashes and preserved 8,206 profile files. Automatic three-role repair completed in 8.019 seconds with protected state and all 730 foreign plugin files preserved.
- Retain actual current-chat attempts, a completed first image-analysis run refused by the literal property check, and blocked Windows notification admission. A valid managed `context_get` recovery was rejected by legacy-only lease bookkeeping in completion. Final release qualification, a tag and publication did not follow; the higher 1.3.18.0 candidate addresses that verified source defect.

## 1.3.16 - Isolated Manager deployment boundary (unpublished, superseded)

- Align active product, package, dependency-manifest and documentation identities to 1.3.16 / 1.3.16.0 for a higher Windows upgrade after the installed, unpublished 1.3.15.0 candidate.
- Retain all 104 Primary/Fallback tools, ten specialist playbooks and five CLU tools, including independent image analysis, completed sealed reviewer receipts, workers, schedules, Office, web, desktop, filesystem, shell/process, governance and continuity workflows.
- Preserve the verified 1.3.15 test-isolation failure: an isolated Manager committed automatic deployment into production LM Studio during the source suite, changing only the three Forge routes and their generated bridge files before installation. Foreign MCP configuration remained semantically identical. Final qualification must verify the corrected external-maintenance boundary and production routing preservation.
- Enable automatic background LM Studio deployment only for the ordinary persistent owner data root. Alternate isolated profiles skip automatic reconciliation before deployment admission; explicit authorized repair and read-only status retain their existing routes. The source suite and production-preservation checks passed for the exact 1.3.16 artifact recorded below.
- Source `25d15f922744c464fdf660fd991f1cdfe1542323` / tree `1b23dfa072ea60e7da0f02dd4bd20f7028ac4dc9` passed 162/162 local tests in 78.19 seconds; Windows CI run 37640187548 passed 162/162 in 92.20 seconds. Static gates, package persistence, Product All and signed packaging passed. The installed 1.3.16.0 upgrade preserved all four payload hashes and 7,991 owner-profile files.
- Explicit repair completed in 62.857 seconds, registered all three 1.3.16 routes and preserved foreign MCP semantics plus 730 foreign plugin files. A subsequent observed cold Manager child exited while the CLI kept waiting for startup; the candidate is superseded for that reporting fix before original/current-chat workflow qualification, a release tag or publication.

## 1.3.15 - Image analysis from the existing LM Studio integrations (unpublished, superseded)

The signed candidate was installed, but preservation verification refused the MCP routing change caused earlier by an isolated test Manager. Its source test and package evidence are retained in `docs/validation/HOST-CAPABILITIES-1.3.15.md`; it has no complete current-chat workflow qualification or public release.

- Retain completed sealed reviewer receipts without a lifetime count limit while bounding concurrent independent reviewers to sixteen. Terminal status reloads the durable receipt and reports its seal and integrity state.

- Add `image_analyze` as an authorized asynchronous independent read-only image analysis, increasing Primary/Fallback to 104 tools while preserving the previous 103 and all five CLU tools.
- Validate and decode the authorized source path before Manager dispatch. Reuse the existing fresh reviewer and Responses image-content path; poll actual output through `reviewer_status` without holding a long MCP call open.
- Keep `image_read` previews compatible and describe their host-dependent interpretation accurately. Preserve explicit independent-run provenance, actual errors/token usage, read-only enforcement and exclusion of recursive analysis from worker scopes.
- Include the corrected native-chat continuity and preference readback wording in the higher Windows upgrade package. Preserve project identity, records, native conversations, policy gates and the three existing integrations.
- Align current native/product/package/documentation identities to 1.3.15 / 1.3.15.0. Retain all earlier engineering-candidate evidence with its original source and executable hashes; final qualification is recorded only after execution.

## 1.3.14 - Dedicated host workflows for local models

- Expand Primary/Fallback from 80 to 103 tools without removing existing tools, specialist sessions, reviews, memory, continuity, policy, shell/process jobs, or the three LM Studio integrations.
- Add native WinHTTP web search, fetch, and bounded explicit HTTP GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS; return observed status, source URLs, truncation, and network/search challenges. Mutating methods retain their original response instead of following redirects automatically.
- Add native stored-ZIP OOXML DOCX/XLSX/PPTX creation from structured text and typed cells. Validate input, XML characters, package bounds, names and integer precision before authorized atomic publication. No Office or Python runtime is required; spreadsheet text remains literal and no formula evaluation is claimed.
- Add visible-window/PID enumeration, accessibility reading, browser opening, screenshot capture, and authorized observed desktop input. Add native PNG drawing and local image previews using Windows codecs; generative image providers and cloud accounts remain external connections.
- Add separately persisted independent Manager workers with frozen roots/grants/tool allowlists, fresh provider histories, actual output/status/cancellation, bounded total lifetimes, and interruption receipts that preserve uncertain effects. Bound simultaneous workers to sixteen and retire finished terminal threads while retaining sealed receipts by run ID beyond the reviewer's sixteen-record lifetime gate. Receipt files remain bounded, storage failures are explicit, and existing outputs are not automatically deleted. Preserve existing specialist-session and read-only reviewer behavior.
- Add Manager-owned persistent schedules with UTC or interval triggers, separately authorized atomic storage, owner/task authorization references, current-policy intersection, no overlap, actual run history, local Windows toast submission receipts, and explicit retry authorization after uncertain interruption. Notifications preserve accepted/failure state and never claim confirmed display; schedules require Manager to remain running, and future triggers restore after restart.
- Bound schedule list/mutation responses with summaries and provide complete record JSON through `schedule_list` pages selected by `schedule_id`. UTF-8 byte boundaries and a persisted per-record revision guard prevent mixed reads; task text, scope, retained history, logs and full notification receipts remain available through page reconstruction.
- Add owner-selected `filesystem_access: host` for ordinary local volumes and retain `workspace` mode. Preserve the selected project's default relative-path directory and enforce native ACL, reparse, namespace, and authority checks. Models cannot grant new filesystem mode through a tool request.
- Preserve registered project identity during host-mode continuity recovery by authorizing both the project alias and recovered candidate, then checking their canonical subtree relationship. Host volume roots do not replace the project directory; revoked or unrelated paths remain denied.
- Expose the existing Ensure manager, Restart service and Stop service actions in Rig's Runtime configuration card, making them accessible from that destination while retaining existing handlers and Settings actions.
- Correct Rig's continuity wording to show the App's observed native conversation and the Manager's enabled/disabled preference readback. Remove the unconditional unavailable claim and obsolete selected-run label; direct live rollover inspection to Primary MCP's `get_forge_status`. The updated App is built into the signed candidate; installed readback remains separate, and earlier artifact evidence retains its original source identity.
- Add bounded PNG image content to managed worker/reviewer Responses function outputs while preserving text-only compatibility and original call identity; encoded previews are limited to 512 KiB. Exclude legacy session_checkpoint/session_handoff from independent worker scopes while retaining their existing authorized tools.
- Remove FILE_ADD_FILE from SQLite's retained directory anchor, retaining traversal/read access and namespace pins so diagnostics can append while WAL/SHM is live without the observed sharing conflict. Creation rights remain checked by native file opens; focused coexistence and creation-denial cases passed.
- Align active product, package, dependency-manifest and documentation versions to 1.3.14 / 1.3.14.0. Historical release and validation records retain their original versions and artifact identities.

## 1.3.13 - Reliable local-model reviews and evidence workflows

- Derive both local-provider HTTP User-Agent versions and active workflow/scaffold expectations from the shared product version.

- Make each independent review's provider receive budget configurable from 1 to 3,600 seconds, defaulting to 600 seconds. Persist the selected timeout in the verified receipt and expose it with infrastructure-failure classification; retain transport-operation deadline and cancellation checks during response-body reads.
- Authorize opening-file paths locally before Manager dispatch; accept either an authorized opening-message file or up to 64 KiB of inline UTF-8. Keep read-only tools as the default and add explicit `text_only` mode for reviews of supplied text.
- Retain explicitly activated owner-configured evidence roots through continuity recovery. Resolve explicit root activation through the owning authority while preserving deliberately removed grants, denied paths, and strict allowlist checks.
- Validate and canonicalize `process_launch` and `shell_job_start` working directories before Manager dispatch using the same local authority as `shell_exec`; report the effective policy in status.
- Remove unnecessary directory `FILE_DELETE_CHILD` and file-creation access from parent-directory creation while retaining its strict sharing, rename, and reparse protection. Distinguish native filesystem access denial from authority-policy denial and include the failing path, operation, and Win32 code.
- Raise synchronous and tracked PowerShell command capacity to 65,536 UTF-8 bytes. Deliver scripts through supervised UTF-8 stdin with fixed launch arguments and preserve explicit exits, the original PowerShell final-statement failure status, nonterminating errors, timeouts, cancellation, and process ownership.
- Discover authenticated matching isolated Managers for custom CLI profiles without dropping home/version checks.
- Align product identity to 1.3.13 and Windows package identity to 1.3.13.0; retain the complete tool/agent catalog, three LM Studio roles, stable publisher, persistence, and existing product surfaces.

## 1.3.12 - Observable process work and verification evidence

- Expand Primary/Fallback to 80 tools: durable Manager-backed process launch/poll/wait/list/kill/adopt, live stdout/stderr paging, native host and loaded-provider inspection, fixed read-only GitHub routes, configured authority binding, evidence capture/readback, pinned verification venvs, and separate read-only reviewer sessions.
- Retain the four tracked shell convenience calls and synchronous `shell_exec` capped at 120 seconds. Bound owned jobs to 3,600 seconds, two active jobs, and sixteen retained records per shell-service instance; retain explicit shutdown/interruption states.
- Add exact existing owner-configured external root binding with strict Windows path checks and rejection of narrowed-capability widening. Existing 2 MiB `fs_write` works under active authorized roots.
- Stream binary SHA-256 and byte counts into an atomically persisted capture chain with cross-process serialization, full-chain verification, bounded paging, and refusal to overwrite altered logs. Document the independently retained head required by an unkeyed chain.
- Materialize exact supported verification pins outside the source root; record observed runtime/distribution versions only after successful creation. Start independently authorized reviewer work with a fresh provider context and enforced read tools.
- Supply explicit account/profile environment and Python UTF-8 defaults; update all ten specialist resources and matching embedded fallbacks with the supported process/evidence workflow.
- Advance product/package identity to 1.3.12 / 1.3.12.0. Retain the selected workspace, ordinary profile, stable publisher, original chat, and foreign LM Studio registrations during deployment.
- Keep original failure evidence, implementation scope, and pending verification separate. Current test/package/native-model results are recorded after execution.

## 1.3.11 - Workspace bootstrap and complete native tool delivery

Public repair release following 1.3.6. The 1.3.7 through 1.3.10 sections below record intermediate local development builds; they were not published GitHub releases.

- Carry the selected registered project ID and workspace through the App, Manager, deployment, and all three LM Studio roles. Report drift against that binding and preserve project-specific stores and foreign registrations.
- Share bounded legacy instruction-package migration between Manager listing and MCP bootstrap. Retain queue order, explicit removal, pinned content, and visible missing-content coverage; include the direct queue editing and policy file/folder browsing improvements.
- Advertise required filesystem-edit arguments and usable glob/move arguments. Match specialist start/status requirements to strict project ownership.
- Scope implicit continuity lookup to the current project and retain authorized workspace/recent-file evidence in budget checkpoints, while preserving explicit historical packet retrieval.
- Page complete policy inventories with exact snapshot cursors and bounded UTF-8 reads. Deliver other large results in complete ordered fragments through LM Studio's per-block limit, without replaying the operation.
- Align VERSION, BUILD, current product documentation, and application/package identity to 1.3.11 / 1.3.11.0. Packaging rejects inconsistent version files and uses exact clean source/staging provenance.
- Normalize supported forward and mixed separators at the Windows requested-path authorization boundary. Retain strict configured roots, access, scope, traversal, namespace and reparse protections.
- Reproduce the real-process 1.3.10 path rejection and verify exact filesystem/shell results for all supported separator forms.
- Record the accepted signed installation, 153-test Release suite, and native loaded-Qwen readback of complete policy, pinned instructions and an untruncated 64,038-byte shell output. The public release package is rebuilt and qualified separately; see the release and installed qualification records for exact provenance and limits.

## 1.3.10 - Complete native MCP delivery

- Page policy inventories using exact project/revision/snapshot cursors and bounded final serialized JSON; preserve global counts and accurate UTF-8 text continuation.
- Return other large results as ordered bounded JSON text fragments, with complete strict native reconstruction and the existing whole-response limit.
- Preserve strict catalog verification and pending governance notifications; verify complete adaptive file reassembly and exact side effects.
- Record the separate forward-slash cwd rejection discovered by native acceptance; the 1.3.11 correction follows without obscuring that failure.

## 1.3.9 - MCP project bootstrap and continuity recovery

- Migrate saved legacy instruction packages through the same implementation in Manager and MCP bootstrap; retrieve pinned stored content even when its original folder is unavailable. Preserve explicit queue removal and report missing legacy files as coverage gaps.
- Advertise the required filesystem-edit arguments and usable glob/move arguments to tool-capable models.
- Keep automatic budget checkpoints bound to the observed authorized workspace and files, and scope default packet recovery to that workspace. Preserve explicit access to historical packets.
- Bind LM Studio install/repair to the selected registered project and check project drift alongside binary, home, roles, and timeout.
- Bind specialist start/status capabilities and durable sessions to the resolved project; retain strict cross-project reattachment rejection.
- Update deployment verification to the exact repaired tool catalog fingerprint.
- Add populated cross-process package/policy, specialist lifecycle, and native/model integration evidence.

## 1.3.8 - Direct instruction queue editing

- Allow immediate removal of any instruction-package queue row, including an active package.
- Save the current queue order by updating its durable record so returning to a previous order remains effective after refresh and restart.
- Keep an explicitly emptied queue empty instead of migrating its deleted legacy package again. Allow a removed package to be added again with the same revision, while reusing an already queued package.
- Serialize Manager queue mutations and verify removal, repeated reordering, database reopen, re-addition, and the MCP status/bootstrap/read views.

## 1.3.7 - CLU policy browsing

- Open the Windows policy picker even when no project is selected. Keep activation scoped to a selected project and report how to activate a source chosen before project selection.
- Add a single-file picker alongside the existing folder picker. File sources inventory only the selected file and support the existing revision, reading, refresh, and governance paths.
- Check the project selection again after the modal picker returns so a source is not bound to a project that changed while browsing.

## 1.3.6 - Stable telemetry refresh layout

- Restore the hidden diagnostic status after the Actions-frame removal and keep background telemetry polls from replacing it with command-progress text. Periodic polling no longer adds and removes a multiline row in the shared page layout. Live telemetry cadence and the existing product controls are retained.
- Verify actual WinUI geometry with an injected isolated Manager: the baseline moved 130.667 logical units with 25 transitions; the fix showed zero transitions across 16 polls while injected CPU values continued changing from 30.0% to 34.0%.
- Pass the full 153-entry x64 Release CTest matrix in 32.36 seconds, App/backend/package builds, and all three static gates. Verify installed 1.3.6 App/Manager/CLI hashes against the package; no installed-window visual inspection is claimed because native-control initialization failed with MODULE_NOT_FOUND for kernel.js.
- Retain the 1.3.5 continuity bounds: reserve-triggered pause, not physical exhaustion; native rollover verified while the primary MCP worker stayed alive; interrupted handoff after idle-process eviction is not durable and is not claimed.

## 1.3.5 - LM Studio chat Auto Continuity

- Remove Managed Run/readback and its callable product routes, Export selected project, Import verify first, the Actions frame/Invoke Tool, Advanced Canonical Catalog, Scope Test, Reset Scope, Apply and Verify, and the old Data Maintenance scheme. Sessions remain LM Studio chats; baseline agent-session tools are not a replacement run manager.
- Restore Load effective settings, Save and read back, Revert pending edits, Test LM Studio, and Restart Manager. Keep Settings data actions as selectable records and buttons; add a Continuity packet list with delete-selection and clear actions.
- Report the authoritative project ID/folder and binding source, ordered instruction-package paths, development-policy folder, tool names/count, and agent count through `get_forge_status`. Add callable selected-package retrieval through `instruction_package.read`.
- Bind a selected development-policy repository immediately from the CLU folder picker. Rename controls to their actions; reinforce bound policies with the model and return findings through tool-result notifications and the findings UI/Activity.
- Add native selected-conversation usage monitoring and a completed-tool-boundary pause. Ask the loaded model for a detailed packet, publish it for project-scoped connector pickup, create the next visible LM Studio chat through product Windows UI Automation, hand over the packet, and verify a following Forge tool call.
- Keep install/repair as one action for the existing Primary, Fallback, and CLU integrations. No fourth plugin, Forge-held credential, login step, or model-instruction file is added.
- Pass the full 153-entry x64 Release CTest matrix, Release backend/App builds, and three static gates. Keep the nine affected Debug suites and isolated native-chat verification labeled separately. Correct the process test's temporary directory placement without stopping the installed Manager or weakening its assertion.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## 1.3.4 — LM Studio unauthenticated-loopback diagnostics

- Record the supported same-host contract: LM Studio must allow unauthenticated loopback requests. Enabling **Require Authentication** (`json.tokenMode=required`) can leave the LM Studio UI showing connected while Work Space model discovery and the Responses contract probe fail.
- Diagnose HTTP 401/403 explicitly in Work Space model discovery, Responses transport, and automatic model preparation, directing the operator to disable LM Studio Require Authentication.
- Add a loopback regression proving the Responses transport returns the Unauthorized error code and the unauthenticated-loopback guidance for an HTTP 401 model-discovery response.
- Keep the local integration free of Forge token storage, bearer-auth configuration, permissions UX, and authentication setup flows. The existing 1.3.3 installation is not replaced during release construction.

## 1.3.3 — Explicit LM Studio workspace context

- Send an MCP `initialize` instruction block that names the registered project folder, ordered instruction-package folders, and active development-policy source and revision before the model's first tool call.
- Extend `forge_status` with structured `workspace`, `instruction_packages`, and `development_policy` fields sourced from the authoritative project registry, project memory, and policy service.
- Mark Forge `home` as application data only, require instruction packages to be read in listed order, and require an active development policy to be read and followed.
- Make the LM Studio serve verifier reject integrations that omit or corrupt this workspace bootstrap context, and describe the expanded status contract in the canonical tool catalog.
- Record that live qualification quarantined the ordinary-profile SQLite main/WAL/shared-memory snapshot after a failed migrated-database quick-check, and that the snapshot was restored byte-for-byte while all recovery copies were retained.

## 1.3.2 — Reliable long-running LM Studio MCP calls

- Write an exact integer `timeout: 180000` to the Primary, Fallback, and CLU registrations so LM Studio's outer request deadline exceeds Forge's 120-second `shell_exec` limit.
- Emit and validate the same timeout in each synchronized plugin bridge and host-activation acknowledgment. Missing, stale, or non-integer timeout values are reported as drift instead of being accepted as healthy.
- Preserve foreign MCP registrations and unknown fields while repairing Forge-owned timeout values.
- Keep Fallback as an independent selectable integration and health role. A timed-out call is not automatically replayed through Fallback because a mutating operation can have an ambiguous completion state.
- Repair the no-attribution gate's self-referential policy exclusions and make the G15 source invariant independent of checkout line endings.

The complete Release build passes all 153 configured tests and the static release gates. A live Windows LM Studio call completed after more than 70 seconds with the repaired configuration, beyond the former 60-second cutoff. The engineering package remains development-signed.

## 1.3.1 — Complete memory browsing and direct record maintenance

- Add **Display All** to project-memory browsing. The Manager protocol accepts stable cursors and the native app retrieves 100 records per page with duplicate, consistency, and termination guards.
- Replace the slow one-record maintenance path with a scrollable, extended-selection list. Every memory row has a visible **Delete record** action; **Delete selected** confirms once, calls `project_memory.forget` for each selected memory key, and refreshes the remaining rows.
- Keep continuity cleanup visibly separate as **Reset project continuity**, and retain the existing exact-confirmation scope reset as a separate control.
- Include continuity operation, handoff, recovery, and active-state readback in the project workspace snapshot used by Data maintenance.
- Prefer PowerShell 7 for native shell execution and construct an explicit safe child environment containing Windows system tools, PowerShell 7, Git, `PATHEXT`, and `COMSPEC` even when the MCP launcher supplies a minimal environment.
- Advance the native runtime, session-host adapter, and stable MSIX package to 1.3.1 / `1.3.1.0`.

The Release build passes all 153 configured tests and both static gates. Automated coverage includes a 101-record multi-page Display All read and native host environment checks. The engineering package is development-signed; installed UI interaction remains an operator acceptance step.

## 1.3.0 — Simplified Workspace and CLU governance

- Replace normal wizard/mission navigation with Workspace, Rig, Activity, and Settings.
- Add a persistent project-scoped ordered queue of universal instruction-package folders with streamed hashes, revision-bound paging, explicit opaque/reparse/failure records, retry, removal, and ordered managed-run attachment.
- Replace the four CLU continuity-control tools with development-governance operations for evidence evaluation, findings, correction receipts, redacted export, and policy document readback.
- Keep governance nonblocking when no policy is bound and keep coverage gaps visible rather than treating unparsed content as omitted.
- Add an independent per-project/provider automatic-continuity preference and carry it through the typed Manager start protocol; disabled runs skip automatic continuity observations.
- Migrate legacy active instruction-package state into the ordered queue and retain compatible project-policy state migration.
- Add project-correlated package/cursor and CLU finding/resolution rows to Activity, plus Settings Doctor checks for queue/cursor, policy repository/notification/export, continuity provider binding, migration, and schema alignment.
- Advance native runtime and package identity to 1.3.0.

The release opens successfully under a disposable profile and passes the 153-test local Release matrix and static gates. The published engineering package is development-signed; live supported-provider successor qualification has not been rerun, so continuity remains `Preparing`, not `Active`.

## 1.2.1 — Complete plugin setup and visible guidance

- Add automatic installation, readback and LM Studio synchronization of Primary, Fallback and Continuity plugins to project preparation. Stop before model/task readiness if this stage fails.
- Fix project governance incorrectly blocking Manager-owned plugin maintenance. Keep project tools behind their policy gate and keep maintenance behind its separate scoped capability.
- Open guided setup for profiles without a completed first setup task, including upgrades with an existing project and a previously disabled guide.
- Put Start here first in navigation, keep a visible setup entry on other pages, retain navigation labels at normal desktop widths, and present preparation, optional policy and task entry in order.
- Move clearly named plugin installation controls to the top of the LM Studio plugins page. Replace misleading guide toggles with direct navigation buttons.
- Update offline help, packaging surface checks and regression coverage for the five-stage setup contract.
## 1.2.0 — Automatic project setup and development policy

- Replace manual setup navigation with folder selection, automatic Manager/project/model preparation, a verified model response and task entry on one page.
- Start the installed LM Studio server when necessary; reuse or load a compatible downloaded model, verify its context capacity, and persist the selected instance. Add cancellation and idempotent retry.
- Import a GitHub policy repository at an immutable commit or a local text snapshot. Use existing Git authentication for private repositories without copying credentials or executing imported scripts.
- Adopt project policy, require a recorded human review, and enforce approved file paths, prohibited paths and exact command arguments at common tool authorization. Expose complete pinned documents through the read-only `project_policy.read` tool.
- Persist provider bindings across Manager restart; new settings apply to new runs without silently changing existing history.
- Expand offline help to 42 articles and correct the newly prepared project's run label.
- Add setup, cancellation, real Manager/tool, policy denial, persistence and recovery tests. Publish release evidence separately from model-generated claims.

## 1.1.44 — Offline setup help (engineering prerelease)

- Add 20 searchable offline help articles under Guided setup, including provider recovery, project instructions, permissions, memory, telemetry, and evidence.
- Open the Windows folder picker immediately from the opening setup action. Describe the final guide state as guide completion rather than verified readiness.
- Isolate the real-process Manager lifecycle test from the installed application's dashboard port.
- Add a consolidated Release verification runner with per-check logs, hashes, exit codes, and explicit outstanding acceptance work.
- Document the proposed shorter setup path and pinned repository-policy intake and enforcement design. Policy repository import and enforcement are not implemented in this version.
- Allow package metadata to identify an engineering prerelease. Full installed acceptance, stale Manager detail presentation, and isolated shutdown behavior remain open.

## 1.1.43 — Complete Guided Mode

- Replace the shallow setup card with a persistent nine-step Guided Mode that covers a real project from folder authorization and Manager verification through scope review, optional instruction packages, optional durable memory, provider readiness, and the first real managed task.
- Add an unmistakable **Guided setup** destination to the main navigation while retaining synchronized on/off controls on Rig and in Settings. The dedicated page shows the complete path and can restart the guide at any time.
- Keep every action on the production workflow: the guide uses the operator's folders, project identity, instruction validation, project-memory store, provider configuration, and managed-run form. Optional steps can be skipped without generating placeholder content, sample projects, or mock records.
- Advance only after authoritative readback for project registration, instruction activation, saved memory, and managed-run creation; display the installed product version in the navigation footer so an upgrade can be confirmed in the app itself.

## 1.1.42 — Guided project setup

- Add a persistent five-step Guided Mode that explains project authorization, durable identity, instruction packages, project memory, and the handoff to managed work while using the operator's real folders and records.
- Expose synchronized Guided Mode toggles on Rig and in Settings, save progress per deployment profile, and allow the walkthrough to be dismissed or restarted at any time.
- Reuse the existing Windows folder picker and Manager-owned registration path; the guide advances past registration only after an authoritative project readback and never creates a tutorial project or mock data.

## 1.1.41 — Project instruction packages

- Add a first-class Direction A instruction-package workflow to Projects: choose a folder, validate every supported UTF-8 text file, inspect its manifest and activate the exact SHA-256 revision.
- Keep validation non-mutating, reject changed-after-preview folders, and persist accepted files plus a project-bound active manifest through the Manager-owned memory service.
- Attach the active project revision to every new managed run within the bounded task budget, with remaining files addressable through project-memory tools. Add protocol round trips and Manager regressions for preview, activation, stale-revision rejection, project isolation and run attachment.

## 1.1.40 — Large-text navigation completion

- Widen the Direction A navigation pane so every destination label remains complete at Windows 200% text scaling.
- Carry forward the 1.1.39 compact Settings reflow and wrapped header metadata after its signed installed large-text pass exposed the final clipped destination label.

## 1.1.39 — Large-text responsive refinement

- Reflow the Settings summary cards into a balanced two-by-two composition at compact width, matching the Rig telemetry treatment instead of compressing four cards into one row.
- Allow environment and last-updated metadata to wrap to two lines so Windows 200% text scaling preserves complete values without overlapping adjacent header controls.
- Carry forward the signed installed 1.1.38 Direction A visual sweep and its compact, ultrawide, filtered-empty, and High Contrast evidence. Desktop-source handoff remains a separate qualification gate.

## 1.1.38 — Direction A depth and compact Rig composition

- Give the native shell and cards layered graphite-to-blue gradients, directional borders, and a brighter hero edge; retain system-color brushes in High Contrast.
- Align the desktop navigation width and mosaic mark with the selected Direction A concept, add chart time-grid lines and the Rig hero action glyph, and title the activity lane Recent events.
- At compact width, stack Rig configuration, live chart, recent events and actions into full-width panels so titles and controls remain legible. Signed installed 1.1.37 and rebuilt staged 1.1.38 screenshots are visual iterations, not final feature or desktop-continuity acceptance.

## 1.1.37 — Correct operational card placement

- Place Runtimes execution and operating policy beside each other on desktop, with project jobs below both; at compact width, present execution, policy, then jobs as a readable single-column sequence.
- Preserve the 1.1.36 Feed filter truth and bounded ultrawide canvas. Signed installed 1.1.36 exposed a covered Runtimes execution card, so its all-view screenshots remain diagnostic iteration evidence rather than final acceptance.

## 1.1.36 — Adaptive Direction A composition

- Bound the native page canvas to a practical 1440-pixel content width on ultrawide displays while preserving full-width cards at normal windows.
- Stack the operational detail and insights under the main panel when the window narrows below 1080 pixels; retain the desktop two-column composition above that breakpoint.
- Make a category/project-filtered empty Feed explicitly say no audited outcome matched, and update the detail message without blaming the Manager for a zero-match filter.
- Carry forward the installed 1.1.35 fourteen-view archive-profile visual sweep and its exact-project audit proof. A disposable unpackaged live model-only run completed and sealed an exact project record, but this is not desktop continuity or independently verified assignment work.

## 1.1.35 — Project-scoped audit feed

- Carry the Manager-verified typed project authority into bounded audit persistence, Manager telemetry and dashboard readback. Requests that claim a mismatched project remain unscoped; projectless requests bound to a verified authority acquire its actual scope. C011 adds only the nullable project column, and historical outcomes remain unscoped instead of acquiring a fabricated binding.
- Give Feed native status, tool-family and current-project/unscoped filters. The current-project choice uses the persistent project selector; no project identifier has to be entered in the Feed.
- Rebalance the fourteen-view native shell toward Direction A's graphite canvas, restrained indigo/azure surfaces, softer card strokes, narrower navigation and more confident page typography. Signed installed archive-profile captures cover the visual iteration; production, scaled and accessibility acceptance remain separate.
- Add migration, audit round-trip, tool-router, Manager protocol and dashboard JSON regressions. Installed exact-project filtered-feed interaction passed; the complete acceptance matrix remains separate qualification work.

## 1.1.34 — Audit and exact-run presentation

- Show the bounded Manager audit window's success, error and denial mix, with its most recent observation, alongside selectable Feed detail and a direct path to project-bound durable evidence.
- Expose the verified run and project identity in Autonomy's live readback and keep the native-evidence path next to model output without implying that a completed response is a verified task result.
- Display the active isolated profile's data root in the Manager card rather than the default production path. Installed visual and integrated acceptance still require review of this exact build.
- Keep the dashboard real-socket smoke test's bounded loopback ports outside this host's reserved IPv6 TCP range so a valid listener is not mistaken for a product regression.

## 1.1.33 — Direction A shell and contrast-theme pass

- Give the active native navigation destination the approved blue treatment and give Tools the same hero-led hierarchy as the other work surfaces.
- Place the exact-run native check and approval controls ahead of provenance digests so the action is visible at a normal desktop viewport; color the latest check state distinctly from unverified or failed outcomes.
- Use runtime theme resources for the console's semantic brushes, with system-color mappings for contrast themes. Installed populated-view, scale, keyboard, and contrast qualification remains required.

## 1.1.32 — Exact-run native check receipt

- Add a bounded post-run native check action for an exact completed, sealed project run. It uses the project's authorized native shell route and persists a sealed receipt with command/stdout/stderr digests, exit state, elapsed time and check timestamp; command and output text are not included in the redacted evidence projection.
- Events & Evidence now requires an explicit approval in the installed UI, separates the specified native check result from durable metadata integrity and model output, and permits local export of the redacted receipt. A passing check verifies only that command, not every assignment requirement.
- Add protocol, cross-project, legacy-unsealed, passing/failing receipt, and durable-seal regressions. Installed passing/failing check and all-view visual acceptance remain required.

## 1.1.31 — Evidence presentation and Direction A lighting

- Replace the flat main console canvas with a blue-lit native gradient shared by every view, tuned against the approved Direction A visual language.
- Present exact-project durable runs through compact identity and digest tiles, with complete provenance and SHA-256 values available under an explicit disclosure. Preserve the separate native-record-integrity and task-outcome states and local redacted export.
- Correct stale Evidence labels that implied no durable projection existed, including singular run counts. Installed populated visual, scaling, and interaction review remains required before parity acceptance.

## 1.1.30 — Durable run evidence readback

- Seal terminal Manager-owned run summaries with an unkeyed SHA-256 consistency digest and validate saved metadata on native read; report legacy-unsealed and mismatch without promoting model text to a trusted task result.
- Add an exact-project Events & Evidence run selector with provider provenance, task and stored-output digests, native record-integrity status, and local redacted JSON export. The Manager excludes other projects and raw mission/output text.
- Retry only the transient same-project repository-opening race during automatic Projects readback. Protocol, redaction, project-boundary and altered-summary regressions accompany the new projection; installed visual and end-to-end checks remain required.

## 1.1.29 — Repeatable project archive export

- Retain an existing project-owned exports directory with only the permissions needed to create and inspect artifact files. A second installed-profile export under inherited Modify permissions no longer fails because of an unnecessary delete-child access request.
- Add a repeated-export repository regression: successive snapshots remain separately retained and byte-equivalent for the same project state. Installed archive preview and confirmed import remain acceptance checks before all-view visual proof.

## 1.1.28 — Archive readback and profile truth

- Correct the Projects archive result check to compare the Manager's exact tool name with the action's stable expected name after background argument transfer. Installed 1.1.27 completed an export but displayed “Export unavailable” because the local comparison string was moved.
- Replace the nonfunctional one-choice environment dropdown with a read-only active-profile chip. Sidebar and header now show the actual isolated or production profile and its effective data root, rather than hard-coded Production labels.
- Bind the archive status to the selected project and report confirmed import dispositions separately: inserted, already present, and updated. Source compilation and signed installed interaction are required before visual acceptance.

## 1.1.27 — Project archive preview fence

- Add a full-width Projects archive workflow for exact-project export, artifact selection, non-mutating verification preview, and confirmed import. The import command carries the preview's checksum; the repository rejects a changed valid artifact before a write and requires a fresh preview.
- Read import artifacts from the application's exact project-owned exports directory rather than an unrelated authorized workspace root. The artifact store still rejects foreign project paths, non-immediate children, and unsafe filesystem links.
- Add a cancellable disposable Responses contract probe. Archive output is labeled as memory integrity, not managed-run completion evidence.
- Full Release tests passed (151/151), and a signed MSIX was unpacked and verified before installed interaction. In an isolated installed profile, native folder registration and durable memory creation worked; the Manager completed project export but the archive card's result-binding bug prevented end-to-end UI confirmation. Final all-view acceptance remains open.

## 1.1.26 — Runtime layout and Responses contract readback

- Move exact project-bound managed jobs and stored results into a full-width Runtimes card. Exclude the inventory summary from job-row parsing; installed 1.1.25 exposed a phantom job row despite correct counts.
- Add a separate disposable, model-only Responses contract probe in Provider. It validates an actual response ID and token usage without treating model discovery, a managed run, or a desktop chat as the same result. Clear Provider's stale “Working…” label when discovery finishes.
- Installed 1.1.25 recorded a read-only `agent_list` primary MCP result in the exact Manager deployment after an LM Studio desktop model call and following answer. This establishes only the observed native call, not source-to-successor desktop continuity or trusted completion.
- Full Release tests passed (151/151). A signed 1.1.26 MSIX was unpacked and verified (324 payload files), installed over a checked backup, and inspected in the native app: two exact-project runtime job rows without the phantom summary; Provider discovery and a disposable real Responses ID/usage probe worked. Final all-view and continuity/evidence acceptance remain open.

## 1.1.25 — Project-bound runtime job results

- Project the bounded recent managed-run window into Runtimes using the Manager's persisted run owner. Exact project filtering protects other projects' missions and results; the view reports real active, completed, failed, and stopped states without inventing a queued-job service.
- Show each run's stored mission and result/error in a compact native card, with an exact-run path to Autonomy for verified inspection and supported control. Disconnect and stale project replies invalidate the visible job rows.
- A focused Manager regression covers persisted-state counting and cross-project result isolation. Installed visual and acceptance qualification for this candidate remain open.

## 1.1.24 — Model-only run scope and live registration repair

- Treat LM Studio's own runtime `install-state.json` marker as a valid bridge receipt while still validating the exact deployed manifest, binary, role, home, and deployment ID. A mismatched bridge deployment remains rejected.
- Make native tool access an explicit, durable per-run choice. The desktop starts model-only by default; authorized tool runs still use the canonical Manager catalog and authority boundary. Scope survives Manager protocol readback and run recovery.
- Distinguish provider endpoint failure from an empty model list, clear stale Manager operational telemetry after disconnect, and retain an exact selected Continuity run as awaiting usage when the provider has not returned token counts.
- Report WinHTTP send/receive stage and numeric error on a failed LM Studio Responses exchange. Focused source tests and a signed installed model-only Qwen run passed; final all-view screenshot acceptance and tool-catalog provider exchange remain open.

## 1.1.23 — Deployment-scoped MCP outcome readback and LM Studio identities

- Record the native MCP role and exact deployment ID with newly routed tool outcomes. A nullable C010 migration preserves historical audit rows without manufacturing provenance; focused persistence and protocol tests cover readback and pairing.
- Let Manager distinguish live role-host presence from a bounded, deployment-scoped successful MCP tool audit. The audit proves neither the external LM Studio caller nor durable verified completion evidence.
- Add local model policy/discovery, Manager-configured backend, and sampled local process identities to the LM Studio command center. Labels distinguish configuration, endpoint discovery, prior audited result, and live process state.
- The signed installed 1.1.22 visual sweep reached all fourteen destinations. Autonomy lacked an attached run, Runtimes lacked jobs/results, and Events & Evidence lacked a verified artifact chain; final screenshot acceptance remains open.

## 1.1.22 — Honest MCP role presence and command-center LM Studio view

- Read a bounded, deployment-specific recent presence set from the shared native MCP ledger. Manager status reports a role host only while its heartbeat is fresh and its owning PID is still running from the exact packaged CLI image; a tool call remains a separate proof.
- Give LM Studio's three roles, deployment posture, connection boundary, and actions a Direction A composition. Label host launch and synchronization accurately instead of suggesting that activation itself proves a working client.
- On signed installed 1.1.21, LM Studio's existing Qwen3 Coder 30B model invoked the read-only CLU `clu_capabilities` control and received a native MCP response. The earlier Qwen3.8 27B model displayed the CLU catalog but did not make a tool call. This live result does not yet verify Primary or Fallback calls, installed 1.1.22 UI, or all-view visual acceptance.

## 1.1.21 — Routed non-writing LM Studio activation authority

- Route the distinct Read, Write, and Execute Manager LM Studio capabilities through their exact issuers. The native deployment service and host activator can now authorize application/configuration paths and narrow an Execute authority for activation without admitting Write, Create, or Delete.
- Test the router's Execute and Read-under-Execute paths, denial of Write, generation-preserving narrowing, and rejection of a write-capable activation policy. Signed installed client verification is still required.

## 1.1.20 — Bound LM Studio activation to the native deployment tool

- Use the same `install-lmstudio-plugin` capability name for both Manager-issued repair (Write effect) and connector activation (Execute effect), matching the deployment service's exact tool-name check. The separate Execute-scoped authority remains mandatory for activation.
- A signed installed 1.1.19 activation passed Manager authorization but failed the service's exact-name check before launching LM Studio. Registration of all three roles was observed; connected-client verification remains pending.

## 1.1.19 — Execute-scoped LM Studio connector activation

- Issue a separate narrow Execute-intent Manager authority for activating the already registered LM Studio connectors. Repair retains its distinct Write-intent authority; a Write token cannot authorize an Execute-effect native action merely because Execute is among its grants.
- Keep LM Studio's registration and connected-client states separate. A cold 1.1.18 installed readback observed all three Forge roles in production `mcp.json` with both foreign entries semantically unchanged, but no client was observed and activation was rejected before launch by the previous Write-intent policy.

## 1.1.18 — Isolated LM Studio smoke working directory

- Bind all three pre/post native serve probes to the selected Forge CLI installation directory, not the live Manager's write-anchored data root. Each probe still receives an exact per-operation `--home` under `.serve-verifier`, so it cannot use the production SQLite store or inherit an unrelated workspace.
- Preserve the process supervisor's strict no-write path anchors and update the verifier request-shape regression. Live packaged registration and connected-client qualification remain required.

## 1.1.17 — Bounded native repair diagnostics and visible action state

- Bound best-effort LM Studio repair and deployment diagnostics to a quarter-second child deadline. A Manager-held write-capable data-root anchor currently contests the diagnostic writer's strict ancestor anchor; diagnostic contention can no longer consume the full native repair deadline. The diagnostic storage contention itself remains open.
- Put queued, active, and failed LM Studio action feedback in the hero and Connection & authority card instead of hiding it in Advanced registration detail. Compact the three native actions into one aligned lane.
- Add a regression for the competing database-like ancestor anchor while retaining the diagnostic sink's strict no-write/no-delete root-anchor tests.

## 1.1.16 — Evidence-chain composition and repair tracing

- Give Events & Evidence its own three-stage trust composition while keeping current Manager audit outcomes visibly distinct from unprojected durable artifacts and unperformed native verification.
- Add bounded, local Manager diagnostic breadcrumbs around LM Studio repair receipt, read inspection, authorization, and deployment admission to locate the live pre-transaction stall without logging configuration contents.
- Describe the three Forge LM Studio roles accurately in the desktop action guidance.

## 1.1.15 — Bounded LM Studio repair and retained inspection state

- Give the six-stage native MCP repair workflow a two-minute desktop request deadline instead of the previous thirty seconds; keep the Manager's transport and each smoke probe independently bounded.
- Explain the pending repair check in the UI and retain the last valid host and registration inspection when a command fails or times out.
- Distinguish the discovered LM Studio application from the separate packaged Forge CLI executable in the readiness path.

## 1.1.14 — Distinct sparse-state compositions

- Give LM Studio MCP separate host, registration, and connector-readiness stages without implying a working client when it is absent.
- Keep exact project UUID and repository hash under Advanced; lead with a verified human-facing binding and concise store readback.
- Replace Runtimes' repeated four-line inventory with a Manager-owned execution topology while honestly labeling unavailable job inspection.
- Tighten the Rig middle row for the usable Windows viewport and stop rebuilding the recent-activity timeline on every telemetry tick when its content has not changed.
- Preserve open feature and installed all-view QA; audit activity is still not durable verified evidence.

## 1.1.13 — Cold-launch catalog and installed-version correction

- Start and authenticate the Manager before restoring a page that immediately needs its catalog; keep the empty Tools state compact and explicitly recoverable if readback fails.
- Show the actual installed package version in Manager and locally exported Diagnostics context, instead of relying on a potentially stale compiled label.
- Smooth only the miniature Rig trend readbacks; leave the measured telemetry values and absolute main chart intact.
- Continue signed installed all-view and feature acceptance before calling any screenshot final proof.

## 1.1.12 — Project run history and sharper native readback

- Add a Manager-backed recent-run timeline to Autonomy and Continuity, filtered by the exact selected project and attachable through the existing verified run path.
- Improve miniature Rig telemetry trends while preserving absolute values in the main chart and gauges; show measured CPU frequency in the primary instrument card.
- Tighten sparse Tools/Runtimes layouts and clarify that optional unavailable LM Studio doctor checks do not negate verified core Manager health.
- Add a local, user-chosen Diagnostics support snapshot export containing the bounded Manager readback; it is explicitly not verified run evidence and is never transmitted automatically.
- Keep installed all-view visual, scaled/accessibility and remaining feature acceptance open until signed-release verification.

## 1.1.11 — Operator detail and truthful runtime states

- Correct Provider, Continuity, and Store status colors to distinguish configured or absent dependencies from verified healthy states.
- Bound high-density CPU-core and GPU-engine details into responsive telemetry tiles rather than unbounded diagnostic text.
- Compose Runtimes from the effective shell policy and owned resource readback, with an explicit unavailable-job state and a compact inventory.
- Add Manager-bound project-memory edit and confirm-to-forget controls, project-scoped guided tool arguments, recent agent sessions, and useful Agents/Diagnostics detail cards.
- Reset the shared page scroll position on navigation so each destination opens with its title and primary controls visible.
- Keep installed feature and all-view visual acceptance open pending signed Windows verification.

## 1.1.10 — DPI-aware cold launch

- Convert the intended console dimensions from Windows layout units to monitor physical pixels before resizing the native window.
- Keep the wide left navigation permanently open; show an expandable compact rail after narrowing the viewport.
- Continue signed installed and feature-parity QA before treating any view captures as proof.

## 1.1.9 — Adaptive native console

- Size the native window to its current monitor's usable work area on launch, with a deliberate full-height command-center viewport.
- Use a compact left rail and two-row Rig metrics on narrower displays; keep the full navigation pane and four-card instrument row on wide displays.
- Stretch detailed Settings controls across the available content width and show sub-millisecond tool outcomes accurately.
- Automatically retry selected operational readback after Manager ensure or restart; installed feature and visual QA remain open.

## 1.1.8 — Structured result and configuration composition

- Present native tool outcomes as bounded records with clear completion and invocation duration; retain exact payload in Advanced.
- Use the Manager's measured tool receipt duration in the typed desktop response.
- Compose Settings around effective endpoint, context and shell readback, with detailed sections and destructive maintenance behind clear expansion.
- Keep visual QA open until the signed install and all required feature outcomes are verified.

## 1.1.7 — Guided workbench and density corrections

- Render Manager tool schemas as guided native argument controls; keep canonical JSON in Advanced for complex forms.
- Tighten short Manager/Runtimes inventories, distinguish record counts from live resources, and preserve concise Provider state readback.
- Collapse rarely used project registration, freeing the primary workspace for scope and memory work.
- Continue installed all-view visual QA before treating screenshots as final proof.

## 1.1.6 — Direction A operational composition pass

- Make Projects a selected-scope and memory-health workbench with a native Windows folder picker; keep project identity internal in routine selection.
- Add a model-first Provider screen with bounded discovery of actual loaded LM Studio models and distinct configured versus discovered states.
- Give Autonomy a mission, live run readback, output and token/tool-activity composition; let the Manager resolve project authority and guard run controls by exact project verification.
- Give Continuity a dedicated capacity and handoff-state composition while clearly labeling desktop capability gaps.
- Improve typography, color, feed filtering, agent search, and Manager/Runtimes status cards; installed all-view QA remains required.

## 1.1.5 — Direction A live-view finish

- Strengthen the native Rig hierarchy with a distinct machine identity, full-height meters, a readable 60-second chart, and structured recent outcomes.
- Put run and log navigation in the command center while retaining service lifecycle controls on Manager.
- Continue operational-view refinement; installed production-data inspection identified follow-on work.

## 1.1.4 — Structured native view refinement

- Replace generic operational text dumps with selectable native records, context-specific status headers, detail readback, and an advanced canonical projection.
- Add tool search, pack filtering, capability selection and details while retaining the Manager-owned invocation path.
- Give LM Studio MCP explicit Primary, Fallback, and CLU role status cards; distinguish Autonomy from retained-context Continuity.
- Group persistent Settings into readable configuration, rollover, maintenance, and verification surfaces.

## 1.1.3 — All-view visual finish

- Finished the tool workbench and operational destinations with the same layered command-center cards, structured actions, bounded live-data panes, and monospace technical readouts used by the Rig dashboard.

## 1.1.2 — Command center fit and finish

- Tightened the Rig viewport so the complete operational action cluster remains above the fold at the reference desktop size.

## 1.1.1 — Direction A command center

- Rebuilt the native Windows shell around the selected Obsidian Command Center direction, including dark window chrome, a compact production command strip, high-density telemetry cards, live sparklines, an operational chart grid, status rows, and a consistent premium surface system across every view.
- Preserved the authenticated Manager data path, existing navigation destinations, native command handlers, and release provenance while advancing the Windows package to `1.1.1.0` for a safe in-place upgrade.

## 1.1.0 — Windows product recovery

- Removed the global WinUI busy gate. Live telemetry now coalesces independently while user commands retain a bounded FIFO disposition, and closing the window cancels both lanes.
- Made ordinary GUI startup attach to an existing matching Manager or launch the packaged sibling Manager automatically, then verify the authenticated pipe handshake before reporting success.
- Preserved pending Provider and Settings edits across delayed Manager readback, and derived run/project authority internally instead of asking users for client IDs, generations, or other plumbing identifiers.
- Added the dedicated `clu` LM Studio MCP role with the exact four continuity-control tools, strict closed schemas, fail-closed shared-MCP behavior, three-role transactional deployment, and three-role health reporting.
- Raised the stable runtime and package version to 1.1.0 / `1.1.0.0`; Release packaging remains bound to a clean committed source tree and exact four-executable payload receipt.

## 1.0.0 — Windows production release

- Promoted runtime and package identity to stable 1.0.0 / `ForgeConductor.Windows`.
- Routed ordinary GUI launches to the production `%LOCALAPPDATA%\Forge Conductor` profile.
- Added strict compatibility with the released central schema 9, including immutable C008/C009 ledger checksums, continuation operation tables, and reset-generation metadata.
- Added an independently reconstructed schema-9 fixture that contains no user content and proves byte-stable compatible open behavior.
- Added Windows Release CI and secret-backed production-signing automation.
- Corrected App Installer metadata to reference the generated MSIX and added optional update-manifest generation.
- Updated shipped profile, package, CLI self-test, installer, and documentation language for the finished product.
- Retained `--alpha-root` only as a backward-compatible isolated-profile option.

## Unreleased — Windows Alpha recovery

### Implemented foundation

- Added the native C++20/WinUI 3 desktop host, per-user Manager attachment, typed provider settings, and isolated `--alpha-root` development profile support.
- Added real loopback LM Studio `/v1/models` and `/v1/responses` bootstrap transport, actual response-ID chaining, tool-call correlation, usage accounting, and Manager-owned continuity automation lifetime.
- Removed count- and time-triggered rollover behavior; context consumption is the only automatic continuity trigger.
- Verified disposable project registration, MCP deployment, filesystem/Git/shell/memory operations, project isolation, and preservation of foreign MCP entries through existing native services.
- Added signed x64 engineering MSIX/ZIP packaging with public certificate material and install guidance.

### R0 — Current baseline and accountable delivery

- Adopted replacement plan `windows-alpha-recovery-2026-09-12` without resetting merged foundation work or local Forge Qwen runtime evidence.
- Reconciled merged PR #2 and the real GitHub main, created R0–R7 milestones and phase issues, and migrated the execution ledger to R phase semantics.
- Reconciled all active first-party documentation and classified prior P0–P6 guidance and audit records as historical reference.

### R1 — Manager-owned managed runs and continuity controls

- Added a durable Manager-owned ordinary run service with typed start/status/pause/resume/cancel commands and native GUI controls.
- Completed `/v1/responses` function-call correlation through the authorized native MCP tool router with bounded turns, exact project/run/generation binding, and safe handling of uncertain recovered work.
- Separated lifetime token accounting from retained context, deduplicated provider observations, and returned productive successor response identity to the ordinary run loop after a canonical context-only handoff.
- Fixed isolated first-start Manager initialization by preparing the memory and handoff roots before workspace authority validation.
- Verified the affected Manager, continuity, transport, environment, and native app builds; live LM Studio continuity remains an R6 acceptance requirement.

### R2 — Native operational telemetry and dashboards

- Replaced the production unavailable telemetry adapter with native Windows CPU, RAM, Forge process, and DXGI capability collectors while preserving explicit warmup, stale, unavailable, and unsupported states.
- Added one typed Manager telemetry snapshot across the authenticated named pipe for resources, runtime diagnostics, provider, authoritative run/context/continuity state, projects, tools, events, and store health.
- Added WinUI status cards, CPU/RAM/GPU/context gauges, bounded resource and latency histories, accessible equivalent values, an activity timeline, and telemetry-backed Provider, Continuity, Runtimes, Projects, Tools, Feed, Events, Diagnostics, Manager, and Settings summaries.
- Added two-second coalesced refresh, disconnected/stale rendering, resize redraw, persisted page selection, and window-close cancellation. Real isolated process probes confirmed repeated refresh and Manager survival after GUI close; a later native review verified rendered telemetry, while complete keyboard and scaled/high-contrast inspection remain open.

### R2 parity follow-up — Complete native Rig instrumentation

- Added persistent PDH collection for per-logical CPU utilization/frequency, GPU engine utilization, and physical-disk bytes/operations per second, plus native volume capacity and relevant Forge/LM Studio process measurements.
- Extended the Manager snapshot and strict pipe codec with metric identity, timestamps, availability, cadence, source, memory scope, GPU engines, disks, volumes, and process samples; the GUI remains a typed consumer rather than a second collector.
- Expanded the WinUI Rig with logical-CPU bars, GPU adapter/engine and scoped-memory details, disk/volume and process panels, GPU/disk histories, workflow inventory, measured cadence, sample ages, and honest disconnected gaps.
- Tiered collection at a 250 ms base cadence with heavier one-second and five-second probes, bounded histories, and refresh cancellation during lifecycle actions. Focused collector/protocol/presentation and version-sensitive MCP/infrastructure tests pass.
- Advanced runtime/package identity to `0.9.5` / `0.9.5.0`. The source-bound signed candidate has MSIX SHA-256 `78683d3cef440a190932b8f8cb0fe53b39a6a1ac2940a80c7e4da9e4309a8f22` and ZIP SHA-256 `6bb46f8992599a4d7da5729570011c76cf6cd0657857505ffbb19ca4737b8e91`.
- Inspected the exact candidate under a disposable `--alpha-root`: real RTX 4090 engines, 32 logical CPUs, three volumes, live disk rates, relevant processes, workflow counts, disconnect state, Manager start/reconnect, and native keyboard focus were exposed. High Contrast passed. A 150% text pass found status-banner clipping; the layout was repaired, rebuilt, repackaged, and then passed a top-to-bottom 150% walkthrough with host settings restored.

### R3 — Native operating pages and local workflows

- Added typed Manager project-list, folder-registration, memory-search/read/write, and persistence-status operations over the authenticated named pipe.
- Replaced the Projects placeholder with native registration and stable selection, authorized-folder and storage-health views, full memory records, and persisted exact-ID binding into ordinary runs.
- Added project-scope fencing for Manager memory responses and focused two-project isolation coverage; a fresh isolated native workflow retained memory after restart without leaking it to the second project.
- Incorporated `continuous-delivery-repair-2026-09-12` into the adopted execution, Git, closeout, and handoff guidance so phase checkpoints and unavailable inspections do not stop independent R0–R7 work.
- Added native LM Studio MCP repair/activation, exact tool catalog/invocation, agent/session actions, audit feed, runtime, diagnostics, and Manager pages backed by typed Manager operations.

### R4 — Accessible settings and scoped maintenance

- Replaced the Settings placeholder with labeled native controls for dashboard, Manager lifecycle, LM Studio model discovery, logging, shell policy, session retention, and context rollover thresholds.
- Added paired context sliders and exact token values, pending-edit revert, provider testing, effective readback after save/restart, and direct links to focused Provider and Manager pages.
- Added typed Manager maintenance for exact-project memory, continuity, combined project data, and separately confirmed all-project data. Resets reuse transactional repositories, close old project generations, preserve source folders, and report affected counts.

### R5 — Signed installer and data compatibility

- Established stable product version `0.9.1` and MSIX identity/version `ForgeConductor.Windows.Alpha` / `0.9.1.0` across the native hosts and package manifest.
- Bound the GUI, Manager, CLI, and SessionHost Release binaries to one committed staging receipt; added complete WinUI/runtime/resources payload validation, embedded provenance, per-file hashes, signature/certificate checks, unpack-and-rehash verification, and a minimal distribution bundle containing no private signing material.
- Added strict install/update validation for package, certificate, identity, publisher, and increasing version while preserving user data through normal MSIX update/uninstall semantics.
- Added a dedicated Manager exit code and actionable GUI explanation when the central store is newer than supported. The schema-9 disposable probe leaves the database unchanged and preserves `--alpha-root` as the explicit isolated alternative.
- Applied the owner execution correction in place: native compaction continues the current slice; draft, checks, readiness, merge, and acceptance are separate; and a single reused continuation is allowed when a primary phase PR merges early.

### R6 — Candidate acceptance and defect repair

- Fixed Alpha profile view-state isolation so project/page selection cannot leak between disposable roots, stale project IDs clear against the Manager snapshot, and an unrelated first project is never selected implicitly.
- Advanced the product and stable MSIX identity to version `0.9.2` / `0.9.2.0`, then built, signed, unpacked, and rehashed the complete committed Release package.
- Exercised the exact unpacked package: CLI version, GUI and Manager paths, live CPU/RAM and explicit GPU capability state, single-Manager detach/reattach, and independent two-profile project selection all passed.
- Completed the native Settings accessibility work with keyboard operation of the context slider and exact token value, actual Windows High Contrast, and actual 150% text scaling; every temporary system setting was restored.
- Retained installation and live-provider gates as blocked: Windows returned `0x800B0109` without machine-level publisher trust, and LM Studio was still unavailable at `127.0.0.1:1234`.

### R6 continuation — Live managed continuity

- Repaired the repository-backed managed-run start transaction so each new run creates the admissible open session and active binding before it enters the running state.
- Moved continuity observation after persisted native tool effects, retained bounded completed-work summaries in the canonical handoff, and instructed the fresh successor to continue without repeating those effects.
- Removed the remaining ordinary-run turn cap; completion, operator cancellation, a real failure, or context-triggered continuity now determine when managed work stops.
- Moved the native session ledger under the memory root and added legacy-root migration before SQLite opens, preventing the ledger and central database from sharing an atomic-replace directory.
- Updated LM Studio bootstrap tool selection to its supported required mode and proved a real context-triggered rollover with authoritative usage, saved handoff, fresh provider root, structured acknowledgment, predecessor fencing, and useful successor filesystem effects.
- Advanced runtime/package identity to `0.9.3` / `0.9.3.0`; built, signed, unpacked, and rehashed the source-bound candidate. MSIX SHA-256 is `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`.

### R6 acceptance closeout — Managed continuity guard

- Corrected the earlier rollover record: the terminal context-budget block was emitted by Forge's legacy desktop-chat invocation guard after repeated native calls, while `PACKAGE_OK` belonged to a separate exact-package run.
- Exempted Manager-owned `managed-run-v1` traffic from that legacy repetition/handoff guard while retaining normal routing, authorization, and audit behavior; ordinary MCP desktop chats retain their existing protection.
- Added a 12-identical-call guard regression and reconciled the managed-run service regression at 80 tool turns. The five focused suites and full x64 Debug/Release product builds passed.
- Advanced runtime/package identity to `0.9.4` / `0.9.4.0`. The source-bound signed candidate has MSIX SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`.
- Ran the exact 0.9.4 GUI and Manager against real LM Studio. Run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed canonical rollover to successor `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed the remaining effect, and returned terminal text `DONE` without the legacy block.
- Preserved the four retired remote branch heads in one verified Git bundle before guarded deletion. Added an exact administrator/test-account handoff for the still-open installed lifecycle; no elevated trust workaround was attempted.

### R6 internal Alpha completion continuation — Installed evidence integrity

- Added `-PreflightOnly` to the engineering installation helper. It validates package, certificate, signer, machine trust, prior version, and update eligibility without registering the package.
- Require the exact development publisher in Local Machine Trusted People before deployment and report the authorized administrator action when it is absent.
- Write a unique timestamped JSON receipt after every successful install or update so lifecycle evidence cannot overwrite a prior result.
- Rebuilt the source-bound 0.9.5.0 candidate from commit `c77c45386b25d7b76270c3685b79c172f41526c8` and tree `2bb16d60c7616f3d6f31ec94c85192dfe30db349`. Its MSIX SHA-256 is `9c772fcc9646f1e876f83c59c9e59a189f6f6881bcd603eb290dd589d5129a74` and ZIP SHA-256 is `2874a1cdf51d6861e780a32b599172f9ca0d84fe9ad9a9bb5eb92b55e3bb8dd4`.
- Made conflicting `-PreflightOnly -TrustDevelopmentPublisher` invocation fail immediately; the focused regression proves that neither certificate import nor package deployment is called.
- Changed ordinary GUI startup to the durable `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile. Its Manager lease, named pipe, DPAPI token, view state, projects, memory, and continuity stay separate from the preserved legacy store, and a persistent header identifies the profile and exact path on every page.
- Recorded the owner's decision to defer schema-9/C008/C009 migration as nonblocking backlog while preserving the legacy database and related files unchanged.
- Rebuilt and fully rehashed the persistent-profile MSIX from application commit `dab23aa8555a37203ba11136c58bb7f799317356`; its SHA-256 is `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`. The final companion distribution records application and distribution provenance and has ZIP SHA-256 `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`.
- Added bounded packaged-Manager startup diagnostics so an early child-process failure reports sanitized stderr/stdout instead of only a generic exit code.
- Fixed MSIX AppData virtualization for the durable Internal Alpha profile with one narrowly scoped manifest exclusion and a focused regression that fences the legacy profile out of that capability.
- Verified exact machine trust, read-only preflight, current-package registration, Start launch, WindowsApps GUI/Manager identity, durable registry survival across reinstall, absence of package-private profile storage, and the installed 53-tool/project/isolation/filesystem/search/Git/shell/memory-restart workflow. The disposable 0.9.4→0.9.5 update and uninstall/reinstall lifecycle remains open.

### R6 simulated acceptance closeout

- Added a reproducible isolated lifecycle that rehashes all 323 manifest-listed files in both retained candidates, validates both signed packages, stages the real 0.9.4 then 0.9.5 CLI/MCP payloads, and verifies version/self-test behavior.
- Proved settings, workspace files, two-project isolation, legacy memory, project memory, and an unrelated LM Studio MCP configuration survive the simulated upgrade, uninstall, and reinstall. The owner explicitly accepted simulated testing for this remaining gate; no actual disposable-machine package deployment is claimed.
- Hardened the full Release test matrix against Windows excluded TCP ranges, asynchronous shutdown publication, Release-disabled managed-run assertions, and the current central schema version. All 150 configured tests pass.
- Accepted the signed 0.9.5.0 distribution for Internal Alpha. Legacy schema-9/C008/C009 migration remains deferred, and the preserved legacy profile remains untouched.

### R7 — Documented delivery

- Reconciled every active first-party document with the implemented capability, exact acceptance evidence, honest limitations, and current PR dependency chain.
- Retained and independently rehashed the unchanged tested 0.9.2.0 candidate: MSIX `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`, distribution ZIP `18e43f5508499b56ec802447cfb98dfe8bfc048ba1f649eb1da657f0ae28f8dd`, source commit `d8a2d68c80f2fd090aa36466a517725a0eb59445`.
- Kept the installed lifecycle visibly open. The later owner decision defers legacy schema-9 migration outside the first Alpha gate. PR #22 merged at `35f61de3c5a8159e20752a3843e5f26bfa5290c9`; PR #23 records the earlier acceptance closeout.

### Remaining Alpha work

- No required Internal Alpha functionality or acceptance work remains. Deliver this tested closeout through the normal pull-request and merge workflow, then synchronize local and GitHub `main`. Legacy schema-9 migration remains deferred outside the accepted first Alpha scope.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
