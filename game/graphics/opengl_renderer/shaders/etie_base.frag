#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// (AI-assisted) relighting mod
uniform float relight_amount;  // 0 = original, 1 = fully relit
uniform vec3 sun_dir;          // towards the sun, world space
uniform vec3 sun_color;
uniform vec3 ambient_color;
uniform int relight_debug;     // 1 = show normals
in vec3 world_normal;
in vec3 view_dir;


// (AI-assisted) sun shadows
uniform sampler2DShadow shadow_tex;
uniform mat4 shadow_mat;          // world -> shadow map texture coordinates
uniform int shadow_enabled;
uniform float shadow_strength;
uniform float shadow_normal_offset;  // game units
uniform vec4 cam_trans;

// 1 = the sun reaches this point, 0 = in shadow
float sun_shadow(vec3 N) {
  if (shadow_enabled == 0) {
    return 1.0;
  }
  vec3 world = view_dir + cam_trans.xyz + N * shadow_normal_offset;
  vec4 sc = shadow_mat * vec4(world, 1.0);
  if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) {
    return 1.0;  // outside the shadow map
  }
  float s = 0.0;
  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      s += textureOffset(shadow_tex, sc.xyz, ivec2(x, y));
    }
  }
  s /= 9.0;
  // fade out at the edge of the shadow map area
  vec2 e = min(sc.xy, 1.0 - sc.xy);
  float edge = clamp(min(e.x, e.y) * 10.0, 0.0, 1.0);
  return mix(1.0, s, shadow_strength * edge);
}

void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }


  // (AI-assisted) relighting: light the surface with a real sun, using the computed normal.
  // Fallback for geometry that came without a usable normal (e.g. thin strips along paths):
  // use the flat normal of the triangle, from how the position changes across the screen.
  vec3 face_n = cross(dFdx(view_dir), dFdy(view_dir));
  vec3 n_in = length(world_normal) > 0.1 ? world_normal : face_n;
  float nlen = length(n_in);
  if (nlen > 1e-8 && (relight_amount > 0.0 || relight_debug != 0)) {
    vec3 N = n_in / nlen;
    if (dot(N, view_dir) > 0.0) {
      N = -N;  // winding in the game data isn't reliable: use the side facing the camera
    }
    if (relight_debug == 1) {
      color.rgb = N * 0.5 + 0.5;
    } else if (relight_debug == 2) {
      color.rgb = vec3(sun_shadow(N));
    } else {
      vec3 light = ambient_color + sun_color * max(dot(N, sun_dir), 0.0) * sun_shadow(N);
      color.rgb *= mix(vec3(1.0), light, relight_amount);
    }
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));

}
