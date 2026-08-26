# Forsetti project rule helpers for the local validator.

function Get-ForsettiRequiredProjectContextFields {
    return @(
        "repository_mode",
        "forsetti_edition",
        "target_platform",
        "framework_version",
        "edition_profile",
        "manifest_schema_version",
        "manifest_template_version",
        "deployment_pattern",
        "module_type",
        "module_id",
        "capabilities_requested",
        "runtime_requirements_declared",
        "uses_public_api_only",
        "touches_framework_internals"
    )
}

function Get-ForsettiCapabilityUseMap {
    param([AllowNull()][string]$Edition)

    $map = [ordered]@{
        networking       = @("NetworkingService", "URLSession", "networking", "network socket")
        storage          = @("StorageService", "UserDefaults", "storage", "file system")
        secure_storage   = @("SecureStorageService", "Keychain", "secure_storage", "credential storage")
        file_export      = @("FileExportService", "NSSavePanel", "file_export", "export(data:")
        crypto_utilities = @("CryptoKit", "SHA256", "AES.GCM", "crypto_utilities")
        telemetry        = @("TelemetryService", "telemetry", "track(event:")
        routing_overlay  = @("routing_overlay", "routeIDs", "pointerIDs", "OverlaySchema")
        ui_theme_mask    = @("ui_theme_mask", "themeIDs", "ThemeMask")
        toolbar_items    = @("toolbar_items", "toolbarItemIDs", "ToolbarItemDescriptor")
        view_injection   = @("view_injection", "viewIDs", "slotIDs", "ViewInjectionDescriptor")
        shared_database  = @("SharedDatabaseService", "shared_database")
        authentication   = @("AuthenticationService", "authentication")
        diagnostics      = @("DiagnosticsService", "diagnostics")
        api              = @("APIService", "capability: .api", '"api"')
        security         = @("SecurityService", "capability: .security", '"security"')
    }

    # The supplied Apple 0.1.5 Capability enum does not define event_publishing.
    # Retain the pre-existing Windows mapping only when the selected profile is Windows.
    if ($Edition -eq "windows") {
        $map.event_publishing = @("event_publishing", "publishEvent", "EventPublisher")
    }

    return $map
}

function Get-ForsettiManifestRuntimeRequirementFields {
    return @("io", "ui", "dataIsolation")
}
