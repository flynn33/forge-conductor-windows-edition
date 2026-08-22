$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function New-LibProject($name, $guid, $relDir, $includes, $compiles, $extraInc = @()) {
    $incXml = ($includes | ForEach-Object { "    <ClInclude Include=`"$_`" />" }) -join "`n"
    $cmpXml = ($compiles | ForEach-Object { "    <ClCompile Include=`"$_`" />" }) -join "`n"
    $extra = if ($extraInc.Count) { ($extraInc | ForEach-Object { "$_;" }) -join "" } else { "" }
    @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{$guid}</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>$name</RootNamespace>
    <WindowsTargetPlatformVersion>10.0.26100.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <WholeProgramOptimization>true</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets">
    <Import Project="`$(UserRootDir)\Microsoft.Cpp.`$(Platform).user.props" Condition="exists('`$(UserRootDir)\Microsoft.Cpp.`$(Platform).user.props')" />
  </ImportGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$extra%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
$incXml
  </ItemGroup>
  <ItemGroup>
$cmpXml
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@ | Set-Content -Encoding UTF8 (Join-Path $root "$relDir\$name.vcxproj")
}

function RelFiles($base, $pattern) {
    Get-ChildItem -Path (Join-Path $root $base) -Recurse -Filter $pattern | ForEach-Object {
        $rel = $_.FullName.Substring((Join-Path $root $base).Length).TrimStart('\')
        $rel
    }
}

# Libraries
New-LibProject "ForgeRuntime" "A81B2C3D-1001-4A01-8001-000000000001" "src\ForgeRuntime" `
    (RelFiles "src\ForgeRuntime" "*.h") (RelFiles "src\ForgeRuntime" "*.cpp")
New-LibProject "ForgeDomain" "A81B2C3D-1001-4A01-8001-000000000004" "src\ForgeDomain" `
    (RelFiles "src\ForgeDomain" "*.h") (RelFiles "src\ForgeDomain" "*.cpp")
New-LibProject "ForgePersistence" "A81B2C3D-1001-4A01-8001-000000000005" "src\ForgePersistence" `
    (RelFiles "src\ForgePersistence" "*.h") (RelFiles "src\ForgePersistence" "*.cpp")
New-LibProject "ForgeOrchestration" "A81B2C3D-1001-4A01-8001-000000000006" "src\ForgeOrchestration" `
    (RelFiles "src\ForgeOrchestration" "*.h") (RelFiles "src\ForgeOrchestration" "*.cpp")
New-LibProject "ForgeMcp" "A81B2C3D-1001-4A01-8001-000000000007" "src\ForgeMcp" `
    (RelFiles "src\ForgeMcp" "*.h") (RelFiles "src\ForgeMcp" "*.cpp")
New-LibProject "ForgeTelemetry" "A81B2C3D-1001-4A01-8001-000000000008" "src\ForgeTelemetry" `
    (RelFiles "src\ForgeTelemetry" "*.h") (RelFiles "src\ForgeTelemetry" "*.cpp")
New-LibProject "ForgeLmStudio" "A81B2C3D-1001-4A01-8001-000000000009" "src\ForgeLmStudio" `
    (RelFiles "src\ForgeLmStudio" "*.h") (RelFiles "src\ForgeLmStudio" "*.cpp")
New-LibProject "ForgeManager" "A81B2C3D-1001-4A01-8001-00000000000A" "src\ForgeManager" `
    (RelFiles "src\ForgeManager" "*.h") (RelFiles "src\ForgeManager" "*.cpp")
New-LibProject "ForgeGaugeKit" "A81B2C3D-1001-4A01-8001-00000000000B" "src\ForgeGaugeKit" `
    (RelFiles "src\ForgeGaugeKit" "*.h") (RelFiles "src\ForgeGaugeKit" "*.cpp")
New-LibProject "ForgeHost" "A81B2C3D-1001-4A01-8001-000000000003" "src\ForgeHost" `
    (RelFiles "src\ForgeHost" "*.h") (RelFiles "src\ForgeHost" "*.cpp")
New-LibProject "ForgeModules" "A81B2C3D-1001-4A01-8001-000000000010" "src\Modules\OperatorAppModule" `
    @("OperatorModules.h") @("OperatorModules.cpp")

function New-ExeProject($name, $guid, $relDir, $subsystem, $files, $resources = @()) {
    $cmp = ($files | ForEach-Object { "    <ClCompile Include=`"$_`" />" }) -join "`n"
    $res = ($resources | ForEach-Object { "    <ResourceCompile Include=`"$_`" />" }) -join "`n"
    $hdr = if (Test-Path (Join-Path $root "$relDir\OperatorWindow.h")) { "    <ClInclude Include=`"OperatorWindow.h`" />" } else { "" }
    @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{$guid}</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>$name</RootNamespace>
    <WindowsTargetPlatformVersion>10.0.26100.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <WholeProgramOptimization>true</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemDefinitionGroup>
    <Link>
      <SubSystem>$subsystem</SubSystem>
    </Link>
    <ClCompile>
      <AdditionalIncludeDirectories>`$(ForgeRoot)src;`$(ForgeRoot)src\Modules\OperatorAppModule;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
$cmp
  </ItemGroup>
  <ItemGroup>
$hdr
  </ItemGroup>
  <ItemGroup>
$res
  </ItemGroup>
  <ItemGroup>
    <ProjectReference Include="..\ForgeRuntime\ForgeRuntime.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000001}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeDomain\ForgeDomain.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000004}</Project></ProjectReference>
    <ProjectReference Include="..\ForgePersistence\ForgePersistence.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000005}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeOrchestration\ForgeOrchestration.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000006}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeMcp\ForgeMcp.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000007}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeTelemetry\ForgeTelemetry.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000008}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeLmStudio\ForgeLmStudio.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000009}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeManager\ForgeManager.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-00000000000A}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeGaugeKit\ForgeGaugeKit.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-00000000000B}</Project></ProjectReference>
    <ProjectReference Include="..\ForgeHost\ForgeHost.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000003}</Project></ProjectReference>
    <ProjectReference Include="..\Modules\OperatorAppModule\ForgeModules.vcxproj"><Project>{A81B2C3D-1001-4A01-8001-000000000010}</Project></ProjectReference>
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@ | Set-Content -Encoding UTF8 (Join-Path $root "$relDir\$name.vcxproj")
}

New-ExeProject "ForgeConductorApp" "A81B2C3D-1001-4A01-8001-000000000020" "src\ForgeConductorApp" "Windows" @("Main.cpp","OperatorWindow.cpp") @("app.rc")

# Tests - same refs but console; fix project refs to be relative from tests/
$testProj = Get-Content (Join-Path $root "src\ForgeConductorApp\ForgeConductorApp.vcxproj") -Raw
$testProj = $testProj -replace "ForgeConductorApp","ForgeConductor.Tests"
$testProj = $testProj -replace "A81B2C3D-1001-4A01-8001-000000000020","A81B2C3D-1001-4A01-8001-000000000030"
$testProj = $testProj -replace "<SubSystem>Windows</SubSystem>","<SubSystem>Console</SubSystem>"
$testProj = $testProj -replace "Main.cpp","Tests.cpp"
$testProj = $testProj -replace "    <ClCompile Include=`"OperatorWindow.cpp`" />\r?\n",""
$testProj = $testProj -replace "    <ClInclude Include=`"OperatorWindow.h`" />",""
$testProj = $testProj -replace "    <ResourceCompile Include=`"app.rc`" />",""
$testProj = $testProj -replace "\.\.\\Forge","..\..\src\Forge"
$testProj = $testProj -replace "\.\.\\\.\.\\\\src\\Modules","..\..\src\Modules"
$testProj = $testProj -replace "\.\.\\Modules","..\..\src\Modules"
Set-Content -Encoding UTF8 (Join-Path $root "tests\ForgeConductor.Tests\ForgeConductor.Tests.vcxproj") $testProj

$smoke = $testProj -replace "ForgeConductor.Tests","ForgeConductor.McpSmoke"
$smoke = $smoke -replace "A81B2C3D-1001-4A01-8001-000000000030","A81B2C3D-1001-4A01-8001-000000000031"
$smoke = $smoke -replace "Tests.cpp","McpSmoke.cpp"
Set-Content -Encoding UTF8 (Join-Path $root "tests\ForgeConductor.McpSmoke\ForgeConductor.McpSmoke.vcxproj") $smoke

Write-Host "vcxproj generated"
