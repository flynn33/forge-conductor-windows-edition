# P03-009: Licensing, Provenance, and Attribution

Status: Accepted

Date: 2026-08-25

## Context

The immutable Forge Conductor, Forsetti Framework Windows, and Forsetti Agentic Edition sources each contain an Apache License 2.0 LICENSE file. Forsetti framework-policy.json declares Proprietary and implementation-policy.json rule R008 requires proprietary headers. These supplied statements conflict and cannot be silently collapsed into one assertion.

## Decision

- Preserve the supplied Apache License 2.0 provenance for Forge Conductor and both Forsetti inputs in THIRD-PARTY-NOTICES.md and in source/release SBOM evidence.
- Keep the pinned Forsetti Framework byte-unchanged and separably linked as a separately built dependency. Do not copy or modify its internal implementation.
- Do not relicense upstream source in this consumer repository.
- Do not place the contradictory Forsetti proprietary header on Forge-owned source. That framework-repository rule cannot silently relicense Apache-2.0 source or become a consumer-app copyright assertion.
- Record the proprietary metadata conflict explicitly. The Apache license files remain the concrete distribution evidence for the pinned archives; the contradictory policy metadata remains a release-critical provenance issue if an owner intends different terms.
- Public release packaging must include the applicable full Apache License 2.0 text and supplied Forge NOTICE. A release under incompatible proprietary terms is blocked until the rights holder provides a written, versioned resolution.
- Product artifacts contain no automated-authorship attribution, model/vendor reference, co-author trailer, bot signature, or authorship badge. Required legal copyright and open-source attribution remain intact; legal attribution is not authorship-tool attribution.

This ADR records source evidence and release controls; it is not a legal opinion and does not alter any right granted by the supplied licenses.

## Consequences

Legal notices remain traceable to immutable files, the conflict cannot be hidden by build metadata, and release validation has a deterministic blocking rule.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. No consumer patch may be used to replace, remove, or reinterpret upstream license evidence.

## Evidence

- .forge-inputs/macos/Forge-Conductor-MacOS-main/LICENSE - SHA-256 cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
- .forge-inputs/macos/Forge-Conductor-MacOS-main/NOTICE - SHA-256 760c83d776e8e7e284d4f56ac9733ca87bcfe8d113a5415bef71747ee9ea2b8a
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/LICENSE - SHA-256 6e3ca1bde7ac8930e70eacf814f07b767e6742268c1bd43f90756a48f6a71c9a
- .forge-inputs/forsetti-agentic/forsetti-agentic-edition-main/LICENSE - SHA-256 6e3ca1bde7ac8930e70eacf814f07b767e6742268c1bd43f90756a48f6a71c9a
- .forge-codex/instructions/governance/source/forsetti-windows/framework-policy.json - SHA-256 2331dffd25a17356457cc64f9fce8fd4b8a8e92f758cea29a13dec44cac9e150
- .forge-codex/instructions/governance/source/forsetti-windows/implementation-policy.json - SHA-256 3b9a1ea2c84aa27f3d1c29f97ab9324095b5fc56a2025ab3e7ad5486bf15e658
- .forge-codex/instructions/governance/NO_ATTRIBUTION_POLICY.md - SHA-256 d039ee001a7ca8dd0963f92ee82d0006c730f8229bafb9fcff8b0980e5baec0f
