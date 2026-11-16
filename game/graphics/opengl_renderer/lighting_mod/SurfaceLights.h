#pragma once

// Lights from glowing level surfaces, for the lighting mod. (AI-assisted)
//
// Jak 1 colours its level geometry from 8 "time of day" palette slots per vertex. On most levels
// some slots are not times of day but effects that the game drives itself every frame: flames
// (flickering), lamps / room lights, and lava (pulsing). Which slot is which per level comes from
// Project Sandover's study of the game's mood code (github.com/bouzou4/project-sandover,
// sandover/kb/facts/mood.md).
//
// When a level loads (on the loader thread), find_surface_lights() looks for where that effect
// light is strongest (the fire, the lava, the lamp) and makes a light there with the baked colour
// of each effect slot. Geometry that is fully bright in every slot, night included, glows by itself
// (lava lakes, glowing panels) and becomes a light too. Each frame, the light's colour is the
// game's current weight of each slot times that colour (PointLights), so it flickers and pulses
// in step with the game.

#include <string>
#include <vector>

namespace tfrag3 {
struct Level;
}

struct SurfaceLight {
  float pos[3];        // world position (game units)
  float color[8][3];   // light colour per palette slot (128 = 1.0)
  float peak;          // strongest baked effect light at this spot (debug)
};

std::vector<SurfaceLight> find_surface_lights(const tfrag3::Level& level);
