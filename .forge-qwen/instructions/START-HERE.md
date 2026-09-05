# Forge Conductor Windows 11 — Qwen guided v2 start

This revision prevents stale continuity or persistent memory from redirecting Qwen into a macOS source tree, prior remediation repository, package `work/` directory, or unrelated Git checkout.

## Windows bootstrap

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\START-HERE.ps1
```

The bootstrap prints an exact `LOCKED WINDOWS TARGET` and writes an absolute, run-specific first message at:

```text
<target>\.forge-qwen\FIRST_MESSAGE.txt
```

Open that exact target repository in LM Studio and paste that generated file. Do not reuse an earlier generic prompt or continuity packet.

For API mode:

```powershell
$env:LM_API_TOKEN = "<token>"
.\START-HERE.ps1 -AutonomousApi
```

## Expected first behavior

Qwen must first prove `WORKSPACE_LOCK.json`. Its first verified working line is:

```text
WORKSPACE_LOCK_VERIFIED <run_id> <workspace_fingerprint>
```

When the lock cannot be proven, it must emit `WRONG_WORKSPACE_BLOCKED` and stop without inspecting Git changes.

## Current-chat recovery

Paste `RECOVER-QWEN-NOW.txt` into any chat that says it adopted a workspace from continuity, found remediation work in a `work/` directory, or began inspecting an unknown repository.

Read `READ-THIS-FIRST-QWEN.txt` for the nonnegotiable start sequence.
