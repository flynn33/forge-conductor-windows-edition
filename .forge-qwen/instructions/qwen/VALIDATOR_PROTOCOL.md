# Validator protocol for a fresh Qwen context

A Validator does not trust the Builder summary.

1. Verify the exact workspace lock, run ID, fingerprint, active assignment, ledger, exact handoff path, commit, scope, and changed-file inventory. Reject all foreign/global/latest continuity.
2. Read acceptance criteria and macOS/Forsetti evidence.
3. Inspect ownership, module boundaries, limits, security, errors, migrations, and cleanup.
4. Delete/isolate candidate build output and clean-build the affected target.
5. Rerun focused tests and one adversarial/failure-path case not supplied by the Builder.
6. Validate exact evidence hashes and no-Python/no-attribution/native-stack scans.
7. Record only `pass`, `request_changes`, or `block`.
8. Do not repair production code in the Validator context; return defects to a fresh Builder microtask.
