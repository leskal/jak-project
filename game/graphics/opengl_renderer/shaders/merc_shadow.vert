#version 410 core

// draws characters (merc) from the sun's point of view, so they cast sun shadows (AI-assisted)
layout (location = 0) in vec3 position_in;
layout (location = 2) in vec3 weights_in;
layout (location = 5) in uvec3 mats;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

uniform mat3 cam_rot;    // camera space = cam_rot * (world - cam_trans)
uniform vec3 cam_trans;
uniform mat4 light_mat;  // world -> shadow map clip space

void main() {
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
  }
  // the bones put the vertex in camera space; go back to world space
  vec3 world = transpose(cam_rot) * (-vtx_pos.xyz) + cam_trans;
  gl_Position = light_mat * vec4(world, 1.0);
}
