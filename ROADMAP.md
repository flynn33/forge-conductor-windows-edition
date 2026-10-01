# Forge Conductor roadmap

## 1.3 workspace and maintenance

Version 1.3.5 is a source candidate. It discovers only loaded LLM instances through LM Studio's native inventory, aligns the durable managed-run goal envelope with bounded instruction-package content, persists a dispatch-pending cursor plan before provider effects, makes instruction cursor admission/replay transactional and crash-recoverable, addresses durable continuity idempotency/checkpoint/repeated-rollover/terminal-cleanup gaps, adds lease-owned pre-ingress orphan cleanup without disturbing live periodic checkpoints, excludes per-user command shims from product-wide tool resolution, and adds direct regression evidence for project-memory Display All and both deletion modes. Fresh post-fix live successor qualification remains pending. See [candidate notes](docs/releases/1.3.5.md).

The release uses the existing native C++20/WinUI architecture, Manager-owned execution, stable project identities, durable memory and native telemetry. Existing runs retain their selected provider binding across restart. Private GitHub policy import uses existing Git authentication and never executes imported scripts.

## Next improvements

- Complete the final release checks, build the uninstalled development-signed MSIX, and then complete installed UI and operator acceptance for the 1.3.5 candidate. Source, protocol, Manager, and live-provider checks do not replace installed interaction evidence.
- Explicit model download/installation choices for hosts without prerequisites.
- Policy-specific semantic validators and richer structured review evidence, with source parity fixtures.
- Unicode-aware policy path scopes; the current gate rejects ambiguous names rather than treating them as allowed.
- Richer contextual help links and policy update comparison.
- Expanded model/hardware compatibility and accessibility acceptance on additional Windows configurations.
- Explicit enrollment and takeover of existing LM Studio desktop chats when a supported native task binding is available. MCP registration alone does not provide this.
- Production certificate distribution and Store delivery.

Historical engineering reports retain their original evidence and open items. They do not establish qualification for later binaries. Current candidate evidence identifies what was actually tested and distinguishes automated contracts, host observations and distribution checks.

The [`v1.3.4` GitHub release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.4) remains the latest published package until the owner publishes a later release.

## Preservation requirements

Future changes must preserve Manager ownership, project isolation, stable package identity, existing user data and immutable migration history. Additional architectures or providers must not replace the native Windows runtime.
