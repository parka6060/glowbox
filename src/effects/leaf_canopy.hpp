#pragma once
#include <memory>
#include <obs-module.h>

namespace glowbox {
struct Compositor;
struct CanopyJob;
struct LeafCanopy;
// Shared CPU geometry jobs contain no OBS objects. Hidden scenes can prepare
// their saved canopy before Move first renders the destination.
std::shared_ptr<CanopyJob> request_canopy(obs_data_t *settings);
bool same_canopy(const Compositor *, const Compositor *);
void prepare_leaf_canopy(Compositor *);
void bind_leaf_canopy(const Compositor *, gs_effect_t *, gs_texture_t *fallback);
void destroy_leaf_canopy(Compositor *); // Graphics context held.
void stop_canopy_worker(); // Stop the worker at module unload, outside graphics.
}
