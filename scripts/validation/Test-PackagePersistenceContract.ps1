#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$WorkspaceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $WorkspaceRoot 'packaging\Package.appxmanifest.template'
[xml]$manifest = Get-Content -Raw -LiteralPath $manifestPath
$namespaces = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
$namespaces.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$namespaces.AddNamespace('rescap', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities')
$namespaces.AddNamespace('virtualization', 'http://schemas.microsoft.com/appx/manifest/virtualization/windows10')

$expectedPath = '$(KnownFolder:LocalAppData)\Forge Conductor'
$excluded = @($manifest.SelectNodes(
    '/f:Package/f:Properties/virtualization:FileSystemWriteVirtualization/virtualization:ExcludedDirectories/virtualization:ExcludedDirectory',
    $namespaces))
if ($excluded.Count -ne 1 -or $excluded[0].InnerText -cne $expectedPath) {
    throw 'The package must exclude exactly the durable production profile from MSIX file-system virtualization.'
}

$capabilities = @($manifest.SelectNodes(
    '/f:Package/f:Capabilities/rescap:Capability[@Name="unvirtualizedResources"]',
    $namespaces))
if ($capabilities.Count -ne 1) {
    throw 'The package must declare exactly one unvirtualizedResources capability for its narrow AppData exclusion.'
}

[ordered]@{
    ok = $true
    excluded_directory = $excluded[0].InnerText
    restricted_capability = 'unvirtualizedResources'
    production_profile_excluded = $true
} | ConvertTo-Json
