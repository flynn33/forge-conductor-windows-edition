# Documentation Policy

**Authority**: Governance policy document. Subordinate to `FORSETTI_CONSTITUTION.md`. Binding on all contributors — human and agent.

**Effective**: From first merged commit to `main`.

**Scope**: All documentation surfaces within the repository and its derived wiki.

---

## 1. Documentation Doctrine

Documentation is part of delivery. It is not a follow-up task, a nice-to-have, or a separate workstream.

A change that materially affects understanding, usage, operation, governance, or release history is **incomplete** if documentation is not updated as required by this policy. Incomplete work must not be marked as done. PRs with outstanding documentation obligations must not be approved without either fulfilling those obligations or explicitly tracking them with the required label.

Documentation is a first-class deliverable. It is subject to the same review, approval, and compliance standards as code and governance artifacts.

---

## 2. Canonical Source Rule

Repository markdown files are the **canonical documentation source**. Every governance document, policy, standard, agent instruction file, and README that resides in the repository is authoritative.

If a conflict exists between repository markdown and any other surface — wiki, external documentation site, cached version, screenshot, conversation log, or third-party reference — the **repository markdown is authoritative**. The conflicting surface is wrong until corrected.

No external surface may override, amend, or extend a canonical source. External surfaces may only summarize, link to, or derive from canonical sources.

### 2.1 Canonical Source Inventory

The following files are canonical sources:

| File | Scope |
|---|---|
| `FORSETTI_CONSTITUTION.md` | Foundational governance principles and doctrine |
| `AGENTS.md` | Agent operating rules, role model, workflow |
| `COMPLIANCE_POLICY.md` | Compliance model, evidence requirements, blocking violations |
| `ACCOUNTABILITY_POLICY.md` | Accountability and non-attribution requirements |
| `CHANGE_CONTROL_POLICY.md` | Change classification, approval workflows, scope control |
| `RELEASE_POLICY.md` | Versioning discipline, changelog requirements, release readiness |
| `DOCUMENTATION_POLICY.md` | This document — documentation standards and sync rules |
| `README.md` | Repository overview, structure, quick start |
| `VISION.md` | Mission, problem statement, strategic goals |
| `CONTRIBUTING.md` | Contributor guide |
| `CODE_OF_DELIVERY.md` | Delivery doctrine |
| `agents/*.md` | Role-specific agent instructions |
| `contracts/*.md` | Task contract templates |
| `standards/*.md` | Operational standards |

Adding a new canonical source requires a governance-class PR with Architect and Validator approval.

---

## 3. Wiki Role

The wiki (`wiki/*.md`) is a **derived publishing surface**. It exists to provide navigational summaries, cross-references, and entry points for contributors who need orientation.

### 3.1 Wiki Constraints

- Wiki pages **summarize and link to** canonical sources. They do not replace them.
- Wiki content **must never be treated as canonical**. If a wiki page contradicts a canonical source, the canonical source is correct.
- Wiki pages **must not duplicate canonical content verbatim**. They provide navigational summaries, context, and cross-references.
- Wiki pages **must include a canonical source reference** — a clear link or path to the canonical document they derive from.
- Wiki pages **must not introduce governance rules, policy statements, or binding requirements** that do not exist in a canonical source.

### 3.2 Wiki Page Format

Every wiki page must begin with:

```markdown
# [Page Title]

> **Canonical source**: `[path/to/canonical/source.md]`
> **Last synced**: [date or PR reference]
```

This header establishes traceability and makes drift detection possible.

---

## 4. Synchronization Rule

When a canonical source file is modified, the corresponding wiki-derived page **must be updated in the same PR** or a `docs:needs-sync` label **must be applied** to the PR to track the outstanding synchronization.

### 4.1 Canonical-to-Wiki Sync Pairs

| Canonical Source | Wiki Derived Page(s) |
|---|---|
| `FORSETTI_CONSTITUTION.md` | `wiki/Constitution.md` |
| `AGENTS.md` | `wiki/Agent-Roles.md`, `wiki/Workflow.md` |
| `COMPLIANCE_POLICY.md` | `wiki/Compliance.md` |
| `ACCOUNTABILITY_POLICY.md` | `wiki/Compliance.md` |
| `RELEASE_POLICY.md` | `wiki/Releases.md` |
| `CHANGE_CONTROL_POLICY.md` | `wiki/Workflow.md` |
| `DOCUMENTATION_POLICY.md` | `wiki/Documentation.md` |
| `README.md` | `wiki/Overview.md` |

When a canonical source has multiple derived wiki pages, **all** derived pages must be updated or labeled for sync.

### 4.2 Machine-Readable Sync Manifest

The canonical machine-readable sync manifest is `core/policies/docs-sync-rules.json`. The root `policies/docs-sync-rules.json` file is a byte-identical compatibility mirror.

The manifest may expand the table above into enforceable sync pairs for role files, standards, changelog records, and canonical core policy manifests. Expanded pairs must reference existing repository paths, identify the canonical source, identify the required derived wiki page, include a stable rule identifier, and define required evidence, rejection condition, and failure action.

Core policy manifests under `core/policies/` are canonical when a matching root mirror exists. Root mirror paths under `policies/` must not be treated as independent canonical sources for documentation sync unless a higher-authority policy explicitly changes that hierarchy.

### 4.3 Sync Completion

A `docs:needs-sync` label is a tracking mechanism, not a permanent exemption. Outstanding syncs must be completed within the next two PRs merged to `main` after the label was applied, or a dedicated documentation sync PR must be opened.

If a `docs:needs-sync` label remains unresolved after three PRs have been merged to `main`, it becomes a **blocking compliance violation**. No further feature or governance PRs may be merged until the sync is resolved.

### 4.4 Sync Direction

Synchronization is **one-directional**: canonical source to wiki. Changes must never flow from wiki to canonical source. If a wiki page contains information that should be canonical, that information must first be added to the appropriate canonical source, then the wiki page must be updated to derive from it.

---

## 5. Documentation Impact Review

Every PR must declare its documentation impact. This is not optional.

### 5.1 PR Template Documentation Section

The PR template must include a documentation impact section requiring explicit confirmation of:

- [ ] **README**: Does this change require a README update? (Yes / No / Not applicable)
- [ ] **Wiki**: Do wiki pages require update? (Yes / No / Not applicable)
- [ ] **Governance documents**: Are governance documents affected? (Yes / No / Not applicable)
- [ ] **Glossary**: Does this change introduce new terms requiring glossary update? (Yes / No / Not applicable)
- [ ] **Changelog**: Has the changelog been updated? (Yes / No / Not applicable — metadata only)

Leaving the documentation impact section blank or removing it from a PR is a **blocking violation**. The PR must not be approved until the section is completed.

### 5.2 Documentation Impact Assessment Responsibility

The PR author is responsible for the initial documentation impact declaration. The Validator role is responsible for verifying the declaration is accurate. If the Validator determines the declaration is incorrect — for example, a feature PR claims no README update is needed when it clearly introduces new repository-level capabilities — the Validator must request changes.

---

## 6. Required Documentation Updates by Change Type

The following table defines minimum documentation obligations by change class. These are minimums — contributors may exceed them.

| Change Class | README | Wiki | Glossary | Changelog |
|---|---|---|---|---|
| `feature` | If scope affects repository overview | Yes, if canonical source changed | If new terms introduced | Yes |
| `bugfix` | Only if README contained the error | Only if canonical source changed | Rarely | Yes |
| `refactor` | Only if repository structure changed | Only if canonical source changed | Rarely | Yes, if meaningful |
| `docs` | If README is the target | If wiki is the target | If glossary is the target | Only if meaningful |
| `governance` | Yes, if policy overview changed | Yes | If governance terms changed | Yes |
| `release` | Only if release process changed | Only if release wiki changed | Rarely | Yes (release entry) |
| `metadata` | No | No | No | No |
| `breaking-change` | Yes | Yes | If terms changed | Yes, with migration note |

### 6.1 Interpretation Rules

- "If canonical source changed" means the canonical source file was modified in the PR. If it was modified, wiki sync is required or must be labeled.
- "If new terms introduced" means the PR introduces terminology not already defined in the glossary. Judgment call belongs to the PR author; verification belongs to the Validator.
- "Yes, with migration note" for breaking changes means the changelog entry must include explicit migration guidance — what changed, what breaks, and what consumers must do.
- `metadata` class changes (CI configuration tweaks, label updates, non-functional repository settings) are exempt from documentation updates. This exemption does not extend to changes that affect contributor workflow, governance enforcement, or approval requirements.

---

## 7. Drift Definition

Documentation drift occurs when documented state diverges from actual state. Drift undermines trust in documentation and creates governance risk.

### 7.1 Drift Conditions

Drift exists when any of the following are true:

1. **Canonical-wiki divergence**: A canonical source has been modified but its wiki-derived page has not been updated and no `docs:needs-sync` label is tracking the gap.
2. **README inaccuracy**: README describes capabilities, structures, roles, workflows, or files that no longer exist or have materially changed.
3. **Glossary inconsistency**: A glossary term definition no longer matches its usage in policy documents, agent instructions, or standards.
4. **Wiki contradiction**: A wiki page contains information that contradicts its canonical source.
5. **Stale references**: Documentation references files, directories, tools, or processes that have been removed, renamed, or replaced.
6. **Orphaned documentation**: Documentation exists for features, roles, or processes that have been removed from the repository.

### 7.2 Drift Severity

- **Drift in governance documents** (constitution, policies, compliance model): blocking violation. Must be resolved before the next governance or feature PR is merged.
- **Drift in operational documents** (README, agent instructions, standards): compliance issue. Must be resolved within three PRs merged to `main` or via a dedicated documentation PR.
- **Drift in wiki pages**: tracked issue. Must be resolved per the sync completion rule in Section 4.2.

### 7.3 Drift Reporting

Drift is a compliance issue. **Unreported drift is a blocking violation.**

Any contributor — human or agent — who detects drift must report it by:
1. Opening an issue with the `docs:drift` label, or
2. Including the drift fix in their current PR, or
3. Noting the drift in their PR's documentation impact section with a reference to the affected files.

Detecting drift and not reporting it is equivalent to introducing drift. Both are violations.

---

## 8. Rejection Conditions

A PR **must be rejected or marked for changes** if any of the following conditions are true:

1. A canonical source was modified but wiki sync was not performed and no `docs:needs-sync` label was applied.
2. README was not updated when the change materially affects the repository overview, structure, or capabilities.
3. A breaking change was made without updating all affected documentation surfaces.
4. Documentation drift was introduced and not reported.
5. The documentation impact section of the PR template was left blank, removed, or obviously incorrect.
6. A governance document was modified without following the governance change approval class.
7. New terminology was introduced without a glossary entry when the term is used in governance or policy context.
8. A wiki page was modified to contain information not derived from a canonical source.

### 8.1 Rejection Authority

The Validator role and the Documentation Manager role both have authority to reject PRs for documentation violations. Either role may independently block a PR that fails documentation review.

### 8.2 Rejection Override

Documentation rejection may only be overridden by:
- Fixing the documentation violation, or
- Demonstrating that the rejection condition does not actually apply (with specific justification), or
- An explicit governance exception approved through the governance change process defined in `CHANGE_CONTROL_POLICY.md`.

Convenience, urgency, and "we'll fix it later" are not valid override justifications.

---

## 9. Documentation Quality Standards

### 9.1 Clarity

Documentation must be written in clear, direct language. Avoid jargon unless the term is defined in the glossary. Prefer short sentences. Prefer active voice. State requirements as requirements, not suggestions.

### 9.2 Accuracy

Documentation must accurately reflect the current state of the repository. Aspirational statements must be clearly marked as aspirational. Planned features must not be documented as existing features.

### 9.3 Completeness

Documentation must cover the scope it claims to cover. A policy document that omits material rules is incomplete. An agent instruction file that omits required workflow steps is incomplete. Incomplete documentation is a compliance issue.

### 9.4 Consistency

Terminology must be used consistently across all documentation surfaces. If the glossary defines a term, all documents must use that term with the glossary's meaning. Synonyms and informal alternatives create ambiguity and are prohibited in governance documents.

---

## 10. Changelog Documentation Requirements

Changelog entries are documentation. They are subject to this policy.

### 10.1 Changelog Entry Requirements

Every changelog entry must include:
- The change class (feature, bugfix, refactor, governance, breaking-change, etc.)
- A clear, factual summary of what changed
- The PR reference
- For breaking changes: explicit migration guidance

### 10.2 Changelog Omission

Omitting a required changelog entry is a documentation violation. The Validator and Release Manager roles must verify changelog completeness before approving a PR for merge.

---

## 11. Enforcement

This policy is enforced through:
- PR template requirements (documentation impact section)
- Validator review (documentation accuracy and completeness)
- Documentation Manager review (wiki sync, drift detection, README integrity)
- CI checks where practical (link validation, sync label verification)
- Compliance audit (drift detection across canonical sources and derived surfaces)

Violations of this policy are compliance issues subject to the enforcement mechanisms defined in `COMPLIANCE_POLICY.md`.

---

## 12. Amendment

This policy may be amended through the governance change process defined in `CHANGE_CONTROL_POLICY.md`. Amendments require Architect proposal, Validator review, and Documentation Manager review at minimum.

---

*This document is a canonical source. Derived wiki page: `wiki/Documentation.md`.*
