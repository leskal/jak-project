#version 410 core

out vec4 color;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

// (AI-assisted) relighting mod: sun shadows on characters
in vec3 merc_world;
in vec3 vtx_sun;
uniform sampler2DShadow shadow_tex;
uniform mat4 shadow_mat;
uniform int shadow_enabled;
uniform float shadow_strength;
uniform int relight_debug;

float sun_shadow_merc() {
  if (shadow_enabled == 0) {
    return 1.0;
  }
  vec4 sc = shadow_mat * vec4(merc_world, 1.0);
  if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) {
    return 1.0;
  }
  float s = 0.0;
  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      s += textureOffset(shadow_tex, sc.xyz, ivec2(x, y));
    }
  }
  s /= 9.0;
  vec2 e = min(sc.xy, 1.0 - sc.xy);
  float edge = clamp(min(e.x, e.y) * 10.0, 0.0, 1.0);
  return mix(1.0, s, shadow_strength * edge);
}

void main() {
  // in shadow, remove the sun's part of the lighting (sky / ambient light stays)
  float sun_vis = sun_shadow_merc();
  vec4 vc = vtx_color;
  vc.rgb = max(vc.rgb - vtx_sun * (1.0 - sun_vis), vec3(0.0));

  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vc * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = vc.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  if (relight_debug == 2 && shadow_enabled == 1) {
    color.rgb = vec3(sun_vis);
  }

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
