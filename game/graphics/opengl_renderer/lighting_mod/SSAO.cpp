#include "SSAO.h"

// Screen-space ambient occlusion pass. (AI-assisted)

#include <algorithm>
#include <array>
#include <cmath>

#include "common/log/log.h"

#include "third-party/imgui/imgui.h"

namespace {

// Saves the OpenGL state we touch, and puts it back when it goes out of scope, so the
// bucket renderers that run after us see exactly what they expect.
struct GlStateSaver {
  GLint fbo, read_fbo, draw_fbo, program, vao, array_buffer, active_tex;
  GLint tex0, tex1, tex2, tex3;
  GLint viewport[4];
  GLboolean blend, depth_test, scissor, stencil, depth_mask;
  GLboolean color_mask[4];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
  GLint depth_func;

  GlStateSaver() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex3);
    glGetIntegerv(GL_VIEWPORT, viewport);
    blend = glIsEnabled(GL_BLEND);
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
  }

  ~GlStateSaver() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex3);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex0);
    glActiveTexture(active_tex);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    set(GL_BLEND, blend);
    set(GL_DEPTH_TEST, depth_test);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
    glDepthFunc(depth_func);
  }

  static void set(GLenum cap, GLboolean on) {
    if (on) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
};

void set_tex_params(GLenum filter) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

}  // namespace

SSAO::SSAO() {
  // full screen quad, same layout as the post_processing shader expects
  struct Vertex {
    float x, y;
  };
  constexpr std::array<Vertex, 4> vertices = {
      Vertex{-1, -1},
      Vertex{-1, 1},
      Vertex{1, -1},
      Vertex{1, 1},
  };
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vbo);
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

SSAO::~SSAO() {
  free_targets();
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vbo);
}

void SSAO::free_targets() {
  if (!m_have_targets) {
    return;
  }
  glDeleteFramebuffers(1, &m_depth_fbo);
  glDeleteTextures(1, &m_depth_tex);
  glDeleteFramebuffers(1, &m_color_fbo);
  glDeleteTextures(1, &m_color_tex);
  glDeleteFramebuffers(2, m_ao_fbo);
  glDeleteTextures(2, m_ao_tex);
  glDeleteTextures(2, m_gi_tex);
  m_have_targets = false;
}

void SSAO::setup_targets(int w, int h, int msaa) {
  free_targets();
  m_w = w;
  m_h = h;
  m_msaa = msaa;
  m_targets_half_res = half_resolution;
  m_ao_w = half_resolution ? std::max(1, w / 2) : w;
  m_ao_h = half_resolution ? std::max(1, h / 2) : h;

  // Depth copy target. Must be the exact same format as the main depth buffer
  // (GL_DEPTH24_STENCIL8, see make_fbo in OpenGLRenderer.cpp), otherwise the blit fails.
  glGenTextures(1, &m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
               GL_UNSIGNED_INT_24_8, nullptr);
  set_tex_params(GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

  glGenFramebuffers(1, &m_depth_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depth_tex,
                         0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SSAO: depth copy framebuffer is incomplete");
  }

  // Color copy target (same format as the main color buffer, needed for the MSAA resolve blit)
  glGenTextures(1, &m_color_tex);
  glBindTexture(GL_TEXTURE_2D, m_color_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  set_tex_params(GL_LINEAR);
  glGenFramebuffers(1, &m_color_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_color_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color_tex, 0);
  {
    GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
  }
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SSAO: color copy framebuffer is incomplete");
  }

  // AO targets: R = occlusion, G = linear depth (used by the blur). GI targets: RGB bounce light.
  glGenTextures(2, m_ao_tex);
  glGenTextures(2, m_gi_tex);
  glGenFramebuffers(2, m_ao_fbo);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, m_ao_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, m_ao_w, m_ao_h, 0, GL_RG, GL_FLOAT, nullptr);
    set_tex_params(GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, m_gi_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ao_w, m_ao_h, 0, GL_RGBA, GL_FLOAT, nullptr);
    set_tex_params(GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[i], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gi_tex[i], 0);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("SSAO: ao framebuffer {} is incomplete", i);
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_have_targets = true;
  lg::info("SSAO: targets set up for {}x{} (msaa {}), ao buffer {}x{}", w, h, msaa, m_ao_w,
           m_ao_h);
}

void SSAO::draw_quad() {
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void SSAO::render(SharedRenderState* render_state, ScopedProfilerNode& prof, int msaa) {
  // (AI-assisted) the depth copy is shared with the water (clear water, shore foam)
  render_state->relight.scene_depth_valid = false;
  if (!enabled) {
    return;
  }
  // we need the camera's projection, which the background renderers give us. If no level
  // geometry was drawn this frame (loading screens, etc.) there's nothing to do.
  if (!render_state->has_pc_data) {
    return;
  }

  const int w = render_state->render_fb_w;
  const int h = render_state->render_fb_h;
  if (w <= 0 || h <= 0) {
    return;
  }

  GlStateSaver saved_state;

  if (!m_have_targets || w != m_w || h != m_h || msaa != m_msaa ||
      half_resolution != m_targets_half_res) {
    setup_targets(w, h, msaa);
  }

  // ---- work out how to turn the depth buffer back into distances ----
  // This mirrors make_new_cam_mat() in background_common.cpp, which builds the projection that
  // all of the level renderers use.
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
  const float depth_offset = fog * persp[3][2] / 8388608.f;

  // ndc_z = depth_a - depth_k / W, with W = |pzw| * camera z  =>  W = depth_k / (depth_a - ndc_z)
  const float depth_a = depth_scale / pzw;
  const float depth_k = depth_offset;
  const float to_meters = 1.f / (std::abs(pzw) * 4096.f);  // 4096 game units = 1 meter
  const float proj_x = std::abs(pc_pxx / pzw);
  const float proj_y = std::abs(pc_pyy * y_adjust / pzw);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // ---- 1. copy depth ----
  {
    auto p = prof.make_scoped_child("ssao-depth-copy");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    render_state->relight.scene_depth_tex = m_depth_tex;
    render_state->relight.scene_depth_valid = true;
    // and the colors, for bounce light (this also resolves MSAA)
    if (gi_enabled) {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_color_fbo);
      glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
  }

  // ---- 2. compute AO ----
  {
    auto p = prof.make_scoped_child("ssao-compute");
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[0]);
    glViewport(0, 0, m_ao_w, m_ao_h);
    auto& sh = render_state->shaders[ShaderId::SSAO];
    sh.activate();
    auto id = sh.id();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glUniform1i(glGetUniformLocation(id, "depth_tex"), 0);
    glUniform2f(glGetUniformLocation(id, "depth_texel"), 1.f / w, 1.f / h);
    glUniform3f(glGetUniformLocation(id, "depth_params"), depth_a, depth_k, to_meters);
    glUniform2f(glGetUniformLocation(id, "proj_scale"), proj_x, proj_y);
    glUniform1f(glGetUniformLocation(id, "radius"), radius_m);
    glUniform1f(glGetUniformLocation(id, "intensity"), intensity);
    glUniform1f(glGetUniformLocation(id, "bias"), bias_m);
    glUniform1f(glGetUniformLocation(id, "bias_per_m"), bias_per_m);
    glUniform1f(glGetUniformLocation(id, "max_distance"), max_distance_m);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_color_tex);
    glUniform1i(glGetUniformLocation(id, "color_tex"), 1);
    glUniform1i(glGetUniformLocation(id, "gi_enabled"), gi_enabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(id, "gi_radius"), gi_radius_m);

    // sun aware: rebuild world positions exactly like the level renderers' projection
    // (see make_new_cam_mat in background_common.cpp) and look them up in the sun shadow map
    const auto& rl = render_state->relight;
    const bool sun_on = sun_aware && rl.shadow_valid && render_state->has_pc_data;
    glUniform1i(glGetUniformLocation(id, "shadow_tex"), 3);
    glUniform1i(glGetUniformLocation(id, "sun_aware"), sun_on ? 1 : 0);
    if (sun_on) {
      float cam_rot[9];
      for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
          cam_rot[c * 3 + r] = render_state->camera_rot[c][r];
        }
      }
      glUniformMatrix3fv(glGetUniformLocation(id, "cam_rot"), 1, GL_FALSE, cam_rot);
      glUniform3f(glGetUniformLocation(id, "cam_trans"), render_state->camera_pos.x(),
                  render_state->camera_pos.y(), render_state->camera_pos.z());
      glUniform4f(glGetUniformLocation(id, "cam_params"), depth_k, depth_a, pc_pxx,
                  pc_pyy * y_adjust);
      glUniform1f(glGetUniformLocation(id, "cam_pzw"), pzw);
      glUniformMatrix4fv(glGetUniformLocation(id, "shadow_mat"), 1, GL_FALSE, rl.shadow_mat);
      glUniform1f(glGetUniformLocation(id, "shadow_normal_offset"), rl.shadow_normal_offset);
      float L[3];
      if (rl.use_game_sun && rl.game_sun_valid) {
        for (int i = 0; i < 3; i++) {
          L[i] = rl.game_sun_dir[i];
        }
      } else {
        const float az = rl.sun_azimuth_deg * 3.14159265f / 180.f;
        const float el = rl.sun_elevation_deg * 3.14159265f / 180.f;
        L[0] = std::cos(el) * std::sin(az);
        L[1] = std::sin(el);
        L[2] = std::cos(el) * std::cos(az);
      }
      glUniform3f(glGetUniformLocation(id, "sun_dir"), L[0], L[1], L[2]);
      glUniform1f(glGetUniformLocation(id, "ao_sun_protect"), ao_sun_protect);
      glUniform1f(glGetUniformLocation(id, "gi_sun_reduce"), gi_sun_reduce);
      glActiveTexture(GL_TEXTURE3);
      glBindTexture(GL_TEXTURE_2D, rl.shadow_tex);
      glActiveTexture(GL_TEXTURE0);
    }
    draw_quad();
    prof.add_draw_call();
    prof.add_tri(2);
  }

  // ---- 3. blur (horizontal into [1], vertical back into [0]) ----
  if (blur) {
    auto p = prof.make_scoped_child("ssao-blur");
    auto& sh = render_state->shaders[ShaderId::SSAO_BLUR];
    sh.activate();
    auto id = sh.id();
    glUniform1i(glGetUniformLocation(id, "ao_tex"), 0);
    glUniform1i(glGetUniformLocation(id, "gi_tex"), 1);
    for (int pass = 0; pass < 2; pass++) {
      const int src = pass == 0 ? 0 : 1;
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[1 - src]);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, m_gi_tex[src]);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_ao_tex[src]);
      if (pass == 0) {
        glUniform2f(glGetUniformLocation(id, "blur_step"), 1.f / m_ao_w, 0.f);
      } else {
        glUniform2f(glGetUniformLocation(id, "blur_step"), 0.f, 1.f / m_ao_h);
      }
      draw_quad();
      prof.add_draw_call();
      prof.add_tri(2);
    }
  }

  // ---- 4. apply to the image ----
  {
    auto p = prof.make_scoped_child("ssao-apply");
    glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
    glViewport(0, 0, w, h);
    auto& sh = render_state->shaders[ShaderId::SSAO_COMPOSITE];
    sh.activate();
    auto id = sh.id();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ao_tex[0]);
    glUniform1i(glGetUniformLocation(id, "ao_tex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gi_tex[0]);
    glUniform1i(glGetUniformLocation(id, "gi_tex"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_color_tex);
    glUniform1i(glGetUniformLocation(id, "color_tex"), 2);
    glUniform1f(glGetUniformLocation(id, "strength"), strength);
    glUniform1f(glGetUniformLocation(id, "max_distance"), max_distance_m);
    glUniform1i(glGetUniformLocation(id, "debug_view"), debug_view);
    glUniform1i(glGetUniformLocation(id, "gi_enabled"), gi_enabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(id, "gi_strength"), gi_strength);
    if (debug_view == 0) {
      // "dual source" blend: result = bounce_light + image * ao.
      // The shader's first output is the light to add, the second is the AO multiplier.
      // Destination alpha is left alone (the game uses it for blending later).
      glEnable(GL_BLEND);
      glBlendEquation(GL_FUNC_ADD);
      glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ZERO, GL_ONE);
    } else {
      // debug views overwrite the color so you can see the raw values
      glDisable(GL_BLEND);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    }
    draw_quad();
    prof.add_draw_call();
    prof.add_tri(2);
  }
  // GlStateSaver restores everything here
}

void SSAO::draw_debug_window() {
  ImGui::Checkbox("Enable SSAO", &enabled);
  ImGui::SliderFloat("Strength", &strength, 0.f, 1.f);
  ImGui::SliderFloat("Radius (m)", &radius_m, 0.1f, 5.f);
  ImGui::SliderFloat("Intensity", &intensity, 0.1f, 4.f);
  ImGui::SliderFloat("Bias (m)", &bias_m, 0.f, 0.3f);
  ImGui::SliderFloat("Bias per meter", &bias_per_m, 0.f, 0.03f, "%.4f");
  ImGui::SliderFloat("Max distance (m)", &max_distance_m, 5.f, 400.f);
  ImGui::Checkbox("Half resolution", &half_resolution);
  ImGui::SliderInt("Run after bucket (Jak 1)", &jak1_after_bucket, 0, 69);
  ImGui::Checkbox("Blur", &blur);
  ImGui::Separator();
  ImGui::Checkbox("Bounce light (GI)", &gi_enabled);
  ImGui::SliderFloat("GI strength", &gi_strength, 0.f, 10.f);
  ImGui::SliderFloat("GI radius (m)", &gi_radius_m, 0.5f, 10.f);
  ImGui::Checkbox("Sun aware (needs Relighting + Sun shadows)", &sun_aware);
  ImGui::SliderFloat("Less AO in sun", &ao_sun_protect, 0.f, 1.f);
  ImGui::SliderFloat("Less GI in sun", &gi_sun_reduce, 0.f, 1.f);
  ImGui::Separator();
  ImGui::RadioButton("Normal", &debug_view, 0);
  ImGui::SameLine();
  ImGui::RadioButton("AO only", &debug_view, 1);
  ImGui::SameLine();
  ImGui::RadioButton("Depth", &debug_view, 2);
  ImGui::SameLine();
  ImGui::RadioButton("GI only", &debug_view, 3);
}
