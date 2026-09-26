# Simplified workflow and CLU governance 1.3.0 validation

Date: 2026-09-26

## Scope

This record covers local native x64 Release verification of the four-destination workflow, universal ordered instruction packages, CLU governance, independent automatic continuity, persistence migration, and 1.3.0 identity. It is not a package-publication or live-provider qualification record.

## Focused verification

- Manager protocol codec: passed.
- Manager request dispatcher: passed, 18 test groups, including package edge records, revision-bound paging, retry/cursor durability, legacy queue migration, Activity correlation, Doctor checks, and project/provider continuity readback.
- Managed-run service: passed.
- Project policy service: passed, including durable legacy migration, a real finding correction, a visibly redacted export receipt, and null-finding Activity projection.
- MCP catalog codec: passed, 500 assertions.
- MCP tool-pack adapter: passed, 571 assertions.
- MCP tool contract matrix: passed.
- MCP serve-process snapshot: passed, 2,047 assertions.
- WinUI Release x64 build: passed with zero warnings and zero errors.

## Release verification

- Complete CMake Release x64 backend build: passed.
- WinUI Release x64 application and sibling-service staging: passed.
- Configured CTest matrix: 153 of 153 passed after repairing a null-finding Activity projection found by the first 152/153 run.
- Static native-stack, no-Python, and no-attribution gates: passed.
- Disposable native launch: passed; responsive `Forge Conductor` window, nonzero main-window handle, disposable Manager started, and no `0xc0000005`/WER event.

## Package and provider boundaries

- The previous development-signed MSIX and ZIP were built from `d514719e42baa1ca647fd26fbb51666a4670cb3d`; their hashes do not validate the current fixes and are intentionally not presented as current artifacts.
- No current package was created, published, tagged, released, or installed by this work.
- No pull request exists or is implied by this record.
- A real supported-provider successor create/restore/acknowledge/fence run was not performed. Continuity remains implemented-but-unqualified and must not be reported `Active`.

## Boundaries

Automated checks use disposable roots and do not mutate the operator's production profile. A development signature validates package construction and payload integrity; it is not a production publisher signature. CLU coverage reports opaque content and read gaps honestly and does not claim semantic proof that is absent from recorded evidence.
