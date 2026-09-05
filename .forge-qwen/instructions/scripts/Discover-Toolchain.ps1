[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)

$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-RepoRoot -Start $Repository
$programFilesX86=[Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$vswhere=Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
$instances=@()
if(Test-Path -LiteralPath $vswhere){
    try{$instances=((& $vswhere -all -products '*' -format json)|Out-String)|ConvertFrom-Json}catch{}
}
$sdk=Join-Path $programFilesX86 'Windows Kits\10'
$versions=@()
if(Test-Path -LiteralPath (Join-Path $sdk 'Include')){$versions=Get-ChildItem (Join-Path $sdk 'Include') -Directory|ForEach-Object Name}
$ram=$null
try{$ram=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory}catch{}
$result=[ordered]@{utc=Get-UtcNow;windows_11=Test-Windows11;os=[Environment]::OSVersion.VersionString;architecture=$env:PROCESSOR_ARCHITECTURE;ram_bytes=$ram;powershell=$PSVersionTable.PSVersion.ToString();tools=[ordered]@{};visual_studio=$instances;windows_sdk_versions=$versions}
foreach($name in @('git','cmake','ctest','msbuild','cl','vcpkg','winget','pwsh','lms','code')){$command=Get-Command $name -ErrorAction SilentlyContinue;$result.tools[$name]=if($command){$command.Source}else{$null}}
$path=Join-Path $Repository '.forge-qwen\state\profiles\toolchain.json'
Write-JsonAtomic -Path $path -Value $result
$result|ConvertTo-Json -Depth 30
