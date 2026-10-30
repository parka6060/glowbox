#include "compositor/compositor.hpp"
#include "properties_ui.hpp"

namespace glowbox {
static const char *subject_unavailable(obs_source_t *filter) {
    auto *parent = filter ? obs_filter_get_parent(filter) : nullptr;
    auto *scene = parent ? obs_scene_from_source(parent) : nullptr;
    if (!scene)
        return "Add Glowbox to a scene to choose Subject items.";
    if (obs_filter_get_target(filter) != parent)
        return "Move Glowbox first in the filter list to use Subject controls.";
    obs_video_info video = {};
    if (obs_get_video_info(&video) && (obs_source_get_base_width(parent) != video.base_width ||
                                       obs_source_get_base_height(parent) != video.base_height))
        return "Subject effects need a scene matching the OBS canvas size.";
    const gs_color_space preferred[] = {GS_CS_SRGB};
    if (obs_source_get_color_space(parent, 1, preferred) != GS_CS_SRGB)
        return "Glowbox currently supports SDR scenes only.";
    return nullptr;
}
static bool scene_light_available(obs_properties_t *p) {

    auto *filter=static_cast<obs_source_t *>(obs_properties_get_param(p));
    return filter && scene_light_present(obs_scene_from_source(obs_filter_get_parent(filter)));
}
static bool rim_direction_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) {
    auto *toggle=obs_properties_get(p,"rim_follow_light");
    auto *angle=obs_properties_get(p,"rim_angle");
    if(!toggle || !angle)return false;
    const bool visible=obs_property_visible(toggle);
    const int style=(int)obs_data_get_int(s,"rim_type");
    const int light=(int)obs_data_get_int(s,"rim_follow_light");
    const bool available=(style==0 || style==2);
    const bool following=available && ((light==1 && property_source_available(s,"light_source")) ||
        (light==2 && scene_light_available(p)));
    auto *status = obs_properties_get(p, "rim_link_status");
    obs_property_set_description(status, following
        ? (light == 2 ? "Linked to Glowbox Light. Spotlight also supplies its transition color." : "Direction follows the reactive source.")
        : "Linked source unavailable. Using the saved manual angle and color.");
    obs_property_set_visible(status, visible && available && light != 0);
    obs_property_set_enabled(toggle,available);
    obs_property_set_visible(angle,visible && !following);
    return true;
}

static bool shadow_direction_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) {
    const bool manual = obs_data_get_int(s,"shadow_follow") == 0 || !scene_light_available(p);
    auto *status = obs_properties_get(p, "shadow_link_status");
    obs_property_set_description(status, manual
        ? "Glowbox Light unavailable. Using the saved manual shadow."
        : "Linked to Glowbox Light. Subject Depth controls projection and size.");
    obs_property_set_visible(status, obs_data_get_bool(s, "shadow_enabled") && obs_data_get_int(s, "shadow_follow") != 0 &&
        obs_property_enabled(obs_properties_get(p, "shadow_enabled")));
    for (const char *key : {"shadow_size", "shadow_distance", "shadow_angle"}) {
        auto *control = obs_properties_get(p,key);
        if (control) obs_property_set_enabled(control,manual);
    }
    return true;
}
static bool effect_enabled_changed(obs_properties_t *p, obs_property_t *toggle, obs_data_t *settings) {
    auto *controls = obs_property_group_content(toggle);
    const bool visible = obs_data_get_bool(settings, obs_property_name(toggle)) &&
                         obs_property_enabled(toggle);
    for (auto *item = obs_properties_first(controls); item; obs_property_next(&item))
        obs_property_set_visible(item, visible);
    if(!strcmp(obs_property_name(toggle),"rim_enabled") || !strcmp(obs_property_name(toggle),"spotlight_enabled"))rim_direction_changed(p,nullptr,settings);
    shadow_direction_changed(p,nullptr,settings);
    return true;
}
static bool update_subject_summary(obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
    auto *group = obs_properties_get(p, "subjects");
    auto *items = group ? obs_property_group_content(group) : p;
    int count = 0;
    std::string names;
    for (auto *item = obs_properties_first(items); item; obs_property_next(&item)) {
        const char *key = obs_property_name(item);
        if (obs_property_get_type(item) == OBS_PROPERTY_BOOL && strncmp(key, "subject_", 8) == 0 &&
            obs_data_get_bool(settings, key)) {
            if (count < 2) {
                if (count)
                    names += ", ";
                names += obs_property_description(item);
            }
            ++count;
        }
    }
    std::string label = count ? std::to_string(count) + " selected: " + names +
                                    (count > 2 ? " and " + std::to_string(count - 2) + " more" : "")
                              : "No subject selected. All items are background.";
    obs_property_set_description(obs_properties_get(items, "subject_summary"), label.c_str());
    const char *reason = subject_unavailable(static_cast<obs_source_t *>(obs_properties_get_param(items)));
    auto *camera = obs_properties_get(p, "camera");
    if (camera) obs_property_set_enabled(camera, reason == nullptr);
    auto *status = obs_properties_get(items, "layer_status");
    obs_property_set_description(status, reason ? reason : "");
    obs_property_set_visible(status, reason != nullptr);
    const bool selected = count > 0 && !reason;
    const bool background_receives = obs_data_get_bool(settings, "light_background") ||
        (obs_data_get_bool(settings, "follow_brightness") && obs_data_get_bool(settings, "exposure_background"));
    const bool background_active = selected || (!reason && background_receives);
    obs_property_set_enabled(obs_properties_get(items, "subject_exposure"), selected);
    obs_property_set_enabled(obs_properties_get(items, "background_exposure"), background_active);
    obs_property_set_visible(obs_properties_get(items, "subject_exposure"), selected);
    obs_property_set_visible(obs_properties_get(items, "background_exposure"), background_active);
    const bool source_selected = *obs_data_get_string(settings, "light_source") != '\0';
    const bool has_light = property_source_available(settings, "light_source");
    obs_property_set_visible(obs_properties_get(p, "reactive_color"), has_light);
    obs_property_set_visible(obs_properties_get(p, "follow_brightness"), has_light);
    obs_property_set_visible(obs_properties_get(p, "reactive_smoothing"), has_light);
    obs_property_set_enabled(obs_properties_get(p, "follow_brightness"), has_light);
    obs_property_set_visible(obs_properties_get(p, "light_background"), has_light);
    auto *light_status = obs_properties_get(p, "light_status");
    obs_property_set_description(light_status, source_selected && !has_light
        ? "Source unavailable. Choose another source under React To."
        : "Select subject items or enable a background option to receive light.");
    obs_property_set_visible(light_status, (source_selected && !has_light) ||
        (has_light && !reason && !selected && !background_receives));
    auto *rim_toggle = obs_properties_get(p, "rim_enabled");
    obs_property_set_enabled(rim_toggle, selected);
    effect_enabled_changed(p, rim_toggle, settings);
    auto *shadow_toggle = obs_properties_get(p,"shadow_enabled");
    if (shadow_toggle) {
        obs_property_set_enabled(shadow_toggle, selected);
        effect_enabled_changed(p,shadow_toggle,settings);
    }
    return true;
}
static bool toggle_subject_selection(obs_properties_t *p, obs_property_t *button, void *) {
    auto *group = obs_properties_get(p, "subjects");
    auto *items = group ? obs_property_group_content(group) : p;
    bool show = !obs_property_visible(obs_properties_get(items, "subject_help"));
    for (auto *item = obs_properties_first(items); item; obs_property_next(&item)) {
        const char *key = obs_property_name(item);
        if ((obs_property_get_type(item) == OBS_PROPERTY_BOOL && strncmp(key, "subject_", 8) == 0) ||
            strcmp(key, "subject_help") == 0 || strcmp(key, "refresh_subjects") == 0)
            obs_property_set_visible(item, show);
    }
    obs_property_set_description(button, show ? "Done" : "Select Subject");
    return true;
}
static bool add_subject_item_property(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    if (!strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)),"glowbox_light_source")) return true;
    auto key = subject_setting_key(item);
    std::string label = obs_source_get_name(obs_sceneitem_get_source(item));
    const std::string base = label;
    int occurrence = 1;
    auto *filter_properties = (obs_properties_t *)data;
    bool duplicate;
    do {
        duplicate = false;
        for (auto *existing = obs_properties_first(filter_properties); existing;
             obs_property_next(&existing)) {
            if (obs_property_get_type(existing) == OBS_PROPERTY_BOOL &&
                label == obs_property_description(existing)) {
                duplicate = true;
                label = base + " (" + std::to_string(++occurrence) + ")";
                break;
            }
        }
    } while (duplicate);
    auto *property = obs_properties_add_bool((obs_properties_t *)data, key.c_str(), label.c_str());
    obs_property_set_visible(property, false);
    obs_property_set_modified_callback(property, update_subject_summary);
    return true;
}
static bool refresh_subject_items(obs_properties_t *, obs_property_t *, void *data) {
    // The signal can destroy the active button handler synchronously.
    // Returning false prevents OBS from accessing that handler again.
    obs_source_update_properties((obs_source_t *)data);
    return false;
}

static bool add_scene_light(obs_properties_t *p, obs_property_t *button, void *) {
    auto *filter=static_cast<obs_source_t *>(obs_properties_get_param(p));
    auto *owner=filter?obs_filter_get_parent(filter):nullptr;
    auto *scene=obs_scene_from_source(owner);
    if (!scene || scene_light_present(scene)) return false;
    const std::string base=std::string(obs_source_get_name(owner))+" Light";
    std::string name=base;
    for (unsigned i=2;;++i) {
        auto *existing=obs_get_source_by_name(name.c_str());
        if (!existing) break;
        obs_source_release(existing);name=base+" "+std::to_string(i);
    }
    auto *light=obs_source_create("glowbox_light_source",name.c_str(),nullptr,nullptr);
    if (!light) return false;
    auto *item=obs_scene_add(scene,light);
    if (item) {
        obs_transform_info transform{};
        transform.alignment=OBS_ALIGN_CENTER;
        transform.pos.x=obs_source_get_width(owner)*.5f;
        transform.pos.y=obs_source_get_height(owner)*.5f;
        transform.scale.x=transform.scale.y=std::max(1.f,obs_source_get_height(owner)*.75f)/512.f;
        obs_sceneitem_set_info2(item,&transform);
        obs_sceneitem_select(item,true);
        obs_property_set_description(button,"Light Added. Edit It in Sources.");
        obs_property_set_enabled(button,false);
    }
    obs_source_release(light);
    return item != nullptr;
}
static bool source_option(void *property, obs_source_t *source) {
    if (!strcmp(obs_source_get_unversioned_id(source),"glowbox_light_source")) return true;
    std::string id = std::string("uuid:") + obs_source_get_uuid(source);
    obs_property_list_add_string((obs_property_t *)property, obs_source_get_name(source), id.c_str());
    return true;
}
static bool follow_brightness_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) {
    const bool enabled = obs_data_get_bool(s, "follow_brightness");
    for (const char *key : {"follow_darken", "follow_brighten", "exposure_background"})
        obs_property_set_visible(obs_properties_get(p, key), enabled);
    return update_subject_summary(p, nullptr, s);
}
static bool mode_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) {
    int mode = (int)obs_data_get_int(s, "screen_mode");
    const char *intensity_help = "Sets the strength of the selected Screen Effect. Use Opacity to fade it.";
    if (mode == 1)
        intensity_help = "Sets how strongly Soft Light changes the scene. Use Opacity to fade it.";
    else if (mode == 2)
        intensity_help = "Sets how much light spreads through the scene. Use Opacity to fade it.";
    else if (mode == 3)
        intensity_help = "Sets glow strength and color saturation. Use Opacity to fade the whole effect.";
    obs_property_set_long_description(obs_properties_get(p, "screen_intensity"), intensity_help);
    obs_property_set_visible(obs_properties_get(p, "screen_intensity"), mode != 0);
    obs_property_set_visible(obs_properties_get(p, "screen_opacity"), mode != 0);
    obs_property_set_visible(obs_properties_get(p, "radius"), mode >= 2);
    obs_property_set_visible(obs_properties_get(p, "threshold"), mode == 3);
    obs_property_set_visible(obs_properties_get(p, "screen_contrast"), mode == 2);
    return true;
}
obs_properties_t *filter_properties(void *data) {
    auto *f = (Compositor *)data;
    const unsigned saved_rim = f ? f->rim_ui_state.load(std::memory_order_relaxed) : 7u;
    const int saved_rim_blend = int(saved_rim & 31u);
    auto *p = obs_properties_create();
    obs_properties_set_param(p, f ? f->source : nullptr, nullptr);
    auto *subject_properties = obs_properties_create();
    obs_properties_set_param(subject_properties, f ? f->source : nullptr, nullptr);
    obs_properties_add_group(p, "subjects", "Subject", OBS_GROUP_NORMAL, subject_properties);
    auto *parent = f ? obs_filter_get_parent(f->source) : nullptr;
    auto *scene = parent ? obs_scene_from_source(parent) : nullptr;
    obs_properties_add_text(subject_properties, "subject_summary",
                            "No subject selected. All items are background.", OBS_TEXT_INFO);
    if (scene)
        obs_properties_add_button(subject_properties, "edit_subject", "Select Subject",
                                  toggle_subject_selection);
    if (scene)
        obs_scene_enum_items(scene, add_subject_item_property, subject_properties);
    obs_properties_add_text(subject_properties, "layer_status", "", OBS_TEXT_INFO);
    obs_properties_add_text(subject_properties, "subject_help",
                            "Select subject items. Everything else is background.", OBS_TEXT_INFO);
    if (f)
        obs_properties_add_button2(subject_properties, "refresh_subjects", "Refresh Scene Items",
                                   refresh_subject_items, f->source);
    obs_property_set_visible(obs_properties_get(subject_properties, "subject_help"), false);
    if (f)
        obs_property_set_visible(obs_properties_get(subject_properties, "refresh_subjects"), false);
    obs_properties_add_float_slider(subject_properties, "subject_exposure", "Subject Exposure", -3, 3,
                                    .05);
    obs_properties_add_float_slider(subject_properties, "background_exposure", "Background Exposure", -3,
                                    3, .05);
    obs_properties_add_text(subject_properties, "exclusion_help",
        "To exclude chat or overlays from scene effects, add Glowbox Source Controls to that source.", OBS_TEXT_INFO);
    auto *depth=obs_properties_add_float_slider(subject_properties,"subject_depth","Subject Depth",0,1,.01);
    obs_property_set_long_description(depth,"Set subject separation for parallax, lighting and linked shadows. OBS keeps the reference layout.");
    auto *camera = obs_properties_create();
    auto *camera_group = obs_properties_add_group(p, "camera", "Camera", OBS_GROUP_NORMAL, camera);
    // Hidden controls still load saved offsets.
    obs_property_set_visible(camera_group, false);
    auto *camera_x=obs_properties_add_float_slider(camera, "camera_x", "Horizontal", -.5, .5, .005);
    obs_property_set_long_description(camera_x,"Camera offset in scene-height units. Zero keeps the OBS reference view.");
    auto *camera_y=obs_properties_add_float_slider(camera, "camera_y", "Vertical", -.5, .5, .005);
    obs_property_set_long_description(camera_y,"Camera offset in scene-height units. Zero keeps the OBS reference view.");
    auto *placed = obs_properties_create();
    obs_properties_add_group(p,"placed_light","Glowbox Light",OBS_GROUP_NORMAL,placed);
    obs_properties_add_text(placed,"placed_help","One active light per scene. The topmost usable Glowbox Light in Sources supplies the lighting. Move blends scene lights automatically.",OBS_TEXT_INFO);
    auto *add_light = obs_properties_add_button(placed,"add_scene_light","Add Light to This Scene",add_scene_light);
    obs_property_set_enabled(add_light,scene && !scene_light_present(scene));
    obs_property_set_long_description(add_light,"Create a separate light for this scene. Select it in Sources to edit its look. Reusing an existing source shares its settings across scenes.");
    auto *matching = obs_properties_create();
    obs_properties_add_group(p, "matching", "Color Match", OBS_GROUP_NORMAL, matching);
    auto *reference = obs_properties_add_list(matching, "reference_source", "Match From",
                                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(reference, "Scene Background", "");
    obs_enum_sources(source_option, reference);
    obs_properties_add_float_slider(matching, "auto_blend", "Match Amount", 0, 1, .01);
    obs_property_set_long_description(obs_properties_get(matching, "auto_blend"),
        "Match subject colors to the reference. 0 leaves colors unchanged. Source Controls can set a separate amount for an individual source.");
    auto *reactive = obs_properties_create();
    obs_properties_add_group(p, "reactive", "Reactive Lighting", OBS_GROUP_NORMAL, reactive);
    auto *light_source = obs_properties_add_list(reactive, "light_source", "React To",
                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(light_source, "Off", "");
    obs_enum_sources(source_option, light_source);
    obs_property_set_modified_callback(light_source, update_subject_summary);
    auto *smoothing = obs_properties_add_int_slider(reactive, "reactive_smoothing", "Response Smoothing", 0, 100, 1);
    obs_property_set_long_description(smoothing,
        "Smooth changes in color and automatic brightness. Zero responds immediately. Higher values respond more slowly and soften brief flashes. Default: 25.");
    auto *colored = obs_properties_create();
    obs_properties_add_group(reactive, "reactive_color", "Colored Light", OBS_GROUP_NORMAL, colored);
    auto *strength = obs_properties_add_float_slider(colored, "light_strength", "Color Strength", 0, 4, .05);
    obs_property_set_long_description(strength,
        "Add color from the light source while keeping surface shading. With Follow Source Brightness off, a small brightness lift remains. Turn it on to set limits for the complete reaction.");
    auto *light_background = obs_properties_add_bool(colored, "light_background", "Also Color the Background");
    obs_property_set_modified_callback(light_background, update_subject_summary);
    obs_property_set_long_description(light_background,
        "Let background items receive colored light too. This does not adjust their exposure. Excluded sources keep their selected treatment.");
    auto *brightness = obs_properties_create();
    auto *follow_brightness = obs_properties_add_group(reactive, "follow_brightness", "Follow Source Brightness", OBS_GROUP_CHECKABLE, brightness);
    obs_property_set_modified_callback(follow_brightness, follow_brightness_changed);
    obs_property_set_long_description(follow_brightness,
        "Bright sources brighten receiving items; dark sources dim them. Your existing scene and manual exposure are the starting point. The limits apply to the complete reactive-lighting change.");
    auto *darken = obs_properties_add_float_slider(brightness, "follow_darken", "Maximum Darkening", 0, 2, .01);
    auto *brighten = obs_properties_add_float_slider(brightness, "follow_brighten", "Maximum Brightening", 0, 1, .01);
    obs_property_float_set_suffix(darken, " stops");
    obs_property_float_set_suffix(brighten, " stops");
    obs_property_set_long_description(darken,
        "Limit how much reactive lighting can darken the finished scene. Zero prevents darkening. One stop halves exposure.");
    obs_property_set_long_description(brighten,
        "Limit how much reactive lighting can brighten the finished scene, including its colored light. Zero prevents brightening. One stop doubles exposure. Existing Rim Light, Glowbox Light and Screen Effects keep their own settings.");
    obs_property_set_visible(darken, false);
    obs_property_set_visible(brighten, false);
    auto *exposure_background = obs_properties_add_bool(brightness, "exposure_background", "Also Adjust Background Brightness");
    obs_property_set_modified_callback(exposure_background, update_subject_summary);
    obs_property_set_long_description(exposure_background,
        "Apply the same automatic exposure change to subject and background, around their manual settings. Excluded sources keep their selected treatment.");
    obs_property_set_visible(exposure_background, false);
    obs_property_set_long_description(light_source,
        "Choose the source that drives color and brightness. Its position and size in the scene guide the color spread. Select receiving items in Select Subject, or enable a background option. Choosing a light source does not exclude it from effects.");
    auto *light_status = obs_properties_add_text(reactive, "light_status",
        "Select subject items or enable a background option to receive light.", OBS_TEXT_INFO);
    obs_property_set_visible(light_status, false);
    auto *rim = obs_properties_create();
    auto *rim_toggle = obs_properties_add_group(p, "rim_enabled", "Rim Light", OBS_GROUP_CHECKABLE, rim);
    obs_property_set_modified_callback(rim_toggle, effect_enabled_changed);
    auto *rim_type =
        obs_properties_add_list(rim, "rim_type", "Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    const char *rim_types[] = {"One Side, Soft", "Two Sides, Soft", "One Side, Sharp", "Two Sides, Sharp"};
    for (int i = 0; i < 4; ++i)
        obs_property_list_add_int(rim_type, rim_types[i], i);
    obs_properties_add_color(rim, "rim_color", "Color");
    obs_properties_add_float_slider(rim, "rim_size", "Rim Width", 0, 1, .01);
    obs_properties_add_float_slider(rim, "rim_brightness", "Brightness Boost", 0, 10, .05);
    // Keep saved nonzero grading values editable.
    obs_property_t *surface_contrast = nullptr;
    if (saved_rim & 32u)
        surface_contrast = obs_properties_add_float_slider(rim, "rim_contrast", "Surface Contrast (Saved)",
                                                           0, 1, .01);
    auto *follow=obs_properties_add_list(rim,"rim_follow_light","Link To", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(follow,"Manual",0);
    obs_property_list_add_int(follow,"Reactive Source",1);
    obs_property_list_add_int(follow,"Glowbox Light",2);
    obs_property_set_modified_callback(follow,rim_direction_changed);
    obs_property_set_modified_callback(rim_type,rim_direction_changed);
    obs_property_set_long_description(follow,
        "Aim a one-sided rim toward the chosen light. Glowbox Light supplies its position and direction. Spotlight also supplies its edge color. "
        "Your manual Angle and Color stay saved. If the light is unavailable, the manual settings are used.");
    obs_properties_add_text(rim, "rim_link_status", "", OBS_TEXT_INFO);
    obs_properties_add_float_slider(rim, "rim_angle", "Angle", -180, 180, 1);
    obs_properties_add_float_slider(rim, "rim_opacity", "Opacity", 0, 1, .01);
    auto *rim_blend =
        obs_properties_add_list(rim, "rim_blend", "Blend Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    const char *rim_blends[] = {"Normal",     "Darken",     "Multiply",    "Color Burn",   "Linear Burn",
                                "Add",        "Lighten",    "Screen",      "Color Dodge",  "Overlay",
                                "Soft Light", "Hard Light", "Vivid Light", "Linear Light", "Pin Light",
                                "Hard Mix",   "Difference", "Exclusion",   "Subtract",     "Divide",
                                "Hue",        "Saturation", "Color",       "Luminosity"};
    const int rim_choices[] = {7, 5, 10, 9, 0};
    for (int i : rim_choices)
        obs_property_list_add_int(rim_blend, rim_blends[i], i);
    // Retain a removed blend when opening a scene that already uses it.
    if (std::find(std::begin(rim_choices), std::end(rim_choices), saved_rim_blend) == std::end(rim_choices)) {
        std::string saved_label = std::string(rim_blends[saved_rim_blend]) + " (Saved)";
        obs_property_list_add_int(rim_blend, saved_label.c_str(), saved_rim_blend);
    }
    obs_property_set_long_description(rim_blend,
        "Screen gently brightens. Add gives stronger light. Soft Light and Overlay change contrast. "
        "Normal blends in the rim color. Existing saved modes keep their original behavior.");
    auto *shadow = obs_properties_create();
    auto *shadow_toggle = obs_properties_add_group(p,"shadow_enabled","Drop Shadow",OBS_GROUP_CHECKABLE,shadow);
    obs_property_set_modified_callback(shadow_toggle,effect_enabled_changed);
    auto *shadow_follow = obs_properties_add_list(shadow,"shadow_follow","Link To",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_set_modified_callback(shadow_follow,shadow_direction_changed);
    obs_property_list_add_int(shadow_follow,"Manual",0);
    obs_property_list_add_int(shadow_follow,"Glowbox Light",1);
    obs_properties_add_text(shadow,"shadow_link_status","",OBS_TEXT_INFO);
    obs_properties_add_float_slider(shadow,"shadow_opacity","Opacity",0,1,.01);
    obs_properties_add_float_slider(shadow,"shadow_size","Shadow Scale",.5,2,.01);
    auto *shadow_soft=obs_properties_add_float_slider(shadow,"shadow_softness","Softness",0,1,.01);
    obs_property_set_long_description(shadow_soft,"When following Spotlight, a wider light source gives a softer shadow as Depth increases. The shadow stays tight at the background. In manual mode this sets blur directly.");
    obs_properties_add_float_slider(shadow,"shadow_distance","Distance",0,.3,.001);
    obs_properties_add_float_slider(shadow,"shadow_angle","Angle",-180,180,1);
    obs_property_set_long_description(shadow_follow,"Glowbox Light links the shadow projection to Subject Depth. Depth sets its size; Softness and Opacity remain adjustable. Size, Distance and Angle stay saved for the manual shadow, used when set to Manual or no light is available.");
    auto *color = obs_properties_create();
    obs_properties_add_group(p, "color", "Scene Color", OBS_GROUP_NORMAL, color);
    obs_properties_add_float_slider(color, "exposure", "Exposure", -3, 3, .05);
    obs_properties_add_color(color, "color_filter", "Tint");
    obs_properties_add_float_slider(color, "saturation_adjust", "Saturation", -1, 1, .01);
    obs_properties_add_float_slider(color, "brightness", "Brightness", -1, 1, .01);
    obs_properties_add_float_slider(color, "contrast_adjust", "Contrast", -1, 1, .01);
    auto *screen = obs_properties_create();
    obs_properties_add_group(p, "screen", "Screen Effects", OBS_GROUP_NORMAL, screen);
    auto *mode =
        obs_properties_add_list(screen, "screen_mode", "Effect", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "None", 0);
    obs_property_list_add_int(mode, "Soft Light", 1);
    obs_property_list_add_int(mode, "Diffusion", 2);
    obs_property_list_add_int(mode, "Glow", 3);
    obs_property_set_modified_callback(mode, mode_changed);
    obs_properties_add_float_slider(screen, "screen_intensity", "Intensity", 0, 1, .01);
    obs_properties_add_float_slider(screen, "screen_opacity", "Opacity", 0, 1, .01);
    obs_properties_add_float_slider(screen, "radius", "Light Spread", .25, 6, .05);
    obs_properties_add_float_slider(screen, "threshold", "Highlight Threshold", 0, .99, .01);
    obs_properties_add_float_slider(screen, "screen_contrast", "Contrast", 0, 1, .01);
    obs_property_set_long_description(
        mode, "Soft Light changes contrast. Diffusion spreads light. Glow adds halos around highlights.");
    obs_property_set_long_description(obs_properties_get(screen, "screen_opacity"),
                                      "Fades the selected Screen Effect. Zero removes it; other effects stay active.");
    obs_property_set_long_description(obs_properties_get(screen, "screen_intensity"),
                                      "Sets the strength of the selected Screen Effect. Use Opacity to fade it.");
    obs_property_set_long_description(obs_properties_get(screen, "threshold"),
                                      "Lower values let more of the scene contribute to Glow.");
    obs_property_set_long_description(obs_properties_get(screen, "screen_contrast"),
                                      "Adds contrast to Diffusion. Keep at zero for lightening only.");
    obs_property_set_long_description(reference,
                                      "Scene Background uses the background colors. Choose a source to use its colors instead. Color Match adjusts color; Reactive Lighting adds light.");
    obs_property_set_long_description(
        obs_properties_get(rim, "rim_type"),
        "Soft fades gradually into the subject. Sharp creates a harder edge. One Side follows Angle.");
    obs_property_set_long_description(obs_properties_get(rim, "rim_size"),
        "Sets how far the rim reaches into the subject.");
    obs_property_set_long_description(obs_properties_get(rim, "rim_brightness"),
        "Boosts the rim light before blending. Zero adds no boost; it does not turn the rim off. "
        "Bright colors can reach the output limit. Use Opacity to fade the rim.");
    obs_property_set_long_description(obs_properties_get(rim, "rim_opacity"),
        "Fades the manual and reactive rim. Zero turns off the rim treatment, including any saved surface adjustment.");
    if (surface_contrast)
        obs_property_set_long_description(surface_contrast,
            "Retains this scene's earlier surface adjustment. Set to zero to remove it. "
            "Use Scene Color for new contrast adjustments.");
    obs_property_set_long_description(obs_properties_get(subject_properties, "edit_subject"),
                                      "Groups and nested scenes are selected as whole items. OBS source "
                                      "order still controls what appears in front.");
    obs_property_set_enabled(obs_properties_get(subject_properties, "subject_exposure"), false);
    obs_property_set_enabled(obs_properties_get(subject_properties, "background_exposure"), false);
    obs_property_set_visible(obs_properties_get(subject_properties, "subject_exposure"), false);
    obs_property_set_visible(obs_properties_get(subject_properties, "background_exposure"), false);
    for (const char *key : {"subject_exposure", "background_exposure", "exposure"}) exposure_units(p, key);
    obs_property_set_long_description(obs_properties_get(color, "saturation_adjust"), "0 keeps the current saturation. -1 removes color; positive values strengthen it.");
    obs_property_set_long_description(obs_properties_get(color, "brightness"), "Lift or lower pixel values. Use Exposure for overall light level. 0 leaves brightness unchanged.");
    if (f) {
        auto *settings = obs_source_get_settings(f->source);
        add_missing_source_choice(reference,settings,"reference_source");
        add_missing_source_choice(light_source,settings,"light_source");
        update_subject_summary(p, nullptr, settings);
        follow_brightness_changed(p, nullptr, settings);
        mode_changed(p, nullptr, settings);
        effect_enabled_changed(p, rim_toggle, settings);


        obs_data_release(settings);
    }
    add_glowbox_footer(p);
    return p;
}

}
