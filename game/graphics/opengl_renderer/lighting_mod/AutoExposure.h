#pragma once

// Auto exposure for the relighting mod. (AI-assisted)
//
// Like a camera or your eyes: every frame it measures how bright the lit world is on screen
// (average brightness, ignoring the sky, a bit more weight in the middle of the screen) and
// slowly changes the exposure of the relit world towards a target brightness. Dark places get
// brighter, very bright places get darker.
//
// The measurement is done on the GPU (a small 128x72 summary of the screen) and read back one
// frame later, so it doesn't make the game wait.

#include <chrono>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

class AutoExposure {
 public:
  AutoExposure() = default;
  ~AutoExposure();
  AutoExposure(const AutoExposure&) = delete;
  AutoExposure& operator=(const AutoExposure&) = delete;

  // call while the main render framebuffer is complete for the world (before particles / HUD).
  // Updates render_state->relight.auto_exposure_factor.
  void update(SharedRenderState* render_state, GLuint render_fbo, int w, int h);
  void draw_debug_text(const SharedRenderState* render_state) const;

 private:
  void setup(int w, int h);
  void free();
  bool compile();

  static constexpr int kW = 128;
  static constexpr int kH = 72;

  GLuint m_copy_fbo = 0, m_copy_color = 0, m_copy_depth = 0;
  GLuint m_sum_fbo = 0, m_sum_tex = 0;
  GLuint m_pbo[2] = {0, 0};
  bool m_pbo_full[2] = {false, false};
  int m_pbo_next = 0;
  GLuint m_program = 0, m_vao = 0;
  bool m_compile_failed = false;
  int m_w = 0, m_h = 0;

  std::chrono::steady_clock::time_point m_last;
  bool m_have_last = false;
  float m_measured = 0.f;
  float m_coverage = 0.f;
};
