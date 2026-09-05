# MCP and LM Studio integration

## MCP transport

JSON-RPC 2.0 over newline-delimited UTF-8 stdio. One compact JSON object plus newline per response. No `Content-Length`, banners, logs, progress, or stack traces on stdout. Enforce maximum line/depth/object/response, bounded outstanding calls, deterministic tool order, cancellation, EOF cleanup, stable errors, and exact supported protocol negotiation.

All 53 baseline tools preserve names, input schemas, defaults, result envelopes, error codes, limits, authorization, and side effects.

## Primary/fallback registration

Distinct role, identity, environment, logs, deployment ID/revision, and independent initialize/tools-list smoke.

## LM Studio discovery/deploy

Discover installed/current version, process image, supported config paths, existing `mcp.json`, API/server capability, and actual reload behavior. Never guess a path.

Transactional deploy: lock, preserve original bytes/metadata, parse, preserve foreign/unknown entries, stage distinct Forge entries, flush/reopen/validate, atomic replace with journal/backups, supported reload/relaunch, verify observed revision, independent role smokes, commit or full rollback.

## Native local model API

The Forge-owned session host may use supported LM Studio v1 stateful chats and configured Forge MCP integration. Use WinHTTP, authentication when configured, explicit capability/version adapters, bounded SSE parsing, cancellation, idempotency, token/context evidence, and no silent authenticated-to-unauthenticated fallback.
