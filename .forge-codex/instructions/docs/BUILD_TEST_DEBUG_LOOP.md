# Build, test, and debug loop

## Build configurations

At minimum:

- Debug x64
- Release x64
- ASan x64 where supported
- Release ARM64 where supported
- packaged Release

## Loop

1. Configure through the repository's canonical PowerShell entry point.
2. Build the smallest affected target.
3. Run the smallest relevant native tests.
4. Launch or exercise the exact affected flow.
5. Inspect diagnostics.
6. Repair the root cause.
7. Rerun focused tests.
8. Run the containing suite.
9. Record evidence.
10. Commit a coherent checkpoint.

## Native diagnostic tools

Use first-party tooling as appropriate:

- MSVC `/analyze`;
- AddressSanitizer;
- Application Verifier;
- GFlags/PageHeap;
- Windows Error Reporting dumps;
- WinDbg;
- ETW/TraceLogging;
- WPR/WPA;
- Process Explorer/Process Monitor evidence when automation can capture it;
- GPUView or PIX;
- Windows Performance Recorder profiles;
- UI Automation test harness;
- MSIX deployment logs.

Do not declare a leak fixed from one lower snapshot. Prove the ownership path or unbounded mechanism and repeat the same scenario.
