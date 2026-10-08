# 1.3.25 capability measurements — superseded, unpublished

The **1.3.25 / 1.3.25.0** installed candidate is **unpublished and superseded**. The source-bound measurements below are historical; they do not qualify a 1.3.26 artifact.

The installed, unpublished **1.3.25 / 1.3.25.0** artifact at source `5708cb9c52ed2f2dd10b3a500b57c7ffff6c6f7b`, tree `fcd027b9bf82bc88673a3ee43069973c1600a831`, passed Product All, all three static gates, package persistence and **167/167 local Release tests in 146.24 seconds**. Its signed installation verified all four executable images and **323 payload files**, preserving **8,553 profile files / 422,592,105 bytes** and the four protected snapshots before launch. Exact-source Windows CI run 37763301666 failed **165/167 in 147.41 seconds** at DesktopArtifact accessibility connection and Infrastructure diagnostic rotation checks; the later CI static-gate/upload steps were skipped. Local success does not replace that failed CI result. Actual native assisted readback reported 1.3.25 and 112 tools; it did not establish all-tool runtime qualification or automatic New chat/Send. Full controller diagnosis remained open, and no 1.3.25 release was published. These observations do not qualify 1.3.26.

## Actual automatic attempt

The captured installed automatic attempt preserved every field of the original **86 messages** in the **59,661,455-byte** native conversation. An **18,697-character** unsent draft exactly matched the traced automatic packet request. Checkpoint revision **14** retained `WaitingPacket`, `effect:null` and a false packet-request acknowledgement. The capture reported no confirmed request Send, no New chat dispatch and no new native tool dispatch for this attempt. It does not establish EOS, a saved new model packet, a successor or automatic delivery. The preserved failure receipt has SHA-256 `8075fd1adeb47c96709d644ac08927dba8074e904e2147537b185ac8c038f6fb`; the native file has SHA-256 `ced8d7316db1ea04bde709cb53cf8424afbb7140dccbac120fd95fa329463547`. This capture does not determine the deadline failure by itself; the later guarded timing comparison below diagnoses that boundary. No query optimization is integrated.

## Retained source contract

The cached `tokenCount` projects LM Studio's complete rendered prompt. Continuity evaluates the larger of admitted cached count and actual latest provider usage at initial observation and again after a confirmed pause. `cached_rendered_prompt_tokens`, `pressure_tokens`, `pressure_source` and `pressure_headroom_tokens` remain separate from actual `tokens_used` and `headroom_tokens`. Cache admission requires the current selected generation/model identifier and equal loaded capacity; unknown, malformed or mismatched evidence is not admitted. LM Studio refreshes this cache around its outer prediction and does not attach a generation timestamp. Physical overflow remains distinct.

## Private controller comparisons

Private read-only comparisons retained their C570 source and copied-library identities. The earlier baseline idle calls measured **1,841.9938–3,157.5090 ms**; typed-cache searches measured **6,473.7840–7,603.1633 ms**, and reversed-condition searches measured **1,838.8690–3,247.9454 ms**. All calls returned idle without input actions or owner writes. Neither candidate supported an idle latency improvement, and neither is integrated. These sequential private comparisons are not a Send profile or automatic-rollover result; neither is a completed Send result; the later guarded comparison diagnoses the caller-budget boundary.

Guarded timing used the same C570 controller and protected native inputs. With the original **20-second** caller budget, Send preparation returned `deadline_exceeded` after **20,375.0247 ms**, before the receipt callback. With a fresh **25-second** caller budget, it reached `BeforeDispatch` after **20,363.4627 ms** and the private callback deliberately refused dispatch with `conflict` at **20,367.1673 ms**, leaving **4,632.8327 ms**. Both captures preserved the protected native inputs; neither invoked Send or New chat. This demonstrates the caller-budget boundary on this measured path, not completed native submission or a general latency guarantee.

## Measurement boundaries

| Scope | Actual 1.3.25 result |
| --- | --- |
| Clean C570 Product All/static/persistence | Passed; original receipts retained |
| Local complete Release graph | 167/167 passed in 146.24 seconds |
| Exact-source Windows CI | 165/167 passed; two failures; 147.41 seconds; later static/upload skipped |
| Installed higher-version package | Four images/323 payloads and 8,553 files/422,592,105 bytes preserved before launch |
| Selected native assisted readback | Actual 1.3.25/112 tool result; not all-tool or automatic qualification |
| Automatic model packet request | Exact 18,697-character unsent draft; no confirmed Send/New/native tool dispatch |
| Query optimization comparisons | Rejected; private idle-only scope, not Send qualification |
| Guarded Send timing | Original20-second caller expired; fresh25-second caller reached refused BeforeDispatch; noSend |
| Complete public/native acceptance/automatic rollover | Not qualified |
| Release publication | No 1.3.25 release published |

Private working-source CI and pressure changes after C570 are separate source checks. The root-reported two-target run passed in 87.01 seconds while its binaries still compiled version 1.3.25, before the 11 active identity inputs were moved to 1.3.26. That run is not a clean 1.3.26 artifact qualification.

See [current 1.3.26 pending qualification](HOST-CAPABILITIES-1.3.26.md) and [historical 1.3.24 scopes](HOST-CAPABILITIES-1.3.24.md).
