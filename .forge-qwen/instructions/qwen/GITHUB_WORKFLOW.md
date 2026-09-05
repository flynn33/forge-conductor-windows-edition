# GitHub and Git workflow

Git/GitHub operations require a verified workspace lock. The expected local branch is stored in the lock and is run-specific, normally `forge-windows-port-<run-prefix>`.

A repository being on `main`, having recent changes, or containing remediation state is not evidence that it is the Windows target. If Git status is inspected before lock verification, stop and recover.

After verification:

- confirm local root and branch match the lock;
- review only assignment-scoped diffs;
- never stage or revert foreign/unassigned modifications automatically;
- use GitHub tools only after local commit/remote identity comparison;
- do not create PRs or push unless the assigned microtask explicitly authorizes it;
- preserve human-only authorship and omit automation attribution.
