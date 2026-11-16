# Point lights (AI-assisted)

Code: `lighting_mod/PointLights.*`, collection in `sprite/Sprite3.cpp`, shading in the level and
merc shaders (`point_lights()`), shadows in `lighting_mod/SunShadow.cpp`.

## How it works
1. Sprite3 records every additive ("glowing") particle that is not HUD: position, colour x alpha x
   area (area capped at 4 m^2). Blend (Cs - 0) * As + Cd.
2. PointLights groups them on a grid (`cluster_size_m`), follows the groups over time (no
   flicker), and gives the strongest 16 to the shaders, with a reach from their brightness.
3. Lights sit mostly where the strongly coloured particles are (saturation^4 weighting), not at
   the middle of a pale smoke cloud: a fire's light then sits at the flames, low, and the shadows
   it casts are long enough to see.
4. Warm lights flicker (`flicker`); each has its own random phase.
5. Glowing surfaces (see `surface-lights.md`) are added as extra samples before step 2.
6. Jak's shadow: up to 24 spheres on his joints (Merc2), tested per pixel against each light in
   the level and merc shaders, with a soft edge from the light's size (0.3 m).

## What gives no light
- Water particles: textures starting `water-` or containing `splash`, `lakedrop`, `bubble`
  (by name, per texture address, cached per frame).
- Pale bluish particles (blue >= 1.15 x red, saturation < 0.55): fountain spray, which uses the
  same `middot` / `bigpuff` textures as sparks and eco.
- Particles more than 80 m from the camera (sky effects around the sun, kilometres away).

## Lessons (mistakes made)
- Restricting lights to camera-facing particles (to stop water rings glowing) silently removed
  every flat glowing particle. Exclude the specific culprit instead.
- The texture-address cache must be cleared every frame: the game reuses addresses for other
  textures, and a stale entry hid unrelated particles.
- Before changing the light logic, find out what the light source really is. The warp gate
  hunt (ranking, merging, colour measure, see-through particles) went the wrong way; it made
  footsteps glow and lights 3x dimmer, and was reverted.
- Rank by brightness, but ignore far particles: one sky effect took a light slot for nothing.
- Debug lines (light positions next to the camera and Jak positions) settled questions that
  screenshots could not.
