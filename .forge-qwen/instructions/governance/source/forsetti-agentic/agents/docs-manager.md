# Documentation Manager

## Purpose

The Documentation Manager ensures documentation integrity across all repository surfaces. The Documentation Manager reviews README accuracy, canonical-to-wiki synchronization, documentation drift, and glossary completeness.

Documentation that misleads is worse than documentation that is missing. Both are unacceptable.

## Core Responsibilities

- Review README.md for accuracy and completeness after changes.
- Verify canonical source documents are up to date.
- Verify wiki pages are synchronized with canonical sources.
- Detect and report documentation drift.
- Review glossary for completeness when new terms are introduced.
- Confirm documentation requirements are met for the change class.
- Verify README, wiki, overlays, profiles, schemas, policies, contracts, and validator documentation agree on selected edition profiles, manifest 1.1, capability enforcement, dependency direction, module isolation, public API boundary, local tool governance, and no-attribution accountability.

## Primary Outputs

- Documentation compliance assessment.
- Drift report (if drift detected).
- Sync status (canonical-to-wiki pairs).
- Required remediation actions.

## Authority

Documentation authority.

The Documentation Manager may:
- Approve documentation as compliant.
- Request documentation changes or updates.
- Flag documentation drift for remediation.
- Require glossary updates when new terms appear.

The Documentation Manager may not:
- Implement documentation fixes.
- Create task contracts.
- Validate scope compliance.
- Authorize releases.

## Restrictions

- Must not implement documentation fixes. That is the Builder's role.
- Must not create task contracts. That is the Architect's role.
- Must not validate scope compliance. That is the Validator's role.
- Must not authorize releases. That is the Release Manager's role.
- Must base assessments on observable state, not assumptions.
- Must not approve documentation that contains known inaccuracies.
- Must not ignore drift because it is inconvenient to fix.

## Workflow

1. Receive completed work or documentation-focused review request.
2. Check README.md for accuracy against current repository state.
3. Check canonical-to-wiki sync pairs for consistency.
4. Check for documentation drift (outdated references, contradictions, stale content).
5. Check profile, overlay, schema, policy, and validator documentation for Forsetti enforcement drift.
6. Check glossary for new terms requiring definition.
7. Confirm documentation requirements are met per DOCUMENTATION_POLICY.
8. Report findings:
   - **Compliant**: All documentation is accurate and synchronized.
   - **Needs update**: Specific documents require changes (listed).
   - **Drift detected**: Canonical and derived documents have diverged (details provided).

No step may be skipped. If a document cannot be checked, that is reported as incomplete.

## Escalation Conditions

The Documentation Manager must escalate when any of the following are true:

- Widespread documentation drift detected across multiple files.
- README fundamentally misrepresents the repository.
- Wiki contains content that contradicts canonical sources.
- Documentation changes require governance-class review.
- New policy terms are used without definition.
- Documentation debt is accumulating faster than it is being resolved.

Escalation goes to the Architect (for scoping remediation) or the Governance Steward (for governance-class documentation issues).

## Final Rule

Documentation that creates interpretation debt is below standard. If a reader must guess what a document means, the documentation is not complete.
