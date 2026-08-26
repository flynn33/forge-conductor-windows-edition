# P06-005: Job Object Process Cancellation and Shutdown

Status: Accepted

Date: 2026-08-25

## Context

Process execution is a high-risk native boundary. It must avoid shell ambiguity, inherited-handle leaks, pipe deadlocks, orphan descendants, unbounded output, and cancellation races while retaining macOS parallel-run parity.

## Decision

- `IProcessSupervisor` admits at most 64 concurrent operations and does not queue overflow.
- Requests require shell-enabled authority with an Execute grant. The executable is an absolute canonical path under an explicit trusted root; the working directory, when present, is also contained by a trusted root.
- Executable and working-directory authorization is established before launch by opening the local-drive root and then every path component relative to its retained parent. Every directory anchor excludes write and delete sharing, opens reparse-point-aware, rejects a case-sensitive directory policy, and remains owned through `CreateProcessW`. The executable leaf also excludes write and delete sharing.
- The complete anchor chain and final leaf are revalidated immediately before `CreateProcessW`. The API still receives absolute path strings, but no component in either string can be renamed, substituted, rewritten as a reparse point, or resolved under case-confused semantics while the native process creation call reopens it.
- The returned process and primary-thread handles are adopted into typed RAII owners immediately after `CreateProcessW` returns and before any observer callback. Both anchor chains are then released regardless of launch success. Authority pinning protects native path resolution without extending directory rename/delete denial across the child lifetime.
- Executable leaves must be ordinary single-link files and must not be delete-pending. A hard link inside a trusted root cannot be used as an alias for executable content whose other name is outside that authority.
- Launch passes a non-null absolute `lpApplicationName` and a mutable exactly quoted UTF-16 command line. No shell or ANSI process API is used.
- The final command line obeys the native 32,767-code-unit limit including its terminator. The sanitized final Unicode environment block obeys Forge's separate 32,767-code-unit bound including terminators.
- Environment names are case-insensitively unique; malformed UTF-8, NUL, `=`, and unsafe inherited entries are rejected.
- `STARTUPINFOEXW` uses `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; only intended redirected handles are inheritable.
- Launch uses `CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT`, assigns the child immediately to a Job Object, and only then calls `ResumeThread`.
- The Job uses `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; breakaway flags are prohibited.
- Stdout and stderr use overlapped pipes and shared bounded completion readers. Retention stops at 80,000/20,000 bytes, but draining continues until direct-child exit so producers never block.
- Timeout, targeted cancellation, cancel-all, and shutdown terminate the whole Job with `TerminateJobObject`, cancel pending I/O with `CancelIoEx`, and confirm tree death. The supervisor never waits indefinitely for inherited-pipe EOF after the direct child exits.
- Confirmed timeout/cancellation returns a successful `ProcessResult` with the matching flag and `terminationConfirmed=true`. Failure to prove tree death returns `process_termination_unconfirmed`.
- A later P13 executable-capability contract may authorize approved external tools. P06 does not widen authority for Program Files or any ambient executable search.

## Consequences

Execution has deterministic tree ownership and bounded retention while preserving 12-parallel and 48-rapid source behavior. Saturation is observable at 64/65. A deterministic launch observer test proves the ancestry leases deny rename immediately before `CreateProcessW` and release immediately after it returns while the child is still suspended; no path-authority handle is retained for the child lifetime. Observer injection is available only through a private Detail test-access factory; the production constructor surface exposes no test observer.

## Rejected alternatives

- 8/10/12 admission derived from manager threads: rejected because it lacks a governing budget and breaks source parity.
- Per-stream detached reader threads: rejected because 64 operations could create an unbounded thread footprint.
- Direct-child-only termination: rejected because descendants can survive and retain handles.
- `cmd.exe`, PowerShell, or implicit executable search: rejected because quoting and executable authority become ambiguous.
- Breakaway Job flags: rejected because they defeat deterministic ownership.

## Evidence

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- Microsoft `CreateProcessW`, Job Objects, and `CancelIoEx` API contracts
- `tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp`
