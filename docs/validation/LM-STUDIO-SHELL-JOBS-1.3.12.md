# LM Studio workflow repair investigation, 1.3.12

On October 5, 2026, the running Forge Conductor 1.3.11.0 installation was investigated using the selected native LM Studio conversation and Primary connector traces. The session snapshot is retained locally under `session-investigation-20261005` in the host's Forge Conductor investigation workspace. Native conversation identifiers and task-specific command paths are omitted from this public record.

## Observed failures

- Instruction-package and policy retrieval succeeded before validation execution; this investigation did not reproduce the earlier bootstrap or large-result truncation defects repaired by 1.3.11.
- A full validation wrapper's connector result contains `timed_out=true`, exit code 1460 and elapsed time 120,015 ms. The expected run receipt was missing afterward. Separate unit-test probes returned at 118,011 ms, 112,023 ms and 118,027 ms, and an initial probe at 118,017 ms. These are terminated runs, not successful measurements of the suite's full duration.
- The model tried hidden background process and scheduled-task workarounds after the bounded foreground failures. The scheduled-task command observed null `USERNAME`; the child environment's baseline allowed only Windows system/temp variables plus explicit toolchain entries.
- A Python wrapper attempted UTF-8 decoding and failed on byte 0x97 after its inner command reported exit 0. The decoding failure prevented that wrapper from treating the command's evidence as complete.
- `fs_write` rejected an external target outside the registered workspace. That was the existing authorization boundary, not a general inability to write files.

The baseline mechanism is in `WindowsShellService::execute`, which rejects timeouts above 120 seconds, and `WindowsProcessSupervisor`, whose Windows job owns the complete process tree and terminates it on deadline/shutdown. The MCP catalog exposed `shell_exec` without a tracked long-running process route.

## Repair boundary

The expanded repair exposes 80 general tools, including the four shell-job convenience calls and eighteen additional process, inspection, GitHub, authority, evidence, verification, and reviewer operations. Manager-backed execution retains owned work across MCP reconnects and exposes actual job/PID/receipt identity, live stdout/stderr paging, final results, explicit cancellation, and verified adoption. Broker interruptions remain explicit rather than becoming an invented exit status.

`WindowsProjectWorkspaceAuthority::bindConfiguredRoot` checks the existing owner allowlist, exact canonical directory, original full capability, and combined root policy before publishing a process-local project binding. It does not turn the original external write refusal into blanket access. `WindowsEvidenceService` uses anchored binary file handles, streamed SHA-256, atomic project-log replacement, and a cross-process capture mutex. It refuses altered or full logs; the unkeyed chain requires an independently retained head to detect complete replacement and rehashing.

The pinned venv route writes dependencies below a separately authorized root and records observed versions only after successful creation. The reviewer route starts a separate provider context and constrains its tools to reads. Its dedicated receipt store preserves the full opening/task and up to 256 KiB of report output, verifies SHA-256 envelopes, refuses new admission at sixteen persisted receipts, and preserves interrupted pending calls as unknown outcomes without replay. Caller-provided authorization text is not itself human-grant evidence. Host/provider/GitHub inspection returns observed facts, provenance, and explicit failures or unknowns. Explicit profile/account values and Python UTF-8 defaults replace the incomplete child baseline without inheriting arbitrary process secrets.

The caller still owns the complete command and evidence contract. The repair preserves the original full validation/report requirements. Installing/reconnecting the connector does not itself complete the user's validation task, obtain a reviewer verdict, or prove an external service's permission.

## Verification record

The final native Release build completed successfully. The full configured CTest run passed **156/156** with zero failures in 44.48 seconds. No-Python, native-stack, and no-attribution static gates passed; the package persistence contract passed with the production profile excluded from package virtualization. The matrix includes the real Manager composition lifecycle, tracked process work, reviewer receipt persistence, evidence capture, and workspace authority checks.

A disposable catalog capture read the complete catalog and verified 80 general tools and five CLU tools. The general descriptor SHA-256 is `cc0cabadf2b9da2ec14036d91c89015492511959164aba6c36a7d2c272f0324b`; CLU remains `08ee5cd873945558906ee878ae5ef1c30b94b56f511ae1dd4eb75e9c0a2d7ff4`. The captured CLI SHA-256 is `6f166bf2b91b413636ae8997ccf4a0c3ed8e784f16835d924a2fd6c3c829ac8f`. The final native-build CLI SHA-256 is `6f166bf2b91b413636ae8997ccf4a0c3ed8e784f16835d924a2fd6c3c829ac8f`. Descriptor capture and final linking retain their separate artifact provenance.

Earlier 1.3.12 source candidates supplied additional live regression evidence: a 46-request shell check completed its 125-second command in 125.543 seconds and exercised profile/UTF-8 defaults, nonzero exit, timeout, cancellation, and discovery. A later 39-request process check exercised exact argv/environment, live and paged logs, cancellation, completed receipt recovery, honest unknown-exit recovery after intentional connector loss, real memory attachment, authorized external text exceeding 64 KiB, binary hashes, and evidence-chain recovery. Its dedicated venv observed Python 3.12.10, `jsonschema==4.25.1`, and `PyYAML==6.0.3`. Four separate diagnostic requests observed provider/native-process facts and a read-only GitHub artifact response with HTTP 200. These earlier probes used CLI SHA-256 `01d21049de51c19a2e83c052588105502d747b8204433be9604b6707e9d4b382` for process/diagnostics and `05d832577eb4529fbaa7691f8a2206438e12ee252e964b371ccbe0b962b8dd36` for shell work; they are separate from qualification of the final installed payload.

The signed development MSIX **1.3.12.1** was installed from source commit `91322cde3310d9ace59bde73c437a07fb300ee81`. App, Manager, CLI, and SessionHost installed hashes match verified staging; the original selected conversation was preserved at installation readback. Native repair verified all three installed registrations, and activation observed LM Studio acknowledge their exact shared revision while preserving foreign configuration/plugins. The activation result reported zero active hosted roles: this production path deliberately leaves roles lazy until selected by a chat. Configuration synchronization does not claim three active native chat connections.

The isolated payload lifecycle check verified the 1.3.12.0 to 1.3.12.1 revision upgrade, both signed candidates and all payload hashes, retained settings/workspace/legacy and project memory, project isolation, and simulated uninstall/reinstall without changing foreign MCP configuration. The actual package-version guards passed eighteen acceptance/rejection cases each, including range limits and trailing LF/CRLF metadata.

Installed executable SHA-256 readback:

| Executable | SHA-256 |
|---|---|
| `ForgeConductor.Manager.exe` | `7ba809f3f678c4347247816d7bb1fbb48d7d1b49b5dd38ad6eedc2c87de9e3a0` |
| `ForgeConductor.SessionHost.exe` | `f07d4889f8d16f25c216b9f91c71808921b5759e14f8b00df20e050e761b7adb` |
| `ForgeConductorApp.exe` | `272af1776c5fb93447cce93e3f523e5f4067c2b2f626b96dfbf2b7ce462cee1e` |
| `forge-conductor.exe` | `6f166bf2b91b413636ae8997ccf4a0c3ed8e784f16835d924a2fd6c3c829ac8f` |

The installed Manager-owned job completed after 125.055 seconds with exit 0. Its actual child PID/creation identity survived MCP disconnect/reconnect and verified adoption. Named log/receipt hashes and a real persisted project-memory record were read back. The same route executed read-only `git ls-remote` successfully.

Git qualification used the installed single-link `C:/Program Files/Git/bin/git.exe`. An earlier probe selected the hard-linked `cmd/git.exe` alias and was correctly rejected by the retained executable authority policy; its failed evidence is preserved separately. The native Git discovery route already selects the supported single-link entry point.

The installed reviewer accepted a 2,247-byte opening, started a fresh read-only provider context without executor history, returned a unique local fixture marker absent from its opening, and retained the same completed provider-response identity through three MCP connections. Its report was not truncated. This is a tooling regression review; it does not approve the user's project gate.

Installed provider/host inspection observed `qwen/qwen3.8-27b` loaded and LM Studio desktop ProductVersion `0.4.25.0`. Exact model file/revision and inference-engine/server-API versions remain unknown where the provider does not report them. Context telemetry retains its latest-generation sampling boundary or explicit unavailable state.

The user's original full validation task, required reports, and independent project-review verdict were not executed or credited by these disposable tooling checks. Earlier 1.3.11 qualification retains its own artifact provenance.
