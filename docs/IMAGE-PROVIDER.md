# Optional local image provider

This feature adds generation and source-image editing through an explicitly configured local ComfyUI server. It preserves the existing image drawing, reading, pixel measurements, desktop capture and reviewer tools. The provider is disabled by default. The root-owned isolated direct-native ComfyUI smoke passed six bounded cases, separately from the ten-target automated pass. Integrated 1.3.23 build/package, Manager IPC and LM Studio qualification remain pending; semantic editing and full host image-model quality are unverified. See [the current pending qualification](validation/HOST-CAPABILITIES-1.3.23.md) and [the retained 1.3.22 provider measurements](validation/HOST-CAPABILITIES-1.3.22.md).

The initial profile is `sd1`. It constructs a fixed core-node workflow with `CheckpointLoaderSimple`, positive and negative `CLIPTextEncode`, `KSampler` (`dpmpp_2m`, `karras`), `VAEDecode` and `SaveImage`. Generation starts with `EmptyLatentImage`. Editing uses `LoadImage` and `VAEEncode`; a red-channel mask uses `LoadImageMask` and `VAEEncodeForInpaint`. No arbitrary workflow, Python node installation, model download, external account or new runtime dependency is required by Forge. ComfyUI and a compatible checkpoint must already be installed and served separately by the owner.

An SD1 checkpoint such as the locally observed `cyberrealistic_final.safetensors` is a candidate for qualification. Finding its filename in the provider inventory does not prove that it is loaded, fits the GPU, supports the requested image quality, or is compatible with a different architecture. This initial profile does not establish parity with the host image-generation model for text rendering, instruction following, background removal, multiple reference images, or general semantic editing.

## Configuration

Configure the existing owner-managed Forge configuration document with an explicit block:

```json
{
  "image_provider": {
    "enabled": true,
    "endpoint": "http://127.0.0.1:8188",
    "profile": "sd1",
    "checkpoint": "cyberrealistic_final.safetensors"
  }
}
```

Endpoints must be canonical `http://127.0.0.1:PORT`, `https://127.0.0.1:PORT`, `http://[::1]:PORT` or `https://[::1]:PORT`, with an explicit port in 1–65535. Hostnames, remote hosts, credentials, paths, queries, fragments and leading-zero ports are rejected. HTTPS uses normal Windows certificate validation. The HTTP transport disables proxies, automatic credential/cookie use and redirects. There is no default endpoint discovery, automatic server startup or fallback to a paid provider. The checkpoint is a safe `.safetensors` basename; it cannot be a path. Unknown owner fields in the configuration document are retained.

## Tools

| Tool | Effect | Contract |
| --- | --- | --- |
| `image_provider_status` | Read | Return explicit configuration and current core-node/checkpoint inventory availability. It does not load a model or generate an image. |
| `image_generate` | Write | Admit a bounded generation, capture destination preconditions and return a durable job receipt. |
| `image_edit` | Write | Capture authorized source/mask pixels, submit the fixed edit workflow and return a durable job receipt. |
| `image_job_status` | Read | Read local durable facts, optionally wait, and observe an exact remote ID after interruption. It cannot submit, resume or publish an artifact. |
| `image_job_cancel` | Write | Seal local publication suppression and stop the local worker. An exact pending prompt may be removed from the provider queue. |
| `image_job_resume` | Write | Explicitly reattach to an existing exact remote prompt after fresh authorization. It can retrieve/publish that artifact, and cannot resubmit generation or upload a replacement source. |

Example generation:

```json
{
  "prompt": "A watercolor landscape with a small red cabin",
  "negative_prompt": "",
  "path": "D:/MyProject/artifacts/landscape.png",
  "seed": 8123,
  "width": 512,
  "height": 512,
  "steps": 20,
  "cfg": 7,
  "denoise": 1,
  "preview_max_dimension": 1024,
  "timeout_sec": 1800
}
```

Pass this object to `image_generate`. For `image_edit`, add an absolute `source_path` and optionally an absolute `mask_path`. The source, mask and requested output dimensions must agree exactly. Mask red values select editing: 0 retains the original, 255 selects the generated image, and intermediate values blend all four RGBA channels. Forge composites the final output natively, so pixels under mask red 0 retain the captured original RGBA bytes, including RGB beneath nonopaque alpha. This pixel contract does not promise that the model follows the semantic edit prompt.

Required parameters are the prompt, absolute `.png` destination, explicit seed, width and height. Dimensions are multiples of eight in 64–1024; seed is an integer in 0–9,007,199,254,740,991. Optional steps are 1–100 (default 20), CFG 0–20 (default 7), denoise 0.05–1 (default 1), timeout 1–3600 seconds (default 1800) and preview dimension 128–2048 (default 256). The denoise floor also bounds the installed SD1 sampler's expanded schedule. Prompt and negative prompt are each bounded to 4096 UTF-8 bytes. Input images and downloaded artifacts are at most 16 MiB, a single WIC-decodable frame, and at most 1024×1024 pixels. Invalid arguments are rejected before file, upload or generation effects.

Poll or wait for the returned `job_id`:

```json
{"job_id":"ACTUAL-RETURNED-UUID","wait_sec":30}
```

`wait_sec` is optional, defaults to zero and is bounded to 60. Cancel or explicitly resume with `{"job_id":"ACTUAL-RETURNED-UUID"}`. Cancellation and resume have no destination/provider override arguments.

## Receipts, authority and recovery

An outer `ok: true` means that a job receipt was read successfully. The actual `state`, `done`, `error`, `remote_state`, `submission_acknowledged` and `artifact_published` fields describe the work. A submission receipt, `queued` state or lost acknowledgement is not a completed image. Artifact byte count, PNG SHA-256 and canonical top-to-bottom tightly packed RGBA8 SHA-256 are returned as actual published measurements; absent artifacts have null measurements. Completed status rechecks the published file and pixel seals before presenting it.

The authenticated same-Windows-user/profile/project Manager boundary owns the durable job. A per-CLI identifier is provenance, and cannot authorize reconnect by itself. The CLI-to-Manager scope envelope is internal to the authenticated transport; public tool schemas cannot supply it. The Manager intersects supplied canonical roots/grants/denials with current policy. Saved scope does not confer authority. Fresh reads/effects must still fit current project, principal, roots and grants; effectful resume checks the same configured endpoint/profile/checkpoint and the original destination precondition. Public MCP write tools retain the existing Write-intent/Write-grant policy. New destinations additionally require Create, and reading preconditions and source images requires Read.

Forge seals the exact prompt ID and workflow before its only generation POST. A lost response remains unknown and is never automatically repeated. After an interrupted Manager, read-only status may query the exact saved ID and workflow. Explicit resume requires that the previous owner be released or proven ended, current permissions and provider agree, captured source/mask seals agree, and the destination is unchanged (or matches the saved publication intent). If the remote ID is absent or the workflow differs, resume cannot invent completion or submit it again. Reattachment has a fresh bounded waiting interval, and does not restart provider generation.

Local cancellation suppresses publication. The native owner can stop its local worker with current owned-project Read scope even if the provider changed or destination effect grants narrowed; remote queue deletion additionally requires unchanged provider settings and fresh effect permissions. The public MCP Write-tool admission policy still applies, so a caller that loses all Write permission cannot reach cancellation through that public route. It does not claim remote termination: `remote_cancel_confirmed` remains false. Only the exact owned pending ID may be deleted. Forge does not call the shared `/interrupt`, global queue clearing, memory unload, server stop or process-kill endpoints. Active provider work may continue after cancellation or timeout. An explicit authorized resume can later collect the already completed exact job.

Destinations are authorized and freshly checked before atomic replacement. The receipt records the admitted presence/hash precondition. External changes fail the job instead of being silently accepted. The existing atomic-file boundary protects publication and path authority; a hash seal is integrity evidence, not origin authentication. It is not a distributed compare-and-swap against arbitrary external writers. ComfyUI uploads and outputs remain in its owned per-job namespace; Forge does not delete foreign provider files or clear shared history.

The Manager permits at most eight active jobs and retains at most 32 evidence directories per project. Admission can retire the oldest verified completed or definitively failed private evidence; published user images are retained. Live owners, entries borrowed by an in-flight call, unknown/cancelled remote work, malformed receipts, reparse points and unexpected private children are preserved and occupy their slots. If all 32 slots contain such evidence, admission fails before provider effects. Retired receipts are no longer available to status. Provider JSON is bounded to 512 KiB and depth 32; typed receipts are bounded to 128 KiB. Output previews preserve alpha through native WIC encoding, retain the old 256-pixel default, never upscale, and adapt downward until the base64 transport payload fits 512 KiB. Returned dimensions, PNG SHA-256, requested dimension and byte-reduction flag describe the emitted preview. Pixel/hash measurements establish byte facts; they do not establish model perception or exact OCR.

The in-memory cache holds at most 256 jobs across projects. It can evict a finished cached entry while preserving its durable receipt, which a later status call can reopen. Live local workers and entries borrowed by in-flight calls are retained, preserving one control and cancellation identity for each job. This memory bound is separate from per-project evidence retirement.

## Qualification still required

Automated tests use an owned loopback server and deterministic image fixtures; they do not establish actual ComfyUI inference quality. The isolated native, MCP, Manager and configuration tests and one explicitly owner-started ComfyUI smoke passed within the [recorded pre-integration scope](validation/HOST-CAPABILITIES-1.3.22.md). Release qualification still requires the final integrated source and installed Manager/MCP/LM Studio paths with actual checkpoint, resource conditions and saved job evidence. Exercise real generation, unmasked editing, exact outside-mask preservation, ambiguous acknowledgement/reconnect, local cancellation and explicit reattachment. Verify native LM Studio calls and artifact delivery separately. Record actual failures and provider limitations instead of presenting provider inventory as successful generation.

The manual `ForgeConductor.NativeTools.ImageProviderSmoke` target is excluded from the default build and has no CTest registration. Build it explicitly, then run it only against an owner-started, explicitly selected provider. Supply a previously nonexistent absolute private home whose parent already exists:

```powershell
cmake --build out/p --config Release --target ForgeConductor.NativeTools.ImageProviderSmoke
& ./out/p/bin/Release/ForgeConductor.NativeTools.ImageProviderSmoke.exe `
    --home 'C:/MyPrivateEvidence/ACTUAL-NEW-UUID' `
    --endpoint 'http://127.0.0.1:8188' `
    --checkpoint 'cyberrealistic_final.safetensors' --profile sd1 --timeout-sec 180
```

The helper refuses an existing home and preserves its new outputs, receipts, HTTP metadata trace and JSON report. It makes at most five owned generation submissions, one remote job at a time, using 128- or 256-pixel dimensions and 2, 4 or 6 steps. Before the next submission it requires terminal history matching the preceding exact prompt ID and workflow. The timeout (1–600 seconds) bounds each individual job, admission, observation and history-drain phase; it is not a total helper deadline. If a job finishes before running cancellation is observed, that case is unverified and the helper returns a partial result. Its lost-acknowledgement case deliberately withholds an actually observed matching HTTP 200 response; it qualifies controlled response-loss recovery, not a real network outage. The helper uses direct native services with private fixture-issued scopes. It does not establish Manager IPC, LM Studio delivery, model quality or semantic instruction following.
