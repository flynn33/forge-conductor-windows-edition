# Windows installer, profile preservation and compatibility

## Reuse the installer already built
The current `scripts/package.ps1` creates a signed engineering MSIX and a companion ZIP with public certificate, installation helper and metadata. It derives a four-part version from the CMake product version and marks `alpha_accepted=false`. `Build-App.ps1` stages the GUI/CLI/Manager/SessionHost executables. Preserve these working mechanisms and package assets; do not begin a second MSI/EXE installer project. [S18, S21, S22]

The installer requirement is an actual installable Windows product, not a renamed archive. Support ordinary Windows installation/Start-menu launch with clear prerequisite/trust steps. The existing administrative trust helper is acceptable for internal certificate onboarding; it must not be required for everyday settings or runtime work. If no authorized trust action or clean test environment is available, continue independent work but keep R6 installed acceptance open. Windows requires both package signing and device trust; a successful SignTool operation alone is not installation. [E05, E06]

## Complete payload and provenance
The signed Release payload must contain the native GUI, Manager, CLI/MCP entry point, SessionHost, app icons/resources, application resources and Forsetti manifests used at runtime, required redistributable dependencies and notices. Verify actual names/paths against the staging manifest. Do not ship development Debug CRT, build-machine absolute paths as runtime requirements, missing DLL placeholders, secrets or a private key. Reuse the repository's approved self-contained/dependency packaging model rather than introducing another one.

The current staging manifest hashes executables but is not by itself proof of a clean, consistent final build. Extend the smallest existing metadata path to identify source commit/tree, relevant dirty source state if any, configuration, architecture, package identity/version and package hash. For final acceptance, build from committed product source with no uncommitted functional changes. A later docs-only commit may legitimately share the tested product tree; document that distinction instead of relabeling the old artifact as built from a future commit.

Inspect resource/assets as well as executable hashes when packaging changes. Rebuild only when product or package inputs change, not merely to create another timestamped engineering ZIP. Keep the exact candidate whose installed bytes passed acceptance. Mark acceptance through a separate factual receipt when appropriate; don't modify the accepted package bytes simply to change a label without retesting the resulting package.

## Stable identity and real upgrades
Keep the existing product identity and selected publisher stable through an upgrade test. Do not change identity/publisher to escape a certificate trust failure or a data path problem. A true upgrade uses a strictly higher supported numeric package version under the same identity/publisher; reinstalling an identical version is not upgrade evidence. Derive version values through the repository's existing authority and keep CMake, app/manifest and documentation consistent. Do not increment the product version merely at every phase boundary. Engineering artifact timestamps distinguish archives, not necessarily installed package upgrade order.

Signing certificates and passwords stay in configured user/secure stores outside Git. Only the public certificate may be included for trust. Request normal approval for an exact certificate import; do not suppress UAC, change global execution/trust policy or import a private key on user test machines. Preserve the original owner's credentials. If an existing package script leaks a secret in output, fix that narrow problem without starting a security-hardening program.

## Preserve the owner's existing database
Repository records report a current-user central store at schema 9 while the available migration implementation reaches C007. The package review did not inspect the live store or recover absent migration bodies; those remain local/runtime facts to revalidate. The source references are S05, S19 and S20. Do not repeatedly diagnose the same old mismatch from scratch unless new evidence changes it.

Use the already implemented explicit `--alpha-root` isolation for disposable development. Preserve GUI-to-Manager profile propagation and the corresponding lease/pipe/secure-storage namespace. Normal startup must never silently fall back to an empty database or a developer `out/` profile. Show active profile and compatibility errors in the native UI.

To resolve existing-data compatibility, inspect current migrations and relevant fetched Git refs/history in the authorized repository. Recover authentic C008/C009 implementation/contract if it has become available. If missing, record precisely what is missing; do not search unrelated drives endlessly or fabricate migration SQL from a schema number. Never lower the live schema version, forge migration checksums, skip validation, delete/rename the original store, or destructively “repair” it for acceptance.

Any migration experiment uses a coherent disposable copy made through a supported SQLite backup/snapshot process. Copying a database and live WAL sequentially is not automatically a consistent snapshot. Quiesce only authorized owned test writers or use a real supported backup mechanism; preserve the original. Do not add a large backup framework just to perform the test. Verify supported upgrades on disposable fixtures and that newer unsupported stores are refused without mutation.

A friendly refusal and isolated new profile allow continued development and may be a documented compatibility behavior. They do not establish that the owner's existing schema-9 data is usable. Keep that acceptance distinction explicit; only the owner can waive an existing-data requirement. Missing source remains a narrowly scoped blocker, never a justification to stop GUI/telemetry/packaging work.

## Installed lifecycle proof
Use a Windows 11 x64 VM or equivalent environment without the source checkout or Visual Studio and with the approved signing trust. A new user on a development machine with all dependencies already installed is not automatically clean-machine prerequisite evidence. Record the actual environment limitation instead of silently treating it as equivalent.

Install the signed package; launch from Start; verify the actual installed GUI/Manager/CLI/SessionHost locations and the profile they use. Configure a disposable project/model through the GUI; perform real MCP/tools/memory workflow. Resolve runtime resources and child executables from installed locations, not A:/D: repository defaults. A machine without LM Studio has an actionable prerequisite state, not a hidden automatic model download.

Perform one higher-version upgrade and one uninstall, preserving user-owned project data/configuration according to the product contract and preserving unrelated MCP entries. Be especially deliberate about MSIX-managed local data versus the canonical retained external store: verify where the installed app actually writes rather than assuming uninstall behavior. Do not remove source repositories during uninstall. Do not indiscriminately leave runnable stale Manager processes bound to an uninstalled path.

Record artifacts, actual package identity/version, installation/launch result, paths, lifecycle results, data sentinel preservation and remaining limitations. This single integrated pass can satisfy R5/R6 checks without repeated installer campaigns. Any product/package change that invalidates the tested path requires only that affected path to be retested.

<!-- alpha-phase-review:start -->
Phase review: R6 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
