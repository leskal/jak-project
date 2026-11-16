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
  float strength = 1.f;           // overall brightness of the lights
  float cluster_size_m = 1.5f;    // particles closer than this become one light
  float max_radius_m = 10.f;      // lights never reach further than this
  int max_lights = 16;

 private:
  struct Tracked {
    float pos[3];
    float color[3];
    float fade;    // 0..1, for smooth appearing / disappearing
    bool matched;
  };
  std::vector<Tracked> m_tracked;
  std::chrono::steady_clock::time_point m_last_time;
  bool m_have_time = false;
  int m_last_samples = 0;
  int m_last_lights = 0;
};
