# Forge Conductor 1.3.2 validation record

## Scope

- exact 180-second LM Studio MCP request deadlines for Primary, Fallback, and CLU;
- shared generation and validation across live configuration, plugin bridges, and host synchronization;
- fail-closed drift detection for missing, stale, or incorrectly typed timeout values;
- preservation of foreign MCP entries and unknown fields;
- native runtime and stable package version 1.3.2 / `1.3.2.0`.

## Root cause and host observation

The affected LM Studio chat reported `MCP error -32001 Request timed out` after approximately 60 seconds while its Forge `shell_exec` requests allowed up to 120 seconds. The chat sent its calls to Primary; no automatic Primary-to-Fallback replay occurred.

After all three live registrations were set to `timeout: 180000`, a Forge `shell_exec` call requested a 90-second limit, slept for 70 seconds, and returned `FORGE_TIMEOUT_REPAIR_OK`. The command elapsed time was 70,312 milliseconds and the LM Studio tool status was 70,545 milliseconds. This is a specific live host observation, not a latency guarantee.

## Automated verification

The final Release matrix completed with:

```text
100% tests passed out of 153
```

Static verification completed with:

```text
No-Python gate passed.
Native-stack gate passed.
No-attribution gate passed.
G15 static validation passed (264 assertions).
```

Focused coverage verifies:

- all three live registrations and all three plugin bridges emit the exact integer value;
- missing and stale 60-second values are rejected;
- exact signed and unsigned integers are accepted;
- floating-point, string, null, and boolean values are rejected;
- synchronized host state carrying a stale timeout is never acknowledged or used to trigger an unsupported host restart.

## Package contract

- identity: `ForgeConductor.Windows`
- numeric package version: `1.3.2.0`
- architecture: x64
- configuration: Release
- signing: development publisher certificate

The packaging workflow validates the compiled XAML surface, exact four-executable staging manifest, payload hashes, release CRT, package signature, and unpacked payload. The published distribution is rebuilt from the exact merged release commit.

## Qualification boundary

Fallback remains an independent LM Studio integration and derived health role. It is not an automatic retry router. A timed-out mutation may have completed after the client stopped waiting, so operators must inspect the affected state before retrying.

Live supported-provider continuity successor qualification was not rerun. This record does not claim an `Active` continuity lifecycle.
