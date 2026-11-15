#version 410 core

// depth-aware blur for the SSAO and bounce light results, run once horizontally and once
// vertically (AI-assisted)

in vec2 tex_coord;
layout (location = 0) out vec4 out_color;  // r = ao, g = linear depth
layout (location = 1) out vec4 out_gi;     // rgb = bounce light

uniform sampler2D ao_tex;  // r = ao, g = linear depth (meters)
uniform sampler2D gi_tex;  // rgb = bounce light
uniform vec2 blur_step;    // one texel along the blur direction

void main() {
  vec2 center = texture(ao_tex, tex_coord).rg;
  float z = center.g;
  float ao_total = 0.0;
  vec3 gi_total = vec3(0.0);
  float weight_sum = 0.0;
  for (int i = -4; i <= 4; i++) {
    vec2 uv = tex_coord + blur_step * float(i);
    vec2 s = texture(ao_tex, uv).rg;
    float w = exp(-float(i * i) / 8.0);
    // don't blur across depth edges (e.g. character in front of a wall)
    w *= max(0.0, 1.0 - abs(s.g - z) / (0.05 * z + 0.05));
    ao_total += s.r * w;
    gi_total += texture(gi_tex, uv).rgb * w;
    weight_sum += w;
  }
  float inv = 1.0 / max(weight_sum, 1e-4);
  out_color = vec4(ao_total * inv, z, 0.0, 1.0);
  out_gi = vec4(gi_total * inv, 1.0);
}
