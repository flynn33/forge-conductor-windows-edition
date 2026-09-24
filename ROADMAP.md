# Forge Conductor roadmap

## 1.2 automatic setup and governance

Version 1.2.1 delivers folder-to-task preparation, automatic deployment and synchronization of all three LM Studio plugins, a persistent visible setup entry, installed LM Studio server startup, compatible model loading, response verification, retry/cancellation, pinned repository policy import, reviewed tool permissions and 42 offline help articles. See [release notes](docs/releases/1.2.1.md) and [validation evidence](docs/validation/SETUP-CORRECTION-1.2.1.md).

The release uses the existing native C++20/WinUI architecture, Manager-owned execution, stable project identities, durable memory and native telemetry. Existing runs retain their selected provider binding across restart. Private GitHub policy import uses existing Git authentication and never executes imported scripts.

## Next improvements

- Explicit model download/installation choices for hosts without prerequisites.
- Policy-specific semantic validators and richer structured review evidence, with source parity fixtures.
- Unicode-aware policy path scopes; the current gate rejects ambiguous names rather than treating them as allowed.
- Richer contextual help links and policy update comparison.
- Expanded model/hardware compatibility and accessibility acceptance on additional Windows configurations.
- Explicit enrollment and takeover of existing LM Studio desktop chats when a supported native task binding is available. MCP registration alone does not provide this.
- Production certificate distribution and Store delivery.

Historical engineering reports retain their original evidence and open items. They do not establish qualification for later binaries. Current release evidence identifies what was actually tested and distinguishes automated contracts, host observations and distribution checks.

## Preservation requirements

Future changes must preserve Manager ownership, project isolation, stable package identity, existing user data and immutable migration history. Additional architectures or providers must not replace the native Windows runtime.
