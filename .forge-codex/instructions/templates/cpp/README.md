# C++ contract blueprints

These headers express required boundaries and ownership intent. Codex must adapt them into the target repository's namespaces, result type, async model, and ABI conventions while preserving:

- pure abstract interfaces;
- constructor injection;
- typed values;
- deadlines/cancellation;
- bounded ownership;
- explicit shutdown;
- no platform leakage into Domain.

They are not a substitute for source-backed tool schemas or Forsetti public contracts.
