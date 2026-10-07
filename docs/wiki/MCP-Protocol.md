<p align="center">
  <img src="images/product/02-lm-studio-mcp.png" alt="LM Studio MCP command center" width="100%">
</p>

# MCP Protocol

Forge Conductor speaks newline-delimited JSON-RPC 2.0 over stdio for LM Studio. The source catalog contains **104 tool descriptors** and is filtered by deployment role. All original 80 remain, with 24 additions including independent `image_analyze`; see [Tool Catalog](Tool-Catalog) and [Windows Workflow Capabilities](Windows-workflow-capabilities).

## Roles

| Role | Server identity | Catalog |
| --- | --- | --- |
| Primary | `forge-conductor` | General native catalog; CLU-only controls excluded |
| Fallback | `forge-conductor-fallback` | Independent general catalog and health |
| CLU | `forge-conductor-clu` | Governance-only `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve`, and `project_policy.read` |

All three registrations share an exact deployment revision while retaining distinct role, health, process, and presence evidence. Version 1.3.16 retains the exact integer `timeout: 180000` contract for every role.

## Project context contract

The `initialize` response provides plain-text instructions that identify the authorized workspace, ordered instruction-package folders, and development-policy source and revision. The same facts are available independently through machine-readable `get_forge_status` fields:

- `workspace.project_root` is the authorized project root, not the Forge data directory.
- `instruction_packages.packages` is ordered and explicitly marked `read_in_order`.
- `development_policy` supplies the source path, revision, and read-and-follow requirement.
- `home_kind: application_data` and `home_is_project: false` distinguish Forge state from the project.

Install or repair carries the selected registered project ID/root into all three roles. This disclosure comes from that binding and remains available when a recovered continuity packet has an empty `paths` object. A different registered project's package/policy records are not substituted for the selected project's configuration.

In host filesystem mode, continuity adoption authorizes both the registered project alias and recovered candidate for Read, then checks their canonical subtree relationship. A broad authorized volume root does not replace registered project identity; unregistered/sibling paths or revoked access remain denied. The focused client workspace-context regression passed, while current installed/native-chat acceptance requires its own executed evidence.

## Transport rules

- one compact UTF-8 JSON object followed by `\n`
- no LSP `Content-Length` framing
- stdout contains responses only
- every response is flushed
- input and parsed-object bounds are enforced
- EOF, cancellation, broken pipe, and parent exit tear down deterministically
- tool failures return stable result envelopes rather than crashing the server

Supported methods include `initialize`, `ping`, `tools/list`, `tools/call`, `notifications/cancelled`, empty `resources/list`, and empty `prompts/list`.

Managed worker/reviewer provider Responses function outputs retain text-string compatibility. A native PNG preview adds `input_text` metadata and `input_image` content within its original `function_call_output`, preserving `call_id`; encoded previews are bounded to 512 KiB. This additive provider-content path is separate from MCP framing. Fresh loopback-provider image transport cases passed; they do not establish that the configured model supports vision, and current installed Qwen acceptance remains pending.

## Bounded native result delivery

`project_policy.read` inventories expose `next_cursor`; the cursor binds project, revision, and the complete inspected index hash, including guidance. Changed content, even under the same revision, invalidates an old cursor. Document and instruction reads use UTF-8-safe `next_offset` continuation. `fs_read` preserves line/byte pagination and accurate EOF metadata. Final serialized read payloads are bounded to 32 KiB; follow the returned continuation until complete.

Bounded read tools return `clu_governance_notifications_deferred: true` and `clu_governance_notifications_read_tool: "clu.findings"`. They do not acknowledge pending findings while deferring them. Read `clu.findings` for that governance evidence.

Other canonical tool payloads above 32 KiB return multiple `content[]` text blocks. Each block is a valid JSON object with exactly `kind`, `version`, `index`, `count`, `total_bytes`, `part`, and `instruction`. The reserved kind is `forge_tool_result_fragment`, version is `1`, and indexes start at zero. Concatenate all `part` strings in index order, then parse the complete JSON once. Each UTF-8 part is at most 12 KiB; each serialized fragment is at most 32 KiB. Never repeat the operation to obtain the remaining parts.

`structuredContent` remains the complete original payload, and `isError` retains the original outcome. The complete serialized JSON-RPC response remains bounded to 1 MiB. The native conversation reader accepts only a complete, consistent, ordered sequence for one tool call; mixed, duplicate, missing, reordered, invalid, or noncanonical fragment sequences do not establish semantic success. Smaller tool results keep their existing single-text-block contract.

This repairs the measured LM Studio per-text-block truncation boundary. It does not change the catalog, add a plugin, or replay side effects.

## Routing

```mermaid
flowchart LR
    Frame[stdio frame] --> Codec[JSON codec]
    Codec --> Guard[Invocation guard]
    Guard --> Role[Role catalog filter]
    Role --> Router[Tool router]
    Router --> Authority[Project/effect authority]
    Authority --> Pack[Tool pack]
    Pack --> Result[Structured result + audit]
```

The router enforces exact catalog membership, one handler per tool, project match, effect authorization, shell policy, deadlines, and bounded arguments. Continuity observations are taken after authorization so wire arguments cannot invent workspace roots.

LM Studio owns the outer MCP request deadline. Forge's `shell_exec` request timeout remains capped at 120 seconds, so the 180-second client deadline allows Forge to return its own bounded result.

## Fallback behavior

Fallback is an independent general-catalog integration selected in the LM Studio chat. `FallbackPromoted` is a derived health/status state, not a request router. A Primary timeout does not automatically replay the call through Fallback, and selecting Fallback does not create an automatic switch back to Primary. This avoids duplicating a mutating call whose completion state may be ambiguous after the client stops waiting.

## Loop protection

Repeated identical calls receive a soft warning and eventually a hard block. Context reserve pressure schedules native Auto Continuity without blocking Forge tools or agents. Pending calls, tracked clients, and continuity clients all have hard capacities.

## LM Studio deployment

Deployment is transactional:

1. discover and validate the Windows LM Studio environment
2. smoke the packaged CLI in each role
3. parse existing MCP configuration
4. preserve foreign entries and unknown fields
5. stage all Forge registrations with the selected project binding, one revision, and exact integer `timeout: 180000`
6. validate the same timeout in every synchronized plugin bridge
7. atomically replace with backup/rollback
8. activate through supported host controls and require matching synchronized state
9. report registration, live presence, and audited outcomes separately

Auto Continuity uses product Windows UI Automation to create and send a native successor chat. It verifies the persisted message, native conversation identity, enabled integrations, packet retrieval, and a following tool call; it does not treat a local connection ID as a native chat.

Historical native rollover was verified with reserve pressure while the primary MCP worker stayed alive. The 1.3.11 checks verify project-scoped pickup and native observing state, without adding a new rollover or physical-exhaustion qualification. Interrupted UI handoff after worker eviction remains process-local. See [Release 1.3.11](Release-1.3.11) and [Continuity](Continuity).

A live 1.3.2 validation call requested a 90-second `shell_exec` limit, slept for 70 seconds, and returned after 70,312 milliseconds of command time and 70,545 milliseconds of LM Studio tool status time. This host observation proves the repaired outer deadline exceeded the former 60-second cutoff; it is not a latency guarantee.

**Next:** [Tool Catalog](Tool-Catalog) · [Native Tools](Native-Tools) · [Guided Setup](Guided-Setup#connect-lm-studio)
