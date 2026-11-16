# Glowing surfaces as lights (AI-assisted)

Code: `lighting_mod/SurfaceLights.*` (found on the loader thread, `loader/Loader.cpp`), used by
`PointLights.cpp`; slot weights recorded in `background/TFragment.cpp` (`level_itimes`).

## The idea
Jak 1 colours level vertices from 8 time-of-day palette slots. Per level some slots are effects
the game drives itself: flames (flicker), lights, lava (pulse). Which slot is which comes from
Project Sandover's study of the mood code (`sandover/kb/facts/mood.md`, part 4). The baked colour
in those slots is the light the effect throws, and it is strongest at the source.

## Rule
- Flame / light slots hold only the effect's light (added on top of the sun): use them as is.
- Lava slots hold a full copy of the lighting: take out the average sun slot.
- Fully bright in every slot, night included (>= 1.5): self-lit (lava lakes, glowing panels).
- Multiply by the texture's average colour (lava is white in the palette, red in the texture).
- Skip additive draws (environment map reflections are additive and carry the slot colours).
- 2 m grid weighted by brightness^4 finds the hot spots; spots within 3 m join; peak >= 0.3;
  at most 128 per level. About 30-40 ms per level, on the loader thread.
- Each frame: colour = sum over slots of (the game's slot weight x baked colour), so lights
  flicker and pulse in step with the game. Weight 64 = 1.0.

## What it found (checked on the level files)
- village1: five hut cooking fires (orange, slot 5) and Samos's green eco equipment (self-lit).
- firecanyon: the lava lake (orange, self-lit) and lava pools (red, slots 4-7).

## Not yet
- Lamps that glow only at night (night slots, not effect slots).
- Lightning (flashes), caustics and fade slots are left out on purpose.

## Dead end
Looking for additive level geometry (glowing meshes): village1 and firecanyon have none apart
from environment-map reflection passes. Fire flames are particles or actors, not level meshes.
