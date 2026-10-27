#include "bounded_setting.hpp"
#include "plugin_registration.hpp"
#include "effects/leaf_canopy.hpp"
#include "compositor/compositor.hpp"
#include "properties_ui.hpp"
#include <util/platform.h>
#include <mutex>

namespace glowbox {
double light_animation_seconds() {
    static const uint64_t epoch = os_gettime_ns();
    return (os_gettime_ns() - epoch) * 1e-9;
}
namespace {
constexpr const char *light_id = "glowbox_light_source";
const char *light_name(void *) {
    return "Glowbox Light (Preview)";
}
struct LightState {
    obs_source_t *source;
    std::shared_ptr<CanopyJob> canopy;
    std::mutex mutex;
    std::atomic<unsigned> property_views{0};
    float status_elapsed = 0;
    std::string status_text;
    struct Motion {
        double offset = 0;
        float speed = 0;
    } leaf, rays;
};
void update_light(void *data, obs_data_t *settings) {
    // Keep old scene files readable, the category replaces the Type x Pattern UI.
    if (!obs_data_has_user_value(settings, "light_category")) {
        const int pattern = (int)obs_data_get_double(settings, "light_pattern");
        obs_data_set_int(settings, "light_category",
                         pattern == 1 ? 3 : obs_data_get_int(settings, "light_type"));
    }
    auto *light = static_cast<LightState *>(data);
    std::lock_guard<std::mutex> lock(light->mutex);
    if (obs_data_get_double(settings, "light_pattern") == 1)
        light->canopy = request_canopy(settings);
    else
        light->canopy.reset();
    const double now = light_animation_seconds();
    const auto speed = [&](LightState::Motion &motion, const char *key) {
        const float value = (float)obs_data_get_double(settings, key);
        const float next = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
        motion.offset += now * (motion.speed - next);
        motion.speed = next;
    };
    speed(light->leaf, "leaf_speed");
    speed(light->rays, "ray_movement");
}
void *create_light(obs_data_t *settings, obs_source_t *source) {
    // Existing leaf looks keep their saved palette when properties first open.
    if (obs_data_has_user_value(settings, "light_pattern") &&
        obs_data_get_double(settings, "light_pattern") == 1)
        obs_data_set_bool(settings, "canopy_palette_set", true);
    auto *light = new LightState;
    light->source = source;
    update_light(light, settings);
    return light;
}
void destroy_light(void *data) {
    delete static_cast<LightState *>(data);
}
obs_source_t *light_source(void *data) {
    return data ? static_cast<LightState *>(data)->source : nullptr;
}
void draw_light(void *, gs_effect_t *) {} // Metadata only, the containing compositor draws it.
uint32_t light_size(void *) {
    return 512;
}
// Types: 0 Spotlight, 1 Wash, 2 Directional. Geometry stays in scene coordinates.
void defaults(obs_data_t *s) {
    obs_data_set_default_int(s, "light_type", 0);
    obs_data_set_default_double(s, "leaf_shadow_variation", .65);
    obs_data_set_default_double(s, "leaf_bottom_fade", 0);
    obs_data_set_default_double(s, "leaf_detail", 0);
    obs_data_set_default_double(s, "leaf_density", .75);
    obs_data_set_default_double(s, "canopy_coverage", 0.49);
    obs_data_set_default_double(s, "canopy_curve", 0.72);
    obs_data_set_default_double(s, "canopy_forks", 0.7);
    obs_data_set_default_double(s, "canopy_clumping", 0.77);
    obs_data_set_default_double(s, "canopy_leaf_size", 1.49);
    obs_data_set_default_int(s, "canopy_seed", 1114847375);
    obs_data_set_default_double(s, "canopy_scale", .9);
    obs_data_set_default_double(s, "canopy_rotation", 119);
    obs_data_set_default_double(s, "leaf_softness", 0.42);
    obs_data_set_default_double(s, "leaf_depth", 0.6);
    obs_data_set_default_double(s, "leaf_speed", .2);
    obs_data_set_default_double(s, "leaf_sway", .3);
    obs_data_set_default_double(s, "leaf_shimmer", .3);
    obs_data_set_default_double(s, "leaf_variation", .25);
    obs_data_set_default_double(s, "leaf_shadow_level", 0.48);
    obs_data_set_default_double(s, "leaf_edge_amount", 0.3);
    obs_data_set_default_double(s, "leaf_seed", 0);
    obs_data_set_default_int(s, "leaf_shadow_color", 0xffd2b291);
    obs_data_set_default_int(s, "leaf_tint", 0xff80bf8f);
    obs_data_set_default_double(s, "light_pattern", 0);
    obs_data_set_default_double(s, "pattern_size", 0.35);
    obs_data_set_default_double(s, "pattern_amount", 0.7);
    obs_data_set_default_double(s, "motion_drift", 0.35);
    obs_data_set_default_double(s, "motion_opposing", 0.5);
    obs_data_set_default_double(s, "motion_fading", 0.3);
    obs_data_set_default_double(s, "light_coverage", 0);

    obs_data_set_default_double(s, "opacity", 1);
    obs_data_set_default_double(s, "wash_intensity", .45);
    obs_data_set_default_int(s, "wash_blend", 1);
    obs_data_set_default_double(s, "ray_width", .35);
    obs_data_set_default_double(s, "ray_movement", .15);
    obs_data_set_default_int(s, "color", 0xffffffff);
    obs_data_set_default_int(s, "edge_color", 0xff6099ff);
    obs_data_set_default_double(s, "brightness", .35);
    obs_data_set_default_double(s, "darkness", .55);
    obs_data_set_default_double(s, "softness", .35);
    obs_data_set_default_double(s, "tilt", 35);
    obs_data_set_default_double(s, "falloff", 0);
    obs_data_set_default_double(s, "rays", 0);
    obs_data_set_default_bool(s, "surface_lighting", true);
    obs_data_set_default_double(s, "ray_detail", .85);
}
std::string light_status(obs_source_t *);
// Properties retain the source for their lifetime. Poll connection metadata only
// while a panel is open, never render a second scene or scan on every frame.
struct LightProperties {
    LightState *light;
    obs_source_t *source;
};
void release_properties(void *data) {
    auto *context = static_cast<LightProperties *>(data);
    if (context->light)
        --context->light->property_views;
    obs_source_release(context->source);
    delete context;
}
obs_source_t *properties_source(obs_properties_t *p) {
    auto *context = static_cast<LightProperties *>(obs_properties_get_param(p));
    return context ? context->source : nullptr;
}
void tick_light(void *data, float seconds) {
    auto *light = static_cast<LightState *>(data);
    if (!light->property_views.load(std::memory_order_relaxed))
        return;
    light->status_elapsed += seconds;
    if (light->status_elapsed < 1.f)
        return;
    light->status_elapsed = 0;
    const auto text = light_status(light->source);
    bool changed;
    {
        std::lock_guard<std::mutex> lock(light->mutex);
        changed = text != light->status_text;
        light->status_text = text;
    }
    // OBS queues its properties-view refresh on the UI thread.
    if (changed)
        obs_source_update_properties(light->source);
}
void canopy_palette(obs_data_t *s, bool approved) {
    obs_data_set_int(s, "color", approved ? 0xffcdf7feu : 0xffbdf1ffu);
    obs_data_set_int(s, "leaf_shadow_color", 0xffd2b291u);
    obs_data_set_int(s, "edge_color", 0xff86baedu);
    obs_data_set_double(s, "leaf_edge_amount", .3);
    obs_data_set_double(s, "leaf_shadow_level", approved ? .48 : .42);
    obs_data_set_double(s, "leaf_variation", 0);
    obs_data_set_double(s, "leaf_shadow_variation", 0);
    obs_data_set_double(s, "brightness", approved ? 0 : std::log2(1.12));
    obs_data_set_double(s, "wash_intensity", approved ? 0 : std::sqrt(.06));
    obs_data_set_double(s, "pattern_amount", 1);
    obs_data_set_bool(s, "canopy_palette_set", true);
}
bool controls_changed(obs_properties_t *, obs_property_t *, obs_data_t *);
obs_data_t *snapshot_look(obs_data_t *settings) {
    auto *look = obs_data_get_defaults(settings);
    obs_data_apply(look, settings);
    for (const char *key : {"effect_looks", "light_category", "advanced"})
        obs_data_erase(look, key);
    return look;
}
bool category_changed(obs_properties_t *p, obs_property_t *property, obs_data_t *s) {
    const int category = std::clamp((int)obs_data_get_int(s, "light_category"), 0, 3);
    // OBS calls the visual update before this modified callback. Infer the old
    // category from its rendering fields, which still describe the old look.
    const int previous_pattern = (int)obs_data_get_double(s, "light_pattern");
    const int previous = previous_pattern == 1 ? 3 : std::clamp((int)obs_data_get_int(s, "light_type"), 0, 2);
    if (category != previous) {
        auto *looks = obs_data_get_obj(s, "effect_looks");
        if (!looks)
            looks = obs_data_create();
        auto *previous_look = snapshot_look(s);
        obs_data_set_obj(looks, std::to_string(previous).c_str(), previous_look);
        obs_data_release(previous_look);
        auto *next = obs_data_get_obj(looks, std::to_string(category).c_str());
        if (!next) {
            auto *initial = obs_data_create();
            defaults(initial);
            next = snapshot_look(initial);
            obs_data_release(initial);
            if (category >= 3)
                obs_data_set_double(next, "light_coverage", 1);
            if (category == 3)
                canopy_palette(next, true);
        }
        obs_data_apply(s, next);
        obs_data_release(next);
        obs_data_set_obj(s, "effect_looks", looks);
        obs_data_release(looks);
    }
    obs_data_set_int(s, "light_type", category >= 3 ? 2 : category);
    obs_data_set_double(s, "light_pattern", category == 3 ? 1 : 0);
    controls_changed(p, property, s);
    return category != previous;
}
bool canopy_preset(obs_properties_t *, obs_property_t *, obs_data_t *s) {
    int choice = (int)obs_data_get_int(s, "canopy_start");
    if (choice < 1 || choice > 6)
        return false;
    const double values[][9] = {
        {.49, .72, .7, .77, 1.49, .42, .6, .9, 119}, {.65, .55, .5, .55, 1, .4, .65, 1, 0},
        {.62, .42, .76, .28, .70, .23, .65, .85, 0}, {.77, .64, .4, .7, 1.32, .4, .75, 1.15, 0},
        {.4, .85, .35, .4, .95, .22, .5, 1.12, 0},   {.87, .72, .7, .48, .92, .5, 1, .9, 0}};
    const char *keys[] = {"canopy_coverage", "canopy_curve",     "canopy_forks",
                          "canopy_clumping", "canopy_leaf_size", "leaf_softness",
                          "leaf_depth",      "canopy_scale",     "canopy_rotation"};
    for (int i = 0; i < 9; ++i)
        obs_data_set_double(s, keys[i], values[choice - 1][i]);
    obs_data_set_int(s, "canopy_seed", choice == 1 ? 1114847375 : 43);
    obs_data_set_int(s, "canopy_start", 0);
    canopy_palette(s, choice == 1);
    return true;
}

bool shuffle_canopy(obs_properties_t *p, obs_property_t *, void *) {
    auto *source = properties_source(p);
    if (!source)
        return false;
    auto *s = obs_source_get_settings(source);
    auto seed = (uint32_t)obs_data_get_int(s, "canopy_seed");
    obs_data_set_int(s, "canopy_seed", (seed * 1664525u + 1013904223u) & 0x7fffffffu);
    obs_source_update(source, s);
    obs_data_release(s);
    return true;
}
bool controls_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) {
    const int type = (int)obs_data_get_int(s, "light_type");
    const bool advanced = obs_data_get_bool(s, "advanced");
    const bool surface = obs_data_get_bool(s, "surface_lighting");
    const int pattern = (int)obs_data_get_double(s, "light_pattern");
    // Only discrete choices rebuild the property layout.
    bool changed = false;
    const auto show = [&](const char *key, bool visible) {
        auto *property = obs_properties_get(p, key);
        if (obs_property_visible(property) != visible) {
            obs_property_set_visible(property, visible);
            changed = true;
        }
    };
    const char *purpose = pattern == 1 ? "Foliage: moving leaf shadows and light gaps."
                          : type == 0
                              ? "Spotlight: a local pool of light. Resize the source to change its reach."
                          : type == 1 ? "Wash: blend a broad color gradient into the scene."
                                      : "Directional: parallel light for broad daylight and aligned shadows. "
                                        "Rotate the source to aim it.";
    obs_property_set_description(obs_properties_get(p, "effect_help"), purpose);
    const bool bounded = type != 2 || obs_data_get_double(s, "light_coverage") < .5;
    show("light_shade", true);
    show("advanced_controls", advanced);
    show("starting_look", pattern == 1);
    show("leaf_motion", pattern == 1);
    show("shape", bounded);
    show("softness", bounded);
    show("wash_intensity", type == 1);
    show("brightness", type != 1);
    show("opacity", true);
    show("darkness", surface && type == 0 && pattern == 0);
    show("tilt", type == 0 && advanced);
    show("falloff", advanced && type != 1);
    show("wash_blend", surface && type == 1 && pattern == 0 && advanced);
    // Leave ray controls available at zero. Rebuilding on the zero crossing
    // destroys the active Qt slider and interrupts the user's drag.
    show("rays_controls", pattern == 0);
    show("surface_lighting", pattern == 0);
    show("ray_width", pattern == 0);
    show("ray_movement", pattern == 0);
    show("ray_detail", pattern == 0 && advanced);
    show("light_coverage", type == 2);
    show("motion_drift", pattern == 0 && advanced);
    show("motion_opposing", pattern == 0 && advanced);
    show("motion_fading", pattern == 0 && advanced);
    show("canopy_scale", pattern == 1);
    show("pattern_amount", pattern > 0);
    show("leaf_controls", pattern == 1);
    show("leaf_depth", pattern == 1);
    show("leaf_shadow_color", pattern == 1 && surface);
    show("leaf_shadow_level", pattern == 1 && surface);
    for (const char *key : {"canopy_curve", "canopy_forks", "canopy_clumping", "canopy_leaf_size",
                            "canopy_seed", "canopy_rotation"})
        show(key, pattern == 1 && advanced);
    for (const char *key : {"leaf_variation", "leaf_edge_amount", "leaf_tint", "leaf_shadow_variation"})
        show(key, pattern == 1 && surface);
    show("leaf_shimmer", pattern == 1);
    show("edge_color", surface && (pattern == 1 || (type == 0 && pattern == 0)));
    return changed;
}
obs_properties_t *properties(void *data) {
    auto *p = obs_properties_create();
    auto *light = static_cast<LightState *>(data);
    if (light)
        ++light->property_views;
    obs_properties_set_param(p, new LightProperties{light, obs_source_get_ref(light_source(data))},
                             release_properties);
    const auto status = light_status(light_source(data));
    // OBS renders a long description on INFO as a second text column, not
    // as a tooltip. Keep status in a single wrapping field.
    obs_properties_add_text(p, "status", status.c_str(), OBS_TEXT_INFO);
    if (light) {
        std::lock_guard<std::mutex> lock(light->mutex);
        light->status_text = status;
    }
    obs_properties_add_text(
        p, "placement_help",
        "View the scene preview to see the light. Move, resize and rotate its source there.", OBS_TEXT_INFO);
    obs_properties_add_text(p, "ownership_help",
                            "One active Glowbox Light per scene. Shared sources share settings. Move blends "
                            "each scene's light automatically; no light-to-light link is needed.",
                            OBS_TEXT_INFO);
    auto *type =
        obs_properties_add_list(p, "light_category", "Effect", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(type, "Spotlight", 0);
    obs_property_list_add_int(type, "Wash", 1);
    obs_property_list_add_int(type, "Directional", 2);
    obs_property_list_add_int(type, "Foliage", 3);
    obs_property_set_modified_callback(type, category_changed);
    obs_property_set_long_description(
        type, "Each effect remembers its own settings. Spotlight makes a local pool of light. Wash blends "
              "color. Directional casts parallel light across an area. Foliage adds moving leaf shadows.");
    obs_properties_add_text(p, "effect_help", "", OBS_TEXT_INFO);
    auto *coverage =
        obs_properties_add_list(p, "light_coverage", "Coverage", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(coverage, "Source Size", 0);
    obs_property_list_add_float(coverage, "Whole Scene", 1);
    obs_property_set_modified_callback(coverage, controls_changed);
    auto *looks = obs_properties_create();
    obs_properties_add_group(p, "starting_look", "Starting Look", OBS_GROUP_NORMAL, looks);
    auto *surface = obs_properties_create();
    obs_properties_add_group(p, "light_shade", "Light & Shade", OBS_GROUP_NORMAL, surface);
    auto *color = obs_properties_add_color(surface, "color", "Light Color");
    obs_property_set_long_description(color, "The color of the light.");

    auto *surface_group = obs_properties_add_bool(surface, "surface_lighting", "Light Surfaces");
    auto *brightness = obs_properties_add_float_slider(surface, "brightness", "Intensity", 0, 2, .01);
    obs_property_set_long_description(brightness, "Raise the illumination inside the light. Use Opacity to "
                                                  "fade the complete treatment, including darkening.");
    auto *wash = obs_properties_add_float_slider(surface, "wash_intensity", "Intensity", 0, 1, .001);
    obs_property_set_long_description(wash,
                                      "Set the strength of the wash. The curve gives finer control at low "
                                      "values. Use Opacity to fade the complete brightening and darkening.");
    auto *opacity = obs_properties_add_float_slider(surface, "opacity", "Opacity", 0, 1, .01);
    obs_property_set_long_description(opacity,
                                      "Fade all light, shade and rays together. 0 removes the effect.");
    auto *darkness = obs_properties_add_float_slider(surface, "darkness", "Outside Darkness", 0, 8, .05);
    obs_property_set_long_description(
        darkness,
        "Darken areas outside the Spotlight and inside its shadow. 0 leaves their brightness unchanged.");
    obs_properties_add_color(surface, "leaf_shadow_color", "Shadow Color");
    obs_properties_add_float_slider(surface, "leaf_shadow_level", "Shade Brightness", 0, 1, .01);
    auto *edge = obs_properties_add_color(surface, "edge_color", "Transition Color");
    obs_property_set_long_description(edge, "Color along a Spotlight edge or through partly lit leaves. "
                                            "Transition Tint sets the foliage amount.");
    obs_properties_add_float_slider(surface, "leaf_edge_amount", "Transition Tint", 0, 1, .01);
    obs_properties_add_color(surface, "leaf_tint", "Reflected Color");
    obs_properties_add_float_slider(surface, "leaf_variation", "Reflected Color Amount", 0, 1, .01);
    obs_properties_add_float_slider(surface, "leaf_shadow_variation", "Shade Variation", 0, 1, .01);
    obs_property_set_modified_callback(surface_group, controls_changed);
    obs_property_set_long_description(
        surface_group, "Change the lighting on scene objects. Turn off to show rays alone. In Source "
                       "Controls, Receive Glowbox Light adjusts this treatment for each object.");

    obs_properties_add_float_slider(surface, "pattern_amount", "Pattern Strength", 0, 1, .01);
    auto *leaves = obs_properties_create();
    auto *start = obs_properties_add_list(looks, "canopy_start", "Apply Starting Look", OBS_COMBO_TYPE_LIST,
                                          OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(start, "Choose a starting point", 0);
    int id = 0;
    for (const char *name :
         {"Default Canopy", "Soft Canopy", "Fine Branches", "Broad Foliage", "Open Boughs", "Layered Shade"})
        obs_property_list_add_int(start, name, ++id);
    obs_property_set_modified_callback(start, canopy_preset);
    obs_property_set_long_description(
        start,
        "Replace the canopy layout and lighting colors with a starting look. Wind settings stay unchanged.");
    obs_properties_add_float_slider(leaves, "canopy_scale", "Pattern Scale", .5, 2, .01);
    obs_properties_add_float_slider(leaves, "canopy_coverage", "Canopy Coverage", 0, 1, .01);
    obs_properties_add_float_slider(leaves, "leaf_softness", "Shadow Softness", 0, 1, .01);
    obs_properties_add_float_slider(leaves, "leaf_depth", "Leaf Layer Depth", 0, 1, .01);
    obs_properties_add_button(leaves, "canopy_shuffle", "New Canopy", shuffle_canopy);
    obs_properties_add_group(p, "leaf_controls", "Canopy", OBS_GROUP_NORMAL, leaves);
    auto *motion = obs_properties_create();
    obs_properties_add_group(p, "leaf_motion", "Wind", OBS_GROUP_NORMAL, motion);
    obs_properties_add_float_slider(motion, "leaf_speed", "Wind Speed", 0, 1, .01);
    obs_properties_add_float_slider(motion, "leaf_sway", "Wind Strength", 0, 1, .01);
    obs_properties_add_float_slider(motion, "leaf_shimmer", "Leaf Flutter", 0, 1, .01);
    auto *shape = obs_properties_create();
    auto *softness = obs_properties_add_float_slider(shape, "softness", "Edge Softness", .01, 1, .01);
    obs_property_set_long_description(softness,
                                      "Soften the edge of the light's area. This also affects rays. ");
    auto *shape_group = obs_properties_add_group(p, "shape", "Shape", OBS_GROUP_NORMAL, shape);
    obs_property_set_long_description(
        shape_group, "Resize the source to change coverage. Rotate Wash or Directional to cast from the top "
                     "of its box toward the bottom. Spotlight rotation changes the direction of Tilt.");

    auto *rays = obs_properties_create();
    auto *amount = obs_properties_add_float_slider(rays, "rays", "Amount", 0, 1, .01);
    obs_property_set_long_description(
        amount, "Show light in the air behind the subject. 0 hides rays. Opacity fades the whole light.");
    auto *width = obs_properties_add_float_slider(rays, "ray_width", "Width", .05, 1, .01);
    obs_property_set_long_description(
        width, "Set the width of individual rays. Resize the source in OBS to change the area they cover.");
    auto *movement = obs_properties_add_float_slider(rays, "ray_movement", "Drift Speed", 0, 1, .01);
    obs_property_set_long_description(movement, "Set how quickly the ray pattern drifts. 0 keeps it still.");
    auto *rays_group = obs_properties_add_group(p, "rays_controls", "Rays", OBS_GROUP_NORMAL, rays);
    obs_property_set_long_description(
        rays_group, "Rays stay behind the selected subject. Receive Rays in Source Controls keeps other "
                    "sources clear. Rays are available for Wash, Spotlight and Directional.");
    auto *advanced = obs_properties_add_bool(p, "advanced", "Show Advanced Controls");
    obs_property_set_modified_callback(advanced, controls_changed);
    auto *advanced_controls = obs_properties_create();
    obs_properties_add_group(p, "advanced_controls", "Advanced Controls", OBS_GROUP_NORMAL,
                             advanced_controls);
    obs_properties_add_float_slider(advanced_controls, "canopy_curve", "Branch Curves", 0, 1, .01);
    obs_properties_add_float_slider(advanced_controls, "canopy_forks", "Branching", 0, 1, .01);
    obs_properties_add_float_slider(advanced_controls, "canopy_clumping", "Leaf Clusters", 0, 1, .01);
    obs_properties_add_float_slider(advanced_controls, "canopy_leaf_size", "Leaf Size", .5, 1.5, .01);
    obs_properties_add_int(advanced_controls, "canopy_seed", "Seed", 0, 2147483647, 1);
    obs_properties_add_float_slider(advanced_controls, "canopy_rotation", "Pattern Angle", -180, 180, 1);
    obs_properties_add_float_slider(advanced_controls, "motion_drift", "Ray Sway", 0, 1, .01);
    obs_properties_add_float_slider(advanced_controls, "motion_opposing", "Crossing Rays", 0, 1, .01);
    obs_properties_add_float_slider(advanced_controls, "motion_fading", "Ray Fading", 0, 1, .01);
    auto *tilt = obs_properties_add_float_slider(advanced_controls, "tilt", "Tilt", 0, 80, 1);
    obs_property_set_long_description(
        tilt, "Tilt the Spotlight beam. Rotate the source in OBS to change the direction of the tilt.");
    auto *fade = obs_properties_add_float_slider(advanced_controls, "falloff", "Distance Fade", 0, 1, .01);
    obs_property_set_long_description(
        fade, "Fade light and rays with distance.  0 keeps the light even along its path.");
    auto *variation =
        obs_properties_add_float_slider(advanced_controls, "ray_detail", "Variation", 0, 1, .01);
    obs_property_set_long_description(variation,
                                      "Break the light into uneven rays. 0 produces an even haze.");
    auto *blend = obs_properties_add_list(advanced_controls, "wash_blend", "Blend Mode", OBS_COMBO_TYPE_LIST,
                                          OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(blend, "Add", 0);
    obs_property_list_add_int(blend, "Screen", 1);
    obs_property_list_add_int(blend, "Color Dodge", 2);

    const struct {
        const char *key, *help;
    } help[] = {
        {"pattern_amount", "Fade the patterned surface lighting."},
        {"leaf_shadow_color", "The color of light in shaded areas."},
        {"canopy_scale", "Resize the pattern. Move can animate this without regenerating the branches."},
        {"canopy_rotation",
         "Turn the pattern inside the Light source. Source rotation also turns the whole light."},
        {"canopy_coverage", "Grow the connected canopy across the scene. 1 gives full coverage. This changes "
                            "structure; animate Opacity or Pattern Scale with Move for a continuous change."},
        {"canopy_curve", "Bend the branches while keeping their leaves attached."},
        {"canopy_forks", "Add smaller branches to the canopy."},
        {"canopy_clumping", "Gather leaves into denser groups with open gaps between them."},
        {"canopy_leaf_size", "Change individual leaves. Pattern Scale resizes the whole pattern."},
        {"canopy_seed", "Save this number and the other foliage settings to recreate the same canopy. New "
                        "Canopy changes only this number."},
        {"canopy_shuffle", "Try another arrangement while keeping your colours, wind and other settings."},
        {"leaf_softness", "Blur the leaf shadows. Lower values show sharper leaves."},
        {"leaf_speed", "Set how fast the canopy moves. 0 pauses it in place."},
        {"leaf_depth",
         "Vary shadow softness and wind across the canopy. Higher values add distance between leaf layers."},
        {"leaf_shadow_variation",
         "Deepen ambient shade beneath denser foliage. Does not make leaves transparent."},
        {"leaf_sway", "How far branches and leaf clusters move."},
        {"leaf_shimmer", "Small leaf movements that change the light gaps."},
        {"leaf_variation", "Add soft reflected colour near the light gaps."},
        {"leaf_tint", "The reflected colour from surrounding leaves."},
        {"leaf_shadow_level", "Keep more or less light in the shade. This does not change leaf density."},
        {"leaf_edge_amount", "Blend Transition Color through broad, partly lit areas. 0 removes the tint."},
        {"motion_drift", "Let rays sway sideways."},
        {"motion_opposing", "Let some rays drift in the opposite direction."},
        {"motion_fading", "Let individual rays fade out and reappear."}};
    for (const auto &entry : help)
        obs_property_set_long_description(obs_properties_get(p, entry.key), entry.help);
    auto *s = data ? obs_source_get_settings(light_source(data)) : obs_data_create();
    controls_changed(p, nullptr, s);
    obs_data_release(s);
    add_glowbox_footer(p);
    return p;
}
struct Search {
    matrix4 parent;
    obs_source_t *source = nullptr;
    vec2 centre = {};
    float radius = 0, direction = 0;
    unsigned count = 0, depth = 0;
    obs_source_t *target = nullptr;
    bool found = false;
    Search() = default;
    Search(const Search &) = delete;
    Search &operator=(const Search &) = delete;
    ~Search() {
        obs_source_release(source);
    }
    void select(obs_source_t *candidate) {
        auto *next = obs_source_get_ref(candidate);
        obs_source_release(source);
        source = next;
    }
};
// OBS holds the scene's video/audio locks during enumeration. Retain items in
// that callback, then release those locks before transforms, signals or groups.
struct LightItems {
    std::vector<obs_sceneitem_t *> items;
    ~LightItems() {
        for (auto *item : items)
            obs_sceneitem_release(item);
    }
};
bool retain_light_item(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    if (obs_sceneitem_visible(item)) {
        obs_sceneitem_addref(item);
        static_cast<LightItems *>(data)->items.push_back(item);
    }
    return true;
}
void scan_lights(obs_scene_t *, Search &);
bool find_light(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    auto &s = *static_cast<Search *>(data);
    if (!obs_sceneitem_visible(item))
        return true;
    auto *source = obs_sceneitem_get_source(item);
    if (!strcmp(obs_source_get_unversioned_id(source), light_id)) {
        ++s.count;
        if (source == s.target)
            s.found = true;
        obs_sceneitem_crop crop{};
        obs_sceneitem_get_crop(item, &crop);
        if (crop.left || crop.top || crop.right || crop.bottom)
            return true;
        matrix4 box, world;
        obs_sceneitem_force_update_transform(item);
        obs_sceneitem_get_box_transform(item, &box);
        matrix4_mul(&world, &box, &s.parent);
        vec3 a, b, c, m;
        vec3_set(&a, 0, 0, 0);
        vec3_set(&b, 1, 0, 0);
        vec3_set(&c, 0, 1, 0);
        vec3_set(&m, .5f, .5f, 0);
        vec3_transform(&a, &a, &world);
        vec3_transform(&b, &b, &world);
        vec3_transform(&c, &c, &world);
        vec3_transform(&m, &m, &world);
        const float radius =
            .5f * std::min(std::hypot(b.x - a.x, b.y - a.y), std::hypot(c.x - a.x, c.y - a.y));
        if (!std::isfinite(radius) || radius <= 0)
            return true;
        s.select(source);
        s.centre = {m.x, m.y};
        s.radius = radius;
        s.direction = std::atan2(b.y - a.y, b.x - a.x) * 57.295779513f;
    } else if (obs_sceneitem_is_group(item) && s.depth < 16) {
        Search nested;
        matrix4 draw;
        obs_sceneitem_get_draw_transform(item, &draw);
        matrix4_mul(&nested.parent, &draw, &s.parent);
        nested.depth = s.depth + 1;
        nested.target = s.target;
        scan_lights(obs_sceneitem_group_get_scene(item), nested);
        if (nested.source) {
            s.select(nested.source);
            s.centre = nested.centre;
            s.radius = nested.radius;
            s.direction = nested.direction;
        }
        s.count += nested.count;
        s.found = s.found || nested.found;
    }
    return true;
}
void scan_lights(obs_scene_t *scene, Search &search) {
    LightItems snapshot;
    obs_scene_enum_items(scene, retain_light_item, &snapshot);
    for (auto *item : snapshot.items)
        find_light(scene, item, &search);
}
struct Connection {
    obs_source_t *light;
    std::vector<std::string> messages;
    unsigned scenes = 0;
};
void find_compositor(obs_source_t *, obs_source_t *filter, void *data) {
    if (!strcmp(obs_source_get_unversioned_id(filter), "glowbox_compositor_filter") &&
        obs_source_enabled(filter) && obs_filter_get_target(filter) == obs_filter_get_parent(filter))
        *static_cast<bool *>(data) = true;
}
bool inspect_connection(void *data, obs_source_t *source) {
    auto &c = *static_cast<Connection *>(data);
    auto *scene = obs_scene_from_source(source);
    if (!scene)
        return true;
    Search s;
    s.target = c.light;
    matrix4_identity(&s.parent);
    scan_lights(scene, s);
    if (!s.found)
        return true;
    ++c.scenes;
    const std::string name = obs_source_get_name(source);
    bool connected = false;
    obs_source_enum_filters(source, find_compositor, &connected);
    if (!connected)
        c.messages.push_back(name + ": add and enable Glowbox Compositor first in the scene's filter list.");
    else if (!s.source || s.radius <= 0)
        c.messages.push_back(name + ": remove cropping and give the Light source a nonzero size.");
    else if (s.source != c.light)
        c.messages.push_back(name + ": inactive. Using " + obs_source_get_name(s.source) +
                             ". Hide other lights or move this uncropped light above them.");
    else {
        c.messages.push_back("Connected to " + name + " Compositor.");
        if (s.count > 1)
            c.messages.push_back(name +
                                 ": only this light is active. Other visible Glowbox Lights are ignored.");
    }
    return true;
}
std::string light_status(obs_source_t *source) {
    if (!source)
        return "Add this light to a scene with Glowbox Compositor.";
    Connection c{};
    c.light = source;
    obs_enum_scenes(inspect_connection, &c);
    if (!c.scenes)
        return "Not connected. Show this light in a scene with Glowbox Compositor.";
    std::string text;
    for (const auto &message : c.messages) {
        if (!text.empty())
            text += '\n';
        text += message;
    }
    if (c.scenes > 1)
        text += "\nShared by " + std::to_string(c.scenes) + " scenes. Changes affect all of them.";
    return text;
}

} // namespace
bool scene_light_present(obs_scene_t *scene) {
    if (!scene)
        return false;
    Search search;
    matrix4_identity(&search.parent);
    scan_lights(scene, search);
    return search.source && search.radius > 0;
}
bool resolve_scene_light(Compositor *f, obs_scene_t *scene) {
    f->lighting.rays_amount = 0;
    f->lighting.spotlight_amount = 0;
    f->lighting.lightwash_intensity = 0;
    f->lighting.light_pattern = 0;
    if (!scene)
        return false;
    Search search;
    matrix4_identity(&search.parent);
    scan_lights(scene, search);
    if (!search.count) {
        f->light_conflict_logged = false;
        return false;
    }
    const bool valid = search.source && search.radius > 0;
    if (!valid) {
        if (!f->light_conflict_logged)
            blog(LOG_WARNING, "[Glowbox] No usable Light placement: remove cropping and check its size");
        f->light_conflict_logged = true;
        return false;
    }
    if (search.count > 1 && !f->light_conflict_logged)
        blog(LOG_WARNING, "[Glowbox] Multiple visible Lights: using topmost usable light '%s'",
             obs_source_get_name(search.source));
    f->light_conflict_logged = search.count > 1;
    const float w = (float)obs_source_get_width(obs_scene_get_source(scene));
    const float h = (float)obs_source_get_height(obs_scene_get_source(scene));
    if (w <= 0 || h <= 0)
        return false;
    auto *s = obs_source_get_settings(search.source);
    f->lighting.spotlight_amount = 1;
    f->lighting.spotlight_x = search.centre.x / w;
    f->lighting.spotlight_y = search.centre.y / h;
    f->lighting.spotlight_size = std::max(.001f, search.radius / h);
    f->lighting.spotlight_softness = bounded_setting(s, "softness", .01f, 1);
    f->lighting.spotlight_tilt = bounded_setting(s, "tilt", 0, 80);
    f->lighting.spotlight_direction = search.direction;
    f->lighting.spotlight_falloff = bounded_setting(s, "falloff", 0, 1);
    f->lighting.spotlight_brightness = bounded_setting(s, "brightness", 0, 2);
    f->lighting.spotlight_darkness = bounded_setting(s, "darkness", 0, 8);
    f->lighting.light_type = (float)std::clamp((int)obs_data_get_int(s, "light_type"), 0, 2);
    f->lighting.ray_width = bounded_setting(s, "ray_width", .05f, 1);
    f->lighting.ray_movement = bounded_setting(s, "ray_movement", 0, 1);
    f->lighting.rays_amount = bounded_setting(s, "rays", 0, 1);
    f->lighting.light_pattern = (float)(int)obs_data_get_double(s, "light_pattern");
    // The current ray pass cannot represent canopy scattering.

    if (f->lighting.light_pattern > 0)
        f->lighting.rays_amount = 0;
    if (auto *light = static_cast<LightState *>(obs_obj_get_data(search.source))) {
        std::lock_guard<std::mutex> lock(light->mutex);
        f->lighting.leaf_time_offset = (float)light->leaf.offset;
        f->lighting.ray_time_offset = (float)light->rays.offset;
    }
    f->lighting.leaf_shadow_variation = bounded_setting(s, "leaf_shadow_variation", 0, 1);
    f->lighting.leaf_bottom_fade = bounded_setting(s, "leaf_bottom_fade", 0, 1);
    f->lighting.canopy_coverage = bounded_setting(s, "canopy_coverage", 0, 1);
    f->lighting.canopy_curve = bounded_setting(s, "canopy_curve", 0, 1);
    f->lighting.canopy_forks = bounded_setting(s, "canopy_forks", 0, 1);
    f->lighting.canopy_clumping = bounded_setting(s, "canopy_clumping", 0, 1);
    f->lighting.canopy_leaf_size = bounded_setting(s, "canopy_leaf_size", .5f, 1.5f);
    f->lighting.canopy_seed = (uint32_t)std::clamp(obs_data_get_int(s, "canopy_seed"), 0LL, 4294967295LL);
    f->lighting.canopy_scale = bounded_setting(s, "canopy_scale", .5f, 2);
    f->lighting.canopy_rotation = bounded_setting(s, "canopy_rotation", -180, 180);
    f->lighting.leaf_softness = bounded_setting(s, "leaf_softness", 0, 1);
    f->lighting.leaf_depth = bounded_setting(s, "leaf_depth", 0, 1);
    f->lighting.leaf_speed = bounded_setting(s, "leaf_speed", 0, 1);
    f->lighting.leaf_sway = bounded_setting(s, "leaf_sway", 0, 1);
    f->lighting.leaf_shimmer = bounded_setting(s, "leaf_shimmer", 0, 1);
    f->lighting.leaf_variation = bounded_setting(s, "leaf_variation", 0, 1);
    f->lighting.leaf_shadow_level = bounded_setting(s, "leaf_shadow_level", 0, 1);
    f->lighting.leaf_edge_amount = bounded_setting(s, "leaf_edge_amount", 0, 1);
    vec4_from_rgba(&f->lighting.leaf_shadow_color, (uint32_t)obs_data_get_int(s, "leaf_shadow_color"));
    vec4_from_rgba(&f->lighting.leaf_tint, (uint32_t)obs_data_get_int(s, "leaf_tint"));
    f->lighting.pattern_size =
        f->lighting.light_pattern == 1 ? .35f : bounded_setting(s, "pattern_size", .05f, 1);
    f->lighting.pattern_amount = bounded_setting(s, "pattern_amount", 0, 1);
    f->lighting.motion_drift = bounded_setting(s, "motion_drift", 0, 1);
    f->lighting.motion_opposing = bounded_setting(s, "motion_opposing", 0, 1);
    f->lighting.motion_fading = bounded_setting(s, "motion_fading", 0, 1);
    f->lighting.light_coverage = bounded_setting(s, "light_coverage", 0, 1);
    f->lighting.light_opacity = bounded_setting(s, "opacity", 0, 1);
    f->lighting.rays_detail = bounded_setting(s, "ray_detail", 0, 1);
    f->lighting.spotlight_surface =
        obs_data_get_bool(s, "surface_lighting") ? bounded_setting(s, "opacity", 0, 1) : 0.f;
    vec4_from_rgba(&f->lighting.spotlight_light_color, (uint32_t)obs_data_get_int(s, "color"));
    vec4_from_rgba(&f->lighting.spotlight_color, (uint32_t)obs_data_get_int(s, "edge_color"));
    if (f->lighting.light_type == 1) {
        const float intensity = bounded_setting(s, "wash_intensity", 0, 1);
        f->lighting.lightwash_intensity = intensity * intensity;
        f->lighting.lightwash_opacity = f->lighting.spotlight_surface;
        f->lighting.lightwash_size = std::clamp(search.radius / h * 2.f, 0.f, 2.f);
        f->lighting.lightwash_angle = search.direction * .01745329252f;
        f->lighting.lightwash_color = f->lighting.spotlight_light_color;
        f->lighting.lightwash_type = std::clamp((int)obs_data_get_int(s, "wash_blend"), 0, 2);
    }
    obs_data_release(s);
    f->wants_layers = true;
    return true;
}
SceneLightScope::SceneLightScope(Compositor *f, obs_scene_t *scene)
    : filter(f), lighting(f->lighting), layers(f->wants_layers), shadow_follow(f->shadow_follow) {
    if (!f->scene_capture_callback)
        resolve_scene_light(f, scene);
}
SceneLightScope::~SceneLightScope() {
    filter->lighting = lighting;
    filter->wants_layers = layers;
    filter->shadow_follow = shadow_follow;
}
void register_light_source() {
    obs_source_info info{};
    info.id = light_id;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = light_name;
    info.create = create_light;
    info.destroy = destroy_light;
    info.video_tick = tick_light;
    info.update = update_light;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.video_render = draw_light;
    info.get_width = light_size;
    info.get_height = light_size;
    obs_register_source(&info);
}
} // namespace glowbox
