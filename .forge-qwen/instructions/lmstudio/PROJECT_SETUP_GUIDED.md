# Guided LM Studio project setup

1. Create a clean LM Studio project for this Windows-port run.
2. Do not import or auto-load a continuity packet from another Forge project.
3. Do not expose the Forge Conductor MCP under test as the project’s memory/continuity controller.
4. Add the seven external tool capabilities only.
5. Set project instructions from `PROJECT_INSTRUCTIONS.txt`.
6. Run `START-HERE.ps1` on Windows.
7. Open the exact target repository printed as `LOCKED WINDOWS TARGET`.
8. Paste the generated `.forge-qwen/FIRST_MESSAGE.txt`, not a generic previous prompt.
9. Each fresh chat uses the newly generated `.forge-qwen/NEXT_MESSAGE.txt`.
10. Never let LM Studio “adopt” a different workspace from continuity or memory.
