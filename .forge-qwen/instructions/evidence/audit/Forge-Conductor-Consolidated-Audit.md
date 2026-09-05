# Forge Conductor — Consolidated Evidence-Based Audit

**Generated:** 2026-08-23T14:03:36.634643+00:00  
**Original archive SHA-256:** `07a0ea359ad93930c5b29d7e368cfe8fdee226f60248b5cd3c19c675372c270e`  
**Repository root:** `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`  
**Swift files analyzed:** 112  
**Swift lines analyzed:** 30,335  
**Promoted findings:** 29  

## Executive determination

The source proves two primary architectural causes in the reported telemetry/gauge path:

1. **Telemetry UI delivery is not bounded by backpressure.** A recurring producer can enqueue fresh MainActor tasks carrying snapshots faster than the UI actor applies them. A slow or blocked UI therefore retains pending tasks and payloads until the backlog drains.
2. **Gauge rendering scales native Metal ownership and recurring work per gauge.** Individual MTKView instances construct command/pipeline resources, render on their own cadence, and the update path can allocate replacement buffers. A separate SwiftUI animation clock compounds the work.

These are deterministic source behaviors. Live object counts, retained paths, CPU/GPU cost, and before/after leak closure still require a native macOS Xcode/Instruments run. The remaining findings are tiered to preserve that distinction.

### Evidence tiers

- **Tier A:** deterministic source behavior directly explaining queue growth or resource scaling.
- **Tier B:** deterministic operation/retaining edge; runtime reachability or a product invariant closes impact.
- **Tier C:** owner-local cleanup/bound absent; external cleanup may exist and must be traced.

## Consolidated findings

| ID | Tier | Severity | Finding | Location | Status |
|---|---|---:|---|---|---|
| `FC-MEM-001` | A — deterministic source behavior | **Critical** | Telemetry delivery can accumulate unbounded pending MainActor tasks | `Sources/ForgeConductorApp/AppTelemetryBinding.swift:48` | Source-proven queueing behavior; live growth magnitude requires macOS profiling |
| `FC-PERF-001` | A — deterministic source behavior | **Critical** | Each gauge MTKView owns an independent command queue/pipeline and recurring render cadence | `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:80` | Source-proven resource topology; device-specific CPU/GPU cost requires Instruments |
| `FC-PERF-001` | A — deterministic source behavior | **Critical** | Each gauge MTKView owns an independent command queue/pipeline and recurring render cadence | `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:40` | Source-proven resource topology; device-specific CPU/GPU cost requires Instruments |
| `FC-PERF-002` | A — deterministic source behavior | **High** | Gauge update path allocates replacement Metal buffers | `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:141` | Source-proven allocation site |
| `FC-PERF-003` | A — deterministic source behavior | **High** | Gauge hierarchy has an animation clock independent of telemetry and MTKView drawing | `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:17` | Source-proven recurring invalidation |
| `FC-TELEM-DUPLICATE` | B — deterministic duplicate mapping; product invariant required | **Medium** | Distinct telemetry cases return the same source expression | `Sources/ForgeConductorApp/AppModel.swift:73` | Source-proven duplicate expression; may be intentional |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | AppTelemetryBinding owns/uses Task without an owner-local cleanup marker | `Sources/ForgeConductorApp/AppTelemetryBinding.swift:48` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ForgeConductorMain owns/uses DispatchSource without an owner-local cleanup marker | `Sources/ForgeConductorCLI/ForgeConductorMain.swift:224` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ForgeConductorMain owns/uses Process without an owner-local cleanup marker | `Sources/ForgeConductorCLI/ForgeConductorMain.swift:401` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ForgeConductorMain owns/uses Pipe/FileHandle without an owner-local cleanup marker | `Sources/ForgeConductorCLI/ForgeConductorMain.swift:416` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`missing\` appends without an owner-local bound | `Sources/ForgeConductorCore/Application/AgentSessionService.swift:198` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`snaps\` appends without an owner-local bound | `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:436` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`merged\` appends without an owner-local bound | `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:469` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ContextContinuityService owns/uses Task without an owner-local cleanup marker | `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:609` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`suffix\` appends without an owner-local bound | `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:218` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ToolRouter owns/uses AsyncStream without an owner-local cleanup marker | `Sources/ForgeConductorCore/Application/ToolRouter.swift:94` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | HandoffPacket owns/uses Task without an owner-local cleanup marker | `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:92` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | DashboardPortGuard owns/uses Process without an owner-local cleanup marker | `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:103` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | DashboardPortGuard owns/uses Pipe/FileHandle without an owner-local cleanup marker | `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:107` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`predicates\` appends without an owner-local bound | `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:279` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`out\` appends without an owner-local bound | `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:317` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-MEM-COLLECTION` | C — owner-local bound absent; cross-owner reset may exist | **High** | Long-lived collection \`closed\` appends without an owner-local bound | `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:551` | Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | MCPServer owns/uses Pipe/FileHandle without an owner-local cleanup marker | `Sources/ForgeConductorCore/MCP/MCPServer.swift:33` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | MCPStreamReader owns/uses Pipe/FileHandle without an owner-local cleanup marker | `Sources/ForgeConductorCore/MCP/MCPServer.swift:482` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ManagerRuntime owns/uses Timer without an owner-local cleanup marker | `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:18` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ManagerRuntime owns/uses DispatchSource without an owner-local cleanup marker | `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:18` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | MCPProcessFixture owns/uses Process without an owner-local cleanup marker | `Tests/ForgeConductorTests/ContinuityTests.swift:1800` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | MCPProcessFixture owns/uses Pipe/FileHandle without an owner-local cleanup marker | `Tests/ForgeConductorTests/ContinuityTests.swift:1801` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |
| `FC-LIFE-GAP` | C — owner-local cleanup signal absent; external cleanup may exist | **High** | ManagerTests owns/uses Task without an owner-local cleanup marker | `Tests/ForgeConductorTests/ManagerTests.swift:265` | Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak |

### FC-MEM-001 — Telemetry delivery can accumulate unbounded pending MainActor tasks

- **Tier:** A — deterministic source behavior
- **Severity:** Critical
- **Location:** `Sources/ForgeConductorApp/AppTelemetryBinding.swift:48`
- **Status:** Source-proven queueing behavior; live growth magnitude requires macOS profiling
- **Evidence-based basis:** The recurring telemetry delivery path creates a fresh unstructured MainActor task that captures the update payload. No pending-task/latest-value gate is visible in the surrounding source. When production outruns MainActor consumption, queued tasks and their captured snapshots accumulate until serviced.
- **Required validation:** Instrument produced, scheduled, applied, dropped/coalesced, and maximum pending depth; stall MainActor for three seconds while telemetry continues.

```swift
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
   51:                 self.apply(frame)
   52:             }
   53:         }
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
   69:         if isLoading && !force { return }
   70:         objectWillChange.send()
   71:         isLoading = true
   72:         updateSubject.send()
```

### FC-PERF-001 — Each gauge MTKView owns an independent command queue/pipeline and recurring render cadence

- **Tier:** A — deterministic source behavior
- **Severity:** Critical
- **Location:** `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:80`
- **Status:** Source-proven resource topology; device-specific CPU/GPU cost requires Instruments
- **Evidence-based basis:** The native gauge view/representable constructs Metal command-queue and pipeline resources in its per-view object graph and configures recurring drawing. Cost therefore scales with the number of simultaneously instantiated gauge views.
- **Required validation:** Record live MTKView, MTLCommandQueue, pipeline-state, and GPU work for 1, 10, 100, and the full gauge count; repeat after hiding and dismissing the screen.

```swift
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

  154:         // MTKView has no sensible intrinsic size; without bounds it reports huge
  155:         // preferred sizes and blows out SwiftUI headers/rows.
  156:         v.translatesAutoresizingMaskIntoConstraints = true
  157:         v.autoResizeDrawable = true
  158:         v.framebufferOnly = true
  159:         v.isPaused = false
  160:         v.enableSetNeedsDisplay = false
  161:         v.setContentHuggingPriority(.defaultLow, for: .horizontal)
  162:         v.setContentHuggingPriority(.required, for: .vertical)
  163:         v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
  164:         v.setContentCompressionResistancePriority(.required, for: .vertical)

  155:         // preferred sizes and blows out SwiftUI headers/rows.
  156:         v.translatesAutoresizingMaskIntoConstraints = true
  157:         v.autoResizeDrawable = true
  158:         v.framebufferOnly = true
  159:         v.isPaused = false
  160:         v.enableSetNeedsDisplay = false
  161:         v.setContentHuggingPriority(.defaultLow, for: .horizontal)
  162:         v.setContentHuggingPriority(.required, for: .vertical)
  163:         v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
  164:         v.setContentCompressionResistancePriority(.required, for: .vertical)
  165:         context.coordinator.attach(v)

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
```

### FC-PERF-001 — Each gauge MTKView owns an independent command queue/pipeline and recurring render cadence

- **Tier:** A — deterministic source behavior
- **Severity:** Critical
- **Location:** `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:40`
- **Status:** Source-proven resource topology; device-specific CPU/GPU cost requires Instruments
- **Evidence-based basis:** The native gauge view/representable constructs Metal command-queue and pipeline resources in its per-view object graph and configures recurring drawing. Cost therefore scales with the number of simultaneously instantiated gauge views.
- **Required validation:** Record live MTKView, MTLCommandQueue, pipeline-state, and GPU work for 1, 10, 100, and the full gauge count; repeat after hiding and dismissing the screen.

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
```

### FC-PERF-002 — Gauge update path allocates replacement Metal buffers

- **Tier:** A — deterministic source behavior
- **Severity:** High
- **Location:** `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:141`
- **Status:** Source-proven allocation site
- **Evidence-based basis:** The SwiftUI/native update path reaches makeBuffer, so model updates allocate a new MTLBuffer rather than writing to stable storage. At telemetry/display cadence this is deterministic allocation churn.
- **Required validation:** Use Allocations filtered to MTLBuffer creation and compare with a persistent-buffer implementation updated through contents().

```swift
  123:                     finishSegment()
  124:                     segmentStart = nil
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
  140:         let bytes = verts.count * MemoryLayout<V>.stride
  141:         vertexBuffer = device.makeBuffer(bytes: verts, length: max(bytes, 16), options: .storageModeShared)
  142:         lock.lock()
  143:         self.drawGrid = gridCount
  144:         self.drawFillStart = fillStart
  145:         self.drawFillCount = fillCount
  146:         self.drawLines = lineRanges
  147:         lock.unlock()
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
```

### FC-PERF-003 — Gauge hierarchy has an animation clock independent of telemetry and MTKView drawing

- **Tier:** A — deterministic source behavior
- **Severity:** High
- **Location:** `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:17`
- **Status:** Source-proven recurring invalidation
- **Evidence-based basis:** TimelineView(.animation) requests recurring SwiftUI updates. In this gauge hierarchy it compounds telemetry delivery and native-view drawing even when sampled values do not change.
- **Required validation:** Use SwiftUI Instruments to compare body-update counts with the timeline removed or scoped to the smallest animated primitive.

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

### FC-TELEM-DUPLICATE — Distinct telemetry cases return the same source expression

- **Tier:** B — deterministic duplicate mapping; product invariant required
- **Severity:** Medium
- **Location:** `Sources/ForgeConductorApp/AppModel.swift:73`
- **Status:** Source-proven duplicate expression; may be intentional
- **Evidence-based basis:** Cases .agents, .diagnostics, .feed, .manager, .mcp, .rig, .tools all return `""`.
- **Required validation:** Add a mapping contract test proving whether the shared source is intentional; otherwise correct the wrong case.

```swift
   59: 
   60:     public enum AppTab: String, CaseIterable, Identifiable {
   61:         case rig = "FORGE RIG"
   62:         case mcp = "LM Studio MCP"
   63:         case agents = "Agents"
   64:         case tools = "Tools"
   65:         case feed = "Live Feed"
   66:         case diagnostics = "Diagnostics"
   67:         case manager = "Manager"
   68: 
   69:         public var id: String { rawValue }
   70: 
   71:         public var accessibilityID: String {
   72:             switch self {
   73:             case .rig: return "rig"
   74:             case .mcp: return "mcp"
   75:             case .agents: return "agents"
   76:             case .tools: return "tools"
   77:             case .feed: return "feed"
   78:             case .diagnostics: return "diagnostics"
   79:             case .manager: return "manager"
   80:             }
   81:         }
   82:     }
   83: 
   84:     public init() {
   85:         bootstrap()
   86:         startManagerPoll()
   87:         bindTelemetryMirror()
```

### FC-LIFE-GAP — AppTelemetryBinding owns/uses Task without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorApp/AppTelemetryBinding.swift:48`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class AppTelemetryBinding contains Task ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

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
   51:                 self.apply(frame)
   52:             }
   53:         }
   54: 
   55:         // One forge seed so MCP/agent cards appear immediately; host already streaming.
   56:         seedForgeOnce()
   57:     }
   58: 
   59:     public func detach() {
   60:         if let id = frameListenerID, let app {
```

### FC-LIFE-GAP — ForgeConductorMain owns/uses DispatchSource without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCLI/ForgeConductorMain.swift:224`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** enum ForgeConductorMain contains DispatchSource ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
  212:         fputs("Prefer: forge-conductor manager run --open\n", stderr)
  213:         if openBrowser {
  214:             let runner = ProcessRunner()
  215:             _ = try? runner.run(
  216:                 executable: "/usr/bin/open",
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
  232:     }
  233: 
  234:     // MARK: - Manager
  235: 
  236:     static func cmdManager(_ args: [String]) throws {
```

### FC-LIFE-GAP — ForgeConductorMain owns/uses Process without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCLI/ForgeConductorMain.swift:401`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** enum ForgeConductorMain contains Process ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
  389:             print("  url: http://\(app.config.string("dashboard", "host", default: "127.0.0.1")):\(app.config.int("dashboard", "port", default: 7788))/")
  390:             return
  391:         }
  392: 
  393:         // Prefer installed stable binary so EP allowlists target a fixed path.
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
  409:         }
  410:         process.arguments = childArgs
  411: 
  412:         try app.paths.ensureLayout()
  413:         if !FileManager.default.fileExists(atPath: app.paths.managerLog.path) {
```

### FC-LIFE-GAP — ForgeConductorMain owns/uses Pipe/FileHandle without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCLI/ForgeConductorMain.swift:416`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** enum ForgeConductorMain contains Pipe/FileHandle ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
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

### FC-MEM-COLLECTION — Long-lived collection \`missing\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/AgentSessionService.swift:198`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `missing` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  182:         let reportObj = report ?? [:]
  183:         let runBody = try store.memoryGet(key: "agent_run/\(sessionID.rawValue)")
  184:         var schema: [String] = []
  185:         var goal = ""
  186:         if let runBody,
  187:            let run = try? JSONSupport.object(from: Data(runBody.utf8)) {
  188:             schema = run["output_schema"] as? [String] ?? []
  189:             goal = run["goal"] as? String ?? ""
  190:         }
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
  206:             "report": reportObj,
  207:             "missing_schema_keys": missing,
  208:         ]
  209:         let summary = try JSONSupport.string(from: summaryObj)
  210:         let closed = try store.sessionEnd(id: sessionID, summary: String(summary.prefix(4000)))
  211: 
  212:         if let clientID {
  213:             try clearBinding(clientID: clientID, sessionID: sessionID)
  214:         }
```

### FC-MEM-COLLECTION — Long-lived collection \`snaps\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:436`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `snaps` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  420:             if seen.contains(s.id.rawValue) { continue }
  421:             seen.insert(s.id.rawValue)
  422: 
  423:             var goal = ""
  424:             var cwd: String?
  425:             if let body = try? store.memoryGet(key: "agent_run/\(s.id.rawValue)"),
  426:                let data = body.data(using: .utf8),
  427:                let obj = try? JSONSupport.object(from: data) {
  428:                 goal = obj["goal"] as? String ?? ""
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
  444:                     resumeHint:
  445:                         "agent_run_status(session_id: \"\(s.id.rawValue)\"); " +
  446:                         "if stale, agent_run_complete then agent_run_start with same goal/cwd"
  447:                 )
  448:             )
  449:         }
  450:         return snaps
  451:     }
  452: 
```

### FC-MEM-COLLECTION — Long-lived collection \`merged\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:469`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `merged` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
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
  480:     }
  481: 
  482:     private func mutateAndPersist(_ mutation: () throws -> HandoffPacket) throws -> PersistenceOutcome {
  483:         guard lock.lock(before: Date().addingTimeInterval(Self.persistenceLockTimeout)) else {
  484:             throw posixPersistenceError(operation: "lock continuity service", code: EBUSY)
  485:         }
```

### FC-LIFE-GAP — ContextContinuityService owns/uses Task without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:609`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class ContextContinuityService contains Task ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
  597:         return NSError(
  598:             domain: NSPOSIXErrorDomain,
  599:             code: Int(code),
  600:             userInfo: [
  601:                 NSLocalizedDescriptionKey:
  602:                     "Failed to \(operation): \(String(cString: strerror(code)))",
  603:             ]
  604:         )
  605:     }
  606: 
  607:     private func projectCurrentTask(_ packet: HandoffPacket) throws {
  608:         var md = """
  609:         # Current Task
  610: 
  611:         **Status:** \(packet.status)
  612:         **Handoff id:** `\(packet.id)`
  613:         **Source:** \(packet.source.rawValue)
  614:         **Resume ready:** \(packet.resumeReady)
  615:         """
  616:         if let slug = packet.projectSlug { md += "\n**Project slug:** \(slug)" }
  617:         if let cwd = packet.cwd { md += "\n**Workspace / cwd:** \(cwd)" }
  618:         md += "\n\n## Goal\n\n\(packet.goal.isEmpty ? "_(not set)_" : packet.goal)\n"
  619:         if !packet.nextActions.isEmpty {
  620:             md += "\n## Next actions\n\n"
  621:             for a in packet.nextActions { md += "- [ ] \(a)\n" }
```

### FC-MEM-COLLECTION — Long-lived collection \`suffix\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:218`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `suffix` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  202: 
  203:     private func resolve(_ raw: String, relativeTo base: URL) -> URL {
  204:         let expanded = (raw as NSString).expandingTildeInPath
  205:         if expanded.hasPrefix("/") {
  206:             return URL(fileURLWithPath: expanded).standardizedFileURL
  207:         }
  208:         return base.appendingPathComponent(expanded).standardizedFileURL
  209:     }
  210: 
  211:     /// Resolve symlinks in the deepest existing ancestor, then append any
  212:     /// not-yet-created path suffix. This prevents writes through a symlink that
  213:     /// points outside an allowed root.
  214:     private func canonicalURL(_ url: URL) -> URL {
  215:         var existing = url.standardizedFileURL
  216:         var suffix: [String] = []
  217:         while !fileManager.fileExists(atPath: existing.path), existing.path != "/" {
  218:             suffix.insert(existing.lastPathComponent, at: 0)
  219:             existing.deleteLastPathComponent()
  220:         }
  221:         var resolved = existing.resolvingSymlinksInPath().standardizedFileURL
  222:         for component in suffix {
  223:             resolved.appendPathComponent(component)
  224:         }
  225:         return resolved.standardizedFileURL
  226:     }
  227: 
  228:     private func contains(_ candidate: URL, root: URL) -> Bool {
  229:         let candidateComponents = candidate.standardizedFileURL.pathComponents
  230:         let rootComponents = root.standardizedFileURL.pathComponents
  231:         guard candidateComponents.count >= rootComponents.count else { return false }
  232:         return Array(candidateComponents.prefix(rootComponents.count)) == rootComponents
  233:     }
  234: 
```

### FC-LIFE-GAP — ToolRouter owns/uses AsyncStream without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Application/ToolRouter.swift:94`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class ToolRouter contains AsyncStream ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
   82:                 start: start,
   83:                 status: "error",
   84:                 auditError: blocked.payload["code"] as? String,
   85:                 mutating: false
   86:             )
   87:         }
   88:         let loopCount = isContinuity
   89:             ? 0
   90:             : recordIdenticalCall(tool: name, arguments: routedArguments, clientID: clientID)
   91: 
   92:         if !isContinuity, loopCount > Self.budgetIdenticalCalls {
   93:             // Hard stop runs before either denial return or dispatch. The repeated
   94:             // tool therefore cannot execute, and continuation is never advertised
   95:             // unless its resume-ready handoff was durably stored.
   96:             let result = hardLoopResult(
   97:                 tool: name,
   98:                 loopCount: loopCount,
   99:                 clientID: clientID,
  100:                 authorizationDenialCode: authorizationDenial?.code
  101:             )
  102:             return recordAndReturn(
  103:                 result,
  104:                 tool: name,
  105:                 arguments: routedArguments,
  106:                 clientID: clientID,
```

### FC-LIFE-GAP — HandoffPacket owns/uses Task without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:92`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** struct HandoffPacket contains Task ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
   80:     public static let schemaVersion = 1
   81:     public static let maxNarrativeChars = 4_000
   82: 
   83:     public var id: String
   84:     public var schemaVersion: Int
   85:     public var createdAt: String
   86:     public var updatedAt: String
   87:     public var source: HandoffSource
   88:     public var resumeReady: Bool
   89:     public var chatLabel: String?
   90:     public var clientID: String?
   91: 
   92:     // Task
   93:     public var goal: String
   94:     public var status: String
   95:     public var projectSlug: String?
   96:     public var cwd: String?
   97:     public var blockers: [String]
   98:     public var nextActions: [String]
   99: 
  100:     // Working set
  101:     public var keyFiles: [String]
  102:     public var decisions: [String]
  103: 
  104:     // Agents
```

### FC-LIFE-GAP — DashboardPortGuard owns/uses Process without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:103`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** enum DashboardPortGuard contains Process ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
   91:                         if err == 0 { open = true }
   92:                     }
   93:                 }
   94:                 close(fd)
   95:                 if open { break }
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
  111:             try proc.run()
  112:             proc.waitUntilExit()
  113:         } catch {
  114:             return nil
  115:         }
```

### FC-LIFE-GAP — DashboardPortGuard owns/uses Pipe/FileHandle without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Infrastructure/DashboardPortGuard.swift:107`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** enum DashboardPortGuard contains Pipe/FileHandle ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
   95:                 if open { break }
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

### FC-MEM-COLLECTION — Long-lived collection \`predicates\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:279`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `predicates` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  263:                   let rowID = textCol(stmt, 0),
  264:                   let cstr = sqlite3_column_text(stmt, 1) else { return nil }
  265:             let text = String(cString: cstr)
  266:             guard let data = text.data(using: .utf8),
  267:                   let obj = try? JSONSupport.object(from: data),
  268:                   let packet = HandoffPacket.fromDictionary(obj),
  269:                   packet.id == rowID else { return nil }
  270:             return packet
  271:         }
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
  287:                   let cstr = sqlite3_column_text(stmt, 1) else { return nil }
  288:             let text = String(cString: cstr)
  289:             guard let data = text.data(using: .utf8),
  290:                   let obj = try? JSONSupport.object(from: data),
  291:                   let packet = HandoffPacket.fromDictionary(obj),
  292:                   packet.id == rowID else { return nil }
  293:             return packet
  294:         }
  295:     }
```

### FC-MEM-COLLECTION — Long-lived collection \`out\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:317`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `out` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  301: 
  302:     public func handoffListAll() throws -> [HandoffPacket] {
  303:         try handoffList(sql: "SELECT id, packet_json FROM context_handoffs ORDER BY write_sequence DESC")
  304:     }
  305: 
  306:     private func handoffList(sql: String) throws -> [HandoffPacket] {
  307:         try withStatement(sql) { stmt in
  308:             var out: [HandoffPacket] = []
  309:             while sqlite3_step(stmt) == SQLITE_ROW {
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
  325:     public func handoffRepairPointers() throws {
  326:         let timestamp = ISO8601.string(from: clock.now())
  327:         try withTransaction {
  328:             let latestID = try handoffIDUnlocked(resumeReadyOnly: false)
  329:             let resumeID = try handoffIDUnlocked(resumeReadyOnly: true)
  330:             try replaceContinuityPointerUnlocked(
  331:                 key: "continuity/latest",
  332:                 id: latestID,
  333:                 tags: ["continuity", "latest"],
```

### FC-MEM-COLLECTION — Long-lived collection \`closed\` appends without an owner-local bound

- **Tier:** C — owner-local bound absent; cross-owner reset may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift:551`
- **Status:** Source-proven append and absence of owner-local cap/eviction; cross-file reset must be traced
- **Evidence-based basis:** The long-lived owner mutates `closed` via append/insert, but its type block contains no count cap, truncation, removal, or clear operation.
- **Required validation:** Log collection count and high-water mark for a 30–60 minute session and across project/session release boundaries.

```swift
  535:             return out
  536:         }
  537:     }
  538: 
  539:     public func sessionCloseOpen(
  540:         for clientID: ClientID,
  541:         except: SessionID? = nil,
  542:         summary: String
  543:     ) throws -> [AgentSession] {
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
  559:     /// Maximum key length accepted by MCP memory tools (UTF-8 bytes).
  560:     public static let memoryKeyMaxBytes = 512
  561:     /// Maximum body length accepted by MCP memory tools (UTF-8 bytes).
  562:     public static let memoryBodyMaxBytes = 512 * 1024
  563:     /// Soft cap on list/search result rows.
  564:     public static let memoryQueryDefaultLimit = 50
  565:     public static let memoryQueryMaxLimit = 200
  566: 
  567:     public func memorySet(key: String, body: String, tags: [String] = []) throws {
```

### FC-LIFE-GAP — MCPServer owns/uses Pipe/FileHandle without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/MCP/MCPServer.swift:33`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class MCPServer contains Pipe/FileHandle ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
   21:         role: LMStudioConnectorRole = LMStudioConnectorRole(
   22:             environmentValue: ProcessInfo.processInfo.environment["FORGE_MCP_ROLE"]
   23:         )
   24:     ) {
   25:         self.app = app
   26:         self.clientID = clientID
   27:         self.role = role
   28:         self.deploymentID = ProcessInfo.processInfo.environment["FORGE_DEPLOYMENT_ID"]?
   29:             .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
   30:     }
   31: 
   32:     /// Blocking serve loop: read newline-delimited or Content-Length framed messages from stdin.
   33:     public func run(input: FileHandle = .standardInput, output: FileHandle = .standardOutput) throws {
   34:         // MCP hosts (LM Studio) attach via pipes. Fully-buffered stdout delays
   35:         // initialize responses until the buffer fills → host reports plugin timeout (~60s).
   36:         setvbuf(stdout, nil, _IONBF, 0)
   37:         setvbuf(stderr, nil, _IONBF, 0)
   38: 
   39:         app.diagnostics.info("mcp_serve_start", [
   40:             "client_id": clientID.rawValue,
   41:             "role": role.rawValue,
   42:             "host_kind": role.hostKind,
   43:             "deployment_id": deploymentID,
   44:         ])
   45:         // Best-effort presence; never block MCP handshake on a locked GUI store.
```

### FC-LIFE-GAP — MCPStreamReader owns/uses Pipe/FileHandle without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/MCP/MCPServer.swift:482`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class MCPStreamReader contains Pipe/FileHandle ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
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
  482:     private let handle: FileHandle
  483:     private let maximumMessageBytes: Int
  484:     private var buffer = Data()
  485: 
  486:     public init(handle: FileHandle, maximumMessageBytes: Int = 4 * 1024 * 1024) {
  487:         self.handle = handle
  488:         self.maximumMessageBytes = maximumMessageBytes
  489:     }
  490: 
  491:     public func readMessage() throws -> [String: Any]? {
  492:         while true {
  493:             if let msg = try extractMessage() {
  494:                 return msg
```

### FC-LIFE-GAP — ManagerRuntime owns/uses Timer without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:18`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class ManagerRuntime contains Timer ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
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

### FC-LIFE-GAP — ManagerRuntime owns/uses DispatchSource without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Sources/ForgeConductorCore/Manager/ManagerRuntime.swift:18`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class ManagerRuntime contains DispatchSource ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
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

### FC-LIFE-GAP — MCPProcessFixture owns/uses Process without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Tests/ForgeConductorTests/ContinuityTests.swift:1800`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** struct MCPProcessFixture contains Process ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
 1788:         defer { lock.unlock() }
 1789:         values.append(value)
 1790:     }
 1791: 
 1792:     var snapshot: [String] {
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
 1808:     case failed(String)
 1809: }
 1810: 
 1811: private func locateContinuityCLIBinary() -> URL? {
 1812:     let products = Bundle(for: ContinuityTests.self).bundleURL.deletingLastPathComponent()
```

### FC-LIFE-GAP — MCPProcessFixture owns/uses Pipe/FileHandle without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Tests/ForgeConductorTests/ContinuityTests.swift:1801`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** struct MCPProcessFixture contains Pipe/FileHandle ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
 1789:         values.append(value)
 1790:     }
 1791: 
 1792:     var snapshot: [String] {
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
 1808:     case failed(String)
 1809: }
 1810: 
 1811: private func locateContinuityCLIBinary() -> URL? {
 1812:     let products = Bundle(for: ContinuityTests.self).bundleURL.deletingLastPathComponent()
 1813:     let adjacent = products.appendingPathComponent("forge-conductor")
```

### FC-LIFE-GAP — ManagerTests owns/uses Task without an owner-local cleanup marker

- **Tier:** C — owner-local cleanup signal absent; external cleanup may exist
- **Severity:** High
- **Location:** `Tests/ForgeConductorTests/ManagerTests.swift:265`
- **Status:** Source-proven absence of matching cleanup syntax in this owner; not yet a proven live leak
- **Evidence-based basis:** class ManagerTests contains Task ownership/usage but no matching cleanup signal in the same type block.
- **Required validation:** Instrument owner/resource counts and exercise the intended release boundary ten times; trace any survivor in a memgraph.

```swift
  253:         Thread.sleep(forTimeInterval: 0.2)
  254:         let url = URL(string: "http://127.0.0.1:\(port)/api/manager/status")!
  255:         let live = try HTTPTestHelpers.fetchJSON(url)
  256:         XCTAssertEqual(live["service_active"] as? Bool, true)
  257:     }
  258: 
  259:     func testDashboardClientAttachesToExistingManager() async throws {
  260:         let app = try ForgeApp.bootstrap(home: home)
  261:         let port = Int.random(in: 29_000...39_000)
  262:         try app.config.update(["dashboard": ["port": port] as [String: Any]], save: true)
  263:         let node = ManagerNode(app: app)
  264:         _ = try node.startService()
  265:         try await Task.sleep(for: .milliseconds(150))
  266: 
  267:         let client = ManagerDashboardClient(host: "127.0.0.1", port: port)
  268:         let status = try await client.status()
  269:         XCTAssertTrue(status.ok)
  270:         XCTAssertTrue(status.serviceActive)
  271:         XCTAssertEqual(status.pid, ProcessInfo.processInfo.processIdentifier)
  272:         XCTAssertEqual(status.dashboardPort, port)
  273: 
  274:         let settings = try await client.settings()
  275:         XCTAssertEqual(settings.dashboardPort, port)
  276:         XCTAssertEqual(settings.dashboardHost, "127.0.0.1")
  277:     }
```

## Build/test availability summary

| Command | Exit | Timeout |
|---|---:|---:|
| `uname -a` | `0` | `False` |
| `swift --version` | `0` | `False` |
| `xcodebuild -version` | `127` | `False` |
| `git --version` | `0` | `False` |
| `swift package describe --type json` | `0` | `False` |
| `swift build` | `1` | `False` |
| `swift test --parallel` | `1` | `False` |
| `xcodebuild -project ForgeConductor.xcodeproj -list` | `127` | `False` |
| `xcodebuild -workspace ForgeConductor.xcworkspace -list` | `127` | `False` |
| `swiftc -frontend -parse <each Swift file>` | `0` | `False` |

Full command output is preserved in `Forge-Conductor-Build-Test-Summary.md` and the JSON evidence.

## Required native runtime validation

Use a Release-with-debug-symbols Xcode build and run identical scenarios: baseline idle; open/close the gauge window ten times; full gauge rig visible for ten minutes; hidden for ten minutes; start/stop every child process; repeated project/session switching; and a three-second MainActor stall while telemetry continues.

Capture Allocations, Leaks, SwiftUI, Time Profiler, Metal System Trace, and a `.memgraph` at each intended release boundary. Record live counts for telemetry bridge/view models, MTKView, command queues, pipeline states, buffers, tasks, timers, observer tokens, subscriptions, Process/Pipe/FileHandle objects, histories, and caches. For every surviving app-owned type, save a retain-path trace; lower aggregate memory alone is not proof of a fixed retaining edge.

## Remediation order supported by the evidence

1. Replace per-update unstructured MainActor task creation with a single pending delivery and latest-value storage, or an AsyncStream configured with `bufferingNewest(1)`. Prove pending depth cannot exceed the selected bound.
2. Collapse per-gauge Metal ownership into shared device/queue/pipeline resources or one renderer/canvas. Pause/stop hidden views and update persistent buffer contents rather than allocating replacements.
3. Repair Tier-B retaining edges and add deinit/resource-count lifecycle tests for each owner.
4. Make telemetry mappings contract-driven: fixed snapshot fixture → exact gauge value, units, normalization, and unavailable-data behavior.
5. Bound every history/cache/queue explicitly and emit current/high-water counts through unified logging/signposts.
6. Re-run the identical Instruments workflow and compare object paths/counts, pending delivery depth, body updates, CPU samples, and GPU work.

## Audit limits

This environment may compile/parse Swift but cannot provide a WindowServer/AppKit/Metal runtime or a macOS memgraph when Xcode is unavailable. Command output records the exact boundary. No live leak is claimed from a static pattern alone; Tier A claims are limited to deterministic queue/resource behavior visible in source.
