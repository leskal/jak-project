# Water (AI-assisted)

Code: `lighting_mod/WaterWaves.h` (shared uniforms, wakes), `shaders/ocean_common.vert/.frag`
(sea), `shaders/merc2.frag` (inland water: `water-anim` models), `ocean/CommonOceanRenderer.cpp`.

## Sea
- Waves: 8 wind-spread sine waves move the near ocean's vertices (unproject, displace,
  reproject); 4 octaves of gradient-noise ripples change only the lighting.
- Anti-aliasing: each wave fades out when it is smaller than a few pixels (the pixel size comes
  from distance and angle, not screen derivatives, which jump at triangle edges and drew lines).
- Gaps between displaced and flat water are filled with a stencil pass.
- Mid ocean: no displacement (it is drawn first, and moving it opened a seam with the near ocean).

## Clear water and shore foam
SSAO copies the depth after bucket 55, before the inland water (58, 61) and the near ocean (63),
so the copy holds the ground under the water. From it:
- inland water: alpha x mix(0.12, 1, 1 - exp(-ray / depth_m)), ray = view distance through water;
- sea: see-through and lighter where shallow; foam as a thin broken edge line (last few cm) plus
  lace patches that are gone by ~0.35 m, from three drifting noise layers.
Needs SSAO on. The mid ocean has no depth (drawn before everything else), so far shores get none.
The near ocean is three passes: colour, alpha, then an envmap layer weighted by that alpha. The
shallow-water alpha must be applied in the alpha pass too, or the envmap layer keeps the
shallows opaque (first version's mistake).

## Wet shoreline
Level shaders (tfrag3, tie_wind, shrub, etie_base: `wet_shore()`): in a band above the sea
(1 m on flat ground, half on steep faces, top edge wandering with noise) the ground is darker
(x 0.68) with a sun glint on the water film. The game's sea is at height 0 (-24 m for the sunken
city when the camera is deep down; `ocean.gc`). Only while the sea was drawn in the last 0.5 s.
Defaults approved in game (2026-11-16): clear depth 1.5 m, foam 1.0, shallow tint 0.5, band 1 m,
wet brightness 0.68.

## Ported from Project Sandover (2026-11-16, untested in game)
ISC licence, credited in README. Settings under Water:
- **Sky in reflections** (0.6): reflection = horizon (fog colour) low down, a bluer zenith at the
  same brightness higher up (R.y 0..0.48), plus a sun glow pow(R.L, 24) x 0.6 in sunlight only;
  hazes back to the fog colour over 400 m. 0 = the old flat fog colour.
- **Light through waves** (1): crests x (1 + 0.25 h), troughs darker; subsurface
  0.6 x max(h, 0) x (0.35 + 1.4 x back-light^3) on a lightened water colour. h = wave height /
  (0.65 x amplitude).
- **Foam on wave crests** (0.8): our waves are sums of sines, not Gerstner, so their Jacobian is
  approximated as J = 1 - 2.5 x sum(a k sin(phase)) (the curvature); foam where J < 0.9 and
  h > 0.2, cut from three drifting noise octaves; none in the first 1.5 m or past 200 m.
- **Hide sea texture repeat** (1): Mikkelsen hex tiling copied from Sandover: 56 m triangle grid,
  3 samples rotated up to 180 degrees, weights^5, textureGrad with the world uv's derivatives.
  Near colour and alpha passes and the mid ocean; not the envmap pass.
- **Calm water in the sunken city**: camera below -100 m (ocean.gc's test) uses waves 4.5x
  shorter and 0.18x lower (Sandover's lake set is 6 m / 0.07 m at most), and no sky or sun.
- Merc water in maincave, darkcave, robocave, sunken, sunkenb: no sky reflection or sun glint
  (is_water = 2); the game's envmap layer stays. Sandover's rule.
- **Misty quicksand as wet mud** (water-anim ... mud, is_water = 3): colour x 0.8, rough sky
  reflection capped at 0.15, broad sun highlight pow 24. Sandover's numbers.
- Not ported: Gerstner + GPU tessellation (large), material-based wet shore (needs their data).
  Sandover's sunken pools are Generic2 (depth only), not touched here.

## Wakes
Rings of ripples from Jak: a ring buffer of 16 drops (position, start time, strength), more
often and stronger when he moves.

## Lessons
- The game's own ripple rings and splashes are particles; hiding them by texture name hit other
  textures (stale address cache) and was removed.
- Water particles must not give light (see point-lights.md).
