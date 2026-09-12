# Accessible settings and memory maintenance

## Native configuration is required
Settings must be obvious from the native navigation and accessible without a developer shell. Provider and Manager may remain separate linked pages, but the operator must not have to infer which hidden JSON file to edit. Do not simply make `IsSettingsVisible` true and count that as implementation: every required setting must have real behavior and effective readback.

Reuse the existing typed ManagerSettings/patch path, validation and Windows secure storage. Organize configuration into Provider/Model, Context & Continuity, Runtime/Shell, Telemetry/Logging, Manager/Startup and Memory & Data. Keep business validation in the service/domain boundary, not solely in the visual form.

## Required controls and behavior
Provider endpoint, model selection/discovery, credential presence/update/remove and connection test must be usable. Do not expose credential values in telemetry, logs, Git history or screenshots. Preserve existing configured values until an explicit successful save. Show automatic model selection distinctly from an empty required model and keep the fixed JSON-null regression covered.

Context controls include effective capacity, response/handoff/safety reserves and the resulting threshold/headroom explanation. Pair numeric sliders with exact-value input/readout where useful. Keyboard users must be able to make exact changes; labels include units and the chosen/effective value. Do not offer tool-call allowances, count rollover or timed-session rollover. Genuine request timeout and process cancellation settings are different and remain supported.

Expose shell preference, actual available logging/telemetry refresh/history preferences and Manager lifecycle/startup preferences supported by the implementation. Shell defaults on for a new profile but honors deliberate opt-out. Unsupported optional runtimes have a useful explanation and supported configuration path; do not silently install a language/runtime or run a login shell to make the page green.

Every editable group shows unsaved changes, validation errors, save/revert state and success/failure. Read back the Manager's effective persisted result rather than blindly copying the submitted form into “saved” state. Make delayed apply/restart requirements explicit. Do not accidentally restart live work on every slider movement or race a configuration change into an in-flight provider request. Respect a safe application boundary and preserve the previous valid configuration on a failed save.

## Reset is a coordinated lifecycle, not file deletion
Provide selected-project memory reset, selected-project continuity reset, combined reset and separately confirmed all-store maintenance. Show project identity and affected stores prominently before confirmation. Give cancel a no-change guarantee. Do not reset the owner's production profile as a test.

Use existing project maintenance/lease/generation mechanisms. A reset must coordinate active writers and sessions, close/evict relevant store handles, apply the scoped change and prevent late prior-generation work from recreating the reset data. Never delete a live SQLite file while processes retain handles. Preserve another project's memory, continuity, source folders and foreign MCP configuration. An all-store operation needs an entirely disposable profile for automated checks and a visibly broader confirmation in the product.

Retain existing backup/receipt facilities where useful. Add only the missing narrow coordination; no new backup service or security-hardening platform is required. Show the result and restored service state. If a reset fails, return a clear failure and consistent state; do not display successful “flushed” status before durable completion.

## Minimum proof
One settings test group should cover a valid save/readback through restart, invalid values and repeated-save behavior. Reuse the existing ACL/atomic-save fix rather than rewriting it. One two-project disposable reset scenario covers A changed/B preserved, cancelled confirmation and a stale write rejection. All-store behavior is exercised only within a fully disposable isolated root.

Finish with a keyboard/focus/scaling walk through Settings and a context slider/exact-value pair. Custom visuals expose name/value text. Do not claim screen-reader usability from a static screenshot alone, and do not create a large UI testing framework just for this pass.

<!-- alpha-phase-review:start -->
Phase review: R0 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
