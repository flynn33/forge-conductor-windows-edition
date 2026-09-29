# Forge Conductor 1.3.3 validation record

This record covers the LM Studio workspace-context disclosure correction.

## Contract

- native runtime and stable package version 1.3.3 / `1.3.3.0`;
- Primary and Fallback registrations launch the same qualified binary from the authorized project folder;
- MCP initialization instructions name the project folder, ordered instruction-package folders, and policy source/revision;
- live `forge_status` returns those same facts as structured JSON, labels `home` as application data, and remains independent of empty handoff paths;
- every disclosed package and policy path exists at validation time.

## Automated verification

- complete x64 Release backend build;
- all 153 configured CTest entries passed;
- repository no-Python, native-stack, and no-attribution static gates passed;
- MCP protocol, process snapshot, tool-pack, contract-matrix, and LM Studio serve-verifier coverage.

## Live verification

LM Studio 0.4.25 loaded `qwen/qwen3.8-27b` and called `forge_status` once through the configured `mcp/forge-conductor` integration. Response `resp_46e940b772756c1b86e1e9aa006a4e86527f97ae341b7903` reported:

- `workspace.project_root`: `D:\GitHub\YWE`;
- ordered instruction package: `A:\Qwen-Projects\YWE\YWE-M2-Completion-Agent-Package-2026-09-23`;
- development-policy source: `A:/raven-forge-development-main`;
- development-policy revision: `e5145d9b23b1f98bf61c83d0dd5cf32ea3428b84fdfddbb7ad6983c40d11af75`;
- `home`: `C:\Users\james\AppData\Local\Forge Conductor 1.3.3 Live`, with `home_kind: application_data` and `home_is_project: false`;
- continuity handoff `1a12b8fd-16e1-4110-b73c-5af855f2d022` remained resumable while the independent workspace binding continued to identify YWE.

The Forge process used `D:\GitHub\Forge-Conductor-Windows-Edition\out\build\windows-msvc-x64\bin\Release\forge-conductor.exe serve` with process current directory `D:\GitHub\YWE\`. `Get-Item` resolved the project root and policy source as directories. The registered instruction-package path was restored as a directory junction to the existing `YWE_M2_VALIDATION_REPAIR_EXECUTION_PACKAGE_2026-09-27` content so the ordered path disclosed to the model is traversable without direct database mutation.

Source tests are supporting evidence only; the live model/tool exchange is the acceptance result.
