# Windows roadmap
**Immediate objective: working internal Windows 11 x64 Alpha, full native GUI and installer.**
This roadmap replaces the old sequential port-gate program for this delivery. Existing useful work remains.
The machine-readable sequence is [docs/alpha-plan.json](docs/alpha-plan.json).
No calendar due dates are invented. Milestones close on delivered behavior, not an estimated percentage.

| Phase | Milestone | Deliverable |
| [P0](docs/implementation/P0.md) | A0 — Reproducible foundation | One governing Alpha scope and reproducible x64 backend build. |
| [P1](docs/implementation/P1.md) | A1 — Native desktop checkpoint | A real WinUI app attaches to the manager and installs as an engineering MSIX. |
| [P2](docs/implementation/P2.md) | A2 — Usable local workflow | An operator can register a project, deploy MCP, use tools and retain memory. |
| [P3](docs/implementation/P3.md) | A3 — Real autonomous continuity | Real LM Studio inference continues across a context-triggered fresh successor. |
| [P4](docs/implementation/P4.md) | A4 — Complete Alpha GUI | All required native surfaces and settings perform real actions. |
| [P5](docs/implementation/P5.md) | A5 — Installer and onboarding | A clean Windows installation works without the development checkout. |
| [P6](docs/implementation/P6.md) | A6 — Working Alpha | Required acceptance is recorded and the installer distribution is delivered. |

## How to execute
P0 documentation work is just enough to unblock development, not a publication project.
Build and install a small real native application during P1 rather than postponing packaging until the end.
The P1 package is clearly an engineering checkpoint; all Alpha functionality is still required before A6.
P2 and P3 connect existing backends. P4 completes the operator surfaces. P5 finishes distribution and usability.
P6 fixes defects exposed by the small acceptance pass; it does not launch a new audit program.

Work on one coherent implementation slice at a time. When a dependency blocks one slice, record the exact failure
and advance independent work; do not fabricate success or repeatedly rerun the same unsupported command.
Use the sixteen prepared issues rather than creating hundreds of microtasks. More issues are justified only by a
new concrete defect that cannot reasonably fit the current issue.

## After Alpha
**B1 — Remaining macOS behavioral parity:** advanced connectors/runtimes, richer telemetry, full optional-dashboard
features, any remaining tool-contract deltas, and portability/import work proven necessary by real use.

**B2 — Optional broader distribution and hardening:** security review/hardening, signed public distribution operations,
ARM64 qualification, stress/performance programs and broader hardware support. These do not block internal Alpha.

See [Parity](docs/PARITY.md) for what is required now versus deliberately deferred.
