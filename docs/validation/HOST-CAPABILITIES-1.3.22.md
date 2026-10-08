# 1.3.22 capability qualification — superseded installed candidate

Product/package identity: **1.3.22 / 1.3.22.0**. Final source commit `9178abdf12998af56df4860a56b25dccfcc30e10`, tree `b5e9fdedc9cb6b7ea0f97f9d93ef2e2d2f313d64`. No final release qualification, tag or publication is asserted. Current candidate [1.3.23](HOST-CAPABILITIES-1.3.23.md) has separate pending checks.

## Final observed scope

| Check | Actual retained result |
| --- | --- |
| Product All | Passed against the clean final source |
| Complete configured Release CTest graph | 166/166 passed in 97.71 seconds |
| Static gates and package persistence | All three gates and persistence passed |
| Signed 1.3.22.0 distribution/install | Valid development signature; all four staging/MSIX/installed executable hashes matched |
| Offline prelaunch preservation | 8,431 files / 410,168,202 bytes and conversation, selection, configuration and MCP snapshots preserved before launch or explicit setting changes |
| Actual selected Qwen Primary calls | Successful session_handoff and two get_forge_status results reporting version 1.3.22 / 112 tools |
| Explicit route recovery | Stored model packet 7f06a8b2-a102-4f82-85ae-93aa6d3a7db4 at write sequence 8; exact original encrypted revision-2 archive, followed by Creating revision 3 |
| Later native handoff | Revision 4 retained uncertain delivery/new_chat; selected new conversation had zero messages, delivery/context recovery false, no confirmed successor |
| Later assisted readback | Manually seeded Primary context_get/get_forge_status advanced the same packet to revision 6/resumed, confirmed New chat and delivery/context-recovery flags; this is not automatic packet Send proof |
| Connector lifetime capture | All three CLI and bridge-parent retained handles exited with code 1; exit cause not established by those codes |
| Completed predecessor preservation | 84 messages, final eosFound, original complete 78-message prefix preserved |
| Installed complete catalogs | Not qualified; catalog-v3 capture failed before authenticated HTTP |
| Integrated public/native image-provider acceptance | Not qualified; isolated smoke remains separately scoped below |
| Private Manager cleanup | Normal exit 0 independently observed through retained handle; cleanup does not qualify catalogs or images |
| CI and publication/readback | Not qualified; no 1.3.22 public release |

## Final receipt seals

Paths below are retained private evidence names under capability-parity; they are identifiers, not public download links.

| Receipt | SHA-256 |
| --- | --- |
| release-clean-final-1322-9178abd/all_product_build/all_product_build.receipt.json | 2e3ece927702b131b26d832a25fd2e8ab9f537190ab3534900f2ef1844b86c48 |
| release-clean-final-1322-9178abd/complete_release_tests/complete_release_tests.receipt.json | f1af54dcbf69429545e1455c4afcf92ec7678c473f6b3c09cea417e078a7fa9a |
| release-clean-final-1322-9178abd/static_gates/static_gates.receipt.json | f89b2ff7c2fdb51df4a3493803c721744e6fad8cc5286003ed8546e642a42364 |
| release-clean-final-1322-9178abd/persistence/persistence.receipt.json | b92aeced98baf8a1aa18107b4a825bace4918320f43c04b51e925840aaeef834 |
| release-clean-final-1322-9178abd/package/package.receipt.json | 06175e8732f9eb0a02bf44de61c405006d3f2213fefd0bd77da78d30c7a561a8 |
| installed-release-qualified-1322/installed-verification.json | a5a6befbf33484e06decba20c22699433efbf97c7f881aeae2bb43a4df88beca |
| installed-catalogs-qualified-1322-final-v3/report.json | 1cbc436537ee5562a203667ebe88d8fcd3c81d3dc4842c67ee7e0dc2f093556d |
| native-continuity-watch-1322-9178abd-next/watch-summary.json | 38f676ca8ccc3b6f500f9dbfa8fd964cfaa42627f4110fb0b7ab4838a81590cf |
| native-connector-lifetime-1322-third-probe/result.json | 930713bb8c4c66c5e530955237d793b6302a460fc02bd6f792909fc2d14a3608 |
| actual-empty-native-new-chat-1322-0533/snapshot.json | 74f3dc42d0a49ed7f9858655ad42aba27d9789e4b3f30b750a24eba8111cc85e |
| native-third-probe-completed-predecessor-1322-0533/capture.json | e8a8808ae554b3d6166ea6839ccf7bd0de1185924cef7636d9f6e90ae60d7ac0 |
| native-third-probe-completed-predecessor-1322-0533/conversation.json | a1808e4b85b77e8e79b36649c94243d01d93cdac15d2eac19ec18931889ab3ab |
| native-assisted-recovery-watch-1322-0542/checkpoints/0e03af722e69f8299e043102bf96f32cf7c6e029009f648515e408dfad527462.json | 3a88e065ae387f3deace8dff635738941aaa86a0ce08fc0edea408aa51655f60 |
| private-manager-control-preparation-1322-v1/actual-runtime.json | 0ea8318e1d3c490c16f0f274e593df5d782e8a8f23ad849bf1d73d4b9fe77ba6 |

The selected Primary native request/result pairs use actual call IDs 1867938446390454 (session_handoff), 1867938446390455 and 1867938446390456 (get_forge_status). They do not establish all three catalog roles or the six provider tools. The 30-minute passive watch observed three checkpoint versions and zero read errors; only copied revisions are evidence. Revision 4 was observed at 2026-10-08T05:32:58.591238Z. The three CLI/bridge pairs exited at 05:33:00.125–126Z. The selected new Qwen conversation 1791437581203 had zero messages. Its existence does not prove packet delivery or safe replay. The exact encrypted archive retained SHA-256 c9786acbe59fa91fa9d234894456f58e986b56cf2c0a99b9ba31108babdd330c; the watch receipt supplies the authoritative original digest. No independent raw SQL crash-durability claim is made.

The unpackaged private registry view was absent and the packaged view was present. The package-only existing nonce allowed normal private Manager shutdown with independent retained-handle exit 0 at 2026-10-08T05:39:25.055580Z–05:39:56.630606Z. It did not establish a successful catalog HTTP capture. The installed owner Manager remained a separate process. Owner state was not altered by documentation work.

The later assisted attempt used a manual short seed requesting the existing packet ID and then status. The observed revision-6 checkpoint at 2026-10-08T05:42:30.543720Z reports phase resumed, the same packet/write sequence, successor 1791437581203, confirmed New chat, delivery_acknowledged/context_recovered true, and packet_request_acknowledged/repair_acknowledged false. The watch saw revision 4 then revision 6; revision 5 was not captured. This establishes the retained assisted reconciliation state, not automatic full-packet Send or all intermediate stages. The model generation had not reached EOS when this observation was recorded. It supplies no automatic 1.3.22 rollover qualification and no 1.3.23 proof.

## Preserved chronological pre-final records

The original record below retains the earlier failure, timing diagnosis, focused source checks and isolated provider measurements. Statements about pending checks describe those stages; the final observed table above is the later source/install result. These earlier measurements are not rebound to 1.3.23.

# 1.3.22 capability qualification — pending integrated release checks

The source candidate identifies **1.3.22 / 1.3.22.0** and defines **112 Primary/Fallback tools**, with five CLU tools and every previous descriptor retained. Canonical descriptor SHA-256: 8c8773f8880fd5925b6da2d1a526c85889eb25f715c0dfaebbfa7456ca409508.

## Qualification boundary

| Check | Current evidence |
| --- | --- |
| Final qualification source commit/tree | Pending clean commit after changed source; preserved earlier clean-source and final focused file bindings are below |
| Complete Product All and configured Release CTest graph | Earlier clean source: Product All passed; Release CTest 165/166 passed, one failure in 110.15 seconds. Fresh final-source checks pending |
| Final integrated static gates and package persistence | Earlier clean source passed all three static gates and package persistence; fresh final-source checks pending |
| Signed 1.3.22 package and four staged/MSIX/installed payload matches | Pending |
| Installed Primary/Fallback/CLU catalogs | Pending; 112/112/5 is the source contract, not an observed installed result |
| Native LM Studio generation/edit/status/cancel/resume | Pending |
| Shared native chat snapshot correction | Earlier narrow Infrastructure 124/124 groups passed, including five real-file reader cases; final focused Infrastructure passed 125/125 groups. Installed caller qualification pending |
| 1.3.22 publication/readback | Pending; no release assets asserted |

The installed 1.3.21 connector's actual read-only native inventory still returned 106 tools without the six new provider names. Model prose cannot replace that native result or an installed upgrade. Drawing through image_write and analysis through image_analyze/reviewer_status are distinct from provider generation jobs.

## Preserved clean-source run and full-suite failure

The normal release scripts ran against clean commit 5b8c5242a0862a32654284c36c4672b58db4d70a, tree ef5625da5482c7cb298ee37d558e64a7ac8440b7. Product All passed at 2026-10-08 03:37:09Z–03:39:50Z; its staging manifest SHA-256 is 421118915594ea9771010c8930f4a717395c1d822acd31d8aaef31f7cc7f2013 and build-log SHA-256 is 44a8351c095a556a6d493cd4555b3eca755d426e128623b1f57bc724e30120fc. All three static gates and package persistence also passed for that identity.

The normal complete Release CTest graph passed 165 of 166 entries in 110.15 seconds. ForgeConductor.NativeTools.CMakeTestTests failed with “Near-bound CTest parse published counts after cancellation; elapsed_us=64145.” The complete-test log SHA-256 is 677128a2443e22a7ad1a6098caeee869a3131c28fd2a53853ad1b94e163dd6c7; LastTest log SHA-256 is 016959ea5dc4312cb7a525c0a5be82ea0c91bc3831eef7b40c9e1ac8c3912677. Earlier narrow passes in 9.82, 9.92 and 9.62 seconds do not erase this full-suite failure.

A separate private timing diagnostic preserved unchanged product inputs. In trial 9, parsing returned at 44.5785 ms and cancellation started at 47.8282 ms, demonstrating late cancellation in that reproduction. The original failed test did not retain cancellation timestamps, so its exact ordering remains unknown. Diagnostic-results SHA-256: 84e8f06d36562ee1f02ec9b89f9489d736bfdc3cff0f1d885690799744a8539c. The test-only correction now waits for cancellation-worker readiness and records stop timestamps. It allows at most ten attempts and requires an actual during-call Cancelled result; pre-call or late cancellation cannot satisfy that assertion. Parser production behavior is unchanged. The narrow result for the changed source is recorded below; these earlier clean-source results do not qualify it.

## Final focused source checks and route-recovery contract

The final focused build and two CTest targets passed on the modified working tree based on HEAD 5b8c5242a0862a32654284c36c4672b58db4d70a / tree ef5625da5482c7cb298ee37d558e64a7ac8440b7. That base identity is not a clean commit of the changed files. Infrastructure passed 125/125 groups in 54.53 seconds, including 37 unique explicit route-recovery cases; CMake/CTest passed in 9.91 seconds, for 64.45 seconds total. The intermediate two-target pass with 35 recovery cases in 62.33 seconds belongs to its earlier file snapshot.

Final focused evidence SHA-256: receipt af7d926b625238897f2e727a1fb1e28e92cf69a2ccce8fb7638354f66365473c; build log 1b1f0d91ef3196d9b60b8ca3ea386461780e665b0d58892a3a76e7b080f215cc; test log 80cb349c62dcfb9b3ce02b42aa931e6a39423aaaa50d366f8774c8147b263178; LastTest log 175698efa6a4ae37ad2c89ada565a991f529ffadd21821aa96fe9f7c06566132. Before/after hashes of all five changed source/test files matched:

| Source/test file | SHA-256 |
| --- | --- |
| WindowsLMStudioChatContinuity.cpp | 7fed2219a614c5b5f304e50032eb96584aea713e2d2eae7d988d8f8b74ebf8b1 |
| LMStudioChatCheckpoint.cpp | eb8a898621206c86424a778b80fec1465e1269ef672e1fdc208313267fcda928 |
| LMStudioChatCheckpoint.h | 6b0f297760f105e9ab4814d80d7310cd4ca4c1413e0fb8bcfdec4af2e875cc4b |
| WindowsLMStudioConversationReaderTests.cpp | 73d87e3404c859f5bec2895a2b0182535ac2c0c6b35ac9518bf7c4db706744a2 |
| WindowsCMakeTestServiceTests.cpp | 462ffa6bd4abcded098c0f68b3eb8b5bf8ae089765737ee800ae61183ca74696 |

The existing authorized Primary session_handoff can request narrowly gated route-upgrade recovery. It requires the matching successful selected-native Primary result, a newer complete model packet and current project pointer, and fresh full three-route/executable/home/workspace/provider checks. Only an undispatched WaitingPacket state with no effect, acknowledgements, new packet, successor or repair qualifies. Original encrypted bytes are archived before current-scope promotion; the old request acknowledgement remains false. Default scope mismatch, uncertain/confirmed effects and missing evidence still defer controls without replay. Planned Creating reconstruction revalidates the complete retained model packet. Catalog schemas and the 112-tool inventory are unchanged.

These private source fixtures do not establish installed current-chat recovery, UI rollover or Qwen acceptance. Fresh complete Product All/166-test, static/persistence, package and installed/native checks remain pending for the eventual clean source.

## Isolated source and automated checks

The feature was developed from commit 1900b16d37f84314fee7a99cdabffbe324f13bf2, tree e666c2d2484f530df530df664212c6c09c6a269d. Its 37-file pre-commit source manifest has SHA-256 e697309b5ca80a1b4b4e4bcd7c407564e718256ae4ad1cbe9960a43a3dc4e23f. Ten focused CTest entries passed in 118.74 seconds; the service target covered 15 cases, HTTP eight and codec six. Three static gates passed. Full integrated release checks remain separate.

Feature commit: 07bc6669b007e8d3546c218cf2ca7a769b5ef83f, tree 43a7db3b1fcf294f6dc157a5012f27ff57e45f15. The commit trimmed one extra blank EOF line in the new HTTP test after staged diff checking; the original file was archived. That whitespace change is not retroactively part of the earlier manifest or binary evidence.

Earlier failed build attempts and the reproduced cache/control-identity failure remain retained. The unstaged diff check did not cover the then-untracked new files; staged checking supplied that additional surface. The focused pass does not erase those limits or qualify a new integrated source identity.

## Root-owned real ComfyUI smoke

The explicit endpoint was http://127.0.0.1:8188, profile sd1, checkpoint cyberrealistic_final.safetensors. The smoke ran at 2026-10-08 03:04:19Z–03:04:33Z, returned exit 0, and used direct native services and real WinHTTP with private fixture-issued scopes.

| Frozen evidence | SHA-256 |
| --- | --- |
| Native smoke executable | b515c5e81ec480b473c1ed17d26f16be7d5c0644a9b07561e97c3e540e8d2c88 |
| Source inventory | e697309b5ca80a1b4b4e4bcd7c407564e718256ae4ad1cbe9960a43a3dc4e23f |
| Runtime log | 0a663ba56928bb4ede7921865fe4ef7d2b752d60eba246573a28dd929333b3f3 |
| Smoke report | 5f3b34ebd8ae3bccacf090bbb360cb123245692bb45e07856ac49538c593bfae |
| HTTP metadata trace | eab731c2e9cc61142cb8890efb323a65a9afc16950008e4178cfa77b9495b16d |

The command receipt separately binds executable and source inventory. The reusable raw report intentionally records source_identity_bound:false and null embedded source hashes; read it together with that wrapper.

Six cases passed. The 180-event trace records five exact generation POSTs, three uploads, no queue mutation and no global interrupt. Independently reconstructed prompt bodies match one trace request each; all five canonical durable payload and graph seals match. Four single-frame 128×128 PNGs match file and decoded RGBA hashes: generation 24,971 bytes, unmasked edit 37,897, masked edit 19,110 and recovered generation 22,600.

The mask has 8,192 red-zero and 8,192 red-255 pixels. Independent decoding verified every red-zero output pixel equals original RGBA, including alpha 0–190; all 8,192 selected pixels changed. This establishes exact native composition, not semantic instruction following.

Running cancellation suppressed local publication and left no cancellation output. The exact remote job subsequently finished; remote termination was not claimed. Before each later submission, the helper required terminal history for the same prompt ID and workflow. Recovery deliberately withheld an actual matching HTTP 200 acknowledgement, preserved an unpublished unknown result, reconstructed the service, read status without publication, then explicitly resumed the same prompt with one total generation POST.

The original smoke metadata trace retains body hashes rather than full queue/history responses. Running-time observations therefore remain runtime helper assertions. A separate independent post-run readback at 2026-10-08 03:24:15Z captured seven GETs and zero POSTs: all five exact prompt histories had terminal success and full graph dictionaries matching durable receipts; queues were empty before/after and the actual provider PID/birth/SID/command/listener were unchanged. Its receipt SHA-256 is e385186e84048ad625a908a6e133b0cf8593b4cf87b2011bb0dc3340180cd844. This qualifies current post-run history, not a replay of the earlier running response. Before/after status manifests were checked at runtime; only the final 20-path manifest is persisted and independently matches the 14 file byte/hash records. Reconstruction occurred within one process. The injected acknowledgement fault is not a real network failure.

Resource bounds were 128/256-pixel outputs, 2/4/6 steps, five owned submissions maximum and one owned remote job at a time. The 180-second timeout applies separately to each job/admission/observation/drain phase, not total helper lifetime. Provider inventory alone does not establish quality or arbitrary checkpoint compatibility.

## Shared native snapshot source check

The narrow Infrastructure CTest entry passed 124/124 groups in 21.94 seconds (21.95 seconds total). Five new real-file reader groups cover a 33 MiB input byte-for-byte, exactly 64 MiB, over-bound refusal without mutation, cancellation/deadline and all seven revision fields. This checks the shared reader and revision predicate; it does not exercise actual Windows UI model acknowledgement, plugin cleanup, a native completed-tool-boundary pause or concurrent mutation during the read.

| Frozen narrow evidence | SHA-256 |
| --- | --- |
| Build log | 1722d1b641169c43076aab0344800859a2f433eb8d34ff0fbd10b30914ae58d1 |
| Runtime log | 02c7aa2254fa12c13484785678745709d31a3a31d7b4bd14dcaed450c8f4ea54 |
| Detailed test log | 670b6e812ea1edbbd165bce0a5cc6fd33709323b8381a96329a658e66dc30e25 |
| Infrastructure executable | 92a5becc64e2e4f893ca84ad701fb5d452bb878ce5281ac85218120f5e343176 |

The changed helper is shared by plugin cleanup, loaded-model acknowledgement and completed-tool-boundary corroboration. The frozen large current chat exceeds the old 32 MiB bound, but the exact live failing caller is not established. Installed current-chat recovery and the complete source graph remain pending.

## Preserved published evidence

The [published 1.3.21 record](HOST-CAPABILITIES-1.3.21.md) remains bound to source 39adf553df320120d214bd90cc49223359054ede, tree c1b295cae2c0a01ce334526d99af6bc3e7b3a243: 163 source tests, installed 106/106/5 catalogs, 13 verified native cases, 118 Infrastructure groups and 20 durability cases. Publication readback at 2026-10-08 03:07:15Z verified release 406365311 and all seven asset digests. These results do not qualify 1.3.22.

Named 1.3.19 and superseded 1.3.20 records keep their original tests, hashes, artifacts and limits. Full host image-model parity, installed observer interruption, physical context exhaustion and already-running agent reattachment remain unqualified here.
