#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/opengl_renderer/Profiler.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"

struct Fbo;

struct LevelVis {
  bool valid = false;
  u8 data[2048];
};

class EyeRenderer;
/*!
 * The main renderer will contain a single SharedRenderState that's passed to all bucket renderers.
 * This allows bucket renders to share textures and shaders.
 */
struct SharedRenderState {
  explicit SharedRenderState(std::shared_ptr<TexturePool> _texture_pool,
                             std::shared_ptr<Loader> _loader,
                             GameVersion _version)
      : shaders(_version), texture_pool(_texture_pool), loader(_loader), version(_version) {}
  ShaderLibrary shaders;
  std::shared_ptr<TexturePool> texture_pool;
  std::shared_ptr<Loader> loader;

  u32 buckets_base = 0;  // address of buckets array.
  u32 next_bucket = 0;   // address of next bucket that we haven't started rendering in buckets
  u32 default_regs_buffer = 0;  // address of the default regs chain.

  void* ee_main_memory = nullptr;
  u32 offset_of_s7;

  bool use_sky_cpu = true;
  bool use_occlusion_culling = true;
  math::Vector<u8, 4> fog_color = math::Vector<u8, 4>{0, 0, 0, 0};
  float fog_intensity = 1.f;
  bool no_multidraw = false;

  void reset();
  bool has_pc_data = false;

  // limit is arbitrary so let's go ham in case we want more levels in the future
  LevelVis occlusion_vis[32];

  math::Vector4f camera_planes[4];

  // including transformation, rotation, perspective
  math::Vector4f camera_matrix[4];
  math::Vector4f camera_hvdf_off;
  math::Vector4f camera_fog;
  math::Vector4f camera_pos;
  // perspective matrix from the game, used by the SSAO mod to rebuild distances (AI-assisted)
  math::Vector4f camera_persp[4];
  // camera rotation (camera space = rot * (world - camera_pos)), for the relighting mod
  math::Vector4f camera_rot[4];
  // (AI-assisted) glow light mod: called by the sprite renderer with 0 just before the particles
  // and 1 right after them (before the HUD)
  std::function<void(int)> sprite_hook;

  // (AI-assisted) point lights mod: glowing (additive) particles seen this frame, by Sprite3
  struct ParticleLightSample {
    float x, y, z;     // world position (game units)
    float color[3];    // color * alpha * area (m^2): how much light it gives
  };
  bool collect_particle_lights = false;
  std::vector<ParticleLightSample> particle_light_samples;
  // point lights for the shaders (filled by PointLights each frame)
  struct PointLightsGpu {
    int count = 0;
    float pos_radius[16 * 4] = {};  // xyz = world position, w = reach (game units)
    float color[16 * 4] = {};
  } point_lights;

  // (AI-assisted) relighting mod: real sun lighting on level geometry
  struct RelightSettings {
    float amount = 1.0f;            // 0 = original baked lighting, 1 = fully relit
    float sun_azimuth_deg = 45.f;   // compass direction of the sun
    float sun_elevation_deg = 50.f; // height of the sun above the horizon
    float sun_intensity = 2.2f;
    float sun_color[3] = {1.f, 0.95f, 0.85f};
    float ambient = 0.8f;           // light that comes from everywhere (sky)
    int debug = 0;                  // 1 = show normals as colors

    // Physical sun mode: surfaces are lit from their own (texture) color by a warm sun and a
    // direction-dependent sky (blue from above, warm bounce from the ground), like the point
    // lights, instead of scaling the game's painted lighting.
    int mode = 1;                   // 0 = classic, 1 = physical
    bool auto_sun_color = true;     // sun / sky colors follow the sun height (atmosphere)
    bool sky_from_game = true;      // (AI-assisted) sky light takes the color of the game's sky
    // (AI-assisted) auto exposure: measures the screen and adapts, like a camera / your eyes
    bool auto_exposure = true;
    float auto_exposure_amount = 0.8f;   // 0 = none, 1 = always reach the target
    float auto_exposure_target = 0.25f;  // average brightness to aim for
    float auto_exposure_speed_s = 1.5f;  // how fast it adapts (seconds)
    float auto_exposure_min = 0.4f;
    float auto_exposure_max = 2.5f;
    float auto_exposure_factor = 1.f;    // (runtime) current auto exposure
    float sky_color[3] = {0.45f, 0.55f, 0.75f};
    float ground_color[3] = {0.35f, 0.30f, 0.24f};
    float exposure = 0.8f;
    float keep_painted = 0.35f;     // how much of the game's painted shading to keep (0..1)
    float ground_smooth = 1.0f;     // (AI-assisted) hide triangle patches on ground under a low sun

    // sun taken from the game (the light the game uses on Jak, see Merc2.cpp)
    bool use_game_sun = true;       // use the game's sun direction instead of the sliders
    bool use_game_colors = false;   // also use the game's sun and ambient colors
    // what the game gave Jak last frame (raw, can jump: indoors the game uses lamp lights)
    bool game_sun_raw_valid = false;
    float game_sun_raw_dir[3] = {0, 1, 0};
    float game_sun_raw_color[3] = {1, 1, 1};
    float game_raw_ambient[3] = {0.5f, 0.5f, 0.5f};
    // filtered version used for lighting (see SunShadow::update_game_sun):
    // follows slow changes (time of day), ignores sudden jumps (going indoors) unless they last
    bool game_sun_valid = false;
    float game_sun_dir[3] = {0, 1, 0};
    float game_sun_color[3] = {1, 1, 1};
    float game_ambient[3] = {0.5f, 0.5f, 0.5f};
    float sun_smooth_s = 2.f;        // how fast the sun follows slow changes (seconds)
    float sun_jump_accept_s = 30.f;  // a sudden change is accepted only after it lasts this long

    // sun shadows (filled in each frame by SunShadow)
    bool shadow_valid = false;
    GLuint shadow_tex = 0;
    float shadow_mat[16] = {};  // world -> shadow map texture coordinates
    float shadow_strength = 0.f;
    float shadow_normal_offset = 0.f;  // game units
    float shadow_clip[16] = {};        // world -> shadow map clip space (for drawing into it)
    // (AI-assisted) far shadow map (bigger area, less sharp)
    // (AI-assisted) point light shadows (cube map array, one cube per shadowed light)
    bool pl_shadow_valid = false;
    GLuint pl_shadow_tex = 0;
    int pl_shadow_layer[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    float pl_shadow_res = 512.f;
    bool far_shadow_valid = false;
    GLuint far_shadow_tex = 0;
    float far_shadow_mat[16] = {};
    float far_shadow_normal_offset = 0.f;

    // characters (filled in each frame by SunShadow, used by Merc2)
    bool chars_receive = true;     // characters get darker in sun shadows
    bool char_shadow_write = false;  // characters should be drawn into char_shadow_fbo
    GLuint char_shadow_fbo = 0;
    int char_shadow_res = 0;
    // character shadows drawn last frame, read by the level shaders
    bool char_shadow_valid = false;
    GLuint char_shadow_tex = 0;
    float char_shadow_mat[16] = {};
  } relight;

  EyeRenderer* eye_renderer = nullptr;

  std::string load_status_debug;

  // Information for renderers that need to read framebuffers:
  // Most renderers can just use the framebuffer/glViewport set up by OpenGLRenderer, but special
  // effects like sprite distort that read the framebuffer will need to know the details of the
  // framebuffer setup.

  // the framebuffer that bucket renderers should render to.
  int render_fb_x = 0;
  int render_fb_y = 0;
  int render_fb_w = 0;
  int render_fb_h = 0;
  GLuint render_fb = -1;

  // the region within that framebuffer to draw to.
  int draw_region_w = 0;
  int draw_region_h = 0;
  int draw_offset_x = 0;
  int draw_offset_y = 0;

  int bucket_for_vis_copy = 0;
  int num_vis_to_copy = 0;
  GameVersion version;
  u64 frame_idx = 0;

  bool stencil_dirty = false;
};

/*!
 * Interface for bucket renders. Each bucket will have its own BucketRenderer.
 */
class BucketRenderer {
 public:
  BucketRenderer(const std::string& name, int my_id) : m_name(name), m_my_id(my_id) {}
  virtual void render(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof) = 0;
  std::string name() const;
  std::string name_and_id() const;
  virtual ~BucketRenderer() = default;
  bool& enabled() { return m_enabled; }
  virtual bool empty() const { return false; }
  virtual void draw_debug_window() = 0;
  virtual void init_shaders(ShaderLibrary&) {}
  virtual void init_textures(TexturePool&, GameVersion) {}

 protected:
  std::string m_name;
  int m_my_id;
  bool m_enabled = true;
};

class RenderMux : public BucketRenderer {
 public:
  RenderMux(const std::string& name,
            int my_id,
            std::vector<std::unique_ptr<BucketRenderer>> renderers);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;
  void init_shaders(ShaderLibrary&) override;
  void init_textures(TexturePool&, GameVersion) override;
  void set_idx(u32 i) { m_render_idx = i; };

 private:
  std::vector<std::unique_ptr<BucketRenderer>> m_renderers;
  int m_render_idx = 0;
  std::vector<std::string> m_name_strs;
  std::vector<const char*> m_name_str_ptrs;
};

/*!
 * Renderer that makes sure the bucket is empty and ignores it.
 */
class EmptyBucketRenderer : public BucketRenderer {
 public:
  EmptyBucketRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};

class SkipRenderer : public BucketRenderer {
 public:
  SkipRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};

/*!
 * Renderer that ignores and prints all DMA transfers.
 */
class PrintRenderer : public BucketRenderer {
 public:
  PrintRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};
