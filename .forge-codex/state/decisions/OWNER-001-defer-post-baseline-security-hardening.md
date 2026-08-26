# OWNER-001: Defer Post-Baseline Security Hardening for the Internal Release

Status: Accepted owner override

Date: 2026-08-26

## Direct owner requirement

The human owner authorized deferring security hardening because this application
will be deployed on an internal, secured network and the Forsetti framework is
the required framework security boundary.

Under the governing instruction precedence, this direct owner requirement takes
priority over the standalone P22/G22 hardening and qualification work in the
port task contract and phase plan.

## Resolution

The first internal shippable release defers these P22 activities:

- the dedicated fuzz campaign;
- the exhaustive negative and fault-injection campaign beyond tests needed for
  correct feature behavior;
- the endpoint-protection and adversarial deployment review;
- defense-in-depth hardening that is not required to make an implemented
  boundary function correctly on the authorized internal deployment.

The following baseline controls remain part of implementation because they are
application-owned correctness and containment boundaries that Forsetti cannot
provide on behalf of Forge-owned code:

- manager and dashboard endpoints remain loopback-only;
- named-pipe and loopback clients remain restricted to the current user;
- filesystem authority remains rooted, canonicalized, and resistant to
  reparse-point escape;
- shell execution remains disabled by default and, when enabled, remains
  deadline-, output-, and Job-Object-bounded;
- secrets remain DPAPI-protected and excluded from logs and durable memory;
- imported databases, archives, JSON, and handoffs retain bounded validation;
- Windows Defender, SmartScreen, UAC, firewall, and execution policy are not
  weakened;
- cancellation, deadlines, bounded collections, typed errors, and RAII resource
  ownership remain mandatory.

P24 may consume the owner waiver instead of a passed G22 for the internal,
development-signed package. P25 through P29 continue to test, profile, validate,
and document the non-deferred product scope. Release documentation must identify
the internal-network assumption and the deferred hardening work.

## Completion accounting

G22 is not to be marked passed without executing its original acceptance work.
The original G30 full-hardening completion claim therefore remains unavailable
until the deferred phase is resumed and passed. An internal shippable artifact
may be delivered earlier when all non-deferred implementation, parity,
installation, reliability, and validation requirements pass; it must be labeled
as the owner-approved internal release with post-baseline hardening deferred.

## Residual risk

Forsetti protects its own framework contracts but does not automatically secure
Forge-owned filesystem, process, IPC, import, persistence, or MCP behavior. The
retained baseline controls reduce that exposure. The internal-network assumption
and the unexecuted P22 qualification remain explicit accepted residual risks.
