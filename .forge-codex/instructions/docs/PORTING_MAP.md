# macOS-to-Windows service map

| macOS implementation | Windows native implementation |
|---|---|
| SwiftUI | WinUI 3 C++/WinRT |
| Metal/MTKView | shared D3D11 + D2D + DirectWrite + Windows Composition |
| Foundation file/process APIs | C++20 filesystem + Win32 file/process APIs |
| SQLite3 | Windows SDK Winsqlite3 |
| Keychain/file protection | DPAPI + ACLs |
| Network framework/dashboard | Windows Sockets/WinRT sockets, loopback only |
| LaunchAgent | per-user Task Scheduler or approved packaged startup |
| App support directory | `%LOCALAPPDATA%\Forge Conductor` |
| `flock`/POSIX locks | named mutex/file locking |
| `/bin/bash` shell | opt-in PowerShell through CreateProcessW and Job Objects |
| `/usr/bin/grep` search | native bounded C++ recursive text search |
| CoreGraphics PDF | native C++ PDF writer/Windows print primitives without third-party runtime |
| OSLog | ETW/TraceLogging plus bounded diagnostic files |
| IOKit/host statistics | documented Windows/PDH/DXGI/process APIs |
| LM Studio macOS path assumptions | evidence-based Windows environment adapter |
| `.app` + CLI install | packaged WinUI app, execution alias, MSIX, native setup EXE |
| Swift actor/task ownership | explicit C++ service ownership, jthreads/stop tokens, bounded mailboxes |
