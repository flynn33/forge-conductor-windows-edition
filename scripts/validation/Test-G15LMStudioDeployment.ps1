[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [string]$LMStudioConfigurationPath,

    [string]$RealHostEvidencePath,

    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0
$realHostTimeoutMilliseconds = 540000

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G15 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) `
        "$Message (expected '$Expected', found '$Actual')"
}

function Assert-Set {
    param([object[]]$Actual, [object[]]$Expected, [string]$Message)
    $actualValues = @($Actual | ForEach-Object { [string]$_ } |
        Sort-Object -CaseSensitive)
    $expectedValues = @($Expected | ForEach-Object { [string]$_ } |
        Sort-Object -CaseSensitive)
    Assert-Exact $actualValues.Count $expectedValues.Count "$Message count"
    for ($index = 0; $index -lt $expectedValues.Count; $index++) {
        Assert-Exact $actualValues[$index] $expectedValues[$index] `
            "$Message item $index"
    }
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

function Assert-CrlfTextFile {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -gt 0) "$Message is nonempty"
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    Assert-True (-not $hasBom) "$Message has no UTF-8 BOM"
    $bareLf = 0
    $bareCr = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10 -and
            ($index -eq 0 -or $bytes[$index - 1] -ne 13)) { $bareLf++ }
        if ($bytes[$index] -eq 13 -and
            ($index -eq $bytes.Length - 1 -or $bytes[$index + 1] -ne 10)) {
            $bareCr++
        }
    }
    Assert-Exact $bareLf 0 "$Message bare-LF count"
    Assert-Exact $bareCr 0 "$Message bare-CR count"
    Assert-True ($bytes.Length -ge 2 -and
        $bytes[$bytes.Length - 2] -eq 13 -and
        $bytes[$bytes.Length - 1] -eq 10) "$Message final CRLF"
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    Assert-NoMatch $text '[ \t]+\r\n' "$Message trailing whitespace" -CaseSensitive
}

function Assert-X64PortableExecutable {
    param([string]$Path, [string]$Message)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-True ($bytes.Length -ge 0x40) "$Message DOS header length"
    Assert-Exact ([Text.Encoding]::ASCII.GetString($bytes, 0, 2)) 'MZ' `
        "$Message DOS signature"
    $offset = [BitConverter]::ToInt32($bytes, 0x3C)
    Assert-True ($offset -ge 0x40 -and $offset + 6 -le $bytes.Length) `
        "$Message bounded PE offset"
    Assert-Exact ([Text.Encoding]::ASCII.GetString($bytes, $offset, 4)) `
        "PE`0`0" "$Message PE signature"
    Assert-Exact ([BitConverter]::ToUInt16($bytes, $offset + 4)) `
        ([uint16]0x8664) "$Message x64 machine"
}

function Resolve-CtestExecutable {
    $command = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $statePath = Join-Path $WorkspaceRoot '.forge-codex\state\toolchain.json'
    Assert-True (Test-Path -LiteralPath $statePath -PathType Leaf) `
        'toolchain state exists for CTest resolution'
    $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
    $candidate = [string]$state.tools.ctest
    Assert-True (-not [string]::IsNullOrWhiteSpace($candidate) -and
        (Test-Path -LiteralPath $candidate -PathType Leaf)) `
        'CTest executable exists from toolchain state'
    return (Resolve-Path -LiteralPath $candidate).Path
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
        $rows.Add($path + [char]9 + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        files = $paths.Count
        bytes = $bytes
        sha256 = Get-StringSha256 ($rows -join [char]10)
    }
}

function Invoke-RepositoryIntegrityChecks {
    $output = @(& git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('git diff --check: ' + ($output -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

function Assert-TrackedTreeClean {
    & git -C $WorkspaceRoot diff --quiet --
    Assert-Exact $LASTEXITCODE 0 'tracked working tree is clean'
    & git -C $WorkspaceRoot diff --cached --quiet --
    Assert-Exact $LASTEXITCODE 0 'tracked index is clean'
}

function Assert-NoUntrackedBuildInputs {
    $untracked = @(& git -C $WorkspaceRoot ls-files --others --exclude-standard --)
    Assert-Exact $LASTEXITCODE 0 'untracked-file inventory exit code'
    $buildInputs = @($untracked | Where-Object {
        $portable = ([string]$_).Replace('\\', '/')
        $portable -match '^(?:CMakeLists[.]txt|CMakePresets[.]json|cmake/|include/|src/|tests/|scripts/|vcpkg(?:-configuration|[.]json)|[.]forge-codex/instructions/|[.]forge-codex/state/decisions/)'
    })
    Assert-Exact $buildInputs.Count 0 `
        ('untracked source or build-input paths: ' + ($buildInputs -join ', '))
}

function Assert-DirectoryChainNoReparsePoint {
    param([string]$Path, [string]$Message)
    $resolvedRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    Assert-True ($resolvedPath.StartsWith(
        $resolvedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) "$Message remains inside the workspace"
    $relative = $resolvedPath.Substring($resolvedRoot.Length + 1)
    $current = $resolvedRoot
    foreach ($segment in $relative.Split(
                 [IO.Path]::DirectorySeparatorChar,
                 [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $segment
        $item = Get-Item -Force -LiteralPath $current
        Assert-True (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Message component is not a reparse point: $segment"
    }
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti tree hash'

& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-Python validation'
& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-attribution validation'

$phases = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\phases.json') | ConvertFrom-Json
$phase = @($phases.phases | Where-Object id -ceq 'P15')
Assert-Exact $phase.Count 1 'single P15 phase plan entry'
Assert-Exact ([string]$phase[0].title) `
    'Windows LM Studio environment and deployment' 'P15 title'
Assert-Set @($phase[0].dependencies) @('P14') 'P15 dependency'
Assert-Set @($phase[0].required_gates) @('G15') 'P15 required gate'
Assert-Set @($phase[0].required_work) @(
    'Evidence-based path/config discovery',
    'Transactional primary/fallback deploy',
    'Smoke, rollback, drift, host-sync tests') 'P15 required work'

$gates = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\gates.json') | ConvertFrom-Json
$gate = @($gates.gates | Where-Object id -ceq 'G15')
Assert-Exact $gate.Count 1 'single G15 gate plan entry'
Assert-Exact ([string]$gate[0].title) 'LM Studio deployment' 'G15 title'
Assert-Exact ([string]$gate[0].class) 'hard' 'G15 class'
Assert-Exact ([string]$gate[0].acceptance) `
    'Transactional deploy/smoke/rollback/drift pass; real-host evidence required when available.' `
    'G15 acceptance'

$matrix = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\plans\test-matrix.json') | ConvertFrom-Json
$suite = @($matrix.suites | Where-Object id -ceq 'T-LMS')
Assert-Exact $suite.Count 1 'single T-LMS suite entry'
Assert-Exact ([string]$suite[0].scope) `
    'LM Studio discovery/deploy/rollback/smoke/host synchronization' `
    'T-LMS scope'
Assert-True ([bool]$suite[0].required) 'T-LMS remains required'

$p15Files = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Contracts/Contracts.h',
    'include/ForgeConductor/Contracts/ILMStudioHostActivator.h',
    'include/ForgeConductor/Contracts/ILMStudioServeVerifier.h',
    'include/ForgeConductor/Domain/EnvironmentModels.h',
    'include/ForgeConductor/Domain/ProcessModels.h',
    'include/ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h',
    'include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'src/Domain/EnvironmentModels.cpp',
    'src/Domain/ProcessModels.cpp',
    'src/Infrastructure/Windows/LMStudioConfigurationCodec.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioDeploymentService.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioEnvironment.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioHostActivator.cpp',
    'src/Infrastructure/Windows/WindowsLMStudioServeVerifier.cpp',
    'src/Infrastructure/Windows/WindowsProcessSupervisor.cpp',
    'tests/Application/LMStudioDeploymentServiceTests.cpp',
    'tests/Contracts/main.cpp',
    'tests/Infrastructure/InfrastructureTestMain.cpp',
    'tests/Infrastructure/LMStudioConfigurationCodecTests.cpp',
    'tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp',
    'tests/Infrastructure/WindowsLMStudioEnvironmentTests.cpp',
    'tests/Infrastructure/WindowsLMStudioHostActivatorTests.cpp',
    'tests/Infrastructure/WindowsLMStudioRealHostTests.cpp',
    'tests/Infrastructure/WindowsLMStudioServeVerifierTests.cpp',
    '.forge-codex/state/decisions/P15-001-evidence-based-lm-studio-deployment-and-maintenance-authority.md',
    'scripts/validation/Test-G15LMStudioDeployment.ps1')
foreach ($relative in $p15Files) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "P15 file exists: $relative"
    Assert-CrlfTextFile $path "P15 text file $relative"
}

$productionPaths = @($p15Files | Where-Object {
    $_ -match '^(?:include|src)/'
})
$productionText = ($productionPaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join "`n"
Assert-NoMatch $productionText 'C:\\Users\\' `
    'production code contains no machine-specific user path' -CaseSensitive
Assert-NoMatch $productionText `
    '\b(?:SendInput|mouse_event|keybd_event|IUIAutomation|SetCursorPos|FindWindowW)\b' `
    'LM Studio integration contains no external GUI automation'

$cmakeText = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
Assert-Match $cmakeText `
    'add_executable\(ForgeConductor[.]LMStudio[.]RealHostTests[\s\S]*?WindowsLMStudioRealHostTests[.]cpp\)' `
    'real-host runner target is declared' -CaseSensitive
Assert-NoMatch $cmakeText `
    'add_test\(\s*(?:NAME\s+)?ForgeConductor[.]LMStudio[.]RealHostTests' `
    'mutating real-host runner is not registered with CTest' -CaseSensitive
Assert-Match $cmakeText `
    'ForgeConductor[.]Contracts[.]ContractTests\s+PROPERTIES[\s\S]*?LABELS\s+"[^"]*G15' `
    'contract tests carry G15 label' -CaseSensitive
Assert-Match $cmakeText `
    'ForgeConductor[.]Infrastructure[.]ProcessTests\s+PROPERTIES[\s\S]*?LABELS\s+"[^"]*G15' `
    'process tests carry G15 label' -CaseSensitive

$adr = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    '.forge-codex\state\decisions\P15-001-evidence-based-lm-studio-deployment-and-maintenance-authority.md')
Assert-Match $adr 'one authoritative G15 rebuild/test invocation' `
    'owner-adjusted one-pass G15 scope is recorded'
Assert-Match $adr 'P15 does not click, type into,\s+inspect, or otherwise automate' `
    'external LM Studio GUI automation prohibition is recorded'
Assert-Match $adr 'Foreign server\s+entries and unknown root and per-server fields survive' `
    'foreign configuration preservation is recorded'
Assert-Match $adr 'restores the\s+exact original configuration bytes' `
    'exact rollback requirement is recorded'

Invoke-RepositoryIntegrityChecks

if ($StaticOnly) {
    Write-Host "G15 static validation passed ($script:AssertionCount assertions)."
    return
}

if ([string]::IsNullOrWhiteSpace($LMStudioConfigurationPath)) {
    $LMStudioConfigurationPath = Join-Path $env:USERPROFILE '.lmstudio\mcp.json'
}
$LMStudioConfigurationPath = (Resolve-Path -LiteralPath $LMStudioConfigurationPath).Path
$canonicalRealHostEvidencePath = [IO.Path]::GetFullPath((Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\windows-lm-studio-real-host.json'))
if ([string]::IsNullOrWhiteSpace($RealHostEvidencePath)) {
    $RealHostEvidencePath = $canonicalRealHostEvidencePath
} else {
    $RealHostEvidencePath = [IO.Path]::GetFullPath($RealHostEvidencePath)
}
Assert-True ([string]::Equals(
    $RealHostEvidencePath,
    $canonicalRealHostEvidencePath,
    [StringComparison]::OrdinalIgnoreCase)) `
    'real-host evidence uses the one canonical P15 evidence file'
$realHostEvidenceDirectory = Split-Path -Parent $RealHostEvidencePath
Assert-True (Test-Path -LiteralPath $realHostEvidenceDirectory -PathType Container) `
    'canonical P15 evidence directory already exists'
Assert-DirectoryChainNoReparsePoint $realHostEvidenceDirectory `
    'canonical P15 evidence directory'
Assert-True (-not (Test-Path -LiteralPath $RealHostEvidencePath)) `
    'canonical real-host evidence does not already exist before its one invocation'

$baselinePath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\windows-lm-studio-host-baseline.json'
$baseline = Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json
Assert-True (Test-Windows11) 'qualification host is Windows 11'
Assert-Exact ([int]$baseline.schema_version) 1 'real-host baseline schema version'
Assert-Exact ([string]$baseline.phase) 'P15' 'real-host baseline phase'
Assert-Exact ([string]$baseline.checkpoint) 'windows_lm_studio_host_baseline' `
    'real-host baseline checkpoint'
Assert-Exact ([string]$baseline.status) 'passed' 'real-host baseline status'
Assert-Exact ([string]$baseline.gate) 'G15' 'real-host baseline gate'
Assert-Exact ([string]$baseline.observation_mode) 'read_only' `
    'real-host baseline observation mode'
Assert-Exact ([int]$baseline.effective_exit_code) 0 `
    'real-host baseline command exit code'
Assert-Exact ([bool]$baseline.timed_out) $false `
    'real-host baseline command did not time out'
Assert-True ([bool]$baseline.lm_studio.executable_exists) `
    'baseline records an installed LM Studio executable'
$hostExecutable = [string]$baseline.lm_studio.executable
Assert-True (Test-Path -LiteralPath $hostExecutable -PathType Leaf) `
    'baseline LM Studio executable still exists'
Assert-Exact (Get-FileSha256 $hostExecutable) `
    ([string]$baseline.lm_studio.sha256) `
    'LM Studio executable still matches the captured host baseline'
Assert-Exact $LMStudioConfigurationPath `
    ([string]$baseline.configuration.path) `
    'qualification uses the captured LM Studio configuration path'
Assert-True ([bool]$baseline.configuration.valid_json) `
    'baseline LM Studio configuration was valid JSON'
Assert-Exact ([bool]$baseline.configuration.reparse_point) $false `
    'baseline LM Studio configuration was not a reparse point'
Assert-True (Test-Path -LiteralPath $LMStudioConfigurationPath -PathType Leaf) `
    'baseline LM Studio configuration still exists'
$configurationItem = Get-Item -Force -LiteralPath $LMStudioConfigurationPath
Assert-Exact (($configurationItem.Attributes -band `
        [IO.FileAttributes]::ReparsePoint) -ne 0) $false `
    'live LM Studio configuration is not a reparse point'

Assert-TrackedTreeClean
Assert-NoUntrackedBuildInputs

$buildTargets = @(
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.UnitTests',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.LMStudio.RealHostTests')
$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
Write-Host 'G15: running the single authoritative affected-target Debug rebuild.'
& $buildScript -Configuration Debug -Architecture x64 `
    -Target $buildTargets -Parallel $Parallel
Assert-True $? 'single authoritative G15 affected-target build'

$ctest = Resolve-CtestExecutable
Push-Location $WorkspaceRoot
try {
    $inventoryText = @(& $ctest --preset windows-msvc-x64-debug `
        --show-only=json-v1 --no-tests=error --label-regex G15 2>&1) -join "`n"
    $inventoryExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
Assert-Exact $inventoryExitCode 0 'G15 CTest inventory query exit code'
$inventory = $inventoryText | ConvertFrom-Json
$expectedTests = @(
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Contracts.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.HeaderSelfContainment',
    'ForgeConductor.Infrastructure.ProcessTests',
    'ForgeConductor.Infrastructure.UnitTests')
Assert-Set @($inventory.tests | ForEach-Object name) $expectedTests `
    'G15 deterministic CTest inventory'

$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G15: running the deterministic G15 CTest suite once.'
& $testScript -Configuration Debug -Architecture x64 `
    -Parallel $Parallel -Label G15
Assert-True $? 'single deterministic G15 CTest execution'
Assert-TrackedTreeClean

$binRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64\bin\Debug'
$artifactNames = @(
    'forge-conductor.exe',
    'ForgeConductor.Contracts.ContractTests.exe',
    'ForgeConductor.Contracts.HeaderSelfContainment.exe',
    'ForgeConductor.Infrastructure.HeaderSelfContainment.exe',
    'ForgeConductor.Infrastructure.ProcessTests.exe',
    'ForgeConductor.Infrastructure.UnitTests.exe',
    'ForgeConductor.LMStudio.RealHostTests.exe',
    'ForgeConductor.ProcessFixture.exe')
$artifacts = [Collections.Generic.List[object]]::new()
foreach ($name in $artifactNames) {
    $path = Join-Path $binRoot $name
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G15 artifact exists: $name"
    Assert-X64PortableExecutable $path "G15 artifact $name"
    $item = Get-Item -LiteralPath $path
    $artifacts.Add([ordered]@{
        path = Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $path
        bytes = [long]$item.Length
        sha256 = Get-FileSha256 $path
    })
}

$forgeBinary = Join-Path $binRoot 'forge-conductor.exe'
$realHostBinary = Join-Path $binRoot 'ForgeConductor.LMStudio.RealHostTests.exe'
Write-Host 'G15: launching LM Studio through its supported activation path; no GUI automation is used.'
$runnerStart = [Diagnostics.ProcessStartInfo]::new()
$runnerStart.FileName = $realHostBinary
$runnerStart.UseShellExecute = $false
$runnerStart.CreateNoWindow = $true
$runnerStart.Arguments = '"' + $forgeBinary + '" "' + `
    $RealHostEvidencePath + '" "' + $LMStudioConfigurationPath + '" "' + `
    $hostExecutable + '"'
$runnerProcess = [Diagnostics.Process]::new()
$runnerProcess.StartInfo = $runnerStart
try {
    Assert-True ($runnerProcess.Start()) 'single real-host runner process started'
    $runnerExited = $runnerProcess.WaitForExit($realHostTimeoutMilliseconds)
    if (-not $runnerExited) {
        try { $runnerProcess.Kill() } catch { Stop-Process -Id $runnerProcess.Id -Force }
        $runnerProcess.WaitForExit()
    }
    Assert-True $runnerExited `
        'single real-host runner remained within its 540-second outer deadline'
    Assert-Exact $runnerProcess.ExitCode 0 `
        'single real-host LM Studio qualification exit code'
}
finally {
    $runnerProcess.Dispose()
}
Assert-True (Test-Path -LiteralPath $RealHostEvidencePath -PathType Leaf) `
    'real-host evidence exists'
$realHost = Get-Content -Raw -LiteralPath $RealHostEvidencePath | ConvertFrom-Json
Assert-Exact ([string]$realHost.phase) 'P15' 'real-host evidence phase'
Assert-Exact ([string]$realHost.gate) 'G15' 'real-host evidence gate'
Assert-Exact ([string]$realHost.result.status) 'passed' `
    'real-host qualification status'
Assert-True ([bool]$realHost.host.expected_application_bound) `
    'runtime discovery remained bound to the captured LM Studio executable'
Assert-True ([bool]$realHost.authority.selected_roots_only) `
    'real-host mutation authority remained narrowed to selected roots'
Assert-True ([bool]$realHost.result.deployment_left_installed) `
    'successful real-host deployment remains installed'
Assert-True ([bool]$realHost.host_observations.preflight.inspection_succeeded) `
    'expected LM Studio image was inspectable at preflight'
Assert-Exact ([bool]$realHost.host_observations.preflight.running) $false `
    'expected LM Studio image was stopped at preflight'
Assert-True ([bool]$realHost.host_observations.before_activation.inspection_succeeded) `
    'expected LM Studio image was inspectable before activation'
Assert-Exact ([bool]$realHost.host_observations.before_activation.running) $false `
    'expected LM Studio image remained stopped before supported activation'
Assert-True ([bool]$realHost.host_observations.after_activation_attempt.inspection_succeeded) `
    'expected LM Studio image was inspectable after activation'
Assert-True ([bool]$realHost.host_observations.after_activation_attempt.running) `
    'expected LM Studio image was running after supported activation'
Assert-Exact ([bool]$realHost.activation.running_before_deploy) $false `
    'LM Studio was stopped immediately before supported activation'
Assert-Exact ([bool]$realHost.activation.launched) $true `
    'LM Studio was launched through the supported activation path'
Assert-Exact ([bool]$realHost.activation.restarted) $false `
    'LM Studio was not killed or restarted'
Assert-Exact ([bool]$realHost.activation.configuration_synchronized) $true `
    'supported activation synchronized the exact configuration'
Assert-True ([bool]$realHost.activation.process_inspection_succeeded) `
    'activation observed the expected LM Studio process image'
Assert-True ([bool]$realHost.activation.host_running_after_attempt) `
    'activation left the expected LM Studio process running'
Assert-True ([bool]$realHost.after_deployment.synchronized_state_byte_identical_before_activation) `
    'deployment did not preemptively alter host synchronization state'
Assert-True ([bool]$realHost.after_deployment.foreign_configuration_semantics_preserved) `
    'deployment preserved foreign configuration semantics'
Assert-True ([bool]$realHost.after_deployment.foreign_plugin_tree_bytes_preserved) `
    'deployment preserved foreign plugin-tree bytes'
Assert-True ([bool]$realHost.after_deployment.no_new_transaction_roots) `
    'deployment left no new transaction roots'
Assert-True ([bool]$realHost.after_activation.exact_synchronized_revision) `
    'host synchronized the exact deployment revision'
Assert-True ([bool]$realHost.after_activation.complete_role_objects_equal) `
    'live and synchronized primary/fallback role objects match completely'
Assert-True ([bool]$realHost.after_activation.live_codec_registered) `
    'live configuration passes strict codec registration checks'
Assert-True ([bool]$realHost.after_activation.synchronized_codec_registered) `
    'synchronized configuration passes strict codec registration checks'
Assert-True ([bool]$realHost.after_activation.foreign_configuration_semantics_preserved) `
    'foreign LM Studio configuration semantics remained preserved'
Assert-True ([bool]$realHost.after_activation.foreign_plugin_tree_bytes_preserved) `
    'foreign LM Studio plugin-tree bytes remained preserved'
Assert-True ([bool]$realHost.after_activation.no_new_transaction_roots) `
    'activation left no new transaction roots'
Assert-Exact ([string]$realHost.before.foreign_configuration.sha256) `
    ([string]$realHost.after_activation.foreign_configuration.sha256) `
    'foreign configuration canonical semantic digest'
Assert-Exact ([long]$realHost.before.foreign_configuration.server_count) `
    ([long]$realHost.after_activation.foreign_configuration.server_count) `
    'foreign configuration server count'
Assert-Exact ([string]$realHost.before.foreign_plugins.sha256) `
    ([string]$realHost.after_activation.foreign_plugins.sha256) `
    'foreign plugin-tree byte digest'
Assert-Exact ([long]$realHost.before.foreign_plugins.root_entries) `
    ([long]$realHost.after_activation.foreign_plugins.root_entries) `
    'foreign plugin root-entry count'
Assert-Exact ([long]$realHost.before.foreign_plugins.tree_entries) `
    ([long]$realHost.after_activation.foreign_plugins.tree_entries) `
    'foreign plugin tree-entry count'
Assert-Exact ([long]$realHost.before.foreign_plugins.file_bytes) `
    ([long]$realHost.after_activation.foreign_plugins.file_bytes) `
    'foreign plugin total byte count'
Assert-Exact ([string]$realHost.deployment.deployment_id) `
    ([string]$realHost.activation.deployment_id) `
    'activation acknowledges the deployed revision'
Assert-TrackedTreeClean

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count remains unchanged'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count remains unchanged'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti tree hash remains unchanged'
Invoke-RepositoryIntegrityChecks

$summaryPath = Join-Path $WorkspaceRoot `
    '.forge-codex\state\evidence\P15\g15-validation-summary.json'
$summary = [ordered]@{
    schema_version = 1
    phase = 'P15'
    gate = 'G15'
    status = 'passed'
    recorded_utc = Get-UtcTimestamp
    working_directory = $WorkspaceRoot
    scope = [ordered]@{
        operating_system = 'Windows 11'
        architecture = 'x64'
        configuration = 'Debug'
        machine_local = $true
        clean_environment_deferred = $true
        security_hardening_deferred = $true
        authoritative_build_invocations = 1
        authoritative_test_invocations = 1
        real_host_invocations = 1
        gui_automation_used_against_lm_studio = $false
    }
    assertions = $script:AssertionCount
    deterministic_tests = $expectedTests
    deterministic_test_count = $expectedTests.Count
    artifacts = $artifacts
    lm_studio_host = [ordered]@{
        executable = [string]$baseline.lm_studio.executable
        bytes = [long](Get-Item -LiteralPath $hostExecutable).Length
        sha256 = [string]$baseline.lm_studio.sha256
        discovery_bound_to_baseline = [bool]$realHost.host.expected_application_bound
        launched = [bool]$realHost.activation.launched
        configuration_synchronized = [bool]$realHost.activation.configuration_synchronized
    }
    real_host_evidence = Get-RelativePathPortable `
        -BasePath $WorkspaceRoot -TargetPath $RealHostEvidencePath
    real_host_evidence_sha256 = Get-FileSha256 $RealHostEvidencePath
    acceptance = [string]$gate[0].acceptance
    remaining_limitations = @(
        'This qualifies P15/G15 only on the owner current Windows 11 x64 machine in Debug configuration.',
        'It does not qualify another machine, a clean environment, Release, ARM64, UI parity, CLI parity, packaging, or the complete alpha.',
        'Security-only hardening is deferred until after alpha under OWNER-002.'
    )
}
Write-JsonFileAtomic -Path $summaryPath -Value $summary

Write-Host "G15 LM Studio deployment validation passed ($script:AssertionCount assertions)."
Write-Host "Real-host evidence: $RealHostEvidencePath"
Write-Host "Validation summary: $summaryPath"
