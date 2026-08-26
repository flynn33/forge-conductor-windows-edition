# Forsetti Agentic Edition

**Governance Framework for Coding Agents Building Forsetti-Compliant Apps and Modules**

**Product version:** `1.1.0`  
**Active Apple profile:** `0.1.5` (compatibility profile `0.1.3` remains explicitly selectable)  
**Windows profile:** `0.2.0`

---

## Purpose

Forsetti Agentic Edition is a governance-only enforcement framework for coding agents that build Forsetti-compliant applications and modules.

It is not a runtime SDK, platform implementation, orchestration server, CLU dependency, MCP dependency, hosted enforcement service, or application framework. It is a **governance layer** that enforces disciplined delivery and Forsetti Framework architectural compliance through:

- Constitutional governance
- Role-based authority boundaries
- Contract-driven execution
- Required Forsetti project context
- Edition/version profile selection
- Manifest, capability, runtime requirement, dependency, module-isolation, and public API enforcement
- Compliance validation with required evidence
- Release traceability
- Documentation integrity and synchronization

## Core Problem

Agentic development, without governance, produces predictable failures:

- **Uncontrolled scope expansion** — agents change more than requested
- **Undocumented changes** — modifications without documentation updates
- **Architectural drift** — structural integrity erodes across changes
- **False completion claims** — "done" without validation
- **Weak validation discipline** — tests and guardrails skipped or ignored
- **Stale documentation** — READMEs and wikis diverge from reality
- **Poor release traceability** — changes merged without version classification or changelog

Forsetti Agentic Edition exists to prevent these failures through explicit governance and to stop coding agents from weakening Forsetti sealed-runtime, public API, manifest, and module-boundary invariants.

---

## What It Governs

- Agent roles and authority boundaries
- Task contracts and scope binding
- Forsetti edition, version, target platform, and profile selection
- Shared Forsetti sealed-runtime invariants
- Forsetti module manifests and runtime requirements
- Capability declarations and capability-using behavior
- Module isolation, dependency direction, and public API boundaries
- Repository change control and approval workflows
- Compliance evidence and validation requirements
- No-attribution accountability
- Versioning discipline and release integrity
- Changelog requirements and release traceability
- Documentation synchronization and drift prevention
- Portable core boundaries
- Optional host adapters
- Platform overlays

## What It Does Not Govern

- Application runtime behavior
- UI design systems
- Language-specific implementation details
- Business-domain logic
- Deployment platform internals
- Apple runtime behavior, SwiftUI composition, Keychain, StoreKit, or platform services
- Windows runtime behavior, WinUI composition, DPAPI, WinHTTP, or platform services

These concerns belong to downstream repositories and their own governance models.

---

## Portable Architecture

Forsetti Agentic Edition is organized around a portable governance core, optional adapters, platform overlays, a trusted source bundle, and native host product surfaces.

| Layer | Path | Purpose |
|---|---|---|
| Portable core | `core/` | Host-neutral governance doctrine, role boundaries, contract concepts, Forsetti project context requirements, evidence requirements, canonical policy registries, schemas, and validation interfaces. |
| Adapters | `adapters/` | Optional host integrations that translate local or hosted platform context into portable validation inputs. |
| Overlays | `overlays/` | Host-neutral and platform-specific execution guidance that preserves core governance meaning while documenting local expectations. |
| Edition profiles | `editions/` | Binding Apple, Windows, and shared Forsetti invariant profiles used by contracts, validator modes, overlays, and completion evidence. |
| Source bundle | `bundle/` | Versioned portable schemas, policies, edition profiles, target instructions, and `product-manifest.json` hash inventory for downstream installation and verification. |
| Native products | `products/` | Swift and C++ host command surfaces that verify the trusted bundle and expose governed repository bootstrap or inspection workflows where implemented. |
| Product evidence | `.forsetti/product-completion/` | Phase 00 through Phase 05 implementation evidence for the current native product and bundle completion pass. |

The portable core must not depend on adapters, overlays, native product hosts, hosted workflow runners, IDEs, local MCP servers, container runtimes, or provider-specific tooling. Those tools may support evidence collection in a governed task, but they are not core product dependencies.

Overlays provide usable guidance profiles: `overlays/generic/` for host-neutral work, `overlays/forsetti-apple/` for Apple-platform alignment, and `overlays/forsetti-windows/` for Windows-native execution evidence. Overlays remain subordinate documentation surfaces. They do not define canonical compliance rules, change role authority, or make platform tools portable core dependencies.

GitHub Actions support belongs in `adapters/github-actions/` as an optional adapter surface. It does not define canonical compliance rules. Workflow files under `.github/workflows/` are thin hosted wrappers that preserve GitHub check names and delegate implementation to adapter-owned scripts under `adapters/github-actions/workflows/`.

`bundle/product-manifest.json` currently declares product version `1.1.0`, schema version `2.0`, source platform `source`, portable architecture, bundle ID `ffae-1.1.0-source`, and 47 required hashed bundle entries. `scripts/generate-product-manifest.py` regenerates that manifest deterministically from the bundle tree. The repository `VERSION` file, source bundle `VERSION`, and native `version` commands report the same product version.

The Apple native product in `products/apple/` builds the `forsetti-governance` Swift executable and the `GovernanceContracts`, `GovernanceCore`, and `GovernanceApple` libraries. Its implemented commands are `version`, `bundle verify`, `init`, `doctor`, and `discover`.

The Windows native product in `products/windows/` builds the `forsetti-governance` C++20 executable and `forsetti_governance_core` library. Its implemented commands are `version` and `bundle verify`; Apple-only bootstrap and discovery parity is not claimed by this repository state.

---

## Current Apple Framework Alignment

The active Apple governance profile is `editions/apple/forsetti-apple-0.1.5.profile.json`. It is source-pinned to the supplied Forsetti Framework for Mac and iOS `0.1.5` contract and is the default selected by project-context templates and the Apple native bootstrap surface. The prior `0.1.3` profile remains available only when a governed project explicitly selects that version. Unknown Apple framework versions fail closed rather than falling back silently.

The current Apple profile binds:

- Swift tools `5.10`, iOS `17.0`, and macOS `14.0`;
- public products `ForsettiCore`, `ForsettiPlatform`, and `ForsettiHostTemplate`;
- internal verification target `ForsettiModulesExample`, which is not a public consumer product;
- manifest schema/template `1.1`, object-encoded semantic versions, reverse-DNS module identifiers, Swift type-path entry points, and explicit runtime requirements;
- exact capability, I/O provider, service, UI contribution, default-role, activation, dependency, and verification contracts;
- manifest `1.0` safe legacy defaults alongside the current `1.1` contract;
- the sealed public-contract boundary, strict object-oriented implementation rules, and deployment patterns A through D;
- the enforced `Combine` prohibition in `ForsettiCore`; and
- source-contract SHA-256 hashes for 29 load-bearing framework files, including the supplied upstream validation report.

`crypto_utilities` is an Apple capability but not an I/O provider kind. Apple `0.1.5` also defines no independent `event_publishing` capability; framework-bound event publication derives source identity from the scoped module context. Windows retains its existing edition-specific `event_publishing` mapping.

---

## Document Hierarchy

Authority flows downward. Higher-ranked documents override lower-ranked documents in case of conflict.

| Rank | Document(s) | Authority |
|------|-------------|-----------|
| 1 | Human owner instruction | Highest task authority. |
| 2 | Selected Forsetti edition/version profile | Binding profile for platform, version, manifest, dependency, capability, and verification rules. |
| 3 | Shared Forsetti sealed-runtime invariants | Binding cross-edition invariants. |
| 4 | `FORSETTI_CONSTITUTION.md` | Foundational governance doctrine. |
| 5 | Policy documents | Binding governance policies. |
| 6 | `core/policies/*.json`, `policies/*.json` | Machine-readable policy manifests. `core/policies/` contains canonical portable registries where present; matching root `policies/` files are compatibility mirrors unless a higher-authority policy says otherwise. |
| 7 | `agents/*.md`, `standards/*.md`, `contracts/*.md` | Role-specific instructions, operational standards, and task contract templates. |
| 8 | Issue, PR, local instructions, and `wiki/*.md` | Local instructions and derived summary content. Not canonical. |

Portable documents under `core/`, `adapters/`, and `overlays/` are subordinate documentation surfaces introduced for portability, except for canonical portable policy registries explicitly designated under `core/policies/`. They do not amend the constitutional authority hierarchy.

### Local Validation

The portable core includes a local validator at `core/validator/forsetti_validate.ps1`. Repository scripts under `scripts/` delegate to that validator so local checks and future optional adapters use the same repository-local validation entry point.

The validator supports repository checks and target Forsetti app/module checks with these modes:

```text
repo
contract
project-context
edition-profile
manifest
dependencies
capabilities
module-isolation
evidence
all
```

Contract mode requires Forsetti project context. Target-repository modes inspect the selected edition profile, manifest schema/template version, platform support, runtime requirements, declared capabilities, changed-file evidence, dependency direction, module isolation, and public API boundary.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\core\validator\forsetti_validate.ps1 `
  -RepoRoot . `
  -Mode contract `
  -ContractPath .\contracts\TASK-CONTRACT.md `
  -ProjectContextPath .\project-context.json `
  -EditionProfilePath .\editions\apple\forsetti-apple-0.1.5.profile.json `
  -ChangedFilesPath .\changed-files.txt `
  -Strict
```

Validator result findings include the compliance rule identifier and, when a machine-readable policy gate supplies one, the policy rule identifier, condition identifier, and gate identifier used to reach the finding.

### GitHub Actions Adapter

The optional GitHub Actions adapter translates pull request, push, and manual workflow event context into repository-local validation inputs. Adapter scripts compute changed-file evidence with local `git diff`, read canonical policy manifests such as `core/policies/repo-boundaries.json`, and invoke the local validator where applicable.

Hosted workflow execution is convenience automation. Required compliance evidence still comes from task contracts, local validator output, changelog and documentation updates, and reviewer decisions.

### Policy Documents (Rank 2)

- `COMPLIANCE_POLICY.md` — Canonical compliance registry, evidence requirements, and compliance outcomes
- `CHANGE_CONTROL_POLICY.md` — Change classification, approval workflows, scope control
- `RELEASE_POLICY.md` — Versioning discipline, changelog requirements, release readiness
- `DOCUMENTATION_POLICY.md` — Canonical sources, wiki role, synchronization rules

`ACCOUNTABILITY_POLICY.md` is a compliance support policy under `COMPLIANCE_POLICY.md`. It defines FAE-C012 accountability and non-attribution requirements.

---

## Role Model

Forsetti Agentic Edition defines five governed roles. Each role has explicit authority and explicit boundaries. No role may exceed its authority. Role separation ensures independent verification and prevents conflicts of interest.

### Architect

Plans tasks, creates contracts, classifies changes, identifies risks, and defines scope. The Architect determines what work should be done and how it should be bounded. The Architect does not execute implementation work.

### Builder

Executes work within contract scope. Updates documentation as required by the documentation policy. Reports blockers and scope questions to the Architect. The Builder does not approve its own work, classify releases, or modify governance documents without governance-class approval.

### Validator

Reviews scope compliance, truthfulness of completion claims, documentation accuracy, and release classification correctness. The Validator provides independent verification. The Validator does not modify production artifacts or execute implementation work during validation.

### Release Manager

Confirms version classification correctness, changelog integrity, and release readiness. The Release Manager gates releases on evidence — correct semantic version, complete changelog, passing validation. The Release Manager does not modify implementation artifacts.

### Documentation Manager

Reviews README integrity, wiki synchronization, glossary consistency, and documentation drift. The Documentation Manager enforces the documentation policy and verifies that derived surfaces remain aligned with canonical sources. The Documentation Manager has authority to block PRs for documentation violations.

---

## Default Posture

This framework operates with a **strict default posture**.

- All governance rules are enforced unless an explicit policy exception exists.
- Convenience does not override compliance.
- Shortcuts that weaken traceability, documentation integrity, or validation discipline are prohibited.
- "We'll fix it later" is not an acceptable justification for skipping required governance steps.
- Outside contributions are not accepted; owner and authorized agent work follow repository governance.

---

## Repository Structure

```
├── Root governance policy, constitution, delivery, release, documentation, and contribution-policy documents
├── .github/
│   ├── ISSUE_TEMPLATE/
│   └── workflows/
├── .forsetti/
│   ├── product-completion/
│   ├── remediation/
│   ├── remediation-v2/
│   └── remediation-v3/
├── adapters/
│   └── github-actions/
│       ├── README.md
│       └── workflows/
├── agents/
├── bundle/
│   ├── VERSION
│   ├── editions/
│   ├── instructions/
│   ├── policies/
│   ├── product-manifest.json
│   └── schemas/
├── changelog/
├── contracts/
├── core/
│   ├── contracts/
│   ├── enforcement/
│   ├── policies/
│   ├── schemas/
│   └── validator/
├── editions/
│   ├── apple/
│   ├── shared/
│   └── windows/
├── overlays/
│   ├── forsetti-apple/
│   ├── forsetti-windows/
│   └── generic/
├── policies/
├── products/
│   ├── apple/
│   └── windows/
├── schemas/
├── scripts/
│   ├── generate-product-manifest.py
│   ├── validate-repo.ps1
│   └── validate-repo.sh
├── standards/
├── tests/
│   └── conformance/
└── wiki/
```

---

## Quick Start for Agents

### Step 1: Understand Governance

Read `FORSETTI_CONSTITUTION.md`. This is the highest-authority document. It establishes the foundational principles that all other documents implement. Do not skip this step.

### Step 2: Understand Your Operating Rules

Read `AGENTS.md` for owner and authorized agent operating rules. Outside contributions are not accepted.
### Step 3: Understand the Relevant Policy

Read the policy document relevant to your current task:

- Making changes? Read `CHANGE_CONTROL_POLICY.md`
- Validating work? Read `COMPLIANCE_POLICY.md`
- Recording governed accountability evidence? Read `ACCOUNTABILITY_POLICY.md`
- Preparing a release? Read `RELEASE_POLICY.md`
- Updating documentation? Read `DOCUMENTATION_POLICY.md`

### Step 4: Operate Under Contract

Create or receive a task contract before beginning meaningful work. The contract defines your scope, authority, expected outputs, and validation requirements. Work performed without a contract is non-compliant.

### Step 5: Follow the Workflow

The required workflow is:

1. **Contract** — Define scope and authority before starting
2. **Scope** — Work within contract boundaries; do not expand unilaterally
3. **Execute** — Implement the contracted work
4. **Validate** — Run required validation and produce evidence
5. **Release** — Classify, changelog, and gate the release on evidence

Each step has requirements defined in the relevant policy document. Skipping steps is a compliance violation.

---

## Contributing

This project is open source under Apache License, Version 2.0. You are welcome to use, modify, and redistribute the code under that license.

Outside contributions to this repository are not accepted. Pull requests and collaboration requests will not be reviewed or merged. See [CONTRIBUTING.md](CONTRIBUTING.md).


---

## Governance Contact

This framework is maintained solely by its owner. Outside contributions, collaboration requests, and proposals intended for merge into this repository are not accepted. See [CONTRIBUTING.md](CONTRIBUTING.md).

---

*This document is a canonical source. Derived wiki page: `wiki/Overview.md`.*

## License

Copyright 2026 James Daley

This project is licensed under the Apache License, Version 2.0.
See the [LICENSE](LICENSE) file for the full terms.
