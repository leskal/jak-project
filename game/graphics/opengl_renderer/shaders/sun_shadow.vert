#version 410 core

// draws level geometry from the sun's point of view, for sun shadows (AI-assisted)
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec2 tex_coord_in;

uniform mat4 light_mat;  // world -> shadow map clip space

out vec2 tex_coord;

void main() {
  gl_Position = light_mat * vec4(position_in, 1.0);
  tex_coord = tex_coord_in;
}
