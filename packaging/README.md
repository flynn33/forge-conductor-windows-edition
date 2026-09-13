# Windows Alpha package inputs

`scripts/package.ps1` turns the committed x64 Release staging manifest into the signed MSIX. It keeps package identity `ForgeConductor.Windows.Alpha`, takes the numeric package version from the CMake product version, substitutes the selected certificate subject, and fails if any manifest placeholder remains.

The payload contains the GUI, Manager, CLI/MCP entry point, SessionHost, self-contained Windows App SDK files, release Visual C++ runtime, WinUI resources, agent resources, Forsetti manifest, assets, and third-party notices. `candidate-provenance.json` records the exact committed source and build identity inside the MSIX; `payload-manifest.json` hashes every pre-package file. The companion distribution records package/certificate hashes and the observed Authenticode signer. Only the public certificate is distributed.

Package creation requires clean committed product and packaging inputs and a Release staging manifest from the same commit/tree. The installation helper validates hashes, signer, stable identity, strictly increasing update version, and Windows' post-install registration. Trusting the internal development publisher remains an explicit administrator action. The telemetry parity closeout supplies the verified candidate at version 0.9.5.0, SHA-256 `78683d3cef440a190932b8f8cb0fe53b39a6a1ac2940a80c7e4da9e4309a8f22`; Windows still lacks machine-level trust for the development publisher, so installed acceptance remains open under the administrator handoff.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
