// (AI-assisted) see AutoExposure.h
#include "AutoExposure.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/log/log.h"

#include "third-party/imgui/imgui.h"

namespace {
const char* kVert = R"(#version 410 core
void main() {
  // one triangle covering the whole target
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// each output texel summarizes a block of the screen: (sum of weight * log(brightness), weight)
// for pixels that are not sky (the depth buffer is cleared to 0, so sky / nothing has depth 0)
const char* kFrag = R"(#version 410 core
uniform sampler2D color_tex;
uniform sampler2D depth_tex;
uniform vec2 grid;
out vec2 o;
void main() {
  vec2 cell = 1.0 / grid;
  vec2 base = floor(gl_FragCoord.xy) * cell;
  float s = 0.0;
  float n = 0.0;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      vec2 uv = base + (vec2(i, j) + 0.5) * 0.25 * cell;
      float d = texture(depth_tex, uv).r;
      if (d > 0.0) {
        vec3 c = texture(color_tex, uv).rgb;
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        // a bit more weight in the middle of the screen, where you look
        float w = mix(1.0, 0.35, clamp(length((uv - 0.5) * 2.0), 0.0, 1.0));
        s += w * log(max(l, 0.004));
        n += w;
      }
    }
  }
  o = vec2(s, n);
}
)";

GLuint compile_one(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    lg::error("AutoExposure: shader compile failed: {}", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

struct StateSaver {
  GLint fbo, read_fbo, draw_fbo, program, vao, active_tex, tex0, tex1, pbo;
  GLint viewport[4];
  GLboolean depth_test, blend, scissor, stencil, cull;
  StateSaver() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
  }
  static void set(GLenum cap, GLboolean on) {
    if (on) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
  ~StateSaver() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glUseProgram(program);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex0);
    glActiveTexture(active_tex);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    set(GL_DEPTH_TEST, depth_test);
    set(GL_BLEND, blend);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    set(GL_CULL_FACE, cull);
  }
};
}  // namespace

AutoExposure::~AutoExposure() {
  free();
  if (m_program) {
    glDeleteProgram(m_program);
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

bool AutoExposure::compile() {
  if (m_program) {
    return true;
  }
  if (m_compile_failed) {
    return false;
  }
  GLuint vs = compile_one(GL_VERTEX_SHADER, kVert);
  GLuint fs = compile_one(GL_FRAGMENT_SHADER, kFrag);
  if (!vs || !fs) {
    m_compile_failed = true;
    return false;
  }
  m_program = glCreateProgram();
  glAttachShader(m_program, vs);
  glAttachShader(m_program, fs);
  glLinkProgram(m_program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    lg::error("AutoExposure: shader link failed");
    glDeleteProgram(m_program);
    m_program = 0;
    m_compile_failed = true;
    return false;
  }
  glGenVertexArrays(1, &m_vao);
  return true;
}

void AutoExposure::free() {
  if (m_copy_fbo) {
    glDeleteFramebuffers(1, &m_copy_fbo);
    glDeleteTextures(1, &m_copy_color);
    glDeleteTextures(1, &m_copy_depth);
    glDeleteFramebuffers(1, &m_sum_fbo);
    glDeleteTextures(1, &m_sum_tex);
    glDeleteBuffers(2, m_pbo);
    m_copy_fbo = m_copy_color = m_copy_depth = m_sum_fbo = m_sum_tex = 0;
    m_pbo[0] = m_pbo[1] = 0;
  }
  m_pbo_full[0] = m_pbo_full[1] = false;
}

void AutoExposure::setup(int w, int h) {
  free();
  m_w = w;
  m_h = h;
  glActiveTexture(GL_TEXTURE0);
  // full size copy of the screen (also resolves anti-aliasing)
  glGenTextures(1, &m_copy_color);
  glBindTexture(GL_TEXTURE_2D, m_copy_color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenTextures(1, &m_copy_depth);
  glBindTexture(GL_TEXTURE_2D, m_copy_depth);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
               GL_UNSIGNED_INT_24_8, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &m_copy_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_copy_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_copy_color, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_copy_depth,
                         0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("AutoExposure: copy framebuffer incomplete");
  }

  // the small summary
  glGenTextures(1, &m_sum_tex);
  glBindTexture(GL_TEXTURE_2D, m_sum_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, kW, kH, 0, GL_RG, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &m_sum_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_sum_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sum_tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("AutoExposure: summary framebuffer incomplete");
  }

  glGenBuffers(2, m_pbo);
  for (int i = 0; i < 2; i++) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
    glBufferData(GL_PIXEL_PACK_BUFFER, kW * kH * 2 * sizeof(float), nullptr, GL_STREAM_READ);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void AutoExposure::update(SharedRenderState* render_state, GLuint render_fbo, int w, int h) {
  auto& rl = render_state->relight;
  if (!rl.auto_exposure || rl.amount <= 0.f || w <= 0 || h <= 0) {
    rl.auto_exposure_factor = 1.f;
    m_have_last = false;
    m_pbo_full[0] = m_pbo_full[1] = false;
    return;
  }
  StateSaver saved;
  if (!compile()) {
    rl.auto_exposure_factor = 1.f;
    return;
  }
  if (!m_copy_fbo || w != m_w || h != m_h) {
    setup(w, h);
  }

  // 1. copy the screen (color + depth) so it can be read as textures
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_copy_fbo);
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);

  // 2. summarize it into 128x72 blocks
  glBindFramebuffer(GL_FRAMEBUFFER, m_sum_fbo);
  glViewport(0, 0, kW, kH);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(m_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_copy_color);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_copy_depth);
  glUniform1i(glGetUniformLocation(m_program, "color_tex"), 0);
  glUniform1i(glGetUniformLocation(m_program, "depth_tex"), 1);
  glUniform2f(glGetUniformLocation(m_program, "grid"), (float)kW, (float)kH);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // 3. start reading it back into one buffer; use the one started last frame
  const int write = m_pbo_next;
  const int read = 1 - write;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[write]);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, kW, kH, GL_RG, GL_FLOAT, nullptr);
  m_pbo_full[write] = true;
  m_pbo_next = read;

  bool have_measurement = false;
  if (m_pbo_full[read]) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[read]);
    const float* data = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                       kW * kH * 2 * sizeof(float),
                                                       GL_MAP_READ_BIT);
    if (data) {
      double s = 0, n = 0;
      for (int i = 0; i < kW * kH; i++) {
        s += data[2 * i];
        n += data[2 * i + 1];
      }
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      // n is weighted; max weight per texel is 16
      m_coverage = (float)(n / (16.0 * kW * kH));
      if (n > 50.0) {
        m_measured = (float)std::exp(s / n);
        have_measurement = true;
      }
    }
    m_pbo_full[read] = false;
  }

  // 4. move the exposure towards the target, smoothly
  const auto now = std::chrono::steady_clock::now();
  float dt = m_have_last ? std::chrono::duration<float>(now - m_last).count() : 0.f;
  m_last = now;
  m_have_last = true;
  dt = std::min(dt, 0.25f);
  if (have_measurement && m_measured > 1e-4f) {
    const float applied = std::max(rl.auto_exposure_factor, 1e-3f);
    // brightness the scene would have without auto exposure
    const float unexposed = m_measured / applied;
    float desired = std::pow(rl.auto_exposure_target / std::max(unexposed, 1e-4f),
                             rl.auto_exposure_amount);
    desired = std::clamp(desired, rl.auto_exposure_min, rl.auto_exposure_max);
    const float k = 1.f - std::exp(-dt / std::max(rl.auto_exposure_speed_s, 0.05f));
    const float la = std::log(applied);
    const float ld = std::log(desired);
    rl.auto_exposure_factor = std::exp(la + (ld - la) * k);
  }
}

void AutoExposure::draw_debug_text(const SharedRenderState* render_state) const {
  const auto& rl = render_state->relight;
  ImGui::Text("Measured brightness %.3f (target %.2f), exposure x%.2f, world on screen %.0f%%",
              m_measured, rl.auto_exposure_target, rl.auto_exposure_factor, m_coverage * 100.f);
}
