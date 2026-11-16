#include "LightingSettings.h"

// (AI-assisted)

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

namespace {
template <typename T>
void get_if(const json& j, const char* key, T& out) {
  if (j.contains(key)) {
    try {
      out = j.at(key).get<T>();
    } catch (const std::exception& e) {
      lg::warn("lighting settings: bad value for {}: {}", key, e.what());
    }
  }
}
}  // namespace

std::string lighting_settings_path() {
  return (file_util::get_user_config_dir() / "lighting_mod.json").string();
}

bool lighting_settings_load(SSAO& ssao,
                            SunShadow& sun,
                            GlowLight& glow,
                            PointLights& lights,
                            SharedRenderState::RelightSettings& rl) {
  const auto path = lighting_settings_path();
  if (!file_util::file_exists(path)) {
    lg::info("lighting settings: no file at {}, using defaults", path);
    return false;
  }
  auto maybe = safe_parse_json(file_util::read_text_file(path));
  if (!maybe) {
    lg::warn("lighting settings: could not read {}", path);
    return false;
  }
  const json& j = *maybe;

  if (j.contains("ssao")) {
    const auto& s = j.at("ssao");
    get_if(s, "enabled", ssao.enabled);
    get_if(s, "half_resolution", ssao.half_resolution);
    get_if(s, "blur", ssao.blur);
    get_if(s, "strength", ssao.strength);
    get_if(s, "radius_m", ssao.radius_m);
    get_if(s, "intensity", ssao.intensity);
    get_if(s, "bias_m", ssao.bias_m);
    get_if(s, "bias_per_m", ssao.bias_per_m);
    get_if(s, "max_distance_m", ssao.max_distance_m);
    get_if(s, "jak1_after_bucket", ssao.jak1_after_bucket);
    get_if(s, "gi_enabled", ssao.gi_enabled);
    get_if(s, "gi_strength", ssao.gi_strength);
    get_if(s, "gi_radius_m", ssao.gi_radius_m);
    get_if(s, "sun_aware", ssao.sun_aware);
    get_if(s, "ao_sun_protect", ssao.ao_sun_protect);
    get_if(s, "gi_sun_reduce", ssao.gi_sun_reduce);
  }
  if (j.contains("relight")) {
    const auto& r = j.at("relight");
    get_if(r, "amount", rl.amount);
    get_if(r, "sun_azimuth_deg", rl.sun_azimuth_deg);
    get_if(r, "sun_elevation_deg", rl.sun_elevation_deg);
    get_if(r, "sun_intensity", rl.sun_intensity);
    get_if(r, "ambient", rl.ambient);
    get_if(r, "use_game_sun", rl.use_game_sun);
    get_if(r, "use_game_colors", rl.use_game_colors);
    get_if(r, "sun_smooth_s", rl.sun_smooth_s);
    get_if(r, "mode", rl.mode);
    get_if(r, "auto_sun_color", rl.auto_sun_color);
    get_if(r, "exposure", rl.exposure);
    get_if(r, "keep_painted", rl.keep_painted);
    get_if(r, "ground_smooth", rl.ground_smooth);
    for (const char* key : {"sky_color", "ground_color"}) {
      if (r.contains(key) && r.at(key).is_array() && r.at(key).size() == 3) {
        float* dst = std::string(key) == "sky_color" ? rl.sky_color : rl.ground_color;
        for (int i = 0; i < 3; i++) {
          dst[i] = r.at(key)[i].get<float>();
        }
      }
    }
    get_if(r, "sun_jump_accept_s", rl.sun_jump_accept_s);
    if (r.contains("sun_color") && r.at("sun_color").is_array() &&
        r.at("sun_color").size() == 3) {
      for (int i = 0; i < 3; i++) {
        rl.sun_color[i] = r.at("sun_color")[i].get<float>();
      }
    }
  }
  if (j.contains("shadows")) {
    const auto& s = j.at("shadows");
    get_if(s, "enabled", sun.enabled);
    get_if(s, "resolution", sun.resolution);
    get_if(s, "half_size_m", sun.half_size_m);
    get_if(s, "strength", sun.strength);
    get_if(s, "normal_offset_m", sun.normal_offset_m);
    get_if(s, "tfrag_casts", sun.tfrag_casts);
    get_if(s, "tie_casts", sun.tie_casts);
    get_if(s, "chars_cast", sun.chars_cast);
    get_if(s, "chars_receive", sun.chars_receive);
    get_if(s, "hide_blob_shadow", sun.hide_blob_shadow);
  }
  if (j.contains("glow")) {
    const auto& g = j.at("glow");
    get_if(g, "enabled", glow.enabled);
    get_if(g, "strength", glow.strength);
    get_if(g, "radius_m", glow.radius_m);
    get_if(g, "particle_gain", glow.particle_gain);
    get_if(g, "bright_surface_gain", glow.bright_surface_gain);
    get_if(g, "bright_threshold", glow.bright_threshold);
  }
  if (j.contains("point_lights")) {
    const auto& p = j.at("point_lights");
    get_if(p, "enabled", lights.enabled);
    get_if(p, "strength", lights.strength);
    get_if(p, "cluster_size_m", lights.cluster_size_m);
    get_if(p, "max_radius_m", lights.max_radius_m);
    get_if(p, "max_lights", lights.max_lights);
  }
  lg::info("lighting settings: loaded {}", path);
  return true;
}

bool lighting_settings_save(const SSAO& ssao,
                            const SunShadow& sun,
                            const GlowLight& glow,
                            const PointLights& lights,
                            const SharedRenderState::RelightSettings& rl) {
  json j;
  j["ssao"] = {{"enabled", ssao.enabled},
               {"half_resolution", ssao.half_resolution},
               {"blur", ssao.blur},
               {"strength", ssao.strength},
               {"radius_m", ssao.radius_m},
               {"intensity", ssao.intensity},
               {"bias_m", ssao.bias_m},
               {"bias_per_m", ssao.bias_per_m},
               {"max_distance_m", ssao.max_distance_m},
               {"jak1_after_bucket", ssao.jak1_after_bucket},
               {"gi_enabled", ssao.gi_enabled},
               {"gi_strength", ssao.gi_strength},
               {"gi_radius_m", ssao.gi_radius_m},
               {"sun_aware", ssao.sun_aware},
               {"ao_sun_protect", ssao.ao_sun_protect},
               {"gi_sun_reduce", ssao.gi_sun_reduce}};
  j["relight"] = {{"amount", rl.amount},
                  {"sun_azimuth_deg", rl.sun_azimuth_deg},
                  {"sun_elevation_deg", rl.sun_elevation_deg},
                  {"sun_intensity", rl.sun_intensity},
                  {"sun_color", {rl.sun_color[0], rl.sun_color[1], rl.sun_color[2]}},
                  {"ambient", rl.ambient},
                  {"use_game_sun", rl.use_game_sun},
                  {"use_game_colors", rl.use_game_colors},
                  {"sun_smooth_s", rl.sun_smooth_s},
                  {"sun_jump_accept_s", rl.sun_jump_accept_s},
                  {"mode", rl.mode},
                  {"auto_sun_color", rl.auto_sun_color},
                  {"exposure", rl.exposure},
                  {"keep_painted", rl.keep_painted},
                  {"ground_smooth", rl.ground_smooth},
                  {"sky_color", {rl.sky_color[0], rl.sky_color[1], rl.sky_color[2]}},
                  {"ground_color", {rl.ground_color[0], rl.ground_color[1], rl.ground_color[2]}}};
  j["shadows"] = {{"enabled", sun.enabled},
                  {"resolution", sun.resolution},
                  {"half_size_m", sun.half_size_m},
                  {"strength", sun.strength},
                  {"normal_offset_m", sun.normal_offset_m},
                  {"tfrag_casts", sun.tfrag_casts},
                  {"tie_casts", sun.tie_casts},
                  {"chars_cast", sun.chars_cast},
                  {"chars_receive", sun.chars_receive},
                  {"hide_blob_shadow", sun.hide_blob_shadow}};
  j["glow"] = {{"enabled", glow.enabled},
               {"strength", glow.strength},
               {"radius_m", glow.radius_m},
               {"particle_gain", glow.particle_gain},
               {"bright_surface_gain", glow.bright_surface_gain},
               {"bright_threshold", glow.bright_threshold}};
  j["point_lights"] = {{"enabled", lights.enabled},
                       {"strength", lights.strength},
                       {"cluster_size_m", lights.cluster_size_m},
                       {"max_radius_m", lights.max_radius_m},
                       {"max_lights", lights.max_lights}};
  const auto path = lighting_settings_path();
  try {
    file_util::create_dir_if_needed_for_file(path);
    file_util::write_text_file(path, j.dump(2));
  } catch (const std::exception& e) {
    lg::error("lighting settings: could not save {}: {}", path, e.what());
    return false;
  }
  lg::info("lighting settings: saved {}", path);
  return true;
}
