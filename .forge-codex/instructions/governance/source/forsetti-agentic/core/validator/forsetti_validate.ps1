# Forsetti Agentic Edition local validator

[CmdletBinding()]
param(
    [string]$RepoRoot = (Get-Location).Path,
    [ValidateSet("all", "repo", "files", "structure", "json", "policies", "policy", "docs", "schemas", "schema", "scripts", "contract", "contracts", "project-context", "edition-profile", "manifest", "dependencies", "capabilities", "module-isolation", "evidence")]
    [string]$Mode = "all",
    [string]$ContractPath,
    [string]$ProjectContextPath,
    [string]$EditionProfilePath,
    [Alias("ChangedFiles")]
    [string[]]$ChangedFile,
    [string]$ChangedFilesPath,
    [string]$ManifestPath,
    [string]$OutputJson,
    [switch]$Strict
)

$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date
$script:Findings = New-Object System.Collections.Generic.List[object]

function Resolve-ValidationRoot {
    param([string]$Path)

    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved)) {
        throw "Validation root does not exist: $Path"
    }
    return $resolved
}

function Resolve-InputPath {
    param([AllowNull()][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $Path))
}

function Get-RepoPath {
    param([AllowNull()][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    try {
        $full = [System.IO.Path]::GetFullPath($Path)
        $root = $script:RepoRoot.TrimEnd('\', '/')
        if ($full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $full.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
        }
    } catch {
        return $Path.Replace('\', '/')
    }
    return $Path.Replace('\', '/')
}

function Add-Finding {
    param(
        [string]$RuleId,
        [ValidateSet("info", "low", "medium", "high", "critical")]
        [string]$Severity,
        [ValidateSet("pass", "request_changes", "block")]
        [string]$Decision,
        [string]$Message,
        [AllowNull()]$Evidence,
        [AllowNull()][string]$Remediation,
        [AllowNull()][string]$Category,
        [AllowNull()][string]$Path
    )

    $evidenceArray = @()
    if ($null -ne $Evidence) {
        foreach ($item in @($Evidence)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$item)) {
                $evidenceArray += [string]$item
            }
        }
    }

    $legacyStatus = "pass"
    if ($Decision -eq "request_changes") {
        $legacyStatus = "warn"
    } elseif ($Decision -eq "block") {
        $legacyStatus = "fail"
    }

    $script:Findings.Add([pscustomobject][ordered]@{
        rule_id          = $RuleId
        severity         = $Severity
        decision         = $Decision
        status           = $legacyStatus
        message          = $Message
        evidence         = $evidenceArray
        remediation      = if ([string]::IsNullOrWhiteSpace($Remediation)) { $null } else { $Remediation }
        remediation_hint = if ([string]::IsNullOrWhiteSpace($Remediation)) { $null } else { $Remediation }
        category         = if ([string]::IsNullOrWhiteSpace($Category)) { $null } else { $Category }
        path             = if ([string]::IsNullOrWhiteSpace($Path)) { $null } else { $Path }
    })
}

function Read-JsonObject {
    param([string]$Path)
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-JsonFiles {
    $excluded = @(".git", "node_modules")
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($script:RepoRoot)
    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        foreach ($file in Get-ChildItem -LiteralPath $current -Filter "*.json" -File -ErrorAction SilentlyContinue) {
            $file
        }
        foreach ($directory in Get-ChildItem -LiteralPath $current -Directory -ErrorAction SilentlyContinue) {
            if ($excluded -notcontains $directory.Name) {
                $stack.Push($directory.FullName)
            }
        }
    }
}

function Test-JsonFiles {
    $errors = 0
    $count = 0
    foreach ($file in Get-JsonFiles) {
        $count++
        try {
            $null = Read-JsonObject -Path $file.FullName
        } catch {
            $errors++
            Add-Finding -RuleId "FAE-C011" -Severity "critical" -Decision "block" -Category "json" -Path (Get-RepoPath $file.FullName) -Message "Invalid JSON file." -Evidence $_.Exception.Message -Remediation "Fix JSON syntax before proceeding."
        }
    }
    if ($errors -eq 0) {
        Add-Finding -RuleId "FAE-C011" -Severity "info" -Decision "pass" -Category "json" -Message "Repository JSON files parse cleanly." -Evidence ("Parsed " + $count + " JSON files.") -Remediation $null
    }
}

function Assert-PathExists {
    param([string]$RelativePath, [string]$RuleId)
    if (-not (Test-Path -LiteralPath (Join-Path $script:RepoRoot $RelativePath))) {
        Add-Finding -RuleId $RuleId -Severity "critical" -Decision "block" -Category "repo" -Path $RelativePath -Message "Required repository path is missing." -Evidence $RelativePath -Remediation "Restore or create the required path."
        return $false
    }
    return $true
}

function Test-RequiredRepositorySurface {
    $required = @(
        "README.md",
        "FORSETTI_CONSTITUTION.md",
        "AGENTS.md",
        "COMPLIANCE_POLICY.md",
        "ACCOUNTABILITY_POLICY.md",
        "core/README.md",
        "core/AGENTS.md",
        "core/enforcement/authority-model.md",
        "core/contracts/forsetti-project-context-template.json",
        "core/contracts/task-contract-template.json",
        "core/schemas/forsetti-project-context.schema.json",
        "core/schemas/edition-profile.schema.json",
        "core/schemas/module-manifest-1.1.schema.json",
        "core/schemas/task-contract.schema.json",
        "core/schemas/validator-result.schema.json",
        "core/policies/forsetti-enforcement-rules.json",
        "core/policies/manifest-rules.json",
        "core/policies/runtime-requirement-rules.json",
        "core/policies/module-isolation-rules.json",
        "core/policies/dependency-boundary-rules.json",
        "core/policies/public-api-rules.json",
        "core/policies/capability-rules.json",
        "core/policies/ui-contribution-rules.json",
        "core/policies/service-access-rules.json",
        "core/policies/mcp-provider-policy.json",
        "core/policies/mcp-resolution-order.json",
        "core/policies/accountability-rules.json",
        "core/policies/agent-enforcement-actions.json",
        "core/validator/forsetti_validate.ps1",
        "core/validator/rules/forsetti_project_rules.ps1",
        "editions/shared/shared-forsetti-invariants.json",
        "editions/apple/forsetti-apple-0.1.5.profile.json",
        "editions/apple/forsetti-apple-0.1.3.profile.json",
        "editions/windows/forsetti-windows-0.2.0.profile.json",
        "editions/README.md",
        "overlays/generic/README.md",
        "overlays/forsetti-apple/README.md",
        "overlays/forsetti-windows/README.md",
        "standards/mcp-local-helper-standard.md",
        "scripts/validate-repo.ps1",
        "scripts/validate-repo.sh"
    )

    $missing = 0
    foreach ($path in $required) {
        if (-not (Assert-PathExists -RelativePath $path -RuleId "FAE-C011")) {
            $missing++
        }
    }
    if ($missing -eq 0) {
        Add-Finding -RuleId "FAE-C011" -Severity "info" -Decision "pass" -Category "repo" -Message "Required repository surface exists." -Evidence ("Checked " + $required.Count + " paths.") -Remediation $null
    }
}

function Test-PolicyMirrors {
    $pairs = @(
        "forsetti-enforcement-rules.json",
        "manifest-rules.json",
        "runtime-requirement-rules.json",
        "module-isolation-rules.json",
        "dependency-boundary-rules.json",
        "public-api-rules.json",
        "capability-rules.json",
        "ui-contribution-rules.json",
        "service-access-rules.json",
        "mcp-provider-policy.json",
        "mcp-resolution-order.json",
        "accountability-rules.json",
        "agent-enforcement-actions.json"
    )

    $errors = 0
    foreach ($file in $pairs) {
        $corePath = Join-Path $script:RepoRoot ("core/policies/" + $file)
        $rootPath = Join-Path $script:RepoRoot ("policies/" + $file)
        if (-not (Test-Path -LiteralPath $corePath) -or -not (Test-Path -LiteralPath $rootPath)) {
            $errors++
            Add-Finding -RuleId "FAE-C011" -Severity "critical" -Decision "block" -Category "policies" -Path $file -Message "Policy mirror pair is missing." -Evidence $file -Remediation "Create both core and root policy mirror files."
            continue
        }
        $coreText = Get-Content -LiteralPath $corePath -Raw
        $rootText = Get-Content -LiteralPath $rootPath -Raw
        if ($coreText -ne $rootText) {
            $errors++
            Add-Finding -RuleId "FAE-C011" -Severity "high" -Decision "request_changes" -Category "policies" -Path $file -Message "Core policy and root mirror differ." -Evidence $file -Remediation "Synchronize the root mirror with the canonical core policy."
        }
    }
    if ($errors -eq 0) {
        Add-Finding -RuleId "FAE-C011" -Severity "info" -Decision "pass" -Category "policies" -Message "Forsetti policy mirrors are synchronized." -Evidence ("Checked " + $pairs.Count + " policy mirrors.") -Remediation $null
    }
}

function Test-ForsettiRuleRegistry {
    $path = Join-Path $script:RepoRoot "core/policies/forsetti-enforcement-rules.json"
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Finding -RuleId "FAE-F002" -Severity "critical" -Decision "block" -Category "policies" -Path "core/policies/forsetti-enforcement-rules.json" -Message "Forsetti enforcement rule registry is missing." -Evidence "core/policies/forsetti-enforcement-rules.json" -Remediation "Create FAE-F001 through FAE-F020 registry."
        return
    }

    $registry = Read-JsonObject -Path $path
    $ids = @($registry.rules | ForEach-Object { [string]$_.rule_id })
    $expected = 1..20 | ForEach-Object { "FAE-F{0:D3}" -f $_ }
    $missing = @($expected | Where-Object { $ids -notcontains $_ })
    $shapeErrors = 0
    foreach ($rule in @($registry.rules)) {
        foreach ($field in @("rule_id", "title", "severity", "decision", "applies_to_modes", "required_evidence", "validation", "remediation")) {
            if (-not $rule.PSObject.Properties[$field]) {
                $shapeErrors++
                Add-Finding -RuleId "FAE-F002" -Severity "critical" -Decision "block" -Category "policies" -Path "core/policies/forsetti-enforcement-rules.json" -Message "Forsetti rule is missing a required field." -Evidence (($rule.rule_id) + " missing " + $field) -Remediation "Add rule ID, title, severity, decision, applies-to modes, required evidence, validation, and remediation fields."
            }
        }
    }
    if ($missing.Count -gt 0) {
        Add-Finding -RuleId "FAE-F002" -Severity "critical" -Decision "block" -Category "policies" -Path "core/policies/forsetti-enforcement-rules.json" -Message "Forsetti enforcement registry is missing rule IDs." -Evidence ($missing -join ", ") -Remediation "Add the missing FAE-F rule IDs."
    } elseif ($shapeErrors -eq 0) {
        Add-Finding -RuleId "FAE-F002" -Severity "info" -Decision "pass" -Category "policies" -Message "FAE-F001 through FAE-F020 are present with required fields." -Evidence ($expected -join ", ") -Remediation $null
    }
}

function Test-CoreBoundary {
    $coreRoot = Join-Path $script:RepoRoot "core"
    if (-not (Test-Path -LiteralPath $coreRoot)) {
        return
    }
    $forbidden = "(Invoke-WebRequest|Invoke-RestMethod|\bgh\b|\bdocker\b|\bwsl\b|https?://github.com|adapters/github-actions/workflows)"
    $hits = @()
    foreach ($file in Get-ChildItem -LiteralPath $coreRoot -File -Recurse) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        if ($text -match $forbidden) {
            $hits += (Get-RepoPath $file.FullName)
        }
    }
    if ($hits.Count -gt 0) {
        Add-Finding -RuleId "FAE-C011" -Severity "high" -Decision "request_changes" -Category "repo" -Message "Core contains hosted or local-tool dependency language." -Evidence ($hits -join ", ") -Remediation "Keep core governance host-neutral and move hosted/tool logic to adapters or standards."
    } else {
        Add-Finding -RuleId "FAE-C011" -Severity "info" -Decision "pass" -Category "repo" -Message "Core remains free of hosted workflow and local-tool dependencies." -Evidence "Scanned core files." -Remediation $null
    }
}

function Test-Repo {
    if (-not (Test-Path -LiteralPath (Join-Path $script:RepoRoot "FORSETTI_CONSTITUTION.md"))) {
        Add-Finding -RuleId "FAE-C011" -Severity "medium" -Decision "request_changes" -Category "repo" -Message "Repo mode was requested for a target repository without FFAE constitution." -Evidence $script:RepoRoot -Remediation "Use project-context, manifest, dependencies, capabilities, module-isolation, or evidence modes for target repositories."
        return
    }
    Test-RequiredRepositorySurface
    Test-JsonFiles
    Test-PolicyMirrors
    Test-ForsettiRuleRegistry
    Test-CoreBoundary
}

function Get-DefaultProfilePath {
    param([object]$Context)
    if ($Context -and $Context.edition_profile) {
        return Resolve-InputPath -Path ([string]$Context.edition_profile)
    }
    if (-not [string]::IsNullOrWhiteSpace($EditionProfilePath)) {
        return Resolve-InputPath -Path $EditionProfilePath
    }
    return $null
}

function ConvertTo-ForsettiSemVer {
    param([AllowNull()]$Value)

    if ($null -eq $Value) {
        return $null
    }

    if ($Value -is [string]) {
        $match = [regex]::Match(
            [string]$Value,
            '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?$'
        )
        if (-not $match.Success) {
            return $null
        }
        return [pscustomobject][ordered]@{
            major      = [int]$match.Groups[1].Value
            minor      = [int]$match.Groups[2].Value
            patch      = [int]$match.Groups[3].Value
            prerelease = if ($match.Groups[4].Success) { $match.Groups[4].Value } else { $null }
        }
    }

    foreach ($field in @('major', 'minor', 'patch')) {
        if (-not $Value.PSObject.Properties[$field]) {
            return $null
        }
        $component = 0
        if (-not [int]::TryParse([string]$Value.$field, [ref]$component) -or $component -lt 0) {
            return $null
        }
    }

    return [pscustomobject][ordered]@{
        major      = [int]$Value.major
        minor      = [int]$Value.minor
        patch      = [int]$Value.patch
        prerelease = if ($Value.PSObject.Properties['prerelease']) { $Value.prerelease } else { $null }
    }
}

function Compare-ForsettiSemVer {
    param($Left, $Right)

    $leftVersion = ConvertTo-ForsettiSemVer -Value $Left
    $rightVersion = ConvertTo-ForsettiSemVer -Value $Right
    if ($null -eq $leftVersion -or $null -eq $rightVersion) {
        throw 'Semantic version comparison requires valid MAJOR.MINOR.PATCH values.'
    }

    foreach ($field in @('major', 'minor', 'patch')) {
        if ($leftVersion.$field -lt $rightVersion.$field) { return -1 }
        if ($leftVersion.$field -gt $rightVersion.$field) { return 1 }
    }

    $leftPrerelease = [string]$leftVersion.prerelease
    $rightPrerelease = [string]$rightVersion.prerelease
    $leftEmpty = [string]::IsNullOrWhiteSpace($leftPrerelease)
    $rightEmpty = [string]::IsNullOrWhiteSpace($rightPrerelease)
    if ($leftEmpty -and $rightEmpty) { return 0 }
    if ($leftEmpty) { return 1 }
    if ($rightEmpty) { return -1 }
    return [string]::Compare($leftPrerelease, $rightPrerelease, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-ForsettiStringSet {
    param($Actual, [string[]]$Expected)

    $actualValues = @($Actual | ForEach-Object { [string]$_ } | Sort-Object -Unique)
    $expectedValues = @($Expected | Sort-Object -Unique)
    $missing = @($expectedValues | Where-Object { $actualValues -notcontains $_ })
    $extra = @($actualValues | Where-Object { $expectedValues -notcontains $_ })
    return [pscustomobject][ordered]@{
        Matches = ($missing.Count -eq 0 -and $extra.Count -eq 0)
        Missing = $missing
        Extra   = $extra
    }
}

function Test-ForsettiStringMap {
    param($Actual, [hashtable]$Expected)

    $actualMap = @{}
    if ($null -ne $Actual) {
        foreach ($property in @($Actual.PSObject.Properties)) {
            $actualMap[$property.Name] = [string]$property.Value
        }
    }

    $missing = @($Expected.Keys | Where-Object { -not $actualMap.ContainsKey($_) } | Sort-Object)
    $extra = @($actualMap.Keys | Where-Object { -not $Expected.ContainsKey($_) } | Sort-Object)
    $mismatched = @(
        $Expected.Keys | Where-Object {
            $actualMap.ContainsKey($_) -and $actualMap[$_] -ne [string]$Expected[$_]
        } | Sort-Object | ForEach-Object {
            $_ + '=' + $actualMap[$_] + ' (expected ' + [string]$Expected[$_] + ')'
        }
    )

    return [pscustomobject][ordered]@{
        Matches    = ($missing.Count -eq 0 -and $extra.Count -eq 0 -and $mismatched.Count -eq 0)
        Missing    = $missing
        Extra      = $extra
        Mismatched = $mismatched
    }
}

function Test-ProjectContextObject {
    param([object]$Context, [string]$SourcePath)

    $required = @(
        'repository_mode',
        'forsetti_edition',
        'target_platform',
        'framework_version',
        'edition_profile',
        'manifest_schema_version',
        'manifest_template_version',
        'deployment_pattern',
        'module_type',
        'module_id',
        'capabilities_requested',
        'runtime_requirements_declared',
        'uses_public_api_only',
        'touches_framework_internals'
    )
    $missing = @()
    foreach ($field in $required) {
        if (-not $Context.PSObject.Properties[$field]) {
            $missing += $field
        } elseif ([string]::IsNullOrWhiteSpace([string]$Context.$field)) {
            if ($field -ne 'capabilities_requested' -and $field -ne 'module_id') {
                $missing += $field
            }
        }
    }
    if ($missing.Count -gt 0) {
        Add-Finding -RuleId 'FAE-F001' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Forsetti project context is missing required fields.' -Evidence ($missing -join ', ') -Remediation 'Complete every required Forsetti project context field before execution.'
        return $false
    }

    if ($Context.forsetti_edition -notin @('apple', 'windows')) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Unsupported Forsetti edition.' -Evidence ([string]$Context.forsetti_edition) -Remediation 'Select the Apple or Windows edition profile.'
        return $false
    }
    if ($Context.target_platform -notin @('iOS', 'macOS', 'Windows')) {
        Add-Finding -RuleId 'FAE-F003' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Unsupported target platform.' -Evidence ([string]$Context.target_platform) -Remediation 'Select a target platform supported by the edition profile.'
        return $false
    }
    if ($null -eq (ConvertTo-ForsettiSemVer -Value $Context.framework_version)) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Framework version is not valid semantic version data.' -Evidence ([string]$Context.framework_version) -Remediation 'Use MAJOR.MINOR.PATCH[-PRERELEASE].'
        return $false
    }
    if ($Context.manifest_schema_version -ne '1.1' -or $Context.manifest_template_version -ne '1.1') {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Manifest schema/template version must be 1.1 for the selected current profiles.' -Evidence (($Context.manifest_schema_version) + '/' + ($Context.manifest_template_version)) -Remediation 'Set manifest schema and template versions to 1.1.'
        return $false
    }
    if ($Context.module_type -notin @('app', 'ui', 'service', 'not_applicable')) {
        Add-Finding -RuleId 'FAE-F001' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Unknown Forsetti module type.' -Evidence ([string]$Context.module_type) -Remediation 'Use app, ui, service, or not_applicable.'
        return $false
    }
    if ($Context.module_type -ne 'not_applicable' -and [string]::IsNullOrWhiteSpace([string]$Context.module_id)) {
        Add-Finding -RuleId 'FAE-F001' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Forsetti project context is missing module_id for a module-bearing task.' -Evidence 'module_id' -Remediation 'Provide the Forsetti module identifier or set module_type to not_applicable for non-module governance work.'
        return $false
    }
    if ($Context.touches_framework_internals -eq $true) {
        Add-Finding -RuleId 'FAE-F012' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Task context indicates framework internals are touched.' -Evidence 'touches_framework_internals=true' -Remediation 'Re-scope the task to public APIs or obtain explicit governance-class authority.'
        return $false
    }
    if ($Context.uses_public_api_only -ne $true) {
        Add-Finding -RuleId 'FAE-F011' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Task context does not confirm public API-only use.' -Evidence 'uses_public_api_only=false' -Remediation 'Confirm and enforce public Forsetti API-only use.'
        return $false
    }

    Add-Finding -RuleId 'FAE-F001' -Severity 'info' -Decision 'pass' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Forsetti project context is complete.' -Evidence (($Context.forsetti_edition) + ' ' + ($Context.framework_version) + ' ' + ($Context.target_platform)) -Remediation $null
    return $true
}

function Test-ProjectContext {
    if ([string]::IsNullOrWhiteSpace($ProjectContextPath)) {
        if ($Mode -eq 'project-context') {
            Add-Finding -RuleId 'FAE-F001' -Severity 'critical' -Decision 'block' -Category 'project-context' -Message 'Project context mode requires -ProjectContextPath.' -Evidence '-ProjectContextPath' -Remediation 'Provide a Forsetti project context JSON file.'
        }
        return $null
    }
    $path = Resolve-InputPath -Path $ProjectContextPath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Finding -RuleId 'FAE-F001' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $path) -Message 'Project context file is missing.' -Evidence $ProjectContextPath -Remediation 'Create the project context file before execution.'
        return $null
    }
    $context = Read-JsonObject -Path $path
    $null = Test-ProjectContextObject -Context $context -SourcePath $path
    return $context
}

function Test-ProjectContextProfileAlignment {
    param([AllowNull()]$Context, [AllowNull()]$Profile, [AllowNull()][string]$SourcePath)

    if ($null -eq $Context -or $null -eq $Profile) {
        return
    }

    if ([string]$Context.forsetti_edition -ne [string]$Profile.edition) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Project context edition does not match the selected profile.' -Evidence @('context=' + $Context.forsetti_edition, 'profile=' + $Profile.edition) -Remediation 'Select the profile named by the project context or correct the context.'
    }
    if ([string]$Context.framework_version -ne [string]$Profile.frameworkVersion) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Project context framework version does not match the selected profile.' -Evidence @('context=' + $Context.framework_version, 'profile=' + $Profile.frameworkVersion) -Remediation 'Use a versioned profile matching the governed framework version.'
    }
    if (@($Profile.supportedPlatforms) -notcontains [string]$Context.target_platform) {
        Add-Finding -RuleId 'FAE-F003' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Project context target platform is not supported by the selected profile.' -Evidence ([string]$Context.target_platform) -Remediation 'Select a supported target platform or another edition profile.'
    }
    if ([string]$Context.manifest_schema_version -ne [string]$Profile.manifest.currentSchemaVersion -or [string]$Context.manifest_template_version -ne [string]$Profile.manifest.currentTemplateVersion) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'project-context' -Path (Get-RepoPath $SourcePath) -Message 'Project context manifest versions do not match the selected profile.' -Evidence @('context=' + $Context.manifest_schema_version + '/' + $Context.manifest_template_version, 'profile=' + $Profile.manifest.currentSchemaVersion + '/' + $Profile.manifest.currentTemplateVersion) -Remediation 'Use the schema/template versions declared current by the selected profile.'
    }
}

function Test-EditionProfileObject {
    param([object]$Profile, [string]$SourcePath)

    $valid = $true
    $required = @('edition', 'frameworkVersion', 'supportedPlatforms', 'nativeTools', 'publicProducts', 'manifest', 'capabilities', 'dependencyRules', 'verificationCommands')
    $missing = @()
    foreach ($field in $required) {
        if (-not $Profile.PSObject.Properties[$field]) {
            $missing += $field
        }
    }
    if ($missing.Count -gt 0) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Edition profile is missing required fields.' -Evidence ($missing -join ', ') -Remediation 'Complete the edition profile before using it.'
        return $false
    }
    if ($null -eq (ConvertTo-ForsettiSemVer -Value $Profile.frameworkVersion)) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Edition profile has an invalid framework version.' -Evidence ([string]$Profile.frameworkVersion) -Remediation 'Use MAJOR.MINOR.PATCH[-PRERELEASE].'
        $valid = $false
    }
    if ($Profile.manifest.currentSchemaVersion -ne '1.1' -or $Profile.manifest.currentTemplateVersion -ne '1.1') {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Edition profile does not select manifest 1.1.' -Evidence (($Profile.manifest.currentSchemaVersion) + '/' + ($Profile.manifest.currentTemplateVersion)) -Remediation 'Set current schema and template versions to 1.1.'
        $valid = $false
    }

    if ($Profile.edition -eq 'apple' -and $Profile.frameworkVersion -eq '0.1.5') {
        $appleRequired = @(
            'supportStatus', 'sourceContract', 'swiftToolsVersion', 'minimumDeploymentTargets',
            'internalTargets', 'ioCapabilityMappings', 'serviceCapabilityMappings',
            'uiCapabilityMappings', 'defaultModuleRoleRules', 'runtimeInvariants',
            'launchActivationStrategies', 'xcodeTemplates', 'consumerVerificationCommands',
            'frameworkIdentity', 'implementationRules', 'deploymentPatterns',
            'upstreamValidationEvidence'
        )
        $appleMissing = @($appleRequired | Where-Object { -not $Profile.PSObject.Properties[$_] })
        if ($appleMissing.Count -gt 0) {
            Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 profile is missing source-grounded contract fields.' -Evidence ($appleMissing -join ', ') -Remediation 'Restore the complete Apple 0.1.5 contract profile.'
            $valid = $false
        } else {
            $expectedPlatforms = @('iOS', 'macOS')
            $expectedProducts = @('ForsettiCore', 'ForsettiPlatform', 'ForsettiHostTemplate')
            $expectedInternalTargets = @('ForsettiModulesExample')
            $expectedCapabilities = @('networking', 'storage', 'secure_storage', 'file_export', 'crypto_utilities', 'telemetry', 'routing_overlay', 'ui_theme_mask', 'toolbar_items', 'view_injection', 'shared_database', 'authentication', 'diagnostics', 'api', 'security')
            $expectedIOKinds = @('networking', 'storage', 'secure_storage', 'file_export', 'telemetry', 'shared_database', 'authentication', 'diagnostics', 'api', 'security')
            $expectedAccess = @('read', 'write', 'read_write', 'execute', 'emit', 'consume')
            $expectedRoles = @('ui', 'shared_database', 'authentication', 'diagnostics', 'api', 'security')
            $expectedStrategies = @('restoreOnly', 'activateAllEligibleForDevelopment', 'activate(moduleIDs:)')
            $expectedIOMap = @{
                networking = 'networking'; storage = 'storage'; secure_storage = 'secure_storage';
                file_export = 'file_export'; telemetry = 'telemetry'; shared_database = 'shared_database';
                authentication = 'authentication'; diagnostics = 'diagnostics'; api = 'api'; security = 'security'
            }
            $expectedServiceMap = @{
                NetworkingService = 'networking'; StorageService = 'storage'; SecureStorageService = 'secure_storage';
                FileExportService = 'file_export'; TelemetryService = 'telemetry'; SharedDatabaseService = 'shared_database';
                AuthenticationService = 'authentication'; DiagnosticsService = 'diagnostics'; APIService = 'api'; SecurityService = 'security'
            }
            $expectedUIMap = @{
                themeIDs = 'ui_theme_mask'; viewIDs = 'view_injection'; slotIDs = 'view_injection';
                toolbarItemIDs = 'toolbar_items'; routeIDs = 'routing_overlay'; pointerIDs = 'routing_overlay'
            }
            $expectedDependencyRules = @{
                ForsettiCore = @(
                    'depends_on:nothing_in_repository', 'must_not_import:ForsettiPlatform',
                    'must_not_import:ForsettiModulesExample', 'must_not_import:ForsettiHostTemplate',
                    'must_not_import:SwiftUI', 'must_not_import:UIKit', 'must_not_import:AppKit',
                    'must_not_import:StoreKit', 'must_not_import:Combine'
                )
                ForsettiPlatform = @(
                    'depends_on:ForsettiCore', 'must_not_import:ForsettiModulesExample',
                    'must_not_import:ForsettiHostTemplate', 'must_not_import:SwiftUI',
                    'must_not_import:UIKit', 'must_not_import:AppKit'
                )
                ForsettiModulesExample = @(
                    'depends_on:ForsettiCore', 'must_not_import:ForsettiPlatform',
                    'must_not_import:ForsettiHostTemplate', 'must_not_import:SwiftUI',
                    'must_not_import:UIKit', 'must_not_import:AppKit', 'must_not_import:StoreKit'
                )
                ForsettiHostTemplate = @(
                    'depends_on:ForsettiCore', 'depends_on:ForsettiPlatform',
                    'must_not_import:ForsettiModulesExample'
                )
                ConsumerModules = @(
                    'may_depend_on:public_products_only', 'must_not_reference_other_modules_directly'
                )
            }

            foreach ($checkDefinition in @(
                [pscustomobject]@{ Label = 'supportedPlatforms'; Actual = $Profile.supportedPlatforms; Expected = $expectedPlatforms },
                [pscustomobject]@{ Label = 'publicProducts'; Actual = $Profile.publicProducts; Expected = $expectedProducts },
                [pscustomobject]@{ Label = 'internalTargets'; Actual = $Profile.internalTargets; Expected = $expectedInternalTargets },
                [pscustomobject]@{ Label = 'capabilities'; Actual = $Profile.capabilities; Expected = $expectedCapabilities },
                [pscustomobject]@{ Label = 'manifest.ioKinds'; Actual = $Profile.manifest.ioKinds; Expected = $expectedIOKinds },
                [pscustomobject]@{ Label = 'manifest.ioAccessModes'; Actual = $Profile.manifest.ioAccessModes; Expected = $expectedAccess },
                [pscustomobject]@{ Label = 'manifest.defaultModuleRoles'; Actual = $Profile.manifest.defaultModuleRoles; Expected = $expectedRoles },
                [pscustomobject]@{ Label = 'launchActivationStrategies'; Actual = $Profile.launchActivationStrategies; Expected = $expectedStrategies }
            )) {
                $setResult = Test-ForsettiStringSet -Actual $checkDefinition.Actual -Expected $checkDefinition.Expected
                if (-not $setResult.Matches) {
                    Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message ('Apple 0.1.5 profile ' + $checkDefinition.Label + ' does not match the framework contract.') -Evidence @('missing=' + ($setResult.Missing -join ','), 'extra=' + ($setResult.Extra -join ',')) -Remediation 'Restore the source-derived Apple 0.1.5 values.'
                    $valid = $false
                }
            }

            if ($Profile.swiftToolsVersion -ne '5.10' -or $Profile.minimumDeploymentTargets.iOS -ne '17.0' -or $Profile.minimumDeploymentTargets.macOS -ne '14.0') {
                Add-Finding -RuleId 'FAE-F003' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 toolchain or minimum deployment targets are incorrect.' -Evidence @('swift-tools=' + $Profile.swiftToolsVersion, 'iOS=' + $Profile.minimumDeploymentTargets.iOS, 'macOS=' + $Profile.minimumDeploymentTargets.macOS) -Remediation 'Use Swift tools 5.10, iOS 17.0, and macOS 14.0.'
                $valid = $false
            }
            if ($Profile.sourceContract.version -ne '0.1.5') {
                Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple profile source contract version does not match frameworkVersion.' -Evidence ([string]$Profile.sourceContract.version) -Remediation 'Pin sourceContract.version to 0.1.5.'
                $valid = $false
            }
            if (@($Profile.capabilities) -contains 'event_publishing') {
                Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 defines no event_publishing capability.' -Evidence 'event_publishing' -Remediation 'Remove event_publishing from the Apple capability contract.'
                $valid = $false
            }
            if (@($Profile.manifest.ioKinds) -contains 'crypto_utilities') {
                Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'crypto_utilities is a capability, not an Apple 0.1.5 I/O provider kind.' -Evidence 'crypto_utilities' -Remediation 'Keep crypto_utilities in capabilities and remove it from manifest.ioKinds.'
                $valid = $false
            }

            foreach ($mappingDefinition in @(
                [pscustomobject]@{ Name = 'ioCapabilityMappings'; Actual = $Profile.ioCapabilityMappings; Expected = $expectedIOMap },
                [pscustomobject]@{ Name = 'serviceCapabilityMappings'; Actual = $Profile.serviceCapabilityMappings; Expected = $expectedServiceMap },
                [pscustomobject]@{ Name = 'uiCapabilityMappings'; Actual = $Profile.uiCapabilityMappings; Expected = $expectedUIMap }
            )) {
                $mapResult = Test-ForsettiStringMap -Actual $mappingDefinition.Actual -Expected $mappingDefinition.Expected
                if (-not $mapResult.Matches) {
                    Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message ('Apple 0.1.5 profile ' + $mappingDefinition.Name + ' does not match the framework contract.') -Evidence @('missing=' + ($mapResult.Missing -join ','), 'extra=' + ($mapResult.Extra -join ','), 'mismatched=' + ($mapResult.Mismatched -join ',')) -Remediation 'Restore the exact source-derived capability mapping.'
                    $valid = $false
                }
            }

            foreach ($targetName in $expectedDependencyRules.Keys) {
                $actualRules = if ($Profile.dependencyRules.PSObject.Properties[$targetName]) { $Profile.dependencyRules.$targetName } else { @() }
                $ruleResult = Test-ForsettiStringSet -Actual $actualRules -Expected $expectedDependencyRules[$targetName]
                if (-not $ruleResult.Matches) {
                    Add-Finding -RuleId 'FAE-F013' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message ('Apple 0.1.5 dependency rules for ' + $targetName + ' do not match the enforced architecture contract.') -Evidence @('missing=' + ($ruleResult.Missing -join ','), 'extra=' + ($ruleResult.Extra -join ',')) -Remediation 'Restore the exact source-derived target dependency and import rules.'
                    $valid = $false
                }
            }

            $legacy = $Profile.manifest.legacyDefaults
            if ($null -eq $legacy -or $legacy.schemaVersion -ne '1.0' -or $legacy.manifestTemplateVersionWhenAbsent -ne '1.0' -or $null -ne $legacy.defaultModuleRole -or @($legacy.runtimeRequirements.io).Count -ne 0 -or $null -ne $legacy.runtimeRequirements.ui -or $legacy.runtimeRequirements.dataIsolation.mode -ne 'private_to_module' -or @($legacy.runtimeRequirements.dataIsolation.ownedStoreIDs).Count -ne 0 -or @($legacy.runtimeRequirements.dataIsolation.requiredDefaultRoles).Count -ne 0) {
                Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 legacy manifest defaults do not fail safely.' -Evidence 'manifest.legacyDefaults' -Remediation 'Restore schema/template 1.0 defaults with no I/O, no UI, no default role, and private_to_module isolation.'
                $valid = $false
            }

            if ($Profile.frameworkIdentity.architectureStyle -ne 'modularity-first_object-oriented' -or $Profile.frameworkIdentity.platformStrategy -ne 'Apple-native' -or $Profile.frameworkIdentity.consumerRuntimeBoundary -ne 'sealed_public_contracts_only' -or $Profile.frameworkIdentity.extensionModel -ne 'public_Forsetti_contracts_or_upstream_framework_enhancement' -or $Profile.frameworkIdentity.allowsThirdPartyRuntimeDependencies -ne $false) {
                Add-Finding -RuleId 'FAE-F011' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 framework identity or sealed-runtime boundary is incorrect.' -Evidence 'frameworkIdentity' -Remediation 'Restore the Apple-native, modularity-first, public-contract-only identity with no third-party runtime dependencies.'
                $valid = $false
            }

            $patternResult = Test-ForsettiStringSet -Actual @($Profile.deploymentPatterns | ForEach-Object { [string]$_.id }) -Expected @('A', 'B', 'C', 'D')
            if (-not $patternResult.Matches) {
                Add-Finding -RuleId 'FAE-F003' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 deployment pattern catalog is incomplete.' -Evidence @('missing=' + ($patternResult.Missing -join ','), 'extra=' + ($patternResult.Extra -join ',')) -Remediation 'Restore deployment patterns A through D.'
                $valid = $false
            }

            $upstream = $Profile.upstreamValidationEvidence
            $sourceReport = @($Profile.sourceContract.files | Where-Object { $_.path -eq '.forsetti/alignment/final-report.json' } | Select-Object -First 1)
            if ($upstream.status -ne 'pass' -or [int]$upstream.acceptanceGateCount -ne 13 -or [int]$upstream.packageTestCount -ne 81 -or @($sourceReport).Count -ne 1 -or [string]$sourceReport[0].sha256 -ne [string]$upstream.reportSHA256) {
                Add-Finding -RuleId 'FAE-F020' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Apple 0.1.5 upstream validation evidence is missing or inconsistent with the pinned source report.' -Evidence 'upstreamValidationEvidence' -Remediation 'Restore the supplied upstream pass report reference, 13 acceptance gates, and 81 package tests.'
                $valid = $false
            }
        }
    }

    if ($valid) {
        Add-Finding -RuleId 'FAE-F002' -Severity 'info' -Decision 'pass' -Category 'edition-profile' -Path (Get-RepoPath $SourcePath) -Message 'Edition profile is complete.' -Evidence (($Profile.edition) + ' ' + ($Profile.frameworkVersion)) -Remediation $null
    }
    return $valid
}

function Test-EditionProfile {
    if (-not [string]::IsNullOrWhiteSpace($EditionProfilePath)) {
        $path = Resolve-InputPath -Path $EditionProfilePath
        if (-not (Test-Path -LiteralPath $path)) {
            Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path (Get-RepoPath $path) -Message 'Edition profile file is missing.' -Evidence $EditionProfilePath -Remediation 'Provide the selected edition profile.'
            return $null
        }
        $profile = Read-JsonObject -Path $path
        $null = Test-EditionProfileObject -Profile $profile -SourcePath $path
        return $profile
    }

    if ($Mode -eq 'edition-profile' -or $Mode -eq 'all') {
        $profilePaths = @(
            'editions/apple/forsetti-apple-0.1.5.profile.json',
            'editions/apple/forsetti-apple-0.1.3.profile.json',
            'editions/windows/forsetti-windows-0.2.0.profile.json'
        )
        foreach ($relative in $profilePaths) {
            $path = Join-Path $script:RepoRoot $relative
            if (Test-Path -LiteralPath $path) {
                $null = Test-EditionProfileObject -Profile (Read-JsonObject -Path $path) -SourcePath $path
            } elseif ($Mode -eq 'edition-profile') {
                Add-Finding -RuleId 'FAE-F002' -Severity 'critical' -Decision 'block' -Category 'edition-profile' -Path $relative -Message 'Required edition profile is missing.' -Evidence $relative -Remediation 'Create the required edition profile.'
            }
        }
    }
    return $null
}

function Test-Manifest {
    param([AllowNull()]$Profile)

    if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        if ($Mode -eq 'manifest') {
            Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Message 'Manifest mode requires -ManifestPath.' -Evidence '-ManifestPath' -Remediation 'Provide a Forsetti module manifest JSON file.'
        }
        return $null
    }

    $path = Resolve-InputPath -Path $ManifestPath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest file is missing.' -Evidence $ManifestPath -Remediation 'Create or provide the module manifest.'
        return $null
    }

    $manifest = Read-JsonObject -Path $path
    $required = @('schemaVersion', 'manifestTemplateVersion', 'moduleID', 'displayName', 'moduleVersion', 'moduleType', 'supportedPlatforms', 'minForsettiVersion', 'capabilitiesRequested', 'entryPoint', 'runtimeRequirements')
    if ($Profile -and $Profile.manifest.requiredFields) {
        $required = @($Profile.manifest.requiredFields)
    }

    $missing = @()
    foreach ($field in $required) {
        if (-not $manifest.PSObject.Properties[$field]) {
            $missing += $field
        }
    }
    if ($missing.Count -gt 0) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest is missing required fields.' -Evidence ($missing -join ', ') -Remediation 'Add all manifest 1.1 fields required by the selected profile.'
        return $manifest
    }

    $expectedSchema = if ($Profile) { [string]$Profile.manifest.currentSchemaVersion } else { '1.1' }
    $expectedTemplate = if ($Profile) { [string]$Profile.manifest.currentTemplateVersion } else { '1.1' }
    if ($manifest.schemaVersion -ne $expectedSchema -or $manifest.manifestTemplateVersion -ne $expectedTemplate) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest schema/template version is unsupported by the selected profile.' -Evidence (($manifest.schemaVersion) + '/' + ($manifest.manifestTemplateVersion)) -Remediation ('Use schemaVersion ' + $expectedSchema + ' and manifestTemplateVersion ' + $expectedTemplate + '.')
    }
    if ($manifest.moduleType -notin @('service', 'ui', 'app')) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest has invalid module type.' -Evidence ([string]$manifest.moduleType) -Remediation 'Use service, ui, or app.'
    }

    if ($Profile) {
        $unsupportedPlatforms = @($manifest.supportedPlatforms | Where-Object { @($Profile.supportedPlatforms) -notcontains $_ })
        if ($unsupportedPlatforms.Count -gt 0) {
            Add-Finding -RuleId 'FAE-F003' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest platform is not supported by selected profile.' -Evidence ($unsupportedPlatforms -join ', ') -Remediation 'Select a matching profile or correct supportedPlatforms.'
        }
        $unknownCapabilities = @($manifest.capabilitiesRequested | Where-Object { @($Profile.capabilities) -notcontains $_ })
        if ($unknownCapabilities.Count -gt 0) {
            Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest requests unknown capabilities.' -Evidence ($unknownCapabilities -join ', ') -Remediation 'Use only capabilities defined by the selected profile.'
        }
    }

    if ($Profile -and $Profile.edition -eq 'apple' -and $Profile.frameworkVersion -eq '0.1.5') {
        $moduleIDPattern = [string]$Profile.manifest.moduleIDPattern
        $entryPointPattern = [string]$Profile.manifest.entryPointPattern
        if ([string]::IsNullOrWhiteSpace([string]$manifest.moduleID) -or $manifest.moduleID -notmatch $moduleIDPattern) {
            Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest moduleID is not a valid reverse-DNS identifier.' -Evidence ([string]$manifest.moduleID) -Remediation 'Use reverse-DNS segments beginning with letters.'
        }
        foreach ($prefix in @($Profile.manifest.reservedModuleIDPrefixes)) {
            if ([string]$manifest.moduleID -like ($prefix + '*')) {
                Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest moduleID uses a reserved framework namespace.' -Evidence ([string]$manifest.moduleID) -Remediation 'Use an application-owned reverse-DNS namespace.'
            }
        }
        if ([string]::IsNullOrWhiteSpace([string]$manifest.entryPoint) -or $manifest.entryPoint -notmatch $entryPointPattern) {
            Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest entryPoint is not a valid Swift type path.' -Evidence ([string]$manifest.entryPoint) -Remediation 'Use a Swift identifier or dotted type path.'
        }
    }

    $moduleVersion = ConvertTo-ForsettiSemVer -Value $manifest.moduleVersion
    $minVersion = ConvertTo-ForsettiSemVer -Value $manifest.minForsettiVersion
    $maxVersion = if ($manifest.PSObject.Properties['maxForsettiVersion'] -and $null -ne $manifest.maxForsettiVersion) { ConvertTo-ForsettiSemVer -Value $manifest.maxForsettiVersion } else { $null }
    if ($null -eq $moduleVersion -or $null -eq $minVersion -or ($manifest.PSObject.Properties['maxForsettiVersion'] -and $null -ne $manifest.maxForsettiVersion -and $null -eq $maxVersion)) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest contains invalid semantic version objects.' -Evidence 'moduleVersion/minForsettiVersion/maxForsettiVersion' -Remediation 'Use non-negative major, minor, and patch integer fields.'
    } else {
        if ($null -ne $maxVersion -and (Compare-ForsettiSemVer -Left $maxVersion -Right $minVersion) -lt 0) {
            Add-Finding -RuleId 'FAE-F005' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'maxForsettiVersion is below minForsettiVersion.' -Evidence 'version range' -Remediation 'Correct the supported Forsetti version range.'
        }
        if ($Profile) {
            $frameworkVersion = ConvertTo-ForsettiSemVer -Value $Profile.frameworkVersion
            if ((Compare-ForsettiSemVer -Left $minVersion -Right $frameworkVersion) -gt 0 -or ($null -ne $maxVersion -and (Compare-ForsettiSemVer -Left $frameworkVersion -Right $maxVersion) -gt 0)) {
                Add-Finding -RuleId 'FAE-F005' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest Forsetti compatibility range excludes the selected framework profile.' -Evidence ([string]$Profile.frameworkVersion) -Remediation 'Select a compatible profile or correct the manifest version range.'
            }
        }
    }

    $runtime = $manifest.runtimeRequirements
    if ($null -eq $runtime) {
        Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'runtimeRequirements is missing.' -Evidence 'runtimeRequirements' -Remediation 'Declare io, ui, and dataIsolation runtime requirements.'
        return $manifest
    }
    foreach ($field in @('io', 'ui', 'dataIsolation')) {
        if (-not $runtime.PSObject.Properties[$field]) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Runtime requirements are incomplete.' -Evidence ('runtimeRequirements.' + $field) -Remediation 'Declare io, ui, and dataIsolation runtime requirements.'
        }
    }

    $ioKinds = if ($Profile -and $Profile.manifest.ioKinds) { @($Profile.manifest.ioKinds) } else { @('networking', 'storage', 'secure_storage', 'file_export', 'telemetry', 'shared_database', 'authentication', 'diagnostics', 'api', 'security') }
    $ioAccessModes = if ($Profile -and $Profile.manifest.ioAccessModes) { @($Profile.manifest.ioAccessModes) } else { @('read', 'write', 'read_write', 'execute', 'emit', 'consume') }
    $ioIDs = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($requirement in @($runtime.io)) {
        $requirementID = [string]$requirement.requirementID
        if ([string]::IsNullOrWhiteSpace($requirementID)) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'I/O requirementID is blank.' -Evidence 'runtimeRequirements.io.requirementID' -Remediation 'Assign a unique nonblank requirementID.'
        } elseif (-not $ioIDs.Add($requirementID.Trim())) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Duplicate I/O requirementID detected.' -Evidence $requirementID -Remediation 'Use unique requirementID values.'
        }

        $kind = [string]$requirement.kind
        if ($ioKinds -notcontains $kind) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest declares an unsupported I/O provider kind.' -Evidence $kind -Remediation 'Use an I/O kind declared by the selected profile.'
        }
        if ($ioAccessModes -notcontains [string]$requirement.access) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest declares an unsupported I/O access mode.' -Evidence ([string]$requirement.access) -Remediation 'Use an I/O access mode declared by the selected profile.'
        }
        if ($Profile -and $Profile.PSObject.Properties['ioCapabilityMappings']) {
            $mapping = $Profile.ioCapabilityMappings.PSObject.Properties[$kind]
            if ($mapping -and @($manifest.capabilitiesRequested) -notcontains [string]$mapping.Value) {
                Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'I/O requirement is missing its mapped capability.' -Evidence @('kind=' + $kind, 'capability=' + [string]$mapping.Value) -Remediation 'Declare the mapped capability or remove the I/O requirement.'
            }
        }
    }

    if ($manifest.moduleType -eq 'service' -and $runtime.PSObject.Properties['ui'] -and $null -ne $runtime.ui) {
        Add-Finding -RuleId 'FAE-F015' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Service module declares UI runtime contribution.' -Evidence 'runtimeRequirements.ui' -Remediation 'Remove UI contribution from service modules or change module type.'
    }
    if ($manifest.moduleType -in @('ui', 'app') -and $runtime.PSObject.Properties['ui'] -and $null -eq $runtime.ui) {
        Add-Finding -RuleId 'FAE-F014' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'UI/app module lacks an active UI runtime surface.' -Evidence 'runtimeRequirements.ui=null' -Remediation 'Declare the UI runtime surface required by the selected profile.'
    }

    if ($null -ne $runtime.ui) {
        foreach ($field in @('themeIDs', 'viewIDs', 'slotIDs', 'toolbarItemIDs', 'routeIDs', 'pointerIDs')) {
            if ($runtime.ui.PSObject.Properties[$field]) {
                $values = @($runtime.ui.$field)
                $normalized = @($values | ForEach-Object { [string]$_ })
                if (@($normalized | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0 -or @($normalized | Sort-Object -Unique).Count -ne $normalized.Count) {
                    Add-Finding -RuleId 'FAE-F014' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message ('UI requirement ' + $field + ' contains blank or duplicate identifiers.') -Evidence $field -Remediation 'Use unique nonblank UI requirement identifiers.'
                }
                if ($Profile -and $Profile.PSObject.Properties['uiCapabilityMappings']) {
                    $mapping = $Profile.uiCapabilityMappings.PSObject.Properties[$field]
                    if ($mapping -and $normalized.Count -gt 0 -and @($manifest.capabilitiesRequested) -notcontains [string]$mapping.Value) {
                        Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message ('UI requirement ' + $field + ' is missing its mapped capability.') -Evidence ([string]$mapping.Value) -Remediation 'Declare the mapped UI capability or remove the declared contribution identifiers.'
                    }
                }
            }
        }
    }

    $validDefaultRoles = if ($Profile -and $Profile.manifest.defaultModuleRoles) { @($Profile.manifest.defaultModuleRoles) } else { @('ui', 'shared_database', 'authentication', 'diagnostics', 'api', 'security') }
    if ($manifest.PSObject.Properties['defaultModuleRole'] -and $null -ne $manifest.defaultModuleRole -and $validDefaultRoles -notcontains [string]$manifest.defaultModuleRole) {
        Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest has invalid default module role.' -Evidence ([string]$manifest.defaultModuleRole) -Remediation ('Use ' + ($validDefaultRoles -join ', ') + ', or null.')
    }
    if ($manifest.PSObject.Properties['defaultModuleRole'] -and $null -ne $manifest.defaultModuleRole) {
        $role = [string]$manifest.defaultModuleRole
        if ($Profile -and $Profile.PSObject.Properties['defaultModuleRoleRules']) {
            $rule = $Profile.defaultModuleRoleRules.PSObject.Properties[$role]
            if ($rule) {
                if (@($rule.Value.allowedModuleTypes) -notcontains [string]$manifest.moduleType) {
                    Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Default module role is invalid for moduleType.' -Evidence @('role=' + $role, 'moduleType=' + $manifest.moduleType) -Remediation 'Select a role permitted for this module type.'
                }
                $requiredCapability = [string]$rule.Value.requiredCapability
                if (-not [string]::IsNullOrWhiteSpace($requiredCapability) -and @($manifest.capabilitiesRequested) -notcontains $requiredCapability) {
                    Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Default role requires its mapped capability.' -Evidence $requiredCapability -Remediation 'Declare the matching capability or set defaultModuleRole to null.'
                }
            }
        } else {
            if ($role -eq 'ui' -and $manifest.moduleType -notin @('ui', 'app')) {
                Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'UI default role is only valid for UI or app modules.' -Evidence ([string]$manifest.moduleType) -Remediation 'Use a service default role or change moduleType.'
            }
            if ($role -ne 'ui' -and $manifest.moduleType -ne 'service') {
                Add-Finding -RuleId 'FAE-F004' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Service default roles are only valid for service modules.' -Evidence ([string]$manifest.moduleType) -Remediation 'Use ui for UI/app modules or null when no default role applies.'
            }
            if ($role -ne 'ui' -and @($manifest.capabilitiesRequested) -notcontains $role) {
                Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Default role requires its matching capability.' -Evidence $role -Remediation 'Declare the matching capability or set defaultModuleRole to null.'
            }
        }
    }

    if ($runtime.PSObject.Properties['dataIsolation'] -and $null -ne $runtime.dataIsolation) {
        $dataIsolationModes = if ($Profile -and $Profile.manifest.dataIsolationModes) { @($Profile.manifest.dataIsolationModes) } else { @('private_to_module', 'framework_mediated_shared') }
        if ($dataIsolationModes -notcontains [string]$runtime.dataIsolation.mode) {
            Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest declares an unsupported data isolation mode.' -Evidence ([string]$runtime.dataIsolation.mode) -Remediation 'Use a data isolation mode declared by the selected profile.'
        }
        foreach ($field in @('ownedStoreIDs', 'requiredDefaultRoles')) {
            if ($runtime.dataIsolation.PSObject.Properties[$field]) {
                $values = @($runtime.dataIsolation.$field | ForEach-Object { [string]$_ })
                if (@($values | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0 -or @($values | Sort-Object -Unique).Count -ne $values.Count) {
                    Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message ('Data isolation ' + $field + ' contains blank or duplicate values.') -Evidence $field -Remediation 'Use unique nonblank values.'
                }
            }
        }
        foreach ($requiredRole in @($runtime.dataIsolation.requiredDefaultRoles)) {
            $role = [string]$requiredRole
            if ($validDefaultRoles -notcontains $role) {
                Add-Finding -RuleId 'FAE-F010' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Data isolation requires an unknown default role.' -Evidence $role -Remediation 'Use a role declared by the selected profile.'
                continue
            }
            if ($Profile -and $Profile.PSObject.Properties['defaultModuleRoleRules']) {
                $rule = $Profile.defaultModuleRoleRules.PSObject.Properties[$role]
                if ($rule) {
                    $requiredCapability = [string]$rule.Value.requiredCapability
                    if (-not [string]::IsNullOrWhiteSpace($requiredCapability) -and @($manifest.capabilitiesRequested) -notcontains $requiredCapability) {
                        Add-Finding -RuleId 'FAE-F009' -Severity 'critical' -Decision 'block' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Data isolation role dependency is missing its mapped capability.' -Evidence @('role=' + $role, 'capability=' + $requiredCapability) -Remediation 'Declare the mapped capability or remove the role dependency.'
                    }
                }
            }
        }
    }

    Add-Finding -RuleId 'FAE-F004' -Severity 'info' -Decision 'pass' -Category 'manifest' -Path (Get-RepoPath $path) -Message 'Manifest file was inspected against the selected profile.' -Evidence ([string]$manifest.moduleID) -Remediation $null
    return $manifest
}

function Get-ChangedFileList {
    $files = @()
    if (@($ChangedFile).Count -gt 0) {
        $files += @($ChangedFile)
    }
    if (-not [string]::IsNullOrWhiteSpace($ChangedFilesPath)) {
        $path = Resolve-InputPath -Path $ChangedFilesPath
        if (Test-Path -LiteralPath $path) {
            $files += @(Get-Content -LiteralPath $path | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        }
    }
    if ($files.Count -eq 0) {
        try {
            $gitFiles = & git -C $script:RepoRoot status --short 2>$null | ForEach-Object {
                if ($_ -match '^\s*\S+\s+(.+)$') { $Matches[1].Trim() }
            }
            $files += @($gitFiles)
        } catch {
            $files = @()
        }
    }
    return @($files | ForEach-Object { $_.Replace('\', '/') } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
}

function Get-ReadableChangedFiles {
    $paths = @()
    foreach ($relative in Get-ChangedFileList) {
        $path = Resolve-InputPath -Path $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $paths += $path
        }
    }
    return $paths
}

function Test-Dependencies {
    param([AllowNull()]$Profile)

    $files = Get-ReadableChangedFiles
    if ($files.Count -eq 0) {
        Add-Finding -RuleId "FAE-F013" -Severity "info" -Decision "pass" -Category "dependencies" -Message "No changed files supplied for dependency inspection." -Evidence "No changed files." -Remediation $null
        return
    }

    $violations = @()
    foreach ($file in $files) {
        $relative = Get-RepoPath $file
        $text = Get-Content -LiteralPath $file -Raw

        if ($Profile -and $Profile.edition -eq "apple") {
            if ($relative -match 'Sources/ForsettiCore/' -and $text -match 'import\s+(ForsettiPlatform|ForsettiModulesExample|ForsettiHostTemplate|SwiftUI|UIKit|AppKit|StoreKit|Combine)') {
                $violations += ($relative + " imports a forbidden Apple dependency from ForsettiCore")
            }
            if ($relative -match 'Sources/ForsettiPlatform/' -and $text -match 'import\s+(ForsettiModulesExample|ForsettiHostTemplate|SwiftUI|UIKit|AppKit)') {
                $violations += ($relative + " imports a forbidden Apple dependency from ForsettiPlatform")
            }
            if ($relative -match 'Sources/ForsettiModulesExample/' -and $text -match 'import\s+(ForsettiPlatform|ForsettiHostTemplate|SwiftUI|UIKit|AppKit|StoreKit)') {
                $violations += ($relative + " imports a forbidden Apple dependency from ForsettiModulesExample")
            }
            if ($relative -match 'Sources/ForsettiHostTemplate/' -and $text -match 'import\s+ForsettiModulesExample') {
                $violations += ($relative + " imports the internal example target from ForsettiHostTemplate")
            }
        }

        if ($relative -match 'include/ForsettiCore/|src/ForsettiCore/' -and $text -match '#include\s+[<"].*Forsetti(Platform|HostTemplate)') {
            $violations += ($relative + " includes upper-layer Windows product from ForsettiCore")
        }
        if ($text -match '(internal|private)\s+.*Forsetti' -and $relative -notmatch '(^Sources/Forsetti|^src/Forsetti|^include/Forsetti)') {
            $violations += ($relative + " appears to reference non-public Forsetti internals")
        }
    }

    if ($violations.Count -gt 0) {
        Add-Finding -RuleId "FAE-F013" -Severity "critical" -Decision "block" -Category "dependencies" -Message "Dependency direction or public API boundary violation detected." -Evidence $violations -Remediation "Use public Forsetti products only and preserve the selected profile's one-way dependency direction."
    } else {
        Add-Finding -RuleId "FAE-F013" -Severity "info" -Decision "pass" -Category "dependencies" -Message "Changed files do not show direct dependency boundary violations." -Evidence ("Checked " + $files.Count + " files.") -Remediation $null
    }
}

function Test-Capabilities {
    param([AllowNull()]$Manifest, [AllowNull()]$Profile)

    $declared = @()
    if ($Manifest -and $Manifest.capabilitiesRequested) {
        $declared = @($Manifest.capabilitiesRequested)
    }
    $files = Get-ReadableChangedFiles
    if ($files.Count -eq 0) {
        Add-Finding -RuleId "FAE-F009" -Severity "info" -Decision "pass" -Category "capabilities" -Message "No changed files supplied for capability inspection." -Evidence "No changed files." -Remediation $null
        return
    }

    $mapPath = Join-Path $script:RepoRoot "core/validator/rules/forsetti_project_rules.ps1"
    if (Test-Path -LiteralPath $mapPath) {
        . $mapPath
    }
    $selectedEdition = if ($Profile) { [string]$Profile.edition } else { $null }
    $capabilityMap = Get-ForsettiCapabilityUseMap -Edition $selectedEdition
    $used = New-Object System.Collections.Generic.HashSet[string]
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file -Raw
        foreach ($capability in $capabilityMap.Keys) {
            foreach ($pattern in @($capabilityMap[$capability])) {
                if ($text -match [regex]::Escape($pattern)) {
                    [void]$used.Add($capability)
                }
            }
        }
    }
    $undeclared = @($used | Where-Object { $declared -notcontains $_ })
    if ($Manifest -and $undeclared.Count -gt 0) {
        Add-Finding -RuleId "FAE-F009" -Severity "critical" -Decision "block" -Category "capabilities" -Message "Changed files use undeclared capabilities." -Evidence ($undeclared -join ", ") -Remediation "Declare each used capability in the module manifest or remove the capability-using behavior."
    } else {
        Add-Finding -RuleId "FAE-F009" -Severity "info" -Decision "pass" -Category "capabilities" -Message "Capability inspection completed." -Evidence ("used=" + (($used | Sort-Object) -join ",") + "; declared=" + ($declared -join ",")) -Remediation $null
    }
}

function Test-ModuleIsolation {
    $files = Get-ReadableChangedFiles
    if ($files.Count -eq 0) {
        Add-Finding -RuleId "FAE-F006" -Severity "info" -Decision "pass" -Category "module-isolation" -Message "No changed files supplied for module-isolation inspection." -Evidence "No changed files." -Remediation $null
        return
    }

    $violations = @()
    foreach ($file in $files) {
        $relative = Get-RepoPath $file
        $text = Get-Content -LiteralPath $file -Raw
        if ($text -match '(import|#include)\s+["<].*(Modules|ExampleModules).*(Module|Service|UI|App)') {
            $violations += ($relative + " imports or includes another module implementation")
        }
        if ($text -match '(sharedDatabase|SharedDatabase|moduleDatabase|directModule|OtherModule)') {
            $violations += ($relative + " appears to use direct module data or implementation coupling")
        }
    }

    if ($violations.Count -gt 0) {
        Add-Finding -RuleId "FAE-F006" -Severity "critical" -Decision "block" -Category "module-isolation" -Message "Direct module coupling was detected." -Evidence $violations -Remediation "Route intermodule interaction through Forsetti orchestration contracts and remove direct coupling."
    } else {
        Add-Finding -RuleId "FAE-F006" -Severity "info" -Decision "pass" -Category "module-isolation" -Message "Changed files do not show direct module coupling patterns." -Evidence ("Checked " + $files.Count + " files.") -Remediation $null
    }
}

function Test-Contract {
    if ([string]::IsNullOrWhiteSpace($ContractPath)) {
        if ($Mode -in @("contract", "contracts")) {
            Add-Finding -RuleId "FAE-C001" -Severity "critical" -Decision "block" -Category "contract" -Message "Contract mode requires -ContractPath." -Evidence "-ContractPath" -Remediation "Provide a governing task contract before execution."
        }
        return
    }
    $path = Resolve-InputPath -Path $ContractPath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Finding -RuleId "FAE-C001" -Severity "critical" -Decision "block" -Category "contract" -Path (Get-RepoPath $path) -Message "Contract file is missing." -Evidence $ContractPath -Remediation "Provide the governing task contract."
        return
    }
    if ($path.EndsWith(".json", [System.StringComparison]::OrdinalIgnoreCase)) {
        $contract = Read-JsonObject -Path $path
        if (-not $contract.PSObject.Properties["forsetti_project_context"]) {
            Add-Finding -RuleId "FAE-F001" -Severity "critical" -Decision "block" -Category "contract" -Path (Get-RepoPath $path) -Message "Task contract is missing Forsetti project context." -Evidence "forsetti_project_context" -Remediation "Add the required Forsetti project context before Builder execution."
        } else {
            $null = Test-ProjectContextObject -Context $contract.forsetti_project_context -SourcePath $path
        }
    } else {
        $text = Get-Content -LiteralPath $path -Raw
        if ($text -notmatch '##\s+Forsetti Project Context') {
            Add-Finding -RuleId "FAE-F001" -Severity "critical" -Decision "block" -Category "contract" -Path (Get-RepoPath $path) -Message "Markdown task contract is missing Forsetti project context section." -Evidence "## Forsetti Project Context" -Remediation "Add the required context section before Builder execution."
        }
    }
    Add-Finding -RuleId "FAE-C001" -Severity "info" -Decision "pass" -Category "contract" -Path (Get-RepoPath $path) -Message "Task contract was inspected." -Evidence (Get-RepoPath $path) -Remediation $null
}

function Test-Evidence {
    $files = Get-ChangedFileList
    if ($Mode -eq "evidence" -and $files.Count -eq 0 -and [string]::IsNullOrWhiteSpace($ChangedFilesPath)) {
        Add-Finding -RuleId "FAE-F020" -Severity "critical" -Decision "block" -Category "evidence" -Message "Evidence mode requires changed-file evidence." -Evidence "-ChangedFilesPath or -ChangedFiles" -Remediation "Provide changed-file evidence and selected profile validation evidence."
        return
    }
    Add-Finding -RuleId "FAE-F020" -Severity "info" -Decision "pass" -Category "evidence" -Message "Evidence inputs were inspected." -Evidence ("changed_files=" + $files.Count) -Remediation $null
}

function Invoke-SelectedChecks {
    $context = $null
    $profile = $null
    $manifest = $null

    if ($Mode -in @("all", "repo", "files", "structure", "json", "policies", "policy", "docs", "schemas", "schema", "scripts")) {
        Test-Repo
    }
    if ($Mode -in @("all", "contract", "contracts")) {
        Test-Contract
    }
    if ($Mode -in @("all", "project-context")) {
        $context = Test-ProjectContext
    }
    if (-not $profile -and $context) {
        $profilePath = Get-DefaultProfilePath -Context $context
        if ($profilePath -and (Test-Path -LiteralPath $profilePath)) {
            $profile = Read-JsonObject -Path $profilePath
            $null = Test-EditionProfileObject -Profile $profile -SourcePath $profilePath
        }
    }
    if ($Mode -in @("all", "edition-profile") -and -not $profile) {
        $profile = Test-EditionProfile
    } elseif (-not [string]::IsNullOrWhiteSpace($EditionProfilePath) -and -not $profile) {
        $profile = Test-EditionProfile
    }
    if ($context -and $profile) {
        $contextSource = if ([string]::IsNullOrWhiteSpace($ProjectContextPath)) { $null } else { Resolve-InputPath -Path $ProjectContextPath }
        Test-ProjectContextProfileAlignment -Context $context -Profile $profile -SourcePath $contextSource
    }
    if ($Mode -in @("manifest", "all") -or -not [string]::IsNullOrWhiteSpace($ManifestPath)) {
        $manifest = Test-Manifest -Profile $profile
    }
    if ($Mode -in @("dependencies", "all")) {
        Test-Dependencies -Profile $profile
    }
    if ($Mode -in @("capabilities", "all")) {
        Test-Capabilities -Manifest $manifest -Profile $profile
    }
    if ($Mode -in @("module-isolation", "all")) {
        Test-ModuleIsolation
    }
    if ($Mode -in @("evidence", "all")) {
        Test-Evidence
    }
}

function New-ValidatorResult {
    $findings = @($script:Findings.ToArray())
    $blockCount = @($findings | Where-Object { $_.decision -eq "block" }).Count
    $requestCount = @($findings | Where-Object { $_.decision -eq "request_changes" }).Count
    $passCount = @($findings | Where-Object { $_.decision -eq "pass" }).Count
    $status = "pass"
    if ($blockCount -gt 0) {
        $status = "block"
    } elseif ($requestCount -gt 0 -or ($Strict -and $requestCount -gt 0)) {
        $status = "request_changes"
    }
    return [pscustomobject][ordered]@{
        schema_version = "1.0"
        status         = $status
        mode           = $Mode
        validator      = [pscustomobject][ordered]@{
            name    = "forsetti_validate"
            version = "0.4.0"
        }
        invocation     = [pscustomobject][ordered]@{
            repo_root            = $script:RepoRoot
            mode                 = $Mode
            strict               = [bool]$Strict
            contract_path        = if ($ContractPath) { Get-RepoPath (Resolve-InputPath -Path $ContractPath) } else { $null }
            project_context_path = if ($ProjectContextPath) { Get-RepoPath (Resolve-InputPath -Path $ProjectContextPath) } else { $null }
            edition_profile_path = if ($EditionProfilePath) { Get-RepoPath (Resolve-InputPath -Path $EditionProfilePath) } else { $null }
            manifest_path        = if ($ManifestPath) { Get-RepoPath (Resolve-InputPath -Path $ManifestPath) } else { $null }
            changed_files_path   = if ($ChangedFilesPath) { Get-RepoPath (Resolve-InputPath -Path $ChangedFilesPath) } else { $null }
            timestamp_utc        = (Get-Date).ToUniversalTime().ToString("o")
        }
        summary        = [pscustomobject][ordered]@{
            status          = $status
            total           = $findings.Count
            passed          = $passCount
            request_changes = $requestCount
            blocks          = $blockCount
            duration_ms     = [int]((Get-Date) - $script:StartTime).TotalMilliseconds
        }
        findings       = $findings
    }
}

function Write-HumanResult {
    param([object]$Result)

    Write-Host "Forsetti Local Validator"
    Write-Host ("Repository: " + $Result.invocation.repo_root)
    Write-Host ("Mode: " + $Result.invocation.mode)
    Write-Host ("Status: " + $Result.status)
    foreach ($finding in $Result.findings) {
        $pathText = if ($finding.path) { " [" + $finding.path + "]" } else { "" }
        Write-Host (($finding.decision).ToUpperInvariant() + " " + $finding.rule_id + $pathText + " - " + $finding.message)
    }
}

$script:RepoRoot = Resolve-ValidationRoot -Path $RepoRoot
Invoke-SelectedChecks
$result = New-ValidatorResult

if (-not [string]::IsNullOrWhiteSpace($OutputJson)) {
    $jsonPath = Resolve-InputPath -Path $OutputJson
    $jsonDir = Split-Path -Parent $jsonPath
    if (-not [string]::IsNullOrWhiteSpace($jsonDir) -and -not (Test-Path -LiteralPath $jsonDir)) {
        New-Item -ItemType Directory -Path $jsonDir -Force | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false
    [System.IO.File]::WriteAllText($jsonPath, ($result | ConvertTo-Json -Depth 20), $utf8NoBom)
}

Write-HumanResult -Result $result

if ($result.status -eq "pass") {
    exit 0
}
if ($result.status -eq "request_changes") {
    exit 1
}
exit 2
