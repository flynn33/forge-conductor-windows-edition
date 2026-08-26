# P13-004: Deterministic Native PDF Writer

Status: Accepted

Date: 2026-08-26

## Context

The macOS product emits PDF 1.4 directly using native Swift byte buffers and a
built-in Helvetica font. It supports headings, bullets, fenced code, wrapping,
titles, repeated page titles, multiple pages, a cross-reference table, and an
atomic destination write. Windows may not add a Python, Node, .NET, or third-party
PDF runtime.

The existing `IPdfService` returned no metadata and omitted the optional title
for `pdf_from_file`, even though both are observable macOS behavior.

## Decision

- `IPdfService` returns an immutable `PdfWriteReceipt` containing destination,
  bytes written, page count, engine identifier, and title. Text-file conversion
  accepts the caller's optional title after P14 applies the compatible default.
- The P13 writer emits a deterministic bounded PDF 1.4 object graph in C++20,
  including catalog, pages, Helvetica font, content streams, exact offsets,
  cross-reference table, trailer, `startxref`, and EOF marker.
- Layout retains markdown-like headings, bullets, fenced code, wrapping,
  repeated titles, and multi-page pagination. Parentheses, backslashes, control
  bytes, and non-ASCII UTF-8 bytes are escaped as valid PDF literal-string octal
  sequences.
- Title is capped at 512 UTF-8 bytes, source/body at 2 MiB, wrapped rows at
  200,000, and final output at 16 MiB. Line wrapping advances monotonically
  through the input. Tab expansion uses bounded linear passes. Both operations
  check cancellation and deadlines while processing long unbroken input as
  well as during layout and before publication.
- The writer depends on `IAtomicFileStore` for bounded source reads and atomic
  binary destination publication, keeping Win32 handles out of the PDF layout
  object. This is intentionally distinct from the UTF-8-only filesystem tool
  adapter because a conforming PDF includes non-text header bytes and may exceed
  the filesystem tool's 2 MiB text cap. Missing authorized destination parents
  are created through the shared handle-aware namespace engine before the
  atomic store publishes the file.
- Qualification loads the emitted file with `Windows.Data.Pdf`, opens page zero,
  and renders it to a nonempty in-memory stream. Structural marker checks alone
  are not accepted as runtime PDF evidence.

## Consequences

ASCII content is rendered and extractable with the built-in Helvetica font.
Arbitrary Unicode bytes remain syntactically preserved but are not guaranteed to
render as the intended glyph because no embedded Unicode font is present. This
matches the practical limitation of the macOS Helvetica emitter and is retained
as an explicit parity risk; Unicode font embedding can be improved later without
changing tool contracts. Windows runtime loading and first-page rendering now
provide an independent validity check on the alpha machine.

## Evidence

- macOS `PDFWriter.swift`, `DocsToolPack.swift`, and PDF tests in `CoreTests.swift`
- `.forge-codex/instructions/docs/PORTING_MAP.md`
- `include/ForgeConductor/Contracts/INativeToolServices.h`
- `include/ForgeConductor/Domain/PdfModels.h`
