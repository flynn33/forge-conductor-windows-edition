# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The **1.3.0 candidate** replaces the setup wizard and mission-oriented shell with a direct Workspace workflow. Select or register a project, choose its provider profile, queue any number of instruction-package folders, bind optional CLU governance, choose whether automatic continuity is enabled, and work only when the readiness summary is clear. See the [release notes](docs/releases/1.3.0.md).

## Product surfaces

- **Workspace:** project and provider selection, ordered instruction packages, CLU development-policy governance, automatic-continuity preference, and readiness.
- **Rig:** live system, model, storage, continuity, process, and workflow status with bounded histories and explicit disconnected states.
- **Activity:** project/run outcomes, governance findings, corrections, notifications, and exported evidence.
- **Settings:** persistent provider, Manager, logging, shell, startup, retention, context, scoped-maintenance controls, and project-aware Doctor checks.

Instruction-package intake inventories the selected folder without extension, encoding, file-count, file-size, aggregate-size, depth, or name-based admission limits. Forge Conductor hashes content while streaming, records files, directories, reparse points and read failures, derives bounded text only when safe, and exposes paged content with stable cursors. Packages stay project-scoped and run in the queue order; failed entries can be retried and inactive entries removed.

CLU is a nonblocking governance role, not a continuity controller. Its tools evaluate development evidence, list findings and notifications, accept correction evidence, export a redacted governance log, and read bound policy documents. Automatic continuity remains an independent per-project/provider preference used only by Manager-owned runs.

The implementation deliberately reports enabled continuity as `Preparing` until a real supported-provider successor create/restore/acknowledge/fence path qualifies it. The current 1.3.0 candidate has not rerun that live-provider qualification and does not claim `Active`.

Ordinary launches use `%LOCALAPPDATA%\Forge Conductor`. Released schema data is migrated in place. The historical `--alpha-root <absolute-path>` option remains available for disposable isolated profiles.

## Build and verify

Requirements and reproducible commands are in [Build](docs/BUILD.md). The complete Release path is:

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend
./scripts/test.ps1 -Configuration Release -Architecture x64
./scripts/Run-Static-Gates.ps1
```

After committing the exact release inputs and rebuilding from that commit, create a development-signed validation package with:

```powershell
./scripts/package.ps1 -DevelopmentSigning
```

Production distributions require an explicitly supplied code-signing PFX. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they are archived delivery records, not the current product definition.
