[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$baselineRoot = Join-Path $WorkspaceRoot '.forge-codex\state\baseline'
$inputRoot = Join-Path $WorkspaceRoot '.forge-inputs'
$archiveRoot = Join-Path $inputRoot 'archives'
$sourceRootsPath = Join-Path $inputRoot 'source-roots.json'

$expected = [ordered]@{
    framework = [ordered]@{
        version = '0.2.0'
        archive = 'Forsetti-Framework-Windows-main.zip'
        archive_bytes = 340536L
        archive_sha256 = '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d'
        extracted_root = '.forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main'
        extracted_files = 171
        extracted_bytes = 723455L
        extracted_sha256 = 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28'
    }
    agentic = [ordered]@{
        version = '1.1.0'
        archive = 'forsetti-agentic-edition-main.zip'
        archive_bytes = 789736L
        archive_sha256 = 'e8ef20ad917bd3165335c03beecc674f21f69ff93cbc64fe5edb4e9ffd79692b'
        extracted_root = '.forge-inputs/forsetti-agentic/forsetti-agentic-edition-main'
        extracted_files = 349
        extracted_bytes = 1765772L
        extracted_sha256 = '00c50957591c27111284098de892a038ee18cad58373976ba731d295636a80fb'
    }
    public_api = [ordered]@{
        headers = 34
        bytes = 81656L
        lines = 2491
        aggregate_sha256 = 'e31c38c4569a67560696085c9ac060f6eac4ea067ed95af515355dc79435e4b3'
        named_types = 145
        classes = 83
        structs = 36
        enums = 23
        aliases = 3
    }
    profile_sha256 = '4a65c4c986da951cddd2376de39ebc87f1186b21c147c53c122ce0c10ac19c8a'
    manifest_schema_sha256 = '0d768364790214335ca3b1b585cfd3be2af5e81313b771463f367a61bd0ed90d'
    apache_license_sha256 = '6e3ca1bde7ac8930e70eacf814f07b767e6742268c1bd43f90756a48f6a71c9a'
}

function Assert-Exact {
    param(
        [Parameter(Mandatory)]$Actual,
        [Parameter(Mandatory)]$Expected,
        [Parameter(Mandatory)][string]$Context
    )
    if ($Actual -cne $Expected) {
        throw "P03 generation refused: $Context expected '$Expected', found '$Actual'."
    }
}

function ConvertTo-PortablePath {
    param([Parameter(Mandatory)][string]$Path)
    return $Path.Replace('\', '/')
}

function Resolve-LockedRoot {
    param(
        [Parameter(Mandatory)][string]$ReportedPath,
        [Parameter(Mandatory)][string]$ExpectedRelativePath,
        [Parameter(Mandatory)][string]$Label
    )
    $expectedPath = [System.IO.Path]::GetFullPath((Join-Path $WorkspaceRoot $ExpectedRelativePath.Replace('/', '\')))
    $reportedFull = (Resolve-Path -LiteralPath $ReportedPath).Path
    Assert-Exact $reportedFull $expectedPath "$Label extracted root"
    return $reportedFull
}

function Get-OrdinalRelativePaths {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][System.IO.FileInfo[]]$Files,
        [switch]$IgnoreCase
    )
    $paths = [System.Collections.Generic.List[string]]::new()
    foreach ($file in $Files) {
        $paths.Add((ConvertTo-PortablePath $file.FullName.Substring($Root.Length + 1)))
    }
    $comparer = if ($IgnoreCase) { [System.StringComparer]::OrdinalIgnoreCase } else { [System.StringComparer]::Ordinal }
    $paths.Sort($comparer)
    return @($paths)
}

function Get-TreeSummary {
    param([Parameter(Mandatory)][string]$Root)
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -Force -File)
    $paths = Get-OrdinalRelativePaths -Root $Root -Files $files
    $rows = [System.Collections.Generic.List[string]]::new()
    $totalBytes = 0L
    foreach ($path in $paths) {
        $fullPath = Join-Path $Root $path.Replace('/', '\')
        $file = Get-Item -LiteralPath $fullPath
        $totalBytes += [long]$file.Length
        $rows.Add($path + "`t" + (Get-FileSha256 $fullPath))
    }
    return [ordered]@{
        algorithm = 'sha256(UTF-8(relative/path<TAB>sha256), LF-separated, ordinal path order, no final LF)'
        files = $paths.Count
        bytes = $totalBytes
        sha256 = Get-StringSha256 ($rows -join "`n")
    }
}

function Get-TextLineCountFromBytes {
    param([Parameter(Mandatory)][string]$Path)
    $count = 0
    foreach ($value in [System.IO.File]::ReadAllBytes($Path)) {
        if ($value -eq 10) { $count++ }
    }
    return $count
}

function New-SourceAnchor {
    param(
        [Parameter(Mandatory)][ValidateSet('framework', 'agentic', 'instruction_package')][string]$Origin,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path
    )
    $portable = ConvertTo-PortablePath $Path
    $fullPath = Join-Path $Root $portable.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "P03 source anchor is missing: $Origin/$portable"
    }
    return [ordered]@{
        origin = $Origin
        path = $portable
        bytes = [long](Get-Item -LiteralPath $fullPath).Length
        sha256 = Get-FileSha256 $fullPath
    }
}

function New-DefectRecord {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][object[]]$Anchors
    )
    return [ordered]@{
        id = $Id
        disposition = 'consumer_validator_compensates; upstream sources remain byte-unchanged'
        title = $Title
        evidence = @($Anchors)
    }
}

if (-not (Test-Path -LiteralPath $sourceRootsPath -PathType Leaf)) {
    throw "P03 generation refused: source roots are missing: $sourceRootsPath"
}
$roots = Read-JsonFile $sourceRootsPath
$frameworkRoot = Resolve-LockedRoot -ReportedPath ([string]$roots.forsetti_framework) -ExpectedRelativePath $expected.framework.extracted_root -Label 'Forsetti Framework'
$agenticRoot = Resolve-LockedRoot -ReportedPath ([string]$roots.forsetti_agentic) -ExpectedRelativePath $expected.agentic.extracted_root -Label 'Forsetti Agentic Edition'

$hashManifestPath = Join-Path $archiveRoot 'SOURCE-HASHES.json'
$hashManifest = Read-JsonFile $hashManifestPath
$sourceSpecs = @(
    [ordered]@{ id = 'forsetti-framework-windows'; data = $expected.framework; root = $frameworkRoot },
    [ordered]@{ id = 'forsetti-agentic-edition'; data = $expected.agentic; root = $agenticRoot }
)
$lockedSources = @()
foreach ($spec in $sourceSpecs) {
    $pin = $spec.data
    $archivePath = Join-Path $archiveRoot ([string]$pin.archive)
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw "P03 generation refused: immutable archive missing: $($pin.archive)"
    }
    Assert-Exact ([long](Get-Item -LiteralPath $archivePath).Length) ([long]$pin.archive_bytes) "$($spec.id) archive byte count"
    Assert-Exact (Get-FileSha256 $archivePath) ([string]$pin.archive_sha256) "$($spec.id) archive SHA-256"
    $manifestEntries = @($hashManifest.files | Where-Object { [string]$_.file -ceq [string]$pin.archive })
    Assert-Exact $manifestEntries.Count 1 "$($spec.id) SOURCE-HASHES entry count"
    Assert-Exact ([long]$manifestEntries[0].bytes) ([long]$pin.archive_bytes) "$($spec.id) SOURCE-HASHES bytes"
    Assert-Exact ([string]$manifestEntries[0].sha256) ([string]$pin.archive_sha256) "$($spec.id) SOURCE-HASHES SHA-256"

    $tree = Get-TreeSummary -Root ([string]$spec.root)
    Assert-Exact ([int]$tree.files) ([int]$pin.extracted_files) "$($spec.id) extracted file count"
    Assert-Exact ([long]$tree.bytes) ([long]$pin.extracted_bytes) "$($spec.id) extracted byte count"
    Assert-Exact ([string]$tree.sha256) ([string]$pin.extracted_sha256) "$($spec.id) extracted tree SHA-256"
    $lockedSources += [ordered]@{
        id = $spec.id
        version = [string]$pin.version
        archive = [ordered]@{
            path = '.forge-inputs/archives/' + [string]$pin.archive
            bytes = [long]$pin.archive_bytes
            sha256 = [string]$pin.archive_sha256
        }
        extracted = [ordered]@{
            root = [string]$pin.extracted_root
            files = [int]$tree.files
            bytes = [long]$tree.bytes
            tree_sha256 = [string]$tree.sha256
        }
    }
}

$profileRelative = 'editions/windows/forsetti-windows-0.2.0.profile.json'
$profilePath = Join-Path $agenticRoot $profileRelative.Replace('/', '\')
$profile = Read-JsonFile $profilePath
Assert-Exact (Get-FileSha256 $profilePath) $expected.profile_sha256 'Windows profile SHA-256'
Assert-Exact ([string]$profile.edition) 'windows' 'Windows profile edition'
Assert-Exact ([string]$profile.frameworkVersion) '0.2.0' 'Windows profile framework version'
Assert-Exact ([string]$profile.manifest.currentSchemaVersion) '1.1' 'Windows profile manifest schema'
Assert-Exact ([string]$profile.manifest.currentTemplateVersion) '1.1' 'Windows profile manifest template'

$frameworkVcpkg = Read-JsonFile (Join-Path $frameworkRoot 'vcpkg.json')
Assert-Exact ([string]$frameworkVcpkg.'builtin-baseline') 'cb2981c4e03d421fa03b9bb5044cd1986180e7e4' 'framework vcpkg baseline'
if (Test-Path -LiteralPath (Join-Path $frameworkRoot 'versions\baseline.json') -PathType Leaf) {
    throw 'P03 generation refused: UP-014 must be reevaluated because versions/baseline.json now exists.'
}

$schemaRelative = '.forge-codex/instructions/governance/schemas/module-manifest-1.1.schema.json'
$schemaPath = Join-Path $WorkspaceRoot $schemaRelative.Replace('/', '\')
Assert-Exact (Get-FileSha256 $schemaPath) $expected.manifest_schema_sha256 'canonical consumer manifest schema SHA-256'

$frameworkPolicyPath = Join-Path $frameworkRoot 'framework-policy.json'
$implementationPolicyPath = Join-Path $frameworkRoot 'implementation-policy.json'
$frameworkLicensePath = Join-Path $frameworkRoot 'LICENSE'
$agenticLicensePath = Join-Path $agenticRoot 'LICENSE'
Assert-Exact (Get-FileSha256 $frameworkLicensePath) $expected.apache_license_sha256 'framework Apache license SHA-256'
Assert-Exact (Get-FileSha256 $agenticLicensePath) $expected.apache_license_sha256 'agentic Apache license SHA-256'
$frameworkPolicy = Read-JsonFile $frameworkPolicyPath
Assert-Exact ([string]$frameworkPolicy.license) 'Proprietary' 'framework-policy license metadata'

$instructionRoot = Join-Path $WorkspaceRoot '.forge-codex\instructions'
$agenticValidator = 'core/validator/forsetti_validate.ps1'
$agenticContractRules = 'core/validator/contract_rules.ps1'
$frameworkManifestCheck = 'Scripts/check-manifests.ps1'
$frameworkGuardrail = 'Scripts/verify-forsetti-guardrails.ps1'
$frameworkCMake = 'CMakeLists.txt'
$defects = @(
    New-DefectRecord 'UP-001' 'Windows profile validation is shallow while deep source-contract checks are Apple-only.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
        New-SourceAnchor agentic $agenticRoot $profileRelative
    )
    New-DefectRecord 'UP-002' 'Contract mode does not invoke the separate contract_rules implementation.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
        New-SourceAnchor agentic $agenticRoot $agenticContractRules
    )
    New-DefectRecord 'UP-003' 'Dependency, capability, and module-isolation modes can false-pass without changed-file evidence.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
    )
    New-DefectRecord 'UP-004' 'The Windows dependency scan is incomplete for a consumer application graph.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
        New-SourceAnchor framework $frameworkRoot 'Scripts/check-dependencies.ps1'
    )
    New-DefectRecord 'UP-005' 'The module-isolation regular expression false-positives on ISharedDatabaseService.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
        New-SourceAnchor framework $frameworkRoot 'include/ForsettiCore/ForsettiServices.h'
    )
    New-DefectRecord 'UP-006' 'Windows moduleID and entryPoint patterns are not fully enforced.' @(
        New-SourceAnchor agentic $agenticRoot $agenticValidator
        New-SourceAnchor instruction_package $instructionRoot 'governance/schemas/module-manifest-1.1.schema.json'
    )
    New-DefectRecord 'UP-007' 'The supplied guardrail invocation fixes validation to the framework/FFAE root instead of a complete consumer audit.' @(
        New-SourceAnchor instruction_package $instructionRoot 'scripts/Validate-Forsetti.ps1'
        New-SourceAnchor framework $frameworkRoot $frameworkGuardrail
    )
    New-DefectRecord 'UP-008' 'Framework manifest scanning succeeds when no consumer manifest is discovered.' @(
        New-SourceAnchor framework $frameworkRoot $frameworkManifestCheck
    )
    New-DefectRecord 'UP-009' 'Framework manifest scanning accepts schema 1.0 despite the selected profile requiring 1.1.' @(
        New-SourceAnchor framework $frameworkRoot $frameworkManifestCheck
        New-SourceAnchor agentic $agenticRoot $profileRelative
    )
    New-DefectRecord 'UP-010' 'crypto_utilities capability and I/O-kind contracts disagree; this consumer does not request it.' @(
        New-SourceAnchor framework $frameworkRoot $frameworkManifestCheck
        New-SourceAnchor agentic $agenticRoot $profileRelative
        New-SourceAnchor instruction_package $instructionRoot 'governance/schemas/module-manifest-1.1.schema.json'
    )
    New-DefectRecord 'UP-011' 'The framework guardrail wrapper can build a consumer working directory while auditing framework-root paths.' @(
        New-SourceAnchor framework $frameworkRoot $frameworkGuardrail
        New-SourceAnchor framework $frameworkRoot 'Scripts/check-architecture.ps1'
    )
    New-DefectRecord 'UP-012' 'Framework CMake include paths are non-relocatable and no installed/exported package is supplied.' @(
        New-SourceAnchor framework $frameworkRoot $frameworkCMake
        New-SourceAnchor framework $frameworkRoot 'src/ForsettiCore/CMakeLists.txt'
        New-SourceAnchor framework $frameworkRoot 'src/ForsettiPlatform/CMakeLists.txt'
        New-SourceAnchor framework $frameworkRoot 'src/ForsettiHostTemplate/CMakeLists.txt'
    )
    New-DefectRecord 'UP-013' 'Apache-2.0 LICENSE/README statements conflict with proprietary policy metadata.' @(
        New-SourceAnchor framework $frameworkRoot 'LICENSE'
        New-SourceAnchor framework $frameworkRoot 'README.md'
        New-SourceAnchor framework $frameworkRoot 'framework-policy.json'
        New-SourceAnchor framework $frameworkRoot 'implementation-policy.json'
    )
    New-DefectRecord 'UP-014' 'The pinned vcpkg baseline has no bundled versions/baseline.json; classic-mode preinstall is required for the byte-unchanged standalone build.' @(
        New-SourceAnchor framework $frameworkRoot 'vcpkg.json'
    )
)

$sourceLock = [ordered]@{
    schema_version = 1
    lock_id = 'P03-FORSETTI-SOURCE-LOCK'
    generated_from = 'immutable attached archives; no network or mutable branch input'
    hashing = [ordered]@{
        file = 'SHA-256 of exact file bytes'
        tree = 'SHA-256 of UTF-8 relative/path<TAB>file-sha256 records, LF-separated in ordinal path order, without a final LF'
    }
    source_hash_manifest = [ordered]@{
        path = '.forge-inputs/archives/SOURCE-HASHES.json'
        sha256 = Get-FileSha256 $hashManifestPath
    }
    sources = $lockedSources
    governance_pins = [ordered]@{
        windows_profile = New-SourceAnchor agentic $agenticRoot $profileRelative
        framework_policy = New-SourceAnchor framework $frameworkRoot 'framework-policy.json'
        implementation_policy = New-SourceAnchor framework $frameworkRoot 'implementation-policy.json'
        manifest_schema = [ordered]@{ origin = 'instruction_package'; path = $schemaRelative; bytes = [long](Get-Item $schemaPath).Length; sha256 = Get-FileSha256 $schemaPath }
        task_contract = [ordered]@{ origin = 'instruction_package'; path = '.forge-codex/instructions/governance/PORT_TASK_CONTRACT.json'; sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot '.forge-codex\instructions\governance\PORT_TASK_CONTRACT.json') }
        project_context = [ordered]@{ origin = 'instruction_package'; path = '.forge-codex/instructions/governance/forsetti-project-context.json'; sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot '.forge-codex\instructions\governance\forsetti-project-context.json') }
    }
    license_evidence = [ordered]@{
        status = 'conflict_recorded; redistribution requires the accepted P03 licensing decision'
        apache_license_sha256 = $expected.apache_license_sha256
        framework_policy_declares = 'Proprietary'
        framework_license = New-SourceAnchor framework $frameworkRoot 'LICENSE'
        framework_readme = New-SourceAnchor framework $frameworkRoot 'README.md'
        implementation_policy = New-SourceAnchor framework $frameworkRoot 'implementation-policy.json'
    }
    upstream_validation = [ordered]@{
        status = 'known_upstream_failure'
        upstream_suite_passed = $false
        approved_build_workaround = 'Keep the attached Forsetti source byte-unchanged, preinstall nlohmann-json with vcpkg classic mode, then build the framework standalone.'
        vcpkg_baseline = 'cb2981c4e03d421fa03b9bb5044cd1986180e7e4'
        commands = @(
            [ordered]@{ evidence_id = '20260825T093342537Z-cf79732f'; operation = 'initial configure'; expected_exit_code = 1; result = 'failed: missing versions/baseline.json for pinned manifest baseline' },
            [ordered]@{ evidence_id = '20260825T093451916Z-b6fcf5ca'; operation = 'classic-mode dependency preinstall'; expected_exit_code = 0; result = 'passed' },
            [ordered]@{ evidence_id = '20260825T093633245Z-35b54512'; operation = 'fresh configure after classic-mode preinstall'; expected_exit_code = 0; result = 'passed' },
            [ordered]@{ evidence_id = '20260825T093648999Z-82d9497b'; operation = 'Debug library build'; expected_exit_code = 0; result = 'passed' },
            [ordered]@{ evidence_id = '20260825T093719995Z-2f4e92bd'; operation = 'Debug CTest'; expected_exit_code = 1; result = 'failed: four deterministic ForsettiCoreTests cases' },
            [ordered]@{ evidence_id = '20260825T093810695Z-2db748d9'; operation = 'Release library build'; expected_exit_code = 0; result = 'passed' }
        )
        passing_surfaces = @(
            'Debug library build',
            'Release library build',
            'ForsettiPlatformTests',
            'ForsettiArchitectureTests'
        )
        failing_test_executable = 'ForsettiCoreTests'
        deterministic_failures = @(
            [ordered]@{ name = 'ScopedServices_StorageAllowedWithCapability'; detail = 'unhandled bad_any_cast' },
            [ordered]@{ name = 'ScopedServices_StorageRejectsTraversalKeys'; detail = 'unhandled bad_any_cast' },
            [ordered]@{ name = 'Runtime_V11UIContributionIDsMustBeDeclared'; detail = 'unhandled ModuleRegistry::makeModule exception through ModuleManager' },
            [ordered]@{ name = 'Runtime_V11DeclaredThemeMaskIsPreserved'; detail = 'unhandled ModuleRegistry::makeModule exception through ModuleManager' }
        )
        consumer_gate_policy = 'G03 may pass only through the custom fail-closed consumer validator; upstream suite success must not be claimed.'
    }
    upstream_defects = $defects
}

$includeRoot = Join-Path $frameworkRoot 'include'
$headerFiles = @(Get-ChildItem -LiteralPath $includeRoot -Recurse -Force -File -Filter '*.h')
$headerPaths = Get-OrdinalRelativePaths -Root $includeRoot -Files $headerFiles -IgnoreCase
$headers = @()
$aggregateRows = [System.Collections.Generic.List[string]]::new()
$headerBytes = 0L
$headerLines = 0
foreach ($path in $headerPaths) {
    $fullPath = Join-Path $includeRoot $path.Replace('/', '\')
    $file = Get-Item -LiteralPath $fullPath
    $sha256 = Get-FileSha256 $fullPath
    $lines = Get-TextLineCountFromBytes $fullPath
    $product = ($path -split '/', 2)[0]
    $headers += [ordered]@{
        path = $path
        product = $product
        bytes = [long]$file.Length
        lines = [int]$lines
        sha256 = $sha256
    }
    $aggregateRows.Add($path + "`t" + $sha256)
    $headerBytes += [long]$file.Length
    $headerLines += [int]$lines
}
$publicAggregate = Get-StringSha256 ($aggregateRows -join "`n")
Assert-Exact $headers.Count ([int]$expected.public_api.headers) 'public header count'
Assert-Exact $headerBytes ([long]$expected.public_api.bytes) 'public header byte count'
Assert-Exact $headerLines ([int]$expected.public_api.lines) 'public header line count'
Assert-Exact $publicAggregate ([string]$expected.public_api.aggregate_sha256) 'public header aggregate SHA-256'

# Inventory every named public type, retaining every declaration anchor while
# consolidating forward declarations by kind/name. RegistryKey is the one named
# declaration in these public headers that is not itself public API: it is a
# private implementation alias nested inside ForsettiViewFactoryRegistry.
$namedTypeMap = [ordered]@{}
$rawNamedTypeDeclarationSites = 0
$typePatterns = [ordered]@{
    class = '^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    struct = '^\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    enum = '^\s*enum(?:\s+class)?\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    alias = '^\s*using\s+([A-Za-z_][A-Za-z0-9_]*)\s*='
}
foreach ($header in $headers) {
    $fullPath = Join-Path $includeRoot ([string]$header.path).Replace('/', '\')
    $lines = [System.IO.File]::ReadAllLines($fullPath)
    for ($index = 0; $index -lt $lines.Count; $index++) {
        foreach ($typePattern in $typePatterns.GetEnumerator()) {
            $typeMatch = [regex]::Match([string]$lines[$index], [string]$typePattern.Value)
            if (-not $typeMatch.Success) { continue }
            $rawNamedTypeDeclarationSites++
            $kind = [string]$typePattern.Key
            $name = [string]$typeMatch.Groups[1].Value
            $key = $kind + "`t" + $name
            if (-not $namedTypeMap.Contains($key)) {
                $namedTypeMap[$key] = [ordered]@{
                    kind = $kind
                    name = $name
                    declarations = [System.Collections.Generic.List[object]]::new()
                }
            }
            $namedTypeMap[$key].declarations.Add([ordered]@{
                path = [string]$header.path
                line = $index + 1
            })
            break
        }
    }
}
$allNamedTypes = @($namedTypeMap.Values)
Assert-Exact $rawNamedTypeDeclarationSites 149 'raw named-type declaration-site count'
Assert-Exact $allNamedTypes.Count 146 'unique named-type count before visibility filtering'
$privateTypeExclusions = @($allNamedTypes | Where-Object {
    [string]$_.kind -ceq 'alias' -and [string]$_.name -ceq 'RegistryKey'
})
Assert-Exact $privateTypeExclusions.Count 1 'private named-type exclusion count'
$privateRegistryKey = $privateTypeExclusions[0]
Assert-Exact @($privateRegistryKey.declarations).Count 1 'RegistryKey declaration count'
Assert-Exact ([string]$privateRegistryKey.declarations[0].path) 'ForsettiPlatform/WindowsViewFactoryRegistry.h' 'RegistryKey declaration path'
Assert-Exact ([int]$privateRegistryKey.declarations[0].line) 89 'RegistryKey declaration line'
$publicNamedTypes = @($allNamedTypes | Where-Object {
    -not ([string]$_.kind -ceq 'alias' -and [string]$_.name -ceq 'RegistryKey')
})
$namedTypeCounts = [ordered]@{
    total = $publicNamedTypes.Count
    classes = @($publicNamedTypes | Where-Object { [string]$_.kind -ceq 'class' }).Count
    structs = @($publicNamedTypes | Where-Object { [string]$_.kind -ceq 'struct' }).Count
    enums = @($publicNamedTypes | Where-Object { [string]$_.kind -ceq 'enum' }).Count
    aliases = @($publicNamedTypes | Where-Object { [string]$_.kind -ceq 'alias' }).Count
}
Assert-Exact ([int]$namedTypeCounts.total) ([int]$expected.public_api.named_types) 'public named-type count'
Assert-Exact ([int]$namedTypeCounts.classes) ([int]$expected.public_api.classes) 'public class count'
Assert-Exact ([int]$namedTypeCounts.structs) ([int]$expected.public_api.structs) 'public struct count'
Assert-Exact ([int]$namedTypeCounts.enums) ([int]$expected.public_api.enums) 'public enum count'
Assert-Exact ([int]$namedTypeCounts.aliases) ([int]$expected.public_api.aliases) 'public alias count'

$policyHash = Get-FileSha256 $frameworkPolicyPath
$symbols = @()
foreach ($kind in @('interfaces', 'valueTypes', 'enums')) {
    foreach ($nameValue in @($frameworkPolicy.corePublicAPI.$kind)) {
        $name = [string]$nameValue
        $pattern = '^\s*(?:(?:class|struct|enum\s+class|enum)\s+|using\s+)' + [regex]::Escape($name) + '(?:\s|=|:|\{|;)'
        $declarations = @()
        foreach ($header in $headers) {
            $fullPath = Join-Path $includeRoot ([string]$header.path).Replace('/', '\')
            $lines = @(Get-Content -LiteralPath $fullPath)
            for ($index = 0; $index -lt $lines.Count; $index++) {
                if ([string]$lines[$index] -cmatch $pattern) {
                    $declarations += [ordered]@{ path = [string]$header.path; line = $index + 1 }
                }
            }
        }
        if ($declarations.Count -lt 1) {
            throw "P03 generation refused: framework-policy public $kind symbol '$name' has no public-header declaration."
        }
        $symbols += [ordered]@{
            kind = $kind
            name = $name
            declaration = $declarations[0]
        }
    }
}

$products = @()
foreach ($productNameValue in @($profile.publicProducts)) {
    $productName = [string]$productNameValue
    $productHeaders = @($headers | Where-Object { [string]$_.product -ceq $productName })
    if ($productHeaders.Count -lt 1) {
        throw "P03 generation refused: selected profile public product '$productName' has no public headers."
    }
    $productBytes = 0L
    $productLines = 0
    foreach ($header in $productHeaders) {
        $productBytes += [long]$header.bytes
        $productLines += [int]$header.lines
    }
    $products += [ordered]@{
        name = $productName
        headers = $productHeaders.Count
        bytes = $productBytes
        lines = $productLines
    }
}

$publicApi = [ordered]@{
    schema_version = 1
    inventory_id = 'P03-FORSETTI-WINDOWS-PUBLIC-API'
    framework_version = '0.2.0'
    source_archive_sha256 = [string]$expected.framework.archive_sha256
    source_root = [string]$expected.framework.extracted_root
    include_root = [string]$expected.framework.extracted_root + '/include'
    aggregate = [ordered]@{
        algorithm = 'sha256(UTF-8(header/path<TAB>file-sha256), LF-separated, ordinal-ignore-case path order, no final LF)'
        headers = $headers.Count
        bytes = $headerBytes
        lines = $headerLines
        sha256 = $publicAggregate
    }
    products = $products
    policy = [ordered]@{
        path = [string]$expected.framework.extracted_root + '/framework-policy.json'
        sha256 = $policyHash
        interface_count = @($frameworkPolicy.corePublicAPI.interfaces).Count
        value_type_count = @($frameworkPolicy.corePublicAPI.valueTypes).Count
        enum_count = @($frameworkPolicy.corePublicAPI.enums).Count
    }
    consumption_boundary = [ordered]@{
        allowed_include_prefixes = @('ForsettiCore/', 'ForsettiPlatform/', 'ForsettiHostTemplate/')
        app_module_allowed_include_prefixes = @('ForsettiCore/')
        sealed_products = @('ForsettiCore', 'ForsettiPlatform', 'ForsettiHostTemplate')
        framework_sources_may_be_modified = $false
    }
    headers = $headers
    named_type_inventory = [ordered]@{
        scope = 'All named class, struct, enum, and namespace-level alias declarations in the exact public-header set; duplicate forward/definition sites are consolidated by kind and name.'
        raw_declaration_sites = $rawNamedTypeDeclarationSites
        unique_before_visibility_filter = $allNamedTypes.Count
        counts = $namedTypeCounts
        exclusions = @(
            [ordered]@{
                kind = 'alias'
                name = 'RegistryKey'
                reason = 'private nested implementation alias; not a public named type'
                declarations = @($privateRegistryKey.declarations)
            }
        )
        types = $publicNamedTypes
    }
    policy_symbols = $symbols
}

New-Item -ItemType Directory -Force -Path $baselineRoot | Out-Null
Write-JsonFileAtomic -Path (Join-Path $baselineRoot 'p03-forsetti-source-lock.json') -Value $sourceLock
Write-JsonFileAtomic -Path (Join-Path $baselineRoot 'p03-forsetti-public-api.json') -Value $publicApi

Write-Host "P03 baseline generated: 2 immutable sources, $($headers.Count) public headers, $($publicNamedTypes.Count) named public types, $($symbols.Count) policy symbols, and $($defects.Count) upstream defect records."
