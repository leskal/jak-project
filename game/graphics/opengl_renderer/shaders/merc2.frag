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
  // (AI-assisted) darks and midtones stay exactly as lit (so shadowed sides stay dark);
  // only light above 0.6 is rolled off smoothly towards white instead of clipping.
  const float a = 0.6;
  if (x <= a) {
    return x;
  }
  return a + (1.0 - a) * (1.0 - exp(-(x - a) / (1.0 - a)));
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

// (AI-assisted) water surfaces (rivers, ponds): ripples, sky reflection, sun glints
uniform int is_water;
uniform int waves_on;
uniform float wave_time;
uniform float wave_height;
uniform float ripple_strength;
uniform float water_reflect;
uniform float water_spec;
uniform vec3 cam_trans_m;
uniform vec4 viewport;  // x, y, width, height
#define water_viewport_h viewport.w

// ---- (AI-assisted) water waves ----
// a few long waves (they move the ocean surface) and short ripples (only change the lighting).
// Positions in meters. Deep water: speed follows w = sqrt(g k).
const int NUM_WAVES = 8;
// roughly following the wind, spread over +-70 degrees; smaller waves are lower
const vec2 WAVE_DIR[8] = vec2[8](vec2(0.8000, 0.6000), vec2(0.4715, 0.8819), vec2(0.9928, 0.1196), vec2(0.0546, 0.9985),
                                 vec2(0.9504, -0.3112), vec2(0.6578, 0.7532), vec2(0.9280, 0.3725), vec2(-0.2902, 0.9570));
const float WAVE_LEN[8] = float[8](28.0, 19.0, 14.0, 10.0, 7.5, 5.5, 4.0, 3.0);
const float WAVE_AMP[8] = float[8](0.40, 0.27, 0.20, 0.14, 0.11, 0.08, 0.06, 0.04);
const float WAVE_PHASE[8] = float[8](0.0, 2.1, 4.4, 1.3, 5.6, 3.2, 0.7, 4.9);

// x = height (m), yz = slope (dh/dx, dh/dz)
vec3 wave_field(vec2 p, float t, float amp) {
  vec3 r = vec3(0.0);
  for (int i = 0; i < NUM_WAVES; i++) {
    float k = 6.2831853 / WAVE_LEN[i];
    float w = sqrt(9.81 * k);
    float ph = k * dot(WAVE_DIR[i], p) - w * t + WAVE_PHASE[i];
    r.x += amp * WAVE_AMP[i] * sin(ph);
    r.yz += amp * WAVE_AMP[i] * k * cos(ph) * WAVE_DIR[i];
  }
  return r;
}


// (AI-assisted) anti-aliasing for waves: a wave that is smaller than a few pixels on screen
// fades out, instead of turning into flickering stripes. fp = meters per pixel here.
float wave_aa(float wavelength, float fp) {
  return clamp((wavelength / max(fp, 1e-4) - 3.0) / 5.0, 0.0, 1.0);
}

vec2 wave_slope_aa(vec2 p, float t, float amp, float fp) {
  vec2 r = vec2(0.0);
  for (int i = 0; i < NUM_WAVES; i++) {
    float k = 6.2831853 / WAVE_LEN[i];
    float w = sqrt(9.81 * k);
    float ph = k * dot(WAVE_DIR[i], p) - w * t + WAVE_PHASE[i];
    r += amp * WAVE_AMP[i] * k * cos(ph) * WAVE_DIR[i] * wave_aa(WAVE_LEN[i], fp);
  }
  return r;
}

// random-looking ripples: gradient noise (with its slope), in 4 layers of smaller and smaller
// ripples. Each layer is two copies drifting in different directions, so the pattern changes
// instead of just sliding.
vec2 rip_hash(vec2 p) {
  uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
  uint n = (q.x ^ q.y) * 1597334673u;
  uvec2 r = uvec2(n, n * 48271u);
  return vec2(r) * (2.0 / 4294967295.0) - 1.0;
}

// x = noise value, yz = its slope
vec3 rip_noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = p - i;
  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  vec2 ga = rip_hash(i);
  vec2 gb = rip_hash(i + vec2(1.0, 0.0));
  vec2 gc = rip_hash(i + vec2(0.0, 1.0));
  vec2 gd = rip_hash(i + vec2(1.0, 1.0));
  float va = dot(ga, f);
  float vb = dot(gb, f - vec2(1.0, 0.0));
  float vc = dot(gc, f - vec2(0.0, 1.0));
  float vd = dot(gd, f - vec2(1.0, 1.0));
  float v = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
  vec2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
           du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va);
  return vec3(v, d);
}

vec2 ripple_slope(vec2 p, float t, float fp) {
  vec2 s = vec2(0.0);
  float len = 3.0;  // ripple size (m) of the first layer
  float c = 0.8;
  float sn = 0.6;
  for (int o = 0; o < 4; o++) {
    float f = 1.0 / len;
    float speed = 0.35 * sqrt(len);  // smaller ripples move slower
    mat2 rot = mat2(c, sn, -sn, c);
    vec2 q = rot * p * f;
    vec2 g1 = rip_noise(q + vec2(t * speed * f, 0.0)).yz;
    vec2 g2 = rip_noise(q * 1.07 + vec2(-0.6, 0.8) * t * speed * f + 17.3).yz;
    vec2 g = (g1 + g2) * 0.5;
    // slope in world directions (undo the rotation, scale by the size)
    s += (transpose(rot) * g) * f * len * 0.09 * wave_aa(len, fp);
    len *= 0.5;
    float c2 = c * 0.6 - sn * 0.8;  // turn the next layer
    sn = c * 0.8 + sn * 0.6;
    c = c2;
  }
  return s;
}

// light the water surface: sky reflection (stronger at grazing angles) and sun glints
vec3 water_shade(vec3 base, vec3 N, vec3 V, vec3 sky_refl) {
  float ndv = max(dot(N, V), 0.0);
  float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
  vec3 L = normalize(sun_dir);
  // the water's own color: a bit brighter where the waves face the sun
  vec3 col = base * (0.88 + 0.24 * clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0));
  col = mix(col, sky_refl, clamp(F * water_reflect, 0.0, 1.0));
  vec3 H = normalize(L + V);
  float spec = pow(max(dot(N, H), 0.0), 350.0) * 25.0;
  if (L.y > 0.0) {
    col += sun_color * spec * water_spec * 0.25;
  }
  return col;
}
// ---- end of water waves ----

// point lights from glowing particles (see PointLights.cpp)
in vec3 merc_normal;
in vec4 vtx_rgba;
uniform int pl_count;
uniform vec4 pl_pos_radius[16];
uniform vec4 pl_color[16];

// (AI-assisted) shadows of the strongest point lights (cube shadow maps, see SunShadow.cpp)
uniform samplerCubeArrayShadow pl_shadow_tex;
uniform int pl_shadow_layer[16];  // -1 = this light has no shadow map
uniform int pl_shadows_on;
uniform float pl_shadow_res;

float pl_shadow(int i, vec3 wp, vec3 N, float d) {
  if (pl_shadows_on == 0) {
    return 1.0;
  }
  int layer = pl_shadow_layer[i];
  if (layer < 0) {
    return 1.0;
  }
  float R = pl_pos_radius[i].w;
  // push off the surface a little (more further from the light, where texels are bigger)
  float texel = 2.0 * d / pl_shadow_res;
  vec3 dir = wp + N * (texel * 1.5 + 20.0) - pl_pos_radius[i].xyz;
  float len = length(dir);
  float ref = len / R - 0.003;
  // 4 samples around the direction, for soft edges
  vec3 a = abs(dir.y) < 0.9 * len ? cross(dir, vec3(0.0, 1.0, 0.0)) : cross(dir, vec3(1.0, 0.0, 0.0));
  a = normalize(a);
  vec3 b = normalize(cross(dir, a));
  float o = len * 1.5 / pl_shadow_res;
  float s = texture(pl_shadow_tex, vec4(dir + (a + b) * o, float(layer)), ref);
  s += texture(pl_shadow_tex, vec4(dir + (a - b) * o, float(layer)), ref);
  s += texture(pl_shadow_tex, vec4(dir - (a - b) * o, float(layer)), ref);
  s += texture(pl_shadow_tex, vec4(dir - (a + b) * o, float(layer)), ref);
  return s * 0.25;
}

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
    float ndl = max(dot(N, L), 0.0) * pl_shadow(i, wp, N, d);
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

  if (is_water == 1 && waves_on == 1 && world_valid == 1) {
    vec2 p = merc_world.xz / 4096.0;
    float dist_m = length(merc_world - cam_trans_m) / 4096.0;
    // rivers and ponds: small waves and ripples (no big ocean swell)
    vec3 V = normalize(cam_trans_m - merc_world);
    // size of one pixel on the water (m), from distance and angle (smooth across triangles)
    float fp = dist_m * (2.0 / max(water_viewport_h, 1.0)) / max(abs(V.y), 0.08);
    vec2 slope = wave_slope_aa(p * 2.5, wave_time, wave_height * 0.15, fp * 2.5);
    slope += ripple_slope(p, wave_time, fp) * ripple_strength;
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    color.rgb = water_shade(color.rgb, N, V, fog_color.rgb * 1.1);
  }

  if (relight_debug == 2 && shadow_enabled == 1) {
    color.rgb = vec3(sun_vis);
  }
  if (relight_debug == 1) {
    // (AI-assisted) same colors as the level's normal view: up = green, so the top of Jak's
    // head should match flat ground
    float nl = length(merc_normal);
    vec3 N = nl > 1e-6 ? merc_normal / nl : vec3(0.0, 1.0, 0.0);
    color.rgb = N * 0.5 + 0.5;
  }
  if (relight_debug == 1 || relight_debug == 2) {
    return;  // no fog in the debug views
  }

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
