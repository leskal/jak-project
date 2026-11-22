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

// sun aware (AI-assisted): where does the sun hit directly?
uniform int sun_aware;
uniform sampler2DShadow shadow_tex;
uniform mat4 shadow_mat;
uniform mat3 cam_rot;        // camera space = cam_rot * (world - cam_trans)
uniform vec3 cam_trans;
uniform vec4 cam_params;     // x = depth k, y = depth a, z = x scale, w = y scale (signed)
uniform float cam_pzw;
uniform float shadow_normal_offset;
uniform vec3 sun_dir;
uniform float ao_sun_protect;
uniform float gi_sun_reduce;

// (AI-assisted) fog fade and light protection (after Project Sandover, ISC licence)
uniform int world_ok;        // the camera uniforms are set
uniform int fog_fade;
uniform vec3 fog_w_row;      // clip w = -dot(fog_w_row, world - cam_trans)... see SSAO.cpp
uniform vec3 fog_params;     // x = hvdf_offset.w, y = fog_min, z = fog_max
uniform float fog_mult;      // fog_intensity / 255
uniform int pl_count;
uniform vec4 pl_pos_radius[16];  // xyz = world position (game units)
uniform vec4 pl_color[16];
uniform float light_protect_m;

// how much of the surface's own colour the game's fog leaves (1 = no fog), like tfrag3.vert/.frag
float fog_transmission(vec3 wp) {
  vec3 v = wp - cam_trans;
  float tw = -dot(fog_w_row, v);
  float fogginess = 255.0 - clamp(-tw + fog_params.x, fog_params.y, fog_params.z);
  return 1.0 - clamp(fogginess * fog_mult, 0.0, 1.0);
}

// 1 right at a strong light source, 0 away from all of them
float light_protection(vec3 wp) {
  float p = 0.0;
  for (int i = 0; i < pl_count; i++) {
    float lum = dot(pl_color[i].rgb, vec3(0.299, 0.587, 0.114));
    float b = smoothstep(0.05, 0.6, lum);
    float R = light_protect_m * clamp(sqrt(lum), 0.6, 2.0);
    float d = length(wp - pl_pos_radius[i].xyz) / 4096.0;
    p = max(p, b * (1.0 - smoothstep(0.35 * R, R, d)));
  }
  return p;
}

vec3 world_pos(vec2 uv) {
  float ndc_z = texture(depth_tex, uv).r * 2.0 - 1.0;
  float W = cam_params.x / (cam_params.y - ndc_z);
  vec2 ndc = uv * 2.0 - 1.0;
  vec3 c = vec3(-ndc.x * W / cam_params.z, -ndc.y * W / cam_params.w, -W / cam_pzw);
  return transpose(cam_rot) * c + cam_trans;
}

vec3 pick_w(vec3 c, vec3 a, vec3 b) {
  return length(a - c) < length(b - c) ? a - c : c - b;
}

// 0 = not in direct sun, 1 = fully in direct sun
float direct_sun(vec2 uv) {
  vec3 wp = world_pos(uv);
  vec3 wx = pick_w(wp, world_pos(uv + vec2(depth_texel.x, 0)), world_pos(uv - vec2(depth_texel.x, 0)));
  vec3 wy = pick_w(wp, world_pos(uv + vec2(0, depth_texel.y)), world_pos(uv - vec2(0, depth_texel.y)));
  vec3 wn = cross(wx, wy);
  float l = length(wn);
  if (l < 1e-8) {
    return 0.0;
  }
  wn /= l;
  if (dot(wn, wp - cam_trans) > 0.0) {
    wn = -wn;  // face the camera
  }
  float ndl = max(dot(wn, sun_dir), 0.0);
  if (ndl <= 0.0) {
    return 0.0;
  }
  vec4 sc = shadow_mat * vec4(wp + wn * shadow_normal_offset, 1.0);
  float vis = 1.0;
  if (sc.x >= 0.0 && sc.x <= 1.0 && sc.y >= 0.0 && sc.y <= 1.0 && sc.z <= 1.0) {
    vis = texture(shadow_tex, sc.xyz);
  }
  return ndl * vis;
}

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

// ---------------------------------------------------------------------------------------------
// (AI-assisted) GTAO: the ground-truth AO main pass of Intel's XeGTAO, as ported to GLSL by
// Project Sandover (ISC licence, github.com/bouzou4/project-sandover, shaders/ssao_ao.frag).
// Changes for us: positions from our depth copy (no depth mips: every sample reads full res),
// normals from depth only (Sandover also uses a normal target), sky counts as very far away,
// a fixed noise pattern (no history; our blur removes it). XeGTAO's licence:
/*
  XeGTAO (https://github.com/GameTechDev/XeGTAO), based on "Jimenez et al. / Practical
  Real-Time Strategies for Accurate Indirect Occlusion". Implementation: Filip Strugar, Steve
  Mccalla. MIT License, Copyright (C) 2016-2021, Intel Corporation.
  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
  and associated documentation files (the "Software"), to deal in the Software without
  restriction, including without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions: The above copyright
  notice and this permission notice shall be included in all copies or substantial portions of
  the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
  PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
*/
uniform int ao_method;        // 0 = our spiral AO, 1 = GTAO
uniform int gtao_slices;      // directions per pixel (Sandover: 2 medium, 3 ultra)
uniform int gtao_steps;       // samples per direction and side (2 medium, 3 high/ultra)
uniform float gtao_radius;    // meters (Sandover 1.25)
uniform float gtao_intensity; // darkening = intensity x sqrt(occlusion) (Sandover 0.70)

#define G_PI 3.1415926535897932
#define G_PI_HALF 1.5707963267948966

// camera-space position of a full-res pixel; sky = very far along its ray
vec3 gtao_pos(ivec2 px) {
  ivec2 size = textureSize(depth_tex, 0);
  px = clamp(px, ivec2(0), size - ivec2(1));
  vec2 uv = (vec2(px) + 0.5) / vec2(size);
  if (texelFetch(depth_tex, px, 0).r <= 0.0) {
    vec2 ndc = uv * 2.0 - 1.0;
    float z = 1e5;
    return vec3(ndc.x * z / proj_scale.x, ndc.y * z / proj_scale.y, z);
  }
  return view_pos(uv);
}

float g_fastACos(float inX) {
  float x = min(abs(inX), 1.0);
  float res = (-0.156583 * x + G_PI_HALF) * sqrt(1.0 - x);
  return inX >= 0.0 ? res : G_PI - res;
}

uint g_hilbertIndex(uint posX, uint posY) {
  uint index = 0u;
  for (uint curLevel = 32u; curLevel > 0u; curLevel >>= 1u) {
    uint regionX = (posX & curLevel) > 0u ? 1u : 0u;
    uint regionY = (posY & curLevel) > 0u ? 1u : 0u;
    index += curLevel * curLevel * ((3u * regionX) ^ regionY);
    if (regionY == 0u) {
      if (regionX == 1u) {
        posX = 63u - posX;
        posY = 63u - posY;
      }
      uint temp = posX;
      posX = posY;
      posY = temp;
    }
  }
  return index;
}

vec2 g_noise(ivec2 p) {
  uint index = g_hilbertIndex(uint(p.x) & 63u, uint(p.y) & 63u);
  return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

float g_sliceAngle(vec3 nrm, vec3 axisVec, vec3 orthoDirectionVec, vec3 viewVec, out float len,
                   out float cosN) {
  vec3 projected = nrm - axisVec * dot(nrm, axisVec);
  float signNorm = sign(dot(orthoDirectionVec, projected));
  len = length(projected);
  cosN = clamp(dot(projected, viewVec) / max(len, 1e-6), 0.0, 1.0);
  return signNorm * g_fastACos(cosN);
}

float g_iarc(float h, float n, float cosNorm) {
  return (cosNorm + 2.0 * h * sin(n) - cos(2.0 * h - n)) * 0.25;
}

// visibility, 1 = unoccluded. ssC: the full-res pixel, C0: its position, n_F: its facet normal
// (facing the camera)
float gtao_visibility(ivec2 ssC, vec3 C0, vec3 n_F) {
  ivec2 size = textureSize(depth_tex, 0);
  float viewspaceZ = C0.z * 0.99999;
  vec3 pixCenterPos = C0 * 0.99999;
  vec3 viewVec = normalize(-pixCenterPos);

  const float radius_mul = 1.457;     // XE_GTAO_DEFAULT_RADIUS_MULTIPLIER
  const float falloff_range = 0.615;  // XE_GTAO_DEFAULT_FALLOFF_RANGE
  const float sample_power = 2.0;     // XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER
  const float thin_comp = 0.0;        // Sandover: 0
  float effectRadius = gtao_radius * radius_mul;
  float falloffRange = falloff_range * effectRadius;
  float falloffFrom = effectRadius * (1.0 - falloff_range);
  float falloffMul = -1.0 / falloffRange;
  float falloffAdd = falloffFrom / falloffRange + 1.0;

  vec2 localNoise = g_noise(ivec2(gl_FragCoord.xy));
  float noiseSlice = localNoise.x;
  float noiseSample = localNoise.y;
  // meters per full-res pixel per meter of distance
  float px_m = 2.0 / (float(size.x) * proj_scale.x);
  float screenspaceRadius = effectRadius / (viewspaceZ * px_m);
  float fade = clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;
  float minS = 1.3 / screenspaceRadius;
  float sliceCount = float(gtao_slices);
  float stepsPerSlice = float(gtao_steps);

  float vis = 0.0;
  float visRef = 0.0;
  for (int slice = 0; slice < gtao_slices; slice++) {
    float phi = (float(slice) + noiseSlice) / sliceCount * G_PI;
    float cosPhi = cos(phi);
    float sinPhi = sin(phi);
    vec2 omega = vec2(cosPhi, sinPhi) * screenspaceRadius;
    vec3 directionVec = vec3(cosPhi, sinPhi, 0.0);
    vec3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
    vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

    float projectedNormalVecLength, cosNorm;
    float n = g_sliceAngle(n_F, axisVec, orthoDirectionVec, viewVec, projectedNormalVecLength,
                           cosNorm);
    float lowHorizonCos0 = cos(n + G_PI_HALF);
    float lowHorizonCos1 = cos(n - G_PI_HALF);
    float horizonCos0 = lowHorizonCos0;
    float horizonCos1 = lowHorizonCos1;

    for (int step = 0; step < gtao_steps; step++) {
      float stepBaseNoise = float(slice + step * gtao_steps) * 0.6180339887498948482;
      float stepNoise = fract(noiseSample + stepBaseNoise);
      float s = (float(step) + stepNoise) / stepsPerSlice;
      s = pow(s, sample_power);
      s += minS;
      ivec2 o = ivec2(round(s * omega));

      vec3 sampleDelta0 = gtao_pos(ssC + o) - pixCenterPos;
      vec3 sampleDelta1 = gtao_pos(ssC - o) - pixCenterPos;
      float sampleDist0 = length(sampleDelta0);
      float sampleDist1 = length(sampleDelta1);
      vec3 sampleHorizonVec0 = sampleDelta0 / max(sampleDist0, 1e-6);
      vec3 sampleHorizonVec1 = sampleDelta1 / max(sampleDist1, 1e-6);

      float falloffBase0 = length(vec3(sampleDelta0.xy, sampleDelta0.z * (1.0 + thin_comp)));
      float falloffBase1 = length(vec3(sampleDelta1.xy, sampleDelta1.z * (1.0 + thin_comp)));
      float weight0 = clamp(falloffBase0 * falloffMul + falloffAdd, 0.0, 1.0);
      float weight1 = clamp(falloffBase1 * falloffMul + falloffAdd, 0.0, 1.0);

      float shc0 = mix(lowHorizonCos0, dot(sampleHorizonVec0, viewVec), weight0);
      float shc1 = mix(lowHorizonCos1, dot(sampleHorizonVec1, viewVec), weight1);
      horizonCos0 = max(horizonCos0, shc0);
      horizonCos1 = max(horizonCos1, shc1);
    }

    projectedNormalVecLength = mix(projectedNormalVecLength, 1.0, 0.05);
    float h0 = -g_fastACos(horizonCos1);
    float h1 = g_fastACos(horizonCos0);
    float r0 = -g_fastACos(lowHorizonCos1);
    float r1 = g_fastACos(lowHorizonCos0);
    vis += projectedNormalVecLength * (g_iarc(h0, n, cosNorm) + g_iarc(h1, n, cosNorm));
    visRef += projectedNormalVecLength * (g_iarc(r0, n, cosNorm) + g_iarc(r1, n, cosNorm));
  }
  float v = visRef > 1e-4 ? clamp(vis / visRef, 0.0, 1.0) : 1.0;
  return clamp(v + fade / sliceCount, 0.0, 1.0);
}
// ---------------------------------------------------------------------------------------------

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
  // (AI-assisted) GTAO instead of the spiral: no spiral samples (the loop runs 0 times)
  int spiral_samples = ao_method == 1 ? 0 : NUM_SAMPLES;
  for (int i = 0; i < spiral_samples; i++) {
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
  if (ao_method == 1) {
    // (AI-assisted) GTAO, with Sandover's response: darkening = intensity x sqrt(occlusion)
    ivec2 ssC = ivec2(uv0 / depth_texel);
    float v = gtao_visibility(ssC, p, n);
    ao = 1.0 - gtao_intensity * sqrt(clamp(1.0 - v, 0.0, 1.0));
  }

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

  // (AI-assisted) fog: AO and bounce light fade with the fog, as the surface's own colour does;
  // light sources: no AO right where a fire, lamp, lava or eco light sits
  if (world_ok == 1) {
    vec3 wp = world_pos(uv0);
    if (fog_fade == 1) {
      float t = fog_transmission(wp);
      out_color.r = mix(1.0, out_color.r, t);
      gi *= t;
    }
    if (pl_count > 0) {
      out_color.r = mix(out_color.r, 1.0, light_protection(wp));
    }
  }

  // sun aware: AO only darkens light from the sky, and bounce light matters less in direct sun
  if (sun_aware == 1) {
    float sunlit = direct_sun(uv0);
    out_color.r = mix(out_color.r, 1.0, sunlit * ao_sun_protect);
    gi *= mix(1.0, 1.0 - gi_sun_reduce, sunlit);
  }
  out_gi = vec4(gi, 1.0);
}
