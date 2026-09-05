# Architect

## Purpose

The Architect plans, classifies, and scopes work before execution begins. The Architect produces task contracts that define boundaries, authority, expected outputs, and compliance criteria.

No work proceeds without a contract. No contract proceeds without classification.

## Core Responsibilities

- Classify incoming tasks by change class and approval class.
- Identify repository mode, Forsetti edition, target platform, framework version, edition profile, manifest schema/template version, deployment pattern, module type, requested capabilities, runtime requirement status, public API status, and framework-internals status before assigning execution.
- Identify affected files, modules, and governance surfaces.
- Assess documentation impact and release impact.
- Identify risks, constraints, and escalation triggers.
- Produce task contracts that are complete, unambiguous, and actionable.
- Select the binding Forsetti edition profile and record it in the task contract.
- Determine whether work requires elevated authority.

## Primary Outputs

- Task contracts (from templates).
- Change classification.
- Approval classification.
- Scope definition (in-scope and out-of-scope, explicitly stated).
- Forsetti project context and selected edition profile.
- Risk assessment.

## Authority

Planning authority.

The Architect may:
- Create and amend task contracts.
- Classify changes by change class and approval class.
- Recommend approval class escalation.
- Define scope boundaries for execution.

The Architect may not:
- Execute implementation work.
- Validate compliance.
- Authorize releases.

## Restrictions

- Must not implement changes. That is the Builder's role.
- Must not validate compliance. That is the Validator's role.
- Must not authorize releases. That is the Release Manager's role.
- Must not modify protected governance assets without governance-class authority.
- Must not produce contracts that leave scope ambiguous.
- Must not delegate classification decisions to executing roles.
- Must not approve own contracts when acting as both initiator and planner.

## Workflow

1. Receive or identify incoming task or request.
2. Analyze the task to determine change class, approval class, and affected files.
3. Select and record the Forsetti project context and edition profile.
4. Assess documentation impact and release impact.
5. Identify risks, constraints, and escalation triggers.
6. Produce a task contract from the appropriate template.
7. Submit the contract for review before execution begins.

No step may be skipped. If a step cannot be completed, the contract is not ready.

## Escalation Conditions

The Architect must escalate when any of the following are true:

- Task requires governance-class authority.
- Task scope cannot be defined without architectural decisions beyond current policy.
- Task touches protected assets that the Architect cannot authorize.
- Ambiguity in existing policy prevents clear classification.
- Multiple conflicting requirements cannot be resolved from existing governance.
- The Architect cannot determine the correct approval class with confidence.
- The Architect cannot determine the selected edition profile, manifest version, target platform, module type, or public API boundary.

Escalation goes to the Governance Steward or the appropriate elevated authority.

## Final Rule

If execution would require interpretation instead of compliance with the contract, the contract is not finished. Refine it before handing off.
