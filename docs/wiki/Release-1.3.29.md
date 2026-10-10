# Forge Conductor 1.3.29 — Local ComfyUI automation

Forge Conductor **1.3.29 / Windows package 1.3.29.0** adds general local ComfyUI image and video automation through the existing Primary and Fallback integrations. An operator describes the result in LM Studio; the model selects or builds a workflow, prepares dependencies, starts the installed runtime, renders a preview, waits for the operator's saved chat approval and delivers the verified final files.

The catalog contains **125 Primary/Fallback tools**, adding eleven `comfy_*` routes, `desktop_scroll` and `desktop_drag` to the previous 112. All six legacy image-provider tools and their receipt contracts remain. CLU retains its five governance tools and the ten specialist playbooks remain. No fourth integration or authentication setup is added.

Settings adds the ComfyUI automation card for the existing installation, model storage, endpoint, automatic setup, transfer budget, disk reserve, generation timeout and quality preference. Rig adds readiness observations. Automatic setup defaults to enabled, the preparation budget to 500 GB, the free-space reserve to 50 GB and generation timeout to 30 minutes. Automation remains opt-in in a new configuration.

## Workflow and runtime

Native HTTP/WebSocket control discovers installed node/model contracts, submits sealed graphs, observes progress and reconciles exact provider history. A dedicated Edge session and native CDP use the installed frontend serializer for editor graphs, including custom widgets, literal arrays, reroutes, bypassed nodes and subgraphs. Typed workflow operations do not submit generation. `comfy_validate` is structural and inventory preflight; ComfyUI's submission-time custom-node errors remain explicit.

Starters cover SD1 images and Wan 2.2 5B text-to-video and image-to-video. Preparation reuses installed content, resolves identified missing nodes/models/packages, pins versions and retains a manifest. Native byte accounting spans preparation phases, retries and resumes; disk reserve checks cover staging, extraction and publication. Installation changes and restarts serialize against rendering, snapshot affected state, and report rollback or unreconciled changes. Forge remains native while the existing ComfyUI interpreter and media components remain external applications.

## Approval, durability and outputs

A saved plan contains explicit preview/final graphs, input seals, dependency identities and intended outputs. Only a subsequent saved operator reply—`approved`, `yes` or `render final`—can authorize its unchanged final revision. Message position/version evidence excludes Forge continuity messages. A delivered preview can be recovered in a later chat with the original conversation and exact artifact evidence; a previously entered valid approval need not be repeated. `verified` acknowledges delivery without approving or requesting revision. Graph, input, dependency or creative changes invalidate approval.

The prompt ID and graph are saved before a single generation POST. Uncertain acknowledgement requires exact reconciliation; resume and duplicate final calls do not resubmit. Owned queue cancellation preserves unrelated work. Streamed artifact collection identifies actual media content, verifies output coverage and records absolute paths, byte counts, SHA-256 and measured metadata. Video/audio inspection uses FFprobe and full FFmpeg decoding, with bounded posters/contact sheets and normal final MP4/H.264 delivery.

The installed LM Studio host rejects ordinary loopback Markdown link opening. Existing browser and desktop tools now provide explicit video delivery and observed address evidence, with the complete local path and bounded contact preview. Browser launch, navigation, media playback, sampled visual findings and operator quality acceptance remain separately measured.

## Verification scope

The pre-version-bump Build36 feature candidate passed Product All, **53 Comfy service regression groups**, all three static gates, package persistence and **177/177 Release tests in 212.47 seconds**. These results identify that candidate, rather than the later versioned package. The versioned Release build, signed-package/install checks, CI and publication readback are recorded separately in the [1.3.29 verification record](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/validation/RELEASE-1.3.29.md); those steps were pending when this source page was prepared.

Actual feature-candidate host qualification used ComfyUI 0.16.3/frontend 1.39.19 and the RTX 4090, with model serving through the existing local LM Studio/DGX Spark connection. Image, text-to-video and image-to-video previews completed. A real Registry custom-node installation and exact interrupted-job reattachment without a new POST passed. The eight-second video draft received genuine operator approval in its delivery chat. Its saved 1280 × 704 H.264 final, 121 frames at 24 fps and 5.041667 seconds, rendered in **214.968 seconds** and passed complete decode, required output coverage, controlled play/seek/resume, independent sampled review and native delivery with exactly one final submission.

These observations do not establish separate approved image or image-to-video finals, an actual lost-acknowledgement event, continuous motion quality from sampled frames or operator final quality acceptance. Publisher build-hook network effects remain outside native transfer accounting. Unsupported dependency contracts return their errors rather than being represented as prepared. See the [full host qualification](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/COMFYUI_HOST_QUALIFICATION.md) for actual timings, shared-GPU observations, failures and visual limits.

See [ComfyUI Automation](ComfyUI-Automation), [Tool Catalog](Tool-Catalog), [Validation Gates](Validation-Gates), [source release notes](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/releases/1.3.29.md) and the retained [1.3.28 release](Release-1.3.28).
