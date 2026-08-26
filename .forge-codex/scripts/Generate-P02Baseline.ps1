[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)

$ErrorActionPreference = 'Stop'

$instructionScripts = Join-Path $WorkspaceRoot '.forge-codex\instructions\scripts'
. (Join-Path $instructionScripts 'Common.ps1')

$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$roots = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-inputs\source-roots.json')
$macRoot = (Resolve-Path -LiteralPath ([string]$roots.macos)).Path
$baselineRoot = Join-Path $WorkspaceRoot '.forge-codex\state\baseline'
New-Item -ItemType Directory -Force -Path $baselineRoot | Out-Null

function ConvertTo-PortablePath {
    param([Parameter(Mandatory)][string]$Path)
    return $Path.Replace('\', '/')
}

function Get-TextLineCount {
    param([Parameter(Mandatory)][System.IO.FileInfo]$File)
    $textExtensions = @('.swift', '.md', '.json', '.jsonl', '.txt', '.xml', '.plist', '.js', '.css', '.html', '.yml', '.yaml', '.sh')
    if ($File.Extension.ToLowerInvariant() -notin $textExtensions -and $File.Name -ne 'Package.swift') { return $null }
    return (Get-Content -LiteralPath $File.FullName | Measure-Object -Line).Lines
}

function Get-FirstSwiftSymbol {
    param([Parameter(Mandatory)][string]$Path)
    $lines = @(Get-Content -LiteralPath $Path)
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*(?:(?:public|internal|private|fileprivate|open|final|static|nonisolated|@MainActor)\s+)*(?:struct|class|enum|actor|protocol|func)\s+([A-Za-z_][A-Za-z0-9_]*)') {
            return [ordered]@{ symbol = $Matches[1]; line = $index + 1 }
        }
    }
    return [ordered]@{ symbol = $null; line = 1 }
}

function New-FileAnchor {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$RelativePath,
        [Parameter(Mandatory)][string]$Origin
    )
    $portable = ConvertTo-PortablePath $RelativePath
    $native = $portable.Replace('/', '\')
    $full = Join-Path $Root $native
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
    $file = Get-Item -LiteralPath $full
    $symbol = if ($file.Extension -eq '.swift') { Get-FirstSwiftSymbol $file.FullName } else { [ordered]@{ symbol = $null; line = 1 } }
    return [ordered]@{
        origin = $Origin
        path = $portable
        symbol = $symbol.symbol
        line = $symbol.line
        bytes = $file.Length
        lines = Get-TextLineCount $file
        sha256 = Get-FileSha256 $file.FullName
    }
}

function Write-BaselineJson {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)]$Value)
    Write-JsonFileAtomic -Path (Join-Path $baselineRoot $Name) -Value $Value
}

function Get-SourceArea {
    param([Parameter(Mandatory)][string]$PortablePath)
    switch -Regex ($PortablePath) {
        '^Tests/ForgeConductorUITests/' { return 'ui_tests' }
        '^Tests/' { return 'tests' }
        '^Sources/ForgeConductorApp/' { return 'app' }
        '^Sources/ForgeConductorCLI/' { return 'cli' }
        '^Sources/ForgeNativeSessionHostPlugin/' { return 'session_host_plugin' }
        '^Sources/ForgeConductorCore/' { return 'core' }
        '^docs/' { return 'documentation' }
        '^Assets/' { return 'assets' }
        '^Package\.swift$' { return 'package' }
        default { return 'other' }
    }
}

$sourceFiles = @(
    Get-ChildItem -LiteralPath $macRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = ConvertTo-PortablePath (Get-RelativePathPortable -BasePath $macRoot -TargetPath $_.FullName)
        [pscustomobject][ordered]@{
            path = $relative
            area = Get-SourceArea $relative
            bytes = $_.Length
            lines = Get-TextLineCount $_
            sha256 = Get-FileSha256 $_.FullName
        }
    }
)
$swiftFiles = @($sourceFiles | Where-Object { $_.path.EndsWith('.swift', [StringComparison]::OrdinalIgnoreCase) })
$testSwift = @($swiftFiles | Where-Object { $_.path.StartsWith('Tests/', [StringComparison]::Ordinal) })
$productionSwift = @($swiftFiles | Where-Object { $_.path.StartsWith('Sources/', [StringComparison]::Ordinal) })
$sourceHashes = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-inputs\archives\SOURCE-HASHES.json')
$macArchive = @($sourceHashes.files | Where-Object file -eq 'Forge-Conductor-MacOS-main.zip')[0]

$sourceFileInventory = [ordered]@{
    schema_version = 1
    source_version = '0.9.0'
    archive = [ordered]@{ file = $macArchive.file; bytes = $macArchive.bytes; sha256 = $macArchive.sha256 }
    root = ConvertTo-PortablePath (Get-RelativePathPortable -BasePath $WorkspaceRoot -TargetPath $macRoot)
    counts = [ordered]@{
        all_files = $sourceFiles.Count
        swift_files = $swiftFiles.Count
        swift_lines = [int](($swiftFiles | Measure-Object -Property lines -Sum).Sum)
        production_swift_files = $productionSwift.Count
        production_swift_lines = [int](($productionSwift | Measure-Object -Property lines -Sum).Sum)
        test_swift_files = $testSwift.Count
        test_swift_lines = [int](($testSwift | Measure-Object -Property lines -Sum).Sum)
        package_swift_files = @($swiftFiles | Where-Object path -eq 'Package.swift').Count
        package_swift_lines = [int](($swiftFiles | Where-Object path -eq 'Package.swift' | Measure-Object -Property lines -Sum).Sum)
    }
    files = $sourceFiles
}
Write-BaselineJson 'p02-source-files.json' $sourceFileInventory

$featurePlan = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\feature-parity-matrix.json')
$requirementAnchorById = @{
    'PMEM-008' = '.forge-codex/instructions/specifications/PROJECT_MEMORY_TOOL_CONTRACT.md'
    'CONT-008' = '.forge-codex/instructions/architecture/CONTINUITY_AND_SESSION_HOST.md'
    'GFX-003' = '.forge-codex/instructions/architecture/TELEMETRY_AND_RENDERING.md'
    'SEC-001' = '.forge-codex/instructions/architecture/SECURITY.md'
    'SEC-003' = '.forge-codex/instructions/architecture/SECURITY.md'
    'DATA-001' = '.forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md'
    'PKG-001' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
    'PKG-002' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
    'PKG-003' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
    'PKG-004' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
    'PKG-005' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
    'PKG-006' = '.forge-codex/instructions/architecture/INSTALLER_AND_RELEASE.md'
}
$features = @(
    foreach ($feature in $featurePlan.features) {
        $evidenceText = [string]$feature.macos_evidence
        $origin = switch ($evidenceText) {
            'Owner requirement' { 'owner_requirement' }
            'Windows requirement' { 'windows_requirement' }
            'Platform replacement' { 'platform_replacement' }
            'macOS data layout docs' { 'cross_source_contract' }
            default { 'macos_source' }
        }
        $anchors = @()
        if ($evidenceText.StartsWith('Sources/') -or $evidenceText.StartsWith('Tests/')) {
            $candidate = Join-Path $macRoot $evidenceText.Replace('/', '\')
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $anchors += New-FileAnchor -Root $macRoot -RelativePath $evidenceText -Origin 'macos_source'
            } elseif (Test-Path -LiteralPath $candidate -PathType Container) {
                $anchors += @(Get-ChildItem -LiteralPath $candidate -Recurse -File -Filter *.swift | Sort-Object FullName | ForEach-Object {
                    $rel = Get-RelativePathPortable -BasePath $macRoot -TargetPath $_.FullName
                    New-FileAnchor -Root $macRoot -RelativePath $rel -Origin 'macos_source'
                })
            }
        }
        if ($feature.id -eq 'DATA-001') {
            foreach ($path in @(
                'Sources/ForgeConductorCore/Infrastructure/AppPaths.swift',
                'Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift',
                'Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift',
                'README.md',
                'docs/ARCHITECTURE.md'
            )) { $anchors += New-FileAnchor -Root $macRoot -RelativePath $path -Origin 'macos_source' }
        }
        if ($requirementAnchorById.ContainsKey([string]$feature.id)) {
            $anchors += New-FileAnchor -Root $WorkspaceRoot -RelativePath $requirementAnchorById[[string]$feature.id] -Origin $origin
        }
        [ordered]@{
            id = $feature.id
            category = $feature.category
            feature = $feature.feature
            behavior = $feature.behavior
            original_evidence = $feature.macos_evidence
            evidence_origin = $origin
            source_anchors = @($anchors | Where-Object { $null -ne $_ })
            windows_acceptance = $feature.windows_acceptance
            windows_status = $feature.status
            baseline_drift = if ($feature.id -in @('PMEM-008','CONT-008','GFX-003','SEC-001','SEC-003','DATA-001','PKG-001','PKG-002','PKG-003','PKG-004','PKG-005','PKG-006')) { 'Target requirement; not a macOS implementation claim.' } else { $null }
        }
    }
)
Write-BaselineJson 'p02-feature-inventory.json' ([ordered]@{
    schema_version = 1
    source_version = '0.9.0'
    retained_feature_count = $features.Count
    original_plan_sha256 = Get-FileSha256 (Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\feature-parity-matrix.json')
    rule = 'All original feature IDs are retained. Target-only requirements identify their provenance explicitly.'
    features = $features
})

$uiAnchor = New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorApp/AppModel.swift' -Origin 'macos_source'
$contentAnchor = New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorApp/Views/ContentView.swift' -Origin 'macos_source'
$uiInventory = [ordered]@{
    schema_version = 1
    navigation = @(
        [ordered]@{ id='forge_rig'; label='FORGE RIG'; view='RigDashboardView'; automation_id='nav.forge-rig' },
        [ordered]@{ id='lm_studio_mcp'; label='LM Studio MCP'; view='MCPServersView'; automation_id='nav.lm-studio-mcp' },
        [ordered]@{ id='agents'; label='Agents'; view='AgentsView'; automation_id='nav.agents' },
        [ordered]@{ id='tools'; label='Tools'; view='ToolsView'; automation_id='nav.tools' },
        [ordered]@{ id='live_feed'; label='Live Feed'; view='LiveFeedView'; automation_id='nav.live-feed' },
        [ordered]@{ id='diagnostics'; label='Diagnostics'; view='DiagnosticsView'; automation_id='nav.diagnostics' },
        [ordered]@{ id='manager'; label='Manager'; view='ManagerSettingsView'; automation_id='nav.manager' }
    )
    settings_scene = [ordered]@{ view='ManagerSettingsView'; source='Sources/ForgeConductorApp/ForgeConductorApp.swift' }
    rig_panels = @('sys_strip','load_trace','cpu_cores','gpu_cores','storage','orchestration','mcp_servers','mcp_tools','sub_agents','hot_processes','live_stream')
    unreachable_source = @([ordered]@{ view='TelemetryDashboardView'; reason='No source reference outside its defining file.' })
    anchors = @($uiAnchor, $contentAnchor)
}
Write-BaselineJson 'p02-ui-navigation-inventory.json' $uiInventory

$cliAnchor = New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCLI/ForgeConductorMain.swift' -Origin 'macos_source'
$entryAnchor = New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Application/ForgeProcessEntry.swift' -Origin 'macos_source'
$cliInventory = [ordered]@{
    schema_version = 1
    process_modes = @(
        [ordered]@{ mode='gui'; aliases=@(); behavior='No recognized process mode.' },
        [ordered]@{ mode='stdio_mcp'; aliases=@('serve','mcp-serve','mcp'); behavior='Run compact newline JSON-RPC server.' },
        [ordered]@{ mode='manager_foreground'; aliases=@('manager run','manager','manager flags'); behavior='Run manager in foreground.' }
    )
    top_level_commands = @('help','-h','--help','version','--version','install','install-lmstudio-plugin','doctor','status','serve','dashboard','manager','agents')
    manager_subcommands = @('run','start','stop','restart','status','install-login','uninstall-login','cleanup-stale','allowlist','help','-h','--help')
    options = [ordered]@{
        global=@('--home'); install=@('--from'); install_lmstudio_plugin=@('--binary'); dashboard=@('--host','--port','--open'); manager=@('--open','--home','--keep-stale')
    }
    exit_codes = [ordered]@{ success=0; failure=1; invalid_invocation=2 }
    anchors = @($cliAnchor, $entryAnchor)
}
Write-BaselineJson 'p02-cli-process-inventory.json' $cliInventory

$mcpPlanPath = Join-Path $WorkspaceRoot '.forge-codex\instructions\plans\mcp-tool-parity.json'
$mcpPlan = Read-JsonFile $mcpPlanPath
$advertisedNames = @($mcpPlan.tools.name | Sort-Object)
$permissiveSchemas = @('forge_status','agent_list','fs_edit','fs_glob','fs_move','git_status','git_diff','git_log','git_add','git_commit')
$mcpTools = @(
    foreach ($tool in $mcpPlan.tools) {
        $sourceAnchor = New-FileAnchor -Root $macRoot -RelativePath ([string]$tool.macos_source) -Origin 'macos_source'
        $sourceText = Get-Content -LiteralPath (Join-Path $macRoot ([string]$tool.macos_source).Replace('/', '\')) -Raw
        $operation = if ($tool.name -in @('agent_run_status','project_memory.export')) { 'write' } else { [string]$tool.operation }
        $drift = @()
        if ($tool.name -eq 'agent_run_status') { $drift += 'Plan said read; source can rehydrate/reattach and is mutating.' }
        if ($tool.name -eq 'project_memory.export') { $drift += 'Plan said read; source writes an export artifact and is mutating.' }
        if ($tool.name -eq 'memory_set') { $drift += 'Advertised schema requires body; handler also accepts content or value.' }
        if ($tool.name -eq 'project_memory.initialize') { $drift += 'Handler accepts path alias; advertised schema declares project_path only.' }
        [ordered]@{
            name = $tool.name
            pack = $tool.pack
            plan_index = [int]$tool.index
            advertised_index = [Array]::IndexOf($advertisedNames, [string]$tool.name) + 1
            operation = $operation
            source_declared = $sourceText.Contains([string]$tool.name)
            source_anchor = $sourceAnchor
            schema_class = if ($tool.name -in $permissiveSchemas) { 'legacy_permissive_object' } else { 'explicit_or_pack_defined' }
            known_schema_drift = $drift
            windows_status = $tool.windows_status
        }
    }
)
$mcpProtocolAnchors = @(
    New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/MCP/MCPServer.swift' -Origin 'macos_source'
    New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Application/ToolRouter.swift' -Origin 'macos_source'
)
Write-BaselineJson 'p02-mcp-semantic-inventory.json' ([ordered]@{
    schema_version = 1
    source_version = '0.9.0'
    expected_tool_count = 53
    plan_sha256 = Get-FileSha256 $mcpPlanPath
    operation_counts = [ordered]@{ status=1; read=22; write=30 }
    protocol = [ordered]@{
        input_framing=@('newline_json','content_length'); output_framing='compact_newline_json'
        versions=@('2025-11-25','2025-06-18','2025-03-26','2024-11-05')
        methods=@('initialize','ping','tools/list','tools/call','resources/list','prompts/list')
        empty_collections=@('resources','prompts'); cancellation_id_capacity=256
        error_codes=@(-32600,-32601,-32800,-32000)
    }
    bounds = [ordered]@{
        text_file_bytes=2097152; directory_entries=1000; glob_results=500; search_results=200
        shell_timeout_seconds=120; shell_capture_bytes=100000; shell_stdout_bytes=80000; shell_stderr_bytes=20000
        legacy_memory_key_bytes=512; legacy_memory_body_bytes=524288; legacy_memory_query_default=50; legacy_memory_query_max=200
        project_title_bytes=512; project_summary_bytes=4096; project_body_bytes=262144; project_source_reference_bytes=2048
        project_tag_count=32; project_tag_bytes=128; project_batch_count=50; project_batch_bytes=1048576
        project_query_bytes=4096; project_page_default=20; project_page_max=100; project_response_default_bytes=65536
        project_response_max_bytes=262144; open_project_lru=8; project_deadline_ms_max=60000
    }
    stable_project_errors = @('invalid_request','unsupported_version','project_not_found','project_scope_mismatch','record_not_found','conflict','payload_too_large','database_busy','storage_full','deadline_exceeded','cancelled','migration_failed','integrity_failure','redaction_rejected')
    anchors = $mcpProtocolAnchors
    tools = $mcpTools
})

$schemaInventory = [ordered]@{
    schema_version = 1
    schemas = @(
        [ordered]@{ id='central_sqlite'; version=5; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift' -Origin 'macos_source'); tables=@('schema_version','memory_notes','agent_sessions','presence','audit_events','context_handoffs') },
        [ordered]@{ id='project_sqlite'; version=1; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift' -Origin 'macos_source'); tables=@('memory_records','memory_tags','memory_record_tags','memory_links','sessions','handoffs','artifacts','project_aliases','maintenance_state','event_journal','continuity_handoffs','rollover_operations','rollover_transitions','project_active_sessions'); optional_virtual_tables=@('memory_records_fts') },
        [ordered]@{ id='project_registry'; version=1; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Application/ProjectMemoryService.swift' -Origin 'macos_source'); fields=@('id','displayName','repositoryIdentity','aliases','createdAt','updatedAt') },
        [ordered]@{ id='legacy_handoff_packet'; version=1; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Domain/HandoffPacket.swift' -Origin 'macos_source'); max_agent_snapshots=128 },
        [ordered]@{ id='continuity_handoff'; version='1.0'; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Domain/ContinuityModels.swift' -Origin 'macos_source'); encoded_max_bytes=131072; list_cap=128 },
        [ordered]@{ id='native_session_ledger'; version=1; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift' -Origin 'macos_source'); record_cap=4096 },
        [ordered]@{ id='app_config'; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Domain/AppConfig.swift' -Origin 'macos_source') },
        [ordered]@{ id='lm_studio_mcp_config'; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Telemetry/LMStudioMCPPluginInstaller.swift' -Origin 'macos_source') },
        [ordered]@{ id='mcp_json_rpc'; source_anchor=(New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/MCP/MCPServer.swift' -Origin 'macos_source') }
    )
}
Write-BaselineJson 'p02-schema-inventory.json' $schemaInventory

$persistenceInventory = [ordered]@{
    schema_version = 1
    home_layout_anchor = New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Infrastructure/AppPaths.swift' -Origin 'macos_source'
    central_store = [ordered]@{
        file='store.sqlite'; schema_version=5; journal_mode='WAL'; foreign_keys=$true; busy_timeout_ms=3000
        backup='none'; integrity='none'
    }
    project_store = [ordered]@{
        pattern='Projects/<uuid>/memory.sqlite3'; user_version=1; journal_mode='WAL'; foreign_keys=$true
        synchronous='NORMAL'; busy_timeout_ms=3000; lru_capacity=8; backup='pre-migration'
        backup_method='raw-file-copy'; backup_scope='main-database-file-only'; backup_wal_safe=$false
        integrity='quick_check'
    }
    durable_files = @('config.json','audit.jsonl','Projects/registry.json','Projects/<uuid>/project.json','native-session-ledger.json','mcp.json','manifest.json','mcp-bridge-config.json','install-state.json')
    import_export = [ordered]@{ checksummed=$true; max_import_bytes=33554432; transactional=$true }
    default_config = [ordered]@{
        log_level='info'; allowed_roots=@(); shell_enabled=$false; shell_timeout_seconds=30
        dashboard_host='127.0.0.1'; dashboard_port=7788; dashboard_refresh_seconds=8
        manager_auto_restart=$true; manager_watchdog_seconds=3; manager_open_browser=$false
        mcp_role='primary'; session_idle_ttl_seconds=14400; coordinator_enabled=$true; coordinator_lease_seconds=60; coordinator_presence_seconds=30
    }
    anchors = @(
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift' -Origin 'macos_source'
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift' -Origin 'macos_source'
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Domain/AppConfig.swift' -Origin 'macos_source'
    )
}
Write-BaselineJson 'p02-persistence-data-inventory.json' $persistenceInventory

$telemetryInventory = [ordered]@{
    schema_version = 1
    top_level_fields = @('system','forge','updated','history','runtime')
    required_contract_fields = @('system','forge','updated','history')
    system_fields = @('ts','host','platform','arch','cpu','ram','disk','disk_io','gpu','processes','power')
    forge_fields = @('ts','home','runtime','presence_count','presence','mcp_servers','mcp_tools','mcp_packs','agents','agent_sessions','agents_summary','jobs','live_feed','feed_stats','orchestration','mcp_load','files','audit_recent')
    known_contract_drift = @(
        'Fixture omits power although the implementation emits it.',
        'Fixture includes diagnostics although ForgeSnapshot.asDictionary does not emit it.'
    )
    windows_semantics = 'Unavailable and stale values must be explicit; placeholder zero values are forbidden.'
    placeholder_values_not_to_port = @('ram.swap_total=0','ram.swap_used=0','disk cumulative byte/op totals=0','gpu.shared_memory=true','gpu.processes=[]')
    dashboard_routes = [ordered]@{
        telemetry=@('/static/*','/api/health','/api/live','/api/frame','/api/snapshot','/api/system','/api/forge','/ping','/api/stream?hz=')
        operational_get=@('/api/doctor','/api/agents','/api/sessions','/api/audit','/api/diagnostics')
        operational_write=@('/api/sessions/prune','/api/sessions/close')
        manager_get=@('/api/manager/status','/api/manager/settings')
        manager_write=@('/api/manager/start','/api/manager/stop','/api/manager/restart','/api/manager/shutdown','/api/manager/settings')
        shell=@('/','/index.html','/control','/manager','/api/status')
    }
    anchors = @(
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift' -Origin 'macos_source'
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift' -Origin 'macos_source'
        New-FileAnchor -Root $macRoot -RelativePath 'Sources/ForgeConductorCore/Dashboard/DashboardServer.swift' -Origin 'macos_source'
    )
}
Write-BaselineJson 'p02-telemetry-dashboard-inventory.json' $telemetryInventory

$agentRoot = Join-Path $macRoot 'Sources\ForgeConductorCore\Resources\Agents'
$agents = @(
    Get-ChildItem -LiteralPath $agentRoot -File -Filter *.md | Sort-Object BaseName | ForEach-Object {
        $relative = ConvertTo-PortablePath (Get-RelativePathPortable -BasePath $macRoot -TargetPath $_.FullName)
        $text = Get-Content -LiteralPath $_.FullName -Raw
        $invalidAllowed = @('python_exec','python_info','search_files') | Where-Object { $text -match [regex]::Escape($_) }
        $invalidForbidden = @('git_push','gh_pr_create') | Where-Object { $text -match [regex]::Escape($_) }
        [ordered]@{
            id = $_.BaseName
            source_anchor = New-FileAnchor -Root $macRoot -RelativePath $relative -Origin 'macos_source'
            invalid_allowed_tools = @($invalidAllowed)
            unavailable_forbidden_tools = @($invalidForbidden)
            windows_normalization = 'Remove unavailable and prohibited tool references while preserving the agent policy intent.'
        }
    }
)
Write-BaselineJson 'p02-agent-inventory.json' ([ordered]@{ schema_version=1; expected_count=10; agents=$agents })

$testFiles = @()
$unitMethodCount = 0
$uiMethodCount = 0
Get-ChildItem -LiteralPath (Join-Path $macRoot 'Tests') -Recurse -File | Sort-Object FullName | ForEach-Object {
    if ($_.Extension -notin @('.swift','.json')) { return }
    $relative = ConvertTo-PortablePath (Get-RelativePathPortable -BasePath $macRoot -TargetPath $_.FullName)
    $methods = @()
    if ($_.Extension -eq '.swift') {
        $lines = @(Get-Content -LiteralPath $_.FullName)
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -match '^\s*(?:(?:@MainActor|async|throws|static)\s+)*func\s+(test[A-Za-z0-9_]+)') {
                $methods += [ordered]@{ symbol=$Matches[1]; line=$index + 1 }
            }
        }
        if ($relative.StartsWith('Tests/ForgeConductorUITests/', [StringComparison]::Ordinal)) { $uiMethodCount += $methods.Count } else { $unitMethodCount += $methods.Count }
    }
    $testFiles += [ordered]@{
        path=$relative; kind=if ($_.Extension -eq '.json') {'fixture'} elseif ($relative.Contains('UITests')) {'ui_test'} else {'unit_integration_test'}
        bytes=$_.Length; lines=Get-TextLineCount $_; sha256=Get-FileSha256 $_.FullName; methods=$methods
    }
}
Write-BaselineJson 'p02-test-fixture-inventory.json' ([ordered]@{
    schema_version=1
    counts=[ordered]@{ files=$testFiles.Count; unit_integration_methods=$unitMethodCount; ui_methods=$uiMethodCount; total_methods=$unitMethodCount + $uiMethodCount }
    evidence_rule='Source declarations are inventory evidence, not proof of runtime execution. Supplied P12 runtime evidence remains separately attributable.'
    files=$testFiles
})

$report = @"
# P02 Source Archaeology Baseline

The canonical immutable input is Forge Conductor macOS 0.9.0 ($($macArchive.sha256)). It contains $($sourceFiles.Count) files, $($swiftFiles.Count) Swift files, and $([int](($swiftFiles | Measure-Object -Property lines -Sum).Sum)) Swift lines.

The supplied audit and several pre-bootstrap baseline files describe an older 0.8.0/34-tool snapshot. They are retained as historical anti-regression evidence and are not used as the Windows parity contract. The current source declares 53 MCP tools, 84 planned feature rows, 269 unit/integration test methods, and 5 UI test methods.

Key reconciliations:

- The router advertises tool names in sorted order; plan order is retained separately.
- 'agent_run_status' and 'project_memory.export' are mutating in source.
- Target-only reset, Windows rendering/security, data migration, and packaging rows identify their requirement origin instead of claiming macOS source evidence.
- Telemetry must expose availability and staleness. Historical placeholder zero values are explicitly excluded.
- 'TelemetryDashboardView' is source-declared but unreachable from the current navigation composition.
- Embedded agent manifests contain unavailable or prohibited tool references; Windows normalization must preserve policy intent without those references.

The JSON inventories beside this report are the machine-readable G02 baseline. 'Test-P02Baseline.ps1' re-hashes immutable sources and checks all structural counts and anchors.
"@
$reportPath = Join-Path $baselineRoot 'p02-archaeology-report.md'
[System.IO.File]::WriteAllText($reportPath, $report, [System.Text.UTF8Encoding]::new($false))

Write-Host "P02 baseline generated at $baselineRoot"
