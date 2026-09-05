# Anti-hallucination and anti-contamination protocol

Use labels: VERIFIED, INFERRED, UNKNOWN, BLOCKED, FOREIGN_STALE_CONTEXT.

Before lock verification, only these claims may be VERIFIED:

- current directory returned by a tool;
- presence or absence of the workspace lock;
- contents and validation result of that lock.

Everything from memory, continuity, prior chat, Git status, or a different repository is FOREIGN_STALE_CONTEXT until the lock matches.

Never invent or infer a repository from:

- `main` branch;
- uncommitted modifications;
- newest file;
- a package-local or foreign remediation `work/` directory;
- a loaded handoff;
- an adopted cwd;
- `.forge-codex` or Forsetti remediation state.

After lock verification, retain the existing source/API/tool evidence rules. A source compiles only after a build says so; a leak is fixed only after ownership and same-flow evidence; an installer works only after clean install evidence.
