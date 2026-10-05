# Installed Forge Conductor 1.3.11 qualification

On October 5, 2026, the authorized host repair installed and exercised the signed 1.3.11.0 package built from clean source `5f938d93d2c80fb57fc39da65cac00a5f14b93c1`. This record is a later documentation update and does not change the package's source commit/tree.

## Verified mechanisms and impact

The original registrations launched the Forge repository, while the packaged desktop's saved project selection and active policy were YWE. The stores are separate. Selected project ID/root now travel through Manager, App, deployment and all three LM Studio roles. Existing registered workspace reuse performs native Read authorization, exact unique alias matching and project-store integrity checks; new initialization retains the repository identity conflict guard.

MCP bootstrap formerly read only version-2 queue records while legacy conversion was in Manager's listing path. Shared bounded migration now makes legacy instructions visible to actual MCP bootstrap. Empty queues, repeated ordering, missing content and idempotent migration have regression coverage. `fs_edit` advertised an empty argument schema although its handler required path/old/new; the corrected schema is verified by actual model edit/readback. Specialist start/status project capability now matches strict service ownership.

Implicit continuity lookup now uses current client/project and authorizes a global fallback only for its matching workspace. Explicit packet lookup remains available. Budget checkpoints retain authorized workspace and recent file/tool evidence; explicit seed and managed run receipts survive updates. No missing historical goal was invented.

Native acceptance exposed LM Studio's installed per-block 50,000-character truncation and a separate strict Windows request-path mismatch. Policy inventories now use snapshot-validated pages; UTF-8 text reads use accurate bounded offsets. Other large results return ordered bounded text fragments without repeating a tool operation. Native ingestion requires the full consistent fragment sequence. Whole-response size limits and strict catalog mismatch rejection remain. Requested Windows tool paths now normalize supported separators before strict authorization; configured roots and unsafe/outside-workspace rejection remain strict.

## Build and installation evidence

| Check | Executed result |
|---|---|
| `scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4` | 153/153, exit 0, 48.65 seconds |
| `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4` | Exit 0, all four products staged |
| `scripts/Run-Static-Gates.ps1` | No-Python, Native-stack and No-attribution passed |
| `scripts/validation/Test-PackagePersistenceContract.ps1` | Ordinary AppData exclusion and unvirtualizedResources passed |
| Actual-process separator regression | Red against preserved 1.3.10; green against 1.3.11, 19,726 assertions |
| `scripts/package.ps1 -DevelopmentSigning` | Clean commit/tree/staging, Valid signature, payload extraction verified |
| Official signed preflight and engineering install | Exit 0, installed 1.3.11.0, four staging hashes match |

Distribution: `D:\GitHub\Forge-Conductor-Windows-Edition\out\dist\release-1.3.11.0-20261005-183614`. MSIX SHA-256: `d86b2395a3d805fad36c787fc543974c03076f7030e024c13f63b644de720cf5`. CLI SHA-256: `b17312805f3d96c0f2d5273e8fa86fa56af86542a6acc696b7e96876af2c90c3`. Existing publisher certificate thumbprint: `4B290FFFF895EAAC959DC343255F0DA043DC12A2`. No new trust was installed. Consistent profile/SQLite and LM Studio configuration/conversation backups were taken before the first upgrade.

Production maintenance ran under the actual package identity; status, repair and activation completed with exit 0. Revision `281a7d75-16a4-4e48-97af-03292c00bba7` binds Primary, Fallback and CLU to `serve --project-id 2f9a1172-3309-4efb-a94a-ed2f56575438`, `cwd=D:\GitHub\YWE`, the ordinary Forge AppData home and 180000 ms timeout. Hot synchronization passed without restarting LM Studio. Independent config readback confirms foreign server semantics match the pre-upgrade backup; production receipts confirm foreign plugin preservation.

## Actual installed feature and model checks

The installed CLI passed 31 calls in a disposable profile/project: filesystem write/edit/read/glob/search/move, shell, Git status, PDF creation, legacy/project memory, specialist start/status/complete, checkpoint/handoff and exact recovery in a new process. No YWE project file edits were requested by the live native probe.

The frozen final CLI matches installed SHA-256 `b17312805f3d96c0f2d5273e8fa86fa56af86542a6acc696b7e96876af2c90c3`. Loaded `qwen/qwen3.8-27b` generated exactly two edit/read calls; disk readback was `AFTER_SCHEMA native verification` with its preserved newline and SHA-256 `21abba2608352fda1a26dbedc45e1662ad1da8a8a5911fa5fbf91753e8b9cca2`. General catalog fingerprint: `8f040e6802b1bd9d729c446ba60987eadb37e7e203b183ba8fd9d6a232e07b04`; CLU fingerprint: `08ee5cd873945558906ee878ae5ef1c30b94b56f511ae1dd4eb75e9c0a2d7ff4`. Installed Fallback and CLU both returned three complete policy pages, 270 coverage entries, zero guidance entries and exact `AGENTS.md` hash.

## Native conversation evidence

The stock product native-chat controller created a separate conversation: `C:\Users\james\.lmstudio\conversations\Forge-Conductor-Windows-Edition\1791225476958.conversation.json`. Qwen completed 9 calls with EOS; `context_get` ran once. Each request/result call ID, Primary integration identity and semantic success were checked independently of the UI label. Every reconstructed result matched ordered `connector_tool_result` rows from PID 35300's trace, `C:\Users\james\AppData\Local\Forge Conductor\continuity\lmstudio-chat-trace-35300.jsonl`.

| Native result | Exact readback |
|---|---|
| Runtime | 1.3.11, PID 35300, 58 tools, 10 agents |
| Workspace | YWE, `2f9a1172-3309-4efb-a94a-ed2f56575438`, `D:\GitHub\YWE`, explicit project binding |
| Package | One ready YWE package; first document 1,808 bytes, SHA-256 `a38ee7758261cad4f76b4963ba822ef19ba15cd32f88aaff739428226c72013b` |
| Policy index | 3 pages; 270 coverage/0 guidance; exact pre-repair full-inventory match; snapshot SHA-256 `93102ac6fb31218a905f37e698b528df8aef36aedc916af7c5d254e30d73270f` |
| Policy document | 14,019 bytes, SHA-256 `c35c58b5d609aa4f89fa2d7795897eb2357f47869c930b1bd1d7bd76136f7e1e` |
| Large shell | Exact same forward-slash cwd; 64,038 stdout bytes, 6 complete fragments, stdout_truncated=false; SHA-256 `4e1eea56cbe6b0834ce261d8146476dc412261e8ec0c21f98bf8b5533e038d6f` |
| Continuity | Enabled, blocked=false, native state observing; no unrelated implicit old packet |

Policy and instruction text also match the pre-upgrade saved records byte-for-byte. Native result blocks are valid JSON within 32 KiB; shell fragments preserve the complete canonical result and original order. The original conversation remains byte-identical to its backup, SHA-256 `a3287c58f25435c3b26c49e3d014601c8db5d4706a87db87629be16aa93bc5ae`.

## Evidence locations and limits

Host evidence is retained in `C:\Users\james\Documents\ChatGPT\Forge Conductor Windows Edition`: `release-tests-1.3.11.log`, `release-all-build-1.3.11.log`, static/persistence/preflight/install logs, `installed-payload-hashes-1.3.11.json`, `native-ywe-*-1311.json`, `live-three-role-bindings-1311.json`, `live-installed-processes-1311.json`, `installed-role-verification-1311.json`, `feature-installed-1.3.11`, `model-final-1.3.11-edit`, and `native-acceptance-1311-readback`. The native folder includes raw conversation, primary trace, parsed calls and `validated-native-results.json`. Before-fix 1.3.9 truncation and 1.3.10 path failures remain separately preserved.

This verifies the reported bootstrap/configuration failures and the additional reproduced native delivery/path defects. It does not exercise a new reserve-triggered native rollover, physical context exhaustion, worker-crash recovery or every GUI action. Interrupted native rollover phase recovery after idle-worker eviction remains the documented in-process limitation. The 42 saved coverage gaps (40 directory inventory entries and two opaque files) remain visible, and absent goals in old packets were not reconstructed. No claim is made that every possible future failure is eliminated.
