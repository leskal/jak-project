#include "GlowLight.h"

// (AI-assisted) see GlowLight.h

#include <algorithm>
#include <array>
#include <cmath>

#include "common/log/log.h"

#include "third-party/imgui/imgui.h"

namespace {

struct GlowStateSaver {
  GLint fbo, read_fbo, draw_fbo, program, vao, array_buffer, active_tex;
  GLint tex[3];
  GLint viewport[4];
  GLboolean blend, depth_test, scissor, stencil, depth_mask, cull;
  GLboolean color_mask[4];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;

  GlowStateSaver() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    for (int i = 0; i < 3; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
    }
    glGetIntegerv(GL_VIEWPORT, viewport);
    blend = glIsEnabled(GL_BLEND);
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
  }

  ~GlowStateSaver() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    for (int i = 2; i >= 0; i--) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex[i]);
    }
    glActiveTexture(active_tex);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    set(GL_BLEND, blend);
    set(GL_DEPTH_TEST, depth_test);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    set(GL_CULL_FACE, cull);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
  }

  static void set(GLenum cap, GLboolean on) {
    if (on) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
};

void make_color_target(GLuint& fbo, GLuint& tex, int w, int h, GLenum internal, GLenum type) {
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, bufs);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("GlowLight: framebuffer is incomplete");
  }
}

}  // namespace

GlowLight::GlowLight() {
  constexpr std::array<float, 8> verts = {-1, -1, -1, 1, 1, -1, 1, 1};
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vbo);
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

GlowLight::~GlowLight() {
  free_targets();
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vbo);
}

void GlowLight::free_targets() {
  if (!m_have_targets) {
    return;
  }
  GLuint fbos[5] = {m_before_fbo, m_after_fbo, m_depth_fbo, m_emit_fbo, m_light_fbo};
  GLuint texs[5] = {m_before_tex, m_after_tex, m_depth_tex, m_emit_tex, m_light_tex};
  glDeleteFramebuffers(5, fbos);
  glDeleteTextures(5, texs);
  m_have_targets = false;
}

void GlowLight::setup_targets(int w, int h) {
  free_targets();
  m_w = w;
  m_h = h;
  m_qw = std::max(1, w / 4);
  m_qh = std::max(1, h / 4);
  glActiveTexture(GL_TEXTURE0);
  // full-res copies of the image before / after the particles (same format as the main image)
  make_color_target(m_before_fbo, m_before_tex, w, h, GL_RGBA8, GL_UNSIGNED_BYTE);
  make_color_target(m_after_fbo, m_after_tex, w, h, GL_RGBA8, GL_UNSIGNED_BYTE);
  // quarter-res emission and light
  make_color_target(m_emit_fbo, m_emit_tex, m_qw, m_qh, GL_RGBA16F, GL_FLOAT);
  glBindTexture(GL_TEXTURE_2D, m_emit_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glGenerateMipmap(GL_TEXTURE_2D);
  make_color_target(m_light_fbo, m_light_tex, m_qw, m_qh, GL_RGBA16F, GL_FLOAT);
  // depth copy (same format as the main depth buffer)
  glGenTextures(1, &m_depth_tex);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
               GL_UNSIGNED_INT_24_8, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  glGenFramebuffers(1, &m_depth_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depth_tex,
                         0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("GlowLight: depth framebuffer is incomplete");
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_have_targets = true;
}

void GlowLight::draw_quad() {
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GlowLight::stage(int stage, SharedRenderState* render_state) {
  if (!enabled || !render_state->has_pc_data) {
    m_before_valid = false;
    return;
  }
  const int w = render_state->render_fb_w;
  const int h = render_state->render_fb_h;
  if (w <= 0 || h <= 0) {
    return;
  }

  GlowStateSaver saved;
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);

  if (stage == 0) {
    if (!m_have_targets || w != m_w || h != m_h) {
      setup_targets(w, h);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_before_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    m_before_valid = true;
    return;
  }

  if (!m_before_valid || !m_have_targets || w != m_w || h != m_h) {
    return;
  }
  m_before_valid = false;

  // ---- projection (same as SSAO.cpp / make_new_cam_mat) ----
  const auto& persp = render_state->camera_persp;
  const float fog = render_state->camera_fog.x();
  const float hvdf_z = render_state->camera_hvdf_off.z();
  const float pzw = persp[2][3];
  if (std::abs(pzw) < 1e-12f || std::abs(fog) < 1e-12f) {
    return;
  }
  const float scissor_height = render_state->version == GameVersion::Jak1 ? 448.f : 416.f;
  const float height_scale = render_state->version == GameVersion::Jak1 ? 1.f : 0.5f;
  const float y_adjust = (512.f / scissor_height) * height_scale;
  const float pc_pxx = fog * persp[0][0] / 256.f;
  const float pc_pyy = -fog * persp[1][1] / 128.f;
  const float depth_scale = fog * persp[2][2] / 8388608.f + (hvdf_z / 8388608.f - 1.f) * pzw;
  const float depth_k = fog * persp[3][2] / 8388608.f;
  const float depth_a = depth_scale / pzw;
  const float to_meters = 1.f / (std::abs(pzw) * 4096.f);
  const float proj_x = std::abs(pc_pxx / pzw);
  const float proj_y = std::abs(pc_pyy * y_adjust / pzw);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // ---- copies ----
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_after_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

  // ---- 1. what glows (quarter res) ----
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_emit_fbo);
    glViewport(0, 0, m_qw, m_qh);
    auto& sh = render_state->shaders[ShaderId::LIGHTGLOW_EMIT];
    sh.activate();
    const auto id = sh.id();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_before_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_after_tex);
    glUniform1i(glGetUniformLocation(id, "before_tex"), 0);
    glUniform1i(glGetUniformLocation(id, "after_tex"), 1);
    glUniform2f(glGetUniformLocation(id, "src_texel"), 1.f / w, 1.f / h);
    glUniform1f(glGetUniformLocation(id, "particle_gain"), particle_gain);
    glUniform1f(glGetUniformLocation(id, "bright_gain"), bright_surface_gain);
    glUniform1f(glGetUniformLocation(id, "bright_threshold"), bright_threshold);
    draw_quad();
    // blurred versions of the glow, for the gather pass
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_emit_tex);
    glGenerateMipmap(GL_TEXTURE_2D);
  }

  // ---- 2. spread it over nearby surfaces (quarter res) ----
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_light_fbo);
    glViewport(0, 0, m_qw, m_qh);
    auto& sh = render_state->shaders[ShaderId::LIGHTGLOW_GATHER];
    sh.activate();
    const auto id = sh.id();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_emit_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glUniform1i(glGetUniformLocation(id, "emit_tex"), 0);
    glUniform1i(glGetUniformLocation(id, "depth_tex"), 1);
    glUniform2f(glGetUniformLocation(id, "depth_texel"), 1.f / w, 1.f / h);
    glUniform3f(glGetUniformLocation(id, "depth_params"), depth_a, depth_k, to_meters);
    glUniform2f(glGetUniformLocation(id, "proj_scale"), proj_x, proj_y);
    glUniform1f(glGetUniformLocation(id, "radius"), radius_m);
    glUniform2f(glGetUniformLocation(id, "emit_size"), (float)m_qw, (float)m_qh);
    draw_quad();
  }

  // ---- 3. add it to the image ----
  {
    glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
    glViewport(0, 0, w, h);
    auto& sh = render_state->shaders[ShaderId::LIGHTGLOW_APPLY];
    sh.activate();
    const auto id = sh.id();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_light_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_after_tex);
    glUniform1i(glGetUniformLocation(id, "light_tex"), 0);
    glUniform1i(glGetUniformLocation(id, "color_tex"), 1);
    glUniform1f(glGetUniformLocation(id, "strength"), strength);
    glUniform1i(glGetUniformLocation(id, "show_only"), show_only ? 1 : 0);
    if (show_only) {
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    } else {
      // add light, leave alpha alone
      glEnable(GL_BLEND);
      glBlendEquation(GL_FUNC_ADD);
      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    }
    draw_quad();
  }
  // GlowStateSaver restores everything
}

void GlowLight::draw_debug_window() {
  ImGui::Checkbox("Glow light", &enabled);
  ImGui::SliderFloat("Glow strength", &strength, 0.f, 6.f);
  ImGui::SliderFloat("Glow reach (m)", &radius_m, 0.5f, 12.f);
  ImGui::SliderFloat("Particles glow", &particle_gain, 0.f, 3.f);
  ImGui::SliderFloat("Bright surfaces glow", &bright_surface_gain, 0.f, 3.f);
  ImGui::SliderFloat("Bright threshold", &bright_threshold, 0.5f, 1.f);
  ImGui::Checkbox("Show glow light only", &show_only);
}
