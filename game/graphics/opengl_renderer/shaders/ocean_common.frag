#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
uniform float color_mult;
uniform float alpha_mult;

uniform vec4 fog_color;
uniform int bucket;

in float fog;

uniform sampler2D tex_T0;

// (AI-assisted) camera, to turn the (already projected) ocean back into world positions
uniform int waves_on;
uniform float wave_time;
uniform float wave_height;     // meters
uniform mat3 cam_rot;          // camera space = cam_rot * (world - cam_trans)
uniform vec3 cam_trans;
uniform vec4 cam_params;       // x = depth k, y = depth a, z = x scale, w = y scale
uniform float cam_pzw;
uniform vec4 viewport;          // x, y, width, height
uniform float ripple_strength;
uniform float water_reflect;
uniform float water_spec;
uniform vec3 sun_dir;
uniform vec3 sun_color;

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

noperspective in vec4 rel_world_h;

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

vec3 frag_world() {
  // interpolated from the triangle's corners (exact and smooth across triangle edges)
  return rel_world_h.xyz / rel_world_h.w + cam_trans;
}

vec3 ocean_waves(vec3 base) {
  vec3 wp = frag_world();
  float dist_m = length(wp - cam_trans) / 4096.0;
  vec2 p = wp.xz / 4096.0;
  float amp = wave_height * (1.0 - smoothstep(60.0, 220.0, dist_m));
  vec3 V = normalize(cam_trans - wp);
  // size of one pixel on the water, in meters: grows with distance and at grazing angles.
  // (worked out from distance and angle, not from screen derivatives, which jump at the edges
  // of the water's triangles and drew lines)
  float fp = dist_m * (2.0 / viewport.w) / max(abs(V.y), 0.08);
  vec2 slope = wave_slope_aa(p, wave_time, amp, fp);
  slope += ripple_slope(p, wave_time, fp) * ripple_strength;
  vec3 wake = wake_field(wp / 4096.0, wave_time, fp);
  slope += wake.yz;
  vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
  // reflected sky: the game's horizon color
  vec3 col = water_shade(base, N, V, fog_color.rgb * 1.1);
  // (AI-assisted) wake crests: a soft light line (tiny foam / bright sky catching the crest)
  col += fog_color.rgb * clamp(wake.x * 5.0, 0.0, 0.3);
  return col;
}

void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy / tex_coord.z);
  if (bucket == 0) {
    color.rgb = fragment_color.rgb * T0.rgb;
    color.a = fragment_color.a;
    if (waves_on == 1) {
      color.rgb = ocean_waves(color.rgb);
    }
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  } else if (bucket == 1 || bucket == 2 || bucket == 4) {
    color = fragment_color * T0;
  } else if (bucket == 3) {
    color = fragment_color * T0;
    if (waves_on == 1) {
      color.rgb = ocean_waves(color.rgb);
    }
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  }

}
