# Third-Party Notices

This repository preserves the license and provenance of source material used by the Forge Conductor Windows port.

## Forge Conductor for macOS 0.9.0

Copyright 2026 Jim Daley.

The supplied source is licensed under the Apache License, Version 2.0. Its immutable archive has SHA-256 3e344d4b3bb0fff80487f99a7c69e7ceadf22aa1e64da3a6f2640ea2fa0072dd. The exact license and notice evidence is retained at:

- .forge-inputs/macos/Forge-Conductor-MacOS-main/LICENSE
- .forge-inputs/macos/Forge-Conductor-MacOS-main/NOTICE

## Forsetti Framework for Windows 0.2.0

Copyright 2026 James Daley.

The supplied framework source contains the Apache License, Version 2.0. Its immutable archive has SHA-256 3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d. The exact license evidence is retained at:

- .forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main/LICENSE

## Forsetti Agentic Edition 1.1.0

The supplied governance source contains the Apache License, Version 2.0. Its immutable archive has SHA-256 e8ef20ad917bd3165335c03beecc674f21f69ff93cbc64fe5edb4e9ffd79692b. The exact license evidence is retained at:

- .forge-inputs/forsetti-agentic/forsetti-agentic-edition-main/LICENSE

Forsetti Agentic Edition is governance/build evidence and is not an installed runtime dependency.

## nlohmann/json

nlohmann/json 3.12.0 is selected through the pinned vcpkg port, which declares the MIT license. Release packaging must reproduce the installed port copyright/license file and include it in the SBOM and distribution notices.

## License-text and metadata rule

Source and binary distributions must include the applicable complete Apache License 2.0 text and supplied notices. The license is available at https://www.apache.org/licenses/LICENSE-2.0.

The Forsetti framework also contains framework-policy.json metadata declaring Proprietary and implementation-policy.json rule R008 requiring proprietary headers. This conflicts with the supplied Apache License 2.0 files. This notice preserves both facts and does not relicense or resolve the conflict. P03-009 defines the release blocking rule.
