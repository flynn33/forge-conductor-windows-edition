# User guide

Launch Forge Conductor from Start. The header reports the Manager connection and production data profile.

## First run

Guided Mode is on for a new profile. Open **Guided setup** in the main navigation to see and resume the complete path:

1. Learn what the setup will change and what it will leave untouched.
2. Choose a folder containing the work you actually want to manage.
3. Give it an optional friendly name, then register it through the Manager.
4. Review the Manager-verified project identity and authorized folder boundary.
5. Optionally validate and activate real project instruction files.
6. Optionally save useful facts, decisions, or constraints as durable project memory.
7. Review the real loopback provider, loaded model, and connection state.
8. Describe the first real task and choose whether it may use the authorized native tool catalog.
9. Finish after the Manager creates and reads back the project-bound run, or finish setup without submitting a task.

The guide never creates a sample project, placeholder note, mock record, or tutorial task. Optional context steps can be skipped. Turn Guided Mode off or restart it from **Guided setup**, the **Rig** toggle, or the matching control under **Settings**. The navigation footer displays the installed application version.

Without Guided Mode:

1. Open **Settings**, confirm the LM Studio loopback host and port, and optionally select a loaded model.
2. Open **Projects**, choose an authorized project folder, provide a display name, and register it.
3. Select the project. The selection is used by Autonomy, project memory, and Tools.
4. Optional: in **Projects**, choose an instruction package folder, validate its exact revision, review the file manifest, and activate it. Activation is project-scoped; a changed folder must be validated again.
5. Open **Autonomy**, enter a goal, and start a managed run. The Manager owns the model request, active project instructions, native tool loop, continuity rollover, and run controls.

## Pages

### Offline help in 1.1.44

Open **Guided setup → Help and troubleshooting**. Search by a word or phrase such as `policy`, `model`, or `memory`, then expand an article. All 20 articles are available without a network connection or model. A no-match message suggests other search terms.

The opening **Choose my project folder** button opens the Windows picker directly. Cancelling returns to folder selection without registering a project. Finishing the guide does not prove provider readiness or task success.

### Development policy limitations

The current instruction-package control accepts local folders, not repository URLs. Activation supplies project guidance; it is not a policy-enforcement engine. Repository intake, pinned policy adoption, and verifiable governance gates are specified in [the research plan](SETUP-GOVERNANCE-RESEARCH.md) and remain future implementation work.

### Navigation

- **Rig** shows live resource telemetry, provider/store/continuity health, relevant processes, workflows, and the Guided Mode toggle. A disconnect is shown as a gap rather than fabricated data.
- **Guided setup** explains and drives the complete real-project setup path and can restart it at any time.
- **Autonomy** starts and reattaches managed runs and provides pause, resume, and stop controls.
- **Continuity** shows handoff and retained-context state for managed work.
- **Projects** registers folders, displays stable project IDs and authorized aliases, and validates or activates checksummed UTF-8 instruction packages. Supported package files are Markdown, text, JSON, YAML, TOML, and CSV, with a 32-file / 212-KiB content bound.
- **Memory** searches the selected project's durable memory and shows complete records.
- **Tools** invokes a named native tool inside the selected project's authority.
- **Settings** controls runtime behavior, mirrors the Guided Mode toggle, and offers confirmed scoped reset operations.

Ordinary LM Studio desktop chats and Forge-managed runs are separate modes. Connecting the MCP server does not retroactively enroll an existing desktop chat into managed continuity.

The production profile is `%LOCALAPPDATA%\Forge Conductor`. For a disposable isolated test only, launch `ForgeConductorApp.exe --alpha-root <absolute-empty-folder>`; the historical option name is retained for command-line compatibility.
