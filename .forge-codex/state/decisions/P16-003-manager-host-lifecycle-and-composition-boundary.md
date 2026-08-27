# P16-003: Manager Host Lifecycle and Composition Boundary

Status: Accepted

Date: 2026-08-27

## Context

P16-A requires a native manager host, while the production manager executable
must initialize a truthful `IManagerRuntime`. `ManagerController::initialize`
rejects a runtime start snapshot unless the loopback listener is listening and
the operational service is active. The loopback listener is delivered by
P16-B, and the repository currently has no production `IManagerRuntime`.

Adding an executable before that runtime exists would either fail every startup
or falsely report a listener and service that were not present. Either result
would be a stub and would violate the no-feature-loss and evidence rules.

The named-pipe dispatcher may still own active controller calls while process
shutdown begins. Closing the controller directly from the host could therefore
destroy application dependencies underneath an admitted callback.

## Decision

- P16-A supplies `ManagerProcessHost` as a real, injected lifecycle boundary.
  It initializes the controller, runs the blocking manager server once, and
  propagates the original initialization or ingress error.
- Every exit closes ingress first and then calls the dispatcher's bounded
  shutdown. The dispatcher remains the sole closer of the controller after its
  admitted callbacks drain or transfer to its documented retained-state path.
- Shutdown is idempotent and may be requested concurrently with the blocking
  server run. The host owns a startup stop source and defers final dispatcher
  and controller closure to the run thread whenever a run is active. The caller
  retains the host until `run` returns; the host never resets an injected owner
  while that call may still be unwinding.
- A run-frame guard keeps that lifetime state active through callback teardown,
  exception cleanup, and final dispatcher shutdown. Destruction while the
  caller-owned run frame is still active fails fast instead of releasing
  dependencies underneath it.
- If external shutdown wins the narrow transition between the pre-ingress check
  and server startup, the resulting `transport_closed` server result is treated
  as orderly termination. Other ingress failures retain their original error.
- The future manager composition root owns the Windows instance lease outside
  `ManagerProcessHost` and releases it only after the host and all other runtime
  dependencies have shut down.
- `ForgeConductor.Manager.exe`, its concrete composition root, and process tests
  are added with the P16-B loopback runtime. Forsetti HostTemplate is not part
  of this independent native process boundary.
- Before that executable accepts production work, the composition must resolve
  the documented non-cooperative detached-worker lifetime: either retain the
  complete lease/dependency bundle with retained work or fail the manager
  process fast while kernel ownership remains held. Releasing the lease while
  an old retained worker can still execute is prohibited.

## Consequences

P16-A gains an independently tested host lifecycle without manufacturing a
false dashboard. P16-B can compose the same host around the real loopback
runtime, controller, dispatcher, and named-pipe server without changing their
ownership rules.

This decision does not complete P16-A or G16 by itself. A production manager
executable, loopback runtime, startup/restart behavior, process-residue tests,
and P16-C recovery ownership remain required.

## Evidence basis

- `include/ForgeConductor/Contracts/IManagerRuntime.h`
- `src/Application/ManagerController.cpp`
- `include/ForgeConductor/Manager/ManagerRequestDispatcher.h`
- `src/Infrastructure/Windows/WindowsManagerNamedPipeServer.cpp`
- `src/Hosts/Manager/ManagerProcessHost.h`
- `src/Hosts/Manager/ManagerProcessHost.cpp`
- `tests/Manager/ManagerProcessHostTests.cpp`
- `.forge-codex/state/decisions/P16-001-single-owner-manager-and-current-user-control-plane.md`
