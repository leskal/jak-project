#include "SunShadow.h"

// Sun shadows for the relighting mod. (AI-assisted)

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/loader/Loader.h"

#include "third-party/imgui/imgui.h"

namespace {

struct ShadowStateSaver {
  GLint fbo, program, vao, active_tex, tex0;
  GLint viewport[4];
  GLboolean depth_test, blend, cull, poly_offset, restart, depth_mask, scissor, stencil;
  GLint depth_func;
  GLfloat clear_depth, offset_factor, offset_units;

  ShadowStateSaver() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_VIEWPORT, viewport);
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    cull = glIsEnabled(GL_CULL_FACE);
    poly_offset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    restart = glIsEnabled(GL_PRIMITIVE_RESTART);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset_units);
  }

  ~ShadowStateSaver() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glUseProgram(program);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex0);
    glActiveTexture(active_tex);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    set(GL_DEPTH_TEST, depth_test);
    set(GL_BLEND, blend);
    set(GL_CULL_FACE, cull);
    set(GL_POLYGON_OFFSET_FILL, poly_offset);
    set(GL_PRIMITIVE_RESTART, restart);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    glDepthMask(depth_mask);
    glDepthFunc(depth_func);
    glClearDepth(clear_depth);
    glPolygonOffset(offset_factor, offset_units);
  }

  static void set(GLenum cap, GLboolean on) {
    if (on) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
};

bool draw_is_transparent(const DrawMode& mode) {
  return mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED;
}

u32 draw_index_count(const tfrag3::StripDraw& draw) {
  u32 n = 0;
  for (const auto& g : draw.vis_groups) {
    n += g.num_inds;
  }
  return n;
}

void setup_vao(GLuint vao, GLuint vertex_buffer, GLuint index_buffer) {
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                        (void*)offsetof(tfrag3::PreloadedVertex, x));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                        (void*)offsetof(tfrag3::PreloadedVertex, s));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace

SunShadow::~SunShadow() {
  free_target();
  free_far_target();
  free_point_target();
  if (m_point_program) {
    glDeleteProgram(m_point_program);
  }
  if (m_char_fbo[0]) {
    glDeleteFramebuffers(2, m_char_fbo);
    glDeleteTextures(2, m_char_tex);
  }
  if (m_quad_vao) {
    glDeleteVertexArrays(1, &m_quad_vao);
    glDeleteBuffers(1, &m_quad_vbo);
  }
  for (auto& [key, t] : m_trees) {
    glDeleteVertexArrays(1, &t.vao);
    if (t.own_index_buffer) {
      glDeleteBuffers(1, &t.own_index_buffer);
    }
  }
}

void SunShadow::free_target() {
  if (m_fbo) {
    glDeleteFramebuffers(1, &m_fbo);
    glDeleteTextures(1, &m_depth_tex);
    m_fbo = 0;
    m_depth_tex = 0;
  }
}

// (AI-assisted) the far shadow map: same kind of texture, covering a much bigger area
void SunShadow::free_far_target() {
  if (m_far_fbo) {
    glDeleteFramebuffers(1, &m_far_fbo);
    glDeleteTextures(1, &m_far_tex);
    m_far_fbo = 0;
    m_far_tex = 0;
  }
}

void SunShadow::setup_far_target() {
  free_far_target();
  m_far_res = far_resolution;
  glGenTextures(1, &m_far_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_far_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, m_far_res, m_far_res, 0,
               GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glGenFramebuffers(1, &m_far_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_far_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_far_tex, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SunShadow: far shadow framebuffer is incomplete");
  }
  lg::info("SunShadow: far shadow map {}x{}", m_far_res, m_far_res);
}

void SunShadow::setup_target() {
  free_target();
  m_tex_res = resolution;
  glGenTextures(1, &m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, m_tex_res, m_tex_res, 0,
               GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  // hardware shadow comparison with smoothing between texels
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depth_tex, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  m_fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (m_fbo_status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SunShadow: shadow framebuffer is incomplete: 0x{:x}", m_fbo_status);
  }
  lg::info("SunShadow: shadow map {}x{}", m_tex_res, m_tex_res);
}

void SunShadow::setup_char_targets() {
  if (m_char_fbo[0]) {
    glDeleteFramebuffers(2, m_char_fbo);
    glDeleteTextures(2, m_char_tex);
  }
  m_char_res = char_resolution;
  glGenTextures(2, m_char_tex);
  glGenFramebuffers(2, m_char_fbo);
  for (int i = 0; i < 2; i++) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_char_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, m_char_res, m_char_res, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, m_char_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_char_tex[i], 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("SunShadow: character shadow framebuffer {} is incomplete", i);
    }
  }
  m_char_prev_valid = false;
}

void SunShadow::update_game_sun(SharedRenderState::RelightSettings& rl) {
  const auto now = std::chrono::steady_clock::now();
  float dt = m_have_time ? std::chrono::duration<float>(now - m_last_time).count() : 0.f;
  dt = std::min(std::max(dt, 0.f), 0.25f);
  m_last_time = now;
  m_have_time = true;
  if (!rl.game_sun_raw_valid) {
    return;
  }

  auto copy3 = [](float* dst, const float* src) {
    for (int i = 0; i < 3; i++) {
      dst[i] = src[i];
    }
  };
  // move the filtered sun towards the raw one by fraction k
  auto approach = [&](float k) {
    float n = 0;
    for (int i = 0; i < 3; i++) {
      rl.game_sun_dir[i] += (rl.game_sun_raw_dir[i] - rl.game_sun_dir[i]) * k;
      rl.game_sun_color[i] += (rl.game_sun_raw_color[i] - rl.game_sun_color[i]) * k;
      rl.game_ambient[i] += (rl.game_raw_ambient[i] - rl.game_ambient[i]) * k;
      n += rl.game_sun_dir[i] * rl.game_sun_dir[i];
    }
    n = std::sqrt(n);
    if (n > 1e-6f) {
      for (int i = 0; i < 3; i++) {
        rl.game_sun_dir[i] /= n;
      }
    }
  };

  if (!rl.game_sun_valid) {
    // first time: take it as it is
    copy3(rl.game_sun_dir, rl.game_sun_raw_dir);
    copy3(rl.game_sun_color, rl.game_sun_raw_color);
    copy3(rl.game_ambient, rl.game_raw_ambient);
    rl.game_sun_valid = true;
    m_pending_time = 0;
    return;
  }

  auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const float cos_20_deg = 0.9397f;
  if (dot3(rl.game_sun_dir, rl.game_sun_raw_dir) >= cos_20_deg) {
    // small change (time of day): follow it smoothly
    m_pending_time = 0;
    approach(1.f - std::exp(-dt / std::max(rl.sun_smooth_s, 0.01f)));
  } else {
    // sudden jump (e.g. Jak walked indoors and the game lights him with a lamp): ignore it,
    // unless the new light stays the same for a long time (e.g. a different level)
    if (m_pending_time > 0 && dot3(m_pending_dir, rl.game_sun_raw_dir) < 0.98f) {
      m_pending_time = 0;  // it keeps changing: start counting again
    }
    copy3(m_pending_dir, rl.game_sun_raw_dir);
    m_pending_time += dt;
    if (m_pending_time >= rl.sun_jump_accept_s) {
      approach(1.f - std::exp(-dt / 3.f));  // move over to it over a few seconds
    }
  }
}

void SunShadow::render(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  auto& rl = render_state->relight;
  update_game_sun(rl);
  rl.shadow_valid = false;
  rl.far_shadow_valid = false;
  rl.char_shadow_write = false;
  rl.char_shadow_valid = false;
  rl.chars_receive = chars_receive;
  m_frame++;
  m_last_draw_calls = 0;
  if (!enabled || rl.amount <= 0.f || !render_state->loader) {
    m_char_prev_valid = false;
    return;
  }

  // ---- direction towards the sun (same choice as the level shaders) ----
  double L[3];
  if (rl.use_game_sun && rl.game_sun_valid) {
    for (int i = 0; i < 3; i++) {
      L[i] = rl.game_sun_dir[i];
    }
  } else {
    const double az = rl.sun_azimuth_deg * 3.14159265358979 / 180.0;
    const double el = rl.sun_elevation_deg * 3.14159265358979 / 180.0;
    L[0] = std::cos(el) * std::sin(az);
    L[1] = std::sin(el);
    L[2] = std::cos(el) * std::cos(az);
  }
  if (L[1] < 0.02) {
    m_char_prev_valid = false;
    return;  // sun at or below the horizon: no sun shadows
  }

  // ---- light space: f = direction the light travels, r/u = the shadow map's x/y axes ----
  const double f[3] = {-L[0], -L[1], -L[2]};
  // right = normalize(cross(f, world_up)); world_up = +y (the sun is never straight down here,
  // and if it nearly is, use +z instead)
  double up[3] = {0, 1, 0};
  if (std::abs(f[1]) > 0.99) {
    up[0] = 0;
    up[1] = 0;
    up[2] = 1;
  }
  double r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                 f[0] * up[1] - f[1] * up[0]};
  double rlen = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (auto& x : r) {
    x /= rlen;
  }
  double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2],
                 r[0] * f[1] - r[1] * f[0]};

  // center on the camera (last frame's position is fine). Snap to whole shadow map texels, so
  // the shadows don't shimmer when the camera moves.
  const double C[3] = {render_state->camera_pos.x(), render_state->camera_pos.y(),
                       render_state->camera_pos.z()};
  if (!m_fbo || m_tex_res != resolution) {
    setup_target();
  }
  auto dot3 = [](const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  // world (game units) -> shadow map clip space (column major for OpenGL), for a map covering
  // +/- H around the camera, +/- D along the sun direction, res x res texels
  auto build = [&](double H, double D, int res, float* clip, float* tex_mat) {
    // snap to whole shadow map texels, so the shadows don't shimmer when the camera moves
    const double texel = 2.0 * H / res;
    const double cr = std::round(dot3(C, r) / texel) * texel;
    const double cu = std::round(dot3(C, u) / texel) * texel;
    const double cf = dot3(C, f);
    for (int col = 0; col < 3; col++) {
      clip[col * 4 + 0] = (float)(r[col] / H);
      clip[col * 4 + 1] = (float)(u[col] / H);
      clip[col * 4 + 2] = (float)(f[col] / D);
      clip[col * 4 + 3] = 0.f;
    }
    clip[12] = (float)(-cr / H);
    clip[13] = (float)(-cu / H);
    clip[14] = (float)(-cf / D);
    clip[15] = 1.f;
    // world -> shadow map texture coordinates (0..1), for the shaders
    for (int col = 0; col < 4; col++) {
      for (int row = 0; row < 3; row++) {
        tex_mat[col * 4 + row] = 0.5f * clip[col * 4 + row] + (col == 3 ? 0.5f : 0.f);
      }
      tex_mat[col * 4 + 3] = clip[col * 4 + 3];
    }
  };
  float clip[16];
  build(half_size_m * 4096.0, depth_range_m * 4096.0, m_tex_res, clip, rl.shadow_mat);
  for (int i = 0; i < 16; i++) {
    rl.shadow_clip[i] = clip[i];
  }

  // ---- draw the level from the sun ----
  auto p = prof.make_scoped_child("sun-shadow");
  ShadowStateSaver saved;

  // character shadow maps: last frame's becomes readable, this frame's is cleared and handed to
  // the character renderer (Merc2), which draws into it as characters are drawn.
  if (chars_cast) {
    if (!m_char_fbo[0] || m_char_res != char_resolution) {
      setup_char_targets();
    }
    m_char_write = 1 - m_char_write;
    const int read = 1 - m_char_write;
    rl.char_shadow_valid = m_char_prev_valid;
    rl.char_shadow_tex = m_char_tex[read];
    for (int i = 0; i < 16; i++) {
      rl.char_shadow_mat[i] = m_char_prev_mat[i];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_char_fbo[m_char_write]);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    rl.char_shadow_fbo = m_char_fbo[m_char_write];
    rl.char_shadow_res = m_char_res;
    rl.char_shadow_write = true;
    m_char_prev_valid = true;  // matrix copied below, once it's known
  } else {
    m_char_prev_valid = false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glViewport(0, 0, m_tex_res, m_tex_res);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);  // two sided: the game's triangle winding isn't reliable
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5f, 4.f);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  auto& sh = render_state->shaders[ShaderId::SUN_SHADOW];
  sh.activate();
  glUniformMatrix4fv(glGetUniformLocation(sh.id(), "light_mat"), 1, GL_FALSE, clip);
  glUniform1i(glGetUniformLocation(sh.id(), "tex_T0"), 0);
  const GLint use_tex_loc = glGetUniformLocation(sh.id(), "use_tex");
  glActiveTexture(GL_TEXTURE0);

  auto draw_level = [&]() { draw_level_geometry(render_state, prof, use_tex_loc); };
  draw_level();

  // (AI-assisted) far shadow map: the same level, from the same sun, over a much bigger area,
  // so buildings and cliffs far away also get shadows
  if (far_enabled) {
    if (!m_far_fbo || m_far_res != far_resolution) {
      setup_far_target();
      glActiveTexture(GL_TEXTURE0);
    }
    float far_clip[16];
    const double far_h = std::max(far_half_size_m, half_size_m * 1.5f) * 4096.0;
    build(far_h, depth_range_m * 4096.0 + far_h, m_far_res, far_clip, rl.far_shadow_mat);
    glBindFramebuffer(GL_FRAMEBUFFER, m_far_fbo);
    glViewport(0, 0, m_far_res, m_far_res);
    glClear(GL_DEPTH_BUFFER_BIT);
    glPolygonOffset(2.f, 8.f);
    sh.activate();
    glUniformMatrix4fv(glGetUniformLocation(sh.id(), "light_mat"), 1, GL_FALSE, far_clip);
    draw_level();
    rl.far_shadow_valid = true;
    rl.far_shadow_tex = m_far_tex;
    // the far map's texels are bigger, so push further off the surface
    rl.far_shadow_normal_offset =
        normal_offset_m * 4096.f * (float)(far_h / (half_size_m * 4096.0)) * 0.5f;
  }

  // forget about trees of levels that were unloaded
  for (auto it = m_trees.begin(); it != m_trees.end();) {
    if (m_frame - it->second.last_used_frame > 120) {
      glDeleteVertexArrays(1, &it->second.vao);
      if (it->second.own_index_buffer) {
        glDeleteBuffers(1, &it->second.own_index_buffer);
      }
      it = m_trees.erase(it);
    } else {
      ++it;
    }
  }

  rl.shadow_valid = true;
  if (chars_cast) {
    for (int i = 0; i < 16; i++) {
      m_char_prev_mat[i] = rl.shadow_mat[i];  // characters drawn this frame use this frame's matrix
    }
  }
  rl.shadow_tex = m_depth_tex;
  rl.shadow_strength = strength;
  rl.shadow_normal_offset = normal_offset_m * 4096.f;
  // ShadowStateSaver puts the OpenGL state back here
}

void SunShadow::draw_debug_window() {
  ImGui::Checkbox("Sun shadows", &enabled);
  ImGui::SliderFloat("Shadow strength", &strength, 0.f, 1.f);
  ImGui::SliderFloat("Shadow area (m)", &half_size_m, 10.f, 200.f);
  ImGui::Checkbox("Point light shadows", &point_shadows);
  ImGui::SameLine();
  ImGui::Text("(%d lights)", m_point_shadow_count);
  ImGui::SliderInt("Point lights with shadows", &point_shadow_count, 1, kMaxPointShadows);
  if (ImGui::RadioButton("256##pl", point_shadow_res == 256)) {
    point_shadow_res = 256;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("512##pl", point_shadow_res == 512)) {
    point_shadow_res = 512;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("1024##pl", point_shadow_res == 1024)) {
    point_shadow_res = 1024;
  }
  ImGui::Checkbox("Far shadows", &far_enabled);
  ImGui::SameLine();
  ImGui::SliderFloat("Far shadow area (m)", &far_half_size_m, 100.f, 1500.f);
  ImGui::SliderFloat("Shadow offset (m)", &normal_offset_m, 0.f, 0.5f);
  if (ImGui::RadioButton("2048", resolution == 2048)) {
    resolution = 2048;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("4096", resolution == 4096)) {
    resolution = 4096;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("8192", resolution == 8192)) {
    resolution = 8192;
  }
  ImGui::Checkbox("Ground/walls cast shadows", &tfrag_casts);
  ImGui::SameLine();
  ImGui::Checkbox("Objects cast shadows", &tie_casts);
  ImGui::Checkbox("Characters cast shadows", &chars_cast);
  ImGui::SameLine();
  ImGui::Checkbox("Characters receive shadows", &chars_receive);
  ImGui::Checkbox("Hide the game's blob shadow", &hide_blob_shadow);
  ImGui::Checkbox("Show shadow map", &show_map);
  ImGui::Text("Shadow draws last frame: %d, framebuffer %s", m_last_draw_calls,
              m_fbo_status == GL_FRAMEBUFFER_COMPLETE ? "ok" : "BROKEN");
}

void SunShadow::draw_overlay(SharedRenderState* render_state) {
  if (!show_map || !m_depth_tex) {
    return;
  }
  ShadowStateSaver saved;
  if (!m_quad_vao) {
    const float verts[8] = {-1, -1, -1, 1, 1, -1, 1, 1};
    glGenVertexArrays(1, &m_quad_vao);
    glGenBuffers(1, &m_quad_vbo);
    glBindVertexArray(m_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  const int size = render_state->render_fb_h / 2;
  glViewport(0, 0, size, size);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  auto& sh = render_state->shaders[ShaderId::SUN_SHADOW_DEBUG];
  sh.activate();
  glUniform1i(glGetUniformLocation(sh.id(), "tex_T0"), 0);
  glBindVertexArray(m_quad_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
}

// draws the level geometry (tfrag + tie, opaque, best detail) with whatever shadow shader is
// active. Vertex attributes: 0 = position (world), 1 = texture coordinate. (AI-assisted)
void SunShadow::draw_level_geometry(SharedRenderState* render_state,
                                    ScopedProfilerNode& prof,
                                    GLint use_tex_loc) {
  auto draw_list = [&](const LevelData* lev, const tfrag3::StripDraw& draw, GLenum prim) {
    if (draw_is_transparent(draw.mode)) {
      return;
    }
    u32 count = draw_index_count(draw);
    if (!count) {
      return;
    }
    // textured, so cut-out leaves and fences cast the right shape
    if (draw.tree_tex_id >= 0 && draw.tree_tex_id < (int)lev->textures.size()) {
      glBindTexture(GL_TEXTURE_2D, lev->textures[draw.tree_tex_id]);
      glUniform1i(use_tex_loc, 1);
    } else {
      glUniform1i(use_tex_loc, 0);
    }
    glDrawElements(prim, count, GL_UNSIGNED_INT,
                   (void*)(sizeof(u32) * (size_t)draw.unpacked.idx_of_first_idx_in_full_buffer));
    m_last_draw_calls++;
    prof.add_draw_call();
  };

  for (const LevelData* lev : render_state->loader->get_in_use_levels()) {
    if (!lev || !lev->level) {
      continue;
    }
    const auto& level = *lev->level;

    // tfrag (ground, walls), best detail level only, normal (opaque) trees only
    const auto& tfrags = level.tfrag_trees[0];
    for (size_t ti = 0;
         tfrag_casts && ti < tfrags.size() && ti < lev->tfrag_vertex_data[0].size(); ti++) {
      const auto& tree = tfrags[ti];
      if (tree.kind != tfrag3::TFragmentTreeKind::NORMAL || tree.unpacked.indices.empty()) {
        continue;
      }
      auto& t = m_trees[{lev->load_id, 0, (int)ti}];
      if (!t.vao) {
        glGenVertexArrays(1, &t.vao);
        glGenBuffers(1, &t.own_index_buffer);
        glBindVertexArray(t.vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, t.own_index_buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                     tree.unpacked.indices.data(), GL_STATIC_DRAW);
        setup_vao(t.vao, lev->tfrag_vertex_data[0][ti], t.own_index_buffer);
      }
      t.last_used_frame = m_frame;
      glBindVertexArray(t.vao);
      const GLenum prim = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      for (const auto& draw : tree.draws) {
        draw_list(lev, draw, prim);
      }
    }

    // tie (objects: houses, rocks, fences...), best detail level, opaque categories only
    const auto& ties = level.tie_trees[0];
    for (size_t ti = 0; tie_casts && ti < ties.size() && ti < lev->tie_data[0].size(); ti++) {
      const auto& tree = ties[ti];
      if (tree.unpacked.indices.empty()) {
        continue;
      }
      auto& t = m_trees[{lev->load_id, 1, (int)ti}];
      if (!t.vao) {
        glGenVertexArrays(1, &t.vao);
        setup_vao(t.vao, lev->tie_data[0][ti].vertex_buffer, lev->tie_data[0][ti].index_buffer);
      }
      t.last_used_frame = m_frame;
      glBindVertexArray(t.vao);
      const GLenum prim = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      for (auto cat : {tfrag3::TieCategory::NORMAL, tfrag3::TieCategory::NORMAL_ENVMAP}) {
        const u32 first = tree.category_draw_indices[(int)cat];
        const u32 last = tree.category_draw_indices[(int)cat + 1];
        for (u32 di = first; di < last && di < tree.static_draws.size(); di++) {
          draw_list(lev, tree.static_draws[di], prim);
        }
      }
    }
  }

}

// ---------------------------------------------------------------------------------------------
// (AI-assisted) point light shadows: a cube shadow map (6 directions) for each of the strongest
// point lights, all in one cube map array. The level is drawn once; a geometry shader sends each
// triangle to the cube faces of the lights it is near.
namespace {
const char* kPointVert = R"(#version 410 core
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec2 tex_coord_in;
out vec3 v_world;
out vec2 v_tc;
void main() {
  v_world = position_in;
  v_tc = tex_coord_in;
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

const char* kPointGeom = R"(#version 410 core
layout (triangles) in;
layout (triangle_strip, max_vertices = 72) out;
in vec3 v_world[];
in vec2 v_tc[];
uniform mat4 face_mat[6];
uniform vec4 lights[4];  // xyz = position, w = reach (game units)
uniform int light_count;
out vec3 g_rel;
out vec2 g_tc;
flat out float g_inv_r;
void main() {
  vec3 mn = min(min(v_world[0], v_world[1]), v_world[2]);
  vec3 mx = max(max(v_world[0], v_world[1]), v_world[2]);
  for (int l = 0; l < light_count; l++) {
    vec3 c = lights[l].xyz;
    float R = lights[l].w;
    vec3 dq = clamp(c, mn, mx) - c;
    if (dot(dq, dq) > R * R) {
      continue;  // triangle is out of this light's reach
    }
    vec3 rel[3];
    for (int k = 0; k < 3; k++) {
      rel[k] = v_world[k] - c;
    }
    for (int f = 0; f < 6; f++) {
      vec4 cp[3];
      for (int k = 0; k < 3; k++) {
        cp[k] = face_mat[f] * vec4(rel[k], 1.0);
      }
      // skip faces the triangle is completely outside of
      if (cp[0].x > cp[0].w && cp[1].x > cp[1].w && cp[2].x > cp[2].w) continue;
      if (cp[0].x < -cp[0].w && cp[1].x < -cp[1].w && cp[2].x < -cp[2].w) continue;
      if (cp[0].y > cp[0].w && cp[1].y > cp[1].w && cp[2].y > cp[2].w) continue;
      if (cp[0].y < -cp[0].w && cp[1].y < -cp[1].w && cp[2].y < -cp[2].w) continue;
      if (cp[0].w < 0.0 && cp[1].w < 0.0 && cp[2].w < 0.0) continue;
      for (int k = 0; k < 3; k++) {
        gl_Layer = l * 6 + f;
        gl_Position = cp[k];
        g_rel = rel[k];
        g_tc = v_tc[k];
        g_inv_r = 1.0 / R;
        EmitVertex();
      }
      EndPrimitive();
    }
  }
}
)";

const char* kPointFrag = R"(#version 410 core
in vec3 g_rel;
in vec2 g_tc;
flat in float g_inv_r;
uniform sampler2D tex_T0;
uniform int use_tex;
void main() {
  if (use_tex == 1 && texture(tex_T0, g_tc).a < 0.2) {
    discard;
  }
  // distance from the light, 0 = at the light, 1 = end of its reach
  gl_FragDepth = clamp(length(g_rel) * g_inv_r, 0.0, 1.0);
}
)";

GLuint compile_stage(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    lg::error("SunShadow: point shadow shader compile failed: {}", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}
}  // namespace

bool SunShadow::compile_point_program() {
  if (m_point_program) {
    return true;
  }
  if (m_point_compile_failed) {
    return false;
  }
  GLuint vs = compile_stage(GL_VERTEX_SHADER, kPointVert);
  GLuint gs = compile_stage(GL_GEOMETRY_SHADER, kPointGeom);
  GLuint fs = compile_stage(GL_FRAGMENT_SHADER, kPointFrag);
  if (!vs || !gs || !fs) {
    m_point_compile_failed = true;
    return false;
  }
  m_point_program = glCreateProgram();
  glAttachShader(m_point_program, vs);
  glAttachShader(m_point_program, gs);
  glAttachShader(m_point_program, fs);
  glLinkProgram(m_point_program);
  glDeleteShader(vs);
  glDeleteShader(gs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(m_point_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(m_point_program, sizeof(log), nullptr, log);
    lg::error("SunShadow: point shadow shader link failed: {}", log);
    glDeleteProgram(m_point_program);
    m_point_program = 0;
    m_point_compile_failed = true;
    return false;
  }
  return true;
}

void SunShadow::free_point_target() {
  if (m_point_fbo) {
    glDeleteFramebuffers(1, &m_point_fbo);
    glDeleteTextures(1, &m_point_tex);
    m_point_fbo = 0;
    m_point_tex = 0;
  }
}

void SunShadow::setup_point_target() {
  free_point_target();
  m_point_res = point_shadow_res;
  glGenTextures(1, &m_point_tex);
  glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_point_tex);
  glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F, m_point_res, m_point_res,
               6 * kMaxPointShadows, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
  glGenFramebuffers(1, &m_point_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_point_fbo);
  glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_point_tex, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SunShadow: point shadow framebuffer is incomplete");
  }
  lg::info("SunShadow: point light shadow maps {}x{} x{}", m_point_res, m_point_res,
           kMaxPointShadows);
}

void SunShadow::render_point_shadows(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  auto& rl = render_state->relight;
  const auto& pl = render_state->point_lights;
  rl.pl_shadow_valid = false;
  for (int i = 0; i < 16; i++) {
    rl.pl_shadow_layer[i] = -1;
  }
  m_point_shadow_count = 0;
  if (!point_shadows || pl.count <= 0 || !render_state->loader) {
    return;
  }
  if (!compile_point_program()) {
    return;
  }

  // pick the strongest lights near the camera
  const float cam[3] = {render_state->camera_pos.x(), render_state->camera_pos.y(),
                        render_state->camera_pos.z()};
  std::vector<std::pair<float, int>> order;
  for (int i = 0; i < pl.count && i < 16; i++) {
    const float* c = &pl.color[i * 4];
    const float* p = &pl.pos_radius[i * 4];
    const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
      d2 += (p[k] - cam[k]) * (p[k] - cam[k]);
    }
    const float d_m = std::sqrt(d2) / 4096.f;
    order.push_back({lum * (p[3] / 4096.f) / (1.f + d_m / 15.f), i});
  }
  std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
  const int n = std::min({(int)order.size(), std::clamp(point_shadow_count, 1, kMaxPointShadows)});
  if (n <= 0) {
    return;
  }

  auto p = prof.make_scoped_child("point-shadows");
  ShadowStateSaver saved;
  if (!m_point_fbo || m_point_res != point_shadow_res) {
    setup_point_target();
  }

  // the 6 cube faces, OpenGL cube map order (+x, -x, +y, -y, +z, -z)
  const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const float ups[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
  const float near_z = 50.f;            // game units
  const float far_z = 4096.f * 200.f;   // only for clipping; the stored depth is distance / reach
  float face_mats[6 * 16];
  for (int f = 0; f < 6; f++) {
    const float* fw = dirs[f];
    const float* up = ups[f];
    float s[3] = {fw[1] * up[2] - fw[2] * up[1], fw[2] * up[0] - fw[0] * up[2],
                  fw[0] * up[1] - fw[1] * up[0]};
    float u[3] = {s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2],
                  s[0] * fw[1] - s[1] * fw[0]};
    float V[16] = {};  // column major, rotation only
    for (int col = 0; col < 3; col++) {
      V[col * 4 + 0] = s[col];
      V[col * 4 + 1] = u[col];
      V[col * 4 + 2] = -fw[col];
    }
    V[15] = 1.f;
    float P[16] = {};  // 90 degree perspective
    P[0] = 1.f;
    P[5] = 1.f;
    P[10] = (far_z + near_z) / (near_z - far_z);
    P[11] = -1.f;
    P[14] = 2.f * far_z * near_z / (near_z - far_z);
    float* M = &face_mats[f * 16];
    for (int col = 0; col < 4; col++) {
      for (int row = 0; row < 4; row++) {
        float sum = 0;
        for (int k = 0; k < 4; k++) {
          sum += P[k * 4 + row] * V[col * 4 + k];
        }
        M[col * 4 + row] = sum;
      }
    }
  }

  float lights[4 * kMaxPointShadows] = {};
  for (int l = 0; l < n; l++) {
    const int li = order[l].second;
    for (int k = 0; k < 4; k++) {
      lights[l * 4 + k] = pl.pos_radius[li * 4 + k];
    }
    rl.pl_shadow_layer[li] = l;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, m_point_fbo);
  glViewport(0, 0, m_point_res, m_point_res);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  glUseProgram(m_point_program);
  glUniformMatrix4fv(glGetUniformLocation(m_point_program, "face_mat"), 6, GL_FALSE, face_mats);
  glUniform4fv(glGetUniformLocation(m_point_program, "lights"), kMaxPointShadows, lights);
  glUniform1i(glGetUniformLocation(m_point_program, "light_count"), n);
  glUniform1i(glGetUniformLocation(m_point_program, "tex_T0"), 0);
  const GLint use_tex_loc = glGetUniformLocation(m_point_program, "use_tex");
  glActiveTexture(GL_TEXTURE0);
  draw_level_geometry(render_state, p, use_tex_loc);

  rl.pl_shadow_valid = true;
  rl.pl_shadow_tex = m_point_tex;
  rl.pl_shadow_res = (float)m_point_res;
  m_point_shadow_count = n;
}
