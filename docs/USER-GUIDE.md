# User guide

Launch Forge Conductor from Start. Normal operation has four destinations: **Workspace**, **Rig**, **Activity**, and **Settings**.

## Prepare a workspace

1. Open **Workspace**, register or select the project folder, and select its saved provider profile.
2. Add zero or more instruction-package folders. Each selected folder becomes a project-scoped queue row. Use **Move up** and **Move down** to define the order supplied to new work.
3. Optionally bind a local or remote development-policy source under **CLU governance**, then inspect its immutable revision and coverage.
4. Set **Automatic continuity** independently for this project/provider pair.
5. Review readiness. A missing optional policy or disabled continuity does not block ordinary work.

Forge Conductor remembers the selected project/provider preference and queue. Closing the window does not cancel Manager-owned work.

## Instruction-package folders

A package is a folder, not a special manifest format. Intake walks the complete directory tree and does not reject a package because of file extension, encoding, NUL bytes, path depth, file count, individual size, aggregate size, or filename. Content is hashed incrementally rather than loaded wholesale.

The queue records:

- files and directories in deterministic order;
- reparse points without silently following them;
- metadata and SHA-256 content identities;
- inaccessible or changed entries as explicit failures;
- bounded derived UTF-8 text when safe, otherwise an opaque-content classification.

Open a package to page through its inventory or bounded content. Cursors are tied to the package revision so a changed revision cannot be resumed accidentally. Retry a failed row after correcting its source. Removing a queued row affects future work; active execution retains the revision it already received.

New managed work receives the ordered package identities and available bounded text. Nothing is silently described as “ignored” or “unsupported”; entries that cannot be interpreted as text remain recorded and retrievable by identity.

## CLU governance

CLU governs development-policy compliance only. It never starts, stops, or reports continuity.

Bind a policy source from the Workspace governance card. The binding records its source identity, immutable revision, entry inventory, interpretation state, and coverage gaps. Use the document reader to inspect bounded pages. Refresh deliberately when the source changes.

During development, CLU can:

- evaluate structured evidence against the bound policy;
- create deduplicated findings and correction requests;
- expose pending notification receipts;
- accept correction evidence and resolve findings;
- export a redacted governance history.

Governance is nonblocking by design. No bound policy produces an inactive state rather than preventing work. A finding informs the operator and requests correction; it does not impersonate the independent runtime-continuity system.

The dedicated `clu` MCP role exposes `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve`, and `project_policy.read`.

## Automatic continuity

The **Automatic continuity** toggle is saved per selected project/provider profile. When enabled, a Manager-owned run may use the existing context-capacity continuity path. When disabled, the run skips automatic continuity observations and remains otherwise unchanged.

LM Studio desktop chats and Forge-managed runs are separate modes. Connecting MCP does not retroactively enroll a desktop chat in Manager-owned continuity.

## Rig, Activity, and Settings

- **Rig** shows live resource, provider, storage, process, context, continuity, and workflow health. Missing observations are displayed as unavailable, never as zero.
- **Activity** presents operational outcomes and is the normal place to correlate work with CLU findings, corrections, notifications, and exported evidence.
- **Settings** contains persistent runtime controls, explicitly scoped maintenance actions, and Doctor. With a project selected, Doctor checks package queue/cursor integrity, CLU repository/notification/export state, project memory, continuity provider binding, migrations, and schema alignment.

An enabled automatic-continuity preference can read `Preparing`. That means the Manager has the project/provider preference but release 1.3.0 has not established a live supported-provider successor lifecycle. Only real provider create/restore/acknowledge/fence evidence can justify `Active`.

Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`. For disposable validation only, launch `ForgeConductorApp.exe --alpha-root <absolute-empty-folder>`.
