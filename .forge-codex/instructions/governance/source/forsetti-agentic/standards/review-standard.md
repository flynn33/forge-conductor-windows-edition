# Review Standard

This standard defines review objectives, requirements, and conduct for the Forsetti Agentic Edition. Reviews are governance checkpoints, not editorial sessions. These rules are binding.

## Review Purpose

Reviews exist to verify that work complies with governance policy. Reviews are not creative editing sessions, style preference discussions, or architectural debates.

The reviewer's job is to check compliance with defined rules. If a rule does not exist for a concern, the concern is not a review finding — it is a candidate for a new rule, to be proposed through the proper governance change process.

## Review Objectives

Every review must evaluate the following objectives:

### 1. Scope Compliance

Did the work stay within the boundaries defined by the task contract? Were any files modified that are outside the contract scope? If scope was exceeded, was the contract amended before the work was performed?

### 2. Role Compliance

Was the acting role authorized for the changes made? Did the contributor operate within their role's authority boundaries? Were protected assets modified only by roles with appropriate authority?

### 3. Documentation Compliance

Were required documentation updates performed as defined by DOCUMENTATION_POLICY? If documentation was not updated, is there a valid justification (e.g., the change does not affect any documented behavior)?

### 4. Release Compliance

Was version impact classified according to the Versioning Standard? Was a changelog entry added according to the Changelog Standard? Is the classification accurate given the nature of the change?

### 5. Truthfulness Compliance

Does the completion summary match observable evidence? Do claimed validation results match actual results? Are known issues disclosed? Is the completion status accurate (complete vs. partial)?

### 6. Protected Asset Handling

Were protected governance paths (constitution, compliance rules, policies) handled with correct authority? Were changes to protected assets performed through governance-class approval?

## Reviewer Checklist

At minimum, every review must answer the following questions:

- Is there a governing task contract for this work?
- Is the acting role appropriate for the change class?
- Did changed files stay within the contract scope?
- Were documentation updates performed as required by DOCUMENTATION_POLICY?
- Was a changelog entry added for meaningful changes?
- Was release impact classified correctly?
- Does the completion summary match the evidence (files changed, validation results)?
- Were protected assets modified with proper authority?
- Are there any known issues that were not disclosed?

If any question cannot be answered from the delivery artifacts alone, the delivery is not in a reviewable state.

## Review Outcomes

### Approve (Pass)

All review objectives satisfied. Evidence confirms compliance. No unresolved issues. The work may proceed to merge.

### Request Changes

One or more objectives partially met. Issues are fixable without fundamental rework. The reviewer must document specific remediation actions — what must change, why, and which policy is not yet satisfied.

Vague change requests are prohibited. "Fix the docs" is not a valid change request. "Update CHANGELOG.md to include a migration_note field as required by the Changelog Standard for breaking changes" is a valid change request.

### Block

Fundamental violation detected. Work must be substantially revised or re-scoped. Blocking conditions include:
- False completion claim (claimed done when validation was not run or failed)
- Unauthorized scope expansion (files modified outside contract scope without amendment)
- Protected asset violation without authority (governance documents modified without governance-class approval)
- Hidden failure (known issues not disclosed in completion summary)

A block must cite the specific violation and the specific policy that was violated.

## Review Conduct

### Evidence-Based

Reviews must be evidence-based, not opinion-based. Every finding must reference observable evidence (a file change, a missing field, a policy clause). Findings without evidence are not valid findings.

### Policy-Referenced

Reviewers must cite specific policy references for any requested change. "This violates the Changelog Standard, section 'Required Fields' — the `version_impact` field is missing" is a valid finding. "I don't think this changelog entry is good enough" is not a valid finding.

### No Preference-Based Findings

"I would have done it differently" is not a valid review comment unless it identifies a specific policy violation. Style preferences, alternative approaches, and architectural opinions are not review findings unless they correspond to a defined rule.

### Documented

Review findings must be documented in PR comments, a compliance report, or another traceable record. Verbal-only reviews are not compliant reviews.

## Governing Principle

Reviews are governance verification, not gatekeeping. A review that blocks work without citing a policy violation is an abuse of the review process. A review that approves work despite known violations is a compliance failure. The standard is objective compliance, verified through evidence.

## Forsetti Enforcement Review

Reviews of Forsetti app/module work must verify:

- the task contract includes Forsetti project context;
- the selected edition profile matches target platform and framework version;
- manifest schema/template `1.1` is valid where a manifest is in scope;
- capability-using behavior maps to declared capabilities;
- runtime requirements cover I/O, UI, and data isolation;
- modules do not directly couple to other module implementations;
- consumer code uses public Forsetti APIs only;
- completion evidence maps to required profile verification commands.
