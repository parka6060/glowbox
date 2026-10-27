#pragma once
#include <graphics/graphics.h>

namespace glowbox {
struct Compositor;
bool prepare_source_spotlight_mask(Compositor *);
bool prepare_source_reactive_mask(Compositor *);
gs_texture_t *source_screen_emission(Compositor *, gs_texture_t *);
void bind_source_screen(Compositor *);
bool prepare_exclusions(Compositor *filter);
void release_exclusion_buffers(Compositor *filter);
gs_texture_t *exclusion_emission_input(Compositor *filter, gs_texture_t *input);
void draw_exclusion_output(Compositor *filter, gs_texture_t *original, gs_texture_t *processed);
}
