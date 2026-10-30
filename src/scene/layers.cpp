#include "plugin_registration.hpp"
#include "compositor/compositor.hpp"

namespace glowbox {
static thread_local obs_source_t *layer_seed = nullptr;
static thread_local gs_effect_t *layer_effect_seed = nullptr;
static thread_local LayerView *layer_created = nullptr;
static const char *layer_name(void *) {
    return "Glowbox internal layer";
}
static void *layer_create(obs_data_t *, obs_source_t *) {
    auto *v = new LayerView;
    v->original = obs_source_get_ref(layer_seed);
    obs_enter_graphics();
    // The layer borrows the effect.
    v->effect = layer_effect_seed;
    obs_leave_graphics();
    layer_created = v;
    return v;
}
static void layer_destroy(void *data) {
    auto *v = (LayerView *)data;
    obs_enter_graphics();
    gs_texrender_destroy(v->capture);
    obs_leave_graphics();
    obs_source_release(v->transition);
    obs_source_release(v->original);
    delete v;
}
static obs_source_t *layer_input(LayerView *view) {
    if (view->capture_input)
        return view->capture_input;
    return view->transition ? view->transition : view->original;
}
static uint32_t layer_width(void *data) {
    return obs_source_get_width(layer_input(static_cast<LayerView *>(data)));
}
static uint32_t layer_height(void *data) {
    return obs_source_get_height(layer_input(static_cast<LayerView *>(data)));
}
struct LayerMatrixScope {
    LayerMatrixScope() {
        gs_matrix_push();
    }
    ~LayerMatrixScope() {
        gs_matrix_pop();
    }
};
static void render_layer_input(LayerView *v) {
    if (v->capture_filter)
        obs_source_skip_video_filter(v->capture_filter);
    else
        obs_source_video_render(layer_input(v));
}
static void layer_render(void *data, gs_effect_t *) {
    auto *v = (LayerView *)data;
    if (v->skip || v->rendering || !v->original)
        return;
    v->rendering = true;
    LayerMatrixScope matrix_scope;
    gs_matrix_translate3f(v->offset_x, v->offset_y, 0);
    if (v->owner && v->owner->scene_capture_project_view && !v->owner->sampling_reference && scene_view_active(v->owner->view)) {
        const float distance = v->is_subject_plane ? subject_plane_distance(v->owner->subject_depth) : 1.f;
        const auto offset = scene_plane_translation(v->owner->view, distance, float(v->owner->height));
        matrix4 placement;
        gs_matrix_get(&placement);
        placement.t.x += offset.x;
        placement.t.y += offset.y;
        gs_matrix_set(&placement);
    }
    if ((!v->mask && v->capture_mode == LayerCaptureMode::None && v->exposure_gain == 1 && v->color_match_amount == 0 && !has_color_adjustment(v->treatment) && v->treatment.reactive == 0) || !v->effect) {
        render_layer_input(v);
        v->rendering = false;
        return;
    }
    const bool custom = has_color_adjustment(v->treatment) ||
                        v->treatment.reactive > 0;
    const uint32_t source_width = layer_width(v), source_height = layer_height(v);
    uint32_t w = source_width, h = source_height;
    matrix4 incoming;
    gs_matrix_get(&incoming);
    if (v->viewport_capture) {
        gs_rect viewport{};
        gs_get_viewport(&viewport);
        w = uint32_t(std::max(viewport.cx, 0));
        h = uint32_t(std::max(viewport.cy, 0));
    }
    if (!w || !h || !source_width || !source_height) {
        v->rendering = false;
        return;
    }
    bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    if (!v->capture)
        v->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    // Reuse raw pixels within one composition.
    // Replay and filter hooks must capture again.
    const bool cacheable = v->owner && !v->owner->scene_capture_callback && !v->capture_filter;
    const bool reused = cacheable && v->capture && gs_texrender_get_texture(v->capture) &&
                        v->capture_epoch == v->owner->composition_epoch && v->capture_owner == v->owner &&
                        v->capture_width == w && v->capture_height == h &&
                        v->capture_offset_x == v->offset_x && v->capture_offset_y == v->offset_y &&
                        v->capture_reference_context == v->owner->sampling_reference;
    bool ok = reused;
    if (!reused) {
        v->capture_epoch = 0;
        if (v->capture)
            gs_texrender_reset(v->capture);
        ok = v->capture && gs_texrender_begin(v->capture, w, h);
        if (ok) {
            vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
            // Texrender preserves projection but resets the model. Whole-group
            // replay needs the caller's model to retain its exact pixel coverage.
            if (v->viewport_capture)
                gs_matrix_set(&incoming);
            else
                gs_ortho(0, (float)w, 0, (float)h, -100, 100);
            render_layer_input(v);
            gs_texrender_end(v->capture);
            if (cacheable) {
                v->capture_epoch = v->owner->composition_epoch;
                v->capture_owner = v->owner;
                v->capture_width = w;
                v->capture_height = h;
                v->capture_offset_x = v->offset_x;
                v->capture_offset_y = v->offset_y;
                v->capture_reference_context = v->owner->sampling_reference;
            }
        }
    }
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    if (ok) {
        auto *tex = gs_texrender_get_texture(v->capture);
        gs_effect_set_texture(gs_effect_get_param_by_name(v->effect, "image"), tex);
        gs_effect_set_texture(gs_effect_get_param_by_name(v->effect, "auto_image"),
                              v->color_sample_texture ? v->color_sample_texture : tex);
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "layer_gain"), v->exposure_gain);
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "auto_blend"), v->color_match_amount);
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "source_exposure"), v->treatment.exposure);
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "source_saturation"), v->treatment.saturation);
        gs_effect_set_vec4(gs_effect_get_param_by_name(v->effect, "source_tint"), &v->treatment.tint);
        const bool emitter = v->owner &&
            source_matches_reference(v->original, v->owner->reactive_source_id.c_str());
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "source_reactive_amount"), emitter ? 0 : v->treatment.reactive);
        vec2 canvas = {v->owner ? (float)v->owner->width : (float)w, v->owner ? (float)v->owner->height : (float)h};
        gs_effect_set_vec2(gs_effect_get_param_by_name(v->effect, "source_canvas"), &canvas);
        if (custom && v->owner) bind_source_reactive(v->owner, tex);
        // The shader works in encoded SDR, native scene blending owns sRGB state.
        gs_enable_framebuffer_srgb(false);
        gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "mask_value"), v->is_treated_subject ? 1.f : 0.f);
        vec2 screen_weights = {1 - v->treatment.produce_effects, 1 - v->treatment.receive_effects};
        gs_effect_set_vec2(gs_effect_get_param_by_name(v->effect, "source_screen_weights"), &screen_weights);
        if (v->capture_mode == LayerCaptureMode::LightingCoverage) {
            const float unprotected=v->protection == SourceProtection::None ? 1.f : 0.f;
            vec3 weights={(1-v->treatment.receive_spotlight)*unprotected,unprotected,
                v->is_treated_subject ? 0.f : v->treatment.receive_rays*unprotected};
            gs_effect_set_vec3(gs_effect_get_param_by_name(v->effect,"source_spotlight_weights"),&weights);
        }
        if (v->capture_mode != LayerCaptureMode::None) {
            float receiving = v->treatment.reactive_override >= 0 ? (emitter ? 0.f : v->treatment.reactive_override)
                : (v->is_treated_subject || (v->owner && v->owner->reactive_background)) ? 1.f : 0.f;
            if (v->protection != SourceProtection::None) receiving = 0;
            gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "source_receive_amount"), receiving);
            gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "protected_value"),
                                v->protection != SourceProtection::None ? 1.f : 0.f);
            gs_effect_set_float(gs_effect_get_param_by_name(v->effect, "spill_value"),
                                std::max(v->treatment.receive, 0.f));
        }
        if (v->viewport_capture) {
            vec2 size = {float(source_width) / w, float(source_height) / h};
            gs_effect_set_vec2(gs_effect_get_param_by_name(v->effect, "layer_source_scale"), &size);
        }
        const char *technique = v->capture_mode == LayerCaptureMode::ProtectedColor
                                    ? (custom ? "ProtectionCustomColor" : "ProtectionColor")
                                : v->capture_mode == LayerCaptureMode::ProtectionCoverage ? "ProtectionMask"
                                : v->capture_mode == LayerCaptureMode::LightingCoverage
                                    ? "SourceSpotlightMask"
                                : v->mask ? "SubjectMask"
                                : custom  ? "SourceLayer"
                                          : "Layer";
        std::string viewport_technique;
        if (v->viewport_capture) {
            viewport_technique = std::string("Viewport") + technique;
            technique = viewport_technique.c_str();
        }
        // Scene rendering resets blending to SRCALPHA. Coverage and captured RGB
        // are already premultiplied, so multiplying alpha again breaks overlaps.
        if (v->viewport_capture || v->capture_mode != LayerCaptureMode::None || v->mask) {
            gs_blend_state_push();
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        }
        while (gs_effect_loop(v->effect, technique))
            gs_draw_sprite(tex, 0, w, h);
        if (v->viewport_capture || v->capture_mode != LayerCaptureMode::None || v->mask)
            gs_blend_state_pop();
        gs_enable_framebuffer_srgb(srgb);
    } else if (v->mask || v->capture_mode != LayerCaptureMode::None)
        v->mask_capture_failed = true;
    else
        render_layer_input(v);
    v->rendering = false;
}

obs_source_t *create_layer_source(obs_source_t *source, gs_effect_t *effect, LayerView **view) {
    layer_seed = source;
    layer_effect_seed = effect;
    layer_created = nullptr;
    auto *wrapper = obs_source_create_private("glowbox_internal_layer", "Glowbox layer", nullptr);
    *view = layer_created;
    layer_seed = nullptr;
    return wrapper;
}
void register_layer_source() {
    obs_source_info layer = {};
    layer.id = "glowbox_internal_layer";
    layer.type = OBS_SOURCE_TYPE_INPUT;
    layer.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
    layer.get_name = layer_name;
    layer.create = layer_create;
    layer.destroy = layer_destroy;
    layer.video_render = layer_render;
    layer.get_width = layer_width;
    layer.get_height = layer_height;
    obs_register_source(&layer);
}

}
