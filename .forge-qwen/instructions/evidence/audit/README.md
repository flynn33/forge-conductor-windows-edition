# Included Audit Evidence

The audit directory is immutable input to the repair run. Do not edit it to make a finding appear resolved.

Use:

- `Forge-Conductor-Consolidated-Audit.md` for the narrative report;
- `Forge-Conductor-Consolidated-Findings.tsv` for sortable findings;
- `Forge-Conductor-Key-Evidence.md` for source excerpts;
- `Forge-Conductor-Build-Test-Summary.md` for the original environment boundary;
- `Forge-Conductor-Audit-Evidence.json` for machine-readable command evidence;
- `VALIDATION-PASS.txt` for audit-package validation.

The prior audit was source-first and could not perform native macOS Instruments/memgraph validation in its Linux environment. The repair run must collect that runtime evidence on macOS before closing E2 ownership risks or performance gates.
