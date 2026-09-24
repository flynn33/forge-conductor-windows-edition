# User guide

Launch Forge Conductor from Start. Guided setup opens until the first setup task has started successfully. You can always reopen it through Start here in the sidebar or the visible setup button on other pages; no hidden guide switch is required.

## Start a project

1. Open **Start here · Guided setup → Choose folder and prepare**. Select an existing folder or create one in the Windows picker.
2. Wait for all five checks: Manager, project, LM Studio plugins, model, and connection. Forge Conductor starts its services, registers the folder, installs and verifies Primary/Fallback/Continuity plugins while preserving other plugins, opens LM Studio and verifies synchronization, starts its local server when needed, loads a compatible downloaded model, saves its selection, and verifies a response.
3. In step 2, optionally import and review your development policy. Then, in step 3, describe what you want to build or fix. Review the file/command permission beside the task, then choose **Start task**. Autonomy displays the actual Manager-owned run and its results.

If a check fails, its message identifies the next action. **Retry preparation** reuses the project and checks the current model state. **Cancel** stops pending preparation; already completed registration or model loading is not undone. The selected folder is remembered per profile.

Preparation requires LM Studio and a downloaded tool-capable model with sufficient context capacity. It does not download weights, install project dependencies, or create starter files. Missing prerequisites produce an actionable result. Provider settings remain available for selecting another loopback address or model.

## Development policy and governance

Expand **Development policy and governance** after selecting a project. Paste a GitHub repository URL, such as `https://github.com/flynn33/raven-forge-development`, or a local source folder. Choose **Import preview**. Private GitHub repositories use the host's existing Git sign-in; credentials are not copied into the application.

Review the source, immutable commit, snapshot digest, file list and exclusions. **Read document** and **Next part** display complete text in bounded pages. Choose **Adopt revision** to make the policy mandatory for the selected project. Adoption immediately blocks write tools and commands until a review is accepted.

Complete the source and project review required by your policy, then record the reviewer, evidence location, permitted paths, prohibited paths, and exact approved commands. The confirmation is the reviewer's attestation that all applicable obligations are resolved. An empty permitted-path list allows no file edits. Native policy path scopes currently require ASCII names and reject Windows short-name aliases; they fail closed for other names. Reimporting and adopting another revision clears the old review.

The Manager checks policy integrity, review acceptance, permitted file paths and exact command arguments at the common tool authorization boundary. New runs receive the policy identity and can retrieve its documents using `project_policy.read`; that tool cannot adopt a policy or approve a review. Imported scripts are never run by import.

Prose requirements, architectural judgments and the contents of review evidence need human assessment. Import is not proof of semantic compliance. Approved shell commands run with existing native command restrictions; they are not an operating-system filesystem sandbox. Review their scripts and effects before approval. Stop active work before changing its governing revision.

Policy snapshots support up to 2,048 files, 2 MiB per text file and 16 MiB total text. Unsupported files are listed for review or justified exclusion. Oversized, malformed or unsafe sources fail without replacing the adopted snapshot. Instruction packages remain a separate, smaller guidance feature under Projects.

## Help and results

Open **Guided setup → Help and troubleshooting**. All 42 searchable articles work offline, without a model. Search terms include project, model, policy, tool, memory, context and evidence.

A successful setup check verifies a model response. A successful task additionally requires the requested tool outcomes and output checks. Use **Events & Evidence** to inspect actual operations and correlate the project/run identity. Model text alone is not proof that files changed.

## Navigation

- **Rig:** system, process, provider, storage and workflow telemetry. Missing observations are not zero measurements.
- **Start here · Guided setup:** automatic preparation, task entry, policy import and offline help.
- **Autonomy:** start, attach, pause, resume and stop Manager-owned runs.
- **Continuity:** handoff and retained-context state.
- **Projects:** folder registration, stable identities, aliases, memory and instruction packages.
- **Tools:** invoke authorized native tools in the selected project.
- **LM Studio plugins:** use the installation controls at the top to install/repair all three plugins, check them, or open LM Studio and synchronize. For desktop chats, select the desired plugin in the LM Studio chat Integrations panel.
- **Settings:** saved runtime configuration and scoped maintenance.

Closing the window does not cancel Manager-owned work. New runs use current provider settings; existing run bindings persist across Manager restarts. An unavailable pinned model or provider response requires attention rather than silently switching its history.

Ordinary LM Studio desktop chats and Forge-managed runs are separate modes. MCP connection does not retroactively enroll a desktop chat into managed continuity.

Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`. For disposable validation only, launch `ForgeConductorApp.exe --alpha-root <absolute-empty-folder>`.
