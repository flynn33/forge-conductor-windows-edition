# P13-002: Handle-Aware Filesystem, Search, and Glob

Status: Accepted

Date: 2026-08-26

## Context

P06's app-data atomic store is deliberately narrow. Its publication rules,
metadata policy, and POSIX-rename requirement are not a general workspace file
API. P13 nevertheless needs read, write, edit, list, recursive glob, recursive
search, directory creation, delete, and move behavior without following Windows
reparse points or reopening an unvalidated namespace at a mutation boundary.

The macOS glob handler invokes `find -name`, although product documentation uses
segment patterns such as `**/README*`. The macOS search handler invokes recursive
grep, excludes `.git` and `node_modules`, and truncates after collecting output.

## Decision

- `WindowsFileSystem` receives `IAtomicFileStore` through constructor injection
  and uses it selectively for bounded file-content reads and complete atomic
  publication of writes and edited results. The contract is not widened.
  Namespace enumeration, traversal, directory creation, deletion, and move stay
  in the separate P13 handle-aware workspace engine.
- The namespace engine reuses the qualified path validation, UTF conversion,
  Win32 error, and RAII handle concepts from P06 without treating the P06
  app-data store as a general namespace API.
- Operations validate the capability access mode and canonical root binding,
  reject device/UNC/alternate-stream/non-normalized paths, pin or revalidate
  ancestry at effects, and never traverse a reparse-point directory.
- Recursive traversal is iterative and bounded. Entries are sorted by Windows
  ordinal case-insensitive order with an ordinal tie-breaker, so results are
  deterministic on the target machine. Directory listing returns the sorted
  bounded prefix together with explicit `truncated` metadata, matching the
  observable macOS result instead of failing when another entry exists.
- Authorized writes and move destinations create missing parent directories
  component by component beneath the already-authorized root. Every component
  is opened without following reparses before the next component is created or
  used.
- An opened native object retains the complete anchored authorized-path owner,
  not only the leaf handle. Active filesystem operations share for ordinary
  reads and writes but deny delete sharing, so an ancestor cannot be renamed
  out of the authority namespace during the effect.
- Anchor sharing is an explicit per-caller policy. P06 atomic storage denies
  concurrent directory writes because its publication boundary must exclude
  in-place reparse mutation. P13 workspace operations permit ordinary
  concurrent writes while continuing to deny delete sharing and ancestor
  rename. Neither caller inherits the other's semantics implicitly.
- `fs_glob` implements component-aware `*`, `?`, and `**`. This corrects the
  macOS implementation defect while preserving the documented feature.
- Text search is a native case-sensitive UTF-8 regular-expression search using
  POSIX basic-regex semantics (`std::regex_constants::basic`) with stable
  `path:line:text` results. It skips `.git`, `node_modules`, binary/NUL files,
  malformed UTF-8, and reparses. Invalid expressions return `InvalidRequest`.
  P13 bounds traversal and checks cancellation and deadlines between lines;
  dedicated pathological-regex hardening is deferred by OWNER-002.
- Text edit replaces every non-overlapping occurrence in a strict UTF-8 file and
  publishes only the complete bounded result. A missing old string is a typed
  no-match failure.
- Recursive delete never follows reparses and cannot target an authority root.
  Move requires separately authorized source and destination capabilities and
  retains every opened destination-parent ancestor through the native rename.
  Those destination anchors request only listing, traversal, and attribute
  access while sharing reads and writes; the native rename performs the
  file-versus-directory insertion access check without a self-conflicting open.

## Consequences

Glob behavior is more consistent with the documented macOS product surface than
the source's basename-only `find -name` call. Search retains recursive text
discovery, case-sensitive POSIX basic-regex matching, line numbers, exclusions,
order, and bounds. Cancellation and deadline checks bound the traversal between
lines; OWNER-002 defers stronger defenses against pathological expressions until
the post-alpha hardening campaign.

Bounded file-content reads and complete replacement publication reuse the
constructor-injected P06 atomic-store boundary. General namespace semantics do
not inherit every app-record metadata promise from P06. Unsupported filesystem
objects fail explicitly; traversal and namespace mutation remain independently
owned by the P13 handle-aware engine. Retained anchors prevent namespace races
without blocking concurrent write-capable opens that do not rename or delete an
anchored directory.

## Evidence

- `.forge-codex/state/decisions/P06-006-handle-relative-atomic-publish.md`
- `.forge-codex/state/decisions/P06-009-same-token-hard-link-publication-boundary.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- macOS `FilesystemToolPack.swift`, `SearchToolPack.swift`, and `CoreTests.swift`
- `.forge-codex/instructions/docs/PORTING_MAP.md`
