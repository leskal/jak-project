#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec3 tex_coord_in;
layout (location = 3) in uint fog_in;

out vec4 fragment_color;
out vec3 tex_coord;
out float fog;
noperspective out vec4 rel_world_h;  // (AI-assisted)

uniform int bucket;

// (AI-assisted) camera, to turn the (already projected) ocean back into world positions
uniform int waves_on;
uniform int displace;          // 0 = draw the flat surface (fills gaps), 1 = with waves
uniform float wave_time;
uniform float wave_height;     // meters
uniform float wave_range;      // the surface moves only this close to the camera (meters)
uniform mat3 cam_rot;          // camera space = cam_rot * (world - cam_trans)
uniform vec3 cam_trans;
uniform vec4 cam_params;       // x = depth k, y = depth a, z = x scale, w = y scale
uniform float cam_pzw;

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

// ---- (AI-assisted) wakes: rings of ripples spreading from Jak ----
uniform vec4 drops[16];      // xyz = where it started (m, y = Jak's feet), w = start time
uniform float drop_amp[16];
uniform int drop_count;

// x = height (m), yz = slope. wp in meters. fp = size of a pixel (m), 0 = don't fade.
vec3 wake_field(vec3 wp, float t, float fp) {
  vec3 r = vec3(0.0);
  const float k = 6.2831853 / 0.6;  // ripples 60 cm apart
  for (int i = 0; i < drop_count; i++) {
    float age = t - drops[i].w;
    if (age < 0.0 || age > 4.0) {
      continue;
    }
    // only on water at the right height: from a bit below Jak's feet to ~2.3 m above them
    float above = wp.y - drops[i].y;
    float vfade = smoothstep(-1.0, -0.6, above) * (1.0 - smoothstep(2.0, 2.6, above));
    if (vfade <= 0.0) {
      continue;
    }
    vec2 d = wp.xz - drops[i].xz;
    float dist = length(d);
    float front = 1.1 * age;  // the ring grows at 1.1 m/s
    float x = (dist - front) / 0.45;
    float env = exp(-x * x) * exp(-age * 0.6) / (1.0 + dist * 0.5);
    float ph = k * (dist - front);
    float a = drop_amp[i] * env * vfade;
    r.x += a * cos(ph);
    r.yz += -a * k * sin(ph) * d / max(dist, 1e-3);
  }
  if (fp > 0.0) {
    r.yz *= clamp((0.6 / max(fp, 1e-4) - 3.0) / 5.0, 0.0, 1.0);
  }
  return r;
}

void main() {
  vec3 ndc = vec3((position_in.x - 0.5) * 16., -(position_in.y - 0.5) * 32, position_in.z * 2 - 1.);
  // scissoring area adjust
  ndc.y *= SCISSOR_ADJUST * HEIGHT_SCALE;

  // (AI-assisted) real waves: find the world position, raise / lower it, and project it again
  rel_world_h = vec4(0.0);
  if (waves_on == 1) {
    float W = cam_params.x / (cam_params.y - ndc.z);
    vec3 c = vec3(-ndc.x * W / cam_params.z, -ndc.y * W / cam_params.w, -W / cam_pzw);
    vec3 wp = transpose(cam_rot) * c + cam_trans;
    float dist_m = length(wp - cam_trans) / 4096.0;
    // only near the camera, fading out before the end of the detailed near ocean mesh
    float amp = wave_height * (1.0 - smoothstep(wave_range * 0.5, wave_range, dist_m));
    if ((amp > 0.0 || drop_count > 0) && displace == 1) {
      float h = (wave_field(wp.xz / 4096.0, wave_time, amp).x +
                 wake_field(wp / 4096.0, wave_time, 0.0).x) * 4096.0;
      vec3 c2 = c + cam_rot * vec3(0.0, h, 0.0);
      float W2 = -c2.z * cam_pzw;
      if (W2 * W > 0.0 && abs(W2) > 1.0) {
        ndc = vec3(-c2.x * cam_params.z / W2, -c2.y * cam_params.w / W2, cam_params.y - cam_params.x / W2);
        W = W2;
        wp.y += h;
      }
    }
    // world position (relative to the camera) for the pixel shader, divided by depth so it can
    // be interpolated with perspective (the ocean is drawn with w = 1)
    rel_world_h = vec4((wp - cam_trans) / W, 1.0 / W);
  }
  gl_Position = vec4(ndc, 1.0);

  fragment_color = vec4(rgba_in.rgb, rgba_in.a * 2);
  tex_coord = tex_coord_in;
  fog = 255 - fog_in;

  if (bucket == 0) {
    fragment_color.rgb *= 2;
  } else if (bucket == 1 || bucket == 3) {
    fragment_color *= 2;
  } else if (bucket == 4) {
    fragment_color.a = 0;
  }
}
