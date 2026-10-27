#pragma once
#include <obs.h>
#include <cstddef>

namespace glowbox {
struct Compositor;

// A provider replays a scene layout for both color and participation-mask passes.

struct SceneCaptureLayer {
    obs_source_t *original;
    obs_source_t *wrapper;
    bool visible;
};
using SceneCaptureCallback = bool (*)(void *context, const SceneCaptureLayer *, size_t layer_count);
// Providers own layout by default. project_view is only for validated direct-input
// Move replay, where wrappers draw in final scene space. Ordinary composition
// already projects its private scene placements and must not project wrappers twice.
bool render_with_scene_capture(Compositor *, SceneCaptureCallback, void *context, bool project_view=false);
}
