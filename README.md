# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The **1.3.5 candidate** corrects Work Space model discovery and Responses-session bootstrap to use LM Studio's native `/api/v1/models` inventory and admit only loaded LLM instance IDs. Downloaded-but-unloaded models, embedding models, and duplicate instance IDs are not presented as ready. LM Studio **Require Authentication** must remain disabled for this same-host loopback connection; Forge does not store or configure a bearer token. See the [1.3.5 candidate notes](docs/releases/1.3.5.md).

## Product surfaces

- **Workspace:** project and provider selection, ordered instruction packages, CLU development-policy governance, automatic-continuity preference, and readiness.
- **Rig:** live system, model, storage, continuity, process, and workflow status with bounded histories and explicit disconnected states.
- **Activity:** project/run outcomes, governance findings, corrections, notifications, and exported evidence.
- **Settings:** persistent provider, Manager, logging, shell, startup, retention, context, record-level and scoped-maintenance controls, and project-aware Doctor checks.

Instruction-package intake inventories the selected folder without extension, encoding, file-count, file-size, aggregate-size, depth, or name-based admission limits. Forge Conductor hashes content while streaming, records files, directories, reparse points and read failures, derives bounded text only when safe, and exposes paged content with stable cursors. Packages stay project-scoped and run in the queue order; failed entries can be retried and inactive entries removed. If an interpreted entry cannot fit beside a managed-run task, startup fails explicitly and leaves the entry pending instead of silently skipping it or looping the cursor. The admitted run durably records its exact cursor plan in a dispatch-pending phase. Multi-row cursor updates commit as one transaction, and no provider or tool work starts until the plan commits or restart replay reconciles an already committed plan. An exact start replay returns the original assignment without consuming packages or preferences that changed afterward.

CLU is a nonblocking governance role, not a continuity controller. Its tools evaluate development evidence, list findings and notifications, accept correction evidence, export a redacted governance log, and read bound policy documents. Automatic continuity remains an independent per-project/provider preference used only by Manager-owned runs.

Project-memory **Browse** and **Display All** read durable project records; they are not automatic continuity. The 1.3.5 candidate also aligns the durable agent-goal limit with the 128 KiB managed-run instruction envelope, persists `automaticContinuity` as part of restart-safe start idempotency, and assigns a unique sequenced identity to each rollover in a run. A checkpoint can be refreshed before successor creation only by comparing the exact prior digest, so stale retry content cannot replace newer completed work. The successor receives a compact UTF-8-safe copy of the original task and latest completed work; a terminal or failed run durably abandons any checkpoint that was never consumed by a successor. On Manager startup, the process-wide instance lease protects a pre-ingress pass that abandons checkpoint-only work left by an earlier process; periodic maintenance preserves live checkpoint-only state and resumes only records with durable successor intent. A fresh disposable post-fix live successor check passed on the Release x64 source build; packaged WinUI and installed acceptance remain unverified.

Ordinary launches use `%LOCALAPPDATA%\Forge Conductor`. Released schema data is migrated in place. The historical `--alpha-root <absolute-path>` option remains available for disposable isolated profiles.

Forge resolves Git and PowerShell for Manager, CLI/MCP, policy import, Doctor, native-shell execution, and LM Studio serve verification through one product-wide machine-tool resolver. It replaces caller-supplied `PATH`, `PATHEXT`, and `COMSPEC`, does not import HKCU `PATH`, and does not rely on per-user command shims. Machine registry strings are read raw from HKLM with no-expand semantics; caller-controlled environment variables are never substituted into those machine values, and only absolute, existing, non-reparse machine directories enter the search path.

## Build and verify

Requirements and reproducible commands are in [Build](docs/BUILD.md). The complete Release path is:

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend
./scripts/test.ps1 -Configuration Release -Architecture x64
./scripts/Run-Static-Gates.ps1
./scripts/validation/Test-PackagePersistenceContract.ps1
```

Version 1.3.5 is a source candidate pending final verification, development-signed package construction, installed UI testing, and operator acceptance. The [`v1.3.4` GitHub release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.4) remains the latest published package. After the source is committed and verified, reproduce an uninstalled candidate with:

```powershell
./scripts/package.ps1 -DevelopmentSigning -ReleaseChannel prerelease
./scripts/validation/Test-ReleaseLifecycle.ps1 `
  -PreviousCandidate 'out/dist/release-1.3.4.0-20260930-131641' `
  -CurrentCandidate 'out/dist/release-1.3.5.0-YYYYMMDD-HHMMSS'
```

Replace the lifecycle command's timestamp placeholder with the distribution directory emitted by packaging.

Production distributions require an explicitly supplied code-signing PFX. See [Known issues](KNOWN_ISSUES.md), [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they are archived delivery records, not the current product definition.
