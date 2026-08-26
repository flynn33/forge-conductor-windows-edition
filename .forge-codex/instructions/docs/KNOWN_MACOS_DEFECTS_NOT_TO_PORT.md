# Known defects and risk patterns that must not be ported

The audit evidence identifies patterns requiring explicit prevention:

1. Unbounded telemetry-to-UI task dispatch and retained snapshots.
2. Per-gauge native graphics devices/queues/pipelines and recurring allocation.
3. Multiple independent telemetry, animation, and native-render clocks.
4. Missing or ambiguous cancellation ownership for tasks, timers, observers, processes, pipes, and callbacks.
5. Main-thread blocking I/O.
6. Detached or unsynchronized mutation.
7. Unbounded histories, logs, process output, caches, and queues.
8. Incorrect or constant telemetry/gauge mappings.
9. Strong delegate/callback cycles.
10. Cleanup inferred from aggregate memory rather than proven ownership.

The Windows design addresses these through latest-value mailboxes, shared graphics services, explicit lifetime matrices, RAII, bounded collections, serialized ownership, and runtime evidence.
