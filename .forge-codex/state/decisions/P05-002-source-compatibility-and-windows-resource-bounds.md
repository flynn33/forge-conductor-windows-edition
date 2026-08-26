# P05-002: Current-Source Compatibility and Windows Resource Bounds

Status: Accepted

Date: 2026-08-25

## Context

The current macOS source is the behavioral parity baseline, but platform-neutral behavior must be reconciled with direct Windows requirements. The current source uses an eight-repository cache, roughly 30 Hz telemetry, multi-megabyte process captures, optional Content-Length MCP framing with a 4 MiB default, eight continuity states, direct UUID/time generation, and singleton registries. The Windows package defines stricter profiles, framing, ownership, and continuity evidence.

## Decision

- Preserve current-source field semantics, stable error codes, deterministic ordering, tombstones, optimistic versions, idempotency, and wire compatibility where they do not conflict with a direct Windows requirement.
- Use the Windows resource profiles exactly. Constrained, standard, and expanded allow 4, 8, and 16 open project repositories. Automatic selection is constrained through 8 GiB, standard below 32 GiB, and expanded at 32 GiB or more.
- Across profiles, telemetry pending capacity is one, MCP input is 1,048,576 bytes, named-pipe frames are 2,097,152 bytes, tool stdout/stderr are 80,000/20,000 bytes, and shell execution is at most 120 seconds. Sampling is governed by the 1–2 Hz Windows profiles rather than the macOS 30 Hz target.
- MCP framing is newline-delimited JSON only. Content-Length framing and the macOS 4 MiB default are not part of the Windows product contract.
- Project-memory limits preserve the current source: 512-byte title, 4 KiB summary, 256 KiB body, 2 KiB source reference, 32 tags of 128 bytes, 50-record/1 MiB batch, 4 KiB query, 20/100 default/max page, 64/256 KiB default/max response, 32 MiB import/export artifact, and 1–60,000 ms request deadline compatibility.
- The macOS `project_memory.search` and `project_memory.list_recent` schemas leave the `kinds` arrays without an aggregate count bound. At the Windows request boundary, both collections are capped at the existing `ProjectMemoryLimits::maximumPageCount` value of 100; each element follows the stored-memory kind normalization and 1-64-byte ASCII identifier rule. This deliberate compatibility restriction satisfies the governing prohibition on unbounded collections without introducing a second hidden count limit. Cap-plus-one requests fail with `payload_too_large`, and later MCP schema adapters must advertise the same bound as `maxItems`.
- Windows process requests admit at most 256 arguments, 4 KiB per argument, and 15 KiB of aggregate raw UTF-8 argument payload. Explicit environment overlays admit at most 128 entries, 128-byte names, 4 KiB values, and 24 KiB of aggregate raw UTF-8 name/value payload. Count overflows fail with `limit_exceeded`; byte overflows fail with `payload_too_large`; malformed names and embedded NUL remain `invalid_request`. The 15 KiB cap leaves conservative headroom under CreateProcessW's 32,767 UTF-16-unit command-line ceiling; the 24 KiB overlay is a Forge semantic workload cap, not a claimed Unicode environment-block ceiling. The P06 adapter must still reject an exact final quoted UTF-16 command line that crosses the native command-line ceiling and a sanitized inherited environment block that crosses the separately documented Forge cap.
- `AppConfig::allowedRoots` counts request entries before normalization and is capped at 32, with cap-plus-one returning `limit_exceeded`. `PathText::MaximumBytes` continues to bound each root.
- Diagnostic envelopes expose their existing 256-byte event, 64-field, 128-byte field-name, and 4 KiB field-value limits as public constants and add a 64-byte role limit. String byte overflows return `payload_too_large`, field-count overflow returns `limit_exceeded`, and required empty strings remain `invalid_request`.
- The canonical Windows continuity state machine is the thirteen-state recovery/cancellation model from the package. The eight macOS wire states import through an explicit lossless mapping; adapters may emit legacy aliases only at compatibility boundaries.
- Canonical handoffs use schema 1.0, a 128 KiB encoded cap, 128 items per bounded list, typed sections, explicit redaction state, and a SHA-256 integrity field. Legacy handoffs preserve schema 1, 4,000 Unicode scalars, and 128 agent snapshots.
- UUIDs, clocks, hashes, and redaction are injected through contracts. Domain constructors do not read time, randomness, environment, filesystem, registry, process, or network state.
- Preserve value models from singleton-backed macOS registries and diagnostics, but replace singleton ownership with constructor-injected, composition-owned services.
- `project_memory.export` and `agent_run_status` receive write authority and mutation auditing because the current implementations mutate durable/access state even where older inventories classified them as read-only.

## Consequences

The Windows port retains current observable semantics while using the stricter platform contract whenever sources disagree. Compatibility translations remain visible at outer adapters. Domain tests can prove exact limits and state transitions without platform access, while runtime/resource compliance remains assigned to later gates.

## Rejected alternatives

- Copying macOS runtime frequencies and capture sizes: rejected because direct Windows budgets are authoritative.
- Retaining eight continuity states internally: rejected because retry, recovery, and cancellation cannot be represented faithfully.
- Keeping global registries for source parity: rejected because ownership semantics are architectural, not user-visible parity.
- Classifying tools only from historical inventories: rejected because current implementation side effects control authorization and auditing.
- Leaving kind-filter collections uncapped: rejected because a transport byte limit does not replace a semantic collection bound. Adding a separate unreported count was also rejected; the existing advertised 100-item page maximum is reused.
- Deferring process, environment, configuration-root, or diagnostic-role bounds to transport and Win32 adapters: rejected because typed Domain requests must be bounded before quoting, encoding, persistence, or logging work begins.

## Evidence

- .forge-codex/instructions/plans/resource-budgets.json - SHA-256 f80c5d57081d47b87ddb77027f843c912bcf3c3e558c7ade42b4db4828760965
- .forge-codex/instructions/architecture/RESOURCE_BUDGETS.md - SHA-256 bca7eeff7df38854853399b3669334b0ff3d3c586aa0c5f977baadd6acef6daa
- .forge-codex/instructions/architecture/CONTINUITY_AND_SESSION_HOST.md - SHA-256 c6819e807940e108eaf46c1c81d0f092fdff8e0b66d79ed570eedfdbe8de856a
- .forge-codex/state/baseline/p02-mcp-semantic-inventory.json
- .forge-codex/state/baseline/p02-feature-inventory.json
- .forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/ProjectMemoryModels.swift
- .forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/ContinuityModels.swift
- .forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ResourcePolicy.swift
- .forge-codex/state/decisions/P03-008-boundary-lifetimes-errors-and-bounds.md

