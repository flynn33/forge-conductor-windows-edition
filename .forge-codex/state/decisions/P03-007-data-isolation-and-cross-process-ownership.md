# P03-007: Data Isolation and Cross-Process Ownership

Status: Accepted

Date: 2026-08-25

## Context

Forge requires central operational data, isolated per-project memory/continuity data, configuration, manager state, and a session ledger across several processes. Concurrent independent owners would make recovery and mutation ordering nondeterministic.

## Decision

All Forge stores are private_to_module. No peer Forsetti module receives direct database or filesystem authority to forge-conductor.central, forge-conductor.project, forge-conductor.configuration, forge-conductor.manager-state, or forge-conductor.session-ledger.

ForgeConductor.Manager.exe is the per-user cross-process coordination owner for durable mutations and long-lived repositories. GUI, CLI, MCP, and session-host processes use versioned, authenticated current-user IPC clients for coordinated state operations. The manager serializes store ownership, transactions, project-repository open/close, recovery, maintenance, and durable publication. Setup/import obtains an explicit quiescence lease before a transactional store replacement.

The default root is %LOCALAPPDATA%\Forge Conductor; FORGE_CONDUCTOR_HOME is accepted only as an explicit development/test override. The central database contains operational, audit, presence, legacy, and global handoff data. Each canonical project identity has a separate database for project memory and continuity. Winsqlite3 connections, statements, transactions, backups, and locks are typed RAII owners.

Project repositories use profile-bound LRU limits of 4, 8, or 16. Named pipes use a current-user ACL, a 4-byte little-endian length plus UTF-8 JSON frame, a 2,097,152-byte maximum frame, protocol/correlation/deadline fields, cancellation, and orderly shutdown. The loopback dashboard is manager-owned, loopback-only, and authenticated with a DPAPI-protected token.

## Consequences

Cross-process writes have one authority, project data cannot bleed across canonical roots, and repository eviction has an explicit close path. Manager unavailability is a typed service failure and never authorizes an uncoordinated second owner.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. Data services are application-owned adapters exposed through declared public contracts; framework internals never receive direct Forge store ownership.

## Evidence

- .forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md - SHA-256 a2b1e5eaf813b1f52ca4c30cd6e1977a054cb416d46b58ff12f13e4cfbbfd519
- .forge-codex/instructions/architecture/PROCESS_MODEL_AND_IPC.md - SHA-256 3da5cb2b39bdd3e55494e172177a052e8999deea9d4e92b4a95e36a7fef31e3d
- .forge-codex/instructions/architecture/SECURITY.md - SHA-256 1ac4e48f30b909a9fdea0f0fb339550da9fc6d4c91a5cd9deb3ab80f0506c681
- .forge-codex/instructions/plans/resource-budgets.json - SHA-256 f80c5d57081d47b87ddb77027f843c912bcf3c3e558c7ade42b4db4828760965
