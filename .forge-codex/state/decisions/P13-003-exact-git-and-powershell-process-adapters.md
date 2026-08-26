# P13-003: Exact Git and PowerShell Process Adapters

Status: Accepted

Date: 2026-08-26

## Context

The macOS tool packs invoke `git` and `/bin/bash` through a bounded process
runner. Windows must use `CreateProcessW`, Job Objects, direct argument vectors,
bounded capture, cancellation, deadlines, and exact executable selection. The
qualified P06 `WindowsProcessSupervisor` already owns these mechanics and
rejects ambient executable discovery.

Git and PowerShell are installed outside project roots on the alpha machine.
Adding broad executable directories to a caller-visible workspace capability
would create unnecessary authority if that capability escaped the adapter.

## Decision

- Git and shell adapters reuse `IProcessSupervisor`; they do not call
  `CreateProcessW`, `system`, `_popen`, `cmd.exe`, `SearchPathW`, or a shell for
  Git composition.
- Composition injects exact absolute executable paths. The adapters store their
  execution authority privately, bind it to the same project/authority identity,
  and construct only the exact injected executable request. No API exposes a
  generic way to execute sibling binaries from its executable directory.
- Executable admission enforces one configured namespace-link identity. On the
  alpha machine the accepted Git link is
  `C:\Program Files\Git\bin\git.exe`; the hard-linked
  `C:\Program Files\Git\cmd\git.exe` alias is rejected even though Windows
  reports the same underlying file identity. Runtime requests cannot select an
  alternate hard-link name.
- Git uses direct arguments:
  `status --porcelain=v1 -b`, `diff [--cached]`,
  `log -n N --oneline`, `add -A` or `add -- <path>`, and
  `commit -m <message>`. `GIT_TERMINAL_PROMPT=0` prevents interactive hangs.
- Git runs for at most 30 seconds and uses the owner budgets of 80,000 stdout
  bytes and 20,000 stderr bytes. Log count, path count, and argument bytes are
  bounded before launch. Every successfully launched Git command returns its
  bounded `ProcessResult`, including numeric exit code, stdout, stderr, timeout,
  cancellation, truncation, termination, and elapsed-time fields. A nonzero
  exit is therefore observable by P14 instead of being collapsed into an error;
  typed `Result` failure is reserved for boundary, authority, admission, and
  process-supervisor failures.
- Shell uses only the exact injected Windows PowerShell or explicitly configured
  PowerShell 7 executable with `-NoLogo -NoProfile -NonInteractive -Command`.
  The default timeout is 30 seconds and values above 120 seconds are clamped.
  Disabled authority fails before the supervisor sees a request.
- Adapter shutdown closes admission and cancels its operations without claiming
  ownership of an injected supervisor shared by other services. Each admitted
  shell operation owns a local stop source, bridges the caller token into it,
  and passes the derived token to the supervisor. Cancellation therefore
  persists even when shutdown occurs before the supervisor has admitted the
  operation.
- Live Windows integration exercises the installed Git executable through
  repository initialization, status, add, cached diff, commit, and log, and the
  installed PowerShell executable through normal output, timeout, output
  truncation, and destructor-driven cancellation.

## Consequences

The current alpha machine paths are discovered and fixed at composition time;
runtime requests cannot use `PATH` to substitute a binary. Installer and
composition phases must report a missing approved executable as a typed missing
dependency and may use first-party discovery/install policy where permitted.

The private execution authority is a low-churn capability bridge for P13. A
future broker can replace it with an explicit `AuthorizedExecutable` public
capability without changing tool behavior. Dedicated hardening of that bridge is
deferred by OWNER-002; exact executable construction and non-leakage remain
functional invariants now.

## Evidence

- `include/ForgeConductor/Contracts/IProcessSupervisor.h`
- `src/Infrastructure/Windows/WindowsProcessSupervisor.cpp`
- `tests/NativeTools/WindowsGitShellTests.cpp`
- `tests/NativeTools/WindowsGitShellIntegrationTests.cpp`
- `.forge-codex/state/decisions/P06-005-job-object-process-cancellation-and-shutdown.md`
- macOS `GitToolPack.swift`, `ShellToolPack.swift`, and `ProcessRunnerTests.swift`
