# Role protocol

Every role begins with exact workspace-lock verification. A role never inherits workspace authority from the prior role's conversation or memory.

- Architect: task contract, scope amendments, ADRs, architecture; no production implementation unless contract explicitly combines a tiny bootstrap slice.
- Builder: implementation and candidate evidence; cannot pass own gate.
- Validator: independent clean validation and adversarial testing; no production repair.
- Documentation Manager: verify docs against current code/tests/settings/limits/security/install/migration.
- Release Manager: version, artifacts, signatures, architecture matrix, clean install, hashes, final decision.

Every role change uses a fresh LM Studio chat and repository handoff.
