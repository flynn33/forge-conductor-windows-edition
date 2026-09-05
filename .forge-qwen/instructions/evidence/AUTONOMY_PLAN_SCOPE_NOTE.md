# Scope note for the attached autonomy plan

The attached `Plan-Forge-Conductor-Autonomy.txt` is architectural evidence, not the active Windows-port execution plan.

It supports an important diagnosis: project storage is already project-aware, while orchestration and legacy continuity can still select global/latest state and therefore assume the wrong project. The guided Qwen package applies that lesson to its own execution control by requiring a durable workspace lock, run-specific memory namespace, exact handoff binding, and no global latest fallback.

Do not copy platform-specific content from the plan into the Windows product without applying the governing Windows requirements. The plan discusses Swift, macOS, XPC, Bash, Python, and macOS PowerShell behavior. The active Windows mission instead requires native object-oriented C++20, Windows-native APIs, and no Python in the target project or build.
