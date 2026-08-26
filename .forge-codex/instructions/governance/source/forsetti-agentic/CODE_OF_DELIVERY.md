# Code of Delivery

This document defines the delivery doctrine for all contributors and agents working in this repository. These principles are binding. They apply to every meaningful delivery regardless of the contributor's role, the size of the change, or the urgency of the task.

## Delivery Principles

### 1. Deliver Under Contract

Every meaningful delivery must be governed by a task contract that defines scope, authority, and expected outputs. Work without a contract is unauthorized work. The contract is not bureaucracy — it is the agreement that makes the work reviewable, traceable, and accountable.

### 2. Deliver Within Scope

The task contract defines the boundaries of delivery. Files outside scope must not be modified. If additional scope is needed, the contract must be amended before proceeding.

Scope creep is not initiative — it is a violation. Even beneficial changes outside scope undermine the contract system. If you see something that needs fixing outside your scope, file a separate issue. Do not fix it as a side effect.

### 3. Deliver Truthfully

Completion claims must match observable evidence.

- "Done" means all required outputs delivered and validated.
- "Tested" means tests were run and results reported.
- "Validated" means validation commands were executed and outcomes recorded.
- Partial completion must be stated as partial.
- Do not present assumptions as facts.
- Do not hide known issues.
- Do not claim validation was performed when it was not.

False completion claims are blocking violations. There is no acceptable reason to misrepresent delivery status.

### 4. Deliver in Reviewable State

Every delivery must be reviewable against its governing contract. The reviewer must be able to verify scope compliance, documentation status, release classification, and truthfulness from the delivery artifacts alone.

If the reviewer has to ask "what was the contract?" the delivery is not reviewable. If the reviewer has to search for evidence that should have been presented, the delivery is not reviewable. Include the evidence. State the facts. Make compliance verification straightforward.

### 5. Deliver Documentation With the Change

Documentation is part of delivery, not an afterthought. If the change affects understanding, usage, governance, or release history, the documentation must be updated as part of the same delivery.

Deferred documentation is documentation drift. Documentation drift compounds — each deferred update makes the next one harder and less likely. Deliver documentation with the change or do not deliver the change.

### 6. Deliver Release Traceability

Every meaningful change must be traceable through version classification and changelog entries. A change that is merged without proper release classification creates traceability gaps that compound over time.

Version impact must be classified. Changelog entries must be written. These are not optional steps to be performed "when convenient." They are required delivery artifacts.

### 7. Deliver Known Limits Honestly

If there are known limitations, unresolved issues, or risks, disclose them in the completion summary. Honest disclosure of limits is professional. Hidden limits are governance violations.

No delivery is expected to be perfect. Every delivery is expected to be honest about what it achieved and what it did not.

## Delivery Checklist

Before submitting a delivery, verify the following:

- [ ] Task contract exists and is current
- [ ] All changes are within contract scope
- [ ] Documentation updated as required
- [ ] Changelog entry added for meaningful changes
- [ ] Release impact classified
- [ ] Completion summary produced with evidence
- [ ] Known issues disclosed (or explicit "none")
- [ ] No drive-by edits or hidden scope expansion

If any item cannot be checked, the delivery is not ready for submission. Identify the blocker and resolve it before submitting.

## Anti-Patterns

The following delivery anti-patterns are prohibited:

### "Fixed" Without Evidence

Claiming a bug is fixed without describing what was wrong, what the fix does, or how it was verified. A fix without evidence is an unverified claim.

### Documentation Omitted

Delivering changes without updating required documentation. "I'll update docs later" is documentation drift in progress. Later rarely arrives. Deliver documentation now.

### Hidden Scope Expansion

Modifying files beyond the contract scope without disclosure. Even beneficial changes outside scope are violations. The harm is not in the change — it is in the uncontrolled expansion of scope that bypasses the contract system.

### Policy Changes Bundled With Unrelated Work

Governance changes must be standalone. Bundling them with feature work, refactoring, or other non-governance changes obscures the governance change and makes it harder to review, approve, and trace.

### Silent Drift

Introducing documentation inconsistencies without reporting them. If you notice drift between canonical sources and derived content, report it — even if you did not cause it. Silent drift is how governance erodes.

## Final Statement

Disciplined delivery is not slow delivery. It is delivery that can be trusted, reviewed, and traced. Speed without governance produces debt that compounds. Govern first, deliver confidently.
