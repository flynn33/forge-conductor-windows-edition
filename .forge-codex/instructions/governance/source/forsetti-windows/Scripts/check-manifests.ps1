# Forsetti Framework - Module Manifest Validation Script
# Copyright (c) 2026 James Daley. All Rights Reserved.
#
# Validates all ForsettiManifests JSON files for correct schema,
# required fields, and naming conventions.

param(
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
} else {
    $repoRoot = (Resolve-Path $RepoRoot).Path
}
$violations = @()
$seenModuleIDs = @{}

Write-Host "=== Manifest Validation ===" -ForegroundColor Cyan

$baseRequiredFields = @("schemaVersion", "moduleID", "displayName", "moduleVersion", "moduleType", "supportedPlatforms", "minForsettiVersion", "capabilitiesRequested", "entryPoint")
$v11RequiredFields = @("manifestTemplateVersion", "maxForsettiVersion", "iapProductID", "defaultModuleRole", "runtimeRequirements")
$validModuleTypes = @("service", "ui", "app")
$validPlatforms = @("Windows")
$validCapabilities = @(
    "networking", "storage", "secure_storage", "file_export", "crypto_utilities",
    "telemetry", "routing_overlay", "toolbar_items", "view_injection", "ui_theme_mask",
    "event_publishing", "shared_database", "authentication", "diagnostics", "api", "security"
)
$validDefaultRoles = @("ui", "shared_database", "authentication", "diagnostics", "api", "security")
$validIOKinds = @("networking", "storage", "secure_storage", "file_export", "crypto_utilities", "telemetry", "shared_database", "authentication", "diagnostics", "api", "security")
$validIOAccess = @("read", "write", "read_write", "execute", "emit", "consume")
$ioCapabilityMap = @{
    networking = "networking"
    storage = "storage"
    secure_storage = "secure_storage"
    file_export = "file_export"
    crypto_utilities = "crypto_utilities"
    telemetry = "telemetry"
    shared_database = "shared_database"
    authentication = "authentication"
    diagnostics = "diagnostics"
    api = "api"
    security = "security"
}

function Test-HasProperty {
    param(
        [Parameter(Mandatory=$true)] [object]$Object,
        [Parameter(Mandatory=$true)] [string]$Name
    )
    return $null -ne $Object.PSObject.Properties[$Name]
}

function Test-DuplicateValues {
    param(
        [Parameter(Mandatory=$true)] [array]$Values,
        [Parameter(Mandatory=$true)] [string]$Field,
        [Parameter(Mandatory=$true)] [string]$Rel
    )

    $seen = @{}
    $messages = @()
    foreach ($value in $Values) {
        if ($seen.ContainsKey([string]$value)) {
            $messages += "$Rel - Duplicate $Field value '$value'"
        } else {
            $seen[[string]$value] = $true
        }
    }
    return $messages
}

# Find all manifest JSON files
$searchRootNames = @("src", "templates", "samples", "tests")
$manifestDirs = @()
foreach ($rootName in $searchRootNames) {
    $searchRoot = Join-Path $repoRoot $rootName
    if (Test-Path $searchRoot) {
        $manifestDirs += Get-ChildItem -Path $searchRoot -Recurse -Directory -Filter "ForsettiManifests" -ErrorAction SilentlyContinue
    }
}
$manifestFiles = @()
foreach ($dir in $manifestDirs) {
    $manifestFiles += Get-ChildItem -Path $dir.FullName -Filter "*.json" -ErrorAction SilentlyContinue
}

if ($manifestFiles.Count -eq 0) {
    Write-Host "No manifest files found." -ForegroundColor Yellow
    exit 0
}

Write-Host "Found $($manifestFiles.Count) manifest file(s).`n" -ForegroundColor Yellow

foreach ($file in $manifestFiles) {
    $rel = $file.FullName.Replace("$repoRoot\", "")
    Write-Host "Checking: $rel" -ForegroundColor Gray

    # Parse JSON
    try {
        $manifest = Get-Content $file.FullName -Raw | ConvertFrom-Json
    } catch {
        $violations += "$rel - Invalid JSON: $($_.Exception.Message)"
        continue
    }

    # Check required fields
    foreach ($field in $baseRequiredFields) {
        if (-not (Test-HasProperty -Object $manifest -Name $field) -or $null -eq $manifest.PSObject.Properties[$field].Value) {
            $violations += "$rel - Missing required field: '$field'"
        }
    }

    # Validate schemaVersion
    if ($manifest.schemaVersion -and $manifest.schemaVersion -notin @("1.0", "1.1")) {
        $violations += "$rel - schemaVersion must be '1.0' or '1.1', got '$($manifest.schemaVersion)'"
    }

    if ($manifest.schemaVersion -eq "1.1") {
        foreach ($field in $v11RequiredFields) {
            if (-not (Test-HasProperty -Object $manifest -Name $field)) {
                $violations += "$rel - Missing required 1.1 field: '$field'"
            }
        }
        if ($manifest.manifestTemplateVersion -ne "1.1") {
            $violations += "$rel - manifestTemplateVersion must be '1.1' for schemaVersion 1.1"
        }
    } elseif ((Test-HasProperty -Object $manifest -Name "manifestTemplateVersion") -and $manifest.manifestTemplateVersion -ne "1.0") {
        $violations += "$rel - manifestTemplateVersion must be '1.0' for schemaVersion 1.0"
    }

    # Validate moduleType
    if ($manifest.moduleType -and $manifest.moduleType -notin $validModuleTypes) {
        $violations += "$rel - Invalid moduleType '$($manifest.moduleType)' (must be one of: $($validModuleTypes -join ', '))"
    }

    # Validate supportedPlatforms uses exact repository casing and includes Windows
    if ($manifest.supportedPlatforms) {
        $platforms = @($manifest.supportedPlatforms)
        foreach ($platform in $platforms) {
            if ($platform -cnotin $validPlatforms) {
                $violations += "$rel - Invalid supportedPlatforms value '$platform' (valid: $($validPlatforms -join ', '))"
            }
        }
        if ("Windows" -cnotin $platforms) {
            $violations += "$rel - supportedPlatforms must include 'Windows'"
        }
        $violations += Test-DuplicateValues -Values $platforms -Field "supportedPlatforms" -Rel $rel
    }

    # Validate capabilitiesRequested (if present)
    if ($manifest.capabilitiesRequested) {
        $capabilities = @($manifest.capabilitiesRequested)
        foreach ($cap in $manifest.capabilitiesRequested) {
            if ($cap -cnotin $validCapabilities) {
                $violations += "$rel - Unknown capability '$cap' (valid: $($validCapabilities -join ', '))"
            }
        }
        $violations += Test-DuplicateValues -Values $capabilities -Field "capabilitiesRequested" -Rel $rel
    }

    # Check for duplicate moduleID
    if ($manifest.moduleID) {
        if ($seenModuleIDs.ContainsKey($manifest.moduleID)) {
            $violations += "$rel - Duplicate moduleID '$($manifest.moduleID)' (also in $($seenModuleIDs[$manifest.moduleID]))"
        } else {
            $seenModuleIDs[$manifest.moduleID] = $rel
        }
    }

    # Validate moduleVersion is an object with major/minor/patch
    if ($manifest.moduleVersion) {
        $mv = $manifest.moduleVersion
        if ($null -eq $mv.major -or $null -eq $mv.minor -or $null -eq $mv.patch) {
            $violations += "$rel - moduleVersion must have 'major', 'minor', and 'patch' fields"
        }
    }

    # Validate minForsettiVersion is an object with major/minor/patch
    if ($manifest.minForsettiVersion) {
        $mfv = $manifest.minForsettiVersion
        if ($null -eq $mfv.major -or $null -eq $mfv.minor -or $null -eq $mfv.patch) {
            $violations += "$rel - minForsettiVersion must have 'major', 'minor', and 'patch' fields"
        }
    }

    # Check key naming convention (camelCase)
    $props = $manifest.PSObject.Properties | Select-Object -ExpandProperty Name
    foreach ($prop in $props) {
        if ($prop -match '_') {
            $violations += "$rel - Key '$prop' uses snake_case (manifests must use camelCase)"
        }
    }

    if ($manifest.schemaVersion -eq "1.1" -and (Test-HasProperty -Object $manifest -Name "runtimeRequirements")) {
        $runtime = $manifest.runtimeRequirements
        if (-not (Test-HasProperty -Object $runtime -Name "io")) {
            $violations += "$rel - runtimeRequirements.io is required"
        }
        if (-not (Test-HasProperty -Object $runtime -Name "ui")) {
            $violations += "$rel - runtimeRequirements.ui is required"
        }
        if (-not (Test-HasProperty -Object $runtime -Name "dataIsolation")) {
            $violations += "$rel - runtimeRequirements.dataIsolation is required"
        }

        $capabilities = @($manifest.capabilitiesRequested)
        $ioRequirementIDs = @()
        foreach ($io in @($runtime.io)) {
            if (-not $io.requirementID) {
                $violations += "$rel - runtimeRequirements.io entry missing requirementID"
            } else {
                $ioRequirementIDs += $io.requirementID
            }
            if ($io.kind -cnotin $validIOKinds) {
                $violations += "$rel - runtimeRequirements.io '$($io.requirementID)' has invalid kind '$($io.kind)'"
            } elseif ($ioCapabilityMap.ContainsKey($io.kind) -and $ioCapabilityMap[$io.kind] -cnotin $capabilities) {
                $violations += "$rel - runtimeRequirements.io '$($io.requirementID)' requires capability '$($ioCapabilityMap[$io.kind])'"
            }
            if ($io.access -cnotin $validIOAccess) {
                $violations += "$rel - runtimeRequirements.io '$($io.requirementID)' has invalid access '$($io.access)'"
            }
        }
        if ($ioRequirementIDs.Count -gt 0) {
            $violations += Test-DuplicateValues -Values $ioRequirementIDs -Field "runtimeRequirements.io.requirementID" -Rel $rel
        }

        if ($manifest.defaultModuleRole) {
            if ($manifest.defaultModuleRole -cnotin $validDefaultRoles) {
                $violations += "$rel - Invalid defaultModuleRole '$($manifest.defaultModuleRole)'"
            } elseif ($manifest.defaultModuleRole -eq "ui" -and $manifest.moduleType -cnotin @("ui", "app")) {
                $violations += "$rel - defaultModuleRole 'ui' requires moduleType 'ui' or 'app'"
            } elseif ($manifest.defaultModuleRole -ne "ui" -and $manifest.moduleType -ne "service") {
                $violations += "$rel - defaultModuleRole '$($manifest.defaultModuleRole)' requires moduleType 'service'"
            }
        }

        if ($manifest.moduleType -eq "service" -and $null -ne $runtime.ui) {
            $violations += "$rel - service modules must set runtimeRequirements.ui to null"
        }
        if ($manifest.moduleType -in @("ui", "app") -and $null -eq $runtime.ui) {
            $violations += "$rel - ui and app modules must declare runtimeRequirements.ui"
        }

        if ($runtime.dataIsolation) {
            $isolation = $runtime.dataIsolation
            if ($isolation.mode -cnotin @("private_to_module", "framework_mediated_shared")) {
                $violations += "$rel - runtimeRequirements.dataIsolation.mode is invalid"
            }
            $ownedStoreIDs = @($isolation.ownedStoreIDs)
            if ($ownedStoreIDs.Count -gt 0) {
                $violations += Test-DuplicateValues -Values $ownedStoreIDs -Field "runtimeRequirements.dataIsolation.ownedStoreIDs" -Rel $rel
            }
            $roles = @($isolation.requiredDefaultRoles)
            foreach ($role in $roles) {
                if ($role -cnotin $validDefaultRoles) {
                    $violations += "$rel - runtimeRequirements.dataIsolation.requiredDefaultRoles contains invalid role '$role'"
                }
            }
            if ($roles.Count -gt 0) {
                $violations += Test-DuplicateValues -Values $roles -Field "runtimeRequirements.dataIsolation.requiredDefaultRoles" -Rel $rel
            }
            if ($isolation.mode -eq "framework_mediated_shared" -and
                $manifest.defaultModuleRole -ne "shared_database" -and
                "shared_database" -cnotin $roles -and
                "shared_database" -cnotin @($runtime.io | ForEach-Object { $_.kind })) {
                $violations += "$rel - framework_mediated_shared requires a shared_database role or I/O requirement"
            }
        }
    }
}

# --- Report ---
Write-Host ""
if ($violations.Count -eq 0) {
    Write-Host "All manifest checks passed ($($manifestFiles.Count) file(s) validated)." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Manifest violations found:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  - $v" -ForegroundColor Red
    }
    Write-Host "`n$($violations.Count) violation(s) found." -ForegroundColor Red
    exit 1
}
