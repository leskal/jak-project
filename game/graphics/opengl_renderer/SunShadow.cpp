#include "SunShadow.h"

// Sun shadows for the relighting mod. (AI-assisted)

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

void SunShadow::render(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  auto& rl = render_state->relight;
  rl.shadow_valid = false;
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
  const double H = half_size_m * 4096.0;  // 4096 game units = 1 meter
  const double D = depth_range_m * 4096.0;
  if (!m_fbo || m_tex_res != resolution) {
    setup_target();
  }
  const double texel = 2.0 * H / m_tex_res;
  auto dot3 = [](const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  const double cr = std::round(dot3(C, r) / texel) * texel;
  const double cu = std::round(dot3(C, u) / texel) * texel;
  const double cf = dot3(C, f);

  // world (game units) -> shadow map clip space, column major for OpenGL
  float clip[16];
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
  // world -> shadow map texture coordinates (0..1), for the level shaders
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 3; row++) {
      rl.shadow_mat[col * 4 + row] = 0.5f * clip[col * 4 + row] + (col == 3 ? 0.5f : 0.f);
    }
    rl.shadow_mat[col * 4 + 3] = clip[col * 4 + 3];
  }

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
