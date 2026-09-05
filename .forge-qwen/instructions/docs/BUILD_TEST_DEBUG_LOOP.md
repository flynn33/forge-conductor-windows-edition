# Build-test-debug loop

Build the smallest target after structural changes and run the narrowest behavior test after behavior changes. Then run the containing suite. Use MSVC warnings-as-errors, `/analyze`, ASan where supported, App Verifier, PageHeap, CRT heap checks, WinDbg/ProcDump, ETW/WPR/WPA, UI Automation, D3D/DXGI debug layers, GPUView/PIX, and MSIX deployment logs.

Record every deterministic failure and regression test. Do not postpone all integration/performance/security work to the end.
