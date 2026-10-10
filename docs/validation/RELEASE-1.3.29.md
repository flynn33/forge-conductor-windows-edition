# Forge Conductor 1.3.29 release verification

The release version is **1.3.29**, with Windows package version **1.3.29.0**. This record separates the versioned release build and installation from the earlier feature qualification candidate.

Versioned Product All, Release tests, package verification, installation and publication are pending at this source snapshot. The completed measurements will be recorded here after the clean source commit has been built and packaged. Earlier Build36 results belong to that candidate and are not measurements of a 1.3.29 package.

The earlier feature candidate passed Product All, all 177 configured Release tests, static gates and package persistence. Actual host observations include image, text-to-video and image-to-video previews, Registry custom-node installation, exact-job recovery after Manager interruption, and one approved final H.264 video. The final was 1280×704, 121 frames at 24 fps, approximately 5.04 seconds, with a measured provider execution time of 214.968 seconds. Its exact graph was submitted once. Independent sampled visual review and controlled playback completed; operator final quality acceptance remains unobserved. The separate longer preview was approximately 8.06 seconds. The image and image-to-video final approval paths have no actual operator-approved final result in this record.

See the [release notes](../releases/1.3.29.md), [ComfyUI automation guide](../COMFYUI-AUTOMATION.md) and [complete host qualification chronology](../COMFYUI_HOST_QUALIFICATION.md) for the contracts, measured cases, failed attempts and limits.

The existing hosted signing workflow requires repository secrets `FORGE_SIGNING_PFX_BASE64` and `FORGE_SIGNING_PASSWORD`. Inspection found neither configured. The intended distribution uses the already installed local development publisher certificate, whose public certificate is trusted on this host. No private key is distributed.
