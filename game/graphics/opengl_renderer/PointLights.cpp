#include "PointLights.h"

// (AI-assisted) see PointLights.h

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "third-party/imgui/imgui.h"

namespace {
constexpr float kMeter = 4096.f;  // game units per meter

struct Cluster {
  double wpos[3] = {0, 0, 0};  // position * weight
  double weight = 0;
  float color[3] = {0, 0, 0};
};

float luminance(const float* c) {
  return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}
}  // namespace

void PointLights::update(SharedRenderState* render_state) {
  auto& out = render_state->point_lights;
  auto& samples = render_state->particle_light_samples;
  render_state->collect_particle_lights = enabled;

  const auto now = std::chrono::steady_clock::now();
  float dt = m_have_time ? std::chrono::duration<float>(now - m_last_time).count() : 0.f;
  dt = std::min(std::max(dt, 0.f), 0.25f);
  m_last_time = now;
  m_have_time = true;

  if (!enabled) {
    out.count = 0;
    m_tracked.clear();
    samples.clear();
    return;
  }

  // ---- 1. group last frame's glowing particles into clusters (grid cells) ----
  m_last_samples = (int)samples.size();
  const float cell = std::max(cluster_size_m, 0.1f) * kMeter;
  std::unordered_map<s64, Cluster> cells;
  for (const auto& s : samples) {
    const s64 cx = (s64)std::floor(s.x / cell);
    const s64 cy = (s64)std::floor(s.y / cell);
    const s64 cz = (s64)std::floor(s.z / cell);
    const s64 key = (cx * 73856093LL) ^ (cy * 19349663LL) ^ (cz * 83492791LL);
    auto& c = cells[key];
    const float w = std::max(luminance(s.color), 1e-6f);
    c.wpos[0] += s.x * w;
    c.wpos[1] += s.y * w;
    c.wpos[2] += s.z * w;
    c.weight += w;
    for (int i = 0; i < 3; i++) {
      c.color[i] += s.color[i];
    }
  }
  samples.clear();

  std::vector<Tracked> fresh;
  fresh.reserve(cells.size());
  for (const auto& [key, c] : cells) {
    Tracked t;
    for (int i = 0; i < 3; i++) {
      t.pos[i] = (float)(c.wpos[i] / c.weight);
      t.color[i] = c.color[i];
    }
    t.fade = 0;
    t.matched = false;
    fresh.push_back(t);
  }
  // brightest first, and only a reasonable number
  std::sort(fresh.begin(), fresh.end(),
            [](const Tracked& a, const Tracked& b) { return luminance(a.color) > luminance(b.color); });
  if ((int)fresh.size() > max_lights * 2) {
    fresh.resize(max_lights * 2);
  }

  // ---- 2. follow lights over time (no flicker, smooth in / out) ----
  for (auto& t : m_tracked) {
    t.matched = false;
  }
  const float k_pos = 1.f - std::exp(-dt / 0.08f);
  const float k_col = 1.f - std::exp(-dt / 0.15f);
  const float match_dist2 = (cell * 1.5f) * (cell * 1.5f);
  for (auto& f : fresh) {
    Tracked* best = nullptr;
    float best_d2 = match_dist2;
    for (auto& t : m_tracked) {
      if (t.matched) {
        continue;
      }
      float d2 = 0;
      for (int i = 0; i < 3; i++) {
        d2 += (t.pos[i] - f.pos[i]) * (t.pos[i] - f.pos[i]);
      }
      if (d2 < best_d2) {
        best_d2 = d2;
        best = &t;
      }
    }
    if (best) {
      for (int i = 0; i < 3; i++) {
        best->pos[i] += (f.pos[i] - best->pos[i]) * k_pos;
        best->color[i] += (f.color[i] - best->color[i]) * k_col;
      }
      best->fade = std::min(1.f, best->fade + dt / 0.2f);
      best->matched = true;
    } else {
      f.fade = 0;
      f.matched = true;
      m_tracked.push_back(f);
    }
  }
  for (auto& t : m_tracked) {
    if (!t.matched) {
      t.fade -= dt / 0.5f;  // source gone (or off screen): fade out over half a second
    }
  }
  m_tracked.erase(std::remove_if(m_tracked.begin(), m_tracked.end(),
                                 [](const Tracked& t) { return t.fade <= 0.f && !t.matched; }),
                  m_tracked.end());
  // new lights start at 0: give them this frame's fade-in
  for (auto& t : m_tracked) {
    if (t.matched && t.fade == 0.f) {
      t.fade = std::min(1.f, dt / 0.2f);
    }
  }

  // ---- 3. strongest lights go to the shaders ----
  std::sort(m_tracked.begin(), m_tracked.end(), [](const Tracked& a, const Tracked& b) {
    return luminance(a.color) * a.fade > luminance(b.color) * b.fade;
  });
  const int n = std::min<int>((int)m_tracked.size(), std::min(max_lights, 16));
  int count = 0;
  for (int li = 0; li < n; li++) {
    const auto& t = m_tracked[li];
    float c[3];
    for (int i = 0; i < 3; i++) {
      c[i] = t.color[i] * t.fade * strength;
    }
    const float lum = luminance(c);
    if (lum < 1e-4f) {
      continue;
    }
    // reach: where the light falls below ~1% (light ~ lum / distance^2), capped
    const float radius_m = std::min(max_radius_m, std::sqrt(lum / 0.01f));
    out.pos_radius[count * 4 + 0] = t.pos[0];
    out.pos_radius[count * 4 + 1] = t.pos[1];
    out.pos_radius[count * 4 + 2] = t.pos[2];
    out.pos_radius[count * 4 + 3] = radius_m * kMeter;
    out.color[count * 4 + 0] = c[0];
    out.color[count * 4 + 1] = c[1];
    out.color[count * 4 + 2] = c[2];
    out.color[count * 4 + 3] = 0;
    count++;
  }
  out.count = count;
  m_last_lights = count;
}

void PointLights::draw_debug_window() {
  ImGui::Checkbox("Point lights from glowing particles", &enabled);
  ImGui::SliderFloat("Light strength", &strength, 0.f, 10.f);
  ImGui::SliderFloat("Group particles within (m)", &cluster_size_m, 0.25f, 5.f);
  ImGui::SliderFloat("Max light reach (m)", &max_radius_m, 1.f, 30.f);
  ImGui::SliderInt("Max lights", &max_lights, 1, 16);
  ImGui::Text("Glowing particles last frame: %d, lights: %d", m_last_samples, m_last_lights);
}
