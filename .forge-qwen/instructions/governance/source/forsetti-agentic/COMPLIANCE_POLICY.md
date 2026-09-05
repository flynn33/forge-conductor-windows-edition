# COMPLIANCE POLICY

**Governing Authority**: FORSETTI_CONSTITUTION.md
**Policy Level**: Tier 2 (Policy Document)
**Approval Class**: Governance-Class

---

## Compliance Doctrine

Compliance is determined by evidence, not by confidence, assertion, or intent.

A change is compliant only when:
- all required policy conditions for the change class and scope are satisfied, and
- all required evidence is present, current, and verifiable.

A claim of compliance without supporting evidence is itself a compliance violation. The burden of proof rests on the party claiming compliance, not on the party evaluating it.

---

## Canonical Rule Registry

The canonical machine-readable compliance registry is `core/policies/compliance-rules.json`.

The root file `policies/compliance-rules.json` is a compatibility mirror and must remain byte-for-byte identical to the core registry until the constitutional policy hierarchy is amended.

Markdown policy text, wiki summaries, workflow wrappers, adapter logic, and validator output must use the same rule identifiers, titles, decisions, and meanings as the canonical registry.

---

## Compliance Model

Every compliance evaluation produces exactly one of three outcomes:

### Pass

All required conditions are met. All required evidence is present. No unresolved issues exist. The work may proceed to the next stage.

### Request Changes

One or more conditions are unmet, incomplete, or inaccurate, but the issue can be fixed within the current scope and does not invalidate the work as a whole. Work may not proceed until the requested changes are made and re-evaluated.

### Block

A fundamental violation has been identified. Blocked work must not proceed until the blocking violation is resolved and the work is re-evaluated from the beginning of the compliance check.

---

## Compliance Rules

### FAE-C001: Task Contract Required Before Execution

Meaningful implementation work must not begin without a governing task contract that identifies the task, change class, approval class, authorized scope, required outputs, documentation impact, release impact, validation gates, and reviewer roles.

Decision: block.

### FAE-C002: Scope Boundary Enforcement

Changed files must stay within the authorized scope defined by the task contract. If additional files are required, the contract must be amended before those files are modified.

Decision: block.

### FAE-C003: Role Separation Enforcement

A role must not perform actions assigned to another role. Builders must not validate their own work. Validators must not implement fixes. Release Managers must not waive compliance gates. Role authority boundaries must match the repository role model.

Decision: block.

### FAE-C004: Protected Asset Governance Gate

Protected governance assets must not be modified without the required approval class and Governance Steward authorization when governance-class authority is required.

Decision: block.

### FAE-C005: Changelog Entry Required for Substantive Changes

Every meaningful change must include a changelog entry with the required fields, accurate change class, accurate version impact, affected area, task reference, approval class, and migration guidance when the change is breaking.

Decision: request changes.

---

## Forsetti Enforcement Rules

The canonical Forsetti-specific enforcement registry is `core/policies/forsetti-enforcement-rules.json`, mirrored at `policies/forsetti-enforcement-rules.json`.

Rules `FAE-F001` through `FAE-F020` extend the generic `FAE-C###` compliance model for Forsetti app/module work. They cover required project context, selected edition/version profile, supported target platform, manifest 1.1 validation, manifest/code identity alignment, module isolation, direct dependency prohibition, direct data sharing prohibition, declared capability use, runtime requirements, public API-only use, sealed framework internals, one-way dependency direction, UI/app active surface invariants, service module UI contribution prohibition, constructor injection, hidden globals/service-location, platform-native toolchain evidence, required verification commands, and completion evidence mapped to the selected profile.

Forsetti enforcement rules are blocking when they protect sealed-runtime, manifest, dependency, capability, public API, or module-isolation invariants. The local validator must emit rule IDs, severity, decision, message, evidence, and remediation.

### FAE-C006: Breaking Change Disclosure Mandate

A change that alters existing governance rules, required fields, default behavior, schemas, workflow enforcement, or consumer obligations must be explicitly classified as breaking-change with major version impact unless the Release Manager documents a higher-authority exception.

Decision: block.

### FAE-C007: Completion Summary Truthfulness

Completion summaries must accurately describe files changed, validation results, known issues, documentation status, release impact, and scope compliance. Claims of done, tested, reviewed, or no issues must be supported by evidence.

Decision: block.

### FAE-C008: Documentation Sync Compliance

Changes that affect documented behavior, policy, process, public interfaces, release history, or canonical documentation must update the corresponding documentation surfaces or explicitly track the sync obligation under the documentation policy.

Decision: request changes.

### FAE-C009: Version Classification Accuracy

Version impact must match the actual effect of the change. Breaking changes require major impact unless a documented higher-authority exception applies. Governance-only impact must not be used to hide consumer-facing breaking changes.

Decision: request changes.

### FAE-C010: Governance Change Isolation

Governance changes must be standalone. A governance or protected policy change must not be bundled with unrelated feature, bugfix, refactor, release, adapter, or implementation work.

Decision: block.

### FAE-C011: Evidence and Validation Integrity

Validation evidence must be real, specific, current, and traceable to commands, tools, reports, or reviewer decisions that actually ran. Failed, skipped, partial, stale, or unavailable validation must be disclosed.

Decision: block.

### FAE-C012: Accountability and Non-Attribution

Assisted work must never replace accountable human ownership or governed role accountability. `ACCOUNTABILITY_POLICY.md` prohibits attribution credit to any tool, model, vendor, automation, or agent in source files, generated code comments, commit messages, changelog entries, release notes, README notices, contributor lists, authorship metadata, and documentation prose. Governance evidence must record only the accountable human owner, acting governed role, contract ID or remediation phase ID, review evidence, validation evidence, and required approval evidence.

Decision: request changes.

---

## Evidence Requirements

### Completion Evidence

Every completion claim must reference:
- specific validation results,
- the list of files changed,
- confirmation that required documentation was updated,
- confirmation that the changelog was updated when required,
- disclosure of known issues, risks, deferred items, skipped checks, or partial completion.

### Scope Evidence

Scope compliance must be demonstrable by comparing actual changed files against the contracted in-scope file list.

### Documentation Evidence

Documentation compliance must identify which documentation artifacts were required to change, confirm those artifacts were updated, and confirm no documentation drift was introduced.

### Release Evidence

Release compliance must include version impact classification, complete changelog entries, migration guidance for breaking changes, and confirmation that required compliance gates passed.

### Governance Evidence

Governance compliance must include approval class verification for governance-touching changes, role authority confirmation, Governance Steward authorization when required, and evidence that governance changes are isolated.

### Policy Gate Evidence

When a machine-readable policy manifest defines a local rule identifier, condition identifier, or gate identifier, validation evidence must preserve those identifiers with the canonical `FAE-C###` compliance rule. Policy-local identifiers do not replace the canonical compliance registry; they make the specific manifest condition and rejection reason traceable.

---

## Compliance Categories

### 1. Scope Compliance

The work stayed within the authorized boundaries defined by the task contract.

Required evidence: changed file list compared against contract scope.

### 2. Documentation Compliance

All required documentation was updated to reflect the changes made.

Required evidence: documentation impact declaration, updated documentation artifacts, and documentation sync confirmation.

### 3. Release Compliance

Version impact was correctly classified. Changelog entries are complete and accurate. Breaking changes include migration guidance.

Required evidence: version impact classification, changelog entry, release review when required, and migration guidance when applicable.

### 4. Truthfulness Compliance

All claims in completion summaries, PR descriptions, review materials, and status reports are accurate and supported by evidence.

Required evidence: summary compared against validation results, file changes, tests, and review output.

### 5. Governance Compliance

Protected governance assets were handled with the required authority level, and governance changes were isolated from unrelated work.

Required evidence: approval class verification, Governance Steward authorization when required, protected path review, and governance isolation review.

### 6. Evidence Integrity

Evidence is current, specific, and traceable to actual checks or reviewer decisions.

Required evidence: commands run, tool outputs, report paths, reviewer decisions, failure notes, and skipped-check rationale.

---

## Truthfulness Standard

- "Done" means all required outputs were delivered, all required validations passed, and all required documentation was updated.
- "Tested" means tests or validations were actually executed and results are available for review.
- "Reviewed" means a qualified reviewer evaluated the work against applicable compliance criteria.
- "No issues" means no issues were found during an actual evaluation.
- Partial completion must be stated as partial completion.
- Failed, skipped, stale, or unavailable checks must be disclosed.

---

## Role Responsibilities

### Architect

- Ensures task contracts define clear, measurable compliance criteria.
- Defines acceptance criteria that map to specific compliance categories.
- Identifies which compliance categories are required for each task.
- Reviews sensitive and breaking changes for architectural impact.

### Builder

- Produces evidence of scope compliance during execution.
- Updates required documentation as part of delivery.
- Creates changelog entries for meaningful changes.
- Reports completion status accurately.
- Does not claim completion without required evidence.

### Validator

- Evaluates compliance across all applicable categories.
- Renders pass, request-changes, or block decisions.
- Identifies violations by canonical rule ID.
- Verifies that evidence supports the claims made.
- Does not pass work that lacks required evidence.

### Release Manager

- Confirms version impact classification.
- Verifies changelog integrity.
- Confirms breaking changes have migration guidance.
- Verifies release readiness conditions.
- Does not authorize release with unresolved blocking violations.

### Documentation Manager

- Confirms required documentation was updated.
- Identifies documentation drift.
- Verifies README integrity and wiki synchronization.
- Confirms documentation artifacts match the current state of governed content.
- Does not approve documentation compliance when drift exists.

### Governance Steward

- Authorizes governance-class changes.
- Resolves governance ambiguities.
- Confirms protected governance assets have required authority.
- Does not waive evidence, scope, documentation, or truthfulness requirements.

---

*Compliance is not a gate to pass through. It is a standard to meet. Evidence is the only currency accepted.*
