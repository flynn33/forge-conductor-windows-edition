# P01-B probe: verify presence of first-party tooling before provisioning.
$ErrorActionPreference = 'SilentlyContinue'
$out = @()

function Add($m) { $script:out += $m }

Add '== vcpkg =='
$v = Get-Command vcpkg -ErrorAction SilentlyContinue
if ($v) { Add "PATH: $($v.Source)" } else { Add 'NOT ON PATH' }
$candidates = @(
  'C:\vcpkg\vcpkg.exe',
  (Join-Path $env:USERPROFILE 'vcpkg\vcpkg.exe'),
  'C:\Program Files\Microsoft\VisualStudio\Shared\vcpkg\vcpkg.exe'
)
foreach ($p in $candidates) { if (Test-Path $p) { Add "FOUND: $p" } }

Add '== Windows Kits SDKs =='
$kits = 'C:\Program Files (x86)\Windows Kits\10'
if (Test-Path (Join-Path $kits 'Include')) {
  Get-ChildItem (Join-Path $kits 'Include') -Directory | ForEach-Object { Add "SDK Include: $($_.Name)" }
} else { Add 'NO KITS INCLUDE DIR' }

Add '== winsqlite3.h =='
$foundSqlite = $false
Get-ChildItem (Join-Path $kits 'Include') -Directory | ForEach-Object {
  $h = Join-Path $_.FullName 'ucrt\winsqlite3.h'
  if (Test-Path $h) { Add "FOUND winsqlite3.h in $($_.Name)"; $script:foundSqlite = $true }
}
if (-not $foundSqlite) { Add 'winsqlite3.h NOT FOUND in any SDK include dir' }

Add '== MakeAppx / SignTool =='
$ma = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter 'MakeAppx.exe' | Select-Object -First 2
foreach ($f in $ma) { Add "MakeAppx: $($f.FullName)" }
if (-not $ma) { Add 'MakeAppx NOT FOUND' }
$st = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter 'signtool.exe' | Select-Object -First 2
foreach ($f in $st) { Add "SignTool: $($f.FullName)" }
if (-not $st) { Add 'SignTool NOT FOUND' }

Add '== Windows App SDK (NuGet cache) =='
$nu = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windowsappsdk'
if (Test-Path $nu) {
  Get-ChildItem $nu -Directory | ForEach-Object { Add "NuGet windowsappsdk: $($_.Name)" }
} else { Add 'NO NuGet microsoft.windowsappsdk cache' }

Add '== winget / vs_installer =='
$w = Get-Command winget -ErrorAction SilentlyContinue
if ($w) { Add "winget: $($w.Source)" } else { Add 'winget NOT ON PATH' }
foreach ($vi in @('C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe',
                  'C:\Program Files\Microsoft Visual Studio\Installer\vs_installer.exe')) {
  if (Test-Path $vi) { Add "vs_installer: $vi" }
}

Add '== nlohmann_json (vcpkg installed tree) =='
foreach ($base in @('C:\vcpkg\installed', (Join-Path $env:USERPROFILE 'vcpkg\installed'))) {
  if (Test-Path $base) {
    Get-ChildItem $base -Directory | ForEach-Object { Add "vcpkg installed triplet dir: $($_.FullName)" }
  }
}

$out | Out-String
