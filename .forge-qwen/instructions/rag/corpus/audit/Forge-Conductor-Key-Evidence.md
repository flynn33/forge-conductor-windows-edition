# Forge Conductor — Key Source Evidence

## Telemetry/backpressure/snapshot evidence

694 lexical hits.

### `Sources/ForgeConductorApp/AppModel.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // AppModel.swift
    2: // What: The main-actor presentation model shared by every native app module.
    3: // How: It composes Core services, transforms typed telemetry into view state,
    4: // and serializes user actions through observable properties and controllers.
    5: // Why: One presentation owner prevents views from duplicating lifecycle and I/O logic.
    6: 
    7: import Foundation
    8: import Combine
    9: import AppKit
   10: import ForgeConductorCore
```

### `Sources/ForgeConductorApp/AppModel.swift:16` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    9: import AppKit
   10: import ForgeConductorCore
   11: import SwiftUI
   12: 
   13: /// Owns the macOS app's observable state and coordinates every user-facing module.
   14: ///
   15: /// Views read immutable projections from this model and send user intent back through
   16: /// its methods. The model keeps process control, persistence, deployment, and telemetry
   17: /// work inside Core services so the SwiftUI layer remains declarative and testable.
   18: @MainActor
   19: public final class AppModel: ObservableObject {
   20:     @Published public private(set) var system: SystemMetrics?
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
```

### `Sources/ForgeConductorApp/AppModel.swift:20` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   13: /// Owns the macOS app's observable state and coordinates every user-facing module.
   14: ///
   15: /// Views read immutable projections from this model and send user intent back through
   16: /// its methods. The model keeps process control, persistence, deployment, and telemetry
   17: /// work inside Core services so the SwiftUI layer remains declarative and testable.
   18: @MainActor
   19: public final class AppModel: ObservableObject {
   20:     @Published public private(set) var system: SystemMetrics?
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
   24:     @Published public private(set) var lastError: String?
   25:     @Published public private(set) var isLoading = false
   26:     @Published public private(set) var version: String = ForgeApp.version
   27:     @Published public private(set) var homePath: String = ""
```

### `Sources/ForgeConductorApp/AppModel.swift:21` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   14: ///
   15: /// Views read immutable projections from this model and send user intent back through
   16: /// its methods. The model keeps process control, persistence, deployment, and telemetry
   17: /// work inside Core services so the SwiftUI layer remains declarative and testable.
   18: @MainActor
   19: public final class AppModel: ObservableObject {
   20:     @Published public private(set) var system: SystemMetrics?
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
   24:     @Published public private(set) var lastError: String?
   25:     @Published public private(set) var isLoading = false
   26:     @Published public private(set) var version: String = ForgeApp.version
   27:     @Published public private(set) var homePath: String = ""
   28:     @Published public private(set) var lastTyped: TelemetrySnapshot?
```

### `Sources/ForgeConductorApp/AppModel.swift:28` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
   24:     @Published public private(set) var lastError: String?
   25:     @Published public private(set) var isLoading = false
   26:     @Published public private(set) var version: String = ForgeApp.version
   27:     @Published public private(set) var homePath: String = ""
   28:     @Published public private(set) var lastTyped: TelemetrySnapshot?
   29:     @Published public private(set) var managerStatus: ManagerStatus?
   30:     @Published public private(set) var managerMessage: String?
   31:     @Published public private(set) var lmStudioPluginStatus: LMStudioMCPPluginInstaller.PluginStatus?
   32:     @Published public private(set) var lmStudioPluginMessage: String?
   33:     @Published public private(set) var isInstallingPlugin = false
   34:     @Published public private(set) var diagnosticPreview: [DiagnosticEnvelope] = []
   35:     @Published public private(set) var lastExportMessage: String?
```

### `Sources/ForgeConductorApp/AppModel.swift:36` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   29:     @Published public private(set) var managerStatus: ManagerStatus?
   30:     @Published public private(set) var managerMessage: String?
   31:     @Published public private(set) var lmStudioPluginStatus: LMStudioMCPPluginInstaller.PluginStatus?
   32:     @Published public private(set) var lmStudioPluginMessage: String?
   33:     @Published public private(set) var isInstallingPlugin = false
   34:     @Published public private(set) var diagnosticPreview: [DiagnosticEnvelope] = []
   35:     @Published public private(set) var lastExportMessage: String?
   36:     @Published public private(set) var measuredTelemetryHz: Double = 0
   37:     @Published public var autoRefresh = true
   38:     @Published public var selectedTab: AppTab = .rig
   39:     @Published public var isNavigationVisible = true
   40: 
   41:     @Published public var setHost: String = "127.0.0.1"
   42:     @Published public var setPort: Int = 7788
   43:     @Published public var setRefresh: Int = 8
```

### `Sources/ForgeConductorApp/AppModel.swift:53` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   46:     @Published public var setShellTimeout: Int = 30
   47:     @Published public var setAutoRestart: Bool = true
   48: 
   49:     public private(set) var app: ForgeApp?
   50:     public private(set) var manager: ManagerNode?
   51:     public private(set) var remoteManager: ManagerDashboardClient?
   52:     public private(set) var deployController: AppDeployController?
   53:     public private(set) var telemetryBinding = AppTelemetryBinding()
   54: 
   55:     private var managerPoll: AnyCancellable?
   56:     private var telemetryBag: AnyCancellable?
   57:     private var managerPollInFlight = false
   58:     private var remoteManagerLastError: String?
   59: 
   60:     public enum AppTab: String, CaseIterable, Identifiable {
```

### `Sources/ForgeConductorApp/AppModel.swift:56` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   49:     public private(set) var app: ForgeApp?
   50:     public private(set) var manager: ManagerNode?
   51:     public private(set) var remoteManager: ManagerDashboardClient?
   52:     public private(set) var deployController: AppDeployController?
   53:     public private(set) var telemetryBinding = AppTelemetryBinding()
   54: 
   55:     private var managerPoll: AnyCancellable?
   56:     private var telemetryBag: AnyCancellable?
   57:     private var managerPollInFlight = false
   58:     private var remoteManagerLastError: String?
   59: 
   60:     public enum AppTab: String, CaseIterable, Identifiable {
   61:         case rig = "FORGE RIG"
   62:         case mcp = "LM Studio MCP"
   63:         case agents = "Agents"
```

### `Sources/ForgeConductorApp/AppModel.swift:87` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   80:             }
   81:         }
   82:     }
   83: 
   84:     public init() {
   85:         bootstrap()
   86:         startManagerPoll()
   87:         bindTelemetryMirror()
   88:     }
   89: 
   90:     public func bootstrap() {
   91:         do {
   92:             let forgeApp = try ForgeApp.bootstrap()
   93:             self.app = forgeApp
   94:             self.homePath = forgeApp.paths.home.path
```

### `Sources/ForgeConductorApp/AppModel.swift:97` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   90:     public func bootstrap() {
   91:         do {
   92:             let forgeApp = try ForgeApp.bootstrap()
   93:             self.app = forgeApp
   94:             self.homePath = forgeApp.paths.home.path
   95:             self.version = ForgeApp.version
   96:             self.deployController = AppDeployController(app: forgeApp)
   97:             telemetryBinding.attach(app: forgeApp)
   98:             if CommandLine.arguments.contains("--uitesting") {
   99:                 managerMessage = "Manager disabled during UI tests"
  100:             } else {
  101:                 attachToOrStartManager(app: forgeApp)
  102:             }
  103:             loadSettingsFromConfig()
  104:             refreshLMStudioPluginStatus()
```

### `Sources/ForgeConductorApp/AppModel.swift:168` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  161:             forgeApp.diagnostics.error("gui_dashboard_bind_failed", [
  162:                 "error": "\(error)",
  163:             ], category: .manager)
  164:         }
  165:     }
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
```

### `Sources/ForgeConductorApp/AppModel.swift:169` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  162:                 "error": "\(error)",
  163:             ], category: .manager)
  164:         }
  165:     }
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
```

### `Sources/ForgeConductorApp/AppModel.swift:170` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  163:             ], category: .manager)
  164:         }
  165:     }
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
```

### `Sources/ForgeConductorApp/AppModel.swift:171` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  164:         }
  165:     }
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
```

### `Sources/ForgeConductorApp/AppModel.swift:174` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
  181:         forge = b.forge
```

### `Sources/ForgeConductorApp/AppModel.swift:178` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
  181:         forge = b.forge
  182:         history = b.history
  183:         updated = b.updated
  184:         lastTyped = b.lastTyped
  185:         measuredTelemetryHz = b.measuredHz
```

### `Sources/ForgeConductorApp/AppModel.swift:179` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
  181:         forge = b.forge
  182:         history = b.history
  183:         updated = b.updated
  184:         lastTyped = b.lastTyped
  185:         measuredTelemetryHz = b.measuredHz
  186:         isLoading = b.isLoading
```

### `Sources/ForgeConductorApp/AppModel.swift:185` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
  181:         forge = b.forge
  182:         history = b.history
  183:         updated = b.updated
  184:         lastTyped = b.lastTyped
  185:         measuredTelemetryHz = b.measuredHz
  186:         isLoading = b.isLoading
  187:         if let e = b.lastError { lastError = e }
  188:     }
  189: 
  190:     public var preferredServeBinary: URL {
  191:         deployController?.preferredServeBinary
  192:             ?? app?.lmStudioDeploy.resolveServeBinary(preferred: Bundle.main.executableURL)
```

### `Sources/ForgeConductorApp/AppModel.swift:277` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  270:                 }
  271:                 self.remoteManagerLastError = detail
  272:             }
  273:         }
  274:     }
  275: 
  276:     public func refresh(force: Bool) {
  277:         telemetryBinding.autoRefresh = autoRefresh
  278:         telemetryBinding.refresh(force: force)
  279:         syncFromTelemetryBinding()
  280:     }
  281: 
  282:     // MARK: - Diagnostics
  283: 
  284:     public func refreshDiagnosticsPreview() {
```

### `Sources/ForgeConductorApp/AppModel.swift:278` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  271:                 self.remoteManagerLastError = detail
  272:             }
  273:         }
  274:     }
  275: 
  276:     public func refresh(force: Bool) {
  277:         telemetryBinding.autoRefresh = autoRefresh
  278:         telemetryBinding.refresh(force: force)
  279:         syncFromTelemetryBinding()
  280:     }
  281: 
  282:     // MARK: - Diagnostics
  283: 
  284:     public func refreshDiagnosticsPreview() {
  285:         diagnosticPreview = app?.diagnostics.recent(limit: 200) ?? []
```

### `Sources/ForgeConductorApp/AppModel.swift:279` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  272:             }
  273:         }
  274:     }
  275: 
  276:     public func refresh(force: Bool) {
  277:         telemetryBinding.autoRefresh = autoRefresh
  278:         telemetryBinding.refresh(force: force)
  279:         syncFromTelemetryBinding()
  280:     }
  281: 
  282:     // MARK: - Diagnostics
  283: 
  284:     public func refreshDiagnosticsPreview() {
  285:         diagnosticPreview = app?.diagnostics.recent(limit: 200) ?? []
  286:     }
```

### `Sources/ForgeConductorApp/AppModel.swift:341` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  334:     public var sysStrip: SysStripModel {
  335:         if let s = system { return SysStripModel(from: s) }
  336:         return SysStripModel(from: emptySystem())
  337:     }
  338: 
  339:     public var perCPU: [Double] { system?.cpu.perCPU ?? [] }
  340:     public var diskVolumes: [DiskVolume] { system?.disk ?? [] }
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
```

### `Sources/ForgeConductorApp/AppModel.swift:343` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  336:         return SysStripModel(from: emptySystem())
  337:     }
  338: 
  339:     public var perCPU: [Double] { system?.cpu.perCPU ?? [] }
  340:     public var diskVolumes: [DiskVolume] { system?.disk ?? [] }
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
```

### `Sources/ForgeConductorApp/AppModel.swift:345` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  338: 
  339:     public var perCPU: [Double] { system?.cpu.perCPU ?? [] }
  340:     public var diskVolumes: [DiskVolume] { system?.disk ?? [] }
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
  351:     public var cpuPercent: Double { system?.cpu.percent ?? 0 }
  352:     public var ramPercent: Double { system?.ram.percent ?? 0 }
```

### `Sources/ForgeConductorApp/AppModel.swift:381` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  374:     }
  375: 
  376:     public var managerVersionNotice: String? {
  377:         guard managerVersionIsCurrent == false else { return nil }
  378:         return "The running manager is \(managerRuntimeVersion), while this app is \(version). Reinstall the login manager from this build before relying on runtime parity."
  379:     }
  380: 
  381:     public var telemetryModeLabel: String {
  382:         let target = app?.telemetry.realtimeEngine.targetSampleHz ?? RealtimeMetricsEngine.defaultTargetHz
  383:         let meas = measuredTelemetryHz
  384:         if meas > 0.5 {
  385:             return String(format: "Real-time native · %.0f Hz target · %.1f Hz measured", target, meas)
  386:         }
  387:         return String(format: "Real-time native · %.0f Hz continuous", target)
  388:     }
```

### `Sources/ForgeConductorApp/AppModel.swift:382` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  375: 
  376:     public var managerVersionNotice: String? {
  377:         guard managerVersionIsCurrent == false else { return nil }
  378:         return "The running manager is \(managerRuntimeVersion), while this app is \(version). Reinstall the login manager from this build before relying on runtime parity."
  379:     }
  380: 
  381:     public var telemetryModeLabel: String {
  382:         let target = app?.telemetry.realtimeEngine.targetSampleHz ?? RealtimeMetricsEngine.defaultTargetHz
  383:         let meas = measuredTelemetryHz
  384:         if meas > 0.5 {
  385:             return String(format: "Real-time native · %.0f Hz target · %.1f Hz measured", target, meas)
  386:         }
  387:         return String(format: "Real-time native · %.0f Hz continuous", target)
  388:     }
  389: 
```

### `Sources/ForgeConductorApp/AppModel.swift:383` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  376:     public var managerVersionNotice: String? {
  377:         guard managerVersionIsCurrent == false else { return nil }
  378:         return "The running manager is \(managerRuntimeVersion), while this app is \(version). Reinstall the login manager from this build before relying on runtime parity."
  379:     }
  380: 
  381:     public var telemetryModeLabel: String {
  382:         let target = app?.telemetry.realtimeEngine.targetSampleHz ?? RealtimeMetricsEngine.defaultTargetHz
  383:         let meas = measuredTelemetryHz
  384:         if meas > 0.5 {
  385:             return String(format: "Real-time native · %.0f Hz target · %.1f Hz measured", target, meas)
  386:         }
  387:         return String(format: "Real-time native · %.0f Hz continuous", target)
  388:     }
  389: 
  390:     // MARK: - Manager
```

### `Sources/ForgeConductorApp/AppModel.swift:600` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  593: 
  594:     public func pruneSessions() {
  595:         try? app?.sessions.pruneStale()
  596:         app?.diagnostics.info("sessions_pruned", [:], category: .agent)
  597:         refresh(force: true)
  598:     }
  599: 
  600:     private func emptySystem() -> SystemMetrics {
  601:         SystemMetrics(
  602:             ts: 0, host: "—", platform: "darwin", arch: "—",
  603:             cpu: CPUMetrics(
  604:                 percent: 0, perCPU: [], countLogical: 0, countPhysical: 0,
  605:                 freqMHz: nil, freqPerCoreMHz: nil, loadAvg: (0, 0, 0),
  606:                 brand: "—", user: 0, system: 0, idle: 100
  607:             ),
```

### `Sources/ForgeConductorApp/AppModel.swift:601` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  594:     public func pruneSessions() {
  595:         try? app?.sessions.pruneStale()
  596:         app?.diagnostics.info("sessions_pruned", [:], category: .agent)
  597:         refresh(force: true)
  598:     }
  599: 
  600:     private func emptySystem() -> SystemMetrics {
  601:         SystemMetrics(
  602:             ts: 0, host: "—", platform: "darwin", arch: "—",
  603:             cpu: CPUMetrics(
  604:                 percent: 0, perCPU: [], countLogical: 0, countPhysical: 0,
  605:                 freqMHz: nil, freqPerCoreMHz: nil, loadAvg: (0, 0, 0),
  606:                 brand: "—", user: 0, system: 0, idle: 100
  607:             ),
  608:             ram: RAMMetrics(
```

### `Sources/ForgeConductorApp/AppModel.swift:603` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  596:         app?.diagnostics.info("sessions_pruned", [:], category: .agent)
  597:         refresh(force: true)
  598:     }
  599: 
  600:     private func emptySystem() -> SystemMetrics {
  601:         SystemMetrics(
  602:             ts: 0, host: "—", platform: "darwin", arch: "—",
  603:             cpu: CPUMetrics(
  604:                 percent: 0, perCPU: [], countLogical: 0, countPhysical: 0,
  605:                 freqMHz: nil, freqPerCoreMHz: nil, loadAvg: (0, 0, 0),
  606:                 brand: "—", user: 0, system: 0, idle: 100
  607:             ),
  608:             ram: RAMMetrics(
  609:                 totalGB: 0, usedGB: 0, availableGB: 0, percent: 0,
  610:                 pressurePercent: 0, activeGB: 0, wiredGB: 0, compressedGB: 0
```

### `Sources/ForgeConductorApp/AppModel.swift:608` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  601:         SystemMetrics(
  602:             ts: 0, host: "—", platform: "darwin", arch: "—",
  603:             cpu: CPUMetrics(
  604:                 percent: 0, perCPU: [], countLogical: 0, countPhysical: 0,
  605:                 freqMHz: nil, freqPerCoreMHz: nil, loadAvg: (0, 0, 0),
  606:                 brand: "—", user: 0, system: 0, idle: 100
  607:             ),
  608:             ram: RAMMetrics(
  609:                 totalGB: 0, usedGB: 0, availableGB: 0, percent: 0,
  610:                 pressurePercent: 0, activeGB: 0, wiredGB: 0, compressedGB: 0
  611:             ),
  612:             disk: [],
  613:             diskIO: DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0),
  614:             gpu: [],
  615:             processes: [],
```

### `Sources/ForgeConductorApp/AppModel.swift:613` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  606:                 brand: "—", user: 0, system: 0, idle: 100
  607:             ),
  608:             ram: RAMMetrics(
  609:                 totalGB: 0, usedGB: 0, availableGB: 0, percent: 0,
  610:                 pressurePercent: 0, activeGB: 0, wiredGB: 0, compressedGB: 0
  611:             ),
  612:             disk: [],
  613:             diskIO: DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0),
  614:             gpu: [],
  615:             processes: [],
  616:             power: .unknown
  617:         )
  618:     }
  619: }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:1` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // AppTelemetryBinding.swift
    2: // What: Bridges the continuously sampled Core telemetry stream into SwiftUI.
    3: // How: It subscribes once, coalesces frames on the main actor, and publishes a
    4: // composed snapshot only after every related field has been updated together.
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // AppTelemetryBinding.swift
    2: // What: Bridges the continuously sampled Core telemetry stream into SwiftUI.
    3: // How: It subscribes once, coalesces frames on the main actor, and publishes a
    4: // composed snapshot only after every related field has been updated together.
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // AppTelemetryBinding.swift
    2: // What: Bridges the continuously sampled Core telemetry stream into SwiftUI.
    3: // How: It subscribes once, coalesces frames on the main actor, and publishes a
    4: // composed snapshot only after every related field has been updated together.
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
   10: 
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:4` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // AppTelemetryBinding.swift
    2: // What: Bridges the continuously sampled Core telemetry stream into SwiftUI.
    3: // How: It subscribes once, coalesces frames on the main actor, and publishes a
    4: // composed snapshot only after every related field has been updated together.
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:11` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    4: // composed snapshot only after every related field has been updated together.
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:12` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    5: // Why: A single binding avoids inconsistent partial frames and duplicate listeners.
    6: 
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:14` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    7: import Foundation
    8: import Combine
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
   20:     public private(set) var measuredHz: Double = 0
   21:     public private(set) var lastError: String?
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:15` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    8: import Combine
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
   20:     public private(set) var measuredHz: Double = 0
   21:     public private(set) var lastError: String?
   22:     public private(set) var isLoading = false
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:16` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    9: import ForgeConductorCore
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
   20:     public private(set) var measuredHz: Double = 0
   21:     public private(set) var lastError: String?
   22:     public private(set) var isLoading = false
   23:     @Published public var autoRefresh = true
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:19` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
   20:     public private(set) var measuredHz: Double = 0
   21:     public private(set) var lastError: String?
   22:     public private(set) var isLoading = false
   23:     @Published public var autoRefresh = true
   24: 
   25:     private weak var app: ForgeApp?
   26:     private var frameListenerID: UUID?
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:29` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   22:     public private(set) var isLoading = false
   23:     @Published public var autoRefresh = true
   24: 
   25:     private weak var app: ForgeApp?
   26:     private var frameListenerID: UUID?
   27:     private let updateSubject = PassthroughSubject<Void, Never>()
   28: 
   29:     /// Emits after a complete telemetry frame has been applied. Consumers that
   30:     /// mirror this state receive one coherent update instead of one per field.
   31:     public var updates: AnyPublisher<Void, Never> {
   32:         updateSubject.eraseToAnyPublisher()
   33:     }
   34: 
   35:     public init() {}
   36: 
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:42` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   35:     public init() {}
   36: 
   37:     public func attach(app: ForgeApp) {
   38:         detach()
   39:         self.app = app
   40: 
   41:         // Always run continuous host sampling for the GUI.
   42:         if !app.telemetry.realtimeEngine.isRunning {
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:43` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   36: 
   37:     public func attach(app: ForgeApp) {
   38:         detach()
   39:         self.app = app
   40: 
   41:         // Always run continuous host sampling for the GUI.
   42:         if !app.telemetry.realtimeEngine.isRunning {
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
   50:                 self.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:46` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   39:         self.app = app
   40: 
   41:         // Always run continuous host sampling for the GUI.
   42:         if !app.telemetry.realtimeEngine.isRunning {
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
   50:                 self.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   51:                 self.apply(frame)
   52:             }
   53:         }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:47` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   40: 
   41:         // Always run continuous host sampling for the GUI.
   42:         if !app.telemetry.realtimeEngine.isRunning {
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
   50:                 self.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   51:                 self.apply(frame)
   52:             }
   53:         }
   54: 
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:50` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
   50:                 self.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   51:                 self.apply(frame)
   52:             }
   53:         }
   54: 
   55:         // One forge seed so MCP/agent cards appear immediately; host already streaming.
   56:         seedForgeOnce()
   57:     }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:61` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   54: 
   55:         // One forge seed so MCP/agent cards appear immediately; host already streaming.
   56:         seedForgeOnce()
   57:     }
   58: 
   59:     public func detach() {
   60:         if let id = frameListenerID, let app {
   61:             app.telemetry.removeListener(id)
   62:         }
   63:         frameListenerID = nil
   64:     }
   65: 
   66:     /// Manual recompose of forge cards only (does not replace the continuous host stream).
   67:     public func refresh(force: Bool) {
   68:         guard let app else { return }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:76` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   69:         if isLoading && !force { return }
   70:         objectWillChange.send()
   71:         isLoading = true
   72:         updateSubject.send()
   73:         Task { [weak self] in
   74:             do {
   75:                 let frame = try await Task.detached {
   76:                     try app.telemetry.snapshotTyped(force: true)
   77:                 }.value
   78:                 await MainActor.run {
   79:                     self?.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   80:                     self?.apply(frame)
   81:                 }
   82:             } catch {
   83:                 await MainActor.run {
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:79` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   72:         updateSubject.send()
   73:         Task { [weak self] in
   74:             do {
   75:                 let frame = try await Task.detached {
   76:                     try app.telemetry.snapshotTyped(force: true)
   77:                 }.value
   78:                 await MainActor.run {
   79:                     self?.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   80:                     self?.apply(frame)
   81:                 }
   82:             } catch {
   83:                 await MainActor.run {
   84:                     self?.objectWillChange.send()
   85:                     self?.lastError = "\(error)"
   86:                     self?.isLoading = false
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:88` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   81:                 }
   82:             } catch {
   83:                 await MainActor.run {
   84:                     self?.objectWillChange.send()
   85:                     self?.lastError = "\(error)"
   86:                     self?.isLoading = false
   87:                     self?.updateSubject.send()
   88:                     app.diagnostics.warn("telemetry_refresh_failed", [
   89:                         "error": "\(error)",
   90:                     ], category: .telemetry)
   91:                 }
   92:             }
   93:         }
   94:     }
   95: 
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:90` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   83:                 await MainActor.run {
   84:                     self?.objectWillChange.send()
   85:                     self?.lastError = "\(error)"
   86:                     self?.isLoading = false
   87:                     self?.updateSubject.send()
   88:                     app.diagnostics.warn("telemetry_refresh_failed", [
   89:                         "error": "\(error)",
   90:                     ], category: .telemetry)
   91:                 }
   92:             }
   93:         }
   94:     }
   95: 
   96:     public var modeLabel: String {
   97:         let target = app?.telemetry.realtimeEngine.targetSampleHz ?? RealtimeMetricsEngine.defaultTargetHz
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:97` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   90:                     ], category: .telemetry)
   91:                 }
   92:             }
   93:         }
   94:     }
   95: 
   96:     public var modeLabel: String {
   97:         let target = app?.telemetry.realtimeEngine.targetSampleHz ?? RealtimeMetricsEngine.defaultTargetHz
   98:         if measuredHz > 0.5 {
   99:             return String(format: "Real-time native · %.0f Hz target · %.1f Hz measured", target, measuredHz)
  100:         }
  101:         return String(format: "Real-time native · %.0f Hz continuous stream", target)
  102:     }
  103: 
  104:     private func seedForgeOnce() {
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:106` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   99:             return String(format: "Real-time native · %.0f Hz target · %.1f Hz measured", target, measuredHz)
  100:         }
  101:         return String(format: "Real-time native · %.0f Hz continuous stream", target)
  102:     }
  103: 
  104:     private func seedForgeOnce() {
  105:         guard let app else { return }
  106:         let frame = app.telemetry.currentFrame()
  107:         measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
  108:         apply(frame)
  109:     }
  110: 
  111:     private func apply(_ typed: TelemetrySnapshot) {
  112:         objectWillChange.send()
  113:         lastTyped = typed
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:107` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  100:         }
  101:         return String(format: "Real-time native · %.0f Hz continuous stream", target)
  102:     }
  103: 
  104:     private func seedForgeOnce() {
  105:         guard let app else { return }
  106:         let frame = app.telemetry.currentFrame()
  107:         measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
  108:         apply(frame)
  109:     }
  110: 
  111:     private func apply(_ typed: TelemetrySnapshot) {
  112:         objectWillChange.send()
  113:         lastTyped = typed
  114:         system = typed.system
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:111` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  104:     private func seedForgeOnce() {
  105:         guard let app else { return }
  106:         let frame = app.telemetry.currentFrame()
  107:         measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
  108:         apply(frame)
  109:     }
  110: 
  111:     private func apply(_ typed: TelemetrySnapshot) {
  112:         objectWillChange.send()
  113:         lastTyped = typed
  114:         system = typed.system
  115:         forge = typed.forge
  116:         history = typed.history
  117:         updated = Date(timeIntervalSince1970: typed.updated)
  118:         lastError = nil
```

### `Sources/ForgeConductorApp/ForgeConductorApp.swift:44` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   37:             CommandGroup(replacing: .newItem) {}
   38:             CommandMenu("Navigation") {
   39:                 Button(model.isNavigationVisible ? "Hide Navigation" : "Show Navigation") {
   40:                     model.toggleNavigation()
   41:                 }
   42:                 .keyboardShortcut("s", modifiers: [.command, .control])
   43:             }
   44:             CommandMenu("Telemetry") {
   45:                 Button("Refresh Now") { model.refresh(force: true) }
   46:                     .keyboardShortcut("r", modifiers: [.command])
   47:                 Toggle(
   48:                     "Auto-refresh",
   49:                     isOn: Binding(
   50:                         get: { model.autoRefresh },
   51:                         set: { model.autoRefresh = $0 }
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // LoadTraceRenderer.swift
    2: // What: Draws the historical load trace into an MTKView.
    3: // How: The delegate converts normalized samples into GPU vertex buffers and
    4: // encodes Metal draw calls whenever SwiftUI supplies updated history.
    5: // Why: GPU rendering keeps a rapidly refreshing chart off the main UI drawing path.
    6: 
    7: import Foundation
    8: import MetalKit
    9: import simd
   10: 
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:18` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   11: /// Metal renderer for CPU load history (glowing cyan line + fill).
   12: @MainActor
   13: final class LoadTraceRenderer: NSObject, MTKViewDelegate {
   14:     private var device: MTLDevice?
   15:     private var queue: MTLCommandQueue?
   16:     private var pipeline: MTLRenderPipelineState?
   17:     private var vertexBuffer: MTLBuffer?
   18:     private var sampleCount = 0
   19:     private let lock = NSLock()
   20:     private var samples: [Float] = []
   21: 
   22:     func attach(to view: MTKView) {
   23:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   24:         guard let device = mtl else { return }
   25:         self.device = device
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:20` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   13: final class LoadTraceRenderer: NSObject, MTKViewDelegate {
   14:     private var device: MTLDevice?
   15:     private var queue: MTLCommandQueue?
   16:     private var pipeline: MTLRenderPipelineState?
   17:     private var vertexBuffer: MTLBuffer?
   18:     private var sampleCount = 0
   19:     private let lock = NSLock()
   20:     private var samples: [Float] = []
   21: 
   22:     func attach(to view: MTKView) {
   23:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   24:         guard let device = mtl else { return }
   25:         self.device = device
   26:         view.device = device
   27:         view.delegate = self
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:32` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   25:         self.device = device
   26:         view.device = device
   27:         view.delegate = self
   28:         queue = device.makeCommandQueue()
   29:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   30:     }
   31: 
   32:     func update(samples: [Float]) {
   33:         lock.lock()
   34:         self.samples = samples
   35:         lock.unlock()
   36:         rebuildVertices()
   37:     }
   38: 
   39:     private func buildPipeline(device: MTLDevice, pixelFormat: MTLPixelFormat) {
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:34` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   27:         view.delegate = self
   28:         queue = device.makeCommandQueue()
   29:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   30:     }
   31: 
   32:     func update(samples: [Float]) {
   33:         lock.lock()
   34:         self.samples = samples
   35:         lock.unlock()
   36:         rebuildVertices()
   37:     }
   38: 
   39:     private func buildPipeline(device: MTLDevice, pixelFormat: MTLPixelFormat) {
   40:         let library: MTLLibrary
   41:         do {
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:66` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   59:         desc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
   60:         pipeline = try? device.makeRenderPipelineState(descriptor: desc)
   61:     }
   62: 
   63:     private func rebuildVertices() {
   64:         guard let device else { return }
   65:         lock.lock()
   66:         let src = samples
   67:         lock.unlock()
   68:         let n = max(src.count, 2)
   69:         // Triangle strip fill under the curve + line on top: store fill verts then line verts.
   70:         // Layout: for each sample i, position xy in NDC-ish [-1,1], color as attribute.
   71:         struct V { var pos: SIMD2<Float>; var color: SIMD4<Float> }
   72:         var verts: [V] = []
   73:         verts.reserveCapacity(n * 2 + n)
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:70` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   63:     private func rebuildVertices() {
   64:         guard let device else { return }
   65:         lock.lock()
   66:         let src = samples
   67:         lock.unlock()
   68:         let n = max(src.count, 2)
   69:         // Triangle strip fill under the curve + line on top: store fill verts then line verts.
   70:         // Layout: for each sample i, position xy in NDC-ish [-1,1], color as attribute.
   71:         struct V { var pos: SIMD2<Float>; var color: SIMD4<Float> }
   72:         var verts: [V] = []
   73:         verts.reserveCapacity(n * 2 + n)
   74: 
   75:         let cyan = SIMD4<Float>(0.09, 0.94, 1.0, 0.55)
   76:         let cyanLine = SIMD4<Float>(0.2, 0.96, 1.0, 1.0)
   77:         let base = SIMD4<Float>(0.05, 0.2, 0.3, 0.0)
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:104` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   97:         for i in 0..<n {
   98:             let val = i < src.count ? src[i] : 0
   99:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyanLine))
  100:         }
  101: 
  102:         let bytes = verts.count * MemoryLayout<V>.stride
  103:         vertexBuffer = device.makeBuffer(bytes: verts, length: bytes, options: .storageModeShared)
  104:         sampleCount = fillCount // first draw fill; line uses rest
  105:         // Store line offset in high bits via sampleCount encoding: fillCount | (lineCount << 16) — simpler: store both
  106:         lock.lock()
  107:         self.fillVertexCount = fillCount
  108:         self.lineVertexCount = n
  109:         lock.unlock()
  110:     }
  111: 
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:105` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   98:             let val = i < src.count ? src[i] : 0
   99:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyanLine))
  100:         }
  101: 
  102:         let bytes = verts.count * MemoryLayout<V>.stride
  103:         vertexBuffer = device.makeBuffer(bytes: verts, length: bytes, options: .storageModeShared)
  104:         sampleCount = fillCount // first draw fill; line uses rest
  105:         // Store line offset in high bits via sampleCount encoding: fillCount | (lineCount << 16) — simpler: store both
  106:         lock.lock()
  107:         self.fillVertexCount = fillCount
  108:         self.lineVertexCount = n
  109:         lock.unlock()
  110:     }
  111: 
  112:     private var fillVertexCount = 0
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:12` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    5: // Why: Centralizing gauge primitives keeps visual behavior consistent and modular.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: import simd
   10: import ForgeConductorCore
   11: 
   12: extension TelemetryStatusTone {
   13:     var color: Color {
   14:         switch self {
   15:         case .healthy: .green
   16:         case .caution: .yellow
   17:         case .failure: .red
   18:         case .informational: .cyan
   19:         case .unavailable: .secondary
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:41` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   34:     static func from(swiftUI color: Color) -> SIMD4<Float> {
   35:         let n = NSColor(color)
   36:         guard let rgb = n.usingColorSpace(.deviceRGB) else { return cyan }
   37:         return SIMD4(Float(rgb.redComponent), Float(rgb.greenComponent), Float(rgb.blueComponent), 1)
   38:     }
   39: 
   40:     static func health(_ h: String) -> SIMD4<Float> {
   41:         switch TelemetryHealth.tone(for: h) {
   42:         case .healthy: return green
   43:         case .caution: return SIMD4(1, 0.8, 0.2, 1)
   44:         case .failure: return red
   45:         case .informational: return cyan
   46:         case .unavailable: return SIMD4(0.48, 0.54, 0.62, 1)
   47:         }
   48:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:440` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  433:         }
  434:         .padding(.horizontal, 8)
  435:         .padding(.vertical, 7)
  436:         .background(RoundedRectangle(cornerRadius: 5).fill(Color.white.opacity(0.05)))
  437:     }
  438: 
  439:     private var healthColor: Color {
  440:         TelemetryHealth.tone(for: health).color
  441:     }
  442: }
  443: 
  444: /// Header status pill with Metal activity bar underneath.
  445: /// Fixed geometry so the upper-right cluster stays toolbar-scale, not MTKView-scale.
  446: struct MetalStatusPill: View {
  447:     var text: String
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:448` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  441:     }
  442: }
  443: 
  444: /// Header status pill with Metal activity bar underneath.
  445: /// Fixed geometry so the upper-right cluster stays toolbar-scale, not MTKView-scale.
  446: struct MetalStatusPill: View {
  447:     var text: String
  448:     var tone: TelemetryStatusTone
  449:     var fraction: Double = 1
  450: 
  451:     /// Compact chip: fits four across a typical detail header without colliding with the title.
  452:     private let width: CGFloat = 80
  453:     private let barHeight: CGFloat = 3
  454:     private var tint: Color { tone.color }
  455: 
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:4` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // MetalLoadChart.swift
    2: // What: Adapts the single-series load renderer to SwiftUI.
    3: // How: NSViewRepresentable creates an MTKView, assigns its coordinator, and
    4: // forwards new sample arrays without rebuilding the native view.
    5: // Why: The adapter isolates AppKit/Metal lifecycle details from dashboard composition.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: 
   10: /// SwiftUI wrapper around an MTKView that draws the load history with Metal.
   11: struct MetalLoadChart: NSViewRepresentable {
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:12` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    5: // Why: The adapter isolates AppKit/Metal lifecycle details from dashboard composition.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: 
   10: /// SwiftUI wrapper around an MTKView that draws the load history with Metal.
   11: struct MetalLoadChart: NSViewRepresentable {
   12:     var samples: [Float]
   13: 
   14:     func makeCoordinator() -> LoadTraceRenderer {
   15:         LoadTraceRenderer()
   16:     }
   17: 
   18:     func makeNSView(context: Context) -> MTKView {
   19:         let view = MTKView()
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:28` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   21:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
   22:         view.colorPixelFormat = .bgra8Unorm
   23:         view.framebufferOnly = true
   24:         view.isPaused = false
   25:         view.enableSetNeedsDisplay = false
   26:         view.preferredFramesPerSecond = 30
   27:         context.coordinator.attach(to: view)
   28:         context.coordinator.update(samples: samples)
   29:         return view
   30:     }
   31: 
   32:     func updateNSView(_ nsView: MTKView, context: Context) {
   33:         context.coordinator.update(samples: samples)
   34:     }
   35: }
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:33` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   26:         view.preferredFramesPerSecond = 30
   27:         context.coordinator.attach(to: view)
   28:         context.coordinator.update(samples: samples)
   29:         return view
   30:     }
   31: 
   32:     func updateNSView(_ nsView: MTKView, context: Context) {
   33:         context.coordinator.update(samples: samples)
   34:     }
   35: }
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:100` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   93:         let fillStart = verts.count
   94:         var fillCount = 0
   95:         if let cpu = seriesCopy.first {
   96:             let n = max(cpu.values.count, 2)
   97:             let fill = SIMD4<Float>(0.09, 0.94, 1.0, 0.22)
   98:             for i in 0..<n {
   99:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  100:                 let sample = i < cpu.values.count ? cpu.values[i] : nil
  101:                 let v = min(max((sample ?? 0) / 100, 0), 1)
  102:                 let y = -0.85 + 1.7 * v
  103:                 verts.append(V(pos: SIMD2(x, -0.85), color: SIMD4<Float>(0.05, 0.2, 0.3, 0)))
  104:                 verts.append(V(pos: SIMD2(x, y), color: fill))
  105:                 fillCount += 2
  106:             }
  107:         }
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:101` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   94:         var fillCount = 0
   95:         if let cpu = seriesCopy.first {
   96:             let n = max(cpu.values.count, 2)
   97:             let fill = SIMD4<Float>(0.09, 0.94, 1.0, 0.22)
   98:             for i in 0..<n {
   99:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  100:                 let sample = i < cpu.values.count ? cpu.values[i] : nil
  101:                 let v = min(max((sample ?? 0) / 100, 0), 1)
  102:                 let y = -0.85 + 1.7 * v
  103:                 verts.append(V(pos: SIMD2(x, -0.85), color: SIMD4<Float>(0.05, 0.2, 0.3, 0)))
  104:                 verts.append(V(pos: SIMD2(x, y), color: fill))
  105:                 fillCount += 2
  106:             }
  107:         }
  108:         var lineRanges: [(Int, Int)] = []
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:121` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  114:             func finishSegment() {
  115:                 if let segmentStart, segmentCount >= 2 {
  116:                     lineRanges.append((segmentStart, segmentCount))
  117:                 }
  118:             }
  119: 
  120:             for i in 0..<n {
  121:                 let sample = i < s.values.count ? s.values[i] : nil
  122:                 guard let sample, sample.isFinite else {
  123:                     finishSegment()
  124:                     segmentStart = nil
  125:                     segmentCount = 0
  126:                     continue
  127:                 }
  128:                 if segmentStart == nil {
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:122` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  115:                 if let segmentStart, segmentCount >= 2 {
  116:                     lineRanges.append((segmentStart, segmentCount))
  117:                 }
  118:             }
  119: 
  120:             for i in 0..<n {
  121:                 let sample = i < s.values.count ? s.values[i] : nil
  122:                 guard let sample, sample.isFinite else {
  123:                     finishSegment()
  124:                     segmentStart = nil
  125:                     segmentCount = 0
  126:                     continue
  127:                 }
  128:                 if segmentStart == nil {
  129:                     segmentStart = verts.count
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:132` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  125:                     segmentCount = 0
  126:                     continue
  127:                 }
  128:                 if segmentStart == nil {
  129:                     segmentStart = verts.count
  130:                 }
  131:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  132:                 let v = min(max(sample / 100, 0), 1)
  133:                 let y = -0.85 + 1.7 * v
  134:                 verts.append(V(pos: SIMD2(x, y), color: s.color))
  135:                 segmentCount += 1
  136:             }
  137:             finishSegment()
  138:         }
  139: 
```

### `Sources/ForgeConductorApp/Views/DiagnosticsView.swift:21` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   14:     var body: some View {
   15:         VStack(alignment: .leading, spacing: 14) {
   16:             HStack(spacing: 12) {
   17:                 Text("Diagnostics")
   18:                     .font(.title2.weight(.bold))
   19:                     .accessibilityIdentifier("detail-diagnostics")
   20:                 Spacer(minLength: 12)
   21:                 Text(model.telemetryModeLabel)
   22:                     .font(.caption)
   23:                     .foregroundStyle(.secondary)
   24:                 Button("Refresh log") {
   25:                     model.refreshDiagnosticsPreview()
   26:                 }
   27:                 Button("Export JSON + Markdown…") {
   28:                     model.exportDiagnostics()
```

### `Sources/ForgeConductorApp/Views/MCPServersView.swift:165` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  158:                 .frame(width: 7, height: 7)
  159:             Text(title)
  160:                 .foregroundStyle(.secondary)
  161:         }
  162:     }
  163: 
  164:     private func serverCard(_ s: MCPServerCard) -> some View {
  165:         let tone = TelemetryHealth.tone(for: s.health)
  166:         let color = tone.color
  167: 
  168:         return VStack(alignment: .leading, spacing: 10) {
  169:             HStack(spacing: 8) {
  170:                 Text(serverDisplayName(s))
  171:                     .font(.headline)
  172:                     .lineLimit(1)
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:52` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   45:                         .font(.caption)
   46:                         .foregroundStyle(.orange)
   47:                         .accessibilityIdentifier("manager-version-mismatch")
   48:                 }
   49:                 LabeledContent("Home", value: model.homePath)
   50:                 LabeledContent("Product", value: ForgeApp.productName)
   51:                 if let updated = model.updated {
   52:                     LabeledContent("Host telemetry", value: model.telemetryModeLabel)
   53:                     LabeledContent("Last host sample", value: updated.formatted())
   54:                     LabeledContent("Dashboard HTML poll", value: "\(model.setRefresh)s (not host telemetry)")
   55:                 }
   56:             }
   57: 
   58:             Section("Settings") {
   59:                 TextField("Dashboard host", text: $model.setHost)
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:53` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   46:                         .foregroundStyle(.orange)
   47:                         .accessibilityIdentifier("manager-version-mismatch")
   48:                 }
   49:                 LabeledContent("Home", value: model.homePath)
   50:                 LabeledContent("Product", value: ForgeApp.productName)
   51:                 if let updated = model.updated {
   52:                     LabeledContent("Host telemetry", value: model.telemetryModeLabel)
   53:                     LabeledContent("Last host sample", value: updated.formatted())
   54:                     LabeledContent("Dashboard HTML poll", value: "\(model.setRefresh)s (not host telemetry)")
   55:                 }
   56:             }
   57: 
   58:             Section("Settings") {
   59:                 TextField("Dashboard host", text: $model.setHost)
   60:                 TextField("Dashboard port", value: $model.setPort, format: .number)
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:54` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   47:                         .accessibilityIdentifier("manager-version-mismatch")
   48:                 }
   49:                 LabeledContent("Home", value: model.homePath)
   50:                 LabeledContent("Product", value: ForgeApp.productName)
   51:                 if let updated = model.updated {
   52:                     LabeledContent("Host telemetry", value: model.telemetryModeLabel)
   53:                     LabeledContent("Last host sample", value: updated.formatted())
   54:                     LabeledContent("Dashboard HTML poll", value: "\(model.setRefresh)s (not host telemetry)")
   55:                 }
   56:             }
   57: 
   58:             Section("Settings") {
   59:                 TextField("Dashboard host", text: $model.setHost)
   60:                 TextField("Dashboard port", value: $model.setPort, format: .number)
   61:                 TextField("UI refresh (sec)", value: $model.setRefresh, format: .number)
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:75` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   68:                     Button("Save settings") { model.saveSettings() }
   69:                         .buttonStyle(.borderedProminent)
   70:                 }
   71:                 .padding(.vertical, 2)
   72:             }
   73: 
   74:             Section("Maintenance") {
   75:                 Toggle("Auto-refresh telemetry", isOn: $model.autoRefresh)
   76:                 Button("Refresh telemetry now") { model.refresh(force: true) }
   77:                 Button("Prune stale presence") { model.prunePresence() }
   78:                 Button("Prune idle sessions") { model.pruneSessions() }
   79:                 Button("Run doctor") {
   80:                     if let d = model.runDoctor() {
   81:                         doctorOK = d.ok
   82:                         let lines = d.checks.map { c in
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:76` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   69:                         .buttonStyle(.borderedProminent)
   70:                 }
   71:                 .padding(.vertical, 2)
   72:             }
   73: 
   74:             Section("Maintenance") {
   75:                 Toggle("Auto-refresh telemetry", isOn: $model.autoRefresh)
   76:                 Button("Refresh telemetry now") { model.refresh(force: true) }
   77:                 Button("Prune stale presence") { model.prunePresence() }
   78:                 Button("Prune idle sessions") { model.pruneSessions() }
   79:                 Button("Run doctor") {
   80:                     if let d = model.runDoctor() {
   81:                         doctorOK = d.ok
   82:                         let lines = d.checks.map { c in
   83:                             "\(c.ok ? "OK" : "FAIL")  \(c.name): \(c.detail)"
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:89` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   82:                         let lines = d.checks.map { c in
   83:                             "\(c.ok ? "OK" : "FAIL")  \(c.name): \(c.detail)"
   84:                         }
   85:                         doctorJSON = ([
   86:                             "ok=\(d.ok)  version=\(d.version)",
   87:                             "home=\(d.home)",
   88:                             "binary=\(d.binaryInstalled ? "yes" : "no")  \(d.binaryPath)",
   89:                             "telemetry=\(d.telemetry.runtime)",
   90:                             "",
   91:                         ] + lines).joined(separator: "\n")
   92:                     } else {
   93:                         doctorJSON = "doctor failed"
   94:                         doctorOK = false
   95:                     }
   96:                 }
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:112` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  105:                             .frame(maxWidth: .infinity, alignment: .leading)
  106:                     }
  107:                     .frame(minHeight: 160)
  108:                 }
  109:             }
  110: 
  111:             Section("Notes") {
  112:                 Text("Start/Stop toggles operational service_active. Restart rebinds the HTTP control plane. Product path: Deploy to LM Studio on the LM Studio MCP tab; configuration, host reload, and both connection checks are automatic. Telemetry is a continuous native stream (~30 Hz host sampling + SSE /api/stream), not multi-second snapshots. Diagnostics export is on the Diagnostics tab.")
  113:                     .font(.caption)
  114:                     .foregroundStyle(.secondary)
  115:             }
  116:         }
  117:         .formStyle(.grouped)
  118:         .padding(16)
  119:         .onAppear { model.loadSettingsFromConfig() }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // RigDashboardView.swift
    2: // What: Builds the real-time host and orchestration instrument panel.
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:11` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
   12: struct RigDashboardView: View {
   13:     @EnvironmentObject private var model: AppModel
   14: 
   15:     var body: some View {
   16:         // TimelineView drives UI at animation/display cadence against the continuous engine.
   17:         TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !model.autoRefresh)) { _ in
   18:             rigContent
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:36` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   29:                     ram: model.historyRAM,
   30:                     gpu: model.historyGPU
   31:                 )
   32:                 .frame(height: 168)
   33:                 .clipShape(RoundedRectangle(cornerRadius: 10))
   34:                 .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.cyan.opacity(0.35), lineWidth: 1))
   35:                 .overlay(alignment: .topLeading) {
   36:                     Text("LOAD TRACE  ·  Metal  ·  REAL-TIME  ·  \(model.telemetryModeLabel)")
   37:                         .font(.system(size: 10, weight: .semibold, design: .monospaced))
   38:                         .foregroundStyle(.cyan.opacity(0.8))
   39:                         .padding(10)
   40:                 }
   41: 
   42:                 LazyVGrid(
   43:                     columns: [GridItem(.adaptive(minimum: 260), spacing: 14, alignment: .top)],
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:103` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   96:                 MetalStatusPill(
   97:                     text: "LINK",
   98:                     tone: model.lastError == nil ? .healthy : .failure,
   99:                     fraction: 1
  100:                 )
  101:                 MetalStatusPill(
  102:                     text: "ORCH \(model.orchestration?.healthLabel ?? "—")",
  103:                     tone: TelemetryHealth.tone(for: model.orchestration?.health),
  104:                     fraction: model.orchestration?.health == "ok" ? 1 : 0.25
  105:                 )
  106:                 MetalStatusPill(
  107:                     text: "MCP \(model.mcpServerCards.filter(\.live).count)/\(model.mcpServerCards.count)",
  108:                     tone: mcpHeaderTone,
  109:                     fraction: model.mcpServerCards.isEmpty ? 0 : Double(model.mcpServerCards.filter(\.live).count) / Double(model.mcpServerCards.count)
  110:                 )
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:128` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  121: 
  122:     // MARK: Sys strip — Metal bars only
  123: 
  124:     private var sysStrip: some View {
  125:         let s = model.sysStrip
  126:         let gpuValue = s.gpuPercent.map { String(format: "%.1f%%", $0) } ?? "—"
  127:         let gpuFraction = s.gpuPercent.map { $0 / 100 } ?? 0
  128:         let gpuMetadata = s.gpuPercent == nil ? "telemetry unavailable" : "Metal IOKit"
  129:         let gpuTint: Color = s.gpuPercent == nil ? .gray : .green
  130:         return LazyVGrid(columns: [GridItem(.adaptive(minimum: 130), spacing: 12)], spacing: 12) {
  131:             sysCard("CPU", value: String(format: "%.1f%%", s.cpuPercent), meta: s.cpuBrand, frac: s.cpuPercent / 100, tint: .cyan)
  132:             sysCard("FREQ", value: s.freqMHz.map { "\($0)" } ?? "—", meta: "MHz · load \(String(format: "%.2f", s.loadM1))", frac: min((Double(s.freqMHz ?? 0) / 4000), 1), tint: .mint)
  133:             sysCard("RAM", value: String(format: "%.1f%%", s.ramPercent), meta: "pressure", frac: s.ramPercent / 100, tint: .orange)
  134:             sysCard(
  135:                 "GPU",
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:350` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  343:                         tone: (o?.serveCount ?? 0) > 0 ? .healthy : .informational
  344:                     ),
  345:                     OrchestrationCardState(
  346:                         title: "STATUS",
  347:                         state: o?.healthLabel ?? "—",
  348:                         detail: mode,
  349:                         fraction: o?.health == "ok" ? 1 : 0.2,
  350:                         tone: TelemetryHealth.tone(for: o?.health)
  351:                     ),
  352:                 ]
  353:             }
  354:             return [
  355:                 OrchestrationCardState(
  356:                     title: "STATUS",
  357:                     state: o?.healthLabel ?? "—",
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:360` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  353:             }
  354:             return [
  355:                 OrchestrationCardState(
  356:                     title: "STATUS",
  357:                     state: o?.healthLabel ?? "—",
  358:                     detail: mode,
  359:                     fraction: o?.health == "ok" ? 1 : 0.2,
  360:                     tone: TelemetryHealth.tone(for: o?.health)
  361:                 ),
  362:             ]
  363:         }()
  364: 
  365:         return panel("ORCHESTRATION", meta: "\(o?.healthLabel ?? "—") · \(mode)") {
  366:             LazyVGrid(columns: [GridItem(.adaptive(minimum: 110), spacing: 10)], spacing: 10) {
  367:                 ForEach(Array(cards.enumerated()), id: \.offset) { _, c in
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:626` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  619:     // MARK: Helpers
  620: 
  621:     private struct OrchestrationCardState {
  622:         var title: String
  623:         var state: String
  624:         var detail: String
  625:         var fraction: Double
  626:         var tone: TelemetryStatusTone
  627:     }
  628: 
  629:     private var mcpHeaderTone: TelemetryStatusTone {
  630:         TelemetryStatusTone.mostSevere(
  631:             model.mcpServerCards.map { TelemetryHealth.tone(for: $0.health) }
  632:         )
  633:     }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:629` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  622:         var title: String
  623:         var state: String
  624:         var detail: String
  625:         var fraction: Double
  626:         var tone: TelemetryStatusTone
  627:     }
  628: 
  629:     private var mcpHeaderTone: TelemetryStatusTone {
  630:         TelemetryStatusTone.mostSevere(
  631:             model.mcpServerCards.map { TelemetryHealth.tone(for: $0.health) }
  632:         )
  633:     }
  634: 
  635:     private func panel<Content: View>(_ title: String, meta: String, @ViewBuilder content: () -> Content) -> some View {
  636:         VStack(alignment: .leading, spacing: 10) {
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:630` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  623:         var state: String
  624:         var detail: String
  625:         var fraction: Double
  626:         var tone: TelemetryStatusTone
  627:     }
  628: 
  629:     private var mcpHeaderTone: TelemetryStatusTone {
  630:         TelemetryStatusTone.mostSevere(
  631:             model.mcpServerCards.map { TelemetryHealth.tone(for: $0.health) }
  632:         )
  633:     }
  634: 
  635:     private func panel<Content: View>(_ title: String, meta: String, @ViewBuilder content: () -> Content) -> some View {
  636:         VStack(alignment: .leading, spacing: 10) {
  637:             HStack(spacing: 10) {
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:631` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  624:         var detail: String
  625:         var fraction: Double
  626:         var tone: TelemetryStatusTone
  627:     }
  628: 
  629:     private var mcpHeaderTone: TelemetryStatusTone {
  630:         TelemetryStatusTone.mostSevere(
  631:             model.mcpServerCards.map { TelemetryHealth.tone(for: $0.health) }
  632:         )
  633:     }
  634: 
  635:     private func panel<Content: View>(_ title: String, meta: String, @ViewBuilder content: () -> Content) -> some View {
  636:         VStack(alignment: .leading, spacing: 10) {
  637:             HStack(spacing: 10) {
  638:                 Text("▸ \(title)")
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:662` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  655:         .clipped()
  656:         .background(RoundedRectangle(cornerRadius: 10).fill(Color.white.opacity(0.035)))
  657:         .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.cyan.opacity(0.15), lineWidth: 1))
  658:         .clipShape(RoundedRectangle(cornerRadius: 10))
  659:     }
  660: 
  661:     private func healthColor(_ h: String) -> Color {
  662:         TelemetryHealth.tone(for: h).color
  663:     }
  664: 
  665:     private func auditStatusColor(_ status: String) -> Color {
  666:         switch AuditOutcome(status: status) {
  667:         case .success:
  668:             return .green
  669:         case .operationalError:
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:1` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryDashboardView.swift
    2: // What: Presents the typed system-telemetry overview outside the dense rig layout.
    3: // How: It derives native cards and charts from AppModel's latest composed frame.
    4: // Why: A separate module supports focused inspection while reusing the same data owner.
    5: 
    6: import SwiftUI
    7: import ForgeConductorCore
    8: 
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryDashboardView.swift
    2: // What: Presents the typed system-telemetry overview outside the dense rig layout.
    3: // How: It derives native cards and charts from AppModel's latest composed frame.
    4: // Why: A separate module supports focused inspection while reusing the same data owner.
    5: 
    6: import SwiftUI
    7: import ForgeConductorCore
    8: 
    9: /// Presents the general-purpose telemetry dashboard from typed, observable metrics.
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:9` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    2: // What: Presents the typed system-telemetry overview outside the dense rig layout.
    3: // How: It derives native cards and charts from AppModel's latest composed frame.
    4: // Why: A separate module supports focused inspection while reusing the same data owner.
    5: 
    6: import SwiftUI
    7: import ForgeConductorCore
    8: 
    9: /// Presents the general-purpose telemetry dashboard from typed, observable metrics.
   10: ///
   11: /// It composes reusable cards and Metal charts but does not collect data itself;
   12: /// `AppModel` owns sampling and publishes a coherent frame for the entire view.
   13: struct TelemetryDashboardView: View {
   14:     @EnvironmentObject private var model: AppModel
   15: 
   16:     var body: some View {
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:13` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    6: import SwiftUI
    7: import ForgeConductorCore
    8: 
    9: /// Presents the general-purpose telemetry dashboard from typed, observable metrics.
   10: ///
   11: /// It composes reusable cards and Metal charts but does not collect data itself;
   12: /// `AppModel` owns sampling and publishes a coherent frame for the entire view.
   13: struct TelemetryDashboardView: View {
   14:     @EnvironmentObject private var model: AppModel
   15: 
   16:     var body: some View {
   17:         ScrollView {
   18:             VStack(alignment: .leading, spacing: 18) {
   19:                 header
   20:                 metricsStrip
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:20` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   13: struct TelemetryDashboardView: View {
   14:     @EnvironmentObject private var model: AppModel
   15: 
   16:     var body: some View {
   17:         ScrollView {
   18:             VStack(alignment: .leading, spacing: 18) {
   19:                 header
   20:                 metricsStrip
   21:                 MultiSeriesLoadChart(
   22:                     cpu: model.historyCPU,
   23:                     ram: model.historyRAM,
   24:                     gpu: model.historyGPU
   25:                 )
   26:                     .frame(height: 160)
   27:                     .clipShape(RoundedRectangle(cornerRadius: 12))
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:45` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   38:             .padding(20)
   39:         }
   40:     }
   41: 
   42:     private var header: some View {
   43:         HStack(spacing: 12) {
   44:             VStack(alignment: .leading, spacing: 4) {
   45:                 Text("TELEMETRY")
   46:                     .font(.system(.title2, design: .rounded).weight(.bold))
   47:                     .foregroundStyle(Color.cyan)
   48:                 Text(model.hostName)
   49:                     .font(.caption)
   50:                     .foregroundStyle(.secondary)
   51:             }
   52:             Spacer(minLength: 16)
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:55` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   48:                 Text(model.hostName)
   49:                     .font(.caption)
   50:                     .foregroundStyle(.secondary)
   51:             }
   52:             Spacer(minLength: 16)
   53:             statusPill(
   54:                 text: model.orchestration?.healthLabel ?? "—",
   55:                 tone: TelemetryHealth.tone(for: model.orchestration?.health)
   56:             )
   57:         }
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:60` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   53:             statusPill(
   54:                 text: model.orchestration?.healthLabel ?? "—",
   55:                 tone: TelemetryHealth.tone(for: model.orchestration?.health)
   56:             )
   57:         }
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:62` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   55:                 tone: TelemetryHealth.tone(for: model.orchestration?.health)
   56:             )
   57:         }
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
   68:                 value: Double(model.agentCards.filter(\.live).count),
   69:                 unit: "live",
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:63` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   56:             )
   57:         }
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
   68:                 value: Double(model.agentCards.filter(\.live).count),
   69:                 unit: "live",
   70:                 tint: .mint,
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:64` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   57:         }
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
   68:                 value: Double(model.agentCards.filter(\.live).count),
   69:                 unit: "live",
   70:                 tint: .mint,
   71:                 asInt: true
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:65` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   58:     }
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
   68:                 value: Double(model.agentCards.filter(\.live).count),
   69:                 unit: "live",
   70:                 tint: .mint,
   71:                 asInt: true
   72:             )
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:66` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   59: 
   60:     private var metricsStrip: some View {
   61:         LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
   62:             metricCard("CPU", value: model.cpuPercent, unit: "%", tint: .cyan)
   63:             metricCard("RAM", value: model.ramPercent, unit: "%", tint: .orange)
   64:             metricCard("GPU", value: model.gpuPercent, unit: "%", tint: .green)
   65:             metricCard("MCP", value: Double(model.mcpServerCards.count), unit: "", tint: .purple, asInt: true)
   66:             metricCard(
   67:                 "AGENTS",
   68:                 value: Double(model.agentCards.filter(\.live).count),
   69:                 unit: "live",
   70:                 tint: .mint,
   71:                 asInt: true
   72:             )
   73:         }
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:76` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   69:                 unit: "live",
   70:                 tint: .mint,
   71:                 asInt: true
   72:             )
   73:         }
   74:     }
   75: 
   76:     private func metricCard(
   77:         _ title: String,
   78:         value: Double?,
   79:         unit: String,
   80:         tint: Color,
   81:         asInt: Bool = false
   82:     ) -> some View {
   83:         let resolvedValue = value ?? 0
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:205` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  198:             }
  199:         }
  200:         .padding(16)
  201:         .frame(maxWidth: .infinity, alignment: .leading)
  202:         .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
  203:     }
  204: 
  205:     private func statusPill(text: String, tone: TelemetryStatusTone) -> some View {
  206:         Text(text)
  207:             .font(.caption.weight(.bold))
  208:             .padding(.horizontal, 12)
  209:             .padding(.vertical, 6)
  210:             .background(Capsule().fill(tone.color.opacity(0.2)))
  211:             .foregroundStyle(tone.color)
  212:             .accessibilityValue(tone.rawValue)
```

### `Sources/ForgeConductorApp/Views/ToolsView.swift:73` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   66:                         }
   67:                         .fixedSize(horizontal: true, vertical: false)
   68:                         Text(t.status)
   69:                             .font(.caption.weight(.semibold))
   70:                             .foregroundStyle(statusColor(t.status))
   71:                         Text(t.healthLabel)
   72:                             .font(.caption2)
   73:                             .foregroundStyle(TelemetryHealth.tone(for: t.health).color)
   74:                             .padding(.horizontal, 8)
   75:                             .padding(.vertical, 3)
   76:                             .background(
   77:                                 Capsule()
   78:                                     .fill(TelemetryHealth.tone(for: t.health).color.opacity(0.1))
   79:                             )
   80:                         Text("\(t.events5m)/5m · \(t.events1h)/1h")
```

### `Sources/ForgeConductorApp/Views/ToolsView.swift:78` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   71:                         Text(t.healthLabel)
   72:                             .font(.caption2)
   73:                             .foregroundStyle(TelemetryHealth.tone(for: t.health).color)
   74:                             .padding(.horizontal, 8)
   75:                             .padding(.vertical, 3)
   76:                             .background(
   77:                                 Capsule()
   78:                                     .fill(TelemetryHealth.tone(for: t.health).color.opacity(0.1))
   79:                             )
   80:                         Text("\(t.events5m)/5m · \(t.events1h)/1h")
   81:                             .font(.system(.caption2, design: .monospaced))
   82:                             .foregroundStyle(.secondary)
   83:                     }
   84:                     .padding(.vertical, 4)
   85:                     .help(toolHelp(t))
```

### `Sources/ForgeConductorApp/Views/ToolsView.swift:102` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   95:         case "warm": return .orange
   96:         default: return .secondary
   97:         }
   98:     }
   99: 
  100:     private func toolHelp(_ tool: ToolCard) -> String {
  101:         let healthDetail: String
  102:         switch TelemetryHealth.tone(for: tool.health) {
  103:         case .healthy:
  104:             healthDetail = "Operational error rate is below the alert threshold."
  105:         case .caution:
  106:             healthDetail = "Recent operational errors exceed the warning threshold."
  107:         case .failure:
  108:             healthDetail = "Recent operational errors exceed the failure threshold."
  109:         case .informational:
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:162` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  155:         let result = try app.doctor()
  156:         print(try JSONSupport.string(from: result))
  157:         if result["ok"] as? Bool != true { exit(1) }
  158:     }
  159: 
  160:     static func cmdStatus(_ args: [String]) throws {
  161:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  162:         var snap = try app.statusSnapshot()
  163:         if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
  164:             snap["manager_pid"] = Int(pid)
  165:             snap["manager_running"] = true
  166:             if let data = try? Data(contentsOf: app.paths.managerState),
  167:                let state = try? JSONSupport.object(from: data) {
  168:                 snap["manager_state"] = state
  169:             }
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // ContextContinuityService.swift
    2: // What: Owns context handoff packets and agent-continuity snapshots for chat resume.
    3: // How: Builds packets from tool args + open agent sessions, persists SQLite/files,
    4: // projects current-task.md, and returns MCP-ready payloads with resume seeds.
    5: // Why: Stdio MCP clients (LM Studio) need durable cross-chat state without HTTP.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:385` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  378:             base.keyFiles = f.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
  379:                 .filter { !$0.isEmpty }
  380:         }
  381:         if let decisions = arguments["decisions"] as? [String] {
  382:             base.decisions = decisions
  383:         }
  384: 
  385:         let currentAgents = try snapshotAgents(clientID: clientID)
  386:         if existingID != nil {
  387:             // A resumed chat may checkpoint the recovered packet before it has
  388:             // reattached every listed agent. Keep prior snapshots whose durable
  389:             // sessions are still open, replacing them as the new client reattaches.
  390:             base.agents = try mergeOpenAgentSnapshots(
  391:                 prior: base.agents,
  392:                 current: currentAgents
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:388` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  381:         if let decisions = arguments["decisions"] as? [String] {
  382:             base.decisions = decisions
  383:         }
  384: 
  385:         let currentAgents = try snapshotAgents(clientID: clientID)
  386:         if existingID != nil {
  387:             // A resumed chat may checkpoint the recovered packet before it has
  388:             // reattached every listed agent. Keep prior snapshots whose durable
  389:             // sessions are still open, replacing them as the new client reattaches.
  390:             base.agents = try mergeOpenAgentSnapshots(
  391:                 prior: base.agents,
  392:                 current: currentAgents
  393:             )
  394:         } else {
  395:             base.agents = currentAgents
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:390` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  383:         }
  384: 
  385:         let currentAgents = try snapshotAgents(clientID: clientID)
  386:         if existingID != nil {
  387:             // A resumed chat may checkpoint the recovered packet before it has
  388:             // reattached every listed agent. Keep prior snapshots whose durable
  389:             // sessions are still open, replacing them as the new client reattaches.
  390:             base.agents = try mergeOpenAgentSnapshots(
  391:                 prior: base.agents,
  392:                 current: currentAgents
  393:             )
  394:         } else {
  395:             base.agents = currentAgents
  396:         }
  397: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:413` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  406:         }
  407:         if explicitResumeSeed == nil, !base.resumeSeedIsCustom {
  408:             base.resumeSeed = base.defaultResumeSeed()
  409:         }
  410:         return base
  411:     }
  412: 
  413:     private func snapshotAgents(clientID: ClientID) throws -> [AgentContinuitySnapshot] {
  414:         let open = try store.sessionList().filter(\.status.isOpen)
  415: 
  416:         // Deduplicate by session id
  417:         var seen = Set<String>()
  418:         var snaps: [AgentContinuitySnapshot] = []
  419:         for s in open where s.clientID == clientID {
  420:             if seen.contains(s.id.rawValue) { continue }
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:418` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  411:     }
  412: 
  413:     private func snapshotAgents(clientID: ClientID) throws -> [AgentContinuitySnapshot] {
  414:         let open = try store.sessionList().filter(\.status.isOpen)
  415: 
  416:         // Deduplicate by session id
  417:         var seen = Set<String>()
  418:         var snaps: [AgentContinuitySnapshot] = []
  419:         for s in open where s.clientID == clientID {
  420:             if seen.contains(s.id.rawValue) { continue }
  421:             seen.insert(s.id.rawValue)
  422: 
  423:             var goal = ""
  424:             var cwd: String?
  425:             if let body = try? store.memoryGet(key: "agent_run/\(s.id.rawValue)"),
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:437` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  430:             }
  431:             if goal.isEmpty, let binding = sessions.binding(for: clientID), binding.sessionID == s.id {
  432:                 goal = binding.goal
  433:                 cwd = binding.cwd
  434:             }
  435: 
  436:             snaps.append(
  437:                 AgentContinuitySnapshot(
  438:                     sessionID: s.id.rawValue,
  439:                     agentID: s.agentID,
  440:                     goal: goal,
  441:                     cwd: cwd,
  442:                     status: s.status.rawValue,
  443:                     updatedAt: ISO8601.string(from: s.updatedAt),
  444:                     resumeHint:
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:453` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  446:                         "if stale, agent_run_complete then agent_run_start with same goal/cwd"
  447:                 )
  448:             )
  449:         }
  450:         return snaps
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:454` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  447:                 )
  448:             )
  449:         }
  450:         return snaps
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:455` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  448:             )
  449:         }
  450:         return snaps
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:456` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  449:         }
  450:         return snaps
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:457` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  450:         return snaps
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:458` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  451:     }
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:459` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  452: 
  453:     private func mergeOpenAgentSnapshots(
  454:         prior: [AgentContinuitySnapshot],
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:462` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  455:         current: [AgentContinuitySnapshot]
  456:     ) throws -> [AgentContinuitySnapshot] {
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:464` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  457:         var currentBySession: [String: AgentContinuitySnapshot] = [:]
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:465` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  458:         for snapshot in current {
  459:             currentBySession[snapshot.sessionID] = snapshot
  460:         }
  461: 
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:469` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
  473:         }
  474:         return merged
  475:     }
  476: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:471` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
  473:         }
  474:         return merged
  475:     }
  476: 
  477:     private struct PersistenceOutcome {
  478:         var packet: HandoffPacket
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:472` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
  473:         }
  474:         return merged
  475:     }
  476: 
  477:     private struct PersistenceOutcome {
  478:         var packet: HandoffPacket
  479:         var projectionWarning: String?
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:233` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  226:             current.progressCount = 0
  227:             current.lastCheckpointCount = 0
  228:             current.lastHandoffCount = 0
  229:             state[clientID.rawValue] = current
  230:         }
  231:     }
  232: 
  233:     public func snapshot(for clientID: ClientID) -> [String: Any] {
  234:         lock.lock()
  235:         let current = state[clientID.rawValue]
  236:         lock.unlock()
  237:         return [
  238:             "enabled": true,
  239:             "checkpoint_every_tools": Self.checkpointEveryTools,
  240:             "handoff_every_tools": Self.handoffEveryTools,
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // ForgeApp.swift
    2: // What: Serves as the Core composition root and owner of long-lived services.
    3: // How: Bootstrap constructs paths, storage, configuration, telemetry, catalogs,
    4: // tool packs, authorization, and routing with explicit dependency injection.
    5: // Why: Central composition makes modules replaceable without service-locator globals.
    6: 
    7: import Foundation
    8: 
    9: /// Composition root for the Forge-Conductor application.
   10: /// All layers hang off this object; suitable for CLI, MCP stdio, and future Xcode shell.
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:37` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   30:         let authorization = ToolAuthorizationService(
   31:             paths: paths,
   32:             config: config,
   33:             workspace: continuityAutomation
   34:         )
   35:         return ToolRouter(app: self, authorization: authorization)
   36:     }()
   37:     public private(set) lazy var telemetry = TelemetryService(
   38:         paths: paths,
   39:         store: store,
   40:         catalog: catalog,
   41:         toolNames: { [weak self] in self?.tools.toolNames ?? [] }
   42:     )
   43: 
   44:     private init(
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:129` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  122:             continuityAutomation: continuityAutomation,
  123:             clock: clock,
  124:             lmStudioDeploy: deploy
  125:         )
  126:         // Resolve lazy services on the bootstrap thread. No caller can observe
  127:         // a partially wired application graph.
  128:         let toolCount = app.tools.toolNames.count
  129:         _ = app.telemetry
  130:         // Continuous native metrics — skip in ephemeral test homes.
  131:         let isTemp = paths.home.path.contains("/T/") || paths.home.path.contains("forge-test")
  132:             || paths.home.path.contains("forge-native") || paths.home.path.contains("forge-doc")
  133:             || paths.home.path.contains("forge-contract") || paths.home.path.contains("forge-dash")
  134:         if !isTemp {
  135:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
  136:         }
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:130` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  123:             clock: clock,
  124:             lmStudioDeploy: deploy
  125:         )
  126:         // Resolve lazy services on the bootstrap thread. No caller can observe
  127:         // a partially wired application graph.
  128:         let toolCount = app.tools.toolNames.count
  129:         _ = app.telemetry
  130:         // Continuous native metrics — skip in ephemeral test homes.
  131:         let isTemp = paths.home.path.contains("/T/") || paths.home.path.contains("forge-test")
  132:             || paths.home.path.contains("forge-native") || paths.home.path.contains("forge-doc")
  133:             || paths.home.path.contains("forge-contract") || paths.home.path.contains("forge-dash")
  134:         if !isTemp {
  135:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
  136:         }
  137: 
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:135` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  128:         let toolCount = app.tools.toolNames.count
  129:         _ = app.telemetry
  130:         // Continuous native metrics — skip in ephemeral test homes.
  131:         let isTemp = paths.home.path.contains("/T/") || paths.home.path.contains("forge-test")
  132:             || paths.home.path.contains("forge-native") || paths.home.path.contains("forge-doc")
  133:             || paths.home.path.contains("forge-contract") || paths.home.path.contains("forge-dash")
  134:         if !isTemp {
  135:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
  136:         }
  137: 
  138:         diagnostics.info("app_bootstrap", [
  139:             "version": version,
  140:             "home": paths.home.path,
  141:             "agents": "\(catalog.all().count)",
  142:             "telemetry": "continuous-native",
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:142` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  135:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
  136:         }
  137: 
  138:         diagnostics.info("app_bootstrap", [
  139:             "version": version,
  140:             "home": paths.home.path,
  141:             "agents": "\(catalog.all().count)",
  142:             "telemetry": "continuous-native",
  143:             "sample_hz_target": "\(Int(RealtimeMetricsEngine.defaultTargetHz))",
  144:             "tools": "\(toolCount)",
  145:         ], category: .bootstrap)
  146:         return app
  147:     }
  148: 
  149:     /// Close durable resources (SQLite) before deleting a temp home in tests.
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:143` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  136:         }
  137: 
  138:         diagnostics.info("app_bootstrap", [
  139:             "version": version,
  140:             "home": paths.home.path,
  141:             "agents": "\(catalog.all().count)",
  142:             "telemetry": "continuous-native",
  143:             "sample_hz_target": "\(Int(RealtimeMetricsEngine.defaultTargetHz))",
  144:             "tools": "\(toolCount)",
  145:         ], category: .bootstrap)
  146:         return app
  147:     }
  148: 
  149:     /// Close durable resources (SQLite) before deleting a temp home in tests.
  150:     public func shutdown() {
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:151` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  144:             "tools": "\(toolCount)",
  145:         ], category: .bootstrap)
  146:         return app
  147:     }
  148: 
  149:     /// Close durable resources (SQLite) before deleting a temp home in tests.
  150:     public func shutdown() {
  151:         telemetry.stopBackgroundRefresh()
  152:         store.close()
  153:     }
  154: 
  155:     public func statusSnapshotModel() throws -> AppStatusSnapshot {
  156:         let open = try store.sessionList().filter(\.status.isOpen)
  157:         let presence = try store.presenceRecords()
  158:         let auditRows = try audit.recent(limit: 20)
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:155` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  148: 
  149:     /// Close durable resources (SQLite) before deleting a temp home in tests.
  150:     public func shutdown() {
  151:         telemetry.stopBackgroundRefresh()
  152:         store.close()
  153:     }
  154: 
  155:     public func statusSnapshotModel() throws -> AppStatusSnapshot {
  156:         let open = try store.sessionList().filter(\.status.isOpen)
  157:         let presence = try store.presenceRecords()
  158:         let auditRows = try audit.recent(limit: 20)
  159:         let cfg = config.model
  160:         return AppStatusSnapshot(
  161:             ok: true,
  162:             version: Self.version,
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:160` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  153:     }
  154: 
  155:     public func statusSnapshotModel() throws -> AppStatusSnapshot {
  156:         let open = try store.sessionList().filter(\.status.isOpen)
  157:         let presence = try store.presenceRecords()
  158:         let auditRows = try audit.recent(limit: 20)
  159:         let cfg = config.model
  160:         return AppStatusSnapshot(
  161:             ok: true,
  162:             version: Self.version,
  163:             product: Self.productName,
  164:             runtime: "swift",
  165:             home: paths.home.path,
  166:             store: paths.storeSQLite.path,
  167:             agents: catalog.all().map(\.id),
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:175` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  168:             agentCount: catalog.all().count,
  169:             openSessions: open.map(AgentSessionSummary.init(from:)),
  170:             openSessionCount: open.count,
  171:             presence: presence,
  172:             presenceCount: presence.count,
  173:             recentAudit: auditRows.map(AuditEventSummary.init(from:)),
  174:             tools: tools.toolNames,
  175:             telemetry: telemetry.health(),
  176:             dashboardHost: cfg.dashboard.host,
  177:             dashboardPort: cfg.dashboard.port,
  178:             pid: ProcessInfo.processInfo.processIdentifier
  179:         )
  180:     }
  181: 
  182:     /// HTTP / CLI edge.
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:183` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  176:             dashboardHost: cfg.dashboard.host,
  177:             dashboardPort: cfg.dashboard.port,
  178:             pid: ProcessInfo.processInfo.processIdentifier
  179:         )
  180:     }
  181: 
  182:     /// HTTP / CLI edge.
  183:     public func statusSnapshot() throws -> [String: Any] {
  184:         try statusSnapshotModel().asDictionary()
  185:     }
  186: 
  187:     public func doctorModel() throws -> DoctorReport {
  188:         var checks: [DoctorCheck] = []
  189:         var ok = true
  190: 
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:184` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  177:             dashboardPort: cfg.dashboard.port,
  178:             pid: ProcessInfo.processInfo.processIdentifier
  179:         )
  180:     }
  181: 
  182:     /// HTTP / CLI edge.
  183:     public func statusSnapshot() throws -> [String: Any] {
  184:         try statusSnapshotModel().asDictionary()
  185:     }
  186: 
  187:     public func doctorModel() throws -> DoctorReport {
  188:         var checks: [DoctorCheck] = []
  189:         var ok = true
  190: 
  191:         func check(_ name: String, _ pass: Bool, _ detail: String, hard: Bool = true) {
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:207` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  200:             _ = try store.sessionList()
  201:             check("sqlite_query", true, "session list ok")
  202:         } catch {
  203:             check("sqlite_query", false, "\(error)")
  204:         }
  205:         check("git_available", ProcessRunner.which("git") != nil, ProcessRunner.which("git") ?? "missing")
  206: 
  207:         let tel = telemetry.health()
  208:         check("telemetry_native", tel.ok, "swift SystemCollector+ForgeCollector")
  209:         check(
  210:             "telemetry_runtime",
  211:             tel.runtime == "swift-native" || tel.runtime == "swift-native-realtime",
  212:             tel.runtime
  213:         )
  214: 
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:208` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  201:             check("sqlite_query", true, "session list ok")
  202:         } catch {
  203:             check("sqlite_query", false, "\(error)")
  204:         }
  205:         check("git_available", ProcessRunner.which("git") != nil, ProcessRunner.which("git") ?? "missing")
  206: 
  207:         let tel = telemetry.health()
  208:         check("telemetry_native", tel.ok, "swift SystemCollector+ForgeCollector")
  209:         check(
  210:             "telemetry_runtime",
  211:             tel.runtime == "swift-native" || tel.runtime == "swift-native-realtime",
  212:             tel.runtime
  213:         )
  214: 
  215:         var snapshotOK = false
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:210` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  203:             check("sqlite_query", false, "\(error)")
  204:         }
  205:         check("git_available", ProcessRunner.which("git") != nil, ProcessRunner.which("git") ?? "missing")
  206: 
  207:         let tel = telemetry.health()
  208:         check("telemetry_native", tel.ok, "swift SystemCollector+ForgeCollector")
  209:         check(
  210:             "telemetry_runtime",
  211:             tel.runtime == "swift-native" || tel.runtime == "swift-native-realtime",
  212:             tel.runtime
  213:         )
  214: 
  215:         var snapshotOK = false
  216:         var snapDetail = "failed"
  217:         do {
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:215` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  208:         check("telemetry_native", tel.ok, "swift SystemCollector+ForgeCollector")
  209:         check(
  210:             "telemetry_runtime",
  211:             tel.runtime == "swift-native" || tel.runtime == "swift-native-realtime",
  212:             tel.runtime
  213:         )
  214: 
  215:         var snapshotOK = false
  216:         var snapDetail = "failed"
  217:         do {
  218:             let snap = try telemetry.snapshot(force: true)
  219:             let missing = TelemetryContract.validate(snapshot: snap)
  220:             snapshotOK = missing.isEmpty
  221:             snapDetail = missing.isEmpty ? "native contract ok" : "missing: \(missing.joined(separator: ", "))"
  222:         } catch {
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:218` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  211:             tel.runtime == "swift-native" || tel.runtime == "swift-native-realtime",
  212:             tel.runtime
  213:         )
  214: 
  215:         var snapshotOK = false
  216:         var snapDetail = "failed"
  217:         do {
  218:             let snap = try telemetry.snapshot(force: true)
  219:             let missing = TelemetryContract.validate(snapshot: snap)
  220:             snapshotOK = missing.isEmpty
  221:             snapDetail = missing.isEmpty ? "native contract ok" : "missing: \(missing.joined(separator: ", "))"
  222:         } catch {
  223:             snapDetail = "\(error)"
  224:         }
  225:         check("telemetry_snapshot", snapshotOK, snapDetail)
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:219` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  212:             tel.runtime
  213:         )
  214: 
  215:         var snapshotOK = false
  216:         var snapDetail = "failed"
  217:         do {
  218:             let snap = try telemetry.snapshot(force: true)
  219:             let missing = TelemetryContract.validate(snapshot: snap)
  220:             snapshotOK = missing.isEmpty
  221:             snapDetail = missing.isEmpty ? "native contract ok" : "missing: \(missing.joined(separator: ", "))"
  222:         } catch {
  223:             snapDetail = "\(error)"
  224:         }
  225:         check("telemetry_snapshot", snapshotOK, snapDetail)
  226: 
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:220` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  213:         )
  214: 
  215:         var snapshotOK = false
  216:         var snapDetail = "failed"
  217:         do {
  218:             let snap = try telemetry.snapshot(force: true)
  219:             let missing = TelemetryContract.validate(snapshot: snap)
  220:             snapshotOK = missing.isEmpty
  221:             snapDetail = missing.isEmpty ? "native contract ok" : "missing: \(missing.joined(separator: ", "))"
  222:         } catch {
  223:             snapDetail = "\(error)"
  224:         }
  225:         check("telemetry_snapshot", snapshotOK, snapDetail)
  226: 
  227:         let installer = ManagerInstaller(app: self)
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:225` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  218:             let snap = try telemetry.snapshot(force: true)
  219:             let missing = TelemetryContract.validate(snapshot: snap)
  220:             snapshotOK = missing.isEmpty
  221:             snapDetail = missing.isEmpty ? "native contract ok" : "missing: \(missing.joined(separator: ", "))"
  222:         } catch {
  223:             snapDetail = "\(error)"
  224:         }
  225:         check("telemetry_snapshot", snapshotOK, snapDetail)
  226: 
  227:         let installer = ManagerInstaller(app: self)
  228:         let binPath = installer.installedBinaryURL.path
  229:         let binInstalled = FileManager.default.isExecutableFile(atPath: binPath)
  230:         check("swift_binary_install", binInstalled, binPath, hard: false)
  231: 
  232:         let binDir = installer.installedBinaryURL.deletingLastPathComponent()
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:256` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  249:         check("lm_studio_mcp_plugin", plug.isFullyInstalled, plug.detail, hard: false)
  250: 
  251:         return DoctorReport(
  252:             ok: ok,
  253:             version: Self.version,
  254:             home: paths.home.path,
  255:             checks: checks,
  256:             telemetry: tel,
  257:             binaryInstalled: binInstalled,
  258:             binaryPath: binPath
  259:         )
  260:     }
  261: 
  262:     /// HTTP / CLI edge.
  263:     public func doctor() throws -> [String: Any] {
```

### `Sources/ForgeConductorCore/Application/Tools/AgentToolPack.swift:89` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   82:             "agents": app.catalog.all().map(\.id),
   83:             "tools": app.tools.toolNames,
   84:             "memory_note_count": memoryCount,
   85:             "presence_count": presence.count,
   86:             "open_sessions": openSessions.count,
   87:             "open_session_ids": openSessions.map(\.id.rawValue),
   88:             "continuity": continuity,
   89:             "auto_continuity": app.continuityAutomation.snapshot(for: clientID),
   90:             "pid": ProcessInfo.processInfo.processIdentifier,
   91:         ])
   92:     }
   93: }
```

### `Sources/ForgeConductorCore/Application/Tools/ContinuityToolPack.swift:40` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   33:             var payload = try app.continuity.get(id: id, preferResumeReady: preferResume)
   34:             if payload["found"] as? Bool == true,
   35:                let packetObj = payload["packet"] as? [String: Any],
   36:                let packet = HandoffPacket.fromDictionary(packetObj) {
   37:                 app.continuityAutomation.adopt(clientID: clientID, packet: packet)
   38:                 app.continuityAutomation.clearBlock(clientID: clientID)
   39:                 payload["workspace_adopted"] = packet.cwd as Any
   40:                 payload["auto_continuity"] = app.continuityAutomation.snapshot(for: clientID)
   41:                 payload["context_budget_cleared"] = true
   42:             }
   43:             return ToolResult(ok: true, payload: payload)
   44:         case "context_list":
   45:             let limit = ToolArgHelpers.int(arguments, "limit") ?? 10
   46:             let payload = try app.continuity.list(limit: limit)
   47:             return ToolResult(ok: true, payload: payload)
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTML.swift:100` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   93: <body>
   94: <header>
   95:   <div>
   96:     <h1>Forge-Conductor · Web control (native app is primary)</h1>
   97:     <div class="meta" id="header-meta">loading…</div>
   98:   </div>
   99:   <div class="actions">
  100:     <a class="btn primary" href="/" id="telemetry-dash-link" style="text-decoration:none;display:inline-block">← Telemetry dashboard</a>
  101:     <span id="svc-pill" class="pill warn">…</span>
  102:     <button class="ok" id="btn-start" onclick="mgrStart()">Start</button>
  103:     <button class="danger" id="btn-stop" onclick="mgrStop()">Stop</button>
  104:     <button id="btn-restart" onclick="mgrRestart()">Restart</button>
  105:     <button class="primary" onclick="refreshAll()">Refresh</button>
  106:     <button onclick="pruneSessions()">Prune stale</button>
  107:     <button onclick="runDoctor()">Doctor</button>
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTTPRequest.swift:9` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    2: // What: Models, parses, and validates requests accepted by the loopback dashboard.
    3: // How: A bounded byte parser produces a typed request, then same-origin/host/content
    4: // policy is evaluated before any route can perform a state-changing operation.
    5: // Why: Protocol parsing and security policy remain independently testable from sockets.
    6: 
    7: import Foundation
    8: 
    9: /// A deliberately small HTTP/1.1 request model for the local telemetry server.
   10: /// The parser owns all size and syntax checks before routing sees a request.
   11: public struct DashboardHTTPRequest: Sendable, Equatable {
   12:     public var method: String
   13:     public var target: String
   14:     public var headers: [String: String]
   15:     public var body: Data
   16: 
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTTPRequest.swift:118` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  111:             target: String(requestParts[1]),
  112:             headers: headers,
  113:             body: body
  114:         ))
  115:     }
  116: }
  117: 
  118: /// Browser boundary for the dashboard. Telemetry reads remain local and simple;
  119: /// state-changing requests must be same-origin JSON. LM Studio never crosses
  120: /// this policy because its privileged connector is MCP over stdio.
  121: public enum DashboardRequestPolicy {
  122:     public static func rejection(
  123:         for request: DashboardHTTPRequest,
  124:         serverPort: UInt16
  125:     ) -> (status: Int, message: String)? {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:31` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   24:         lock.lock()
   25:         defer { lock.unlock() }
   26:         return error
   27:     }
   28: }
   29: 
   30: /// Loopback HTTP control surface: status, agents, sessions, audit, diagnostics, manager controls.
   31: /// Routing is delegated to modular route handlers (Telemetry / Manager / Operational).
   32: public final class DashboardServer: @unchecked Sendable {
   33:     private let app: ForgeApp
   34:     private let host: String
   35:     private let port: UInt16
   36:     private var listener: NWListener?
   37:     private let queue = DispatchQueue(label: "forge.dashboard", qos: .userInitiated)
   38:     private let lock = NSLock()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:235` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  228:                     return
  229:                 }
  230:                 try ManagerRoutes(manager: manager, http: http)
  231:                     .handle(method: m, path: path, body: body, connection: connection)
  232:                 return
  233:             }
  234: 
  235:             if pathOnly.hasPrefix("/api/snapshot") || pathOnly.hasPrefix("/api/live")
  236:                 || pathOnly.hasPrefix("/api/frame") || pathOnly.hasPrefix("/api/system")
  237:                 || pathOnly.hasPrefix("/api/forge") || pathOnly.hasPrefix("/api/stream")
  238:                 || pathOnly.hasPrefix("/api/health") || pathOnly.hasPrefix("/static/")
  239:                 || pathOnly == "/ping" {
  240:                 // Pass raw path (with query) so /api/stream?hz=20 can parse target rate.
  241:                 try TelemetryRoutes(app: app, http: http)
  242:                     .handle(method: m, path: rawPath.hasPrefix("/") ? rawPath : "/" + rawPath, connection: connection)
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:241` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  234: 
  235:             if pathOnly.hasPrefix("/api/snapshot") || pathOnly.hasPrefix("/api/live")
  236:                 || pathOnly.hasPrefix("/api/frame") || pathOnly.hasPrefix("/api/system")
  237:                 || pathOnly.hasPrefix("/api/forge") || pathOnly.hasPrefix("/api/stream")
  238:                 || pathOnly.hasPrefix("/api/health") || pathOnly.hasPrefix("/static/")
  239:                 || pathOnly == "/ping" {
  240:                 // Pass raw path (with query) so /api/stream?hz=20 can parse target rate.
  241:                 try TelemetryRoutes(app: app, http: http)
  242:                     .handle(method: m, path: rawPath.hasPrefix("/") ? rawPath : "/" + rawPath, connection: connection)
  243:                 return
  244:             }
  245: 
  246:             switch (m, path) {
  247:             case ("GET", "/"), ("GET", "/index.html"):
  248:                 if let (data, type) = app.telemetry.loadStatic("index.html") {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:248` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  241:                 try TelemetryRoutes(app: app, http: http)
  242:                     .handle(method: m, path: rawPath.hasPrefix("/") ? rawPath : "/" + rawPath, connection: connection)
  243:                 return
  244:             }
  245: 
  246:             switch (m, path) {
  247:             case ("GET", "/"), ("GET", "/index.html"):
  248:                 if let (data, type) = app.telemetry.loadStatic("index.html") {
  249:                     http.respondData(connection, status: 200, data: data, contentType: type)
  250:                 } else {
  251:                     http.respond(connection, status: 200, body: DashboardHTML.index, contentType: "text/html; charset=utf-8")
  252:                 }
  253:             case ("GET", "/control"), ("GET", "/manager"):
  254:                 http.respond(connection, status: 200, body: DashboardHTML.index, contentType: "text/html; charset=utf-8")
  255:             case ("GET", "/api/status"):
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:256` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  249:                     http.respondData(connection, status: 200, data: data, contentType: type)
  250:                 } else {
  251:                     http.respond(connection, status: 200, body: DashboardHTML.index, contentType: "text/html; charset=utf-8")
  252:                 }
  253:             case ("GET", "/control"), ("GET", "/manager"):
  254:                 http.respond(connection, status: 200, body: DashboardHTML.index, contentType: "text/html; charset=utf-8")
  255:             case ("GET", "/api/status"):
  256:                 var snap = try app.statusSnapshot()
  257:                 if let manager {
  258:                     snap["manager"] = manager.status()
  259:                     snap["service_active"] = manager.isServiceActive()
  260:                 } else {
  261:                     snap["service_active"] = true
  262:                     snap["manager"] = ["manager": false, "state": "standalone"] as [String: Any]
  263:                 }
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:272` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  265:             case ("OPTIONS", _):
  266:                 http.respond(connection, status: 405, body: "Method Not Allowed", contentType: "text/plain")
  267:             default:
  268:                 if let manager, !manager.isServiceActive(), path.hasPrefix("/api/") {
  269:                     http.respondJSON(connection, status: 503, object: [
  270:                         "ok": false,
  271:                         "code": "service_stopped",
  272:                         "message": "Operational APIs paused. Telemetry remains at / and /api/snapshot.",
  273:                         "manager": manager.status(),
  274:                     ])
  275:                     return
  276:                 }
  277:                 try OperationalRoutes(app: app, http: http)
  278:                     .handle(method: m, path: path, body: body, connection: connection)
  279:             }
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:97` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   90:         payload.append(bodyData)
   91:         connection.send(content: payload, completion: .contentProcessed { _ in
   92:             connection.cancel()
   93:         })
   94:     }
   95: 
   96:     /// Legacy one-shot SSE (compat). Prefer `startRealtimeSSE`.
   97:     public func respondSSE(connection: NWConnection, snapshot: [String: Any]) {
   98:         let json = (try? JSONSupport.string(from: snapshot)) ?? "{}"
   99:         var body = ": connected\n\n"
  100:         body += "data: \(json)\n\n"
  101:         let bodyData = Data(body.utf8)
  102:         var header = "HTTP/1.1 200 OK\r\n"
  103:         header += "Content-Type: text/event-stream; charset=utf-8\r\n"
  104:         header += "Cache-Control: no-cache, no-transform\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:98` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   91:         connection.send(content: payload, completion: .contentProcessed { _ in
   92:             connection.cancel()
   93:         })
   94:     }
   95: 
   96:     /// Legacy one-shot SSE (compat). Prefer `startRealtimeSSE`.
   97:     public func respondSSE(connection: NWConnection, snapshot: [String: Any]) {
   98:         let json = (try? JSONSupport.string(from: snapshot)) ?? "{}"
   99:         var body = ": connected\n\n"
  100:         body += "data: \(json)\n\n"
  101:         let bodyData = Data(body.utf8)
  102:         var header = "HTTP/1.1 200 OK\r\n"
  103:         header += "Content-Type: text/event-stream; charset=utf-8\r\n"
  104:         header += "Cache-Control: no-cache, no-transform\r\n"
  105:         header += "Connection: close\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:122` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  115:     private var securityHeaders: String {
  116:         "X-Content-Type-Options: nosniff\r\n"
  117:             + "Referrer-Policy: no-referrer\r\n"
  118:             + "Cross-Origin-Resource-Policy: same-origin\r\n"
  119:             + "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'\r\n"
  120:     }
  121: 
  122:     /// Continuous SSE: keep-alive stream of live frames from the realtime metrics engine.
  123:     @discardableResult
  124:     public func startRealtimeSSE(
  125:         connection: NWConnection,
  126:         telemetry: TelemetryService,
  127:         targetHz: Double = 20,
  128:         maxDurationSec: TimeInterval = 3600
  129:     ) -> SSEStreamSession {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:126` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  119:             + "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'\r\n"
  120:     }
  121: 
  122:     /// Continuous SSE: keep-alive stream of live frames from the realtime metrics engine.
  123:     @discardableResult
  124:     public func startRealtimeSSE(
  125:         connection: NWConnection,
  126:         telemetry: TelemetryService,
  127:         targetHz: Double = 20,
  128:         maxDurationSec: TimeInterval = 3600
  129:     ) -> SSEStreamSession {
  130:         let session = SSEStreamSession(
  131:             connection: connection,
  132:             telemetry: telemetry,
  133:             targetHz: targetHz,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:132` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  125:         connection: NWConnection,
  126:         telemetry: TelemetryService,
  127:         targetHz: Double = 20,
  128:         maxDurationSec: TimeInterval = 3600
  129:     ) -> SSEStreamSession {
  130:         let session = SSEStreamSession(
  131:             connection: connection,
  132:             telemetry: telemetry,
  133:             targetHz: targetHz,
  134:             maxDurationSec: maxDurationSec,
  135:             responder: self
  136:         )
  137:         retainStream(session)
  138:         return session
  139:     }
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:145` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  138:         return session
  139:     }
  140: }
  141: 
  142: /// Continuous SSE over an accepted `NWConnection`.
  143: ///
  144: /// Uses a serial send pipeline with `isComplete: false` (TCP stream semantics)
  145: /// and a Dispatch timer as the product clock for frames (engine advances host metrics).
  146: ///
  147: /// **Must be retained** by `HTTPResponder` for the life of the stream.
  148: public final class SSEStreamSession: @unchecked Sendable {
  149:     private let connection: NWConnection
  150:     private let telemetry: TelemetryService
  151:     private weak var responder: HTTPResponder?
  152:     private let periodMs: Int
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:150` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  143: ///
  144: /// Uses a serial send pipeline with `isComplete: false` (TCP stream semantics)
  145: /// and a Dispatch timer as the product clock for frames (engine advances host metrics).
  146: ///
  147: /// **Must be retained** by `HTTPResponder` for the life of the stream.
  148: public final class SSEStreamSession: @unchecked Sendable {
  149:     private let connection: NWConnection
  150:     private let telemetry: TelemetryService
  151:     private weak var responder: HTTPResponder?
  152:     private let periodMs: Int
  153:     private let maxDurationSec: TimeInterval
  154:     private let queue = DispatchQueue(label: "forge.telemetry.sse", qos: .userInitiated)
  155:     private let lock = NSLock()
  156:     private var timer: DispatchSourceTimer?
  157:     private var startedAt = Date()
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:154` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  147: /// **Must be retained** by `HTTPResponder` for the life of the stream.
  148: public final class SSEStreamSession: @unchecked Sendable {
  149:     private let connection: NWConnection
  150:     private let telemetry: TelemetryService
  151:     private weak var responder: HTTPResponder?
  152:     private let periodMs: Int
  153:     private let maxDurationSec: TimeInterval
  154:     private let queue = DispatchQueue(label: "forge.telemetry.sse", qos: .userInitiated)
  155:     private let lock = NSLock()
  156:     private var timer: DispatchSourceTimer?
  157:     private var startedAt = Date()
  158:     private var closed = false
  159:     private var eventCount = 0
  160:     private var sendChain: [(Data, Bool)] = []
  161:     private var sending = false
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:171` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  164:     public var eventsSent: Int {
  165:         lock.lock(); defer { lock.unlock() }
  166:         return eventCount
  167:     }
  168: 
  169:     public init(
  170:         connection: NWConnection,
  171:         telemetry: TelemetryService,
  172:         targetHz: Double,
  173:         maxDurationSec: TimeInterval,
  174:         responder: HTTPResponder? = nil
  175:     ) {
  176:         self.connection = connection
  177:         self.telemetry = telemetry
  178:         self.responder = responder
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:177` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  170:         connection: NWConnection,
  171:         telemetry: TelemetryService,
  172:         targetHz: Double,
  173:         maxDurationSec: TimeInterval,
  174:         responder: HTTPResponder? = nil
  175:     ) {
  176:         self.connection = connection
  177:         self.telemetry = telemetry
  178:         self.responder = responder
  179:         let hz = min(max(targetHz, 1), 60)
  180:         self.periodMs = max(20, Int((1000.0 / hz).rounded()))
  181:         self.maxDurationSec = maxDurationSec
  182:         begin()
  183:     }
  184: 
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:186` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  179:         let hz = min(max(targetHz, 1), 60)
  180:         self.periodMs = max(20, Int((1000.0 / hz).rounded()))
  181:         self.maxDurationSec = maxDurationSec
  182:         begin()
  183:     }
  184: 
  185:     private func begin() {
  186:         if !telemetry.realtimeEngine.isRunning {
  187:             telemetry.startBackgroundRefresh(intervalSec: 0.5)
  188:         }
  189: 
  190:         var bootstrap = "HTTP/1.1 200 OK\r\n"
  191:         bootstrap += "Content-Type: text/event-stream; charset=utf-8\r\n"
  192:         bootstrap += "Cache-Control: no-cache, no-transform\r\n"
  193:         bootstrap += "Connection: keep-alive\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:187` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  180:         self.periodMs = max(20, Int((1000.0 / hz).rounded()))
  181:         self.maxDurationSec = maxDurationSec
  182:         begin()
  183:     }
  184: 
  185:     private func begin() {
  186:         if !telemetry.realtimeEngine.isRunning {
  187:             telemetry.startBackgroundRefresh(intervalSec: 0.5)
  188:         }
  189: 
  190:         var bootstrap = "HTTP/1.1 200 OK\r\n"
  191:         bootstrap += "Content-Type: text/event-stream; charset=utf-8\r\n"
  192:         bootstrap += "Cache-Control: no-cache, no-transform\r\n"
  193:         bootstrap += "Connection: keep-alive\r\n"
  194:         bootstrap += "X-Content-Type-Options: nosniff\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:289` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  282:                 }
  283:                 self.queue.async { self.drainSendChain() }
  284:             }
  285:         )
  286:     }
  287: 
  288:     private func framePayload(full: Bool) -> String {
  289:         let frame = telemetry.currentFrame()
  290:         let system = frame.system
  291:         var dict: [String: Any]
  292:         if full {
  293:             dict = frame.asDictionary()
  294:         } else {
  295:             dict = [
  296:                 "updated": system.ts,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:314` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  307:                     "disk": system.disk.map { $0.asDictionary() },
  308:                     "processes": system.processes.prefix(8).map { $0.asDictionary() },
  309:                 ] as [String: Any],
  310:                 "history": frame.history.suffix(20).map { $0.asDictionary() },
  311:             ]
  312:         }
  313:         dict["stream"] = "realtime"
  314:         dict["sample_hz"] = telemetry.realtimeEngine.measuredSampleHz
  315:         let json = (try? JSONSupport.string(from: dict)) ?? "{}"
  316:         return "event: telemetry\ndata: \(json)\n\n"
  317:     }
  318: 
  319:     public func close() {
  320:         lock.lock()
  321:         if closed {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:316` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  309:                 ] as [String: Any],
  310:                 "history": frame.history.suffix(20).map { $0.asDictionary() },
  311:             ]
  312:         }
  313:         dict["stream"] = "realtime"
  314:         dict["sample_hz"] = telemetry.realtimeEngine.measuredSampleHz
  315:         let json = (try? JSONSupport.string(from: dict)) ?? "{}"
  316:         return "event: telemetry\ndata: \(json)\n\n"
  317:     }
  318: 
  319:     public func close() {
  320:         lock.lock()
  321:         if closed {
  322:             lock.unlock()
  323:             return
```

### `Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // OperationalRoutes.swift
    2: // What: Serves non-manager operational endpoints such as health and status.
    3: // How: It reads composed ForgeApp services and returns bounded snapshots without
    4: // exposing privileged tool execution through the browser surface.
    5: // Why: Read-oriented observability must remain separated from control-plane authority.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Read-mostly operational APIs for the local dashboard. Privileged tool calls
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:1` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:4` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
   11: public final class TelemetryRoutes: @unchecked Sendable {
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:5` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
   11: public final class TelemetryRoutes: @unchecked Sendable {
   12:     private let app: ForgeApp
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:10` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
   11: public final class TelemetryRoutes: @unchecked Sendable {
   12:     private let app: ForgeApp
   13:     private let http: HTTPResponder
   14: 
   15:     public init(app: ForgeApp, http: HTTPResponder) {
   16:         self.app = app
   17:         self.http = http
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:11` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
   11: public final class TelemetryRoutes: @unchecked Sendable {
   12:     private let app: ForgeApp
   13:     private let http: HTTPResponder
   14: 
   15:     public init(app: ForgeApp, http: HTTPResponder) {
   16:         self.app = app
   17:         self.http = http
   18:     }
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:28` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   21:         guard method == "GET" else {
   22:             http.respond(connection, status: 405, body: "Method Not Allowed", contentType: "text/plain")
   23:             return
   24:         }
   25: 
   26:         if path.hasPrefix("/static/") {
   27:             let rel = String(path.dropFirst("/static/".count))
   28:             if let (data, type) = app.telemetry.loadStatic(rel) {
   29:                 http.respondData(connection, status: 200, data: data, contentType: type)
   30:             } else {
   31:                 http.respond(connection, status: 404, body: "Not Found", contentType: "text/plain")
   32:             }
   33:             return
   34:         }
   35: 
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:42` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   35: 
   36:         // Strip query for path switch; parse query for stream Hz.
   37:         let pathOnly = path.split(separator: "?", maxSplits: 1).first.map(String.init) ?? path
   38:         let query = path.split(separator: "?", maxSplits: 1).dropFirst().first.map(String.init) ?? ""
   39: 
   40:         switch pathOnly {
   41:         case "/api/health":
   42:             http.respondJSON(connection, status: 200, object: app.telemetry.healthDictionary())
   43:         case "/api/live", "/api/frame":
   44:             // Preferred name for “current live frame” (not a multi-second product poll).
   45:             var obj = app.telemetry.currentFrame().asDictionary()
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:45` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   38:         let query = path.split(separator: "?", maxSplits: 1).dropFirst().first.map(String.init) ?? ""
   39: 
   40:         switch pathOnly {
   41:         case "/api/health":
   42:             http.respondJSON(connection, status: 200, object: app.telemetry.healthDictionary())
   43:         case "/api/live", "/api/frame":
   44:             // Preferred name for “current live frame” (not a multi-second product poll).
   45:             var obj = app.telemetry.currentFrame().asDictionary()
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:47` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   40:         switch pathOnly {
   41:         case "/api/health":
   42:             http.respondJSON(connection, status: 200, object: app.telemetry.healthDictionary())
   43:         case "/api/live", "/api/frame":
   44:             // Preferred name for “current live frame” (not a multi-second product poll).
   45:             var obj = app.telemetry.currentFrame().asDictionary()
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:49` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   42:             http.respondJSON(connection, status: 200, object: app.telemetry.healthDictionary())
   43:         case "/api/live", "/api/frame":
   44:             // Preferred name for “current live frame” (not a multi-second product poll).
   45:             var obj = app.telemetry.currentFrame().asDictionary()
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:51` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   44:             // Preferred name for “current live frame” (not a multi-second product poll).
   45:             var obj = app.telemetry.currentFrame().asDictionary()
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:53` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   46:             obj["stream"] = "realtime"
   47:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   48:             http.respondJSON(connection, status: 200, object: obj)
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
   59:         case "/ping":
   60:             let html = """
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:56` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   49:         case "/api/snapshot":
   50:             // Compatibility alias for current live frame.
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
   59:         case "/ping":
   60:             let html = """
   61:             <!DOCTYPE html><html><head><meta charset="utf-8"><title>Forge Telemetry OK</title></head>
   62:             <body style="background:#02040a;color:#e8fbff;font-family:system-ui;padding:2rem">
   63:             <h1 style="color:#18f0ff">Forge Telemetry is reachable</h1>
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:58` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   51:             var obj = app.telemetry.currentFrame().asDictionary()
   52:             obj["stream"] = "realtime"
   53:             obj["sample_hz"] = app.telemetry.realtimeEngine.measuredSampleHz
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
   59:         case "/ping":
   60:             let html = """
   61:             <!DOCTYPE html><html><head><meta charset="utf-8"><title>Forge Telemetry OK</title></head>
   62:             <body style="background:#02040a;color:#e8fbff;font-family:system-ui;padding:2rem">
   63:             <h1 style="color:#18f0ff">Forge Telemetry is reachable</h1>
   64:             <p>Integrated Swift host · continuous native collectors (~30&nbsp;Hz)</p>
   65:             <p><a style="color:#18f0ff" href="/">Open dashboard</a> ·
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:61` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   54:             http.respondJSON(connection, status: 200, object: obj)
   55:         case "/api/system":
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
   59:         case "/ping":
   60:             let html = """
   61:             <!DOCTYPE html><html><head><meta charset="utf-8"><title>Forge Telemetry OK</title></head>
   62:             <body style="background:#02040a;color:#e8fbff;font-family:system-ui;padding:2rem">
   63:             <h1 style="color:#18f0ff">Forge Telemetry is reachable</h1>
   64:             <p>Integrated Swift host · continuous native collectors (~30&nbsp;Hz)</p>
   65:             <p><a style="color:#18f0ff" href="/">Open dashboard</a> ·
   66:             <a style="color:#18f0ff" href="/api/health">/api/health</a> ·
   67:             <a style="color:#18f0ff" href="/api/stream">/api/stream (SSE realtime)</a> ·
   68:             <a style="color:#18f0ff" href="/api/live">/api/live</a> ·
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:63` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   56:             http.respondJSON(connection, status: 200, object: try app.telemetry.systemOnly(force: false))
   57:         case "/api/forge":
   58:             http.respondJSON(connection, status: 200, object: try app.telemetry.forgeOnly(force: false))
   59:         case "/ping":
   60:             let html = """
   61:             <!DOCTYPE html><html><head><meta charset="utf-8"><title>Forge Telemetry OK</title></head>
   62:             <body style="background:#02040a;color:#e8fbff;font-family:system-ui;padding:2rem">
   63:             <h1 style="color:#18f0ff">Forge Telemetry is reachable</h1>
   64:             <p>Integrated Swift host · continuous native collectors (~30&nbsp;Hz)</p>
   65:             <p><a style="color:#18f0ff" href="/">Open dashboard</a> ·
   66:             <a style="color:#18f0ff" href="/api/health">/api/health</a> ·
   67:             <a style="color:#18f0ff" href="/api/stream">/api/stream (SSE realtime)</a> ·
   68:             <a style="color:#18f0ff" href="/api/live">/api/live</a> ·
   69:             <a style="color:#18f0ff" href="/control">Manager controls</a></p>
   70:             </body></html>
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:79` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   72:             http.respond(connection, status: 200, body: html, contentType: "text/html; charset=utf-8")
   73:         default:
   74:             if pathOnly.hasPrefix("/api/stream") {
   75:                 let hz = Self.parseStreamHz(query: query)
   76:                 // Continuous keep-alive SSE — product path for browser + tools.
   77:                 _ = http.startRealtimeSSE(
   78:                     connection: connection,
   79:                     telemetry: app.telemetry,
   80:                     targetHz: hz,
   81:                     maxDurationSec: 3600
   82:                 )
   83:                 return
   84:             }
   85:             http.respond(connection, status: 404, body: "Not Found", contentType: "text/plain")
   86:         }
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // DoctorModels.swift
    2: // What: Carries structured health checks, doctor reports, and app status snapshots.
    3: // How: Small value types normalize service results into dictionaries for CLI, MCP,
    4: // dashboard, and tests without importing infrastructure implementations.
    5: // Why: Typed diagnostic contracts keep every presentation surface consistent.
    6: 
    7: import Foundation
    8: 
    9: /// One doctor health check result.
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:34` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   27: 
   28: /// Typed doctor report (application domain). Dictionary only at HTTP/CLI edge.
   29: public struct DoctorReport: Sendable, Equatable {
   30:     public var ok: Bool
   31:     public var version: String
   32:     public var home: String
   33:     public var checks: [DoctorCheck]
   34:     public var telemetry: TelemetryHealthReport
   35:     public var binaryInstalled: Bool
   36:     public var binaryPath: String
   37: 
   38:     public init(
   39:         ok: Bool,
   40:         version: String,
   41:         home: String,
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:43` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   36:     public var binaryPath: String
   37: 
   38:     public init(
   39:         ok: Bool,
   40:         version: String,
   41:         home: String,
   42:         checks: [DoctorCheck],
   43:         telemetry: TelemetryHealthReport,
   44:         binaryInstalled: Bool,
   45:         binaryPath: String
   46:     ) {
   47:         self.ok = ok
   48:         self.version = version
   49:         self.home = home
   50:         self.checks = checks
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:51` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   44:         binaryInstalled: Bool,
   45:         binaryPath: String
   46:     ) {
   47:         self.ok = ok
   48:         self.version = version
   49:         self.home = home
   50:         self.checks = checks
   51:         self.telemetry = telemetry
   52:         self.binaryInstalled = binaryInstalled
   53:         self.binaryPath = binaryPath
   54:     }
   55: 
   56:     public func asDictionary() -> [String: Any] {
   57:         [
   58:             "ok": ok,
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:62` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   55: 
   56:     public func asDictionary() -> [String: Any] {
   57:         [
   58:             "ok": ok,
   59:             "version": version,
   60:             "home": home,
   61:             "checks": checks.map { $0.asDictionary() },
   62:             "telemetry": telemetry.asDictionary(),
   63:             "binary": [
   64:                 "installed": binaryInstalled,
   65:                 "path": binaryPath,
   66:             ] as [String: Any],
   67:         ]
   68:     }
   69: }
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:71` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   64:                 "installed": binaryInstalled,
   65:                 "path": binaryPath,
   66:             ] as [String: Any],
   67:         ]
   68:     }
   69: }
   70: 
   71: /// Typed runtime status snapshot for dashboard / forge_status.
   72: public struct AppStatusSnapshot: Sendable, Equatable {
   73:     public var ok: Bool
   74:     public var version: String
   75:     public var product: String
   76:     public var runtime: String
   77:     public var home: String
   78:     public var store: String
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:72` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   65:                 "path": binaryPath,
   66:             ] as [String: Any],
   67:         ]
   68:     }
   69: }
   70: 
   71: /// Typed runtime status snapshot for dashboard / forge_status.
   72: public struct AppStatusSnapshot: Sendable, Equatable {
   73:     public var ok: Bool
   74:     public var version: String
   75:     public var product: String
   76:     public var runtime: String
   77:     public var home: String
   78:     public var store: String
   79:     public var agents: [String]
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:87` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   80:     public var agentCount: Int
   81:     public var openSessions: [AgentSessionSummary]
   82:     public var openSessionCount: Int
   83:     public var presence: [PresenceRecord]
   84:     public var presenceCount: Int
   85:     public var recentAudit: [AuditEventSummary]
   86:     public var tools: [String]
   87:     public var telemetry: TelemetryHealthReport
   88:     public var dashboardHost: String
   89:     public var dashboardPort: Int
   90:     public var pid: Int32
   91: 
   92:     public func asDictionary() -> [String: Any] {
   93:         [
   94:             "ok": ok,
```

### `Sources/ForgeConductorCore/Domain/DoctorModels.swift:108` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  101:             "agent_count": agentCount,
  102:             "open_sessions": openSessions.map { $0.asDictionary() },
  103:             "open_session_count": openSessionCount,
  104:             "presence": presence.map { $0.asDictionary() },
  105:             "presence_count": presenceCount,
  106:             "recent_audit": recentAudit.map { $0.asDictionary() },
  107:             "tools": tools,
  108:             "telemetry": telemetry.asDictionary(),
  109:             "dashboard": [
  110:                 "host": dashboardHost,
  111:                 "port": dashboardPort,
  112:             ] as [String: Any],
  113:             "pid": Int(pid),
  114:         ]
  115:     }
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:1` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // ForgeSnapshot.swift
    2: // What: Represents a composed point-in-time view of Forge orchestration health.
    3: // How: It aggregates tool, session, connector, process, usage, file, and audit values
    4: // into immutable domain summaries with explicit dictionary projections.
    5: // Why: Consumers receive one stable contract instead of coordinating many live services.
    6: 
    7: import Foundation
    8: 
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:9` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    2: // What: Represents a composed point-in-time view of Forge orchestration health.
    3: // How: It aggregates tool, session, connector, process, usage, file, and audit values
    4: // into immutable domain summaries with explicit dictionary projections.
    5: // Why: Consumers receive one stable contract instead of coordinating many live services.
    6: 
    7: import Foundation
    8: 
    9: /// Fully typed forge-side telemetry frame (no [String: Any] in domain).
   10: /// Name retained for API stability; this is a live-composed frame, not a multi-second poll product.
   11: public struct ForgeSnapshot: Sendable, Equatable {
   12:     public var ts: TimeInterval
   13:     public var home: String
   14:     public var runtime: String
   15:     public var presenceCount: Int
   16:     public var presence: [PresenceRecord]
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:11` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    4: // into immutable domain summaries with explicit dictionary projections.
    5: // Why: Consumers receive one stable contract instead of coordinating many live services.
    6: 
    7: import Foundation
    8: 
    9: /// Fully typed forge-side telemetry frame (no [String: Any] in domain).
   10: /// Name retained for API stability; this is a live-composed frame, not a multi-second poll product.
   11: public struct ForgeSnapshot: Sendable, Equatable {
   12:     public var ts: TimeInterval
   13:     public var home: String
   14:     public var runtime: String
   15:     public var presenceCount: Int
   16:     public var presence: [PresenceRecord]
   17:     public var mcpServers: [MCPServerCard]
   18:     public var mcpTools: [ToolCard]
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:69` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   62:         self.orchestration = orchestration
   63:         self.mcpLoad = mcpLoad
   64:         self.files = files
   65:         self.auditRecent = auditRecent
   66:     }
   67: 
   68:     /// Placeholder until the first forge composition runs (host stream still live).
   69:     public static func empty(home: String) -> ForgeSnapshot {
   70:         let emptyWindow = UsageWindow.empty
   71:         return ForgeSnapshot(
   72:             ts: Date().timeIntervalSince1970,
   73:             home: home,
   74:             runtime: "swift-native-realtime",
   75:             presenceCount: 0,
   76:             presence: [],
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:71` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   64:         self.files = files
   65:         self.auditRecent = auditRecent
   66:     }
   67: 
   68:     /// Placeholder until the first forge composition runs (host stream still live).
   69:     public static func empty(home: String) -> ForgeSnapshot {
   70:         let emptyWindow = UsageWindow.empty
   71:         return ForgeSnapshot(
   72:             ts: Date().timeIntervalSince1970,
   73:             home: home,
   74:             runtime: "swift-native-realtime",
   75:             presenceCount: 0,
   76:             presence: [],
   77:             mcpServers: [],
   78:             mcpTools: [],
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:17` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   10: public enum HandoffSource: String, Sendable, Codable, Equatable {
   11:     case model
   12:     case budget
   13:     case user
   14:     case auto
   15: }
   16: 
   17: /// Snapshot of an open (or recently open) specialist agent for resume.
   18: public struct AgentContinuitySnapshot: Sendable, Equatable {
   19:     public var sessionID: String
   20:     public var agentID: String
   21:     public var goal: String
   22:     public var cwd: String?
   23:     public var status: String
   24:     public var updatedAt: String?
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:18` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   11:     case model
   12:     case budget
   13:     case user
   14:     case auto
   15: }
   16: 
   17: /// Snapshot of an open (or recently open) specialist agent for resume.
   18: public struct AgentContinuitySnapshot: Sendable, Equatable {
   19:     public var sessionID: String
   20:     public var agentID: String
   21:     public var goal: String
   22:     public var cwd: String?
   23:     public var status: String
   24:     public var updatedAt: String?
   25:     public var resumeHint: String
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:58` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   51:             "resume_hint": resumeHint,
   52:         ]
   53:         if let cwd { d["cwd"] = cwd }
   54:         if let updatedAt { d["updated_at"] = updatedAt }
   55:         return d
   56:     }
   57: 
   58:     public static func fromDictionary(_ d: [String: Any]) -> AgentContinuitySnapshot? {
   59:         for key in ["session_id", "agent_id", "goal", "cwd", "status", "updated_at", "resume_hint"] {
   60:             if d[key] != nil, !(d[key] is String) { return nil }
   61:         }
   62:         guard let sessionID = d["session_id"] as? String,
   63:               !sessionID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
   64:               let agentID = d["agent_id"] as? String,
   65:               !agentID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:66` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   59:         for key in ["session_id", "agent_id", "goal", "cwd", "status", "updated_at", "resume_hint"] {
   60:             if d[key] != nil, !(d[key] is String) { return nil }
   61:         }
   62:         guard let sessionID = d["session_id"] as? String,
   63:               !sessionID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
   64:               let agentID = d["agent_id"] as? String,
   65:               !agentID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
   66:         return AgentContinuitySnapshot(
   67:             sessionID: sessionID,
   68:             agentID: agentID,
   69:             goal: d["goal"] as? String ?? "",
   70:             cwd: d["cwd"] as? String,
   71:             status: d["status"] as? String ?? "open",
   72:             updatedAt: d["updated_at"] as? String,
   73:             resumeHint: d["resume_hint"] as? String ?? ""
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:105` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   98:     public var nextActions: [String]
   99: 
  100:     // Working set
  101:     public var keyFiles: [String]
  102:     public var decisions: [String]
  103: 
  104:     // Agents
  105:     public var agents: [AgentContinuitySnapshot]
  106: 
  107:     // Narrative + resume seed
  108:     public var narrative: String
  109:     public var resumeSeed: String
  110:     public var resumeSeedIsCustom: Bool
  111: 
  112:     public init(
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:129` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  122:         status: String = "in_progress",
  123:         projectSlug: String? = nil,
  124:         cwd: String? = nil,
  125:         blockers: [String] = [],
  126:         nextActions: [String] = [],
  127:         keyFiles: [String] = [],
  128:         decisions: [String] = [],
  129:         agents: [AgentContinuitySnapshot] = [],
  130:         narrative: String = "",
  131:         resumeSeed: String = "",
  132:         resumeSeedIsCustom: Bool = false
  133:     ) {
  134:         self.id = id
  135:         self.schemaVersion = schemaVersion
  136:         self.createdAt = createdAt
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:298` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  291:             customMarker = nil
  292:         }
  293: 
  294:         let sourceRaw = meta["source"] as? String ?? HandoffSource.model.rawValue
  295:         guard let source = HandoffSource(rawValue: sourceRaw) else { return nil }
  296:         if root["agents"] != nil, !(root["agents"] is [[String: Any]]) { return nil }
  297:         let rawAgents = root["agents"] as? [[String: Any]] ?? []
  298:         var agentList: [AgentContinuitySnapshot] = []
  299:         for rawAgent in rawAgents {
  300:             guard let agent = AgentContinuitySnapshot.fromDictionary(rawAgent) else { return nil }
  301:             agentList.append(agent)
  302:         }
  303: 
  304:         let resumeSeed = resume["seed"] as? String ?? ""
  305:         var packet = HandoffPacket(
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:300` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  293: 
  294:         let sourceRaw = meta["source"] as? String ?? HandoffSource.model.rawValue
  295:         guard let source = HandoffSource(rawValue: sourceRaw) else { return nil }
  296:         if root["agents"] != nil, !(root["agents"] is [[String: Any]]) { return nil }
  297:         let rawAgents = root["agents"] as? [[String: Any]] ?? []
  298:         var agentList: [AgentContinuitySnapshot] = []
  299:         for rawAgent in rawAgents {
  300:             guard let agent = AgentContinuitySnapshot.fromDictionary(rawAgent) else { return nil }
  301:             agentList.append(agent)
  302:         }
  303: 
  304:         let resumeSeed = resume["seed"] as? String ?? ""
  305:         var packet = HandoffPacket(
  306:             id: id,
  307:             schemaVersion: rootVersion ?? metaVersion ?? schemaVersion,
```

### `Sources/ForgeConductorCore/Domain/ManagerModels.swift:2` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // ManagerModels.swift
    2: // What: Defines manager status, editable settings, patches, and telemetry health.
    3: // How: Value types validate dictionary input and emit stable wire representations used
    4: // by the manager client, routes, CLI, and SwiftUI settings module.
    5: // Why: A shared contract prevents control-plane request/response drift.
    6: 
    7: import Foundation
    8: 
    9: /// Typed manager runtime status (no dictionary in domain path).
```

### `Sources/ForgeConductorCore/Domain/ManagerModels.swift:301` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  294:         if !sessions.isEmpty { patch["sessions"] = sessions }
  295:         if !shell.isEmpty { patch["shell"] = shell }
  296:         if let logLevel { patch["log_level"] = logLevel }
  297:         return patch
  298:     }
  299: }
  300: 
  301: public struct TelemetryHealthReport: Sendable, Equatable {
  302:     public var ok: Bool
  303:     public var service: String
  304:     public var runtime: String
  305:     public var interferesWithMCP: Bool
  306:     public var mode: String
  307:     public var collectors: String
  308:     public var ui: String
```

### `Sources/ForgeConductorCore/Domain/Models.swift:241` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
  234: }
  235: 
  236: // MARK: - Audit / diagnostics
  237: 
  238: /// Typed interpretation of the audit statuses emitted by Core services.
  239: ///
  240: /// Operational failures, authorization policy, and maintenance advisories are
  241: /// intentionally distinct so telemetry does not turn every non-success result
  242: /// into an execution failure.
  243: public enum AuditOutcome: Sendable, Equatable {
  244:     case success
  245:     case operationalError
  246:     case policyDenied
  247:     case maintenanceWarning
  248:     case other
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:3` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
    1: // Protocols.swift
    2: // What: Declares the dependency-injection ports between Core modules.
    3: // How: Narrow protocols separate configuration, stores, collectors, tools, telemetry,
    4: // deployment, manager control, and connector side effects from concrete adapters.
    5: // Why: New connectors or test doubles can be added without rewriting framework logic.
    6: 
    7: import Foundation
    8: 
    9: // MARK: - Layer contracts (dependency inversion)
   10: // Wire formats (JSON-RPC / HTTP) may use dictionaries *only* at adapters.
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:41` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   34:     func presenceList() throws -> [[String: Any]]
   35:     func presenceUpsert(clientID: String, hostKind: String, pid: Int32, cwd: String) throws
   36:     func presenceDelete(clientID: String) throws
   37:     func presencePrune(maxAgeSec: TimeInterval) throws -> Int
   38: }
   39: 
   40: /// Collects current memory-pressure and physical-memory measurements.
   41: public protocol RAMMetricsCollecting: Sendable {
   42:     func collect() -> RAMMetrics
   43: }
   44: 
   45: /// Collects mounted-volume capacity and utilization measurements.
   46: public protocol DiskVolumeCollecting: Sendable {
   47:     func collect() -> [DiskVolume]
   48: }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:42` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   35:     func presenceUpsert(clientID: String, hostKind: String, pid: Int32, cwd: String) throws
   36:     func presenceDelete(clientID: String) throws
   37:     func presencePrune(maxAgeSec: TimeInterval) throws -> Int
   38: }
   39: 
   40: /// Collects current memory-pressure and physical-memory measurements.
   41: public protocol RAMMetricsCollecting: Sendable {
   42:     func collect() -> RAMMetrics
   43: }
   44: 
   45: /// Collects mounted-volume capacity and utilization measurements.
   46: public protocol DiskVolumeCollecting: Sendable {
   47:     func collect() -> [DiskVolume]
   48: }
   49: 
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:75` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   68: 
   69: /// Exposes a named tool collection behind a uniform invocation boundary.
   70: public protocol ToolExecuting: AnyObject, Sendable {
   71:     var toolNames: [String] { get }
   72:     func call(name: String, arguments: [String: Any], clientID: ClientID) throws -> ToolResult
   73: }
   74: 
   75: // MARK: Telemetry collectors (strict SRP)
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:77` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   70: public protocol ToolExecuting: AnyObject, Sendable {
   71:     var toolNames: [String] { get }
   72:     func call(name: String, arguments: [String: Any], clientID: ClientID) throws -> ToolResult
   73: }
   74: 
   75: // MARK: Telemetry collectors (strict SRP)
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:78` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   71:     var toolNames: [String] { get }
   72:     func call(name: String, arguments: [String: Any], clientID: ClientID) throws -> ToolResult
   73: }
   74: 
   75: // MARK: Telemetry collectors (strict SRP)
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:79` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   72:     func call(name: String, arguments: [String: Any], clientID: ClientID) throws -> ToolResult
   73: }
   74: 
   75: // MARK: Telemetry collectors (strict SRP)
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:82` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   75: // MARK: Telemetry collectors (strict SRP)
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:83` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   76: 
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:84` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   77: /// Samples host-wide and per-logical-core CPU utilization.
   78: public protocol CPUMetricsCollecting: Sendable {
   79:     func collect() -> CPUMetrics
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:87` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   80: }
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:88` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   81: 
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
   95: }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:89` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   82: /// Samples installed GPUs and their available utilization and memory evidence.
   83: public protocol GPUMetricsCollecting: Sendable {
   84:     func collect() -> [GPUMetrics]
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
   95: }
   96: 
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:92` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   85: }
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
   95: }
   96: 
   97: /// IOKit IOPowerSources (`IOPSCopyPowerSourcesInfo` / list / description).
   98: public protocol PowerMetricsCollecting: Sendable {
   99:     func collect() -> PowerMetrics
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:93` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   86: 
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
   95: }
   96: 
   97: /// IOKit IOPowerSources (`IOPSCopyPowerSourcesInfo` / list / description).
   98: public protocol PowerMetricsCollecting: Sendable {
   99:     func collect() -> PowerMetrics
  100: }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:94` — telemetr\|snapshot\|metric\|sample\|coalesc\|backpressure

```swift
   87: /// Samples aggregate storage read and write throughput.
   88: public protocol DiskIOMetricsCollecting: Sendable {
   89:     func collect() -> DiskIOMetrics
   90: }
   91: 
   92: /// Samples resource usage for relevant Forge and model-host processes.
   93: public protocol ProcessMetricsCollecting: Sendable {
   94:     func collect() -> [ProcessMetrics]
   95: }
   96: 
   97: /// IOKit IOPowerSources (`IOPSCopyPowerSourcesInfo` / list / description).
   98: public protocol PowerMetricsCollecting: Sendable {
   99:     func collect() -> PowerMetrics
  100: }
  101: 
```

444 additional hits are retained in the JSON evidence.

## Gauge and Metal evidence

117 lexical hits.

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:2` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // LoadTraceRenderer.swift
    2: // What: Draws the historical load trace into an MTKView.
    3: // How: The delegate converts normalized samples into GPU vertex buffers and
    4: // encodes Metal draw calls whenever SwiftUI supplies updated history.
    5: // Why: GPU rendering keeps a rapidly refreshing chart off the main UI drawing path.
    6: 
    7: import Foundation
    8: import MetalKit
    9: import simd
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:22` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   15:     private var queue: MTLCommandQueue?
   16:     private var pipeline: MTLRenderPipelineState?
   17:     private var vertexBuffer: MTLBuffer?
   18:     private var sampleCount = 0
   19:     private let lock = NSLock()
   20:     private var samples: [Float] = []
   21: 
   22:     func attach(to view: MTKView) {
   23:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   24:         guard let device = mtl else { return }
   25:         self.device = device
   26:         view.device = device
   27:         view.delegate = self
   28:         queue = device.makeCommandQueue()
   29:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:28` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   21: 
   22:     func attach(to view: MTKView) {
   23:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   24:         guard let device = mtl else { return }
   25:         self.device = device
   26:         view.device = device
   27:         view.delegate = self
   28:         queue = device.makeCommandQueue()
   29:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   30:     }
   31: 
   32:     func update(samples: [Float]) {
   33:         lock.lock()
   34:         self.samples = samples
   35:         lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:60` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   53:         desc.colorAttachments[0].isBlendingEnabled = true
   54:         desc.colorAttachments[0].rgbBlendOperation = .add
   55:         desc.colorAttachments[0].alphaBlendOperation = .add
   56:         desc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
   57:         desc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
   58:         desc.colorAttachments[0].sourceAlphaBlendFactor = .one
   59:         desc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
   60:         pipeline = try? device.makeRenderPipelineState(descriptor: desc)
   61:     }
   62: 
   63:     private func rebuildVertices() {
   64:         guard let device else { return }
   65:         lock.lock()
   66:         let src = samples
   67:         lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:103` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   96:         // Line
   97:         for i in 0..<n {
   98:             let val = i < src.count ? src[i] : 0
   99:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyanLine))
  100:         }
  101: 
  102:         let bytes = verts.count * MemoryLayout<V>.stride
  103:         vertexBuffer = device.makeBuffer(bytes: verts, length: bytes, options: .storageModeShared)
  104:         sampleCount = fillCount // first draw fill; line uses rest
  105:         // Store line offset in high bits via sampleCount encoding: fillCount | (lineCount << 16) — simpler: store both
  106:         lock.lock()
  107:         self.fillVertexCount = fillCount
  108:         self.lineVertexCount = n
  109:         lock.unlock()
  110:     }
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:115` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  108:         self.lineVertexCount = n
  109:         lock.unlock()
  110:     }
  111: 
  112:     private var fillVertexCount = 0
  113:     private var lineVertexCount = 0
  114: 
  115:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  116: 
  117:     func draw(in view: MTKView) {
  118:         guard let drawable = view.currentDrawable,
  119:               let rpd = view.currentRenderPassDescriptor,
  120:               let pipeline,
  121:               let queue,
  122:               let buffer = queue.makeCommandBuffer(),
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:117` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  110:     }
  111: 
  112:     private var fillVertexCount = 0
  113:     private var lineVertexCount = 0
  114: 
  115:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  116: 
  117:     func draw(in view: MTKView) {
  118:         guard let drawable = view.currentDrawable,
  119:               let rpd = view.currentRenderPassDescriptor,
  120:               let pipeline,
  121:               let queue,
  122:               let buffer = queue.makeCommandBuffer(),
  123:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  124:               let vertexBuffer else { return }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:1` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalGaugeKit.swift
    2: // What: Supplies the reusable Metal-backed gauges used throughout the rig.
    3: // How: Shared palettes and pipelines feed dedicated renderers, while small
    4: // NSViewRepresentable adapters expose bars, rings, tiles, and status pills to SwiftUI.
    5: // Why: Centralizing gauge primitives keeps visual behavior consistent and modular.
    6: 
    7: import SwiftUI
    8: import MetalKit
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:2` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalGaugeKit.swift
    2: // What: Supplies the reusable Metal-backed gauges used throughout the rig.
    3: // How: Shared palettes and pipelines feed dedicated renderers, while small
    4: // NSViewRepresentable adapters expose bars, rings, tiles, and status pills to SwiftUI.
    5: // Why: Centralizing gauge primitives keeps visual behavior consistent and modular.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: import simd
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:5` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalGaugeKit.swift
    2: // What: Supplies the reusable Metal-backed gauges used throughout the rig.
    3: // How: Shared palettes and pipelines feed dedicated renderers, while small
    4: // NSViewRepresentable adapters expose bars, rings, tiles, and status pills to SwiftUI.
    5: // Why: Centralizing gauge primitives keeps visual behavior consistent and modular.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: import simd
   10: import ForgeConductorCore
   11: 
   12: extension TelemetryStatusTone {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:26` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   19:         case .unavailable: .secondary
   20:         }
   21:     }
   22: }
   23: 
   24: // MARK: - Shared shader + types
   25: 
   26: enum MetalGaugePalette {
   27:     static let cyan = SIMD4<Float>(0.09, 0.94, 1.0, 1)
   28:     static let orange = SIMD4<Float>(1.0, 0.42, 0.12, 1)
   29:     static let green = SIMD4<Float>(0.18, 1.0, 0.55, 1)
   30:     static let purple = SIMD4<Float>(0.75, 0.45, 1.0, 1)
   31:     static let red = SIMD4<Float>(1.0, 0.25, 0.35, 1)
   32:     static let track = SIMD4<Float>(0.05, 0.12, 0.18, 1)
   33: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:51` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   44:         case .failure: return red
   45:         case .informational: return cyan
   46:         case .unavailable: return SIMD4(0.48, 0.54, 0.62, 1)
   47:         }
   48:     }
   49: }
   50: 
   51: private struct GaugeVertex {
   52:     var pos: SIMD2<Float>
   53:     var color: SIMD4<Float>
   54: }
   55: 
   56: /// Shared pipeline builder for 2D colored primitives.
   57: enum MetalGaugePipeline {
   58:     static let shader = """
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:57` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   50: 
   51: private struct GaugeVertex {
   52:     var pos: SIMD2<Float>
   53:     var color: SIMD4<Float>
   54: }
   55: 
   56: /// Shared pipeline builder for 2D colored primitives.
   57: enum MetalGaugePipeline {
   58:     static let shader = """
   59:     #include <metal_stdlib>
   60:     using namespace metal;
   61:     struct P { float2 p; float4 c; };
   62:     struct O { float4 position [[position]]; float4 c; };
   63:     vertex O g_vert(uint i [[vertex_id]], const device P *v [[buffer(0)]]) {
   64:         O o; o.position = float4(v[i].p, 0, 1); o.c = v[i].c; return o;
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:80` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   73:         let d = MTLRenderPipelineDescriptor()
   74:         d.vertexFunction = v
   75:         d.fragmentFunction = f
   76:         d.colorAttachments[0].pixelFormat = pixelFormat
   77:         d.colorAttachments[0].isBlendingEnabled = true
   78:         d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
   79:         d.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
   80:         return try? device.makeRenderPipelineState(descriptor: d)
   81:     }
   82: }
   83: 
   84: // MARK: - Horizontal meter
   85: 
   86: @MainActor
   87: final class MetalBarRenderer: NSObject, MTKViewDelegate {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:84` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   77:         d.colorAttachments[0].isBlendingEnabled = true
   78:         d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
   79:         d.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
   80:         return try? device.makeRenderPipelineState(descriptor: d)
   81:     }
   82: }
   83: 
   84: // MARK: - Horizontal meter
   85: 
   86: @MainActor
   87: final class MetalBarRenderer: NSObject, MTKViewDelegate {
   88:     private var device: MTLDevice?
   89:     private var queue: MTLCommandQueue?
   90:     private var pipeline: MTLRenderPipelineState?
   91:     private var buffer: MTLBuffer?
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:93` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   86: @MainActor
   87: final class MetalBarRenderer: NSObject, MTKViewDelegate {
   88:     private var device: MTLDevice?
   89:     private var queue: MTLCommandQueue?
   90:     private var pipeline: MTLRenderPipelineState?
   91:     private var buffer: MTLBuffer?
   92:     private var fraction: Float = 0
   93:     private var color = MetalGaugePalette.cyan
   94:     private let lock = NSLock()
   95: 
   96:     func attach(_ view: MTKView) {
   97:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   98:         guard let device = mtl else { return }
   99:         self.device = device
  100:         view.device = device
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:96` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   89:     private var queue: MTLCommandQueue?
   90:     private var pipeline: MTLRenderPipelineState?
   91:     private var buffer: MTLBuffer?
   92:     private var fraction: Float = 0
   93:     private var color = MetalGaugePalette.cyan
   94:     private let lock = NSLock()
   95: 
   96:     func attach(_ view: MTKView) {
   97:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   98:         guard let device = mtl else { return }
   99:         self.device = device
  100:         view.device = device
  101:         view.delegate = self
  102:         view.clearColor = MTLClearColor(red: 0.02, green: 0.04, blue: 0.08, alpha: 1)
  103:         view.isPaused = false
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:106` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   99:         self.device = device
  100:         view.device = device
  101:         view.delegate = self
  102:         view.clearColor = MTLClearColor(red: 0.02, green: 0.04, blue: 0.08, alpha: 1)
  103:         view.isPaused = false
  104:         view.enableSetNeedsDisplay = false
  105:         view.preferredFramesPerSecond = 24
  106:         queue = device.makeCommandQueue()
  107:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  108:         rebuild()
  109:     }
  110: 
  111:     func set(fraction: Float, color: SIMD4<Float>) {
  112:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  113:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:107` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  100:         view.device = device
  101:         view.delegate = self
  102:         view.clearColor = MTLClearColor(red: 0.02, green: 0.04, blue: 0.08, alpha: 1)
  103:         view.isPaused = false
  104:         view.enableSetNeedsDisplay = false
  105:         view.preferredFramesPerSecond = 24
  106:         queue = device.makeCommandQueue()
  107:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  108:         rebuild()
  109:     }
  110: 
  111:     func set(fraction: Float, color: SIMD4<Float>) {
  112:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  113:         rebuild()
  114:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:120` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  113:         rebuild()
  114:     }
  115: 
  116:     private func rebuild() {
  117:         guard let device else { return }
  118:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  119:         let x = -1 + 2 * f
  120:         var v: [GaugeVertex] = [
  121:             .init(pos: SIMD2(-1, -0.55), color: MetalGaugePalette.track),
  122:             .init(pos: SIMD2(1, -0.55), color: MetalGaugePalette.track),
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:121` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  114:     }
  115: 
  116:     private func rebuild() {
  117:         guard let device else { return }
  118:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  119:         let x = -1 + 2 * f
  120:         var v: [GaugeVertex] = [
  121:             .init(pos: SIMD2(-1, -0.55), color: MetalGaugePalette.track),
  122:             .init(pos: SIMD2(1, -0.55), color: MetalGaugePalette.track),
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:122` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  115: 
  116:     private func rebuild() {
  117:         guard let device else { return }
  118:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  119:         let x = -1 + 2 * f
  120:         var v: [GaugeVertex] = [
  121:             .init(pos: SIMD2(-1, -0.55), color: MetalGaugePalette.track),
  122:             .init(pos: SIMD2(1, -0.55), color: MetalGaugePalette.track),
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:123` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  116:     private func rebuild() {
  117:         guard let device else { return }
  118:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  119:         let x = -1 + 2 * f
  120:         var v: [GaugeVertex] = [
  121:             .init(pos: SIMD2(-1, -0.55), color: MetalGaugePalette.track),
  122:             .init(pos: SIMD2(1, -0.55), color: MetalGaugePalette.track),
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:124` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  117:         guard let device else { return }
  118:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  119:         let x = -1 + 2 * f
  120:         var v: [GaugeVertex] = [
  121:             .init(pos: SIMD2(-1, -0.55), color: MetalGaugePalette.track),
  122:             .init(pos: SIMD2(1, -0.55), color: MetalGaugePalette.track),
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:130` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  123:             .init(pos: SIMD2(-1, 0.55), color: MetalGaugePalette.track),
  124:             .init(pos: SIMD2(1, 0.55), color: MetalGaugePalette.track),
  125:             .init(pos: SIMD2(-1, -0.55), color: c),
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
  132: 
  133:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  134:     func draw(in view: MTKView) {
  135:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  136:               let pipeline, let queue, let buffer,
  137:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:133` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  126:             .init(pos: SIMD2(x, -0.55), color: c),
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
  132: 
  133:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  134:     func draw(in view: MTKView) {
  135:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  136:               let pipeline, let queue, let buffer,
  137:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  138:         enc.setRenderPipelineState(pipeline)
  139:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  140:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:134` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  127:             .init(pos: SIMD2(-1, 0.55), color: c),
  128:             .init(pos: SIMD2(x, 0.55), color: c),
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
  132: 
  133:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  134:     func draw(in view: MTKView) {
  135:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  136:               let pipeline, let queue, let buffer,
  137:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  138:         enc.setRenderPipelineState(pipeline)
  139:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  140:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  141:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 4, vertexCount: 4)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:146` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  139:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  140:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  141:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 4, vertexCount: 4)
  142:         enc.endEncoding(); cmd.present(d); cmd.commit()
  143:     }
  144: }
  145: 
  146: struct MetalBarGauge: NSViewRepresentable {
  147:     var fraction: Double
  148:     var tint: Color
  149: 
  150:     func makeCoordinator() -> MetalBarRenderer { MetalBarRenderer() }
  151: 
  152:     func makeNSView(context: Context) -> MTKView {
  153:         let v = MTKView(frame: NSRect(x: 0, y: 0, width: 48, height: 8))
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:152` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  145: 
  146: struct MetalBarGauge: NSViewRepresentable {
  147:     var fraction: Double
  148:     var tint: Color
  149: 
  150:     func makeCoordinator() -> MetalBarRenderer { MetalBarRenderer() }
  151: 
  152:     func makeNSView(context: Context) -> MTKView {
  153:         let v = MTKView(frame: NSRect(x: 0, y: 0, width: 48, height: 8))
  154:         // MTKView has no sensible intrinsic size; without bounds it reports huge
  155:         // preferred sizes and blows out SwiftUI headers/rows.
  156:         v.translatesAutoresizingMaskIntoConstraints = true
  157:         v.autoResizeDrawable = true
  158:         v.framebufferOnly = true
  159:         v.isPaused = false
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:153` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  146: struct MetalBarGauge: NSViewRepresentable {
  147:     var fraction: Double
  148:     var tint: Color
  149: 
  150:     func makeCoordinator() -> MetalBarRenderer { MetalBarRenderer() }
  151: 
  152:     func makeNSView(context: Context) -> MTKView {
  153:         let v = MTKView(frame: NSRect(x: 0, y: 0, width: 48, height: 8))
  154:         // MTKView has no sensible intrinsic size; without bounds it reports huge
  155:         // preferred sizes and blows out SwiftUI headers/rows.
  156:         v.translatesAutoresizingMaskIntoConstraints = true
  157:         v.autoResizeDrawable = true
  158:         v.framebufferOnly = true
  159:         v.isPaused = false
  160:         v.enableSetNeedsDisplay = false
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:154` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  147:     var fraction: Double
  148:     var tint: Color
  149: 
  150:     func makeCoordinator() -> MetalBarRenderer { MetalBarRenderer() }
  151: 
  152:     func makeNSView(context: Context) -> MTKView {
  153:         let v = MTKView(frame: NSRect(x: 0, y: 0, width: 48, height: 8))
  154:         // MTKView has no sensible intrinsic size; without bounds it reports huge
  155:         // preferred sizes and blows out SwiftUI headers/rows.
  156:         v.translatesAutoresizingMaskIntoConstraints = true
  157:         v.autoResizeDrawable = true
  158:         v.framebufferOnly = true
  159:         v.isPaused = false
  160:         v.enableSetNeedsDisplay = false
  161:         v.setContentHuggingPriority(.defaultLow, for: .horizontal)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:166` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  159:         v.isPaused = false
  160:         v.enableSetNeedsDisplay = false
  161:         v.setContentHuggingPriority(.defaultLow, for: .horizontal)
  162:         v.setContentHuggingPriority(.required, for: .vertical)
  163:         v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
  164:         v.setContentCompressionResistancePriority(.required, for: .vertical)
  165:         context.coordinator.attach(v)
  166:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  167:         return v
  168:     }
  169: 
  170:     func updateNSView(_ nsView: MTKView, context: Context) {
  171:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  172:     }
  173: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:170` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  163:         v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
  164:         v.setContentCompressionResistancePriority(.required, for: .vertical)
  165:         context.coordinator.attach(v)
  166:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  167:         return v
  168:     }
  169: 
  170:     func updateNSView(_ nsView: MTKView, context: Context) {
  171:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  172:     }
  173: 
  174:     /// Honor the SwiftUI proposed size so Metal bars never invent their own scale.
  175:     func sizeThatFits(_ proposal: ProposedViewSize, nsView: MTKView, context: Context) -> CGSize? {
  176:         let width = proposal.width ?? 48
  177:         let height = proposal.height ?? 8
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:171` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  164:         v.setContentCompressionResistancePriority(.required, for: .vertical)
  165:         context.coordinator.attach(v)
  166:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  167:         return v
  168:     }
  169: 
  170:     func updateNSView(_ nsView: MTKView, context: Context) {
  171:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  172:     }
  173: 
  174:     /// Honor the SwiftUI proposed size so Metal bars never invent their own scale.
  175:     func sizeThatFits(_ proposal: ProposedViewSize, nsView: MTKView, context: Context) -> CGSize? {
  176:         let width = proposal.width ?? 48
  177:         let height = proposal.height ?? 8
  178:         return CGSize(width: max(width, 1), height: max(height, 1))
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:175` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  168:     }
  169: 
  170:     func updateNSView(_ nsView: MTKView, context: Context) {
  171:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  172:     }
  173: 
  174:     /// Honor the SwiftUI proposed size so Metal bars never invent their own scale.
  175:     func sizeThatFits(_ proposal: ProposedViewSize, nsView: MTKView, context: Context) -> CGSize? {
  176:         let width = proposal.width ?? 48
  177:         let height = proposal.height ?? 8
  178:         return CGSize(width: max(width, 1), height: max(height, 1))
  179:     }
  180: }
  181: 
  182: // MARK: - Activity ring (MCP cards)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:192` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  185: final class MetalRingRenderer: NSObject, MTKViewDelegate {
  186:     private var device: MTLDevice?
  187:     private var queue: MTLCommandQueue?
  188:     private var pipeline: MTLRenderPipelineState?
  189:     private var buffer: MTLBuffer?
  190:     private var count = 0
  191:     private var fraction: Float = 0
  192:     private var color = MetalGaugePalette.cyan
  193:     private let lock = NSLock()
  194: 
  195:     func attach(_ view: MTKView) {
  196:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
  197:         guard let device = mtl else { return }
  198:         self.device = device
  199:         view.device = device
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:195` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  188:     private var pipeline: MTLRenderPipelineState?
  189:     private var buffer: MTLBuffer?
  190:     private var count = 0
  191:     private var fraction: Float = 0
  192:     private var color = MetalGaugePalette.cyan
  193:     private let lock = NSLock()
  194: 
  195:     func attach(_ view: MTKView) {
  196:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
  197:         guard let device = mtl else { return }
  198:         self.device = device
  199:         view.device = device
  200:         view.delegate = self
  201:         view.clearColor = MTLClearColor(red: 0.015, green: 0.03, blue: 0.06, alpha: 1)
  202:         view.isPaused = false
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:205` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  198:         self.device = device
  199:         view.device = device
  200:         view.delegate = self
  201:         view.clearColor = MTLClearColor(red: 0.015, green: 0.03, blue: 0.06, alpha: 1)
  202:         view.isPaused = false
  203:         view.enableSetNeedsDisplay = false
  204:         view.preferredFramesPerSecond = 24
  205:         queue = device.makeCommandQueue()
  206:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  207:         rebuild()
  208:     }
  209: 
  210:     func set(fraction: Float, color: SIMD4<Float>) {
  211:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  212:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:206` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  199:         view.device = device
  200:         view.delegate = self
  201:         view.clearColor = MTLClearColor(red: 0.015, green: 0.03, blue: 0.06, alpha: 1)
  202:         view.isPaused = false
  203:         view.enableSetNeedsDisplay = false
  204:         view.preferredFramesPerSecond = 24
  205:         queue = device.makeCommandQueue()
  206:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  207:         rebuild()
  208:     }
  209: 
  210:     func set(fraction: Float, color: SIMD4<Float>) {
  211:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  212:         rebuild()
  213:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:218` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  211:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  212:         rebuild()
  213:     }
  214: 
  215:     private func rebuild() {
  216:         guard let device else { return }
  217:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  218:         var verts: [GaugeVertex] = []
  219:         let segments = 64
  220:         let outer: Float = 0.88
  221:         let inner: Float = 0.62
  222:         // Background ring full 360
  223:         appendRing(into: &verts, from: 0, to: 1, outer: outer, inner: inner, color: MetalGaugePalette.track, segments: segments)
  224:         // Progress arc (start at top, clockwise)
  225:         if f > 0.001 {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:223` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  216:         guard let device else { return }
  217:         lock.lock(); let f = fraction; let c = color; lock.unlock()
  218:         var verts: [GaugeVertex] = []
  219:         let segments = 64
  220:         let outer: Float = 0.88
  221:         let inner: Float = 0.62
  222:         // Background ring full 360
  223:         appendRing(into: &verts, from: 0, to: 1, outer: outer, inner: inner, color: MetalGaugePalette.track, segments: segments)
  224:         // Progress arc (start at top, clockwise)
  225:         if f > 0.001 {
  226:             appendRing(into: &verts, from: 0, to: f, outer: outer, inner: inner, color: c, segments: max(4, Int(Float(segments) * f)))
  227:         }
  228:         count = verts.count
  229:         buffer = device.makeBuffer(bytes: verts, length: max(verts.count, 1) * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  230:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:229` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  222:         // Background ring full 360
  223:         appendRing(into: &verts, from: 0, to: 1, outer: outer, inner: inner, color: MetalGaugePalette.track, segments: segments)
  224:         // Progress arc (start at top, clockwise)
  225:         if f > 0.001 {
  226:             appendRing(into: &verts, from: 0, to: f, outer: outer, inner: inner, color: c, segments: max(4, Int(Float(segments) * f)))
  227:         }
  228:         count = verts.count
  229:         buffer = device.makeBuffer(bytes: verts, length: max(verts.count, 1) * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  230:     }
  231: 
  232:     private func appendRing(
  233:         into verts: inout [GaugeVertex],
  234:         from: Float,
  235:         to: Float,
  236:         outer: Float,
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:233` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  226:             appendRing(into: &verts, from: 0, to: f, outer: outer, inner: inner, color: c, segments: max(4, Int(Float(segments) * f)))
  227:         }
  228:         count = verts.count
  229:         buffer = device.makeBuffer(bytes: verts, length: max(verts.count, 1) * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  230:     }
  231: 
  232:     private func appendRing(
  233:         into verts: inout [GaugeVertex],
  234:         from: Float,
  235:         to: Float,
  236:         outer: Float,
  237:         inner: Float,
  238:         color: SIMD4<Float>,
  239:         segments: Int
  240:     ) {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:264` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  267:               let pipeline, let queue, let buffer, count >= 3,
  268:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  269:         enc.setRenderPipelineState(pipeline)
  270:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  271:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:265` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  267:               let pipeline, let queue, let buffer, count >= 3,
  268:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  269:         enc.setRenderPipelineState(pipeline)
  270:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  271:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  272:         enc.endEncoding(); cmd.present(d); cmd.commit()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:276` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  269:         enc.setRenderPipelineState(pipeline)
  270:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  271:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  272:         enc.endEncoding(); cmd.present(d); cmd.commit()
  273:     }
  274: }
  275: 
  276: struct MetalRingGauge: NSViewRepresentable {
  277:     var fraction: Double
  278:     var tint: Color
  279:     var label: String = ""
  280: 
  281:     func makeCoordinator() -> MetalRingRenderer { MetalRingRenderer() }
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:282` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  275: 
  276: struct MetalRingGauge: NSViewRepresentable {
  277:     var fraction: Double
  278:     var tint: Color
  279:     var label: String = ""
  280: 
  281:     func makeCoordinator() -> MetalRingRenderer { MetalRingRenderer() }
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
  284:         context.coordinator.attach(v)
  285:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  286:         return v
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:283` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  276: struct MetalRingGauge: NSViewRepresentable {
  277:     var fraction: Double
  278:     var tint: Color
  279:     var label: String = ""
  280: 
  281:     func makeCoordinator() -> MetalRingRenderer { MetalRingRenderer() }
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
  284:         context.coordinator.attach(v)
  285:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  286:         return v
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  290:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:285` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  278:     var tint: Color
  279:     var label: String = ""
  280: 
  281:     func makeCoordinator() -> MetalRingRenderer { MetalRingRenderer() }
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
  284:         context.coordinator.attach(v)
  285:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  286:         return v
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  290:     }
  291: }
  292: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:288` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  281:     func makeCoordinator() -> MetalRingRenderer { MetalRingRenderer() }
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
  284:         context.coordinator.attach(v)
  285:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  286:         return v
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  290:     }
  291: }
  292: 
  293: /// Ring with centered text overlay (SwiftUI text + Metal ring).
  294: struct MetalRingGaugeLabeled: View {
  295:     var fraction: Double
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:289` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  282:     func makeNSView(context: Context) -> MTKView {
  283:         let v = MTKView()
  284:         context.coordinator.attach(v)
  285:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  286:         return v
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  290:     }
  291: }
  292: 
  293: /// Ring with centered text overlay (SwiftUI text + Metal ring).
  294: struct MetalRingGaugeLabeled: View {
  295:     var fraction: Double
  296:     var tint: Color
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:294` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  287:     }
  288:     func updateNSView(_ nsView: MTKView, context: Context) {
  289:         context.coordinator.set(fraction: Float(fraction), color: MetalGaugePalette.from(swiftUI: tint))
  290:     }
  291: }
  292: 
  293: /// Ring with centered text overlay (SwiftUI text + Metal ring).
  294: struct MetalRingGaugeLabeled: View {
  295:     var fraction: Double
  296:     var tint: Color
  297:     var centerText: String
  298: 
  299:     var body: some View {
  300:         ZStack {
  301:             MetalRingGauge(fraction: fraction, tint: tint)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:301` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  294: struct MetalRingGaugeLabeled: View {
  295:     var fraction: Double
  296:     var tint: Color
  297:     var centerText: String
  298: 
  299:     var body: some View {
  300:         ZStack {
  301:             MetalRingGauge(fraction: fraction, tint: tint)
  302:             Text(centerText)
  303:                 .font(.system(size: 11, weight: .bold, design: .monospaced))
  304:                 .foregroundStyle(tint)
  305:         }
  306:     }
  307: }
  308: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:321` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  314:     private var queue: MTLCommandQueue?
  315:     private var pipeline: MTLRenderPipelineState?
  316:     private var buffer: MTLBuffer?
  317:     private var count = 0
  318:     private var cores: [Float] = []
  319:     private let lock = NSLock()
  320: 
  321:     func attach(_ view: MTKView) {
  322:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
  323:         guard let device = mtl else { return }
  324:         self.device = device
  325:         view.device = device
  326:         view.delegate = self
  327:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
  328:         view.isPaused = false
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:331` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  324:         self.device = device
  325:         view.device = device
  326:         view.delegate = self
  327:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
  328:         view.isPaused = false
  329:         view.enableSetNeedsDisplay = false
  330:         view.preferredFramesPerSecond = 20
  331:         queue = device.makeCommandQueue()
  332:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  333:         rebuild()
  334:     }
  335: 
  336:     func set(cores: [Float]) {
  337:         lock.lock(); self.cores = cores; lock.unlock()
  338:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:332` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  325:         view.device = device
  326:         view.delegate = self
  327:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
  328:         view.isPaused = false
  329:         view.enableSetNeedsDisplay = false
  330:         view.preferredFramesPerSecond = 20
  331:         queue = device.makeCommandQueue()
  332:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  333:         rebuild()
  334:     }
  335: 
  336:     func set(cores: [Float]) {
  337:         lock.lock(); self.cores = cores; lock.unlock()
  338:         rebuild()
  339:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:349` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  342:         guard let device else { return }
  343:         lock.lock(); let cores = self.cores; lock.unlock()
  344:         guard !cores.isEmpty else {
  345:             count = 0
  346:             buffer = nil
  347:             return
  348:         }
  349:         var verts: [GaugeVertex] = []
  350:         let n = cores.count
  351:         let gap: Float = 0.015
  352:         let totalGap = gap * Float(n + 1)
  353:         let barW = (2.0 - totalGap) / Float(n)
  354:         let bottom: Float = -0.9
  355:         let top: Float = 0.9
  356:         let height = top - bottom
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:362` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  355:         let top: Float = 0.9
  356:         let height = top - bottom
  357:         for (i, pct) in cores.enumerated() {
  358:             let p = min(max(pct / 100, 0), 1)
  359:             let x0 = -1 + gap + Float(i) * (barW + gap)
  360:             let x1 = x0 + barW
  361:             // track
  362:             verts.append(contentsOf: quad(x0, bottom, x1, top, MetalGaugePalette.track))
  363:             // fill
  364:             let y1 = bottom + height * p
  365:             let hot = p >= 0.75
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:366` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  359:             let x0 = -1 + gap + Float(i) * (barW + gap)
  360:             let x1 = x0 + barW
  361:             // track
  362:             verts.append(contentsOf: quad(x0, bottom, x1, top, MetalGaugePalette.track))
  363:             // fill
  364:             let y1 = bottom + height * p
  365:             let hot = p >= 0.75
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
  370:         buffer = device.makeBuffer(bytes: verts, length: verts.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  371:     }
  372: 
  373:     private func quad(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ c: SIMD4<Float>) -> [GaugeVertex] {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:370` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  363:             // fill
  364:             let y1 = bottom + height * p
  365:             let hot = p >= 0.75
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
  370:         buffer = device.makeBuffer(bytes: verts, length: verts.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  371:     }
  372: 
  373:     private func quad(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ c: SIMD4<Float>) -> [GaugeVertex] {
  374:         [
  375:             .init(pos: SIMD2(x0, y0), color: c),
  376:             .init(pos: SIMD2(x1, y0), color: c),
  377:             .init(pos: SIMD2(x0, y1), color: c),
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:373` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
  370:         buffer = device.makeBuffer(bytes: verts, length: verts.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  371:     }
  372: 
  373:     private func quad(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ c: SIMD4<Float>) -> [GaugeVertex] {
  374:         [
  375:             .init(pos: SIMD2(x0, y0), color: c),
  376:             .init(pos: SIMD2(x1, y0), color: c),
  377:             .init(pos: SIMD2(x0, y1), color: c),
  378:             .init(pos: SIMD2(x1, y0), color: c),
  379:             .init(pos: SIMD2(x1, y1), color: c),
  380:             .init(pos: SIMD2(x0, y1), color: c),
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:384` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  377:             .init(pos: SIMD2(x0, y1), color: c),
  378:             .init(pos: SIMD2(x1, y0), color: c),
  379:             .init(pos: SIMD2(x1, y1), color: c),
  380:             .init(pos: SIMD2(x0, y1), color: c),
  381:         ]
  382:     }
  383: 
  384:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  385:     func draw(in view: MTKView) {
  386:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  387:               let pipeline, let queue, let buffer, count >= 3,
  388:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  389:         enc.setRenderPipelineState(pipeline)
  390:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  391:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:385` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  378:             .init(pos: SIMD2(x1, y0), color: c),
  379:             .init(pos: SIMD2(x1, y1), color: c),
  380:             .init(pos: SIMD2(x0, y1), color: c),
  381:         ]
  382:     }
  383: 
  384:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  385:     func draw(in view: MTKView) {
  386:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  387:               let pipeline, let queue, let buffer, count >= 3,
  388:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  389:         enc.setRenderPipelineState(pipeline)
  390:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  391:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  392:         enc.endEncoding(); cmd.present(d); cmd.commit()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:399` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  392:         enc.endEncoding(); cmd.present(d); cmd.commit()
  393:     }
  394: }
  395: 
  396: struct MetalCoreBarsView: NSViewRepresentable {
  397:     var cores: [Double]
  398:     func makeCoordinator() -> MetalCoreBarsRenderer { MetalCoreBarsRenderer() }
  399:     func makeNSView(context: Context) -> MTKView {
  400:         let v = MTKView()
  401:         context.coordinator.attach(v)
  402:         context.coordinator.set(cores: cores.map { Float($0) })
  403:         return v
  404:     }
  405:     func updateNSView(_ nsView: MTKView, context: Context) {
  406:         context.coordinator.set(cores: cores.map { Float($0) })
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:400` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  393:     }
  394: }
  395: 
  396: struct MetalCoreBarsView: NSViewRepresentable {
  397:     var cores: [Double]
  398:     func makeCoordinator() -> MetalCoreBarsRenderer { MetalCoreBarsRenderer() }
  399:     func makeNSView(context: Context) -> MTKView {
  400:         let v = MTKView()
  401:         context.coordinator.attach(v)
  402:         context.coordinator.set(cores: cores.map { Float($0) })
  403:         return v
  404:     }
  405:     func updateNSView(_ nsView: MTKView, context: Context) {
  406:         context.coordinator.set(cores: cores.map { Float($0) })
  407:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:405` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  398:     func makeCoordinator() -> MetalCoreBarsRenderer { MetalCoreBarsRenderer() }
  399:     func makeNSView(context: Context) -> MTKView {
  400:         let v = MTKView()
  401:         context.coordinator.attach(v)
  402:         context.coordinator.set(cores: cores.map { Float($0) })
  403:         return v
  404:     }
  405:     func updateNSView(_ nsView: MTKView, context: Context) {
  406:         context.coordinator.set(cores: cores.map { Float($0) })
  407:     }
  408: }
  409: 
  410: // MARK: - Tool load tile gauge (0–3 tiers as metal fill)
  411: 
  412: struct MetalToolLoadTile: View {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:410` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  403:         return v
  404:     }
  405:     func updateNSView(_ nsView: MTKView, context: Context) {
  406:         context.coordinator.set(cores: cores.map { Float($0) })
  407:     }
  408: }
  409: 
  410: // MARK: - Tool load tile gauge (0–3 tiers as metal fill)
  411: 
  412: struct MetalToolLoadTile: View {
  413:     var shortLabel: String
  414:     var activity: Double // 0-100
  415:     var health: String
  416:     var loadTier: Int = 0
  417: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:423` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  416:     var loadTier: Int = 0
  417: 
  418:     var body: some View {
  419:         VStack(spacing: 4) {
  420:             Text(shortLabel)
  421:                 .font(.system(size: 9, weight: .bold, design: .monospaced))
  422:                 .foregroundStyle(Color.cyan)
  423:             MetalBarGauge(fraction: min(max(activity / 100, Double(loadTier) / 3.0), 1), tint: healthColor)
  424:                 .frame(height: 6)
  425:                 .clipShape(Capsule())
  426:             // Load tier as 3 micro Metal bars
  427:             HStack(spacing: 3) {
  428:                 ForEach(0..<3, id: \.self) { i in
  429:                     MetalBarGauge(fraction: loadTier > i ? 1 : 0, tint: healthColor)
  430:                         .frame(height: 3)
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:429` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  422:                 .foregroundStyle(Color.cyan)
  423:             MetalBarGauge(fraction: min(max(activity / 100, Double(loadTier) / 3.0), 1), tint: healthColor)
  424:                 .frame(height: 6)
  425:                 .clipShape(Capsule())
  426:             // Load tier as 3 micro Metal bars
  427:             HStack(spacing: 3) {
  428:                 ForEach(0..<3, id: \.self) { i in
  429:                     MetalBarGauge(fraction: loadTier > i ? 1 : 0, tint: healthColor)
  430:                         .frame(height: 3)
  431:                 }
  432:             }
  433:         }
  434:         .padding(.horizontal, 8)
  435:         .padding(.vertical, 7)
  436:         .background(RoundedRectangle(cornerRadius: 5).fill(Color.white.opacity(0.05)))
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:445` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  438: 
  439:     private var healthColor: Color {
  440:         TelemetryHealth.tone(for: health).color
  441:     }
  442: }
  443: 
  444: /// Header status pill with Metal activity bar underneath.
  445: /// Fixed geometry so the upper-right cluster stays toolbar-scale, not MTKView-scale.
  446: struct MetalStatusPill: View {
  447:     var text: String
  448:     var tone: TelemetryStatusTone
  449:     var fraction: Double = 1
  450: 
  451:     /// Compact chip: fits four across a typical detail header without colliding with the title.
  452:     private let width: CGFloat = 80
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:464` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  457:         VStack(spacing: 3) {
  458:             Text(text)
  459:                 .font(.system(size: 9, weight: .bold, design: .monospaced))
  460:                 .foregroundStyle(tint)
  461:                 .lineLimit(1)
  462:                 .minimumScaleFactor(0.7)
  463:                 .frame(maxWidth: .infinity)
  464:             MetalBarGauge(fraction: max(fraction, 0.05), tint: tint)
  465:                 .frame(width: width - 16, height: barHeight)
  466:                 .clipShape(Capsule())
  467:                 .allowsHitTesting(false)
  468:         }
  469:         .padding(.horizontal, 8)
  470:         .padding(.vertical, 5)
  471:         .frame(width: width, height: 32)
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:3` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalLoadChart.swift
    2: // What: Adapts the single-series load renderer to SwiftUI.
    3: // How: NSViewRepresentable creates an MTKView, assigns its coordinator, and
    4: // forwards new sample arrays without rebuilding the native view.
    5: // Why: The adapter isolates AppKit/Metal lifecycle details from dashboard composition.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: 
   10: /// SwiftUI wrapper around an MTKView that draws the load history with Metal.
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:10` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    3: // How: NSViewRepresentable creates an MTKView, assigns its coordinator, and
    4: // forwards new sample arrays without rebuilding the native view.
    5: // Why: The adapter isolates AppKit/Metal lifecycle details from dashboard composition.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: 
   10: /// SwiftUI wrapper around an MTKView that draws the load history with Metal.
   11: struct MetalLoadChart: NSViewRepresentable {
   12:     var samples: [Float]
   13: 
   14:     func makeCoordinator() -> LoadTraceRenderer {
   15:         LoadTraceRenderer()
   16:     }
   17: 
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:18` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   11: struct MetalLoadChart: NSViewRepresentable {
   12:     var samples: [Float]
   13: 
   14:     func makeCoordinator() -> LoadTraceRenderer {
   15:         LoadTraceRenderer()
   16:     }
   17: 
   18:     func makeNSView(context: Context) -> MTKView {
   19:         let view = MTKView()
   20:         view.device = MTLCreateSystemDefaultDevice()
   21:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
   22:         view.colorPixelFormat = .bgra8Unorm
   23:         view.framebufferOnly = true
   24:         view.isPaused = false
   25:         view.enableSetNeedsDisplay = false
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:19` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   12:     var samples: [Float]
   13: 
   14:     func makeCoordinator() -> LoadTraceRenderer {
   15:         LoadTraceRenderer()
   16:     }
   17: 
   18:     func makeNSView(context: Context) -> MTKView {
   19:         let view = MTKView()
   20:         view.device = MTLCreateSystemDefaultDevice()
   21:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
   22:         view.colorPixelFormat = .bgra8Unorm
   23:         view.framebufferOnly = true
   24:         view.isPaused = false
   25:         view.enableSetNeedsDisplay = false
   26:         view.preferredFramesPerSecond = 30
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:32` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   25:         view.enableSetNeedsDisplay = false
   26:         view.preferredFramesPerSecond = 30
   27:         context.coordinator.attach(to: view)
   28:         context.coordinator.update(samples: samples)
   29:         return view
   30:     }
   31: 
   32:     func updateNSView(_ nsView: MTKView, context: Context) {
   33:         context.coordinator.update(samples: samples)
   34:     }
   35: }
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:1` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalMeterView.swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:2` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalMeterView.swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
    9: typealias MetalMeterView = MetalBarGauge
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:3` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalMeterView.swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
    9: typealias MetalMeterView = MetalBarGauge
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:4` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalMeterView.swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
    9: typealias MetalMeterView = MetalBarGauge
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:8` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // MetalMeterView.swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
    9: typealias MetalMeterView = MetalBarGauge
```

### `Sources/ForgeConductorApp/Metal/MetalMeterView.swift:9` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    2: // What: Retains the semantic meter name used by higher-level views.
    3: // How: A type alias maps that API to the shared MetalBarGauge implementation.
    4: // Why: Call sites express intent while rendering stays consolidated in one gauge module.
    5: 
    6: import SwiftUI
    7: 
    8: /// Back-compat alias — all meters use `MetalBarGauge` from MetalGaugeKit.
    9: typealias MetalMeterView = MetalBarGauge
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:6` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: import Foundation
    2: import MetalKit
    3: // MultiSeriesLoadRenderer.swift
    4: // What: Renders synchronized CPU, RAM, and GPU histories in one Metal chart.
    5: // How: It normalizes series into a common viewport, uploads per-series vertices,
    6: // and issues distinct colored line passes through an MTKView delegate.
    7: // Why: One renderer guarantees aligned time axes and predictable high-frequency cost.
    8: 
    9: import SwiftUI
   10: import simd
   11: 
   12: /// Metal multi-series load trace: CPU / RAM / GPU (parity with old LOAD TRACE + richer).
   13: @MainActor
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:32` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   25:     private var queue: MTLCommandQueue?
   26:     private var pipeline: MTLRenderPipelineState?
   27:     private var vertexBuffer: MTLBuffer?
   28:     private var vertexCount = 0
   29:     private let lock = NSLock()
   30:     private var series: [Series] = []
   31: 
   32:     public func attach(to view: MTKView) {
   33:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   34:         guard let device = mtl else { return }
   35:         self.device = device
   36:         view.device = device
   37:         view.delegate = self
   38:         view.clearColor = MTLClearColor(red: 0.01, green: 0.015, blue: 0.04, alpha: 1)
   39:         view.colorPixelFormat = .bgra8Unorm
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:43` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   36:         view.device = device
   37:         view.delegate = self
   38:         view.clearColor = MTLClearColor(red: 0.01, green: 0.015, blue: 0.04, alpha: 1)
   39:         view.colorPixelFormat = .bgra8Unorm
   40:         view.isPaused = false
   41:         view.enableSetNeedsDisplay = false
   42:         view.preferredFramesPerSecond = 30
   43:         queue = device.makeCommandQueue()
   44:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   45:     }
   46: 
   47:     public func update(cpu: [Float], ram: [Float], gpu: [Float?]) {
   48:         lock.lock()
   49:         series = [
   50:             Series(values: cpu.map(Optional.some), color: SIMD4(0.09, 0.94, 1.0, 1.0)),
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:69` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   62:         let desc = MTLRenderPipelineDescriptor()
   63:         desc.vertexFunction = vert
   64:         desc.fragmentFunction = frag
   65:         desc.colorAttachments[0].pixelFormat = pixelFormat
   66:         desc.colorAttachments[0].isBlendingEnabled = true
   67:         desc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
   68:         desc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
   69:         pipeline = try? device.makeRenderPipelineState(descriptor: desc)
   70:     }
   71: 
   72:     private struct V {
   73:         var pos: SIMD2<Float>
   74:         var color: SIMD4<Float>
   75:     }
   76: 
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:141` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  134:                 verts.append(V(pos: SIMD2(x, y), color: s.color))
  135:                 segmentCount += 1
  136:             }
  137:             finishSegment()
  138:         }
  139: 
  140:         let bytes = verts.count * MemoryLayout<V>.stride
  141:         vertexBuffer = device.makeBuffer(bytes: verts, length: max(bytes, 16), options: .storageModeShared)
  142:         lock.lock()
  143:         self.drawGrid = gridCount
  144:         self.drawFillStart = fillStart
  145:         self.drawFillCount = fillCount
  146:         self.drawLines = lineRanges
  147:         lock.unlock()
  148:     }
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:155` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  148:     }
  149: 
  150:     private var drawGrid = 0
  151:     private var drawFillStart = 0
  152:     private var drawFillCount = 0
  153:     private var drawLines: [(Int, Int)] = []
  154: 
  155:     public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  156: 
  157:     public func draw(in view: MTKView) {
  158:         guard let drawable = view.currentDrawable,
  159:               let rpd = view.currentRenderPassDescriptor,
  160:               let pipeline,
  161:               let queue,
  162:               let buffer = queue.makeCommandBuffer(),
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:157` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  150:     private var drawGrid = 0
  151:     private var drawFillStart = 0
  152:     private var drawFillCount = 0
  153:     private var drawLines: [(Int, Int)] = []
  154: 
  155:     public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  156: 
  157:     public func draw(in view: MTKView) {
  158:         guard let drawable = view.currentDrawable,
  159:               let rpd = view.currentRenderPassDescriptor,
  160:               let pipeline,
  161:               let queue,
  162:               let buffer = queue.makeCommandBuffer(),
  163:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  164:               let vertexBuffer else { return }
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:211` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  204: struct MultiSeriesLoadChart: NSViewRepresentable {
  205:     var cpu: [Float]
  206:     var ram: [Float]
  207:     var gpu: [Float?]
  208: 
  209:     func makeCoordinator() -> MultiSeriesLoadRenderer { MultiSeriesLoadRenderer() }
  210: 
  211:     func makeNSView(context: Context) -> MTKView {
  212:         let view = MTKView()
  213:         context.coordinator.attach(to: view)
  214:         context.coordinator.update(cpu: cpu, ram: ram, gpu: gpu)
  215:         return view
  216:     }
  217: 
  218:     func updateNSView(_ nsView: MTKView, context: Context) {
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:212` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  205:     var cpu: [Float]
  206:     var ram: [Float]
  207:     var gpu: [Float?]
  208: 
  209:     func makeCoordinator() -> MultiSeriesLoadRenderer { MultiSeriesLoadRenderer() }
  210: 
  211:     func makeNSView(context: Context) -> MTKView {
  212:         let view = MTKView()
  213:         context.coordinator.attach(to: view)
  214:         context.coordinator.update(cpu: cpu, ram: ram, gpu: gpu)
  215:         return view
  216:     }
  217: 
  218:     func updateNSView(_ nsView: MTKView, context: Context) {
  219:         context.coordinator.update(cpu: cpu, ram: ram, gpu: gpu)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:218` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  211:     func makeNSView(context: Context) -> MTKView {
  212:         let view = MTKView()
  213:         context.coordinator.attach(to: view)
  214:         context.coordinator.update(cpu: cpu, ram: ram, gpu: gpu)
  215:         return view
  216:     }
  217: 
  218:     func updateNSView(_ nsView: MTKView, context: Context) {
  219:         context.coordinator.update(cpu: cpu, ram: ram, gpu: gpu)
  220:     }
  221: }
```

### `Sources/ForgeConductorApp/Views/AppSidebarView.swift:84` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   77:         .accessibilityAddTraits(selected ? .isSelected : [])
   78:     }
   79: }
   80: 
   81: private extension AppModel.AppTab {
   82:     var systemImage: String {
   83:         switch self {
   84:         case .rig: "gauge.with.dots.needle.67percent"
   85:         case .mcp: "server.rack"
   86:         case .agents: "person.3"
   87:         case .tools: "wrench.and.screwdriver"
   88:         case .feed: "waveform.path.ecg"
   89:         case .diagnostics: "doc.text.magnifyingglass"
   90:         case .manager: "gearshape.2"
   91:         }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:2` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // RigDashboardView.swift
    2: // What: Builds the real-time host and orchestration instrument panel.
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:4` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    1: // RigDashboardView.swift
    2: // What: Builds the real-time host and orchestration instrument panel.
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:10` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
   12: struct RigDashboardView: View {
   13:     @EnvironmentObject private var model: AppModel
   14: 
   15:     var body: some View {
   16:         // TimelineView drives UI at animation/display cadence against the continuous engine.
   17:         TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !model.autoRefresh)) { _ in
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:94` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   87:                     .foregroundStyle(.secondary)
   88:                     .lineLimit(1)
   89:                     .minimumScaleFactor(0.8)
   90:             }
   91:             .frame(maxWidth: .infinity, alignment: .leading)
   92:             .layoutPriority(1)
   93: 
   94:             // Fixed-width chip strip: 4×80 + 3×6 spacing = 338pt — never grows with MTKView.
   95:             HStack(spacing: 6) {
   96:                 MetalStatusPill(
   97:                     text: "LINK",
   98:                     tone: model.lastError == nil ? .healthy : .failure,
   99:                     fraction: 1
  100:                 )
  101:                 MetalStatusPill(
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:161` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  154:                 .font(.system(size: 10, weight: .semibold, design: .monospaced))
  155:                 .foregroundStyle(.secondary)
  156:             Text(value)
  157:                 .font(.system(.title2, design: .monospaced).weight(.bold))
  158:                 .foregroundStyle(tint)
  159:                 .lineLimit(1)
  160:                 .minimumScaleFactor(0.7)
  161:             MetalBarGauge(fraction: frac, tint: tint)
  162:                 .frame(height: 14)
  163:                 .clipShape(Capsule())
  164:             Text(meta)
  165:                 .font(.system(size: 9, design: .monospaced))
  166:                 .foregroundStyle(.secondary)
  167:                 .lineLimit(1)
  168:         }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:250` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  243:         return VStack(alignment: .leading, spacing: 4) {
  244:             Text(label)
  245:                 .font(.system(size: 8, design: .monospaced))
  246:                 .foregroundStyle(.secondary)
  247:             Text(measured ? String(format: "%.0f%%", value) : "—")
  248:                 .font(.system(size: 10, weight: .bold, design: .monospaced))
  249:                 .foregroundStyle(measured ? Color.purple : Color.secondary)
  250:             MetalBarGauge(
  251:                 fraction: measured ? min(max(value / 100, 0), 1) : 0,
  252:                 tint: measured ? .purple : .secondary
  253:             )
  254:             .frame(height: 6)
  255:             .clipShape(Capsule())
  256:         }
  257:         .frame(maxWidth: .infinity, alignment: .leading)
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:260` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  253:             )
  254:             .frame(height: 6)
  255:             .clipShape(Capsule())
  256:         }
  257:         .frame(maxWidth: .infinity, alignment: .leading)
  258:     }
  259: 
  260:     // MARK: Storage — Metal meters
  261: 
  262:     private var storagePanel: some View {
  263:         let disks = model.diskVolumes
  264:         let io = model.diskIO
  265:         return panel("STORAGE", meta: String(format: "%.1f MB/s total", io.totalMBs)) {
  266:             HStack(spacing: 14) {
  267:                 ioStat("READ", io.readMBs, io.readIOPS, frac: min(io.readMBs / 100, 1), tint: .cyan)
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:285` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  278:                             HStack(spacing: 8) {
  279:                                 Text(d.mount).font(.system(size: 11, design: .monospaced))
  280:                                 Spacer(minLength: 8)
  281:                                 Text(String(format: "%.0f/%.0f GB · %.0f%%", d.usedGB, d.totalGB, d.percent))
  282:                                     .font(.system(size: 10, design: .monospaced))
  283:                                     .foregroundStyle(.secondary)
  284:                             }
  285:                             MetalBarGauge(fraction: d.percent / 100, tint: .cyan)
  286:                                 .frame(height: 10)
  287:                                 .clipShape(Capsule())
  288:                         }
  289:                     }
  290:                 }
  291:             }
  292:         }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:299` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  292:         }
  293:     }
  294: 
  295:     private func ioStat(_ title: String, _ mbs: Double, _ iops: Double, frac: Double, tint: Color) -> some View {
  296:         VStack(alignment: .leading, spacing: 4) {
  297:             Text(title).font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary)
  298:             Text(String(format: "%.1f MB/s", mbs)).font(.system(size: 11, weight: .bold, design: .monospaced)).foregroundStyle(tint)
  299:             MetalBarGauge(fraction: frac, tint: tint).frame(height: 8).clipShape(Capsule())
  300:             Text(String(format: "%.0f IOPS", iops)).font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary)
  301:         }
  302:         .frame(maxWidth: .infinity, alignment: .leading)
  303:     }
  304: 
  305:     // MARK: Orchestration
  306: 
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:376` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  369:                         HStack(spacing: 6) {
  370:                             Text(c.title).font(.system(size: 10, weight: .bold, design: .monospaced))
  371:                             Spacer(minLength: 6)
  372:                             Text(c.state)
  373:                                 .font(.system(size: 9, weight: .bold, design: .monospaced))
  374:                                 .foregroundStyle(c.tone.color)
  375:                         }
  376:                         MetalBarGauge(fraction: c.fraction, tint: c.tone.color)
  377:                             .frame(height: 8)
  378:                             .clipShape(Capsule())
  379:                         Text(c.detail)
  380:                             .font(.system(size: 9, design: .monospaced))
  381:                             .foregroundStyle(.secondary)
  382:                             .lineLimit(2)
  383:                     }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:418` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  411:                                     .lineLimit(1)
  412:                                 Spacer(minLength: 8)
  413:                                 Text(s.healthLabel)
  414:                                     .font(.system(size: 9, weight: .bold, design: .monospaced))
  415:                                     .foregroundStyle(healthColor(s.health))
  416:                             }
  417:                             HStack(alignment: .top, spacing: 12) {
  418:                                 MetalRingGaugeLabeled(
  419:                                     fraction: s.activity / 100,
  420:                                     tint: healthColor(s.health),
  421:                                     centerText: "\(Int(s.activity))"
  422:                                 )
  423:                                 .frame(width: 56, height: 56)
  424:                                 VStack(alignment: .leading, spacing: 4) {
  425:                                     Text("\(s.role) · \(s.status)\(s.live ? " · LINK" : "")")
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:447` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  440:                                         Text(s.healthReason)
  441:                                             .font(.system(size: 8, design: .monospaced))
  442:                                             .foregroundStyle(.secondary)
  443:                                             .lineLimit(1)
  444:                                     }
  445:                                 }
  446:                             }
  447:                             MetalBarGauge(fraction: s.activity / 100, tint: healthColor(s.health))
  448:                                 .frame(height: 5)
  449:                                 .clipShape(Capsule())
  450:                         }
  451:                         .padding(10)
  452:                         .background(RoundedRectangle(cornerRadius: 6).stroke(healthColor(s.health).opacity(0.35)))
  453:                     }
  454:                 }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:473` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  466:                 HStack(spacing: 8) {
  467:                     ForEach(Array(packs.prefix(12).enumerated()), id: \.offset) { _, p in
  468:                         let active = Double(p.activeCount)
  469:                         let total = max(Double(p.toolCount), 1)
  470:                         VStack(spacing: 4) {
  471:                             Text(p.pack)
  472:                                 .font(.system(size: 9, design: .monospaced))
  473:                             MetalBarGauge(fraction: active / total, tint: .cyan)
  474:                                 .frame(width: 64, height: 6)
  475:                                 .clipShape(Capsule())
  476:                         }
  477:                         .padding(.horizontal, 10)
  478:                         .padding(.vertical, 7)
  479:                         .background(Capsule().stroke(Color.cyan.opacity(0.35)))
  480:                     }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:515` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  508:                                 .lineLimit(1)
  509:                             Spacer(minLength: 8)
  510:                             Text(a.healthLabel)
  511:                                 .font(.system(size: 9, weight: .bold, design: .monospaced))
  512:                                 .foregroundStyle(healthColor(a.health))
  513:                         }
  514:                         HStack(alignment: .top, spacing: 10) {
  515:                             MetalRingGaugeLabeled(
  516:                                 fraction: a.activity / 100,
  517:                                 tint: healthColor(a.health),
  518:                                 centerText: a.live ? "ON" : "SB"
  519:                             )
  520:                             .frame(width: 48, height: 48)
  521:                             VStack(alignment: .leading, spacing: 4) {
  522:                                 Text(a.status)
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:536` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  529:                                 }
  530:                                 if let sum = a.summary, !sum.isEmpty {
  531:                                     Text(sum).font(.system(size: 8, design: .monospaced))
  532:                                         .foregroundStyle(.secondary).lineLimit(2)
  533:                                 }
  534:                             }
  535:                         }
  536:                         MetalBarGauge(fraction: a.activity / 100, tint: healthColor(a.health))
  537:                             .frame(height: 6)
  538:                             .clipShape(Capsule())
  539:                     }
  540:                     .padding(10)
  541:                     .background(RoundedRectangle(cornerRadius: 6).stroke(healthColor(a.health).opacity(0.35)))
  542:                 }
  543:             }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:567` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  560:                     }
  561:                     .font(.system(size: 9, weight: .bold, design: .monospaced))
  562:                     .foregroundStyle(.secondary)
  563:                     ForEach(Array(model.hotProcesses.prefix(12).enumerated()), id: \.offset) { _, p in
  564:                         HStack(spacing: 10) {
  565:                             Text("\(p.pid)").frame(width: 52, alignment: .leading)
  566:                             Text(p.name).frame(maxWidth: .infinity, alignment: .leading).lineLimit(1)
  567:                             MetalBarGauge(fraction: min(p.cpuPercent / 100, 1), tint: p.cpuPercent > 50 ? .orange : .cyan)
  568:                                 .frame(width: 100, height: 8)
  569:                                 .clipShape(Capsule())
  570:                             Text(String(format: "%.2fG", p.rssGB)).frame(width: 48, alignment: .trailing)
  571:                         }
  572:                         .font(.system(size: 10, design: .monospaced))
  573:                     }
  574:                 }
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:599` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  592:                             .foregroundStyle(auditStatusColor(e.status))
  593:                             .frame(width: 36, alignment: .leading)
  594:                         Text(e.tool)
  595:                             .lineLimit(1)
  596:                             .truncationMode(.middle)
  597:                             .frame(maxWidth: .infinity, alignment: .leading)
  598:                         if let ms = e.durationMs {
  599:                             MetalBarGauge(fraction: min(Double(ms) / 2000, 1), tint: .mint)
  600:                                 .frame(width: 40, height: 5)
  601:                                 .clipShape(Capsule())
  602:                                 .allowsHitTesting(false)
  603:                             Text("\(ms)ms")
  604:                                 .foregroundStyle(.secondary)
  605:                                 .frame(width: 44, alignment: .trailing)
  606:                         } else {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:96` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   89:                 "holder": h.command ?? "",
   90:             ], category: .manager)
   91:             throw DashboardError.portInUse(msg)
   92:         case .unknown(let d):
   93:             app.diagnostics.warn("dashboard_port_unknown", ["detail": d], category: .manager)
   94:         }
   95: 
   96:         let params = NWParameters.tcp
   97:         // Do NOT reuse address for product dashboard — second instance must fail clearly.
   98:         params.allowLocalEndpointReuse = false
   99:         if host == "127.0.0.1" || host == "localhost" {
  100:             params.requiredInterfaceType = .loopback
  101:         }
  102: 
  103:         let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:622` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  615:             bind(stmt, 1, key)
  616:             try stepDone(stmt)
  617:             return changesUnlocked() > 0
  618:         }
  619:     }
  620: 
  621:     /// List durable notes, newest updates first.
  622:     /// - Parameters:
  623:     ///   - prefix: Optional key prefix filter (e.g. `project/`).
  624:     ///   - tag: Optional exact tag match (JSON array contains).
  625:     ///   - includeSystem: When false (default), hides internal agent and continuity keys.
  626:     ///   - limit: Max rows (clamped to `memoryQueryMaxLimit`).
  627:     public func memoryList(
  628:         prefix: String? = nil,
  629:         tag: String? = nil,
```

### `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:260` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  253:     }
  254: }
  255: 
  256: public struct OrchRoleCard: Sendable, Equatable {
  257:     public var name: String
  258:     public var live: Bool
  259:     public var status: String
  260:     public var gauge: Double
  261: 
  262:     public init(name: String, live: Bool, status: String, gauge: Double) {
  263:         self.name = name
  264:         self.live = live
  265:         self.status = status
  266:         self.gauge = gauge
  267:     }
```

### `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:262` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  255: 
  256: public struct OrchRoleCard: Sendable, Equatable {
  257:     public var name: String
  258:     public var live: Bool
  259:     public var status: String
  260:     public var gauge: Double
  261: 
  262:     public init(name: String, live: Bool, status: String, gauge: Double) {
  263:         self.name = name
  264:         self.live = live
  265:         self.status = status
  266:         self.gauge = gauge
  267:     }
  268: }
  269: 
```

### `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:266` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
  259:     public var status: String
  260:     public var gauge: Double
  261: 
  262:     public init(name: String, live: Bool, status: String, gauge: Double) {
  263:         self.name = name
  264:         self.live = live
  265:         self.status = status
  266:         self.gauge = gauge
  267:     }
  268: }
  269: 
  270: public enum ForgeUIModelFactory {
  271:     public static func mcpServers(from forge: ForgeSnapshot) -> [MCPServerCard] { forge.mcpServers }
  272:     public static func tools(from forge: ForgeSnapshot) -> [ToolCard] { forge.mcpTools }
  273:     public static func agents(from forge: ForgeSnapshot) -> [AgentCard] { forge.agents }
```

### `Sources/ForgeConductorCore/Telemetry/RealtimeMetricsEngine.swift:20` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   13: /// - CPU: `host_processor_info` / `host_statistics` (delta ticks → %)
   14: /// - RAM: `host_statistics64` `HOST_VM_INFO64`
   15: /// - Processes: `proc_pidinfo` `PROC_PIDTASKINFO` (task CPU/RSS; Mach task times)
   16: /// - Disk I/O / GPU: IOKit counters (delta rates between engine ticks)
   17: ///
   18: /// Samples at a target Hz with **tiered** collectors so heavy walks never block
   19: /// the Mach CPU/RAM path:
   20: /// - every tick: CPU + RAM (Mach realtime gauges)
   21: /// - every 3rd tick: disk I/O + GPU
   22: /// - every 10th tick: processes + disk volumes
   23: public final class RealtimeMetricsEngine: RealtimeMetricsStreaming, @unchecked Sendable {
   24:     public static let defaultTargetHz: Double = 30
   25: 
   26:     private let systemCollector: any SystemMetricsCollecting
   27:     private let tieredCollector: SystemCollector?
```

### `Sources/ForgeConductorCore/Telemetry/SystemCollector.swift:25` — gauge\|instrument\|dial\|meter\|\bMTKView\b\|makeCommandQueue\|makeRenderPipelineState\|makeBuffer\s*\(

```swift
   18: /// | GPU | IOKit + **IORegistry** (IOAccelerator / AGX / IOGPU PerformanceStatistics) |
   19: /// | Power | IOKit **IOPowerSources** (`IOPSCopyPowerSourcesInfo` …) |
   20: /// | Volumes | FileManager filesystem attributes |
   21: ///
   22: /// Tiered sampling keeps Mach CPU/RAM at ~30 Hz without waiting on heavier walks.
   23: public final class SystemCollector: SystemMetricsCollecting, @unchecked Sendable {
   24:     public enum SampleTier: Sendable {
   25:         /// CPU + RAM only (Mach realtime gauges).
   26:         case realtime
   27:         /// + disk I/O + GPU (IOKit/IORegistry).
   28:         case medium
   29:         /// Full: processes (libproc), volumes, power (IOPowerSources).
   30:         case full
   31:     }
   32: 
```

## Recurring update clocks

15 lexical hits.

### `Sources/ForgeConductorApp/AppModel.swift:233` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  226:         }
  227:     }
  228: 
  229:     public func installLMStudioPlugin() { deployToLMStudio() }
  230: 
  231:     private func startManagerPoll() {
  232:         managerPoll?.cancel()
  233:         managerPoll = Timer.publish(every: 1.0, on: .main, in: .common)
  234:             .autoconnect()
  235:             .sink { [weak self] _ in
  236:                 guard let self else { return }
  237:                 if let manager = self.manager {
  238:                     self.managerStatus = manager.statusModel()
  239:                 } else {
  240:                     self.refreshRemoteManagerStatus()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:105` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   98:         guard let device = mtl else { return }
   99:         self.device = device
  100:         view.device = device
  101:         view.delegate = self
  102:         view.clearColor = MTLClearColor(red: 0.02, green: 0.04, blue: 0.08, alpha: 1)
  103:         view.isPaused = false
  104:         view.enableSetNeedsDisplay = false
  105:         view.preferredFramesPerSecond = 24
  106:         queue = device.makeCommandQueue()
  107:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  108:         rebuild()
  109:     }
  110: 
  111:     func set(fraction: Float, color: SIMD4<Float>) {
  112:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:204` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  197:         guard let device = mtl else { return }
  198:         self.device = device
  199:         view.device = device
  200:         view.delegate = self
  201:         view.clearColor = MTLClearColor(red: 0.015, green: 0.03, blue: 0.06, alpha: 1)
  202:         view.isPaused = false
  203:         view.enableSetNeedsDisplay = false
  204:         view.preferredFramesPerSecond = 24
  205:         queue = device.makeCommandQueue()
  206:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  207:         rebuild()
  208:     }
  209: 
  210:     func set(fraction: Float, color: SIMD4<Float>) {
  211:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:330` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  323:         guard let device = mtl else { return }
  324:         self.device = device
  325:         view.device = device
  326:         view.delegate = self
  327:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
  328:         view.isPaused = false
  329:         view.enableSetNeedsDisplay = false
  330:         view.preferredFramesPerSecond = 20
  331:         queue = device.makeCommandQueue()
  332:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  333:         rebuild()
  334:     }
  335: 
  336:     func set(cores: [Float]) {
  337:         lock.lock(); self.cores = cores; lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:26` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   19:         let view = MTKView()
   20:         view.device = MTLCreateSystemDefaultDevice()
   21:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
   22:         view.colorPixelFormat = .bgra8Unorm
   23:         view.framebufferOnly = true
   24:         view.isPaused = false
   25:         view.enableSetNeedsDisplay = false
   26:         view.preferredFramesPerSecond = 30
   27:         context.coordinator.attach(to: view)
   28:         context.coordinator.update(samples: samples)
   29:         return view
   30:     }
   31: 
   32:     func updateNSView(_ nsView: MTKView, context: Context) {
   33:         context.coordinator.update(samples: samples)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:42` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   35:         self.device = device
   36:         view.device = device
   37:         view.delegate = self
   38:         view.clearColor = MTLClearColor(red: 0.01, green: 0.015, blue: 0.04, alpha: 1)
   39:         view.colorPixelFormat = .bgra8Unorm
   40:         view.isPaused = false
   41:         view.enableSetNeedsDisplay = false
   42:         view.preferredFramesPerSecond = 30
   43:         queue = device.makeCommandQueue()
   44:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   45:     }
   46: 
   47:     public func update(cpu: [Float], ram: [Float], gpu: [Float?]) {
   48:         lock.lock()
   49:         series = [
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:3` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
    1: // RigDashboardView.swift
    2: // What: Builds the real-time host and orchestration instrument panel.
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:16` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
   12: struct RigDashboardView: View {
   13:     @EnvironmentObject private var model: AppModel
   14: 
   15:     var body: some View {
   16:         // TimelineView drives UI at animation/display cadence against the continuous engine.
   17:         TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !model.autoRefresh)) { _ in
   18:             rigContent
   19:         }
   20:     }
   21: 
   22:     private var rigContent: some View {
   23:         ScrollView {
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:17` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
   12: struct RigDashboardView: View {
   13:     @EnvironmentObject private var model: AppModel
   14: 
   15:     var body: some View {
   16:         // TimelineView drives UI at animation/display cadence against the continuous engine.
   17:         TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !model.autoRefresh)) { _ in
   18:             rigContent
   19:         }
   20:     }
   21: 
   22:     private var rigContent: some View {
   23:         ScrollView {
   24:             VStack(alignment: .leading, spacing: 16) {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:211` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  204:         }
  205:         queue.asyncAfter(deadline: .now() + maxDurationSec) { [weak self] in
  206:             self?.close()
  207:         }
  208:     }
  209: 
  210:     private func startTimer() {
  211:         let t = DispatchSource.makeTimerSource(queue: queue)
  212:         t.schedule(
  213:             deadline: .now() + .milliseconds(periodMs),
  214:             repeating: .milliseconds(periodMs),
  215:             leeway: .milliseconds(max(2, periodMs / 5))
  216:         )
  217:         t.setEventHandler { [weak self] in
  218:             self?.onTick()
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:50` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   43:             "deployment_id": deploymentID,
   44:         ])
   45:         // Best-effort presence; never block MCP handshake on a locked GUI store.
   46:         refreshPresence()
   47: 
   48:         // Idle stdio sessions receive no host messages. Heartbeat on a timer so
   49:         // the dashboard does not treat a live-but-quiet serve as gone.
   50:         let heartbeat = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "forge.mcp.presence"))
   51:         heartbeat.schedule(deadline: .now() + 10, repeating: 10)
   52:         heartbeat.setEventHandler { [weak self] in
   53:             self?.refreshPresence()
   54:         }
   55:         heartbeat.resume()
   56:         defer { heartbeat.cancel() }
   57: 
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:346` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  339:     }
  340: 
  341:     // MARK: - Watchdog
  342: 
  343:     private func startWatchdog() {
  344:         stopWatchdog()
  345:         let interval = max(1, app.config.model.manager.watchdogIntervalSec)
  346:         let timer = DispatchSource.makeTimerSource(queue: runtime.queue)
  347:         timer.schedule(deadline: .now() + .seconds(interval), repeating: .seconds(interval))
  348:         timer.setEventHandler { [weak self] in
  349:             self?.watchdogTick()
  350:         }
  351:         timer.resume()
  352:         lock.lock()
  353:         runtime.watchdog = timer
```

### `Sources/ForgeConductorCore/Telemetry/RealtimeMetricsEngine.swift:87` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   80:         windowStarted = Date()
   81:         tick = 0
   82:         lock.unlock()
   83: 
   84:         // Warm full sample so heavy fields are not empty on first paint.
   85:         sampleOnce(forceFull: true)
   86: 
   87:         let t = DispatchSource.makeTimerSource(queue: queue)
   88:         t.schedule(
   89:             deadline: .now() + .milliseconds(periodMs),
   90:             repeating: .milliseconds(periodMs),
   91:             leeway: .milliseconds(max(2, periodMs / 10))
   92:         )
   93:         t.setEventHandler { [weak self] in
   94:             self?.sampleOnce(forceFull: false)
```

### `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift:75` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
   68: 
   69:         systemListenerID = realtimeEngine.addListener { [weak self] system in
   70:             self?.onSystemSample(system)
   71:         }
   72: 
   73:         // Forge/MCP composition is utility work — never the host telemetry clock.
   74:         let forgePeriod = max(0.25, intervalSec)
   75:         let t = DispatchSource.makeTimerSource(queue: forgeQueue)
   76:         t.schedule(
   77:             deadline: .now(),
   78:             repeating: .milliseconds(Int(forgePeriod * 1000)),
   79:             leeway: .milliseconds(50)
   80:         )
   81:         t.setEventHandler { [weak self] in
   82:             self?.recomposeForgeAndPublish()
```

### `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift:233` — TimelineView\|Timer\.publish\|scheduledTimer\|makeTimerSource\|preferredFramesPerSecond

```swift
  226:         return TelemetryHealthReport(
  227:             ok: true,
  228:             service: "forge-telemetry",
  229:             runtime: Self.runtimeIdentifier,
  230:             interferesWithMCP: false,
  231:             mode: realtimeEngine.isRunning ? "continuous-native" : "continuous-native-idle",
  232:             collectors: "RealtimeMetricsEngine@\(String(format: "%.0f", target))Hz(meas \(String(format: "%.1f", hz)))+ForgeCollector",
  233:             ui: "ForgeConductor.app TimelineView + Metal + SSE stream",
  234:             nodeRequired: false
  235:         )
  236:     }
  237: 
  238:     public func healthDictionary() -> [String: Any] {
  239:         var d = health().asDictionary()
  240:         d["sample_hz_target"] = realtimeEngine.targetSampleHz
```

## Task creation and cancellation

27 lexical hits.

### `Sources/ForgeConductorApp/AppModel.swift:170` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  163:             ], category: .manager)
  164:         }
  165:     }
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
```

### `Sources/ForgeConductorApp/AppModel.swift:206` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  199:     }
  200: 
  201:     public func deployToLMStudio() {
  202:         guard !isInstallingPlugin, let forgeApp = app else { return }
  203:         isInstallingPlugin = true
  204:         lmStudioPluginMessage = nil
  205:         let binary = preferredServeBinary
  206:         Task { [weak self] in
  207:             do {
  208:                 let result = try await Task.detached {
  209:                     try forgeApp.lmStudioDeploy.deploy(preferredBinary: binary)
  210:                 }.value
  211:                 await MainActor.run {
  212:                     self?.lmStudioPluginMessage = result.message
  213:                     self?.refreshLMStudioPluginStatus()
```

### `Sources/ForgeConductorApp/AppModel.swift:208` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  201:     public func deployToLMStudio() {
  202:         guard !isInstallingPlugin, let forgeApp = app else { return }
  203:         isInstallingPlugin = true
  204:         lmStudioPluginMessage = nil
  205:         let binary = preferredServeBinary
  206:         Task { [weak self] in
  207:             do {
  208:                 let result = try await Task.detached {
  209:                     try forgeApp.lmStudioDeploy.deploy(preferredBinary: binary)
  210:                 }.value
  211:                 await MainActor.run {
  212:                     self?.lmStudioPluginMessage = result.message
  213:                     self?.refreshLMStudioPluginStatus()
  214:                     self?.isInstallingPlugin = false
  215:                     self?.refreshDiagnosticsPreview()
```

### `Sources/ForgeConductorApp/AppModel.swift:232` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  225:             }
  226:         }
  227:     }
  228: 
  229:     public func installLMStudioPlugin() { deployToLMStudio() }
  230: 
  231:     private func startManagerPoll() {
  232:         managerPoll?.cancel()
  233:         managerPoll = Timer.publish(every: 1.0, on: .main, in: .common)
  234:             .autoconnect()
  235:             .sink { [weak self] _ in
  236:                 guard let self else { return }
  237:                 if let manager = self.manager {
  238:                     self.managerStatus = manager.statusModel()
  239:                 } else {
```

### `Sources/ForgeConductorApp/AppModel.swift:248` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  241:                 }
  242:             }
  243:     }
  244: 
  245:     private func refreshRemoteManagerStatus() {
  246:         guard let client = remoteManager, !managerPollInFlight else { return }
  247:         managerPollInFlight = true
  248:         Task { [weak self] in
  249:             do {
  250:                 let status = try await client.status()
  251:                 guard let self else { return }
  252:                 self.managerStatus = status
  253:                 self.managerPollInFlight = false
  254:                 if self.remoteManagerLastError != nil {
  255:                     self.managerMessage = "Manager connection restored"
```

### `Sources/ForgeConductorApp/AppModel.swift:395` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  388:     }
  389: 
  390:     // MARK: - Manager
  391: 
  392:     public func managerStart() {
  393:         if let client = remoteManager {
  394:             managerMessage = "Starting service…"
  395:             Task { [weak self] in
  396:                 do {
  397:                     let status = try await client.startService()
  398:                     guard let self else { return }
  399:                     self.managerStatus = status
  400:                     self.managerMessage = "Service started"
  401:                     self.app?.diagnostics.info("manager_start", ["via": "loopback"], category: .manager)
  402:                     self.refresh(force: true)
```

### `Sources/ForgeConductorApp/AppModel.swift:427` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  420:             app?.diagnostics.error("manager_start_failed", ["error": "\(error)"], category: .manager)
  421:         }
  422:     }
  423: 
  424:     public func managerStop() {
  425:         if let client = remoteManager {
  426:             managerMessage = "Stopping service…"
  427:             Task { [weak self] in
  428:                 do {
  429:                     let status = try await client.stopService()
  430:                     guard let self else { return }
  431:                     self.managerStatus = status
  432:                     self.managerMessage = "Service stopped (control plane stays available)"
  433:                     self.app?.diagnostics.info("manager_stop", ["via": "loopback"], category: .manager)
  434:                     self.refresh(force: true)
```

### `Sources/ForgeConductorApp/AppModel.swift:458` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  451:             managerMessage = "Stop failed: \(error)"
  452:         }
  453:     }
  454: 
  455:     public func managerRestart() {
  456:         if let client = remoteManager {
  457:             managerMessage = "Restarting service…"
  458:             Task { [weak self] in
  459:                 do {
  460:                     let status = try await client.restartService()
  461:                     guard let self else { return }
  462:                     self.managerStatus = status
  463:                     self.managerMessage = "Service restarted"
  464:                     self.app?.diagnostics.info("manager_restart", ["via": "loopback"], category: .manager)
  465:                     self.refresh(force: true)
```

### `Sources/ForgeConductorApp/AppModel.swift:501` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  494:         setPort = app.config.int("dashboard", "port", default: 7788)
  495:         setRefresh = app.config.int("dashboard", "refresh_interval_sec", default: 8)
  496:         setWatchdog = app.config.int("manager", "watchdog_interval_sec", default: 3)
  497:         setIdleTTL = app.config.int("sessions", "idle_ttl_sec", default: 14_400)
  498:         setShellTimeout = app.config.int("shell", "default_timeout_sec", default: 30)
  499:         setAutoRestart = app.config.bool("manager", "auto_restart", default: true)
  500:         if let client = remoteManager {
  501:             Task { [weak self] in
  502:                 do {
  503:                     let settings = try await client.settings()
  504:                     self?.apply(settings: settings)
  505:                 } catch {
  506:                     self?.recordRemoteManagerFailure(action: "Load settings", error: error)
  507:                 }
  508:             }
```

### `Sources/ForgeConductorApp/AppModel.swift:524` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  517:             autoRestart: setAutoRestart,
  518:             watchdogIntervalSec: setWatchdog,
  519:             sessionIdleTTLSec: setIdleTTL,
  520:             shellTimeoutSec: setShellTimeout
  521:         )
  522:         if let client = remoteManager {
  523:             managerMessage = "Saving settings…"
  524:             Task { [weak self] in
  525:                 do {
  526:                     let settings = try await client.updateSettings(patch, apply: true)
  527:                     guard let self else { return }
  528:                     self.apply(settings: settings)
  529:                     self.remoteManager = ManagerDashboardClient(
  530:                         host: settings.dashboardHost,
  531:                         port: settings.dashboardPort
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:48` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   41:         // Always run continuous host sampling for the GUI.
   42:         if !app.telemetry.realtimeEngine.isRunning {
   43:             app.telemetry.startBackgroundRefresh(intervalSec: 0.5)
   44:         }
   45: 
   46:         // TelemetryService publishes one composed frame for every host sample.
   47:         frameListenerID = app.telemetry.addListener { [weak self] frame in
   48:             Task { @MainActor in
   49:                 guard let self, self.autoRefresh else { return }
   50:                 self.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   51:                 self.apply(frame)
   52:             }
   53:         }
   54: 
   55:         // One forge seed so MCP/agent cards appear immediately; host already streaming.
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:73` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   66:     /// Manual recompose of forge cards only (does not replace the continuous host stream).
   67:     public func refresh(force: Bool) {
   68:         guard let app else { return }
   69:         if isLoading && !force { return }
   70:         objectWillChange.send()
   71:         isLoading = true
   72:         updateSubject.send()
   73:         Task { [weak self] in
   74:             do {
   75:                 let frame = try await Task.detached {
   76:                     try app.telemetry.snapshotTyped(force: true)
   77:                 }.value
   78:                 await MainActor.run {
   79:                     self?.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   80:                     self?.apply(frame)
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:75` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   68:         guard let app else { return }
   69:         if isLoading && !force { return }
   70:         objectWillChange.send()
   71:         isLoading = true
   72:         updateSubject.send()
   73:         Task { [weak self] in
   74:             do {
   75:                 let frame = try await Task.detached {
   76:                     try app.telemetry.snapshotTyped(force: true)
   77:                 }.value
   78:                 await MainActor.run {
   79:                     self?.measuredHz = app.telemetry.realtimeEngine.measuredSampleHz
   80:                     self?.apply(frame)
   81:                 }
   82:             } catch {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:133` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  126:             default:
  127:                 break
  128:             }
  129:         }
  130:         listener.start(queue: queue)
  131:         let wait = gate.wait(timeout: .now() + 3)
  132:         if wait == .timedOut {
  133:             listener.cancel()
  134:             app.diagnostics.error("dashboard_bind_timeout", ["port": "\(port)"], category: .manager)
  135:             throw DashboardError.bindTimeout(port)
  136:         }
  137:         if let bindError = bindResult.recordedError() {
  138:             listener.cancel()
  139:             throw bindError
  140:         }
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:138` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  131:         let wait = gate.wait(timeout: .now() + 3)
  132:         if wait == .timedOut {
  133:             listener.cancel()
  134:             app.diagnostics.error("dashboard_bind_timeout", ["port": "\(port)"], category: .manager)
  135:             throw DashboardError.bindTimeout(port)
  136:         }
  137:         if let bindError = bindResult.recordedError() {
  138:             listener.cancel()
  139:             throw bindError
  140:         }
  141: 
  142:         lock.lock()
  143:         self.listener = listener
  144:         isRunning = true
  145:         lock.unlock()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:151` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  144:         isRunning = true
  145:         lock.unlock()
  146:     }
  147: 
  148:     public func stop() {
  149:         lock.lock()
  150:         defer { lock.unlock() }
  151:         listener?.cancel()
  152:         listener = nil
  153:         isRunning = false
  154:     }
  155: 
  156:     /// Run until interrupted (SIGINT/SIGTERM).
  157:     public func runForever() throws {
  158:         try start()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:183` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  176:         connection.start(queue: queue)
  177:         receive(on: connection, buffer: Data())
  178:     }
  179: 
  180:     private func receive(on connection: NWConnection, buffer: Data) {
  181:         connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
  182:             guard let self else {
  183:                 connection.cancel()
  184:                 return
  185:             }
  186:             if let error {
  187:                 self.app.diagnostics.warn("dashboard_recv_error", ["error": "\(error)"])
  188:                 connection.cancel()
  189:                 return
  190:             }
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:188` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  181:         connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
  182:             guard let self else {
  183:                 connection.cancel()
  184:                 return
  185:             }
  186:             if let error {
  187:                 self.app.diagnostics.warn("dashboard_recv_error", ["error": "\(error)"])
  188:                 connection.cancel()
  189:                 return
  190:             }
  191:             var buf = buffer
  192:             if let data { buf.append(data) }
  193:             switch DashboardHTTPRequestParser.parse(buf, streamComplete: isComplete) {
  194:             case .incomplete:
  195:                 self.receive(on: connection, buffer: buf)
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:54` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   47:         header += "Connection: close\r\n"
   48:         header += "Cache-Control: no-store\r\n"
   49:         header += securityHeaders
   50:         header += "\r\n"
   51:         var payload = Data(header.utf8)
   52:         payload.append(data)
   53:         connection.send(content: payload, completion: .contentProcessed { _ in
   54:             connection.cancel()
   55:         })
   56:     }
   57: 
   58:     public func respond(
   59:         _ connection: NWConnection,
   60:         status: Int,
   61:         body: String,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:92` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   85:         header += "Cache-Control: no-store\r\n"
   86:         header += securityHeaders
   87:         for h in extraHeaders { header += h + "\r\n" }
   88:         header += "\r\n"
   89:         var payload = Data(header.utf8)
   90:         payload.append(bodyData)
   91:         connection.send(content: payload, completion: .contentProcessed { _ in
   92:             connection.cancel()
   93:         })
   94:     }
   95: 
   96:     /// Legacy one-shot SSE (compat). Prefer `startRealtimeSSE`.
   97:     public func respondSSE(connection: NWConnection, snapshot: [String: Any]) {
   98:         let json = (try? JSONSupport.string(from: snapshot)) ?? "{}"
   99:         var body = ": connected\n\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:111` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  104:         header += "Cache-Control: no-cache, no-transform\r\n"
  105:         header += "Connection: close\r\n"
  106:         header += securityHeaders
  107:         header += "Content-Length: \(bodyData.count)\r\n\r\n"
  108:         var payload = Data(header.utf8)
  109:         payload.append(bodyData)
  110:         connection.send(content: payload, completion: .contentProcessed { _ in
  111:             connection.cancel()
  112:         })
  113:     }
  114: 
  115:     private var securityHeaders: String {
  116:         "X-Content-Type-Options: nosniff\r\n"
  117:             + "Referrer-Policy: no-referrer\r\n"
  118:             + "Cross-Origin-Resource-Policy: same-origin\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:326` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  319:     public func close() {
  320:         lock.lock()
  321:         if closed {
  322:             lock.unlock()
  323:             return
  324:         }
  325:         closed = true
  326:         timer?.cancel()
  327:         timer = nil
  328:         sendChain.removeAll()
  329:         sending = false
  330:         lock.unlock()
  331:         responder?.releaseStream(self)
  332:         connection.send(
  333:             content: nil,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:337` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  330:         lock.unlock()
  331:         responder?.releaseStream(self)
  332:         connection.send(
  333:             content: nil,
  334:             contentContext: .defaultStream,
  335:             isComplete: true,
  336:             completion: .contentProcessed { [weak self] _ in
  337:                 self?.connection.cancel()
  338:             }
  339:         )
  340:     }
  341: }
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:56` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   49:         // the dashboard does not treat a live-but-quiet serve as gone.
   50:         let heartbeat = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "forge.mcp.presence"))
   51:         heartbeat.schedule(deadline: .now() + 10, repeating: 10)
   52:         heartbeat.setEventHandler { [weak self] in
   53:             self?.refreshPresence()
   54:         }
   55:         heartbeat.resume()
   56:         defer { heartbeat.cancel() }
   57: 
   58:         var lastPresence = Date()
   59:         let reader = MCPStreamReader(handle: input)
   60:         while let message = try reader.readMessage() {
   61:             // Refresh presence while the stdio session is active (dashboard TTL ~45s).
   62:             let now = Date()
   63:             if now.timeIntervalSince(lastPresence) >= 15 {
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:363` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
  356: 
  357:     private func restartWatchdog() {
  358:         startWatchdog()
  359:     }
  360: 
  361:     private func stopWatchdog() {
  362:         lock.lock()
  363:         runtime.watchdog?.cancel()
  364:         runtime.watchdog = nil
  365:         lock.unlock()
  366:     }
  367: 
  368:     private func watchdogTick() {
  369:         lock.lock()
  370:         let want = runtime.desiredRunning
```

### `Sources/ForgeConductorCore/Telemetry/RealtimeMetricsEngine.swift:104` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   97:         lock.lock()
   98:         timer = t
   99:         lock.unlock()
  100:     }
  101: 
  102:     public func stop() {
  103:         lock.lock()
  104:         timer?.cancel()
  105:         timer = nil
  106:         _running = false
  107:         lock.unlock()
  108:     }
  109: 
  110:     @discardableResult
  111:     public func addListener(_ block: @escaping (SystemMetrics) -> Void) -> UUID {
```

### `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift:100` — \bTask\s*\{\|Task\.detached\|\.cancel\s*\(\|Task\.isCancelled\|checkCancellation

```swift
   93:     public func stopBackgroundRefresh() {
   94:         if let id = systemListenerID {
   95:             realtimeEngine.removeListener(id)
   96:             systemListenerID = nil
   97:         }
   98:         realtimeEngine.stop()
   99:         lock.lock()
  100:         forgeTimer?.cancel()
  101:         forgeTimer = nil
  102:         lock.unlock()
  103:     }
  104: 
  105:     @discardableResult
  106:     public func addListener(_ block: @escaping (TelemetrySnapshot) -> Void) -> UUID {
  107:         let id = UUID()
```

## Observers and subscriptions

6 lexical hits.

### `Sources/ForgeConductorApp/AppModel.swift:55` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
   48: 
   49:     public private(set) var app: ForgeApp?
   50:     public private(set) var manager: ManagerNode?
   51:     public private(set) var remoteManager: ManagerDashboardClient?
   52:     public private(set) var deployController: AppDeployController?
   53:     public private(set) var telemetryBinding = AppTelemetryBinding()
   54: 
   55:     private var managerPoll: AnyCancellable?
   56:     private var telemetryBag: AnyCancellable?
   57:     private var managerPollInFlight = false
   58:     private var remoteManagerLastError: String?
   59: 
   60:     public enum AppTab: String, CaseIterable, Identifiable {
   61:         case rig = "FORGE RIG"
   62:         case mcp = "LM Studio MCP"
```

### `Sources/ForgeConductorApp/AppModel.swift:56` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
   49:     public private(set) var app: ForgeApp?
   50:     public private(set) var manager: ManagerNode?
   51:     public private(set) var remoteManager: ManagerDashboardClient?
   52:     public private(set) var deployController: AppDeployController?
   53:     public private(set) var telemetryBinding = AppTelemetryBinding()
   54: 
   55:     private var managerPoll: AnyCancellable?
   56:     private var telemetryBag: AnyCancellable?
   57:     private var managerPollInFlight = false
   58:     private var remoteManagerLastError: String?
   59: 
   60:     public enum AppTab: String, CaseIterable, Identifiable {
   61:         case rig = "FORGE RIG"
   62:         case mcp = "LM Studio MCP"
   63:         case agents = "Agents"
```

### `Sources/ForgeConductorApp/AppModel.swift:173` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
  166: 
  167:     /// Mirror complete stream frames into AppModel published fields for views.
  168:     /// Driven by one post-apply event per frame — no snapshot polling timer.
  169:     private func bindTelemetryMirror() {
  170:         telemetryBag?.cancel()
  171:         telemetryBag = telemetryBinding.updates
  172:             .receive(on: RunLoop.main)
  173:             .sink { [weak self] _ in
  174:                 self?.syncFromTelemetryBinding()
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
```

### `Sources/ForgeConductorApp/AppModel.swift:235` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
  228: 
  229:     public func installLMStudioPlugin() { deployToLMStudio() }
  230: 
  231:     private func startManagerPoll() {
  232:         managerPoll?.cancel()
  233:         managerPoll = Timer.publish(every: 1.0, on: .main, in: .common)
  234:             .autoconnect()
  235:             .sink { [weak self] _ in
  236:                 guard let self else { return }
  237:                 if let manager = self.manager {
  238:                     self.managerStatus = manager.statusModel()
  239:                 } else {
  240:                     self.refreshRemoteManagerStatus()
  241:                 }
  242:             }
```

### `Sources/ForgeConductorApp/ForgeConductorApp.swift:63` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
   56:     }
   57: }
   58: 
   59: @MainActor
   60: final class ForgeApplicationDelegate: NSObject, NSApplicationDelegate, ObservableObject {
   61:     let model = AppModel()
   62: 
   63:     private var modelObservation: AnyCancellable?
   64:     private var mainWindowController: ForgeMainWindowController?
   65: 
   66:     override init() {
   67:         super.init()
   68:         modelObservation = model.objectWillChange.sink { [weak self] _ in
   69:             self?.objectWillChange.send()
   70:         }
```

### `Sources/ForgeConductorApp/ForgeConductorApp.swift:68` — addObserver\s*\(\|removeObserver\s*\(\|\.sink\s*\{\|\.store\s*\(\s*in:\|AnyCancellable

```swift
   61:     let model = AppModel()
   62: 
   63:     private var modelObservation: AnyCancellable?
   64:     private var mainWindowController: ForgeMainWindowController?
   65: 
   66:     override init() {
   67:         super.init()
   68:         modelObservation = model.objectWillChange.sink { [weak self] _ in
   69:             self?.objectWillChange.send()
   70:         }
   71:     }
   72: 
   73:     func applicationDidFinishLaunching(_ notification: Notification) {
   74:         NSApp.setActivationPolicy(.regular)
   75:         NSApp.activate(ignoringOtherApps: true)
```

## Process, Pipe, and FileHandle lifetime

174 lexical hits.

### `Package.swift:22` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   15:         .executable(name: "forge-conductor-app", targets: ["ForgeConductorApp"]),
   16:     ],
   17:     targets: [
   18:         .target(
   19:             name: "ForgeConductorCore",
   20:             path: "Sources/ForgeConductorCore",
   21:             resources: [
   22:                 .process("Resources"),
   23:             ]
   24:         ),
   25:         .executableTarget(
   26:             name: "ForgeConductorCLI",
   27:             dependencies: ["ForgeConductorCore"],
   28:             path: "Sources/ForgeConductorCLI"
   29:         ),
```

### `Package.swift:41` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   34:             exclude: ["Resources"]
   35:         ),
   36:         .testTarget(
   37:             name: "ForgeConductorTests",
   38:             dependencies: ["ForgeConductorCore", "ForgeConductorCLI"],
   39:             path: "Tests/ForgeConductorTests",
   40:             resources: [
   41:                 .process("Fixtures"),
   42:             ]
   43:         ),
   44:     ],
   45:     swiftLanguageModes: [.v5]
   46: )
```

### `Sources/ForgeConductorApp/AppModel.swift:16` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    9: import AppKit
   10: import ForgeConductorCore
   11: import SwiftUI
   12: 
   13: /// Owns the macOS app's observable state and coordinates every user-facing module.
   14: ///
   15: /// Views read immutable projections from this model and send user intent back through
   16: /// its methods. The model keeps process control, persistence, deployment, and telemetry
   17: /// work inside Core services so the SwiftUI layer remains declarative and testable.
   18: @MainActor
   19: public final class AppModel: ObservableObject {
   20:     @Published public private(set) var system: SystemMetrics?
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
```

### `Sources/ForgeConductorApp/AppModel.swift:117` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  110:             refresh(force: true)
  111:         } catch {
  112:             lastError = "Bootstrap failed: \(error)"
  113:         }
  114:     }
  115: 
  116:     /// A GUI is a presentation client when the LaunchAgent manager already
  117:     /// owns the dashboard. Only one process is ever allowed to bind the port.
  118:     private func attachToOrStartManager(app forgeApp: ForgeApp) {
  119:         let host = forgeApp.config.model.dashboard.host
  120:         let port = forgeApp.config.model.dashboard.port
  121:         let currentPID = ProcessInfo.processInfo.processIdentifier
  122:         var externalPID = ManagerPIDFile.runningPID(paths: forgeApp.paths)
  123:         if externalPID == currentPID {
  124:             externalPID = nil
```

### `Sources/ForgeConductorApp/ForgeConductorApp.swift:2` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ForgeConductorApp.swift
    2: // What: Defines the process and scene entry points for the native macOS product.
    3: // How: Command-line modes are routed before SwiftUI starts; GUI mode then
    4: // creates scenes, injects AppModel, and activates a regular foreground app.
    5: // Why: One binary can safely serve GUI, manager, and MCP roles without parallel bootstraps.
    6: 
    7: import SwiftUI
    8: import AppKit
    9: import Combine
```

### `Sources/ForgeConductorApp/ForgeConductorApp.swift:12` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    5: // Why: One binary can safely serve GUI, manager, and MCP roles without parallel bootstraps.
    6: 
    7: import SwiftUI
    8: import AppKit
    9: import Combine
   10: import ForgeConductorCore
   11: 
   12: /// Process entry: the app binary already receives argv from LaunchAgent and must
   13: /// also accept `serve` from LM Studio. Route non-GUI modes before SwiftUI starts.
   14: @main
   15: enum ForgeConductorMain {
   16:     static func main() {
   17:         // LaunchAgent:  …/Forge Conductor manager run --home …
   18:         // LM Studio:    …/Forge Conductor serve   (+ FORGE_MCP_ROLE)
   19:         // Double-click: no subcommand → GUI
```

### `Sources/ForgeConductorApp/Views/MCPServersView.swift:13` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Presents MCP server health and the transactional LM Studio deployment workflow.
   11: ///
   12: /// The view reports installer and verification states while `AppModel` performs all
   13: /// filesystem, process, reload, and connector work through Core abstractions.
   14: struct MCPServersView: View {
   15:     @EnvironmentObject private var model: AppModel
   16: 
   17:     var body: some View {
   18:         VStack(alignment: .leading, spacing: 14) {
   19:             HStack(spacing: 12) {
   20:                 Text("LM Studio · MCP")
```

### `Sources/ForgeConductorApp/Views/MCPServersView.swift:135` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  128:             if let path = st?.binaryPath {
  129:                 Text("Serve binary: \(path)")
  130:                     .font(.system(.caption2, design: .monospaced))
  131:                     .foregroundStyle(.secondary)
  132:                     .textSelection(.enabled)
  133:                     .lineLimit(2)
  134:             }
  135:             Text("No manual file editing or LM Studio restart is required. A deployment may relaunch LM Studio when hot reload cannot replace a stale plugin process; plugin selection remains a per-chat LM Studio choice.")
  136:                 .font(.caption2)
  137:                 .foregroundStyle(.secondary)
  138:         }
  139:         .padding(12)
  140:         .frame(maxWidth: .infinity, alignment: .leading)
  141:         .background(
  142:             RoundedRectangle(cornerRadius: 10)
```

### `Sources/ForgeConductorApp/Views/MCPServersView.swift:199` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  192:                     .foregroundStyle(.secondary)
  193:                     .lineLimit(2)
  194:             }
  195:             HStack(spacing: 5) {
  196:                 Circle()
  197:                     .fill(color)
  198:                     .frame(width: 8, height: 8)
  199:                 Text(s.live ? "live process" : (s.health == "config" ? "configured · starts on demand" : "not running"))
  200:                     .font(.caption2)
  201:                     .foregroundStyle(.secondary)
  202:             }
  203:             .frame(maxWidth: .infinity, alignment: .trailing)
  204:         }
  205:         .padding(16)
  206:         .background(
```

### `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:5` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ManagerSettingsView.swift
    2: // What: Provides native controls for the persistent manager and its configuration.
    3: // How: Form fields bind to staged AppModel values, while commands call typed manager
    4: // operations and render returned health/doctor information.
    5: // Why: A single settings module replaces ad-hoc process and configuration mutations.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Full management console parity with classic `/control` surface.
   11: struct ManagerSettingsView: View {
   12:     @EnvironmentObject private var model: AppModel
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:4` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // RigDashboardView.swift
    2: // What: Builds the real-time host and orchestration instrument panel.
    3: // How: A display-cadence TimelineView projects AppModel telemetry into modular Metal
    4: // gauges, charts, process panels, MCP cards, and event summaries.
    5: // Why: The rig provides one coherent operational view without slowing Core sampling.
    6: 
    7: import SwiftUI
    8: import ForgeConductorCore
    9: 
   10: /// Single-screen FORGE RIG board — full panel parity; **all gauges are Metal**.
   11: /// Display updates continuously from the realtime metrics engine (not a 2s snapshot).
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:402` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  395: 
  396:     // MARK: MCP servers — Metal rings
  397: 
  398:     private var mcpServersPanel: some View {
  399:         let cards = model.mcpServerCards
  400:         return panel("MCP SERVERS", meta: "\(cards.count) cards · Metal rings") {
  401:             if cards.isEmpty {
  402:                 Text("NO MCP PRESENCE — WAITING FOR HEARTBEAT / PROCESS SCAN")
  403:                     .font(.caption).foregroundStyle(.secondary)
  404:             } else {
  405:                 LazyVGrid(columns: [GridItem(.adaptive(minimum: 180), spacing: 10)], spacing: 10) {
  406:                     ForEach(Array(cards.prefix(12).enumerated()), id: \.offset) { _, s in
  407:                         VStack(alignment: .leading, spacing: 8) {
  408:                             HStack(spacing: 8) {
  409:                                 Text(s.label)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:348` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  347:         let installer = ManagerInstaller(app: app)
  348:         // Stop process first
  349:         _ = ManagerPIDFile.signalStop(paths: app.paths)
  350:         let removed = try installer.uninstallLoginAgent()
  351:         print(removed ? "Login agent removed" : "Login agent was not installed")
  352:     }
  353: 
  354:     static func managerAllowlist(_ args: [String]) throws {
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:401` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  394:         let installer = ManagerInstaller(app: app)
  395:         let exe: String
  396:         if FileManager.default.isExecutableFile(atPath: installer.installedBinaryURL.path) {
  397:             exe = installer.installedBinaryURL.path
  398:         } else {
  399:             exe = try SelfExecutable.path()
  400:         }
  401:         let process = Process()
  402:         process.executableURL = URL(fileURLWithPath: exe)
  403:         var childArgs = ["manager", "run"]
  404:         if let home = homeOverride(args) {
  405:             childArgs += ["--home", home.path]
  406:         }
  407:         if args.contains("--open") {
  408:             childArgs.append("--open")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:402` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  395:         let exe: String
  396:         if FileManager.default.isExecutableFile(atPath: installer.installedBinaryURL.path) {
  397:             exe = installer.installedBinaryURL.path
  398:         } else {
  399:             exe = try SelfExecutable.path()
  400:         }
  401:         let process = Process()
  402:         process.executableURL = URL(fileURLWithPath: exe)
  403:         var childArgs = ["manager", "run"]
  404:         if let home = homeOverride(args) {
  405:             childArgs += ["--home", home.path]
  406:         }
  407:         if args.contains("--open") {
  408:             childArgs.append("--open")
  409:         }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:410` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  403:         var childArgs = ["manager", "run"]
  404:         if let home = homeOverride(args) {
  405:             childArgs += ["--home", home.path]
  406:         }
  407:         if args.contains("--open") {
  408:             childArgs.append("--open")
  409:         }
  410:         process.arguments = childArgs
  411: 
  412:         try app.paths.ensureLayout()
  413:         if !FileManager.default.fileExists(atPath: app.paths.managerLog.path) {
  414:             FileManager.default.createFile(atPath: app.paths.managerLog.path, contents: nil)
  415:         }
  416:         let log = try FileHandle(forWritingTo: app.paths.managerLog)
  417:         try log.seekToEnd()
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:420` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  413:         if !FileManager.default.fileExists(atPath: app.paths.managerLog.path) {
  414:             FileManager.default.createFile(atPath: app.paths.managerLog.path, contents: nil)
  415:         }
  416:         let log = try FileHandle(forWritingTo: app.paths.managerLog)
  417:         try log.seekToEnd()
  418:         let banner = Data("\n--- manager start \(ISO8601.string(from: Date())) ---\n".utf8)
  419:         try log.write(contentsOf: banner)
  420:         process.standardOutput = log
  421:         process.standardError = log
  422:         process.standardInput = FileHandle.nullDevice
  423: 
  424:         try process.run()
  425:         // Wait for pid file / listener
  426:         var launched: Int32?
  427:         for _ in 0..<30 {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:421` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  414:             FileManager.default.createFile(atPath: app.paths.managerLog.path, contents: nil)
  415:         }
  416:         let log = try FileHandle(forWritingTo: app.paths.managerLog)
  417:         try log.seekToEnd()
  418:         let banner = Data("\n--- manager start \(ISO8601.string(from: Date())) ---\n".utf8)
  419:         try log.write(contentsOf: banner)
  420:         process.standardOutput = log
  421:         process.standardError = log
  422:         process.standardInput = FileHandle.nullDevice
  423: 
  424:         try process.run()
  425:         // Wait for pid file / listener
  426:         var launched: Int32?
  427:         for _ in 0..<30 {
  428:             Thread.sleep(forTimeInterval: 0.1)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:422` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  415:         }
  416:         let log = try FileHandle(forWritingTo: app.paths.managerLog)
  417:         try log.seekToEnd()
  418:         let banner = Data("\n--- manager start \(ISO8601.string(from: Date())) ---\n".utf8)
  419:         try log.write(contentsOf: banner)
  420:         process.standardOutput = log
  421:         process.standardError = log
  422:         process.standardInput = FileHandle.nullDevice
  423: 
  424:         try process.run()
  425:         // Wait for pid file / listener
  426:         var launched: Int32?
  427:         for _ in 0..<30 {
  428:             Thread.sleep(forTimeInterval: 0.1)
  429:             if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:424` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  417:         try log.seekToEnd()
  418:         let banner = Data("\n--- manager start \(ISO8601.string(from: Date())) ---\n".utf8)
  419:         try log.write(contentsOf: banner)
  420:         process.standardOutput = log
  421:         process.standardError = log
  422:         process.standardInput = FileHandle.nullDevice
  423: 
  424:         try process.run()
  425:         // Wait for pid file / listener
  426:         var launched: Int32?
  427:         for _ in 0..<30 {
  428:             Thread.sleep(forTimeInterval: 0.1)
  429:             if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
  430:                 launched = pid
  431:                 break
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:441` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  434:         let host = app.config.string("dashboard", "host", default: "127.0.0.1")
  435:         let port = app.config.int("dashboard", "port", default: 7788)
  436:         if let launched {
  437:             print("Manager started (pid \(launched))")
  438:             print("  dashboard: http://\(host):\(port)/")
  439:             print("  log: \(app.paths.managerLog.path)")
  440:         } else {
  441:             fputs("Manager process launched but pid file not seen yet. Check \(app.paths.managerLog.path)\n", stderr)
  442:             exit(1)
  443:         }
  444:     }
  445: 
  446:     static func managerStop(_ args: [String]) throws {
  447:         let paths = AppPaths(home: homeOverride(args))
  448:         try paths.ensureLayout()
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:463` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  456:             for _ in 0..<50 {
  457:                 if ManagerPIDFile.runningPID(paths: paths) == nil { break }
  458:                 Thread.sleep(forTimeInterval: 0.1)
  459:             }
  460:             if ManagerPIDFile.runningPID(paths: paths) == nil {
  461:                 print("Manager stopped (was pid \(pid))")
  462:             } else {
  463:                 fputs("Sent SIGTERM to \(pid); process still alive\n", stderr)
  464:                 exit(1)
  465:             }
  466:         } else {
  467:             fputs("Failed to signal manager\n", stderr)
  468:             exit(1)
  469:         }
  470:     }
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:385` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  378:                     doneDefinition: spec?.doneDefinition ?? []
  379:                 )
  380:                 try setBinding(clientID: clientID, binding: binding)
  381:                 diagnostics.warn("agent_binding_rehydrated", [
  382:                     "source": "open_session",
  383:                     "agent_id": binding.agentID,
  384:                     "session_id": s.id.rawValue,
  385:                     "message": "In-process binding missing; rehydrated from SQLite",
  386:                 ])
  387:                 return binding
  388:             }
  389:         }
  390:         return nil
  391:     }
  392: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:504` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  497:                     "error": "\(error)",
  498:                 ], category: .general)
  499:                 return PersistenceOutcome(packet: packet, projectionWarning: "\(error)")
  500:             }
  501:         }
  502:     }
  503: 
  504:     /// SQLite is authoritative. Rebuild the readable projections at process start
  505:     /// so an interrupted write cannot leave LATEST/current-task.json out of sync.
  506:     private func reconcileProjections() throws {
  507:         guard lock.lock(before: Date().addingTimeInterval(Self.persistenceLockTimeout)) else {
  508:             throw posixPersistenceError(operation: "lock continuity service", code: EBUSY)
  509:         }
  510:         defer { lock.unlock() }
  511:         try paths.ensureLayout()
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:569` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  562:     }
  563: 
  564:     /// Primary and fallback MCP processes share one home. An advisory file lock
  565:     /// makes the SQLite write and its JSON/Markdown projections one serialized unit.
  566:     private func withPersistenceFileLock<T>(_ body: () throws -> T) throws -> T {
  567:         let deadline = Date().addingTimeInterval(Self.persistenceLockTimeout)
  568:         guard Self.processPersistenceLock.lock(before: deadline) else {
  569:             throw posixPersistenceError(operation: "lock process continuity service", code: EBUSY)
  570:         }
  571:         defer { Self.processPersistenceLock.unlock() }
  572: 
  573:         let mode = mode_t(S_IRUSR | S_IWUSR)
  574:         let descriptor = paths.memoryContinuityLock.path.withCString {
  575:             Darwin.open($0, O_CREAT | O_RDWR, mode)
  576:         }
```

### `Sources/ForgeConductorCore/Application/ForgeProcessEntry.swift:2` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ForgeProcessEntry.swift
    2: // What: Routes a shared executable into GUI, MCP-server, or manager process modes.
    3: // How: It parses argv before UI startup, resolves the configured home, and transfers
    4: // control to the corresponding Core service with one well-defined exit path.
    5: // Why: A unified binary remains safe only when mutually exclusive roles are explicit.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
```

### `Sources/ForgeConductorCore/Application/ForgeProcessEntry.swift:10` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    3: // How: It parses argv before UI startup, resolves the configured home, and transfers
    4: // control to the corresponding Core service with one well-defined exit path.
    5: // Why: A unified binary remains safe only when mutually exclusive roles are explicit.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Shared process entry for the **app binary** and any host that spawns it with argv.
   11: ///
   12: /// Evidence already in-tree:
   13: /// - LaunchAgent ProgramArguments: `Forge Conductor.app/.../Forge Conductor manager run --home …`
   14: ///   (`ManagerInstaller.installLoginAgent`)
   15: /// - LM Studio mcp.json / mcpBridge should spawn: `…/Forge Conductor serve` (stdio MCP)
   16: ///
   17: /// Until this router runs, the SwiftUI `@main` ignored argv and never spoke MCP.
```

### `Sources/ForgeConductorCore/Application/ForgeProcessEntry.swift:55` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   48:         if let idx = args.firstIndex(of: "--home"), args.index(after: idx) < args.endIndex {
   49:             let raw = args[args.index(after: idx)] as NSString
   50:             return URL(fileURLWithPath: raw.expandingTildeInPath, isDirectory: true)
   51:         }
   52:         return nil
   53:     }
   54: 
   55:     /// Run stdio MCP until stdin closes. Does not return on success (process exits 0).
   56:     public static func runServe(home: URL? = nil) -> Never {
   57:         do {
   58:             let app = try ForgeApp.bootstrap(home: home ?? homeOverride())
   59:             defer { app.shutdown() }
   60:             // MCP owns stdout. Normal lifecycle diagnostics are persisted by
   61:             // DiagnosticLog; keep stderr quiet unless startup actually fails.
   62:             try MCPServer(app: app).run()
```

### `Sources/ForgeConductorCore/Application/Tools/SearchToolPack.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // SearchToolPack.swift
    2: // What: Provides recursive text search inside authorized workspaces.
    3: // How: It validates the query/root, invokes the native process adapter with limits,
    4: // and converts matches into a stable tool response.
    5: // Why: Search behavior stays modular and cannot silently broaden filesystem access.
    6: 
    7: import Foundation
    8: 
    9: /// Search tools: search_text (recursive grep).
   10: public struct SearchToolPack: ToolPackHandling {
```

### `Sources/ForgeConductorCore/Application/Tools/ShellToolPack.swift:4` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ShellToolPack.swift
    2: // What: Implements the explicitly granted shell-execution capability.
    3: // How: It requires an active authorized workspace, applies timeout/output limits,
    4: // and delegates process mechanics to ProcessRunner before returning structured status.
    5: // Why: The most powerful tool needs a narrow, independently reviewable boundary.
    6: 
    7: import Foundation
    8: 
    9: /// Shell tool pack: shell_exec.
   10: public struct ShellToolPack: ToolPackHandling {
   11:     private let runner = ProcessRunner()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTML.swift:128` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  121:       <button class="ok" onclick="mgrStart()">Start service</button>
  122:       <button class="danger" onclick="mgrStop()">Stop service</button>
  123:       <button onclick="mgrRestart()">Restart service</button>
  124:       <button class="danger" onclick="mgrShutdown()">Shutdown manager</button>
  125:     </div>
  126:     <p class="muted" style="margin:0.75rem 0 0;font-size:0.8rem">
  127:       Stop pauses operational APIs but keeps this control surface up.
  128:       Shutdown ends the manager process (start again with <span class="mono">forge-conductor manager run</span>).
  129:     </p>
  130:   </div>
  131: 
  132:   <div class="card half">
  133:     <h2>Settings</h2>
  134:     <div class="form-grid">
  135:       <div>
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTML.swift:351` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  344:   } catch (e) {
  345:     // restart may drop connection mid-flight
  346:     await new Promise(r => setTimeout(r, 600));
  347:     try { await refreshAll(); } catch (e2) { showErr(String(e2)); }
  348:   }
  349: }
  350: async function mgrShutdown() {
  351:   if (!confirm('Shutdown the manager process? The dashboard will go offline until you run: forge-conductor manager run')) return;
  352:   try {
  353:     await jpost('/api/manager/shutdown', {});
  354:     $('header-meta').textContent = 'manager shutting down…';
  355:     showSvcWarn('Manager process is shutting down. Start it again from the CLI.');
  356:   } catch (e) { showErr(String(e)); }
  357: }
  358: async function refreshAll() {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTML.swift:355` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  348:   }
  349: }
  350: async function mgrShutdown() {
  351:   if (!confirm('Shutdown the manager process? The dashboard will go offline until you run: forge-conductor manager run')) return;
  352:   try {
  353:     await jpost('/api/manager/shutdown', {});
  354:     $('header-meta').textContent = 'manager shutting down…';
  355:     showSvcWarn('Manager process is shutting down. Start it again from the CLI.');
  356:   } catch (e) { showErr(String(e)); }
  357: }
  358: async function refreshAll() {
  359:   try {
  360:     showErr('');
  361:     let status;
  362:     try {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:71` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   64:         }
   65:         lock.unlock()
   66: 
   67:         guard DashboardRequestPolicy.isConfiguredLoopbackHost(host) else {
   68:             throw DashboardError.nonLoopbackHost(host)
   69:         }
   70: 
   71:         // Fail closed if another process already owns the port (dual Forge / foreign app).
   72:         let state = DashboardPortGuard.inspect(host: host, port: Int(port))
   73:         switch state {
   74:         case .free, .heldBySelf:
   75:             break
   76:         case .heldByOtherForge(let h):
   77:             let msg = "Dashboard port \(port) held by another Forge process pid=\(h.pid.map(String.init) ?? "?") cmd=\(h.command ?? "?"). Stop the other instance or run only one product (LaunchAgent manager OR GUI), not both claiming HTTP."
   78:             app.diagnostics.error("dashboard_port_conflict_forge", [
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:77` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   70: 
   71:         // Fail closed if another process already owns the port (dual Forge / foreign app).
   72:         let state = DashboardPortGuard.inspect(host: host, port: Int(port))
   73:         switch state {
   74:         case .free, .heldBySelf:
   75:             break
   76:         case .heldByOtherForge(let h):
   77:             let msg = "Dashboard port \(port) held by another Forge process pid=\(h.pid.map(String.init) ?? "?") cmd=\(h.command ?? "?"). Stop the other instance or run only one product (LaunchAgent manager OR GUI), not both claiming HTTP."
   78:             app.diagnostics.error("dashboard_port_conflict_forge", [
   79:                 "port": "\(port)",
   80:                 "holder_pid": h.pid.map(String.init) ?? "",
   81:                 "holder": h.command ?? "",
   82:             ], category: .manager)
   83:             throw DashboardError.portInUse(msg)
   84:         case .heldByForeign(let h):
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:85` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   78:             app.diagnostics.error("dashboard_port_conflict_forge", [
   79:                 "port": "\(port)",
   80:                 "holder_pid": h.pid.map(String.init) ?? "",
   81:                 "holder": h.command ?? "",
   82:             ], category: .manager)
   83:             throw DashboardError.portInUse(msg)
   84:         case .heldByForeign(let h):
   85:             let msg = "Dashboard port \(port) held by non-Forge process pid=\(h.pid.map(String.init) ?? "?") cmd=\(h.command ?? "?")."
   86:             app.diagnostics.error("dashboard_port_conflict_foreign", [
   87:                 "port": "\(port)",
   88:                 "holder_pid": h.pid.map(String.init) ?? "",
   89:                 "holder": h.command ?? "",
   90:             ], category: .manager)
   91:             throw DashboardError.portInUse(msg)
   92:         case .unknown(let d):
```

### `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ForgeSnapshot.swift
    2: // What: Represents a composed point-in-time view of Forge orchestration health.
    3: // How: It aggregates tool, session, connector, process, usage, file, and audit values
    4: // into immutable domain summaries with explicit dictionary projections.
    5: // Why: Consumers receive one stable contract instead of coordinating many live services.
    6: 
    7: import Foundation
    8: 
    9: /// Fully typed forge-side telemetry frame (no [String: Any] in domain).
   10: /// Name retained for API stability; this is a live-composed frame, not a multi-second poll product.
```

### `Sources/ForgeConductorCore/Domain/LMStudioConnector.swift:4` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // LMStudioConnector.swift
    2: // What: Defines connector roles, per-role health, aggregate state, and host activation.
    3: // How: Typed value objects derive fail-forward status from independent primary/fallback
    4: // observations without depending on filesystem or process implementations.
    5: // Why: Connector policy remains portable and testable apart from LM Studio integration code.
    6: 
    7: import Foundation
    8: 
    9: /// The two independently launched stdio connections installed into LM Studio.
   10: ///
   11: /// Roles are values rather than free-form strings so deployment, health checks,
```

### `Sources/ForgeConductorCore/Domain/Models.swift:53` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   46:     public let rawValue: String
   47:     public init(_ rawValue: String = UUID().uuidString) { self.rawValue = rawValue }
   48: }
   49: 
   50: // MARK: - Durable memory notes (SQLite-backed MCP memory tools)
   51: 
   52: /// One durable key/value memory note stored in `memory_notes`.
   53: /// Survives MCP process restarts and LM Studio chat sessions when `FORGE_CONDUCTOR_HOME` is stable.
   54: public struct MemoryNote: Sendable, Equatable, Codable {
   55:     public var key: String
   56:     public var body: String
   57:     public var tags: [String]
   58:     public var createdAt: String
   59:     public var updatedAt: String
   60: 
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:132` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  125:     var realtimeEngine: any RealtimeMetricsStreaming { get }
  126:     /// Receive every live frame update (host sample rate when stream is running).
  127:     @discardableResult
  128:     func addListener(_ block: @escaping (TelemetrySnapshot) -> Void) -> UUID
  129:     func removeListener(_ id: UUID)
  130: }
  131: 
  132: /// Continuous system-metrics stream driven by native host sampling (CPU/RAM/GPU/disk/process).
  133: public protocol RealtimeMetricsStreaming: AnyObject, Sendable {
  134:     /// Most recent host sample (lock-free enough for UI read on main).
  135:     var latestSystem: SystemMetrics { get }
  136:     /// Configured sample rate (Hz).
  137:     var targetSampleHz: Double { get }
  138:     /// Actual samples completed in the last second.
  139:     var measuredSampleHz: Double { get }
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:163` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  156: /// installer behind an interface makes deploy orchestration deterministic and
  157: /// testable without touching an operator's live `~/.lmstudio` directory.
  158: public protocol LMStudioPluginInstalling: Sendable {
  159:     func status(preferredBinary: URL?) -> LMStudioMCPPluginInstaller.PluginStatus
  160:     func install(preferredBinary: URL?) throws -> LMStudioMCPPluginInstaller.InstallResult
  161: }
  162: 
  163: /// Process-boundary health check for one independently spawned connector role.
  164: public protocol MCPServeVerifying: Sendable {
  165:     func verify(
  166:         binary: URL,
  167:         home: URL,
  168:         role: LMStudioConnectorRole,
  169:         timeoutSec: TimeInterval
  170:     ) throws -> MCPServeVerifier.Result
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:5` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // DashboardPortGuard.swift
    2: // What: Determines whether the configured loopback dashboard port is safely available.
    3: // How: Native socket probes distinguish free, Forge-owned, and foreign listeners before
    4: // ManagerNode attempts to bind or the GUI decides to attach.
    5: // Why: Explicit ownership prevents duplicate listeners and accidental process conflicts.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Detects who holds the dashboard TCP port so dual Forge instances cannot silently fight.
   11: public enum DashboardPortGuard {
   12:     public struct Holder: Sendable, Equatable {
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:103` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   96:             }
   97:             ptr = ai.pointee.ai_next
   98:         }
   99:         return open
  100:     }
  101: 
  102:     private static func lsofHolders(port: Int) -> [Holder]? {
  103:         let proc = Process()
  104:         proc.executableURL = URL(fileURLWithPath: "/usr/sbin/lsof")
  105:         // -nP -iTCP:PORT -sTCP:LISTEN
  106:         proc.arguments = ["-nP", "-iTCP:\(port)", "-sTCP:LISTEN"]
  107:         let out = Pipe()
  108:         proc.standardOutput = out
  109:         proc.standardError = Pipe()
  110:         do {
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:107` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  100:     }
  101: 
  102:     private static func lsofHolders(port: Int) -> [Holder]? {
  103:         let proc = Process()
  104:         proc.executableURL = URL(fileURLWithPath: "/usr/sbin/lsof")
  105:         // -nP -iTCP:PORT -sTCP:LISTEN
  106:         proc.arguments = ["-nP", "-iTCP:\(port)", "-sTCP:LISTEN"]
  107:         let out = Pipe()
  108:         proc.standardOutput = out
  109:         proc.standardError = Pipe()
  110:         do {
  111:             try proc.run()
  112:             proc.waitUntilExit()
  113:         } catch {
  114:             return nil
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:109` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  102:     private static func lsofHolders(port: Int) -> [Holder]? {
  103:         let proc = Process()
  104:         proc.executableURL = URL(fileURLWithPath: "/usr/sbin/lsof")
  105:         // -nP -iTCP:PORT -sTCP:LISTEN
  106:         proc.arguments = ["-nP", "-iTCP:\(port)", "-sTCP:LISTEN"]
  107:         let out = Pipe()
  108:         proc.standardOutput = out
  109:         proc.standardError = Pipe()
  110:         do {
  111:             try proc.run()
  112:             proc.waitUntilExit()
  113:         } catch {
  114:             return nil
  115:         }
  116:         let data = out.fileHandleForReading.readDataToEndOfFile()
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:112` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  105:         // -nP -iTCP:PORT -sTCP:LISTEN
  106:         proc.arguments = ["-nP", "-iTCP:\(port)", "-sTCP:LISTEN"]
  107:         let out = Pipe()
  108:         proc.standardOutput = out
  109:         proc.standardError = Pipe()
  110:         do {
  111:             try proc.run()
  112:             proc.waitUntilExit()
  113:         } catch {
  114:             return nil
  115:         }
  116:         let data = out.fileHandleForReading.readDataToEndOfFile()
  117:         guard let text = String(data: data, encoding: .utf8), !text.isEmpty else {
  118:             return []
  119:         }
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:5` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // DiagnosticLog.swift
    2: // What: Implements the append-only structured diagnostic event store and exporter.
    3: // How: Thread-safe JSONL writes feed bounded recent reads, severity/category envelopes,
    4: // and paired machine-readable JSON plus operator-readable Markdown exports.
    5: // Why: Failures need durable, correlatable evidence across short-lived process roles.
    6: 
    7: import Foundation
    8: 
    9: /// Persistent, structured diagnostic logging for Forge Conductor.
   10: ///
   11: /// - Append-only JSONL on disk under `~/.forge-conductor/logs/`
   12: /// - In-memory ring for recent UI inspection
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ProcessRunner.swift
    2: // What: Provides the bounded native subprocess adapter used by connector modules.
    3: // How: Foundation.Process is wrapped with explicit environment, working directory,
    4: // timeout, termination, and capped stdout/stderr collection behavior.
    5: // Why: Every module must share the same resource and failure semantics for child processes.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Describes a completed subprocess, including timeout and output-cap evidence.
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:13` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Describes a completed subprocess, including timeout and output-cap evidence.
   11: ///
   12: /// Truncation flags let callers distinguish complete diagnostics from output that was
   13: /// intentionally bounded to protect the long-running host process.
   14: public struct ProcessResult: Sendable {
   15:     public var exitCode: Int32
   16:     public var stdout: String
   17:     public var stderr: String
   18:     public var timedOut: Bool
   19:     public var stdoutTruncated: Bool
   20:     public var stderrTruncated: Bool
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:25` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   18:     public var timedOut: Bool
   19:     public var stdoutTruncated: Bool
   20:     public var stderrTruncated: Bool
   21: }
   22: 
   23: /// Failures that are specific to subprocess lifecycle management.
   24: public enum ProcessRunnerError: Error, Equatable, Sendable, LocalizedError {
   25:     /// TERM and KILL were requested, but process termination was not observed before
   26:     /// the final bounded wait expired. No termination status is available in this state.
   27:     case terminationUnconfirmed(processIdentifier: Int32, signalError: Int32?)
   28: 
   29:     public var errorDescription: String? {
   30:         switch self {
   31:         case let .terminationUnconfirmed(pid, signalError):
   32:             if let signalError {
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:33` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   26:     /// the final bounded wait expired. No termination status is available in this state.
   27:     case terminationUnconfirmed(processIdentifier: Int32, signalError: Int32?)
   28: 
   29:     public var errorDescription: String? {
   30:         switch self {
   31:         case let .terminationUnconfirmed(pid, signalError):
   32:             if let signalError {
   33:                 return "process \(pid) did not confirm termination; SIGKILL failed with errno \(signalError)"
   34:             }
   35:             return "process \(pid) did not confirm termination after SIGKILL"
   36:         }
   37:     }
   38: }
   39: 
   40: /// Runs allowlisted processes with timeout (no shell injection — argv array).
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:35` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   28: 
   29:     public var errorDescription: String? {
   30:         switch self {
   31:         case let .terminationUnconfirmed(pid, signalError):
   32:             if let signalError {
   33:                 return "process \(pid) did not confirm termination; SIGKILL failed with errno \(signalError)"
   34:             }
   35:             return "process \(pid) did not confirm termination after SIGKILL"
   36:         }
   37:     }
   38: }
   39: 
   40: /// Runs allowlisted processes with timeout (no shell injection — argv array).
   41: ///
   42: /// Reads stdout/stderr concurrently so large or chatty children cannot deadlock
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:43` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   36:         }
   37:     }
   38: }
   39: 
   40: /// Runs allowlisted processes with timeout (no shell injection — argv array).
   41: ///
   42: /// Reads stdout/stderr concurrently so large or chatty children cannot deadlock
   43: /// on full pipe buffers (common with Node diagnostics on stderr).
   44: public final class ProcessRunner: @unchecked Sendable {
   45:     private let terminationGraceSec: TimeInterval
   46:     private let forcedTerminationGraceSec: TimeInterval
   47: 
   48:     public init() {
   49:         terminationGraceSec = 0.5
   50:         forcedTerminationGraceSec = 1.0
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:67` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   60:         executable: String,
   61:         arguments: [String] = [],
   62:         currentDirectory: String? = nil,
   63:         environment: [String: String]? = nil,
   64:         timeoutSec: TimeInterval = 30,
   65:         maximumOutputBytes: Int = 1_048_576
   66:     ) throws -> ProcessResult {
   67:         let process = Process()
   68:         let exeURL: URL
   69:         if executable.hasPrefix("/") {
   70:             exeURL = URL(fileURLWithPath: executable)
   71:         } else if let path = ProcessRunner.which(executable) {
   72:             exeURL = URL(fileURLWithPath: path)
   73:         } else {
   74:             throw NSError(
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:80` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   73:         } else {
   74:             throw NSError(
   75:                 domain: "ProcessRunner",
   76:                 code: 1,
   77:                 userInfo: [NSLocalizedDescriptionKey: "executable not found: \(executable)"]
   78:             )
   79:         }
   80:         process.executableURL = exeURL
   81:         process.arguments = arguments
   82:         if let currentDirectory {
   83:             process.currentDirectoryURL = URL(fileURLWithPath: currentDirectory)
   84:         }
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:81` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   74:             throw NSError(
   75:                 domain: "ProcessRunner",
   76:                 code: 1,
   77:                 userInfo: [NSLocalizedDescriptionKey: "executable not found: \(executable)"]
   78:             )
   79:         }
   80:         process.executableURL = exeURL
   81:         process.arguments = arguments
   82:         if let currentDirectory {
   83:             process.currentDirectoryURL = URL(fileURLWithPath: currentDirectory)
   84:         }
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:83` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   76:                 code: 1,
   77:                 userInfo: [NSLocalizedDescriptionKey: "executable not found: \(executable)"]
   78:             )
   79:         }
   80:         process.executableURL = exeURL
   81:         process.arguments = arguments
   82:         if let currentDirectory {
   83:             process.currentDirectoryURL = URL(fileURLWithPath: currentDirectory)
   84:         }
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:88` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   81:         process.arguments = arguments
   82:         if let currentDirectory {
   83:             process.currentDirectoryURL = URL(fileURLWithPath: currentDirectory)
   84:         }
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:91` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   84:         }
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
   96: 
   97:         final class BufferBox: @unchecked Sendable {
   98:             let condition = NSCondition()
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:92` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   85:         if let environment {
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
   96: 
   97:         final class BufferBox: @unchecked Sendable {
   98:             let condition = NSCondition()
   99:             var data = Data()
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:93` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   86:             var env = ProcessInfo.processInfo.environment
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
   96: 
   97:         final class BufferBox: @unchecked Sendable {
   98:             let condition = NSCondition()
   99:             var data = Data()
  100:             var truncated = false
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:94` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   87:             for (k, v) in environment { env[k] = v }
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
   96: 
   97:         final class BufferBox: @unchecked Sendable {
   98:             let condition = NSCondition()
   99:             var data = Data()
  100:             var truncated = false
  101:             let limit: Int
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:95` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   88:             process.environment = env
   89:         }
   90: 
   91:         let outPipe = Pipe()
   92:         let errPipe = Pipe()
   93:         process.standardOutput = outPipe
   94:         process.standardError = errPipe
   95:         process.standardInput = FileHandle.nullDevice
   96: 
   97:         final class BufferBox: @unchecked Sendable {
   98:             let condition = NSCondition()
   99:             var data = Data()
  100:             var truncated = false
  101:             let limit: Int
  102:             var acceptsCallbacks = true
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:146` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  139:             func stopCallbacks(on handle: FileHandle) {
  140:                 condition.lock()
  141:                 let shouldClearHandler = acceptsCallbacks
  142:                 acceptsCallbacks = false
  143:                 condition.unlock()
  144: 
  145:                 if shouldClearHandler {
  146:                     handle.readabilityHandler = nil
  147:                 }
  148: 
  149:                 condition.lock()
  150:                 while activeCallbacks > 0 {
  151:                     condition.wait()
  152:                 }
  153:                 condition.unlock()
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:163` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  156:             func take() -> (data: Data, truncated: Bool) {
  157:                 condition.lock()
  158:                 defer { condition.unlock() }
  159:                 return (data, truncated)
  160:             }
  161: 
  162:             /// Drains bytes that are already readable without waiting for every inherited
  163:             /// writer to close. A descendant can retain a pipe after the direct child exits,
  164:             /// so an EOF-based read would make the caller's timeout unbounded.
  165:             func drainCurrentlyAvailableData(from handle: FileHandle) {
  166:                 let descriptor = handle.fileDescriptor
  167:                 let originalFlags = fcntl(descriptor, F_GETFL)
  168:                 guard originalFlags >= 0 else {
  169:                     markTruncated()
  170:                     return
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:259` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  252:             }
  253:         }
  254:         let outBox = BufferBox(limit: maximumOutputBytes)
  255:         let errBox = BufferBox(limit: maximumOutputBytes)
  256:         let outHandle = outPipe.fileHandleForReading
  257:         let errHandle = errPipe.fileHandleForReading
  258: 
  259:         outHandle.readabilityHandler = { handle in
  260:             outBox.consumeAvailableData(from: handle)
  261:         }
  262:         errHandle.readabilityHandler = { handle in
  263:             errBox.consumeAvailableData(from: handle)
  264:         }
  265:         defer {
  266:             outBox.stopCallbacks(on: outHandle)
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:262` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  255:         let errBox = BufferBox(limit: maximumOutputBytes)
  256:         let outHandle = outPipe.fileHandleForReading
  257:         let errHandle = errPipe.fileHandleForReading
  258: 
  259:         outHandle.readabilityHandler = { handle in
  260:             outBox.consumeAvailableData(from: handle)
  261:         }
  262:         errHandle.readabilityHandler = { handle in
  263:             errBox.consumeAvailableData(from: handle)
  264:         }
  265:         defer {
  266:             outBox.stopCallbacks(on: outHandle)
  267:             errBox.stopCallbacks(on: errHandle)
  268:         }
  269: 
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:274` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  267:             errBox.stopCallbacks(on: errHandle)
  268:         }
  269: 
  270:         let terminationGroup = DispatchGroup()
  271:         let terminationBox = TerminationBox()
  272:         var timedOut = false
  273:         terminationGroup.enter()
  274:         process.terminationHandler = { terminatedProcess in
  275:             if terminationBox.complete(status: terminatedProcess.terminationStatus) {
  276:                 terminationGroup.leave()
  277:             }
  278:         }
  279:         do {
  280:             try process.run()
  281:         } catch {
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:280` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  273:         terminationGroup.enter()
  274:         process.terminationHandler = { terminatedProcess in
  275:             if terminationBox.complete(status: terminatedProcess.terminationStatus) {
  276:                 terminationGroup.leave()
  277:             }
  278:         }
  279:         do {
  280:             try process.run()
  281:         } catch {
  282:             process.terminationHandler = nil
  283:             if terminationBox.complete(status: nil) {
  284:                 terminationGroup.leave()
  285:             }
  286:             throw error
  287:         }
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:282` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  275:             if terminationBox.complete(status: terminatedProcess.terminationStatus) {
  276:                 terminationGroup.leave()
  277:             }
  278:         }
  279:         do {
  280:             try process.run()
  281:         } catch {
  282:             process.terminationHandler = nil
  283:             if terminationBox.complete(status: nil) {
  284:                 terminationGroup.leave()
  285:             }
  286:             throw error
  287:         }
  288:         let processIdentifier = process.processIdentifier
  289: 
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:288` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  281:         } catch {
  282:             process.terminationHandler = nil
  283:             if terminationBox.complete(status: nil) {
  284:                 terminationGroup.leave()
  285:             }
  286:             throw error
  287:         }
  288:         let processIdentifier = process.processIdentifier
  289: 
  290:         func confirmedStatus(waiting seconds: TimeInterval) -> Int32? {
  291:             if seconds == .infinity {
  292:                 terminationGroup.wait()
  293:                 return terminationBox.load()
  294:             }
  295:             let boundedSeconds = seconds.isFinite ? max(0, seconds) : 0
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:307` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  300:         }
  301: 
  302:         let exitCode: Int32
  303:         if let status = confirmedStatus(waiting: timeoutSec) {
  304:             exitCode = status
  305:         } else {
  306:             timedOut = true
  307:             process.terminate()
  308:             if let status = confirmedStatus(waiting: terminationGraceSec) {
  309:                 exitCode = status
  310:             } else {
  311:                 // Some system tools ignore SIGTERM (e.g. systemextensionsctl).
  312:                 let killResult = kill(processIdentifier, SIGKILL)
  313:                 let signalError: Int32? = killResult == 0 || errno == ESRCH ? nil : errno
  314:                 guard let status = confirmedStatus(waiting: forcedTerminationGraceSec) else {
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:81` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   74: 
   75:     private static func withInitializationFileLock<T>(
   76:         databasePath: URL,
   77:         _ body: () throws -> T
   78:     ) throws -> T {
   79:         let processDeadline = Date().addingTimeInterval(initializationLockTimeout)
   80:         guard processInitializationLock.lock(before: processDeadline) else {
   81:             throw StoreError.openFailed("timed out waiting for process initialization lock")
   82:         }
   83:         defer { processInitializationLock.unlock() }
   84: 
   85:         let lockURL = databasePath.appendingPathExtension("initialization.lock")
   86:         let mode = mode_t(S_IRUSR | S_IWUSR)
   87:         let descriptor = lockURL.path.withCString {
   88:             Darwin.open($0, O_CREAT | O_RDWR, mode)
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:845` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  838:     public func presenceDelete(clientID: String) throws {
  839:         try withStatement("DELETE FROM presence WHERE client_id = ?") { stmt in
  840:             bind(stmt, 1, clientID)
  841:             try stepDone(stmt)
  842:         }
  843:     }
  844: 
  845:     /// Remove presence rows whose process is gone or heartbeat is older than `maxAgeSec`.
  846:     @discardableResult
  847:     public func presencePrune(maxAgeSec: TimeInterval = 120) throws -> Int {
  848:         let rows = try presenceRecords()
  849:         var removed = 0
  850:         let now = clock.now()
  851:         for row in rows {
  852:             let clientID = row.clientID
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:2` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // MCPServeVerifier.swift
    2: // What: Performs a process-level MCP initialize and tools/list acceptance smoke.
    3: // How: It launches a candidate binary with a clean role environment, exchanges bounded
    4: // NDJSON frames, validates negotiated protocol and role identity, then terminates it.
    5: // Why: Deployment must fail closed before registering a binary that a host cannot use.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:79` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   72:                 toolCount: 0,
   73:                 toolNames: [],
   74:                 detail: "not executable: \(binary.path)",
   75:                 durationMs: 0
   76:             )
   77:         }
   78: 
   79:         let proc = Process()
   80:         proc.executableURL = binary
   81:         proc.arguments = ["serve"]
   82:         try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
   83:         proc.environment = [
   84:             "HOME": home.path,
   85:             "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
   86:             "FORGE_CONDUCTOR_HOME": home.path,
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:91` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   84:             "HOME": home.path,
   85:             "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
   86:             "FORGE_CONDUCTOR_HOME": home.path,
   87:             "FORGE_MCP_ROLE": connectorRole.rawValue,
   88:             "TMPDIR": NSTemporaryDirectory(),
   89:         ]
   90: 
   91:         let stdin = Pipe()
   92:         let stdout = Pipe()
   93:         let stderr = Pipe()
   94:         proc.standardInput = stdin
   95:         proc.standardOutput = stdout
   96:         proc.standardError = stderr
   97: 
   98:         try proc.run()
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:92` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   85:             "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
   86:             "FORGE_CONDUCTOR_HOME": home.path,
   87:             "FORGE_MCP_ROLE": connectorRole.rawValue,
   88:             "TMPDIR": NSTemporaryDirectory(),
   89:         ]
   90: 
   91:         let stdin = Pipe()
   92:         let stdout = Pipe()
   93:         let stderr = Pipe()
   94:         proc.standardInput = stdin
   95:         proc.standardOutput = stdout
   96:         proc.standardError = stderr
   97: 
   98:         try proc.run()
   99:         setNonblocking(stdout.fileHandleForReading)
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:93` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   86:             "FORGE_CONDUCTOR_HOME": home.path,
   87:             "FORGE_MCP_ROLE": connectorRole.rawValue,
   88:             "TMPDIR": NSTemporaryDirectory(),
   89:         ]
   90: 
   91:         let stdin = Pipe()
   92:         let stdout = Pipe()
   93:         let stderr = Pipe()
   94:         proc.standardInput = stdin
   95:         proc.standardOutput = stdout
   96:         proc.standardError = stderr
   97: 
   98:         try proc.run()
   99:         setNonblocking(stdout.fileHandleForReading)
  100:         setNonblocking(stderr.fileHandleForReading)
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:270` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  263:             }
  264:             if count < 0, errno == EINTR { continue }
  265:             return
  266:         }
  267:     }
  268: }
  269: 
  270: /// Foundation `Process` adapter behind the deployment verifier port.
  271: public struct NativeMCPServeVerifier: MCPServeVerifying {
  272:     public init() {}
  273: 
  274:     public func verify(
  275:         binary: URL,
  276:         home: URL,
  277:         role: LMStudioConnectorRole,
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:4` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ManagerInstaller.swift
    2: // What: Installs and controls the per-user LaunchAgent manager integration.
    3: // How: It stages the executable/framework/app layout, writes validated launchd metadata,
    4: // loads or unloads the agent, and verifies runtime state through native process APIs.
    5: // Why: Persistent service ownership needs one transactional, repeatable installation module.
    6: 
    7: import Foundation
    8: import Darwin
    9: import Security
   10: 
   11: enum ManagerArtifactKind: Equatable {
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:940` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  933:         return out
  934:     }
  935: 
  936:     // MARK: - Login LaunchAgent
  937: 
  938:     @discardableResult
  939:     public func installLoginAgent(openBrowser: Bool = false) throws -> URL {
  940:         // Always stage from this process before launchd starts the manager. Reusing an existing
  941:         // executable here can silently keep an older manager and framework running after upgrade.
  942:         _ = try installBinary()
  943: 
  944:         try FileManager.default.createDirectory(at: launchAgentsDir, withIntermediateDirectories: true)
  945: 
  946:         let exe = appExecutableURL.path
  947: 
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1013` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1006:         _ = try? ProcessRunner().run(
 1007:             executable: "/bin/launchctl",
 1008:             arguments: ["unload", launchAgentURL.path],
 1009:             timeoutSec: 10
 1010:         )
 1011: 
 1012:         // launchd owns these descriptors while the agent is loaded. Rotate only after bootout so
 1013:         // the replacement process starts with fresh logs and retained failures stay size-bounded.
 1014:         try rotateLaunchAgentLogs()
 1015:         try plist.write(to: launchAgentURL, atomically: true, encoding: .utf8)
 1016: 
 1017:         // Ensure domain enablement
 1018:         _ = try? ProcessRunner().run(
 1019:             executable: "/bin/launchctl",
 1020:             arguments: ["enable", "gui/\(uid)/\(Self.launchAgentLabel)"],
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1356` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1349:                 "Endpoint Security Extensions (Falcon/Jamf Protect/Cortex) are IT-managed — leave enabled",
 1350:             ],
 1351:             "macos_firewall": [
 1352:                 "Allow incoming for: \(binaryPath)",
 1353:                 "And: \(appExecutableURL.path)",
 1354:             ],
 1355:             "crowdstrike_falcon": [
 1356:                 "Allow process: \(binaryPath)",
 1357:                 "Allow app bundle: \(appBundleURL.path)",
 1358:                 "Allow listen 127.0.0.1:\(port)",
 1359:             ],
 1360:             "jamf_protect": [
 1361:                 "Exception for \(binaryPath) and \(Self.bundleIdentifier)",
 1362:             ],
 1363:             "commands": [
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:21` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   14:     case running
   15:     case restarting
   16:     case stopping
   17:     case failed
   18: }
   19: 
   20: /// Supervisor that keeps the dashboard control surface available.
   21: /// Mutable process state lives in `ManagerRuntime` (SRP).
   22: public final class ManagerNode: ManagerControlling, @unchecked Sendable {
   23:     private static let presencePruneInterval: TimeInterval = 60
   24:     private static let presenceMaxAge: TimeInterval = 120
   25: 
   26:     public let app: ForgeApp
   27:     private let lock = NSLock()
   28:     private let runtime = ManagerRuntime()
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:264` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  257:         }
  258: 
  259:         startWatchdog()
  260:         installSignalHandlers()
  261: 
  262:         fputs("Forge-Conductor manager running — \(dashboardURLString())\n", stderr)
  263:         fputs("  controls: Start / Stop / Restart / Settings on the dashboard\n", stderr)
  264:         fputs("  stop process: forge-conductor manager stop   or dashboard Shutdown\n", stderr)
  265: 
  266:         runtime.runLock.wait()
  267:     }
  268: 
  269:     private func halt() {
  270:         stopWatchdog()
  271:         tearDownDashboard()
```

### `Sources/ForgeConductorCore/Manager/ManagerPIDFile.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ManagerPIDFile.swift
    2: // What: Reads, validates, and removes the persistent manager PID marker.
    3: // How: Small static helpers parse a positive process identifier and perform atomic writes.
    4: // Why: PID-file semantics stay consistent across installer, manager, CLI, and tests.
    5: 
    6: import Foundation
    7: 
    8: /// Owns the manager PID-file format and its liveness/termination operations.
    9: ///
   10: /// Centralizing this behavior prevents the CLI, installer, and runtime from applying
```

### `Sources/ForgeConductorCore/Manager/ManagerPIDFile.swift:11` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    4: // Why: PID-file semantics stay consistent across installer, manager, CLI, and tests.
    5: 
    6: import Foundation
    7: 
    8: /// Owns the manager PID-file format and its liveness/termination operations.
    9: ///
   10: /// Centralizing this behavior prevents the CLI, installer, and runtime from applying
   11: /// different parsing rules or accidentally signalling an invalid process identifier.
   12: public enum ManagerPIDFile {
   13:     public static func write(paths: AppPaths) throws {
   14:         try paths.ensureLayout()
   15:         let pid = ProcessInfo.processInfo.processIdentifier
   16:         try "\(pid)\n".write(to: paths.managerPid, atomically: true, encoding: .utf8)
   17:     }
   18: 
```

### `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:9` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    2: // What: Stores the mutable objects owned by one ManagerNode instance.
    3: // How: A lock protects service state, dashboard reference, last errors, and timestamps
    4: // while exposing narrow mutation/snapshot operations.
    5: // Why: Separating synchronization state keeps ManagerNode orchestration readable.
    6: 
    7: import Foundation
    8: 
    9: /// Mutable runtime state for the manager process (thread-safe via owner lock).
   10: /// Extracted so `ManagerNode` is orchestration-only, not a god object of fields + timers.
   11: public final class ManagerRuntime: @unchecked Sendable {
   12:     public var dashboard: DashboardServer?
   13:     public var state: ManagerServiceState = .stopped
   14:     public var desiredRunning = true
   15:     public var lastError: String?
   16:     public var startedAt: Date?
```

### `Sources/ForgeConductorCore/Manager/ManagerSettingsNormalizer.swift:8` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ManagerSettingsNormalizer.swift
    2: // What: Validates and canonicalizes incoming manager configuration patches.
    3: // How: It clamps numeric ranges, normalizes host/boolean values, and emits a typed patch.
    4: // Why: Every settings entry path must enforce the same safe operating limits.
    5: 
    6: import Foundation
    7: 
    8: /// Pure settings-patch normalization (no process state).
    9: public enum ManagerSettingsNormalizer {
   10:     public static func normalize(_ patch: [String: Any]) -> [String: Any] {
   11:         var normalized: [String: Any] = [:]
   12:         if let dash = patch["dashboard"] as? [String: Any] {
   13:             var d: [String: Any] = [:]
   14:             if let host = dash["host"] as? String {
   15:                 let trimmed = host.trimmingCharacters(in: .whitespacesAndNewlines)
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:10` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    3: // How: libproc enumerates identities and Mach task/thread counters provide CPU, memory,
    4: // and thread measurements with filtering and hard result limits.
    5: // Why: Native bounded collection is faster and safer than repeatedly invoking ps.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Hot-process metrics via **libproc** (never `/bin/ps`).
   11: ///
   12: /// Primary path:
   13: /// - `proc_listpids` — enumerate PIDs
   14: /// - `proc_pidpath` — identity filter
   15: /// - `proc_pid_rusage(..., RUSAGE_INFO_V3)` — user/system time, RSS, phys footprint
   16: ///
   17: /// Fallback:
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:22` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   15: /// - `proc_pid_rusage(..., RUSAGE_INFO_V3)` — user/system time, RSS, phys footprint
   16: ///
   17: /// Fallback:
   18: /// - `proc_pidinfo(..., PROC_PIDTASKINFO)` — `proc_taskinfo` (Mach task times/RSS)
   19: ///
   20: /// Thread count:
   21: /// - `proc_pidinfo(..., PROC_PIDLISTTHREADS)` when available
   22: /// - self-process: Mach `task_threads` + `thread_info` (THREAD_BASIC_INFO)
   23: ///
   24: /// CPU% = Δ(`ri_user_time`+`ri_system_time`) / Δwall — first sample 0 until next realtime tick.
   25: public final class ProcessMetricsCollector: ProcessMetricsCollecting, @unchecked Sendable {
   26:     private let lock = NSLock()
   27:     private var previousCPU: [Int32: (user: UInt64, system: UInt64, at: Date)] = [:]
   28: 
   29:     private static let interestKeys = [
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:204` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  197:         let bytes = Int32(buf.count * MemoryLayout<UInt64>.stride)
  198:         let ret = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, &buf, bytes)
  199:         guard ret > 0 else { return nil }
  200:         return Int(ret) / MemoryLayout<UInt64>.stride
  201:     }
  202: }
  203: 
  204: // MARK: - Mach task/thread (self process)
  205: 
  206: /// Mach Task / Thread APIs for the **current** process (no `task_for_pid` privilege needed).
  207: enum MachTaskThreadSampler {
  208:     /// `task_threads(mach_task_self_)` then release ports.
  209:     static func currentProcessThreadCount() -> Int? {
  210:         var threadList: thread_act_array_t?
  211:         var threadCount: mach_msg_type_number_t = 0
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:206` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  199:         guard ret > 0 else { return nil }
  200:         return Int(ret) / MemoryLayout<UInt64>.stride
  201:     }
  202: }
  203: 
  204: // MARK: - Mach task/thread (self process)
  205: 
  206: /// Mach Task / Thread APIs for the **current** process (no `task_for_pid` privilege needed).
  207: enum MachTaskThreadSampler {
  208:     /// `task_threads(mach_task_self_)` then release ports.
  209:     static func currentProcessThreadCount() -> Int? {
  210:         var threadList: thread_act_array_t?
  211:         var threadCount: mach_msg_type_number_t = 0
  212:         let kr = task_threads(mach_task_self_, &threadList, &threadCount)
  213:         guard kr == KERN_SUCCESS, let list = threadList else { return nil }
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:225` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  218:                 mach_port_deallocate(mach_task_self_, list[i])
  219:             }
  220:             vm_deallocate(mach_task_self_, vm_address_t(bitPattern: list), size)
  221:         }
  222:         return Int(threadCount)
  223:     }
  224: 
  225:     /// `task_info(mach_task_self_, TASK_BASIC_INFO, …)` resident size of this process.
  226:     static func currentTaskRSSBytes() -> UInt64? {
  227:         var info = mach_task_basic_info()
  228:         var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info_data_t>.stride / MemoryLayout<natural_t>.stride)
  229:         let kr = withUnsafeMutablePointer(to: &info) {
  230:             $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
  231:                 task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
  232:             }
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:559` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  552:     private func windowEvents(_ events: [AuditEvent], windowSec: TimeInterval) -> [AuditEvent] {
  553:         let cutoff = Date().addingTimeInterval(-windowSec)
  554:         return events.filter { $0.timestamp >= cutoff }
  555:     }
  556: 
  557: }
  558: 
  559: /// Deterministically reconciles process, configuration, presence, and audit evidence
  560: /// into one MCP card set. Runtime process enumeration stays in `ProcessDiscovery`;
  561: /// this type owns only the pure merge and presentation-state policy.
  562: struct MCPServerCardAssembler {
  563:     private struct PresenceObservation {
  564:         var record: PresenceRecord
  565:         var processUp: Bool
  566:     }
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:560` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  553:         let cutoff = Date().addingTimeInterval(-windowSec)
  554:         return events.filter { $0.timestamp >= cutoff }
  555:     }
  556: 
  557: }
  558: 
  559: /// Deterministically reconciles process, configuration, presence, and audit evidence
  560: /// into one MCP card set. Runtime process enumeration stays in `ProcessDiscovery`;
  561: /// this type owns only the pure merge and presentation-state policy.
  562: struct MCPServerCardAssembler {
  563:     private struct PresenceObservation {
  564:         var record: PresenceRecord
  565:         var processUp: Bool
  566:     }
  567: 
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:604` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  597:         var consumedPresenceIDs = Set<String>()
  598:         var consumedPresencePIDs = Set<Int32>()
  599:         var representedConnectorRoles = Set<String>()
  600:         var emittedLivePIDs = Set<Int32>()
  601: 
  602:         // Live processes are authoritative for liveness. Matching presence contributes
  603:         // the role and stable client identity needed to correlate audit activity.
  604:         for process in live where !emittedLivePIDs.contains(process.pid) {
  605:             emittedLivePIDs.insert(process.pid)
  606:             if process.hostKind == "lm-studio-host" || process.hostKind == "model-backend" {
  607:                 continue
  608:             }
  609:             let processIsStdio = ProcessDiscovery.isForgeMCPStdioHostKind(
  610:                 process.hostKind
  611:             )
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:605` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  598:         var consumedPresencePIDs = Set<Int32>()
  599:         var representedConnectorRoles = Set<String>()
  600:         var emittedLivePIDs = Set<Int32>()
  601: 
  602:         // Live processes are authoritative for liveness. Matching presence contributes
  603:         // the role and stable client identity needed to correlate audit activity.
  604:         for process in live where !emittedLivePIDs.contains(process.pid) {
  605:             emittedLivePIDs.insert(process.pid)
  606:             if process.hostKind == "lm-studio-host" || process.hostKind == "model-backend" {
  607:                 continue
  608:             }
  609:             let processIsStdio = ProcessDiscovery.isForgeMCPStdioHostKind(
  610:                 process.hostKind
  611:             )
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:606` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  599:         var representedConnectorRoles = Set<String>()
  600:         var emittedLivePIDs = Set<Int32>()
  601: 
  602:         // Live processes are authoritative for liveness. Matching presence contributes
  603:         // the role and stable client identity needed to correlate audit activity.
  604:         for process in live where !emittedLivePIDs.contains(process.pid) {
  605:             emittedLivePIDs.insert(process.pid)
  606:             if process.hostKind == "lm-studio-host" || process.hostKind == "model-backend" {
  607:                 continue
  608:             }
  609:             let processIsStdio = ProcessDiscovery.isForgeMCPStdioHostKind(
  610:                 process.hostKind
  611:             )
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
  613:             if let observation {
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:610` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  603:         // the role and stable client identity needed to correlate audit activity.
  604:         for process in live where !emittedLivePIDs.contains(process.pid) {
  605:             emittedLivePIDs.insert(process.pid)
  606:             if process.hostKind == "lm-studio-host" || process.hostKind == "model-backend" {
  607:                 continue
  608:             }
  609:             let processIsStdio = ProcessDiscovery.isForgeMCPStdioHostKind(
  610:                 process.hostKind
  611:             )
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
  613:             if let observation {
  614:                 consumedPresenceIDs.insert(observation.record.clientID)
  615:                 consumedPresencePIDs.insert(observation.record.pid)
  616:             }
  617: 
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:612` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  605:             emittedLivePIDs.insert(process.pid)
  606:             if process.hostKind == "lm-studio-host" || process.hostKind == "model-backend" {
  607:                 continue
  608:             }
  609:             let processIsStdio = ProcessDiscovery.isForgeMCPStdioHostKind(
  610:                 process.hostKind
  611:             )
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
  613:             if let observation {
  614:                 consumedPresenceIDs.insert(observation.record.clientID)
  615:                 consumedPresencePIDs.insert(observation.record.pid)
  616:             }
  617: 
  618:             let hostKind = nonempty(observation?.record.hostKind) ?? process.hostKind
  619:             let roleKey = connectorRoleKey(label: process.label, hostKind: hostKind)
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:618` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  611:             )
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
  613:             if let observation {
  614:                 consumedPresenceIDs.insert(observation.record.clientID)
  615:                 consumedPresencePIDs.insert(observation.record.pid)
  616:             }
  617: 
  618:             let hostKind = nonempty(observation?.record.hostKind) ?? process.hostKind
  619:             let roleKey = connectorRoleKey(label: process.label, hostKind: hostKind)
  620:             if let roleKey {
  621:                 representedConnectorRoles.insert(roleKey)
  622:             }
  623:             let label = connectorLabel(roleKey: roleKey) ?? process.label
  624:             let auditClientID = observation.map {
  625:                 Self.normalizedAuditClientID(fromPresenceID: $0.record.clientID)
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:619` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  612:             let observation = processIsStdio ? presenceByPID[process.pid] : nil
  613:             if let observation {
  614:                 consumedPresenceIDs.insert(observation.record.clientID)
  615:                 consumedPresencePIDs.insert(observation.record.pid)
  616:             }
  617: 
  618:             let hostKind = nonempty(observation?.record.hostKind) ?? process.hostKind
  619:             let roleKey = connectorRoleKey(label: process.label, hostKind: hostKind)
  620:             if let roleKey {
  621:                 representedConnectorRoles.insert(roleKey)
  622:             }
  623:             let label = connectorLabel(roleKey: roleKey) ?? process.label
  624:             let auditClientID = observation.map {
  625:                 Self.normalizedAuditClientID(fromPresenceID: $0.record.clientID)
  626:             }
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:623` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  616:             }
  617: 
  618:             let hostKind = nonempty(observation?.record.hostKind) ?? process.hostKind
  619:             let roleKey = connectorRoleKey(label: process.label, hostKind: hostKind)
  620:             if let roleKey {
  621:                 representedConnectorRoles.insert(roleKey)
  622:             }
  623:             let label = connectorLabel(roleKey: roleKey) ?? process.label
  624:             let auditClientID = observation.map {
  625:                 Self.normalizedAuditClientID(fromPresenceID: $0.record.clientID)
  626:             }
  627:             let usage = auditClientID.map {
  628:                 usageForClient(audit, clientID: $0, windowSec: 300)
  629:             } ?? .empty
  630:             let health = mcpHealth(live: true, usage: usage)
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:633` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  626:             }
  627:             let usage = auditClientID.map {
  628:                 usageForClient(audit, clientID: $0, windowSec: 300)
  629:             } ?? .empty
  630:             let health = mcpHealth(live: true, usage: usage)
  631: 
  632:             cards.append(MCPServerCard(
  633:                 id: observation?.record.clientID ?? "proc-\(label)-\(process.pid)",
  634:                 label: label,
  635:                 role: roleFromLabel(label, hostKind: hostKind),
  636:                 hostKind: hostKind,
  637:                 pid: Int(process.pid),
  638:                 live: true,
  639:                 status: usage.eventCount > 0 ? "active" : "idle",
  640:                 health: health.health,
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:637` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  630:             let health = mcpHealth(live: true, usage: usage)
  631: 
  632:             cards.append(MCPServerCard(
  633:                 id: observation?.record.clientID ?? "proc-\(label)-\(process.pid)",
  634:                 label: label,
  635:                 role: roleFromLabel(label, hostKind: hostKind),
  636:                 hostKind: hostKind,
  637:                 pid: Int(process.pid),
  638:                 live: true,
  639:                 status: usage.eventCount > 0 ? "active" : "idle",
  640:                 health: health.health,
  641:                 healthLabel: health.label,
  642:                 healthReason: health.reason,
  643:                 activity: activity(for: usage),
  644:                 source: observation == nil ? "live-proc" : "live-proc+presence",
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:653` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  646:                 eventCount5m: usage.eventCount,
  647:                 errorRate: usage.errorRate,
  648:                 lastTool: usage.lastTool,
  649:                 topTools: usage.topTools.map(\.tool)
  650:             ))
  651:         }
  652: 
  653:         // Presence catches live app-bundle serves even if an OS process API omits argv.
  654:         // A process already merged above must not produce a second card.
  655:         for observation in observations {
  656:             let record = observation.record
  657:             if consumedPresenceIDs.contains(record.clientID)
  658:                 || (record.pid > 0 && consumedPresencePIDs.contains(record.pid)) {
  659:                 continue
  660:             }
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:654` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  647:                 errorRate: usage.errorRate,
  648:                 lastTool: usage.lastTool,
  649:                 topTools: usage.topTools.map(\.tool)
  650:             ))
  651:         }
  652: 
  653:         // Presence catches live app-bundle serves even if an OS process API omits argv.
  654:         // A process already merged above must not produce a second card.
  655:         for observation in observations {
  656:             let record = observation.record
  657:             if consumedPresenceIDs.contains(record.clientID)
  658:                 || (record.pid > 0 && consumedPresencePIDs.contains(record.pid)) {
  659:                 continue
  660:             }
  661: 
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:857` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  850:         Double(min(100, Int(usage.eventsPerMin / 12 * 100)))
  851:     }
  852: 
  853:     private func mcpHealth(
  854:         live: Bool,
  855:         usage: UsageWindow
  856:     ) -> (health: String, label: String, reason: String) {
  857:         if !live { return ("error", "DOWN", "process not running") }
  858:         if usage.errorRate >= 0.25 || usage.errorCount >= 3 {
  859:             return ("error", "ERROR", "elevated tool errors")
  860:         }
  861:         if usage.errorRate > 0.05 || usage.errorCount > 0 {
  862:             return ("warn", "WARN", "recent tool errors")
  863:         }
  864:         return ("ok", "READY", "live and healthy")
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:51` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   44:     /// Then home install app, then CLI.
   45:     public func resolveServeBinary(preferred: URL? = nil) -> URL {
   46:         if let preferred {
   47:             // Explicit override: do not silently fall back (tests and operator --binary).
   48:             return preferred.resolvingSymlinksInPath()
   49:         }
   50: 
   51:         // 1) Running GUI / process (Deploy from the app the operator is using)
   52:         if let running = Bundle.main.executableURL,
   53:            FileManager.default.isExecutableFile(atPath: running.path),
   54:            isForgeExecutable(running) {
   55:             return running.resolvingSymlinksInPath()
   56:         }
   57: 
   58:         let home = paths.home
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:194` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  187:             if hostActivation?.allRolesConnected == true {
  188:                 hostSummary = "LM Studio synchronized revision \(result.deploymentID) and connected both hosted roles"
  189:             } else {
  190:                 hostSummary = "LM Studio synchronized primary+failover revision \(result.deploymentID); hosted processes will start lazily when selected by a chat"
  191:             }
  192: 
  193:             // Return install result; message distinguishes host configuration
  194:             // synchronization from standalone per-role process verification.
  195:             return LMStudioMCPPluginInstaller.InstallResult(
  196:                 ok: fullyOK,
  197:                 binaryPath: result.binaryPath,
  198:                 pluginsWritten: result.pluginsWritten,
  199:                 mcpConfigPath: result.mcpConfigPath,
  200:                 deploymentID: result.deploymentID,
  201:                 message: fullyOK
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:408` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  401:     private func runningApplications() -> [NSRunningApplication] {
  402:         NSRunningApplication.runningApplications(withBundleIdentifier: Self.bundleIdentifier)
  403:             .filter { !$0.isTerminated }
  404:     }
  405: 
  406:     /// Launch Services does not always expose Electron applications through
  407:     /// `NSRunningApplication.runningApplications(withBundleIdentifier:)` to a
  408:     /// headless CLI process. Fall back to an exact executable-name lookup and
  409:     /// retain only concrete positive PIDs; no wildcard process matching.
  410:     private func runningPIDs() -> Set<Int32> {
  411:         var pids = Set(runningApplications().map(\.processIdentifier))
  412:         if let result = try? ProcessRunner().run(
  413:             executable: "/usr/bin/pgrep",
  414:             arguments: ["-x", "LM Studio"],
  415:             timeoutSec: 2,
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:409` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  402:         NSRunningApplication.runningApplications(withBundleIdentifier: Self.bundleIdentifier)
  403:             .filter { !$0.isTerminated }
  404:     }
  405: 
  406:     /// Launch Services does not always expose Electron applications through
  407:     /// `NSRunningApplication.runningApplications(withBundleIdentifier:)` to a
  408:     /// headless CLI process. Fall back to an exact executable-name lookup and
  409:     /// retain only concrete positive PIDs; no wildcard process matching.
  410:     private func runningPIDs() -> Set<Int32> {
  411:         var pids = Set(runningApplications().map(\.processIdentifier))
  412:         if let result = try? ProcessRunner().run(
  413:             executable: "/usr/bin/pgrep",
  414:             arguments: ["-x", "LM Studio"],
  415:             timeoutSec: 2,
  416:             maximumOutputBytes: 4_096
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:429` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  422:             }
  423:         }
  424:         return pids
  425:     }
  426: 
  427:     private func isLMStudioRunning() -> Bool {
  428:         // This AppleScript predicate queries Launch Services without launching
  429:         // the app and remains available when process enumeration is restricted.
  430:         if let result = try? ProcessRunner().run(
  431:             executable: "/usr/bin/osascript",
  432:             arguments: ["-e", "application \"\(Self.applicationName)\" is running"],
  433:             timeoutSec: 3,
  434:             maximumOutputBytes: 4_096
  435:         ), result.exitCode == 0 {
  436:             return result.stdout.trimmingCharacters(in: .whitespacesAndNewlines) == "true"
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:446` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  439:     }
  440: 
  441:     private func launch() throws {
  442:         var failures: [String] = []
  443: 
  444:         // `/usr/bin/open` is Apple's Launch Services client. Bundle-ID launch is
  445:         // more reliable for LM Studio's Electron bundle than opening its path,
  446:         // which can incorrectly return "corrupt" from a headless CLI process.
  447:         do {
  448:             let result = try ProcessRunner().run(
  449:                 executable: "/usr/bin/open",
  450:                 arguments: ["-a", Self.applicationName],
  451:                 timeoutSec: 8,
  452:                 maximumOutputBytes: 16_384
  453:             )
```

### `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:480` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  473:             failures.append("osascript: \(error.localizedDescription)")
  474:         }
  475:         throw HostActivationError.launchFailed(failures.joined(separator: "; "))
  476:     }
  477: 
  478:     private func relaunch() throws {
  479:         // A graceful application-level quit works even when the caller cannot
  480:         // enumerate Electron's process through NSWorkspace/pgrep.
  481:         _ = try? ProcessRunner().run(
  482:             executable: "/usr/bin/osascript",
  483:             arguments: ["-e", "tell application \"\(Self.applicationName)\" to quit"],
  484:             timeoutSec: 6,
  485:             maximumOutputBytes: 4_096
  486:         )
  487:         let applications = runningApplications()
```

### `Sources/ForgeConductorCore/Telemetry/ProcessDiscovery.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ProcessDiscovery.swift
    2: // What: Discovers Forge, manager, LM Studio, model, and MCP processes.
    3: // How: Native process enumeration classifies executable paths and arguments with strict
    4: // allow/deny rules, deduplicates PIDs, and returns bounded role-specific collections.
    5: // Why: Product health must not count unrelated local orchestration processes.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Live process discovery for **LM Studio local-model** workflows.
```

### `Sources/ForgeConductorCore/Telemetry/ProcessDiscovery.swift:10` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    3: // How: Native process enumeration classifies executable paths and arguments with strict
    4: // allow/deny rules, deduplicates PIDs, and returns bounded role-specific collections.
    5: // Why: Product health must not count unrelated local orchestration processes.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Live process discovery for **LM Studio local-model** workflows.
   11: ///
   12: /// Surfaces:
   13: /// - LM Studio host app
   14: /// - Local model backends (llama.cpp / mlx under `~/.lmstudio`)
   15: /// - Forge Conductor Swift MCP: `forge-conductor serve` (from `~/.lmstudio/mcp.json`)
   16: ///
   17: /// Explicitly **not** Claude Code orchestration, CCDT, `~/.claude/local-mcp`,
```

### `Sources/ForgeConductorCore/Telemetry/ProcessDiscovery.swift:25` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   18: /// or legacy Python/bash `forge-serve` wrappers.
   19: public enum ProcessDiscovery {
   20:     static let unknownMCPHostKind = "mcp-stdio-unknown"
   21:     static let unknownMCPLabel = "Forge MCP (role unknown)"
   22: 
   23:     /// Pure classification of this product's executable modes.
   24:     ///
   25:     /// Keeping argv classification separate from process enumeration makes the
   26:     /// app-bundle `serve` path testable without depending on the host process list.
   27:     enum ForgeCommandKind: Equatable {
   28:         case unrelated
   29:         case gui
   30:         case manager
   31:         case serve
   32:         case legacy
```

### `Sources/ForgeConductorCore/Telemetry/ProcessDiscovery.swift:26` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   19: public enum ProcessDiscovery {
   20:     static let unknownMCPHostKind = "mcp-stdio-unknown"
   21:     static let unknownMCPLabel = "Forge MCP (role unknown)"
   22: 
   23:     /// Pure classification of this product's executable modes.
   24:     ///
   25:     /// Keeping argv classification separate from process enumeration makes the
   26:     /// app-bundle `serve` path testable without depending on the host process list.
   27:     enum ForgeCommandKind: Equatable {
   28:         case unrelated
   29:         case gui
   30:         case manager
   31:         case serve
   32:         case legacy
   33:     }
```

### `Sources/ForgeConductorCore/Telemetry/SystemCollector.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // SystemCollector.swift
    2: // What: Composes all native hardware collectors into one SystemMetrics sample.
    3: // How: Injected CPU, RAM, disk, GPU, process, power, and frequency modules are sampled
    4: // and normalized with host/platform metadata at a shared timestamp.
    5: // Why: The engine depends on one replaceable system port rather than concrete collectors.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Composes modular host collectors into `SystemMetrics`.
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:748` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  741:         let client = ClientID("restart-writer")
  742:         let app = try ForgeApp.bootstrap(home: tempHome)
  743:         let handoff = try app.tools.call(
  744:             name: "session_handoff",
  745:             arguments: [
  746:                 "goal": "Resume after restart",
  747:                 "next_actions": ["Reload durable state"],
  748:                 "narrative": "State written before process shutdown",
  749:             ],
  750:             clientID: client
  751:         )
  752:         let handoffID = try XCTUnwrap(handoff.payload["handoff_id"] as? String)
  753:         let packetURL = app.paths.memoryHandoffsDir.appendingPathComponent("\(handoffID).json")
  754:         let latestURL = app.paths.memoryHandoffsDir.appendingPathComponent("LATEST")
  755:         let currentTaskURL = app.paths.memoryCurrentTask
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1127` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1120:     }
 1121: 
 1122:     func testPrimaryAndFallbackMCPProcessesShareContinuitySafely() throws {
 1123:         let binary = try XCTUnwrap(
 1124:             locateContinuityCLIBinary(),
 1125:             "The current test build must provide an adjacent forge-conductor CLI"
 1126:         )
 1127:         let processHome = tempHome.appendingPathComponent("process-home", isDirectory: true)
 1128:         try FileManager.default.createDirectory(at: processHome, withIntermediateDirectories: true)
 1129:         let primary = try launchMCPFixture(binary: binary, home: processHome, role: "primary")
 1130:         let fallback = try launchMCPFixture(binary: binary, home: processHome, role: "fallback")
 1131: 
 1132:         try sendMCPHandoff(primary, id: 2, goal: "Primary process handoff")
 1133:         try sendMCPHandoff(fallback, id: 2, goal: "Fallback process handoff")
 1134:         let primaryOutput = try waitForMCPFixture(primary, timeout: 10)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1132` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1125:             "The current test build must provide an adjacent forge-conductor CLI"
 1126:         )
 1127:         let processHome = tempHome.appendingPathComponent("process-home", isDirectory: true)
 1128:         try FileManager.default.createDirectory(at: processHome, withIntermediateDirectories: true)
 1129:         let primary = try launchMCPFixture(binary: binary, home: processHome, role: "primary")
 1130:         let fallback = try launchMCPFixture(binary: binary, home: processHome, role: "fallback")
 1131: 
 1132:         try sendMCPHandoff(primary, id: 2, goal: "Primary process handoff")
 1133:         try sendMCPHandoff(fallback, id: 2, goal: "Fallback process handoff")
 1134:         let primaryOutput = try waitForMCPFixture(primary, timeout: 10)
 1135:         let fallbackOutput = try waitForMCPFixture(fallback, timeout: 10)
 1136: 
 1137:         for output in [primaryOutput, fallbackOutput] {
 1138:             let response = output.first { ($0["id"] as? Int) == 2 }
 1139:             let result = try XCTUnwrap(response?["result"] as? [String: Any])
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1133` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1126:         )
 1127:         let processHome = tempHome.appendingPathComponent("process-home", isDirectory: true)
 1128:         try FileManager.default.createDirectory(at: processHome, withIntermediateDirectories: true)
 1129:         let primary = try launchMCPFixture(binary: binary, home: processHome, role: "primary")
 1130:         let fallback = try launchMCPFixture(binary: binary, home: processHome, role: "fallback")
 1131: 
 1132:         try sendMCPHandoff(primary, id: 2, goal: "Primary process handoff")
 1133:         try sendMCPHandoff(fallback, id: 2, goal: "Fallback process handoff")
 1134:         let primaryOutput = try waitForMCPFixture(primary, timeout: 10)
 1135:         let fallbackOutput = try waitForMCPFixture(fallback, timeout: 10)
 1136: 
 1137:         for output in [primaryOutput, fallbackOutput] {
 1138:             let response = output.first { ($0["id"] as? Int) == 2 }
 1139:             let result = try XCTUnwrap(response?["result"] as? [String: Any])
 1140:             XCTAssertEqual(result["isError"] as? Bool, false)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1155` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1148:         let store = try SQLiteStore(path: processHome.appendingPathComponent("store.sqlite"))
 1149:         let packets = try store.handoffList(limit: 10)
 1150:         let latest = try XCTUnwrap(store.handoffLatest())
 1151:         store.close()
 1152:         XCTAssertEqual(packets.count, 2)
 1153:         XCTAssertEqual(
 1154:             Set(packets.map(\.goal)),
 1155:             Set(["Primary process handoff", "Fallback process handoff"])
 1156:         )
 1157: 
 1158:         let paths = AppPaths(home: processHome)
 1159:         let pointer = try String(
 1160:             contentsOf: paths.memoryHandoffsDir.appendingPathComponent("LATEST"),
 1161:             encoding: .utf8
 1162:         )
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1800` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1793:         lock.lock()
 1794:         defer { lock.unlock() }
 1795:         return values
 1796:     }
 1797: }
 1798: 
 1799: private struct MCPProcessFixture {
 1800:     var process: Process
 1801:     var input: Pipe
 1802:     var output: Pipe
 1803:     var error: Pipe
 1804: }
 1805: 
 1806: private enum MCPProcessFixtureError: Error {
 1807:     case timeout(String)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1801` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1794:         defer { lock.unlock() }
 1795:         return values
 1796:     }
 1797: }
 1798: 
 1799: private struct MCPProcessFixture {
 1800:     var process: Process
 1801:     var input: Pipe
 1802:     var output: Pipe
 1803:     var error: Pipe
 1804: }
 1805: 
 1806: private enum MCPProcessFixtureError: Error {
 1807:     case timeout(String)
 1808:     case failed(String)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1802` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1795:         return values
 1796:     }
 1797: }
 1798: 
 1799: private struct MCPProcessFixture {
 1800:     var process: Process
 1801:     var input: Pipe
 1802:     var output: Pipe
 1803:     var error: Pipe
 1804: }
 1805: 
 1806: private enum MCPProcessFixtureError: Error {
 1807:     case timeout(String)
 1808:     case failed(String)
 1809: }
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1803` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1796:     }
 1797: }
 1798: 
 1799: private struct MCPProcessFixture {
 1800:     var process: Process
 1801:     var input: Pipe
 1802:     var output: Pipe
 1803:     var error: Pipe
 1804: }
 1805: 
 1806: private enum MCPProcessFixtureError: Error {
 1807:     case timeout(String)
 1808:     case failed(String)
 1809: }
 1810: 
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1821` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1814:     if FileManager.default.isExecutableFile(atPath: adjacent.path) {
 1815:         return adjacent
 1816:     }
 1817:     return nil
 1818: }
 1819: 
 1820: private func launchMCPFixture(binary: URL, home: URL, role: String) throws -> MCPProcessFixture {
 1821:     let process = Process()
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1822` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1815:         return adjacent
 1816:     }
 1817:     return nil
 1818: }
 1819: 
 1820: private func launchMCPFixture(binary: URL, home: URL, role: String) throws -> MCPProcessFixture {
 1821:     let process = Process()
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1823` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1816:     }
 1817:     return nil
 1818: }
 1819: 
 1820: private func launchMCPFixture(binary: URL, home: URL, role: String) throws -> MCPProcessFixture {
 1821:     let process = Process()
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1827` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1820: private func launchMCPFixture(binary: URL, home: URL, role: String) throws -> MCPProcessFixture {
 1821:     let process = Process()
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1828` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1821:     let process = Process()
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1829` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1822:     process.executableURL = binary
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1830` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1823:     process.arguments = ["serve"]
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1831` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1824:     var environment = ProcessInfo.processInfo.environment
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1832` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1825:     environment["FORGE_CONDUCTOR_HOME"] = home.path
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
 1839: private func sendMCPHandoff(_ fixture: MCPProcessFixture, id: Int, goal: String) throws {
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1833` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1826:     environment["FORGE_MCP_ROLE"] = role
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
 1839: private func sendMCPHandoff(_ fixture: MCPProcessFixture, id: Int, goal: String) throws {
 1840:     let initialize: [String: Any] = [
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1834` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1827:     environment["FORGE_DEPLOYMENT_ID"] = "continuity-process-test"
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
 1839: private func sendMCPHandoff(_ fixture: MCPProcessFixture, id: Int, goal: String) throws {
 1840:     let initialize: [String: Any] = [
 1841:         "jsonrpc": "2.0",
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1835` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1828:     process.environment = environment
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
 1839: private func sendMCPHandoff(_ fixture: MCPProcessFixture, id: Int, goal: String) throws {
 1840:     let initialize: [String: Any] = [
 1841:         "jsonrpc": "2.0",
 1842:         "id": 1,
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1836` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1829:     let input = Pipe()
 1830:     let output = Pipe()
 1831:     let error = Pipe()
 1832:     process.standardInput = input
 1833:     process.standardOutput = output
 1834:     process.standardError = error
 1835:     try process.run()
 1836:     return MCPProcessFixture(process: process, input: input, output: output, error: error)
 1837: }
 1838: 
 1839: private func sendMCPHandoff(_ fixture: MCPProcessFixture, id: Int, goal: String) throws {
 1840:     let initialize: [String: Any] = [
 1841:         "jsonrpc": "2.0",
 1842:         "id": 1,
 1843:         "method": "initialize",
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1847` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1840:     let initialize: [String: Any] = [
 1841:         "jsonrpc": "2.0",
 1842:         "id": 1,
 1843:         "method": "initialize",
 1844:         "params": [
 1845:             "protocolVersion": "2025-11-25",
 1846:             "capabilities": [:] as [String: Any],
 1847:             "clientInfo": ["name": "continuity-process-test", "version": "1"],
 1848:         ] as [String: Any],
 1849:     ]
 1850:     let handoff: [String: Any] = [
 1851:         "jsonrpc": "2.0",
 1852:         "id": id,
 1853:         "method": "tools/call",
 1854:         "params": [
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1870` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1863: }
 1864: 
 1865: private func waitForMCPFixture(
 1866:     _ fixture: MCPProcessFixture,
 1867:     timeout: TimeInterval
 1868: ) throws -> [[String: Any]] {
 1869:     let deadline = Date().addingTimeInterval(timeout)
 1870:     while fixture.process.isRunning, Date() < deadline {
 1871:         Thread.sleep(forTimeInterval: 0.02)
 1872:     }
 1873:     guard !fixture.process.isRunning else {
 1874:         fixture.process.terminate()
 1875:         throw MCPProcessFixtureError.timeout("MCP process did not exit after stdin closed")
 1876:     }
 1877:     let outputData = fixture.output.fileHandleForReading.readDataToEndOfFile()
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1873` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1866:     _ fixture: MCPProcessFixture,
 1867:     timeout: TimeInterval
 1868: ) throws -> [[String: Any]] {
 1869:     let deadline = Date().addingTimeInterval(timeout)
 1870:     while fixture.process.isRunning, Date() < deadline {
 1871:         Thread.sleep(forTimeInterval: 0.02)
 1872:     }
 1873:     guard !fixture.process.isRunning else {
 1874:         fixture.process.terminate()
 1875:         throw MCPProcessFixtureError.timeout("MCP process did not exit after stdin closed")
 1876:     }
 1877:     let outputData = fixture.output.fileHandleForReading.readDataToEndOfFile()
 1878:     let errorData = fixture.error.fileHandleForReading.readDataToEndOfFile()
 1879:     guard fixture.process.terminationStatus == 0 else {
 1880:         let stderr = String(data: errorData, encoding: .utf8) ?? ""
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1874` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1867:     timeout: TimeInterval
 1868: ) throws -> [[String: Any]] {
 1869:     let deadline = Date().addingTimeInterval(timeout)
 1870:     while fixture.process.isRunning, Date() < deadline {
 1871:         Thread.sleep(forTimeInterval: 0.02)
 1872:     }
 1873:     guard !fixture.process.isRunning else {
 1874:         fixture.process.terminate()
 1875:         throw MCPProcessFixtureError.timeout("MCP process did not exit after stdin closed")
 1876:     }
 1877:     let outputData = fixture.output.fileHandleForReading.readDataToEndOfFile()
 1878:     let errorData = fixture.error.fileHandleForReading.readDataToEndOfFile()
 1879:     guard fixture.process.terminationStatus == 0 else {
 1880:         let stderr = String(data: errorData, encoding: .utf8) ?? ""
 1881:         throw MCPProcessFixtureError.failed(stderr)
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1875` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1868: ) throws -> [[String: Any]] {
 1869:     let deadline = Date().addingTimeInterval(timeout)
 1870:     while fixture.process.isRunning, Date() < deadline {
 1871:         Thread.sleep(forTimeInterval: 0.02)
 1872:     }
 1873:     guard !fixture.process.isRunning else {
 1874:         fixture.process.terminate()
 1875:         throw MCPProcessFixtureError.timeout("MCP process did not exit after stdin closed")
 1876:     }
 1877:     let outputData = fixture.output.fileHandleForReading.readDataToEndOfFile()
 1878:     let errorData = fixture.error.fileHandleForReading.readDataToEndOfFile()
 1879:     guard fixture.process.terminationStatus == 0 else {
 1880:         let stderr = String(data: errorData, encoding: .utf8) ?? ""
 1881:         throw MCPProcessFixtureError.failed(stderr)
 1882:     }
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1879` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
 1872:     }
 1873:     guard !fixture.process.isRunning else {
 1874:         fixture.process.terminate()
 1875:         throw MCPProcessFixtureError.timeout("MCP process did not exit after stdin closed")
 1876:     }
 1877:     let outputData = fixture.output.fileHandleForReading.readDataToEndOfFile()
 1878:     let errorData = fixture.error.fileHandleForReading.readDataToEndOfFile()
 1879:     guard fixture.process.terminationStatus == 0 else {
 1880:         let stderr = String(data: errorData, encoding: .utf8) ?? ""
 1881:         throw MCPProcessFixtureError.failed(stderr)
 1882:     }
 1883:     return try outputData.split(separator: 0x0A, omittingEmptySubsequences: true).map { line in
 1884:         try JSONSupport.object(from: Data(line))
 1885:     }
 1886: }
```

### `Tests/ForgeConductorTests/CoreTests.swift:153` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  146:     }
  147: 
  148:     func testBindingRehydrateFromMemory() throws {
  149:         let app = try ForgeApp.bootstrap(home: tempHome)
  150:         let client = ClientID("c4")
  151:         let start = try app.sessions.start(agentID: "plan", goal: "design feature X", clientID: client)
  152:         let sid = start["session_id"] as! String
  153:         // Simulate process restart: new service stack, same store
  154:         let app2 = try ForgeApp.bootstrap(home: tempHome)
  155:         let binding = try app2.sessions.rehydrate(clientID: client)
  156:         XCTAssertNotNil(binding)
  157:         XCTAssertEqual(binding?.sessionID.rawValue, sid)
  158:         XCTAssertEqual(binding?.agentID, "plan")
  159:     }
  160: 
```

### `Tests/ForgeConductorTests/ForgeProcessEntryTests.swift:2` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ForgeProcessEntryTests.swift
    2: // Verifies argv classification for GUI, manager, and MCP-serving process modes.
    3: // These cases prevent the shared app binary from starting the wrong lifecycle.
    4: 
    5: import XCTest
    6: @testable import ForgeConductorCore
    7: 
    8: final class ForgeProcessEntryTests: XCTestCase {
    9:     func testParseModeGUIWhenNoArgs() {
```

### `Tests/ForgeConductorTests/G1G10AcceptanceTests.swift:137` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  130:         XCTAssertEqual((fallback?["env"] as? [String: String])?["FORGE_MCP_ROLE"], "fallback")
  131: 
  132:         let mcpRoot = try JSONSerialization.jsonObject(with: Data(contentsOf: lmHome.appendingPathComponent("mcp.json"))) as? [String: Any]
  133:         let servers = mcpRoot?["mcpServers"] as? [String: Any]
  134:         XCTAssertNotNil(servers?["forge-conductor"])
  135:         XCTAssertNotNil(servers?["forge-conductor-fallback"])
  136: 
  137:         // G7: process-level smoke already ran inside deploy; re-verify + tool call in-process
  138:         let smoke = try MCPServeVerifier.verify(binary: binary, home: forgeHome, role: "primary")
  139:         XCTAssertTrue(smoke.ok, smoke.detail)
  140:         XCTAssertEqual(smoke.protocolVersion, "2025-11-25")
  141:         XCTAssertGreaterThanOrEqual(smoke.toolCount, 20)
  142:         XCTAssertTrue(MCPServeVerifier.requiredProductTools.isSubset(of: Set(smoke.toolNames)))
  143: 
  144:         let app = try ForgeApp.bootstrap(home: forgeHome)
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:241` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  234:         XCTAssertTrue(TelemetryContract.validate(snapshot: a).isEmpty)
  235:         XCTAssertTrue(TelemetryContract.validate(snapshot: b).isEmpty)
  236:         let cpuA = ((a["system"] as? [String: Any])?["cpu"] as? [String: Any])?["per_cpu"] as? [Double]
  237:         XCTAssertEqual(cpuA?.count, ((a["system"] as? [String: Any])?["cpu"] as? [String: Any])?["count_logical"] as? Int)
  238:     }
  239: }
  240: 
  241: /// Hermetic regressions for Forge MCP process classification and card reconciliation.
  242: /// These tests never inspect or mutate the host process list or live LM Studio files.
  243: final class MCPProcessTelemetryRegressionTests: XCTestCase {
  244:     private let appExecutable =
  245:         "/Applications/Forge Conductor.app/Contents/MacOS/Forge Conductor"
  246:     private let sharedCommand =
  247:         "/Applications/Forge Conductor.app/Contents/MacOS/Forge Conductor"
  248:     private let now = Date(timeIntervalSince1970: 1_735_689_600)
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:242` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  235:         XCTAssertTrue(TelemetryContract.validate(snapshot: b).isEmpty)
  236:         let cpuA = ((a["system"] as? [String: Any])?["cpu"] as? [String: Any])?["per_cpu"] as? [Double]
  237:         XCTAssertEqual(cpuA?.count, ((a["system"] as? [String: Any])?["cpu"] as? [String: Any])?["count_logical"] as? Int)
  238:     }
  239: }
  240: 
  241: /// Hermetic regressions for Forge MCP process classification and card reconciliation.
  242: /// These tests never inspect or mutate the host process list or live LM Studio files.
  243: final class MCPProcessTelemetryRegressionTests: XCTestCase {
  244:     private let appExecutable =
  245:         "/Applications/Forge Conductor.app/Contents/MacOS/Forge Conductor"
  246:     private let sharedCommand =
  247:         "/Applications/Forge Conductor.app/Contents/MacOS/Forge Conductor"
  248:     private let now = Date(timeIntervalSince1970: 1_735_689_600)
  249: 
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:306` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  299:             ),
  300:             .serve
  301:         )
  302:     }
  303: 
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:307` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  300:             .serve
  301:         )
  302:     }
  303: 
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
  314:                 hostKind: ProcessDiscovery.unknownMCPHostKind
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:308` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  301:         )
  302:     }
  303: 
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
  314:                 hostKind: ProcessDiscovery.unknownMCPHostKind
  315:             ),
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:309` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  302:     }
  303: 
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
  314:                 hostKind: ProcessDiscovery.unknownMCPHostKind
  315:             ),
  316:         ]
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:310` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  303: 
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
  314:                 hostKind: ProcessDiscovery.unknownMCPHostKind
  315:             ),
  316:         ]
  317: 
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:311` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  304:     func testMCPExternalCountIncludesBothStdioRolesAndExcludesModelBackends() {
  305:         let processes = [
  306:             process(pid: 11, label: "forge-conductor", hostKind: "mcp-stdio"),
  307:             process(pid: 12, label: "forge-conductor-fallback", hostKind: "mcp-stdio-fallback"),
  308:             process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  309:             process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  310:             process(pid: 11, label: "duplicate-primary", hostKind: "mcp-stdio"),
  311:             process(
  312:                 pid: 15,
  313:                 label: ProcessDiscovery.unknownMCPLabel,
  314:                 hostKind: ProcessDiscovery.unknownMCPHostKind
  315:             ),
  316:         ]
  317: 
  318:         XCTAssertEqual(ProcessDiscovery.mcpExternalProcessCount(processes), 3)
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:367` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  360:         XCTAssertEqual(metadata.connectorRole, "fallback")
  361:     }
  362: 
  363:     func testUnknownLiveRoleDoesNotSuppressEitherConfiguredRole() {
  364:         let cards = assembler(alivePIDs: [16]).build(
  365:             presence: [],
  366:             live: [
  367:                 process(
  368:                     pid: 16,
  369:                     label: ProcessDiscovery.unknownMCPLabel,
  370:                     hostKind: ProcessDiscovery.unknownMCPHostKind
  371:                 ),
  372:             ],
  373:             configured: connectorConfigurations(),
  374:             audit: []
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:393` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  386:         )
  387:     }
  388: 
  389:     func testAssemblerOmitsLMStudioHostAndModelBackendCards() {
  390:         let cards = assembler(alivePIDs: [14, 13, 21]).build(
  391:             presence: [],
  392:             live: [
  393:                 process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  394:                 process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  395:                 process(pid: 21, label: "forge-conductor", hostKind: "mcp-stdio"),
  396:             ],
  397:             configured: connectorConfigurations(),
  398:             audit: []
  399:         )
  400: 
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:394` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  387:     }
  388: 
  389:     func testAssemblerOmitsLMStudioHostAndModelBackendCards() {
  390:         let cards = assembler(alivePIDs: [14, 13, 21]).build(
  391:             presence: [],
  392:             live: [
  393:                 process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  394:                 process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  395:                 process(pid: 21, label: "forge-conductor", hostKind: "mcp-stdio"),
  396:             ],
  397:             configured: connectorConfigurations(),
  398:             audit: []
  399:         )
  400: 
  401:         XCTAssertFalse(cards.contains { $0.hostKind == "lm-studio-host" })
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:395` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  388: 
  389:     func testAssemblerOmitsLMStudioHostAndModelBackendCards() {
  390:         let cards = assembler(alivePIDs: [14, 13, 21]).build(
  391:             presence: [],
  392:             live: [
  393:                 process(pid: 14, label: "LM Studio", hostKind: "lm-studio-host"),
  394:                 process(pid: 13, label: "llama-server", hostKind: "model-backend"),
  395:                 process(pid: 21, label: "forge-conductor", hostKind: "mcp-stdio"),
  396:             ],
  397:             configured: connectorConfigurations(),
  398:             audit: []
  399:         )
  400: 
  401:         XCTAssertFalse(cards.contains { $0.hostKind == "lm-studio-host" })
  402:         XCTAssertFalse(cards.contains { $0.hostKind == "model-backend" })
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:410` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  403:         XCTAssertEqual(cards.filter(\.live).map(\.role), ["primary"])
  404:     }
  405: 
  406:     func testLivePrimaryDoesNotSuppressFallbackConfigurationWithSharedCommand() {
  407:         let cards = assembler(alivePIDs: [21]).build(
  408:             presence: [],
  409:             live: [
  410:                 process(pid: 21, label: "forge-conductor", hostKind: "mcp-stdio"),
  411:             ],
  412:             configured: connectorConfigurations(),
  413:             audit: []
  414:         )
  415: 
  416:         XCTAssertEqual(cards.count, 2)
  417:         let primary = cards.first { $0.role == "primary" }
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:439` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  432:                     hostKind: "mcp-stdio-fallback",
  433:                     pid: 31,
  434:                     cwd: "/tmp/forge",
  435:                     lastHeartbeat: heartbeat
  436:                 ),
  437:             ],
  438:             live: [
  439:                 process(
  440:                     pid: 31,
  441:                     label: ProcessDiscovery.unknownMCPLabel,
  442:                     hostKind: ProcessDiscovery.unknownMCPHostKind
  443:                 ),
  444:             ],
  445:             configured: connectorConfigurations(),
  446:             audit: []
```

### `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift:607` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  600:     private func assembler(alivePIDs: Set<Int32>) -> MCPServerCardAssembler {
  601:         MCPServerCardAssembler(
  602:             now: now,
  603:             isPIDAlive: { alivePIDs.contains($0) }
  604:         )
  605:     }
  606: 
  607:     private func process(
  608:         pid: Int32,
  609:         label: String,
  610:         hostKind: String
  611:     ) -> ProcessDiscovery.MCPProcess {
  612:         ProcessDiscovery.MCPProcess(
  613:             pid: pid,
  614:             label: label,
```

### `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift:25` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   18:         XCTAssertEqual(text.filter { $0 == "\n" }.count, 1)
   19:         let decoded = try JSONSupport.object(from: Data(text.dropLast().utf8))
   20:         XCTAssertEqual(decoded["jsonrpc"] as? String, "2.0")
   21:         XCTAssertEqual(decoded["id"] as? Int, 1)
   22:     }
   23: 
   24:     func testStreamReaderRejectsOversizedContentLengthBeforeReadingBody() throws {
   25:         let pipe = Pipe()
   26:         let reader = MCPStreamReader(handle: pipe.fileHandleForReading, maximumMessageBytes: 64)
   27:         try pipe.fileHandleForWriting.write(contentsOf: Data("Content-Length: 100\r\n\r\n".utf8))
   28:         try pipe.fileHandleForWriting.close()
   29: 
   30:         XCTAssertThrowsError(try reader.readMessage()) { error in
   31:             guard case MCPStreamError.messageTooLarge(64) = error else {
   32:                 return XCTFail("unexpected error: \(error)")
```

### `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift:26` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   19:         let decoded = try JSONSupport.object(from: Data(text.dropLast().utf8))
   20:         XCTAssertEqual(decoded["jsonrpc"] as? String, "2.0")
   21:         XCTAssertEqual(decoded["id"] as? Int, 1)
   22:     }
   23: 
   24:     func testStreamReaderRejectsOversizedContentLengthBeforeReadingBody() throws {
   25:         let pipe = Pipe()
   26:         let reader = MCPStreamReader(handle: pipe.fileHandleForReading, maximumMessageBytes: 64)
   27:         try pipe.fileHandleForWriting.write(contentsOf: Data("Content-Length: 100\r\n\r\n".utf8))
   28:         try pipe.fileHandleForWriting.close()
   29: 
   30:         XCTAssertThrowsError(try reader.readMessage()) { error in
   31:             guard case MCPStreamError.messageTooLarge(64) = error else {
   32:                 return XCTFail("unexpected error: \(error)")
   33:             }
```

### `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift:27` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   20:         XCTAssertEqual(decoded["jsonrpc"] as? String, "2.0")
   21:         XCTAssertEqual(decoded["id"] as? Int, 1)
   22:     }
   23: 
   24:     func testStreamReaderRejectsOversizedContentLengthBeforeReadingBody() throws {
   25:         let pipe = Pipe()
   26:         let reader = MCPStreamReader(handle: pipe.fileHandleForReading, maximumMessageBytes: 64)
   27:         try pipe.fileHandleForWriting.write(contentsOf: Data("Content-Length: 100\r\n\r\n".utf8))
   28:         try pipe.fileHandleForWriting.close()
   29: 
   30:         XCTAssertThrowsError(try reader.readMessage()) { error in
   31:             guard case MCPStreamError.messageTooLarge(64) = error else {
   32:                 return XCTFail("unexpected error: \(error)")
   33:             }
   34:         }
```

### `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift:28` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
   21:         XCTAssertEqual(decoded["id"] as? Int, 1)
   22:     }
   23: 
   24:     func testStreamReaderRejectsOversizedContentLengthBeforeReadingBody() throws {
   25:         let pipe = Pipe()
   26:         let reader = MCPStreamReader(handle: pipe.fileHandleForReading, maximumMessageBytes: 64)
   27:         try pipe.fileHandleForWriting.write(contentsOf: Data("Content-Length: 100\r\n\r\n".utf8))
   28:         try pipe.fileHandleForWriting.close()
   29: 
   30:         XCTAssertThrowsError(try reader.readMessage()) { error in
   31:             guard case MCPStreamError.messageTooLarge(64) = error else {
   32:                 return XCTFail("unexpected error: \(error)")
   33:             }
   34:         }
   35:     }
```

### `Tests/ForgeConductorTests/MemoryToolTests.swift:126` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
  119:             let set = try app.tools.call(
  120:                 name: "memory_set",
  121:                 arguments: ["key": "task/current", "body": "Ship durable memory MCP"],
  122:                 clientID: client
  123:             )
  124:             XCTAssertTrue(set.ok)
  125:         }
  126:         // New process/composition root, same home → note still present.
  127:         let app2 = try ForgeApp.bootstrap(home: tempHome)
  128:         let get = try app2.tools.call(
  129:             name: "memory_get",
  130:             arguments: ["key": "task/current"],
  131:             clientID: client
  132:         )
  133:         XCTAssertTrue(get.ok)
```

### `Tests/ForgeConductorTests/ProductPathReliabilityTests.swift:3` — \bProcess\b\|\bPipe\b\|readabilityHandler\|terminationHandler\|waitUntilExit\|closeFile

```swift
    1: // ProductPathReliabilityTests.swift
    2: // Exercises operator-critical paths such as MCP negotiation and tool discovery.
    3: // In-process protocol calls provide deterministic coverage without automating LM Studio.
    4: 
    5: import XCTest
    6: @testable import ForgeConductorCore
    7: 
    8: /// G1/G7: product reliability — MCP negotiate + tools surface without LM Studio UI.
    9: final class ProductPathReliabilityTests: XCTestCase {
   10:     func testInProcessMCPHandshakeToolsList() throws {
```

## History/cache/queue growth and bounds

372 lexical hits.

### `Sources/ForgeConductorApp/AppModel.swift:22` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   15: /// Views read immutable projections from this model and send user intent back through
   16: /// its methods. The model keeps process control, persistence, deployment, and telemetry
   17: /// work inside Core services so the SwiftUI layer remains declarative and testable.
   18: @MainActor
   19: public final class AppModel: ObservableObject {
   20:     @Published public private(set) var system: SystemMetrics?
   21:     @Published public private(set) var forge: ForgeSnapshot?
   22:     @Published public private(set) var history: [HistoryPoint] = []
   23:     @Published public private(set) var updated: Date?
   24:     @Published public private(set) var lastError: String?
   25:     @Published public private(set) var isLoading = false
   26:     @Published public private(set) var version: String = ForgeApp.version
   27:     @Published public private(set) var homePath: String = ""
   28:     @Published public private(set) var lastTyped: TelemetrySnapshot?
   29:     @Published public private(set) var managerStatus: ManagerStatus?
```

### `Sources/ForgeConductorApp/AppModel.swift:182` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  175:             }
  176:     }
  177: 
  178:     private func syncFromTelemetryBinding() {
  179:         let b = telemetryBinding
  180:         system = b.system
  181:         forge = b.forge
  182:         history = b.history
  183:         updated = b.updated
  184:         lastTyped = b.lastTyped
  185:         measuredTelemetryHz = b.measuredHz
  186:         isLoading = b.isLoading
  187:         if let e = b.lastError { lastError = e }
  188:     }
  189: 
```

### `Sources/ForgeConductorApp/AppModel.swift:346` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  339:     public var perCPU: [Double] { system?.cpu.perCPU ?? [] }
  340:     public var diskVolumes: [DiskVolume] { system?.disk ?? [] }
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
  351:     public var cpuPercent: Double { system?.cpu.percent ?? 0 }
  352:     public var ramPercent: Double { system?.ram.percent ?? 0 }
  353:     public var gpuPercent: Double? { system?.gpu.first?.utilGPU }
```

### `Sources/ForgeConductorApp/AppModel.swift:347` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  340:     public var diskVolumes: [DiskVolume] { system?.disk ?? [] }
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
  351:     public var cpuPercent: Double { system?.cpu.percent ?? 0 }
  352:     public var ramPercent: Double { system?.ram.percent ?? 0 }
  353:     public var gpuPercent: Double? { system?.gpu.first?.utilGPU }
  354:     public var hostName: String { system?.host ?? "host" }
```

### `Sources/ForgeConductorApp/AppModel.swift:348` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  341:     public var diskIO: DiskIOMetrics {
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
  351:     public var cpuPercent: Double { system?.cpu.percent ?? 0 }
  352:     public var ramPercent: Double { system?.ram.percent ?? 0 }
  353:     public var gpuPercent: Double? { system?.gpu.first?.utilGPU }
  354:     public var hostName: String { system?.host ?? "host" }
  355: 
```

### `Sources/ForgeConductorApp/AppModel.swift:349` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  342:         system?.diskIO
  343:             ?? DiskIOMetrics(readMBs: 0, writeMBs: 0, totalMBs: 0, readIOPS: 0, writeIOPS: 0, totalIOPS: 0)
  344:     }
  345:     public var hotProcesses: [ProcessMetrics] { system?.processes ?? [] }
  346:     public var historyCPU: [Float] { history.map { Float($0.cpu) } }
  347:     public var historyRAM: [Float] { history.map { Float($0.ram) } }
  348:     public var historyGPU: [Float?] {
  349:         history.map { point in point.gpu.map(Float.init) }
  350:     }
  351:     public var cpuPercent: Double { system?.cpu.percent ?? 0 }
  352:     public var ramPercent: Double { system?.ram.percent ?? 0 }
  353:     public var gpuPercent: Double? { system?.gpu.first?.utilGPU }
  354:     public var hostName: String { system?.host ?? "host" }
  355: 
  356:     public var mcpServerCards: [MCPServerCard] { forge?.mcpServers ?? [] }
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:17` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   10: 
   11: /// Binds the continuous realtime telemetry stream to UI state.
   12: /// Host metrics come from `RealtimeMetricsEngine` samples — never a multi-second snapshot poll.
   13: @MainActor
   14: public final class AppTelemetryBinding: ObservableObject {
   15:     public private(set) var system: SystemMetrics?
   16:     public private(set) var forge: ForgeSnapshot?
   17:     public private(set) var history: [HistoryPoint] = []
   18:     public private(set) var updated: Date?
   19:     public private(set) var lastTyped: TelemetrySnapshot?
   20:     public private(set) var measuredHz: Double = 0
   21:     public private(set) var lastError: String?
   22:     public private(set) var isLoading = false
   23:     @Published public var autoRefresh = true
   24: 
```

### `Sources/ForgeConductorApp/AppTelemetryBinding.swift:116` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  109:     }
  110: 
  111:     private func apply(_ typed: TelemetrySnapshot) {
  112:         objectWillChange.send()
  113:         lastTyped = typed
  114:         system = typed.system
  115:         forge = typed.forge
  116:         history = typed.history
  117:         updated = Date(timeIntervalSince1970: typed.updated)
  118:         lastError = nil
  119:         isLoading = false
  120:         updateSubject.send()
  121:     }
  122: }
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:4` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    1: // LoadTraceRenderer.swift
    2: // What: Draws the historical load trace into an MTKView.
    3: // How: The delegate converts normalized samples into GPU vertex buffers and
    4: // encodes Metal draw calls whenever SwiftUI supplies updated history.
    5: // Why: GPU rendering keeps a rapidly refreshing chart off the main UI drawing path.
    6: 
    7: import Foundation
    8: import MetalKit
    9: import simd
   10: 
   11: /// Metal renderer for CPU load history (glowing cyan line + fill).
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:11` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    4: // encodes Metal draw calls whenever SwiftUI supplies updated history.
    5: // Why: GPU rendering keeps a rapidly refreshing chart off the main UI drawing path.
    6: 
    7: import Foundation
    8: import MetalKit
    9: import simd
   10: 
   11: /// Metal renderer for CPU load history (glowing cyan line + fill).
   12: @MainActor
   13: final class LoadTraceRenderer: NSObject, MTKViewDelegate {
   14:     private var device: MTLDevice?
   15:     private var queue: MTLCommandQueue?
   16:     private var pipeline: MTLRenderPipelineState?
   17:     private var vertexBuffer: MTLBuffer?
   18:     private var sampleCount = 0
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:15` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    8: import MetalKit
    9: import simd
   10: 
   11: /// Metal renderer for CPU load history (glowing cyan line + fill).
   12: @MainActor
   13: final class LoadTraceRenderer: NSObject, MTKViewDelegate {
   14:     private var device: MTLDevice?
   15:     private var queue: MTLCommandQueue?
   16:     private var pipeline: MTLRenderPipelineState?
   17:     private var vertexBuffer: MTLBuffer?
   18:     private var sampleCount = 0
   19:     private let lock = NSLock()
   20:     private var samples: [Float] = []
   21: 
   22:     func attach(to view: MTKView) {
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:28` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   21: 
   22:     func attach(to view: MTKView) {
   23:         let mtl = view.device ?? MTLCreateSystemDefaultDevice()
   24:         guard let device = mtl else { return }
   25:         self.device = device
   26:         view.device = device
   27:         view.delegate = self
   28:         queue = device.makeCommandQueue()
   29:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   30:     }
   31: 
   32:     func update(samples: [Float]) {
   33:         lock.lock()
   34:         self.samples = samples
   35:         lock.unlock()
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:91` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   84:             let t = min(max(v / 100.0, 0), 1)
   85:             return -0.85 + 1.7 * t
   86:         }
   87: 
   88:         // Fill strip
   89:         for i in 0..<n {
   90:             let val = i < src.count ? src[i] : 0
   91:             verts.append(V(pos: SIMD2(x(i), -0.85), color: base))
   92:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyan))
   93:         }
   94:         let fillCount = verts.count
   95: 
   96:         // Line
   97:         for i in 0..<n {
   98:             let val = i < src.count ? src[i] : 0
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:92` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   85:             return -0.85 + 1.7 * t
   86:         }
   87: 
   88:         // Fill strip
   89:         for i in 0..<n {
   90:             let val = i < src.count ? src[i] : 0
   91:             verts.append(V(pos: SIMD2(x(i), -0.85), color: base))
   92:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyan))
   93:         }
   94:         let fillCount = verts.count
   95: 
   96:         // Line
   97:         for i in 0..<n {
   98:             let val = i < src.count ? src[i] : 0
   99:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyanLine))
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:99` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   92:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyan))
   93:         }
   94:         let fillCount = verts.count
   95: 
   96:         // Line
   97:         for i in 0..<n {
   98:             let val = i < src.count ? src[i] : 0
   99:             verts.append(V(pos: SIMD2(x(i), y(val)), color: cyanLine))
  100:         }
  101: 
  102:         let bytes = verts.count * MemoryLayout<V>.stride
  103:         vertexBuffer = device.makeBuffer(bytes: verts, length: bytes, options: .storageModeShared)
  104:         sampleCount = fillCount // first draw fill; line uses rest
  105:         // Store line offset in high bits via sampleCount encoding: fillCount | (lineCount << 16) — simpler: store both
  106:         lock.lock()
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:121` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  114: 
  115:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  116: 
  117:     func draw(in view: MTKView) {
  118:         guard let drawable = view.currentDrawable,
  119:               let rpd = view.currentRenderPassDescriptor,
  120:               let pipeline,
  121:               let queue,
  122:               let buffer = queue.makeCommandBuffer(),
  123:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  124:               let vertexBuffer else { return }
  125: 
  126:         encoder.setRenderPipelineState(pipeline)
  127:         encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
  128: 
```

### `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:122` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  115:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  116: 
  117:     func draw(in view: MTKView) {
  118:         guard let drawable = view.currentDrawable,
  119:               let rpd = view.currentRenderPassDescriptor,
  120:               let pipeline,
  121:               let queue,
  122:               let buffer = queue.makeCommandBuffer(),
  123:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  124:               let vertexBuffer else { return }
  125: 
  126:         encoder.setRenderPipelineState(pipeline)
  127:         encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
  128: 
  129:         lock.lock()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:89` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   82: }
   83: 
   84: // MARK: - Horizontal meter
   85: 
   86: @MainActor
   87: final class MetalBarRenderer: NSObject, MTKViewDelegate {
   88:     private var device: MTLDevice?
   89:     private var queue: MTLCommandQueue?
   90:     private var pipeline: MTLRenderPipelineState?
   91:     private var buffer: MTLBuffer?
   92:     private var fraction: Float = 0
   93:     private var color = MetalGaugePalette.cyan
   94:     private let lock = NSLock()
   95: 
   96:     func attach(_ view: MTKView) {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:106` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   99:         self.device = device
  100:         view.device = device
  101:         view.delegate = self
  102:         view.clearColor = MTLClearColor(red: 0.02, green: 0.04, blue: 0.08, alpha: 1)
  103:         view.isPaused = false
  104:         view.enableSetNeedsDisplay = false
  105:         view.preferredFramesPerSecond = 24
  106:         queue = device.makeCommandQueue()
  107:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  108:         rebuild()
  109:     }
  110: 
  111:     func set(fraction: Float, color: SIMD4<Float>) {
  112:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  113:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:136` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  129:         ]
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
  132: 
  133:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  134:     func draw(in view: MTKView) {
  135:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  136:               let pipeline, let queue, let buffer,
  137:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  138:         enc.setRenderPipelineState(pipeline)
  139:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  140:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  141:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 4, vertexCount: 4)
  142:         enc.endEncoding(); cmd.present(d); cmd.commit()
  143:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:137` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  130:         buffer = device.makeBuffer(bytes: &v, length: v.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  131:     }
  132: 
  133:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  134:     func draw(in view: MTKView) {
  135:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  136:               let pipeline, let queue, let buffer,
  137:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  138:         enc.setRenderPipelineState(pipeline)
  139:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  140:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  141:         enc.drawPrimitives(type: .triangleStrip, vertexStart: 4, vertexCount: 4)
  142:         enc.endEncoding(); cmd.present(d); cmd.commit()
  143:     }
  144: }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:187` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  180: }
  181: 
  182: // MARK: - Activity ring (MCP cards)
  183: 
  184: @MainActor
  185: final class MetalRingRenderer: NSObject, MTKViewDelegate {
  186:     private var device: MTLDevice?
  187:     private var queue: MTLCommandQueue?
  188:     private var pipeline: MTLRenderPipelineState?
  189:     private var buffer: MTLBuffer?
  190:     private var count = 0
  191:     private var fraction: Float = 0
  192:     private var color = MetalGaugePalette.cyan
  193:     private let lock = NSLock()
  194: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:205` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  198:         self.device = device
  199:         view.device = device
  200:         view.delegate = self
  201:         view.clearColor = MTLClearColor(red: 0.015, green: 0.03, blue: 0.06, alpha: 1)
  202:         view.isPaused = false
  203:         view.enableSetNeedsDisplay = false
  204:         view.preferredFramesPerSecond = 24
  205:         queue = device.makeCommandQueue()
  206:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  207:         rebuild()
  208:     }
  209: 
  210:     func set(fraction: Float, color: SIMD4<Float>) {
  211:         lock.lock(); self.fraction = min(max(fraction, 0), 1); self.color = color; lock.unlock()
  212:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:255` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  248:             let a0 = start + (end - start) * t0
  249:             let a1 = start + (end - start) * t1
  250:             let o0 = SIMD2(cos(a0) * outer, sin(a0) * outer)
  251:             let o1 = SIMD2(cos(a1) * outer, sin(a1) * outer)
  252:             let i0 = SIMD2(cos(a0) * inner, sin(a0) * inner)
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:256` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  249:             let a1 = start + (end - start) * t1
  250:             let o0 = SIMD2(cos(a0) * outer, sin(a0) * outer)
  251:             let o1 = SIMD2(cos(a1) * outer, sin(a1) * outer)
  252:             let i0 = SIMD2(cos(a0) * inner, sin(a0) * inner)
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:257` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  250:             let o0 = SIMD2(cos(a0) * outer, sin(a0) * outer)
  251:             let o1 = SIMD2(cos(a1) * outer, sin(a1) * outer)
  252:             let i0 = SIMD2(cos(a0) * inner, sin(a0) * inner)
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:258` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  251:             let o1 = SIMD2(cos(a1) * outer, sin(a1) * outer)
  252:             let i0 = SIMD2(cos(a0) * inner, sin(a0) * inner)
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:259` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  252:             let i0 = SIMD2(cos(a0) * inner, sin(a0) * inner)
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:260` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  253:             let i1 = SIMD2(cos(a1) * inner, sin(a1) * inner)
  254:             // two triangles
  255:             verts.append(.init(pos: o0, color: color))
  256:             verts.append(.init(pos: i0, color: color))
  257:             verts.append(.init(pos: o1, color: color))
  258:             verts.append(.init(pos: o1, color: color))
  259:             verts.append(.init(pos: i0, color: color))
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  267:               let pipeline, let queue, let buffer, count >= 3,
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:267` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  260:             verts.append(.init(pos: i1, color: color))
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  267:               let pipeline, let queue, let buffer, count >= 3,
  268:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  269:         enc.setRenderPipelineState(pipeline)
  270:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  271:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  272:         enc.endEncoding(); cmd.present(d); cmd.commit()
  273:     }
  274: }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:268` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  261:         }
  262:     }
  263: 
  264:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  265:     func draw(in view: MTKView) {
  266:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  267:               let pipeline, let queue, let buffer, count >= 3,
  268:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  269:         enc.setRenderPipelineState(pipeline)
  270:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  271:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  272:         enc.endEncoding(); cmd.present(d); cmd.commit()
  273:     }
  274: }
  275: 
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:314` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  307: }
  308: 
  309: // MARK: - Core bars (all Metal)
  310: 
  311: @MainActor
  312: final class MetalCoreBarsRenderer: NSObject, MTKViewDelegate {
  313:     private var device: MTLDevice?
  314:     private var queue: MTLCommandQueue?
  315:     private var pipeline: MTLRenderPipelineState?
  316:     private var buffer: MTLBuffer?
  317:     private var count = 0
  318:     private var cores: [Float] = []
  319:     private let lock = NSLock()
  320: 
  321:     func attach(_ view: MTKView) {
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:331` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  324:         self.device = device
  325:         view.device = device
  326:         view.delegate = self
  327:         view.clearColor = MTLClearColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1)
  328:         view.isPaused = false
  329:         view.enableSetNeedsDisplay = false
  330:         view.preferredFramesPerSecond = 20
  331:         queue = device.makeCommandQueue()
  332:         pipeline = MetalGaugePipeline.make(device: device, pixelFormat: view.colorPixelFormat)
  333:         rebuild()
  334:     }
  335: 
  336:     func set(cores: [Float]) {
  337:         lock.lock(); self.cores = cores; lock.unlock()
  338:         rebuild()
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:362` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  355:         let top: Float = 0.9
  356:         let height = top - bottom
  357:         for (i, pct) in cores.enumerated() {
  358:             let p = min(max(pct / 100, 0), 1)
  359:             let x0 = -1 + gap + Float(i) * (barW + gap)
  360:             let x1 = x0 + barW
  361:             // track
  362:             verts.append(contentsOf: quad(x0, bottom, x1, top, MetalGaugePalette.track))
  363:             // fill
  364:             let y1 = bottom + height * p
  365:             let hot = p >= 0.75
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:367` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  360:             let x1 = x0 + barW
  361:             // track
  362:             verts.append(contentsOf: quad(x0, bottom, x1, top, MetalGaugePalette.track))
  363:             // fill
  364:             let y1 = bottom + height * p
  365:             let hot = p >= 0.75
  366:             let c = hot ? MetalGaugePalette.orange : MetalGaugePalette.cyan
  367:             verts.append(contentsOf: quad(x0, bottom, x1, y1, c))
  368:         }
  369:         count = verts.count
  370:         buffer = device.makeBuffer(bytes: verts, length: verts.count * MemoryLayout<GaugeVertex>.stride, options: .storageModeShared)
  371:     }
  372: 
  373:     private func quad(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ c: SIMD4<Float>) -> [GaugeVertex] {
  374:         [
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:387` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  380:             .init(pos: SIMD2(x0, y1), color: c),
  381:         ]
  382:     }
  383: 
  384:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  385:     func draw(in view: MTKView) {
  386:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  387:               let pipeline, let queue, let buffer, count >= 3,
  388:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  389:         enc.setRenderPipelineState(pipeline)
  390:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  391:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  392:         enc.endEncoding(); cmd.present(d); cmd.commit()
  393:     }
  394: }
```

### `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:388` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  381:         ]
  382:     }
  383: 
  384:     func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  385:     func draw(in view: MTKView) {
  386:         guard let d = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
  387:               let pipeline, let queue, let buffer, count >= 3,
  388:               let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
  389:         enc.setRenderPipelineState(pipeline)
  390:         enc.setVertexBuffer(buffer, offset: 0, index: 0)
  391:         enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
  392:         enc.endEncoding(); cmd.present(d); cmd.commit()
  393:     }
  394: }
  395: 
```

### `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:10` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    3: // How: NSViewRepresentable creates an MTKView, assigns its coordinator, and
    4: // forwards new sample arrays without rebuilding the native view.
    5: // Why: The adapter isolates AppKit/Metal lifecycle details from dashboard composition.
    6: 
    7: import SwiftUI
    8: import MetalKit
    9: 
   10: /// SwiftUI wrapper around an MTKView that draws the load history with Metal.
   11: struct MetalLoadChart: NSViewRepresentable {
   12:     var samples: [Float]
   13: 
   14:     func makeCoordinator() -> LoadTraceRenderer {
   15:         LoadTraceRenderer()
   16:     }
   17: 
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:25` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   18:         public init(values: [Float?], color: SIMD4<Float>) {
   19:             self.values = values
   20:             self.color = color
   21:         }
   22:     }
   23: 
   24:     private var device: MTLDevice?
   25:     private var queue: MTLCommandQueue?
   26:     private var pipeline: MTLRenderPipelineState?
   27:     private var vertexBuffer: MTLBuffer?
   28:     private var vertexCount = 0
   29:     private let lock = NSLock()
   30:     private var series: [Series] = []
   31: 
   32:     public func attach(to view: MTKView) {
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:43` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   36:         view.device = device
   37:         view.delegate = self
   38:         view.clearColor = MTLClearColor(red: 0.01, green: 0.015, blue: 0.04, alpha: 1)
   39:         view.colorPixelFormat = .bgra8Unorm
   40:         view.isPaused = false
   41:         view.enableSetNeedsDisplay = false
   42:         view.preferredFramesPerSecond = 30
   43:         queue = device.makeCommandQueue()
   44:         buildPipeline(device: device, pixelFormat: view.colorPixelFormat)
   45:     }
   46: 
   47:     public func update(cpu: [Float], ram: [Float], gpu: [Float?]) {
   48:         lock.lock()
   49:         series = [
   50:             Series(values: cpu.map(Optional.some), color: SIMD4(0.09, 0.94, 1.0, 1.0)),
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:89` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   82: 
   83:         var verts: [V] = []
   84:         // Grid lines (horizontal at 25/50/75).
   85:         let gridColor = SIMD4<Float>(0.1, 0.35, 0.45, 0.35)
   86:         var gridCount = 0
   87:         for g in [Float(0.25), Float(0.5), Float(0.75)] {
   88:             let y = Float(-0.85) + Float(1.7) * g
   89:             verts.append(V(pos: SIMD2(Float(-1), y), color: gridColor))
   90:             verts.append(V(pos: SIMD2(Float(1), y), color: gridColor))
   91:             gridCount += 2
   92:         }
   93:         let fillStart = verts.count
   94:         var fillCount = 0
   95:         if let cpu = seriesCopy.first {
   96:             let n = max(cpu.values.count, 2)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:90` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   83:         var verts: [V] = []
   84:         // Grid lines (horizontal at 25/50/75).
   85:         let gridColor = SIMD4<Float>(0.1, 0.35, 0.45, 0.35)
   86:         var gridCount = 0
   87:         for g in [Float(0.25), Float(0.5), Float(0.75)] {
   88:             let y = Float(-0.85) + Float(1.7) * g
   89:             verts.append(V(pos: SIMD2(Float(-1), y), color: gridColor))
   90:             verts.append(V(pos: SIMD2(Float(1), y), color: gridColor))
   91:             gridCount += 2
   92:         }
   93:         let fillStart = verts.count
   94:         var fillCount = 0
   95:         if let cpu = seriesCopy.first {
   96:             let n = max(cpu.values.count, 2)
   97:             let fill = SIMD4<Float>(0.09, 0.94, 1.0, 0.22)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:103` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   96:             let n = max(cpu.values.count, 2)
   97:             let fill = SIMD4<Float>(0.09, 0.94, 1.0, 0.22)
   98:             for i in 0..<n {
   99:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  100:                 let sample = i < cpu.values.count ? cpu.values[i] : nil
  101:                 let v = min(max((sample ?? 0) / 100, 0), 1)
  102:                 let y = -0.85 + 1.7 * v
  103:                 verts.append(V(pos: SIMD2(x, -0.85), color: SIMD4<Float>(0.05, 0.2, 0.3, 0)))
  104:                 verts.append(V(pos: SIMD2(x, y), color: fill))
  105:                 fillCount += 2
  106:             }
  107:         }
  108:         var lineRanges: [(Int, Int)] = []
  109:         for s in seriesCopy {
  110:             let n = max(s.values.count, 2)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:104` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   97:             let fill = SIMD4<Float>(0.09, 0.94, 1.0, 0.22)
   98:             for i in 0..<n {
   99:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  100:                 let sample = i < cpu.values.count ? cpu.values[i] : nil
  101:                 let v = min(max((sample ?? 0) / 100, 0), 1)
  102:                 let y = -0.85 + 1.7 * v
  103:                 verts.append(V(pos: SIMD2(x, -0.85), color: SIMD4<Float>(0.05, 0.2, 0.3, 0)))
  104:                 verts.append(V(pos: SIMD2(x, y), color: fill))
  105:                 fillCount += 2
  106:             }
  107:         }
  108:         var lineRanges: [(Int, Int)] = []
  109:         for s in seriesCopy {
  110:             let n = max(s.values.count, 2)
  111:             var segmentStart: Int?
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:116` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  109:         for s in seriesCopy {
  110:             let n = max(s.values.count, 2)
  111:             var segmentStart: Int?
  112:             var segmentCount = 0
  113: 
  114:             func finishSegment() {
  115:                 if let segmentStart, segmentCount >= 2 {
  116:                     lineRanges.append((segmentStart, segmentCount))
  117:                 }
  118:             }
  119: 
  120:             for i in 0..<n {
  121:                 let sample = i < s.values.count ? s.values[i] : nil
  122:                 guard let sample, sample.isFinite else {
  123:                     finishSegment()
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:134` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  127:                 }
  128:                 if segmentStart == nil {
  129:                     segmentStart = verts.count
  130:                 }
  131:                 let x = -1 + 2 * Float(i) / Float(n - 1)
  132:                 let v = min(max(sample / 100, 0), 1)
  133:                 let y = -0.85 + 1.7 * v
  134:                 verts.append(V(pos: SIMD2(x, y), color: s.color))
  135:                 segmentCount += 1
  136:             }
  137:             finishSegment()
  138:         }
  139: 
  140:         let bytes = verts.count * MemoryLayout<V>.stride
  141:         vertexBuffer = device.makeBuffer(bytes: verts, length: max(bytes, 16), options: .storageModeShared)
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:161` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  154: 
  155:     public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  156: 
  157:     public func draw(in view: MTKView) {
  158:         guard let drawable = view.currentDrawable,
  159:               let rpd = view.currentRenderPassDescriptor,
  160:               let pipeline,
  161:               let queue,
  162:               let buffer = queue.makeCommandBuffer(),
  163:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  164:               let vertexBuffer else { return }
  165: 
  166:         encoder.setRenderPipelineState(pipeline)
  167:         encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
  168: 
```

### `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:162` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  155:     public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  156: 
  157:     public func draw(in view: MTKView) {
  158:         guard let drawable = view.currentDrawable,
  159:               let rpd = view.currentRenderPassDescriptor,
  160:               let pipeline,
  161:               let queue,
  162:               let buffer = queue.makeCommandBuffer(),
  163:               let encoder = buffer.makeRenderCommandEncoder(descriptor: rpd),
  164:               let vertexBuffer else { return }
  165: 
  166:         encoder.setRenderPipelineState(pipeline)
  167:         encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
  168: 
  169:         lock.lock()
```

### `Sources/ForgeConductorApp/Views/LiveFeedView.swift:36` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   29:             }
   30:             .padding(.horizontal, 16)
   31:             .padding(.top, 16)
   32: 
   33:             List {
   34:                 ForEach(Array(model.liveFeedEvents.enumerated()), id: \.offset) { _, e in
   35:                     HStack(alignment: .firstTextBaseline, spacing: 10) {
   36:                         Text(String(e.timestamp.suffix(8)))
   37:                             .font(.system(.caption, design: .monospaced))
   38:                             .foregroundStyle(.secondary)
   39:                             .frame(width: 64, alignment: .leading)
   40:                         Text(e.status.uppercased())
   41:                             .font(.caption.weight(.bold))
   42:                             .foregroundStyle(statusColor(e.status))
   43:                             .frame(width: 56, alignment: .leading)
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:28` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   21: 
   22:     private var rigContent: some View {
   23:         ScrollView {
   24:             VStack(alignment: .leading, spacing: 16) {
   25:                 headerPills
   26:                 sysStrip
   27:                 MultiSeriesLoadChart(
   28:                     cpu: model.historyCPU,
   29:                     ram: model.historyRAM,
   30:                     gpu: model.historyGPU
   31:                 )
   32:                 .frame(height: 168)
   33:                 .clipShape(RoundedRectangle(cornerRadius: 10))
   34:                 .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.cyan.opacity(0.35), lineWidth: 1))
   35:                 .overlay(alignment: .topLeading) {
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:29` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   22:     private var rigContent: some View {
   23:         ScrollView {
   24:             VStack(alignment: .leading, spacing: 16) {
   25:                 headerPills
   26:                 sysStrip
   27:                 MultiSeriesLoadChart(
   28:                     cpu: model.historyCPU,
   29:                     ram: model.historyRAM,
   30:                     gpu: model.historyGPU
   31:                 )
   32:                 .frame(height: 168)
   33:                 .clipShape(RoundedRectangle(cornerRadius: 10))
   34:                 .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.cyan.opacity(0.35), lineWidth: 1))
   35:                 .overlay(alignment: .topLeading) {
   36:                     Text("LOAD TRACE  ·  Metal  ·  REAL-TIME  ·  \(model.telemetryModeLabel)")
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:30` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   23:         ScrollView {
   24:             VStack(alignment: .leading, spacing: 16) {
   25:                 headerPills
   26:                 sysStrip
   27:                 MultiSeriesLoadChart(
   28:                     cpu: model.historyCPU,
   29:                     ram: model.historyRAM,
   30:                     gpu: model.historyGPU
   31:                 )
   32:                 .frame(height: 168)
   33:                 .clipShape(RoundedRectangle(cornerRadius: 10))
   34:                 .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.cyan.opacity(0.35), lineWidth: 1))
   35:                 .overlay(alignment: .topLeading) {
   36:                     Text("LOAD TRACE  ·  Metal  ·  REAL-TIME  ·  \(model.telemetryModeLabel)")
   37:                         .font(.system(size: 10, weight: .semibold, design: .monospaced))
```

### `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:588` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  581:     private var liveFeedPanel: some View {
  582:         // Column budget (pt): ts 56 + status 36 + gap×3(24) + bar 40 + ms 44 = 200 fixed.
  583:         // Tool name takes remaining width and truncates — never pushes past the panel.
  584:         panel("LIVE STREAM ▮ TOOLS · AGENTS · DIAGNOSTICS", meta: "\(model.liveFeedEvents.count)") {
  585:             VStack(alignment: .leading, spacing: 5) {
  586:                 ForEach(Array(model.liveFeedEvents.prefix(24).enumerated()), id: \.offset) { _, e in
  587:                     HStack(spacing: 8) {
  588:                         Text(String(e.timestamp.suffix(8)))
  589:                             .foregroundStyle(.secondary)
  590:                             .frame(width: 56, alignment: .leading)
  591:                         Text(e.status)
  592:                             .foregroundStyle(auditStatusColor(e.status))
  593:                             .frame(width: 36, alignment: .leading)
  594:                         Text(e.tool)
  595:                             .lineLimit(1)
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:22` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   15: 
   16:     var body: some View {
   17:         ScrollView {
   18:             VStack(alignment: .leading, spacing: 18) {
   19:                 header
   20:                 metricsStrip
   21:                 MultiSeriesLoadChart(
   22:                     cpu: model.historyCPU,
   23:                     ram: model.historyRAM,
   24:                     gpu: model.historyGPU
   25:                 )
   26:                     .frame(height: 160)
   27:                     .clipShape(RoundedRectangle(cornerRadius: 12))
   28:                     .overlay(
   29:                         RoundedRectangle(cornerRadius: 12)
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:23` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   16:     var body: some View {
   17:         ScrollView {
   18:             VStack(alignment: .leading, spacing: 18) {
   19:                 header
   20:                 metricsStrip
   21:                 MultiSeriesLoadChart(
   22:                     cpu: model.historyCPU,
   23:                     ram: model.historyRAM,
   24:                     gpu: model.historyGPU
   25:                 )
   26:                     .frame(height: 160)
   27:                     .clipShape(RoundedRectangle(cornerRadius: 12))
   28:                     .overlay(
   29:                         RoundedRectangle(cornerRadius: 12)
   30:                             .stroke(Color.cyan.opacity(0.35), lineWidth: 1)
```

### `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:24` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   17:         ScrollView {
   18:             VStack(alignment: .leading, spacing: 18) {
   19:                 header
   20:                 metricsStrip
   21:                 MultiSeriesLoadChart(
   22:                     cpu: model.historyCPU,
   23:                     ram: model.historyRAM,
   24:                     gpu: model.historyGPU
   25:                 )
   26:                     .frame(height: 160)
   27:                     .clipShape(RoundedRectangle(cornerRadius: 12))
   28:                     .overlay(
   29:                         RoundedRectangle(cornerRadius: 12)
   30:                             .stroke(Color.cyan.opacity(0.35), lineWidth: 1)
   31:                     )
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:224` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  217:                 arguments: [server.baseURL.absoluteString],
  218:                 timeoutSec: 5
  219:             )
  220:         }
  221:         let sem = DispatchSemaphore(value: 0)
  222:         signal(SIGINT, SIG_IGN)
  223:         signal(SIGTERM, SIG_IGN)
  224:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
  225:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
  226:         sigInt.setEventHandler { sem.signal() }
  227:         sigTerm.setEventHandler { sem.signal() }
  228:         sigInt.resume()
  229:         sigTerm.resume()
  230:         sem.wait()
  231:         server.stop()
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:225` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  218:                 timeoutSec: 5
  219:             )
  220:         }
  221:         let sem = DispatchSemaphore(value: 0)
  222:         signal(SIGINT, SIG_IGN)
  223:         signal(SIGTERM, SIG_IGN)
  224:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
  225:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
  226:         sigInt.setEventHandler { sem.signal() }
  227:         sigTerm.setEventHandler { sem.signal() }
  228:         sigInt.resume()
  229:         sigTerm.resume()
  230:         sem.wait()
  231:         server.stop()
  232:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:408` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  401:         let process = Process()
  402:         process.executableURL = URL(fileURLWithPath: exe)
  403:         var childArgs = ["manager", "run"]
  404:         if let home = homeOverride(args) {
  405:             childArgs += ["--home", home.path]
  406:         }
  407:         if args.contains("--open") {
  408:             childArgs.append("--open")
  409:         }
  410:         process.arguments = childArgs
  411: 
  412:         try app.paths.ensureLayout()
  413:         if !FileManager.default.fileExists(atPath: app.paths.managerLog.path) {
  414:             FileManager.default.createFile(atPath: app.paths.managerLog.path, contents: nil)
  415:         }
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:12` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    5: // Why: New agent modules can be added as data without changing framework control flow.
    6: 
    7: import Foundation
    8: 
    9: /// Loads agent playbooks from bundled Resources/Agents and ~/.forge-conductor/agents.
   10: public final class AgentCatalog: AgentCatalogProviding, @unchecked Sendable {
   11:     private let paths: AppPaths
   12:     private var cache: [String: AgentSpec] = [:]
   13:     private let lock = NSLock()
   14: 
   15:     public init(paths: AppPaths) {
   16:         self.paths = paths
   17:         reload()
   18:     }
   19: 
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:72` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   65:                 }
   66:             }
   67:         }
   68:         // Ensure minimal defaults always exist
   69:         for spec in AgentCatalog.builtinDefaults() {
   70:             if map[spec.id] == nil { map[spec.id] = spec }
   71:         }
   72:         cache = map
   73:     }
   74: 
   75:     public func all() -> [AgentSpec] {
   76:         lock.lock(); defer { lock.unlock() }
   77:         return cache.values.sorted { $0.id < $1.id }
   78:     }
   79: 
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:77` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   70:             if map[spec.id] == nil { map[spec.id] = spec }
   71:         }
   72:         cache = map
   73:     }
   74: 
   75:     public func all() -> [AgentSpec] {
   76:         lock.lock(); defer { lock.unlock() }
   77:         return cache.values.sorted { $0.id < $1.id }
   78:     }
   79: 
   80:     public func get(_ id: String) -> AgentSpec? {
   81:         lock.lock(); defer { lock.unlock() }
   82:         return cache[id]
   83:     }
   84: 
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:82` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   75:     public func all() -> [AgentSpec] {
   76:         lock.lock(); defer { lock.unlock() }
   77:         return cache.values.sorted { $0.id < $1.id }
   78:     }
   79: 
   80:     public func get(_ id: String) -> AgentSpec? {
   81:         lock.lock(); defer { lock.unlock() }
   82:         return cache[id]
   83:     }
   84: 
   85:     public func recommend(task: String) -> AgentSpec {
   86:         let t = task.lowercased()
   87:         let rules: [(String, [String])] = [
   88:             ("precommit-audit", ["commit", "precommit", "pull request", "pr ", "ok_to_commit"]),
   89:             ("security", ["security", "auth", "secret", "injection"]),
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:347` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  340:         )
  341:     }
  342: 
  343:     private static func list(_ raw: String?) -> [String] {
  344:         guard let raw, !raw.isEmpty else { return [] }
  345:         // bracket list or comma list
  346:         var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
  347:         if s.hasPrefix("[") && s.hasSuffix("]") {
  348:             s = String(s.dropFirst().dropLast())
  349:         }
  350:         return s.split(separator: ",").map {
  351:             $0.trimmingCharacters(in: .whitespacesAndNewlines)
  352:                 .trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
  353:         }.filter { !$0.isEmpty }
  354:     }
```

### `Sources/ForgeConductorCore/Application/AgentCatalog.swift:377` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  370:         for line in text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init) {
  371:             if line.trimmingCharacters(in: .whitespaces).hasPrefix("#") { continue }
  372:             if let r = line.range(of: ":"), !line.hasPrefix(" ") && !line.hasPrefix("\t") && !line.hasPrefix("-") {
  373:                 flush()
  374:                 let k = String(line[..<r.lowerBound]).trimmingCharacters(in: .whitespaces)
  375:                 var v = String(line[r.upperBound...]).trimmingCharacters(in: .whitespaces)
  376:                 if v.hasPrefix(">") || v.hasPrefix("|") { v = "" }
  377:                 if v.hasPrefix("\"") && v.hasSuffix("\"") && v.count >= 2 {
  378:                     v = String(v.dropFirst().dropLast())
  379:                 }
  380:                 currentKey = k
  381:                 currentVal = v
  382:             } else if currentKey != nil {
  383:                 let t = line.trimmingCharacters(in: .whitespaces)
  384:                 if t.hasPrefix("- ") {
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:85` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   78:         ]
   79:         try store.memorySet(
   80:             key: "agent_run/\(session.id.rawValue)",
   81:             body: try JSONSupport.string(from: runState.compactNSNull()),
   82:             tags: ["agent_run", agentID]
   83:         )
   84: 
   85:         try audit.append(
   86:             tool: "agent_run_start",
   87:             status: "ok",
   88:             clientID: clientID.rawValue,
   89:             args: [
   90:                 "session_id": session.id.rawValue,
   91:                 "agent_id": agentID,
   92:                 "agent_session_id": session.id.rawValue,
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:198` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  191:         if schema.isEmpty, let spec = catalog.get(session.agentID) {
  192:             schema = spec.outputSchema
  193:         }
  194: 
  195:         var missing: [String] = []
  196:         for key in schema {
  197:             let v = reportObj[key]
  198:             if v == nil { missing.append(key); continue }
  199:             if let s = v as? String, s.isEmpty { missing.append(key); continue }
  200:             if let a = v as? [Any], a.isEmpty { missing.append(key); continue }
  201:             if let d = v as? [String: Any], d.isEmpty { missing.append(key); continue }
  202:         }
  203: 
  204:         let summaryObj: [String: Any] = [
  205:             "goal": goal,
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:199` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  192:             schema = spec.outputSchema
  193:         }
  194: 
  195:         var missing: [String] = []
  196:         for key in schema {
  197:             let v = reportObj[key]
  198:             if v == nil { missing.append(key); continue }
  199:             if let s = v as? String, s.isEmpty { missing.append(key); continue }
  200:             if let a = v as? [Any], a.isEmpty { missing.append(key); continue }
  201:             if let d = v as? [String: Any], d.isEmpty { missing.append(key); continue }
  202:         }
  203: 
  204:         let summaryObj: [String: Any] = [
  205:             "goal": goal,
  206:             "report": reportObj,
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:200` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  193:         }
  194: 
  195:         var missing: [String] = []
  196:         for key in schema {
  197:             let v = reportObj[key]
  198:             if v == nil { missing.append(key); continue }
  199:             if let s = v as? String, s.isEmpty { missing.append(key); continue }
  200:             if let a = v as? [Any], a.isEmpty { missing.append(key); continue }
  201:             if let d = v as? [String: Any], d.isEmpty { missing.append(key); continue }
  202:         }
  203: 
  204:         let summaryObj: [String: Any] = [
  205:             "goal": goal,
  206:             "report": reportObj,
  207:             "missing_schema_keys": missing,
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:201` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  194: 
  195:         var missing: [String] = []
  196:         for key in schema {
  197:             let v = reportObj[key]
  198:             if v == nil { missing.append(key); continue }
  199:             if let s = v as? String, s.isEmpty { missing.append(key); continue }
  200:             if let a = v as? [Any], a.isEmpty { missing.append(key); continue }
  201:             if let d = v as? [String: Any], d.isEmpty { missing.append(key); continue }
  202:         }
  203: 
  204:         let summaryObj: [String: Any] = [
  205:             "goal": goal,
  206:             "report": reportObj,
  207:             "missing_schema_keys": missing,
  208:         ]
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:217` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  210:         let closed = try store.sessionEnd(id: sessionID, summary: String(summary.prefix(4000)))
  211: 
  212:         if let clientID {
  213:             try clearBinding(clientID: clientID, sessionID: sessionID)
  214:         }
  215: 
  216:         let status = missing.isEmpty ? "ok" : "warn"
  217:         try audit.append(
  218:             tool: "agent_run_complete",
  219:             status: status,
  220:             clientID: clientID?.rawValue,
  221:             args: [
  222:                 "session_id": sessionID.rawValue,
  223:                 "agent_id": session.agentID,
  224:                 "agent_session_id": sessionID.rawValue,
```

### `Sources/ForgeConductorCore/Application/AgentSessionService.swift:416` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  409:                 ])
  410:                 _ = try store.sessionEnd(id: s.id, summary: summary)
  411:                 diagnostics.warn("agent_session_auto_closed", [
  412:                     "agent_id": s.agentID,
  413:                     "session_id": s.id.rawValue,
  414:                     "age_sec": "\(age)",
  415:                 ])
  416:                 try? audit.append(
  417:                     tool: "agent_session_auto_closed",
  418:                     status: "warn",
  419:                     clientID: s.clientID?.rawValue,
  420:                     args: [
  421:                         "session_id": s.id.rawValue,
  422:                         "agent_id": s.agentID,
  423:                         "age_sec": age,
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:436` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  429:                 cwd = obj["cwd"] as? String
  430:             }
  431:             if goal.isEmpty, let binding = sessions.binding(for: clientID), binding.sessionID == s.id {
  432:                 goal = binding.goal
  433:                 cwd = binding.cwd
  434:             }
  435: 
  436:             snaps.append(
  437:                 AgentContinuitySnapshot(
  438:                     sessionID: s.id.rawValue,
  439:                     agentID: s.agentID,
  440:                     goal: goal,
  441:                     cwd: cwd,
  442:                     status: s.status.rawValue,
  443:                     updatedAt: ISO8601.string(from: s.updatedAt),
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:469` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  462:         var merged: [AgentContinuitySnapshot] = []
  463:         var seen = Set<String>()
  464:         for snapshot in prior where seen.insert(snapshot.sessionID).inserted {
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
  473:         }
  474:         return merged
  475:     }
  476: 
```

### `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:472` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  465:             guard let session = try store.sessionGet(id: SessionID(snapshot.sessionID)),
  466:                   session.status.isOpen else {
  467:                 continue
  468:             }
  469:             merged.append(currentBySession.removeValue(forKey: snapshot.sessionID) ?? snapshot)
  470:         }
  471:         for snapshot in current where seen.insert(snapshot.sessionID).inserted {
  472:             merged.append(snapshot)
  473:         }
  474:         return merged
  475:     }
  476: 
  477:     private struct PersistenceOutcome {
  478:         var packet: HandoffPacket
  479:         var projectionWarning: String?
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:72` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   65:     public func additionalRoots(for clientID: ClientID) -> [URL] {
   66:         lock.lock()
   67:         let implicit = state[clientID.rawValue]?.implicitRoots ?? []
   68:         lock.unlock()
   69: 
   70:         var roots = implicit
   71:         if let binding = sessions.binding(for: clientID), let cwd = binding.cwd, !cwd.isEmpty {
   72:             roots.append(ToolArgHelpers.resolvePath(cwd))
   73:         }
   74:         if let packet = try? store.handoffLatest(clientID: clientID.rawValue)
   75:             ?? store.handoffLatest(resumeReadyOnly: false),
   76:            let cwd = packet.cwd, !cwd.isEmpty {
   77:             roots.append(ToolArgHelpers.resolvePath(cwd))
   78:         }
   79:         if let packet = try? store.handoffLatest(resumeReadyOnly: false) {
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:77` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   70:         var roots = implicit
   71:         if let binding = sessions.binding(for: clientID), let cwd = binding.cwd, !cwd.isEmpty {
   72:             roots.append(ToolArgHelpers.resolvePath(cwd))
   73:         }
   74:         if let packet = try? store.handoffLatest(clientID: clientID.rawValue)
   75:             ?? store.handoffLatest(resumeReadyOnly: false),
   76:            let cwd = packet.cwd, !cwd.isEmpty {
   77:             roots.append(ToolArgHelpers.resolvePath(cwd))
   78:         }
   79:         if let packet = try? store.handoffLatest(resumeReadyOnly: false) {
   80:             for file in packet.keyFiles {
   81:                 let url = ToolArgHelpers.resolvePath(file)
   82:                 var isDir: ObjCBool = false
   83:                 if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) {
   84:                     roots.append(isDir.boolValue ? url : url.deletingLastPathComponent())
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:84` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   77:             roots.append(ToolArgHelpers.resolvePath(cwd))
   78:         }
   79:         if let packet = try? store.handoffLatest(resumeReadyOnly: false) {
   80:             for file in packet.keyFiles {
   81:                 let url = ToolArgHelpers.resolvePath(file)
   82:                 var isDir: ObjCBool = false
   83:                 if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) {
   84:                     roots.append(isDir.boolValue ? url : url.deletingLastPathComponent())
   85:                 }
   86:             }
   87:         }
   88:         return uniqued(roots)
   89:     }
   90: 
   91:     /// Remember a workspace from a loaded handoff packet or an adopted path.
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:129` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  122: 
  123:         guard succeeded, Self.progressTools.contains(tool) else { return nil }
  124: 
  125:         let now = clock.now()
  126:         lock.lock()
  127:         var current = state[clientID.rawValue] ?? ClientState()
  128:         current.progressCount += 1
  129:         current.lastTools.append(tool)
  130:         if current.lastTools.count > 12 {
  131:             current.lastTools.removeFirst(current.lastTools.count - 12)
  132:         }
  133:         if let path = ToolArgHelpers.string(arguments, "path") ?? ToolArgHelpers.string(arguments, "cwd") {
  134:             current.lastPaths.append(path)
  135:             if current.lastPaths.count > 16 {
  136:                 current.lastPaths.removeFirst(current.lastPaths.count - 16)
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:131` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  124: 
  125:         let now = clock.now()
  126:         lock.lock()
  127:         var current = state[clientID.rawValue] ?? ClientState()
  128:         current.progressCount += 1
  129:         current.lastTools.append(tool)
  130:         if current.lastTools.count > 12 {
  131:             current.lastTools.removeFirst(current.lastTools.count - 12)
  132:         }
  133:         if let path = ToolArgHelpers.string(arguments, "path") ?? ToolArgHelpers.string(arguments, "cwd") {
  134:             current.lastPaths.append(path)
  135:             if current.lastPaths.count > 16 {
  136:                 current.lastPaths.removeFirst(current.lastPaths.count - 16)
  137:             }
  138:         }
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:134` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  127:         var current = state[clientID.rawValue] ?? ClientState()
  128:         current.progressCount += 1
  129:         current.lastTools.append(tool)
  130:         if current.lastTools.count > 12 {
  131:             current.lastTools.removeFirst(current.lastTools.count - 12)
  132:         }
  133:         if let path = ToolArgHelpers.string(arguments, "path") ?? ToolArgHelpers.string(arguments, "cwd") {
  134:             current.lastPaths.append(path)
  135:             if current.lastPaths.count > 16 {
  136:                 current.lastPaths.removeFirst(current.lastPaths.count - 16)
  137:             }
  138:         }
  139:         let progress = current.progressCount
  140:         let sinceCheckpoint = progress - current.lastCheckpointCount
  141:         let sinceHandoff = progress - current.lastHandoffCount
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:136` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  129:         current.lastTools.append(tool)
  130:         if current.lastTools.count > 12 {
  131:             current.lastTools.removeFirst(current.lastTools.count - 12)
  132:         }
  133:         if let path = ToolArgHelpers.string(arguments, "path") ?? ToolArgHelpers.string(arguments, "cwd") {
  134:             current.lastPaths.append(path)
  135:             if current.lastPaths.count > 16 {
  136:                 current.lastPaths.removeFirst(current.lastPaths.count - 16)
  137:             }
  138:         }
  139:         let progress = current.progressCount
  140:         let sinceCheckpoint = progress - current.lastCheckpointCount
  141:         let sinceHandoff = progress - current.lastHandoffCount
  142:         let forcePersist = Self.forcePersistTools.contains(tool)
  143:         let checkpointDue = forcePersist
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:262` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  255:             if !binding.goal.isEmpty { args["goal"] = binding.goal }
  256:             if let cwd = binding.cwd, !cwd.isEmpty { args["cwd"] = cwd }
  257:         }
  258:         if args["cwd"] == nil, let first = additionalRoots(for: clientID).first {
  259:             args["cwd"] = first.path
  260:         }
  261:         if !lastPaths.isEmpty {
  262:             args["key_files"] = Array(Set(lastPaths)).sorted().suffix(12).map { $0 }
  263:         }
  264:         let uniqueTools = Array(NSOrderedSet(array: lastTools)) as? [String] ?? lastTools
  265:         args["narrative"] = "Auto-saved after tools: \(uniqueTools.suffix(8).joined(separator: ", "))."
  266:         args["next_actions"] = [
  267:             "Call context_get if this is a new chat",
  268:             "Continue from the workspace in this packet",
  269:         ]
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:265` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  258:         if args["cwd"] == nil, let first = additionalRoots(for: clientID).first {
  259:             args["cwd"] = first.path
  260:         }
  261:         if !lastPaths.isEmpty {
  262:             args["key_files"] = Array(Set(lastPaths)).sorted().suffix(12).map { $0 }
  263:         }
  264:         let uniqueTools = Array(NSOrderedSet(array: lastTools)) as? [String] ?? lastTools
  265:         args["narrative"] = "Auto-saved after tools: \(uniqueTools.suffix(8).joined(separator: ", "))."
  266:         args["next_actions"] = [
  267:             "Call context_get if this is a new chat",
  268:             "Continue from the workspace in this packet",
  269:         ]
  270:         return args
  271:     }
  272: 
```

### `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:287` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  280: 
  281:     private func uniqued(_ urls: [URL]) -> [URL] {
  282:         var seen = Set<String>()
  283:         var out: [URL] = []
  284:         for url in urls {
  285:             let path = url.standardizedFileURL.path
  286:             if seen.insert(path).inserted {
  287:                 out.append(url.standardizedFileURL)
  288:             }
  289:         }
  290:         return out
  291:     }
  292: 
  293:     static let forcePersistTools: Set<String> = [
  294:         "agent_run_start", "agent_run_complete",
```

### `Sources/ForgeConductorCore/Application/ForgeApp.swift:193` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  186: 
  187:     public func doctorModel() throws -> DoctorReport {
  188:         var checks: [DoctorCheck] = []
  189:         var ok = true
  190: 
  191:         func check(_ name: String, _ pass: Bool, _ detail: String, hard: Bool = true) {
  192:             if hard && !pass { ok = false }
  193:             checks.append(DoctorCheck(name: name, ok: pass, detail: detail, hard: hard))
  194:         }
  195: 
  196:         check("home_layout", FileManager.default.fileExists(atPath: paths.home.path), paths.home.path)
  197:         check("sqlite_store", FileManager.default.fileExists(atPath: paths.storeSQLite.path), paths.storeSQLite.path)
  198:         check("agent_catalog", catalog.all().count >= 5, "\(catalog.all().count) agents loaded")
  199:         do {
  200:             _ = try store.sessionList()
```

### `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:156` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  149:                 destinationKey.flatMap { access($0) },
  150:             ].compactMap { $0 }
  151:         case "pdf_write":
  152:             return [access("path")].compactMap { $0 }
  153:         case "pdf_from_file":
  154:             var accesses = [access("source_path")].compactMap { $0 }
  155:             if let destination = access("dest_path") {
  156:                 accesses.append(destination)
  157:             } else if let source = accesses.first {
  158:                 accesses.append(PathAccess(
  159:                     key: "dest_path",
  160:                     url: source.url.deletingPathExtension().appendingPathExtension("pdf"),
  161:                     protectRoot: false
  162:                 ))
  163:             }
```

### `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:158` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  151:         case "pdf_write":
  152:             return [access("path")].compactMap { $0 }
  153:         case "pdf_from_file":
  154:             var accesses = [access("source_path")].compactMap { $0 }
  155:             if let destination = access("dest_path") {
  156:                 accesses.append(destination)
  157:             } else if let source = accesses.first {
  158:                 accesses.append(PathAccess(
  159:                     key: "dest_path",
  160:                     url: source.url.deletingPathExtension().appendingPathExtension("pdf"),
  161:                     protectRoot: false
  162:                 ))
  163:             }
  164:             return accesses
  165:         case "git_status", "git_diff", "git_log", "git_add", "git_commit", "shell_exec":
```

### `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:181` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  174:     private func authorizedRoots(binding: ActiveBinding?, clientID: ClientID) -> [URL] {
  175:         let configured = config.model.allowedRoots.map(ToolArgHelpers.resolvePath)
  176:         let bindingRoot = binding.flatMap(\.cwd).map(ToolArgHelpers.resolvePath)
  177:         let extra = workspace?.additionalRoots(for: clientID) ?? []
  178:         return ([paths.home] + configured + [bindingRoot].compactMap { $0 } + extra)
  179:             .map(canonicalURL)
  180:             .reduce(into: [URL]()) { roots, root in
  181:                 if !roots.contains(root) { roots.append(root) }
  182:             }
  183:     }
  184: 
  185:     /// Read-only access under the interactive user's home, excluding secret/system trees.
  186:     private func isPermittedHomeRead(_ url: URL) -> Bool {
  187:         let home = fileManager.homeDirectoryForCurrentUser.resolvingSymlinksInPath().standardizedFileURL
  188:         guard contains(url, root: home) else { return false }
```

### `Sources/ForgeConductorCore/Application/ToolRouter.swift:293` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  286:         clientID: ClientID,
  287:         start: Date,
  288:         status: String,
  289:         auditError: String?,
  290:         mutating: Bool
  291:     ) -> ToolResult {
  292:         let durationMs = Int(Date().timeIntervalSince(start) * 1000)
  293:         try? app.audit.append(
  294:             tool: tool,
  295:             status: status,
  296:             clientID: clientID.rawValue,
  297:             args: ToolAuditSanitizer.sanitize(arguments),
  298:             durationMs: durationMs,
  299:             error: auditError,
  300:             mutating: mutating
```

### `Sources/ForgeConductorCore/Application/Tools/GitToolPack.swift:28` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   21:         let cwd = ToolArgHelpers.string(arguments, "cwd") ?? FileManager.default.currentDirectoryPath
   22:         var gitArgs: [String] = []
   23:         switch name {
   24:         case "git_status":
   25:             gitArgs = ["status", "--porcelain=v1", "-b"]
   26:         case "git_diff":
   27:             gitArgs = ["diff"]
   28:             if ToolArgHelpers.bool(arguments, "staged") == true { gitArgs.append("--cached") }
   29:         case "git_log":
   30:             let n = ToolArgHelpers.int(arguments, "limit") ?? 20
   31:             gitArgs = ["log", "-n", "\(n)", "--oneline"]
   32:         case "git_add":
   33:             if let path = ToolArgHelpers.string(arguments, "path") { gitArgs = ["add", path] }
   34:             else { gitArgs = ["add", "-A"] }
   35:         case "git_commit":
```

### `Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift:5` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    1: // MemoryToolPack.swift
    2: // What: Exposes durable SQLite-backed memory notes as MCP tools for cross-session continuity.
    3: // How: Validates keys/bodies/tags, delegates persistence to SQLiteStore, and returns stable
    4: // JSON payloads for set/get/list/delete/search without requiring an agent session.
    5: // Why: LM Studio chat history is ephemeral; models need a durable local memory surface.
    6: 
    7: import Foundation
    8: 
    9: /// Durable key/value memory tools backed by `memory_notes` in the Forge home SQLite store.
   10: public struct MemoryToolPack: ToolPackHandling {
   11:     public init() {}
   12: 
```

### `Sources/ForgeConductorCore/Dashboard/DashboardHTML.swift:214` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  207: }
  208: function showSvcWarn(msg) {
  209:   const el = $('svc-warn');
  210:   if (!msg) { el.style.display = 'none'; el.textContent = ''; return; }
  211:   el.style.display = 'block'; el.textContent = msg;
  212: }
  213: async function jget(path) {
  214:   const r = await fetch(path, { cache: 'no-store' });
  215:   const j = await r.json().catch(() => ({}));
  216:   if (!r.ok) {
  217:     const err = new Error(j.message || (path + ' → ' + r.status));
  218:     err.payload = j;
  219:     err.status = r.status;
  220:     throw err;
  221:   }
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:37` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   30: /// Loopback HTTP control surface: status, agents, sessions, audit, diagnostics, manager controls.
   31: /// Routing is delegated to modular route handlers (Telemetry / Manager / Operational).
   32: public final class DashboardServer: @unchecked Sendable {
   33:     private let app: ForgeApp
   34:     private let host: String
   35:     private let port: UInt16
   36:     private var listener: NWListener?
   37:     private let queue = DispatchQueue(label: "forge.dashboard", qos: .userInitiated)
   38:     private let lock = NSLock()
   39:     private let http = HTTPResponder()
   40:     public private(set) var isRunning = false
   41: 
   42:     /// Optional supervisor; when set, manager control APIs are available.
   43:     public weak var manager: ManagerNode?
   44: 
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:130` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  123:                 gate.signal()
  124:             case .cancelled:
  125:                 break
  126:             default:
  127:                 break
  128:             }
  129:         }
  130:         listener.start(queue: queue)
  131:         let wait = gate.wait(timeout: .now() + 3)
  132:         if wait == .timedOut {
  133:             listener.cancel()
  134:             app.diagnostics.error("dashboard_bind_timeout", ["port": "\(port)"], category: .manager)
  135:             throw DashboardError.bindTimeout(port)
  136:         }
  137:         if let bindError = bindResult.recordedError() {
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:161` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  154:     }
  155: 
  156:     /// Run until interrupted (SIGINT/SIGTERM).
  157:     public func runForever() throws {
  158:         try start()
  159:         fputs("Forge-Conductor dashboard: \(baseURL.absoluteString)\n", stderr)
  160:         let sem = DispatchSemaphore(value: 0)
  161:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
  162:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
  163:         signal(SIGINT, SIG_IGN)
  164:         signal(SIGTERM, SIG_IGN)
  165:         sigInt.setEventHandler { sem.signal() }
  166:         sigTerm.setEventHandler { sem.signal() }
  167:         sigInt.resume()
  168:         sigTerm.resume()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:162` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  155: 
  156:     /// Run until interrupted (SIGINT/SIGTERM).
  157:     public func runForever() throws {
  158:         try start()
  159:         fputs("Forge-Conductor dashboard: \(baseURL.absoluteString)\n", stderr)
  160:         let sem = DispatchSemaphore(value: 0)
  161:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
  162:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
  163:         signal(SIGINT, SIG_IGN)
  164:         signal(SIGTERM, SIG_IGN)
  165:         sigInt.setEventHandler { sem.signal() }
  166:         sigTerm.setEventHandler { sem.signal() }
  167:         sigInt.resume()
  168:         sigTerm.resume()
  169:         sem.wait()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:176` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  169:         sem.wait()
  170:         stop()
  171:     }
  172: 
  173:     // MARK: - Connection
  174: 
  175:     private func handle(connection: NWConnection) {
  176:         connection.start(queue: queue)
  177:         receive(on: connection, buffer: Data())
  178:     }
  179: 
  180:     private func receive(on connection: NWConnection, buffer: Data) {
  181:         connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
  182:             guard let self else {
  183:                 connection.cancel()
```

### `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:192` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  185:             }
  186:             if let error {
  187:                 self.app.diagnostics.warn("dashboard_recv_error", ["error": "\(error)"])
  188:                 connection.cancel()
  189:                 return
  190:             }
  191:             var buf = buffer
  192:             if let data { buf.append(data) }
  193:             switch DashboardHTTPRequestParser.parse(buf, streamComplete: isComplete) {
  194:             case .incomplete:
  195:                 self.receive(on: connection, buffer: buf)
  196:             case .rejected(let status, let message):
  197:                 self.http.respond(connection, status: status, body: message, contentType: "text/plain")
  198:             case .request(let request):
  199:                 if let rejection = DashboardRequestPolicy.rejection(for: request, serverPort: self.port) {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:3` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    1: // HTTPResponder.swift
    2: // What: Encodes HTTP responses and retains continuous SSE sessions.
    3: // How: It builds bounded headers/bodies, serializes JSON, and queues streaming frames
    4: // through one send pipeline until the client, timer, or network closes the session.
    5: // Why: Central response mechanics keep every route consistent and prevent stream loss.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Low-level HTTP response writer for the loopback control surface.
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:48` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   41:         contentType: String
   42:     ) {
   43:         let reason = status == 200 ? "OK" : "Error"
   44:         var header = "HTTP/1.1 \(status) \(reason)\r\n"
   45:         header += "Content-Type: \(contentType)\r\n"
   46:         header += "Content-Length: \(data.count)\r\n"
   47:         header += "Connection: close\r\n"
   48:         header += "Cache-Control: no-store\r\n"
   49:         header += securityHeaders
   50:         header += "\r\n"
   51:         var payload = Data(header.utf8)
   52:         payload.append(data)
   53:         connection.send(content: payload, completion: .contentProcessed { _ in
   54:             connection.cancel()
   55:         })
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:52` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   45:         header += "Content-Type: \(contentType)\r\n"
   46:         header += "Content-Length: \(data.count)\r\n"
   47:         header += "Connection: close\r\n"
   48:         header += "Cache-Control: no-store\r\n"
   49:         header += securityHeaders
   50:         header += "\r\n"
   51:         var payload = Data(header.utf8)
   52:         payload.append(data)
   53:         connection.send(content: payload, completion: .contentProcessed { _ in
   54:             connection.cancel()
   55:         })
   56:     }
   57: 
   58:     public func respond(
   59:         _ connection: NWConnection,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:85` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   78:         default: reason = "OK"
   79:         }
   80:         let bodyData = Data(body.utf8)
   81:         var header = "HTTP/1.1 \(status) \(reason)\r\n"
   82:         header += "Content-Type: \(contentType)\r\n"
   83:         header += "Content-Length: \(bodyData.count)\r\n"
   84:         header += "Connection: close\r\n"
   85:         header += "Cache-Control: no-store\r\n"
   86:         header += securityHeaders
   87:         for h in extraHeaders { header += h + "\r\n" }
   88:         header += "\r\n"
   89:         var payload = Data(header.utf8)
   90:         payload.append(bodyData)
   91:         connection.send(content: payload, completion: .contentProcessed { _ in
   92:             connection.cancel()
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:90` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   83:         header += "Content-Length: \(bodyData.count)\r\n"
   84:         header += "Connection: close\r\n"
   85:         header += "Cache-Control: no-store\r\n"
   86:         header += securityHeaders
   87:         for h in extraHeaders { header += h + "\r\n" }
   88:         header += "\r\n"
   89:         var payload = Data(header.utf8)
   90:         payload.append(bodyData)
   91:         connection.send(content: payload, completion: .contentProcessed { _ in
   92:             connection.cancel()
   93:         })
   94:     }
   95: 
   96:     /// Legacy one-shot SSE (compat). Prefer `startRealtimeSSE`.
   97:     public func respondSSE(connection: NWConnection, snapshot: [String: Any]) {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:104` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   97:     public func respondSSE(connection: NWConnection, snapshot: [String: Any]) {
   98:         let json = (try? JSONSupport.string(from: snapshot)) ?? "{}"
   99:         var body = ": connected\n\n"
  100:         body += "data: \(json)\n\n"
  101:         let bodyData = Data(body.utf8)
  102:         var header = "HTTP/1.1 200 OK\r\n"
  103:         header += "Content-Type: text/event-stream; charset=utf-8\r\n"
  104:         header += "Cache-Control: no-cache, no-transform\r\n"
  105:         header += "Connection: close\r\n"
  106:         header += securityHeaders
  107:         header += "Content-Length: \(bodyData.count)\r\n\r\n"
  108:         var payload = Data(header.utf8)
  109:         payload.append(bodyData)
  110:         connection.send(content: payload, completion: .contentProcessed { _ in
  111:             connection.cancel()
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:109` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  102:         var header = "HTTP/1.1 200 OK\r\n"
  103:         header += "Content-Type: text/event-stream; charset=utf-8\r\n"
  104:         header += "Cache-Control: no-cache, no-transform\r\n"
  105:         header += "Connection: close\r\n"
  106:         header += securityHeaders
  107:         header += "Content-Length: \(bodyData.count)\r\n\r\n"
  108:         var payload = Data(header.utf8)
  109:         payload.append(bodyData)
  110:         connection.send(content: payload, completion: .contentProcessed { _ in
  111:             connection.cancel()
  112:         })
  113:     }
  114: 
  115:     private var securityHeaders: String {
  116:         "X-Content-Type-Options: nosniff\r\n"
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:154` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  147: /// **Must be retained** by `HTTPResponder` for the life of the stream.
  148: public final class SSEStreamSession: @unchecked Sendable {
  149:     private let connection: NWConnection
  150:     private let telemetry: TelemetryService
  151:     private weak var responder: HTTPResponder?
  152:     private let periodMs: Int
  153:     private let maxDurationSec: TimeInterval
  154:     private let queue = DispatchQueue(label: "forge.telemetry.sse", qos: .userInitiated)
  155:     private let lock = NSLock()
  156:     private var timer: DispatchSourceTimer?
  157:     private var startedAt = Date()
  158:     private var closed = false
  159:     private var eventCount = 0
  160:     private var sendChain: [(Data, Bool)] = []
  161:     private var sending = false
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:192` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  185:     private func begin() {
  186:         if !telemetry.realtimeEngine.isRunning {
  187:             telemetry.startBackgroundRefresh(intervalSec: 0.5)
  188:         }
  189: 
  190:         var bootstrap = "HTTP/1.1 200 OK\r\n"
  191:         bootstrap += "Content-Type: text/event-stream; charset=utf-8\r\n"
  192:         bootstrap += "Cache-Control: no-cache, no-transform\r\n"
  193:         bootstrap += "Connection: keep-alive\r\n"
  194:         bootstrap += "X-Content-Type-Options: nosniff\r\n"
  195:         bootstrap += "Cross-Origin-Resource-Policy: same-origin\r\n"
  196:         bootstrap += "X-Accel-Buffering: no\r\n"
  197:         bootstrap += "\r\n"
  198:         bootstrap += ": connected realtime\n\n"
  199:         bootstrap += framePayload(full: false)
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:201` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  194:         bootstrap += "X-Content-Type-Options: nosniff\r\n"
  195:         bootstrap += "Cross-Origin-Resource-Policy: same-origin\r\n"
  196:         bootstrap += "X-Accel-Buffering: no\r\n"
  197:         bootstrap += "\r\n"
  198:         bootstrap += ": connected realtime\n\n"
  199:         bootstrap += framePayload(full: false)
  200: 
  201:         enqueue(Data(bootstrap.utf8), countsAsEvent: true)
  202:         queue.async { [weak self] in
  203:             self?.startTimer()
  204:         }
  205:         queue.asyncAfter(deadline: .now() + maxDurationSec) { [weak self] in
  206:             self?.close()
  207:         }
  208:     }
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:202` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  195:         bootstrap += "Cross-Origin-Resource-Policy: same-origin\r\n"
  196:         bootstrap += "X-Accel-Buffering: no\r\n"
  197:         bootstrap += "\r\n"
  198:         bootstrap += ": connected realtime\n\n"
  199:         bootstrap += framePayload(full: false)
  200: 
  201:         enqueue(Data(bootstrap.utf8), countsAsEvent: true)
  202:         queue.async { [weak self] in
  203:             self?.startTimer()
  204:         }
  205:         queue.asyncAfter(deadline: .now() + maxDurationSec) { [weak self] in
  206:             self?.close()
  207:         }
  208:     }
  209: 
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:205` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  198:         bootstrap += ": connected realtime\n\n"
  199:         bootstrap += framePayload(full: false)
  200: 
  201:         enqueue(Data(bootstrap.utf8), countsAsEvent: true)
  202:         queue.async { [weak self] in
  203:             self?.startTimer()
  204:         }
  205:         queue.asyncAfter(deadline: .now() + maxDurationSec) { [weak self] in
  206:             self?.close()
  207:         }
  208:     }
  209: 
  210:     private func startTimer() {
  211:         let t = DispatchSource.makeTimerSource(queue: queue)
  212:         t.schedule(
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:211` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  204:         }
  205:         queue.asyncAfter(deadline: .now() + maxDurationSec) { [weak self] in
  206:             self?.close()
  207:         }
  208:     }
  209: 
  210:     private func startTimer() {
  211:         let t = DispatchSource.makeTimerSource(queue: queue)
  212:         t.schedule(
  213:             deadline: .now() + .milliseconds(periodMs),
  214:             repeating: .milliseconds(periodMs),
  215:             leeway: .milliseconds(max(2, periodMs / 5))
  216:         )
  217:         t.setEventHandler { [weak self] in
  218:             self?.onTick()
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:240` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  233:             lock.unlock()
  234:             close()
  235:             return
  236:         }
  237:         tick += 1
  238:         let full = tick % 10 == 0
  239:         lock.unlock()
  240:         enqueue(Data(framePayload(full: full).utf8), countsAsEvent: true)
  241:     }
  242: 
  243:     private func enqueue(_ data: Data, countsAsEvent: Bool) {
  244:         lock.lock()
  245:         if closed {
  246:             lock.unlock()
  247:             return
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:243` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  236:         }
  237:         tick += 1
  238:         let full = tick % 10 == 0
  239:         lock.unlock()
  240:         enqueue(Data(framePayload(full: full).utf8), countsAsEvent: true)
  241:     }
  242: 
  243:     private func enqueue(_ data: Data, countsAsEvent: Bool) {
  244:         lock.lock()
  245:         if closed {
  246:             lock.unlock()
  247:             return
  248:         }
  249:         sendChain.append((data, countsAsEvent))
  250:         let kick = !sending
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:249` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  242: 
  243:     private func enqueue(_ data: Data, countsAsEvent: Bool) {
  244:         lock.lock()
  245:         if closed {
  246:             lock.unlock()
  247:             return
  248:         }
  249:         sendChain.append((data, countsAsEvent))
  250:         let kick = !sending
  251:         if kick { sending = true }
  252:         lock.unlock()
  253:         if kick { drainSendChain() }
  254:     }
  255: 
  256:     private func drainSendChain() {
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:260` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  253:         if kick { drainSendChain() }
  254:     }
  255: 
  256:     private func drainSendChain() {
  257:         lock.lock()
  258:         if closed {
  259:             sending = false
  260:             sendChain.removeAll()
  261:             lock.unlock()
  262:             return
  263:         }
  264:         guard !sendChain.isEmpty else {
  265:             sending = false
  266:             lock.unlock()
  267:             return
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:269` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  262:             return
  263:         }
  264:         guard !sendChain.isEmpty else {
  265:             sending = false
  266:             lock.unlock()
  267:             return
  268:         }
  269:         let (data, counts) = sendChain.removeFirst()
  270:         if counts { eventCount += 1 }
  271:         lock.unlock()
  272: 
  273:         connection.send(
  274:             content: data,
  275:             contentContext: .defaultStream,
  276:             isComplete: false,
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:280` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  273:         connection.send(
  274:             content: data,
  275:             contentContext: .defaultStream,
  276:             isComplete: false,
  277:             completion: .contentProcessed { [weak self] error in
  278:                 guard let self else { return }
  279:                 if error != nil {
  280:                     self.queue.async { self.close() }
  281:                     return
  282:                 }
  283:                 self.queue.async { self.drainSendChain() }
  284:             }
  285:         )
  286:     }
  287: 
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:283` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  276:             isComplete: false,
  277:             completion: .contentProcessed { [weak self] error in
  278:                 guard let self else { return }
  279:                 if error != nil {
  280:                     self.queue.async { self.close() }
  281:                     return
  282:                 }
  283:                 self.queue.async { self.drainSendChain() }
  284:             }
  285:         )
  286:     }
  287: 
  288:     private func framePayload(full: Bool) -> String {
  289:         let frame = telemetry.currentFrame()
  290:         let system = frame.system
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:310` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  303:                     "cpu": system.cpu.asDictionary(),
  304:                     "ram": system.ram.asDictionary(),
  305:                     "disk_io": system.diskIO.asDictionary(),
  306:                     "gpu": system.gpu.map { $0.asDictionary() },
  307:                     "disk": system.disk.map { $0.asDictionary() },
  308:                     "processes": system.processes.prefix(8).map { $0.asDictionary() },
  309:                 ] as [String: Any],
  310:                 "history": frame.history.suffix(20).map { $0.asDictionary() },
  311:             ]
  312:         }
  313:         dict["stream"] = "realtime"
  314:         dict["sample_hz"] = telemetry.realtimeEngine.measuredSampleHz
  315:         let json = (try? JSONSupport.string(from: dict)) ?? "{}"
  316:         return "event: telemetry\ndata: \(json)\n\n"
  317:     }
```

### `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:328` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  321:         if closed {
  322:             lock.unlock()
  323:             return
  324:         }
  325:         closed = true
  326:         timer?.cancel()
  327:         timer = nil
  328:         sendChain.removeAll()
  329:         sending = false
  330:         lock.unlock()
  331:         responder?.releaseStream(self)
  332:         connection.send(
  333:             content: nil,
  334:             contentContext: .defaultStream,
  335:             isComplete: true,
```

### `Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift:53` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   46:                         "duration_ms": e.durationMs as Any,
   47:                         "error": e.error as Any,
   48:                     ].compactNSNull()
   49:                 },
   50:             ])
   51:         case ("GET", "/api/diagnostics"):
   52:             let text = (try? String(contentsOf: app.paths.agentDiagnostics, encoding: .utf8)) ?? ""
   53:             let lines = text.split(separator: "\n").suffix(100).map(String.init)
   54:             http.respondJSON(connection, status: 200, object: ["ok": true, "lines": Array(lines)])
   55:         case ("POST", "/api/sessions/prune"):
   56:             try app.sessions.pruneStale()
   57:             http.respondJSON(connection, status: 200, object: ["ok": true, "message": "Pruned stale sessions"])
   58:         case ("POST", "/api/sessions/close"):
   59:             let obj = (try? JSONSupport.object(from: body)) ?? [:]
   60:             guard let sid = obj["session_id"] as? String else {
```

### `Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift:3` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    1: // TelemetryRoutes.swift
    2: // What: Adapts telemetry snapshots and live streams to dashboard routes.
    3: // How: It selects typed snapshot/history payloads or starts an SSE session through
    4: // HTTPResponder while leaving sampling ownership with TelemetryService.
    5: // Why: Transport consumers should not create competing telemetry engines.
    6: 
    7: import Foundation
    8: import Network
    9: 
   10: /// Telemetry HTTP routes: health, current frame, system, forge, **continuous SSE stream**, static.
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:206` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  199: 
  200:     public func defaultResumeSeed() -> String {
  201:         var lines: [String] = [
  202:             "Forge Continuity resume (handoff \(id)).",
  203:             "Goal: \(goal.isEmpty ? "(none recorded)" : goal)",
  204:             "Status: \(status)",
  205:         ]
  206:         if let cwd, !cwd.isEmpty { lines.append("cwd: \(cwd)") }
  207:         if let projectSlug, !projectSlug.isEmpty { lines.append("project: \(projectSlug)") }
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:207` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  200:     public func defaultResumeSeed() -> String {
  201:         var lines: [String] = [
  202:             "Forge Continuity resume (handoff \(id)).",
  203:             "Goal: \(goal.isEmpty ? "(none recorded)" : goal)",
  204:             "Status: \(status)",
  205:         ]
  206:         if let cwd, !cwd.isEmpty { lines.append("cwd: \(cwd)") }
  207:         if let projectSlug, !projectSlug.isEmpty { lines.append("project: \(projectSlug)") }
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:209` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  202:             "Forge Continuity resume (handoff \(id)).",
  203:             "Goal: \(goal.isEmpty ? "(none recorded)" : goal)",
  204:             "Status: \(status)",
  205:         ]
  206:         if let cwd, !cwd.isEmpty { lines.append("cwd: \(cwd)") }
  207:         if let projectSlug, !projectSlug.isEmpty { lines.append("project: \(projectSlug)") }
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:210` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  203:             "Goal: \(goal.isEmpty ? "(none recorded)" : goal)",
  204:             "Status: \(status)",
  205:         ]
  206:         if let cwd, !cwd.isEmpty { lines.append("cwd: \(cwd)") }
  207:         if let projectSlug, !projectSlug.isEmpty { lines.append("project: \(projectSlug)") }
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
  217:                 )
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:213` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  206:         if let cwd, !cwd.isEmpty { lines.append("cwd: \(cwd)") }
  207:         if let projectSlug, !projectSlug.isEmpty { lines.append("project: \(projectSlug)") }
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
  217:                 )
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:215` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  208:         if !nextActions.isEmpty {
  209:             lines.append("Next actions:")
  210:             for a in nextActions.prefix(8) { lines.append("- \(a)") }
  211:         }
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
  217:                 )
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
  221:         if !narrative.isEmpty {
  222:             lines.append("Summary: \(narrative.prefix(500))")
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:219` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  212:         if !agents.isEmpty {
  213:             lines.append("Open agents:")
  214:             for a in agents.prefix(8) {
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
  217:                 )
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
  221:         if !narrative.isEmpty {
  222:             lines.append("Summary: \(narrative.prefix(500))")
  223:         }
  224:         lines.append("Continue this packet with handoff_id: \(id) on later checkpoints or handoffs.")
  225:         lines.append("Call context_get for the full structured packet, then continue the task.")
  226:         return lines.joined(separator: "\n")
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:222` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  215:                 lines.append(
  216:                     "- \(a.agentID) session=\(a.sessionID) status=\(a.status) goal=\(a.goal.prefix(80))"
  217:                 )
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
  221:         if !narrative.isEmpty {
  222:             lines.append("Summary: \(narrative.prefix(500))")
  223:         }
  224:         lines.append("Continue this packet with handoff_id: \(id) on later checkpoints or handoffs.")
  225:         lines.append("Call context_get for the full structured packet, then continue the task.")
  226:         return lines.joined(separator: "\n")
  227:     }
  228: 
  229:     public static func fromDictionary(_ root: [String: Any]) -> HandoffPacket? {
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:224` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  217:                 )
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
  221:         if !narrative.isEmpty {
  222:             lines.append("Summary: \(narrative.prefix(500))")
  223:         }
  224:         lines.append("Continue this packet with handoff_id: \(id) on later checkpoints or handoffs.")
  225:         lines.append("Call context_get for the full structured packet, then continue the task.")
  226:         return lines.joined(separator: "\n")
  227:     }
  228: 
  229:     public static func fromDictionary(_ root: [String: Any]) -> HandoffPacket? {
  230:         for key in ["meta", "task", "working_set", "resume"] {
  231:             if root[key] != nil, !(root[key] is [String: Any]) { return nil }
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:225` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  218:             }
  219:             lines.append("Use agent_run_status / agent_run_complete then continue; do not invent session state.")
  220:         }
  221:         if !narrative.isEmpty {
  222:             lines.append("Summary: \(narrative.prefix(500))")
  223:         }
  224:         lines.append("Continue this packet with handoff_id: \(id) on later checkpoints or handoffs.")
  225:         lines.append("Call context_get for the full structured packet, then continue the task.")
  226:         return lines.joined(separator: "\n")
  227:     }
  228: 
  229:     public static func fromDictionary(_ root: [String: Any]) -> HandoffPacket? {
  230:         for key in ["meta", "task", "working_set", "resume"] {
  231:             if root[key] != nil, !(root[key] is [String: Any]) { return nil }
  232:         }
```

### `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:301` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  294:         let sourceRaw = meta["source"] as? String ?? HandoffSource.model.rawValue
  295:         guard let source = HandoffSource(rawValue: sourceRaw) else { return nil }
  296:         if root["agents"] != nil, !(root["agents"] is [[String: Any]]) { return nil }
  297:         let rawAgents = root["agents"] as? [[String: Any]] ?? []
  298:         var agentList: [AgentContinuitySnapshot] = []
  299:         for rawAgent in rawAgents {
  300:             guard let agent = AgentContinuitySnapshot.fromDictionary(rawAgent) else { return nil }
  301:             agentList.append(agent)
  302:         }
  303: 
  304:         let resumeSeed = resume["seed"] as? String ?? ""
  305:         var packet = HandoffPacket(
  306:             id: id,
  307:             schemaVersion: rootVersion ?? metaVersion ?? schemaVersion,
  308:             createdAt: meta["created_at"] as? String ?? ISO8601.string(from: Date()),
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:55` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   48: }
   49: 
   50: /// Reads typed agent sessions without exposing the persistence implementation.
   51: public protocol SessionStore: AnyObject, Sendable {
   52:     func sessionList(agentID: String?, status: SessionStatus?) throws -> [AgentSession]
   53: }
   54: 
   55: /// Reads the bounded audit history consumed by diagnostics and the live feed.
   56: public protocol AuditReading: AnyObject, Sendable {
   57:     func auditRecent(limit: Int) throws -> [AuditEvent]
   58: }
   59: 
   60: // MARK: Catalog & tools
   61: 
   62: /// Resolves agent playbooks and recommends a playbook for a task description.
```

### `Sources/ForgeConductorCore/Domain/Protocols.swift:112` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  105: }
  106: 
  107: /// Collects Forge orchestration state independently from physical host telemetry.
  108: public protocol ForgeMetricsCollecting: Sendable {
  109:     func collect() -> ForgeSnapshot
  110: }
  111: 
  112: /// Supplies cached and forced telemetry snapshots to application and transport layers.
  113: public protocol TelemetryProviding: AnyObject, Sendable {
  114:     /// Current live frame (host from continuous engine + last forge composition).
  115:     /// Not a multi-second poll — call freely; host half is always the latest sample.
  116:     func currentFrame() -> TelemetrySnapshot
  117:     /// Edge compatibility: `force` recomposes forge once; otherwise same as `currentFrame()`.
  118:     func snapshotTyped(force: Bool) throws -> TelemetrySnapshot
  119:     func snapshot(force: Bool) throws -> [String: Any]
```

### `Sources/ForgeConductorCore/Infrastructure/AppPaths.swift:28` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   21:         }
   22:     }
   23: 
   24:     public var storeSQLite: URL { home.appendingPathComponent("store.sqlite") }
   25:     public var auditJSONL: URL { home.appendingPathComponent("audit.jsonl") }
   26:     public var configJSON: URL { home.appendingPathComponent("config.json") }
   27:     public var agentsDir: URL { home.appendingPathComponent("agents", isDirectory: true) }
   28:     public var cacheDir: URL { home.appendingPathComponent("cache", isDirectory: true) }
   29:     public var logsDir: URL { home.appendingPathComponent("logs", isDirectory: true) }
   30:     public var agentDiagnostics: URL { logsDir.appendingPathComponent("agent-diagnostics.jsonl") }
   31:     public var toolDiagnostics: URL { logsDir.appendingPathComponent("tool-diagnostics.jsonl") }
   32:     public var failoverDiagnostics: URL { logsDir.appendingPathComponent("failover-diagnostics.jsonl") }
   33:     /// Master append-only diagnostic stream (JSONL).
   34:     public var masterDiagnostics: URL { logsDir.appendingPathComponent(DiagnosticLog.masterLogName) }
   35:     /// Operator exports (.json / .md).
```

### `Sources/ForgeConductorCore/Infrastructure/AppPaths.swift:54` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   47:     public var memoryNextChat: URL { memoryDir.appendingPathComponent("NEXT-CHAT.md") }
   48:     public var memoryIndex: URL { memoryDir.appendingPathComponent("INDEX.md") }
   49: 
   50:     @discardableResult
   51:     public func ensureLayout() throws -> URL {
   52:         let fm = FileManager.default
   53:         for dir in [
   54:             home, agentsDir, cacheDir, logsDir, dashboardDir, exportsDir,
   55:             memoryDir, memoryHandoffsDir,
   56:             cacheDir.appendingPathComponent("browser", isDirectory: true),
   57:         ] {
   58:             try fm.createDirectory(at: dir, withIntermediateDirectories: true)
   59:         }
   60:         if !fm.fileExists(atPath: configJSON.path) {
   61:             let cfg: [String: Any] = [
```

### `Sources/ForgeConductorCore/Infrastructure/AppPaths.swift:56` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   49: 
   50:     @discardableResult
   51:     public func ensureLayout() throws -> URL {
   52:         let fm = FileManager.default
   53:         for dir in [
   54:             home, agentsDir, cacheDir, logsDir, dashboardDir, exportsDir,
   55:             memoryDir, memoryHandoffsDir,
   56:             cacheDir.appendingPathComponent("browser", isDirectory: true),
   57:         ] {
   58:             try fm.createDirectory(at: dir, withIntermediateDirectories: true)
   59:         }
   60:         if !fm.fileExists(atPath: configJSON.path) {
   61:             let cfg: [String: Any] = [
   62:                 "log_level": "info",
   63:                 "allowed_roots": [] as [String],
```

### `Sources/ForgeConductorCore/Infrastructure/AuditService.swift:74` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   67:         ]
   68:         // strip NSNull-ish
   69:         lineObj = lineObj.compactMapValues { v in
   70:             if v is NSNull { return nil }
   71:             return v
   72:         }
   73:         var data = try JSONSupport.data(from: lineObj)
   74:         data.append(0x0A)
   75:         if !FileManager.default.fileExists(atPath: paths.auditJSONL.path) {
   76:             FileManager.default.createFile(atPath: paths.auditJSONL.path, contents: nil)
   77:         }
   78:         let h = try FileHandle(forWritingTo: paths.auditJSONL)
   79:         defer { try? h.close() }
   80:         try h.seekToEnd()
   81:         try h.write(contentsOf: data)
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:127` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  120:         var holders: [Holder] = []
  121:         for line in text.split(separator: "\n").dropFirst() {
  122:             let parts = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
  123:             guard parts.count >= 2, let pid = Int32(parts[1]) else { continue }
  124:             let cmd = parts[0]
  125:             let isForge = cmd.localizedCaseInsensitiveContains("Forge")
  126:                 || cmd.localizedCaseInsensitiveContains("forge-conductor")
  127:             holders.append(Holder(
  128:                 pid: pid,
  129:                 command: cmd,
  130:                 isForge: isForge,
  131:                 detail: String(line)
  132:             ))
  133:         }
  134:         return holders
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:46` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   39:                 event: record.event,
   40:                 severity: record.severity,
   41:                 role: record.role.isEmpty ? role : record.role,
   42:                 pid: ProcessInfo.processInfo.processIdentifier,
   43:                 category: record.category,
   44:                 fields: record.fields
   45:             )
   46:             ring.append(envelope)
   47:             if ring.count > Self.ringCapacity {
   48:                 ring.removeFirst(ring.count - Self.ringCapacity)
   49:             }
   50: 
   51:             let data = try envelope.jsonLine()
   52:             try append(data, to: paths.masterDiagnostics)
   53:             try append(data, to: paths.toolDiagnostics)
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:48` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   41:                 role: record.role.isEmpty ? role : record.role,
   42:                 pid: ProcessInfo.processInfo.processIdentifier,
   43:                 category: record.category,
   44:                 fields: record.fields
   45:             )
   46:             ring.append(envelope)
   47:             if ring.count > Self.ringCapacity {
   48:                 ring.removeFirst(ring.count - Self.ringCapacity)
   49:             }
   50: 
   51:             let data = try envelope.jsonLine()
   52:             try append(data, to: paths.masterDiagnostics)
   53:             try append(data, to: paths.toolDiagnostics)
   54: 
   55:             if record.event.hasPrefix("agent_") || record.event == "agent_health" {
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:91` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   84: 
   85:     // MARK: - Read
   86: 
   87:     /// Recent records from the in-memory ring (newest last).
   88:     public func recent(limit: Int = 200) -> [DiagnosticEnvelope] {
   89:         lock.lock()
   90:         defer { lock.unlock() }
   91:         return Array(ring.suffix(limit))
   92:     }
   93: 
   94:     /// Load all on-disk master JSONL records (best-effort; large files may be capped).
   95:     public func loadPersisted(maxLines: Int = 50_000) throws -> [DiagnosticEnvelope] {
   96:         let url = paths.masterDiagnostics
   97:         guard FileManager.default.fileExists(atPath: url.path) else { return [] }
   98:         let text = try String(contentsOf: url, encoding: .utf8)
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:101` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   94:     /// Load all on-disk master JSONL records (best-effort; large files may be capped).
   95:     public func loadPersisted(maxLines: Int = 50_000) throws -> [DiagnosticEnvelope] {
   96:         let url = paths.masterDiagnostics
   97:         guard FileManager.default.fileExists(atPath: url.path) else { return [] }
   98:         let text = try String(contentsOf: url, encoding: .utf8)
   99:         var out: [DiagnosticEnvelope] = []
  100:         out.reserveCapacity(min(maxLines, 4_096))
  101:         for line in text.split(whereSeparator: \.isNewline).suffix(maxLines) {
  102:             let s = String(line).trimmingCharacters(in: .whitespacesAndNewlines)
  103:             guard !s.isEmpty, let data = s.data(using: .utf8) else { continue }
  104:             if let env = try? JSONDecoder().decode(DiagnosticEnvelope.self, from: data) {
  105:                 out.append(env)
  106:             }
  107:         }
  108:         return out
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:105` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   98:         let text = try String(contentsOf: url, encoding: .utf8)
   99:         var out: [DiagnosticEnvelope] = []
  100:         out.reserveCapacity(min(maxLines, 4_096))
  101:         for line in text.split(whereSeparator: \.isNewline).suffix(maxLines) {
  102:             let s = String(line).trimmingCharacters(in: .whitespacesAndNewlines)
  103:             guard !s.isEmpty, let data = s.data(using: .utf8) else { continue }
  104:             if let env = try? JSONDecoder().decode(DiagnosticEnvelope.self, from: data) {
  105:                 out.append(env)
  106:             }
  107:         }
  108:         return out
  109:     }
  110: 
  111:     // MARK: - Export
  112: 
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:136` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  129:         var merged = try loadPersisted()
  130:         lock.lock()
  131:         let live = ring
  132:         lock.unlock()
  133:         // Prefer disk order; append any ring entries not already present by (ts,event,pid)
  134:         let seen = Set(merged.map(\.identityKey))
  135:         for e in live where !seen.contains(e.identityKey) {
  136:             merged.append(e)
  137:         }
  138:         merged.sort { $0.ts < $1.ts }
  139: 
  140:         let stamp = ISO8601DateFormatter()
  141:         stamp.formatOptions = [.withInternetDateTime]
  142:         let name = basename ?? "forge-diagnostics-\(Self.fileStamp())"
  143:         let jsonURL = dir.appendingPathComponent("\(name).json")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:224` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  217:     private static func renderMarkdown(
  218:         product: String,
  219:         version: String,
  220:         home: String,
  221:         records: [DiagnosticEnvelope]
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:225` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  218:         product: String,
  219:         version: String,
  220:         home: String,
  221:         records: [DiagnosticEnvelope]
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:226` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  219:         version: String,
  220:         home: String,
  221:         records: [DiagnosticEnvelope]
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:227` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  220:         home: String,
  221:         records: [DiagnosticEnvelope]
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:228` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  221:         records: [DiagnosticEnvelope]
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:229` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  222:     ) -> String {
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:230` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  223:         var lines: [String] = []
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:231` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  224:         lines.append("# \(product) Diagnostic Export")
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:232` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  225:         lines.append("")
  226:         lines.append("- **Version:** \(version)")
  227:         lines.append("- **Home:** `\(home)`")
  228:         lines.append("- **Exported:** \(ISO8601.string(from: Date()))")
  229:         lines.append("- **Records:** \(records.count)")
  230:         lines.append("")
  231:         lines.append("## Summary by severity")
  232:         lines.append("")
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
  239:         for k in bySev.keys.sorted() {
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:240` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  233:         var bySev: [String: Int] = [:]
  234:         var byCat: [String: Int] = [:]
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
  239:         for k in bySev.keys.sorted() {
  240:             lines.append("- \(k): \(bySev[k] ?? 0)")
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:242` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  235:         for r in records {
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
  239:         for k in bySev.keys.sorted() {
  240:             lines.append("- \(k): \(bySev[k] ?? 0)")
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:243` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  236:             bySev[r.severity.rawValue, default: 0] += 1
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
  239:         for k in bySev.keys.sorted() {
  240:             lines.append("- \(k): \(bySev[k] ?? 0)")
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:244` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  237:             byCat[r.category.rawValue, default: 0] += 1
  238:         }
  239:         for k in bySev.keys.sorted() {
  240:             lines.append("- \(k): \(bySev[k] ?? 0)")
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:246` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  239:         for k in bySev.keys.sorted() {
  240:             lines.append("- \(k): \(bySev[k] ?? 0)")
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:248` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  241:         }
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:249` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  242:         lines.append("")
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:250` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  243:         lines.append("## Summary by category")
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
  257:                 .joined(separator: "; ")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:251` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  244:         lines.append("")
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
  257:                 .joined(separator: "; ")
  258:                 .replacingOccurrences(of: "|", with: "\\|")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:252` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  245:         for k in byCat.keys.sorted() {
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
  257:                 .joined(separator: "; ")
  258:                 .replacingOccurrences(of: "|", with: "\\|")
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:253` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  246:             lines.append("- \(k): \(byCat[k] ?? 0)")
  247:         }
  248:         lines.append("")
  249:         lines.append("## Timeline")
  250:         lines.append("")
  251:         lines.append("| Time (UTC) | Severity | Category | Event | Fields |")
  252:         lines.append("|---|---|---|---|---|")
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
  257:                 .joined(separator: "; ")
  258:                 .replacingOccurrences(of: "|", with: "\\|")
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
  260:             lines.append(
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:260` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  253:         for r in records.suffix(2_000) {
  254:             let fields = r.fields
  255:                 .map { "\($0.key)=\($0.value)" }
  256:                 .sorted()
  257:                 .joined(separator: "; ")
  258:                 .replacingOccurrences(of: "|", with: "\\|")
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
  260:             lines.append(
  261:                 "| \(r.tsISO) | \(r.severity.rawValue) | \(r.category.rawValue) | \(event) | \(fields) |"
  262:             )
  263:         }
  264:         lines.append("")
  265:         lines.append("_End of export._")
  266:         lines.append("")
  267:         return lines.joined(separator: "\n")
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:264` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  257:                 .joined(separator: "; ")
  258:                 .replacingOccurrences(of: "|", with: "\\|")
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
  260:             lines.append(
  261:                 "| \(r.tsISO) | \(r.severity.rawValue) | \(r.category.rawValue) | \(event) | \(fields) |"
  262:             )
  263:         }
  264:         lines.append("")
  265:         lines.append("_End of export._")
  266:         lines.append("")
  267:         return lines.joined(separator: "\n")
  268:     }
  269: }
  270: 
  271: // MARK: - Models
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:265` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  258:                 .replacingOccurrences(of: "|", with: "\\|")
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
  260:             lines.append(
  261:                 "| \(r.tsISO) | \(r.severity.rawValue) | \(r.category.rawValue) | \(event) | \(fields) |"
  262:             )
  263:         }
  264:         lines.append("")
  265:         lines.append("_End of export._")
  266:         lines.append("")
  267:         return lines.joined(separator: "\n")
  268:     }
  269: }
  270: 
  271: // MARK: - Models
  272: 
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:266` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  259:             let event = r.event.replacingOccurrences(of: "|", with: "\\|")
  260:             lines.append(
  261:                 "| \(r.tsISO) | \(r.severity.rawValue) | \(r.category.rawValue) | \(event) | \(fields) |"
  262:             )
  263:         }
  264:         lines.append("")
  265:         lines.append("_End of export._")
  266:         lines.append("")
  267:         return lines.joined(separator: "\n")
  268:     }
  269: }
  270: 
  271: // MARK: - Models
  272: 
  273: public enum DiagnosticCategory: String, Sendable, Codable, CaseIterable {
```

### `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:328` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  321:             obj["fields"] = fields
  322:         }
  323:         return obj
  324:     }
  325: 
  326:     public func jsonLine() throws -> Data {
  327:         var data = try JSONSerialization.data(withJSONObject: asDictionary(), options: [.sortedKeys])
  328:         data.append(0x0A)
  329:         return data
  330:     }
  331: 
  332:     enum CodingKeys: String, CodingKey {
  333:         case ts, event, severity, role, pid, category, fields
  334:     }
  335: 
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:28` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   21: 
   22:         let rows = layoutLines(content)
   23:         var pages: [String] = []
   24:         var y = topY
   25:         var stream: [String] = []
   26: 
   27:         func emit(_ yy: Int, _ size: Int, _ text: String) {
   28:             stream.append("BT")
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:29` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   22:         let rows = layoutLines(content)
   23:         var pages: [String] = []
   24:         var y = topY
   25:         var stream: [String] = []
   26: 
   27:         func emit(_ yy: Int, _ size: Int, _ text: String) {
   28:             stream.append("BT")
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
   36:             pages.append(body)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:30` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   23:         var pages: [String] = []
   24:         var y = topY
   25:         var stream: [String] = []
   26: 
   27:         func emit(_ yy: Int, _ size: Int, _ text: String) {
   28:             stream.append("BT")
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
   36:             pages.append(body)
   37:             stream = []
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:31` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   24:         var y = topY
   25:         var stream: [String] = []
   26: 
   27:         func emit(_ yy: Int, _ size: Int, _ text: String) {
   28:             stream.append("BT")
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
   36:             pages.append(body)
   37:             stream = []
   38:             y = topY
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:32` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   25:         var stream: [String] = []
   26: 
   27:         func emit(_ yy: Int, _ size: Int, _ text: String) {
   28:             stream.append("BT")
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
   36:             pages.append(body)
   37:             stream = []
   38:             y = topY
   39:         }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:36` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   29:             stream.append("/F1 \(size) Tf")
   30:             stream.append("\(marginX) \(yy) Td")
   31:             stream.append("(\(escape(text))) Tj")
   32:             stream.append("ET")
   33:         }
   34:         func flush() {
   35:             let body = stream.isEmpty ? "BT /F1 11 Tf 50 750 Td ( ) Tj ET" : stream.joined(separator: "\n")
   36:             pages.append(body)
   37:             stream = []
   38:             y = topY
   39:         }
   40: 
   41:         if !title.isEmpty {
   42:             emit(y, 18, String(title.prefix(120)))
   43:             y -= 22
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:79` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   72:             y -= lineH + (style == "h1" ? 4 : 0)
   73:         }
   74:         if !stream.isEmpty || pages.isEmpty { flush() }
   75: 
   76:         // Build PDF objects
   77:         var objects: [Data] = []
   78:         func add(_ s: String) -> Int {
   79:             objects.append(Data(s.utf8))
   80:             return objects.count
   81:         }
   82:         func addData(_ d: Data) -> Int {
   83:             objects.append(d)
   84:             return objects.count
   85:         }
   86: 
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:83` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   76:         // Build PDF objects
   77:         var objects: [Data] = []
   78:         func add(_ s: String) -> Int {
   79:             objects.append(Data(s.utf8))
   80:             return objects.count
   81:         }
   82:         func addData(_ d: Data) -> Int {
   83:             objects.append(d)
   84:             return objects.count
   85:         }
   86: 
   87:         _ = add("<< /Type /Catalog /Pages 2 0 R >>")
   88:         objects.append(Data()) // placeholder pages obj index 1
   89:         let fontID = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
   90: 
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:88` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   81:         }
   82:         func addData(_ d: Data) -> Int {
   83:             objects.append(d)
   84:             return objects.count
   85:         }
   86: 
   87:         _ = add("<< /Type /Catalog /Pages 2 0 R >>")
   88:         objects.append(Data()) // placeholder pages obj index 1
   89:         let fontID = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
   90: 
   91:         var pageIDs: [Int] = []
   92:         for streamBody in pages {
   93:             let raw = Data(streamBody.utf8)
   94:             var content = Data()
   95:             content.append(contentsOf: "<< /Length \(raw.count) >>\nstream\n".utf8)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:95` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   88:         objects.append(Data()) // placeholder pages obj index 1
   89:         let fontID = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
   90: 
   91:         var pageIDs: [Int] = []
   92:         for streamBody in pages {
   93:             let raw = Data(streamBody.utf8)
   94:             var content = Data()
   95:             content.append(contentsOf: "<< /Length \(raw.count) >>\nstream\n".utf8)
   96:             content.append(raw)
   97:             content.append(contentsOf: "\nendstream".utf8)
   98:             let contentID = addData(content)
   99:             let pageID = add(
  100:                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 \(pageW) \(pageH)] "
  101:                     + "/Contents \(contentID) 0 R /Resources << /Font << /F1 \(fontID) 0 R >> >> >>"
  102:             )
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:96` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   89:         let fontID = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
   90: 
   91:         var pageIDs: [Int] = []
   92:         for streamBody in pages {
   93:             let raw = Data(streamBody.utf8)
   94:             var content = Data()
   95:             content.append(contentsOf: "<< /Length \(raw.count) >>\nstream\n".utf8)
   96:             content.append(raw)
   97:             content.append(contentsOf: "\nendstream".utf8)
   98:             let contentID = addData(content)
   99:             let pageID = add(
  100:                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 \(pageW) \(pageH)] "
  101:                     + "/Contents \(contentID) 0 R /Resources << /Font << /F1 \(fontID) 0 R >> >> >>"
  102:             )
  103:             pageIDs.append(pageID)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:97` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   90: 
   91:         var pageIDs: [Int] = []
   92:         for streamBody in pages {
   93:             let raw = Data(streamBody.utf8)
   94:             var content = Data()
   95:             content.append(contentsOf: "<< /Length \(raw.count) >>\nstream\n".utf8)
   96:             content.append(raw)
   97:             content.append(contentsOf: "\nendstream".utf8)
   98:             let contentID = addData(content)
   99:             let pageID = add(
  100:                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 \(pageW) \(pageH)] "
  101:                     + "/Contents \(contentID) 0 R /Resources << /Font << /F1 \(fontID) 0 R >> >> >>"
  102:             )
  103:             pageIDs.append(pageID)
  104:         }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:103` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   96:             content.append(raw)
   97:             content.append(contentsOf: "\nendstream".utf8)
   98:             let contentID = addData(content)
   99:             let pageID = add(
  100:                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 \(pageW) \(pageH)] "
  101:                     + "/Contents \(contentID) 0 R /Resources << /Font << /F1 \(fontID) 0 R >> >> >>"
  102:             )
  103:             pageIDs.append(pageID)
  104:         }
  105:         let kids = pageIDs.map { "\($0) 0 R" }.joined(separator: " ")
  106:         objects[1] = Data("<< /Type /Pages /Kids [ \(kids) ] /Count \(pageIDs.count) >>".utf8)
  107: 
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:109` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  102:             )
  103:             pageIDs.append(pageID)
  104:         }
  105:         let kids = pageIDs.map { "\($0) 0 R" }.joined(separator: " ")
  106:         objects[1] = Data("<< /Type /Pages /Kids [ \(kids) ] /Count \(pageIDs.count) >>".utf8)
  107: 
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:112` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  105:         let kids = pageIDs.map { "\($0) 0 R" }.joined(separator: " ")
  106:         objects[1] = Data("<< /Type /Pages /Kids [ \(kids) ] /Count \(pageIDs.count) >>".utf8)
  107: 
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:113` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  106:         objects[1] = Data("<< /Type /Pages /Kids [ \(kids) ] /Count \(pageIDs.count) >>".utf8)
  107: 
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:114` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  107: 
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:115` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  108:         var buf = Data("%PDF-1.4\n".utf8)
  109:         buf.append(contentsOf: [0x25, 0xE2, 0xE3, 0xCF, 0xD3, 0x0A])
  110:         var offsets: [Int] = [0]
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
  122:         }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:118` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  111:         for (i, obj) in objects.enumerated() {
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
  122:         }
  123:         buf.append(contentsOf: """
  124:         trailer
  125:         << /Size \(objects.count + 1) /Root 1 0 R >>
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:119` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  112:             offsets.append(buf.count)
  113:             buf.append(contentsOf: "\(i + 1) 0 obj\n".utf8)
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
  122:         }
  123:         buf.append(contentsOf: """
  124:         trailer
  125:         << /Size \(objects.count + 1) /Root 1 0 R >>
  126:         startxref
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:121` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  114:             buf.append(obj)
  115:             buf.append(contentsOf: "\nendobj\n".utf8)
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
  122:         }
  123:         buf.append(contentsOf: """
  124:         trailer
  125:         << /Size \(objects.count + 1) /Root 1 0 R >>
  126:         startxref
  127:         \(xref)
  128:         %%EOF
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:123` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  116:         }
  117:         let xref = buf.count
  118:         buf.append(contentsOf: "xref\n0 \(objects.count + 1)\n".utf8)
  119:         buf.append(contentsOf: "0000000000 65535 f \n".utf8)
  120:         for off in offsets.dropFirst() {
  121:             buf.append(contentsOf: String(format: "%010d 00000 n \n", off).utf8)
  122:         }
  123:         buf.append(contentsOf: """
  124:         trailer
  125:         << /Size \(objects.count + 1) /Root 1 0 R >>
  126:         startxref
  127:         \(xref)
  128:         %%EOF
  129: 
  130:         """.utf8)
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:158` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  151:     private static func layoutLines(_ content: String) -> [(String, String)] {
  152:         var rows: [(String, String)] = []
  153:         var inCode = false
  154:         for raw in content.split(separator: "\n", omittingEmptySubsequences: false).map(String.init) {
  155:             let line = raw
  156:             if line.trimmingCharacters(in: .whitespaces).hasPrefix("```") {
  157:                 inCode.toggle()
  158:                 rows.append(("blank", ""))
  159:                 continue
  160:             }
  161:             if inCode {
  162:                 var t = line.replacingOccurrences(of: "\t", with: "    ")
  163:                 while t.count > 92 {
  164:                     rows.append(("code", String(t.prefix(92))))
  165:                     t = String(t.dropFirst(92))
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:164` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  157:                 inCode.toggle()
  158:                 rows.append(("blank", ""))
  159:                 continue
  160:             }
  161:             if inCode {
  162:                 var t = line.replacingOccurrences(of: "\t", with: "    ")
  163:                 while t.count > 92 {
  164:                     rows.append(("code", String(t.prefix(92))))
  165:                     t = String(t.dropFirst(92))
  166:                 }
  167:                 rows.append(("code", t))
  168:                 continue
  169:             }
  170:             let trimmed = line.trimmingCharacters(in: .whitespaces)
  171:             if trimmed.isEmpty {
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:167` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  160:             }
  161:             if inCode {
  162:                 var t = line.replacingOccurrences(of: "\t", with: "    ")
  163:                 while t.count > 92 {
  164:                     rows.append(("code", String(t.prefix(92))))
  165:                     t = String(t.dropFirst(92))
  166:                 }
  167:                 rows.append(("code", t))
  168:                 continue
  169:             }
  170:             let trimmed = line.trimmingCharacters(in: .whitespaces)
  171:             if trimmed.isEmpty {
  172:                 rows.append(("blank", ""))
  173:                 continue
  174:             }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:172` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  165:                     t = String(t.dropFirst(92))
  166:                 }
  167:                 rows.append(("code", t))
  168:                 continue
  169:             }
  170:             let trimmed = line.trimmingCharacters(in: .whitespaces)
  171:             if trimmed.isEmpty {
  172:                 rows.append(("blank", ""))
  173:                 continue
  174:             }
  175:             var style = "normal"
  176:             var text = trimmed
  177:             if trimmed.hasPrefix("### ") { style = "h3"; text = String(trimmed.dropFirst(4)) }
  178:             else if trimmed.hasPrefix("## ") { style = "h2"; text = String(trimmed.dropFirst(3)) }
  179:             else if trimmed.hasPrefix("# ") { style = "h1"; text = String(trimmed.dropFirst(2)).uppercased() }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:189` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  182:                 text = "• " + String(trimmed.dropFirst(2))
  183:             }
  184:             text = text.replacingOccurrences(of: "*", with: "")
  185:                 .replacingOccurrences(of: "_", with: "")
  186:                 .replacingOccurrences(of: "`", with: "")
  187:             let width = (style == "h1" || style == "h2" || style == "h3") ? 88 : 92
  188:             for part in wrap(text, width: width) {
  189:                 rows.append((style, part))
  190:             }
  191:         }
  192:         if rows.isEmpty { rows.append(("normal", "(empty document)")) }
  193:         return rows
  194:     }
  195: 
  196:     private static func wrap(_ text: String, width: Int) -> [String] {
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:192` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  185:                 .replacingOccurrences(of: "_", with: "")
  186:                 .replacingOccurrences(of: "`", with: "")
  187:             let width = (style == "h1" || style == "h2" || style == "h3") ? 88 : 92
  188:             for part in wrap(text, width: width) {
  189:                 rows.append((style, part))
  190:             }
  191:         }
  192:         if rows.isEmpty { rows.append(("normal", "(empty document)")) }
  193:         return rows
  194:     }
  195: 
  196:     private static func wrap(_ text: String, width: Int) -> [String] {
  197:         let words = text.split(separator: " ").map(String.init)
  198:         guard !words.isEmpty else { return [""] }
  199:         var lines: [String] = []
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:205` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  198:         guard !words.isEmpty else { return [""] }
  199:         var lines: [String] = []
  200:         var cur: [String] = []
  201:         var n = 0
  202:         for w in words {
  203:             let add = w.count + (cur.isEmpty ? 0 : 1)
  204:             if n + add > width, !cur.isEmpty {
  205:                 lines.append(cur.joined(separator: " "))
  206:                 cur = [w]
  207:                 n = w.count
  208:             } else {
  209:                 cur.append(w)
  210:                 n += add
  211:             }
  212:         }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:209` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  202:         for w in words {
  203:             let add = w.count + (cur.isEmpty ? 0 : 1)
  204:             if n + add > width, !cur.isEmpty {
  205:                 lines.append(cur.joined(separator: " "))
  206:                 cur = [w]
  207:                 n = w.count
  208:             } else {
  209:                 cur.append(w)
  210:                 n += add
  211:             }
  212:         }
  213:         if !cur.isEmpty { lines.append(cur.joined(separator: " ")) }
  214:         return lines
  215:     }
  216: }
```

### `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:213` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  206:                 cur = [w]
  207:                 n = w.count
  208:             } else {
  209:                 cur.append(w)
  210:                 n += add
  211:             }
  212:         }
  213:         if !cur.isEmpty { lines.append(cur.joined(separator: " ")) }
  214:         return lines
  215:     }
  216: }
```

### `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:215` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  208:                     }
  209:                     return
  210:                 }
  211:             }
  212: 
  213:             private func appendLocked(_ chunk: Data) {
  214:                 let remaining = max(0, limit - data.count)
  215:                 if remaining > 0 { data.append(chunk.prefix(remaining)) }
  216:                 if chunk.count > remaining { truncated = true }
  217:             }
  218: 
  219:             private func shouldContinueNativeDrain() -> Bool {
  220:                 condition.lock()
  221:                 defer { condition.unlock() }
  222:                 return !truncated
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:279` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  272:     }
  273: 
  274:     public func handoffLatest(
  275:         resumeReadyOnly: Bool = false,
  276:         clientID: String? = nil
  277:     ) throws -> HandoffPacket? {
  278:         var predicates: [String] = []
  279:         if resumeReadyOnly { predicates.append("resume_ready = 1") }
  280:         if clientID != nil { predicates.append("client_id = ?") }
  281:         let whereClause = predicates.isEmpty ? "" : " WHERE \(predicates.joined(separator: " AND "))"
  282:         let sql = "SELECT id, packet_json FROM context_handoffs\(whereClause) ORDER BY write_sequence DESC LIMIT 1"
  283:         return try withStatement(sql) { stmt in
  284:             if let clientID { bind(stmt, 1, clientID) }
  285:             guard sqlite3_step(stmt) == SQLITE_ROW,
  286:                   let rowID = textCol(stmt, 0),
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:280` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  273: 
  274:     public func handoffLatest(
  275:         resumeReadyOnly: Bool = false,
  276:         clientID: String? = nil
  277:     ) throws -> HandoffPacket? {
  278:         var predicates: [String] = []
  279:         if resumeReadyOnly { predicates.append("resume_ready = 1") }
  280:         if clientID != nil { predicates.append("client_id = ?") }
  281:         let whereClause = predicates.isEmpty ? "" : " WHERE \(predicates.joined(separator: " AND "))"
  282:         let sql = "SELECT id, packet_json FROM context_handoffs\(whereClause) ORDER BY write_sequence DESC LIMIT 1"
  283:         return try withStatement(sql) { stmt in
  284:             if let clientID { bind(stmt, 1, clientID) }
  285:             guard sqlite3_step(stmt) == SQLITE_ROW,
  286:                   let rowID = textCol(stmt, 0),
  287:                   let cstr = sqlite3_column_text(stmt, 1) else { return nil }
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:317` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  310:                 guard let rowID = textCol(stmt, 0),
  311:                       let cstr = sqlite3_column_text(stmt, 1) else { continue }
  312:                 let text = String(cString: cstr)
  313:                 guard let data = text.data(using: .utf8),
  314:                       let obj = try? JSONSupport.object(from: data),
  315:                       let packet = HandoffPacket.fromDictionary(obj),
  316:                       packet.id == rowID else { continue }
  317:                 out.append(packet)
  318:             }
  319:             return out
  320:         }
  321:     }
  322: 
  323:     /// Rebuilds pointer notes from authoritative handoff rows. Used at bootstrap
  324:     /// after legacy migration or recovery from an interrupted older-version write.
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:533` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  526:         sql += " ORDER BY created_at DESC"
  527:         return try withStatement(sql) { stmt in
  528:             var i: Int32 = 1
  529:             if let agentID { bind(stmt, i, agentID); i += 1 }
  530:             if let status { bind(stmt, i, status.rawValue); i += 1 }
  531:             var out: [AgentSession] = []
  532:             while sqlite3_step(stmt) == SQLITE_ROW {
  533:                 out.append(mapSession(stmt))
  534:             }
  535:             return out
  536:         }
  537:     }
  538: 
  539:     public func sessionCloseOpen(
  540:         for clientID: ClientID,
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:551` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  544:         var closed: [AgentSession] = []
  545:         var seen = Set<String>()
  546:         for st in [SessionStatus.open, .active, .running, .started] {
  547:             for s in try sessionList(status: st) where s.clientID == clientID {
  548:                 if seen.contains(s.id.rawValue) { continue }
  549:                 seen.insert(s.id.rawValue)
  550:                 if let except, s.id == except { continue }
  551:                 closed.append(try sessionEnd(id: s.id, summary: summary))
  552:             }
  553:         }
  554:         return closed
  555:     }
  556: 
  557:     // MARK: - Memory notes
  558: 
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:657` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  650:                 i += 1
  651:             }
  652:             sqlite3_bind_int(stmt, i, Int32(capped))
  653:             var out: [MemoryNote] = []
  654:             while sqlite3_step(stmt) == SQLITE_ROW {
  655:                 let note = mapMemoryNote(stmt)
  656:                 if let tag, !note.tags.contains(tag) { continue }
  657:                 out.append(note)
  658:             }
  659:             return out
  660:         }
  661:     }
  662: 
  663:     /// Case-insensitive substring search over key, body, and tags_json.
  664:     public func memorySearch(
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:693` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  686:         return try withStatement(sql) { stmt in
  687:             bind(stmt, 1, pattern)
  688:             bind(stmt, 2, pattern)
  689:             bind(stmt, 3, pattern)
  690:             sqlite3_bind_int(stmt, 4, Int32(capped))
  691:             var out: [MemoryNote] = []
  692:             while sqlite3_step(stmt) == SQLITE_ROW {
  693:                 out.append(mapMemoryNote(stmt))
  694:             }
  695:             return out
  696:         }
  697:     }
  698: 
  699:     public func memoryCount(includeSystem: Bool = false) throws -> Int {
  700:         var sql = "SELECT COUNT(*) FROM memory_notes WHERE 1=1"
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:779` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  772:                 let client: String? = textCol(stmt, 1)
  773:                 let tool = String(cString: sqlite3_column_text(stmt, 2))
  774:                 let digest = textCol(stmt, 3)
  775:                 let args = textCol(stmt, 4)
  776:                 let status = textCol(stmt, 5) ?? "ok"
  777:                 let ms: Int? = sqlite3_column_type(stmt, 6) == SQLITE_NULL ? nil : Int(sqlite3_column_int(stmt, 6))
  778:                 let err = textCol(stmt, 7)
  779:                 out.append(AuditEvent(
  780:                     timestamp: ISO8601.date(from: ts) ?? Date(),
  781:                     clientID: client,
  782:                     tool: tool,
  783:                     argsDigest: digest,
  784:                     argsJSON: args,
  785:                     status: status,
  786:                     durationMs: ms,
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:821` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  814: 
  815:     public func presenceRecords() throws -> [PresenceRecord] {
  816:         try withStatement(
  817:             "SELECT client_id, host_kind, pid, cwd, last_heartbeat FROM presence ORDER BY last_heartbeat DESC"
  818:         ) { stmt in
  819:             var out: [PresenceRecord] = []
  820:             while sqlite3_step(stmt) == SQLITE_ROW {
  821:                 out.append(PresenceRecord(
  822:                     clientID: String(cString: sqlite3_column_text(stmt, 0)),
  823:                     hostKind: textCol(stmt, 1) ?? "",
  824:                     pid: sqlite3_column_int(stmt, 2),
  825:                     cwd: textCol(stmt, 3) ?? "",
  826:                     lastHeartbeat: textCol(stmt, 4) ?? ""
  827:                 ))
  828:             }
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:227` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  220:         return nil
  221:     }
  222: 
  223:     private static func decodeFrames(_ data: Data) -> [[String: Any]] {
  224:         var messages: [[String: Any]] = []
  225:         for line in data.split(separator: 0x0A, omittingEmptySubsequences: true) {
  226:             if !line.isEmpty, let object = try? JSONSupport.object(from: line) {
  227:                 messages.append(object)
  228:             }
  229:         }
  230:         return messages
  231:     }
  232: 
  233:     private static func isValidNDJSON(_ data: Data) -> Bool {
  234:         let lines = data.split(separator: 0x0A, omittingEmptySubsequences: true)
```

### `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:259` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  252:         let descriptor = handle.fileDescriptor
  253:         var buffer = [UInt8](repeating: 0, count: min(16_384, limit - data.count))
  254:         while !buffer.isEmpty {
  255:             let count = buffer.withUnsafeMutableBytes { bytes in
  256:                 Darwin.read(descriptor, bytes.baseAddress, bytes.count)
  257:             }
  258:             if count > 0 {
  259:                 data.append(buffer, count: count)
  260:                 if data.count >= limit { return }
  261:                 buffer = [UInt8](repeating: 0, count: min(16_384, limit - data.count))
  262:                 continue
  263:             }
  264:             if count < 0, errno == EINTR { continue }
  265:             return
  266:         }
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:50` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   43:             "deployment_id": deploymentID,
   44:         ])
   45:         // Best-effort presence; never block MCP handshake on a locked GUI store.
   46:         refreshPresence()
   47: 
   48:         // Idle stdio sessions receive no host messages. Heartbeat on a timer so
   49:         // the dashboard does not treat a live-but-quiet serve as gone.
   50:         let heartbeat = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "forge.mcp.presence"))
   51:         heartbeat.schedule(deadline: .now() + 10, repeating: 10)
   52:         heartbeat.setEventHandler { [weak self] in
   53:             self?.refreshPresence()
   54:         }
   55:         heartbeat.resume()
   56:         defer { heartbeat.cancel() }
   57: 
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:474` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  467: }
  468: 
  469: /// MCP stdio wire encoder. The specification requires one compact JSON-RPC
  470: /// message per line; Content-Length headers belong to LSP, not MCP stdio.
  471: public enum MCPStdioTransport {
  472:     public static func encode(_ object: [String: Any]) throws -> Data {
  473:         var data = try JSONSupport.data(from: object)
  474:         data.append(0x0A)
  475:         return data
  476:     }
  477: }
  478: 
  479: // MARK: - Stream reader
  480: 
  481: public final class MCPStreamReader {
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:506` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  499:                 if buffer.isEmpty { return nil }
  500:                 // try parse remaining as NDJSON line
  501:                 if let msg = try extractMessage(forceLine: true) {
  502:                     return msg
  503:                 }
  504:                 return nil
  505:             }
  506:             buffer.append(chunk)
  507:             if buffer.count > maximumMessageBytes {
  508:                 throw MCPStreamError.messageTooLarge(maximumMessageBytes)
  509:             }
  510:         }
  511:     }
  512: 
  513:     private func extractMessage(forceLine: Bool = false) throws -> [String: Any]? {
```

### `Sources/ForgeConductorCore/MCP/MCPServer.swift:548` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  541:             if line.isEmpty || line == Data([0x0D]) { return try extractMessage(forceLine: forceLine) }
  542:             let trimmed = line.drop(while: { $0 == 0x0D })
  543:             if trimmed.isEmpty { return try extractMessage(forceLine: forceLine) }
  544:             return try JSONSupport.object(from: Data(trimmed))
  545:         }
  546:         if forceLine, !buffer.isEmpty {
  547:             let body = buffer
  548:             buffer.removeAll()
  549:             return try JSONSupport.object(from: body)
  550:         }
  551:         return nil
  552:     }
  553: }
  554: 
  555: public enum MCPStreamError: Error, LocalizedError, Sendable {
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:257` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  250:             operation: "sign \(kind.description)"
  251:         )
  252:     }
  253: 
  254:     func verify(_ url: URL, kind: ManagerArtifactKind) throws {
  255:         var verifyArguments = ["--verify", "--strict"]
  256:         if kind != .executable {
  257:             verifyArguments.append("--deep")
  258:         }
  259:         verifyArguments.append(url.path)
  260:         try runRequired(
  261:             executable: "/usr/bin/codesign",
  262:             arguments: verifyArguments,
  263:             timeoutSec: kind == .applicationBundle ? 60 : 30,
  264:             operation: "verify \(kind.description)"
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:259` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  252:     }
  253: 
  254:     func verify(_ url: URL, kind: ManagerArtifactKind) throws {
  255:         var verifyArguments = ["--verify", "--strict"]
  256:         if kind != .executable {
  257:             verifyArguments.append("--deep")
  258:         }
  259:         verifyArguments.append(url.path)
  260:         try runRequired(
  261:             executable: "/usr/bin/codesign",
  262:             arguments: verifyArguments,
  263:             timeoutSec: kind == .applicationBundle ? 60 : 30,
  264:             operation: "verify \(kind.description)"
  265:         )
  266:     }
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:562` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  555:         var replacements = [
  556:             ArtifactReplacement(target: frameworkTarget, staged: frameworkStage),
  557:             ArtifactReplacement(target: mirroredFrameworkTarget, staged: mirroredFrameworkStage),
  558:             ArtifactReplacement(target: binaryTarget, staged: binaryStage),
  559:             ArtifactReplacement(target: appTarget, staged: appStage),
  560:         ]
  561:         if let commandLinkTarget {
  562:             replacements.append(
  563:                 ArtifactReplacement(target: commandLinkTarget, staged: commandLinkStage)
  564:             )
  565:         }
  566:         try commitArtifactReplacements(replacements, transactionID: transactionID)
  567:         return binaryTarget
  568:     }
  569: 
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:721` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  714:         }
  715:     }
  716: 
  717:     private func sourceFramework(for sourceExecutable: URL) -> URL? {
  718:         let name = "ForgeConductorCore.framework"
  719:         var candidates: [URL] = []
  720:         if let sourceBundle = sourceAppBundle(containing: sourceExecutable) {
  721:             candidates.append(
  722:                 sourceBundle
  723:                     .appendingPathComponent("Contents/Frameworks", isDirectory: true)
  724:                     .appendingPathComponent(name)
  725:             )
  726:         }
  727:         candidates.append(sourceExecutable.deletingLastPathComponent().appendingPathComponent(name))
  728:         candidates.append(
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:727` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  720:         if let sourceBundle = sourceAppBundle(containing: sourceExecutable) {
  721:             candidates.append(
  722:                 sourceBundle
  723:                     .appendingPathComponent("Contents/Frameworks", isDirectory: true)
  724:                     .appendingPathComponent(name)
  725:             )
  726:         }
  727:         candidates.append(sourceExecutable.deletingLastPathComponent().appendingPathComponent(name))
  728:         candidates.append(
  729:             sourceExecutable.deletingLastPathComponent()
  730:                 .appendingPathComponent("PackageFrameworks", isDirectory: true)
  731:                 .appendingPathComponent(name)
  732:         )
  733:         return candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) })
  734:     }
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:728` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  721:             candidates.append(
  722:                 sourceBundle
  723:                     .appendingPathComponent("Contents/Frameworks", isDirectory: true)
  724:                     .appendingPathComponent(name)
  725:             )
  726:         }
  727:         candidates.append(sourceExecutable.deletingLastPathComponent().appendingPathComponent(name))
  728:         candidates.append(
  729:             sourceExecutable.deletingLastPathComponent()
  730:                 .appendingPathComponent("PackageFrameworks", isDirectory: true)
  731:                 .appendingPathComponent(name)
  732:         )
  733:         return candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) })
  734:     }
  735: 
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:790` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  783:                 )
  784:                 let record = CommitRecord(
  785:                     target: replacement.target,
  786:                     staged: replacement.staged,
  787:                     backup: backup,
  788:                     hadOriginal: itemExists(at: replacement.target)
  789:                 )
  790:                 records.append(record)
  791:                 try artifactReplacer.applyReplacement(
  792:                     target: replacement.target,
  793:                     staged: replacement.staged,
  794:                     backup: backup,
  795:                     hadOriginal: record.hadOriginal
  796:                 )
  797:             }
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:835` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  828:                     try fm.moveItem(at: record.backup, to: record.target)
  829:                 } else if !record.hadOriginal,
  830:                           itemExists(at: record.target),
  831:                           record.staged.map({ !itemExists(at: $0) }) ?? false {
  832:                     try fm.removeItem(at: record.target)
  833:                 }
  834:             } catch {
  835:                 failures.append("\(record.target.path): \(error.localizedDescription)")
  836:             }
  837:         }
  838:         return failures
  839:     }
  840: 
  841:     private func itemExists(at url: URL) -> Bool {
  842:         FileManager.default.fileExists(atPath: url.path)
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:907` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  900:                 removed = true
  901:                 entry["archived_to"] = dest.path
  902:             }
  903: 
  904:             entry["bootout_exit"] = bootout?.exitCode as Any
  905:             entry["removed"] = removed
  906:             entry["ok"] = true
  907:             results.append(entry)
  908:         }
  909: 
  910:         // Do not run `sfltool resetbtm` — it wipes all Background Items for the user.
  911:         // macOS refreshes Login Items after bootout + log out/in.
  912:         return results
  913:     }
  914: 
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:925` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  918:         for label in labels {
  919:             let plist = launchAgentsDir.appendingPathComponent("\(label).plist")
  920:             let loaded = (try? ProcessRunner().run(
  921:                 executable: "/bin/launchctl",
  922:                 arguments: ["print", "gui/\(getuid())/\(label)"],
  923:                 timeoutSec: 3
  924:             ))?.exitCode == 0
  925:             out.append([
  926:                 "label": label,
  927:                 "plist_exists": FileManager.default.fileExists(atPath: plist.path),
  928:                 "plist": plist.path,
  929:                 "loaded": loaded,
  930:                 "stale": Self.staleLaunchAgentLabels.contains(label),
  931:             ])
  932:         }
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:956` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  949:             exe,
  950:             "manager",
  951:             "run",
  952:             "--home",
  953:             paths.home.path,
  954:         ]
  955:         if openBrowser {
  956:             programArgs.append("--open")
  957:         }
  958: 
  959:         // AssociatedBundleIdentifiers ties the agent to the .app for Login Items naming.
  960:         let plist = """
  961:         <?xml version="1.0" encoding="UTF-8"?>
  962:         <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  963:         <plist version="1.0">
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1259` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
 1252:             _ = try? ProcessRunner().run(
 1253:                 executable: "/usr/libexec/ApplicationFirewall/socketfilterfw",
 1254:                 arguments: ["--unblockapp", path],
 1255:                 timeoutSec: 5
 1256:             )
 1257:             let ok = (add?.exitCode == 0) || (blocked?.stdout.contains("permitted") == true)
 1258:             if ok { anyOK = true }
 1259:             details.append([
 1260:                 "path": path,
 1261:                 "ok": ok,
 1262:                 "getappblocked": blocked?.stdout ?? "",
 1263:             ])
 1264:         }
 1265:         return [
 1266:             "ok": anyOK,
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1288` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
 1281:         ), r.exitCode == 0 {
 1282:             for line in r.stdout.split(separator: "\n").map(String.init) {
 1283:                 let lower = line.lowercased()
 1284:                 if lower.contains("endpoint_security") || lower.contains("network_extension")
 1285:                     || lower.contains("falcon") || lower.contains("jamf.protect")
 1286:                     || lower.contains("traps") || lower.contains("globalprotect")
 1287:                     || lower.contains("cortex") {
 1288:                     extensions.append(["line": line.trimmingCharacters(in: .whitespaces)])
 1289:                 }
 1290:             }
 1291:         }
 1292: 
 1293:         let binary = installedBinaryURL.path
 1294:         let binaryExists = FileManager.default.isExecutableFile(atPath: binary)
 1295:         let legacyLink = FileManager.default.homeDirectoryForCurrentUser
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:226` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  219:     }
  220: 
  221:     public func requestShutdown(delayMs: Int = 300) {
  222:         lock.lock()
  223:         runtime.requestShutdown()
  224:         lock.unlock()
  225:         app.diagnostics.info("manager_shutdown_requested", [:])
  226:         runtime.queue.asyncAfter(deadline: .now() + .milliseconds(delayMs)) { [weak self] in
  227:             self?.halt()
  228:         }
  229:     }
  230: 
  231:     // MARK: - Run loop
  232: 
  233:     public func run(openBrowser: Bool = false) throws {
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:346` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  339:     }
  340: 
  341:     // MARK: - Watchdog
  342: 
  343:     private func startWatchdog() {
  344:         stopWatchdog()
  345:         let interval = max(1, app.config.model.manager.watchdogIntervalSec)
  346:         let timer = DispatchSource.makeTimerSource(queue: runtime.queue)
  347:         timer.schedule(deadline: .now() + .seconds(interval), repeating: .seconds(interval))
  348:         timer.setEventHandler { [weak self] in
  349:             self?.watchdogTick()
  350:         }
  351:         timer.resume()
  352:         lock.lock()
  353:         runtime.watchdog = timer
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:447` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  440:             ], category: .manager)
  441:         }
  442:     }
  443: 
  444:     private func installSignalHandlers() {
  445:         signal(SIGINT, SIG_IGN)
  446:         signal(SIGTERM, SIG_IGN)
  447:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: runtime.queue)
  448:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: runtime.queue)
  449:         sigInt.setEventHandler { [weak self] in self?.requestShutdown(delayMs: 50) }
  450:         sigTerm.setEventHandler { [weak self] in self?.requestShutdown(delayMs: 50) }
  451:         sigInt.resume()
  452:         sigTerm.resume()
  453:         lock.lock()
  454:         runtime.signalSources = [sigInt, sigTerm]
```

### `Sources/ForgeConductorCore/Manager/ManagerNode.swift:448` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  441:         }
  442:     }
  443: 
  444:     private func installSignalHandlers() {
  445:         signal(SIGINT, SIG_IGN)
  446:         signal(SIGTERM, SIG_IGN)
  447:         let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: runtime.queue)
  448:         let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: runtime.queue)
  449:         sigInt.setEventHandler { [weak self] in self?.requestShutdown(delayMs: 50) }
  450:         sigTerm.setEventHandler { [weak self] in self?.requestShutdown(delayMs: 50) }
  451:         sigInt.resume()
  452:         sigTerm.resume()
  453:         lock.lock()
  454:         runtime.signalSources = [sigInt, sigTerm]
  455:         lock.unlock()
```

### `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:23` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   16:     public var startedAt: Date?
   17:     public var restartCount = 0
   18:     public var watchdog: DispatchSourceTimer?
   19:     public var lastPresencePruneAt: Date?
   20:     public var shutdownRequested = false
   21:     public var signalSources: [Any] = []
   22:     public let runLock = DispatchSemaphore(value: 0)
   23:     public let queue = DispatchQueue(label: "forge.manager", qos: .userInitiated)
   24: 
   25:     public init() {}
   26: 
   27:     public var isHTTPUp: Bool {
   28:         dashboard?.isRunning == true
   29:     }
   30: 
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/CPUCollector.swift:16` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    9: 
   10: /// Real-time per-core and aggregate CPU via Mach host APIs.
   11: ///
   12: /// - `host_processor_info(..., PROCESSOR_CPU_LOAD_INFO)` — per-core tick counters
   13: /// - `host_statistics(..., HOST_CPU_LOAD_INFO)` — host-wide tick counters (fallback)
   14: ///
   15: /// Utilization is always a **delta between samples** stored on this collector.
   16: /// Never blocks the realtime queue with `Thread.sleep` (that is a snapshot anti-pattern).
   17: public final class CPUCollector: CPUMetricsCollecting, @unchecked Sendable {
   18:     private let lock = NSLock()
   19:     private var previousCores: [CoreTicks]?
   20:     private var previousHost: CoreTicks?
   21: 
   22:     private struct CoreTicks {
   23:         var user: UInt32
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/CPUCollector.swift:159` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  152:         var cores: [CoreTicks] = []
  153:         cores.reserveCapacity(Int(cpuCount))
  154:         // PROCESSOR_CPU_LOAD_INFO: CPU_STATE_MAX integer_t ticks per logical CPU
  155:         let stride = Int(CPU_STATE_MAX)
  156:         for i in 0..<Int(cpuCount) {
  157:             let base = i * stride
  158:             guard base + 3 < Int(cpuInfoCount) else { break }
  159:             cores.append(CoreTicks(
  160:                 user: UInt32(bitPattern: info[base + Int(CPU_STATE_USER)]),
  161:                 system: UInt32(bitPattern: info[base + Int(CPU_STATE_SYSTEM)]),
  162:                 idle: UInt32(bitPattern: info[base + Int(CPU_STATE_IDLE)]),
  163:                 nice: UInt32(bitPattern: info[base + Int(CPU_STATE_NICE)])
  164:             ))
  165:         }
  166:         return cores
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/CPUFrequencyEstimator.swift:91` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   84:                 : 0
   85:             // Apple lists Performance cores first in host_processor_info on AS.
   86:             let isP = i < pCount
   87:             let peak = isP ? pPeak : ePeak
   88:             let floor = isP ? pFloor : eFloor
   89:             // Effective clock for this core between floor and peak by utilization.
   90:             let mhz = Int((Double(floor) + Double(peak - floor) * util).rounded())
   91:             per.append(mhz)
   92:             // Weight busier cores more for the strip average (matches "how hard is the chip working").
   93:             let w = 0.15 + util
   94:             weighted += Double(mhz) * w
   95:             weight += w
   96:         }
   97: 
   98:         let avg = weight > 0 ? Int((weighted / weight).rounded()) : pPeak
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/DiskVolumeCollector.swift:25` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   18:             let total = (u[.systemSize] as? NSNumber)?.uint64Value ?? 0
   19:             let free = (u[.systemFreeSize] as? NSNumber)?.uint64Value ?? 0
   20:             let used = total > free ? total - free : 0
   21:             let pct = total > 0 ? 100.0 * Double(used) / Double(total) : 0
   22:             let key = "\(total)-\(mount)"
   23:             if seen.contains(key) { continue }
   24:             seen.insert(key)
   25:             rows.append(DiskVolume(
   26:                 device: mount,
   27:                 mount: mount,
   28:                 fstype: "apfs",
   29:                 totalGB: round1(Double(total) / 1_073_741_824),
   30:                 usedGB: round1(Double(used) / 1_073_741_824),
   31:                 availableGB: round1(Double(free) / 1_073_741_824),
   32:                 percent: round1(pct)
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:73` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
   66:             let threads: Int?
   67:             if pid == selfPID {
   68:                 threads = MachTaskThreadSampler.currentProcessThreadCount()
   69:             } else {
   70:                 threads = threadCount(pid)
   71:             }
   72: 
   73:             rows.append(ProcessMetrics(
   74:                 pid: Int(pid),
   75:                 name: String(leaf.prefix(48)),
   76:                 cpuPercent: (cpu * 10).rounded() / 10,
   77:                 rssGB: (sample.rssBytes / 1_073_741_824.0 * 100).rounded() / 100,
   78:                 footprintGB: sample.footprintBytes.map { ($0 / 1_073_741_824.0 * 100).rounded() / 100 },
   79:                 threadCount: threads,
   80:                 source: sample.source
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/RAMCollector.swift:4` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
    1: // RAMCollector.swift
    2: // What: Measures physical memory pressure and page-class usage.
    3: // How: host_statistics64 page counts are converted with the host page size into typed
    4: // used, available, wired, compressed, and cached byte totals.
    5: // Why: Direct Mach counters provide consistent native memory telemetry.
    6: 
    7: import Foundation
    8: import Darwin
    9: 
   10: /// Real-time RAM via Mach `host_statistics64(mach_host_self(), HOST_VM_INFO64, …)`.
   11: /// Instant counters (free/active/inactive/wired/compressor) — no poll interval, no sleep.
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:340` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  333:         let activeWindow: TimeInterval = 15 * 60
  334: 
  335:         var cards: [AgentCard] = []
  336:         var seen = Set<String>()
  337:         for spec in catalog.all() {
  338:             seen.insert(spec.id)
  339:             let latest = byID[spec.id]
  340:             cards.append(agentCard(
  341:                 id: spec.id,
  342:                 name: spec.displayName,
  343:                 description: spec.description,
  344:                 tools: spec.tools,
  345:                 session: latest,
  346:                 audit: audit,
  347:                 now: now,
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:352` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  345:                 session: latest,
  346:                 audit: audit,
  347:                 now: now,
  348:                 activeWindow: activeWindow
  349:             ))
  350:         }
  351:         for (id, s) in byID where !seen.contains(id) {
  352:             cards.append(agentCard(
  353:                 id: id,
  354:                 name: id,
  355:                 description: "",
  356:                 tools: [],
  357:                 session: s,
  358:                 audit: audit,
  359:                 now: now,
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:463` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  456:         let names = Set(toolNamesProvider() + knownToolNames())
  457:         var tools: [ToolCard] = []
  458:         for name in names.sorted() {
  459:             let usage = usageForTool(audit, tool: name, windowSec: 3600)
  460:             let e5 = usageForTool(audit, tool: name, windowSec: 300)
  461:             let status = e5.eventCount > 0 ? "active" : (usage.eventCount > 0 ? "warm" : "idle")
  462:             let health = ToolUsageHealthPolicy.health(for: usage)
  463:             tools.append(ToolCard(
  464:                 name: name,
  465:                 pack: packForTool(name),
  466:                 status: status,
  467:                 health: health.rawValue,
  468:                 healthLabel: health.label,
  469:                 activity: Double(min(100, usage.eventCount)),
  470:                 live: e5.eventCount > 0,
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:632` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  625:                 Self.normalizedAuditClientID(fromPresenceID: $0.record.clientID)
  626:             }
  627:             let usage = auditClientID.map {
  628:                 usageForClient(audit, clientID: $0, windowSec: 300)
  629:             } ?? .empty
  630:             let health = mcpHealth(live: true, usage: usage)
  631: 
  632:             cards.append(MCPServerCard(
  633:                 id: observation?.record.clientID ?? "proc-\(label)-\(process.pid)",
  634:                 label: label,
  635:                 role: roleFromLabel(label, hostKind: hostKind),
  636:                 hostKind: hostKind,
  637:                 pid: Int(process.pid),
  638:                 live: true,
  639:                 status: usage.eventCount > 0 ? "active" : "idle",
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:672` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  665:                 representedConnectorRoles.insert(roleKey)
  666:             }
  667:             let label = connectorLabel(roleKey: roleKey) ?? mcpLabel(cwd: record.cwd)
  668:             let auditClientID = Self.normalizedAuditClientID(fromPresenceID: record.clientID)
  669:             let usage = usageForClient(audit, clientID: auditClientID, windowSec: 300)
  670:             let health = mcpHealth(live: observation.processUp, usage: usage)
  671: 
  672:             cards.append(MCPServerCard(
  673:                 id: record.clientID.isEmpty ? "presence-\(record.pid)" : record.clientID,
  674:                 label: label,
  675:                 role: roleFromLabel(label, hostKind: hostKind),
  676:                 hostKind: hostKind,
  677:                 pid: record.pid > 0 ? Int(record.pid) : nil,
  678:                 live: observation.processUp,
  679:                 status: observation.processUp
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:708` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  701:             let roleKey = configuredConnectorRoleKey(server)
  702:             if let roleKey, representedConnectorRoles.contains(roleKey) {
  703:                 continue
  704:             }
  705: 
  706:             let role = roleFromLabel(server.id, hostKind: "lm-studio-mcp")
  707:             let usage = usageForClient(audit, clientID: server.id, windowSec: 300)
  708:             cards.append(MCPServerCard(
  709:                 id: "cfg-\(server.id)",
  710:                 label: server.id,
  711:                 role: role,
  712:                 hostKind: "lm-studio-mcp",
  713:                 pid: nil,
  714:                 live: false,
  715:                 status: "configured",
```

### `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:742` — \.append\s*\(\|removeFirst\s*\(\|removeAll\s*\(\|suffix\s*\(\|history\|cache\|queue

```swift
  735:     }
  736: 
  737:     /// Current presence records append role to the UUID while audit events retain
  738:     /// the bare UUID. Accept both shapes so existing databases remain readable.
  739:     static func normalizedAuditClientID(fromPresenceID presenceID: String) -> String {
  740:         for role in LMStudioConnectorRole.allCases {
  741:             let suffix = ":\(role.rawValue)"
  742:             if presenceID.hasSuffix(suffix), presenceID.count > suffix.count {
  743:                 return String(presenceID.dropLast(suffix.count))
  744:             }
  745:         }
  746:         return presenceID
  747:     }
  748: 
  749:     /// Counts the same recent, product-relevant presence observations considered
```

122 additional hits are retained in the JSON evidence.

## Unified logging/signposts and print usage

86 lexical hits.

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:26` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
   19:         let rest = Array(args.dropFirst())
   20: 
   21:         do {
   22:             switch command {
   23:             case "help", "-h", "--help":
   24:                 printHelp()
   25:             case "version", "--version":
   26:                 print(ForgeApp.version)
   27:             case "install":
   28:                 try cmdInstall(rest)
   29:             case "install-lmstudio-plugin":
   30:                 try cmdInstallLMStudioPlugin(rest)
   31:             case "doctor":
   32:                 try cmdDoctor(rest)
   33:             case "status":
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:55` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
   48:         } catch {
   49:             fputs("error: \(error)\n", stderr)
   50:             exit(1)
   51:         }
   52:     }
   53: 
   54:     static func printHelp() {
   55:         print("""
   56:         Forge-Conductor \(ForgeApp.version) — native Swift MCP orchestrator
   57: 
   58:         Usage:
   59:           forge-conductor <command> [options]
   60: 
   61:         Commands:
   62:           install              Layout + install Swift binary to ~/.forge-conductor/bin
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:116` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  109:         var source: URL?
  110:         if let idx = args.firstIndex(of: "--from"), args.index(after: idx) < args.endIndex {
  111:             source = URL(fileURLWithPath: args[args.index(after: idx)])
  112:         }
  113:         let dest = try installer.installBinary(from: source)
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:117` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  110:         if let idx = args.firstIndex(of: "--from"), args.index(after: idx) < args.endIndex {
  111:             source = URL(fileURLWithPath: args[args.index(after: idx)])
  112:         }
  113:         let dest = try installer.installBinary(from: source)
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:118` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  111:             source = URL(fileURLWithPath: args[args.index(after: idx)])
  112:         }
  113:         let dest = try installer.installBinary(from: source)
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:119` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  112:         }
  113:         let dest = try installer.installBinary(from: source)
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:120` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  113:         let dest = try installer.installBinary(from: source)
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:121` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  114:         _ = try? app.store.presencePrune(maxAgeSec: 60)
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:122` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  115:         let fw = installer.tryAllowFirewall()
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:123` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  116:         print("Installed Forge-Conductor at \(app.paths.home.path)")
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:124` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  117:         print("  CLI binary: \(dest.path)")
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:125` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  118:         print("  App:        \(installer.appExecutableURL.path)")
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:126` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  119:         print("  link:   ~/.local/bin/forge-conductor-swift → \(dest.path)")
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
  133:     static func cmdInstallLMStudioPlugin(_ args: [String]) throws {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:127` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  120:         print("  store:  \(app.paths.storeSQLite.path)")
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
  133:     static func cmdInstallLMStudioPlugin(_ args: [String]) throws {
  134:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:128` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  121:         print("  config: \(app.paths.configJSON.path)")
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
  133:     static func cmdInstallLMStudioPlugin(_ args: [String]) throws {
  134:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  135:         defer { app.shutdown() }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:129` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  122:         print("  agents: \(app.catalog.all().count) loaded")
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
  133:     static func cmdInstallLMStudioPlugin(_ args: [String]) throws {
  134:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  135:         defer { app.shutdown() }
  136:         var preferred: URL? = nil
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:130` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  123:         print("  firewall: \(fw["ok"] as? Bool == true ? "ok/permitted" : "may need admin — see manager allowlist")")
  124:         print("")
  125:         print("LM Studio is NOT modified by install. Product path:")
  126:         print("  1) Open Forge Conductor GUI → LM Studio MCP → Deploy to LM Studio")
  127:         print("  2) Or: \(dest.path) install-lmstudio-plugin")
  128:         print("  Deploy writes LM Studio configuration, activates both roles, and verifies hosted tools automatically.")
  129:         print("")
  130:         print("App argv:  serve | manager run [--home PATH] | (none = GUI)")
  131:     }
  132: 
  133:     static func cmdInstallLMStudioPlugin(_ args: [String]) throws {
  134:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  135:         defer { app.shutdown() }
  136:         var preferred: URL? = nil
  137:         if let idx = args.firstIndex(of: "--binary"), args.index(after: idx) < args.endIndex {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:141` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  134:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  135:         defer { app.shutdown() }
  136:         var preferred: URL? = nil
  137:         if let idx = args.firstIndex(of: "--binary"), args.index(after: idx) < args.endIndex {
  138:             preferred = URL(fileURLWithPath: (args[args.index(after: idx)] as NSString).expandingTildeInPath)
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:142` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  135:         defer { app.shutdown() }
  136:         var preferred: URL? = nil
  137:         if let idx = args.firstIndex(of: "--binary"), args.index(after: idx) < args.endIndex {
  138:             preferred = URL(fileURLWithPath: (args[args.index(after: idx)] as NSString).expandingTildeInPath)
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:143` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  136:         var preferred: URL? = nil
  137:         if let idx = args.firstIndex(of: "--binary"), args.index(after: idx) < args.endIndex {
  138:             preferred = URL(fileURLWithPath: (args[args.index(after: idx)] as NSString).expandingTildeInPath)
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:144` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  137:         if let idx = args.firstIndex(of: "--binary"), args.index(after: idx) < args.endIndex {
  138:             preferred = URL(fileURLWithPath: (args[args.index(after: idx)] as NSString).expandingTildeInPath)
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:145` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  138:             preferred = URL(fileURLWithPath: (args[args.index(after: idx)] as NSString).expandingTildeInPath)
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
  152: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:146` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  139:         }
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
  152: 
  153:     static func cmdDoctor(_ args: [String]) throws {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:147` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  140:         let result = try app.lmStudioDeploy.deploy(preferredBinary: preferred)
  141:         print(result.message)
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
  152: 
  153:     static func cmdDoctor(_ args: [String]) throws {
  154:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:149` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  142:         print("  binary:  \(result.binaryPath)")
  143:         print("  args:    serve")
  144:         print("  plugins: \(result.pluginsWritten.joined(separator: ", "))")
  145:         print("  mcp.json: \(result.mcpConfigPath)")
  146:         print("  revision: \(result.deploymentID)")
  147:         print("  host:    configuration synchronized; no manual file editing or restart required")
  148:         let st = app.lmStudioDeploy.status(preferredBinary: preferred)
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
  152: 
  153:     static func cmdDoctor(_ args: [String]) throws {
  154:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  155:         let result = try app.doctor()
  156:         print(try JSONSupport.string(from: result))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:156` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  149:         print("  status:  \(st.detail)")
  150:         if !result.ok { exit(1) }
  151:     }
  152: 
  153:     static func cmdDoctor(_ args: [String]) throws {
  154:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  155:         let result = try app.doctor()
  156:         print(try JSONSupport.string(from: result))
  157:         if result["ok"] as? Bool != true { exit(1) }
  158:     }
  159: 
  160:     static func cmdStatus(_ args: [String]) throws {
  161:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  162:         var snap = try app.statusSnapshot()
  163:         if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:173` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  166:             if let data = try? Data(contentsOf: app.paths.managerState),
  167:                let state = try? JSONSupport.object(from: data) {
  168:                 snap["manager_state"] = state
  169:             }
  170:         } else {
  171:             snap["manager_running"] = false
  172:         }
  173:         print(try JSONSupport.string(from: snap))
  174:     }
  175: 
  176:     static func cmdAgents(_ args: [String]) throws {
  177:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  178:         for a in app.catalog.all() {
  179:             print("\(a.id)\t\(a.displayName)\t\(a.source)")
  180:         }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:179` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  172:         }
  173:         print(try JSONSupport.string(from: snap))
  174:     }
  175: 
  176:     static func cmdAgents(_ args: [String]) throws {
  177:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  178:         for a in app.catalog.all() {
  179:             print("\(a.id)\t\(a.displayName)\t\(a.source)")
  180:         }
  181:     }
  182: 
  183:     static func cmdServe(_ args: [String]) throws {
  184:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  185:         let server = MCPServer(app: app)
  186:         try server.run()
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:259` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  252:         case "uninstall-login":
  253:             try managerUninstallLogin(rest)
  254:         case "cleanup-stale":
  255:             try managerCleanupStale(rest)
  256:         case "allowlist":
  257:             try managerAllowlist(rest)
  258:         case "help", "-h", "--help":
  259:             print("""
  260:             forge-conductor manager <subcommand>
  261: 
  262:               run [--open] [--home PATH]    Foreground supervised dashboard
  263:               start [--open] [--home PATH]  Background daemon
  264:               stop [--home PATH]            SIGTERM running manager
  265:               restart [--open]              stop + start
  266:               status [--home PATH]          JSON status
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:290` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  283:     static func managerInstallLogin(_ args: [String]) throws {
  284:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  285:         let installer = ManagerInstaller(app: app)
  286: 
  287:         // Always clear stale agents first so Login Items is not cluttered with bash/python3.
  288:         if !args.contains("--keep-stale") {
  289:             let cleaned = try installer.cleanupStaleLaunchAgents()
  290:             print("Cleaned stale LaunchAgents:")
  291:             for row in cleaned {
  292:                 print("  - \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false)")
  293:             }
  294:         }
  295: 
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:292` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  285:         let installer = ManagerInstaller(app: app)
  286: 
  287:         // Always clear stale agents first so Login Items is not cluttered with bash/python3.
  288:         if !args.contains("--keep-stale") {
  289:             let cleaned = try installer.cleanupStaleLaunchAgents()
  290:             print("Cleaned stale LaunchAgents:")
  291:             for row in cleaned {
  292:                 print("  - \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false)")
  293:             }
  294:         }
  295: 
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:300` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  293:             }
  294:         }
  295: 
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:301` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  294:         }
  295: 
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:302` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  295: 
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:303` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  296:         let plist = try installer.installLoginAgent(openBrowser: args.contains("--open"))
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:304` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  297:         let appBundle = installer.appBundleURL
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:305` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  298:         _ = installer.tryAllowFirewall()
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:306` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  299: 
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:307` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  300:         print("")
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:308` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  301:         print("Forge Conductor login item installed")
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:309` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  302:         print("  display name: \(ManagerInstaller.appDisplayName)")
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:310` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  303:         print("  app bundle:   \(appBundle.path)")
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:311` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  304:         print("  label:        \(ManagerInstaller.launchAgentLabel)")
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:312` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  305:         print("  plist:        \(plist.path)")
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:313` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  306:         print("  binary:       \(installer.installedBinaryURL.path)")
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
  320: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:314` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  307:         print("  dashboard:    http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
  320: 
  321:     static func managerCleanupStale(_ args: [String]) throws {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:315` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  308:         print("")
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
  320: 
  321:     static func managerCleanupStale(_ args: [String]) throws {
  322:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:316` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  309:         print("Where to look in System Settings:")
  310:         print("  General → Login Items & Extensions → Allow in the Background")
  311:         print("  → \"\(ManagerInstaller.appDisplayName)\"  (NOT bash / python3)")
  312:         print("")
  313:         print("If it still does not appear:")
  314:         print("  1. Log out and back in (BTM refresh)")
  315:         print("  2. Or reboot once")
  316:         print("  3. Check: launchctl print gui/$(id -u)/\(ManagerInstaller.launchAgentLabel)")
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
  320: 
  321:     static func managerCleanupStale(_ args: [String]) throws {
  322:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  323:         let installer = ManagerInstaller(app: app)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:324` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  317:         Thread.sleep(forTimeInterval: 1.0)
  318:         try managerStatus(args)
  319:     }
  320: 
  321:     static func managerCleanupStale(_ args: [String]) throws {
  322:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  323:         let installer = ManagerInstaller(app: app)
  324:         print("Before:")
  325:         for row in installer.listForgeLaunchAgents() {
  326:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false) stale=\(row["stale"] as? Bool ?? false)")
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:326` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  319:     }
  320: 
  321:     static func managerCleanupStale(_ args: [String]) throws {
  322:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  323:         let installer = ManagerInstaller(app: app)
  324:         print("Before:")
  325:         for row in installer.listForgeLaunchAgents() {
  326:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false) stale=\(row["stale"] as? Bool ?? false)")
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:329` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  322:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  323:         let installer = ManagerInstaller(app: app)
  324:         print("Before:")
  325:         for row in installer.listForgeLaunchAgents() {
  326:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false) stale=\(row["stale"] as? Bool ?? false)")
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:330` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  323:         let installer = ManagerInstaller(app: app)
  324:         print("Before:")
  325:         for row in installer.listForgeLaunchAgents() {
  326:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false) stale=\(row["stale"] as? Bool ?? false)")
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:332` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  325:         for row in installer.listForgeLaunchAgents() {
  326:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false) stale=\(row["stale"] as? Bool ?? false)")
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:334` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  327:         }
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:335` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  328:         let cleaned = try installer.cleanupStaleLaunchAgents()
  329:         print("")
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:337` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  330:         print("Cleanup results:")
  331:         for row in cleaned {
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:339` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  332:             print("  \(row["label"] as? String ?? "?") removed=\(row["removed"] as? Bool ?? false) archived=\(row["archived_to"] as? String ?? "—")")
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:340` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  333:         }
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  347:         let installer = ManagerInstaller(app: app)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:341` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  334:         print("")
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  347:         let installer = ManagerInstaller(app: app)
  348:         // Stop process first
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:342` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  335:         print("After:")
  336:         for row in installer.listForgeLaunchAgents() {
  337:             print("  \(row["label"] as? String ?? "?") exists=\(row["plist_exists"] as? Bool ?? false) loaded=\(row["loaded"] as? Bool ?? false)")
  338:         }
  339:         print("")
  340:         print("Legacy plists archived under ~/.forge-conductor/legacy-launchagents/")
  341:         print("Note: Login Items UI may still show ghost entries until log out/in.")
  342:         print("Next: forge-conductor manager install-login")
  343:     }
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  347:         let installer = ManagerInstaller(app: app)
  348:         // Stop process first
  349:         _ = ManagerPIDFile.signalStop(paths: app.paths)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:351` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  344: 
  345:     static func managerUninstallLogin(_ args: [String]) throws {
  346:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  347:         let installer = ManagerInstaller(app: app)
  348:         // Stop process first
  349:         _ = ManagerPIDFile.signalStop(paths: app.paths)
  350:         let removed = try installer.uninstallLoginAgent()
  351:         print(removed ? "Login agent removed" : "Login agent was not installed")
  352:     }
  353: 
  354:     static func managerAllowlist(_ args: [String]) throws {
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:358` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  351:         print(removed ? "Login agent removed" : "Login agent was not installed")
  352:     }
  353: 
  354:     static func managerAllowlist(_ args: [String]) throws {
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:359` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  352:     }
  353: 
  354:     static func managerAllowlist(_ args: [String]) throws {
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:361` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  354:     static func managerAllowlist(_ args: [String]) throws {
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:362` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  355:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:363` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  356:         let installer = ManagerInstaller(app: app)
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:364` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  357:         let report = installer.endpointProtectionReport()
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:365` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  358:         print(try JSONSupport.string(from: report))
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:366` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  359:         print("")
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:367` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  360:         // Human summary
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:368` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  361:         print("=== Quick allowlist checklist (managed Mac) ===")
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:369` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  362:         print("1. Binary path to allow: \(installer.installedBinaryURL.path)")
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
  376:         }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:370` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  363:         print("2. Loopback listen: 127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))")
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
  376:         }
  377:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:371` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  364:         print("3. System Settings → General → Login Items & Extensions")
  365:         print("   - Background: allow forge-conductor / \(ManagerInstaller.launchAgentLabel)")
  366:         print("   - Endpoint Security Extensions: Falcon, Jamf Protect, Cortex (IT — leave on)")
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
  376:         }
  377:     }
  378: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:374` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  367:         print("   - Network Extensions: GlobalProtect / Cortex (if localhost blocked, ticket IT)")
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
  376:         }
  377:     }
  378: 
  379:     static func managerRun(_ args: [String], open: Bool) throws {
  380:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  381:         let node = ManagerNode(app: app)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:375` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  368:         print("4. macOS Firewall: allow incoming for the binary above")
  369:         print("5. Open dashboard with Chrome (not Safari):")
  370:         print("   open -a \"Google Chrome\" http://127.0.0.1:\(app.config.int("dashboard", "port", default: 7788))/")
  371:         print("6. Avoid old path ~/.local/bin/forge-conductor if it points at Python venv")
  372:         if let bin = report["binary"] as? [String: Any],
  373:            let w = bin["legacy_warning"] as? String, !w.isEmpty {
  374:             print("")
  375:             print("WARNING: \(w)")
  376:         }
  377:     }
  378: 
  379:     static func managerRun(_ args: [String], open: Bool) throws {
  380:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  381:         let node = ManagerNode(app: app)
  382:         try node.run(openBrowser: open)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:388` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  381:         let node = ManagerNode(app: app)
  382:         try node.run(openBrowser: open)
  383:     }
  384: 
  385:     static func managerStartBackground(_ args: [String]) throws {
  386:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  387:         if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
  388:             print("Manager already running (pid \(pid))")
  389:             print("  url: http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  390:             return
  391:         }
  392: 
  393:         // Prefer installed stable binary so EP allowlists target a fixed path.
  394:         let installer = ManagerInstaller(app: app)
  395:         let exe: String
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:389` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  382:         try node.run(openBrowser: open)
  383:     }
  384: 
  385:     static func managerStartBackground(_ args: [String]) throws {
  386:         let app = try ForgeApp.bootstrap(home: homeOverride(args))
  387:         if let pid = ManagerPIDFile.runningPID(paths: app.paths) {
  388:             print("Manager already running (pid \(pid))")
  389:             print("  url: http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  390:             return
  391:         }
  392: 
  393:         // Prefer installed stable binary so EP allowlists target a fixed path.
  394:         let installer = ManagerInstaller(app: app)
  395:         let exe: String
  396:         if FileManager.default.isExecutableFile(atPath: installer.installedBinaryURL.path) {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:437` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  430:                 launched = pid
  431:                 break
  432:             }
  433:         }
  434:         let host = app.config.string("dashboard", "host", default: "127.0.0.1")
  435:         let port = app.config.int("dashboard", "port", default: 7788)
  436:         if let launched {
  437:             print("Manager started (pid \(launched))")
  438:             print("  dashboard: http://\(host):\(port)/")
  439:             print("  log: \(app.paths.managerLog.path)")
  440:         } else {
  441:             fputs("Manager process launched but pid file not seen yet. Check \(app.paths.managerLog.path)\n", stderr)
  442:             exit(1)
  443:         }
  444:     }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:438` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  431:                 break
  432:             }
  433:         }
  434:         let host = app.config.string("dashboard", "host", default: "127.0.0.1")
  435:         let port = app.config.int("dashboard", "port", default: 7788)
  436:         if let launched {
  437:             print("Manager started (pid \(launched))")
  438:             print("  dashboard: http://\(host):\(port)/")
  439:             print("  log: \(app.paths.managerLog.path)")
  440:         } else {
  441:             fputs("Manager process launched but pid file not seen yet. Check \(app.paths.managerLog.path)\n", stderr)
  442:             exit(1)
  443:         }
  444:     }
  445: 
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:439` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  432:             }
  433:         }
  434:         let host = app.config.string("dashboard", "host", default: "127.0.0.1")
  435:         let port = app.config.int("dashboard", "port", default: 7788)
  436:         if let launched {
  437:             print("Manager started (pid \(launched))")
  438:             print("  dashboard: http://\(host):\(port)/")
  439:             print("  log: \(app.paths.managerLog.path)")
  440:         } else {
  441:             fputs("Manager process launched but pid file not seen yet. Check \(app.paths.managerLog.path)\n", stderr)
  442:             exit(1)
  443:         }
  444:     }
  445: 
  446:     static func managerStop(_ args: [String]) throws {
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:450` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  443:         }
  444:     }
  445: 
  446:     static func managerStop(_ args: [String]) throws {
  447:         let paths = AppPaths(home: homeOverride(args))
  448:         try paths.ensureLayout()
  449:         guard let pid = ManagerPIDFile.runningPID(paths: paths) else {
  450:             print("Manager is not running")
  451:             // clean stale pid
  452:             ManagerPIDFile.remove(paths: paths)
  453:             return
  454:         }
  455:         if ManagerPIDFile.signalStop(paths: paths) {
  456:             for _ in 0..<50 {
  457:                 if ManagerPIDFile.runningPID(paths: paths) == nil { break }
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:461` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  454:         }
  455:         if ManagerPIDFile.signalStop(paths: paths) {
  456:             for _ in 0..<50 {
  457:                 if ManagerPIDFile.runningPID(paths: paths) == nil { break }
  458:                 Thread.sleep(forTimeInterval: 0.1)
  459:             }
  460:             if ManagerPIDFile.runningPID(paths: paths) == nil {
  461:                 print("Manager stopped (was pid \(pid))")
  462:             } else {
  463:                 fputs("Sent SIGTERM to \(pid); process still alive\n", stderr)
  464:                 exit(1)
  465:             }
  466:         } else {
  467:             fputs("Failed to signal manager\n", stderr)
  468:             exit(1)
```

### `Sources/ForgeConductorCLI/ForgeConductorMain.swift:507` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  500:             let host = app.config.string("dashboard", "host", default: "127.0.0.1")
  501:             let port = app.config.int("dashboard", "port", default: 7788)
  502:             if let url = URL(string: "http://\(host):\(port)/api/manager/status"),
  503:                let live = try? liveJSON(url: url) {
  504:                 out["live"] = live
  505:             }
  506:         }
  507:         print(try JSONSupport.string(from: out.compactNSNull()))
  508:     }
  509: 
  510:     static func liveJSON(url: URL) throws -> [String: Any] {
  511:         // Thread-safe box avoids Swift 6 Sendable warnings on URLSession callbacks.
  512:         final class ResponseBox: @unchecked Sendable {
  513:             let lock = NSLock()
  514:             var data: Data?
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1345` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
 1338:             "port_in_use": isPortListening(port),
 1339:             "allowlist": allowlistInstructions(binaryPath: binary, port: port),
 1340:         ]
 1341:     }
 1342: 
 1343:     public func allowlistInstructions(binaryPath: String, port: Int) -> [String: Any] {
 1344:         [
 1345:             "macos_login_items": [
 1346:                 "System Settings → General → Login Items & Extensions → Allow in the Background",
 1347:                 "Look for \"\(Self.appDisplayName)\" (not bash/python3)",
 1348:                 "If missing: run manager cleanup-stale then manager install-login, then log out/in once",
 1349:                 "Endpoint Security Extensions (Falcon/Jamf Protect/Cortex) are IT-managed — leave enabled",
 1350:             ],
 1351:             "macos_firewall": [
 1352:                 "Allow incoming for: \(binaryPath)",
```

### `Tests/ForgeConductorTests/ManagerTests.swift:332` — \bLogger\s*\(\|OSLog\|os_log\|signpost\|\bprint\s*\(

```swift
  325:                 || installer.installedBinaryURL.path.contains(home.lastPathComponent)
  326:         )
  327:         let report = installer.endpointProtectionReport()
  328:         XCTAssertEqual(report["ok"] as? Bool, true)
  329:         let allow = report["allowlist"] as? [String: Any]
  330:         XCTAssertNotNil(allow?["crowdstrike_falcon"])
  331:         XCTAssertNotNil(allow?["jamf_protect"])
  332:         XCTAssertNotNil(allow?["macos_login_items"])
  333:     }
  334: 
  335:     func testManagerArtifactStagingReplacesStaleBinaryFrameworkAndAppBundle() throws {
  336:         let validator = TestManagerArtifactValidator()
  337:         let fixture = try makeArtifactFixture(validator: validator)
  338: 
  339:         let stagedBinary = try fixture.installer.stageInstalledArtifacts(
```

## Unsafe/native ownership evidence

13 lexical hits.

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:68` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   61:             ai_socktype: SOCK_STREAM,
   62:             ai_protocol: 0,
   63:             ai_addrlen: 0,
   64:             ai_canonname: nil,
   65:             ai_addr: nil,
   66:             ai_next: nil
   67:         )
   68:         var result: UnsafeMutablePointer<addrinfo>?
   69:         let portStr = String(port)
   70:         guard getaddrinfo(host, portStr, &hints, &result) == 0, let res = result else {
   71:             return false
   72:         }
   73:         defer { freeaddrinfo(res) }
   74:         var open = false
   75:         var ptr: UnsafeMutablePointer<addrinfo>? = res
```

### `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:75` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   68:         var result: UnsafeMutablePointer<addrinfo>?
   69:         let portStr = String(port)
   70:         guard getaddrinfo(host, portStr, &hints, &result) == 0, let res = result else {
   71:             return false
   72:         }
   73:         defer { freeaddrinfo(res) }
   74:         var open = false
   75:         var ptr: UnsafeMutablePointer<addrinfo>? = res
   76:         while let ai = ptr {
   77:             let fd = socket(ai.pointee.ai_family, ai.pointee.ai_socktype, ai.pointee.ai_protocol)
   78:             if fd >= 0 {
   79:                 let flags = fcntl(fd, F_GETFL, 0)
   80:                 _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
   81:                 let rc = connect(fd, ai.pointee.ai_addr, ai.pointee.ai_addrlen)
   82:                 if rc == 0 {
```

### `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:882` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
  875:         lock.lock()
  876:         defer { lock.unlock() }
  877:         try execUnlocked(sql)
  878:     }
  879: 
  880:     private func execUnlocked(_ sql: String) throws {
  881:         guard let db else { throw StoreError.openFailed("nil db") }
  882:         var err: UnsafeMutablePointer<CChar>?
  883:         if sqlite3_exec(db, sql, nil, nil, &err) != SQLITE_OK {
  884:             let msg = err.map { String(cString: $0) } ?? "unknown"
  885:             sqlite3_free(err)
  886:             throw StoreError.execFailed(msg)
  887:         }
  888:     }
  889: 
```

### `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1417` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
 1410:         return URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
 1411:             .appendingPathComponent(arg0)
 1412:             .resolvingSymlinksInPath()
 1413:     }
 1414: }
 1415: 
 1416: @_silgen_name("_NSGetExecutablePath")
 1417: func _NSGetExecutablePath(_ buf: UnsafeMutablePointer<CChar>, _ bufsize: UnsafeMutablePointer<UInt32>) -> Int32
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/CPUCollector.swift:185` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
  178: 
  179:     /// Mach: `host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, …)`
  180:     private func readHostTicks() -> CoreTicks? {
  181:         var load = host_cpu_load_info()
  182:         var count = mach_msg_type_number_t(
  183:             MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size
  184:         )
  185:         let kr = withUnsafeMutablePointer(to: &load) {
  186:             $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
  187:                 host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, $0, &count)
  188:             }
  189:         }
  190:         guard kr == KERN_SUCCESS else { return nil }
  191:         return CoreTicks(
  192:             user: load.cpu_ticks.0,
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/IOKitPropertyWalk.swift:19` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   12: /// - `IOServiceMatching` + `IOServiceGetMatchingServices` — locate services
   13: /// - `IORegistryEntryCreateCFProperties` — read the IORegistry property plane
   14: ///
   15: /// NSDictionary-safe: Swift `[String: Any]` casts often fail on nested CF types.
   16: enum IOKitPropertyWalk {
   17:     /// `IORegistryEntryCreateCFProperties` for a live `io_object_t` service.
   18:     static func props(for service: io_object_t) -> NSDictionary? {
   19:         var ref: Unmanaged<CFMutableDictionary>?
   20:         let kr = IORegistryEntryCreateCFProperties(service, &ref, kCFAllocatorDefault, 0)
   21:         guard kr == KERN_SUCCESS, let dict = ref?.takeRetainedValue() as NSDictionary? else { return nil }
   22:         return dict
   23:     }
   24: 
   25:     static func double(_ dict: NSDictionary, keys: [String]) -> Double? {
   26:         for k in keys {
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/IOKitPropertyWalk.swift:75` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   68:     }
   69: 
   70:     /// Iterate all services of a class.
   71:     static func forEachService(className: String, body: (io_object_t, NSDictionary) -> Void) {
   72:         let matching = IOServiceMatching(className)
   73:         var iterator: io_iterator_t = 0
   74:         guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return }
   75:         defer { IOObjectRelease(iterator) }
   76:         var service = IOIteratorNext(iterator)
   77:         while service != 0 {
   78:             defer {
   79:                 IOObjectRelease(service)
   80:                 service = IOIteratorNext(iterator)
   81:             }
   82:             if let p = props(for: service) {
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/IOKitPropertyWalk.swift:79` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   72:         let matching = IOServiceMatching(className)
   73:         var iterator: io_iterator_t = 0
   74:         guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return }
   75:         defer { IOObjectRelease(iterator) }
   76:         var service = IOIteratorNext(iterator)
   77:         while service != 0 {
   78:             defer {
   79:                 IOObjectRelease(service)
   80:                 service = IOIteratorNext(iterator)
   81:             }
   82:             if let p = props(for: service) {
   83:                 body(service, p)
   84:             }
   85:         }
   86:     }
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:175` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
  168:             as: UTF8.self
  169:         )
  170:     }
  171: 
  172:     /// `proc_pid_rusage(pid, RUSAGE_INFO_V3, …)`
  173:     private func rusageV3(_ pid: Int32) -> rusage_info_v3? {
  174:         var info = rusage_info_v3()
  175:         let ret = withUnsafeMutablePointer(to: &info) { ptr -> Int32 in
  176:             ptr.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { rebound in
  177:                 proc_pid_rusage(pid, RUSAGE_INFO_V3, rebound)
  178:             }
  179:         }
  180:         guard ret == 0 else { return nil }
  181:         return info
  182:     }
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/ProcessMetricsCollector.swift:229` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
  222:         return Int(threadCount)
  223:     }
  224: 
  225:     /// `task_info(mach_task_self_, TASK_BASIC_INFO, …)` resident size of this process.
  226:     static func currentTaskRSSBytes() -> UInt64? {
  227:         var info = mach_task_basic_info()
  228:         var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info_data_t>.stride / MemoryLayout<natural_t>.stride)
  229:         let kr = withUnsafeMutablePointer(to: &info) {
  230:             $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
  231:                 task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
  232:             }
  233:         }
  234:         guard kr == KERN_SUCCESS else { return nil }
  235:         return UInt64(info.resident_size)
  236:     }
```

### `Sources/ForgeConductorCore/Telemetry/Collectors/RAMCollector.swift:19` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
   12: public final class RAMCollector: RAMMetricsCollecting, @unchecked Sendable {
   13:     public init() {}
   14: 
   15:     public func collect() -> RAMMetrics {
   16:         let page = UInt64(sysctlInt("hw.pagesize") ?? 16384)
   17:         var stats = vm_statistics64()
   18:         var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.stride / MemoryLayout<integer_t>.stride)
   19:         let kr = withUnsafeMutablePointer(to: &stats) {
   20:             $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
   21:                 host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
   22:             }
   23:         }
   24:         let total = ProcessInfo.processInfo.physicalMemory
   25:         var free: UInt64 = 0, active: UInt64 = 0, inactive: UInt64 = 0, wired: UInt64 = 0, compressed: UInt64 = 0
   26:         if kr == KERN_SUCCESS {
```

### `Tests/ForgeConductorTests/ContinuityTests.swift:1767` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
 1760:         throw SQLiteFixtureError.failure(message)
 1761:     }
 1762:     defer { sqlite3_close(database) }
 1763:     try body(database)
 1764: }
 1765: 
 1766: private func executeSQLiteFixture(_ database: OpaquePointer, sql: String) throws {
 1767:     var errorMessage: UnsafeMutablePointer<CChar>?
 1768:     guard sqlite3_exec(database, sql, nil, nil, &errorMessage) == SQLITE_OK else {
 1769:         let message = errorMessage.map { String(cString: $0) } ?? String(cString: sqlite3_errmsg(database))
 1770:         sqlite3_free(errorMessage)
 1771:         throw SQLiteFixtureError.failure(message)
 1772:     }
 1773: }
 1774: 
```

### `Tests/ForgeConductorTests/RealtimeStreamTests.swift:140` — Unsafe(?:Mutable)?(?:Raw)?Pointer\|\.allocate\s*\(\|\.deallocate\s*\(\|Unmanaged\|IOObjectRelease\|CFRelease

```swift
  133: 
  134:         var addr = sockaddr_in()
  135:         addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
  136:         addr.sin_family = sa_family_t(AF_INET)
  137:         addr.sin_port = in_port_t(UInt16(port).bigEndian)
  138:         addr.sin_addr = in_addr(s_addr: inet_addr(host))
  139: 
  140:         let connectResult = withUnsafePointer(to: &addr) {
  141:             $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
  142:                 connect(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
  143:             }
  144:         }
  145:         guard connectResult == 0 else {
  146:             throw NSError(domain: "sse", code: 2, userInfo: [NSLocalizedDescriptionKey: "connect failed errno=\(errno)"])
  147:         }
```

## Resource-owner/cleanup matrix

| Owner | Kind | Resource | Location | Resource lines | Cleanup lines | Process lifetime |
|---|---|---|---|---|---|---:|
| `AppModel` | class | Task | `Sources/ForgeConductorApp/AppModel.swift` | 206, 208, 248, 395, 427, 458, 501, 524 | 170, 232 | False |
| `AppModel` | class | Timer | `Sources/ForgeConductorApp/AppModel.swift` | 233 | 170, 232 | False |
| `AppTelemetryBinding` | class | Task | `Sources/ForgeConductorApp/AppTelemetryBinding.swift` | 48, 73, 75 | — | False |
| `ForgeConductorMain` | enum | Process | `Sources/ForgeConductorCLI/ForgeConductorMain.swift` | 401 | — | False |
| `ForgeConductorMain` | enum | Pipe/FileHandle | `Sources/ForgeConductorCLI/ForgeConductorMain.swift` | 416, 422 | — | False |
| `ForgeConductorMain` | enum | DispatchSource | `Sources/ForgeConductorCLI/ForgeConductorMain.swift` | 224, 225 | — | False |
| `ContextContinuityService` | class | Task | `Sources/ForgeConductorCore/Application/ContextContinuityService.swift` | 609 | — | False |
| `ToolRouter` | class | AsyncStream | `Sources/ForgeConductorCore/Application/ToolRouter.swift` | 94 | — | False |
| `DashboardServer` | class | DispatchSource | `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift` | 161, 162 | 133, 138, 151, 183, 188 | False |
| `SSEStreamSession` | class | Timer | `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift` | 156, 211 | 326, 337 | False |
| `SSEStreamSession` | class | DispatchSource | `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift` | 156, 211 | 326, 337 | False |
| `HandoffPacket` | struct | Task | `Sources/ForgeConductorCore/Domain/HandoffPacket.swift` | 92 | — | False |
| `AuditService` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/Infrastructure/AuditService.swift` | 78 | 79 | False |
| `DashboardPortGuard` | enum | Process | `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift` | 103, 112 | — | False |
| `DashboardPortGuard` | enum | Pipe/FileHandle | `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift` | 107, 109 | — | False |
| `DiagnosticLog` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift` | 188 | 189 | False |
| `ProcessRunner` | class | Process | `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift` | 67, 274, 282 | 282, 307 | False |
| `ProcessRunner` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift` | 91, 92, 95, 115, 139, 146, 165, 259, 262 | 146, 319, 320, 337, 338 | False |
| `MCPServeVerifier` | enum | Process | `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift` | 79 | 130 | False |
| `MCPServeVerifier` | enum | Pipe/FileHandle | `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift` | 91, 92, 93, 242, 250 | 111 | False |
| `MCPServer` | class | Timer | `Sources/ForgeConductorCore/MCP/MCPServer.swift` | 50 | 56 | False |
| `MCPServer` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/MCP/MCPServer.swift` | 33, 457 | — | False |
| `MCPServer` | class | DispatchSource | `Sources/ForgeConductorCore/MCP/MCPServer.swift` | 50 | 56 | False |
| `MCPStreamReader` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/MCP/MCPServer.swift` | 482, 486 | — | False |
| `ManagerInstaller` | class | Pipe/FileHandle | `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift` | 1187 | 1188 | False |
| `ManagerNode` | class | Timer | `Sources/ForgeConductorCore/Manager/ManagerNode.swift` | 346 | 34, 363 | False |
| `ManagerNode` | class | DispatchSource | `Sources/ForgeConductorCore/Manager/ManagerNode.swift` | 346, 447, 448 | 34, 363 | False |
| `ManagerRuntime` | class | Timer | `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift` | 18 | — | False |
| `ManagerRuntime` | class | DispatchSource | `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift` | 18 | — | False |
| `RealtimeMetricsEngine` | class | Timer | `Sources/ForgeConductorCore/Telemetry/RealtimeMetricsEngine.swift` | 31, 87 | 104 | False |
| `RealtimeMetricsEngine` | class | DispatchSource | `Sources/ForgeConductorCore/Telemetry/RealtimeMetricsEngine.swift` | 31, 87 | 104 | False |
| `TelemetryService` | class | Timer | `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift` | 28, 75 | 100 | False |
| `TelemetryService` | class | DispatchSource | `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift` | 28, 75 | 100 | False |
| `MCPProcessFixture` | struct | Process | `Tests/ForgeConductorTests/ContinuityTests.swift` | 1800 | — | False |
| `MCPProcessFixture` | struct | Pipe/FileHandle | `Tests/ForgeConductorTests/ContinuityTests.swift` | 1801, 1802, 1803 | — | False |
| `MCPProtocolAndDiagnosticsTests` | class | Pipe/FileHandle | `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift` | 25 | 28 | False |
| `ManagerTests` | class | Task | `Tests/ForgeConductorTests/ManagerTests.swift` | 265 | — | False |
