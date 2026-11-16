#pragma once

// "Glow light" for the lighting mod: glowing things (particles like the teleporter swirl, eco,
// fire, and optionally very bright surfaces) light up the surfaces around them. (AI-assisted)
//
// Works in screen space, inside the particle (sprite) renderer:
//   stage 0 (before particles): copy the image
//   stage 1 (after particles, before the HUD): copy the image again. The difference is the light
//   the particles added. Spread it over nearby surfaces (using depth to know what is near what)
//   and add it to the image, tinted by each surface's color.

#include "game/graphics/opengl_renderer/BucketRenderer.h"

class GlowLight {
 public:
  GlowLight();
  ~GlowLight();
  GlowLight(const GlowLight&) = delete;
  GlowLight& operator=(const GlowLight&) = delete;

  void stage(int stage, SharedRenderState* render_state);
  void draw_debug_window();

  // --- settings ---
  bool enabled = false;  // replaced by PointLights, kept as an option
  float strength = 2.5f;           // how bright the light on surfaces is
  float radius_m = 4.f;            // how far the light reaches (meters)
  float particle_gain = 1.f;       // how much glowing particles emit
  float bright_surface_gain = 0.f; // how much very bright surfaces emit (lamps, glowing parts)
  float bright_threshold = 0.9f;   // what counts as "very bright"
  bool show_only = false;          // debug: show only the glow light

 private:
  void setup_targets(int w, int h);
  void free_targets();
  void draw_quad();

  GLuint m_vao = 0, m_vbo = 0;
  GLuint m_before_fbo = 0, m_before_tex = 0;
  GLuint m_after_fbo = 0, m_after_tex = 0;
  GLuint m_depth_fbo = 0, m_depth_tex = 0;
  GLuint m_emit_fbo = 0, m_emit_tex = 0;
  GLuint m_light_fbo = 0, m_light_tex = 0;
  int m_w = 0, m_h = 0, m_qw = 0, m_qh = 0;
  bool m_have_targets = false;
  bool m_before_valid = false;
};
