# User guide

Launch Forge Conductor from Start. The header reports the Manager connection and production data profile.

## First run

Guided Mode is on for a new profile. Follow its five steps to choose one of your real folders, register it through the Manager, understand its authorization boundary, add optional project context, and continue to managed work. The guide does not create a sample project or mock records. Turn it off or restart it from the **Rig** toggle; the same control is available under **Settings**.

Without Guided Mode:

1. Open **Settings**, confirm the LM Studio loopback host and port, and optionally select a loaded model.
2. Open **Projects**, choose an authorized project folder, provide a display name, and register it.
3. Select the project. The selection is used by Autonomy, project memory, and Tools.
4. Optional: in **Projects**, choose an instruction package folder, validate its exact revision, review the file manifest, and activate it. Activation is project-scoped; a changed folder must be validated again.
5. Open **Autonomy**, enter a goal, and start a managed run. The Manager owns the model request, active project instructions, native tool loop, continuity rollover, and run controls.

## Pages

- **Rig** shows live resource telemetry, provider/store/continuity health, relevant processes, workflows, and the Guided Mode toggle. A disconnect is shown as a gap rather than fabricated data.
- **Autonomy** starts and reattaches managed runs and provides pause, resume, and stop controls.
- **Continuity** shows handoff and retained-context state for managed work.
- **Projects** registers folders, displays stable project IDs and authorized aliases, and validates or activates checksummed UTF-8 instruction packages. Supported package files are Markdown, text, JSON, YAML, TOML, and CSV, with a 32-file / 212-KiB content bound.
- **Memory** searches the selected project's durable memory and shows complete records.
- **Tools** invokes a named native tool inside the selected project's authority.
- **Settings** controls runtime behavior, mirrors the Guided Mode toggle, and offers confirmed scoped reset operations.

Ordinary LM Studio desktop chats and Forge-managed runs are separate modes. Connecting the MCP server does not retroactively enroll an existing desktop chat into managed continuity.

The production profile is `%LOCALAPPDATA%\Forge Conductor`. For a disposable isolated test only, launch `ForgeConductorApp.exe --alpha-root <absolute-empty-folder>`; the historical option name is retained for command-line compatibility.
