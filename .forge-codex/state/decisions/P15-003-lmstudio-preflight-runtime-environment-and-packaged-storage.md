# P15-003: LM Studio Preflight Runtime Environment and Packaged Storage

Status: Accepted

Date: 2026-08-27

## Context

The fifth G15 real-host attempt reached the LM Studio predeployment MCP smoke
without changing host configuration, but the child exited before its MCP
handshake. The verifier intentionally provides an empty `FORGE_DEPLOYMENT_ID`
so the isolated smoke process cannot inherit an installed deployment identity.
The MCP serve root rejected the successful zero-character environment read.

Exact diagnostic reproduction also established three startup requirements that
the preflight must satisfy on the owner machine:

- the deployed `FORGE_CONDUCTOR_HOME` value must be honored when no explicit
  `--home` argument is supplied;
- the process supervisor's sanitized environment needs a bounded explicit
  `PATH` so the MCP composition root can discover Git and PowerShell; and
- app-owned registry, atomic-file, and SQLite handles opened beneath logical
  LocalAppData may resolve through the current packaged parent's exact
  `LocalCache\Local` mapping described by P12-004 and P15-002.

## Decision

- Treat a present, successfully read empty MCP environment value as an empty
  string. Keep strict path converters unchanged and continue rejecting empty
  path inputs.
- Enable the existing opt-in `FORGE_CONDUCTOR_HOME` application-path override
  in the MCP serve composition root. An explicit `--home` remains authoritative.
- Add the current bounded parent `PATH` to the LM Studio smoke request's
  explicit environment overlay. The process supervisor continues to discard
  ambient variables that were not selected by the caller.
- Apply the existing exact current-package LocalAppData redirect predicate when
  verifying app-owned atomic-file, project-registry, registry-lock, database
  directory, and database-leaf handles.
- Retain all existing local-drive, authority-root, reparse-point, object-type,
  suffix-identity, normalization, and package-family validation.
- Exercise the deployed environment shape through a separate LocalAppData MCP
  process snapshot and require creation of both the project registry and SQLite
  store beneath that isolated home.

## Rejected alternatives

- Supplying `--home` in the deployed LM Studio arguments would diverge from the
  existing deployment contract and leave the environment-based host path
  untested.
- Allowing empty strings in the shared path converter would weaken unrelated
  executable and directory validation.
- Restoring the entire ambient process environment would expand authority and
  make the smoke dependent on unrelated parent state.
- Textually accepting any `LocalCache` suffix would permit broader aliases than
  the operating-system mapping already bounded by P12-004.
- Repeating the full G15 rebuild or five-test suite would violate the owner's
  accepted alpha gate policy after the authoritative invocation had passed.

## Consequences

The LM Studio predeployment smoke now uses the same home and executable-search
inputs as the installed configuration while preserving bounded child-process
state. The focused MCP process test covers the empty identity, environment home,
and persistent-store initialization path. A broader environment allowlist and
foreign-package hardening remain in the owner-authorized post-alpha security
campaign.

The failed four-test correction remains recorded as failed evidence. Its three
passing tests are not rerun because the final change touched only the CLI MCP
environment boundary and its process snapshot; the exact affected target was
rebuilt and the previously failing test then passed.

## Evidence basis

- `.forge-codex/state/evidence/P15/windows-lm-studio-real-host-attempt-5-failed.json`
- `.forge-codex/state/evidence/P15/g15-attempt-5-predeployment-diagnostic.json`
- `.forge-codex/state/commands/20260827T152856112Z-b9099742.json`
- `.forge-codex/state/commands/20260827T154643696Z-7eedb3e9.json`
- `.forge-codex/state/commands/20260827T155313063Z-971b814b.json`
- `src/Hosts/Cli/McpServeCompositionRoot.cpp`
- `src/Infrastructure/Windows/WindowsLMStudioServeVerifier.cpp`
- `src/Infrastructure/Windows/Detail/AtomicReplaceEngine.cpp`
- `src/Persistence/Windows/WindowsProjectRegistryRepository.cpp`
- `src/Persistence/Windows/Detail/DatabaseNamespaceLease.cpp`
- `tests/Infrastructure/WindowsLMStudioServeVerifierTests.cpp`
- `tests/Mcp/McpServeProcessSnapshotTests.cpp`
- `.forge-codex/state/decisions/P12-004-packaged-localappdata-path-identity.md`
- `.forge-codex/state/decisions/P15-002-packaged-localappdata-process-path-equivalence.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
