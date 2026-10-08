# 1.3.23 capability investigation — installed, superseded and unpublished

Product/package identity: **1.3.23 / 1.3.23.0**. Actual clean source commit `b42a80df7db637337ea60737ef8e1049159cf448`, tree `a475d6d04e5a70b32bb72390afc7347d2d97ff65`. This evidence does not qualify the current [1.3.24 candidate](HOST-CAPABILITIES-1.3.24.md).

## Observed results

| Check | Actual 1.3.23 result |
| --- | --- |
| Normal Product All build | Passed, same clean C/T before and after |
| Entire configured Release CTest graph | 167/167 passed in 95.75 seconds |
| Three static gates and package persistence | Passed |
| Signed package and staging/MSIX/installed comparison | Valid reused signer; all four executables and 323 complete payload files matched |
| Owner profile before launch | 8,471 files / 412,433,227 bytes preserved; full backup and four protected snapshots verified |
| Normal higher package installation | 1.3.22.0 to 1.3.23.0 passed; postinstall verification required and passed |
| Actual selected Qwen Primary calls | Saved context_get packet and get_forge_status succeeded; status version 1.3.23/tool count 112/Manager owner; generation eosFound |
| Native scope/route recovery | Failed to reconcile: recovery_pending retained and no new 1.3.23 recovery archive captured |
| Automatic successor New chat and delivery Send | Unqualified |
| Fresh eight/original seven cases and complete installed 112/112/5 catalogs | Incomplete |
| Integrated public MCP image-provider and selected Qwen image tools | Incomplete |
| Source Windows CI and GitHub publication | Not qualified; no release published |

## Native failure evidence

The copied selected response contains two Primary requests: `context_get` request `2z5O76QtcLPYpM5ukxeLsJk9zTxb1YRi` and `get_forge_status` request `806FZkrhY447EOhfynihNyq7GB6Op64G`. The actual packet lookup returned `ok:true`, `found:true` and the retained packet `7f06a8b2-a102-4f82-85ae-93aa6d3a7db4`. The following status returned 1.3.23/112 with `owner:manager`, Manager PID 38704, and `recovery_pending` reporting that checkpoint schema/source/fresh scope differed. Final generation recorded `eosFound`.

The Manager trace's callback result and the selected native `context_get` result have identical callback-owned fields. The sole extra native field is Boolean `context_budget_cleared:false`. `McpInvocationGuard::afterInvoke` adds that client-local annotation after the adapter callback; the prior exact whole-result comparison did not accept it. The selected two calls were manually seeded assisted readback. They do not establish automatic packet Send, a new encrypted archive, completed migration or installed observer survival across a disconnected Primary client. The encrypted checkpoint remained the earlier 1.3.22 revision 6; its historical resumed flags are not a 1.3.23 result.

## Retained private evidence identities

The following private receipt names and hashes identify the actual root captures; private profile contents are not published here.

| Receipt | SHA-256 |
| --- | --- |
| installed-release-qualified-1323/installed-verification.json | dbf25df746e558d3e6816f206cb36450e66d3477b7393d2d24ccc420f0afb5ec |
| native-assisted-readback-1323-b42a80d-0706/snapshot.json | 9e1dd28ae194eb120426f2ce19d1d4b37a9f68ea5a4e49fcb41631b73b10853a |
| native-assisted-readback-1323-b42a80d-0706/conversation.json | 6d9b3f96a9bb18eeea7d7fced9dfa844813a6a447220b7930e8a398b86c20025 |
| native-assisted-readback-1323-b42a80d-0706/manager-trace-38704.jsonl | 397fffaa2a9f384a505549a5d0c5e2f698ec84f8f76ae9d663dfd1dcc6d2f241 |
| Signed distribution.json | 2a93ac101e6ca33724743c59513c42329ff91b92ec6d144c9c40ddbbff1e59b6 |
| Signed MSIX | f4cd8a015a018403a0deceef49b69460edb74d283786b6926558443dbb1dd31e |
| Full backup manifest | f4468a7c0e20ead9d2d72e5b6eb855f804c60ea29a78a4a19b32a3da8a453e5b |

Complete source receipts remain under `release-clean-final-1323-b42a80d` for Product All, tests, static gates and persistence. The complete tests receipt binds its actual configured graph and LastTest.log. Installed verification records 323 payload files, four executable hashes, signature, full backup, four protected snapshots and two zero-process prelaunch observations.

## Preserved previous evidence

[1.3.22](HOST-CAPABILITIES-1.3.22.md) retains its 166/166 source pass, installed profile comparison, native sequence 8 and failed automatic delivery plus assisted revision 6 recovery. Published [1.3.21](HOST-CAPABILITIES-1.3.21.md) and [1.3.19](HOST-CAPABILITIES-1.3.19.md) retain their source, binaries, native cases and release assets. None is relabeled as a 1.3.23 or 1.3.24 qualification.
