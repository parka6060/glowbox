#include "bounded_setting.hpp"
#include "plugin_registration.hpp"
#include "sources/source_policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "properties_ui.hpp"
#include <atomic>

namespace glowbox {
static constexpr const char *source_controls_id = "glowbox_source_controls_filter";
static std::atomic<unsigned> source_controls_instance_count{0};
struct SourceControlsFilter {
    obs_source_t *source;
};
static const char *source_controls_name(void *) {
    return "Glowbox Source Controls";
}
static float source_saturation(obs_data_t *settings) {
    if (!obs_data_has_user_value(settings, "source_saturation_adjust") &&
        obs_data_has_user_value(settings, "source_saturation"))
        return bounded_setting(settings, "source_saturation", 0.f, 2.f);
    return 1.f + bounded_setting(settings, "source_saturation_adjust", -1.f, 1.f);
}
static void *source_controls_create(obs_data_t *settings, obs_source_t *source) {
    if (!obs_data_has_user_value(settings, "source_saturation_adjust") &&
        obs_data_has_user_value(settings, "source_saturation"))
        obs_data_set_double(settings, "source_saturation_adjust", source_saturation(settings) - 1.f);
    // Migrate both the original spill toggle and the later amount-only UI.
    if (!obs_data_has_user_value(settings, "receive_light_enabled")) {
        const double amount = obs_data_has_user_value(settings, "receive_light")
                                  ? obs_data_get_double(settings, "receive_light")
                              : obs_data_get_bool(settings, "allow_light_spill") ? 1
                                                                                 : 0;
        obs_data_set_bool(settings, "receive_light_enabled", amount > 0);
        obs_data_set_double(settings, "receive_light", amount > 0 ? amount : 1);
    }
    auto *filter = new SourceControlsFilter{source};
    source_controls_instance_count.fetch_add(1, std::memory_order_relaxed);
    return filter;
}
static void source_controls_destroy(void *data) {
    source_controls_instance_count.fetch_sub(1, std::memory_order_relaxed);
    delete static_cast<SourceControlsFilter *>(data);
}
static void source_controls_render(void *data, gs_effect_t *) {
    obs_source_skip_video_filter(static_cast<SourceControlsFilter *>(data)->source);
}
static void source_controls_defaults(obs_data_t *settings) {
    // Default to protection, disabling this filter follows the scene.
    obs_data_set_default_int(settings, "source_treatment", 0);
    // Saved adjustments apply only when their enable key is set.
    for (auto *key : {"adjust_color", "adjust_match", "adjust_reactive"})
        obs_data_set_default_bool(settings, key, false);
    obs_data_set_default_double(settings, "produce_effects", 1);
    obs_data_set_default_double(settings, "receive_effects", 1);
    obs_data_set_default_double(settings, "receive_spotlight", 1);
    obs_data_set_default_double(settings, "receive_rays", 1);
    obs_data_set_default_double(settings, "source_match", 1);
    obs_data_set_default_bool(settings, "source_correction", false);
    obs_data_set_default_double(settings, "source_exposure", 0);
    obs_data_set_default_double(settings, "source_saturation_adjust", 0);
    obs_data_set_default_int(settings, "source_tint", 0xffffff);
    obs_data_set_default_bool(settings, "source_reactive_enabled", false);
    obs_data_set_default_double(settings, "source_reactive", 1);
    obs_data_set_default_bool(settings, "allow_light_spill", false);
    obs_data_set_default_bool(settings, "receive_light_enabled", false);
    obs_data_set_default_double(settings, "receive_light", 1);
}
static bool receive_light_changed(obs_properties_t *properties, obs_property_t *, obs_data_t *settings) {
    auto *amount = obs_properties_get(properties, "receive_light");
    const bool visible = obs_data_get_bool(settings, "receive_light_enabled");
    if (obs_property_visible(amount) == visible)
        return false;
    obs_property_set_visible(amount, visible);
    return true;
}
static bool treatment_changed(obs_properties_t *properties, obs_property_t *, obs_data_t *settings) {
    const bool selected_effects = obs_data_get_int(settings, "source_treatment") == 1;
    const bool follow = obs_data_get_int(settings, "source_treatment") == 2;
    obs_property_set_visible(obs_properties_get(properties, "screen_effects"), follow);
    obs_property_set_visible(obs_properties_get(properties, "lighting"), follow);
    obs_property_set_visible(obs_properties_get(properties, "receive_spotlight"), follow);
    obs_property_set_visible(obs_properties_get(properties, "receive_rays"), follow);
    obs_property_set_visible(obs_properties_get(properties, "adjust_match"), follow);
    obs_property_set_visible(obs_properties_get(properties, "source_match"),
                             selected_effects || (follow && obs_data_get_bool(settings, "adjust_match")));
    obs_property_set_visible(obs_properties_get(properties, "adjust_color"), follow);
    obs_property_set_visible(obs_properties_get(properties, "adjust_reactive"), follow);
    obs_property_set_visible(obs_properties_get(properties, "receive_light_enabled"), !selected_effects && !follow);
    obs_property_set_visible(obs_properties_get(properties, "source_correction"), selected_effects);
    obs_property_set_visible(obs_properties_get(properties, "source_reactive_enabled"), selected_effects);
    const bool correction = (selected_effects && obs_data_get_bool(settings, "source_correction")) ||
                            (follow && obs_data_get_bool(settings, "adjust_color"));
    for (auto *key : {"source_exposure", "source_saturation_adjust", "source_tint"})
        obs_property_set_visible(obs_properties_get(properties, key), correction);
    obs_property_set_visible(obs_properties_get(properties, "source_reactive"),
                             (selected_effects && obs_data_get_bool(settings, "source_reactive_enabled")) ||
                                 (follow && obs_data_get_bool(settings, "adjust_reactive")));
    return true;
}
static obs_properties_t *source_controls_properties(void *data) {
    auto *properties = obs_properties_create();
    obs_properties_add_text(properties, "scope",
                            "Changes affect every scene using this source. Disable this filter to follow the "
                            "scene without source adjustments.",
                            OBS_TEXT_INFO);
    auto *treatment = obs_properties_add_list(properties, "source_treatment", "Scene Effects",
                                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(treatment, "Exclude scene effects", 0);
    obs_property_list_add_int(treatment, "Selected effects only", 1);
    obs_property_list_add_int(treatment, "Follow scene with adjustments", 2);
    obs_property_set_modified_callback(treatment, treatment_changed);
    obs_property_set_long_description(treatment,
                                      "Choose how this source participates. Exclude skips scene effects, "
                                      "with optional glow. Selected effects only uses the controls below. "
                                      "Follow scene keeps scene effects and allows individual adjustments.");
    auto *match_override = obs_properties_add_bool(properties, "adjust_match", "Override Color Match");
    obs_property_set_modified_callback(match_override, treatment_changed);
    obs_property_set_long_description(
        match_override, "Off uses the scene's Color Match setting. On sets a separate amount for this "
                        "source, using the same Match From reference. Matching is applied once.");
    obs_property_set_visible(match_override, false);
    auto *match = obs_properties_add_float_slider(properties, "source_match", "Color Match", 0, 1, .01);
    obs_property_set_long_description(
        match,
        "Match this source to the reference chosen under Color Match in the scene compositor. 0 skips "
        "matching. 1 applies full matching. Color Correction and reactive lighting have their own controls.");
    obs_property_set_visible(match, false);
    auto *color_override = obs_properties_add_bool(properties, "adjust_color", "Additional Color Adjustment");
    obs_property_set_modified_callback(color_override, treatment_changed);
    obs_property_set_long_description(
        color_override, "Make an extra exposure, saturation or tint adjustment before the scene's effects. "
                        "Off keeps the saved values and uses the scene normally.");
    obs_property_set_visible(color_override, false);
    auto *correction =
        obs_properties_add_bool(properties, "source_correction", "Additional Color Adjustment");
    obs_property_set_modified_callback(correction, treatment_changed);
    obs_property_set_long_description(correction,
                                      "Adjust this source's exposure, saturation and tint before Color Match "
                                      "and reactive lighting. Turning this off keeps the saved values.");
    obs_properties_add_float_slider(properties, "source_exposure", "Exposure", -3, 3, .01);
    auto *saturation =
        obs_properties_add_float_slider(properties, "source_saturation_adjust", "Saturation", -1, 1, .01);
    obs_property_set_long_description(
        saturation, "0 keeps the current saturation. -1 removes color; positive values strengthen it.");
    auto *tint = obs_properties_add_color(properties, "source_tint", "Tint");
    obs_property_set_long_description(
        tint, "Tint this source with the selected color. White leaves the tint unchanged.");
    auto *reactive_override =
        obs_properties_add_bool(properties, "adjust_reactive", "Override Reactive Color");
    obs_property_set_modified_callback(reactive_override, treatment_changed);
    obs_property_set_long_description(
        reactive_override,
        "Off uses the scene's receiving settings. On sets how much colored reactive light this source "
        "receives. Exposure, including Follow Source Brightness, stays controlled by the scene.");
    obs_property_set_visible(reactive_override, false);
    auto *reactive = obs_properties_add_bool(properties, "source_reactive_enabled", "Receive Reactive Color");
    obs_property_set_modified_callback(reactive, treatment_changed);
    obs_property_set_long_description(reactive,
                                      "Add colored light from the source chosen under React To in the scene "
                                      "compositor. Turning this off keeps the saved Reactive Color Amount.");
    auto *reactive_amount =
        obs_properties_add_float_slider(properties, "source_reactive", "Reactive Color Amount", 0, 1, .01);
    obs_property_set_long_description(
        reactive_amount, "Use React To and Color Strength from the scene compositor. 0 adds no reactive "
                         "light. 1 receives the full amount. This works even when Also Color the Background "
                         "is off. The source does not receive its own light through this control.");
    for (auto *key : {"source_correction", "source_exposure", "source_saturation_adjust", "source_tint",
                      "source_reactive_enabled", "source_reactive"})
        obs_property_set_visible(obs_properties_get(properties, key), false);
    auto *screen = obs_properties_create();
    auto *produce =
        obs_properties_add_float_slider(screen, "produce_effects", "Contribution to Effect", 0, 1, .01);
    obs_property_set_long_description(
        produce, "How much this source contributes to the scene's glow, diffusion or soft light. 0 removes "
                 "its contribution. 1 keeps the full contribution. This does not control reactive lighting.");
    auto *receive = obs_properties_add_float_slider(screen, "receive_effects", "Effect Received", 0, 1, .01);
    obs_property_set_long_description(
        receive, "How strongly the scene's glow, diffusion or soft light affects this source. 0 skips Screen "
                 "Effects on this source. 1 uses the full scene effect. Other scene settings, including "
                 "exposure, color, reactive lighting, rim light and authored lighting, still apply.");
    auto *screen_group =
        obs_properties_add_group(properties, "screen_effects", "Screen Effects", OBS_GROUP_NORMAL, screen);
    obs_property_set_visible(screen_group, false);
    auto *lighting = obs_properties_create();
    auto *spotlight =
        obs_properties_add_float_slider(lighting, "receive_spotlight", "Receive Glowbox Light", 0, 1, .01);
    obs_property_set_long_description(
        spotlight,
        "Control lighting from the Glowbox Light source, including its tint and shade. 0 removes that light "
        "from this source. 1 receives it fully. Scene exposure and other effects still apply.");
    obs_property_set_visible(spotlight, false);
    auto *rays = obs_properties_add_float_slider(lighting, "receive_rays", "Receive Rays", 0, 1, .01);
    obs_property_set_long_description(rays, "Blend rays over this source. 0 keeps it clear. The subject "
                                            "selected in the compositor stays in front of rays.");
    obs_property_set_visible(rays, false);
    auto *lighting_group =
        obs_properties_add_group(properties, "lighting", "Lighting", OBS_GROUP_NORMAL, lighting);
    obs_property_set_visible(lighting_group, false);
    // OBS groups span the form, so a child label cannot shift the toggle or help text.
    auto *controls = obs_properties_create();
    auto *toggle = obs_properties_add_group(properties, "receive_light_enabled", "Allow Glow Spill",
                                            OBS_GROUP_CHECKABLE, controls);
    obs_property_set_modified_callback(toggle, receive_light_changed);
    obs_property_set_long_description(
        toggle, "Let surrounding light from Screen Effects spill onto this source. The other scene effects "
                "stay excluded. Turning this off keeps the saved Glow Spill Amount.");
    auto *amount = obs_properties_add_float_slider(controls, "receive_light", "Glow Spill Amount", 0, 1, .01);
    obs_property_set_long_description(amount,
                                      "How much surrounding glow reaches this source. 0 adds none. 1 allows "
                                      "the full amount. This does not control reactive lighting.");
    obs_property_set_visible(amount, false);
    exposure_units(properties, "source_exposure");
    if (data) {
        auto *settings = obs_source_get_settings(static_cast<SourceControlsFilter *>(data)->source);
        receive_light_changed(properties, nullptr, settings);
        treatment_changed(properties, nullptr, settings);
        obs_data_release(settings);
    }
    obs_properties_add_text(properties, "help",
                            "Each receiving scene needs Glowbox Compositor. To adjust a whole group or "
                            "nested scene, add Source Controls to that group or scene.",
                            OBS_TEXT_INFO);
    add_glowbox_footer(properties);
    return properties;
}
void register_source_controls_filter() {
    obs_source_info info = {};
    info.id = source_controls_id;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = source_controls_name;
    info.create = source_controls_create;
    info.destroy = source_controls_destroy;
    info.get_defaults = source_controls_defaults;
    info.get_properties = source_controls_properties;
    info.video_render = source_controls_render;
    obs_register_source(&info);
}
struct MarkerPolicy {
    bool found = false, plain_protection = false;
    float receive = 1, color_match = 1;
    SourcePolicy custom;
    bool custom_found = false;
    bool follow_found = false;
    float produce_effects = 1, receive_effects = 1, receive_spotlight = 1, receive_rays = 1;
    SourcePolicy adjusted;
};
static void read_color_adjustment(obs_data_t *settings, SourcePolicy &policy) {
    policy.exposure = std::exp2(bounded_setting(settings, "source_exposure", -3.f, 3.f));
    policy.saturation = source_saturation(settings);
    const auto tint = (uint32_t)obs_data_get_int(settings, "source_tint");
    policy.tint = vec4{(tint & 255) / 255.f, ((tint >> 8) & 255) / 255.f, ((tint >> 16) & 255) / 255.f, 1};
}
static void read_marker(obs_source_t *, obs_source_t *filter, void *data) {
    if (!obs_source_enabled(filter) || strcmp(obs_source_get_unversioned_id(filter), source_controls_id) != 0)
        return;
    auto &policy = *static_cast<MarkerPolicy *>(data);
    auto *settings = obs_source_get_settings(filter);
    if (obs_data_get_int(settings, "source_treatment") == 2) {
        if (!policy.follow_found) {
            if (obs_data_get_bool(settings, "adjust_color")) {
                read_color_adjustment(settings, policy.adjusted);
            }
            if (obs_data_get_bool(settings, "adjust_match"))
                policy.adjusted.match_override = bounded_setting(settings, "source_match", 0.f, 1.f);
            if (obs_data_get_bool(settings, "adjust_reactive"))
                policy.adjusted.reactive_override = bounded_setting(settings, "source_reactive", 0.f, 1.f);
        }
        policy.follow_found = true;
        policy.produce_effects =
            std::min(policy.produce_effects, bounded_setting(settings, "produce_effects", 0.f, 1.f));
        policy.receive_effects =
            std::min(policy.receive_effects, bounded_setting(settings, "receive_effects", 0.f, 1.f));
        policy.receive_spotlight =
            std::min(policy.receive_spotlight, bounded_setting(settings, "receive_spotlight", 0.f, 1.f));
        policy.receive_rays =
            std::min(policy.receive_rays, bounded_setting(settings, "receive_rays", 0.f, 1.f));
        obs_data_release(settings);
        return;
    }
    const bool selected_effects = obs_data_get_int(settings, "source_treatment") == 1;
    if (selected_effects && !policy.custom_found) {
        policy.custom_found = true;
        if (obs_data_get_bool(settings, "source_correction")) {
            read_color_adjustment(settings, policy.custom);
        }
        if (obs_data_get_bool(settings, "source_reactive_enabled"))
            policy.custom.reactive = bounded_setting(settings, "source_reactive", 0.f, 1.f);
    }
    if (selected_effects)
        policy.color_match =
            std::min(policy.color_match, bounded_setting(settings, "source_match", 0.f, 1.f));
    else
        policy.plain_protection = true;
    float receive = obs_data_has_user_value(settings, "receive_light")
                        ? (float)obs_data_get_double(settings, "receive_light")
                    : obs_data_get_bool(settings, "allow_light_spill") ? 1.f
                                                                       : 0.f;
    if (obs_data_has_user_value(settings, "receive_light_enabled") &&
        !obs_data_get_bool(settings, "receive_light_enabled"))
        receive = 0;
    if (selected_effects)
        receive = 0; // selected effects don't receive screen spill
    obs_data_release(settings);
    policy.found = true;
    policy.receive = std::min(policy.receive, std::clamp(receive, 0.f, 1.f));
}
SourcePolicy source_policy(obs_source_t *source) {
    MarkerPolicy policy;
    if (source && source_controls_instance_count.load(std::memory_order_relaxed) != 0)
        obs_source_enum_filters(source, read_marker, &policy);
    // exclusion overrides selected effects, repeated filters use the lowest
    // spill and match amounts.
    SourcePolicy result = policy.plain_protection ? SourcePolicy{} : policy.custom;
    result.receive = policy.found ? policy.receive : -1.f;
    result.color_match = policy.found && !policy.plain_protection ? policy.color_match : 0.f;
    if (policy.follow_found && !policy.found) {
        result = policy.adjusted;
        result.produce_effects = policy.produce_effects;
        result.receive_effects = policy.receive_effects;
        result.receive_spotlight = policy.receive_spotlight;
        result.receive_rays = policy.receive_rays;
    }
    return result;
}
// One traversal per composition. Keep this snapshot local to a render so
// property edits and shared-source changes never wait for a cache invalidation.
SceneFeatures scene_features(obs_scene_t *scene) {
    SceneFeatures result;
    if (scene && source_controls_instance_count.load(std::memory_order_relaxed))
        obs_scene_enum_items(
            scene,
            [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
                if (!obs_sceneitem_visible(item))
                    return true;
                auto &f = *static_cast<SceneFeatures *>(opaque);
                const auto p = source_policy(obs_sceneitem_get_source(item));
                f.exclusions |= p.receive >= 0;
                f.screen |= p.produce_effects != 1 || p.receive_effects != 1;
                f.reactive_overrides |= p.reactive_override >= 0;
                f.light |= p.receive_spotlight != 1;
                f.rays |= p.receive_rays != 1;
                f.treatment |= has_source_treatment(p);
                return true;
            },
            &result);
    return result;
}
static bool find_source_treatment(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    const auto p = source_policy(obs_sceneitem_get_source(item));
    if (obs_sceneitem_visible(item) && has_source_treatment(p))
        *static_cast<bool *>(data) = true;
    return true;
}
bool scene_has_source_treatment(obs_scene_t *scene) {
    bool found = false;
    if (scene && source_controls_instance_count.load(std::memory_order_relaxed))
        obs_scene_enum_items(scene, find_source_treatment, &found);
    return found;
}
static bool find_source_reactive(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    if (!obs_sceneitem_visible(item))
        return true;
    const auto policy = source_policy(obs_sceneitem_get_source(item));
    if (policy.reactive > 0 || policy.reactive_override > 0)
        *static_cast<bool *>(data) = true;
    return true;
}
bool scene_has_source_reactive(obs_scene_t *scene) {
    bool found = false;
    if (scene && source_controls_instance_count.load(std::memory_order_relaxed))
        obs_scene_enum_items(scene, find_source_reactive, &found);
    return found;
}
SourceProtection source_protection(obs_source_t *source) {
    return source_protection(source_policy(source));
}
} // namespace glowbox
