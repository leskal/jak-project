#pragma once

// Point lights from glowing particles, for the lighting mod. (AI-assisted)
//
// The particle (sprite) renderer reports every additive ("glowing") particle it sees: its 3D
// position, color and size (SharedRenderState::particle_light_samples). Once per frame, this
// groups nearby particles into a small number of point lights, follows them over time so they
// don't flicker, and hands them to the level and character shaders, which light surfaces with
// them using real distance falloff and surface normals.

#include <chrono>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

class PointLights {
 public:
  void update(SharedRenderState* render_state);
  void draw_debug_window();

  // --- settings ---
  bool enabled = true;
  float strength = 4.03f;         // overall brightness of the lights (AI-assisted: tuned default)
  float cluster_size_m = 3.65f;   // particles closer than this become one light (tuned)
  float max_radius_m = 18.17f;    // lights never reach further than this (tuned)
  int max_lights = 16;
  bool jak_shadows = true;        // (AI-assisted) Jak casts soft shadows from point lights
  float flicker = 0.35f;          // (AI-assisted) how much fire-colored lights flicker (0 = steady)
  // (AI-assisted) lights from glowing level surfaces (fires, lava, lamps; see SurfaceLights.h)
  bool surface_lights = true;
  float surface_strength = 0.3f;

 private:
  struct Tracked {
    float pos[3];
    float color[3];
    float fade;    // 0..1, for smooth appearing / disappearing
    bool matched;
    float seed;    // (AI-assisted) random phase for the flicker
  };
  std::vector<Tracked> m_tracked;
  std::chrono::steady_clock::time_point m_last_time;
  bool m_have_time = false;
  float m_time = 0.f;  // (AI-assisted) seconds, for the flicker
  int m_last_surface = 0;  // (AI-assisted) glowing surface lights used last frame
  // (AI-assisted) debug for Jak's shadows
  bool m_occ_debug = false;
  int m_occ_count = 0;
  int m_occ_age = 0;
  float m_occ_size_m = 0.f;
  bool m_strongest_valid = false;
  float m_strongest_dist_m = 0.f;
  float m_strongest_height_m = 0.f;
  int m_last_samples = 0;
  int m_last_lights = 0;
};
