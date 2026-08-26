# P13-005: Native Services to P14 MCP Parity Boundary

Status: Accepted

Date: 2026-08-26

## Context

P13 supplies bounded native Windows services for seventeen canonical tool rows,
but a native service method is not itself an MCP tool. End-to-end parity also
requires the canonical name, input schema, compatible aliases and defaults,
dispatch, authorization translation, result metadata, error conversion, and
wire response shape. Those responsibilities belong to the P14 tool registry and
router rather than the platform adapters qualified by G13.

## Decision

The exact native-service handoff for the seventeen rows is:

| MCP row | P13 contract method | P13 Windows implementation |
| --- | --- | --- |
| `fs_read` | `IFileSystem::readFile` | `WindowsFileSystem` |
| `fs_write` | `IFileSystem::writeFile` | `WindowsFileSystem` |
| `fs_edit` | `ITextFileEditService::replaceAll` | `WindowsFileSystem` |
| `fs_list` | `IFileSystem::list` | `WindowsFileSystem` |
| `fs_glob` | `IPathGlobService::glob` | `WindowsPathGlobService` |
| `fs_mkdir` | `IFileSystem::createDirectory` | `WindowsFileSystem` |
| `fs_delete` | `IFileSystem::remove` | `WindowsFileSystem` |
| `fs_move` | `IFileSystem::move` | `WindowsFileSystem` |
| `git_status` | `IGitService::status` | `WindowsGitService` |
| `git_diff` | `IGitService::diff` | `WindowsGitService` |
| `git_log` | `IGitService::log` | `WindowsGitService` |
| `git_add` | `IGitService::add` | `WindowsGitService` |
| `git_commit` | `IGitService::commit` | `WindowsGitService` |
| `search_text` | `ITextSearchService::search` | `WindowsTextSearchService` |
| `pdf_write` | `IPdfService::write` | `WindowsPdfService` |
| `pdf_from_file` | `IPdfService::fromTextFile` | `WindowsPdfService` |
| `shell_exec` | `IShellService::execute` | `WindowsShellService` |

P14 owns the MCP schemas and tool registry, name-to-service routing, capability
translation, defaults, error and success response payloads, and observable
metadata. In particular, P14 owns the macOS-compatible `fs_read` field aliases,
the 1-based offset and length interpretation, the default 200-line window, line
range and truncation metadata, and the final response object. P13 provides the
bounded byte read beneath those semantics.

The seventeen rows remain open in `mcp-tool-parity.json` through G13. They may
advance only when P14 supplies runtime evidence for their registered schemas,
routing, defaults, metadata, aliases, and wire payloads. A G13 pass qualifies the
native service layer and must not be presented as end-to-end MCP parity.

## Consequences

P14 can compose and test MCP behavior without duplicating Windows filesystem,
process, Git, search, or PDF effects. Conversely, P13 tests cannot satisfy a P14
tool row merely by invoking the service directly. Any MCP compatibility detail
not represented by the contracts above remains an explicit P14 responsibility.

## Evidence

- `.forge-codex/instructions/plans/mcp-tool-parity.json`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
- `include/ForgeConductor/Contracts/IFileSystemServices.h`
- `include/ForgeConductor/Contracts/IPathGlobService.h`
- `include/ForgeConductor/Contracts/INativeToolServices.h`
- `.forge-codex/state/decisions/P13-001-bounded-native-tool-boundaries.md`
