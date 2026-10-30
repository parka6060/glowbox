#include "compositor/compositor.hpp"

namespace glowbox {
void draw_mask_scene(Compositor *filter, obs_scene_t *scene, LayerCaptureMode mode) {
    const bool subject = mode == LayerCaptureMode::SubjectCoverage;
    for (auto &item : filter->items) {
        if (subject)
            item.view->mask = true;
        else
            item.view->capture_mode = mode;
        item.view->mask_capture_failed = false;
    }
    filter->rendering_layers = true;
    draw_compositor_scene(filter, scene);
    filter->rendering_layers = false;
    for (auto &item : filter->items) {
        if (subject)
            item.view->mask = false;
        else
            item.view->capture_mode = LayerCaptureMode::None;
    }
}

void draw_compositor_scene(Compositor *f, obs_scene_t *scene) {
    if (f->scene_capture_callback) {
        std::vector<SceneCaptureLayer> layers;
        for (const auto &entry : f->items) {
            auto *item = scene == f->background_mirror ? entry.background_item
                       : scene == f->mask_mirror ? entry.mask_item
                       : scene == f->protected_mirror ? entry.protected_item
                       : scene == f->protection_mask_mirror ? entry.protection_mask_item
                       : scene == f->lighting_mask_mirror ? entry.lighting_mask_item : entry.item;
            layers.push_back({entry.view->original, entry.wrapper, item && obs_sceneitem_visible(item)});
        }
        f->scene_capture_is_base_pass = scene == f->mirror;
        if (!f->scene_capture_callback(f->scene_capture_context, layers.data(), layers.size()))
            f->scene_capture_failed = true;
        return;
    }
    obs_source_video_render(obs_scene_get_source(scene));
}

void clear_mirror(Compositor *f) {
    if (f->mirror)
        obs_scene_release(f->mirror);
    if (f->background_mirror)
        obs_scene_release(f->background_mirror);
    if (f->mask_mirror)
        obs_scene_release(f->mask_mirror);
    if (f->protected_mirror)
        obs_scene_release(f->protected_mirror);
    if (f->protection_mask_mirror)
        obs_scene_release(f->protection_mask_mirror);
    if (f->lighting_mask_mirror) obs_scene_release(f->lighting_mask_mirror);
    f->lighting_mask_mirror=nullptr;
    f->protected_mirror = nullptr;
    f->protection_mask_mirror = nullptr;
    f->background_mirror = nullptr;
    f->mask_mirror = nullptr;
    f->mirror = nullptr;
    for (auto &item : f->items)
        obs_source_release(item.wrapper);
    f->items.clear();
}
std::string subject_setting_key(obs_sceneitem_t *item) {
    auto *data = obs_sceneitem_get_private_settings(item);
    std::string key = obs_data_get_string(data, "glowbox_subject_key");
    obs_data_release(data);
    return key.empty() ? "subject_" + std::to_string(obs_sceneitem_get_id(item)) : key;
}
// Studio Mode copies item private settings, but renumbers scene-item IDs.
// Keep selection identity on the item and migrate the old numeric keys once.
void prepare_subject_settings(Compositor *f, obs_scene_t *scene) {
    if (!scene || !f->layer_settings) return;
    struct Context { Compositor *filter; obs_data_t *settings; } context{f, obs_source_get_settings(f->source)};
    obs_scene_enum_items(scene, [](obs_scene_t *owner, obs_sceneitem_t *item, void *opaque) {
        auto &context = *static_cast<Context *>(opaque);
        auto *data = obs_sceneitem_get_private_settings(item);
        const std::string legacy = "subject_" + std::to_string(obs_sceneitem_get_id(item));
        if (!*obs_data_get_string(data, "glowbox_subject_key")) {
            const std::string key = "subject_item_" + std::string(obs_source_get_uuid(obs_scene_get_source(owner))) + "_" + std::to_string(obs_sceneitem_get_id(item));
            obs_data_set_string(data, "glowbox_subject_key", key.c_str());
        }
        const std::string key = obs_data_get_string(data, "glowbox_subject_key");
        for (auto *settings : {context.settings, context.filter->layer_settings}) {
            if (obs_data_has_user_value(settings, legacy.c_str())) {
                obs_data_set_bool(settings, key.c_str(), obs_data_get_bool(settings, legacy.c_str()));
                obs_data_unset_user_value(settings, legacy.c_str());
            }
        }
        obs_data_release(data);
        return true;
    }, &context);
    obs_data_release(context.settings);
}
static bool collect_item(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    if (obs_source_removed(obs_sceneitem_get_source(item)))
        return true;
    obs_sceneitem_addref(item);
    ((std::vector<obs_sceneitem_t *> *)data)->push_back(item);
    return true;
}
static obs_source_t *active_item_transition(obs_sceneitem_t *item) {
    auto *transition = obs_sceneitem_get_transition(item, obs_sceneitem_visible(item));
    if (!transition)
        return nullptr;
    // Show/Hide runs between a source and transparency.
    // Once playback stops, the active source is input A.
    auto *from = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
    auto *destination = obs_transition_get_active_source(transition);
    const bool playing = from != destination;
    obs_source_release(from);
    obs_source_release(destination);
    return playing ? obs_source_get_ref(transition) : nullptr;
}
bool sync_mirror(Compositor *f, obs_scene_t *scene) {
    if (!scene)
        return false;
    prepare_subject_settings(f, scene);
    std::vector<obs_sceneitem_t *> live;
    obs_scene_enum_items(scene, collect_item, &live);
    bool has_group = false;
    for (auto *item : live)
        has_group |= obs_sceneitem_is_group(item);
    if (has_group) {
        // OBS refreshes group bounds during its owning scene's render_filter.
        if (!f->layout_render)
            f->layout_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        gs_texrender_reset(f->layout_render);
        if (gs_texrender_begin(f->layout_render, 1, 1)) {
            gs_ortho(0, (float)f->width, 0, (float)f->height, -100, 100);
            f->rendering_layers = true;
            obs_source_video_render(obs_scene_get_source(scene));
            f->rendering_layers = false;
            gs_texrender_end(f->layout_render);
        }
        for (auto *item : live)
            obs_sceneitem_release(item);
        live.clear();
        obs_scene_enum_items(scene, collect_item, &live);
    }
    bool supported = true, rebuild = !f->mirror || live.size() != f->items.size() ||
                                    f->has_exclusions != (f->protected_mirror != nullptr) ||
                                    (f->has_exclusions || f->has_reactive_overrides) != (f->protection_mask_mirror != nullptr) ||
                                    f->has_spotlight_overrides != (f->lighting_mask_mirror != nullptr);
    for (size_t i = 0; i < live.size(); ++i) {
        // Protected contributions use normal premultiplied over-compositing.
        if ((f->has_exclusions || f->has_reactive_overrides || f->has_spotlight_overrides) && obs_sceneitem_visible(live[i]) &&
            obs_sceneitem_get_blending_mode(live[i]) != OBS_BLEND_NORMAL)
            supported = false;
        if (!rebuild && (f->items[i].id != obs_sceneitem_get_id(live[i]) ||
                         f->items[i].view->original != obs_sceneitem_get_source(live[i])))
            rebuild = true;
    }
    if (supported && rebuild) {
        clear_mirror(f);
        f->mirror = obs_scene_create_private("Glowbox composition");
        f->background_mirror = obs_scene_create_private("Glowbox background");
        // OBS caches cropped/scaled scene items within a frame. Mask rendering
        // needs its own items so it cannot reuse the color capture's textures.
        f->mask_mirror = obs_scene_create_private("Glowbox subject mask");
        if (f->has_exclusions) {
            f->protected_mirror = obs_scene_create_private("Glowbox protected colors");
            if (!f->protected_mirror) supported = false;
        }
        if (f->has_exclusions || f->has_reactive_overrides) {
            f->protection_mask_mirror = obs_scene_create_private("Glowbox source coverage");
            if (!f->protection_mask_mirror) supported = false;
        }
        if (f->has_spotlight_overrides) {
            f->lighting_mask_mirror=obs_scene_create_private("Glowbox lighting coverage");
            if (!f->lighting_mask_mirror) supported=false;
        }
        if (!f->mirror || !f->background_mirror || !f->mask_mirror)
            supported = false;
        for (auto *item : live) {
            if (!supported)
                break;
            LayerView *view = nullptr;
            auto *wrapper = create_layer_source(obs_sceneitem_get_source(item), f->effect, &view);
            if (!wrapper || !view || !view->effect) {
                if (wrapper)
                    obs_source_release(wrapper);
                supported = false;
                break;
            }
            auto *copy = obs_scene_add(f->mirror, wrapper);
            if (!copy) {
                obs_source_release(wrapper);
                supported = false;
                break;
            }
            auto *background_copy = obs_scene_add(f->background_mirror, wrapper);
            if (!background_copy) {
                obs_source_release(wrapper);
                supported = false;
                break;
            }
            auto *mask_copy = obs_scene_add(f->mask_mirror, wrapper);
            if (!mask_copy) {
                obs_source_release(wrapper);
                supported = false;
                break;
            }
            auto *protected_copy = f->has_exclusions ? obs_scene_add(f->protected_mirror, wrapper) : nullptr;
            auto *protection_mask_copy =
                f->protection_mask_mirror ? obs_scene_add(f->protection_mask_mirror, wrapper) : nullptr;
            auto *lighting_copy=f->lighting_mask_mirror ? obs_scene_add(f->lighting_mask_mirror,wrapper) : nullptr;
            if ((f->lighting_mask_mirror && !lighting_copy) || (f->has_exclusions && !protected_copy) || (f->protection_mask_mirror && !protection_mask_copy)) {
                obs_source_release(wrapper);
                supported = false;
                break;
            }
            f->items.push_back({obs_sceneitem_get_id(item), wrapper, copy, view, background_copy, mask_copy,
                                protected_copy, protection_mask_copy, lighting_copy});
        }
    }
    if (supported) {
        // Mirror scenes use the OBS video canvas, so source dimensions must match.

        auto *mirror_source = obs_scene_get_source(f->mirror);
        supported = obs_source_get_width(mirror_source) == f->width &&
                    obs_source_get_height(mirror_source) == f->height;
    }
    if (supported)
        for (size_t i = 0; i < live.size(); ++i) {
            auto &entry = f->items[i];
            auto *transition = f->scene_capture_callback ? nullptr : active_item_transition(live[i]);
            obs_source_release(entry.view->transition);
            entry.view->transition = transition;
            const bool visible = obs_sceneitem_visible(live[i]) || transition;
            if (transition) {
                obs_transition_set_size(transition, obs_source_get_width(entry.view->original),
                                        obs_source_get_height(entry.view->original));
                if ((f->has_exclusions || f->has_reactive_overrides || f->has_spotlight_overrides) &&
                    obs_sceneitem_get_blending_mode(live[i]) != OBS_BLEND_NORMAL)
                    supported = false;
            }
            obs_transform_info transform = {};
            obs_sceneitem_get_info2(live[i], &transform);
            obs_sceneitem_crop crop = {};
            obs_sceneitem_get_crop(live[i], &crop);
            bool transform_changed =
                !entry.configured || memcmp(&transform, &entry.transform, sizeof(transform)) != 0;
            bool crop_changed = !entry.configured || memcmp(&crop, &entry.crop, sizeof(crop)) != 0;
            bool subject = obs_data_get_bool(f->layer_settings, subject_setting_key(live[i]).c_str());
            entry.view->is_subject_plane=subject;
            const auto view_translation=scene_plane_translation(f->view,subject?subject_plane_distance(f->subject_depth):1.f,float(f->height));
            transform_changed=transform_changed || entry.view_translation.x!=view_translation.x || entry.view_translation.y!=view_translation.y;
            auto projected_transform=transform;
            if (scene_view_active(f->view)) {
                projected_transform.pos.x+=view_translation.x;
                projected_transform.pos.y+=view_translation.y;
            }
            const auto policy = source_policy(entry.view->original);
            entry.view->treatment = policy;
            entry.view->owner = f;
            entry.view->protection = source_protection(policy);
            for (auto *copy : {entry.item, entry.background_item, entry.mask_item,
                               entry.protected_item, entry.protection_mask_item, entry.lighting_mask_item}) {
                if (!copy)
                    continue;
                auto *copy_settings = obs_sceneitem_get_private_settings(copy);
                obs_data_set_string(copy_settings, "glowbox_subject_key", subject_setting_key(live[i]).c_str());
                obs_data_release(copy_settings);
                if (transform_changed)
                    obs_sceneitem_set_info2(copy, &projected_transform);
                if (crop_changed)
                    obs_sceneitem_set_crop(copy, &crop);
                auto scale = obs_sceneitem_get_scale_filter(live[i]);
                auto method = obs_sceneitem_get_blending_method(live[i]);
                auto mode = obs_sceneitem_get_blending_mode(live[i]);
                if (scale != obs_sceneitem_get_scale_filter(copy))
                    obs_sceneitem_set_scale_filter(copy, scale);
                if (method != obs_sceneitem_get_blending_method(copy))
                    obs_sceneitem_set_blending_method(copy, method);
                if (mode != obs_sceneitem_get_blending_mode(copy))
                    obs_sceneitem_set_blending_mode(copy, mode);
                obs_sceneitem_set_visible(copy, visible &&
                                                    !((subject || entry.view->protection != SourceProtection::None) && copy == entry.background_item));
            }
            if (obs_sceneitem_is_group(live[i])) {
                obs_sceneitem_force_update_transform(entry.item);
                matrix4 original, copy;
                obs_sceneitem_get_draw_transform(live[i], &original);
                obs_sceneitem_get_draw_transform(entry.item, &copy);
                float dx = original.t.x+view_translation.x-copy.t.x, dy = original.t.y+view_translation.y-copy.t.y;
                float determinant = copy.x.x * copy.y.y - copy.y.x * copy.x.y;
                if (std::abs(determinant) > .000001f) {
                    entry.view->offset_x = (dx * copy.y.y - dy * copy.y.x) / determinant;
                    entry.view->offset_y = (dy * copy.x.x - dx * copy.x.y) / determinant;
                }
            }
            entry.view_translation=view_translation;
            entry.transform = transform;
            entry.crop = crop;
            entry.configured = true;
        }
    for (auto *item : live)
        obs_sceneitem_release(item);
    return supported;
}
void configure_layers(Compositor *f, bool background_only) {
    const bool source_matching = std::any_of(f->items.begin(), f->items.end(),
        [](const MirrorItem &item) { return item.view->treatment.match_override >= 0; });
    const bool selected = std::any_of(f->items.begin(), f->items.end(), [&](const MirrorItem &item) {
        return item.view->protection == SourceProtection::None &&
            obs_data_get_bool(f->layer_settings, subject_setting_key(item.item).c_str());
    });
    for (auto &item : f->items) {
        std::string key = subject_setting_key(item.item);
        bool subject = obs_data_get_bool(f->layer_settings, key.c_str());
        item.view->skip = false;
        item.view->mask = false;
        item.view->is_subject_plane=subject;
        subject = subject && item.view->protection == SourceProtection::None;
        item.view->is_treated_subject = subject;
        item.view->exposure_gain = 1;
        item.view->color_match_amount =
            !background_only && gs_texrender_get_texture(f->color_sample)
                ? (item.view->treatment.match_override >= 0 ? item.view->treatment.match_override
                      : (subject || (source_matching && !selected && item.view->protection == SourceProtection::None))
                          ? f->color_match_strength : item.view->treatment.color_match)
                : 0;
        item.view->color_sample_texture = gs_texrender_get_texture(f->color_sample);
    }
}
bool sample_background_color(Compositor *f) {
    configure_layers(f, true);
    gs_texrender_reset(f->background_render);
    bool ok = gs_texrender_begin(f->background_render, 112, 60);
    if (ok) {
        vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, (float)f->width, 0, (float)f->height, -100, 100);
        f->rendering_layers = true;
        draw_compositor_scene(f, f->background_mirror);
        f->rendering_layers = false;
        gs_texrender_end(f->background_render);
        // Explicit reference still overrides the automatic background sample.
        ok = sample_color_reference(f, gs_texrender_get_texture(f->background_render));
    }
    return ok;
}

}
