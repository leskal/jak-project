# Lighting mod (AI-assisted): moves the mod's own files into their own folders. Run once:
#
#   powershell -ExecutionPolicy Bypass -File lighting_mod\scripts\organize_mod_files.ps1
#
# - our engine files   game\graphics\opengl_renderer\*  ->  game\graphics\opengl_renderer\lighting_mod\
# - release scripts    scripts\lighting_mod\*           ->  lighting_mod\scripts\
# - modpack guide      MODPACK.md                       ->  lighting_mod\docs\MODPACK.md
# Tracked files are moved with "git mv" (keeps their history). Every #include of a moved header
# and game\CMakeLists.txt are updated. Running it again does nothing.
# Not moved: our edits inside OpenGOAL's own files (marked "(AI-assisted)"), the shaders (the
# engine loads them from shaders\), and mods.json (the launcher reads it from the repo's root).

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$Utf8 = New-Object System.Text.UTF8Encoding($false)

$OldDir = "game\graphics\opengl_renderer"
$NewDir = "game\graphics\opengl_renderer\lighting_mod"
$Files = @(
  "AutoExposure.cpp", "AutoExposure.h",
  "GlowLight.cpp", "GlowLight.h",
  "LightingSettings.cpp", "LightingSettings.h",
  "PointLights.cpp", "PointLights.h",
  "SSAO.cpp", "SSAO.h",
  "SunModel.h",
  "SunShadow.cpp", "SunShadow.h",
  "SurfaceLights.cpp", "SurfaceLights.h",
  "WaterWaves.h"
)

function Move-ModFile([string]$from, [string]$to) {
  if (!(Test-Path $from)) {
    if (Test-Path $to) { Write-Host "  already moved: $to" } else { Write-Host "  not found: $from" }
    return
  }
  $dir = Split-Path $to -Parent
  if (!(Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
  $tracked = git ls-files -- $from
  if ($tracked) { git mv -- $from $to } else { Move-Item $from $to }
  Write-Host "  $from -> $to"
}

function Edit-Text([string]$path, [scriptblock]$change) {
  $text = [System.IO.File]::ReadAllText($path)
  $new = & $change $text
  if ($new -ne $text) {
    [System.IO.File]::WriteAllText($path, $new, $Utf8)
    Write-Host "  updated: $path"
  }
}

Write-Host "1. Engine files"
foreach ($f in $Files) { Move-ModFile (Join-Path $OldDir $f) (Join-Path $NewDir $f) }

Write-Host "2. Includes"
$Headers = $Files | Where-Object { $_ -like "*.h" }
$OldFull = (Resolve-Path $OldDir).Path
$NewFull = (Resolve-Path $NewDir).Path
# headers that stay in the old folder (for bare #include "X.h" inside the moved files)
$Staying = Get-ChildItem $OldFull -File -Filter *.h | ForEach-Object { $_.Name }
Get-ChildItem (Join-Path $Root "game") -Recurse -File -Include *.cpp, *.h | ForEach-Object {
  $file = $_
  Edit-Text $file.FullName {
    param($t)
    foreach ($h in $Headers) {
      $t = $t.Replace("#include `"game/graphics/opengl_renderer/$h`"",
                      "#include `"game/graphics/opengl_renderer/lighting_mod/$h`"")
    }
    if ($file.DirectoryName -eq $OldFull) {
      foreach ($h in $Headers) { $t = $t.Replace("#include `"$h`"", "#include `"lighting_mod/$h`"") }
    }
    if ($file.DirectoryName -eq $NewFull) {
      foreach ($h in $Staying) {
        $t = $t.Replace("#include `"$h`"", "#include `"game/graphics/opengl_renderer/$h`"")
      }
    }
    $t
  }
}

Write-Host "3. Build file"
Edit-Text (Join-Path $Root "game\CMakeLists.txt") {
  param($t)
  foreach ($f in ($Files | Where-Object { $_ -like "*.cpp" })) {
    $t = $t.Replace("graphics/opengl_renderer/$f", "graphics/opengl_renderer/lighting_mod/$f")
  }
  $t
}

Write-Host "4. Release scripts and guide"
Move-ModFile "scripts\lighting_mod\make_release.ps1" "lighting_mod\scripts\make_release.ps1"
Move-ModFile "scripts\lighting_mod\mod_source.ps1" "lighting_mod\scripts\mod_source.ps1"
Move-ModFile "MODPACK.md" "lighting_mod\docs\MODPACK.md"
foreach ($p in @("lighting_mod\scripts\make_release.ps1", "lighting_mod\scripts\mod_source.ps1",
                 "lighting_mod\docs\MODPACK.md")) {
  if (Test-Path $p) {
    Edit-Text (Join-Path $Root $p) {
      param($t)
      $t = $t.Replace("scripts\lighting_mod\", "lighting_mod\scripts\")
      $t.Replace("(see MODPACK.md)", "(see lighting_mod\docs\MODPACK.md)")
    }
  }
}
if ((Test-Path "scripts\lighting_mod") -and !(Get-ChildItem "scripts\lighting_mod")) {
  Remove-Item "scripts\lighting_mod"
}

Write-Host ""
Write-Host "Done. Rebuild (CMake picks up the new paths by itself):  cmake --build out/build/Release"
Write-Host "Then check with:  git status"
