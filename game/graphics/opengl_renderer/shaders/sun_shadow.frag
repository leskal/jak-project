#version 410 core

// only depth is written. Cut-out parts of textures (leaves, fences) don't cast shadows.
// (AI-assisted)
in vec2 tex_coord;
uniform sampler2D tex_T0;
uniform int use_tex;

void main() {
  if (use_tex == 1 && texture(tex_T0, tex_coord).a < 0.2) {
    discard;
  }
}
