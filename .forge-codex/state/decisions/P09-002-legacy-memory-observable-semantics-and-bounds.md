# P09-002: Legacy Memory Observable Semantics and Bounds

Status: Accepted

Date: 2026-08-26

## Context

The macOS implementation defines more behavior than its MCP schemas advertise.
It accepts legacy aliases, trims keys and queries, escapes SQL wildcard
characters, clamps result limits, hides three system-key families by default,
and filters list tags only after SQL has already applied the row limit. Its
`total` value ignores the caller's prefix and tag filters. These details are
observable and therefore form part of the compatibility contract.

The source caps keys, bodies, and returned rows, but places no hard bound on a
search query, a list prefix or tag filter, or the number and size of tags. That
conflicts with the governing prohibition on unbounded request fields and
collections. The port must add finite admission limits without silently
truncating accepted data.

## Decision

- Key validation trims surrounding whitespace and newlines, rejects an empty
  result and control characters, and accepts at most 512 raw UTF-8 bytes. The
  raw cap is checked before decoding or trimming so padded input cannot force
  unbounded preprocessing. Get and delete use the resulting key as an exact,
  case-sensitive primary-key lookup.
- A body is stored byte-for-byte, may be empty, and is limited to 524,288 raw
  UTF-8 bytes. It is never trimmed or silently shortened. U+0000 is rejected
  before persistence because the source binds and reads SQLite text through
  NUL-terminated C strings and therefore cannot round-trip content after that
  scalar. Failing explicitly resolves that source defect without silent data
  loss.
- New tags are trimmed, empty values are removed, duplicates are removed, and
  the result is sorted deterministically as in the source. The Windows target
  admits at most 32 raw input tags and at most 128 raw UTF-8 bytes per tag. A
  list tag filter and a list prefix each admit at most 512 raw UTF-8 bytes; a
  search query admits at most 4,096 raw UTF-8 bytes. Collection and byte caps
  are checked before iteration, decoding, or trimming. Exceeding a Windows-only
  bound returns a typed limit error and performs no mutation; it never drops
  tags or truncates a field. Body, tag, filter, and query values containing
  U+0000 are rejected deterministically for the same SQLite text-boundary
  reason as bodies.
- The tag, query, and prefix caps are a material governance resolution. The
  higher-precedence source supplies no finite values, while the governing
  resource rule requires them. The selected limits preserve ordinary source
  behavior, keep one request finite, and leave the source row cap and body cap
  unchanged.
- List and search default to 50 rows. Any supplied limit is clamped to the
  inclusive range 1 through 200, including non-positive and over-maximum
  values, rather than rejected.
- Delete's returned `system_key` predicate is exactly the case-sensitive
  prefixes `agent_run/`, `agent_active/`, and `continuity/`. Default list,
  search, and count visibility is a distinct predicate implemented by the
  source SQL `LIKE` expressions: it hides those families and their ASCII-case
  variants unless `include_system` is true. Set, get, and delete do not reserve
  or prohibit system keys.
- A list prefix is a literal prefix. Search is a literal substring over key,
  body, and the stored `tags_json` text. Backslash, percent, and underscore are
  escaped before use with `LIKE ... ESCAPE '\\'`; caller text never becomes a
  wildcard. Winsqlite's default `LIKE` behavior supplies the same
  case-insensitive ASCII matching as the source. The port does not enable
  `case_sensitive_like` or claim full Unicode case folding.
- Results order by `updated_at DESC, key ASC`. The key tie-break is a deliberate
  deterministic refinement when source timestamps are equal; the source race
  leaves tied rows unspecified, so no defined behavior is lost.
- Preserve the source's tag post-limit quirk. `memory_list` first applies the
  prefix, system visibility, ordering, and clamped SQL limit, then performs a
  case-sensitive, Unicode-canonical-equivalence decoded-tag membership test in
  memory. A tag-filtered page may consequently contain fewer than the requested
  limit even when later matching rows exist.
- Preserve the source's count quirk. `memory_list.total` counts all rows under
  the selected system-key visibility and ignores both prefix and tag filters.
  `count` is the number of returned rows.
- A valid decoded tag array is returned exactly as stored for migrated rows.
  Malformed JSON, a non-array value, or an array containing non-string values is
  treated as an empty tag list, matching the source. New writes always store a
  compact JSON string array. New-tag equality and ordering use NFC keys, but the
  first trimmed spelling is retained byte-for-byte when canonically equivalent
  inputs collapse, matching Swift `Set<String>` followed by `sorted()` without
  rewriting the visible tag.
- One injected UTC clock value is used for each upsert. Insert sets
  `created_at` and `updated_at` to that value. Update preserves `created_at` and
  replaces body, tags, and `updated_at`. Read values preserve the stored
  timestamps. Equal timestamps use the key tie-break above.
- Upsert plus its returned stored note is one transactionally consistent
  operation. Delete derives its boolean result from the same SQL mutation, so
  later wire fields `deleted` and `existed` agree. These close source races that
  were never an intentional or tested compatibility behavior.
- No legacy memory content redaction occurs before persistence. The notes are a
  user-visible source-compatible durable key/value surface, and rewriting their
  body would lose data. P14 must redact `body`, `content`, and `value` from audit
  records, diagnostics, and error detail; neither P09 layer logs note content.
- The row limit remains 200 and the body limit remains 512 KiB. Together with
  the key and tag caps, every typed result has a finite maximum. Neither the
  repository nor the later wire adapter may silently omit a selected row or
  truncate its body. A lower downstream envelope limit, if required, must fail
  as a typed all-or-error result.
- P14 maps typed validation outcomes to the source-visible errors and defaults:
  `invalid_key`, `missing_body`, `body_too_large`, `missing_query`,
  `empty_query`, and `store_error`; it also owns the `content`/`value` and
  `q`/`pattern` aliases and the list/search `include_body` representation.

## Consequences

The five operations retain their unusual but observable filtering, counting,
matching, ordering, timestamp, and clamping behavior. Existing callers do not
gain wildcard power, system rows remain hidden by default, and malformed legacy
tag JSON remains readable as an empty tag set.

The new tag and filter bounds are explicit policy rather than accidental
truncation. Worst-case responses remain large because source parity permits 200
maximum-size bodies, but they are finite and admission is deterministic.

## Evidence basis and source hashes

- `916df67b5ddd32538732cbe82e9c1382e1ddcf2817723055368e54298e325ee7`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift`
- `a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
- `d71f812c181d22c35e449e1bd293f3e056f8290e5b8131f6a1727d8ca5a6fc30`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/Models.swift`
- `cc6fb5cce18ac243fae179f66535bfc6ffc173860e3935d240aa64fa815a821a`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/MemoryToolTests.swift`
- `1ac4e48f30b909a9fdea0f0fb339550da9fc6d4c91a5cd9deb3ab80f0506c681`
  — `.forge-codex/instructions/architecture/SECURITY.md`

