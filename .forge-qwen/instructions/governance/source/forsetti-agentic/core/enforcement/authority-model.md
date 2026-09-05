# Forsetti Authority Model

Forsetti Agentic Edition governs coding-agent compliance with Forsetti Framework architecture. It does not implement framework runtime behavior, platform services, module activation, service resolution, UI composition, storage adapters, or hosted orchestration.

## Binding Authority Order

1. Human owner instruction
2. Selected Forsetti edition and version profile
3. Shared Forsetti sealed-runtime invariants
4. FFAE constitutional governance
5. FFAE policy manifests
6. Agent role instructions
7. Task contract
8. Issue, pull request, or local task instructions

Lower-authority instructions may narrow work, evidence, or scope. They may not widen or override selected edition profile rules, shared sealed-runtime invariants, public API boundaries, module isolation rules, manifest requirements, or no-attribution accountability requirements.

## Edition Profile Authority

An Architect must select the Forsetti edition profile before Builder execution begins. The selected profile controls:

- supported platforms;
- framework version;
- manifest schema and template version;
- module types;
- supported capabilities;
- dependency direction;
- public product boundary;
- required verification commands.

If task instructions conflict with the selected profile, the profile wins unless the human owner supplies an updated profile or explicit governance-class exception.

## Governance Boundary

FFAE may inspect repository files, contracts, manifests, profiles, changed-file evidence, and validation reports. FFAE must not become a runtime framework, orchestration server, hosted dependency, platform SDK, CLU dependency, MCP dependency, or hidden module loader.

## Escalation

A task is blocked until Architect or human-owner resolution when the Forsetti project context is missing, the profile cannot be selected, the target platform is unsupported, framework internals are touched without explicit authority, or the requested work would weaken sealed-runtime invariants.
