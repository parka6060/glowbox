#include "compositor/compositor_lifecycle.hpp"
#include "compositor/compositor.hpp"
#include "compositor/compositor_graphics.hpp"

namespace glowbox {

// OBS creates transition copies synchronously through its ordinary source callback.
// This thread-local slot passes the borrowed effect to that callback.
static thread_local gs_effect_t *effect_for_transition_copy = nullptr;

struct TransitionEffectScope {
    gs_effect_t *previous_effect;

    explicit TransitionEffectScope(gs_effect_t *effect) : previous_effect(effect_for_transition_copy) {
        effect_for_transition_copy = effect;
    }

    TransitionEffectScope(const TransitionEffectScope &) = delete;
    TransitionEffectScope &operator=(const TransitionEffectScope &) = delete;

    ~TransitionEffectScope() {
        effect_for_transition_copy = previous_effect;
    }
};
static void destroy_compositor(void *data);

static const char *compositor_display_name(void *) {
    return "Glowbox Compositor (Preview)";
}

static void *create_compositor(obs_data_t *settings, obs_source_t *source) {
    auto *compositor = new Compositor;
    compositor->source = source;
    prepare_compositor_graphics(compositor, effect_for_transition_copy);
    if (!compositor->effect && !compositor->support_pending) {
        destroy_compositor(compositor);
        return nullptr;
    }
    update_settings(compositor, settings);
    return compositor;
}

static void destroy_compositor(void *data) {
    auto *compositor = static_cast<Compositor *>(data);
    clear_mirror(compositor);
    if (compositor->layer_settings)
        obs_data_release(compositor->layer_settings);
    release_compositor_graphics(compositor);
    delete compositor;
}

void register_compositor_filter() {
    obs_source_info info = {};
    info.id = "glowbox_compositor_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = compositor_display_name;
    info.create = create_compositor;
    info.destroy = destroy_compositor;
    info.update = update_settings;
    info.filter_add = [](void *data, obs_source_t *parent) {
        prepare_subject_settings(static_cast<Compositor *>(data), obs_scene_from_source(parent));
    };
    info.load = load_compositor_settings;
    info.save = save_compositor_settings;
    info.get_defaults = default_settings;
    info.get_properties = filter_properties;
    info.video_render = render_filter;
    info.video_tick = tick_filter;
    obs_register_source(&info);
}

obs_source_t *create_transition_treatment(obs_data_t *settings, Compositor *source_compositor) {
    if (!source_compositor || !source_compositor->effect)
        return nullptr;
    obs_source_t *transition_source = nullptr;
    {
        TransitionEffectScope borrowed_effect(source_compositor->effect);
        transition_source =
            obs_source_create_private("glowbox_compositor_filter", "Glowbox transition treatment", settings);
    }
    if (transition_source) {
        auto *transition_compositor = static_cast<Compositor *>(obs_obj_get_data(transition_source));
        // OBS retains a source handle even when its create callback fails.
        if (!transition_compositor) {
            obs_source_release(transition_source);
            return nullptr;
        }
        transition_compositor->follow_owner = source_compositor;
        obs_enter_graphics();
        copy_follow_brightness(source_compositor, transition_compositor);
        copy_color_match_history(source_compositor, transition_compositor);
        obs_leave_graphics();
    }
    return transition_source;
}

}
