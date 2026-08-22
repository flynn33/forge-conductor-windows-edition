param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    $msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
}
& $msbuild "$PSScriptRoot\..\ForgeConductor.sln" /m /p:Configuration=$Configuration /p:Platform=x64 /p:RestorePackages=false
exit $LASTEXITCODE
