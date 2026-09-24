# Forge Conductor for Windows

Forge Conductor is a native Windows 11 control application and MCP tool server for project work with local models in LM Studio.

Version **1.2.1** corrects setup discovery and completes automatic LM Studio plugin deployment. Open **Start here · Guided setup**, choose a folder, and follow preparation → policy → first task on one page. Preparation installs, verifies and synchronizes Primary, Fallback and Continuity plugins before model readiness. See the [release notes](docs/releases/1.2.1.md).

The 1.1 product includes a full WinUI 3 desktop interface, an automatically attached or started per-user Manager process, managed inference and context continuity, project registration, legacy and project memory, 58 native MCP tools, three-role LM Studio deployment, scoped maintenance, and live CPU, RAM, GPU, disk, volume, process, and workflow telemetry.

## Product surfaces

- **Rig:** live system, model, store, continuity, process, and workflow status with bounded histories and disconnect visibility.
- **Autonomy:** start, inspect, pause, resume, stop, and reattach managed local-model runs.
- **Projects:** register authorized folders, manage stable project identities and aliases, and validate/activate checksummed instruction-package folders for new managed runs.
- **Memory:** search and inspect durable project memory with project isolation.
- **Tools:** invoke authorized filesystem, search, Git, PowerShell, PDF, project, memory, agent, and continuity tools.
- **LM Studio plugins:** install, verify, and activate synchronized primary, fallback, and continuity-control registrations. The dedicated `clu` role exposes only `clu_capabilities`, `clu_start_handoff`, `clu_status`, and `clu_cancel`.
- **Settings:** configure Manager, dashboard, LM Studio, logs, shell, startup, retention, and context thresholds; perform confirmed scoped resets.

Ordinary launches use `%LOCALAPPDATA%\Forge Conductor`. Released schema-9 data is supported in place. The historical `--alpha-root <absolute-path>` option remains as a compatibility alias for disposable isolated profiles.

## Build and verify

Requirements and reproducible commands are in [Build](docs/BUILD.md). The complete Release path is:

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend
./scripts/test.ps1 -Configuration Release -Architecture x64
./scripts/Run-Static-Gates.ps1
```

Create a development-signed validation package with:

```powershell
./scripts/package.ps1 -DevelopmentSigning
```

Production distributions require an explicitly supplied code-signing PFX. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), and [Architecture](docs/ARCHITECTURE.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they are archived delivery records, not the current product definition.

Guided setup includes 42 searchable offline articles and optional policy adoption from GitHub or a local folder. The Manager enforces reviewed write paths and exact commands. Human review remains necessary for prose requirements. See the [user guide](docs/USER-GUIDE.md), [research](docs/SETUP-GOVERNANCE-RESEARCH.md), and [verification record](docs/validation/SETUP-CORRECTION-1.2.1.md).
