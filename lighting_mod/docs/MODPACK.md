# Jak 1 Realistic Lighting: making a release for OpenGOAL's launcher (AI-assisted)

The mod is installed through OpenGOAL's launcher, from a "mod source": `mods.json` at the root of
this branch. Each version is a GitHub release with one ZIP (`windows-jak1-realistic-lighting-vX.Y.Z.zip`)
holding `gk.exe`, `goalc.exe`, `extractor.exe` and a `data/` folder, like OpenGOAL's own releases.

## Players: installing it

1. In the OpenGOAL launcher: Settings > Mods > add a mod source, and paste
   `https://raw.githubusercontent.com/leskal/jak-project/gi-mod/mods.json`
2. Jak 1 > Mods > Jak 1 Realistic Lighting > Install, then Play.
3. The settings are in the Renderer Debug window (Debugging menu at the top of the game window,
   Left Alt shows/hides it). "Save lighting settings" keeps them.

## Making a new version

All commands from the repository folder, in the x64 Native Tools Command Prompt.

1. Build the statically linked programs (no DLLs needed by players), in their own folder:

       cmake --preset Release-windows-clang-static -B out/build/Release-static
       cmake --build out/build/Release-static

2. Make the ZIP (pick the new version number):

       powershell -ExecutionPolicy Bypass -File lighting_mod\scripts\make_release.ps1 -Version v0.1.0

   It writes `out\lighting-mod\windows-jak1-realistic-lighting-v0.1.0.zip` and `out\lighting-mod\metadata.json`.

3. Commit and push your changes, then on GitHub: your fork > Releases > Draft a new release.
   Tag `v0.1.0` on branch `gi-mod`, upload both files from `out\lighting-mod`, publish.

4. Add the version to the mod source and push it:

       powershell -ExecutionPolicy Bypass -File lighting_mod\scripts\mod_source.ps1 -Add v0.1.0
       git add mods.json
       git commit -m "Lighting mod: mod source v0.1.0 (AI-assisted)"
       git push myfork gi-mod

The launcher reads `mods.json` from GitHub, so players see the new version after this push.
