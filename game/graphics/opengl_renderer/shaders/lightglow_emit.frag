#version 410 core

// glow light (AI-assisted): what glows? = the light the particles added (after - before),
// plus optionally very bright surfaces. Quarter resolution.
in vec2 tex_coord;
out vec4 out_color;

uniform sampler2D before_tex;  // image before particles
uniform sampler2D after_tex;   // image after particles
uniform vec2 src_texel;        // 1 / full resolution
uniform float particle_gain;
uniform float bright_gain;
uniform float bright_threshold;

void main() {
  vec3 sum = vec3(0.0);
  // 4 bilinear taps cover the 4x4 full-res pixels of this quarter-res pixel
  for (int x = 0; x < 2; x++) {
    for (int y = 0; y < 2; y++) {
      vec2 uv = tex_coord + (vec2(x, y) - 0.5) * 2.0 * src_texel;
      vec3 b = texture(before_tex, uv).rgb;
      vec3 a = texture(after_tex, uv).rgb;
      // ignore tiny differences (blending / rounding), only real added light counts
      vec3 e = max(a - b - vec3(0.04), vec3(0.0)) * particle_gain;
      float lb = max(max(b.r, b.g), b.b);
      e += b * max(lb - bright_threshold, 0.0) / max(1.0 - bright_threshold, 0.01) * bright_gain;
      sum += e;
    }
  }
  out_color = vec4(sum * 0.25, 1.0);
}
