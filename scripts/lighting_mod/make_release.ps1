# Lighting mod (AI-assisted): builds the release ZIP for OpenGOAL's launcher.
#
#   powershell -ExecutionPolicy Bypass -File scripts\lighting_mod\make_release.ps1 -Version v0.1.0
#
# Same layout as OpenGOAL's own releases (.github/scripts/releases/extract_build_windows.sh):
# gk.exe, goalc.exe and extractor.exe at the root (the statically linked build: no DLLs needed),
# and next to them data/ with the decompiler config, the game code, the assets and the shaders.
# The launcher unpacks the ZIP into its own mod folder and runs the extractor itself.
#
# Only files tracked by git go in (git ls-files), so nothing from the game disc can slip in.
# Output: out\lighting-mod\windows-<mod id>-<version>.zip and metadata.json, for the GitHub release.

param(
  [string]$Version = "v0.1.0",
  [string]$Bin = "out\build\Release-static\bin"
)

$ErrorActionPreference = "Stop"
$ModId = "jak1-realistic-lighting"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$BinDir = Join-Path $Root $Bin
foreach ($exe in "gk.exe", "goalc.exe", "extractor.exe") {
  if (-not (Test-Path (Join-Path $BinDir $exe))) {
    throw "$exe not found in $BinDir. Build the static release first (see MODPACK.md)."
  }
}

$Out = Join-Path $Root "out\lighting-mod"
$Dest = Join-Path $Out "package"
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

foreach ($exe in "gk.exe", "goalc.exe", "extractor.exe") {
  Copy-Item (Join-Path $BinDir $exe) (Join-Path $Dest $exe)
}

$DataFolders = @(
  "decompiler/config",
  "goal_src",
  "game/assets",
  "game/graphics/opengl_renderer/shaders",
  "custom_assets"
)
$count = 0
foreach ($folder in $DataFolders) {
  $files = git ls-files -- $folder
  foreach ($rel in $files) {
    if (-not $rel) { continue }
    $src = Join-Path $Root $rel
    if (-not (Test-Path $src -PathType Leaf)) { continue }
    $target = Join-Path (Join-Path $Dest "data") $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item $src $target
    $count++
  }
}

New-Item -ItemType Directory -Force -Path (Join-Path $Dest "data\launcher") | Out-Null
Copy-Item (Join-Path $Root ".github\scripts\releases\error-code-metadata.json") `
  (Join-Path $Dest "data\launcher\error-code-metadata.json")
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "data\log") | Out-Null
$rev = (git rev-parse --short HEAD).Trim()
Set-Content -Path (Join-Path $Dest "data\VERSION") -Value "$Version-$rev" -Encoding ascii -NoNewline

$dirty = git status --porcelain -- game common decompiler goalc goal_src custom_assets | Where-Object { $_ -notmatch '^\?\?' }
if ($dirty) {
  Write-Warning "There are uncommitted changes; the release contains the files as they are now:"
  $dirty | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" }
}

$Zip = Join-Path $Out "windows-$ModId-$Version.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
# (no top folder: the launcher wants gk.exe and data/ at the root of the ZIP). Written with .NET
# instead of Compress-Archive, which in Windows PowerShell 5 stores paths with "\" (not every
# unzipper understands that).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($Zip, "Create")
try {
  Get-ChildItem -Path $Dest -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($Dest.Length + 1).Replace("\", "/")
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $rel, "Optimal") | Out-Null
  }
  $archive.CreateEntry("data/log/") | Out-Null
} finally {
  $archive.Dispose()
}
Set-Content -Path (Join-Path $Out "metadata.json") -Value '{"supportedGames": ["jak1"]}' -Encoding ascii -NoNewline

$size = [math]::Round((Get-Item $Zip).Length / 1MB)
Write-Host "$count data files; $Zip ($size MB)"
Write-Host "Next: create the GitHub release $Version and upload the ZIP and metadata.json (see MODPACK.md)."
