# Windows Alpha package inputs

`scripts/package.ps1` turns the committed x64 Release staging manifest into the signed MSIX. It keeps package identity `ForgeConductor.Windows.Alpha`, takes the numeric package version from the CMake product version, substitutes the selected certificate subject, and fails if any manifest placeholder remains.

The payload contains the GUI, Manager, CLI/MCP entry point, SessionHost, self-contained Windows App SDK files, release Visual C++ runtime, WinUI resources, agent resources, Forsetti manifest, assets, and third-party notices. `candidate-provenance.json` records the exact committed source and build identity inside the MSIX; `payload-manifest.json` hashes every pre-package file. The companion distribution records package/certificate hashes and the observed Authenticode signer. Only the public certificate is distributed.

Package creation requires clean committed product and packaging inputs and a Release staging manifest from the same commit/tree. The installation helper validates hashes, signer, stable identity, Local Machine publisher trust, strictly increasing update version, and Windows' post-install registration. `-PreflightOnly` performs those prerequisite checks without deployment, rejects the trust switch before any side effect, and each successful install writes a timestamped receipt. Distribution metadata records hashes for the package, certificate, helper, and README. The R6 continuation supplies the persistent-profile candidate at version 0.9.5.0, SHA-256 `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`. The manifest uses `unvirtualizedResources` with one exact LocalAppData exclusion so the declared durable profile survives package removal. Machine trust, current registration, Start launch, installed tool smoke, and the owner-approved simulated lower-version lifecycle pass.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
