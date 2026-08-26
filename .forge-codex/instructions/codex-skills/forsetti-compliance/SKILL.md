---
name: forsetti-windows-compliance
description: Enforce Forsetti Windows 0.2.0 and Agentic Edition governance in the consumer app.
---

- Use public Forsetti headers only.
- Do not modify or subclass sealed internals.
- Validate manifest schema/template 1.1 and Windows profile 0.2.0.
- Declare every capability and runtime requirement before use.
- Keep one-way dependencies and module isolation.
- Use a governing task contract and authorized scope.
- Update documentation and changelog with meaningful changes.
- Builders do not approve their own work; use a fresh Validator session.
- Run the supplied Forsetti guardrails and preserve output.
