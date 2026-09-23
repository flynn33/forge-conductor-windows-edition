# Automatic project setup delivery record

Status: implementation in progress; not release qualification.

The setup flow accepts a folder, starts or attaches to the Manager, activates its
service, registers the project, starts an installed LM Studio server when needed,
loads or reuses a compatible downloaded tool model, saves the selected instance,
and verifies a model response. Task entry and tool permission are on this page.
Preparation can be retried and reports the stage that needs attention.

## Evidence collected

- Release backend and WinUI application build passed on the development host.
- ProjectSetupCoordinatorTests passed: stage failures stop dependent work,
  cancellation, missing identities, exception handling and retry.
- WinHttpTransportTests passed: 12 cases / 133 assertions, including model load,
  readback, reuse, malformed or unusable inventory, and provider settings applied
  to new runs while existing runs retain their connection.

These checks do not prove a complete installed user journey or release readiness.

## Required before publication

- Exercise the actual Manager-backed preparation flow in an isolated profile,
  including first task, retry, restart, cancellation and recovery.
- Bound cancellation during model loading; verify server startup failure paths.
- Review persisted provider binding and continuity behavior across Manager restart.
- Add policy repository import, immutable adoption and authorization enforcement.
  The existing instruction-package memory importer is not enforcement.
- Verify the Raven Forge policy source requirements and distinguish machine
  checks from human review obligations; never infer completed review from import.
- Update in-app help, README, changelog, documentation and wiki to match behavior.
- Run the complete Release build, tests, static gates, feature/control inventory,
  package validation and isolated installation lifecycle, preserving evidence.
- Commit, review, merge, publish the release and synchronize the local repository.

## Model API sources

- https://lmstudio.ai/docs/developer/rest/list
- https://lmstudio.ai/docs/developer/rest/load
- https://lmstudio.ai/docs/cli/serve/server-start

Automatic preparation does not download model weights or run repository scripts.
Model metadata must establish tool support and enough loaded context; a model name
alone is insufficient. Missing software or weights must produce an actionable
next step rather than a false ready state.
