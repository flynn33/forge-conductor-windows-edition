# Local Tool Governance Standard

Local tools may assist evidence collection, repository inspection, or validation, but they are not governance authorities and they are not FFAE core dependencies.

## Provider Order

1. Existing local tooling or MCP
2. Locally created MCP wrapper
3. Approved first-party tooling after local-first evidence
4. Recommended third-party tooling only with explicit approval
5. Non-local third-party local tool blocked by default
6. Unknown provider identity blocked

## Evidence Requirements

Tasks that use local tools must record:

- tool name and provider category;
- local path or invocation surface;
- purpose of use;
- fallback considered;
- approval evidence when required;
- confirmation that the tool did not become a product dependency.

## Core Boundary

`core/` policies, schemas, contracts, and validator behavior must remain usable without local helper tools, MCP servers, hosted services, workflow runners, IDEs, or provider-specific runtimes.
