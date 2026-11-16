#pragma once

// Sun shadows for the relighting mod. (AI-assisted)
//
// Once per frame, before the level is drawn, the level geometry around the camera (tfrag and tie)
// is drawn from the sun's point of view into a depth texture (a "shadow map"). The level shaders
// then check every pixel against it to know whether the sun can see that point.
//
// The geometry is taken directly from the level data the loader already put on the GPU, so this
// does not depend on the game sending anything extra.

#include <chrono>
#include <map>
#include <tuple>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Profiler.h"

class SunShadow {
 public:
  SunShadow() = default;
  ~SunShadow();
  SunShadow(const SunShadow&) = delete;
  SunShadow& operator=(const SunShadow&) = delete;

  // Draw the shadow map. Fills in render_state->relight.shadow_* for the level shaders.
  void render(SharedRenderState* render_state, ScopedProfilerNode& prof);
  // (AI-assisted) cube shadow maps for the strongest point lights (call after PointLights::update)
  void render_point_shadows(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window();
  // shows the shadow map in the bottom left corner (debugging)
  void draw_overlay(SharedRenderState* render_state);

  // --- settings ---
  bool enabled = true;
  int resolution = 4096;       // shadow map size in pixels
  float half_size_m = 60.f;    // the shadow map covers this many meters around the camera
  // second, bigger shadow map for things far away (less sharp) (AI-assisted)
  bool point_shadows = true;     // point lights (fire, glowing things) cast shadows
  int point_shadow_count = 4;    // how many of the strongest point lights get shadows
  int point_shadow_res = 512;    // size of each cube face
  static constexpr int kMaxPointShadows = 4;
  bool far_enabled = true;
  float far_half_size_m = 400.f;
  int far_resolution = 4096;
  float depth_range_m = 400.f; // how far along the sun direction things can cast shadows
  float strength = 0.6f;       // 0 = no shadows, 1 = full shadows
  float normal_offset_m = 0.05f;  // pushes the lookup off the surface to avoid "acne"
  bool tfrag_casts = true;     // ground/walls cast shadows
  bool tie_casts = true;       // objects cast shadows
  bool show_map = false;       // show the shadow map on screen
  bool chars_cast = true;      // characters cast shadows (on the level, one frame later)
  bool chars_receive = true;   // characters get darker in shadows
  int char_resolution = 2048;
  bool hide_blob_shadow = true;  // hide the game's own blob shadow when characters cast real ones
  bool blob_hidden_by_mod = false;  // (internal) we turned the blob shadow renderer off

 private:
  void setup_target();
  void free_target();
  void draw_level_geometry(SharedRenderState* render_state,
                           ScopedProfilerNode& prof,
                           GLint use_tex_loc);
  bool compile_point_program();
  void setup_point_target();
  void free_point_target();
  GLuint m_point_program = 0;
  bool m_point_compile_failed = false;
  GLuint m_point_fbo = 0;
  GLuint m_point_tex = 0;
  int m_point_res = 0;
  int m_point_shadow_count = 0;
  void setup_far_target();
  void free_far_target();
  GLuint m_far_fbo = 0;
  GLuint m_far_tex = 0;
  int m_far_res = 0;

  // filters the game's sun (see RelightSettings::game_sun_*)
  void update_game_sun(SharedRenderState::RelightSettings& rl);
  std::chrono::steady_clock::time_point m_last_time;
  bool m_have_time = false;
  float m_pending_time = 0.f;
  float m_pending_dir[3] = {0, 1, 0};

  struct TreeGl {
    GLuint vao = 0;
    GLuint own_index_buffer = 0;  // only for tfrag (the loader doesn't make one)
    u64 last_used_frame = 0;
  };
  // key: level load id, kind (0 = tfrag, 1 = tie), tree index
  std::map<std::tuple<u64, int, int>, TreeGl> m_trees;

  GLuint m_fbo = 0;
  GLuint m_depth_tex = 0;
  int m_tex_res = 0;
  u64 m_frame = 0;
  int m_last_draw_calls = 0;
  GLenum m_fbo_status = 0;
  GLuint m_quad_vao = 0, m_quad_vbo = 0;

  // character shadow maps: one is drawn this frame, the other (last frame's) is read
  void setup_char_targets();
  GLuint m_char_fbo[2] = {0, 0};
  GLuint m_char_tex[2] = {0, 0};
  int m_char_res = 0;
  int m_char_write = 0;
  bool m_char_prev_valid = false;
  float m_char_prev_mat[16] = {};
};
