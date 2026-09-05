# Persistence and macOS migration

Default home is `%LOCALAPPDATA%\Forge Conductor`; support `FORGE_CONDUCTOR_HOME` and `--home` after canonicalization/ACL/identity checks.

Separate central operational storage from per-project memory/continuity storage. Migrations are versioned, forward-only, transactional, backed up/checksummed, tested from fixtures, and never auto-destructively recreate corruption before preserving evidence.

Neutral cross-platform import/export uses UTF-8 versioned JSON with source product/version, project identity, records, links, tombstones, handoffs, per-record/canonical hashes, preview, collision policy, idempotency, transaction, rollback, and size limits.

Native `import-macos` dry-run/imports compatible config, central/project data, memory, handoffs/operations, agent sessions, audit where compatible, and playbooks/resources. Translate platform paths; do not import LaunchAgents, entitlements, Keychain payloads, Metal state, or Apple executable paths.
