# Windows Alpha package inputs

`scripts/package.ps1` turns the committed x64 Release staging manifest into the signed MSIX. It keeps package identity `ForgeConductor.Windows.Alpha`, takes the numeric package version from the CMake product version, substitutes the selected certificate subject, and fails if any manifest placeholder remains.

The payload contains the GUI, Manager, CLI/MCP entry point, SessionHost, self-contained Windows App SDK files, release Visual C++ runtime, WinUI resources, agent resources, Forsetti manifest, assets, and third-party notices. `candidate-provenance.json` records the exact committed source and build identity inside the MSIX; `payload-manifest.json` hashes every pre-package file. The companion distribution records package/certificate hashes and the observed Authenticode signer. Only the public certificate is distributed.

Package creation requires clean committed product and packaging inputs and a Release staging manifest from the same commit/tree. The installation helper validates hashes, signer, stable identity, strictly increasing update version, and Windows' post-install registration. Trusting the internal development publisher remains an explicit administrator action. The R7 delivery retains the unchanged verified candidate at version 0.9.2.0, SHA-256 `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`; Windows returned `0x800B0109` without machine-level trust, so installed acceptance remains open.

<!-- alpha-phase-review:start -->
Phase review: R7 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
