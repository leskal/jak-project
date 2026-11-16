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
  // (AI-assisted) Jak's shadows from point lights (his spheres are set by Merc2 when drawn)
  render_state->relight.jak_pl_shadows = jak_shadows;
  render_state->relight.jak_occ_age = std::min(render_state->relight.jak_occ_age + 1, 100);
  render_state->relight.jak_occ_debug = m_occ_debug;
  m_occ_count = render_state->relight.jak_occ_count;
  m_occ_age = render_state->relight.jak_occ_age;
  m_occ_size_m = render_state->relight.jak_occ_bound[3] / 4096.f;

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
  // (AI-assisted) particles far from the camera (sky effects around the sun, kilometers away)
  // can't light anything we see; without this they take light slots.
  const float cam_x = render_state->camera_pos.x(), cam_y = render_state->camera_pos.y(),
              cam_z = render_state->camera_pos.z();
  const float max_d = 80.f * kMeter;
  for (const auto& s : samples) {
    {
      const float dx = s.x - cam_x, dy = s.y - cam_y, dz = s.z - cam_z;
      if (dx * dx + dy * dy + dz * dz > max_d * max_d) {
        continue;
      }
    }
    const s64 cx = (s64)std::floor(s.x / cell);
    const s64 cy = (s64)std::floor(s.y / cell);
    const s64 cz = (s64)std::floor(s.z / cell);
    const s64 key = (cx * 73856093LL) ^ (cy * 19349663LL) ^ (cz * 83492791LL);
    auto& c = cells[key];
    // (AI-assisted) where the light sits: mostly where the strongly colored particles are
    // (glowing embers and sparks at the base of a fire), hardly where pale smoke is. The pale
    // smoke cloud above a fire would otherwise lift the light meters above the flames, and the
    // shadows it casts would be short and weak. (Brightness still counts every particle.)
    const float mx = std::max(s.color[0], std::max(s.color[1], s.color[2]));
    const float mn = std::min(s.color[0], std::min(s.color[1], s.color[2]));
    const float sat = mx > 1e-6f ? (mx - mn) / mx : 0.f;
    const float sat2 = sat * sat;
    const float w = std::max(luminance(s.color) * (0.05f + sat2 * sat2), 1e-7f);
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
    // (AI-assisted) a different flicker for every light, from its position
    t.seed = std::fmod(std::abs(t.pos[0] * 0.000731f + t.pos[1] * 0.000419f + t.pos[2] * 0.000283f),
                       6.2831853f);
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
  m_time = std::fmod(m_time + dt, 1000.f);
  int count = 0;
  for (int li = 0; li < n; li++) {
    const auto& t = m_tracked[li];
    float c[3];
    for (int i = 0; i < 3; i++) {
      c[i] = t.color[i] * t.fade * strength;
    }
    // (AI-assisted) fire flickers: warm lights (much more red than blue) pulse irregularly, a mix
    // of a slow sway and fast flutter. Blue / white glows stay steady.
    {
      const float mx = std::max(c[0], 1e-6f);
      const float warm = std::clamp((c[0] - c[2]) / mx * 1.5f - 0.3f, 0.f, 1.f);
      if (warm > 0.f && flicker > 0.f) {
        const float t2 = m_time * 6.2831853f;
        const float s = t.seed;
        const float noise = 0.45f * std::sin(t2 * 1.7f + s) + 0.30f * std::sin(t2 * 4.3f + 2.1f * s) +
                            0.15f * std::sin(t2 * 9.1f + 3.7f * s) +
                            0.10f * std::sin(t2 * 13.9f + 5.3f * s);  // about -1 .. 1
        const float k = std::max(0.f, 1.f + flicker * warm * noise);
        for (int i = 0; i < 3; i++) {
          c[i] *= k;
        }
      }
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
  // (AI-assisted) debug: where the light closest to Jak is (at a fire: the fire's light)
  m_strongest_valid = false;
  if (render_state->relight.jak_valid) {
    const auto& rl = render_state->relight;
    float best = 1e30f;
    for (int li = 0; li < count; li++) {
      const float dx = out.pos_radius[li * 4 + 0] / kMeter - rl.jak_pos_m[0];
      const float dz = out.pos_radius[li * 4 + 2] / kMeter - rl.jak_pos_m[2];
      const float h = out.pos_radius[li * 4 + 1] / kMeter - rl.jak_feet_m;
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d * d + h * h < best) {
        best = d * d + h * h;
        m_strongest_dist_m = d;
        m_strongest_height_m = h;
        m_strongest_valid = true;
      }
    }
  }
}

void PointLights::draw_debug_window() {
  ImGui::Checkbox("Point lights from glowing particles", &enabled);
  ImGui::SliderFloat("Light strength", &strength, 0.f, 10.f);
  ImGui::SliderFloat("Group particles within (m)", &cluster_size_m, 0.25f, 5.f);
  ImGui::SliderFloat("Max light reach (m)", &max_radius_m, 1.f, 30.f);
  ImGui::SliderInt("Max lights", &max_lights, 1, 16);
  ImGui::SliderFloat("Fire flicker", &flicker, 0.f, 1.f);
  ImGui::Checkbox("Jak casts shadows from these lights", &jak_shadows);
  ImGui::Text("  Jak: %d spheres, last seen %d frames ago, %.2f m around his middle", m_occ_count,
              m_occ_age, m_occ_size_m);
  ImGui::Checkbox("Show Jak's shadow in red (debug)", &m_occ_debug);
  ImGui::Text("Glowing particles last frame: %d, lights: %d", m_last_samples, m_last_lights);
  if (m_strongest_valid) {
    ImGui::Text("  Closest light: %.1f m from Jak (sideways), %.1f m above his feet",
                m_strongest_dist_m, m_strongest_height_m);
  }
}
