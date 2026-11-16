#pragma once

// Bloom: a soft glow around bright things (fire, eco, lamps, the sun on water). (AI-assisted)
//
// Runs after the particles and before the HUD (Sprite3's hook), so glowing particles bloom and
// the HUD doesn't. Method (as in most current games):
//   1. copy the image (this also resolves MSAA)
//   2. keep only what is brighter than a threshold (soft knee, hue kept), at half size
//   3. blur by halving the size several times, then building back up, adding each level
//      ("dual filter"): a wide, smooth glow for little cost
//   4. add the glow on top of the image
// Settings are in RelightSettings (bloom_*), saved with the lighting settings.

#include "game/graphics/opengl_renderer/BucketRenderer.h"

class Bloom {
 public:
  Bloom() = default;
  ~Bloom();
  Bloom(const Bloom&) = delete;
  Bloom& operator=(const Bloom&) = delete;

  void render(SharedRenderState* render_state);

 private:
  static constexpr int kLevels = 6;
  bool setup_programs();
  void setup_targets(int w, int h);
  void free_targets();

  GLuint m_down_prog = 0;
  GLuint m_up_prog = 0;
  bool m_compile_failed = false;
  GLuint m_vao = 0;

  GLuint m_src_fbo = 0, m_src_tex = 0;  // full size copy of the image
  GLuint m_fbo[kLevels] = {}, m_tex[kLevels] = {};
  int m_lw[kLevels] = {}, m_lh[kLevels] = {};
  int m_w = 0, m_h = 0;
};
