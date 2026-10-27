#pragma once
#include <string>
#include <stdint.h>
namespace glowbox {
enum class ShaderPreparation { Idle, Pending, Ready, Failed, Unsupported };
// Requires the OBS graphics context. The worker owns a separate D3D context.
ShaderPreparation request_shader_preparation(const std::string &support);
ShaderPreparation shader_preparation_state();
std::string native_effect_source();
void stop_shader_preparation();
// Decode CPU image data at module startup, before source rendering.
void load_loading_images();
void release_shader_preparation_badge();
void draw_shader_preparation(uint32_t width, uint32_t height, bool attachment_failed = false);
} // namespace glowbox
