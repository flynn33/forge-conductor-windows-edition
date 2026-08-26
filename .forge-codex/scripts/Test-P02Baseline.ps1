[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)

$ErrorActionPreference = 'Stop'

$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts\Common.ps1')

$roots = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-inputs\source-roots.json')
$macRoot = (Resolve-Path -LiteralPath ([string]$roots.macos)).Path
$baselineRoot = Join-Path $WorkspaceRoot '.forge-codex\state\baseline'

function Assert-True {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Message)
    if (-not $Condition) { throw "P02 assertion failed: $Message" }
}

function Read-Baseline {
    param([Parameter(Mandatory)][string]$Name)
    $path = Join-Path $baselineRoot $Name
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Missing baseline artifact $Name"
    return Read-JsonFile $path
}

function Get-AnchorFullPath {
    param([Parameter(Mandatory)]$Anchor)
    if ([string]$Anchor.origin -eq 'macos_source') {
        return Join-Path $macRoot ([string]$Anchor.path).Replace('/', '\')
    }
    return Join-Path $WorkspaceRoot ([string]$Anchor.path).Replace('/', '\')
}

function Assert-Anchor {
    param([Parameter(Mandatory)]$Anchor, [Parameter(Mandatory)][string]$Context)
    Assert-True ($null -ne $Anchor) "$Context has a null anchor"
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Anchor.path)) "$Context has an empty anchor path"
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Anchor.sha256)) "$Context has an empty anchor hash"
    $path = Get-AnchorFullPath $Anchor
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "$Context anchor is missing: $($Anchor.path)"
    Assert-True ((Get-FileSha256 $path) -eq [string]$Anchor.sha256) "$Context anchor hash changed: $($Anchor.path)"
}

$requiredJson = @(
    'p02-source-files.json',
    'p02-feature-inventory.json',
    'p02-ui-navigation-inventory.json',
    'p02-cli-process-inventory.json',
    'p02-mcp-semantic-inventory.json',
    'p02-schema-inventory.json',
    'p02-persistence-data-inventory.json',
    'p02-telemetry-dashboard-inventory.json',
    'p02-agent-inventory.json',
    'p02-test-fixture-inventory.json'
)
foreach ($name in $requiredJson) { Assert-True (Test-Path -LiteralPath (Join-Path $baselineRoot $name) -PathType Leaf) "Missing $name" }
Assert-True (Test-Path -LiteralPath (Join-Path $baselineRoot 'p02-archaeology-report.md') -PathType Leaf) 'Missing p02-archaeology-report.md'

$hashManifestPath = Join-Path $WorkspaceRoot '.forge-inputs\archives\SOURCE-HASHES.json'
$hashManifest = Read-JsonFile $hashManifestPath
foreach ($entry in $hashManifest.files) {
    $archivePath = Join-Path $WorkspaceRoot ('.forge-inputs\archives\' + [string]$entry.file)
    Assert-True (Test-Path -LiteralPath $archivePath -PathType Leaf) "Immutable archive missing: $($entry.file)"
    Assert-True ((Get-Item -LiteralPath $archivePath).Length -eq [long]$entry.bytes) "Immutable archive byte count changed: $($entry.file)"
    Assert-True ((Get-FileSha256 $archivePath) -eq [string]$entry.sha256) "Immutable archive hash changed: $($entry.file)"
}

$sources = Read-Baseline 'p02-source-files.json'
Assert-True ([int]$sources.counts.all_files -eq 870) 'Expected 870 extracted source files'
Assert-True ([int]$sources.counts.swift_files -eq 133) 'Expected 133 Swift files'
Assert-True ([int]$sources.counts.swift_lines -eq 33763) 'Expected 33,763 Swift lines'
Assert-True ([int]$sources.counts.production_swift_files -eq 102) 'Expected 102 production Swift files'
Assert-True ([int]$sources.counts.production_swift_lines -eq 25244) 'Expected 25,244 production Swift lines'
Assert-True ([int]$sources.counts.test_swift_files -eq 30) 'Expected 30 test Swift files'
Assert-True ([int]$sources.counts.test_swift_lines -eq 8468) 'Expected 8,468 test Swift lines'
Assert-True ([int]$sources.counts.package_swift_files -eq 1) 'Expected Package.swift'
Assert-True ([int]$sources.counts.package_swift_lines -eq 51) 'Expected 51 Package.swift lines'
Assert-True (@($sources.files).Count -eq 870) 'Source file list count mismatch'
foreach ($source in $sources.files) {
    $path = Join-Path $macRoot ([string]$source.path).Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Indexed source missing: $($source.path)"
    Assert-True ((Get-FileSha256 $path) -eq [string]$source.sha256) "Indexed source hash changed: $($source.path)"
}

$features = Read-Baseline 'p02-feature-inventory.json'
$featurePlan = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\feature-parity-matrix.json')
$expectedFeatureIds = @($featurePlan.features.id | Sort-Object)
$actualFeatureIds = @($features.features.id | Sort-Object)
Assert-True ($actualFeatureIds.Count -eq 84) 'Expected 84 retained feature rows'
Assert-True (($actualFeatureIds -join "`n") -ceq ($expectedFeatureIds -join "`n")) 'Feature IDs differ from the supplied plan'
Assert-True (@($actualFeatureIds | Group-Object | Where-Object Count -ne 1).Count -eq 0) 'Feature IDs are not unique'
foreach ($feature in $features.features) {
    Assert-True (@($feature.source_anchors).Count -gt 0) "Feature $($feature.id) has no source or requirement anchor"
    foreach ($anchor in $feature.source_anchors) { Assert-Anchor $anchor "Feature $($feature.id)" }
}

$ui = Read-Baseline 'p02-ui-navigation-inventory.json'
Assert-True (@($ui.navigation).Count -eq 7) 'Expected seven navigation surfaces'
Assert-True (@($ui.rig_panels).Count -eq 11) 'Expected eleven Rig panel IDs'
foreach ($anchor in $ui.anchors) { Assert-Anchor $anchor 'UI inventory' }

$cli = Read-Baseline 'p02-cli-process-inventory.json'
Assert-True (@($cli.process_modes).Count -eq 3) 'Expected three process-mode classes'
Assert-True (@($cli.top_level_commands).Count -eq 13) 'Expected thirteen top-level command tokens/aliases'
Assert-True (@($cli.manager_subcommands).Count -eq 12) 'Expected twelve manager subcommand tokens/aliases'
foreach ($anchor in $cli.anchors) { Assert-Anchor $anchor 'CLI inventory' }

$mcp = Read-Baseline 'p02-mcp-semantic-inventory.json'
$mcpPlan = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\mcp-tool-parity.json')
$expectedTools = @($mcpPlan.tools.name | Sort-Object)
$actualTools = @($mcp.tools.name | Sort-Object)
Assert-True ($actualTools.Count -eq 53) 'Expected exactly 53 MCP tools'
Assert-True (($actualTools -join "`n") -ceq ($expectedTools -join "`n")) 'MCP tool set differs from the supplied plan'
Assert-True (@($mcp.tools | Where-Object { -not $_.source_declared }).Count -eq 0) 'One or more MCP names were not found in declared source files'
Assert-True (@($mcp.tools | Group-Object plan_index | Where-Object Count -ne 1).Count -eq 0) 'MCP plan indexes are not unique'
Assert-True (@($mcp.tools | Group-Object advertised_index | Where-Object Count -ne 1).Count -eq 0) 'MCP advertised indexes are not unique'
Assert-True (@($mcp.tools | Where-Object operation -eq 'status').Count -eq 1) 'Expected one status MCP operation'
Assert-True (@($mcp.tools | Where-Object operation -eq 'read').Count -eq 22) 'Expected 22 read MCP operations'
Assert-True (@($mcp.tools | Where-Object operation -eq 'write').Count -eq 30) 'Expected 30 mutating MCP operations'
foreach ($tool in $mcp.tools) { Assert-Anchor $tool.source_anchor "MCP tool $($tool.name)" }
foreach ($anchor in $mcp.anchors) { Assert-Anchor $anchor 'MCP protocol' }

$schemas = Read-Baseline 'p02-schema-inventory.json'
Assert-True (@($schemas.schemas).Count -eq 9) 'Expected nine schema families'
foreach ($schema in $schemas.schemas) { Assert-Anchor $schema.source_anchor "Schema $($schema.id)" }

$persistence = Read-Baseline 'p02-persistence-data-inventory.json'
Assert-True ([int]$persistence.central_store.schema_version -eq 5) 'Expected central schema version 5'
Assert-True ([int]$persistence.project_store.user_version -eq 1) 'Expected per-project user_version 1'
Assert-Anchor $persistence.home_layout_anchor 'Persistence home layout'
foreach ($anchor in $persistence.anchors) { Assert-Anchor $anchor 'Persistence inventory' }

$telemetry = Read-Baseline 'p02-telemetry-dashboard-inventory.json'
Assert-True (@($telemetry.system_fields).Count -eq 11) 'Expected eleven system telemetry keys including power'
Assert-True (@($telemetry.forge_fields).Count -eq 18) 'Expected eighteen source-emitted Forge telemetry keys'
foreach ($anchor in $telemetry.anchors) { Assert-Anchor $anchor 'Telemetry inventory' }

$agents = Read-Baseline 'p02-agent-inventory.json'
Assert-True (@($agents.agents).Count -eq 10) 'Expected ten embedded agent manifests'
foreach ($agent in $agents.agents) { Assert-Anchor $agent.source_anchor "Agent $($agent.id)" }

$tests = Read-Baseline 'p02-test-fixture-inventory.json'
Assert-True ([int]$tests.counts.files -eq 31) 'Expected 31 test/fixture files'
Assert-True ([int]$tests.counts.unit_integration_methods -eq 269) 'Expected 269 unit/integration methods'
Assert-True ([int]$tests.counts.ui_methods -eq 5) 'Expected five UI methods'
Assert-True ([int]$tests.counts.total_methods -eq 274) 'Expected 274 total XCTest methods'
foreach ($test in $tests.files) {
    $path = Join-Path $macRoot ([string]$test.path).Replace('/', '\')
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Test inventory path missing: $($test.path)"
    Assert-True ((Get-FileSha256 $path) -eq [string]$test.sha256) "Test inventory hash changed: $($test.path)"
}

Write-Host 'P02 baseline validation passed: immutable sources, 84 features, 53 MCP tools, seven UI surfaces, schemas, persistence, telemetry, ten agents, and 274 test methods are source-backed.'
