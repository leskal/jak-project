#include "SurfaceLights.h"

// (AI-assisted) see SurfaceLights.h

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"

namespace {

// Effect slots per level, from Project Sandover's table (kb/facts/mood.md, part 4).
// Flame / light slots hold only the effect's light (the game adds them on top of the sun).
// Lava slots hold a full copy of the lighting, so the sun's share is taken out of them.
// Lightning, caustics and fade slots are left out (flashes and shimmer, not lights).
struct EffectSlots {
  std::vector<int> add_only;
  std::vector<int> lava;
};

EffectSlots effect_slots_for_level(const std::string& name) {
  static const std::map<std::string, EffectSlots> table = {
      {"village1", {{5}, {}}},           {"beach", {{5}, {}}},
      {"jungle", {{5}, {}}},             {"training", {{5}, {}}},
      {"misty", {{2, 3, 4, 5, 6, 7}, {}}}, {"village2", {{4, 5, 6, 7}, {}}},
      {"swamp", {{4, 5, 6, 7}, {}}},     {"sunken", {{2}, {}}},
      {"sunkenb", {{2}, {}}},            {"rolling", {{4, 5, 6, 7}, {}}},
      {"firecanyon", {{}, {4, 5, 6, 7}}}, {"snow", {{4, 5, 6, 7}, {}}},
      {"village3", {{4, 5, 6}, {}}},     {"maincave", {{4, 5, 6, 7}, {}}},
      {"robocave", {{4, 5, 6, 7}, {}}},  {"lavatube", {{1}, {4, 5, 6, 7}}},
      {"citadel", {{1, 4, 5, 6}, {}}}};
  auto it = table.find(name);
  return it == table.end() ? EffectSlots{} : it->second;
}

constexpr float kMeter = 4096.f;
constexpr float kCellM = 2.f;      // grid for finding where the light is strongest
constexpr float kMergeM = 3.f;     // strong spots closer than this become one light
constexpr float kMinPeak = 0.3f;   // weaker effect light is a receiver, not a light
constexpr float kMinVertex = 0.1f;
constexpr int kMaxLights = 128;    // per level

struct Cell {
  double w = 0;
  double p[3] = {0, 0, 0};
  double c[8][3] = {};
  double cw[8] = {};
  float peak = 0;
};

}  // namespace

std::vector<SurfaceLight> find_surface_lights(const tfrag3::Level& lev) {
  std::vector<SurfaceLight> out;
  const auto es = effect_slots_for_level(lev.level_name);
  bool is_effect[8] = {};
  bool is_lava[8] = {};
  for (int s : es.add_only) {
    is_effect[s] = true;
  }
  for (int s : es.lava) {
    is_effect[s] = true;
    is_lava[s] = true;
  }

  // average colour of each texture (alpha weighted, 128 = 1): lava is white in the palette and
  // gets its colour from the texture
  std::vector<std::array<float, 3>> tex_avg(lev.textures.size(), {1.f, 1.f, 1.f});
  for (size_t i = 0; i < lev.textures.size(); i++) {
    const auto& t = lev.textures[i];
    double sum[3] = {0, 0, 0}, a = 0;
    for (u32 p : t.data) {
      const double al = std::min(1.0, ((p >> 24) & 255) / 128.0);
      sum[0] += (p & 255) * al;
      sum[1] += ((p >> 8) & 255) * al;
      sum[2] += ((p >> 16) & 255) * al;
      a += al;
    }
    if (a > 0) {
      for (int c = 0; c < 3; c++) {
        tex_avg[i][c] = (float)(sum[c] / a / 128.0);
      }
    }
  }

  const double cell = kCellM * kMeter;
  std::unordered_map<long long, Cell> cells;
  auto key = [&](double x, double y, double z) {
    const long long a = (long long)std::floor(x / cell), b = (long long)std::floor(y / cell),
                    c = (long long)std::floor(z / cell);
    return (a * 73856093LL) ^ (b * 19349663LL) ^ (c * 83492791LL);
  };

  auto visit = [&](const tfrag3::PackedTimeOfDay& cols, const tfrag3::PreloadedVertex& v,
                   const std::array<float, 3>& ta) {
    if (v.color_index >= cols.color_count) {
      return;
    }
    // the average sun slot, and the dimmest slot (night included)
    float sun[3] = {0, 0, 0};
    int nsun = 0;
    float dimmest = 10.f;
    for (int s = 0; s < 8; s++) {
      float mx = 0;
      for (int ch = 0; ch < 3; ch++) {
        mx = std::max(mx, cols.read(v.color_index, s, ch) / 128.f);
      }
      dimmest = std::min(dimmest, mx);
      if (!is_effect[s]) {
        nsun++;
        for (int ch = 0; ch < 3; ch++) {
          sun[ch] += cols.read(v.color_index, s, ch) / 128.f;
        }
      }
    }
    for (int ch = 0; ch < 3; ch++) {
      sun[ch] /= std::max(nsun, 1);
    }
    // fully bright in every slot, night included: glows by itself
    const bool self_lit = dimmest >= 1.5f;

    float ex[8][3] = {};
    float e[8] = {};
    float best = 0;
    for (int s = 0; s < 8; s++) {
      if (!is_effect[s] && !self_lit) {
        continue;
      }
      for (int ch = 0; ch < 3; ch++) {
        const float c = cols.read(v.color_index, s, ch) / 128.f;
        ex[s][ch] = (self_lit ? c * 0.5f : std::max(0.f, c - (is_lava[s] ? sun[ch] : 0.f))) * ta[ch];
        e[s] = std::max(e[s], ex[s][ch]);
      }
      best = std::max(best, e[s]);
    }
    if (best < kMinVertex) {
      return;
    }
    auto& cl = cells[key(v.x, v.y, v.z)];
    // weighted towards the brightest spots: the fire itself, not the walls it lights
    const double w = std::pow(best, 4);
    cl.w += w;
    cl.p[0] += v.x * w;
    cl.p[1] += v.y * w;
    cl.p[2] += v.z * w;
    cl.peak = std::max(cl.peak, best);
    for (int s = 0; s < 8; s++) {
      const double ws = std::pow(e[s], 4);
      for (int ch = 0; ch < 3; ch++) {
        cl.c[s][ch] += ex[s][ch] * ws;
      }
      cl.cw[s] += ws;
    }
  };

  auto visit_draws = [&](const tfrag3::PackedTimeOfDay& cols,
                         const std::vector<tfrag3::PreloadedVertex>& verts,
                         const std::vector<tfrag3::StripDraw>& draws) {
    std::vector<char> seen(verts.size(), 0);
    for (const auto& dr : draws) {
      // additive layers drawn on top (environment map reflections) are not light sources
      if (dr.mode.get_ab_enable() &&
          dr.mode.get_alpha_blend() != DrawMode::AlphaBlend::SRC_DST_SRC_DST) {
        continue;
      }
      const std::array<float, 3> ta = (dr.tree_tex_id >= 0 && dr.tree_tex_id < (int)tex_avg.size())
                                          ? tex_avg[dr.tree_tex_id]
                                          : std::array<float, 3>{1.f, 1.f, 1.f};
      auto one = [&](u32 i) {
        if (i < verts.size() && !seen[i]) {
          seen[i] = 1;
          visit(cols, verts[i], ta);
        }
      };
      for (const auto& r : dr.runs) {
        for (u32 i = 0; i < r.length; i++) {
          one(r.vertex0 + i);
        }
      }
      for (auto i : dr.plain_indices) {
        one(i);
      }
    }
  };

  // highest detail geometry only (the other geos are lower detail copies)
  for (const auto& tree : lev.tfrag_trees[0]) {
    visit_draws(tree.colors, tree.unpacked.vertices, tree.draws);
  }
  for (const auto& tree : lev.tie_trees[0]) {
    visit_draws(tree.colors, tree.unpacked.vertices, tree.static_draws);
  }

  // strongest spots first; spots within kMergeM of a light join it
  std::vector<const Cell*> strong;
  for (const auto& [k, c] : cells) {
    if (c.peak >= kMinPeak) {
      strong.push_back(&c);
    }
  }
  std::sort(strong.begin(), strong.end(),
            [](const Cell* a, const Cell* b) { return a->peak > b->peak; });
  struct Acc {
    double w = 0;
    double p[3] = {0, 0, 0};
    double c[8][3] = {};
    double cw[8] = {};
    float peak = 0;
    float seed[3] = {0, 0, 0};
  };
  std::vector<Acc> acc;
  const double md = kMergeM * kMeter;
  for (const Cell* c : strong) {
    const float p[3] = {(float)(c->p[0] / c->w), (float)(c->p[1] / c->w), (float)(c->p[2] / c->w)};
    Acc* into = nullptr;
    for (auto& a : acc) {
      const double dx = a.seed[0] - p[0], dy = a.seed[1] - p[1], dz = a.seed[2] - p[2];
      if (dx * dx + dy * dy + dz * dz < md * md) {
        into = &a;
        break;
      }
    }
    if (!into) {
      if ((int)acc.size() >= kMaxLights) {
        continue;
      }
      acc.emplace_back();
      into = &acc.back();
      for (int i = 0; i < 3; i++) {
        into->seed[i] = p[i];
      }
      into->peak = c->peak;
    }
    into->w += c->w;
    for (int i = 0; i < 3; i++) {
      into->p[i] += c->p[i];
    }
    for (int s = 0; s < 8; s++) {
      for (int ch = 0; ch < 3; ch++) {
        into->c[s][ch] += c->c[s][ch];
      }
      into->cw[s] += c->cw[s];
    }
  }

  for (const auto& a : acc) {
    SurfaceLight L;
    for (int i = 0; i < 3; i++) {
      L.pos[i] = (float)(a.p[i] / a.w);
    }
    L.peak = a.peak;
    for (int s = 0; s < 8; s++) {
      for (int ch = 0; ch < 3; ch++) {
        L.color[s][ch] = a.cw[s] > 0 ? (float)(a.c[s][ch] / a.cw[s]) : 0.f;
      }
    }
    out.push_back(L);
  }
  return out;
}
