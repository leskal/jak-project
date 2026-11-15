#version 410 core

// glow light (AI-assisted): add the collected light to the image, tinted by the surface color.
in vec2 tex_coord;
out vec4 out_color;

uniform sampler2D light_tex;
uniform sampler2D color_tex;  // the image (after particles)
uniform float strength;
uniform int show_only;

void main() {
  vec3 light = texture(light_tex, tex_coord).rgb * strength;
  if (show_only == 1) {
    out_color = vec4(light, 1.0);
    return;
  }
  // estimate the surface's own color (same as the bounce light)
  vec3 c = texture(color_tex, tex_coord).rgb;
  float m = max(max(c.r, c.g), c.b);
  vec3 surface = c / max(m, 0.35) * 0.8;
  surface = mix(vec3(dot(surface, vec3(0.333))), surface, 0.6);
  out_color = vec4(surface * light, 0.0);
}
