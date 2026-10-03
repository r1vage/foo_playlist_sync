# Builds x86 + x64 Release, runs the engine tests and packages dist\foo_playlist_sync-<version>.fb2k-component
param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (install Visual Studio 2022 with C++ desktop development).' }

foreach ($platform in 'Win32', 'x64') {
    & $msbuild "$root\foo_playlist_sync.vcxproj" -m -nologo -v:minimal -p:Configuration=Release -p:Platform=$platform
    if ($LASTEXITCODE) { throw "Build failed for $platform" }
}

if (-not $SkipTests) {
    & $msbuild "$root\tests\core_tests.vcxproj" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64
    if ($LASTEXITCODE) { throw 'Test build failed' }
    & "$root\build\tests\Release\core_tests.exe"
    if ($LASTEXITCODE) { throw 'Tests failed' }
}

$version = (Select-String -Path "$root\src\version.h" -Pattern 'PSYNC_VERSION_STRING "(.+)"').Matches[0].Groups[1].Value
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$component = Join-Path $dist "foo_playlist_sync-$version.fb2k-component"
Remove-Item $component -Force -ErrorAction SilentlyContinue

# Compress-Archive on Windows PowerShell writes backslashes into entry names; build the zip by hand
# so the x64 DLL is stored as "x64/foo_playlist_sync.dll" where foobar2000 looks for it.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($component, 'Create')
try {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, "$root\build\Win32\Release\foo_playlist_sync.dll", 'foo_playlist_sync.dll')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, "$root\build\x64\Release\foo_playlist_sync.dll", 'x64/foo_playlist_sync.dll')
} finally {
    $archive.Dispose()
}
Write-Host "Packaged $component"
