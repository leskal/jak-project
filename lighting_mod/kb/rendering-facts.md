# OpenGOAL renderer facts this mod relies on (AI-assisted)

- Shaders are read at startup from `game/graphics/opengl_renderer/shaders/`: a shader-only change
  needs a restart, not a rebuild.
- Jak 1 buckets: 55 MERC_AFTER_PRIS (Jak drawn) -> SSAO runs here; 58 / 61 merc water;
  63 OCEAN_NEAR; 66 SPRITE. Ocean mid/far is bucket 4 (before the level).
- Projection (background_common make_new_cam_mat / SSAO.cpp): W = k / (a - ndc_z),
  camera = (-ndc.x W / pxx, -ndc.y W / pyy_adj, -W / pzw), world = transpose(cam_rot) camera + cam_trans.
- Merc: camera-space normals and lights; world = transpose(cam_rot) x camera. The model colour
  is in the vertex rgba (albedo = texture x rgba x 2).
- Level vertex colours: 8 palette slots blended by the game's weights (`itimes`, 64 = 1.0);
  128 = 1.0 colour.
- Loaded levels keep their unpacked vertices and texture pixels in memory (`LevelData::level`).
- Particle colours: 128 = 1.0; additive blend is a = SOURCE, b = ZERO, d = DEST.
