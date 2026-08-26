# Evidence rules

## Source evidence

Source evidence includes path, line, symbol, excerpt hash, and interpretation. It proves code structure, not runtime behavior.

## Command evidence

Record:

- command and arguments;
- working directory;
- environment/tool versions;
- start/end UTC;
- exit code;
- stdout/stderr paths;
- output hashes;
- artifacts created;
- related phase/gate.

## Runtime evidence

Performance, leaks, lifecycle, GPU work, install behavior, and host synchronization require runtime captures. Store ETL/WPR, logs, screenshots where appropriate, database reports, UI automation output, and before/after measurements.

## Reproducibility

Every evidence item must be reproducible through a checked-in PowerShell script or documented native command. Redact secrets without destroying the facts needed to reproduce.

## Honest status

Allowed statuses:

- `not_started`
- `in_progress`
- `passed`
- `failed`
- `blocked_external`
- `not_applicable` with rationale

Do not use ambiguous statuses such as “mostly done.”
