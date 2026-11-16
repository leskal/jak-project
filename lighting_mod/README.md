# Jak 1 Realistic Lighting (AI-assisted)

A lighting mod for OpenGOAL's Jak and Daxter: The Precursor Legacy. Gameplay, levels and art are
untouched; only how the world is lit changes. Installed through the OpenGOAL launcher as a mod
(see `docs/MODPACK.md`). Everything can be tuned in game: Debugging menu > Renderer Debug
("Save lighting settings" keeps it in `%APPDATA%\OpenGOAL\lighting_mod.json`).

Branch `gi-mod` of github.com/leskal/jak-project. Written with AI help (Claude); every change
inside OpenGOAL's own files is marked `(AI-assisted)`, and no AI pull requests go upstream
(see the repo's AGENTS.md).

## Features

- **Sun and sky**: a physical sun with sky and sun colours from the time of day, the level's own
  sky tint, and auto exposure.
- **Shadows**: sun shadows near and far (two cascades), from the level and from characters,
  and shadows from point lights (cube maps).
- **Ambient occlusion and bounce light** (SSAO / SSGI), aware of where the sun shines.
- **Point lights from glowing particles**: fire, eco, sparks; fire flickers. Water particles
  (rings, splashes, fountain spray) give no light.
- **Glowing surfaces as lights**: fires, lava and lamps found in the level data when a level
  loads, flickering and pulsing with the game's own effects (see `kb/surface-lights.md`).
- **Jak's soft shadows from point lights** (spheres on his skeleton).
- **Water**: moving waves and ripples on the sea and inland water, sun glints, wakes around Jak,
  clear shallow water and shore foam.
- **Smoother level normals** (no lighting seams on the ground).

## Layout

```
lighting_mod/                         this folder: everything that is not engine code
  README.md                           this file
  docs/MODPACK.md                     installing, and making a release for the launcher
  kb/                                 knowledge base: how things work and what we learned
  scripts/make_release.ps1            builds the release ZIP for the launcher
  scripts/mod_source.ps1              updates mods.json (the launcher's mod source)
  scripts/organize_mod_files.ps1      one-time move into this layout
game/graphics/opengl_renderer/lighting_mod/   the mod's own engine code
mods.json                             the mod source (stays at the root: launchers read it there)
```

Our edits inside OpenGOAL's own files (renderers such as Merc2, TFragment, Sprite3, the ocean,
and the shaders in `game/graphics/opengl_renderer/shaders/`) stay where the engine expects them;
search for `(AI-assisted)` to find them.

### Engine files (`game/graphics/opengl_renderer/lighting_mod/`)

| File | What it does |
|---|---|
| `SSAO` | ambient occlusion and bounce light; also shares its depth copy with the water |
| `SunShadow` | sun shadow cascades and point light shadow cube maps |
| `SunModel.h` | sun and sky colours from the time of day |
| `AutoExposure` | measured auto exposure |
| `PointLights` | lights from glowing particles and surfaces; flicker; Jak's shadow spheres |
| `SurfaceLights` | finds a level's fires, lava and lamps when it loads |
| `GlowLight` | older screen-space glow light |
| `WaterWaves.h` | shared water settings: waves, wakes, clear water and foam |
| `LightingSettings` | saving and loading the settings file |

## Credits

- OpenGOAL (github.com/open-goal/jak-project), which this is built on.
- Project Sandover (github.com/bouzou4/project-sandover, ISC license): its research on Jak 1's
  time-of-day palette slots (which slots are flames, lava and lamps per level) is what the
  glowing surface lights are built on, and its notes shaped our water and folder layout.
