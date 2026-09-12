# Installed Alpha acceptance record
Status: IN PROGRESS. Package/build evidence is recorded; installed, native visual, and live-provider checks remain open.

Version / numeric MSIX version: `0.9.1` / `0.9.1.0`
Commit / branch: `3bcaeb3481022b38d6d6c9783510ace910957cf8` / `alpha/r5-installer-data`
Windows configuration: Windows 11 x64 Release; development-signed internal package
Installer path or retained artifact URL: ignored local `out/dist/candidate-0.9.1.0-20260912-225739/ForgeConductor-0.9.1.0-x64.msix`
Installer SHA-256: `ddb3e8c6c43aedc21be0747f46431061f29c2ed3dfaad332e79f1026073f5087`
Provider / model / LM Studio version:
Loaded context capacity / effective context target / reserves / usage source:

| Check | Result (not run/pass/fail) | Command/action and evidence |
|---|---|---|
| Reproducible x64 Release product build | pass | All four products built and staged at the committed source; package extraction and payload rehash passed. |
| Signed MSIX clean install and Start launch | blocked | Normal `Add-AppxPackage` returned `0x800B0109`; the development publisher requires explicit Local Machine trust. No Start launch is claimed. |
| Projects and memory persistence/isolation | not run | |
| Installed MCP primary/fallback and representative native tools | not run | |
| Real context-triggered provider successor and useful continued work | not run | |
| GUI detach/reattach during manager-owned work | not run | |
| Required native pages and Settings/reset actions | not run | |
| Installed CLI serve stdout and path correctness | not run | |
| Update/reinstall and uninstall preserve user data | not run | |

Required failures still open: machine publisher trust/install, native page and accessibility walkthroughs, live tool-capable LM Studio continuity, higher-version update/uninstall, and authentic schema-9 compatibility.
Known limitations and deferred B1/B2 issues:
Operator launch instructions:

Do not attach private credentials, signing keys or unrelated project data. Do not prefill pass statuses.

<!-- alpha-phase-review:start -->
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
