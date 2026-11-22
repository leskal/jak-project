# Lighting mod (AI-assisted): the mod source for OpenGOAL's launcher, mods.json at the repo's root.
# Players add it in the launcher (Settings > Mods > add a source) by its raw GitHub URL:
#
#   https://raw.githubusercontent.com/leskal/jak-project/gi-mod/mods.json
#
#   powershell -ExecutionPolicy Bypass -File lighting_mod\scripts\mod_source.ps1 -Add v0.1.0
#       adds a version (newest first), after its GitHub release exists
#   powershell -ExecutionPolicy Bypass -File lighting_mod\scripts\mod_source.ps1
#       only rewrites the mod's texts (below)

param([string]$Add = "")

$ErrorActionPreference = "Stop"
$Repo = "leskal/jak-project"
$Branch = "gi-mod"
$ModId = "jak1-realistic-lighting"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Path = Join-Path $Root "mods.json"

function Now-Utc { (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") }

$versions = @()
if (Test-Path $Path) {
  $old = Get-Content $Path -Raw | ConvertFrom-Json
  if ($old.mods.$ModId.versions) { $versions = @($old.mods.$ModId.versions) }
}

if ($Add) {
  $v = $Add.TrimStart("v")
  $versions = @($versions | Where-Object { $_.version -ne $v })
  $entry = [ordered]@{
    version        = $v
    publishedDate  = Now-Utc
    assets         = [ordered]@{
      windows = "https://github.com/$Repo/releases/download/$Add/windows-$ModId-$Add.zip"
      linux   = $null
      macos   = $null
    }
    supportedGames = @("jak1")
  }
  $versions = @($entry) + $versions
}

$mod = [ordered]@{
  displayName    = "Jak 1 Realistic Lighting"
  description    = ("Realistic lighting for Jak and Daxter, gameplay untouched: a real sun with shadows " +
                    "(also from far away and from characters), sun and sky colors from the time of day, " +
                    "ambient occlusion (GTAO) and bounce light, light and shadows from fires, lamps, lava " +
                    "and glowing surfaces, bloom, auto exposure, and water with waves, clear shallows that " +
                    "turn blue-green with depth, refraction, foam, a wet shoreline, sky reflections and " +
                    "sun glints. " +
                    "Everything can be tuned in the Renderer Debug window (Debugging menu).")
  authors        = @("leskal")
  tags           = @("graphics", "visual", "lighting")
  supportedGames = @("jak1")
  websiteUrl     = "https://github.com/$Repo/tree/$Branch"
  versions       = $versions
}
$out = [ordered]@{
  schemaVersion = "1.0.0"
  sourceName    = "Jak 1 Realistic Lighting"
  lastUpdated   = Now-Utc
  mods          = [ordered]@{ $ModId = $mod }
  texturePacks  = [ordered]@{}
}
$json = ConvertTo-Json -InputObject $out -Depth 10
# (Windows PowerShell writes UTF-16 by default: write UTF-8 without a BOM)
[System.IO.File]::WriteAllText($Path, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host "$Path : $($versions.Count) version(s)"
