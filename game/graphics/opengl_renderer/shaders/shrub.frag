#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// (AI-assisted) relighting mod
uniform float relight_amount;  // 0 = original, 1 = fully relit
uniform vec3 sun_dir;          // towards the sun, world space
uniform vec3 sun_color;
uniform vec3 ambient_color;
uniform int relight_debug;     // 1 = show normals
uniform int relight_mode;      // 0 = classic, 1 = physical
uniform vec3 sky_color;        // light from the sky (from above)
uniform vec3 ground_color;     // light bounced from the ground (from below)
uniform float exposure;
uniform float keep_painted;
uniform float ground_smooth;

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
in vec3 world_normal;
in vec3 view_dir;

// (AI-assisted) sun shadows
uniform sampler2DShadow shadow_tex;
uniform mat4 shadow_mat;          // world -> shadow map texture coordinates
uniform int shadow_enabled;
uniform float shadow_strength;
uniform float shadow_normal_offset;  // game units
uniform vec4 cam_trans;
// character shadows, drawn last frame
uniform sampler2DShadow char_shadow_tex;
uniform mat4 char_shadow_mat;
uniform int char_shadow_enabled;

bool in_shadow_map(vec4 sc) {
  return sc.x >= 0.0 && sc.x <= 1.0 && sc.y >= 0.0 && sc.y <= 1.0 && sc.z <= 1.0;
}

float pcf9(sampler2DShadow t, vec3 c) {
  float s = 0.0;
  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      s += textureOffset(t, c, ivec2(x, y));
    }
  }
  return s / 9.0;
}

// 1 = the sun reaches this point, 0 = in shadow
float sun_shadow(vec3 N) {
  if (shadow_enabled == 0) {
    return 1.0;
  }
  vec3 world = view_dir + cam_trans.xyz + N * shadow_normal_offset;
  float s = 1.0;
  vec4 sc = shadow_mat * vec4(world, 1.0);
  if (in_shadow_map(sc)) {
    // fade out at the edge of the shadow map area
    vec2 e = min(sc.xy, 1.0 - sc.xy);
    float edge = clamp(min(e.x, e.y) * 10.0, 0.0, 1.0);
    s = mix(1.0, pcf9(shadow_tex, sc.xyz), edge);
  }
  if (char_shadow_enabled == 1) {
    vec4 cc = char_shadow_mat * vec4(world, 1.0);
    if (in_shadow_map(cc)) {
      s = min(s, pcf9(char_shadow_tex, cc.xyz));
    }
  }
  return mix(1.0, s, shadow_strength);
}

// (AI-assisted) point lights from glowing particles (see PointLights.cpp)
uniform int pl_count;
uniform vec4 pl_pos_radius[16];  // xyz = world position, w = reach (game units)
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
    // smooth end of the light's reach
    float x = d2 / (R * R);
    float window = (1.0 - x * x);
    window *= window;
    // inverse square falloff, in meters, with a 0.5 m soft core
    float d2_m = d2 / (4096.0 * 4096.0);
    sum += pl_color[i].rgb * ndl * window / (d2_m + 0.25);
  }
  return sum;
}

// (AI-assisted) sun on up-facing ground: bend the normal towards "up" so the small slope changes
// between big flat triangles don't show as patches when the sun is low.
float sun_ndl(vec3 N) {
  float k = ground_smooth * smoothstep(0.3, 0.9, N.y);
  vec3 Ns = normalize(mix(N, vec3(0.0, 1.0, 0.0), k));
  return max(dot(Ns, sun_dir), 0.0);
}

void main() {
  vec3 albedo = vec3(0.5);
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    albedo = T0.rgb;
    color = fragment_color * T0;
  } else {
    color = fragment_color;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }


  // (AI-assisted) relighting: light the surface with a real sun, using the computed normal.
  // Fallback for geometry that came without a usable normal (e.g. thin strips along paths):
  // use the flat normal of the triangle, from how the position changes across the screen.
  vec3 face_n = cross(dFdx(view_dir), dFdy(view_dir));
  vec3 n_in = length(world_normal) > 0.1 ? world_normal : face_n;
  float nlen = length(n_in);
  if (nlen > 1e-8 && (relight_amount > 0.0 || relight_debug != 0)) {
    vec3 N = n_in / nlen;
    if (dot(N, view_dir) > 0.0) {
      N = -N;  // winding in the game data isn't reliable: use the side facing the camera
    }
    // plants: lean the normal towards "up" so leaves and grass are lit softly like a canopy
    N = normalize(N + vec3(0.0, 1.0, 0.0));
    if (relight_debug == 1) {
      color.rgb = N * 0.5 + 0.5;
    } else if (relight_debug == 2) {
      color.rgb = vec3(sun_shadow(N));
    } else {
      float sun_vis = sun_shadow(N);
      if (relight_mode == 1) {
        // physical: light the surface's own color with sun + sky, like real light
        vec3 sky = mix(ground_color, sky_color, N.y * 0.5 + 0.5);
        vec3 phys = albedo * (sun_color * sun_ndl(N) * sun_vis + sky) * exposure;
        // keep a little of the game's painted shading (dirt, painted corners)
        float painted = dot(fragment_color.rgb, vec3(0.333));
        phys *= mix(1.0, clamp(painted, 0.2, 1.5), keep_painted);
        phys = filmic(phys);
        color.rgb = mix(color.rgb, phys, relight_amount);
      } else {
        vec3 light = ambient_color + sun_color * sun_ndl(N) * sun_vis;
        color.rgb *= mix(vec3(1.0), light, relight_amount);
      }
    }
  }

  // (AI-assisted) point lights from glowing particles: real light, added on top
  if (pl_count > 0) {
    vec3 Np = nlen > 1e-8 ? n_in / nlen : vec3(0.0, 1.0, 0.0);
    if (dot(Np, view_dir) > 0.0) {
      Np = -Np;
    }
    color.rgb += albedo * point_lights(view_dir + cam_trans.xyz, Np);
  }

  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
