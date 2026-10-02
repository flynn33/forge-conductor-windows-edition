# Known issues

## Open for the 1.3.5 candidate

- **Installed acceptance is pending.** An uninstalled development-signed 1.3.5 MSIX candidate exists, but native tests, Manager integration tests, simulated lifecycle validation, and same-host LM Studio checks do not prove the packaged WinUI controls, taskbar launch, upgrade behavior, or installed operator workflow. Installed acceptance must explicitly cover Workspace model discovery and Test, MCP project binding and the instruction-package folder picker, project-memory Browse and Display All, and Data maintenance single-record and multi-selection deletion.
- **A legacy launcher can show a different UI.** A standalone executable left from an older deployment is independent of the registered MSIX and is not updated when the taskbar or Start-menu package is updated. Confirm the displayed package version before recording installed acceptance. Candidate construction does not install, repair, replace, or remove either copy.
- **Existing LM Studio desktop chats are not automatically enrolled.** The qualified automatic-continuity path belongs to Manager-owned runs started with an exact project/provider binding. MCP registration alone does not take over a pre-existing desktop chat.
- **Production signing is not complete.** The planned validation candidate will be development-signed. Production publication still requires the owner's approved signing identity and release process.

## Evidence boundaries

- Work Space discovery and Test use `/api/v1/models` and accept only loaded LLM instance IDs. A live unauthenticated loopback check passed with `openai/gpt-oss-20b`; Forge did not add an authentication path.
- Project-memory **Browse** and **Display All** are record-navigation features. Their 101-record paging proof and the per-record and multi-record deletion proofs do not establish automatic continuity.
- A fresh post-fix disposable live automatic-continuity run passed with loaded model `openai/gpt-oss-20b`, including Manager-owned create/bootstrap, acknowledgement, predecessor fencing, and productive successor work. Installed operator acceptance remains open.

The [`v1.3.4` GitHub release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.4) remains the latest published package. Historical release notes retain the evidence and limitations of their own binaries.
