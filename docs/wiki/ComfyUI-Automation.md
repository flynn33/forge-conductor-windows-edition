# Local ComfyUI automation

<p align="center"><img src="images/diagram-comfy.png" alt="Preview, saved approval, then one sealed final render." width="100%"></p>


Forge Conductor 1.3.29 lets an operator describe an image or video in LM Studio while the model handles the installed ComfyUI workflow, dependencies, runtime and output files. Primary and Fallback expose the same eleven ComfyUI tools. Forge remains native C++; ComfyUI, its interpreter, Microsoft Edge and FFmpeg/FFprobe remain external applications.

Use the **ComfyUI automation** card in Settings to enable the service, select the installation and optional model storage, and save the loopback endpoint and limits. Rig provides a readiness probe. Automation is disabled in a new configuration; automatic setup defaults to enabled, downloads to **500 GB per preparation operation**, the free-space reserve to **50 GB**, generation timeout to **30 minutes**, and quality preference to **balanced**. The configured qualification host enabled automation for its existing portable installation. No additional login or authentication setup is introduced.

An empty installation path requests detection of known local installations. An empty model storage path uses the installed model folders. `comfy_status` reports the effective configuration, actual endpoint and installation observations, provider queue, process identity and available GPU facts. A saved setting or a successful probe does not establish that a model can render the requested workflow.

## Model workflow

1. Read status and discover installed node contracts, models and templates. Select a starter or construct an API graph from the actual contracts.
2. Import, inspect and revise the graph through typed operations. Save explicit preview and proposed final graphs, input bindings and intended output node IDs.
3. Run structural and inventory **preflight**, then prepare missing dependencies when needed. ComfyUI performs its own submission validation; preflight cannot promise that a custom node will execute successfully.
4. Submit a managed preview and poll the durable job. Present the verified image or playable video, proposed final settings and approval reply choices.
5. Wait for the operator's saved LM Studio reply: **`approved`**, **`yes`** or **`render final`**. Then call `comfy_run` with only the saved plan ID and final stage.
6. Poll to completion, report actual errors or partial outputs, and deliver the verified files. For video, use the sampled contact sheet with the existing independent `image_analyze` route before describing visible content.

The packaged starters cover SD1 images and Wan 2.2 5B text-to-video and image-to-video. When duration is omitted, model instructions propose a motion draft of about five seconds. Requested quality, installed contracts and measured resources determine the final plan. Unknown graphs receive no universal resolution or frame-count reduction. Playable MP4/H.264 is the normal final video choice; a supported operator-requested alternative belongs in the proposed graph before approval.

| Tool | Purpose |
| --- | --- |
| `comfy_status` | Installation, endpoint, queue, process, GPU observations and effective limits |
| `comfy_catalog` | Paged nodes/schemas, models, local workflows and templates |
| `comfy_workflow` | Typed import, inspect, modify and export without rendering |
| `comfy_validate` | Structural and inventory preflight, missing requirements and intended outputs |
| `comfy_prepare` | Durable dependency resolution, transfer, installation and readiness |
| `comfy_control` | Start, stop or restart the selected installation |
| `comfy_run` | Render a saved preview plan or its previously approved final |
| `comfy_job_status` | Actual phase, progress, errors, approval state and artifacts |
| `comfy_job_list` | Recover project-owned jobs after reconnect or restart |
| `comfy_job_cancel` | Cancel local work and reconcile the exact owned provider prompt |
| `comfy_job_resume` | Resume preparation or reattach to the recorded generation without resubmission |

The frontend path uses a dedicated Edge profile and native CDP to invoke ComfyUI's installed `loadGraphData` and `graphToPrompt`. It retains editor and API graphs for custom widgets, reroutes, bypassed nodes and subgraphs. `desktop_scroll` and `desktop_drag` extend existing desktop tools when a freshly observed UI interaction is necessary. Generation submission stays in the durable service.

## Preparation and recovery

Preparation reuses compatible installed content. Missing nodes resolve through Comfy Registry mappings or uniquely identified publisher sources; catalog-backed models resolve to identified publisher files; Python requirements resolve to compatible pinned packages. The manifest retains resolved versions, sources, hashes, package inventory and actual changes. Unsupported or ambiguous dependency contracts return their actual failure.

An installation lock and shared provider-operation lease serialize setup/restart against rendering. Forge snapshots the affected configuration and package state, verifies staged content before activation, and attempts rollback on failure. A busy queue or unreconciled partial change is reported explicitly. Startup uses the installed interpreter, loopback networking and private Forge input/output namespaces. Lifecycle controls stop only the exact verified Forge-owned runtime.

Native transfer accounting covers metadata, payloads, wheels, retries, error bodies and resumes against the saved operation budget. Free-space checks cover transfers, extraction, staging, backups and cross-volume publication. Publisher build hooks and custom nodes execute with the external interpreter's host permissions; their internal network or filesystem effects are not sandboxed by Forge or counted by its native transfer ledger. The detailed [preparation contract](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/COMFYUI-AUTOMATION.md#dependency-preparation) records supported and unsupported cases.

Each generation seals its exact graph and prompt ID before one POST. A lost acknowledgement produces an uncertain state requiring exact-ID reconciliation. Resume and duplicate final requests do not create replacement generations. One provider workflow executes at a time, with bounded local work; unrelated ComfyUI work is preserved. Cancellation requires matching queue/graph and provider identity before interrupting an owned active prompt.

## Approval and delivery

Approval is bound to the saved final revision, graphs, inputs, dependencies and preview artifacts. The service reads actual saved message positions and selected versions after the delivered preview. Model-supplied approval flags are unsupported. Forge continuity messages are excluded. `verified` acknowledges delivery without approving or revising the plan; another reply requests revision, and changes to creative parameters or sealed content require another preview.

A later chat can recover a pending plan by receiving its exact verified preview through the native ComfyUI tools. The service re-reads the original conversation and compares its unchanged prefix, then verifies the new chat's saved preview receipt and subsequent actual approval. This lets an existing valid reply authorize the unchanged plan without requiring the operator to repeat it in the older chat. A continuity packet alone does not establish preview delivery or approval.

Large media streams through staging files. `artifacts[]` records node ID, provider descriptor, absolute local path, media type, byte count, SHA-256 and measured metadata. Actual content decides the type: core videos can arrive under `images`, VideoHelperSuite videos under `gifs`, and audio under `audio`. FFprobe inspection and a complete FFmpeg decode verify media before publication; posters/contact sheets support bounded delivery. Required output-node coverage must pass before completion. Failed or uncertain publication suppresses successful artifact reporting and retains its evidence.

On the qualified LM Studio 0.4.26+4 host, ordinary chat Markdown links reject loopback URLs. Model instructions therefore use the existing `browser_open` on the exact returned `provider_view_url`, observe the actual browser address with `desktop_read`, and provide the complete local path and contact preview. Browser launch acceptance is separate from observed navigation and playback. Sampled still review does not establish continuous motion quality.

## Qualification

The recorded host uses ComfyUI 0.16.3/frontend 1.39.19, an RTX 4090 for media inference, and the existing local LM Studio connection to a DGX Spark model. Actual native qualification produced image, text-to-video and image-to-video previews, installed a real Registry custom node, reattached an interrupted generation without another POST, and rendered an eight-second draft. A genuine operator-approved text-to-video final completed as 1280 × 704 H.264, 121 frames at 24 fps, 5.041667 seconds, with verified decoding, controlled playback, independent sampled review and native delivery. The provider execution took 214.968 seconds. Separate approved image/image-to-video finals, a real lost-acknowledgement event and operator final quality acceptance remain unqualified.

The [host report](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/COMFYUI_HOST_QUALIFICATION.md) identifies those feature-candidate binaries and evidence separately from the [1.3.29 release verification](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/validation/RELEASE-1.3.29.md). See [Tool Catalog](Tool-Catalog), [Product Surfaces](Product-Surfaces) and [Release 1.3.29](Release-1.3.29).
