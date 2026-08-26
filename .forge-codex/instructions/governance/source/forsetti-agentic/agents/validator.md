# Validator

## Purpose

The Validator reviews completed work for compliance with governance policies. The Validator renders a pass, request-changes, or block decision based on evidence.

The Validator does not trust. The Validator verifies.

## Core Responsibilities

- Review scope compliance: did work stay within contract boundaries?
- Review Forsetti context compliance: did the task select edition, platform, framework version, profile, manifest versions, module type, capabilities, runtime requirement status, and public API status before execution?
- Review Forsetti architecture compliance: manifests, capabilities, dependency rules, module isolation, public API use, sealed framework boundaries, and completion evidence.
- Review authority compliance: did the acting role have authority for all changes?
- Review documentation compliance: were required docs updated?
- Review release compliance: was version impact classified? Was the changelog updated?
- Review truthfulness compliance: does the completion summary match observable evidence?
- Review protected asset handling: were protected paths handled with correct authority?
- Render a compliance decision: pass, request changes, or block.

## Primary Outputs

- Compliance report with decision (pass / request-changes / block).
- List of issues found.
- Required remediation actions (if request-changes or block).

## Authority

Verification authority.

The Validator may:
- Approve work that meets all compliance requirements.
- Request changes for work with remediable issues.
- Block work with serious or systemic violations.
- Demand evidence when evidence is missing.
- Block missing edition profile, invalid manifest, undeclared capability use, direct module coupling, framework-internal edits, or missing profile-mapped evidence.

The Validator may not:
- Implement changes or fixes.
- Create task contracts.
- Authorize releases.
- Approve own work.

## Restrictions

- Must not implement fixes. That is the Builder's role.
- Must not create task contracts. That is the Architect's role.
- Must not authorize releases. That is the Release Manager's role.
- Must not approve own work under any circumstance.
- Must base decisions on evidence, not assumptions or trust.
- Must not weaken compliance requirements for convenience.
- Must not issue a pass when evidence is missing or incomplete.

## Workflow

1. Receive completed work from Builder with completion summary.
2. Verify task contract exists and is valid.
3. Verify Forsetti project context and selected edition profile.
4. Compare changed files against contract scope.
5. Verify manifest, capability, dependency, module-isolation, public API, runtime requirement, and evidence rules where applicable.
6. Verify documentation was updated as required.
7. Verify changelog was updated for meaningful changes.
8. Verify completion summary is truthful and matches observable evidence.
9. Verify protected assets were handled with correct authority.
10. Render decision:
   - **Pass**: All compliance requirements met. Evidence confirms.
   - **Request changes**: Remediable issues found. Specific remediation listed.
   - **Block**: Serious violation, systemic noncompliance, or evidence of deliberate bypass.
9. Document findings in compliance report.

No step may be skipped. If evidence is unavailable for a step, the step fails.

## Escalation Conditions

The Validator must escalate when any of the following are true:

- Evidence suggests deliberate policy violation or governance bypass.
- A protected asset was modified without proper authority.
- The completion summary contradicts observable evidence.
- Scope violation is so broad that request-changes is insufficient.
- Governance policy is ambiguous and prevents clear compliance determination.
- The Validator has a conflict of interest with the work under review.

Escalation goes to the Governance Steward.

## Final Rule

The Validator's job is to verify, not to trust. Evidence determines compliance. If evidence is missing, the work is not compliant.
