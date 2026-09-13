# Installed Alpha acceptance record

Status: IN PROGRESS. Package, exact unpacked native workflow, accessibility, and real live-provider continuity pass; registered install/update/uninstall and authentic schema-9 compatibility remain open.

Version / numeric MSIX version: `0.9.3` / `0.9.3.0`
Product commit / delivery branch: `0cd18ae1e7fd160e2fcbbe690552ef5c61a4a203` / `alpha/r6-live-continuity-fix`
Windows configuration: Windows 11 x64 Release; development-signed internal package
Installer: ignored local `out/dist/candidate-0.9.3.0-20260913-033258/ForgeConductor-0.9.3.0-x64.msix`
Installer SHA-256: `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`
Provider / model: LM Studio loopback `127.0.0.1:1234` / `qwen3-coder-30b`
Loaded/effective context and reserves: 32768 / 13500; output, handoff, and safety reserves 512 each; provider usage fields authoritative

| Check | Result | Command/action and evidence |
|---|---|---|
| Reproducible x64 Release product build | pass | All products built at `0cd18ae`; package signing, extraction, payload rehash, and exact CLI/GUI/Manager execution passed. |
| Signed MSIX clean install and Start launch | blocked | Windows returned `0x800B0109` without Local Machine Trusted People publisher trust; no package registered. |
| Projects and memory persistence/isolation | partial | Exact unpacked package and disposable profiles preserve exact project identity; registered installation remains open. |
| Installed MCP and representative native tools | blocked | Source and exact-unpacked workflows pass; registered paths remain open. |
| Real context-triggered provider successor | pass | Run `786d0672-6231-4202-a46c-401732ade283` activated successor `7dd7aece-532a-459a-8489-ab9d62567466` after authoritative usage crossed the threshold. Exact handoff retrieval, structured provider acknowledgment, predecessor fencing, and useful post-successor filesystem effects passed. |
| GUI detach/reattach during Manager work | pass | One operation recovered to completion after GUI close and exact rebuilt Manager restart; prior exact-package single-PID reattachment also passed. |
| Required native pages and Settings/reset actions | partial | Fourteen-page walkthrough, settings readback, keyboard controls, High Contrast, and 150% text passed; installed confirmed reset remains open. |
| Exact package CLI/GUI/Manager | pass | Current candidate CLI returned `Forge Conductor 0.9.3 (Windows native)`. The immediately preceding exact 0.9.3 package GUI and Manager completed live run `b74477f9-25e8-4e6b-ba3c-d73daeb370f8` with `PACKAGE_OK`; the final delta only removes the hidden turn cap and its 80-turn regression passes. |
| Update/reinstall and uninstall preserve data | blocked | Candidate version is higher, but deployment cannot begin without machine trust. |

Remaining failures: machine publisher trust/registered lifecycle and authentic C008/C009 history. The owner schema-9 store was never opened or modified. GPU utilization remains explicitly unsupported where DXGI provides no utilization source.

Operator action: verify `distribution.json`, trust only the included public development certificate from Administrator PowerShell, run `Install-Engineering.ps1 -TrustDevelopmentPublisher`, launch from Start, and perform the update/uninstall preservation checks on disposable data.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
