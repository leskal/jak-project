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

// (AI-assisted) wakes: rings of ripples spreading from where Jak is. While he moves, a new ring
// starts every ~0.35 s (stronger when faster); standing still, a gentle one every ~1.6 s.
// The shader only shows them on water surfaces at the right height (Jak in the water).
struct WaterDrop {
  float x, y, z, t0, amp;
};
constexpr int kMaxWaterDrops = 16;

struct WaterWakeState {
  WaterDrop drops[kMaxWaterDrops];
  int next = 0;
  int count = 0;
  float last_t = -1.f;
  float last_emit_t = -100.f;
  float last_pos[3] = {0, 0, 0};
  float last_pos_t = 0.f;
  float speed = 0.f;
  bool have_last = false;
};

inline WaterWakeState& water_wake_state() {
  static WaterWakeState s;
  return s;
}

inline void update_water_wakes(const SharedRenderState::RelightSettings& rl, float t) {
  auto& s = water_wake_state();
  if (t < s.last_t) {
    s.last_emit_t = -100.f;  // the clock wrapped
    s.count = 0;
    s.have_last = false;
  }
  s.last_t = t;
  if (!rl.jak_valid || !rl.water_wakes) {
    s.have_last = false;
    return;
  }
  // Jak's position only changes once per frame, but this runs several times per frame:
  // measure his speed only when the position actually changed.
  const float dx = rl.jak_pos_m[0] - s.last_pos[0];
  const float dz = rl.jak_pos_m[2] - s.last_pos[2];
  const bool changed = !s.have_last || dx != 0.f || dz != 0.f ||
                       rl.jak_pos_m[1] != s.last_pos[1];
  if (changed) {
    if (s.have_last && t > s.last_pos_t) {
      float speed = std::sqrt(dx * dx + dz * dz) / (t - s.last_pos_t);
      s.speed = speed > 30.f ? 0.f : speed;  // a jump of position = teleport / respawn
    }
    for (int c = 0; c < 3; c++) {
      s.last_pos[c] = rl.jak_pos_m[c];
    }
    s.last_pos_t = t;
    s.have_last = true;
  } else if (t - s.last_pos_t > 0.2f) {
    s.speed = 0.f;  // hasn't moved for a while
  }

  const bool moving = s.speed > 0.4f;
  const float interval = moving ? 0.3f : 1.2f;
  if (t - s.last_emit_t >= interval) {
    s.last_emit_t = t;
    WaterDrop& d = s.drops[s.next];
    d.x = rl.jak_pos_m[0];
    d.y = rl.jak_feet_m;
    d.z = rl.jak_pos_m[2];
    d.t0 = t;
    d.amp = moving ? std::min(0.03f + s.speed * 0.015f, 0.09f) : 0.02f;
    s.next = (s.next + 1) % kMaxWaterDrops;
    s.count = std::min(s.count + 1, kMaxWaterDrops);
  }
}

inline int water_wake_active_count(float t) {
  auto& s = water_wake_state();
  int n = 0;
  for (int i = 0; i < s.count; i++) {
    const float age = t - s.drops[i].t0;
    if (age >= 0.f && age < 4.f) {
      n++;
    }
  }
  return n;
}

inline void set_wake_uniforms(GLuint prog, const SharedRenderState::RelightSettings& rl) {
  auto& s = water_wake_state();
  float drops[4 * kMaxWaterDrops] = {};
  float amps[kMaxWaterDrops] = {};
  int n = 0;
  if (rl.water_wakes) {
    for (int i = 0; i < s.count; i++) {
      const auto& d = s.drops[i];
      drops[n * 4 + 0] = d.x;
      drops[n * 4 + 1] = d.y;
      drops[n * 4 + 2] = d.z;
      drops[n * 4 + 3] = d.t0;
      amps[n] = d.amp * rl.wake_strength;
      n++;
    }
  }
  glUniform1i(glGetUniformLocation(prog, "drop_count"), n);
  if (n > 0) {
    glUniform4fv(glGetUniformLocation(prog, "drops"), n, drops);
    glUniform1fv(glGetUniformLocation(prog, "drop_amp"), n, amps);
  }
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
  const float t = water_wave_time();
  glUniform1f(loc("wave_time"), t);
  update_water_wakes(rl, t);
  set_wake_uniforms(prog, rl);
  glUniform1f(loc("wave_height"), rl.wave_height_m);
  glUniform1f(loc("wave_range"), rl.wave_range_m);
  glUniform1f(loc("ripple_strength"), rl.water_ripples);
  glUniform1f(loc("water_reflect"), rl.water_reflect);
  glUniform1f(loc("water_spec"), rl.water_spec);
  return true;
}
