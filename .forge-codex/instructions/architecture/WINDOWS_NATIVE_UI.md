# Windows-native GUI

## Framework

Use WinUI 3 with C++/WinRT and Windows App SDK. The application is packaged and supports x64; build ARM64 when supported by the installed SDK/toolchain.

## Shell

Use a `NavigationView` or equivalent native desktop shell with stable items:

- Forge Rig
- LM Studio MCP
- Agents
- Tools
- Feed
- Diagnostics
- Manager

Preserve sidebar show/hide behavior, manual refresh, auto-refresh, last-updated display, version, home path, loading state, and nonmodal error state.

## State ownership

- `AppWindowController` owns each window.
- `MainWindowViewModel` owns navigation selection and view-model lifetimes.
- Feature view models expose immutable snapshots and commands.
- Long-running services live outside views.
- Event subscriptions use revokers and are released on view-model shutdown.
- Window close and process shutdown must cancel outstanding operations.

## Desktop behavior

Implement:

- keyboard navigation and accelerators;
- menu/command equivalents for critical actions;
- window placement persistence per display/DPI;
- minimum size and responsive layouts;
- high-contrast and light/dark theme support;
- 100–300% DPI support;
- screen-reader names, roles, states, and automation IDs;
- reduced-motion behavior;
- no touch-only actions.

## UI automation

Use native Microsoft UI Automation from a C++ test host. Every navigation item, action, status, error, setting, list, table, and dialog must have stable automation IDs. Tests launch the packaged app, navigate all seven surfaces, execute safe commands, verify state, close windows, and confirm process/resource release.
