# Copies the x64 Release build into a portable foobar2000 install for testing.
# foobar2000 must be closed (it locks loaded component DLLs).
param([string]$Foobar = 'C:\Software\foobar2000 test 64')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $Foobar 'foobar2000.exe'
if (Get-Process foobar2000 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    throw "Close foobar2000 at $Foobar first."
}
$dest = Join-Path $Foobar 'profile\user-components-x64\foo_playlist_sync'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$root\build\x64\Release\foo_playlist_sync.dll" $dest -Force
Write-Host "Installed into $dest"
