#pragma once

// Saving and loading the lighting mod's settings (SSAO / GI, relighting, sun shadows) to a JSON
// file in the user's OpenGOAL config folder. (AI-assisted)

#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SSAO.h"
#include "game/graphics/opengl_renderer/SunShadow.h"

std::string lighting_settings_path();
// returns true if a settings file was found and read
bool lighting_settings_load(SSAO& ssao, SunShadow& sun, SharedRenderState::RelightSettings& rl);
bool lighting_settings_save(const SSAO& ssao,
                            const SunShadow& sun,
                            const SharedRenderState::RelightSettings& rl);
