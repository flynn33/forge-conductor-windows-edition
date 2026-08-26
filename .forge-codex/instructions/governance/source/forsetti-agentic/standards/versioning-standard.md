# Versioning Standard

This standard defines how version impact is classified for all changes in the Forsetti Agentic Edition. Version impact classification is a governed decision, not guesswork. These classifications are binding.

## Version Impact Classifications

### none

No version impact. Used for metadata, whitespace, formatting, or trivial changes that do not affect the meaning or enforcement of any governance rule.

Examples:
- Fixing a typo in a comment
- Adjusting whitespace
- Reformatting a table without changing content
- Correcting punctuation that does not alter meaning

### patch

Correction of errors in existing governance content without changing policy meaning. The intent and enforcement behavior of the governance rule remains identical after the change.

Examples:
- Fixing a broken link
- Correcting a cross-reference to another document
- Fixing a schema validation error
- Correcting a workflow syntax error
- Fixing an incorrect file path reference

### minor

New governance capabilities that do not break existing consumers. All previously-compliant work remains compliant after the change.

Examples:
- Adding a new standard
- Adding a new contract template
- Adding a new label family
- Adding a new optional field to a schema
- Clarifying a policy without changing its meaning
- Adding a new wiki summary page
- Adding a new role definition that does not alter existing roles

### major

Breaking changes that require existing consumers to adapt. Previously-compliant work may no longer be compliant after the change.

Examples:
- Changing required fields in a schema
- Altering the meaning of a compliance rule
- Removing or renaming a policy file
- Changing workflow enforcement so previously-passing PRs would fail
- Altering role authority boundaries
- Removing a previously-valid change class or approval class
- Changing the task contract required fields

### governance-only

Changes to constitutional or compliance content that affect governance posture but are tracked separately from semantic versioning. These changes alter the foundational rules of the framework itself.

Examples:
- Constitutional amendments
- Compliance rule additions or modifications
- Policy doctrine changes
- Authority hierarchy changes
- Default posture changes

Governance-only changes are tracked in the changelog but are not reflected in the semantic version number. They represent changes to the governing framework itself, not to the governed outputs.

## Breaking Change Expectations

A change is breaking if consumers of this governance framework must modify their behavior, contracts, workflows, or integrations to remain compliant.

Breaking changes must:
1. Be classified as `major` or `governance-only` (if constitutional)
2. Be documented with explicit migration guidance
3. Be reviewed at sensitive or governance-class approval level
4. Include a changelog entry with `breaking_change: true` and a `migration_note`

## Batch Release Rule

When multiple changes are released together, the aggregate version impact is the highest individual impact among all included changes.

One `major` change in a batch of `patch` changes makes the release `major`. One `minor` change in a batch of `none` changes makes the release `minor`. There is no averaging, weighting, or negotiation.

## Classification Responsibility

The contributor classifies version impact at the time of delivery. The reviewer validates the classification during review. Misclassification is a review finding.

## Governing Principle

Version impact is a governed classification, not guesswork. When in doubt, classify conservatively — higher impact rather than lower. Underclassifying a breaking change is a compliance violation. Overclassifying a trivial change is cautious but acceptable.

## Forsetti Profile and Enforcement Versioning

Changes that alter selected edition profile meaning, manifest requirements, supported capabilities, dependency rules, public API boundaries, or validator decisions affect downstream consumers and must be classified according to their consumer impact. Breaking governance enforcement changes require explicit migration guidance.
