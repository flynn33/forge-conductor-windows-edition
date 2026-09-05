# OOP, ownership, and concurrency

## Required style

- abstract interfaces at external/replaceable boundaries;
- virtual destructors;
- `final` concrete classes;
- constructor injection;
- immutable value objects and snapshots;
- composition over implementation inheritance;
- typed result/error objects;
- exceptions contained at boundaries.

## RAII owners

Every `HANDLE`, `HKEY`, socket, WinHTTP handle, COM object/apartment, timer, event, mutex, named pipe, file mapping, process/thread, Job Object, SQLite connection/statement/backup, PDH query/counter, ETW registration, DirectX object, WinRT event token, and cancellation registration has one explicit owner.

Each long-lived owner documents lifetime, owned resources, cancellation source, thread/apartment affinity, queue limits, shutdown order, and destructor invariant after explicit stop.

## Execution domains

- WinUI dispatcher: presentation state only.
- Telemetry scheduler: serialized sample scheduling, worker collection.
- Database executor: serialized writer per database; bounded reads.
- MCP: one reader, ordered writer, bounded calls.
- Manager IOCP/named-pipe workers: bounded I/O and handler dispatch.
- Model stream: one bounded owner per turn.
- Renderer: one explicit shared graphics owner.

No blocking process, disk, network, JSON, SQLite, collector, or wait operation runs on the UI thread. No detached threads. Callbacks use weak ownership or revocable subscriptions.
