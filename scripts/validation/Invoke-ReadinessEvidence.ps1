#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateRange(1, 64)][int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$evidence = Join-Path $repo "out\validation\readiness-$runId"
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$pwsh = Join-Path $PSHOME 'pwsh.exe'
$steps = @(
    @{ Name = 'release-app'; Script = 'scripts/build.ps1'; Arguments = @('-Configuration', 'Release', '-Architecture', 'x64', '-Product', 'All', '-Parallel', "$Parallel") },
    @{ Name = 'release-tests'; Script = 'scripts/test.ps1'; Arguments = @('-Configuration', 'Release', '-Architecture', 'x64', '-Parallel', "$Parallel") },
    @{ Name = 'static-gates'; Script = 'scripts/Run-Static-Gates.ps1'; Arguments = @() },
    @{ Name = 'package-persistence'; Script = 'scripts/validation/Test-PackagePersistenceContract.ps1'; Arguments = @() }
)
$results = [Collections.Generic.List[object]]::new()
Push-Location $repo
try {
    $commit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
    & git status --porcelain=v1 > (Join-Path $evidence 'source-status.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source status.' }
    & git diff --binary HEAD > (Join-Path $evidence 'source-changes.patch')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source changes.' }
    $sourceHashBefore = (Get-FileHash -LiteralPath (Join-Path $evidence 'source-changes.patch') -Algorithm SHA256).Hash
    foreach ($step in $steps) {
        $started = [DateTime]::UtcNow
        $log = Join-Path $evidence ($step.Name + '.log')
        Write-Host "Running $($step.Name); evidence: $log"
        $exit = -1
        try {
            & $pwsh -NoProfile -NonInteractive -File (Join-Path $repo $step.Script) @($step.Arguments) *> $log
            $exit = $LASTEXITCODE
        } catch {
            $_ | Out-String | Add-Content -LiteralPath $log
        }
        $results.Add([ordered]@{
            name = $step.Name
            script = $step.Script
            arguments = $step.Arguments
            started_utc = $started.ToString('o')
            finished_utc = [DateTime]::UtcNow.ToString('o')
            exit_code = $exit
            passed = ($exit -eq 0)
            log = [IO.Path]::GetFileName($log)
            log_sha256 = (Get-FileHash -LiteralPath $log -Algorithm SHA256).Hash.ToLowerInvariant()
        })
        # Snapshot CTest details before a subsequent invocation replaces them.
        if ($step.Name -eq 'release-tests') {
            $ctestLogs = Join-Path $repo 'out\build\windows-msvc-x64\Testing\Temporary'
            foreach ($name in @('LastTest.log', 'LastTestsFailed.log', 'CTestCostData.txt')) {
                $source = Join-Path $ctestLogs $name
                if (Test-Path -LiteralPath $source -PathType Leaf) {
                    Copy-Item -LiteralPath $source -Destination (Join-Path $evidence $name)
                }
            }
        }
    }
    & git diff --binary HEAD > (Join-Path $evidence 'source-changes-after.patch')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot verify source changes after testing.' }
    $sourceHashAfter = (Get-FileHash -LiteralPath (Join-Path $evidence 'source-changes-after.patch') -Algorithm SHA256).Hash
    $allPassed = $sourceHashBefore -eq $sourceHashAfter
    foreach ($result in $results) { if (-not $result.passed) { $allPassed = $false } }
    $summary = [ordered]@{
        schema = 'forge-readiness-evidence-v1'
        source_commit = $commit
        tracked_diff_sha256 = $sourceHashBefore.ToLowerInvariant()
        tracked_source_stable_during_checks = ($sourceHashBefore -eq $sourceHashAfter)
        generated_utc = [DateTime]::UtcNow.ToString('o')
        host_checks_passed = $allPassed
        shippable = $false
        qualification_note = 'Build and automated checks are evidence only. Untracked inputs are listed in source-status.txt; a clean exact-commit rebuild is required for release.'
        pending_acceptance = @(
            'Installed first-project workflow, including error recovery',
            'Policy repository intake and enforcement behavior',
            'Every interactive control and keyboard/accessibility workflow',
            'Live telemetry comparison against independent host measurements',
            'Real provider/tool/continuity workflows and useful successor work',
            'Clean exact-commit signed package and upgrade/repair/uninstall lifecycle',
            'Owner release acceptance'
        )
        steps = $results.ToArray()
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'summary.json') -Encoding utf8
    Write-Host "Evidence saved to $evidence"
    if (-not $allPassed) { throw 'One or more host checks failed, or tracked inputs changed. See summary.json and logs.' }
} finally {
    Pop-Location
}
