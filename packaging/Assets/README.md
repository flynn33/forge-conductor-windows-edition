# Starter Windows package assets
The supplied PNGs use the exact pixel dimensions in their names; the ICO has multiple native icon sizes.
They are a simple geometric FC monogram, with no font files or third-party image downloads bundled.
Copy the four manifest-named PNGs to the implemented packaging Assets directory and reference the actual paths.
Use the ICO for the native executable resource as appropriate to the WinUI project.
These assets are copied into the signed Alpha MSIX and referenced by its generated manifest. Their presence is verified as part of package creation.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](../../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
