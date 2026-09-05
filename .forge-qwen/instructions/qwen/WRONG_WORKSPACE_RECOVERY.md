# Wrong-workspace recovery

The following statements are contamination indicators:

- “continuity packet loaded and workspace adopted”;
- “the local repo is on main with uncommitted modifications” before lock verification;
- “remediation work is in the package’s work directory”;
- “the extracted source baseline is the work repository”;
- “I will review the instruction package and determine where the previous run stopped.”

When any indicator appears:

1. Stop all writes and Git operations.
2. Do not clean or alter the foreign repository.
3. Do not inspect its uncommitted files beyond the minimum needed to identify that it is foreign.
4. Locate no repository from memory. Use the operator-opened target directory or rerun bootstrap.
5. Require `WORKSPACE_LOCK.json` in the exact current root.
6. Run `Assert-QwenWorkspace.ps1 -RequireCurrentDirectory`.
7. Regenerate `ACTIVE_ASSIGNMENT.json` with `Prepare-QwenAssignment.ps1`.
8. If the regenerated assignment requires persistent memory, retrieve only its exact cursor key; absence is normal. Ignore generic or prior-run keys.
9. Resume only the exact active assignment. Generate and replace the new cursor only when that assignment closes with a handoff.

A foreign repository may contain valuable work. Recovery therefore never reverts, deletes, stages, commits, or migrates it automatically.
