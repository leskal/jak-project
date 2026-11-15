#version 410 core

// (AI-assisted) white = nothing there, gray/black = geometry (darker = closer to the sun)
in vec2 tex_coord;
out vec4 color;
uniform sampler2D tex_T0;

void main() {
  float d = texture(tex_T0, tex_coord).r;
  color = vec4(vec3(d >= 1.0 ? 1.0 : smoothstep(0.3, 0.7, d) * 0.8), 1.0);
}
