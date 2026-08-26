# RELEASE POLICY

**Governing Authority**: FORSETTI_CONSTITUTION.md
**Policy Level**: Tier 2 (Policy Document)
**Approval Class**: Governance-Class

---

## Version Impact Classes

Every change must be classified with a version impact. The version impact determines how the change affects the framework's semantic version.

### none

No version impact. The change affects only metadata, formatting, whitespace, or cosmetic presentation. It does not alter the meaning, structure, or enforcement of any governed content. No changelog entry is required.

### patch

Corrections to existing governance content that **do not change policy meaning**. This includes typo fixes, broken link repairs, schema validation fixes, and cross-reference corrections. The governance rules mean the same thing before and after — the defect in their expression has been repaired.

### minor

New governance capabilities, new standards, new templates, new schemas, or policy clarifications that **do not break existing consumers**. After a minor change, all previously-valid contracts, workflows, and integrations continue to function without modification. Minor changes are additive or clarifying.

### major

Breaking changes to governance rules, policy structure, schema contracts, or workflow enforcement that **require existing consumers to adapt**. After a major change, some previously-valid contracts, workflows, or integrations may need modification. Major changes alter the governance contract between the framework and its consumers.

### governance-only

Changes to constitutional or compliance content that affect the **governance posture** of the framework but do not map cleanly to semantic versioning. These changes affect how governance is conducted rather than what the framework provides to consumers. Governance-only changes are tracked in the changelog but do not increment the semantic version.

---

## Change-to-Version Guidance

The following table provides the typical version impact for each change class. The actual version impact must be determined by the Release Manager based on the specific change, not mechanically from this table.

| Change Class | Typical Version Impact |
|---|---|
| feature | minor |
| bugfix | patch |
| refactor | none or patch |
| docs | none |
| governance | governance-only or major |
| release | (release itself, not a version trigger) |
| metadata | none |
| breaking-change | major |

### Classification Judgment

- A `governance` change is `governance-only` when it adjusts internal governance process without affecting what consumers must do.
- A `governance` change is `major` when it changes rules that consumers must comply with.
- A `refactor` is `none` when it reorganizes content without altering meaning. It is `patch` when the reorganization fixes an ambiguity or improves enforceability.
- When in doubt, classify the version impact **higher** rather than lower. Underclassification is a greater risk than overclassification.

---

## Breaking Change Rules

### Definition

A change is **breaking** if it:

- Alters the required fields in a contract template or schema.
- Changes the meaning of an existing compliance rule.
- Modifies workflow enforcement in a way that previously-passing PRs would now fail.
- Removes or renames a policy, standard, or schema file.
- Changes the authority hierarchy.
- Alters role boundaries in a way that restricts previously-allowed actions.
- Removes a previously-available extension point or integration mechanism.
- Changes the structure of machine-readable policy manifests in a way that existing consumers cannot parse.

### Requirements for Breaking Changes

Every breaking change must satisfy all of the following:

1. **Explicit classification.** The change must be classified as `breaking-change` in the task contract and changelog. Silent reclassification is a blocking violation (FAE-C006).

2. **Elevated review.** Breaking changes require Sensitive approval at minimum. Breaking changes to governance content require Governance-Class approval.

3. **Migration guidance.** The changelog entry must include specific, actionable migration guidance explaining what consumers must change and how. "See documentation" is not sufficient — the guidance must be in the changelog entry itself.

4. **Downstream impact assessment.** The task contract must identify which downstream documents, schemas, workflows, integrations, and consumers are affected.

5. **Version impact classification.** Breaking changes are classified as `major` version impact unless the Release Manager provides documented justification for a different classification.

---

## Changelog Requirement

### When a Changelog Entry Is Required

Every meaningful change (any change class except `none` and `metadata`) must have a changelog entry. This is a compliance requirement — missing changelog entries are blocking violations (FAE-C005).

### Required Changelog Fields

Every changelog entry must include:

| Field | Description |
|---|---|
| **title** | Brief, descriptive title of the change |
| **change_class** | One of: feature, bugfix, refactor, docs, governance, release, breaking-change |
| **version_impact** | One of: none, patch, minor, major, governance-only |
| **summary** | Clear description of what changed and why |
| **affected_area** | The governance area(s) affected (e.g., compliance, contracts, schemas, workflows) |
| **task_reference** | The task contract, issue, or pull request that authorized the change |
| **approval_class** | The approval class used for the change |

### Additional Fields for Breaking Changes

Breaking change changelog entries must also include:

| Field | Description |
|---|---|
| **migration_note** | Concise actionable migration instructions |
| **migration_guidance** | Specific instructions for consumers on what to change and how |
| **affected_consumers** | Which downstream documents, schemas, or integrations are affected |

The canonical machine-readable versioning and changelog manifests are `core/policies/versioning-rules.json` and `core/policies/changelog-rules.json`, with byte-identical compatibility mirrors under `policies/`. These manifests define pre-merge gates for version consistency, breaking-change disclosure, downgrade controls, required changelog entries, and append-only changelog history.

### Changelog Standards

- The changelog is a **governance record**, not marketing copy. Entries must be factual, specific, and complete.
- Entries must describe what actually changed, not what was intended to change.
- Entries must be written at the time of the change, not retroactively compiled at release time.
- Each change gets its own entry. Multiple changes must not be collapsed into a single entry.

---

## Release Readiness Requirements

A release is ready **only when all of the following conditions are satisfied**:

1. **All included changes have been validated.** Every change in the release has passed compliance evaluation. No unvalidated changes are included.

2. **All changelog entries are complete and accurate.** Every meaningful change has a changelog entry with all required fields. Entries match the actual changes.

3. **Version classification is consistent.** The aggregate version impact is consistent across all included changes. No change is classified in a way that contradicts its actual impact.

4. **No unresolved blocking violations exist.** No blocking FAE-C001 through FAE-C012 violations are open against any included change.

5. **Documentation is synchronized.** All documentation artifacts are current and reflect the state of the release. No documentation drift exists.

6. **Breaking changes have migration notes.** If the release includes breaking changes, migration guidance is present in the changelog and (if applicable) in a dedicated migration section of the release notes.

7. **Release Manager has confirmed readiness.** The Release Manager has reviewed and explicitly confirmed that all readiness conditions are satisfied.

If any condition above is not met, the release is **not ready** and must not proceed.

---

## Release Authority

### Release Manager Role

Release preparation requires **Release Manager authority** operating under the **release-critical approval class**. The Release Manager is responsible for:

- Confirming version impact classification across all included changes.
- Verifying changelog integrity (completeness, accuracy, required fields).
- Confirming release readiness conditions are satisfied.
- Authorizing the release to proceed.

### Release Gate

No release may proceed if:

- Any compliance gate has not passed.
- Any blocking violation is unresolved.
- The Release Manager has not explicitly authorized the release.
- Changelog entries are incomplete or inaccurate.
- Documentation is not synchronized.

### Release Manager Authority Limits

The Release Manager governs release mechanics. The Release Manager does not have authority to:

- Waive compliance requirements to meet a release deadline.
- Override blocking violations.
- Modify policy content as part of release preparation.
- Reclassify breaking changes as non-breaking to simplify the release.

---

## Batch Release Handling

Multiple changes may be batched into a single release. Batch releases must follow these rules:

### Aggregate Version Impact

The aggregate version impact of a batch release is the **highest individual impact** among all included changes.

| Example | Aggregate Impact |
|---|---|
| 3 patches | patch |
| 2 patches + 1 minor | minor |
| 4 patches + 2 minors + 1 major | major |
| 5 changes + 1 governance-only | governance-only does not affect semantic version; aggregate is the highest of the other 5 |

### Individual Change Requirements

Each individual change in a batch release must have:

- Its own changelog entry with all required fields.
- Its own compliance evaluation result.
- Its own version impact classification.

Batch releases do not excuse individual changes from compliance requirements. A batch release with one non-compliant change is a non-compliant release.

### Release Notes

The release notes for a batch release must:

- Summarize all included changes.
- Highlight breaking changes prominently.
- Group changes by change class or affected area for readability.
- Reference individual changelog entries for detail.

### Batch Release Readiness

A batch release is ready only when **every individual change** satisfies the release readiness requirements and the aggregate release satisfies them as well. One ready change does not compensate for one unready change.

---

*Release integrity is the final governance gate. A release that ships without compliance is a governance failure, regardless of the quality of the code inside it.*
