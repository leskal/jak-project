#include "Bloom.h"

// (AI-assisted) see Bloom.h

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

namespace {

// full screen triangle, no vertex buffer needed
const char* kVert = R"(#version 410 core
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// downsample (dual filter); the first step also keeps only the bright part
const char* kDown = R"(#version 410 core
in vec2 uv;
out vec4 color;
uniform sampler2D src;
uniform vec2 texel;      // one texel of src
uniform int prefilter;   // 1 on the first step
uniform float threshold;
uniform float knee;
vec3 tap(vec2 p) {
  vec3 c = texture(src, p).rgb;
  if (prefilter == 1) {
    float L = max(c.r, max(c.g, c.b));
    float soft = clamp(L - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    c *= max(soft, L - threshold) / max(L, 1e-4);   // brightness above the threshold, same hue
  }
  return c;
}
void main() {
  vec3 s = tap(uv) * 4.0;
  s += tap(uv - texel);
  s += tap(uv + texel);
  s += tap(uv + vec2(texel.x, -texel.y));
  s += tap(uv - vec2(texel.x, -texel.y));
  color = vec4(s / 8.0, 1.0);
}
)";

// upsample (dual filter), scaled; added onto the destination with blending
const char* kUp = R"(#version 410 core
in vec2 uv;
out vec4 color;
uniform sampler2D src;
uniform vec2 texel;      // one texel of src
uniform float scale;
void main() {
  vec3 s = texture(src, uv + vec2(-2.0 * texel.x, 0.0)).rgb;
  s += texture(src, uv + vec2(-texel.x, texel.y)).rgb * 2.0;
  s += texture(src, uv + vec2(0.0, 2.0 * texel.y)).rgb;
  s += texture(src, uv + vec2(texel.x, texel.y)).rgb * 2.0;
  s += texture(src, uv + vec2(2.0 * texel.x, 0.0)).rgb;
  s += texture(src, uv + vec2(texel.x, -texel.y)).rgb * 2.0;
  s += texture(src, uv + vec2(0.0, -2.0 * texel.y)).rgb;
  s += texture(src, uv + vec2(-texel.x, -texel.y)).rgb * 2.0;
  color = vec4(s / 12.0 * scale, 1.0);
}
)";

GLuint compile_stage(GLenum kind, const char* src) {
  GLuint s = glCreateShader(kind);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    lg::error("Bloom: shader compile failed: {}", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link_program(const char* vs_src, const char* fs_src) {
  GLuint vs = compile_stage(GL_VERTEX_SHADER, vs_src);
  GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) {
    if (vs) {
      glDeleteShader(vs);
    }
    if (fs) {
      glDeleteShader(fs);
    }
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    lg::error("Bloom: shader link failed: {}", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// saves the GL state this pass changes, and puts it back
struct StateSaver {
  GLint read_fbo, draw_fbo, program, vao, active_tex, tex0;
  GLint viewport[4];
  GLboolean blend, depth_test, scissor, stencil, depth_mask, cull;
  GLboolean color_mask[4];
  GLint src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
  StateSaver() {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_VIEWPORT, viewport);
    blend = glIsEnabled(GL_BLEND);
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
  }
  ~StateSaver() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glUseProgram(program);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex0);
    glActiveTexture(active_tex);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    set(GL_BLEND, blend);
    set(GL_DEPTH_TEST, depth_test);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    set(GL_CULL_FACE, cull);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
    glBlendEquationSeparate(eq_rgb, eq_a);
  }
  static void set(GLenum cap, GLboolean on) {
    if (on) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
};

GLuint make_target(int w, int h, GLenum internal, GLuint* tex) {
  glGenTextures(1, tex);
  glBindTexture(GL_TEXTURE_2D, *tex);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA,
               internal == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
  return fbo;
}

}  // namespace

Bloom::~Bloom() {
  free_targets();
  if (m_down_prog) {
    glDeleteProgram(m_down_prog);
  }
  if (m_up_prog) {
    glDeleteProgram(m_up_prog);
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

bool Bloom::setup_programs() {
  if (m_down_prog && m_up_prog) {
    return true;
  }
  if (m_compile_failed) {
    return false;
  }
  m_down_prog = link_program(kVert, kDown);
  m_up_prog = link_program(kVert, kUp);
  if (!m_down_prog || !m_up_prog) {
    m_compile_failed = true;
    return false;
  }
  glGenVertexArrays(1, &m_vao);
  return true;
}

void Bloom::free_targets() {
  if (m_src_fbo) {
    glDeleteFramebuffers(1, &m_src_fbo);
    glDeleteTextures(1, &m_src_tex);
    m_src_fbo = m_src_tex = 0;
  }
  for (int i = 0; i < kLevels; i++) {
    if (m_fbo[i]) {
      glDeleteFramebuffers(1, &m_fbo[i]);
      glDeleteTextures(1, &m_tex[i]);
      m_fbo[i] = m_tex[i] = 0;
    }
  }
  m_w = m_h = 0;
}

void Bloom::setup_targets(int w, int h) {
  free_targets();
  m_src_fbo = make_target(w, h, GL_RGBA8, &m_src_tex);
  int lw = w, lh = h;
  for (int i = 0; i < kLevels; i++) {
    lw = std::max(1, lw / 2);
    lh = std::max(1, lh / 2);
    m_lw[i] = lw;
    m_lh[i] = lh;
    m_fbo[i] = make_target(lw, lh, GL_RGBA16F, &m_tex[i]);
  }
  m_w = w;
  m_h = h;
}

void Bloom::render(SharedRenderState* render_state) {
  const auto& rl = render_state->relight;
  if (!rl.bloom_on || rl.bloom_strength <= 0.f || !render_state->has_pc_data) {
    return;
  }
  const int w = render_state->render_fb_w;
  const int h = render_state->render_fb_h;
  if (w < 16 || h < 16) {
    return;
  }
  StateSaver saved;
  if (!setup_programs()) {
    return;
  }
  if (w != m_w || h != m_h) {
    setup_targets(w, h);
  }
  const int levels = std::clamp(rl.bloom_size, 2, kLevels);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindVertexArray(m_vao);
  glActiveTexture(GL_TEXTURE0);

  // 1. copy the image (resolves MSAA)
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_src_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // 2. + 3. bright part, then smaller and smaller
  glDisable(GL_BLEND);
  glUseProgram(m_down_prog);
  glUniform1i(glGetUniformLocation(m_down_prog, "src"), 0);
  glUniform1f(glGetUniformLocation(m_down_prog, "threshold"), rl.bloom_threshold);
  glUniform1f(glGetUniformLocation(m_down_prog, "knee"), std::max((1.f - rl.bloom_threshold) * 0.5f, 0.02f));
  for (int i = 0; i < levels; i++) {
    const GLuint src = i == 0 ? m_src_tex : m_tex[i - 1];
    const int sw = i == 0 ? w : m_lw[i - 1];
    const int sh = i == 0 ? h : m_lh[i - 1];
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
    glViewport(0, 0, m_lw[i], m_lh[i]);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform2f(glGetUniformLocation(m_down_prog, "texel"), 1.f / sw, 1.f / sh);
    glUniform1i(glGetUniformLocation(m_down_prog, "prefilter"), i == 0 ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  // ... and back up, adding each level onto the one above
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
  glUseProgram(m_up_prog);
  glUniform1i(glGetUniformLocation(m_up_prog, "src"), 0);
  glUniform1f(glGetUniformLocation(m_up_prog, "scale"), 1.f);
  for (int i = levels - 1; i >= 1; i--) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i - 1]);
    glViewport(0, 0, m_lw[i - 1], m_lh[i - 1]);
    glBindTexture(GL_TEXTURE_2D, m_tex[i]);
    glUniform2f(glGetUniformLocation(m_up_prog, "texel"), 1.f / m_lw[i], 1.f / m_lh[i]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  // 4. add the glow onto the image (the levels were summed, so divide by their count)
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glViewport(0, 0, w, h);
  glBindTexture(GL_TEXTURE_2D, m_tex[0]);
  glUniform2f(glGetUniformLocation(m_up_prog, "texel"), 1.f / m_lw[0], 1.f / m_lh[0]);
  glUniform1f(glGetUniformLocation(m_up_prog, "scale"), rl.bloom_strength / (float)levels);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}
