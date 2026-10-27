#include "bounded_setting.hpp"
#include "compositor/compositor.hpp"

namespace glowbox {
bool reactive_requested(const Compositor *f) {
    return !f->reactive_source_id.empty() && (f->reactive_strength > 0 || f->follow_amount > 0) && (f->wants_layers || f->reactive_background || (f->follow_amount > 0 && f->exposure_background) ||
        scene_has_source_reactive(obs_scene_from_source(obs_filter_get_parent(f->source))));
}
void release_reactive_buffers(Compositor *f) {
    for (auto target : {&f->reactive_capture, &f->reactive_history[0], &f->reactive_history[1],
                        &f->reactive_treatment, &f->reactive_spatial[0], &f->reactive_spatial[1]}) {
        gs_texrender_destroy(*target);
        *target = nullptr;
    }
    for (auto &target : f->follow_history) { gs_texrender_destroy(target); target = nullptr; }
    f->follow_ready = false;
    f->reactive_ready = false;
    f->reactive_spatial_ready = false;
    f->reactive_index = 0;
}
void update_reactive_source(Compositor *f, obs_data_t *settings) {
    f->bounded_reactive = obs_data_get_bool(settings, "experimental_bounded_reactive");
    // The old background toggle controlled both color and exposure. Preserve that
    // choice on first load, subsequent edits are independent and saved normally.
    if (!obs_data_has_user_value(settings, "exposure_background"))
        obs_data_set_bool(settings, "exposure_background", obs_data_get_bool(settings, "light_background"));
    // Fold the retired Amount into limits and metering slope exactly once.
    if (obs_data_has_user_value(settings, "follow_brightness_amount") &&
        !obs_data_get_bool(settings, "follow_controls_migrated")) {
        float amount = bounded_setting(settings, "follow_brightness_amount", 0.f, 1.f);
        if (amount == 0) obs_data_set_bool(settings, "follow_brightness", false);
        else {
            obs_data_set_double(settings, "follow_darken", obs_data_get_double(settings, "follow_darken") * amount);
            obs_data_set_double(settings, "follow_brighten", obs_data_get_double(settings, "follow_brighten") * amount);
            obs_data_set_double(settings, "follow_sensitivity", amount);
        }
        obs_data_unset_user_value(settings, "follow_brightness_amount");
        obs_data_set_bool(settings, "follow_controls_migrated", true);
    }
    std::string name = obs_data_get_string(settings, "light_source");
    auto *source = acquire_source_reference(name.c_str());
    if (source) {
        name = std::string("uuid:") + obs_source_get_uuid(source);
        obs_data_set_string(settings, "light_source", name.c_str());
        obs_source_release(source);
    }
    if (name != f->reactive_source_id) {
        f->reactive_source_id = name;
        f->reactive_ready = false;
        f->reactive_spatial_ready = false;
        f->follow_ready = false;
        f->follow_index = 0;
    }
    f->follow_amount = obs_data_get_bool(settings, "follow_brightness") ? 1.f : 0.f;
    f->follow_sensitivity = bounded_setting(settings, "follow_sensitivity", 0.f, 1.f);
    f->follow_darken = bounded_setting(settings, "follow_darken", 0.f, 2.f);
    f->follow_brighten = bounded_setting(settings, "follow_brighten", 0.f, 1.f);
    if (!f->bounded_reactive && f->follow_darken == 0 && f->follow_brighten == 0) f->follow_amount = 0;
    f->reactive_background = obs_data_get_bool(settings, "light_background");
    f->exposure_background = obs_data_get_bool(settings, "exposure_background");
    f->reactive_strength = bounded_setting(settings, "light_strength", 0.f, 4.f);
    const int smoothing = std::clamp((int)obs_data_get_int(settings, "reactive_smoothing"), 0, 100);
    // Zero bypasses smoothing history.

    const float exponent = smoothing <= 25 ? (smoothing - 25) / 25.f : (smoothing - 25) * (8.f / 75.f);
    f->reactive_smoothing = smoothing == 0 ? 0.f : std::exp2(exponent);
}
struct SourceCheck { obs_source_t *parent; obs_source_t *filter; bool recursive = false; };
static void check_source(obs_source_t *, obs_source_t *child, void *data) {
    auto &check = *static_cast<SourceCheck *>(data);
    check.recursive |= child == check.parent || child == check.filter;
}
bool sample_reactive_light(Compositor *f, gs_texture_t *fallback) {
    if (!reactive_requested(f) || !f->layered ||
        (!f->reactive_background && !(f->follow_amount > 0 && f->exposure_background) &&
         !std::any_of(f->items.begin(), f->items.end(), [](const MirrorItem &i) { return i.view->is_treated_subject || i.view->treatment.reactive > 0; }))) {
        release_reactive_buffers(f);
        return true;
    }
    if (!f->reactive_capture) f->reactive_capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    for (auto &target : f->reactive_history)
        if (!target) target = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f->reactive_capture || !f->reactive_history[0] || !f->reactive_history[1]) return false;
    auto *source = acquire_source_reference(f->reactive_source_id.c_str());
    SourceCheck check{obs_filter_get_parent(f->source), f->source};
    if (source && source != check.parent && source != f->source)
        obs_source_enum_full_tree(source, check_source, &check);
    const gs_color_space preferred[] = {GS_CS_SRGB};
    const uint32_t w = source ? obs_source_get_width(source) : 0;
    const uint32_t h = source ? obs_source_get_height(source) : 0;
    bool valid = source && !obs_source_removed(source) && w && h && !check.recursive &&
                 source != check.parent && source != f->source &&
                 obs_source_get_color_space(source, 1, preferred) == GS_CS_SRGB;
    if (valid) {
        gs_texrender_reset(f->reactive_capture);
        valid = gs_texrender_begin(f->reactive_capture, 128, 72);
        if (valid) {
            vec4 clear; vec4_zero(&clear); gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
            gs_ortho(0, (float)w, 0, (float)h, -100, 100);
            gs_blend_state_push(); gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
            f->sampling_reference = true;
            obs_source_video_render(source);
            f->sampling_reference = false;
            gs_blend_state_pop(); gs_texrender_end(f->reactive_capture);
        }
    }
    if (source) obs_source_release(source);
    if (!valid && !fallback) { release_reactive_buffers(f); return true; }
    const int next = 1 - f->reactive_index;
    gs_effect_set_bool(shader_parameter(f, "bounded_analysis"), f->bounded_reactive);
    gs_effect_set_float(shader_parameter(f, "reactive_response_scale"), f->reactive_smoothing);
    gs_effect_set_bool(shader_parameter(f, "reactive_valid"), valid);
    gs_effect_set_bool(shader_parameter(f, "reactive_has_history"), f->reactive_ready);
    gs_effect_set_texture(shader_parameter(f, "reactive_previous"),
        f->reactive_ready ? gs_texrender_get_texture(f->reactive_history[f->reactive_index]) : fallback);
    gs_effect_set_float(shader_parameter(f, "reactive_elapsed"),
                        std::clamp(f->reactive_elapsed, 1.f / 240.f, .1f));
    if (!render_pass(f, f->reactive_history[next],
                    valid ? gs_texrender_get_texture(f->reactive_capture) : fallback,
                    2, 1, "ReactiveAnalysis")) return false;
    if(!f->reactive_geometry_override)
        f->reactive_geometry=reactive_geometry(f,obs_scene_from_source(obs_filter_get_parent(f->source)));
    const auto &g=f->reactive_geometry;
    if(valid && g.valid) {
        for(auto &target:f->reactive_spatial)if(!target)target=gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
        if(!f->reactive_spatial[0] || !f->reactive_spatial[1])return false;
        gs_effect_set_bool(shader_parameter(f, "bounded_analysis"), f->bounded_reactive);
    gs_effect_set_float(shader_parameter(f, "reactive_response_scale"), f->reactive_smoothing);
        gs_effect_set_vec2(shader_parameter(f,"reactive_origin"),&g.origin);
        gs_effect_set_vec2(shader_parameter(f,"reactive_axis_x"),&g.axis_x);
        gs_effect_set_vec2(shader_parameter(f,"reactive_axis_y"),&g.axis_y);
        gs_effect_set_vec4(shader_parameter(f,"reactive_crop"),&g.crop);
        gs_effect_set_float(shader_parameter(f,"reactive_depth"),g.depth);
        gs_effect_set_float(shader_parameter(f,"reactive_aspect"),g.aspect);
        gs_effect_set_float(shader_parameter(f,"reactive_elapsed"),std::clamp(f->reactive_elapsed,1.f/240.f,.1f));
        gs_effect_set_bool(shader_parameter(f,"reactive_has_spatial_history"),f->reactive_spatial_ready);
        gs_effect_set_texture(shader_parameter(f,"reactive_spatial_previous"),f->reactive_spatial_ready?
            gs_texrender_get_texture(f->reactive_spatial[f->reactive_index]):fallback);
        if(!render_pass(f,f->reactive_spatial[next],gs_texrender_get_texture(f->reactive_capture),16,9,"ReactiveSpatialAnalysis"))return false;
        f->reactive_spatial_ready=true;
    } else f->reactive_spatial_ready=false;
    if (f->follow_amount > 0) {
        for (auto &target : f->follow_history) if (!target) target = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        if (!f->follow_history[0] || !f->follow_history[1]) return false;
        const int next_follow = 1 - f->follow_index;
        gs_effect_set_texture(shader_parameter(f, "follow_previous"), f->follow_ready
            ? gs_texrender_get_texture(f->follow_history[f->follow_index]) : (valid ? gs_texrender_get_texture(f->reactive_capture) : fallback));
        gs_effect_set_float(shader_parameter(f, "follow_sensitivity"), f->follow_sensitivity);
        gs_effect_set_float(shader_parameter(f, "follow_darken"), f->follow_darken);
        gs_effect_set_float(shader_parameter(f, "follow_brighten"), f->follow_brighten);
        gs_effect_set_bool(shader_parameter(f, "follow_valid"), valid);
        gs_effect_set_bool(shader_parameter(f, "follow_has_history"), f->follow_ready);
        gs_effect_set_bool(shader_parameter(f, "bounded_analysis"), f->bounded_reactive);
    gs_effect_set_float(shader_parameter(f, "reactive_response_scale"), f->reactive_smoothing);
        gs_effect_set_float(shader_parameter(f, "follow_elapsed"), std::clamp(f->reactive_elapsed, 1.f/240.f, .1f));
        if (!render_pass(f, f->follow_history[next_follow], valid ? gs_texrender_get_texture(f->reactive_capture) : fallback,
                         1, 1, "FollowBrightnessAnalysis")) return false;
        f->follow_index = next_follow; f->follow_ready = true;
        if (f->follow_owner && !copy_follow_brightness(f, f->follow_owner)) return false;
    } else f->follow_ready = false;
    f->reactive_index = next; f->reactive_ready = true; f->reactive_elapsed = 0;
    return true;
}
bool copy_follow_brightness(Compositor *from, Compositor *to) {
    if (!from->follow_ready || to->follow_amount <= 0) return true;
    if (!to->follow_history[to->follow_index]) to->follow_history[to->follow_index] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!to->follow_history[to->follow_index] || !render_pass(to, to->follow_history[to->follow_index],
        gs_texrender_get_texture(from->follow_history[from->follow_index]), 1, 1, "Copy")) return false;
    to->follow_ready = true;
    return true;
}
void bind_follow_brightness(Compositor *f, gs_effect_t *effect, gs_texture_t *fallback) {
    const auto param=[&](const char *name){return gs_effect_get_param_by_name(effect,name);};
    gs_effect_set_bool(param("bounded_pipeline"),f->bounded_reactive);
    gs_effect_set_bool(param("bounded_enabled"),f->bounded_reactive && f->reactive_ready && !f->has_exclusions);
    gs_effect_set_texture(param("bounded_spatial"),f->reactive_spatial_ready?gs_texrender_get_texture(f->reactive_spatial[f->reactive_index]):fallback);
    gs_effect_set_texture(param("bounded_signal"),f->reactive_ready?gs_texrender_get_texture(f->reactive_history[f->reactive_index]):fallback);
    gs_effect_set_texture(param("bounded_follow"),f->follow_ready?gs_texrender_get_texture(f->follow_history[f->follow_index]):fallback);
    gs_effect_set_texture(param("bounded_mask"),f->subject_texture?f->subject_texture:fallback);
    gs_effect_set_texture(param("bounded_overrides"),f->has_reactive_overrides?gs_texrender_get_texture(f->protection_mask):fallback);
    gs_effect_set_bool(param("bounded_has_overrides"),f->has_reactive_overrides);
    gs_effect_set_float(param("bounded_spatial_amount"),f->reactive_spatial_ready?f->reactive_geometry.amount:0);
    gs_effect_set_float(param("bounded_strength"),f->reactive_strength);
    gs_effect_set_float(param("bounded_follow_amount"),f->follow_ready?f->follow_amount:0);
    gs_effect_set_float(param("bounded_darken"),f->follow_darken);
    gs_effect_set_float(param("bounded_brighten"),f->follow_brighten);
    gs_effect_set_bool(param("bounded_color_background"),f->reactive_background);
    gs_effect_set_bool(param("bounded_exposure_background"),f->exposure_background);

    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "follow_signal"), f->follow_ready
        ? gs_texrender_get_texture(f->follow_history[f->follow_index]) : fallback);
    gs_effect_set_float(gs_effect_get_param_by_name(effect, "follow_amount"), f->follow_ready ? f->follow_amount : 0);
    gs_effect_set_bool(gs_effect_get_param_by_name(effect, "follow_background"), f->exposure_background);
}
void bind_source_reactive(Compositor *f, gs_texture_t *fallback) {
    gs_effect_set_bool(shader_parameter(f, "reactive_source_overrides"), f->has_reactive_overrides);
    gs_effect_set_texture(shader_parameter(f, "reactive_override_mask"), f->has_reactive_overrides
        ? gs_texrender_get_texture(f->protection_mask) : fallback);
    bind_follow_brightness(f, f->effect, fallback);
    gs_effect_set_texture(shader_parameter(f, "reactive_signal"),
                         f->reactive_ready ? gs_texrender_get_texture(f->reactive_history[f->reactive_index]) : fallback);
    gs_effect_set_texture(shader_parameter(f, "reactive_mask"), f->subject_texture ? f->subject_texture : fallback);
    gs_effect_set_texture(shader_parameter(f,"reactive_spatial"),f->reactive_spatial_ready?
        gs_texrender_get_texture(f->reactive_spatial[f->reactive_index]):fallback);
    gs_effect_set_float(shader_parameter(f,"reactive_spatial_amount"),f->reactive_spatial_ready?f->reactive_geometry.amount:0);
    gs_effect_set_float(shader_parameter(f, "reactive_strength"), f->reactive_ready ? f->reactive_strength : 0);
    gs_effect_set_bool(shader_parameter(f, "reactive_background"), f->reactive_background);
}
gs_texture_t *apply_reactive_light(Compositor *f, gs_texture_t *input) {
    if (f->bounded_reactive || !reactive_requested(f) || !f->reactive_ready) return input;
    if (!f->reactive_treatment) f->reactive_treatment = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f->reactive_treatment) return nullptr;
    bind_source_reactive(f, input);
    return render_pass(f, f->reactive_treatment, input, f->width, f->height, "ReactiveTreatment")
               ? gs_texrender_get_texture(f->reactive_treatment) : nullptr;
}
}
