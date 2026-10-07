# Unpublished 1.3.18 host workflow investigation

This record identifies the **unpublished, superseded 1.3.18 / 1.3.18.0** candidate. Its complete release qualification was not achieved; no tag or publication followed. The [1.3.19 candidate](HOST-CAPABILITIES-1.3.19.md) requires fresh source, payload and native-model evidence.

Source `ffd10dce9125bf700ad5efbfd1228475704cc792`, tree `2781ec95847694ff4225471a8655fa64632bb11e`, passed **162/162 local Release tests in 78.61 seconds**, Product All, all three static gates and package persistence. [Windows CI 37657943054](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37657943054) failed **161/162 in 115.37 seconds**: the sole failing WinHttp transport assertion required `receive_timeout_ms=1000`. CI did not print the returned timeout message, so its exact error branch remains unknown. Twenty unchanged-source isolated repetitions passed locally and do not override the CI failure.

The signed 1.3.18.0 upgrade matched all four installed/staging/MSIX payloads and preserved the complete **8,256-file, 401,730,716-byte** owner profile and protected snapshots. Eleven actual current-chat native calls succeeded across status, Host read, owned directory/write/readback, native Office and desktop observation/capture. The complete nine-case acceptance remained unfinished. A separate actual managed recovery returned the exact `found:true` packet and file contents, but the first probe failed its final narrative-format assertion and remains unqualified.

A final preservation readback verified all **1,199 original workspace files / 12,663,710 bytes**, Git status, the complete prior conversation prefix, owner configuration and selection, foreign MCP semantics and all **730 foreign plugin files**. Project-policy differences were limited to verified appended evaluation history; no project gate was executed or approved. The incomplete observations and failed assertions remain retained. No final release qualification, tag or publication followed.

## Source and timeout diagnostic observations

The normal local suite passed 162/162 in 78.61 seconds; the independent Windows CI suite failed only `ForgeConductor.SessionHost.WinHttpTransportTests`. The failing assertion required `receive_timeout_ms=1000`; prior assertions required the actual deadline error and completion within two seconds. The exact returned CI message and timeout branch were not logged. Two native WinHTTP error paths recalculated remaining operation time under that diagnostic label while the request guard used the selected budget. This source inconsistency supports a diagnostic repair; it does not prove which branch produced the unprinted CI value. Twenty unchanged-source isolated repetitions passed locally.

The managed context-recovery completion regression first reproduced the legacy missing-lease error, then passed the existing InvocationGuard CTest entry 1/1 in 0.09 seconds (0.10 seconds total) after the one-condition fix. Native recovery subsequently returned the actual packet and file content; the first model probe's final-format assertion still failed. Its overall receipt remains unqualified.

## Signed installation and preservation

The actual MSIX SHA-256 is `92aae3af465272b698bf43e46df889eba6830609f459b151b753a47bdc27da38`. Its signature was Valid using the retained development certificate thumbprint `4B290FFFF895EAAC959DC343255F0DA043DC12A2`. The stable identity and publisher were preserved; all four installed executables matched clean staging, distribution provenance and actual MSIX streams.

| Executable | Installed / staging / MSIX SHA-256 |
| --- | --- |
| App | `40b1e88ccf7fabb6ae9ab7145dd70c164e3f34a9451e9c0c13a0bca06c3f8ddd` |
| CLI | `948add2c0f2d74bc82e1d08860afbada21cfe8b20544299a3cb6d6af0bdd0ab1` |
| Manager | `b6db0c42a10271b11f0b0adeb5de70f968ddf1e919b7f8accd711ccc1f537bdd` |
| SessionHost | `3110bf9d25045ae0ac5bb701ab811a7fcba04d3e0ed3e4f587db29fda3f095de` |

Before first launch, the complete offline 8,256-file profile inventory and protected conversation, selection, configuration and MCP snapshots matched. Later current-chat activity legitimately appended messages. Final preservation compared the complete prior forty-message prefix, exact owner configuration and selection, original workspace files/Git status, foreign MCP semantics and all 730 foreign plugin files. Policy differences were limited to verified appended evaluation history; no gate was executed or approved. The frozen incomplete record explicitly reports qualification false.

## Retained native scope and limits

Eleven successful current-chat calls exercised status, a Host filesystem read, owned directory/write/readback, native DOCX/XLSX/PPTX creation and desktop list/read/capture. This is partial workflow evidence, not complete nine-case acceptance, a qualified blind image analysis, a completed worker/reviewer/schedule sequence, notification admission or final release qualification. Successful managed recovery results and the failed final-format assertion remain separately recorded.

All 104 Primary/Fallback tools, ten playbooks, five CLU tools and their permission/governance bounds were retained. New physical context exhaustion or rollover, exhaustive GUI coverage, three simultaneous connections and cloud parity were not established. Private chat/profile contents, fixture truth, paths and native identifiers remain local. Historical [1.3.17 evidence](HOST-CAPABILITIES-1.3.17.md) is unchanged; none of these 1.3.18 measurements qualifies 1.3.19.
