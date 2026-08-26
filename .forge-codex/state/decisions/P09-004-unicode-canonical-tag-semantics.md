# P09-004: Unicode Canonical Tag Semantics

Status: Accepted

Date: 2026-08-26

## Context

The macOS 0.9.0 memory tool trims tags, constructs `Set<String>`, sorts that
set, and later tests a list filter with `String.contains`. The attached package
pins Swift tools 6.2. Swift string equality and ordering operate on canonical
Unicode representations. Consequently, composed and decomposed spellings of
the same text are one tag: the first trimmed spelling survives set insertion,
sorting follows the canonical scalar key, and either spelling matches the
other during list filtering.

Raw UTF-8 byte equality is not compatible with that behavior. It admits both
spellings as distinct tags, orders them by their encodings, and makes a
cross-form filter miss. Rewriting every visible tag to one normalized spelling
would also lose observable source behavior because the source retains the
first input spelling.

The neutral Domain and Contracts layers cannot call Windows NLS APIs. Adding a
Unicode table dependency is prohibited, and maintaining an application-owned
copy of Unicode normalization data would create a second security and update
surface.

## Decision

- Introduce one narrow `IUnicodeCanonicalizer` contract. It accepts a bounded,
  valid UTF-8 string and returns its NFC UTF-8 comparison key as a typed result.
  The interface owns no state, resource, or platform type. `NfcUtf8Key` has no
  public arbitrary-string constructor: its factory revalidates the UTF-8 and
  output-size invariant so a faulty adapter cannot inject malformed or
  unbounded comparison state into application or persistence collections.
- Implement the production adapter as a final Windows infrastructure class. It
  converts with the existing strict UTF boundary, calls
  `NormalizeString(NormalizationC, ...)` with explicit lengths, bounds the
  two-pass result before allocating, and converts back to strict UTF-8. No
  exception crosses the contract boundary.
- The legacy-memory application service receives shared immutable ownership of
  the canonicalizer through its constructor, so callers cannot create a
  dangling dependency by passing a temporary. Tag admission still applies the
  raw collection and byte limits, UTF-8 validation, trimming, and empty removal
  before canonicalization.
- For new writes, use the NFC key for equality and ordering while retaining the
  first trimmed original string as the stored and returned value. Compare keys
  as unsigned UTF-8 bytes; valid UTF-8 byte order preserves Unicode scalar
  order and avoids UTF-16 supplementary-plane ordering differences.
- The Windows repository receives the same abstraction. Its canonical-write
  guard and post-SQL-limit tag filter use NFC keys. Valid migrated tag arrays
  remain byte-for-byte readable and are not rewritten on read.
- Service dependency postconditions compare tag filters by NFC key. All
  contract fakes share one deterministic canonicalizer fake and apply the same
  first-spelling, equality, ordering, and filter policy as production. Native
  tests cover composed/decomposed collapse with first-spelling retention,
  cross-form filtering, and ordering across BMP and supplementary scalars. A
  native integration case also drives the final application service through
  the real Windows repository and Winsqlite store with cross-form input and
  filtering.
- This policy is locale-independent. It does not case-fold tags and does not
  change SQLite's separate ASCII-insensitive `LIKE` behavior for prefixes,
  searches, or default system-row visibility.

## Consequences

Legacy tag behavior matches the source for canonically equivalent Unicode
without changing user-visible bytes or adding an unapproved dependency. The
platform implementation remains below the Contracts boundary, and the Domain
library remains free of Windows headers and APIs.

Windows NLS tables are supplied by the installed operating system, whereas
Swift supplies its own Unicode implementation. The tests pin the source-used
canonical-equivalence cases and scalar-order boundary; broader cross-version
Unicode corpus comparison remains appropriate evidence for a later full parity
and release gate.

## Evidence basis and source hashes

- `2b9da6f8c1debce8fcf55ad647f6efb209a8b8b73e0fe11778feb9362bcbd146`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Package.swift`
- `916df67b5ddd32538732cbe82e9c1382e1ddcf2817723055368e54298e325ee7`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift`
- `a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
- `c7e06d649906d0854f1bc0d4e435219b2036268dae8734618f5dd97c4e42a36f`
  — `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
