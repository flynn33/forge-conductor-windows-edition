# P03-002: Sealed Forsetti Consumption

Status: Accepted

Date: 2026-08-25

## Context

ForsettiCore, ForsettiPlatform, and ForsettiHostTemplate are sealed upstream products. The source bundle does not expose a relocatable install package, while the consumer must remain independently buildable and must not absorb framework implementation files into product targets.

## Decision

- Preserve the pinned Forsetti source byte-for-byte.
- Build that source in its own source/build roots and expose the resulting libraries to Forge as separately built imported targets.
- Do not use add_subdirectory to merge the framework project into the consumer build graph.
- Compile consumer code against the published include tree only.
- Limit the Forge module boundary to public contracts including IForsettiAppModule, IForsettiModuleContext, module registration/value models, host bootstrap/controller interfaces, service interfaces, UI contribution models, and Windows view factory/dispatcher interfaces.
- Keep upstream source and build outputs outside Forge product source targets. Source-file copying, unity-build inclusion, private include paths, implementation-header inclusion, subclassing sealed concrete classes, and direct construction of framework internals are prohibited.
- Add Forge behavior through the application-owned module, public extension interfaces, and application-owned adapters. A missing public extension point is recorded as an upstream requirement; it is not repaired in the consumer by patching Forsetti.

## Consequences

Framework provenance remains auditable and upgrades can be reviewed as a pinned dependency change. Consumer tests can substitute public interfaces without coupling to Forsetti implementation layout.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Only public headers/interfaces and separately built imported targets may cross the dependency boundary.

## Evidence

- .forge-codex/instructions/governance/source/forsetti-windows/implementation-policy.json - SHA-256 3b9a1ea2c84aa27f3d1c29f97ab9324095b5fc56a2025ab3e7ad5486bf15e658
- .forge-codex/instructions/governance/source/forsetti-windows/framework-policy.json - SHA-256 2331dffd25a17356457cc64f9fce8fd4b8a8e92f758cea29a13dec44cac9e150
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiCore/ForsettiProtocols.h - SHA-256 7b5ae10f232186f1958369c5021fd6d4c92e961333265fe3d8e72fbe9653f55d
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiCore/ForsettiServices.h - SHA-256 e0776abd915ffd0792e35d1302077fd42419fd7f4bde55a87e0a94ba234afb87
- .forge-codex/instructions/governance/source/forsetti-windows/include/ForsettiHostTemplate/ForsettiHostBootstrap.h - SHA-256 83083eded3387541fbff847ebf3c26e54087f3c807f580ca7ae6710a5b6fc965
- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/CMakeLists.txt - SHA-256 d586bb4b7c9f6d00a987357c0df916718485aba34c13c86a3b5050c32cdbaa3d
