# P12-004: Packaged LocalAppData Path Identity

Status: Accepted

Date: 2026-08-26

## Context

The standalone native session host uses the P06 default data root at
`%LOCALAPPDATA%\Forge Conductor`. On this Windows 11 machine, a desktop process
launched from a packaged parent has no package identity of its own, while
Windows still redirects an opened LocalAppData handle beneath
`%LOCALAPPDATA%\Packages\<family>\LocalCache\Local`. The path resolver correctly
noticed that the opened handle name differed from the logical path, but treated
the operating-system package redirection as an authority escape. Consequently
the production host could print its manifest but could not load its ledger.

## Decision

- Keep the P06 logical default root and exact-file capabilities unchanged.
- Continue to reject reparse points, UNC/device paths, lexical changes, and
  arbitrary opened-handle substitutions.
- When an opened LocalAppData handle differs from the requested path, accept it
  only when the complete final path is the exact Windows package-local mapping
  `LocalAppData\Packages\<one safe family component>\LocalCache\Local` plus the
  unchanged requested suffix.
- If the process exposes a package family through
  `GetCurrentPackageFamilyName`, require the opened family component to match
  it. The structural mapping remains available for the observed packaged-parent
  case where Windows reports `APPMODEL_ERROR_NO_PACKAGE` for the child process.

## Consequences

The installed or packaged-parent session host can use the documented
LocalAppData root without broadening its filesystem authority. Unpackaged
processes retain the prior exact-handle behavior. Release security hardening is
still deferred by OWNER-002, but the retained mapping is bounded and covered by
the real session-host health/self-test commands on the owner machine.

## Evidence basis

- `src/Infrastructure/Windows/Detail/WindowsPathResolver.cpp`
- `src/Hosts/SessionHost/SessionHostCompositionRoot.cpp`
- `ForgeConductor.SessionHost.exe --health`
- `ForgeConductor.SessionHost.exe --self-test`
- `.forge-codex/state/decisions/P06-001-windows-root-layout-and-infrastructure-bounds.md`
