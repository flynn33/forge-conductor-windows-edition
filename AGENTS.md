# Forge Conductor Windows Port — governing instructions

## Mission

Port Forge Conductor 0.9.0 from macOS to an installable Windows 11 application with complete behavioral feature parity, while adopting the attached Forsetti Framework for Windows and Forsetti Agentic Edition. The Windows product must be reliable, autonomous, resource-bounded, secure, and maintainable.

## Instruction precedence

Resolve conflicts in this exact order:

1. Direct human-owner requirements in the initiating request.
2. `governance/PORT_TASK_CONTRACT.json`.
3. Forsetti Agentic Edition governance copied under `governance/source/forsetti-agentic/`.
4. Forsetti Framework Windows public contracts and policies copied under `governance/source/forsetti-windows/`.
5. The attached macOS source as behavioral evidence.
6. This package's architecture, specifications, plans, and gates.
7. Local implementation preferences and inferred conventions.

Record every material conflict and resolution in `.forge-codex/state/decisions/`.

## Mandatory technology rules

- Production application and runtime code: object-oriented C++20.
- GUI: native WinUI 3 using C++/WinRT and Windows App SDK.
- Native platform services: Windows SDK, Win32, COM/WinRT, Direct3D 11, Direct2D, DirectWrite, Windows Composition, ETW/TraceLogging, DPAPI, WinHTTP, Windows Sockets, Task Scheduler, Deployment APIs, MSIX/App Installer.
- Build: MSVC, Visual Studio Build Tools/Visual Studio, CMake, MSBuild, CTest, vcpkg, PowerShell.
- Tests: native C++ test executables and Microsoft C++ Unit Test Framework or CTest-compatible native harnesses.
- JSON: nlohmann/json through the Forsetti-approved vcpkg path.
- SQLite: Windows SDK `winsqlite3`, wrapped behind RAII C++ interfaces.
- Optional neutral TypeScript is allowed only for precompiled static dashboard assets. It may not own business logic and may not create a Node runtime dependency in the installed product.
- PowerShell may be used for build, validation, packaging, installation, and the opt-in shell tool.
- Python is forbidden in the target repository, source, scripts, tests, generators, build, packaging, installer, and installed runtime.
- Do not introduce Electron, Qt, Java, a .NET application layer, a Node runtime, Boost, or unapproved non-Microsoft dependencies.

## Forsetti rules

- Treat `ForsettiCore`, `ForsettiPlatform`, and `ForsettiHostTemplate` as sealed.
- Do not patch framework internals.
- Use public headers and public extension interfaces only.
- Use the Windows profile `0.2.0`, manifest schema/template `1.1`.
- Create an application-owned Forsetti app module with a declared manifest and runtime requirements.
- Use interface-first design, constructor injection, and explicit composition roots.
- Make concrete classes `final` unless deliberate extension is documented.
- No hidden service locators, process-wide mutable globals, or singleton ownership of product state.
- Preserve one-way dependency direction.
- Do not create direct dependencies between independently registered Forsetti modules.
- Run the framework guardrails on Windows and retain the output.

## Product requirements

Preserve every current macOS feature and tool. The canonical feature inventory is `plans/feature-parity-matrix.tsv`; the canonical MCP inventory is `plans/mcp-tool-parity.json`.

Required product surfaces:

1. Forge Rig.
2. LM Studio MCP.
3. Agents.
4. Tools.
5. Feed.
6. Diagnostics.
7. Manager.

Required runtime surfaces:

- GUI process.
- CLI process.
- stdio MCP server mode.
- per-user manager process.
- native session-host adapter/plugin.
- project memory MCP.
- continuity MCP and autonomous rollover.
- optional loopback dashboard.
- installer/bootstrapper and MSIX package.

## Strict OOP requirements

- Domain objects must not depend on WinUI, Win32 handles, database APIs, or transport details.
- Application services depend only on abstract interfaces.
- Infrastructure classes implement interfaces and own resources through RAII.
- UI view models expose immutable snapshots and commands; views must not perform process, database, filesystem, network, or registry operations directly.
- Every asynchronous operation has an owner, cancellation mechanism, deadline, bounded queue, and shutdown path.
- Every native handle is owned by a typed RAII object.
- Every background thread, timer, callback, event token, named pipe, socket, process, job object, graphics resource, and database statement has a documented lifetime owner.
- Avoid inheritance for code reuse. Use interfaces for polymorphism and composition for reuse.
- Exceptions may not cross ABI, process, COM, or MCP boundaries. Convert them to typed error results.

## Autonomy requirements

- Do not ask the operator to choose routine implementation details.
- Resolve ambiguity using instruction precedence, source evidence, Windows conventions, and the smallest architecture preserving all features.
- Record material decisions as ADRs and proceed.
- A missing optional tool must trigger automatic discovery or installation through first-party tooling where legally and technically permitted.
- A missing production signing secret must not halt development. Generate a per-user development certificate, produce an installable development-signed package, and keep release signing parameterized through environment variables.
- A transient build, test, package, runtime, or service failure must be diagnosed, fixed, and rerun.
- A blocked phase must not erase successful work. Persist state, evidence, the blocker, and the next safe action; continue with independent phases when dependencies allow.
- Every Codex context rollover must create a durable handoff before ending the current session.
- A fresh Codex session must be able to resume solely from repository state.

## Evidence rules

Do not guess at parity, correctness, performance, leaks, or host behavior.

Every completion claim must identify:

- command or test executed;
- exact working directory;
- toolchain and configuration;
- exit code;
- output/evidence path;
- relevant binary hashes;
- measured result;
- gate satisfied;
- remaining limitations.

Source inspection can prove deterministic code structure. Runtime behavior requires runtime evidence.

## No feature loss

- Do not remove, hide, stub, rename, or silently weaken a macOS feature to make the port easier.
- Unsupported host behavior must be implemented through a native adapter/plugin or a Forge-owned logical session host.
- Preserve legacy MCP tools while adding project-scoped tools.
- Preserve foreign MCP configuration entries during deployment.
- Preserve user data through upgrades and uninstalls by default.
- Reset and purge operations must be explicit, scoped, transactional, and available in Settings and CLI.
- Do not claim parity while any parity row is `unknown`, `stub`, `not_tested`, or `failed`.

## Performance and memory

- Use the budgets in `plans/resource-budgets.json` as release gates.
- No unbounded task dispatch, callback backlog, collection, cache, history, log, process output, request queue, or render queue.
- Telemetry delivery uses a latest-value mailbox with capacity one.
- Graphics devices, command queues, factories, shaders, brushes, and immutable geometry are shared.
- Hidden or occluded views pause or sharply reduce work.
- Avoid polling when events are available.
- Use per-project LRU repository caching with explicit close/eviction.
- Stress-test repeated launch/close, project switching, MCP reconnects, process start/stop, continuity rollovers, installer operations, and GPU device loss.
- Do not call lower memory usage alone proof of a leak fix; establish ownership and repeat the same flow before and after.

## Security

- Bind manager/dashboard services to loopback only.
- Authenticate named-pipe and loopback clients to the current user.
- Derive workspace authority from canonical trusted roots; defend against reparse-point escapes and case/normalization confusion.
- Disable shell execution by default.
- Cap command time and output; use Job Objects to terminate process trees.
- Store secrets with DPAPI and never log them.
- Redact credentials before project memory writes.
- Treat imported databases, archives, JSON, and handoffs as hostile input.
- Never automate another application's GUI to simulate a supported session API.
- Do not weaken Windows Defender, SmartScreen, UAC, firewall, or execution policy globally.

## Required work sequence

1. Read all governing files and validate source hashes.
2. Create or confirm the task contract and project context.
3. Inventory macOS features, tools, schemas, tests, data, and behavior.
4. Inventory Forsetti public APIs and constraints.
5. Establish the target architecture and ADRs.
6. Scaffold a buildable Windows repository.
7. Execute phases in `plans/phases.json`.
8. Build after every coherent slice.
9. Run the smallest relevant tests, then full gates.
10. Debug failures to root cause; do not suppress tests.
11. Keep parity, evidence, decisions, risks, and documentation synchronized.
12. Run an independent Validator-role session.
13. Package, install, launch, repair, upgrade, uninstall, reinstall, and profile.
14. Complete only when every hard gate passes.

## Completion prohibition

Do not state that the project is complete merely because it builds, opens a window, lists tools, produces an MSIX, or passes unit tests. Completion requires every hard gate in `plans/gates.json` and every definition-of-done item in `docs/DEFINITION_OF_DONE.md`.

## Attribution prohibition

Do not add phrases or metadata such as “generated by,” “AI-generated,” “created with,” model/vendor names, co-author trailers, bot signatures, or automated-authorship badges to product artifacts. Internal `.forge-codex` orchestration files may identify Codex because they are not product attribution.
