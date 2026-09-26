# Simplified workflow and CLU governance 1.3.0 validation

Date: 2026-09-26

## Scope

This record covers the native x64 Release implementation of the four-destination workflow, universal ordered instruction packages, CLU governance, independent automatic continuity, persistence migration, and package identity.

## Focused verification

- Manager protocol codec: passed, 727 assertions.
- Manager request dispatcher: passed, 15 test groups.
- Managed-run service: passed.
- MCP catalog codec: passed, 500 assertions.
- MCP tool-pack adapter: passed, 571 assertions.
- MCP tool contract matrix: passed.
- MCP serve-process snapshot: passed, 2,047 assertions.
- WinUI Release x64 build: passed with zero warnings and zero errors.

## Release verification

- Complete CMake Release x64 backend build: passed.
- WinUI Release x64 application and sibling-service staging: passed.
- Configured CTest matrix: 153 of 153 passed.
- Static native-stack gate: passed.
- Static no-Python gate: passed.
- Static no-attribution gate: passed.
- Development-signed MSIX construction, signature validation, unpack, and payload rehash: passed.

Package receipt:

- Source commit: `d514719e42baa1ca647fd26fbb51666a4670cb3d`
- Source tree: `7cb4971b38f85405f2ebac026bbb8f2ec91c25a3`
- Package: `ForgeConductor-1.3.0.0-x64.msix`
- Package SHA-256: `56b87b1fd801e37d0cdc80c8ad7d7996eb1688a6d0b0fbdadefbfea0290fb33d`
- Bundle: `ForgeConductor-1.3.0.0-x64.zip`
- Bundle SHA-256: `85c3a3830d1dc13d1ac3e8c01a1cf1bf6c10b379a51230659a80454e65b30d32`
- Signature status: `Valid` (development certificate)

## Boundaries

Automated checks use disposable roots and do not mutate the operator's production profile. A development signature validates package construction and payload integrity; it is not a production publisher signature. CLU coverage reports opaque content and read gaps honestly and does not claim semantic proof that is absent from recorded evidence.
