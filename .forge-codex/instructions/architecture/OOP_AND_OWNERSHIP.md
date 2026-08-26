# Object-oriented design and ownership rules

## Interface-first contracts

Every replaceable external boundary requires an abstract interface with a virtual destructor:

- clock and scheduler;
- filesystem and workspace authority;
- process execution/supervision;
- Git;
- shell;
- secure storage;
- configuration;
- project registry;
- project memory repository;
- legacy memory repository;
- continuity repository/coordinator;
- session-host adapter;
- telemetry collectors;
- diagnostics and audit;
- MCP transport/router;
- manager client/server;
- LM Studio environment/deployment;
- graphics device/render service;
- installer/deployment service.

Concrete implementations are `final`.

## Constructor injection

All required dependencies are constructor parameters. Optional behavior uses explicit `std::optional`, Null Object implementations, or capability objects—not hidden lookup.

## RAII

Use typed owners for:

- `HANDLE`, `HKEY`, `HINTERNET`, `SOCKET`, COM apartments and interfaces;
- process/thread handles and Job Objects;
- file mappings and mapped views;
- named pipes, events, timers, mutexes;
- SQLite connections, statements, transactions, and backups;
- ETW registrations;
- Direct3D/Direct2D/DirectWrite/Composition resources;
- WinRT event revokers and cancellation registrations.

Each owner documents:

- creating method;
- owning class;
- shutdown ordering;
- cancellation behavior;
- thread/apartment affinity;
- maximum lifetime.

## Concurrency

Prefer:

- one serialized owner for mutable service state;
- `std::jthread` and `std::stop_token`;
- WinRT cancellation tokens at UI/asynchronous boundaries;
- bounded producer/consumer mailboxes;
- immutable snapshots across threads;
- explicit executors/dispatchers.

Do not:

- detach threads;
- capture owning `shared_ptr` cyclically in callbacks;
- queue unbounded lambdas to the UI dispatcher;
- invoke blocking I/O on the UI thread;
- hold locks while awaiting, invoking callbacks, or performing I/O.

## Error model

Domain and application layers use typed result/error objects. Infrastructure may throw internally, but exceptions are caught at the boundary and converted. Error payloads include stable code, human message, retryability, and optional evidence ID.

## Testing seams

Every platform service must have a deterministic fake. Time, randomness, process output, filesystem state, host responses, database busy behavior, and telemetry are injectable.
