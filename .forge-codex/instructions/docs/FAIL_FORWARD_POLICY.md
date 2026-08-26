# Fail-forward policy

## Principles

- Preserve valid completed work.
- Fix forward rather than reverting unrelated successful phases.
- Keep the repository buildable at coherent checkpoints.
- Never suppress, delete, or weaken a failing test merely to progress.
- Never convert a hard gate into a warning.
- Never lose user data during migration or upgrade testing.

## Failure handling

For every failure:

1. capture the exact command, output, exit code, configuration, and environment;
2. classify: source, compiler, linker, runtime, protocol, data, security, packaging, environment, or flaky test;
3. find the first actionable root cause;
4. create or update a blocker record;
5. implement the smallest durable fix;
6. add a regression test;
7. rerun focused validation;
8. rerun the containing gate;
9. update evidence and close or narrow the blocker.

## Retry policy

Retries require a reason. Transient operations use exponential backoff, jitter, a hard maximum, and persisted retry state. Deterministic failures are never blindly retried.

## Partial platform availability

- No ARM64 toolchain: complete x64 and leave an executable ARM64 validation script; do not claim ARM64 passed.
- No supported GPU counters: expose unavailable/stale state and validate fallback; do not report zero.
- No LM Studio install: validate stdio MCP and deployment with fixtures; retain a real-host gate.
- No production signing secret: create a development-signed installable package and parameterize release signing.
- No supported host session API: build and validate the Forge-native logical session host.
