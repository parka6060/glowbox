#pragma once
#include <obs.h>
#include <graphics/vec4.h>

namespace glowbox {
enum class SourceProtection { None, Protected, Spill };
struct SourcePolicy {
    // receive: negative = normal, zero = protected, positive = spill amount.
    float receive = -1, color_match = 0, reactive = 0;
    float exposure = 1, saturation = 1;
    float produce_effects = 1, receive_effects = 1, receive_spotlight = 1, receive_rays = 1;
    float match_override = -1, reactive_override = -1;
    vec4 tint = {1, 1, 1, 1};
};
inline bool has_color_adjustment(const SourcePolicy &policy) {
    return policy.exposure != 1 || policy.saturation != 1 || policy.tint.x != 1 || policy.tint.y != 1 ||
           policy.tint.z != 1;
}
inline bool has_source_treatment(const SourcePolicy &policy) {
    return policy.color_match > 0 || policy.reactive > 0 || policy.match_override >= 0 ||
           policy.reactive_override >= 0 || has_color_adjustment(policy);
}
inline SourceProtection source_protection(const SourcePolicy &policy) {
    return policy.receive < 0    ? SourceProtection::None
           : policy.receive == 0 ? SourceProtection::Protected
                                 : SourceProtection::Spill;
}
SourceProtection source_protection(obs_source_t *source);
struct SceneFeatures {
    bool exclusions = false, screen = false, reactive_overrides = false;
    bool light = false, rays = false, treatment = false;
};
SceneFeatures scene_features(obs_scene_t *scene);
SourcePolicy source_policy(obs_source_t *source);
bool scene_has_source_treatment(obs_scene_t *scene);
bool scene_has_source_reactive(obs_scene_t *scene);
} // namespace glowbox
