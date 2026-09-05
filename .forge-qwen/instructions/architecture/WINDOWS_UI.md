# Native Windows UI

Use packaged WinUI 3 C++/WinRT with `NavigationView` and seven stable destinations:

- Forge Rig
- LM Studio MCP
- Agents
- Tools
- Feed
- Diagnostics
- Manager

Preserve navigation show/hide, manual/automatic refresh, loading, last update, version, home, errors, settings, and baseline actions.

View models receive interfaces and immutable snapshots; views contain no process, database, filesystem, network, deployment, or telemetry collection logic. Virtualize lists. Page-specific subscriptions and rendering stop when hidden. Navigation must not recreate long-lived services.

Acceptance includes keyboard/pointer, accelerators, focus order, screen-reader names/roles/states, stable Automation IDs, 100–300% DPI, responsive resize, light/dark/high contrast, reduced motion, multi-monitor placement, and repeated navigation lifecycle tests.
