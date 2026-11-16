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
- sea: see-through and lighter where shallow, animated foam where the water is < 0.45 m deep.
Needs SSAO on. The mid ocean has no depth (drawn before everything else), so far shores get none.

## Wakes
Rings of ripples from Jak: a ring buffer of 16 drops (position, start time, strength), more
often and stronger when he moves.

## Lessons
- The game's own ripple rings and splashes are particles; hiding them by texture name hit other
  textures (stale address cache) and was removed.
- Water particles must not give light (see point-lights.md).
