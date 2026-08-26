# OWNER-002: Alpha Release Qualification Scope

Status: Accepted owner override

Date: 2026-08-26

## Direct owner requirements

For the first alpha release, the human owner directed the project to:

- eliminate repeated full-gate rebuilds and accept a gate after its one
  authoritative rebuild succeeds;
- defer all security-hardening work until after alpha;
- qualify the installer only on the current deployment machine;
- defer clean-environment testing until after alpha; and
- defer bespoke UI polish until after alpha without removing product features;
  while retaining the complete planned UI-automation coverage for alpha.

These requirements take precedence over conflicting task-contract, phase-plan,
gate, and definition-of-done requirements for the alpha release.

## Gate execution resolution

Each remaining alpha gate receives one authoritative gate invocation. That
invocation may build the affected targets and run the gate-specific native test
executables and deterministic checks needed to establish that the implemented
slice works. If it succeeds, the gate passes for alpha. A Validator reviews the
recorded command, outputs, hashes, and scope without repeating the full rebuild.

Incremental developer builds and focused tests remain permitted while fixing a
slice, but successful work is not rebuilt merely to duplicate evidence in a
second role or full-gate pass. A source change after the authoritative invocation
invalidates only the affected gate evidence and requires one new authoritative
invocation for that changed scope.

This changes evidence repetition, not the no-feature-loss requirement. A
successful compile alone is not treated as evidence of runtime behavior when
the authoritative gate invocation includes a gate-specific executable.

## Security resolution

P22/G22 and all additional security-only review, hardening, fuzzing, adversarial
qualification, and defense-in-depth work are deferred until after alpha. This
supersedes OWNER-001 where OWNER-001 scheduled any hardening work before the
internal alpha.

Already implemented controls are not removed, and controls inseparable from
correct product behavior remain in their owning feature slices. Examples are
DPAPI-backed secret storage, loopback/current-user service scope, bounded process
execution, and rooted workspace access. Keeping those existing functional
boundaries avoids regression work; it does not constitute a separate pre-alpha
hardening campaign.

## Installer and environment resolution

Alpha installer qualification is limited to this machine, its installed Windows
11 version, architecture, SDK/runtime state, user account, and hardware. The
alpha still receives a local install, launch, upgrade/repair where applicable,
uninstall, and reinstall smoke flow on this machine. Cross-machine, virtual
machine, clean-profile, clean-OS, and alternate-hardware qualification is
deferred until after alpha.

The resulting package is qualified for this machine only and must not be
described as generally qualified for arbitrary Windows 11 systems.

## UI resolution

All seven required product surfaces, commands, state, error handling, and
feature behavior remain in alpha. Standard WinUI 3 controls and conventional
Windows layout are sufficient. Custom styling, decorative treatments,
micro-animations, pixel-level visual refinement, and other bespoke polish are
deferred until after alpha.

Functional UI automation remains in the alpha scope at the complete planned
coverage. Native C++ UI Automation tests must launch the packaged app, cover all
seven surfaces and their required workflows, verify observable state, and prove
clean window and process shutdown. Deferring bespoke visual polish does not
reduce this behavioral automation requirement.

## Completion accounting

The deliverable may be called the owner-approved, machine-qualified internal
alpha when every non-deferred feature is present and its alpha gate has passed
under this decision. It may not be represented as satisfying the original full
cross-environment, security-hardened G30 release definition until the deferred
work is resumed and passed.

## Accepted residual risks

- defects observable only on another Windows machine, user profile, or clean OS;
- security defects that dedicated post-alpha hardening would have found;
- reduced independent reproduction because validation reuses authoritative gate
  evidence rather than rebuilding it;
- functional but less refined visual presentation; and
- installer dependencies that happen to be preinstalled on this machine.
