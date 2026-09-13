# Installed Alpha acceptance record

Status: IN PROGRESS. Package, exact unpacked native workflow, accessibility, and real live-provider continuity pass; registered install/update/uninstall and authentic schema-9 compatibility remain open.

Version / numeric MSIX version: `0.9.4` / `0.9.4.0`
Product commit / delivery branch: `3fb70143ef2db9704c8a395b49a29857bd087976` / `alpha/r6-installed-acceptance-closeout`
Product tree: `fad39f10bdb2638a74a0b28deb0c0ecc1e88dede`
Windows configuration: Windows 11 x64 Release; development-signed internal package
Installer: ignored local `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`
Installer SHA-256: `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`
Provider / model: LM Studio loopback `127.0.0.1:1234` / `qwen3-coder-30b`

| Check | Result | Command/action and evidence |
|---|---|---|
| Reproducible x64 Release product build | pass | All products built at `3fb7014`; package signing, extraction, all 324 payload rehashes, and exact CLI/GUI/Manager execution passed. |
| Signed MSIX clean install and Start launch | blocked | Windows returned `0x800B0109` without Local Machine Trusted People publisher trust; no package registered. No elevation workaround was attempted. |
| Projects and memory persistence/isolation | partial | Exact unpacked package and disposable profiles preserve exact project identity; registered installation remains open. |
| Installed MCP and representative native tools | blocked | Source and exact-unpacked workflows pass; registered paths remain open. |
| Real context-triggered provider successor | pass | Exact 0.9.4 run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed the durable eight-transition rollover to `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed both requested effects, and returned terminal `DONE`. |
| Managed-run repetition guard | pass | Twelve identical `managed-run-v1` invocations pass in the focused guard regression. A live stress run produced more than 60 identical audited calls without the legacy desktop-chat block. |
| GUI detach/reattach during Manager work | pass | One operation recovered to completion after GUI close and exact rebuilt Manager restart; prior exact-package single-PID reattachment also passed. |
| Required native pages and Settings/reset actions | partial | Fourteen-page walkthrough, settings readback, keyboard controls, High Contrast, and 150% text passed; installed confirmed reset remains open. |
| Exact package CLI/GUI/Manager | pass | Candidate CLI returned `Forge Conductor 0.9.4 (Windows native)`, and the candidate GUI/Manager completed a live handoff plus terminal run. The separate 0.9.3 `PACKAGE_OK` run remains package-execution evidence only. |
| Update/reinstall and uninstall preserve data | blocked | Retained 0.9.3 and current 0.9.4 candidates provide a real increasing-version path, but deployment cannot begin without machine trust. |

The earlier rollover run `786d0672-6231-4202-a46c-401732ade283` produced a real acknowledged successor and useful effects, then ended in a Forge runtime `context_budget_exceeded` block from the legacy `McpInvocationGuard`. The 0.9.4 fix scopes that policy away from Manager-owned traffic. This correction does not convert the separate `PACKAGE_OK` run into rollover evidence.

Remaining failures are machine publisher trust/registered lifecycle and authentic C008/C009 history. The owner schema-9 store was never opened or modified. GPU utilization remains explicitly unsupported where DXGI provides no utilization source. G04's aggregate wrapper overflow remains failed; direct builds and focused functional checks are recorded instead.

Operator action: follow [Installed acceptance handoff](INSTALLED-ACCEPTANCE-HANDOFF.md) on an authorized Windows 11 x64 environment using dedicated account `.\ForgeAlphaTest`. Import only the public certificate through the approved administrator process, then perform the 0.9.3→0.9.4 installed lifecycle. Do not use the owner profile.

<!-- alpha-phase-review:start -->
Phase review: R6 acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
