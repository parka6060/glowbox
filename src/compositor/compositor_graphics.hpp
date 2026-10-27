#pragma once
#include <obs.h>

namespace glowbox {
struct Compositor;

// Setup and teardown enter the graphics context themselves. A borrowed effect
// stays owned by the source compositor for the lifetime of its transition copy.
void prepare_compositor_graphics(Compositor *, gs_effect_t *borrowed_effect);
void release_compositor_graphics(Compositor *);
// Readiness checks run in the caller's graphics context.
bool ensure_support_effect(Compositor *);
}
