# Automatic project setup delivery record

Version: 1.2.0. Implementation is complete; release qualification is recorded separately in [the validation report](../validation/AUTOMATIC-SETUP-1.2.0.md).

The coordinator performs Manager activation, stable project registration, model preparation and response verification. Setup shows progress, retry/cancellation and task entry. The Windows model service uses native loopback HTTP and the installed LM Studio CLI. It checks tool capability and actual context capacity; missing software or weights produces an actionable error.

The policy service imports a bounded text snapshot from a local folder or immutable GitHub commit. Native HTTPS supports public sources; normal Git credential-helper authentication supports private sources through a no-checkout object reader. Repository scripts, hooks and filters are not executed. Unsupported files are reported.

The Manager persists adopted snapshots and human review records atomically. The common authorizer checks integrity, review status, path restrictions and exact commands. Policy retrieval is read-only and paginated. Private state cannot overlap authorized project roots. Arbitrary semantic obligations still require human assessment.

Provider bindings are persisted before run admission and after response creation. Existing runs and known response chains retain their endpoint and model through restart. Unknown response bindings fail explicitly. New runs use current saved settings.

## Host evidence already collected

- Actual native folder picker and setup completed in an isolated profile with qwen3.8-27b.
- The first task wrote hello.txt containing exactly `Automatic setup works.` and read it back; the on-disk contents were independently checked.
- Raven Forge private repository intake resolved commit ed0028a46bac9c5b92876a6ad6589ca421fd9499: 204 text files, 26 reported exclusions.
- Real Manager tests exercise registration reuse, model pinning, actual file writes, pending-review denial, accepted scope, forbidden paths and commands, and persisted review readback.

## API research

- https://lmstudio.ai/docs/developer/rest/list
- https://lmstudio.ai/docs/developer/rest/load
- https://lmstudio.ai/docs/cli/serve/server-start

Full logs and exact release provenance belong to the release evidence archive; model-generated text is not a substitute for native outcomes.
