#pragma once
#include <graphics/graphics.h>
namespace glowbox {
struct Compositor;
struct NativeRenderer;
bool prepare_native_renderer(Compositor *filter);
void destroy_native_renderer(Compositor *filter);
void release_native_targets(Compositor *filter);
bool render_native_pipeline(Compositor *filter, gs_texture_t *original_texture);
gs_texture_t *native_spill_texture(Compositor *filter);
void draw_native_output(Compositor *filter, gs_texture_t *original_texture);
}
