# P03-010: G03 Validation and Upstream Defects

Status: Accepted

Date: 2026-08-25

## Context

The supplied Forsetti validation scripts are useful upstream evidence but do not fully validate this consumer repository. Treating their successful exit alone as G03 proof would create false assurance.

## Decision

G03 uses a fail-closed consumer validator that checks the task contract, selected Windows profile, canonical manifest and mirror, exact capability/I/O/UI/isolation values, public-header-only consumption, one app-module identity, architecture decisions, source pins, license conflict, no internal framework changes, and ledger/evidence integrity. A fresh Validator-role session owns the gate decision; the builder cannot approve its own work.

The following upstream defects are recorded with their required consumer disposition:

1. UP-001 - Windows profile validation is shallow while deep source-contract checks are Apple-only. The consumer validator performs the Windows source-contract checks explicitly.
2. UP-002 - Contract mode checks only nested project context and never invokes contract_rules.ps1. The consumer validator checks the complete task contract and authorized scope.
3. UP-003 - Dependency, capability, and module-isolation modes report pass when no changed files are supplied. Consumer validation supplies a concrete scope or scans the full target tree and rejects empty evidence.
4. UP-004 - The Windows dependency check catches only Core importing Platform/Host and does not enforce the full include and CMake graph. Consumer validation checks the complete accepted graph.
5. UP-005 - The module-isolation regex falsely flags legitimate ISharedDatabaseService usage. Consumer rules distinguish the public interface name from direct module dependencies.
6. UP-006 - Windows moduleID and entryPoint patterns are not checked because those checks are Apple-only. Consumer validation applies the schema patterns to both fields.
7. UP-007 - scripts/validate-repo.ps1 fixes RepoRoot to the Forsetti Agentic Edition source and is not a consumer-repository validator. It is not used as sole G03 authority.
8. UP-008 - The framework check-manifests.ps1 scans only src, templates, samples, and tests and exits successfully when no manifest is found. Consumer validation requires the canonical manifest path and one Forge app-module identity.
9. UP-009 - The framework manifest script accepts schema 1.0 although the selected Windows profile requires 1.1. Consumer validation requires schema/template 1.1.
10. UP-010 - crypto_utilities contracts disagree across C++ declarations, schema I/O kinds, and profile capability lists. Forge does not request crypto_utilities and uses the declared secure_storage/security services.
11. UP-011 - verify-forsetti-guardrails.ps1 can build a consumer root while auditing framework paths because its build and audit roots are mixed. Consumer and framework validation use explicit, separate roots.
12. UP-012 - The framework CMake project is non-relocatable and has no install/export package. Forge builds the pinned framework separately and imports public targets without editing upstream internals.
13. UP-013 - The Forsetti Apache License 2.0 LICENSE conflicts with proprietary framework-policy and header metadata. P03-009 and THIRD-PARTY-NOTICES.md preserve both facts and impose the release blocking rule.
14. UP-014 - Framework vcpkg baseline cb2981c4e03d421fa03b9bb5044cd1986180e7e4 lacks versions/baseline.json, so manifest-mode restore fails deterministically. The approved consumer workaround is a standalone byte-unchanged Forsetti build with nlohmann-json preinstalled through vcpkg classic mode; the sealed source is never patched.

These defects are compensated in the consumer. They are not fixed by modifying the immutable inputs or sealed framework internals.

## Deterministic upstream test limitations

The standalone framework configured and built in Debug and Release after the classic-mode dependency preinstall. The Debug CTest run failed in ForsettiCoreTests with these four deterministic upstream tests:

- ScopedServices_StorageAllowedWithCapability (unhandled bad_any_cast)
- ScopedServices_StorageRejectsTraversalKeys (unhandled bad_any_cast)
- Runtime_V11UIContributionIDsMustBeDeclared (unhandled exception from ModuleRegistry::makeModule through ModuleManager)
- Runtime_V11DeclaredThemeMaskIsPreserved (unhandled exception from ModuleRegistry::makeModule through ModuleManager)

ForsettiPlatformTests and ForsettiArchitectureTests passed in that Debug run. The full upstream test suite therefore did not pass, and G03 does not claim otherwise. Recorded command evidence:

- Manifest-mode configure failure: .forge-codex/state/commands/20260825T093342537Z-cf79732f.json, exit 1, SHA-256 15dd060166210fb45106c956b0c14559fa1e39eb1828c4796f848289d9b07992
- Classic dependency install: .forge-codex/state/commands/20260825T093451916Z-b6fcf5ca.json, exit 0, SHA-256 585e16447dac12d25fa07d7dde8e5eabf5cdee52590146839ab91be2d289abf7
- Fresh standalone configure: .forge-codex/state/commands/20260825T093633245Z-35b54512.json, exit 0, SHA-256 4fbfd29eb97fc5d1cce850ee7653b8d06906d7b224c326ceeded154c0c1a2d40
- Debug build: .forge-codex/state/commands/20260825T093648999Z-82d9497b.json, exit 0, SHA-256 97698f0cf281f064a315dceaeb6502c6a504a9c59b6d071bc4ec16d73605fd9c
- Debug CTest: .forge-codex/state/commands/20260825T093719995Z-2f4e92bd.json, exit 1, SHA-256 e4fad91d7cf8df5aceed8ea2971267f164289dba718ef357a09f7f4153a67fa0
- Release build: .forge-codex/state/commands/20260825T093810695Z-2db748d9.json, exit 0, SHA-256 3b7bf35d1aefc31b0c7ebbb51b0df452b16f37c9ac5b9d3d4945ea7b02f8860d

## Consequences

G03 evidence is consumer-specific, exact, independently judged, and resistant to empty-scope success. Upstream validation remains supplemental evidence and its deterministic failures remain visible.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Validator and build defects are mitigated with consumer-owned checks, separately rooted builds, and classic-mode dependency provisioning, not patches to upstream scripts or runtime sources.

## Evidence

- .forge-inputs/forsetti-agentic/forsetti-agentic-edition-main/core/validator/forsetti_validate.ps1 - SHA-256 dea0a2a144953589448143b6538702c5050b3d03a16bb257a8f77aa90215ab13
- .forge-inputs/forsetti-agentic/forsetti-agentic-edition-main/core/validator/contract_rules.ps1 - SHA-256 2f1deef32d1b424b0a8df732451e0d9566f5102f4a4f4d207eb090b30fd55844
- .forge-inputs/forsetti-agentic/forsetti-agentic-edition-main/scripts/validate-repo.ps1 - SHA-256 568dadd36c056907296900a6293abb3486a0a3fd0197e3949a3a342be3710d59
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/Scripts/check-manifests.ps1 - SHA-256 f5b6c2392a31edfab222949068274f19e045314beaa70e2f4bea0a7f7565d859
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/Scripts/verify-forsetti-guardrails.ps1 - SHA-256 340b057e3d797d5477b8c6df2ee6163bfe0a04c420d0366e147284d8e85d9267
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/CMakeLists.txt - SHA-256 d586bb4b7c9f6d00a987357c0df916718485aba34c13c86a3b5050c32cdbaa3d
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/LICENSE - SHA-256 6e3ca1bde7ac8930e70eacf814f07b767e6742268c1bd43f90756a48f6a71c9a
