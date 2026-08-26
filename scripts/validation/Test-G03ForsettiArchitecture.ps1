[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$script:AssertionCount = 0
$baselineRoot = Join-Path $WorkspaceRoot '.forge-codex\state\baseline'
$instructionRoot = Join-Path $WorkspaceRoot '.forge-codex\instructions'
$decisionRoot = Join-Path $WorkspaceRoot '.forge-codex\state\decisions'
$frameworkRelative = '.forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main'
$agenticRelative = '.forge-inputs/forsetti-agentic/forsetti-agentic-edition-main'
$frameworkRoot = Join-Path $WorkspaceRoot $frameworkRelative.Replace('/', '\')
$agenticRoot = Join-Path $WorkspaceRoot $agenticRelative.Replace('/', '\')

$expectedCapabilities = @(
    'networking',
    'storage',
    'secure_storage',
    'file_export',
    'telemetry',
    'routing_overlay',
    'toolbar_items',
    'view_injection',
    'event_publishing',
    'shared_database',
    'diagnostics',
    'api',
    'security'
)
$expectedStores = @(
    'forge-conductor.central',
    'forge-conductor.project',
    'forge-conductor.configuration',
    'forge-conductor.manager-state',
    'forge-conductor.session-ledger'
)
$expectedIo = @(
    [ordered]@{ requirementID = 'forge.network.client'; kind = 'networking'; access = 'read_write' },
    [ordered]@{ requirementID = 'forge.settings.storage'; kind = 'storage'; access = 'read_write' },
    [ordered]@{ requirementID = 'forge.secrets.secure-storage'; kind = 'secure_storage'; access = 'read_write' },
    [ordered]@{ requirementID = 'forge.data.export'; kind = 'file_export'; access = 'write' },
    [ordered]@{ requirementID = 'forge.telemetry.events'; kind = 'telemetry'; access = 'emit' },
    [ordered]@{ requirementID = 'forge.database.access'; kind = 'shared_database'; access = 'read_write' },
    [ordered]@{ requirementID = 'forge.diagnostics.events'; kind = 'diagnostics'; access = 'emit' },
    [ordered]@{ requirementID = 'forge.api.invoke'; kind = 'api'; access = 'execute' },
    [ordered]@{ requirementID = 'forge.security.authorization'; kind = 'security'; access = 'execute' }
)

function Assert-True {
    param(
        [Parameter(Mandatory)][bool]$Condition,
        [Parameter(Mandatory)][string]$Message
    )
    if (-not $Condition) { throw "G03 assertion failed: $Message" }
    $script:AssertionCount++
}

function Assert-Exact {
    param(
        [Parameter(Mandatory)][AllowNull()]$Actual,
        [Parameter(Mandatory)][AllowNull()]$Expected,
        [Parameter(Mandatory)][string]$Message
    )
    Assert-True ($Actual -ceq $Expected) "$Message (expected '$Expected', found '$Actual')"
}

function Get-OrdinalSortedStrings {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Values, [switch]$IgnoreCase)
    $list = [System.Collections.Generic.List[string]]::new()
    foreach ($value in $Values) { $list.Add([string]$value) }
    $comparer = if ($IgnoreCase) { [System.StringComparer]::OrdinalIgnoreCase } else { [System.StringComparer]::Ordinal }
    $list.Sort($comparer)
    return @($list)
}

function Assert-Sequence {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Actual,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Expected,
        [Parameter(Mandatory)][string]$Message
    )
    Assert-Exact $Actual.Count $Expected.Count "$Message count"
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        Assert-Exact ([string]$Actual[$index]) ([string]$Expected[$index]) "$Message item $index"
    }
}

function Assert-Set {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Actual,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Expected,
        [Parameter(Mandatory)][string]$Message
    )
    $actualSorted = Get-OrdinalSortedStrings @($Actual | ForEach-Object { [string]$_ })
    $expectedSorted = Get-OrdinalSortedStrings @($Expected | ForEach-Object { [string]$_ })
    Assert-Sequence $actualSorted $expectedSorted $Message
}

function Assert-Unique {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Values,
        [Parameter(Mandatory)][string]$Message
    )
    Assert-Exact @($Values | Group-Object | Where-Object Count -ne 1).Count 0 $Message
}

function Assert-Properties {
    param(
        [Parameter(Mandatory)]$Object,
        [Parameter(Mandatory)][string[]]$Expected,
        [Parameter(Mandatory)][string]$Message
    )
    $actual = @($Object.PSObject.Properties.Name)
    Assert-Set $actual $Expected "$Message properties"
}

function Assert-ContainsPattern {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Pattern,
        [Parameter(Mandatory)][string]$Message
    )
    Assert-True ([regex]::IsMatch($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor [System.Text.RegularExpressions.RegexOptions]::Multiline)) $Message
}

function Get-TreeSummary {
    param([Parameter(Mandatory)][string]$Root)
    $rootFull = (Resolve-Path -LiteralPath $Root).Path
    $paths = [System.Collections.Generic.List[string]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File)) {
        $paths.Add($file.FullName.Substring($rootFull.Length + 1).Replace('\', '/'))
    }
    $paths.Sort([System.StringComparer]::Ordinal)
    $rows = [System.Collections.Generic.List[string]]::new()
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

function Get-LineCountFromBytes {
    param([Parameter(Mandatory)][string]$Path)
    $count = 0
    foreach ($value in [System.IO.File]::ReadAllBytes($Path)) {
        if ($value -eq 10) { $count++ }
    }
    return $count
}

function Resolve-DefectAnchor {
    param([Parameter(Mandatory)]$Anchor)
    switch ([string]$Anchor.origin) {
        'framework' { return Join-Path $frameworkRoot ([string]$Anchor.path).Replace('/', '\') }
        'agentic' { return Join-Path $agenticRoot ([string]$Anchor.path).Replace('/', '\') }
        'instruction_package' { return Join-Path $instructionRoot ([string]$Anchor.path).Replace('/', '\') }
        default { throw "G03 assertion failed: unknown source-anchor origin '$($Anchor.origin)'." }
    }
}

function Assert-SemVer {
    param(
        [Parameter(Mandatory)]$Value,
        [Parameter(Mandatory)][int]$Major,
        [Parameter(Mandatory)][int]$Minor,
        [Parameter(Mandatory)][int]$Patch,
        [Parameter(Mandatory)][string]$Message
    )
    Assert-Properties $Value @('major', 'minor', 'patch', 'prerelease') $Message
    Assert-Exact ([int]$Value.major) $Major "$Message major"
    Assert-Exact ([int]$Value.minor) $Minor "$Message minor"
    Assert-Exact ([int]$Value.patch) $Patch "$Message patch"
    Assert-True ($null -eq $Value.prerelease) "$Message prerelease must be null"
}

function Assert-Decision {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string[]]$Patterns
    )
    $path = Join-Path $decisionRoot $Name
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "missing accepted P03 decision $Name"
    $text = Get-Content -Raw -LiteralPath $path
    Assert-ContainsPattern $text '^Status:\s*Accepted\s*$' "$Name is not accepted"
    Assert-ContainsPattern $text '^##\s+Evidence\s*$' "$Name has no Evidence section"
    Assert-True (-not [regex]::IsMatch($text, '\b(?:TBD|TODO|unknown|placeholder)\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) "$Name contains an unresolved marker"
    foreach ($pattern in $Patterns) {
        Assert-ContainsPattern $text $pattern "$Name is missing semantic marker /$pattern/"
    }
}

Assert-True (Test-Path -LiteralPath $frameworkRoot -PathType Container) 'pinned Forsetti Framework root is missing'
Assert-True (Test-Path -LiteralPath $agenticRoot -PathType Container) 'pinned Forsetti Agentic Edition root is missing'

# Immutable source lock and extracted-tree identity.
$sourceLockPath = Join-Path $baselineRoot 'p03-forsetti-source-lock.json'
$publicApiPath = Join-Path $baselineRoot 'p03-forsetti-public-api.json'
Assert-True (Test-Path -LiteralPath $sourceLockPath -PathType Leaf) 'P03 source lock is missing'
Assert-True (Test-Path -LiteralPath $publicApiPath -PathType Leaf) 'P03 public API inventory is missing'
$sourceLock = Read-JsonFile $sourceLockPath
$publicApi = Read-JsonFile $publicApiPath
Assert-Exact ([int]$sourceLock.schema_version) 1 'source-lock schema version'
Assert-Exact ([string]$sourceLock.lock_id) 'P03-FORSETTI-SOURCE-LOCK' 'source-lock identity'
Assert-Exact (Get-FileSha256 (Join-Path $WorkspaceRoot '.forge-inputs\archives\SOURCE-HASHES.json')) '1032838a2da517f391693bef862167bdb7cf434520ef42e314c2983bc2195cd3' 'SOURCE-HASHES SHA-256'

$sourceExpectations = @(
    [ordered]@{ id = 'forsetti-framework-windows'; version = '0.2.0'; archive = '.forge-inputs/archives/Forsetti-Framework-Windows-main.zip'; archive_bytes = 340536L; archive_sha256 = '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d'; root = $frameworkRelative; files = 171; bytes = 723455L; tree_sha256 = 'cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28' },
    [ordered]@{ id = 'forsetti-agentic-edition'; version = '1.1.0'; archive = '.forge-inputs/archives/forsetti-agentic-edition-main.zip'; archive_bytes = 789736L; archive_sha256 = 'e8ef20ad917bd3165335c03beecc674f21f69ff93cbc64fe5edb4e9ffd79692b'; root = $agenticRelative; files = 349; bytes = 1765772L; tree_sha256 = '00c50957591c27111284098de892a038ee18cad58373976ba731d295636a80fb' }
)
Assert-Exact @($sourceLock.sources).Count 2 'locked source count'
foreach ($expectedSource in $sourceExpectations) {
    $matches = @($sourceLock.sources | Where-Object { [string]$_.id -ceq [string]$expectedSource.id })
    Assert-Exact $matches.Count 1 "source-lock entry count for $($expectedSource.id)"
    $source = $matches[0]
    Assert-Exact ([string]$source.version) ([string]$expectedSource.version) "$($expectedSource.id) version"
    Assert-Exact ([string]$source.archive.path) ([string]$expectedSource.archive) "$($expectedSource.id) archive path"
    Assert-Exact ([long]$source.archive.bytes) ([long]$expectedSource.archive_bytes) "$($expectedSource.id) archive bytes"
    Assert-Exact ([string]$source.archive.sha256) ([string]$expectedSource.archive_sha256) "$($expectedSource.id) archive lock hash"
    $archivePath = Join-Path $WorkspaceRoot ([string]$expectedSource.archive).Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $archivePath -PathType Leaf) "$($expectedSource.id) archive missing"
    Assert-Exact ([long](Get-Item -LiteralPath $archivePath).Length) ([long]$expectedSource.archive_bytes) "$($expectedSource.id) actual archive bytes"
    Assert-Exact (Get-FileSha256 $archivePath) ([string]$expectedSource.archive_sha256) "$($expectedSource.id) actual archive SHA-256"
    Assert-Exact ([string]$source.extracted.root) ([string]$expectedSource.root) "$($expectedSource.id) extracted root"
    $tree = Get-TreeSummary (Join-Path $WorkspaceRoot ([string]$expectedSource.root).Replace('/', '\'))
    Assert-Exact ([int]$tree.files) ([int]$expectedSource.files) "$($expectedSource.id) extracted file count"
    Assert-Exact ([long]$tree.bytes) ([long]$expectedSource.bytes) "$($expectedSource.id) extracted byte count"
    Assert-Exact ([string]$tree.sha256) ([string]$expectedSource.tree_sha256) "$($expectedSource.id) extracted tree SHA-256"
    Assert-Exact ([string]$source.extracted.tree_sha256) ([string]$expectedSource.tree_sha256) "$($expectedSource.id) baseline tree SHA-256"
}

# Task contract, project context, profile, and canonical schemas.
$contractPath = Join-Path $instructionRoot 'governance\PORT_TASK_CONTRACT.json'
$contextPath = Join-Path $instructionRoot 'governance\forsetti-project-context.json'
$taskSchemaPath = Join-Path $instructionRoot 'governance\schemas\task-contract.schema.json'
$contextSchemaPath = Join-Path $instructionRoot 'governance\schemas\forsetti-project-context.schema.json'
$manifestSchemaPath = Join-Path $instructionRoot 'governance\schemas\module-manifest-1.1.schema.json'
$profilePath = Join-Path $agenticRoot 'editions\windows\forsetti-windows-0.2.0.profile.json'
$profileCopyPath = Join-Path $instructionRoot 'governance\source\forsetti-agentic\editions\windows\forsetti-windows-0.2.0.profile.json'
foreach ($path in @($contractPath, $contextPath, $taskSchemaPath, $contextSchemaPath, $manifestSchemaPath, $profilePath, $profileCopyPath)) {
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "required governance input missing: $path"
}
Assert-Exact (Get-FileSha256 $contractPath) ([string]$sourceLock.governance_pins.task_contract.sha256) 'task contract lock hash'
Assert-Exact (Get-FileSha256 $contextPath) ([string]$sourceLock.governance_pins.project_context.sha256) 'project context lock hash'
Assert-Exact (Get-FileSha256 $manifestSchemaPath) '0d768364790214335ca3b1b585cfd3be2af5e81313b771463f367a61bd0ed90d' 'canonical manifest schema hash'
Assert-Exact (Get-FileSha256 $profilePath) '4a65c4c986da951cddd2376de39ebc87f1186b21c147c53c122ce0c10ac19c8a' 'immutable Windows profile hash'
Assert-Exact (Get-FileSha256 $profileCopyPath) '4a65c4c986da951cddd2376de39ebc87f1186b21c147c53c122ce0c10ac19c8a' 'governance Windows profile copy hash'

$contract = Read-JsonFile $contractPath
$context = Read-JsonFile $contextPath
$profile = Read-JsonFile $profilePath
Assert-Properties $contract @('$schema', 'schema_version', 'task_id', 'title', 'date', 'initiating_request', 'contract_authoring_role', 'acting_role', 'reviewer_role', 'release_review_role', 'documentation_review_role', 'required_advisory_reviewers', 'change_class', 'approval_class', 'governance_authorization', 'forsetti_project_context', 'objective', 'business_reason', 'scope', 'required_outputs', 'documentation_impact', 'release_impact', 'validation_requirements', 'evidence_requirements', 'constraints', 'risks', 'escalation_triggers', 'definition_of_done', 'completion_summary_requirements') 'task contract'
Assert-Exact ([string]$contract.schema_version) '1.0' 'task contract schema version'
Assert-Exact ([string]$contract.task_id) 'FAE-TASK-2026-08-24-016' 'task contract ID'
Assert-True ([regex]::IsMatch([string]$contract.task_id, '^FAE-(TASK|BUG|GOV|REL|META)-(\d{3,}|\d{4}-\d{2}-\d{2}-\d{3,})$')) 'task contract ID pattern'
Assert-Exact ([string]$contract.contract_authoring_role) 'architect' 'contract authoring role'
Assert-Exact ([string]$contract.acting_role) 'builder' 'contract acting role'
Assert-Exact ([string]$contract.reviewer_role) 'validator' 'contract reviewer role'
Assert-Exact ([string]$contract.approval_class) 'release-critical' 'contract approval class'
Assert-True (@($contract.scope.in_scope).Count -gt 0) 'contract in-scope list is empty'
Assert-True (@($contract.required_outputs).Count -gt 0) 'contract required outputs are empty'
Assert-True (@($contract.validation_requirements).Count -gt 0) 'contract validation requirements are empty'
Assert-True (@($contract.evidence_requirements).Count -gt 0) 'contract evidence requirements are empty'
Assert-True ([bool]$contract.documentation_impact.readme_update -and [bool]$contract.documentation_impact.wiki_update -and [bool]$contract.documentation_impact.glossary_update -and [bool]$contract.documentation_impact.changelog_update) 'contract documentation impact is incomplete'

$contextProperties = @('repository_mode', 'forsetti_edition', 'target_platform', 'framework_version', 'edition_profile', 'manifest_schema_version', 'manifest_template_version', 'deployment_pattern', 'module_type', 'module_id', 'capabilities_requested', 'runtime_requirements_declared', 'uses_public_api_only', 'touches_framework_internals')
Assert-Properties $context $contextProperties 'project context'
Assert-Properties $contract.forsetti_project_context $contextProperties 'nested project context'
Assert-Exact (ConvertTo-CompactJson $contract.forsetti_project_context) (ConvertTo-CompactJson $context) 'task contract/project-context equality'
Assert-Exact ([string]$context.repository_mode) 'consumer_app_repo' 'repository mode'
Assert-Exact ([string]$context.forsetti_edition) 'windows' 'Forsetti edition'
Assert-Exact ([string]$context.target_platform) 'Windows' 'target platform'
Assert-Exact ([string]$context.framework_version) '0.2.0' 'context framework version'
Assert-Exact ([string]$context.edition_profile) '0.2.0' 'context edition profile'
Assert-Exact ([string]$context.manifest_schema_version) '1.1' 'context manifest schema'
Assert-Exact ([string]$context.manifest_template_version) '1.1' 'context manifest template'
Assert-Exact ([string]$context.deployment_pattern) 'single_app_module' 'deployment pattern'
Assert-Exact ([string]$context.module_type) 'app' 'context module type'
Assert-Exact ([string]$context.module_id) 'com.forsetti.app.forge-conductor-windows' 'context module ID'
Assert-Sequence @($context.capabilities_requested) $expectedCapabilities 'context capabilities'
Assert-Unique @($context.capabilities_requested) 'context capabilities must be unique'
Assert-True ([bool]$context.runtime_requirements_declared) 'runtime requirements must be declared'
Assert-True ([bool]$context.uses_public_api_only) 'public API-only declaration must be true'
Assert-True (-not [bool]$context.touches_framework_internals) 'framework internals declaration must be false'

Assert-Properties $profile @('edition', 'frameworkVersion', 'supportedPlatforms', 'nativeLanguage', 'nativeTools', 'publicProducts', 'manifest', 'capabilities', 'dependencyRules', 'verificationCommands') 'Windows profile'
Assert-Exact ([string]$profile.edition) 'windows' 'profile edition'
Assert-Exact ([string]$profile.frameworkVersion) '0.2.0' 'profile framework version'
Assert-Sequence @($profile.supportedPlatforms) @('Windows') 'profile platforms'
Assert-Exact ([string]$profile.nativeLanguage) 'C++20' 'profile native language'
Assert-Sequence @($profile.publicProducts) @('ForsettiCore', 'ForsettiPlatform', 'ForsettiHostTemplate') 'profile public products'
Assert-Exact ([string]$profile.manifest.currentSchemaVersion) '1.1' 'profile schema version'
Assert-Exact ([string]$profile.manifest.currentTemplateVersion) '1.1' 'profile template version'
Assert-Set @($profile.manifest.requiredFields) @('schemaVersion', 'manifestTemplateVersion', 'moduleID', 'displayName', 'moduleVersion', 'moduleType', 'supportedPlatforms', 'minForsettiVersion', 'maxForsettiVersion', 'capabilitiesRequested', 'iapProductID', 'entryPoint', 'defaultModuleRole', 'runtimeRequirements') 'profile manifest required fields'
Assert-True ('crypto_utilities' -in @($profile.capabilities)) 'profile must retain upstream crypto_utilities capability evidence'
foreach ($capability in $expectedCapabilities) {
    Assert-True ($capability -in @($profile.capabilities)) "consumer capability '$capability' is not profile-approved"
}
Assert-Sequence @($profile.dependencyRules.ForsettiCore) @('depends_on:nothing_in_repository') 'ForsettiCore dependency rule'
Assert-Sequence @($profile.dependencyRules.ForsettiPlatform) @('depends_on:ForsettiCore') 'ForsettiPlatform dependency rule'
Assert-Sequence @($profile.dependencyRules.ForsettiHostTemplate) @('depends_on:ForsettiCore', 'depends_on:ForsettiPlatform') 'ForsettiHostTemplate dependency rules'

# Canonical manifest, byte-identical packaging mirror, and one logical app module.
$canonicalManifestRelative = 'src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests/ForgeConductorAppModule.json'
$mirrorManifestRelative = 'manifests/ForsettiManifests/forge-conductor.json'
$canonicalManifestPath = Join-Path $WorkspaceRoot $canonicalManifestRelative.Replace('/', '\')
$mirrorManifestPath = Join-Path $WorkspaceRoot $mirrorManifestRelative.Replace('/', '\')
Assert-True (Test-Path -LiteralPath $canonicalManifestPath -PathType Leaf) 'canonical app-module manifest is missing'
Assert-True (Test-Path -LiteralPath $mirrorManifestPath -PathType Leaf) 'packaging manifest mirror is missing'
$manifestHash = Get-FileSha256 $canonicalManifestPath
Assert-Exact $manifestHash '36221cb7e23c2e1ab170afb96f9254fb518e4ca1e8b1215e8a2216f8a3effed0' 'canonical manifest SHA-256'
Assert-Exact (Get-FileSha256 $mirrorManifestPath) $manifestHash 'manifest mirror byte equality'

$manifestFiles = @()
foreach ($rootName in @('src', 'manifests')) {
    $rootPath = Join-Path $WorkspaceRoot $rootName
    if (Test-Path -LiteralPath $rootPath -PathType Container) {
        foreach ($file in @(Get-ChildItem -LiteralPath $rootPath -Recurse -Force -File -Filter '*.json')) {
            if ($file.DirectoryName -match '[\\/]ForsettiManifests$') {
                $manifestFiles += $file.FullName.Substring($WorkspaceRoot.Length + 1).Replace('\', '/')
            }
        }
    }
}
$manifestFiles = Get-OrdinalSortedStrings $manifestFiles
Assert-Sequence $manifestFiles @($mirrorManifestRelative, $canonicalManifestRelative) 'Forsetti manifest file set'

$manifest = Read-JsonFile $canonicalManifestPath
Assert-Properties $manifest @('schemaVersion', 'manifestTemplateVersion', 'moduleID', 'displayName', 'moduleVersion', 'moduleType', 'supportedPlatforms', 'minForsettiVersion', 'maxForsettiVersion', 'capabilitiesRequested', 'iapProductID', 'entryPoint', 'defaultModuleRole', 'runtimeRequirements') 'module manifest'
Assert-Exact ([string]$manifest.schemaVersion) '1.1' 'manifest schemaVersion'
Assert-Exact ([string]$manifest.manifestTemplateVersion) '1.1' 'manifest template version'
Assert-Exact ([string]$manifest.moduleID) 'com.forsetti.app.forge-conductor-windows' 'manifest module ID'
Assert-True ([regex]::IsMatch([string]$manifest.moduleID, '^[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9-]*)+$')) 'manifest moduleID pattern'
Assert-True (-not ([string]$manifest.moduleID).StartsWith('forsetti.', [System.StringComparison]::Ordinal)) 'manifest may not use reserved forsetti module namespace'
Assert-Exact ([string]$manifest.displayName) 'Forge Conductor' 'manifest display name'
Assert-SemVer $manifest.moduleVersion 0 9 0 'manifest module version'
Assert-Exact ([string]$manifest.moduleType) 'app' 'manifest module type'
Assert-Sequence @($manifest.supportedPlatforms) @('Windows') 'manifest supported platforms'
Assert-SemVer $manifest.minForsettiVersion 0 2 0 'minimum Forsetti version'
Assert-SemVer $manifest.maxForsettiVersion 0 2 0 'maximum Forsetti version'
Assert-Sequence @($manifest.capabilitiesRequested) $expectedCapabilities 'manifest capabilities'
Assert-Unique @($manifest.capabilitiesRequested) 'manifest capabilities must be unique'
Assert-True ('crypto_utilities' -notin @($manifest.capabilitiesRequested)) 'consumer manifest must not request disputed crypto_utilities'
Assert-True ($null -eq $manifest.iapProductID) 'manifest iapProductID must be null'
Assert-Exact ([string]$manifest.entryPoint) 'ForgeConductorAppModule' 'manifest entry point'
Assert-True ([regex]::IsMatch([string]$manifest.entryPoint, '^[A-Za-z_][A-Za-z0-9_.]*$')) 'manifest entryPoint pattern'
Assert-Exact ([string]$manifest.defaultModuleRole) 'ui' 'manifest default role'

Assert-Properties $manifest.runtimeRequirements @('io', 'ui', 'dataIsolation') 'runtime requirements'
Assert-Exact @($manifest.runtimeRequirements.io).Count $expectedIo.Count 'manifest I/O requirement count'
$requirementIds = @($manifest.runtimeRequirements.io | ForEach-Object { [string]$_.requirementID })
Assert-Unique $requirementIds 'manifest I/O requirement IDs must be unique'
for ($index = 0; $index -lt $expectedIo.Count; $index++) {
    $actualIo = $manifest.runtimeRequirements.io[$index]
    $expectedIoItem = $expectedIo[$index]
    Assert-Properties $actualIo @('requirementID', 'kind', 'access', 'required') "manifest I/O requirement $index"
    Assert-Exact ([string]$actualIo.requirementID) ([string]$expectedIoItem.requirementID) "manifest I/O requirement $index ID"
    Assert-Exact ([string]$actualIo.kind) ([string]$expectedIoItem.kind) "manifest I/O requirement $index kind"
    Assert-Exact ([string]$actualIo.access) ([string]$expectedIoItem.access) "manifest I/O requirement $index access"
    Assert-True ([bool]$actualIo.required) "manifest I/O requirement $index must be required"
    Assert-True ([string]$actualIo.kind -in @($manifest.capabilitiesRequested)) "manifest I/O kind '$($actualIo.kind)' lacks its capability"
}

$ui = $manifest.runtimeRequirements.ui
Assert-Properties $ui @('controlSchemeID', 'layoutID', 'themeIDs', 'viewIDs', 'slotIDs', 'toolbarItemIDs', 'routeIDs', 'pointerIDs') 'manifest UI requirements'
Assert-Exact ([string]$ui.controlSchemeID) 'forge-conductor.windows.controls.v1' 'UI control scheme'
Assert-Exact ([string]$ui.layoutID) 'forge-conductor.windows.shell.v1' 'UI layout'
Assert-Sequence @($ui.themeIDs) @() 'UI theme IDs'
Assert-Sequence @($ui.viewIDs) @('ForgeConductorShellView', 'ForgeConductorSettingsOverlayView', 'ForgeConductorResetConfirmationView', 'ForgeConductorPurgeConfirmationView') 'UI view IDs'
Assert-Sequence @($ui.slotIDs) @('appShell') 'UI slot IDs'
Assert-Sequence @($ui.toolbarItemIDs) @('forge-conductor.toolbar.settings') 'UI toolbar IDs'
Assert-Sequence @($ui.routeIDs) @('forge-conductor.route.settings', 'forge-conductor.route.reset-project', 'forge-conductor.route.purge-all') 'UI route IDs'
Assert-Sequence @($ui.pointerIDs) @() 'UI pointer IDs'
$isolation = $manifest.runtimeRequirements.dataIsolation
Assert-Properties $isolation @('mode', 'ownedStoreIDs', 'requiredDefaultRoles') 'manifest data isolation'
Assert-Exact ([string]$isolation.mode) 'private_to_module' 'data isolation mode'
Assert-Sequence @($isolation.ownedStoreIDs) $expectedStores 'owned store IDs'
Assert-Unique @($isolation.ownedStoreIDs) 'owned store IDs must be unique'
Assert-Sequence @($isolation.requiredDefaultRoles) @() 'required default roles'

# Public API inventory and exact public-header tree.
Assert-Exact ([int]$publicApi.schema_version) 1 'public API inventory schema'
Assert-Exact ([string]$publicApi.inventory_id) 'P03-FORSETTI-WINDOWS-PUBLIC-API' 'public API inventory ID'
Assert-Exact ([string]$publicApi.framework_version) '0.2.0' 'public API framework version'
Assert-Exact ([string]$publicApi.source_archive_sha256) '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d' 'public API source archive pin'
Assert-Exact ([int]$publicApi.aggregate.headers) 34 'public header count'
Assert-Exact ([long]$publicApi.aggregate.bytes) 81656L 'public header bytes'
Assert-Exact ([int]$publicApi.aggregate.lines) 2491 'public header lines'
Assert-Exact ([string]$publicApi.aggregate.sha256) 'e31c38c4569a67560696085c9ac060f6eac4ea067ed95af515355dc79435e4b3' 'public header aggregate SHA-256'
Assert-Exact @($publicApi.headers).Count 34 'public header inventory rows'

$includeRoot = Join-Path $frameworkRoot 'include'
$actualHeaderPaths = [System.Collections.Generic.List[string]]::new()
foreach ($file in @(Get-ChildItem -LiteralPath $includeRoot -Recurse -Force -File -Filter '*.h')) {
    $actualHeaderPaths.Add($file.FullName.Substring($includeRoot.Length + 1).Replace('\', '/'))
}
$actualHeaderPaths.Sort([System.StringComparer]::OrdinalIgnoreCase)
$aggregateRows = [System.Collections.Generic.List[string]]::new()
$totalHeaderBytes = 0L
$totalHeaderLines = 0
for ($index = 0; $index -lt $actualHeaderPaths.Count; $index++) {
    $path = [string]$actualHeaderPaths[$index]
    $row = $publicApi.headers[$index]
    Assert-Exact ([string]$row.path) $path "public header row $index path"
    $fullPath = Join-Path $includeRoot $path.Replace('/', '\')
    $file = Get-Item -LiteralPath $fullPath
    $hash = Get-FileSha256 $fullPath
    $lineCount = Get-LineCountFromBytes $fullPath
    Assert-Exact ([long]$row.bytes) ([long]$file.Length) "public header $path bytes"
    Assert-Exact ([int]$row.lines) ([int]$lineCount) "public header $path lines"
    Assert-Exact ([string]$row.sha256) $hash "public header $path SHA-256"
    Assert-Exact ([string]$row.product) (($path -split '/', 2)[0]) "public header $path product"
    $aggregateRows.Add($path + "`t" + $hash)
    $totalHeaderBytes += [long]$file.Length
    $totalHeaderLines += [int]$lineCount
}
Assert-Exact $actualHeaderPaths.Count 34 'actual public header count'
Assert-Exact $totalHeaderBytes 81656L 'actual public header bytes'
Assert-Exact $totalHeaderLines 2491 'actual public header lines'
Assert-Exact (Get-StringSha256 ($aggregateRows -join "`n")) 'e31c38c4569a67560696085c9ac060f6eac4ea067ed95af515355dc79435e4b3' 'actual public header aggregate SHA-256'
Assert-Sequence @($publicApi.products.name) @('ForsettiCore', 'ForsettiPlatform', 'ForsettiHostTemplate') 'public API products'
Assert-Sequence @($publicApi.products.headers) @(24, 5, 5) 'public API product header counts'
Assert-Sequence @($publicApi.products.bytes) @(65109, 9299, 7248) 'public API product bytes'
Assert-Sequence @($publicApi.products.lines) @(1984, 275, 232) 'public API product lines'
Assert-Exact ([int]$publicApi.policy.interface_count) 25 'public interface count'
Assert-Exact ([int]$publicApi.policy.value_type_count) 19 'public value-type count'
Assert-Exact ([int]$publicApi.policy.enum_count) 17 'public enum/alias count'
Assert-Exact @($publicApi.policy_symbols).Count 61 'public policy symbol count'

# Full named public-type inventory. This is intentionally separate from the
# narrower framework-policy symbol subset above.
$namedInventory = $publicApi.named_type_inventory
Assert-Exact ([int]$namedInventory.raw_declaration_sites) 149 'raw named-type declaration-site count'
Assert-Exact ([int]$namedInventory.unique_before_visibility_filter) 146 'named types before visibility filtering'
Assert-Exact ([int]$namedInventory.counts.total) 145 'named public-type count'
Assert-Exact ([int]$namedInventory.counts.classes) 83 'named public class count'
Assert-Exact ([int]$namedInventory.counts.structs) 36 'named public struct count'
Assert-Exact ([int]$namedInventory.counts.enums) 23 'named public enum count'
Assert-Exact ([int]$namedInventory.counts.aliases) 3 'named public alias count'
Assert-Exact @($namedInventory.types).Count 145 'named public-type inventory rows'
Assert-Exact @($namedInventory.exclusions).Count 1 'named-type visibility exclusion count'
$visibilityExclusion = $namedInventory.exclusions[0]
Assert-Exact ([string]$visibilityExclusion.kind) 'alias' 'visibility exclusion kind'
Assert-Exact ([string]$visibilityExclusion.name) 'RegistryKey' 'visibility exclusion name'
Assert-Exact ([string]$visibilityExclusion.declarations[0].path) 'ForsettiPlatform/WindowsViewFactoryRegistry.h' 'visibility exclusion path'
Assert-Exact ([int]$visibilityExclusion.declarations[0].line) 89 'visibility exclusion line'

$actualNamedTypeMap = [ordered]@{}
$actualRawNamedTypeSites = 0
$typePatterns = [ordered]@{
    class = '^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    struct = '^\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    enum = '^\s*enum(?:\s+class)?\s+([A-Za-z_][A-Za-z0-9_]*)\b'
    alias = '^\s*using\s+([A-Za-z_][A-Za-z0-9_]*)\s*='
}
foreach ($headerPath in $actualHeaderPaths) {
    $declarationLines = [System.IO.File]::ReadAllLines((Join-Path $includeRoot $headerPath.Replace('/', '\')))
    for ($lineIndex = 0; $lineIndex -lt $declarationLines.Count; $lineIndex++) {
        foreach ($typePattern in $typePatterns.GetEnumerator()) {
            $typeMatch = [regex]::Match([string]$declarationLines[$lineIndex], [string]$typePattern.Value)
            if (-not $typeMatch.Success) { continue }
            $actualRawNamedTypeSites++
            $kind = [string]$typePattern.Key
            $name = [string]$typeMatch.Groups[1].Value
            $key = $kind + "`t" + $name
            if (-not $actualNamedTypeMap.Contains($key)) {
                $actualNamedTypeMap[$key] = [ordered]@{
                    kind = $kind
                    name = $name
                    declarations = [System.Collections.Generic.List[object]]::new()
                }
            }
            $actualNamedTypeMap[$key].declarations.Add([ordered]@{ path = $headerPath; line = $lineIndex + 1 })
            break
        }
    }
}
$actualAllNamedTypes = @($actualNamedTypeMap.Values)
Assert-Exact $actualRawNamedTypeSites 149 'actual raw named-type declaration-site count'
Assert-Exact $actualAllNamedTypes.Count 146 'actual unique named-type count before visibility filtering'
$actualPublicNamedTypes = @($actualAllNamedTypes | Where-Object {
    -not ([string]$_.kind -ceq 'alias' -and [string]$_.name -ceq 'RegistryKey')
})
Assert-Exact $actualPublicNamedTypes.Count 145 'actual named public-type count'
for ($typeIndex = 0; $typeIndex -lt $actualPublicNamedTypes.Count; $typeIndex++) {
    $expectedType = $namedInventory.types[$typeIndex]
    $actualType = $actualPublicNamedTypes[$typeIndex]
    Assert-Exact ([string]$expectedType.kind) ([string]$actualType.kind) "named public type $typeIndex kind"
    Assert-Exact ([string]$expectedType.name) ([string]$actualType.name) "named public type $typeIndex name"
    Assert-Exact @($expectedType.declarations).Count @($actualType.declarations).Count "named public type $($actualType.name) declaration count"
    for ($siteIndex = 0; $siteIndex -lt @($actualType.declarations).Count; $siteIndex++) {
        Assert-Exact ([string]$expectedType.declarations[$siteIndex].path) ([string]$actualType.declarations[$siteIndex].path) "named public type $($actualType.name) declaration $siteIndex path"
        Assert-Exact ([int]$expectedType.declarations[$siteIndex].line) ([int]$actualType.declarations[$siteIndex].line) "named public type $($actualType.name) declaration $siteIndex line"
    }
}

$frameworkPolicyPath = Join-Path $frameworkRoot 'framework-policy.json'
$frameworkPolicy = Read-JsonFile $frameworkPolicyPath
Assert-Exact (Get-FileSha256 $frameworkPolicyPath) '2331dffd25a17356457cc64f9fce8fd4b8a8e92f758cea29a13dec44cac9e150' 'framework policy SHA-256'
$symbolIndex = 0
foreach ($kind in @('interfaces', 'valueTypes', 'enums')) {
    foreach ($nameValue in @($frameworkPolicy.corePublicAPI.$kind)) {
        $symbol = $publicApi.policy_symbols[$symbolIndex]
        $name = [string]$nameValue
        Assert-Exact ([string]$symbol.kind) $kind "public symbol $symbolIndex kind"
        Assert-Exact ([string]$symbol.name) $name "public symbol $symbolIndex name"
        Assert-True ($null -ne $symbol.declaration) "public symbol $name has no declaration anchor"
        $declarationPath = [string]$symbol.declaration.path
        $declarationLine = [int]$symbol.declaration.line
        Assert-True ($declarationPath -in @($actualHeaderPaths)) "public symbol $name anchor path is not a public header"
        $declarationLines = @(Get-Content -LiteralPath (Join-Path $includeRoot $declarationPath.Replace('/', '\')))
        Assert-True ($declarationLine -ge 1 -and $declarationLine -le $declarationLines.Count) "public symbol $name anchor line is out of range"
        Assert-ContainsPattern ([string]$declarationLines[$declarationLine - 1]) ('\b' + [regex]::Escape($name) + '\b') "public symbol $name anchor line does not declare the symbol"
        $symbolIndex++
    }
}
Assert-Sequence @($publicApi.consumption_boundary.allowed_include_prefixes) @('ForsettiCore/', 'ForsettiPlatform/', 'ForsettiHostTemplate/') 'allowed Forsetti include prefixes'
Assert-Sequence @($publicApi.consumption_boundary.app_module_allowed_include_prefixes) @('ForsettiCore/') 'app-module Forsetti include prefixes'
Assert-Sequence @($publicApi.consumption_boundary.sealed_products) @('ForsettiCore', 'ForsettiPlatform', 'ForsettiHostTemplate') 'sealed products'
Assert-True (-not [bool]$publicApi.consumption_boundary.framework_sources_may_be_modified) 'framework source modification must be prohibited'

# Accepted architecture/dependency/licensing decisions.
$decisionSpecifications = [ordered]@{
    'P03-001-governance-profile-and-source-pins.md' = @('Windows', '0\.2\.0', 'schema\s+1\.1', '3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d', 'e8ef20ad917bd3165335c03beecc674f21f69ff93cbc64fe5edb4e9ffd79692b', 'source precedence')
    'P03-002-sealed-forsetti-consumption.md' = @('byte-for-byte|byte-unchanged', 'published include|public header', 'separately built imported targets', 'add_subdirectory', 'internal.*patch|patching Forsetti')
    'P03-003-single-app-module-and-process-topology.md' = @('com\.forsetti\.app\.forge-conductor-windows', 'only registered Forge app/UI module', 'ForgeConductor\.App\.exe', 'forge-conductor\.exe', 'ForgeConductor\.Manager\.exe', 'ForgeConductor\.SessionHost\.exe', 'ForgeConductor\.Setup\.exe', 'not Forsetti modules')
    'P03-004-dependency-direction-and-composition-roots.md' = @('ForgeConductor\.Domain', 'ForgeConductor\.Contracts', 'ForgeConductor\.Application', 'ForsettiModule.*links only.*ForsettiCore', 'ForsettiHostTemplate', 'ForsettiPlatform', 'constructors', 'service locator')
    'P03-005-manifest-capabilities-and-runtime-requirements.md' = @([regex]::Escape($canonicalManifestRelative), 'byte-equivalent packaging mirror', 'moduleVersion:\s*0\.9\.0', 'canonical order', 'defaultModuleRole:\s*ui', 'contract test.*registered module descriptor')
    'P03-006-host-activation-service-and-ui-composition.md' = @('Production composition.*Provider', 'AllowAll', 'Noop', 'Immediate', 'registerAll', 'restoreOnly', 'explicitModuleIDs.*com\.forsetti\.app\.forge-conductor-windows')
    'P03-007-data-isolation-and-cross-process-ownership.md' = @('private_to_module', 'forge-conductor\.central.*forge-conductor\.project.*forge-conductor\.configuration.*forge-conductor\.manager-state.*forge-conductor\.session-ledger', 'ForgeConductor\.Manager\.exe', 'serializes', 'cross-process', 'authenticated.*IPC', 'peer Forsetti module')
    'P03-008-boundary-lifetimes-errors-and-bounds.md' = @('lifecycle owner', 'weak lifetime', 'reverse-order destruction', 'raw this', 'exceptions.*caught', 'deadline', 'cancellation', 'bounded', 'shutdown')
    'P03-009-licensing-provenance-and-attribution.md' = @('separably linked', 'byte-unchanged', 'Apache License 2\.0', 'THIRD-PARTY-NOTICES', 'proprietary header', 'conflict', 'legal attribution', 'automated-authorship attribution')
    'P03-010-g03-validation-and-upstream-defects.md' = @('fail-closed consumer validator', 'full upstream test suite therefore did not pass', 'bad_any_cast', 'ModuleRegistry', 'ModuleManager', 'classic mode')
}
foreach ($entry in $decisionSpecifications.GetEnumerator()) {
    Assert-Decision ([string]$entry.Key) @($entry.Value)
}
$p03DecisionFiles = @(Get-ChildItem -LiteralPath $decisionRoot -File -Filter 'P03-*.md' | ForEach-Object { $_.Name })
Assert-Exact $p03DecisionFiles.Count 10 'P03 decision count'
Assert-Set $p03DecisionFiles @($decisionSpecifications.Keys) 'P03 decision filenames'

$upstreamDecisionPath = Join-Path $decisionRoot 'P03-010-g03-validation-and-upstream-defects.md'
$upstreamDecisionText = Get-Content -Raw -LiteralPath $upstreamDecisionPath
$expectedDefectIds = @(1..14 | ForEach-Object { 'UP-{0:D3}' -f $_ })
Assert-Sequence @($sourceLock.upstream_defects.id) $expectedDefectIds 'source-lock upstream defect IDs'
foreach ($defect in $sourceLock.upstream_defects) {
    Assert-ContainsPattern $upstreamDecisionText ('\b' + [regex]::Escape([string]$defect.id) + '\b') "P03-010 is missing $($defect.id)"
    Assert-Exact ([string]$defect.disposition) 'consumer_validator_compensates; upstream sources remain byte-unchanged' "$($defect.id) disposition"
    Assert-True (@($defect.evidence).Count -gt 0) "$($defect.id) has no source evidence"
    foreach ($anchor in $defect.evidence) {
        $anchorPath = Resolve-DefectAnchor $anchor
        Assert-True (Test-Path -LiteralPath $anchorPath -PathType Leaf) "$($defect.id) source anchor is missing: $($anchor.path)"
        if ($anchor.PSObject.Properties.Name -contains 'bytes') {
            Assert-Exact ([long](Get-Item -LiteralPath $anchorPath).Length) ([long]$anchor.bytes) "$($defect.id) anchor bytes: $($anchor.path)"
        }
        Assert-Exact (Get-FileSha256 $anchorPath) ([string]$anchor.sha256) "$($defect.id) anchor SHA-256: $($anchor.path)"
    }
}

Assert-Exact ([string]$sourceLock.upstream_validation.status) 'known_upstream_failure' 'upstream validation status'
Assert-True (-not [bool]$sourceLock.upstream_validation.upstream_suite_passed) 'upstream suite must not be recorded as passed'
Assert-Exact ([string]$sourceLock.upstream_validation.vcpkg_baseline) 'cb2981c4e03d421fa03b9bb5044cd1986180e7e4' 'upstream vcpkg baseline'
$frameworkVcpkg = Read-JsonFile (Join-Path $frameworkRoot 'vcpkg.json')
Assert-Exact ([string]$frameworkVcpkg.'builtin-baseline') 'cb2981c4e03d421fa03b9bb5044cd1986180e7e4' 'framework vcpkg manifest baseline'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $frameworkRoot 'versions\baseline.json') -PathType Leaf)) 'UP-014 must be reevaluated because versions/baseline.json now exists'

$failureNames = @('ScopedServices_StorageAllowedWithCapability', 'ScopedServices_StorageRejectsTraversalKeys', 'Runtime_V11UIContributionIDsMustBeDeclared', 'Runtime_V11DeclaredThemeMaskIsPreserved')
Assert-Sequence @($sourceLock.upstream_validation.deterministic_failures.name) $failureNames 'known upstream failure names'
Assert-Sequence @($sourceLock.upstream_validation.deterministic_failures.detail) @('unhandled bad_any_cast', 'unhandled bad_any_cast', 'unhandled ModuleRegistry::makeModule exception through ModuleManager', 'unhandled ModuleRegistry::makeModule exception through ModuleManager') 'known upstream failure details'
foreach ($name in $failureNames) { Assert-ContainsPattern $upstreamDecisionText ([regex]::Escape($name)) "P03-010 missing upstream failure $name" }

$commandExpectations = @(
    [ordered]@{ id = '20260825T093342537Z-cf79732f'; exit = 1; sha256 = '15dd060166210fb45106c956b0c14559fa1e39eb1828c4796f848289d9b07992' },
    [ordered]@{ id = '20260825T093451916Z-b6fcf5ca'; exit = 0; sha256 = '585e16447dac12d25fa07d7dde8e5eabf5cdee52590146839ab91be2d289abf7' },
    [ordered]@{ id = '20260825T093633245Z-35b54512'; exit = 0; sha256 = '4fbfd29eb97fc5d1cce850ee7653b8d06906d7b224c326ceeded154c0c1a2d40' },
    [ordered]@{ id = '20260825T093648999Z-82d9497b'; exit = 0; sha256 = '97698f0cf281f064a315dceaeb6502c6a504a9c59b6d071bc4ec16d73605fd9c' },
    [ordered]@{ id = '20260825T093719995Z-2f4e92bd'; exit = 1; sha256 = 'e4fad91d7cf8df5aceed8ea2971267f164289dba718ef357a09f7f4153a67fa0' },
    [ordered]@{ id = '20260825T093810695Z-2db748d9'; exit = 0; sha256 = '3b7bf35d1aefc31b0c7ebbb51b0df452b16f37c9ac5b9d3d4945ea7b02f8860d' }
)
Assert-Sequence @($sourceLock.upstream_validation.commands.evidence_id) @($commandExpectations.id) 'upstream command IDs'
Assert-Sequence @($sourceLock.upstream_validation.commands.expected_exit_code) @($commandExpectations.exit) 'upstream command exits'
foreach ($expectedCommand in $commandExpectations) {
    $recordPath = Join-Path $WorkspaceRoot ('.forge-codex\state\commands\' + [string]$expectedCommand.id + '.json')
    Assert-True (Test-Path -LiteralPath $recordPath -PathType Leaf) "upstream command evidence missing: $($expectedCommand.id)"
    Assert-Exact (Get-FileSha256 $recordPath) ([string]$expectedCommand.sha256) "upstream command record SHA-256: $($expectedCommand.id)"
    $record = Read-JsonFile $recordPath
    Assert-Exact ([string]$record.id) ([string]$expectedCommand.id) "upstream command record ID: $($expectedCommand.id)"
    Assert-Exact ([string]$record.phase) 'P03' "upstream command phase: $($expectedCommand.id)"
    Assert-Exact ([int]$record.exit_code) ([int]$expectedCommand.exit) "upstream command exit: $($expectedCommand.id)"
    Assert-True (-not [bool]$record.timed_out) "upstream command timed out: $($expectedCommand.id)"
    $stdoutPath = Join-Path $WorkspaceRoot ([string]$record.stdout)
    $stderrPath = Join-Path $WorkspaceRoot ([string]$record.stderr)
    Assert-True (Test-Path -LiteralPath $stdoutPath -PathType Leaf) "upstream stdout missing: $($expectedCommand.id)"
    Assert-True (Test-Path -LiteralPath $stderrPath -PathType Leaf) "upstream stderr missing: $($expectedCommand.id)"
    Assert-Exact (Get-FileSha256 $stdoutPath) ([string]$record.stdout_sha256) "upstream stdout SHA-256: $($expectedCommand.id)"
    Assert-Exact (Get-FileSha256 $stderrPath) ([string]$record.stderr_sha256) "upstream stderr SHA-256: $($expectedCommand.id)"
}

# Full consumer source scan: public include prefixes only and no framework merge/patch.
$consumerSources = @()
$srcRoot = Join-Path $WorkspaceRoot 'src'
if (Test-Path -LiteralPath $srcRoot -PathType Container) {
    $consumerSources = @(Get-ChildItem -LiteralPath $srcRoot -Recurse -Force -File | Where-Object { $_.Extension.ToLowerInvariant() -in @('.h', '.hpp', '.hh', '.cpp', '.cxx', '.cc', '.ixx', '.cppm') })
}
foreach ($sourceFile in $consumerSources) {
    $relative = $sourceFile.FullName.Substring($WorkspaceRoot.Length + 1).Replace('\', '/')
    $text = Get-Content -Raw -LiteralPath $sourceFile.FullName
    foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]')) {
        $include = [string]$match.Groups[1].Value
        if ($include.StartsWith('Forsetti', [System.StringComparison]::Ordinal)) {
            $allowed = $include.StartsWith('ForsettiCore/', [System.StringComparison]::Ordinal) -or $include.StartsWith('ForsettiPlatform/', [System.StringComparison]::Ordinal) -or $include.StartsWith('ForsettiHostTemplate/', [System.StringComparison]::Ordinal)
            Assert-True $allowed "$relative uses non-public Forsetti include '$include'"
            if ($relative.StartsWith('src/ForgeConductor.ForsettiModule/', [System.StringComparison]::Ordinal)) {
                Assert-True ($include.StartsWith('ForsettiCore/', [System.StringComparison]::Ordinal)) "$relative app-module target may include ForsettiCore public headers only"
            }
        }
        Assert-True (-not [regex]::IsMatch($include, '(^|/)(src|internal|private)/', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) "$relative includes internal path '$include'"
    }
    Assert-True (-not [regex]::IsMatch($text, '\bForsettiExample(?:Service|UI|App)Module\b')) "$relative references an upstream example module"
}
foreach ($forbiddenRoot in @('src\ForsettiCore', 'src\ForsettiPlatform', 'src\ForsettiHostTemplate')) {
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot $forbiddenRoot))) "consumer contains copied sealed framework source at $forbiddenRoot"
}
$cmakeFiles = @(Get-ChildItem -LiteralPath $WorkspaceRoot -Recurse -Force -File | Where-Object {
    $_.FullName -notmatch '[\\/]\.forge-inputs[\\/]|[\\/]\.forge-codex[\\/]|[\\/]out[\\/]|[\\/]build[\\/]' -and ($_.Name -eq 'CMakeLists.txt' -or $_.Extension -eq '.cmake')
})
foreach ($cmakeFile in $cmakeFiles) {
    $text = Get-Content -Raw -LiteralPath $cmakeFile.FullName
    Assert-True (-not [regex]::IsMatch($text, 'add_subdirectory\s*\([^\)]*Forsetti', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) "$($cmakeFile.FullName) merges sealed Forsetti with add_subdirectory"
}

# Licensing conflict is explicit and product source does not inherit the contradictory header rule.
$frameworkLicensePath = Join-Path $frameworkRoot 'LICENSE'
$frameworkImplementationPolicyPath = Join-Path $frameworkRoot 'implementation-policy.json'
Assert-Exact (Get-FileSha256 $frameworkLicensePath) '6e3ca1bde7ac8930e70eacf814f07b767e6742268c1bd43f90756a48f6a71c9a' 'framework Apache license hash'
Assert-ContainsPattern (Get-Content -Raw -LiteralPath $frameworkLicensePath) 'Apache License' 'framework LICENSE is not Apache-2.0 evidence'
Assert-Exact ([string]$frameworkPolicy.license) 'Proprietary' 'framework proprietary metadata evidence'
Assert-ContainsPattern (Get-Content -Raw -LiteralPath $frameworkImplementationPolicyPath) 'Proprietary Licensing' 'implementation policy proprietary-header evidence missing'
foreach ($sourceFile in $consumerSources) {
    $text = Get-Content -Raw -LiteralPath $sourceFile.FullName
    Assert-True (-not [regex]::IsMatch($text, 'All Rights Reserved|Proprietary', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) "$($sourceFile.FullName) carries the contradictory upstream proprietary header"
}

Write-Host "G03 Forsetti consumer architecture validation passed: $script:AssertionCount fail-closed assertions; exact contract/profile/manifest, one app-module identity, sealed public API, dependency ADRs, licensing conflict, UP-001..UP-014, and known upstream failures verified."
