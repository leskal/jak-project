#pragma once

// Water waves for the relighting mod. (AI-assisted)
//
// The wave pattern itself lives in the shaders (ocean_common.vert/.frag, merc2.frag). This sets
// the uniforms they need: time, wave settings, sun, and the camera, so the shaders can turn
// screen positions back into world positions (the ocean is sent to us already projected).

#include <chrono>
#include <cmath>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SunModel.h"

inline float water_wave_time() {
  static const auto t0 = std::chrono::steady_clock::now();
  const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
  return std::fmod(t, 3600.f);
}

// prog must be active. Returns false (and turns the waves off in the shader) if the camera isn't
// known yet or waves are disabled.
inline bool set_water_uniforms(GLuint prog, SharedRenderState* rs) {
  const auto& rl = rs->relight;
  auto loc = [&](const char* n) { return glGetUniformLocation(prog, n); };

  // same projection as make_new_cam_mat() / SSAO.cpp
  const auto& persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  const float hvdf_z = rs->camera_hvdf_off.z();
  const float pzw = persp[2][3];
  const bool ok = rl.water_waves && std::abs(pzw) > 1e-12f && std::abs(fog) > 1e-12f;
  glUniform1i(loc("waves_on"), ok ? 1 : 0);
  if (!ok) {
    return false;
  }
  const float scissor_height = rs->version == GameVersion::Jak1 ? 448.f : 416.f;
  const float height_scale = rs->version == GameVersion::Jak1 ? 1.f : 0.5f;
  const float y_adjust = (512.f / scissor_height) * height_scale;
  const float pc_pxx = fog * persp[0][0] / 256.f;
  const float pc_pyy = -fog * persp[1][1] / 128.f;
  const float depth_scale = fog * persp[2][2] / 8388608.f + (hvdf_z / 8388608.f - 1.f) * pzw;
  const float depth_offset = fog * persp[3][2] / 8388608.f;
  const float depth_a = depth_scale / pzw;
  const float depth_k = depth_offset;

  float cam_rot[9];
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      cam_rot[c * 3 + r] = rs->camera_rot[c][r];
    }
  }
  glUniformMatrix3fv(loc("cam_rot"), 1, GL_FALSE, cam_rot);
  glUniform3f(loc("cam_trans"), rs->camera_pos.x(), rs->camera_pos.y(), rs->camera_pos.z());
  glUniform4f(loc("cam_params"), depth_k, depth_a, pc_pxx, pc_pyy * y_adjust);
  glUniform1f(loc("cam_pzw"), pzw);
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  glUniform4f(loc("viewport"), (float)vp[0], (float)vp[1], (float)std::max(vp[2], 1),
              (float)std::max(vp[3], 1));

  float L[3], sun[3], sky[3], ground[3];
  relight_sun_dir(rl, L);
  relight_light_colors(rl, L, sun, sky, ground, &rs->fog_color[0]);
  glUniform3f(loc("sun_dir"), L[0], L[1], L[2]);
  glUniform3f(loc("sun_color"), sun[0], sun[1], sun[2]);

  glUniform1i(loc("displace"), 1);
  glUniform1f(loc("wave_time"), water_wave_time());
  glUniform1f(loc("wave_height"), rl.wave_height_m);
  glUniform1f(loc("wave_range"), rl.wave_range_m);
  glUniform1f(loc("ripple_strength"), rl.water_ripples);
  glUniform1f(loc("water_reflect"), rl.water_reflect);
  glUniform1f(loc("water_spec"), rl.water_spec);
  return true;
}
