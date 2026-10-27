#pragma once
#include <obs-module.h>
#include "scene/scene_capture.hpp"
#include "scene/scene_depth.hpp"
#include "scene/lighting_geometry.hpp"
#include "sources/source_policy.hpp"
#include "sources/source_reference.hpp"
#include <graphics/vec4.h>
#include <graphics/matrix4.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace glowbox {
struct NativeRenderer;
struct LeafCanopy;

struct Compositor;
enum class LayerCaptureMode : int {
    None = 0,
    ProtectedColor = 1,
    ProtectionCoverage = 2,
    LightingCoverage = 3,
    SubjectCoverage = 4,
};
inline bool is_coverage_capture(LayerCaptureMode mode) {
    return mode == LayerCaptureMode::ProtectionCoverage || mode == LayerCaptureMode::LightingCoverage;
}
struct LayerView {
    obs_source_t *original = nullptr;
    obs_source_t *capture_filter = nullptr; // Scoped replay hook, never owns this filter.
    obs_source_t *capture_input =
        nullptr;                   // borrowed for replay, original is the retained source
    bool viewport_capture = false; // Scoped whole-group replay in the caller's exact viewport.
    gs_texrender_t *capture = nullptr;
    gs_effect_t *effect = nullptr;
    gs_texture_t *color_sample_texture = nullptr;
    float exposure_gain = 1, color_match_amount = 0, offset_x = 0, offset_y = 0;
    bool skip = false, rendering = false, mask = false, is_treated_subject = false;
    bool is_subject_plane = false;
    bool mask_capture_failed = false;
    SourceProtection protection = SourceProtection::None;
    SourcePolicy treatment;
    Compositor *owner = nullptr;
    LayerCaptureMode capture_mode = LayerCaptureMode::None;
    uint64_t capture_epoch = 0;
    Compositor *capture_owner = nullptr;
    uint32_t capture_width = 0, capture_height = 0;
    float capture_offset_x = 0, capture_offset_y = 0;
    bool capture_reference_context = false;
};
struct MirrorItem {
    int64_t id;
    obs_source_t *wrapper;
    obs_sceneitem_t *item;
    LayerView *view;
    obs_sceneitem_t *background_item;
    obs_sceneitem_t *mask_item;
    obs_sceneitem_t *protected_item = nullptr, *protection_mask_item = nullptr;
    obs_sceneitem_t *lighting_mask_item = nullptr;
    obs_transform_info transform = {};
    obs_sceneitem_crop crop = {};
    vec2 view_translation = {};
    bool configured = false;
};
struct LightingValues {
    float canopy_coverage = .49f;
    float canopy_curve = .72f;
    float canopy_forks = .7f;
    float canopy_clumping = .77f;
    float canopy_leaf_size = 1.49f;
    uint32_t canopy_seed = 1114847375u;
    float canopy_scale = .9f;
    float canopy_rotation = 119.f;
    int lightwash_type = 0;
    float lightwash_size = .5f;
    float lightwash_intensity = .2f;
    float lightwash_angle = 0;
    float lightwash_opacity = .25f;
    vec4 lightwash_color = {};
    float spotlight_amount = 0;
    float spotlight_x = .5f;
    float spotlight_y = .35f;
    float spotlight_size = .55f;
    float spotlight_softness = .25f;
    float spotlight_tilt = 0;
    float spotlight_direction = 0;
    float spotlight_falloff = 0;
    float spotlight_brightness = .35f;
    float spotlight_darkness = 6;
    vec4 spotlight_color = {1, .3f, .12f, 1};
    vec4 spotlight_light_color = {1, 1, 1, 1};
    float rays_amount = 0;
    float rays_detail = 0;
    float spotlight_surface = 1;
    float leaf_time_offset = 0;
    float ray_time_offset = 0;
    float light_type = 0;
    float ray_width = .35f;
    float ray_movement = .15f;
    float leaf_shadow_variation = .65f;
    float leaf_bottom_fade = .35f;
    float leaf_softness = .35f;
    float leaf_depth = .65f;
    float leaf_speed = .2f;
    float leaf_sway = .3f;
    float leaf_shimmer = .3f;
    float leaf_variation = .25f;
    float leaf_shadow_level = .5f;
    float leaf_edge_amount = .08f;
    vec4 leaf_shadow_color = {.44f, .61f, .82f, 1.f};
    vec4 leaf_tint = {.56f, .75f, .5f, 1.f};
    float light_pattern = 0.f;
    float pattern_size = 0.35f;
    float pattern_amount = 0.7f;
    float motion_drift = 0.35f;
    float motion_opposing = 0.5f;
    float motion_fading = 0.3f;
    float light_coverage = 0.f;
    float light_opacity = 1.f;
};
struct Compositor {
    LightingValues lighting;
    SceneView view;
    SceneCaptureCallback scene_capture_callback = nullptr;
    void *scene_capture_context = nullptr;
    bool scene_capture_failed = false;
    bool scene_capture_is_base_pass = false;
    bool scene_capture_project_view = false;
    NativeRenderer *native = nullptr;
    LeafCanopy *leaf_canopy = nullptr;

    uint64_t renderer_retry_after_ns = 0; // Render-thread retry backoff after failed initialization.
    // Saved rim choice and legacy contrast visibility, read by the properties UI.
    std::atomic<unsigned> rim_ui_state{7};
    bool native_requested = false, native_active = false;
    bool support_pending = false;
    bool support_failed = false;
    gs_texrender_t *rim_boundary_mask = nullptr;
    bool rim_boundary_ready = false;
    vec4 rim_boundary_uv = {1, 1, 0, 0};
    bool rim_enabled = false;
    bool rim_follow_source = false, rim_follow_ready = false;
    float rim_follow_angle = 0;
    int rim_type = 0, rim_blend = 7;
    float rim_size = .5f, rim_brightness = 0, rim_contrast = 0, rim_angle = 0, rim_opacity = 1;
    vec4 rim_color = {};
    obs_scene_t *mirror = nullptr, *background_mirror = nullptr, *mask_mirror = nullptr;
    obs_scene_t *protected_mirror = nullptr, *protection_mask_mirror = nullptr;
    obs_scene_t *lighting_mask_mirror = nullptr;
    bool has_exclusions = false, has_spill = false, exclusion_failure_logged = false;
    gs_texrender_t *protected_color = nullptr, *protection_mask = nullptr;
    gs_texrender_t *exclusion_input = nullptr, *exclusion_emission = nullptr;
    gs_texrender_t *final_capture = nullptr, *final_scratch = nullptr;
    gs_texrender_t *exclusion_subject = nullptr;
    std::string reactive_source_id;
    float reactive_strength = 0, reactive_elapsed = 1.f / 60.f;
    bool reactive_ready = false, reactive_background = false;
    int reactive_index = 0;
    ReactiveGeometry reactive_geometry;
    bool reactive_geometry_override = false, reactive_spatial_ready = false;
    gs_texrender_t *reactive_spatial[2] = {};
    gs_texrender_t *reactive_capture = nullptr, *reactive_history[2] = {}, *reactive_treatment = nullptr;
    gs_texture_t *subject_texture = nullptr;
    std::vector<MirrorItem> items;
    obs_data_t *layer_settings = nullptr;
    float subject_exposure_gain = 1, background_exposure_gain = 1;
    bool layered = false, rendering_layers = false, wants_layers = false;
    bool mask_failure_logged = false, sample_failure_logged = false;
    obs_source_t *source = nullptr;
    gs_effect_t *effect = nullptr;
    bool owns_effect = true;
    gs_texrender_t *base = nullptr, *layout_render = nullptr;
    gs_texture_t *lightwash_texture = nullptr;
    gs_texrender_t *color_sample_history = nullptr;
    gs_texrender_t *subject_mask = nullptr;
    gs_texture_t *screen_texture = nullptr;
    gs_texrender_t *color_sample = nullptr, *reference_render = nullptr, *background_render = nullptr;
    obs_weak_source_t *reference = nullptr;
    gs_texture_t *soft = nullptr, *light = nullptr;
    float diffusion = 0, glow = 0, radius = 2, threshold = .75f;
    float exposure_gain = 1, contrast = 1, saturation = 1;
    int divisor = 4, levels = 3, blend = 2;
    int screen_mode = 2;
    float screen_intensity = .3f, screen_opacity = 1, screen_contrast = 0;
    float brightness = 0, color_match_strength = 1;

    vec4 color_filter = {};
    float color_sample_elapsed = .1f;
    bool sampling_reference = false;
    bool processed = false;
    uint32_t width = 0, height = 0;
    float follow_amount = 0;
    bool follow_ready = false;
    int follow_index = 0;
    gs_texrender_t *follow_history[2] = {};
    Compositor *follow_owner = nullptr; // Move retains the owning source while this treatment exists.
    float follow_darken = .5f, follow_brighten = .2f;
    float follow_sensitivity = 1; // Preserves the response of saved Amount settings.
    bool exposure_background = false;
    float reactive_smoothing = 1;
    bool has_screen_overrides = false;
    bool has_reactive_overrides = false;
    bool has_spotlight_overrides = false;
    gs_texrender_t *source_spotlight_mask = nullptr;
    gs_texrender_t *source_emission = nullptr;

    float subject_depth = .25f, shadow_amount = 0;
    float shadow_size = 1, shadow_softness = .2f, shadow_distance = .035f, shadow_angle = 135;
    int shadow_follow = 1, rim_follow_light = 0;
    vec2 shadow_pivot = {.5f, .5f};
    gs_texrender_t *shadow_field[2] = {};
    bool shadow_field_ready = false;

    bool rays_ready = false, light_conflict_logged = false;
    gs_texrender_t *rays_field = nullptr;
    bool bounded_reactive = false;  // Use the bounded SDR response for reactive lighting.
    uint64_t composition_epoch = 0; // One immutable raw-source snapshot per composition attempt.
};

// Parallel-plane spotlight geometry: one scale for the footprint and shadow.
inline float spotlight_plane_scale(const Compositor *f) {
    return subject_plane_distance(f->subject_depth);
}
// Light origin projected onto the wall, shared by shadow projection and rim aiming.
inline vec2 spotlight_emitter_position(const Compositor *f, float aspect) {
    const float tilt = f->lighting.spotlight_tilt * .01745329252f;
    const float direction = f->lighting.spotlight_direction * .01745329252f;
    const float shift = f->lighting.light_type > .5f
                            ? 4.f
                            : 2.f * std::max(.01f, f->lighting.spotlight_size) * std::tan(tilt);
    return {f->lighting.spotlight_x + std::sin(direction) * shift / std::max(aspect, .00001f),
            f->lighting.spotlight_y - std::cos(direction) * shift};
}
inline float effective_rim_angle(const Compositor *f) {
    if (!f->rim_follow_source || !f->rim_follow_ready)
        return f->rim_angle;
    const float weight = f->rim_follow_light == 2 ? f->lighting.spotlight_amount : 1.f;
    return f->rim_angle + std::remainder(f->rim_follow_angle - f->rim_angle, 360.f) * weight;
}
bool prepare_rim_boundary(Compositor *filter);
void bind_spotlight(Compositor *, gs_effect_t *, gs_texture_t *fallback);
bool prepare_spotlight_shadow(Compositor *, gs_texture_t *);
void load_compositor_settings(void *data, obs_data_t *settings);
void save_compositor_settings(void *data, obs_data_t *settings);
void update_settings(void *data, obs_data_t *settings);
void default_settings(obs_data_t *settings);
obs_properties_t *filter_properties(void *data);
void render_filter(void *data, gs_effect_t *effect);
bool compositor_neutral(const Compositor *);
void tick_filter(void *data, float seconds);
double light_animation_seconds();
bool scene_light_present(obs_scene_t *);
bool resolve_scene_light(Compositor *, obs_scene_t *);
bool prepare_light_rays(Compositor *, gs_texture_t *);
// Restore compositor controls after borrowing the Scene Light.
struct SceneLightScope {
    Compositor *filter;
    LightingValues lighting;
    bool layers;
    int shadow_follow;
    explicit SceneLightScope(Compositor *, obs_scene_t *);
    ~SceneLightScope();
};
void bind_source_reactive(Compositor *, gs_texture_t *fallback);
void bind_follow_brightness(Compositor *, gs_effect_t *, gs_texture_t *fallback);
bool copy_follow_brightness(Compositor *, Compositor *);
bool reactive_requested(const Compositor *filter);
void update_reactive_source(Compositor *filter, obs_data_t *settings);
void release_reactive_buffers(Compositor *filter);
bool sample_reactive_light(Compositor *filter, gs_texture_t *fallback);
gs_texture_t *apply_reactive_light(Compositor *filter, gs_texture_t *input);
obs_source_t *create_layer_source(obs_source_t *source, gs_effect_t *effect, LayerView **view);
std::string subject_setting_key(obs_sceneitem_t *item);
void prepare_subject_settings(Compositor *f, obs_scene_t *scene);
void clear_mirror(Compositor *filter);
bool sync_mirror(Compositor *filter, obs_scene_t *scene);
void configure_layers(Compositor *filter, bool background_only);
void draw_compositor_scene(Compositor *filter, obs_scene_t *scene);
void draw_mask_scene(Compositor *filter, obs_scene_t *scene, LayerCaptureMode mode);
bool sample_background_color(Compositor *filter);
bool copy_color_match_history(Compositor *from, Compositor *to);
bool sample_color_reference(Compositor *filter, gs_texture_t *fallback);
gs_eparam_t *shader_parameter(Compositor *filter, const char *name);
bool render_pass(Compositor *filter, gs_texrender_t *destination, gs_texture_t *input, uint32_t width,
                 uint32_t height, const char *technique);
bool render_effect_pipeline(Compositor *filter, gs_texture_t *original);
} // namespace glowbox
