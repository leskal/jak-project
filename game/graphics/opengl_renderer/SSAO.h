#pragma once

// Screen-space ambient occlusion (SSAO) post pass. (AI-assisted)
//
// Runs in the middle of the frame, after the opaque world has been drawn and before
// transparent things / sprites / HUD. It:
//   1. copies the depth buffer of the main render target into a texture
//   2. computes ambient occlusion from that depth (optionally at half resolution)
//   3. blurs it with a depth-aware blur
//   4. darkens the image by multiplying it with the AO
// It also does a simple screen-space global illumination (SSGI): nearby surfaces that face a pixel
// add a bit of their color to it ("color bleeding" / bounce light).

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Profiler.h"

class SSAO {
 public:
  SSAO();
  ~SSAO();
  SSAO(const SSAO&) = delete;
  SSAO& operator=(const SSAO&) = delete;

  // run the effect. Call with the main render framebuffer bound.
  // msaa: sample count of the main render framebuffer (needed to know when to rebuild textures)
  void render(SharedRenderState* render_state, ScopedProfilerNode& prof, int msaa);

  // ImGui settings panel (shown in the "Render Debug" window)
  void draw_debug_window();

  // --- settings ---
  bool enabled = true;
  bool half_resolution = false;  // full res looks best; half res is faster
  bool blur = true;
  float strength = 0.5f;       // 0 = no effect, 1 = full AO
  float radius_m = 0.6f;       // sample radius in meters
  float intensity = 1.2f;      // AO contrast
  float bias_m = 0.03f;        // ignore occluders closer than this to the surface plane (meters)
  float bias_per_m = 0.012f;   // extra bias per meter of distance (hides depth precision steps)
  float max_distance_m = 50.f; // fade the effect out towards this distance (meters)
  int debug_view = 0;          // 0 = normal, 1 = AO only, 2 = linear depth, 3 = GI only
  // bounce light (SSGI)
  bool gi_enabled = true;
  float gi_strength = 1.0f;    // how much bounce light is added
  float gi_radius_m = 2.0f;    // how far light bounces (meters)

  // Sun aware: in reality AO only blocks sky light and bounce light is small next to direct sun.
  // Using the sun shadow map, the effect knows where the sun hits and is weaker there.
  bool sun_aware = true;
  float ao_sun_protect = 0.6f;  // how much AO is removed on surfaces in direct sun (0..1)
  float gi_sun_reduce = 0.7f;   // how much bounce light is removed in direct sun (0..1)
  // Jak 1: the effect runs right after this bucket is drawn (see buckets.h, jak1::BucketId).
  // Everything drawn after it is not darkened and does not cast AO.
  // 55 = MERC_AFTER_PRIS: Jak himself is drawn there, so this is the first bucket where he is included.
  int jak1_after_bucket = 55;

 private:
  void setup_targets(int w, int h, int msaa);
  void free_targets();
  void draw_quad();

  GLuint m_vao = 0;
  GLuint m_vbo = 0;

  // full res copy of the depth buffer
  GLuint m_depth_fbo = 0;
  GLuint m_depth_tex = 0;

  // copy of the image as it was before we change it (source colors for bounce light)
  GLuint m_color_fbo = 0;
  GLuint m_color_tex = 0;

  // AO result (R = ao, G = view-space depth in meters), bounce light (RGB) and blur temps.
  // Each fbo has two color attachments: [0] = ao/depth, [1] = bounce light
  GLuint m_ao_fbo[2] = {0, 0};
  GLuint m_ao_tex[2] = {0, 0};
  GLuint m_gi_tex[2] = {0, 0};

  int m_w = 0, m_h = 0, m_msaa = 0;
  int m_ao_w = 0, m_ao_h = 0;
  bool m_targets_half_res = true;
  bool m_have_targets = false;
};
