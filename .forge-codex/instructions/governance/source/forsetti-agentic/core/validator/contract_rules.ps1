# Contract enforcement rules for the Forsetti local validator.

function ConvertTo-ForsettiToken {
    param([AllowNull()][string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    return $Value.Trim().ToLowerInvariant().Replace(" ", "_")
}

function ConvertTo-ForsettiYesNo {
    param([AllowNull()][string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }

    return ($Value.Trim() -match '^(yes|true|required)$')
}

function ConvertTo-ForsettiRepoPath {
    param(
        [string]$RepoRoot,
        [AllowNull()][string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    $cleanPath = $Path.Trim().Trim('"').Trim("'")
    if ([string]::IsNullOrWhiteSpace($cleanPath)) {
        return $null
    }

    try {
        $rootFull = [System.IO.Path]::GetFullPath($RepoRoot)
        $rootTrim = $rootFull.TrimEnd(@([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar))
        $fullPath = $cleanPath
        if (-not [System.IO.Path]::IsPathRooted($cleanPath)) {
            $fullPath = Join-Path $rootTrim $cleanPath
        }

        $fullPath = [System.IO.Path]::GetFullPath($fullPath)
        $rootWithSeparator = $rootTrim + [System.IO.Path]::DirectorySeparatorChar

        if ($fullPath.Equals($rootTrim, [System.StringComparison]::OrdinalIgnoreCase)) {
            return ""
        }

        if ($fullPath.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $fullPath.Substring($rootWithSeparator.Length).Replace('\', '/')
        }
    } catch {
        # Fall through to lexical normalization for invalid or non-filesystem inputs.
    }

    $lexical = $cleanPath.Replace('\', '/')
    while ($lexical.StartsWith("./", [System.StringComparison]::Ordinal)) {
        $lexical = $lexical.Substring(2)
    }
    return $lexical.TrimStart('/')
}

function ConvertTo-ForsettiGlobRegex {
    param([string]$Pattern)

    $normalized = $Pattern.Replace('\', '/').Trim()
    while ($normalized.StartsWith("./", [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    if ($normalized.EndsWith("/", [System.StringComparison]::Ordinal)) {
        $normalized = $normalized + "**"
    }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append("^")
    for ($i = 0; $i -lt $normalized.Length; $i++) {
        $ch = [string]$normalized[$i]
        if ($ch -eq "*") {
            if (($i + 1) -lt $normalized.Length -and [string]$normalized[$i + 1] -eq "*") {
                [void]$builder.Append(".*")
                $i++
            } else {
                [void]$builder.Append("[^/]*")
            }
        } elseif ($ch -eq "?") {
            [void]$builder.Append("[^/]")
        } else {
            [void]$builder.Append([regex]::Escape($ch))
        }
    }
    [void]$builder.Append("$")
    return $builder.ToString()
}

function Test-ForsettiPathPattern {
    param(
        [string]$Path,
        [string]$Pattern
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Pattern)) {
        return $false
    }

    $pathValue = $Path.Replace('\', '/').TrimStart('/')
    $patternValue = $Pattern.Replace('\', '/').Trim()
    while ($patternValue.StartsWith("./", [System.StringComparison]::Ordinal)) {
        $patternValue = $patternValue.Substring(2)
    }

    if ($patternValue.IndexOfAny(@([char]'*', [char]'?')) -lt 0 -and -not $patternValue.EndsWith("/", [System.StringComparison]::Ordinal)) {
        return $pathValue.Equals($patternValue.TrimStart('/'), [System.StringComparison]::OrdinalIgnoreCase)
    }

    $regex = ConvertTo-ForsettiGlobRegex -Pattern $patternValue
    return [regex]::IsMatch($pathValue, $regex, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

function Get-ForsettiMarkdownSection {
    param(
        [string[]]$Lines,
        [string]$Title,
        [int]$Level
    )

    $startIndex = -1
    $headerPattern = ('^#{' + $Level + '}\s+' + [regex]::Escape($Title) + '\s*$')
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match $headerPattern) {
            $startIndex = $i + 1
            break
        }
    }

    if ($startIndex -lt 0) {
        return @()
    }

    $items = New-Object System.Collections.Generic.List[string]
    for ($i = $startIndex; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^(#{1,6})\s+') {
            if ($Matches[1].Length -le $Level) {
                break
            }
        }
        $items.Add($Lines[$i])
    }
    return @($items.ToArray())
}

function Get-ForsettiMarkdownBullets {
    param([string[]]$Lines)

    $items = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if ($line -match '^\s*-\s+(.*)\s*$') {
            $value = $Matches[1].Trim()
            $value = ($value -replace '^\[[ xX]\]\s*', '').Trim()
            $value = $value.Trim([char]0x60)
            if (-not [string]::IsNullOrWhiteSpace($value)) {
                $items.Add($value)
            }
        }
    }
    return @($items.ToArray())
}

function Get-ForsettiInlineValue {
    param(
        [string]$Content,
        [string]$Label
    )

    $pattern = '(?m)^\*\*' + [regex]::Escape($Label) + ':\*\*\s*(.*?)\s*$'
    if ($Content -match $pattern) {
        return $Matches[1].Trim()
    }
    return $null
}

function Read-ForsettiJsonContract {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    $contractObject = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $scopeObject = $contractObject.scope
    if (-not $scopeObject -and $contractObject.in_scope) {
        $scopeObject = [pscustomobject]@{
            in_scope = @($contractObject.in_scope)
            out_of_scope = @($contractObject.out_of_scope)
        }
    }

    return [pscustomobject][ordered]@{
        format                     = "json"
        path                       = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $Path
        task_id                    = [string]$contractObject.task_id
        title                      = [string]$contractObject.title
        date                       = [string]$contractObject.date
        contract_authoring_role    = ConvertTo-ForsettiToken $contractObject.contract_authoring_role
        acting_role                = ConvertTo-ForsettiToken $contractObject.acting_role
        reviewer_role              = ConvertTo-ForsettiToken $contractObject.reviewer_role
        release_review_role        = ConvertTo-ForsettiToken $contractObject.release_review_role
        documentation_review_role  = ConvertTo-ForsettiToken $contractObject.documentation_review_role
        required_advisory_reviewers = @($contractObject.required_advisory_reviewers)
        change_class               = ConvertTo-ForsettiToken $contractObject.change_class
        approval_class             = ConvertTo-ForsettiToken $contractObject.approval_class
        governance_authorization   = $contractObject.governance_authorization
        objective                  = [string]$contractObject.objective
        business_reason            = [string]$contractObject.business_reason
        in_scope                   = @($scopeObject.in_scope)
        out_of_scope               = @($scopeObject.out_of_scope)
        required_outputs           = @($contractObject.required_outputs)
        documentation_impact       = $contractObject.documentation_impact
        release_impact             = ConvertTo-ForsettiToken $contractObject.release_impact
        validation_requirements    = @($contractObject.validation_requirements)
        evidence_requirements      = @($contractObject.evidence_requirements)
        constraints                = @($contractObject.constraints)
        risks                      = @($contractObject.risks)
        escalation_triggers        = @($contractObject.escalation_triggers)
        definition_of_done         = @($contractObject.definition_of_done)
        completion_summary_requirements = @($contractObject.completion_summary_requirements)
    }
}

function Read-ForsettiMarkdownContract {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    $content = Get-Content -LiteralPath $Path -Raw
    $lines = $content -split "`r?`n"
    $authContent = (Get-ForsettiMarkdownSection -Lines $lines -Title "Governance Steward Authorization" -Level 2) -join "`n"

    $title = $null
    if ($content -match '(?m)^#\s+Task Contract:\s*(.*?)\s*$') {
        $title = $Matches[1].Trim()
    }

    $documentationImpact = [pscustomobject][ordered]@{
        readme_update    = ConvertTo-ForsettiYesNo (Get-ForsettiInlineValue -Content $content -Label "README update required")
        wiki_update      = ConvertTo-ForsettiYesNo (Get-ForsettiInlineValue -Content $content -Label "Wiki update required")
        glossary_update  = ConvertTo-ForsettiYesNo (Get-ForsettiInlineValue -Content $content -Label "Glossary update required")
        changelog_update = ConvertTo-ForsettiYesNo (Get-ForsettiInlineValue -Content $content -Label "Changelog entry required")
        rationale        = "Parsed from Markdown documentation impact section."
    }

    $governanceAuthorization = [pscustomobject][ordered]@{
        required  = ConvertTo-ForsettiYesNo (Get-ForsettiInlineValue -Content $authContent -Label "Required")
        authority = Get-ForsettiInlineValue -Content $authContent -Label "Authority"
        evidence  = Get-ForsettiInlineValue -Content $authContent -Label "Evidence"
    }

    return [pscustomobject][ordered]@{
        format                     = "markdown"
        path                       = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $Path
        task_id                    = Get-ForsettiInlineValue -Content $content -Label "Task ID"
        title                      = $title
        date                       = Get-ForsettiInlineValue -Content $content -Label "Created"
        contract_authoring_role    = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Contract Authoring Role")
        acting_role                = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Acting Role")
        reviewer_role              = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Reviewer Role")
        release_review_role        = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Release Review Role")
        documentation_review_role  = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Documentation Review Role")
        required_advisory_reviewers = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Required Advisory Reviewers" -Level 2)
        change_class               = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Change Class")
        approval_class             = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Approval Class")
        governance_authorization   = $governanceAuthorization
        objective                  = ((Get-ForsettiMarkdownSection -Lines $lines -Title "Objective" -Level 2) -join "`n").Trim()
        business_reason            = ((Get-ForsettiMarkdownSection -Lines $lines -Title "Business Reason" -Level 2) -join "`n").Trim()
        in_scope                   = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "In Scope" -Level 3)
        out_of_scope               = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Out of Scope" -Level 3)
        required_outputs           = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Required Outputs" -Level 2)
        documentation_impact       = $documentationImpact
        release_impact             = ConvertTo-ForsettiToken (Get-ForsettiInlineValue -Content $content -Label "Release Impact")
        validation_requirements    = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Validation Requirements" -Level 2)
        evidence_requirements      = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Evidence Requirements" -Level 2)
        constraints                = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Constraints" -Level 2)
        risks                      = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Risks" -Level 2)
        escalation_triggers        = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Escalation Triggers" -Level 2)
        definition_of_done         = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Definition of Done" -Level 2)
        completion_summary_requirements = Get-ForsettiMarkdownBullets (Get-ForsettiMarkdownSection -Lines $lines -Title "Completion Summary Requirements" -Level 2)
    }
}

function Read-ForsettiTaskContract {
    param(
        [string]$RepoRoot,
        [string]$ContractPath
    )

    $resolved = $ContractPath
    if (-not [System.IO.Path]::IsPathRooted($resolved)) {
        $resolved = Join-Path $RepoRoot $resolved
    }
    $resolved = [System.IO.Path]::GetFullPath($resolved)

    if (-not (Test-Path -LiteralPath $resolved)) {
        Add-Finding `
            -FindingId "FFAE-CONTRACT-001" `
            -Severity "error" `
            -Status "fail" `
            -Category "contract" `
            -Path (ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $ContractPath) `
            -Message "Task contract path does not exist." `
            -Evidence $ContractPath `
            -RemediationHint "Create the task contract before execution." `
            -RuleId "FAE-C001"
        return $null
    }

    try {
        if ($resolved.EndsWith(".json", [System.StringComparison]::OrdinalIgnoreCase)) {
            return Read-ForsettiJsonContract -RepoRoot $RepoRoot -Path $resolved
        }
        return Read-ForsettiMarkdownContract -RepoRoot $RepoRoot -Path $resolved
    } catch {
        Add-Finding `
            -FindingId "FFAE-CONTRACT-002" `
            -Severity "error" `
            -Status "fail" `
            -Category "contract" `
            -Path (ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $resolved) `
            -Message "Task contract could not be parsed." `
            -Evidence $_.Exception.Message `
            -RemediationHint "Fix the task contract syntax and required fields." `
            -RuleId "FAE-C001"
        return $null
    }
}

function Get-ForsettiChangedFiles {
    param(
        [string]$RepoRoot,
        [AllowNull()][string[]]$ChangedFile,
        [AllowNull()][string]$ChangedFilesPath
    )

    $items = New-Object System.Collections.Generic.List[string]

    foreach ($path in @($ChangedFile)) {
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            $items.Add($path)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ChangedFilesPath)) {
        $resolved = $ChangedFilesPath
        if (-not [System.IO.Path]::IsPathRooted($resolved)) {
            $resolved = Join-Path $RepoRoot $resolved
        }
        if (Test-Path -LiteralPath $resolved) {
            foreach ($line in Get-Content -LiteralPath $resolved) {
                if (-not [string]::IsNullOrWhiteSpace($line)) {
                    $items.Add($line.Trim())
                }
            }
        } else {
            Add-Finding `
                -FindingId "FFAE-CONTRACT-003" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path (ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $ChangedFilesPath) `
                -Message "Changed-file input path does not exist." `
                -Evidence $ChangedFilesPath `
                -RemediationHint "Provide an existing newline-delimited changed-file list." `
                -RuleId "FAE-C011"
        }
    }

    if ($items.Count -eq 0) {
        try {
            $statusLines = @(git -C $RepoRoot status --porcelain=v1)
            foreach ($line in $statusLines) {
                if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) {
                    continue
                }
                $pathText = $line.Substring(3).Trim()
                if ($pathText -match '\s+->\s+') {
                    foreach ($part in ($pathText -split '\s+->\s+')) {
                        if (-not [string]::IsNullOrWhiteSpace($part)) {
                            $items.Add($part.Trim())
                        }
                    }
                } else {
                    $items.Add($pathText)
                }
            }
        } catch {
            Add-Finding `
                -FindingId "FFAE-CONTRACT-004" `
                -Severity "warning" `
                -Status "warn" `
                -Category "contract" `
                -Path $null `
                -Message "Changed files were not supplied and could not be inferred from local git status." `
                -Evidence $_.Exception.Message `
                -RemediationHint "Provide -ChangedFile or -ChangedFilesPath for reproducible contract validation." `
                -RuleId "FAE-C011"
        }
    }

    $normalized = New-Object System.Collections.Generic.List[string]
    foreach ($item in $items) {
        $candidate = $item.Trim()
        $candidateFull = $candidate
        if (-not [System.IO.Path]::IsPathRooted($candidateFull)) {
            $candidateFull = Join-Path $RepoRoot $candidateFull
        }

        if ((Test-Path -LiteralPath $candidateFull -PathType Container)) {
            foreach ($file in Get-ChildItem -LiteralPath $candidateFull -Recurse -File) {
                $path = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $file.FullName
                if (-not [string]::IsNullOrWhiteSpace($path) -and -not $normalized.Contains($path)) {
                    $normalized.Add($path)
                }
            }
        } else {
            $path = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $candidate
            if (-not [string]::IsNullOrWhiteSpace($path) -and -not $normalized.Contains($path)) {
                $normalized.Add($path)
            }
        }
    }

    return @($normalized.ToArray())
}

function Get-ForsettiApprovalRank {
    param([string]$ApprovalClass)

    switch ($ApprovalClass) {
        "standard" { return 1 }
        "sensitive" { return 2 }
        "governance-class" { return 3 }
        default { return 0 }
    }
}

function Test-ForsettiApprovalSufficient {
    param(
        [string]$Actual,
        [string]$Required
    )

    $actualValue = ConvertTo-ForsettiToken $Actual
    $requiredValue = ConvertTo-ForsettiToken $Required

    if ([string]::IsNullOrWhiteSpace($requiredValue)) {
        return $true
    }
    if ($actualValue -eq "governance-class") {
        return $true
    }
    if ($requiredValue -eq "governance-class") {
        return $false
    }
    if ($actualValue -eq "emergency" -and ($requiredValue -eq "standard" -or $requiredValue -eq "sensitive")) {
        return $true
    }
    if ($actualValue -eq "release-critical") {
        return ($requiredValue -eq "release-critical")
    }

    return ((Get-ForsettiApprovalRank $actualValue) -ge (Get-ForsettiApprovalRank $requiredValue))
}

function Get-ForsettiProtectedPathRules {
    param([string]$RepoRoot)

    $rules = New-Object System.Collections.Generic.List[object]
    $policyPath = Join-Path $RepoRoot "core/policies/repo-boundaries.json"
    if (-not (Test-Path -LiteralPath $policyPath)) {
        Add-Finding `
            -FindingId "FFAE-APPROVAL-001" `
            -Severity "error" `
            -Status "fail" `
            -Category "approval" `
            -Path "core/policies/repo-boundaries.json" `
            -Message "Repository boundary policy is missing." `
            -Evidence "core/policies/repo-boundaries.json" `
            -RemediationHint "Restore the boundary policy before protected-path validation." `
            -RuleId "FAE-C004"
        return $null
    }

    try {
        $policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
        foreach ($mapping in @($policy.approval_required_paths.mappings)) {
            $rules.Add([pscustomobject][ordered]@{
                pattern = [string]$mapping.path_pattern
                minimum_approval_class = ConvertTo-ForsettiToken $mapping.minimum_approval_class
                policy_rule_id = [string]$mapping.policy_rule_id
                gate_id = [string]$mapping.gate_id
                source = "core/policies/repo-boundaries.json"
            })
        }
    } catch {
        Add-Finding `
            -FindingId "FFAE-APPROVAL-002" `
            -Severity "error" `
            -Status "fail" `
            -Category "approval" `
            -Path "core/policies/repo-boundaries.json" `
            -Message "Repository boundary policy could not be parsed." `
            -Evidence $_.Exception.Message `
            -RemediationHint "Fix the boundary policy JSON before protected-path validation." `
            -RuleId "FAE-C004"
        return $null
    }

    return @($rules.ToArray())
}

function Get-ForsettiRoleLimitedPathRules {
    param([string]$RepoRoot)

    $rules = New-Object System.Collections.Generic.List[object]
    $policyPath = Join-Path $RepoRoot "core/policies/repo-boundaries.json"
    if (-not (Test-Path -LiteralPath $policyPath)) {
        Add-Finding `
            -FindingId "FFAE-ROLE-001" `
            -Severity "error" `
            -Status "fail" `
            -Category "approval" `
            -Path "core/policies/repo-boundaries.json" `
            -Message "Repository boundary policy is missing." `
            -Evidence "core/policies/repo-boundaries.json" `
            -RemediationHint "Restore the boundary policy before role-limited path validation." `
            -RuleId "FAE-C003"
        return $null
    }

    try {
        $policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
        foreach ($mapping in @($policy.role_limited_paths.mappings)) {
            $rules.Add([pscustomobject][ordered]@{
                pattern = [string]$mapping.path_pattern
                allowed_roles = @($mapping.allowed_roles | ForEach-Object { ConvertTo-ForsettiToken $_ })
                policy_rule_id = [string]$mapping.policy_rule_id
                gate_id = [string]$mapping.gate_id
                source = "core/policies/repo-boundaries.json"
            })
        }
    } catch {
        Add-Finding `
            -FindingId "FFAE-ROLE-002" `
            -Severity "error" `
            -Status "fail" `
            -Category "approval" `
            -Path "core/policies/repo-boundaries.json" `
            -Message "Repository boundary policy could not be parsed for role-limited path validation." `
            -Evidence $_.Exception.Message `
            -RemediationHint "Fix the boundary policy JSON before role-limited path validation." `
            -RuleId "FAE-C003"
        return $null
    }

    return @($rules.ToArray())
}

function Get-ForsettiRequiredApproval {
    param(
        [string]$Path,
        [object[]]$Rules
    )

    $matches = New-Object System.Collections.Generic.List[object]
    foreach ($rule in @($Rules)) {
        if (Test-ForsettiPathPattern -Path $Path -Pattern $rule.pattern) {
            $matches.Add($rule)
        }
    }

    if ($matches.Count -eq 0) {
        return $null
    }

    $winner = $null
    foreach ($match in @($matches.ToArray())) {
        if (-not $winner -or (Get-ForsettiApprovalRank $match.minimum_approval_class) -gt (Get-ForsettiApprovalRank $winner.minimum_approval_class)) {
            $winner = $match
        }
    }
    return $winner
}

function Test-ForsettiContractShape {
    param([object]$Contract)

    $requiredScalarFields = @(
        "task_id",
        "title",
        "date",
        "contract_authoring_role",
        "acting_role",
        "reviewer_role",
        "change_class",
        "approval_class",
        "objective",
        "business_reason",
        "release_impact"
    )

    $errors = 0
    foreach ($field in $requiredScalarFields) {
        if ([string]::IsNullOrWhiteSpace([string]$Contract.$field)) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-CONTRACT-005" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path $Contract.path `
                -Message ("Task contract missing required field: " + $field) `
                -Evidence $field `
                -RemediationHint "Complete all required task contract fields before execution." `
                -RuleId "FAE-C001"
        }
    }

    $requiredArrayFields = @(
        "in_scope",
        "required_outputs",
        "validation_requirements",
        "evidence_requirements",
        "definition_of_done",
        "completion_summary_requirements"
    )
    foreach ($field in $requiredArrayFields) {
        if (@($Contract.$field).Count -eq 0) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-CONTRACT-006" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path $Contract.path `
                -Message ("Task contract missing required list: " + $field) `
                -Evidence $field `
                -RemediationHint "Add the required list items to the task contract." `
                -RuleId "FAE-C001"
        }
    }

    if ($Contract.approval_class -eq "governance-class") {
        $authorization = $Contract.governance_authorization
        if (-not $authorization -or -not [bool]$authorization.required -or [string]::IsNullOrWhiteSpace([string]$authorization.authority) -or [string]::IsNullOrWhiteSpace([string]$authorization.evidence)) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-APPROVAL-003" `
                -Severity "error" `
                -Status "fail" `
                -Category "approval" `
                -Path $Contract.path `
                -Message "Governance-class contract lacks Governance Steward authorization evidence." `
                -Evidence "approval_class=governance-class" `
                -RemediationHint "Record the Governance Steward authority and approval evidence in the task contract." `
                -RuleId "FAE-C004"
        }
    }

    if ($errors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-CONTRACT-005" `
            -Severity "info" `
            -Status "pass" `
            -Category "contract" `
            -Path $Contract.path `
            -Message "Task contract contains required governance fields." `
            -Evidence ("task_id=" + $Contract.task_id + "; approval_class=" + $Contract.approval_class) `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-ForsettiContractScope {
    param(
        [object]$Contract,
        [string[]]$ChangedFiles
    )

    if (@($ChangedFiles).Count -eq 0) {
        Add-Finding `
            -FindingId "FFAE-SCOPE-001" `
            -Severity "warning" `
            -Status "warn" `
            -Category "scope" `
            -Path $Contract.path `
            -Message "No changed-file set was supplied or inferred for contract scope validation." `
            -Evidence "changed_files=0" `
            -RemediationHint "Provide -ChangedFile or -ChangedFilesPath for reproducible scope enforcement." `
            -RuleId "FAE-C002"
        return
    }

    $outOfScopeErrors = 0
    foreach ($path in @($ChangedFiles)) {
        $matchesInScope = $false
        foreach ($scopePattern in @($Contract.in_scope)) {
            if (Test-ForsettiPathPattern -Path $path -Pattern $scopePattern) {
                $matchesInScope = $true
                break
            }
        }

        $matchesOutOfScope = $false
        foreach ($scopePattern in @($Contract.out_of_scope)) {
            if (Test-ForsettiPathPattern -Path $path -Pattern $scopePattern) {
                $matchesOutOfScope = $true
                break
            }
        }

        if (-not $matchesInScope -or $matchesOutOfScope) {
            $outOfScopeErrors++
            Add-Finding `
                -FindingId "FFAE-SCOPE-002" `
                -Severity "error" `
                -Status "fail" `
                -Category "scope" `
                -Path $path `
                -Message "Changed file is not authorized by the task contract scope." `
                -Evidence ("in_scope_match=" + $matchesInScope + "; out_of_scope_match=" + $matchesOutOfScope) `
                -RemediationHint "Remove the change or amend the task contract before modifying this path." `
                -RuleId "FAE-C002"
        }
    }

    if ($outOfScopeErrors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-SCOPE-002" `
            -Severity "info" `
            -Status "pass" `
            -Category "scope" `
            -Path $Contract.path `
            -Message "Changed files are authorized by the task contract scope." `
            -Evidence ("Checked " + @($ChangedFiles).Count + " changed paths.") `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-ForsettiProtectedApprovals {
    param(
        [string]$RepoRoot,
        [object]$Contract,
        [string[]]$ChangedFiles
    )

    $rules = Get-ForsettiProtectedPathRules -RepoRoot $RepoRoot
    if ($null -eq $rules) {
        return
    }

    $protectedCount = 0
    $errors = 0
    foreach ($path in @($ChangedFiles)) {
        $required = Get-ForsettiRequiredApproval -Path $path -Rules $rules
        if (-not $required) {
            continue
        }

        $protectedCount++
        if (-not (Test-ForsettiApprovalSufficient -Actual $Contract.approval_class -Required $required.minimum_approval_class)) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-APPROVAL-004" `
                -Severity "error" `
                -Status "fail" `
                -Category "approval" `
                -Path $path `
                -Message "Changed protected path lacks the required approval class." `
                -Evidence ("required=" + $required.minimum_approval_class + "; actual=" + $Contract.approval_class + "; source=" + $required.source + "; pattern=" + $required.pattern) `
                -RemediationHint "Raise the approval class or remove the protected-path change." `
                -RuleId "FAE-C004" `
                -PolicyRuleId $required.policy_rule_id `
                -ConditionId "protected_path_without_required_approval" `
                -GateId $required.gate_id
            continue
        }

        if ($required.minimum_approval_class -eq "governance-class") {
            $authorization = $Contract.governance_authorization
            if (-not $authorization -or -not [bool]$authorization.required -or [string]::IsNullOrWhiteSpace([string]$authorization.authority) -or [string]::IsNullOrWhiteSpace([string]$authorization.evidence)) {
                $errors++
                Add-Finding `
                    -FindingId "FFAE-APPROVAL-005" `
                    -Severity "error" `
                    -Status "fail" `
                    -Category "approval" `
                    -Path $path `
                    -Message "Governance-class protected path lacks Governance Steward authorization evidence." `
                    -Evidence ("required=" + $required.minimum_approval_class + "; pattern=" + $required.pattern) `
                    -RemediationHint "Record Governance Steward authorization evidence in the task contract." `
                    -RuleId "FAE-C004" `
                    -PolicyRuleId $required.policy_rule_id `
                    -ConditionId "governance_authorization_missing" `
                    -GateId $required.gate_id
            }
        }
    }

    if ($errors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-APPROVAL-004" `
            -Severity "info" `
            -Status "pass" `
            -Category "approval" `
            -Path $Contract.path `
            -Message "Protected-path approval checks passed." `
            -Evidence ("Protected paths checked: " + $protectedCount + "; approval_class=" + $Contract.approval_class) `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-ForsettiRoleLimitedPaths {
    param(
        [string]$RepoRoot,
        [object]$Contract,
        [string[]]$ChangedFiles
    )

    $rules = Get-ForsettiRoleLimitedPathRules -RepoRoot $RepoRoot
    if ($null -eq $rules) {
        return
    }

    $limitedCount = 0
    $errors = 0
    $actingRole = ConvertTo-ForsettiToken $Contract.acting_role

    foreach ($path in @($ChangedFiles)) {
        foreach ($rule in @($rules)) {
            if (-not (Test-ForsettiPathPattern -Path $path -Pattern $rule.pattern)) {
                continue
            }

            $limitedCount++
            if (@($rule.allowed_roles) -notcontains $actingRole) {
                $errors++
                Add-Finding `
                    -FindingId "FFAE-ROLE-003" `
                    -Severity "error" `
                    -Status "fail" `
                    -Category "approval" `
                    -Path $path `
                    -Message "Changed role-limited path is not authorized for the task contract acting role." `
                    -Evidence ("acting_role=" + $actingRole + "; allowed_roles=" + (@($rule.allowed_roles) -join ",") + "; source=" + $rule.source + "; pattern=" + $rule.pattern) `
                    -RemediationHint "Use an authorized acting role, amend the contract, or remove the role-limited path change." `
                    -RuleId "FAE-C003" `
                    -PolicyRuleId $rule.policy_rule_id `
                    -ConditionId "role_not_allowed_for_changed_path" `
                    -GateId $rule.gate_id
            }
        }
    }

    if ($errors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-ROLE-003" `
            -Severity "info" `
            -Status "pass" `
            -Category "approval" `
            -Path $Contract.path `
            -Message "Role-limited path checks passed." `
            -Evidence ("Role-limited path matches checked: " + $limitedCount + "; acting_role=" + $actingRole) `
            -RemediationHint $null `
            -RuleId $null `
            -PolicyRuleId "BOUNDARY-ROLE-LIMITED-PATHS" `
            -ConditionId $null `
            -GateId "boundary-role-limited-paths"
    }
}

function Test-ForsettiRequiredOutputs {
    param(
        [string]$RepoRoot,
        [object]$Contract,
        [AllowNull()][string]$PendingOutputPath
    )

    $errors = 0
    $pendingOutput = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $PendingOutputPath
    foreach ($output in @($Contract.required_outputs)) {
        $repoPath = ConvertTo-ForsettiRepoPath -RepoRoot $RepoRoot -Path $output
        $fullPath = Join-Path $RepoRoot $repoPath
        if ($pendingOutput -and $repoPath.Equals($pendingOutput, [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (-not (Test-Path -LiteralPath $fullPath)) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-EVIDENCE-001" `
                -Severity "error" `
                -Status "fail" `
                -Category "evidence" `
                -Path $repoPath `
                -Message "Required output is missing." `
                -Evidence $repoPath `
                -RemediationHint "Produce the required output before claiming completion." `
                -RuleId "FAE-C011"
        }
    }

    if ($errors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-EVIDENCE-001" `
            -Severity "info" `
            -Status "pass" `
            -Category "evidence" `
            -Path $Contract.path `
            -Message "Required outputs exist or are pending from the current validator invocation." `
            -Evidence ("Checked " + @($Contract.required_outputs).Count + " required outputs.") `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-ForsettiDocsSyncForChangedFiles {
    param(
        [string]$RepoRoot,
        [string[]]$ChangedFiles
    )

    $warnings = 0
    $policyPath = Join-Path $RepoRoot "core/policies/docs-sync-rules.json"
    if (-not (Test-Path -LiteralPath $policyPath)) {
        Add-Finding `
            -FindingId "FFAE-DOCS-005" `
            -Severity "warning" `
            -Status "warn" `
            -Category "docs" `
            -Path "core/policies/docs-sync-rules.json" `
            -Message "Documentation sync policy is missing, so changed canonical sources cannot be checked." `
            -Evidence "core/policies/docs-sync-rules.json" `
            -RemediationHint "Restore the docs sync policy manifest." `
            -RuleId "FAE-C008" `
            -PolicyRuleId "DOCSYNC-POLICY-MISSING" `
            -ConditionId "docs_sync_policy_missing" `
            -GateId "docs-sync-changed-source"
        return 1
    }

    try {
        $sync = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
        foreach ($pair in @($sync.sync_pairs)) {
            $triggered = $false
            foreach ($trigger in @($pair.trigger_paths)) {
                foreach ($changed in @($ChangedFiles)) {
                    if (Test-ForsettiPathPattern -Path $changed -Pattern ([string]$trigger)) {
                        $triggered = $true
                        break
                    }
                }
                if ($triggered) { break }
            }

            if (-not $triggered) {
                continue
            }

            foreach ($derived in @($pair.required_derived_paths)) {
                $derivedPath = [string]$derived
                if ($ChangedFiles -notcontains $derivedPath) {
                    $warnings++
                    Add-Finding `
                        -FindingId "FFAE-DOCS-005" `
                        -Severity "warning" `
                        -Status "warn" `
                        -Category "docs" `
                        -Path $derivedPath `
                        -Message "Changed canonical source requires derived documentation sync in the same change or an approved deferral." `
                        -Evidence ("canonical=" + [string]$pair.canonical + "; derived=" + $derivedPath) `
                        -RemediationHint "Update the derived documentation path or record an approved docs sync deferral." `
                        -RuleId "FAE-C008" `
                        -PolicyRuleId ([string]$pair.rule_id) `
                        -ConditionId ([string]$pair.rejection_condition) `
                        -GateId "docs-sync-changed-source"
                }
            }
        }
    } catch {
        $warnings++
        Add-Finding `
            -FindingId "FFAE-DOCS-006" `
            -Severity "warning" `
            -Status "warn" `
            -Category "docs" `
            -Path "core/policies/docs-sync-rules.json" `
            -Message "Documentation sync policy could not be evaluated." `
            -Evidence $_.Exception.Message `
            -RemediationHint "Fix the docs sync policy JSON before contract validation." `
            -RuleId "FAE-C008" `
            -PolicyRuleId "DOCSYNC-POLICY-EVALUATION" `
            -ConditionId "docs_sync_policy_parse_error" `
            -GateId "docs-sync-changed-source"
    }

    return $warnings
}

function Get-ForsettiChangelogEntry {
    param(
        [string]$RepoRoot,
        [string]$TaskId
    )

    $changelogPath = Join-Path $RepoRoot "changelog/CHANGELOG.md"
    if (-not (Test-Path -LiteralPath $changelogPath)) {
        return [pscustomobject][ordered]@{
            found = $false
            text = ""
            task_index = -1
            is_unreleased = $false
        }
    }

    $lines = @(Get-Content -LiteralPath $changelogPath)
    $taskIndex = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match [regex]::Escape($TaskId)) {
            $taskIndex = $i
            break
        }
    }

    if ($taskIndex -lt 0) {
        return [pscustomobject][ordered]@{
            found = $false
            text = ""
            task_index = -1
            is_unreleased = $false
        }
    }

    $start = $taskIndex
    while ($start -gt 0 -and $lines[$start] -notmatch '^\*\*Title\*\*:') {
        $start--
    }
    if ($lines[$start] -notmatch '^\*\*Title\*\*:') {
        $start = $taskIndex
    }

    $end = $start + 1
    while ($end -lt $lines.Count) {
        if ($end -gt $start -and ($lines[$end] -match '^\*\*Title\*\*:' -or $lines[$end] -match '^## \[')) {
            break
        }
        $end++
    }

    $unreleasedIndex = -1
    $nextReleaseIndex = $lines.Count
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^## \[Unreleased\]') {
            $unreleasedIndex = $i
            continue
        }
        if ($unreleasedIndex -ge 0 -and $i -gt $unreleasedIndex -and $lines[$i] -match '^## \[') {
            $nextReleaseIndex = $i
            break
        }
    }

    return [pscustomobject][ordered]@{
        found = $true
        text = (($lines[$start..($end - 1)]) -join "`n")
        task_index = $taskIndex
        is_unreleased = ($unreleasedIndex -ge 0 -and $taskIndex -gt $unreleasedIndex -and $taskIndex -lt $nextReleaseIndex)
    }
}

function Test-ForsettiChangelogEntryRequirements {
    param(
        [string]$RepoRoot,
        [object]$Contract
    )

    $warnings = 0
    $entry = Get-ForsettiChangelogEntry -RepoRoot $RepoRoot -TaskId $Contract.task_id
    if (-not $entry.found) {
        Add-Finding `
            -FindingId "FFAE-CHANGELOG-003" `
            -Severity "warning" `
            -Status "warn" `
            -Category "changelog" `
            -Path "changelog/CHANGELOG.md" `
            -Message "Required changelog entry for the task contract was not found." `
            -Evidence ("task_id=" + $Contract.task_id) `
            -RemediationHint "Add a changelog entry containing the task reference and required fields." `
            -RuleId "FAE-C005" `
            -PolicyRuleId "CHANGELOG-REQUIRED-FIELDS" `
            -ConditionId "missing_entry_for_required_class" `
            -GateId "changelog-entry-required"
        return 1
    }

    if (-not $entry.is_unreleased) {
        $warnings++
        Add-Finding `
            -FindingId "FFAE-CHANGELOG-004" `
            -Severity "warning" `
            -Status "warn" `
            -Category "changelog" `
            -Path "changelog/CHANGELOG.md" `
            -Message "New changelog entry must be located under the Unreleased section." `
            -Evidence ("task_id=" + $Contract.task_id) `
            -RemediationHint "Move the new changelog entry under ## [Unreleased]." `
            -RuleId "FAE-C005" `
            -PolicyRuleId "CHANGELOG-ENTRY-LOCATION" `
            -ConditionId "new_entry_outside_unreleased" `
            -GateId "changelog-entry-required"
    }

    $expectedClass = [regex]::Escape([string]$Contract.change_class)
    $expectedImpact = [regex]::Escape([string]$Contract.release_impact)
    $expectedApproval = [regex]::Escape([string]$Contract.approval_class)
    $requiredPatterns = @(
        @{ pattern = '(?im)^\*\*Change Class\*\*:\s*' + $expectedClass + '\s*$'; field = "Change Class"; condition = "change_class_mismatch"; rule = "CHANGELOG-VERSION-CONSISTENCY"; gate = "changelog-version-consistency"; compliance = "FAE-C009" },
        @{ pattern = '(?im)^\*\*Version Impact\*\*:\s*' + $expectedImpact + '\s*$'; field = "Version Impact"; condition = "version_impact_mismatch"; rule = "CHANGELOG-VERSION-CONSISTENCY"; gate = "changelog-version-consistency"; compliance = "FAE-C009" },
        @{ pattern = '(?im)^\*\*Task Reference\*\*:\s*' + [regex]::Escape([string]$Contract.task_id) + '\s*$'; field = "Task Reference"; condition = "missing_required_field"; rule = "CHANGELOG-REQUIRED-FIELDS"; gate = "changelog-entry-required"; compliance = "FAE-C005" },
        @{ pattern = '(?im)^\*\*Approval Class\*\*:\s*' + $expectedApproval + '\s*$'; field = "Approval Class"; condition = "approval_class_mismatch"; rule = "CHANGELOG-REQUIRED-FIELDS"; gate = "changelog-entry-required"; compliance = "FAE-C005" }
    )

    if ($Contract.change_class -eq "breaking-change") {
        $requiredPatterns += @(
            @{ pattern = '(?im)^\*\*breaking_change\*\*:\s*true\s*$'; field = "breaking_change"; condition = "breaking_change_without_required_disclosure"; rule = "CHANGELOG-BREAKING-DISCLOSURE"; gate = "changelog-breaking-disclosure"; compliance = "FAE-C006" },
            @{ pattern = '(?im)^\*\*migration_note\*\*:\s*\S'; field = "migration_note"; condition = "breaking_change_without_required_disclosure"; rule = "CHANGELOG-BREAKING-DISCLOSURE"; gate = "changelog-breaking-disclosure"; compliance = "FAE-C006" },
            @{ pattern = '(?im)^\*\*Migration Guidance\*\*:\s*\S'; field = "Migration Guidance"; condition = "breaking_change_without_required_disclosure"; rule = "CHANGELOG-BREAKING-DISCLOSURE"; gate = "changelog-breaking-disclosure"; compliance = "FAE-C006" },
            @{ pattern = '(?im)^\*\*Affected Consumers\*\*:\s*\S'; field = "Affected Consumers"; condition = "breaking_change_without_required_disclosure"; rule = "CHANGELOG-BREAKING-DISCLOSURE"; gate = "changelog-breaking-disclosure"; compliance = "FAE-C006" }
        )
    }

    foreach ($required in $requiredPatterns) {
        if ($entry.text -notmatch $required.pattern) {
            $warnings++
            Add-Finding `
                -FindingId "FFAE-CHANGELOG-003" `
                -Severity "warning" `
                -Status "warn" `
                -Category "changelog" `
                -Path "changelog/CHANGELOG.md" `
                -Message ("Changelog entry is missing or mismatches required field: " + $required.field) `
                -Evidence ("task_id=" + $Contract.task_id + "; field=" + $required.field) `
                -RemediationHint "Update the changelog entry so required fields match the task contract and breaking-change policy." `
                -RuleId $required.compliance `
                -PolicyRuleId $required.rule `
                -ConditionId $required.condition `
                -GateId $required.gate
        }
    }

    return $warnings
}

function Test-ForsettiChangelogHistoryIntegrity {
    param(
        [string]$RepoRoot,
        [string[]]$ChangedFiles
    )

    if ($ChangedFiles -notcontains "changelog/CHANGELOG.md") {
        return 0
    }

    try {
        $base = (& git -C $RepoRoot merge-base HEAD origin/main 2>$null)
        if ([string]::IsNullOrWhiteSpace($base)) {
            return 0
        }

        $baseChangelog = @(& git -C $RepoRoot show ($base + ":changelog/CHANGELOG.md"))
        $releasedStartLine = [int]::MaxValue
        $unreleasedIndex = -1
        for ($i = 0; $i -lt $baseChangelog.Count; $i++) {
            if ($baseChangelog[$i] -match '^## \[Unreleased\]') {
                $unreleasedIndex = $i
                continue
            }
            if ($unreleasedIndex -ge 0 -and $i -gt $unreleasedIndex -and $baseChangelog[$i] -match '^## \[') {
                $releasedStartLine = $i + 1
                break
            }
        }

        $diffLines = @(& git -C $RepoRoot diff --unified=0 $base -- changelog/CHANGELOG.md)
        $deletedReleasedLines = New-Object System.Collections.Generic.List[string]
        $oldLine = 0
        foreach ($line in $diffLines) {
            if ($line -match '^@@ -(?<start>\d+)(,(?<count>\d+))? \+\d+(,\d+)? @@') {
                $oldLine = [int]$Matches["start"]
                continue
            }

            if ($line.StartsWith("---", [System.StringComparison]::Ordinal) -or $line.StartsWith("+++", [System.StringComparison]::Ordinal)) {
                continue
            }

            if ($line.StartsWith("-", [System.StringComparison]::Ordinal)) {
                if ($oldLine -ge $releasedStartLine) {
                    $deletedReleasedLines.Add(("line " + $oldLine + ": " + $line.Substring(1)))
                }
                $oldLine++
                continue
            }

            if ($line.StartsWith("+", [System.StringComparison]::Ordinal)) {
                continue
            }

            if ($oldLine -gt 0) {
                $oldLine++
            }
        }

        if ($deletedReleasedLines.Count -gt 0) {
            Add-Finding `
                -FindingId "FFAE-CHANGELOG-005" `
                -Severity "error" `
                -Status "fail" `
                -Category "changelog" `
                -Path "changelog/CHANGELOG.md" `
                -Message "Changelog diff deletes or rewrites released history." `
                -Evidence (("deleted_released_lines=" + $deletedReleasedLines.Count + "; first=" + $deletedReleasedLines[0]) -replace "`r?`n", " ") `
                -RemediationHint "Keep released changelog history append-only, or use a dedicated correction contract." `
                -RuleId "FAE-C011" `
                -PolicyRuleId "CHANGELOG-HISTORY-IMMUTABLE" `
                -ConditionId "historical_changelog_mutation" `
                -GateId "changelog-history-immutable"
            return 1
        }
    } catch {
        Add-Finding `
            -FindingId "FFAE-CHANGELOG-005" `
            -Severity "warning" `
            -Status "warn" `
            -Category "changelog" `
            -Path "changelog/CHANGELOG.md" `
            -Message "Changelog history integrity could not be checked against origin/main." `
            -Evidence $_.Exception.Message `
            -RemediationHint "Run the validator in a git repository with origin/main available, or provide equivalent diff evidence in the phase report." `
            -RuleId "FAE-C011" `
            -PolicyRuleId "CHANGELOG-HISTORY-IMMUTABLE" `
            -ConditionId "changelog_history_diff_unavailable" `
            -GateId "changelog-history-immutable"
        return 1
    }

    return 0
}

function Test-ForsettiDocumentationObligations {
    param(
        [string]$RepoRoot,
        [object]$Contract,
        [string[]]$ChangedFiles
    )

    $warnings = 0
    $docImpact = $Contract.documentation_impact
    $warnings += Test-ForsettiDocsSyncForChangedFiles -RepoRoot $RepoRoot -ChangedFiles $ChangedFiles

    if ($docImpact.readme_update -and ($ChangedFiles -notcontains "README.md")) {
        $warnings++
        Add-Finding `
            -FindingId "FFAE-DOCS-003" `
            -Severity "warning" `
            -Status "warn" `
            -Category "docs" `
            -Path "README.md" `
            -Message "Task contract requires a README update, but README.md is not in the changed-file set." `
            -Evidence "readme_update=true" `
            -RemediationHint "Update README.md or amend the task contract documentation impact." `
            -RuleId "FAE-C008"
    }

    if ($docImpact.wiki_update) {
        $wikiChanged = @($ChangedFiles | Where-Object { $_ -like "wiki/*" }).Count -gt 0
        if (-not $wikiChanged) {
            $warnings++
            Add-Finding `
                -FindingId "FFAE-DOCS-004" `
                -Severity "warning" `
                -Status "warn" `
                -Category "docs" `
                -Path "wiki/" `
                -Message "Task contract requires wiki updates, but no wiki path is in the changed-file set." `
                -Evidence "wiki_update=true" `
                -RemediationHint "Update the relevant wiki pages or amend the task contract documentation impact." `
                -RuleId "FAE-C008"
        }
    }

    if ($docImpact.changelog_update -and ($ChangedFiles -notcontains "changelog/CHANGELOG.md")) {
        $warnings++
        Add-Finding `
            -FindingId "FFAE-CHANGELOG-001" `
            -Severity "warning" `
            -Status "warn" `
            -Category "changelog" `
            -Path "changelog/CHANGELOG.md" `
            -Message "Task contract requires a changelog entry, but changelog/CHANGELOG.md is not in the changed-file set." `
            -Evidence "changelog_update=true" `
            -RemediationHint "Add a changelog entry or amend the task contract documentation impact." `
            -RuleId "FAE-C005"
    }

    if ($docImpact.changelog_update -or $Contract.change_class -eq "breaking-change") {
        $warnings += Test-ForsettiChangelogEntryRequirements -RepoRoot $RepoRoot -Contract $Contract
    }

    $warnings += Test-ForsettiChangelogHistoryIntegrity -RepoRoot $RepoRoot -ChangedFiles $ChangedFiles

    if ($warnings -eq 0) {
        Add-Finding `
            -FindingId "FFAE-DOCS-003" `
            -Severity "info" `
            -Status "pass" `
            -Category "docs" `
            -Path $Contract.path `
            -Message "Documentation and changelog obligations are represented in the changed-file set." `
            -Evidence ("readme=" + $docImpact.readme_update + "; wiki=" + $docImpact.wiki_update + "; changelog=" + $docImpact.changelog_update) `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-ContractInfrastructure {
    param([string]$RepoRoot)

    $required = @(
        "core/schemas/task-contract.schema.json",
        "schemas/task-contract.schema.json",
        "core/contracts/task-contract-template.json",
        "core/validator/contract_rules.ps1"
    )

    $errors = 0
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $path))) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-CONTRACT-INFRA-001" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path $path `
                -Message "Contract enforcement infrastructure file is missing." `
                -Evidence $path `
                -RemediationHint "Create the required Phase 04 contract enforcement file." `
                -RuleId "FAE-C011"
        }
    }

    foreach ($jsonPath in @("core/schemas/task-contract.schema.json", "schemas/task-contract.schema.json", "core/contracts/task-contract-template.json")) {
        $fullPath = Join-Path $RepoRoot $jsonPath
        if (Test-Path -LiteralPath $fullPath) {
            try {
                $null = Get-Content -LiteralPath $fullPath -Raw | ConvertFrom-Json
            } catch {
                $errors++
                Add-Finding `
                    -FindingId "FFAE-CONTRACT-INFRA-002" `
                    -Severity "error" `
                    -Status "fail" `
                    -Category "json" `
                    -Path $jsonPath `
                    -Message "Contract infrastructure JSON does not parse." `
                    -Evidence $_.Exception.Message `
                    -RemediationHint "Fix the JSON syntax." `
                    -RuleId "FAE-C011"
            }
        }
    }

    $coreSchema = Join-Path $RepoRoot "core/schemas/task-contract.schema.json"
    $rootSchema = Join-Path $RepoRoot "schemas/task-contract.schema.json"
    if ((Test-Path -LiteralPath $coreSchema) -and (Test-Path -LiteralPath $rootSchema)) {
        $coreHash = Get-FileSha256 -Path $coreSchema
        $rootHash = Get-FileSha256 -Path $rootSchema
        if ($coreHash -ne $rootHash) {
            $errors++
            Add-Finding `
                -FindingId "FFAE-CONTRACT-INFRA-003" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path "core/schemas/task-contract.schema.json" `
                -Message "Core and root task contract schemas differ." `
                -Evidence ("core=" + $coreHash + "; root=" + $rootHash) `
                -RemediationHint "Keep the root compatibility schema byte-identical to the core schema." `
                -RuleId "FAE-C011"
        }
    }

    if ($errors -eq 0) {
        Add-Finding `
            -FindingId "FFAE-CONTRACT-INFRA-001" `
            -Severity "info" `
            -Status "pass" `
            -Category "contract" `
            -Path $null `
            -Message "Contract enforcement infrastructure is present and parseable." `
            -Evidence ("Checked " + $required.Count + " files.") `
            -RemediationHint $null `
            -RuleId $null
    }
}

function Test-TaskContractRules {
    param(
        [string]$RepoRoot,
        [AllowNull()][string]$ContractPath,
        [AllowNull()][string[]]$ChangedFile,
        [AllowNull()][string]$ChangedFilesPath,
        [AllowNull()][string]$PendingOutputPath,
        [switch]$RequireContract
    )

    if ([string]::IsNullOrWhiteSpace($ContractPath)) {
        if ($RequireContract) {
            Add-Finding `
                -FindingId "FFAE-CONTRACT-001" `
                -Severity "error" `
                -Status "fail" `
                -Category "contract" `
                -Path $null `
                -Message "Contract validation mode requires -ContractPath." `
                -Evidence "-ContractPath missing" `
                -RemediationHint "Provide the governing task contract path." `
                -RuleId "FAE-C001"
        }
        return
    }

    $contract = Read-ForsettiTaskContract -RepoRoot $RepoRoot -ContractPath $ContractPath
    if (-not $contract) {
        return
    }

    $changedFiles = Get-ForsettiChangedFiles -RepoRoot $RepoRoot -ChangedFile $ChangedFile -ChangedFilesPath $ChangedFilesPath
    $script:ContractChangedFilesCount = @($changedFiles).Count
    Test-ForsettiContractShape -Contract $contract
    Test-ForsettiContractScope -Contract $contract -ChangedFiles $changedFiles
    Test-ForsettiProtectedApprovals -RepoRoot $RepoRoot -Contract $contract -ChangedFiles $changedFiles
    Test-ForsettiRoleLimitedPaths -RepoRoot $RepoRoot -Contract $contract -ChangedFiles $changedFiles
    Test-ForsettiRequiredOutputs -RepoRoot $RepoRoot -Contract $contract -PendingOutputPath $PendingOutputPath
    Test-ForsettiDocumentationObligations -RepoRoot $RepoRoot -Contract $contract -ChangedFiles $changedFiles
}
