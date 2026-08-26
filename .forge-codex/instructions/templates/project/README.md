# Forge Conductor for Windows

Native Windows 11 port workspace.

The governing implementation instructions are in `AGENTS.md` and `.forge-codex/instructions/`. The immutable macOS, Forsetti Framework Windows, Forsetti Agentic Edition, and audit inputs are under `.forge-inputs/`.

Canonical commands:

```powershell
.\scripts\Build.ps1 -Configuration Debug -Architecture x64
.\scripts\Test.ps1 -Configuration Debug -Architecture x64
.\scripts\Run-All-Gates.ps1
```

The bootstrap executable is temporary and must be replaced by the full Forsetti-compliant product through the governed phase plan.
