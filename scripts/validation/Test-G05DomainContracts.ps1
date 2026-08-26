[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount)
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G05 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) "$Message (expected '$Expected', found '$Actual')"
}

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message, [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor [Text.RegularExpressions.RegexOptions]::IgnoreCase
    }
    Assert-True ([regex]::IsMatch($Text, $Pattern, $options)) $Message
}

function Assert-NoMatch {
    param([string]$Text, [string]$Pattern, [string]$Message, [switch]$CaseSensitive)
    $options = [Text.RegularExpressions.RegexOptions]::Multiline -bor
        [Text.RegularExpressions.RegexOptions]::Singleline
    if (-not $CaseSensitive) {
        $options = $options -bor [Text.RegularExpressions.RegexOptions]::IgnoreCase
    }
    Assert-True (-not [regex]::IsMatch($Text, $Pattern, $options)) $Message
}
function Get-MethodParameterSets {
    param([string]$Text, [string]$MethodName)
    $pattern = '\b' + [regex]::Escape($MethodName) +
        '\s*\((?<parameters>.*?)\)\s*noexcept\s*=\s*0\s*;'
    return @([regex]::Matches(
        $Text,
        $pattern,
        [Text.RegularExpressions.RegexOptions]::Singleline) | ForEach-Object {
            $_.Groups['parameters'].Value
        })
}


function Assert-Set {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualSorted = @($Actual | ForEach-Object { [string]$_ } | Sort-Object)
    $expectedSorted = @($Expected | ForEach-Object { [string]$_ } | Sort-Object)
    Assert-Exact $actualSorted.Count $expectedSorted.Count "$Message count"
    for ($index = 0; $index -lt $expectedSorted.Count; $index++) {
        Assert-Exact $actualSorted[$index] $expectedSorted[$index] "$Message item $index"
    }
}

function Read-Json {
    param([string]$Path)
    try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
    catch { throw "G05 assertion failed: invalid JSON at $Path - $($_.Exception.Message)" }
}

function Get-TreeSummary {
    param([string]$Root)
    $rootFull = (Resolve-Path -LiteralPath $Root).Path
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File)) {
        $paths.Add($file.FullName.Substring($rootFull.Length + 1).Replace('\', '/'))
    }
    $paths.Sort([StringComparer]::Ordinal)
    $rows = [Collections.Generic.List[string]]::new()
    $bytes = 0L
    foreach ($path in $paths) {
        $fullPath = Join-Path $rootFull $path.Replace('/', '\')
        $bytes += [long](Get-Item -LiteralPath $fullPath).Length
        $rows.Add($path + "`t" + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join "`n")
    }
}

$requiredFiles = @(
    'CMakeLists.txt',
    'Directory.Build.props',
    'include/ForgeConductor/Domain/Domain.h',
    'include/ForgeConductor/Contracts/AuthorityCapabilities.h',
    'include/ForgeConductor/Contracts/AuthorizedToolCall.h',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Contracts/IGraphicsServices.h',
    'include/ForgeConductor/Contracts/IInstallerDeploymentService.h',
    'include/ForgeConductor/Contracts/ILMStudioDeploymentService.h',
    'include/ForgeConductor/Contracts/ILMStudioEnvironment.h',
    'include/ForgeConductor/Contracts/IToolServices.h',
    'tests/Domain/DomainTests.cpp',
    'tests/Contracts/main.cpp',
    'tests/Contracts/FoundationTelemetryFakeContractTests.h',
    'tests/Contracts/FoundationTelemetryFakeContractTests.cpp',
    'tests/Contracts/GroupedFakeContractTests.h',
    'tests/Contracts/GroupedFakeContractTests.cpp',
    'tests/Contracts/ApplicationServiceFakeContractTests.h',
    'tests/Contracts/AuthorityAdjacentPlatformFakeContractTests.h',
    'tests/Contracts/McpCancellationContractTests.h',
    'tests/Contracts/RepositoryDiagnosticsManagerFakeContractTests.h',
    'tests/Fakes/ApplicationServiceFakes.h',
    'tests/Fakes/ExternalServiceFakes.h',
    'tests/Fakes/ProjectRepositoryFakes.h',
    'tests/Fakes/ToolServiceFakes.h',
    'tests/Architecture/P05HeaderSelfContainmentMain.cpp',
    'src/Hosts/Cli/CliCompositionRoot.h',
    'src/Hosts/Cli/CliCompositionRoot.cpp',
    'src/Hosts/Cli/main.cpp',
    '.forge-codex/state/decisions/P05-001-typed-results-operation-context-and-authority.md',
    '.forge-codex/state/decisions/P05-002-source-compatibility-and-windows-resource-bounds.md',
    '.forge-codex/state/decisions/P05-003-executable-owned-composition-roots.md',
    '.forge-codex/state/decisions/P05-004-interface-issued-capability-values.md',
    '.forge-codex/instructions/plans/resource-budgets.json'
)
foreach ($relativePath in $requiredFiles) {
    Assert-True (Test-Path -LiteralPath (Join-Path $WorkspaceRoot $relativePath.Replace('/', '\')) -PathType Leaf) "required P05 file is missing: $relativePath"
}

$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile($PSCommandPath, [ref]$tokens, [ref]$errors) | Out-Null
Assert-Exact $errors.Count 0 'G05 validator PowerShell parser errors'

$domainHeaderRoot = Join-Path $WorkspaceRoot 'include\ForgeConductor\Domain'
$domainSourceRoot = Join-Path $WorkspaceRoot 'src\Domain'
$contractsRoot = Join-Path $WorkspaceRoot 'include\ForgeConductor\Contracts'
$fakesRoot = Join-Path $WorkspaceRoot 'tests\Fakes'
$domainHeaders = @(Get-ChildItem -LiteralPath $domainHeaderRoot -File -Filter '*.h' | Sort-Object Name)
$domainSources = @(Get-ChildItem -LiteralPath $domainSourceRoot -File -Filter '*.cpp' | Sort-Object Name)
$contractHeaders = @(Get-ChildItem -LiteralPath $contractsRoot -File -Filter '*.h' | Sort-Object Name)
$fakeHeaders = @(Get-ChildItem -LiteralPath $fakesRoot -File -Filter '*.h' | Sort-Object Name)
$contractTestHeaders = @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'tests\Contracts') -File -Filter '*.h' | Sort-Object Name)
$contractTestSources = @(Get-ChildItem -LiteralPath (Join-Path $WorkspaceRoot 'tests\Contracts') -File -Filter '*.cpp' | Sort-Object Name)
Assert-Set @($domainHeaders.Name) @(
    'AgentModels.h','ConfigurationModels.h','ContinuityModels.h','DeploymentModels.h',
    'DiagnosticsModels.h','Domain.h','EnvironmentModels.h','Error.h',
    'FileSystemModels.h','GraphicsModels.h','Identifiers.h','LegacyMemoryModels.h',
    'ManagerModels.h','OperationContext.h','ProcessModels.h','ProjectMemoryModels.h',
    'ResourcePolicy.h','Result.h','TelemetryModels.h','ToolModels.h') 'Domain public header inventory'
Assert-Set @($domainSources.Name) @(
    'AgentModels.cpp','ConfigurationModels.cpp','ContinuityModels.cpp','DeploymentModels.cpp',
    'DiagnosticsModels.cpp','EnvironmentModels.cpp','FileSystemModels.cpp','GraphicsModels.cpp',
    'Identifiers.cpp','LegacyMemoryModels.cpp','ManagerModels.cpp','ProcessModels.cpp',
    'ProjectMemoryModels.cpp','ResourcePolicy.cpp','TelemetryModels.cpp','ToolModels.cpp') 'Domain implementation source inventory'
Assert-Set @($contractHeaders.Name) @(
    'AuthorityCapabilities.h','AuthorizedToolCall.h','Contracts.h','IAgentServices.h',
    'IConfigurationStore.h','IContinuityCoordinator.h','IDiagnosticsServices.h',
    'IFileSystemServices.h','IForgeApplicationLifecycle.h','IFoundationServices.h',
    'IGraphicsServices.h','IInstallerDeploymentService.h','ILatestValueMailbox.h',
    'ILegacyMemoryService.h','ILMStudioDeploymentService.h','ILMStudioEnvironment.h',
    'IManagerServices.h','IMcpServer.h','IMcpTransport.h','INativeToolServices.h',
    'IProcessSupervisor.h','IProjectMemoryService.h','ISecureStorage.h',
    'ISessionHostAdapter.h','ITelemetryService.h','IToolServices.h') 'Contracts public header inventory'
Assert-Set @($fakeHeaders.Name) @(
    'ApplicationServiceFakes.h','AtomicFileStoreFake.h','BoundaryFakeSupport.h',
    'BoundedFakeSupport.h','BoundedLatestValueMailboxFake.h','ConfigurationStoreFake.h',
    'ContinuityRepositoryFake.h','DeterministicResult.h','DeterministicWorkspaceAuthority.h',
    'DiagnosticsFakes.h','ExternalServiceFakes.h','FileSystemFake.h','FoundationFakes.h',
    'GitServiceFake.h','ManagerFakes.h','McpTransportFake.h','PdfServiceFake.h',
    'PlatformPathFakes.h','ProjectRepositoryFakes.h','RecordingContinuityCoordinator.h',
    'RecordingProcessSupervisor.h','RecordingProjectMemoryService.h',
    'RecordingSessionHostAdapter.h','SecureStorageFake.h','ShellServiceFake.h',
    'TelemetryFakes.h','TextSearchServiceFake.h','ToolServiceFakes.h') 'bounded deterministic fake header inventory'
Assert-Set @($contractTestHeaders.Name) @(
    'ApplicationServiceFakeContractTests.h','AuthorityAdjacentPlatformFakeContractTests.h',
    'AuthorityContextFakeContractTests.h','FoundationTelemetryFakeContractTests.h',
    'GroupedFakeContractTests.h','McpCancellationContractTests.h',
    'NativeToolBoundaryFakeContractTests.h','PlatformBoundaryFakeTestSupport.h',
    'PlatformStorageFakeContractTests.h','RepositoryDiagnosticsManagerFakeContractTests.h') 'standalone contract-test header inventory'
Assert-Set @($contractTestSources.Name) @(
    'FoundationTelemetryFakeContractTests.cpp','GroupedFakeContractTests.cpp','main.cpp') 'contract-test source inventory'

$productFiles = @($domainHeaders) + @($domainSources) + @($contractHeaders)
$productText = ($productFiles | ForEach-Object {
    "`n/* FILE: $($_.FullName) */`n" + (Get-Content -Raw -LiteralPath $_.FullName)
}) -join "`n"

$forbiddenInclude = '(?im)^\s*#\s*include\s*[<"][^>"]*(?:windows\.h|winrt[/\\]|microsoft\.ui|wil[/\\]|winsqlite3|sqlite3|winhttp|winsock|d3d|d2d|dxgi|dwrite|wrl|forsetti|nlohmann|boost|qt)'
Assert-NoMatch $productText $forbiddenInclude 'Domain/Contracts platform or third-party include leakage'
Assert-NoMatch $productText '\b(?:HANDLE|HKEY|HWND|HRESULT|DWORD|SOCKET|HINTERNET|IUnknown)\b' 'native handle/type leakage' -CaseSensitive
Assert-NoMatch $productText '(?:Forsetti::|nlohmann::|boost::|Microsoft\.UI)' 'platform or third-party namespace leakage' -CaseSensitive
Assert-NoMatch $productText 'std::(?:filesystem|wstring|wstream|istream|ostream)' 'platform path or stream-boundary leakage' -CaseSensitive
Assert-NoMatch $productText 'Result\s*<\s*void\s*\*\s*>' 'Result<void*> workaround is prohibited' -CaseSensitive
Assert-NoMatch $productText '(?m)^\s*#\s*(?:if|ifdef|ifndef).*\b(?:_WIN32|WIN32|WINAPI)\b' 'platform-conditional source branch'
Assert-NoMatch $productText '\b(?:WINAPI|__declspec)\b' 'platform calling convention or declaration leakage' -CaseSensitive
Assert-NoMatch $productText '(?:system_clock|steady_clock)::now\s*\(|random_device\s*\(|UuidCreate\s*\(|CoCreateGuid\s*\(|getenv\s*\(' 'ambient time/random/environment access in Domain or Contracts' -CaseSensitive
Assert-NoMatch $productText '(?:\bwinrt::|\bWindows::|\bGUID\b|\bBSTR\b|\bVARIANT\b|\bIInspectable\b)' 'WinRT or COM type leakage' -CaseSensitive
Assert-NoMatch $productText '\b(?:Domain::WorkspaceAuthority|Domain::AuthorizedPath|ToolAuthorizationDecision)\b' 'forgeable or stale authorization type remains' -CaseSensitive

$domainText = (@($domainHeaders) + @($domainSources) | ForEach-Object {
    Get-Content -Raw -LiteralPath $_.FullName
}) -join [Environment]::NewLine
Assert-NoMatch $domainText '(?:ForgeConductor[/\\]Contracts|ForgeConductor::Contracts|\bContracts::)' 'Domain must not depend on Contracts' -CaseSensitive
$domainUmbrella = Get-Content -Raw -LiteralPath (Join-Path $domainHeaderRoot 'Domain.h')
foreach ($header in @($domainHeaders | Where-Object Name -ne 'Domain.h')) {
    Assert-Match $domainUmbrella ('#include\s+"ForgeConductor/Domain/' + [regex]::Escape($header.Name) + '"') "Domain umbrella missing $($header.Name)" -CaseSensitive
}
$contractsUmbrella = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'Contracts.h')
foreach ($header in @($contractHeaders | Where-Object Name -ne 'Contracts.h')) {
    Assert-Match $contractsUmbrella ('#include\s+"ForgeConductor/Contracts/' + [regex]::Escape($header.Name) + '"') "Contracts umbrella missing $($header.Name)" -CaseSensitive
}

$testText = (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'tests\Domain\DomainTests.cpp')) +
    (($contractTestSources | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n") +
    (($contractTestHeaders | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n") +
    (($fakeHeaders | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n")
Assert-NoMatch $testText '(?:sleep_for|sleep_until|\bSleep\s*\(|random_device\s*\(|getenv\s*\(|std::filesystem)' 'P05 tests must not use ambient or nondeterministic facilities' -CaseSensitive
foreach ($fakeHeader in $fakeHeaders) {
    $fakeText = Get-Content -Raw -LiteralPath $fakeHeader.FullName
    foreach ($match in [regex]::Matches($fakeText, '(?m)^\s*class\s+(?<name>[A-Za-z_]\w*)[^\{;]*')) {
        Assert-Match $match.Value '\bfinal\b' "fake class $($match.Groups['name'].Value) in $($fakeHeader.Name) is not final" -CaseSensitive
    }
}
$contractText = ($contractHeaders | ForEach-Object {
    "`n/* FILE: $($_.FullName) */`n" + (Get-Content -Raw -LiteralPath $_.FullName)
}) -join "`n"
$requiredExternalInterfaces = @(
    'ILMStudioEnvironment',
    'ILMStudioDeploymentService',
    'IGraphicsDeviceService',
    'IRenderService',
    'IInstallerDeploymentService'
)
foreach ($interfaceName in $requiredExternalInterfaces) {
    Assert-Match $contractText ('\bclass\s+' + [regex]::Escape($interfaceName) + '\s*\{') "required abstract seam $interfaceName" -CaseSensitive
    Assert-Match $contractText ('\bclass\s+' + [regex]::Escape($interfaceName) + '\s*\{.*?\bvirtual\s+~' + [regex]::Escape($interfaceName) + '\s*\(\s*\)\s*=\s*default\s*;') "virtual destructor for $interfaceName" -CaseSensitive
}
$authorityCapabilitiesText = Get-Content -Raw -LiteralPath (
    Join-Path $contractsRoot 'AuthorityCapabilities.h')
$authorizedToolCallText = Get-Content -Raw -LiteralPath (
    Join-Path $contractsRoot 'AuthorizedToolCall.h')
$toolContractText = Get-Content -Raw -LiteralPath (
    Join-Path $contractsRoot 'IToolServices.h')
$contractMainText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\main.cpp')

$capabilitySpecifications = @(
    [ordered]@{
        name = 'WorkspaceAuthority'
        text = $authorityCapabilitiesText
        issuer = 'IWorkspaceAuthority'
        memberNames = @('authorityId_','projectId_','callerId_','trustedRoots_','intent_','grants_','denials_','shellEnabled_','generation_')
        members = @(
            'const\s+Domain::AuthorityId\s+authorityId_',
            'const\s+Domain::ProjectId\s+projectId_',
            'const\s+Domain::ClientId\s+callerId_',
            'const\s+std::vector\s*<\s*Domain::PathText\s*>\s+trustedRoots_',
            'const\s+Domain::FileAccess\s+intent_',
            'const\s+std::vector\s*<\s*Domain::FileAccess\s*>\s+grants_',
            'const\s+std::vector\s*<\s*Domain::FileAccess\s*>\s+denials_',
            'const\s+bool\s+shellEnabled_',
            'const\s+std::uint64_t\s+generation_')
    },
    [ordered]@{
        name = 'AuthorizedPath'
        text = $authorityCapabilitiesText
        issuer = 'IWorkspaceAuthority'
        memberNames = @('authorityId_','canonicalPath_','authorityRoot_','access_')
        members = @(
            'const\s+Domain::AuthorityId\s+authorityId_',
            'const\s+Domain::PathText\s+canonicalPath_',
            'const\s+Domain::PathText\s+authorityRoot_',
            'const\s+Domain::FileAccess\s+access_')
    },
    [ordered]@{
        name = 'AuthorizedToolCall'
        text = $authorizedToolCallText
        issuer = 'IToolAuthorizer'
        memberNames = @('request_','effect_','authorityId_','authorityGeneration_')
        members = @(
            'const\s+Domain::ToolCallRequest\s+request_',
            'const\s+Domain::ToolEffect\s+effect_',
            'const\s+Domain::AuthorityId\s+authorityId_',
            'const\s+std::uint64_t\s+authorityGeneration_')
    })
function Get-FinalClassBody {
    param([string]$Text, [string]$Name)

    $match = [regex]::Match(
        $Text,
        ('(?m)^[ \t]*class\s+' + [regex]::Escape($Name) +
         '\s+final\s*\{(?<body>[\s\S]*?)^[ \t]*\};[ \t]*\r?$'))
    Assert-True $match.Success "isolated class body for $Name"
    return $match.Groups['body'].Value
}

foreach ($capability in $capabilitySpecifications) {
    $name = [string]$capability.name
    $body = Get-FinalClassBody ([string]$capability.text) $name

    $accessLabels = @(
        [regex]::Matches($body, '(?m)^[ \t]*(?<name>public|private|protected):') |
            ForEach-Object { $_.Groups['name'].Value })
    Assert-Set $accessLabels @('public','private') "$name exact access sections"

    $publicMatch = [regex]::Match(
        $body,
        '(?ms)^[ \t]*public:\s*(?<body>.*?)^[ \t]*private:')
    Assert-True $publicMatch.Success "$name public section"
    Assert-NoMatch $publicMatch.Groups['body'].Value '(?m)^[ \t]*static\b' "$name public static factory is prohibited" -CaseSensitive

    $privateMatch = [regex]::Match(
        $body,
        '(?ms)^[ \t]*private:\s*(?<body>.*)$')
    Assert-True $privateMatch.Success "$name private section"
    $privateBody = $privateMatch.Groups['body'].Value

    $friends = @(
        [regex]::Matches($privateBody, '(?m)^[ \t]*friend\s+class\s+(?<name>[A-Za-z_]\w*)\s*;') |
            ForEach-Object { $_.Groups['name'].Value })
    Assert-Set $friends @([string]$capability.issuer) "$name exact friend inventory"

    $constructorPattern = '(?m)^[ \t]*' + [regex]::Escape($name) + '\s*\('
    Assert-Exact ([regex]::Matches($body, $constructorPattern).Count) 3 "$name exact copy, move, and issuer constructor count"
    Assert-Exact ([regex]::Matches($privateBody, $constructorPattern).Count) 1 "$name exact private issuer constructor count"
    Assert-Exact ([regex]::Matches($body, [regex]::Escape($name) + '\s*&\s+operator\s*=\s*\([^\)]*\)\s*=\s*delete\s*;').Count) 2 "$name deleted copy/move assignment count"

    $stateMatches = @([regex]::Matches(
        $privateBody,
        '(?m)^[ \t]*(?<const>const\s+)?' +
        '(?!(?:friend|using|static_assert)\b)' +
        '[A-Za-z_:][^;(){}\r\n]*?\s+' +
        '(?<name>[A-Za-z_]\w*)[ \t]*' +
        '(?:\{[^{}\r\n]*\}|=[^;\r\n]*)?;[ \t]*\r?$'))
    Assert-Set @(
        $stateMatches | ForEach-Object { $_.Groups['name'].Value }
    ) @($capability.memberNames) "$name exact state inventory"
    foreach ($state in $stateMatches) {
        Assert-True $state.Groups['const'].Success "$name mutable state member $($state.Groups['name'].Value)"
    }

    foreach ($memberPattern in @($capability.members)) {
        Assert-Match $privateBody $memberPattern "$name exact private state type $memberPattern" -CaseSensitive
    }
    foreach ($trait in @('is_default_constructible_v','is_aggregate_v','is_copy_assignable_v','is_move_assignable_v')) {
        Assert-Match $contractMainText ('static_assert\s*\(\s*!std::' + $trait + '\s*<\s*Contracts::' + $name + '\s*>\s*\)\s*;') "$name compile-time $trait rejection" -CaseSensitive
    }
}

Assert-Match $toolContractText 'Result\s*<\s*AuthorizedToolCall\s*>\s+authorize\s*\(' 'tool authorizer issues an opaque capability' -CaseSensitive
Assert-Match $toolContractText 'handle\s*\(\s*const\s+AuthorizedToolCall\s*&\s*authorizedCall\s*,\s*const\s+Domain::OperationContext\s*&\s*context\s*\)' 'tool handler consumes only an authorized capability and context' -CaseSensitive
Assert-NoMatch $toolContractText 'handle\s*\(\s*const\s+Domain::ToolCallRequest' 'tool handler must not parse a second raw request' -CaseSensitive
Assert-Match $contractMainText '\bvoid\s+testAuthorizedToolCapability\s*\(' 'authorized-tool capability runtime test' -CaseSensitive
foreach ($capabilityAnchor in @(
    'authorization\.requestId\(\)\s*==\s*fixture\.requestId',
    'authorization\.correlationId\(\)\s*==\s*fixture\.correlationId',
    'authorization\.clientId\(\)\s*==\s*fixture\.clientId',
    'authorization\.request\(\)\.metadata\.protocolVersion\s*==\s*"1\.0"',
    'authorization\.toolName\(\)\s*==\s*"project_memory\.export"',
    'authorization\.canonicalRequest\(\)\s*==\s*call\.canonicalArguments',
    'authorization\.effect\(\)\s*==\s*Domain::ToolEffect::Write',
    'authorization\.projectId\(\)\.value\(\)\s*==\s*fixture\.projectId',
    'authorization\.authorityId\(\)\s*==\s*fixture\.authorityId',
    'authorization\.authorityGeneration\(\)\s*==\s*7',
    'callerMismatchRequest',
    'correlationMismatchRequest',
    'projectMismatchRequest',
    'authorityIdMismatchRequest',
    'generationMismatchRequest',
    'projectlessRequest\.call\.metadata\.projectId\.reset\(\)',
    '!projectless\.matchesProject\(fixture\.projectId\)',
    'tool handler ignored operation cancellation',
    'tool handler ignored the operation deadline',
    'tool router ignored targeted cancellation')) {
    Assert-Match $contractMainText $capabilityAnchor "authorized-tool capability test anchor $capabilityAnchor" -CaseSensitive
}
Assert-Exact ([regex]::Matches($contractMainText, '\btestAuthorizedToolCapability\s*\(\s*fixture\s*\)\s*;').Count) 1 'authorized-tool capability test invocation count'

$fakeClassMap = [ordered]@{
    IAgentCatalog = 'RecordingAgentCatalogFake'
    IAgentSessionRepository = 'RecordingAgentSessionRepositoryFake'
    IAgentSessionService = 'RecordingAgentSessionServiceFake'
    IConfigurationStore = 'RecordingConfigurationStoreFake'
    IContinuityRepository = 'ContinuityRepositoryFake'
    IContinuityCoordinator = 'RecordingContinuityCoordinator'
    IDiagnosticSink = 'DiagnosticSinkFake'
    IAuditRepository = 'AuditRepositoryFake'
    IDoctorService = 'DoctorServiceFake'
    IRuntimeDiagnostics = 'RuntimeDiagnosticsFake'
    IApplicationPaths = 'RecordingApplicationPathsFake'
    IWorkspaceAuthority = 'DeterministicWorkspaceAuthority'
    IAtomicFileStore = 'RecordingAtomicFileStoreFake'
    IFileSystem = 'RecordingFileSystemFake'
    IGraphicsDeviceService = 'RecordingGraphicsDeviceServiceFake'
    IRenderService = 'RecordingRenderServiceFake'
    IInstallerDeploymentService = 'RecordingInstallerDeploymentServiceFake'
    ILMStudioDeploymentService = 'RecordingLMStudioDeploymentServiceFake'
    ILMStudioEnvironment = 'RecordingLMStudioEnvironmentFake'
    IForgeApplicationLifecycle = 'RecordingForgeApplicationLifecycleFake'
    IClock = 'FakeClock'
    IUuidGenerator = 'SequenceUuidGenerator'
    IHasher = 'ScriptedHasher'
    IRedactor = 'ScriptedRedactor'
    IDeadlineScheduler = 'FakeDeadlineScheduler'
    ILatestValueMailbox = 'BoundedLatestValueMailboxFake'
    ILegacyMemoryService = 'LegacyMemoryServiceFake'
    IManagerClient = 'ManagerClientFake'
    IManagerServer = 'ManagerServerFake'
    IMcpServer = 'RecordingMcpServerFake'
    IMcpTransport = 'McpTransportFake'
    ITextSearchService = 'RecordingTextSearchServiceFake'
    IGitService = 'RecordingGitServiceFake'
    IShellService = 'RecordingShellServiceFake'
    IPdfService = 'RecordingPdfServiceFake'
    IProcessSupervisor = 'RecordingProcessSupervisor'
    IProjectRegistryRepository = 'ProjectRegistryRepositoryFake'
    IProjectMemoryRepository = 'ProjectMemoryRepositoryFake'
    IProjectMemoryRepositoryFactory = 'ProjectMemoryRepositoryFactoryFake'
    IProjectMemoryService = 'RecordingProjectMemoryService'
    ISecureStorage = 'RecordingSecureStorageFake'
    ISessionHostAdapter = 'RecordingSessionHostAdapter'
    ILocalModelClient = 'RecordingLocalModelClientFake'
    ITelemetryService = 'RecordingTelemetryService'
    IToolAuthorizer = 'DeterministicToolAuthorizerFake'
    IToolHandler = 'RecordingToolHandlerFake'
    IToolCatalog = 'BoundedToolCatalogFake'
    IToolRouter = 'RecordingToolRouterFake'
}
$telemetryAliasMap = [ordered]@{
    ICpuMetricsCollector = 'CpuMetricsCollectorFake'
    IRamMetricsCollector = 'RamMetricsCollectorFake'
    IDiskVolumeCollector = 'DiskVolumeCollectorFake'
    IDiskIoMetricsCollector = 'DiskIoMetricsCollectorFake'
    IGpuMetricsCollector = 'GpuMetricsCollectorFake'
    IProcessMetricsCollector = 'ProcessMetricsCollectorFake'
    IPowerMetricsCollector = 'PowerMetricsCollectorFake'
    ISystemMetricsCollector = 'SystemMetricsCollectorFake'
    IForgeMetricsCollector = 'ForgeMetricsCollectorFake'
}
$declaredInterfaces = @([regex]::Matches(
    $contractText,
    '\bclass\s+(?<name>I[A-Za-z0-9_]+)\s*\{') | ForEach-Object {
        $_.Groups['name'].Value
    } | Sort-Object -Unique)
$mappedInterfaces = @($fakeClassMap.Keys) + @($telemetryAliasMap.Keys)
Assert-Exact $mappedInterfaces.Count 57 'complete fake interface-map count'
Assert-Set $mappedInterfaces $declaredInterfaces 'complete public-interface fake coverage'
foreach ($entry in $fakeClassMap.GetEnumerator()) {
    $interfacePattern = [regex]::Escape([string]$entry.Key)
    if ($entry.Key -eq 'ILatestValueMailbox') {
        $interfacePattern += '\s*<\s*T\s*>'
    }
    Assert-Match $testText ('\bclass\s+' + [regex]::Escape([string]$entry.Value) + '\s+final\s*:\s*public\s+Contracts::' + $interfacePattern) "fake $($entry.Value) implements $($entry.Key)" -CaseSensitive
}
$telemetryFakeText = Get-Content -Raw -LiteralPath (Join-Path $fakesRoot 'TelemetryFakes.h')
foreach ($entry in $telemetryAliasMap.GetEnumerator()) {
    Assert-Match $telemetryFakeText ('\busing\s+' + [regex]::Escape([string]$entry.Value) + '\s*=\s*RecordingTelemetryCollector\s*<[^;]*?Contracts::' + [regex]::Escape([string]$entry.Key) + '\s*>\s*;') "telemetry fake $($entry.Value) implements $($entry.Key)" -CaseSensitive
}

$inlineEntrypoints = [ordered]@{
    runApplicationServiceFakeContractTests = 'ApplicationServiceFakeContractTests.h'
    runAuthorityAdjacentPlatformFakeContractTests = 'AuthorityAdjacentPlatformFakeContractTests.h'
    runMcpCancellationContractTests = 'McpCancellationContractTests.h'
    runRepositoryDiagnosticsManagerFakeContractTests = 'RepositoryDiagnosticsManagerFakeContractTests.h'
    runAuthorityContextFakeContractTests = 'AuthorityContextFakeContractTests.h'
    runPlatformStorageFakeContractTests = 'PlatformStorageFakeContractTests.h'
    runNativeToolBoundaryFakeContractTests = 'NativeToolBoundaryFakeContractTests.h'
}
$externalEntrypoints = [ordered]@{
    runFoundationTelemetryFakeContractTests = 'FoundationTelemetryFakeContractTests'
    runGroupedFakeContractTests = 'GroupedFakeContractTests'
}
$contractTestImplementationText = (@($contractTestHeaders) + @($contractTestSources) |
    ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join [Environment]::NewLine
$expectedEntrypoints = @($inlineEntrypoints.Keys) + @($externalEntrypoints.Keys)
$definitionPattern =
    '(?m)^[ \t]*(?:inline[ \t]+)?void[ \t]+(?<name>run[A-Za-z0-9_]+ContractTests)' +
    '\s*\(\s*\)\s*\{'
$definedEntrypoints = @(
    [regex]::Matches($contractTestImplementationText, $definitionPattern) |
        ForEach-Object { $_.Groups['name'].Value })
Assert-Set $definedEntrypoints $expectedEntrypoints 'exact contract-test definition inventory'

foreach ($entry in $inlineEntrypoints.GetEnumerator()) {
    $headerText = Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot ('tests\Contracts\' + $entry.Value))
    $pattern = '(?m)^[ \t]*inline[ \t]+void[ \t]+' +
        [regex]::Escape([string]$entry.Key) + '\s*\(\s*\)\s*\{'
    Assert-Exact ([regex]::Matches($headerText, $pattern).Count) 1 "definition count for $($entry.Key)"
}
foreach ($entry in $externalEntrypoints.GetEnumerator()) {
    $sourceText = Get-Content -Raw -LiteralPath (
        Join-Path $WorkspaceRoot ('tests\Contracts\' + $entry.Value + '.cpp'))
    $pattern = '(?m)^[ \t]*void[ \t]+' +
        [regex]::Escape([string]$entry.Key) + '\s*\(\s*\)\s*\{'
    Assert-Exact ([regex]::Matches($sourceText, $pattern).Count) 1 "definition count for $($entry.Key)"
}

function Assert-RunnerCallSet {
    param([string]$Text, [object[]]$Expected, [string]$Message)

    $actual = @(
        [regex]::Matches(
            $Text,
            '\b(?<name>run[A-Za-z0-9_]+ContractTests)\s*\(\s*\)\s*;') |
            ForEach-Object { $_.Groups['name'].Value })
    Assert-Set $actual $Expected $Message
}

$groupedRunnerText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\GroupedFakeContractTests.cpp')
$authorityAggregatorText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\AuthorityAdjacentPlatformFakeContractTests.h')
Assert-RunnerCallSet $contractMainText @(
    'runFoundationTelemetryFakeContractTests',
    'runGroupedFakeContractTests') 'main runner call set'
Assert-RunnerCallSet $groupedRunnerText @(
    'runApplicationServiceFakeContractTests',
    'runAuthorityAdjacentPlatformFakeContractTests',
    'runMcpCancellationContractTests',
    'runRepositoryDiagnosticsManagerFakeContractTests') 'grouped runner call set'
Assert-RunnerCallSet $authorityAggregatorText @(
    'runAuthorityContextFakeContractTests',
    'runPlatformStorageFakeContractTests',
    'runNativeToolBoundaryFakeContractTests') 'authority-adjacent runner call set'
$foundationTelemetryTestText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\FoundationTelemetryFakeContractTests.cpp')
foreach ($fakeName in @(
    'FakeClock','SequenceUuidGenerator','ScriptedHasher','ScriptedRedactor',
    'CpuMetricsCollectorFake','RamMetricsCollectorFake','DiskVolumeCollectorFake',
    'DiskIoMetricsCollectorFake','GpuMetricsCollectorFake','ProcessMetricsCollectorFake',
    'PowerMetricsCollectorFake','SystemMetricsCollectorFake','ForgeMetricsCollectorFake')) {
    Assert-Match $foundationTelemetryTestText ('\bFakes::' + $fakeName + '\b') "runtime instantiation for $fakeName" -CaseSensitive
}
foreach ($behaviorAnchor in @(
    'LimitExceeded',
    'RedactionRejected',
    'telemetry collector fake ignored operation cancellation',
    'telemetry collector fake ignored the injected monotonic deadline',
    'telemetry collector fake accepted work after shutdown')) {
    Assert-Match $foundationTelemetryTestText $behaviorAnchor "foundation/telemetry runtime behavior $behaviorAnchor" -CaseSensitive
}


$cmakePath = Join-Path $WorkspaceRoot 'CMakeLists.txt'
$cmake = Get-Content -Raw -LiteralPath $cmakePath
Assert-Match $cmake 'add_library\s*\(\s*ForgeConductor\.Domain\s+STATIC\s+\$\{FORGE_DOMAIN_SOURCES\}\s*\)' 'Domain must be a concrete static library'
Assert-Match $cmake 'forge_configure_standard_target\s*\(\s*ForgeConductor\.Domain\s*\)' 'Domain must use portable compile settings'
Assert-NoMatch $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Domain\s*\)' 'Domain must not use Windows-native compile settings'
Assert-NoMatch $cmake 'target_compile_definitions\s*\(\s*ForgeConductor\.Domain(?=\s|\))' 'Domain must not add platform compile definitions'
Assert-NoMatch $cmake 'target_link_libraries\s*\(\s*ForgeConductor\.Domain(?=\s|\))' 'Domain must not link another target'
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor\.Domain\s+ForgeConductor::Domain\s*\)' 'Domain layer declaration'
Assert-Match $cmake 'forge_add_layer\s*\(\s*ForgeConductor\.Contracts\s+ForgeConductor::Contracts\s+ForgeConductor::Domain\s*\)' 'Contracts must depend only on Domain'
Assert-NoMatch $cmake 'forge_configure_native_target\s*\(\s*ForgeConductor\.Contracts\s*\)' 'Contracts must not use Windows-native compile settings'
Assert-NoMatch $cmake 'target_compile_definitions\s*\(\s*ForgeConductor\.Contracts(?=\s|\))' 'Contracts must not add platform compile definitions'
Assert-NoMatch $cmake 'target_link_libraries\s*\(\s*ForgeConductor\.Contracts(?=\s|\))' 'Contracts must not add dependencies beyond Domain'
Assert-NoMatch (Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'Directory.Build.props')) '<CharacterSet>' 'global Unicode property would leak platform definitions into Domain'

$domainSourceBlock = [regex]::Match($cmake, 'set\s*\(\s*FORGE_DOMAIN_SOURCES(?<body>.*?)\)', [Text.RegularExpressions.RegexOptions]::Singleline)
Assert-True $domainSourceBlock.Success 'explicit Domain source inventory is missing'
$declaredSources = @($domainSourceBlock.Groups['body'].Value -split '\s+' | Where-Object { $_ })
$actualSources = @($domainSources | ForEach-Object { 'src/Domain/' + $_.Name })
Assert-Set $declaredSources $actualSources 'explicit Domain source inventory'

foreach ($target in @(
    'ForgeConductor.Domain.UnitTests',
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment')) {
    Assert-Match $cmake ('add_executable\s*\(\s*' + [regex]::Escape($target)) "missing P05 test target $target"
    Assert-Match $cmake ([regex]::Escape($target) + '[\s\S]*?LABELS\s+"T-UNIT;G05"') "missing exact labels for $target"
}
Assert-Match $cmake 'add_executable\s*\(\s*ForgeConductor\.Contracts\.ContractTests\s+tests/Contracts/FoundationTelemetryFakeContractTests\.cpp\s+tests/Contracts/GroupedFakeContractTests\.cpp\s+tests/Contracts/main\.cpp\s*\)' 'exact Contracts test translation-unit inventory' -CaseSensitive
Assert-Match $cmake 'file\s*\(\s*GLOB\s+_forge_domain_contract_headers\s+CONFIGURE_DEPENDS' 'public header isolation inventory'
Assert-Match $cmake 'add_library\s*\(\s*ForgeConductor\.DomainContracts\.HeaderObjects\s+OBJECT' 'one-TU-per-header object target'
Assert-Match $cmake 'add_executable\s*\(\s*ForgeConductor\.Cli[\s\S]*?src/Hosts/Cli/CliCompositionRoot\.cpp[\s\S]*?src/Hosts/Cli/CliCompositionRoot\.h[\s\S]*?src/Hosts/Cli/main\.cpp' 'CLI composition-root source inventory' -CaseSensitive
Assert-Match $cmake 'ForgeConductor\.Cli\.SelfTest[\s\S]*?LABELS\s+"T-UNIT;G04;G05"' 'CLI G04/G05 self-test labels' -CaseSensitive

$memoryContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IProjectMemoryService.h')
foreach ($operation in @('initialize','remember','rememberBatch','search','get','update','forget','listRecent','link','exportMemory','importMemory','status')) {
    Assert-Match $memoryContract ('\b' + [regex]::Escape($operation) + '\s*\(') "project-memory operation missing: $operation" -CaseSensitive
}
Assert-Match $memoryContract '\bcloseProject\s*\(' 'project-memory close operation' -CaseSensitive
Assert-Match $memoryContract '\bresetProjectMemory\s*\(' 'project-memory reset operation' -CaseSensitive
Assert-Match $memoryContract '\bshutdown\s*\(\s*\)\s*noexcept' 'project-memory shutdown operation' -CaseSensitive
$exportSignatures = @(Get-MethodParameterSets $memoryContract 'exportMemory')
Assert-Exact $exportSignatures.Count 2 'repository and service exportMemory signature count'
$exportParametersPattern = 'const\s+Domain::ExportProjectMemoryRequest\s*&\s*request\s*,\s*const\s+WorkspaceAuthority\s*&\s*writeAuthority\s*,\s*const\s+AuthorizedToolCall\s*&\s*authorization\s*,\s*const\s+Domain::OperationContext\s*&\s*context'
foreach ($parameters in $exportSignatures) {
    Assert-Match $parameters $exportParametersPattern 'project-memory export write authority, mutation audit, and operation context' -CaseSensitive
}

$projectMemoryHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'ProjectMemoryModels.h')
$projectMemorySourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'ProjectMemoryModels.cpp')
$domainTestSource = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Domain\DomainTests.cpp')
$projectMemoryLimitPatterns = [ordered]@{
    maximumTitleBytes = 'maximumTitleBytes\s*\{\s*512\s*\}'
    maximumSummaryBytes = 'maximumSummaryBytes\s*\{\s*4\s*\*\s*1024\s*\}'
    maximumBodyBytes = 'maximumBodyBytes\s*\{\s*256\s*\*\s*1024\s*\}'
    maximumSourceReferenceBytes = 'maximumSourceReferenceBytes\s*\{\s*2\s*\*\s*1024\s*\}'
    maximumTagCount = 'maximumTagCount\s*\{\s*32\s*\}'
    maximumTagBytes = 'maximumTagBytes\s*\{\s*128\s*\}'
    maximumRelatedIdCount = 'maximumRelatedIdCount\s*\{\s*32\s*\}'
    maximumBatchCount = 'maximumBatchCount\s*\{\s*50\s*\}'
    maximumBatchBytes = 'maximumBatchBytes\s*\{\s*1024\s*\*\s*1024\s*\}'
    maximumQueryBytes = 'maximumQueryBytes\s*\{\s*4\s*\*\s*1024\s*\}'
    maximumPageCount = 'maximumPageCount\s*\{\s*100\s*\}'
    defaultPageCount = 'defaultPageCount\s*\{\s*20\s*\}'
    maximumResponseBytes = 'maximumResponseBytes\s*\{\s*256\s*\*\s*1024\s*\}'
    defaultResponseBytes = 'defaultResponseBytes\s*\{\s*64\s*\*\s*1024\s*\}'
    maximumOpenProjects = 'maximumOpenProjects\s*\{\s*8\s*\}'
    maximumArtifactBytes = 'maximumArtifactBytes\s*\{\s*32\s*\*\s*1024\s*\*\s*1024\s*\}'
}
foreach ($entry in $projectMemoryLimitPatterns.GetEnumerator()) {
    Assert-Match $projectMemoryHeaderText ([string]$entry.Value) "exact project-memory limit $($entry.Key)" -CaseSensitive
}
Assert-Match $projectMemoryHeaderText 'ProjectMemoryCapabilityVersion\s*=\s*1\s*;' 'project-memory capability version 1' -CaseSensitive
Assert-Match $projectMemoryHeaderText 'ProjectMemorySchemaVersion\s*=\s*1\s*;' 'project-memory schema version 1' -CaseSensitive
Assert-Match $projectMemoryHeaderText 'MinimumProjectMemoryDeadline\s*\{\s*1\s*\}' 'project-memory minimum deadline 1 ms' -CaseSensitive
Assert-Match $projectMemoryHeaderText 'MaximumProjectMemoryDeadline\s*\{\s*60''000\s*\}' 'project-memory maximum deadline 60000 ms' -CaseSensitive
Assert-Match $projectMemorySourceText 'kind\.size\(\)\s*>\s*64' 'project-memory kind byte cap 64' -CaseSensitive
Assert-NoMatch $projectMemoryHeaderText 'no aggregate count cap' 'stale unbounded kind-filter declaration'
Assert-Match $projectMemorySourceText 'if\s*\(\s*kinds\.size\(\)\s*>\s*limits\.maximumPageCount\s*\)[\s\S]{0,240}ErrorCodes::PayloadTooLarge' 'kind-filter cap and typed error' -CaseSensitive
Assert-Exact ([regex]::Matches($projectMemorySourceText, 'normalizeKindFilters\s*\(\s*request\.kinds\s*,\s*limits\s*\)').Count) 2 'exact kind-filter validator call count'
Assert-Match $projectMemorySourceText 'Result<SearchProjectMemoryRequest>\s+validateSearchProjectMemoryRequest\s*\([\s\S]*?\)\s*\{[\s\S]{0,700}?normalizeKindFilters\s*\(\s*request\.kinds\s*,\s*limits\s*\)' 'search request applies kind-filter normalization' -CaseSensitive
Assert-Match $projectMemorySourceText 'Result<ListRecentProjectMemoryRequest>\s+validateListRecentProjectMemoryRequest\s*\([\s\S]*?\)\s*\{[\s\S]{0,500}?normalizeKindFilters\s*\(\s*request\.kinds\s*,\s*limits\s*\)' 'recent request applies kind-filter normalization' -CaseSensitive
$p05BoundsAdr = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot '.forge-codex\state\decisions\P05-002-source-compatibility-and-windows-resource-bounds.md')
Assert-Match $p05BoundsAdr 'The macOS `project_memory\.search` and `project_memory\.list_recent` schemas leave the `kinds` arrays without an aggregate count bound' 'kind-filter source divergence recorded' -CaseSensitive
Assert-Match $p05BoundsAdr 'ProjectMemoryLimits::maximumPageCount` value of 100[\s\S]*prohibition on unbounded collections[\s\S]*payload_too_large' 'kind-filter Windows bound rationale' -CaseSensitive
Assert-Match $projectMemorySourceText 'request\.relation\.size\(\)\s*>\s*128U' 'project-memory relation byte cap 128' -CaseSensitive
Assert-Match $projectMemorySourceText 'std::clamp\s*\(\s*requested\s*,\s*std::size_t\s*\{\s*1024\s*\}\s*,\s*limits\.maximumResponseBytes\s*\)' 'project-memory response floor 1024' -CaseSensitive
Assert-Match $projectMemorySourceText 'maximumOpenProjects\s*=\s*budgetsForProfile\s*\(\s*profile\s*\)\.openProjectRepositoriesMaximum' 'project-memory 4/8/16 profile mapping' -CaseSensitive
Assert-Match $projectMemorySourceText 'base64\s*\(\s*"v1:<nonnegative Int>"\s*\)' 'canonical project-memory cursor source form' -CaseSensitive
foreach ($cursorAnchor in @(
    'decoded\[0\]\s*!=\s*''v''',
    'decoded\[1\]\s*!=\s*''1''',
    'decoded\[2\]\s*!=\s*'':''',
    'digitCount\s*>\s*1U\s*&&\s*decoded\[3\]\s*==\s*''0''',
    'numericValue\s*>\s*\(std::numeric_limits<std::size_t>::max\(\)\s*-\s*digit\)\s*/\s*10U')) {
    Assert-Match $projectMemorySourceText $cursorAnchor "canonical cursor guard $cursorAnchor" -CaseSensitive
}

$domainTestMap = [ordered]@{
    result_and_identifier_boundaries = 'resultAndIdentifierBoundaries'
    path_boundaries = 'pathBoundaries'
    resource_and_configuration_boundaries = 'resourceAndConfigurationBoundaries'
    agent_and_legacy_memory_parity = 'agentAndLegacyMemoryParity'
    project_memory_boundaries = 'projectMemoryBoundaries'
    continuity_state_and_integrity = 'continuityStateAndIntegrity'
    process_tool_telemetry_and_manager_bounds = 'processToolTelemetryAndManagerBounds'
    project_memory_request_boundaries = 'projectMemoryRequestBoundaries'
    continuity_binding_and_isolation = 'continuityBindingAndIsolation'
    deployment_and_manager_defaults = 'deploymentAndManagerDefaults'
    diagnostics_environment_and_graphics = 'diagnosticsEnvironmentAndGraphics'
}
$domainRegistryMatch = [regex]::Match(
    $domainTestSource,
    'const\s+std::vector<std::pair<std::string_view,\s*std::function<void\(\)>>> tests\s*\{(?<body>[\s\S]*?)\};')
Assert-True $domainRegistryMatch.Success 'Domain unit-test registry body'
$registeredDomainTests = @([regex]::Matches(
    $domainRegistryMatch.Groups['body'].Value,
    '\{\s*"(?<name>[a-z0-9_]+)"\s*,\s*(?<function>[A-Za-z0-9_]+)\s*\}') |
    ForEach-Object { $_.Groups['name'].Value })
Assert-Set $registeredDomainTests @($domainTestMap.Keys) 'exact Domain unit-test inventory'
foreach ($entry in $domainTestMap.GetEnumerator()) {
    Assert-Match $domainRegistryMatch.Groups['body'].Value ('\{\s*"' + [regex]::Escape([string]$entry.Key) + '"\s*,\s*' + [regex]::Escape([string]$entry.Value) + '\s*\}') "Domain test registration $($entry.Key)" -CaseSensitive
}
$memoryRequestTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+projectMemoryRequestBoundaries\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\nvoid\s+continuityStateAndIntegrity)')
Assert-True $memoryRequestTestMatch.Success 'project_memory_request_boundaries body'
$memoryRequestTestText = $memoryRequestTestMatch.Groups['body'].Value
Assert-Match $memoryRequestTestText 'search\.kinds\.assign\s*\(\s*limits\.maximumPageCount[\s\S]*?search\.kinds\.push_back[\s\S]*?tooManySearchKinds\.error\(\)\.code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' 'search kind-filter cap and cap-plus-one typed-error test' -CaseSensitive
Assert-Match $memoryRequestTestText 'recent\.kinds\.assign\s*\(\s*limits\.maximumPageCount[\s\S]*?recent\.kinds\.push_back[\s\S]*?tooManyRecentKinds\.error\(\)\.code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' 'recent kind-filter cap and cap-plus-one typed-error test' -CaseSensitive
foreach ($memoryTestAnchor in @(
    'maximumTitleBytes\s*==\s*512U',
    'maximumSummaryBytes\s*==\s*4U\s*\*\s*1024U',
    'maximumBodyBytes\s*==\s*256U\s*\*\s*1024U',
    'maximumSourceReferenceBytes\s*==\s*2U\s*\*\s*1024U',
    'maximumTagCount\s*==\s*32U',
    'maximumTagBytes\s*==\s*128U',
    'maximumRelatedIdCount\s*==\s*32U',
    'maximumBatchCount\s*==\s*50U',
    'maximumBatchBytes\s*==\s*1024U\s*\*\s*1024U',
    'maximumQueryBytes\s*==\s*4U\s*\*\s*1024U',
    'defaultPageCount\s*==\s*20U',
    'maximumPageCount\s*==\s*100U',
    'defaultResponseBytes\s*==\s*64U\s*\*\s*1024U',
    'maximumResponseBytes\s*==\s*256U\s*\*\s*1024U',
    'maximumArtifactBytes\s*==\s*32U\s*\*\s*1024U\s*\*\s*1024U',
    'maximumOpenProjects\s*==\s*4U',
    'Standard16GiB\)\.maximumOpenProjects\s*==\s*8U',
    'Expanded32GiBPlus\)\.maximumOpenProjects\s*==\s*16U',
    'maximumRelatedIdCount\s*\+\s*1U',
    'relatedIds\.size\(\)\s*==\s*limits\.maximumRelatedIdCount',
    'maximumBatchCount\s*\+\s*1U',
    'maximumQueryBytes\s*\+\s*1U',
    'MinimumProjectMemoryDeadline',
    'milliseconds\s*\{\s*0\s*\}',
    'MaximumProjectMemoryDeadline\s*\+\s*std::chrono::milliseconds\s*\{\s*1\s*\}',
    'djE6MTAw',
    'not-base64',
    'djE6MDA=',
    'djE6MTg0NDY3NDQwNzM3MDk1NTE2MTU=',
    'djE6MTg0NDY3NDQwNzM3MDk1NTE2MTY=',
    'maximumPageCount\s*,\s*recordId',
    'get\.ids\.push_back',
    'get\.ids\.clear',
    'update\.summary\s*=\s*std::string\s*\(\s*limits\.maximumSummaryBytes',
    'update\.tags\s*=\s*std::vector<std::string>\s*\(\s*limits\.maximumTagCount',
    'limits\.maximumTagBytes',
    'recent\.kinds\.assign\s*\(\s*limits\.maximumPageCount',
    'recent\.kinds\.push_back',
    'std::string\s*\(\s*128\s*,\s*''r''\s*\)',
    'maximumArtifactBytes\s*\+\s*1U',
    'validateSearchProjectMemoryRequest',
    'validateGetProjectMemoryRequest',
    'validateUpdateProjectMemoryRequest',
    'validateListRecentProjectMemoryRequest',
    'validateLinkProjectMemoryRequest',
    'validateExportProjectMemoryRequest',
    'validateImportProjectMemoryRequest',
    'validateProjectMemoryStatusRequest')) {
    Assert-Match $memoryRequestTestText $memoryTestAnchor "project-memory request boundary anchor $memoryTestAnchor" -CaseSensitive
}
$agentContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IAgentServices.h')
$agentStatusParameters = @(Get-MethodParameterSets $agentContract 'status')
Assert-Exact $agentStatusParameters.Count 1 'agent session status signature count'
Assert-Match $agentStatusParameters[0] 'const\s+Domain::SessionId\s*&\s*sessionId\s*,\s*const\s+WorkspaceAuthority\s*&\s*mutationAuthority\s*,\s*const\s+AuthorizedToolCall\s*&\s*authorization\s*,\s*const\s+Domain::OperationContext\s*&\s*context' 'agent_run_status mutation authority, authorization capability, and operation context' -CaseSensitive

$foundationContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IFoundationServices.h')
$waitParameters = @(Get-MethodParameterSets $foundationContract 'waitUntil')
Assert-Exact $waitParameters.Count 1 'deadline scheduler waitUntil signature count'
Assert-Match $waitParameters[0] '^\s*const\s+Domain::OperationContext\s*&\s*context\s*$' 'deadline scheduler must receive the complete operation context' -CaseSensitive
Assert-NoMatch $waitParameters[0] ',' 'deadline scheduler must not expose a parallel raw deadline/cancellation path' -CaseSensitive
Assert-NoMatch $contractText '\bstd::stop_token\b' 'Contracts must carry cancellation through OperationContext' -CaseSensitive

$lmEnvironmentContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'ILMStudioEnvironment.h')
$lmEnvironmentInspect = @(Get-MethodParameterSets $lmEnvironmentContract 'inspect')
Assert-Exact $lmEnvironmentInspect.Count 1 'LM Studio environment inspect signature count'
Assert-Match $lmEnvironmentInspect[0] 'const\s+std::optional\s*<\s*Domain::PathText\s*>\s*&\s*explicitConfigurationPath\s*,\s*const\s+WorkspaceAuthority\s*&\s*readAuthority\s*,\s*const\s+Domain::OperationContext\s*&\s*context' 'LM Studio discovery explicit configuration, read authority, and context' -CaseSensitive
$lmEnvironmentHealth = @(Get-MethodParameterSets $lmEnvironmentContract 'connectionHealth')
Assert-Exact $lmEnvironmentHealth.Count 1 'LM Studio connection-health signature count'
Assert-NoMatch $lmEnvironmentHealth[0] ',' 'LM Studio connection health must take exactly one context' -CaseSensitive
Assert-Match $lmEnvironmentHealth[0] 'const\s+Domain::OperationContext\s*&\s*context' 'LM Studio connection health operation context' -CaseSensitive
Assert-Match $lmEnvironmentContract '\bshutdown\s*\(\s*\)\s*noexcept\s*=\s*0' 'LM Studio environment shutdown contract' -CaseSensitive

$lmDeploymentContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'ILMStudioDeploymentService.h')
$lmDeploymentStatus = @(Get-MethodParameterSets $lmDeploymentContract 'status')
Assert-Exact $lmDeploymentStatus.Count 1 'LM Studio deployment status signature count'
Assert-Match $lmDeploymentStatus[0] 'LMStudioDeploymentRequest\s*&\s*request\s*,\s*const\s+WorkspaceAuthority\s*&\s*readAuthority\s*,\s*const\s+Domain::OperationContext\s*&\s*context' 'LM Studio deployment status authority and context' -CaseSensitive
foreach ($methodName in @('deploy','activate')) {
    $methodParameters = @(Get-MethodParameterSets $lmDeploymentContract $methodName)
    Assert-Exact $methodParameters.Count 1 "LM Studio $methodName signature count"
    Assert-Match $methodParameters[0] 'const\s+WorkspaceAuthority\s*&\s*(?:writeAuthority|executionAuthority)\s*,\s*const\s+AuthorizedToolCall\s*&\s*authorization\s*,\s*const\s+Domain::OperationContext\s*&\s*context' "LM Studio $methodName authority, authorization capability, and context" -CaseSensitive
}
Assert-Match $lmDeploymentContract '\bcancel\s*\(\s*const\s+Domain::OperationId\s*&\s*operationId\s*\)' 'LM Studio operation-targeted cancellation' -CaseSensitive
Assert-Match $lmDeploymentContract '\bshutdown\s*\(\s*\)\s*noexcept\s*=\s*0' 'LM Studio deployment shutdown contract' -CaseSensitive

$graphicsContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IGraphicsServices.h')
foreach ($methodName in @('initialize','status','recover')) {
    $methodParameters = @(Get-MethodParameterSets $graphicsContract $methodName)
    Assert-Exact $methodParameters.Count 1 "graphics $methodName signature count"
    Assert-NoMatch $methodParameters[0] ',' "graphics $methodName must take exactly one context" -CaseSensitive
    Assert-Match $methodParameters[0] 'const\s+Domain::OperationContext\s*&\s*context' "graphics $methodName operation context" -CaseSensitive
}
$renderParameters = @(Get-MethodParameterSets $graphicsContract 'render')
Assert-Exact $renderParameters.Count 1 'render signature count'
Assert-Match $renderParameters[0] 'const\s+Domain::RenderRequest\s*&\s*request\s*,\s*const\s+Domain::OperationContext\s*&\s*context' 'render request and operation context' -CaseSensitive
Assert-Exact ([regex]::Matches($graphicsContract, '\bcancel\s*\(\s*const\s+Domain::OperationId\s*&\s*operationId\s*\)').Count) 2 'graphics operation-targeted cancellation count'
Assert-Exact ([regex]::Matches($graphicsContract, '\bshutdown\s*\(\s*\)\s*noexcept\s*=\s*0').Count) 2 'graphics shutdown contract count'

$installerContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IInstallerDeploymentService.h')
$installerStatus = @(Get-MethodParameterSets $installerContract 'status')
Assert-Exact $installerStatus.Count 1 'installer deployment status signature count'
Assert-NoMatch $installerStatus[0] ',' 'installer deployment status must take exactly one context' -CaseSensitive
Assert-Match $installerStatus[0] 'const\s+Domain::OperationContext\s*&\s*context' 'installer deployment status context' -CaseSensitive
$installerExecute = @(Get-MethodParameterSets $installerContract 'execute')
Assert-Exact $installerExecute.Count 1 'installer deployment execute signature count'
Assert-Match $installerExecute[0] 'const\s+Domain::DeploymentRequest\s*&\s*request\s*,\s*const\s+WorkspaceAuthority\s*&\s*mutationAuthority\s*,\s*const\s+AuthorizedToolCall\s*&\s*authorization\s*,\s*const\s+Domain::OperationContext\s*&\s*context' 'installer deployment mutation authority, authorization capability, and context' -CaseSensitive
Assert-Match $installerContract '\bcancel\s*\(\s*const\s+Domain::OperationId\s*&\s*operationId\s*\)' 'installer operation-targeted cancellation' -CaseSensitive
Assert-Match $installerContract '\bshutdown\s*\(\s*\)\s*noexcept\s*=\s*0' 'installer deployment shutdown contract' -CaseSensitive

$externalFakeText = Get-Content -Raw -LiteralPath (Join-Path $fakesRoot 'ExternalServiceFakes.h')
$requiredExternalFakes = [ordered]@{
    RecordingLMStudioEnvironmentFake = 'ILMStudioEnvironment'
    RecordingLMStudioDeploymentServiceFake = 'ILMStudioDeploymentService'
    RecordingGraphicsDeviceServiceFake = 'IGraphicsDeviceService'
    RecordingRenderServiceFake = 'IRenderService'
    RecordingInstallerDeploymentServiceFake = 'IInstallerDeploymentService'
}
foreach ($entry in $requiredExternalFakes.GetEnumerator()) {
    Assert-Match $externalFakeText ('\bclass\s+' + [regex]::Escape($entry.Key) + '\s+final\s*:?\s*public\s+Contracts::' + [regex]::Escape($entry.Value)) "deterministic final fake for $($entry.Value)" -CaseSensitive
}
Assert-Match $externalFakeText '\bBoundedOperationGate\s+final\b' 'external fakes use a bounded operation gate' -CaseSensitive
Assert-Match $externalFakeText '\bisExpired\s*\(' 'external fakes enforce deterministic deadlines' -CaseSensitive
Assert-Match $externalFakeText '\bisCancellationRequested\s*\(' 'external fakes enforce deterministic cancellation' -CaseSensitive

$managerSource = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'src\Domain\ManagerModels.cpp')
Assert-Match $managerSource 'return\s+host\s*==\s*"127\.0\.0\.1"\s*\|\|\s*host\s*==\s*"::1"\s*;' 'manager bind authority accepts only exact numeric loopback addresses' -CaseSensitive
Assert-NoMatch $managerSource '"localhost"' 'manager bind authority must not accept hostnames' -CaseSensitive
$domainTestSource = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'tests\Domain\DomainTests.cpp')
Assert-Match $domainTestSource 'rejectedDashboardHosts\s*\{[^}]*"localhost"[^}]*"0\.0\.0\.0"[^}]*"127\.0\.0\.2"[^}]*"\[::1\]"' 'manager tests cover hostname, wildcard, alternate, and bracketed address rejection' -CaseSensitive
Assert-Match $domainTestSource 'for\s*\(\s*const\s+auto\s*&\s*host\s*:\s*rejectedDashboardHosts\s*\).*?REQUIRE\s*\(\s*!Domain::validateManagerSettings\s*\(\s*settings\s*\)\s*\)' 'manager rejected-address table is asserted' -CaseSensitive


$processContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IProcessSupervisor.h')
Assert-Match $processContract 'WorkspaceAuthority' 'process execution authority' -CaseSensitive
Assert-Match $processContract 'OperationContext' 'process absolute deadline/cancellation context' -CaseSensitive
Assert-Match $processContract '\bcancel\s*\([^\)]*OperationId' 'operation-targeted process cancellation' -CaseSensitive

$processHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'ProcessModels.h')
$processSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'ProcessModels.cpp')
$configurationHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'ConfigurationModels.h')
$configurationSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'ConfigurationModels.cpp')
$diagnosticsHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'DiagnosticsModels.h')
$diagnosticsSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'DiagnosticsModels.cpp')

$processLimitPatterns = [ordered]@{
    MaximumProcessArgumentCount = 'MaximumProcessArgumentCount\s*=\s*256U\s*;'
    MaximumProcessArgumentBytes = 'MaximumProcessArgumentBytes\s*=\s*4U\s*\*\s*1024U\s*;'
    MaximumProcessArgumentsBytes = 'MaximumProcessArgumentsBytes\s*=\s*15U\s*\*\s*1024U\s*;'
    MaximumProcessEnvironmentVariableCount = 'MaximumProcessEnvironmentVariableCount\s*=\s*128U\s*;'
    MaximumProcessEnvironmentNameBytes = 'MaximumProcessEnvironmentNameBytes\s*=\s*128U\s*;'
    MaximumProcessEnvironmentValueBytes = 'MaximumProcessEnvironmentValueBytes\s*=\s*4U\s*\*\s*1024U\s*;'
    MaximumProcessEnvironmentBytes = 'MaximumProcessEnvironmentBytes\s*=\s*24U\s*\*\s*1024U\s*;'
}
foreach ($entry in $processLimitPatterns.GetEnumerator()) {
    Assert-Match $processHeaderText $entry.Value "public process bound $($entry.Key)" -CaseSensitive
}
foreach ($entry in @(
    [ordered]@{ name = 'argument count'; pattern = 'request\.arguments\.size\(\)\s*>\s*MaximumProcessArgumentCount[\s\S]{0,180}ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'environment count'; pattern = 'request\.environment\.size\(\)\s*>\s*MaximumProcessEnvironmentVariableCount[\s\S]{0,180}ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'argument bytes'; pattern = 'argument\.size\(\)\s*>\s*MaximumProcessArgumentBytes[\s\S]{0,180}ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'argument aggregate'; pattern = 'argument\.size\(\)\s*>\s*MaximumProcessArgumentsBytes\s*-\s*argumentBytes[\s\S]{0,180}ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'environment bytes'; pattern = 'variable\.name\.size\(\)\s*>\s*MaximumProcessEnvironmentNameBytes[\s\S]{0,260}ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'environment aggregate'; pattern = 'variableBytes\s*>\s*MaximumProcessEnvironmentBytes\s*-\s*environmentBytes[\s\S]{0,180}ErrorCodes::PayloadTooLarge' })) {
    Assert-Match $processSourceText $entry.pattern "process validator typed boundary $($entry.name)" -CaseSensitive
}
Assert-Match $processHeaderText 'must also bound the final quoted UTF-16 command line and environment block' 'P06 exact native encoding guard assignment' -CaseSensitive

Assert-Match $configurationHeaderText 'MaximumAppConfigAllowedRootCount\s*=\s*32U\s*;' 'public AppConfig allowed-root count bound' -CaseSensitive
Assert-Match $configurationSourceText 'config\.allowedRoots\.size\(\)\s*>\s*MaximumAppConfigAllowedRootCount[\s\S]{0,180}ErrorCodes::LimitExceeded' 'AppConfig allowed-root cap and typed error' -CaseSensitive

$diagnosticLimitPatterns = [ordered]@{
    MaximumDiagnosticEventBytes = 'MaximumDiagnosticEventBytes\s*=\s*256U\s*;'
    MaximumDiagnosticRoleBytes = 'MaximumDiagnosticRoleBytes\s*=\s*64U\s*;'
    MaximumDiagnosticFieldCount = 'MaximumDiagnosticFieldCount\s*=\s*64U\s*;'
    MaximumDiagnosticFieldNameBytes = 'MaximumDiagnosticFieldNameBytes\s*=\s*128U\s*;'
    MaximumDiagnosticFieldValueBytes = 'MaximumDiagnosticFieldValueBytes\s*=\s*4U\s*\*\s*1024U\s*;'
}
foreach ($entry in $diagnosticLimitPatterns.GetEnumerator()) {
    Assert-Match $diagnosticsHeaderText $entry.Value "public diagnostic bound $($entry.Key)" -CaseSensitive
}
foreach ($entry in @(
    [ordered]@{ name = 'required event and role'; pattern = 'envelope\.event\.empty\(\)\s*\|\|\s*envelope\.role\.empty\(\)[\s\S]{0,180}ErrorCodes::InvalidRequest' },
    [ordered]@{ name = 'event and role bytes'; pattern = 'envelope\.event\.size\(\)\s*>\s*MaximumDiagnosticEventBytes[\s\S]{0,220}ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'field count'; pattern = 'envelope\.fields\.size\(\)\s*>\s*MaximumDiagnosticFieldCount[\s\S]{0,180}ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'required field name'; pattern = 'field\.name\.empty\(\)[\s\S]{0,180}ErrorCodes::InvalidRequest' },
    [ordered]@{ name = 'field bytes'; pattern = 'field\.name\.size\(\)\s*>\s*MaximumDiagnosticFieldNameBytes[\s\S]{0,240}ErrorCodes::PayloadTooLarge' })) {
    Assert-Match $diagnosticsSourceText $entry.pattern "diagnostic validator typed boundary $($entry.name)" -CaseSensitive
}
Assert-NoMatch $diagnosticsSourceText 'size\(\)\s*>\s*(?:256|64|128|4096)\b' 'diagnostic validator must consume public constants'

$configurationBoundaryTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+resourceAndConfigurationBoundaries\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\nvoid\s+agentAndLegacyMemoryParity)')
Assert-True $configurationBoundaryTestMatch.Success 'resource_and_configuration_boundaries body'
$configurationBoundaryTestText = $configurationBoundaryTestMatch.Groups['body'].Value
Assert-Match $configurationBoundaryTestText 'allowedRoots\.assign\s*\(\s*Domain::MaximumAppConfigAllowedRootCount[\s\S]{0,500}allowedRoots\.push_back[\s\S]{0,300}tooManyAllowedRoots\.error\(\)\.code\s*==\s*Domain::ErrorCodes::LimitExceeded' 'AppConfig allowed-root cap and cap-plus-one typed-error test' -CaseSensitive

$processBoundaryTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+processToolTelemetryAndManagerBounds\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\nvoid\s+deploymentAndManagerDefaults)')
Assert-True $processBoundaryTestMatch.Success 'process_tool_telemetry_and_manager_bounds body'
$processBoundaryTestText = $processBoundaryTestMatch.Groups['body'].Value
foreach ($entry in @(
    [ordered]@{ name = 'argument count'; pattern = 'arguments\.assign\s*\(\s*Domain::MaximumProcessArgumentCount[\s\S]{0,500}tooManyArguments\.error\(\)\.code\s*==\s*Domain::ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'argument bytes'; pattern = 'MaximumProcessArgumentBytes[\s\S]{0,500}oversizedArgument\.error\(\)\.code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'argument aggregate'; pattern = 'MaximumProcessArgumentsBytes[\s\S]{0,700}oversizedArgumentAggregate\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'environment count'; pattern = 'environment\.assign\s*\(\s*Domain::MaximumProcessEnvironmentVariableCount[\s\S]{0,700}tooManyEnvironmentVariables\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'environment name'; pattern = 'MaximumProcessEnvironmentNameBytes[\s\S]{0,600}oversizedEnvironmentName\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'environment value'; pattern = 'MaximumProcessEnvironmentValueBytes[\s\S]{0,600}oversizedEnvironmentValue\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'environment aggregate'; pattern = 'MaximumProcessEnvironmentBytes[\s\S]{0,1200}oversizedEnvironmentAggregate\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' })) {
    Assert-Match $processBoundaryTestText $entry.pattern "process cap and cap-plus-one typed-error test $($entry.name)" -CaseSensitive
}

$diagnosticBoundaryTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+diagnosticsEnvironmentAndGraphics\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\n\}\s*// namespace)')
Assert-True $diagnosticBoundaryTestMatch.Success 'diagnostics_environment_and_graphics body'
$diagnosticBoundaryTestText = $diagnosticBoundaryTestMatch.Groups['body'].Value
foreach ($entry in @(
    [ordered]@{ name = 'event bytes'; pattern = 'event\.assign\s*\(\s*Domain::MaximumDiagnosticEventBytes[\s\S]{0,500}oversizedDiagnosticEvent\.error\(\)\.code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'role bytes'; pattern = 'role\.assign\s*\(\s*Domain::MaximumDiagnosticRoleBytes[\s\S]{0,500}oversizedDiagnosticRole\.error\(\)\.code\s*==\s*Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'field count'; pattern = 'fields\.assign\s*\(\s*Domain::MaximumDiagnosticFieldCount[\s\S]{0,600}tooManyDiagnosticFields\.error\(\)\.code\s*==\s*Domain::ErrorCodes::LimitExceeded' },
    [ordered]@{ name = 'field name bytes'; pattern = 'MaximumDiagnosticFieldNameBytes[\s\S]{0,700}oversizedDiagnosticFieldName\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' },
    [ordered]@{ name = 'field value bytes'; pattern = 'MaximumDiagnosticFieldValueBytes[\s\S]{0,1100}oversizedDiagnosticFieldValue\.error\(\)\.code\s*==[\s\S]{0,100}Domain::ErrorCodes::PayloadTooLarge' })) {
    Assert-Match $diagnosticBoundaryTestText $entry.pattern "diagnostic cap and cap-plus-one typed-error test $($entry.name)" -CaseSensitive
}

foreach ($entry in @(
    [ordered]@{ name = 'process and environment bounds'; pattern = 'Windows process requests admit at most 256 arguments[\s\S]*15 KiB[\s\S]*128 entries[\s\S]*24 KiB' },
    [ordered]@{ name = 'native UTF-16 final guards'; pattern = 'final quoted UTF-16 command line[\s\S]*sanitized inherited environment block' },
    [ordered]@{ name = 'configuration root bound'; pattern = 'AppConfig::allowedRoots[\s\S]*capped at 32[\s\S]*limit_exceeded' },
    [ordered]@{ name = 'diagnostic role bound'; pattern = 'Diagnostic envelopes[\s\S]*64-byte role limit[\s\S]*payload_too_large' },
    [ordered]@{ name = 'unbounded alternative rejected'; pattern = 'Deferring process, environment, configuration-root, or diagnostic-role bounds[\s\S]*rejected' })) {
    Assert-Match $p05BoundsAdr $entry.pattern "P05 Windows-bound ADR anchor $($entry.name)" -CaseSensitive
}

$mcpServerContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IMcpServer.h')
Assert-Match $mcpServerContract 'IMcpTransport\s*&' 'MCP transport injection' -CaseSensitive
Assert-NoMatch $mcpServerContract 'std::(?:istream|ostream)' 'MCP server must not own standard streams' -CaseSensitive
$mcpTransportContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IMcpTransport.h')
Assert-Match $mcpTransportContract 'Result\s*<\s*std::optional\s*<\s*Domain::McpFrame' 'MCP clean EOF result shape' -CaseSensitive

$sessionContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'ISessionHostAdapter.h')
foreach ($operation in @('capabilities','createSession','queryByIdempotencyKey','bootstrap','awaitAcknowledgement','query','recover','cancel','shutdown')) {
    Assert-Match $sessionContract ('\b' + [regex]::Escape($operation) + '\s*\(') "session-host operation missing: $operation" -CaseSensitive
}
$lifecycleContract = Get-Content -Raw -LiteralPath (Join-Path $contractsRoot 'IForgeApplicationLifecycle.h')
Assert-Match $lifecycleContract 'Result\s*<\s*void\s*>\s+start\s*\(\s*\)\s*noexcept' 'typed noexcept lifecycle start' -CaseSensitive
Assert-Match $lifecycleContract 'Result\s*<\s*void\s*>\s+stop\s*\(\s*\)\s*noexcept' 'typed noexcept lifecycle stop' -CaseSensitive

$authorityIssuerContractText = Get-Content -Raw -LiteralPath (
    Join-Path $contractsRoot 'IFileSystemServices.h')
Assert-Match $authorityIssuerContractText '!contains\s*\(\s*grants\s*,\s*intent\s*\)' 'authority issuance requires the intent grant' -CaseSensitive
Assert-Match $authorityIssuerContractText 'contains\s*\(\s*denials\s*,\s*intent\s*\)' 'authority issuance rejects denied intent' -CaseSensitive
Assert-Match $authorityIssuerContractText 'contains\s*\(\s*authority\.grants\(\)\s*,\s*access\s*\)' 'authorized path requires an explicit grant' -CaseSensitive
Assert-Match $authorityIssuerContractText 'contains\s*\(\s*authority\.denials\(\)\s*,\s*access\s*\)' 'authorized path rejects explicit denial' -CaseSensitive

$projectMemoryServiceFakeText = Get-Content -Raw -LiteralPath (
    Join-Path $fakesRoot 'RecordingProjectMemoryService.h')
$projectMemoryRepositoryFakeText = Get-Content -Raw -LiteralPath (
    Join-Path $fakesRoot 'ProjectRepositoryFakes.h')
foreach ($entry in @(
    [ordered]@{ name = 'project-memory service'; text = $projectMemoryServiceFakeText; project = 'request\.projectId' },
    [ordered]@{ name = 'project-memory repository'; text = $projectMemoryRepositoryFakeText; project = 'projectId_' })) {
    $sensitiveText = [string]$entry.text
    Assert-Match $sensitiveText '\bexportMemory\s*\([\s\S]*?writeAuthority\.intent\(\)\s*!=\s*Domain::FileAccess::Write' "$($entry.name) export requires Write intent" -CaseSensitive
    Assert-Match $sensitiveText '\bexportMemory\s*\([\s\S]*?!authorization\.matches\s*\(\s*writeAuthority\s*,\s*context\s*\)' "$($entry.name) export binds authority and context" -CaseSensitive
    Assert-Match $sensitiveText ('\bexportMemory\s*\([\s\S]*?!authorization\.matchesProject\s*\(\s*' + [string]$entry.project + '\s*\)') "$($entry.name) export binds the explicit project" -CaseSensitive
    Assert-Match $sensitiveText '\bexportMemory\s*\([\s\S]*?authorization\.toolName\(\)\s*!=\s*"project_memory\.export"' "$($entry.name) export exact tool" -CaseSensitive
    Assert-Match $sensitiveText '\bexportMemory\s*\([\s\S]*?authorization\.effect\(\)\s*!=\s*Domain::ToolEffect::Write' "$($entry.name) export exact effect" -CaseSensitive
}
$applicationServiceFakeText = Get-Content -Raw -LiteralPath (
    Join-Path $fakesRoot 'ApplicationServiceFakes.h')
foreach ($agentStatusAnchor in @(
    'StatusToolName\s*=\s*"agent_run_status"',
    'authorization\.toolName\(\)\s*!=\s*StatusToolName',
    'authorization\.effect\(\)\s*!=\s*Domain::ToolEffect::Write',
    'mutationAuthority\.intent\(\)\s*!=\s*Domain::FileAccess::Write',
    'hasMutationGrant\s*\(\s*mutationAuthority\s*\)',
    '!authorization\.matchesProject\s*\(\s*mutationAuthority\.projectId\(\)\s*\)',
    '!authorization\.matches\s*\(\s*mutationAuthority\s*,\s*context\s*\)',
    'consumedStatusRequestId_',
    'consumedStatusSessionId_')) {
    Assert-Match $applicationServiceFakeText $agentStatusAnchor "agent status sensitive-boundary anchor $agentStatusAnchor" -CaseSensitive
}
foreach ($externalAuthorizationAnchor in @(
    '\bdeploy\s*\([\s\S]*?writeAuthority\.intent\(\)\s*!=\s*Domain::FileAccess::Write',
    '\bdeploy\s*\([\s\S]*?!authorization\.matches\s*\(\s*writeAuthority\s*,\s*context\s*\)',
    '\bdeploy\s*\([\s\S]*?!authorization\.matchesProject\s*\(\s*writeAuthority\.projectId\(\)\s*\)',
    '\bdeploy\s*\([\s\S]*?authorization\.effect\(\)\s*!=\s*Domain::ToolEffect::Write',
    '\bactivate\s*\([\s\S]*?executionAuthority\.intent\(\)\s*!=\s*Domain::FileAccess::Execute',
    '\bactivate\s*\([\s\S]*?!authorization\.matches\s*\(\s*executionAuthority\s*,\s*context\s*\)',
    '\bactivate\s*\([\s\S]*?!authorization\.matchesProject\s*\(\s*executionAuthority\.projectId\(\)\s*\)',
    '\bactivate\s*\([\s\S]*?authorization\.effect\(\)\s*!=\s*Domain::ToolEffect::Execute',
    '\bexecute\s*\([\s\S]*?mutationAuthority\.intent\(\)\s*!=\s*Domain::FileAccess::Write',
    '\bexecute\s*\([\s\S]*?!authorization\.matches\s*\(\s*mutationAuthority\s*,\s*context\s*\)',
    '\bexecute\s*\([\s\S]*?!authorization\.matchesProject\s*\(\s*mutationAuthority\.projectId\(\)\s*\)',
    '\bexecute\s*\([\s\S]*?authorization\.effect\(\)\s*!=\s*expectedEffect')) {
    Assert-Match $externalFakeText $externalAuthorizationAnchor "external typed-service authorization anchor $externalAuthorizationAnchor" -CaseSensitive
}
$capabilityAdrText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot '.forge-codex\state\decisions\P05-004-interface-issued-capability-values.md')
Assert-Match $capabilityAdrText 'projectless tool call remains projectless' 'projectless capability rationale' -CaseSensitive
Assert-Match $capabilityAdrText 'Typed LM Studio and installer services are not MCP handlers' 'typed service single-parser rationale' -CaseSensitive
Assert-Match $capabilityAdrText 'application tool handler is the single parser' 'single canonical parser ownership' -CaseSensitive

$continuityHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'ContinuityModels.h')
$continuitySourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'ContinuityModels.cpp')
$hostSessionMatch = [regex]::Match(
    $continuityHeaderText,
    '(?ms)^\s*struct\s+HostSession\s+final\s*\{(?<body>.*?)^\s*\};')
Assert-True $hostSessionMatch.Success 'HostSession structure'
Assert-Exact ([regex]::Matches($hostSessionMatch.Groups['body'].Value, ';').Count) 8 'HostSession exact field count'
Assert-Match $hostSessionMatch.Groups['body'].Value '^\s*SessionId\s+id\s*;\s*ProjectId\s+projectId\s*;\s*ContinuityOperationId\s+operationId\s*;\s*SessionId\s+predecessorSessionId\s*;\s*IdempotencyKey\s+idempotencyKey\s*;\s*std::optional\s*<\s*ProviderSessionId\s*>\s+providerSessionId\s*;\s*std::optional\s*<\s*std::string\s*>\s+model\s*;\s*HostSessionStatus\s+status\s*\{\s*HostSessionStatus::Creating\s*\}\s*;\s*$' 'HostSession exact eight-field order' -CaseSensitive
$sessionCreationMatch = [regex]::Match(
    $continuityHeaderText,
    '(?ms)^\s*struct\s+SessionCreationRequest\s+final\s*\{(?<body>.*?)^\s*\};')
Assert-True $sessionCreationMatch.Success 'SessionCreationRequest structure'
Assert-Exact ([regex]::Matches($sessionCreationMatch.Groups['body'].Value, ';').Count) 4 'SessionCreationRequest exact field count'
Assert-Match $sessionCreationMatch.Groups['body'].Value '^\s*ContinuityOperationId\s+operationId\s*;\s*ProjectId\s+projectId\s*;\s*SessionId\s+predecessorSessionId\s*;\s*IdempotencyKey\s+idempotencyKey\s*;\s*$' 'SessionCreationRequest exact four-field order' -CaseSensitive
foreach ($validatorName in @(
    'validateHostSessionBinding',
    'validateBootstrapCompatibility',
    'validateHandoffAcknowledgement')) {
    Assert-Match $continuitySourceText ('\bResult<void>\s+' + $validatorName + '\s*\(') "continuity validator $validatorName" -CaseSensitive
}
$continuityTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+continuityBindingAndIsolation\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\nvoid\s+processToolTelemetryAndManagerBounds)')
Assert-True $continuityTestMatch.Success 'continuity_binding_and_isolation body'
$continuityTestText = $continuityTestMatch.Groups['body'].Value
foreach ($continuityAnchor in @(
    'mismatchedSession\.projectId','mismatchedSession\.operationId',
    'mismatchedSession\.predecessorSessionId','mismatchedSession\.idempotencyKey',
    'mismatchedSession\.id','handoffWithoutSuccessor\.successorSession\.reset',
    'mismatchedOperation\.projectId','mismatchedOperation\.operationId',
    'mismatchedOperation\.handoffId','mismatchedOperation\.predecessorSessionId',
    'mismatchedHandoff\.successorSession->sessionId',
    'mismatchedAcknowledgement\.successorSessionId',
    'mismatchedAcknowledgement\.handoffId',
    'mismatchedAcknowledgement\.adapterId',
    'mismatchedAcknowledgement\.canonicalHandoffSha256')) {
    Assert-Match $continuityTestText $continuityAnchor "continuity mismatch anchor $continuityAnchor" -CaseSensitive
}
$sessionFakeText = Get-Content -Raw -LiteralPath (
    Join-Path $fakesRoot 'RecordingSessionHostAdapter.h')
Assert-Match $sessionFakeText 'lastBootstrapSession_\.emplace\s*\(\s*session\s*\)' 'session fake stores complete successful bootstrap' -CaseSensitive
Assert-Match $sessionFakeText '!sameSessionBinding\s*\(\s*lastBootstrapSession_\.value\(\)\s*,\s*session\s*\)' 'acknowledgement requires exact prior bootstrap session' -CaseSensitive
foreach ($sessionField in @(
    'id','projectId','operationId','predecessorSessionId','idempotencyKey',
    'providerSessionId','model','status')) {
    Assert-Match $sessionFakeText ('left\.' + $sessionField + '\s*==\s*right\.' + $sessionField) "session fake equality field $sessionField" -CaseSensitive
}
Assert-Match $contractMainText '\btestSessionHostContract\s*\(\s*fixture\s*\)\s*;' 'session-host contract invocation' -CaseSensitive
foreach ($sessionAcknowledgementAnchor in @(
    'acknowledgementPredecessorMismatch\.predecessorSessionId[\s\S]{0,220}expectSubstitutedSessionRejected\s*\(\s*acknowledgementPredecessorMismatch',
    'acknowledgementProviderMismatch\.providerSessionId\.reset\s*\(\s*\)[\s\S]{0,180}expectSubstitutedSessionRejected\s*\(\s*acknowledgementProviderMismatch',
    'acknowledgementModelMismatch\.model\s*=[\s\S]{0,180}expectSubstitutedSessionRejected\s*\(\s*acknowledgementModelMismatch',
    'acknowledgementStatusMismatch\.status\s*=[\s\S]{0,180}expectSubstitutedSessionRejected\s*\(\s*acknowledgementStatusMismatch')) {
    Assert-Match $contractMainText $sessionAcknowledgementAnchor "session acknowledgement runtime negative $sessionAcknowledgementAnchor" -CaseSensitive
}

$deploymentSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'DeploymentModels.cpp')
foreach ($deploymentAnchor in @(
    'request\.action\s*!=\s*DeploymentAction::Purge',
    '!request\.preserveUserData\s*\|\|\s*request\.confirmation',
    'if\s*\(\s*request\.preserveUserData\s*\)',
    'if\s*\(\s*!request\.confirmation\s*\)',
    'validateDestructiveConfirmation\s*\(\s*\*request\.confirmation\s*,\s*"purge"\s*,\s*expectedPurgeScope\s*,\s*expectedPurgeToken\s*\)')) {
    Assert-Match $deploymentSourceText $deploymentAnchor "deployment purge invariant $deploymentAnchor" -CaseSensitive
}
$deploymentTestMatch = [regex]::Match(
    $domainTestSource,
    'void\s+deploymentAndManagerDefaults\s*\(\s*\)\s*\{(?<body>[\s\S]*?)(?=\r?\nvoid\s+diagnosticsEnvironmentAndGraphics)')
Assert-True $deploymentTestMatch.Success 'deployment_and_manager_defaults body'
$deploymentTestText = $deploymentTestMatch.Groups['body'].Value
foreach ($deploymentTestAnchor in @(
    'const\s+Domain::DeploymentRequest\s+install','DeploymentAction::Uninstall',
    'nonDestructive\.preserveUserData\s*=\s*false','nonDestructive\.confirmation',
    'DeploymentAction::Purge','purge\.preserveUserData\s*=\s*true',
    'purge\.confirmation\.reset','DestructiveConfirmation\s*\{\s*"reset"',
    'DestructiveConfirmation\s*\{\s*"purge"\s*,\s*"one-project"',
    'DestructiveConfirmation\s*\{\s*"purge"\s*,\s*"all-user-data"\s*,\s*"wrong-token"')) {
    Assert-Match $deploymentTestText $deploymentTestAnchor "deployment boundary test $deploymentTestAnchor" -CaseSensitive
}

$managerHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $domainHeaderRoot 'ManagerModels.h')
$configurationSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $domainSourceRoot 'ConfigurationModels.cpp')
Assert-Match $managerHeaderText 'DefaultManagerDashboardPort\s*=\s*7788\s*;' 'manager dashboard port 7788' -CaseSensitive
Assert-Exact ([regex]::Matches($managerHeaderText, 'dashboardPort\s*\{\s*DefaultManagerDashboardPort\s*\}').Count) 2 'ManagerSettings and ManagerStatus dashboard defaults'
Assert-Match $configurationSourceText 'DashboardConfig\s*\{\s*"127\.0\.0\.1"\s*,\s*DefaultManagerDashboardPort\s*,' 'AppConfig uses the shared dashboard port' -CaseSensitive
foreach ($loopbackSource in @(
    [ordered]@{ name = 'ManagerSettings'; text = $managerSource },
    [ordered]@{ name = 'AppConfig'; text = $configurationSourceText })) {
    Assert-Match ([string]$loopbackSource.text) 'return\s+host\s*==\s*"127\.0\.0\.1"\s*\|\|\s*host\s*==\s*"::1"\s*;' "$($loopbackSource.name) exact numeric loopback allowlist" -CaseSensitive
    Assert-NoMatch ([string]$loopbackSource.text) '"localhost"' "$($loopbackSource.name) must not accept a hostname" -CaseSensitive
}
foreach ($table in @(
    [ordered]@{ name = 'rejectedDashboardHosts'; validator = 'validateManagerSettings' },
    [ordered]@{ name = 'rejectedAppConfigDashboardHosts'; validator = 'validateAppConfig' })) {
    $tableMatch = [regex]::Match(
        $domainTestSource,
        ('\b' + [regex]::Escape([string]$table.name) + '\s*\{(?<body>[\s\S]*?)\};'))
    Assert-True $tableMatch.Success "dashboard rejection table $($table.name)"
    foreach ($literal in @(
        '"localhost"','"LOCALHOST"','"0.0.0.0"','"127.0.0.2"',
        '"::"','"[::1]"','"example.test"','""')) {
        Assert-Match $tableMatch.Groups['body'].Value ([regex]::Escape($literal)) "$($table.name) contains $literal" -CaseSensitive
    }
    Assert-Match $domainTestSource ('for\s*\(\s*const\s+auto\s*&\s*host\s*:\s*' + [regex]::Escape([string]$table.name) + '\s*\)[\s\S]*?REQUIRE\s*\(\s*!Domain::' + [regex]::Escape([string]$table.validator) + '\s*\(') "$($table.name) rejection loop is asserted" -CaseSensitive
}

$platformStorageRuntimeText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\PlatformStorageFakeContractTests.h')
foreach ($configurationRuntimeAnchor in @(
    'configuration\.update\s*\(\s*configPatch\s*,\s*context\s*\)\.hasValue\s*\(\s*\)',
    'ConfigurationStoreCall::Update',
    'lastCapture\(\)->patch->allowedRoots\s*==\s*configPatch\.allowedRoots',
    'lastCapture\(\)->patch->dashboardPort\s*==\s*configPatch\.dashboardPort')) {
    Assert-Match $platformStorageRuntimeText $configurationRuntimeAnchor "configuration-store update runtime anchor $configurationRuntimeAnchor" -CaseSensitive
}
$repositoryManagerRuntimeText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\RepositoryDiagnosticsManagerFakeContractTests.h')
foreach ($repositoryManagerRuntimeAnchor in @(
    'continuity fake accepted a substituted acknowledgement digest',
    'continuity fake did not persist an exact acknowledgement',
    'continuity fake did not retain exact acknowledgement evidence',
    'client\.control\s*\(\s*controlRequest\s*,\s*fixture\.activeContext\(\)\s*\)\.hasValue\s*\(\s*\)',
    'client\.lastControlRequest\(\)',
    'client\.control\s*\(\s*controlRequest\s*,\s*fixture\.expiredContext\(\)\s*\)',
    'manager client did not forward the control deadline')) {
    Assert-Match $repositoryManagerRuntimeText $repositoryManagerRuntimeAnchor "repository/manager runtime anchor $repositoryManagerRuntimeAnchor" -CaseSensitive
}
Assert-Match $mcpServerContract 'virtual\s+void\s+cancel\s*\(\s*const\s+Domain::OperationId\s*&\s*operationId\s*\)\s*noexcept\s*=\s*0\s*;' 'exact MCP OperationId cancellation contract' -CaseSensitive
Assert-NoMatch $mcpServerContract 'cancel\s*\(\s*const\s+Domain::RequestId' 'MCP cancellation must not target RequestId' -CaseSensitive
$mcpTestText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\McpCancellationContractTests.h')
Assert-Match $mcpTestText 'using\s+McpServerCancelSignature\s*=\s*void\s*\(\s*ProductContracts::IMcpServer::\*\s*\)\s*\(\s*const\s+Domain::OperationId\s*&\s*\)\s*noexcept\s*;' 'MCP cancellation member-pointer signature proof' -CaseSensitive
foreach ($mcpAnchor in @(
    'inbound\.emplace_back\s*\(\s*16\s*,\s*''x''\s*\)',
    'inbound\.emplace_back\s*\(\s*17\s*,\s*''x''\s*\)',
    'exactCapInbound',
    'outbound frame at the exact byte cap',
    'clean EOF distinction',
    'outbound queue capacity',
    'MCP receive ignored operation cancellation',
    'MCP send ignored operation cancellation',
    'MCP receive ignored the operation deadline',
    'MCP send ignored the operation deadline',
    'MCP receive accepted work after shutdown',
    'MCP send accepted work after shutdown')) {
    Assert-Match $mcpTestText $mcpAnchor "MCP runtime boundary $mcpAnchor" -CaseSensitive
}
$mcpTransportFakeText = Get-Content -Raw -LiteralPath (
    Join-Path $fakesRoot 'McpTransportFake.h')
Assert-Match $mcpTransportFakeText 'DefaultFrameBytesMaximum\s*=\s*1''048''576' 'MCP fake default byte cap' -CaseSensitive
Assert-Match $mcpTransportFakeText 'DefaultFrameCountMaximum\s*=\s*64' 'MCP fake default frame-count cap' -CaseSensitive
$applicationContractTestText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'tests\Contracts\ApplicationServiceFakeContractTests.h')
Assert-Match $applicationContractTestText 'MCP server ignored targeted cancellation' 'MCP targeted cancellation test' -CaseSensitive
Assert-Match $applicationContractTestText 'MCP targeted cancellation leaked to another operation' 'MCP cancellation isolation test' -CaseSensitive

$cliRootHeaderText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Hosts\Cli\CliCompositionRoot.h')
$cliRootSourceText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Hosts\Cli\CliCompositionRoot.cpp')
$cliMainText = Get-Content -Raw -LiteralPath (
    Join-Path $WorkspaceRoot 'src\Hosts\Cli\main.cpp')
Assert-Match $cliRootHeaderText '\bclass\s+CliCompositionRoot\s+final\s*\{' 'CLI composition root is final' -CaseSensitive
Assert-Exact ([regex]::Matches($cliRootHeaderText, 'CliCompositionRoot\s*\([^\)]*&&[^\)]*\)\s*=\s*delete\s*;').Count) 1 'CLI composition root deleted move construction'
Assert-Exact ([regex]::Matches($cliRootHeaderText, 'operator\s*=\s*\([^\)]*CliCompositionRoot[^\)]*\)\s*=\s*delete\s*;').Count) 2 'CLI composition root deleted copy/move assignment'
Assert-Match $cliRootHeaderText 'CliCompositionRoot\s*\(\s*const\s+CliCompositionRoot\s*&\s*\)\s*=\s*delete\s*;' 'CLI composition root deleted copy construction' -CaseSensitive
Assert-Match $cliRootHeaderText 'std::unique_ptr\s*<\s*CliApplication\s*>\s+application_\s*;' 'CLI application unique ownership' -CaseSensitive
Assert-Match $cliRootSourceText 'application_\s*\{\s*std::make_unique\s*<\s*CliApplication\s*>\s*\(\s*\)\s*\}' 'CLI executable-owned application construction' -CaseSensitive
Assert-Match $cliRootSourceText 'return\s+application_->run\s*\(\s*arguments\s*\)\s*;' 'CLI composition-root delegation' -CaseSensitive
Assert-Exact ([regex]::Matches($cliMainText, 'CliCompositionRoot\s+compositionRoot\s*;').Count) 1 'one local CLI composition root'
Assert-Match $cliMainText 'return\s+compositionRoot\.run\s*\(\s*arguments\s*\)\s*;' 'main delegates to the local composition root' -CaseSensitive
Assert-NoMatch ($cliRootHeaderText + $cliRootSourceText + $cliMainText) '(?:ServiceLocator|getInstance\s*\(|globalCompositionRoot|(?m:^\s*static\b[^\r\n;\{]*\bCliCompositionRoot\b))' 'CLI global/service-locator ownership is prohibited' -CaseSensitive
Assert-Match $cmake 'add_test\s*\(\s*NAME\s+ForgeConductor\.Cli\.SelfTest\s+COMMAND\s+\$<TARGET_FILE:ForgeConductor\.Cli>\s+--self-test\s*\)' 'CLI exact self-test command' -CaseSensitive
$budgetPath = Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\resource-budgets.json'
Assert-Exact (Get-FileSha256 $budgetPath) 'f80c5d57081d47b87ddb77027f843c912bcf3c3e558c7ade42b4db4828760965' 'authoritative resource-budget hash'
$budgets = Read-Json $budgetPath
Assert-Exact ([int]$budgets.profiles.constrained_8gb.open_project_repositories_max) 4 'constrained repository cap'
Assert-Exact ([int]$budgets.profiles.standard_16gb.open_project_repositories_max) 8 'standard repository cap'
Assert-Exact ([int]$budgets.profiles.expanded_32gb_plus.open_project_repositories_max) 16 'expanded repository cap'
foreach ($profileName in @('constrained_8gb','standard_16gb','expanded_32gb_plus')) {
    $profile = $budgets.profiles.$profileName
    Assert-Exact ([int]$profile.telemetry_pending_snapshots_max) 1 "$profileName mailbox cap"
    Assert-Exact ([int]$profile.tool_stdout_bytes_max) 80000 "$profileName stdout cap"
    Assert-Exact ([int]$profile.tool_stderr_bytes_max) 20000 "$profileName stderr cap"
    Assert-Exact ([int]$profile.shell_timeout_seconds_max) 120 "$profileName shell cap"
    Assert-Exact ([int]$profile.mcp_input_line_bytes_max) 1048576 "$profileName MCP cap"
    Assert-Exact ([int]$profile.named_pipe_frame_bytes_max) 2097152 "$profileName pipe cap"
}

$frameworkRoot = Join-Path $WorkspaceRoot '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count before P05 builds'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count before P05 builds'
Assert-Exact ([string]$frameworkBefore.sha256) 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' 'sealed Forsetti hash before P05 builds'

$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G05: building complete x64 Debug tree from a fresh build directory.'
& $buildScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Fresh
Write-Host 'G05: testing x64 Debug Domain/Contracts.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G05
Write-Host 'G05: testing x64 Debug P04 regression.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel -Label G04
Write-Host 'G05: building complete x64 Release tree.'
& $buildScript -Configuration Release -Architecture x64 -Parallel $Parallel
Write-Host 'G05: testing x64 Release Domain/Contracts.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G05
Write-Host 'G05: testing x64 Release P04 regression.'
& $testScript -Configuration Release -Architecture x64 -Parallel $Parallel -Label G04

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$expectedG05Tests = @(
    'ForgeConductor.Cli.SelfTest',
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Domain.UnitTests')
$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if ($ctestCommand) {
    $ctestPath = $ctestCommand.Source
} else {
    $toolchainStatePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
    Assert-True (Test-Path -LiteralPath $toolchainStatePath -PathType Leaf) 'toolchain state for CTest resolution'
    $toolchainState = Read-Json $toolchainStatePath
    $ctestCandidate = [string]$toolchainState.tools.ctest
    Assert-True (-not [string]::IsNullOrWhiteSpace($ctestCandidate) -and (Test-Path -LiteralPath $ctestCandidate -PathType Leaf)) 'CTest executable from toolchain state'
    $ctestPath = (Resolve-Path -LiteralPath $ctestCandidate).Path
}

foreach ($configuration in @('Debug','Release')) {
    foreach ($relativeArtifact in @(
        "lib/$configuration/ForgeConductor.Domain.lib",
        "bin/$configuration/ForgeConductor.Domain.UnitTests.exe",
        "bin/$configuration/ForgeConductor.Contracts.ContractTests.exe",
        "bin/$configuration/ForgeConductor.Contracts.HeaderSelfContainment.exe",
        "bin/$configuration/forge-conductor.exe",
        "bin/$configuration/ForgeConductor.ForsettiHostSmoke.exe")) {
        $artifact = Join-Path $buildRoot $relativeArtifact.Replace('/', '\')
        Assert-True (Test-Path -LiteralPath $artifact -PathType Leaf) "$configuration artifact missing: $relativeArtifact"
        Assert-True ([long](Get-Item -LiteralPath $artifact).Length -gt 0) "$configuration artifact is empty: $relativeArtifact"
    }

    $ctestJsonText = (& $ctestPath --test-dir $buildRoot -C $configuration -L G05 --show-only=json-v1) -join [Environment]::NewLine
    Assert-Exact $LASTEXITCODE 0 "$configuration G05 CTest JSON inventory command"
    try {
        $ctestInventory = $ctestJsonText | ConvertFrom-Json
    } catch {
        throw "G05 assertion failed: invalid $configuration CTest JSON inventory - $($_.Exception.Message)"
    }
    Assert-Set @($ctestInventory.tests | ForEach-Object { $_.name }) $expectedG05Tests "$configuration exact G05 CTest inventory"
    $buildRootForward = $buildRoot.Replace('\', '/')
    $expectedCTestCommands = [ordered]@{
        'ForgeConductor.Domain.UnitTests' = @("$buildRootForward/bin/$configuration/ForgeConductor.Domain.UnitTests.exe")
        'ForgeConductor.Contracts.ContractTests' = @("$buildRootForward/bin/$configuration/ForgeConductor.Contracts.ContractTests.exe")
        'ForgeConductor.Contracts.HeaderSelfContainment' = @("$buildRootForward/bin/$configuration/ForgeConductor.Contracts.HeaderSelfContainment.exe")
        'ForgeConductor.Cli.SelfTest' = @("$buildRootForward/bin/$configuration/forge-conductor.exe", '--self-test')
    }
    foreach ($test in @($ctestInventory.tests)) {
        Assert-Exact ([string]$test.config) $configuration "$configuration CTest configuration for $($test.name)"
        Assert-Set @($test.properties | ForEach-Object { $_.name }) @('LABELS','WORKING_DIRECTORY') "$configuration CTest property inventory for $($test.name)"
        $expectedCommand = @($expectedCTestCommands[[string]$test.name])
        Assert-Exact @($test.command).Count $expectedCommand.Count "$configuration CTest command count for $($test.name)"
        for ($commandIndex = 0; $commandIndex -lt $expectedCommand.Count; $commandIndex++) {
            Assert-Exact ([string]$test.command[$commandIndex]) $expectedCommand[$commandIndex] "$configuration CTest command item $commandIndex for $($test.name)"
        }
        $labelsProperty = @($test.properties | Where-Object { $_.name -ceq 'LABELS' })
        Assert-Exact $labelsProperty.Count 1 "$configuration CTest label property count for $($test.name)"
        $expectedLabels = if ($test.name -ceq 'ForgeConductor.Cli.SelfTest') {
            @('G04','G05','T-UNIT')
        } else {
            @('G05','T-UNIT')
        }
        Assert-Set @($labelsProperty[0].value) $expectedLabels "$configuration exact CTest labels for $($test.name)"
        $workingDirectoryProperty = @($test.properties | Where-Object { $_.name -ceq 'WORKING_DIRECTORY' })
        Assert-Exact $workingDirectoryProperty.Count 1 "$configuration CTest working-directory property count for $($test.name)"
        Assert-Exact ([string]$workingDirectoryProperty[0].value) $buildRootForward "$configuration CTest working directory for $($test.name)"
    }
}

$generatedHeaderSources = @(Get-ChildItem -LiteralPath (Join-Path $buildRoot 'generated\p05-header-isolation') -File -Filter '*.cpp')
Assert-Exact $generatedHeaderSources.Count ($domainHeaders.Count + $contractHeaders.Count) 'generated isolated-header translation-unit count'
$expectedHeaderSources = [ordered]@{}
foreach ($headerGroup in @(
    [ordered]@{ area = 'Domain'; headers = $domainHeaders },
    [ordered]@{ area = 'Contracts'; headers = $contractHeaders })) {
    foreach ($header in $headerGroup.headers) {
        $includePath = "ForgeConductor/$($headerGroup.area)/$($header.Name)"
        $stem = $includePath.Replace('/', '_').Replace('.', '_')
        $expectedHeaderSources["$stem.cpp"] = "#include <$includePath>`nint ${stem}_isolated() noexcept { return 0; }"
    }
}
Assert-Set @($generatedHeaderSources.Name) @($expectedHeaderSources.Keys) 'exact generated isolated-header translation-unit inventory'
foreach ($source in $generatedHeaderSources) {
    $sourceText = (Get-Content -Raw -LiteralPath $source.FullName).Replace("`r`n", "`n").TrimEnd([char[]]"`r`n")
    Assert-Exact $sourceText $expectedHeaderSources[$source.Name] "generated isolated-header translation unit $($source.Name)"
}
$domainProjectPath = Join-Path $buildRoot 'ForgeConductor.Domain.vcxproj'
Assert-True (Test-Path -LiteralPath $domainProjectPath -PathType Leaf) 'generated Domain Visual Studio project'
$domainProject = Get-Content -Raw -LiteralPath $domainProjectPath
Assert-Match $domainProject '<PlatformToolset>v143</PlatformToolset>' 'Domain v143 toolset'
Assert-Match $domainProject '<WindowsTargetPlatformVersion>10\.0\.26100\.0</WindowsTargetPlatformVersion>' 'Domain SDK pin'
Assert-Match $domainProject '<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>' 'Domain Debug DLL CRT'
Assert-Match $domainProject '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>' 'Domain Release DLL CRT'
$domainDependencyMetadata = @($domainProject -split "`r?`n" | Where-Object {
    $_ -match '<(?:AdditionalIncludeDirectories|PreprocessorDefinitions|AdditionalDependencies|ProjectReference)\b'
}) -join "`n"
Assert-NoMatch $domainDependencyMetadata '(?:UNICODE|_UNICODE|WIN32_LEAN_AND_MEAN|NOMINMAX|Forsetti|nlohmann|sqlite|winhttp|winsock|d3d|d2d|dxgi)' 'Domain generated-project dependency leakage' -CaseSensitive
$projectReferences = @([regex]::Matches($domainProject, '<ProjectReference\s+Include="(?<path>[^"]+)"') | ForEach-Object {
    [IO.Path]::GetFileName($_.Groups['path'].Value)
})
Assert-Set $projectReferences @('ZERO_CHECK.vcxproj') 'Domain generated project references'

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) 'sealed Forsetti file count after P05 builds'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) 'sealed Forsetti bytes after P05 builds'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) 'sealed Forsetti hash after P05 builds'

$gitOutput = & git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1
Assert-Exact $LASTEXITCODE 0 ('git diff --check failed: ' + ($gitOutput -join "`n"))
& (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Verify-Ledger.ps1') -WorkspaceRoot $WorkspaceRoot

$productFileCount = $productFiles.Count
$isolatedHeaderCount = $domainHeaders.Count + $contractHeaders.Count
Write-Host "G05 Domain/Contracts validation passed: $script:AssertionCount fail-closed assertions; $productFileCount product files, $isolatedHeaderCount isolated headers, and x64 Debug/Release G04+G05 tests passed."
