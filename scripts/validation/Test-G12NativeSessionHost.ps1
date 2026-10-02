[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WorkspaceRoot,

    [ValidateRange(1, 256)]
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),

    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "G12 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param($Actual, $Expected, [string]$Message)
    Assert-True ($Actual -ceq $Expected) `
        "$Message (expected '$Expected', found '$Actual')"
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

function Invoke-RepositoryIntegrityChecks {
    $output = @(& git -c core.safecrlf=false -C $WorkspaceRoot diff --check 2>&1)
    Assert-Exact $LASTEXITCODE 0 `
        ('git diff --check: ' + ($output -join [Environment]::NewLine))
    & (Join-Path $WorkspaceRoot `
        '.forge-codex\instructions\scripts\Verify-Ledger.ps1') `
        -WorkspaceRoot $WorkspaceRoot
    Assert-True $? 'governance ledger verification'
}

$frameworkRoot = Join-Path $WorkspaceRoot `
    '.forge-inputs\forsetti-framework\Forsetti-Framework-Windows-main'
$frameworkBefore = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkBefore.files) 171 'sealed Forsetti file count'
Assert-Exact ([long]$frameworkBefore.bytes) 723455L 'sealed Forsetti byte count'
Assert-Exact ([string]$frameworkBefore.sha256) `
    'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' `
    'sealed Forsetti tree hash'

$retained = Join-Path $WorkspaceRoot 'scripts\validation\Test-G11Continuity.ps1'
Assert-True (Test-Path -LiteralPath $retained -PathType Leaf) `
    'retained G11 validator exists'
Write-Host 'G12: running retained G11 static validation without a retained rebuild.'
& $retained -WorkspaceRoot $WorkspaceRoot -Parallel $Parallel -StaticOnly
Assert-True $? 'retained G11 static validation'

& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoPython.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-Python validation'
& (Join-Path $WorkspaceRoot `
    '.forge-codex\instructions\scripts\Validate-NoAttribution.ps1') `
    -WorkspaceRoot $WorkspaceRoot
Assert-True $? 'repository no-attribution validation'

$p12Files = @(
    'CMakeLists.txt',
    'include/ForgeConductor/Application/ContinuityAutomation.h',
    'include/ForgeConductor/Contracts/INativeSessionHostServices.h',
    'include/ForgeConductor/Domain/ContinuityAutomationModels.h',
    'include/ForgeConductor/Domain/ContinuityModels.h',
    'include/ForgeConductor/Domain/SessionHostModels.h',
    'include/ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h',
    'include/ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h',
    'include/ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h',
    'include/ForgeConductor/SessionHost/LocalLogicalSessionTransport.h',
    'include/ForgeConductor/SessionHost/PluginAbi.h',
    'include/ForgeConductor/SessionHost/SessionHost.h',
    'src/Application/ContinuityAutomation.cpp',
    'src/Domain/ContinuityModels.cpp',
    'src/Hosts/SessionHost/SessionHostCompositionRoot.cpp',
    'src/Hosts/SessionHost/SessionHostCompositionRoot.h',
    'src/Hosts/SessionHost/main.cpp',
    'src/Infrastructure/Windows/Detail/WindowsPathResolver.cpp',
    'src/Infrastructure/Windows/WinHttpLocalModelSessionTransport.cpp',
    'src/Infrastructure/Windows/WindowsNativeSessionLedger.cpp',
    'src/SessionHost/BoundedLogicalContinuationQueue.cpp',
    'src/SessionHost/ForgeNativeSessionHostAdapter.cpp',
    'src/SessionHost/LocalLogicalSessionTransport.cpp',
    'src/SessionHost/Plugin/ForgeNativeSessionHostPlugin.cpp',
    'tests/Continuity/ContinuityAutomationTests.cpp',
    'tests/SessionHost/NativeSessionContinuityEndToEndTests.cpp',
    'tests/SessionHost/NativeSessionHostAdapterTests.cpp',
    'tests/SessionHost/NativeSessionHostPluginSmokeTests.cpp',
    'tests/SessionHost/WinHttpLocalModelSessionTransportTests.cpp',
    'tests/SessionHost/WindowsNativeSessionLedgerTests.cpp',
    '.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md',
    '.forge-codex/state/decisions/P12-001-native-logical-session-host-and-production-wiring.md',
    '.forge-codex/state/decisions/P12-002-native-session-ledger-privacy-and-recovery.md',
    '.forge-codex/state/decisions/P12-003-g12-regression-and-alpha-qualification.md',
    '.forge-codex/state/decisions/P12-004-packaged-local-appdata-path-identity.md',
    'scripts/validation/Test-G12NativeSessionHost.ps1')
foreach ($relative in $p12Files) {
    $path = Join-Path $WorkspaceRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "P12 file exists: $relative"
    Assert-CrlfTextFile $path "P12 text file $relative"
}

$portablePaths = @($p12Files | Where-Object {
    $_ -match '^(?:include/ForgeConductor/(?:Domain|Contracts|Application)|src/(?:Domain|Application))/'
})
$portableText = ($portablePaths | ForEach-Object {
    Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot $_.Replace('/', '\'))
}) -join "`n"
Assert-NoMatch $portableText `
    '(?im)^\s*#\s*include\s*[<"][^>"]*(?:windows[.]h|winrt[/\\]|winsqlite|sqlite3|nlohmann|filesystem)' `
    'Domain, Contracts, and Application remain platform neutral'
Assert-NoMatch $portableText `
    '\b(?:HANDLE|HWND|HRESULT|sqlite3|nlohmann::|std::filesystem)\b' `
    'portable native-session layers expose no platform implementation types'

$automation = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Application\ContinuityAutomation.cpp')
Assert-Match $automation 'MaximumTrackedProjects\s*=\s*128U' `
    'continuity automation project bound' -CaseSensitive
Assert-Match $automation 'completedProgressUnits' `
    'progress trigger is derived in the automation service' -CaseSensitive
Assert-Match $automation 'checkpointIntervalSeconds' `
    'time trigger is derived in the automation service' -CaseSensitive
Assert-Match $automation `
    'coordinator_[.]checkpoint[\s\S]*?coordinator_[.]requestRollover[\s\S]*?coordinator_[.]resume' `
    'one automatic observation drives checkpoint, rollover, and resume' -CaseSensitive

$contracts = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'include\ForgeConductor\Contracts\INativeSessionHostServices.h')
Assert-Match $contracts 'class\s+INativeSessionLedger' `
    'native session ledger contract' -CaseSensitive
Assert-Match $contracts 'class\s+INativeSessionTransport' `
    'native transport contract' -CaseSensitive
Assert-Match $contracts 'class\s+INativeLogicalContinuationScheduler' `
    'bounded continuation scheduler contract' -CaseSensitive

$adapter = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\SessionHost\ForgeNativeSessionHostAdapter.cpp')
Assert-Match $adapter 'MaximumPendingOperations\s*=\s*128U' `
    'adapter serialized-operation bound' -CaseSensitive
Assert-Match $adapter 'cancelledOperations[.]size\(\)\s*>=\s*256U' `
    'adapter cancellation bound' -CaseSensitive
Assert-Match $adapter `
    'transport[.]createSession[\s\S]*?commitOwned\(context\)' `
    'provider create effect is durably published' -CaseSensitive
Assert-Match $adapter `
    'transport[.]bootstrap[\s\S]*?commitOwned\(context\)' `
    'provider bootstrap effect is durably published' -CaseSensitive

$localTransport = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\SessionHost\LocalLogicalSessionTransport.cpp')
Assert-Match $localTransport 'codec->decode' `
    'logical transport uses strict canonical continuity decoding' -CaseSensitive
Assert-Match $localTransport 'successorSessionId[\s\S]*?handoffSha256[\s\S]*?exactMatch' `
    'logical replay checks successor, digest, and continuation' -CaseSensitive
Assert-Match $localTransport 'activeOperations\s*==\s*0U[\s\S]*?scheduler->shutdown' `
    'transport shutdown drains operations before scheduler shutdown' -CaseSensitive

$queue = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\SessionHost\BoundedLogicalContinuationQueue.cpp')
Assert-Match $queue 'MaximumContinuationFieldBytes\s*=\s*4U\s*[*]\s*1024U' `
    'continuation field byte bound' -CaseSensitive
Assert-Match $queue 'bindings[.]erase\(binding\)' `
    'queue enqueue rollback is exception-atomic' -CaseSensitive
Assert-Match $queue 'accepted->second[\s\S]*?pending[.]pop_front' `
    'queue materializes continuation before dequeue commit' -CaseSensitive

$ledger = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot `
    'src\Infrastructure\Windows\WindowsNativeSessionLedger.cpp')
Assert-Match $ledger 'MaximumNativeSessionRecords' `
    'durable native ledger is record bounded' -CaseSensitive
Assert-Match $ledger 'content_sha256' `
    'durable native ledger is checksum bound' -CaseSensitive
Assert-NoMatch $ledger `
    'canonicalHandoffUtf8|next_actions|success_condition' `
    'native ledger retains no canonical handoff or continuation text' -CaseSensitive

$cmake = Get-Content -Raw -LiteralPath (Join-Path $WorkspaceRoot 'CMakeLists.txt')
$g12Tests = @(
    'ForgeConductor.Continuity.AutomationTests',
    'ForgeConductor.SessionHost.PluginSmokeTests',
    'ForgeConductor.SessionHost.AdapterTests',
    'ForgeConductor.SessionHost.WinHttpTransportTests',
    'ForgeConductor.SessionHost.LedgerTests',
    'ForgeConductor.SessionHost.ContinuityEndToEndTests')
foreach ($test in $g12Tests) {
    Assert-Match $cmake `
        ('add_test\s*\(\s*NAME\s+' + [regex]::Escape($test)) `
        "CMake test registration $test" -CaseSensitive
    Assert-Match $cmake `
        ([regex]::Escape($test) + '[\s\S]*?LABELS\s+"[^"]*G12[^"]*"') `
        "CMake G12 label $test" -CaseSensitive
}
foreach ($command in @('manifest','health','recover','cancel-orphans','self-test')) {
    Assert-Match $cmake ('\b' + [regex]::Escape($command) + '\b') `
        "session-host command registration $command" -CaseSensitive
}
Assert-Match $cmake `
    'ForgeConductor[.]SessionHost[.]Command[.][$][{]session_host_command[}][\s\S]*?LABELS\s+"T-INTEGRATION;T-CONT;G12"' `
    'all session-host commands carry the G12 label' -CaseSensitive

if ($StaticOnly) {
    $frameworkAfter = Get-TreeSummary $frameworkRoot
    Assert-Exact ([string]$frameworkAfter.sha256) `
        ([string]$frameworkBefore.sha256) 'sealed Forsetti unchanged after static G12'
    Invoke-RepositoryIntegrityChecks
    Write-Host "G12 static native-session validation passed: $script:AssertionCount assertions."
    return
}

$buildTargets = @(
    'ForgeConductor.Domain.UnitTests',
    'ForgeConductor.Contracts.ContractTests',
    'ForgeConductor.Persistence.UnitTests',
    'ForgeConductor.ProjectMemory.CacheTests',
    'ForgeConductor.Continuity.DocumentCodecTests',
    'ForgeConductor.Continuity.LegacyApplicationTests',
    'ForgeConductor.Continuity.CoordinatorTests',
    'ForgeConductor.Continuity.ProcessFixtureTests',
    'ForgeConductor.Continuity.RepositoryWindowsTests',
    'ForgeConductor.LegacyContinuityPersistence.WindowsTests',
    'ForgeConductor.Continuity.AutomationTests',
    'ForgeConductor.SessionHost.PluginSmokeTests',
    'ForgeConductor.SessionHost.AdapterTests',
    'ForgeConductor.SessionHost.WinHttpTransportTests',
    'ForgeConductor.SessionHost.LedgerTests',
    'ForgeConductor.SessionHost.ContinuityEndToEndTests',
    'ForgeConductor.SessionHost',
    'ForgeNativeSessionHostPlugin')
$buildScript = Join-Path $WorkspaceRoot 'scripts\build.ps1'
$testScript = Join-Path $WorkspaceRoot 'scripts\test.ps1'
Write-Host 'G12: performing the one authoritative fresh x64 Debug target build.'
& $buildScript -Configuration Debug -Architecture x64 -Target $buildTargets `
    -Parallel $Parallel -Fresh
Assert-True $? 'one authoritative fresh x64 Debug G11/G12 target build'
Write-Host 'G12: running the one authoritative x64 Debug retained G11 plus G12 CTest pass.'
& $testScript -Configuration Debug -Architecture x64 -Parallel $Parallel `
    -Label 'G11|G12'
Assert-True $? 'one authoritative x64 Debug retained G11 plus G12 CTest pass'

$buildRoot = Join-Path $WorkspaceRoot 'out\build\windows-msvc-x64'
$artifacts = @(
    'lib/Debug/ForgeConductor.Domain.lib',
    'lib/Debug/ForgeConductor.Application.lib',
    'lib/Debug/ForgeConductor.Infrastructure.Windows.lib',
    'lib/Debug/ForgeConductor.Persistence.Windows.lib',
    'lib/Debug/ForgeConductor.SessionHost.Core.lib',
    'bin/Debug/ForgeConductor.Domain.UnitTests.exe',
    'bin/Debug/ForgeConductor.Contracts.ContractTests.exe',
    'bin/Debug/ForgeConductor.Persistence.ProcessFixture.exe',
    'bin/Debug/ForgeConductor.Persistence.UnitTests.exe',
    'bin/Debug/ForgeConductor.ProjectMemory.CacheTests.exe',
    'bin/Debug/ForgeConductor.Continuity.DocumentCodecTests.exe',
    'bin/Debug/ForgeConductor.Continuity.LegacyApplicationTests.exe',
    'bin/Debug/ForgeConductor.Continuity.CoordinatorTests.exe',
    'bin/Debug/ForgeConductor.Continuity.ProcessFixtureTests.exe',
    'bin/Debug/ForgeConductor.Continuity.RepositoryWindowsTests.exe',
    'bin/Debug/ForgeConductor.LegacyContinuityPersistence.WindowsTests.exe',
    'bin/Debug/ForgeConductor.Continuity.AutomationTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.PluginSmokeTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.AdapterTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.WinHttpTransportTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.LedgerTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.ContinuityEndToEndTests.exe',
    'bin/Debug/ForgeConductor.SessionHost.exe',
    'bin/Debug/ForgeNativeSessionHostPlugin.dll')
$artifactHashes = [ordered]@{}
foreach ($relative in $artifacts) {
    $path = Join-Path $buildRoot $relative.Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
        "G12 artifact exists: $relative"
    Assert-True ([long](Get-Item -LiteralPath $path).Length -gt 0) `
        "G12 artifact nonempty: $relative"
    $hash = Get-FileSha256 $path
    Assert-Match $hash '^[0-9a-f]{64}$' "G12 artifact hash: $relative" -CaseSensitive
    $artifactHashes[$relative] = $hash
    Write-Host "$relative SHA-256: $hash"
    if ($relative.EndsWith('.exe', [StringComparison]::Ordinal) -or
        $relative.EndsWith('.dll', [StringComparison]::Ordinal)) {
        Assert-X64PortableExecutable $path $relative
    }
}

$frameworkAfter = Get-TreeSummary $frameworkRoot
Assert-Exact ([int]$frameworkAfter.files) ([int]$frameworkBefore.files) `
    'sealed Forsetti file count after full G12'
Assert-Exact ([long]$frameworkAfter.bytes) ([long]$frameworkBefore.bytes) `
    'sealed Forsetti byte count after full G12'
Assert-Exact ([string]$frameworkAfter.sha256) ([string]$frameworkBefore.sha256) `
    'sealed Forsetti hash after full G12'
Invoke-RepositoryIntegrityChecks
Write-Host "G12 native-session validation passed: $script:AssertionCount assertions; 10 retained G11 plus 11 G12 CTest registrations, $($artifactHashes.Count) artifact hashes, retained G11 static validation, and the single fresh x64 Debug build/test invocation succeeded."
