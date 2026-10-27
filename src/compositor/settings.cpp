#include "bounded_setting.hpp"
#include "compositor/compositor.hpp"

namespace glowbox {
static void migrate_lightwash_settings(obs_data_t *s) {
    if (!obs_data_get_bool(s, "lightwash_split_controls")) {
        float intensity =
            obs_data_has_user_value(s, "flare_intensity") ? bounded_setting(s, "flare_intensity", 0, 1) : .2f;
        float opacity = bounded_setting(s, "flare_opacity", 0, 1);
        if (obs_data_has_user_value(s, "flare_amount")) {
            const float amount = bounded_setting(s, "flare_amount", 0, 1);
            if (obs_data_get_bool(s, "lightwash_amount_curve")) {
                intensity = 1;
                opacity = amount * amount;
            } else {
                intensity = amount;
                opacity = 1;
            }
        }
        obs_data_set_double(s, "flare_intensity", std::sqrt(intensity));
        obs_data_set_double(s, "flare_opacity", opacity);
        obs_data_set_bool(s, "lightwash_split_controls", true);
    }
}

static void migrate_light_follow_settings(obs_data_t *s) {
    if (!obs_data_has_user_value(s, "rim_follow_light"))
        obs_data_set_int(s, "rim_follow_light", obs_data_get_bool(s, "rim_follow_source") ? 1 : 0);
    if (!obs_data_get_bool(s, "spotlight_shadow_split")) {
        if (!obs_data_has_user_value(s, "shadow_enabled") &&
            obs_data_has_user_value(s, "spotlight_enabled")) {
            obs_data_set_bool(s, "shadow_enabled",
                              obs_data_get_bool(s, "spotlight_enabled") &&
                                  obs_data_get_double(s, "spotlight_shadow") > 0);
            obs_data_set_double(s, "shadow_opacity", obs_data_get_double(s, "spotlight_shadow"));
        }
        obs_data_set_bool(s, "spotlight_shadow_split", true);
    }
}

static void update_color_reference(Compositor *f, obs_data_t *s) {
    if (f->reference) {
        obs_weak_source_release(f->reference);
        f->reference = nullptr;
    }
    const char *reference_name = obs_data_get_string(s, "reference_source");
    bool stable_reference = strncmp(reference_name, "uuid:", 5) == 0;
    auto *reference = acquire_source_reference(reference_name);
    if (reference) {
        // Migrate old name-based selections once, renames retain the same source.
        if (!stable_reference) {
            std::string id = std::string("uuid:") + obs_source_get_uuid(reference);
            obs_data_set_string(s, "reference_source", id.c_str());
        }
        if (reference != f->source && reference != obs_filter_get_parent(f->source))
            f->reference = obs_source_get_weak_source(reference);
        obs_source_release(reference);
    }
}

void update_settings(void *data, obs_data_t *s) {
    auto *f = (Compositor *)data;
    // Intensity stores a curved slider position, Opacity is a linear fade.

    migrate_lightwash_settings(s);
    if (f->layer_settings)
        obs_data_release(f->layer_settings);
    f->layer_settings = obs_data_create();
    obs_data_apply(f->layer_settings, s);
    prepare_subject_settings(f, obs_scene_from_source(obs_filter_get_parent(f->source)));
    f->native_requested = true;
    update_reactive_source(f, s);
    migrate_light_follow_settings(s);
    f->subject_depth = bounded_setting(s, "subject_depth", 0, 1);
    f->view = {scene_view_coordinate(obs_data_get_double(s, "camera_x")),
               scene_view_coordinate(obs_data_get_double(s, "camera_y"))};
    obs_data_set_double(s, "camera_x", f->view.x);
    obs_data_set_double(s, "camera_y", f->view.y);
    f->shadow_amount =
        obs_data_get_bool(s, "shadow_enabled") ? bounded_setting(s, "shadow_opacity", 0, 1) : 0;
    f->shadow_follow = std::clamp((int)obs_data_get_int(s, "shadow_follow"), 0, 1);
    f->shadow_size = bounded_setting(s, "shadow_size", .5f, 2);
    f->shadow_softness = bounded_setting(s, "shadow_softness", 0, 1);
    f->shadow_distance = bounded_setting(s, "shadow_distance", 0, .3f);
    f->shadow_angle = bounded_setting(s, "shadow_angle", -180, 180);
    f->lighting.spotlight_amount = 0; // The Scene Light supplies lighting.
    f->lighting.spotlight_x = bounded_setting(s, "spotlight_x", -5, 6);
    f->lighting.spotlight_y = bounded_setting(s, "spotlight_y", -5, 6);
    f->lighting.spotlight_size = bounded_setting(s, "spotlight_size", .05f, 1.5f);
    f->lighting.spotlight_softness = bounded_setting(s, "spotlight_softness", .01f, 1);
    f->lighting.spotlight_tilt = bounded_setting(s, "spotlight_tilt", 0, 60);
    f->lighting.spotlight_direction = bounded_setting(s, "spotlight_direction", -180, 180);
    f->lighting.spotlight_falloff = bounded_setting(s, "spotlight_falloff", 0, 1);
    f->lighting.spotlight_brightness = bounded_setting(s, "spotlight_brightness", 0, 2);
    f->lighting.spotlight_darkness = bounded_setting(s, "spotlight_darkness", 0, 8);
    vec4_from_rgba(&f->lighting.spotlight_light_color,
                   (uint32_t)obs_data_get_int(s, "spotlight_light_color"));
    vec4_from_rgba(&f->lighting.spotlight_color, (uint32_t)obs_data_get_int(s, "spotlight_color"));
    f->rim_enabled = obs_data_get_bool(s, "rim_enabled");
    f->rim_follow_light = std::clamp((int)obs_data_get_int(s, "rim_follow_light"), 0, 2);
    f->rim_follow_source = f->rim_follow_light != 0;
    f->rim_follow_ready = false;
    f->rim_type = std::clamp((int)obs_data_get_int(s, "rim_type"), 0, 3);
    f->rim_blend = std::clamp((int)obs_data_get_int(s, "rim_blend"), 0, 23);
    f->rim_size = bounded_setting(s, "rim_size", 0, 1);
    f->rim_brightness = bounded_setting(s, "rim_brightness", 0, 10);
    f->rim_contrast = bounded_setting(s, "rim_contrast", 0, 1);
    f->rim_ui_state.store(unsigned(f->rim_blend) | (f->rim_contrast > 0 ? 32u : 0u),
                          std::memory_order_relaxed);
    f->rim_angle = bounded_setting(s, "rim_angle", -180, 180);
    f->rim_opacity = bounded_setting(s, "rim_opacity", 0, 1);
    vec4_from_rgba(&f->rim_color, (uint32_t)obs_data_get_int(s, "rim_color"));
    f->subject_exposure_gain = std::exp2(bounded_setting(s, "subject_exposure", -3, 3));
    f->background_exposure_gain = std::exp2(bounded_setting(s, "background_exposure", -3, 3));
    f->wants_layers = f->subject_exposure_gain != 1 || f->background_exposure_gain != 1;
    for (auto *i = obs_data_first(s); i; obs_data_item_next(&i))
        if (strncmp(obs_data_item_get_name(i), "subject_", 8) == 0 &&
            obs_data_item_gettype(i) == OBS_DATA_BOOLEAN && obs_data_item_get_bool(i))
            f->wants_layers = true;
    f->diffusion = bounded_setting(s, "diffusion", 0, 1);
    f->glow = bounded_setting(s, "glow", 0, 2);
    f->radius = bounded_setting(s, "radius", .25f, 6);
    f->threshold = bounded_setting(s, "threshold", 0, .99f);
    f->exposure_gain = std::exp2(bounded_setting(s, "exposure", -3, 3));
    f->contrast = bounded_setting(s, "contrast", 0, 2);
    f->saturation = bounded_setting(s, "saturation", 0, 2);
    if (obs_data_has_user_value(s, "contrast_adjust"))
        f->contrast = 1 + bounded_setting(s, "contrast_adjust", -1, 1);
    if (obs_data_has_user_value(s, "saturation_adjust"))
        f->saturation = 1 + bounded_setting(s, "saturation_adjust", -1, 1);
    f->screen_mode = std::clamp((int)obs_data_get_int(s, "screen_mode"), 0, 3);
    f->screen_intensity = bounded_setting(s, "screen_intensity", 0, 1);
    f->screen_opacity = bounded_setting(s, "screen_opacity", 0, 1);
    f->screen_contrast = bounded_setting(s, "screen_contrast", 0, 1);
    if (obs_data_get_double(s, "screen_contrast") < 0)
        obs_data_set_double(s, "screen_contrast", 0);
    // A missing screen_mode can retain both saved effects.
    if (obs_data_has_user_value(s, "screen_mode") ||
        (!obs_data_has_user_value(s, "diffusion") && !obs_data_has_user_value(s, "glow"))) {
        f->diffusion = f->screen_mode == 2 ? std::min(1.f, f->screen_intensity * f->screen_opacity) : 0;
        f->glow = f->screen_mode == 3 ? f->screen_intensity * f->screen_opacity : 0;
    } else {
        f->screen_mode = f->glow > 0 ? 3 : f->diffusion > 0 ? 2 : 0;
        f->screen_intensity = f->glow > 0 ? f->glow : f->diffusion;
    }
    f->brightness = bounded_setting(s, "brightness", -1, 1);
    f->color_match_strength = bounded_setting(s, "auto_blend", 0, 1);
    vec4_from_rgba(&f->color_filter, (uint32_t)obs_data_get_int(s, "color_filter"));
    vec4_from_rgba(&f->lighting.lightwash_color, (uint32_t)obs_data_get_int(s, "flare_color"));

    f->lighting.lightwash_size = bounded_setting(s, "flare_size", 0, 2);

    f->lighting.lightwash_intensity = 0;
    f->lighting.lightwash_opacity = bounded_setting(s, "flare_opacity", 0, 1);
    f->lighting.lightwash_angle = bounded_setting(s, "flare_angle", -180, 180) * .01745329252f;
    f->lighting.lightwash_type = std::clamp((int)obs_data_get_int(s, "flare_type"), 0, 2);
    update_color_reference(f, s);
    f->color_sample_elapsed = .1f;

    f->divisor = (int)obs_data_get_int(s, "resolution");
    if (f->divisor != 2 && f->divisor != 4 && f->divisor != 8)
        f->divisor = 4;
    f->levels = std::clamp((int)obs_data_get_int(s, "levels"), 1, 5);
    f->blend = std::clamp((int)obs_data_get_int(s, "blend"), 0, 2);
    f->processed = false;
}
void default_settings(obs_data_t *s) {
    obs_data_set_default_bool(s, "experimental_native_renderer", true);

    obs_data_set_default_bool(s, "experimental_bounded_reactive", true);
    obs_data_set_default_string(s, "light_source", "");
    obs_data_set_default_double(s, "light_strength", .5);
    obs_data_set_default_int(s, "reactive_smoothing", 25);
    obs_data_set_default_bool(s, "light_background", false);
    obs_data_set_default_bool(s, "exposure_background", false);
    obs_data_set_default_bool(s, "rim_enabled", false);
    obs_data_set_default_bool(s, "rim_follow_source", false);
    obs_data_set_default_int(s, "rim_type", 0);
    obs_data_set_default_int(s, "rim_blend", 7);
    obs_data_set_default_int(s, "rim_color", 0xffffffff);
    obs_data_set_default_double(s, "rim_size", .5);
    obs_data_set_default_double(s, "rim_brightness", 0);
    obs_data_set_default_double(s, "rim_contrast", 0);
    obs_data_set_default_double(s, "rim_angle", 0);
    obs_data_set_default_double(s, "rim_opacity", 1);
    obs_data_set_default_double(s, "diffusion", .12);
    obs_data_set_default_double(s, "glow", .12);
    obs_data_set_default_double(s, "radius", 4);
    obs_data_set_default_bool(s, "follow_brightness", false);
    obs_data_set_default_double(s, "follow_brightness_amount", 1); // Legacy migration only.
    obs_data_set_default_double(s, "follow_sensitivity", 1);
    obs_data_set_default_double(s, "follow_darken", .5);
    obs_data_set_default_double(s, "follow_brighten", .2);
    obs_data_set_default_double(s, "threshold", .3);
    obs_data_set_default_double(s, "exposure", 0);
    obs_data_set_default_double(s, "subject_exposure", -1);
    obs_data_set_default_double(s, "background_exposure", 0);
    obs_data_set_default_double(s, "contrast", 1);
    obs_data_set_default_double(s, "saturation", 1);
    obs_data_set_default_int(s, "resolution", 4);
    obs_data_set_default_int(s, "levels", 3);
    obs_data_set_default_int(s, "blend", 2);
    obs_data_set_default_int(s, "screen_mode", 2);
    obs_data_set_default_double(s, "screen_intensity", .6);
    obs_data_set_default_double(s, "screen_opacity", .5);
    obs_data_set_default_double(s, "screen_contrast", 0);
    obs_data_set_default_double(s, "contrast_adjust", 0);
    obs_data_set_default_double(s, "saturation_adjust", 0);
    obs_data_set_default_double(s, "brightness", 0);
    obs_data_set_default_double(s, "auto_blend", .6);
    obs_data_set_default_int(s, "color_filter", 0xffffffff);
    // A missing enable key means the wash is enabled.
    obs_data_set_default_double(s, "subject_depth", .25);
    obs_data_set_default_double(s, "camera_x", 0);
    obs_data_set_default_double(s, "camera_y", 0);
    obs_data_set_default_bool(s, "shadow_enabled", false);
    obs_data_set_default_double(s, "shadow_opacity", .65);
    obs_data_set_default_int(s, "shadow_follow", 1);
    obs_data_set_default_double(s, "shadow_size", 1);
    obs_data_set_default_double(s, "shadow_softness", .2);
    obs_data_set_default_double(s, "shadow_distance", .035);
    obs_data_set_default_double(s, "shadow_angle", 135);
    obs_data_set_default_bool(s, "spotlight_enabled", false);
    obs_data_set_default_double(s, "spotlight_x", .5);
    obs_data_set_default_double(s, "spotlight_y", .35);
    obs_data_set_default_double(s, "spotlight_size", .55);
    obs_data_set_default_double(s, "spotlight_softness", .25);
    obs_data_set_default_double(s, "spotlight_tilt", 0);
    obs_data_set_default_double(s, "spotlight_direction", 0);
    obs_data_set_default_double(s, "spotlight_falloff", 0);
    obs_data_set_default_double(s, "spotlight_brightness", .35);
    obs_data_set_default_double(s, "spotlight_darkness", 6);
    obs_data_set_default_double(s, "spotlight_shadow", .65);
    obs_data_set_default_int(s, "spotlight_light_color", 0xffffffff);
    obs_data_set_default_int(s, "spotlight_color", 0xff6099ff);
    obs_data_set_default_bool(s, "lightwash_enabled", true);
    obs_data_set_default_int(s, "flare_type", 2);
    obs_data_set_default_int(s, "flare_color", 0xffffffff);
    obs_data_set_default_double(s, "flare_size", .5);
    obs_data_set_default_double(s, "flare_intensity", std::sqrt(.2));
    obs_data_set_default_double(s, "flare_opacity", .25);
}
void load_compositor_settings(void *data, obs_data_t *settings) {
    // Older scene files may omit their previous default (Overlay).
    if (!obs_data_has_user_value(settings, "rim_blend")) {
        obs_data_set_int(settings, "rim_blend", 9);
        update_settings(data, settings);
    }
}

void save_compositor_settings(void *data, obs_data_t *settings) {
    // Persist the resolved default so new Screen filters reload as Screen.
    obs_data_set_int(settings, "rim_blend", static_cast<Compositor *>(data)->rim_blend);
}

} // namespace glowbox
