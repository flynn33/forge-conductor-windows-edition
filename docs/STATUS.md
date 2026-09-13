# Product status

Updated September 13, 2026 for plan `windows-alpha-recovery-2026-09-12`. R0–R7 and PR #22 are merged; local/origin/GitHub main were synchronized at `35f61de3c5a8159e20752a3843e5f26bfa5290c9` before the current closeout. Draft [PR #23](https://github.com/flynn33/forge-conductor-windows-edition/pull/23) fixes the legacy invocation-guard defect exposed by live acceptance and supplies signed candidate 0.9.4.0. R6-G1 remains open for registered installation and authentic schema-9 compatibility.

| Area | Current source/evidence | Remaining requirement |
|---|---|---|
| Native backend | CLI, Manager, SessionHost, and WinUI GUI build in x64 Debug/Release. MCP ProtocolServer, serve snapshot, invocation guard, infrastructure, and 80-turn managed-run suites pass. | Registered-package acceptance remains separate |
| Native GUI and telemetry | All 14 destinations, exact project restoration, measured CPU/RAM, explicit GPU capability, keyboard context controls, High Contrast, 150% text, and Manager detach/reattach passed on exact unpacked package bytes. | Repeat the essential workflow through a registered package after machine trust becomes available |
| Managed provider path | The Manager owns durable ordinary runs, Responses turns, authorized native tools, authoritative usage, canonical handoff, fresh successor roots, and pause/resume/cancel. Exact 0.9.4 run `34c078f8-6256-4b03-80e1-936e2e50f2f4` rolled over to `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, completed both effects, and returned `DONE`. | R6-G2 passed; no live-provider requirement remains |
| Projects, MCP, memory, and tools | Native registration, exact selection, memory, LM Studio MCP deployment, 53-tool catalog/invocation, agent/session controls, feed, runtimes, diagnostics, and two-profile isolation passed. | Verify the same workflow through the registered package |
| Settings and maintenance | Effective Manager/provider/shell/logging/session/context settings, paired exact context inputs, and transactional project/all-store resets are implemented and covered. | Exercise installed confirmed-reset workflow |
| Installer | `ForgeConductor-0.9.4.0-x64.msix` was built from commit `3fb7014`, development-signed, unpacked, and rehashed with all 324 payload files and provenance. MSIX SHA-256: `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`. | An authorized administrator must provision machine trust, then complete the dedicated-account lifecycle in the installed acceptance handoff |
| Data compatibility | Authentic central migrations C001–C007 are present. A disposable schema-9 probe exits with code 20 and leaves the database unchanged. | Recover authentic C008/C009 history or retain explicit newer-store rejection; never mutate the owner store |

The earlier real rollover run `786d0672-6231-4202-a46c-401732ade283` produced useful successor effects, then Forge's legacy desktop-chat invocation guard emitted `context_budget_exceeded` after repeated native calls. That was a product-runtime defect rather than model text or a Codex context limit. `managed-run-v1` now bypasses only that legacy continuity policy while keeping normal MCP routing, authorization, invocation, and audit. Ordinary desktop MCP chats retain the guard. The 0.9.3 `PACKAGE_OK` run remains separate package-execution evidence.

The current ignored candidate is `out/dist/candidate-0.9.4.0-20260913-115325/`, built from commit `3fb70143ef2db9704c8a395b49a29857bd087976` and tree `fad39f10bdb2638a74a0b28deb0c0ecc1e88dede`. MSIX SHA-256 is `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`; ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`. Windows still returns `0x800B0109` without machine-level publisher trust, so no registered install/update/uninstall result is claimed. The exact authorized procedure is in [Installed acceptance handoff](INSTALLED-ACCEPTANCE-HANDOFF.md).

The four retired remote branch heads are preserved together in ignored verified bundle `out/alpha-evidence/branch-retirement/redundant-alpha-branches-20260913.bundle`, SHA-256 `fc1085d1979739eb1cf313ddf5556d5f5f1e05143c2f7c521c37998895b76ea9`. Their integration/equality was verified separately from working-tree cleanliness, and unrelated `.forge-qwen` state remains untouched.

GitHub authentication is verified as owner `flynn33`. R6 issue #9 and milestone 7 remain open only for the registered lifecycle and authentic schema-history dependencies.

<!-- alpha-phase-review:start -->
Phase review: R6 acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
