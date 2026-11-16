#pragma once

// Sun direction and sun / sky / ground light colors for the relighting mod. (AI-assisted)
//
// With "auto_sun_color", the colors follow the sun's height like in a real atmosphere:
// a low sun has to shine through much more air, which scatters away blue light first, so it
// becomes dimmer and orange; the sky gets darker and warmer around sunset and dim at night.

#include <algorithm>
#include <cmath>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

inline void relight_sun_dir(const SharedRenderState::RelightSettings& rl, float L[3]) {
  if (rl.use_game_sun && rl.game_sun_valid) {
    for (int i = 0; i < 3; i++) {
      L[i] = rl.game_sun_dir[i];
    }
  } else {
    const float az = rl.sun_azimuth_deg * 3.14159265f / 180.f;
    const float el = rl.sun_elevation_deg * 3.14159265f / 180.f;
    L[0] = std::cos(el) * std::sin(az);
    L[1] = std::sin(el);
    L[2] = std::cos(el) * std::cos(az);
  }
}

namespace relight_detail {
inline float lum(const float c[3]) {
  return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// the atmosphere model, without exposure adaptation
inline void atmosphere_colors(const SharedRenderState::RelightSettings& rl,
                              const float L[3],
                              const float sky_base[3],
                              float warm_amount,
                              float sun[3],
                              float sky[3],
                              float ground[3]) {
  const float* base_sun =
      (rl.use_game_colors && rl.game_sun_valid) ? rl.game_sun_color : rl.sun_color;
  const float s = std::min(std::max(L[1], -1.f), 1.f);  // sin(sun height)
  const float s_pos = std::max(s, 0.f);
  const float el_deg = std::asin(s_pos) * 57.29578f;
  // how much air the sunlight goes through (Kasten-Young), 1 = sun straight up
  const float airmass = 1.f / (s_pos + 0.50572f * std::pow(el_deg + 6.07995f, -1.6364f));
  // how strongly red / green / blue are scattered out of the sunlight
  const float beta[3] = {0.035f, 0.075f, 0.16f};
  // the sun fades out as it reaches the horizon
  const float day = std::min(std::max((s + 0.05f) / 0.15f, 0.f), 1.f);

  // sky: full brightness by day, darker near sunset, dim at night, warmer around sunset
  const float sky_level = 0.25f + 0.75f * std::min(std::max(s / 0.4f, 0.f), 1.f);
  const float warm = (1.f - std::min(std::max(s / 0.3f, 0.f), 1.f)) * day * warm_amount;
  const float sunset_tint[3] = {1.25f, 0.85f, 0.7f};

  for (int i = 0; i < 3; i++) {
    sun[i] = base_sun[i] * rl.sun_intensity * std::exp(-beta[i] * airmass) * day;
    const float tint = 1.f + (sunset_tint[i] - 1.f) * warm * 0.6f;
    sky[i] = sky_base[i] * rl.ambient * sky_level * tint;
    // ground bounce: sunlight reflected off the (brownish) ground, plus some sky
    ground[i] = rl.ground_color[i] * rl.ambient * (0.3f + 0.7f * sky_level) +
                rl.ground_color[i] * sun[i] * s_pos * 0.5f;
  }
}
}  // namespace relight_detail

// fog_rgba: the game's fog color (0-255), which follows the game's time of day and matches the
// sky near the horizon. Used to give the sky light the color of the sky you actually see.
inline void relight_light_colors(const SharedRenderState::RelightSettings& rl,
                                 const float L[3],
                                 float sun[3],
                                 float sky[3],
                                 float ground[3],
                                 const unsigned char* fog_rgba = nullptr) {
  const float* base_sun =
      (rl.use_game_colors && rl.game_sun_valid) ? rl.game_sun_color : rl.sun_color;
  if (!rl.auto_sun_color) {
    for (int i = 0; i < 3; i++) {
      sun[i] = base_sun[i] * rl.sun_intensity;
      sky[i] = rl.sky_color[i] * rl.ambient;
      ground[i] = rl.ground_color[i] * rl.ambient;
    }
    return;
  }
  using namespace relight_detail;

  // sky light color: the color picked in the menu, pulled towards the game's own sky color
  float sky_base[3] = {rl.sky_color[0], rl.sky_color[1], rl.sky_color[2]};
  float game_mix = 0.f;
  if (rl.sky_from_game && fog_rgba) {
    float f[3] = {fog_rgba[0] / 255.f, fog_rgba[1] / 255.f, fog_rgba[2] / 255.f};
    const float fl = lum(f);
    if (fl > 0.02f) {
      game_mix = 0.6f;
      const float target = lum(rl.sky_color);
      for (int i = 0; i < 3; i++) {
        const float g = std::min(f[i] / fl, 2.5f) * target;  // game sky hue, menu brightness
        sky_base[i] = sky_base[i] + (g - sky_base[i]) * game_mix;
      }
    }
  }
  // the game's sky color already contains the sunset warmth
  atmosphere_colors(rl, L, sky_base, 1.f - game_mix, sun, sky, ground);

  if (rl.auto_exposure) {
    // like eyes or a camera: when there is less light (evening, night), open up the exposure.
    // Reference: the same settings with the sun 60 degrees high (no change at midday).
    const float Lref[3] = {0.f, 0.866f, 0.5f};
    float rs[3], rk[3], rg[3];
    atmosphere_colors(rl, Lref, rl.sky_color, 1.f, rs, rk, rg);
    const float key_ref = 0.5f * lum(rs) + lum(rk);
    const float key = 0.5f * lum(sun) + lum(sky);
    float k = std::pow(key_ref / std::max(key, 1e-4f), rl.auto_exposure_amount);
    k = std::min(std::max(k, 1.f), 2.f);
    for (int i = 0; i < 3; i++) {
      sun[i] *= k;
      sky[i] *= k;
      ground[i] *= k;
    }
  }
}
