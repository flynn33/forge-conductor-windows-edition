# P16-033: Post-Listener Browser Activation and Event-Driven Stop Edge

Status: Accepted

Date: 2026-08-30

## Context

The macOS Manager opens its dashboard after startup when configured, while a
Windows browser must receive the bearer material without placing it in an HTTP
request target. Dashboard and named-pipe shutdown also need one process owner
that consumes the existing stop signal without polling or allowing an ingress
callback to destroy its own host.

## Decision

- `ManagerProcessHost` evaluates the initialized `openBrowserOnStart` setting
  together with the explicit process override using OR semantics. It invokes
  browser activation at most once, after controller initialization has
  published the listener and before transition workers and ingress start.
- `WindowsDashboardBrowserLauncher` accepts only canonical IPv4 or IPv6
  loopback endpoints with a nonzero port. It builds the exact dashboard URI and
  carries the hexadecimal bearer in the fragment, where it is consumed by the
  embedded shell rather than transmitted as the HTTP request target.
- `WindowsDashboardBrowserLauncher` admits at most one activation to an owned
  `std::jthread` and returns immediately. The worker runs the exact sibling
  `forge-conductor.exe` through `IProcessSupervisor`, with a bounded deadline,
  kill-on-close Job Object, exact execute-only authority, the exact authorized
  helper parent as its working directory, and the authenticated URI delivered
  only through inherited standard input. Bearer material is not placed in
  argv, the child environment, diagnostics, or child output.
- The hidden CLI helper validates a bounded canonical loopback URI, initializes
  and balances an STA, and connects to `CLSID_ShellWindows` using
  `CLSCTX_LOCAL_SERVER`. It follows the Explorer-hosted desktop chain through
  `SID_STopLevelBrowser` and invokes `IShellDispatch2::ShellExecute` with the
  explicit `open` verb and normal-show value. Explorer therefore owns any
  registered-browser process, allowing it to outlive the helper Job Object.
  Direct in-helper `ShellExecuteExW`, in-process `Shell.Application`, and WinRT
  `LaunchUriAsync` are not used: the first two can job-inherit a new browser,
  while the latter requires foreground user-initiated activation.
- The adapter never clicks, types into, or inspects another application's GUI.
  URI and token material are excluded from diagnostics and error text.
- Browser activation is a best-effort presentation side effect. A typed launch
  failure is normalized to a fixed allow-listed code and recorded
  asynchronously without surrendering Manager ownership or preventing workers
  and ingress from starting. Immediate admission failures use a separately
  owned capacity-eight diagnostic executor, so a slow diagnostic sink cannot
  hold Manager startup. `beginShutdown()` closes admission and signals
  cancellation without joining. A shutdown call from either internal worker
  signals both owners and defers both joins; the external Manager run owner
  exact-joins the helper and diagnostic workers before lease release. Terminal
  `Closed` state wins every completion/shutdown and cross-worker callback race.
- `ManagerProcessStopWatcher` owns exactly one `std::jthread`, waits on
  `ManagerProcessStopSignal` without polling, and calls a narrow retained
  shutdown target on the one stop edge. Cancellation is idempotent and has one
  exact join owner shared by concurrent callers.
- The production root must retain the stop signal beyond dashboard clients and
  the watcher, and retain the host beyond the watcher's exact join.

## Consequences

Configured startup presents the authenticated local dashboard without making
browser availability a Manager liveness dependency. HTTP and pipe shutdown
acknowledgements continue to publish the already delivery-safe stop edge, and
one process-owned watcher converts that edge into host shutdown without a
polling loop or detached callback.

This URI activation is not the retained Forge Conductor UI automation. Native
UI Automation qualification of the product's own WinUI surface remains in P20.
The production root, real-process lifecycle evidence, and authoritative G16
gate remain pending.

## Evidence basis

- `include/ForgeConductor/Contracts/IDashboardBrowserLauncher.h`
- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardBrowserLauncher.h`
- `src/Infrastructure/Windows/WindowsDashboardBrowserLauncher.cpp`
- `src/Infrastructure/Windows/WindowsDashboardUriActivationCommand.cpp`
- `src/Infrastructure/Windows/Detail/WindowsDashboardUriLaunchPlatform.cpp`
- `src/Hosts/Cli/CliCompositionRoot.cpp`
- `src/Hosts/Manager/ManagerProcessHost.h`
- `src/Hosts/Manager/ManagerProcessHost.cpp`
- `src/Hosts/Manager/ManagerProcessStopWatcher.h`
- `src/Hosts/Manager/ManagerProcessStopWatcher.cpp`
- `tests/Manager/WindowsDashboardBrowserLauncherTests.cpp`
- `tests/Manager/ManagerProcessHostTests.cpp`
- `tests/Manager/ManagerProcessStopWatcherTests.cpp`
