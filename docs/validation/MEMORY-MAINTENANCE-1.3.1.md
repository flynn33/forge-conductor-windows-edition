# Forge Conductor 1.3.1 validation record

## Scope

- complete project-memory browsing through Manager cursors;
- scrollable Data maintenance rows with visible single-row and multi-selection deletion;
- separate continuity and confirmed scope-reset controls;
- Git and PowerShell 7 discovery under constrained MCP launch environments;
- native runtime and stable package version 1.3.1 / `1.3.1.0`.

## Automated verification

The final Release matrix completed with:

```text
100% tests passed out of 153
```

The focused Display All integration emits:

```text
PASS manager_connection.project_memory_display_all_pages
```

That test creates 101 records, obtains the expected record order from Manager pagination, invokes the application connection's Display All path, and compares the complete result.

Static verification completed with:

```text
No-Python gate passed.
Native-stack gate passed.
No-attribution gate passed.
```

## Package contract

- identity: `ForgeConductor.Windows`
- numeric package version: `1.3.1.0`
- architecture: x64
- configuration: Release
- signing: development publisher certificate

The packaging workflow validates the compiled XAML surface, exact four-executable staging manifest, payload hashes, release CRT, package signature, and unpacked payload.

## Qualification boundary

The automated matrix and package checks do not substitute for an installed GUI walkthrough. The operator acceptance path is:

1. Open **Projects**, select a project, and choose **Display All**.
2. Open **Settings → Data maintenance** and verify scrollable rows, **Delete record**, and **Delete selected**.
3. Delete a disposable record and verify that the refreshed list no longer contains it.

Live supported-provider continuity successor qualification was not rerun. This record does not claim an `Active` continuity lifecycle.
