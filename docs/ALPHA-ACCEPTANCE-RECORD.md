# Installed Alpha acceptance record
Status: IN PROGRESS. Package, unpacked native workflow, visual accessibility, and defect-repair evidence are recorded; installed lifecycle and live-provider continuity remain externally blocked.

Version / numeric MSIX version: `0.9.2` / `0.9.2.0`
Product commit / delivery branch: `d8a2d68c80f2fd090aa36466a517725a0eb59445` / `alpha/r7-delivery`
Windows configuration: Windows 11 x64 Release; development-signed internal package
Installer path or retained artifact URL: ignored local `out/dist/candidate-0.9.2.0-20260912-235342/ForgeConductor-0.9.2.0-x64.msix`
Installer SHA-256: `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`
Provider / model / LM Studio version: not available; TCP `127.0.0.1:1234` unavailable at `2026-09-13T00:20:40Z`
Loaded context capacity / effective context target / reserves / usage source: no live provider sample; Settings effective context value observed at 32768 and restored after keyboard checks

| Check | Result (not run/pass/fail) | Command/action and evidence |
|---|---|---|
| Reproducible x64 Release product build | pass | All four products built from commit `d8a2d68`; package signing, extraction, payload rehash, and focused App/MCP/Infrastructure tests passed. |
| Signed MSIX clean install and Start launch | blocked | Normal 0.9.1.0 `Add-AppxPackage` returned `0x800B0109`; current-user trust was insufficient and the machine-level trust action was unavailable. No package registered. |
| Projects and memory persistence/isolation | partial | Exact unpacked 0.9.2 GUI restored project `51e1f9a4-5943-46f1-9797-35a275768cc3` in profile A while profile B retained zero projects and no selection. Installed persistence remains open. |
| Installed MCP primary/fallback and representative native tools | blocked | Source/native workflow evidence exists, but the package is not registered. |
| Real context-triggered provider successor and useful continued work | blocked | LM Studio was offline; no provider-originated acknowledgment or productive successor is claimed. |
| GUI detach/reattach during manager-owned work | pass | Exact unpacked package retained Manager PID 3820 after GUI close and reattached one GUI without creating another Manager. |
| Required native pages and Settings/reset actions | partial | Fourteen-page walkthrough, Settings labels/readback, keyboard context controls, High Contrast, and 150% text size passed. Installed confirmed reset remains open. |
| Installed CLI serve stdout and path correctness | partial | Exact package bytes reported `Forge Conductor 0.9.2 (Windows native)` and all sibling paths resolved inside the unpacked MSIX. Installed alias remains open. |
| Update/reinstall and uninstall preserve user data | blocked | 0.9.1.0 and higher 0.9.2.0 candidates exist, but package deployment cannot start without machine trust. |

Required failures still open: machine publisher trust/install, live tool-capable LM Studio continuity, installed update/uninstall, and authentic schema-9 compatibility. Native page and accessibility checks are complete.
Known limitations: GPU utilization is explicitly unsupported where Windows/DXGI exposes adapter memory but no utilization source. The live schema-9 owner store remains unsupported because authentic C008/C009 migrations are unavailable and was never opened during this work.
R7 delivery recheck: candidate MSIX and ZIP hashes still match `distribution.json`; product/package bytes are unchanged. The development certificate was absent from Local Machine Trusted People and no Forge package was registered at `2026-09-13T00:20:40Z`.
Operator launch instructions: verify `distribution.json`, trust only the included publisher certificate from an Administrator PowerShell, run `Install-Engineering.ps1 -TrustDevelopmentPublisher`, launch Forge Conductor from Start, and keep acceptance data in a disposable profile.

Do not attach private credentials, signing keys or unrelated project data. Do not prefill pass statuses.

<!-- alpha-phase-review:start -->
Phase review: R7 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
