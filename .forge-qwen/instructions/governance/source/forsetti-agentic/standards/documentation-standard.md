# Documentation Standard

This standard defines documentation quality rules for the Forsetti Agentic Edition. Documentation is governance infrastructure. It is not optional, decorative, or deferrable. These rules are binding.

## Canonical Source Rule

Repository markdown files (`*.md` at repo root and in defined directories) are the canonical documentation source. If a conflict exists between repository markdown and any derived surface (wiki, external documentation, cached versions, generated outputs), the repository markdown is authoritative.

No derived surface may override, extend, or contradict the canonical source. When a conflict is detected, the derived surface must be corrected to match the canonical source.

## Wiki as Derived Content

Wiki pages (`wiki/*.md`) are navigational summaries derived from canonical sources. They serve as orientation aids, not as primary authority.

Wiki pages must:
- Link back to the canonical document they summarize
- Provide navigation and orientation, not new policy
- Summarize rather than duplicate — do not copy canonical content verbatim

Wiki pages must not:
- Introduce new policy, rules, or definitions not present in the canonical source
- Contradict the canonical source
- Replace reading the canonical source for compliance purposes

A wiki page that introduces rules not found in any canonical source is a compliance violation.

## Documentation Quality Requirements

### Explicit

State rules and requirements directly. Do not imply, suggest, or hint. A reader should not need to infer what is required. If a behavior is mandatory, say "must." If a behavior is prohibited, say "must not." If a behavior is optional, say "may."

### Structured

Use headers, lists, tables, and clear formatting. Avoid wall-of-text paragraphs. Structure aids comprehension and enables machine parsing. Every document should be navigable by heading scan.

### Accurate

All references to files, policies, roles, and rules must be correct and current. A broken reference is a documentation defect. A reference to a renamed or removed file is a compliance issue that must be corrected.

### Consistent

Terminology must match the glossary. Role names must match the naming standard. File references must use correct paths. If a concept has a defined term, use that term — do not introduce synonyms.

### Complete

All required sections defined by the MASTER_REPOSITORY_SPEC must be present. Missing sections are a compliance issue. A document that omits a required section is incomplete regardless of the quality of the sections that are present.

## README Quality

`README.md` is the entry point for all readers — human and agent. It must be high-signal:
- Accurate overview of the repository's purpose and identity
- Clear navigation to key documents
- Role model summary
- Default posture statement
- Current status and version

The README must not be decorative, promotional, or aspirational. It must describe what the repository is and how it operates, not what it hopes to become.

A README that requires additional context to understand the repository is below standard.

## Policy Document Quality

Policy documents must define behavior and rejection conditions. A policy must be operationally specific — specific enough that an agent can follow it without interpretation, and specific enough that a reviewer can verify compliance against it.

A policy that says "do good things" without defining what that means operationally is below standard. A policy that says "changes should be reviewed" without defining what the review must check is below standard.

Policies must define:
- What is required
- What is prohibited
- What triggers the policy
- What constitutes a violation
- What happens when a violation is detected

## Interpretation Debt

Documentation that creates interpretation debt is below standard.

Interpretation debt occurs when a reader must make assumptions, infer meaning, or resolve ambiguity to understand what a document requires. Interpretation debt compounds across documents — ambiguity in one document propagates to every document that references it.

Reduce interpretation debt by:
- Being explicit about requirements and prohibitions
- Using defined terms consistently
- Providing examples for non-obvious rules
- Stating edge cases and exceptions directly
- Avoiding qualifiers that introduce ambiguity ("generally," "typically," "where appropriate")

## Governing Principle

Documentation is not a secondary artifact. It is the primary interface through which governance is communicated and enforced. Below-standard documentation produces below-standard compliance. Documentation quality is governance quality.

## Forsetti Enforcement Documentation

Documentation that describes Forsetti work must identify the selected edition profile, manifest schema/template version, supported platforms, module type, capability requirements, runtime requirements, dependency direction, module isolation, public API boundary, and validation evidence expectations.

Derived wiki pages must not contradict `editions/`, `core/policies/`, `core/schemas/`, `contracts/task-contract-template.md`, or `core/validator/README.md`.
