#version 410 core

// Screen-space ambient occlusion, depth only. (AI-assisted)
// Based on the "Scalable Ambient Obscurance" idea: sample points on a spiral around the pixel,
// rebuild their 3D positions from depth, and count how much of the hemisphere above the surface
// is blocked.

in vec2 tex_coord;
layout (location = 0) out vec4 out_color;  // r = ao (1 = not occluded), g = linear depth (m)
layout (location = 1) out vec4 out_gi;     // rgb = bounce light arriving at this pixel

uniform sampler2D depth_tex;
uniform vec2 depth_texel;    // 1 / full resolution size
uniform vec3 depth_params;   // x = a, y = k, z = to_meters  (see SSAO.cpp)
uniform vec2 proj_scale;     // screen-space scale of the projection
uniform float radius;        // meters
uniform float intensity;
uniform float bias;          // meters
uniform float bias_per_m;    // extra bias per meter of distance (depth precision gets worse far away)
uniform float max_distance;  // meters
uniform sampler2D color_tex; // the image before AO, source of bounce light colors
uniform int gi_enabled;
uniform float gi_radius;     // meters

const int NUM_SAMPLES = 16;
const float NUM_TURNS = 7.0;
const int GI_SAMPLES = 16;

float linear_depth(float d) {
  float ndc = d * 2.0 - 1.0;
  return abs(depth_params.y / (depth_params.x - ndc)) * depth_params.z;
}

vec3 view_pos(vec2 uv) {
  float z = linear_depth(texture(depth_tex, uv).r);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj_scale.x, ndc.y * z / proj_scale.y, z);
}

// pick the neighbor with the smaller depth jump, so edges don't get halos
vec3 pick(vec3 p, vec3 a, vec3 b) {
  return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

void main() {
  // At half resolution our pixel centers fall exactly between two full-res depth texels, so
  // nearest sampling would randomly pick one or the other and produce dotted lines.
  // Snap to the center of one full-res texel instead.
  vec2 uv0 = (floor(tex_coord / depth_texel) + 0.5) * depth_texel;
  float d = texture(depth_tex, uv0).r;
  // the depth buffer is cleared to 0, so 0 means "nothing drawn here" (sky)
  if (d <= 0.0) {
    out_color = vec4(1.0, 1e6, 0.0, 1.0);
    out_gi = vec4(0.0);
    return;
  }

  vec3 p = view_pos(uv0);
  if (p.z > max_distance) {
    out_color = vec4(1.0, p.z, 0.0, 1.0);
    out_gi = vec4(0.0);
    return;
  }

  // normal from neighboring depths
  vec3 px1 = view_pos(uv0 + vec2(depth_texel.x, 0));
  vec3 px0 = view_pos(uv0 - vec2(depth_texel.x, 0));
  vec3 py1 = view_pos(uv0 + vec2(0, depth_texel.y));
  vec3 py0 = view_pos(uv0 - vec2(0, depth_texel.y));
  vec3 n = normalize(cross(pick(p, px1, px0), pick(p, py1, py0)));
  // make it face the camera (camera is at the origin, looking down +z)
  if (dot(n, p) > 0.0) {
    n = -n;
  }

  // per-pixel random rotation so the pattern turns into noise that the blur removes
  float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;

  // sample radius on screen (in uv units)
  vec2 radius_uv = 0.5 * radius * proj_scale / p.z;

  float r2 = radius * radius;
  float total_bias = bias + bias_per_m * p.z;
  float sum = 0.0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    float alpha = (float(i) + 0.5) / float(NUM_SAMPLES);
    float angle = alpha * NUM_TURNS * 6.2831853 + rot;
    vec2 offset = vec2(cos(angle), sin(angle)) * alpha * radius_uv;
    vec2 uv = uv0 + offset;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      continue;
    }
    float sd = texture(depth_tex, uv).r;
    if (sd <= 0.0) {
      continue;  // sky never occludes
    }
    vec3 q = view_pos(uv);
    vec3 v = q - p;
    float vv = dot(v, v);
    float vn = dot(v, n);
    float f = max(r2 - vv, 0.0);
    sum += f * f * f * max((vn - total_bias) / (vv + 0.01 * r2), 0.0);
  }

  float ao = max(0.0, 1.0 - sum * intensity * 5.0 / (r2 * r2 * r2 * float(NUM_SAMPLES)));

  // fade out with distance
  float fade = clamp((max_distance - p.z) / (0.25 * max_distance), 0.0, 1.0);
  ao = mix(1.0, ao, fade);

  out_color = vec4(ao, p.z, 0.0, 1.0);

  // ---- bounce light ----
  // Look at nearby visible surfaces. Each one that lies in front of this surface (in the
  // direction its normal points) sends some of its color here, less with distance.
  vec3 gi = vec3(0.0);
  if (gi_enabled == 1) {
    vec2 gi_radius_uv = 0.5 * gi_radius * proj_scale / p.z;
    float g2 = gi_radius * gi_radius;
    for (int i = 0; i < GI_SAMPLES; i++) {
      float alpha = (float(i) + 0.5) / float(GI_SAMPLES);
      float angle = alpha * 5.0 * 6.2831853 + rot * 1.618;
      // sqrt spreads the samples evenly over the disk
      vec2 uv = uv0 + vec2(cos(angle), sin(angle)) * sqrt(alpha) * gi_radius_uv;
      if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        continue;
      }
      if (texture(depth_tex, uv).r <= 0.0) {
        continue;  // sky: no surface to bounce from
      }
      vec3 v = view_pos(uv) - p;
      float vv = dot(v, v);
      if (vv < 1e-4 || vv > g2) {
        continue;
      }
      vec3 dir = v * inversesqrt(vv);
      // the -0.1 stops flat ground from "lighting itself"
      float facing = max(dot(n, dir) - 0.1, 0.0);
      float falloff = 1.0 - vv / g2;
      gi += texture(color_tex, uv).rgb * facing * falloff;
    }
    gi *= 2.0 / float(GI_SAMPLES);
    gi *= fade;
  }
  out_gi = vec4(gi, 1.0);
}
