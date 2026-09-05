# Forge Conductor for Windows

Native Windows 11 port workspace. Governing instructions are `AGENTS.md` and `.forge-qwen/instructions/`. Immutable sources are under `.forge-inputs/`.

```powershell
.\scripts\Build.ps1 -Configuration Debug -Architecture x64
.\scripts\Test.ps1 -Configuration Debug -Architecture x64
.\scripts\Run-Static-Gates.ps1
```

The bootstrap executable proves the initial build path only. Replace it through the governed microtask plan; do not call it feature parity.
