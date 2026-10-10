# Forge Conductor for Windows

Local orchestration and tool use for LM Studio on Windows 11 x64.

Forge Conductor ships as a WinUI app, a per-user Manager, a CLI, and three LM Studio plugins. Chat stays in LM Studio. Forge provides the tools, project memory, continuity, and local services behind that chat.

**Current release: 1.3.29** (Windows package 1.3.29.0).

## Download

| | |
| --- | --- |
| Release | [v1.3.29](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.29) |
| Installation bundle | [ForgeConductor-1.3.29.0-x64.zip](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.29/ForgeConductor-1.3.29.0-x64.zip) |
| Windows package | [ForgeConductor-1.3.29.0-x64.msix](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.29/ForgeConductor-1.3.29.0-x64.msix) |

The release contains seven verified assets. The bundle includes the signed package, the public certificate, the installer, and checksums.

## 1.3.29 — local ComfyUI automation

Describe an image or video in LM Studio. The model discovers nodes and models, builds the workflow, prepares the selected local ComfyUI installation, and returns a verified preview. Reply `approved`, `yes`, or `render final` in that chat. Forge then runs the sealed final plan.

- Primary and Fallback expose **125 tools**: the previous catalog, eleven ComfyUI tools, and `desktop_scroll` / `desktop_drag`.
- Five CLU tools, ten specialist playbooks, and the six existing image tools remain.
- Preparation defaults are 500 GB per operation, a 50 GB free-space reserve, and 30 minutes.
- Starter workflows cover image, text-to-video, and image-to-video.
- ComfyUI automation is off until you enable it in Settings.

Media inference runs on the Windows host. LM Studio keeps its existing model connection. Image-provider jobs (`image_generate`, `image_edit`, and the `image_job_*` tools) and ComfyUI jobs are separate paths with separate job IDs.

| Guide | What it covers |
| --- | --- |
| [Automation](docs/COMFYUI-AUTOMATION.md) | Tools, setup, approval, limits |
| [Host qualification](docs/COMFYUI_HOST_QUALIFICATION.md) | Inference timings and visual limits |
| [Release notes](docs/releases/1.3.29.md) | What shipped in 1.3.29 |
| [Release verification](docs/validation/RELEASE-1.3.29.md) | Build, package, install, publication |

## Install

You need Windows 11 x64. Chat features also need LM Studio and a loaded tool-capable model. The app, Manager, CLI, and local tools install together.

Extract the bundle, open PowerShell in that folder, and run:

```powershell
./Install.ps1 -PreflightOnly
./Install.ps1
```

Preflight checks the package, certificate, signer, and version, and it rejects a downgrade. Launch **Forge Conductor** from Start.

After an upgrade:

1. Select the project in Workspace.
2. Open **Settings → LM Studio plugins → Install or repair all three plugins**.
3. Start a new model bootstrap, or reconnect the integrations.
4. In LM Studio, open the plugin connection and select the integration for the chat.

Repair updates Primary (`forge-conductor`), Fallback (`forge-conductor-fallback`), and CLU (`forge-conductor-clu`) to the current binary, the selected project, and the 180-second request deadline. Other MCP registrations stay as they are.

Day-to-day data is stored in `%LOCALAPPDATA%\Forge Conductor`. **Production** is that store. **Isolated profile** is a data folder you select on purpose. Uninstalling the package leaves the profile on disk.

Development-signed builds, certificate trust, and upgrade rules are in [Install](docs/INSTALL.md).

## Using the app

| Surface | Use it for |
| --- | --- |
| Workspace | Project and provider, instruction packages, CLU policy, Auto Continuity, readiness |
| Rig | System, chat context, model, storage, processes, and ComfyUI readiness. Runtime: Ensure manager, Restart service, Stop service |
| Continuity | Saved packets: open, delete a selection, or clear |
| Activity | Outcomes, governance findings, corrections, notifications, exported evidence |
| Settings | Configuration, ComfyUI limits, context reserves, and saved records |

`get_forge_status` reports the bound project, instruction packages in execution order, the policy source, and the tool catalog. `host_capabilities` reports what this host can run.

Auto Continuity watches the selected LM Studio conversation. At reserve pressure it requests a model-written packet, opens a native successor chat, sends the packet, and checks a following Forge tool result. Leave the Manager running. Primary owns that rollover. Fallback is the second general catalog. CLU evaluates development policy.

Workflows, jobs, desktop and image tools, schedules, and filesystem authority are in the [user guide](docs/USER-GUIDE.md). Schemas are in the [model capability guide](docs/LM-STUDIO-MODEL-CAPABILITIES.md) and the [JSON catalog](docs/LM-STUDIO-MODEL-CAPABILITIES.json).

## Verification

Figures below are for the signed 1.3.29 package. Host inference and operator quality acceptance are separate records.

| Check | Result |
| --- | --- |
| Product All, static gates, package persistence | Passed |
| Release tests | 178/178 in 226.22 seconds |
| Tag CI on the same source | 178/178 in 209.94 seconds |
| Payload readback | All 331 declared files, archive and installed copy |
| Cold connector to its Manager | 2.1128484 seconds |
| Installed host after publication | System online / Production. Three integrations on the repository project. ComfyUI ready on the RTX 4090 |

The same-source main CI run recorded timeouts and popup-automation failures. Both are written up in the [verification record](docs/validation/RELEASE-1.3.29.md).

## Build

Windows 11 x64, Visual Studio 2022 (MSVC v143, Desktop C++, UWP/XAML C++ tools, Windows SDK 10.0.26100.0), CMake 3.28 or later, PowerShell 7, and vcpkg with `VCPKG_ROOT` set.

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend
./scripts/test.ps1 -Configuration Release -Architecture x64
./scripts/Run-Static-Gates.ps1
```

A local engineering package:

```powershell
./scripts/package.ps1 -DevelopmentSigning
```

[Build](docs/BUILD.md) · [Testing](docs/TESTING.md) · [Windows toolchain](docs/WINDOWS_TOOLCHAIN.md) · [Packaging](packaging/README.md)

## Documentation

| Document | |
| --- | --- |
| [Documentation index](docs/DOCUMENTATION-INDEX.md) | Current docs and the historical record |
| [Product status](docs/STATUS.md) | Current source and retained measurements |
| [Changelog](CHANGELOG.md) | Release history |
| [User guide](docs/USER-GUIDE.md) | Operator workflows |
| [Architecture](docs/ARCHITECTURE.md) | Ownership, continuity, and service boundaries |
| [Host capabilities](docs/HOST-CAPABILITIES.md) | Tool packs and limits |
| [Roadmap](ROADMAP.md) | Planned work |
| [Third-party notices](THIRD-PARTY-NOTICES.md) | Bundled components |

## Earlier releases

1.3.28 and the unpublished candidates through 1.3.27 keep their original commits, trees, package identities, and test counts. Read them in the [changelog](CHANGELOG.md), [product status](docs/STATUS.md), [release notes](docs/releases/), and [validation records](docs/validation/). The [documentation index](docs/DOCUMENTATION-INDEX.md) lists each record.

Alpha plans under `.forge-alpha/` and `docs/implementation/alpha-recovery/` describe the 0.9.x validation program.
