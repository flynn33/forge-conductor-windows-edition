# P06-003: Current-User DPAPI Private Store

Status: Accepted

Date: 2026-08-25

## Context

Forge Conductor needs a per-user secret store without plaintext configuration, logs, ETW, backups, or handoffs. It also needs finite registry ownership and deterministic tamper behavior.

## Decision

- Secrets use `CryptProtectData` and `CryptUnprotectData` with `CRYPTPROTECT_UI_FORBIDDEN` only. `CRYPTPROTECT_LOCAL_MACHINE` is prohibited.
- Versioned `REG_BINARY` values live below an application-owned HKCU subkey. Logical keys are validated, SHA-256 hashed for value names, and also bound as optional DPAPI entropy.
- Keys are at most 128 UTF-8 bytes, secrets at most 64 KiB, and the store at most 128 distinct entries. Overwriting an existing entry at capacity is allowed; adding a new one is not.
- Enumeration is bounded before allocation and rejects unexpected value types or names as integrity failures.
- `DATA_BLOB` output is owned through `LocalFree` RAII. Internal plaintext and entropy buffers are erased with `SecureZeroMemory`.
- Missing values return an empty optional. Corrupt, swapped, or entropy-mismatched ciphertext returns `integrity_failure`.
- Mutation is serialized and rejects work after shutdown.
- Production construction remains with the Manager durable owner from P03-007. P06 exercises a dedicated integration-test composition and does not create a second CLI durable owner.

## Consequences

Secrets are current-user bound, finite, and never serialized with configuration. Cross-user binding evidence requires a genuinely different Windows SID; same-user processes or wrong entropy do not substitute for that proof.

## Rejected alternatives

- Machine-scope DPAPI: rejected because any local machine account could decrypt the blob.
- Plaintext files or configuration fields: rejected because they violate the security architecture.
- Unbounded registry values: rejected because key and value byte caps alone do not bound persistent collection size.
- CLI-owned production storage: rejected because the Manager is the single durable mutation coordinator.

## Evidence

- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/state/decisions/P03-007-data-isolation-and-cross-process-ownership.md`
- Microsoft `CryptProtectData` and `CryptUnprotectData` API contracts
