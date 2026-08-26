# P03-001: Governance Profile and Source Pins

Status: Accepted

Date: 2026-08-25

## Context

The port is a Forsetti consumer application governed by task contract FAE-TASK-2026-08-24-016. Reproducibility requires one selected platform profile, one manifest contract, and immutable source inputs.

## Decision

- Select the Forsetti Windows edition profile and framework version 0.2.0.
- Select manifest schema 1.1 and manifest template 1.1.
- Identify the product as Forge Conductor 0.9.0 for Windows 11 and use the single module ID com.forsetti.app.forge-conductor-windows.
- Pin the immutable inputs by SHA-256:
  - Forge-Conductor-MacOS-main.zip: 3e344d4b3bb0fff80487f99a7c69e7ceadf22aa1e64da3a6f2640ea2fa0072dd
  - Forsetti-Framework-Windows-main.zip: 3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d
  - forsetti-agentic-edition-main.zip: e8ef20ad917bd3165335c03beecc674f21f69ff93cbc64fe5edb4e9ffd79692b
  - Forge-Conductor-Audit-Bundle.zip: 7be0130778d980567a961c51b5172d0948b4923b134c7dac608f805272f86e6c
- Apply the exact source precedence in AGENTS.md. Record each material conflict and accepted resolution before implementation relies on it.

The selected contracts are release inputs. Changing a profile, schema, template, product version, module identity, or immutable source hash requires a superseding ADR and a new independent validation.

## Consequences

Builds and validation can reject source drift deterministically. Historical audit data remains anti-regression evidence and cannot replace the current 0.9.0 source baseline.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. The consumer uses separately built targets and published headers/interfaces only.

## Evidence

- .forge-codex/instructions/governance/PORT_TASK_CONTRACT.json - SHA-256 cd2714169189e274eb51c3b0bb666b2b59a68042f4c01b0a60f7506ab6ad0547
- .forge-codex/instructions/governance/forsetti-project-context.json - SHA-256 3188896487cc49e2ab3cc54bfe8e8dfd4f8ae1c502b7f9476a18ca38af6c83be
- .forge-codex/instructions/governance/schemas/module-manifest-1.1.schema.json - SHA-256 0d768364790214335ca3b1b585cfd3be2af5e81313b771463f367a61bd0ed90d
- .forge-codex/instructions/governance/source/forsetti-agentic/editions/windows/forsetti-windows-0.2.0.profile.json - SHA-256 4a65c4c986da951cddd2376de39ebc87f1186b21c147c53c122ce0c10ac19c8a
- .forge-inputs/archives/SOURCE-HASHES.json - SHA-256 1032838a2da517f391693bef862167bdb7cf434520ef42e314c2983bc2195cd3
