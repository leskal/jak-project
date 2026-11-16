#version 410 core

out vec4 color;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

// (AI-assisted) relighting mod: sun shadows on characters
in vec3 merc_world;
in vec3 vtx_sun;
uniform sampler2DShadow shadow_tex;
uniform mat4 shadow_mat;
uniform int shadow_enabled;
uniform int world_valid;  // world position / normal available (same lighting with shadows on or off)
uniform float shadow_strength;
uniform int relight_debug;
// physical sun mode (same as the level shaders)
uniform int relight_mode;
uniform float relight_amount;
uniform vec3 sun_dir;
uniform vec3 sun_color;
uniform vec3 sky_color;
uniform vec3 ground_color;
uniform float exposure;

// filmic tone curve (ACES fit): bright light rolls off smoothly instead of clipping
float filmic_curve(float x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 filmic(vec3 x) {
  // (AI-assisted) tone curve on brightness only, so colors keep their hue (blue stays blue)
  // instead of each channel clipping towards white/cream. Only very bright light desaturates.
  x = max(x, vec3(0.0));
  float l = dot(x, vec3(0.2126, 0.7152, 0.0722));
  if (l < 1e-5) {
    return vec3(0.0);
  }
  vec3 c = x * (filmic_curve(l) / l);
  float m = max(c.r, max(c.g, c.b));
  if (m > 1.0) {
    c /= m;
  }
  vec3 per_channel = vec3(filmic_curve(x.r), filmic_curve(x.g), filmic_curve(x.b));
  return mix(c, per_channel, 0.15);
}

// point lights from glowing particles (see PointLights.cpp)
in vec3 merc_normal;
in vec4 vtx_rgba;
uniform int pl_count;
uniform vec4 pl_pos_radius[16];
uniform vec4 pl_color[16];

vec3 point_lights(vec3 wp, vec3 N) {
  vec3 sum = vec3(0.0);
  for (int i = 0; i < pl_count; i++) {
    vec3 L = pl_pos_radius[i].xyz - wp;
    float d2 = dot(L, L);
    float R = pl_pos_radius[i].w;
    if (d2 >= R * R) {
      continue;
    }
    float d = sqrt(d2);
    L /= max(d, 1.0);
    float ndl = max(dot(N, L), 0.0);
    float x = d2 / (R * R);
    float window = (1.0 - x * x);
    window *= window;
    float d2_m = d2 / (4096.0 * 4096.0);
    sum += pl_color[i].rgb * ndl * window / (d2_m + 0.25);
  }
  return sum;
}

float sun_shadow_merc() {
  if (shadow_enabled == 0) {
    return 1.0;
  }
  vec4 sc = shadow_mat * vec4(merc_world, 1.0);
  if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) {
    return 1.0;
  }
  float s = 0.0;
  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      s += textureOffset(shadow_tex, sc.xyz, ivec2(x, y));
    }
  }
  s /= 9.0;
  vec2 e = min(sc.xy, 1.0 - sc.xy);
  float edge = clamp(min(e.x, e.y) * 10.0, 0.0, 1.0);
  return mix(1.0, s, shadow_strength * edge);
}

void main() {
  // in shadow, remove the sun's part of the lighting (sky / ambient light stays)
  float sun_vis = sun_shadow_merc();
  vec4 vc = vtx_color;
  vc.rgb = max(vc.rgb - vtx_sun * (1.0 - sun_vis), vec3(0.0));

  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vc * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = vc.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  // physical sun mode: light the character's own color with our sun + sky, so characters match
  // the level (instead of the game's own lights)
  if (relight_mode == 1 && relight_amount > 0.0 && decal_enable == 0 && world_valid == 1) {
    vec3 albedo = (gfx_hack_no_tex == 0 ? texture(tex_T0, vtx_st).rgb : vec3(0.5)) *
                  (vtx_rgba.rgb * 2.0);  // texture x model color (128 = 1.0)
    float nl = length(merc_normal);
    vec3 N = nl > 1e-6 ? merc_normal / nl : vec3(0.0, 1.0, 0.0);
    vec3 sky = mix(ground_color, sky_color, N.y * 0.5 + 0.5);
    vec3 phys = filmic(albedo * (sun_color * max(dot(N, sun_dir), 0.0) * sun_vis + sky) * exposure);
    color.rgb = mix(color.rgb, phys, relight_amount);
  }

  // point lights: real light, added on top
  if (pl_count > 0 && decal_enable == 0 && world_valid == 1) {
    vec3 albedo = (gfx_hack_no_tex == 0 ? texture(tex_T0, vtx_st).rgb : vec3(0.5)) *
                  (vtx_rgba.rgb * 2.0);  // texture x model color (128 = 1.0)
    float nl = length(merc_normal);
    vec3 N = nl > 1e-6 ? merc_normal / nl : vec3(0.0, 1.0, 0.0);
    color.rgb += albedo * point_lights(merc_world, N);
  }

  if (relight_debug == 2 && shadow_enabled == 1) {
    color.rgb = vec3(sun_vis);
  }

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
