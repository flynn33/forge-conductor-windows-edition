# Known issues

## Open for the 1.3.5 candidate

- **Package construction and installed acceptance are pending.** Version 1.3.5 is currently a source candidate; no 1.3.5 MSIX is claimed by this document until the committed release pipeline emits and verifies one. Native tests, Manager integration tests, and same-host LM Studio checks do not prove the packaged WinUI controls, taskbar launch, upgrade behavior, or installed operator workflow. Installed acceptance must explicitly cover Workspace model discovery and Test, MCP project binding and the instruction-package folder picker, project-memory Browse and Display All, and Data maintenance single-record and multi-selection deletion.
- **A legacy launcher can show a different UI.** A standalone executable left from an older deployment is independent of the registered MSIX and is not updated when the taskbar or Start-menu package is updated. Confirm the displayed package version before recording installed acceptance. Candidate construction does not install, repair, replace, or remove either copy.
- **Existing LM Studio desktop chats are not automatically enrolled.** The qualified automatic-continuity path belongs to Manager-owned runs started with an exact project/provider binding. MCP registration alone does not take over a pre-existing desktop chat.
- **Production signing is not complete.** The planned validation candidate will be development-signed. Production publication still requires the owner's approved signing identity and release process.

## Evidence boundaries

- Work Space discovery and Test use `/api/v1/models` and accept only loaded LLM instance IDs. A live unauthenticated loopback check passed with `openai/gpt-oss-20b`; Forge did not add an authentication path.
- Project-memory **Browse** and **Display All** are record-navigation features. Their 101-record paging proof and the per-record and multi-record deletion proofs do not establish automatic continuity.
- A fresh post-fix live automatic-continuity run is still required. Earlier live evidence predates the final successor-prompt and recovery corrections and is not qualification evidence for this source candidate. Automated continuity regressions do not replace that live Manager-owned create/bootstrap, acknowledgement, predecessor-fence, and productive-resume check.

The [`v1.3.4` GitHub release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.4) remains the latest published package. Historical release notes retain the evidence and limitations of their own binaries.
