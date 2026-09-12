# Evidence index

Repository links are pinned. External documentation was checked September 10, 2026.

**W01** — [README.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/README.md)

Bootstrap-era public instructions; all-gates entry point.

**W02** — [AGENTS.md](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/AGENTS.md)

Old mission, WinUI/C++20 rules, shell-default conflict, all-hard-gates completion.

**W03** — [CMakeLists.txt](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/CMakeLists.txt)

Existing native libraries, CLI/manager/session-host targets, test targets; no WinUI target found.

**W04** — [CMakePresets.json](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/CMakePresets.json)

VS 2022/v143, SDK 26100, C++20, configure/build/test preset names.

**W05** — [cmake/ForgeForsettiExternal.cmake](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/cmake/ForgeForsettiExternal.cmake)

Missing clean-clone dependency; hashes; no download; BUILD_ALWAYS.

**W06** — [.forge-codex/state/baseline/p03-forsetti-source-lock.json](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/.forge-codex/state/baseline/p03-forsetti-source-lock.json)

Original dependency archive and tree digests, source inventory.

**W07** — [scripts/build.ps1](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/scripts/build.ps1)

Existing Target, Parallel, Fresh, Analyze and vcpkg behavior.

**W08** — [scripts/test.ps1](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/scripts/test.ps1)

Existing label-filtered CTest entry point.

**W09** — [scripts/package.ps1](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/scripts/package.ps1)

Unconditional packaging-not-implemented exception.

**W10** — [packaging/Package.appxmanifest.template](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/packaging/Package.appxmanifest.template)

Identity placeholders, version mismatch, GUI executable and CLI alias.

**W11** — [src/Bootstrap/main.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/Bootstrap/main.cpp)

Temporary output and trivial self-test; not proof the production CLI is this bootstrap.

**W12** — [src/Application/ContinuityAutomation.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/Application/ContinuityAutomation.cpp)

Context resolver plus independent progress/time rollover; coordinator actions.

**W13** — [include/ForgeConductor/Domain/ContinuityModels.h](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/include/ForgeConductor/Domain/ContinuityModels.h)

Durable state machine, handoff and context signals.

**W14** — [include/ForgeConductor/Domain/ConfigurationModels.h](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/include/ForgeConductor/Domain/ConfigurationModels.h)

Shell default false in ShellConfig; configuration surface.

**W15** — [src/Infrastructure/Windows/WinHttpLocalModelSessionTransport.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/Infrastructure/Windows/WinHttpLocalModelSessionTransport.cpp)

Custom session/create/bootstrap/query HTTP contract.

**W16** — [include/ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/include/ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h)

Default /v1/forge base path.

**W17** — [src/Hosts/SessionHost/SessionHostCompositionRoot.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/Hosts/SessionHost/SessionHostCompositionRoot.cpp)

Production composition uses LocalLogicalSessionTransport.

**W18** — [src/SessionHost/LocalLogicalSessionTransport.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/SessionHost/LocalLogicalSessionTransport.cpp)

Synthetic logical IDs, local acknowledgment and continuation scheduling.

**W19** — [src/Hosts/Cli/McpServeCompositionRoot.cpp](https://github.com/flynn33/forge-conductor-windows-edition/blob/3e67a03b6c64d9a105b677dfe41e413bddd7762c/src/Hosts/Cli/McpServeCompositionRoot.cpp)

Native tool/persistence/MCP composition and local logical session transport.

**M01** — [README.md](https://github.com/flynn33/Forge-Conductor-MacOS/blob/6c9c91da74d44a57812887dc71f5c1de7abeddf7/README.md)

Implemented UI/tool/runtime features and explicit open live-provider/release qualification.

**M02** — [Package.swift](https://github.com/flynn33/Forge-Conductor-MacOS/blob/6c9c91da74d44a57812887dc71f5c1de7abeddf7/Package.swift)

Native module/product layout.

**M03** — [Sources/ForgeConductorApp/OperatorConsole/ViewModels/ProviderViewModel.swift](https://github.com/flynn33/Forge-Conductor-MacOS/blob/6c9c91da74d44a57812887dc71f5c1de7abeddf7/Sources/ForgeConductorApp/OperatorConsole/ViewModels/ProviderViewModel.swift)

Offline save vs discovery/connection probe; endpoint/model/credential settings.

**M04** — [Sources/ForgeConductorApp/OperatorConsole/ViewModels/ProjectsViewModel.swift](https://github.com/flynn33/Forge-Conductor-MacOS/blob/6c9c91da74d44a57812887dc71f5c1de7abeddf7/Sources/ForgeConductorApp/OperatorConsole/ViewModels/ProjectsViewModel.swift)

Register/select/reset/relink, durable identity/generation and reconciliation.

**M05** — [Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift](https://github.com/flynn33/Forge-Conductor-MacOS/blob/6c9c91da74d44a57812887dc71f5c1de7abeddf7/Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift)

Real /v1/responses provider adapter, context_get and provider-originated acknowledgment.

**E01** — [official_documentation](https://lmstudio.ai/docs/developer/openai-compat/responses)

LM Studio Responses endpoint, actual response IDs, stateful follow-up and streaming.

**E02** — [official_documentation](https://lmstudio.ai/docs/developer/openai-compat/tools)

Function-tool request/execute/return loop; model-dependent support.

**E03** — [official_documentation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps)

Windows App SDK self-contained deployment.

**E04** — [official_documentation](https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview)

MSIX signing and trusted certificate requirement.

**D01** — [Fixed Forsetti upstream CMakeLists.txt](https://github.com/flynn33/Forsetti-Framework-Windows/blob/63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5/CMakeLists.txt)

Public fixed upstream recovery candidate. Retrieved CMakeLists.txt Git blob and SHA-256 match the required source file; complete candidate source tree still requires verification.
