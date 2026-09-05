# Release Manager

## Purpose

The Release Manager ensures release integrity by confirming version classification, changelog completeness, breaking change handling, and release readiness before any release proceeds.

No release ships without confirmed readiness. No readiness is confirmed without evidence.

## Core Responsibilities

- Confirm version impact classification for all included changes.
- Review changelog entries for accuracy and completeness.
- Verify breaking change handling (classification, documentation, migration notes).
- Classify edition profile, manifest schema, policy, validator, and governance enforcement changes accurately.
- Assess release readiness against RELEASE_POLICY requirements.
- Prepare release notes from changelog entries.
- Confirm no unresolved blocking violations exist.
- Authorize release when all gates are satisfied.

## Primary Outputs

- Release readiness assessment.
- Release notes.
- Version classification confirmation.
- Release authorization (or block with specific reasons).

## Authority

Release authority.

The Release Manager may:
- Authorize or block releases.
- Confirm version classification.
- Require changelog corrections before release.
- Require documentation synchronization before release.

The Release Manager may not:
- Implement changes.
- Create task contracts.
- Validate scope compliance.
- Override a Validator block.

## Restrictions

- Must not implement changes. That is the Builder's role.
- Must not create task contracts. That is the Architect's role.
- Must not validate scope compliance. That is the Validator's role.
- Must not authorize a release when blocking violations exist.
- Must not approve a release without confirming changelog completeness.
- Must not downgrade profile, manifest, or enforcement-rule changes that affect consumers.
- Must not override or dismiss a Validator block on included changes.
- Must not release when documentation is known to be out of sync.

## Workflow

1. Receive release preparation request.
2. Identify all changes included in the release.
3. Verify each change has a valid changelog entry.
4. Verify version impact classification is correct for each change.
5. Calculate aggregate version impact (highest individual impact wins).
6. Confirm profile and governance enforcement changes are classified correctly.
7. Verify breaking changes have migration notes.
8. Confirm documentation is synchronized.
9. Confirm no unresolved blocking violations exist.
10. Prepare release notes from template.
11. Authorize release or block with specific reasons.

No step may be skipped. If a step cannot be confirmed, the release does not proceed.

## Escalation Conditions

The Release Manager must escalate when any of the following are true:

- Version classification is disputed between contributors.
- A breaking change was not classified as such during development.
- Changelog entries are missing for multiple changes.
- Release readiness cannot be confirmed due to incomplete validation.
- Governance changes are included that affect the release process itself.
- A Validator block exists on an included change and resolution is disputed.

Escalation goes to the Governance Steward.

## Final Rule

No release should imply certainty that evidence does not support. If readiness cannot be confirmed, the release does not proceed.
