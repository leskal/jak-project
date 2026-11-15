#version 410 core

// applies the SSAO and bounce light results to the image (AI-assisted)
// Uses "dual source" blending: final = out_add + image * out_mul

in vec2 tex_coord;
layout (location = 0, index = 0) out vec4 out_add;  // light to add (also used by debug views)
layout (location = 0, index = 1) out vec4 out_mul;  // ao multiplier

uniform sampler2D ao_tex;     // r = ao, g = linear depth (meters)
uniform sampler2D gi_tex;     // rgb = bounce light arriving here
uniform sampler2D color_tex;  // the image before AO
uniform float strength;
uniform float max_distance;
uniform int debug_view;       // 0 = normal, 1 = AO only, 2 = depth, 3 = GI only
uniform int gi_enabled;
uniform float gi_strength;

void main() {
  vec2 s = texture(ao_tex, tex_coord).rg;
  vec3 gi = gi_enabled == 1 ? texture(gi_tex, tex_coord).rgb * gi_strength : vec3(0.0);
  float ao = mix(1.0, s.r, strength);
  out_mul = vec4(ao, ao, ao, 1.0);

  if (debug_view == 1) {
    out_add = vec4(vec3(s.r), 1.0);
  } else if (debug_view == 2) {
    // near = dark, far = bright, sky = white
    out_add = vec4(vec3(clamp(s.g / max_distance, 0.0, 1.0)), 1.0);
  } else if (debug_view == 3) {
    out_add = vec4(gi, 1.0);
  } else {
    // The lighting is baked into the colors, so guess the surface's own color by keeping the
    // hue of the pixel and normalizing its brightness. Bounce light is tinted by it.
    vec3 c = texture(color_tex, tex_coord).rgb;
    float m = max(max(c.r, c.g), c.b);
    vec3 surface = c / max(m, 0.2) * 0.8;
    out_add = vec4(surface * gi, 0.0);
  }
}
