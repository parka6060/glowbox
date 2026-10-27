#pragma once
#include <graphics/vec2.h>
#include <algorithm>
#include <cmath>

namespace glowbox {
// Camera translations are measured in scene-height units. OBS owns reference framing.
struct SceneView {
    float x = 0, y = 0;
};
inline float scene_view_coordinate(double value) {
    return std::isfinite(value) ? static_cast<float>(std::clamp(value, -.5, .5)) : 0.f;
}
inline bool scene_view_active(const SceneView &view) {
    return std::isfinite(view.x) && std::isfinite(view.y) && (view.x != 0.f || view.y != 0.f);
}
// The background is distance 1. The calibrated subject separation is not metres.
inline float subject_plane_distance(float separation) {
    if (!std::isfinite(separation)) separation = .25f;
    return 1.f - .45f * std::clamp(separation, 0.f, 1.f);
}
inline vec2 scene_plane_translation(const SceneView &view, float distance, float scene_height) {
    if (!scene_view_active(view) || !std::isfinite(distance) || distance <= 0.f ||
        !std::isfinite(scene_height) || scene_height < 0.f) return {};
    return {-view.x * scene_height / distance, -view.y * scene_height / distance};
}
inline vec2 scene_project_point(const SceneView &view, float distance, float scene_height, vec2 point) {
    if (!scene_view_active(view)) return point;
    const auto offset = scene_plane_translation(view, distance, scene_height);
    return {point.x + offset.x, point.y + offset.y};
}
}
