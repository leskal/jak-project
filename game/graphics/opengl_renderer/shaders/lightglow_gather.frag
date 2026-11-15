#version 410 core

// glow light (AI-assisted): for each surface, collect light from glowing things nearby.
// Distances come from the depth buffer, so light lands on walls and floors near the glow.
in vec2 tex_coord;
out vec4 out_color;

uniform sampler2D emit_tex;   // what glows (quarter res)
uniform sampler2D depth_tex;  // depth (full res)
uniform vec2 depth_texel;
uniform vec3 depth_params;    // a, k, to_meters (see SSAO.cpp)
uniform vec2 proj_scale;
uniform float radius;         // meters
uniform vec2 emit_size;       // size of emit_tex (level 0)

const int NUM_SAMPLES = 40;

float linear_depth(float d) {
  float ndc = d * 2.0 - 1.0;
  return abs(depth_params.y / (depth_params.x - ndc)) * depth_params.z;
}

vec3 pos_at(vec2 uv, float z) {
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj_scale.x, ndc.y * z / proj_scale.y, z);
}

vec3 view_pos(vec2 uv) {
  return pos_at(uv, linear_depth(texture(depth_tex, uv).r));
}

vec3 pick(vec3 p, vec3 a, vec3 b) {
  return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

void main() {
  vec2 uv0 = (floor(tex_coord / depth_texel) + 0.5) * depth_texel;
  float d = texture(depth_tex, uv0).r;
  if (d <= 0.0) {
    out_color = vec4(0.0);  // sky
    return;
  }
  vec3 p = view_pos(uv0);
  vec3 n = normalize(cross(pick(p, view_pos(uv0 + vec2(depth_texel.x, 0)), view_pos(uv0 - vec2(depth_texel.x, 0))),
                           pick(p, view_pos(uv0 + vec2(0, depth_texel.y)), view_pos(uv0 - vec2(0, depth_texel.y)))));
  if (dot(n, p) > 0.0) {
    n = -n;
  }

  vec2 r_uv = min(0.5 * radius * proj_scale / p.z, vec2(0.4));
  float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
  // each sample reads a blurred (lower mip) version of the glow that covers the area around it,
  // so small bright things are not missed between samples
  float lod = log2(max(length(r_uv * emit_size) * 1.8 / sqrt(float(NUM_SAMPLES)), 1.0));
  vec3 sum = vec3(0.0);
  for (int i = 0; i < NUM_SAMPLES; i++) {
    float alpha = (float(i) + 0.5) / float(NUM_SAMPLES);
    float angle = alpha * 9.0 * 6.2831853 + rot;
    vec2 uv = uv0 + vec2(cos(angle), sin(angle)) * sqrt(alpha) * r_uv;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      continue;
    }
    vec3 e = textureLod(emit_tex, uv, lod).rgb;
    if (max(max(e.r, e.g), e.b) < 1e-4) {
      continue;
    }
    // Particles don't write depth. Assume the glow is no farther away than this surface
    // (if something closer is behind it on screen, it's at that depth).
    float dq = texture(depth_tex, uv).r;
    float zq = dq <= 0.0 ? p.z : min(linear_depth(dq), p.z);
    vec3 v = pos_at(uv, zq) - p;
    float dist2 = dot(v, v);
    float dist = sqrt(dist2);
    float facing = dist > 1e-3 ? clamp(dot(n, v / dist) * 0.75 + 0.25, 0.0, 1.0) : 1.0;
    float falloff = 1.0 / (1.0 + dist2 / (0.1 * radius * radius)) * clamp(1.0 - dist / radius, 0.0, 1.0);
    sum += e * facing * falloff;
  }
  out_color = vec4(sum * (16.0 / float(NUM_SAMPLES)), 1.0);
}
