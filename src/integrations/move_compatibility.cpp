#include "compositor/compositor_lifecycle.hpp"
#include "compositor/compositor_graphics.hpp"
#include "plugin_registration.hpp"
#include "effects/perceptual_color.hpp"
#include "effects/leaf_canopy.hpp"
#include "compositor/compositor.hpp"
#include "compositor/shader_preparation.hpp"
#include <map>
#include <sstream>
#include <iomanip>

namespace glowbox {
namespace {
constexpr const char *bridge_id = "glowbox_move_bridge";
constexpr const char *tag_id = "glowbox_move_capture";
thread_local const SceneCaptureLayer *capture_layers = nullptr;
thread_local size_t capture_count = 0;
thread_local uint8_t *capture_seen = nullptr;
thread_local unsigned capture_depth = 0;
thread_local bool replaying = false;
thread_local bool capture_failed = false;
thread_local bool capture_mask = false;
// A replay occurrence, rather than a source ID, distinguishes repeated placements.
struct CaptureOccurrence {
    obs_source_t *source;
    bool is_subject_plane;
};
struct PlacementReplayState {
    std::vector<CaptureOccurrence> observed_order;
    bool discovering = true, mixed_plane_fade = false;
    size_t ordinal = 0, run_begin = 0, run_end = 0;
    int endpoint_index = 0;
    const std::vector<obs_source_t *> *group_moves = nullptr;
};
thread_local PlacementReplayState *placement_replay_state =
    nullptr; // Borrowed during one synchronous replay.
struct PlacementReplayScope {
    PlacementReplayState *previous;
    explicit PlacementReplayScope(PlacementReplayState &state) : previous(placement_replay_state) {
        placement_replay_state = &state;
    }
    ~PlacementReplayScope() {
        placement_replay_state = previous;
    }
};

struct MovePreparation {
    struct Node {
        obs_source_t *source; // Owned reference, released with this preparation.
        std::string parent;   // Copied UUID, does not borrow an OBS string.
    };
    obs_source_t *root;    // Borrowed from the bridge during synchronous preparation.
    obs_source_t *current; // Borrowed traversal parent, restored after each child.
    std::vector<Node> nodes;
    explicit MovePreparation(obs_source_t *source) : root(source), current(source) {
        add(source, nullptr);
    }
    ~MovePreparation() {
        for (auto &node : nodes)
            obs_source_release(node.source);
    }
    void add(obs_source_t *source, obs_source_t *parent) {
        if (!source)
            return;
        const char *id = obs_source_get_unversioned_id(source);
        if (strcmp(id, "move_transition") && strcmp(id, "fade_transition"))
            return;
        for (const auto &node : nodes)
            if (node.source == source)
                return;
        nodes.push_back({obs_source_get_ref(source), parent ? obs_source_get_uuid(parent) : ""});
    }
};
thread_local MovePreparation *move_preparation =
    nullptr; // Borrowed while rendering this root's readiness passes.
struct MovePreparationScope {
    MovePreparation *previous;
    explicit MovePreparationScope(MovePreparation &preparation) : previous(move_preparation) {
        move_preparation = &preparation;
    }
    ~MovePreparationScope() {
        move_preparation = previous;
    }
};

struct Tag {
    obs_source_t *source;
    bool owned = false, nested_move = false;
    gs_effect_t *fade_effect = nullptr; // Borrowed from the active bridge.
    signal_handler_t *transition_signals =
        nullptr; // Borrowed from the tagged parent, disconnected before release.
    gs_texrender_t *fade_inputs[2]{};
};
void observe_tag_start(void *, calldata_t *params) {
    if (move_preparation)
        move_preparation->add(static_cast<obs_source_t *>(calldata_ptr(params, "source")),
                              move_preparation->current);
}
void disconnect_tag_start(Tag *tag) {
    if (tag->transition_signals) {
        signal_handler_disconnect(tag->transition_signals, "transition_start", observe_tag_start, tag);
        tag->transition_signals = nullptr;
    }
}
const char *tag_name(void *) {
    return "Glowbox internal capture";
}
void *tag_create(obs_data_t *, obs_source_t *source) {
    return new Tag{source};
}
void tag_destroy(void *data) {
    auto *tag = static_cast<Tag *>(data);
    disconnect_tag_start(tag);
    obs_enter_graphics();
    for (auto *input : tag->fade_inputs)
        gs_texrender_destroy(input);
    obs_leave_graphics();
    delete tag;
}
// Move asks child dimensions again during replay. Newly created stationary
// transitions can report zero until their first tick, use their live inputs.
uint32_t tag_dimension(void *data, bool width) {
    auto *tag = static_cast<Tag *>(data);
    auto *target = obs_filter_get_target(tag->source);
    uint32_t size = width ? obs_source_get_base_width(target) : obs_source_get_base_height(target);
    if (!tag->fade_effect && !tag->nested_move)
        return size;
    if (tag->nested_move && (move_preparation || placement_replay_state)) {
        auto *parent = obs_filter_get_parent(tag->source);
        uint32_t w = 0, h = 0;
        obs_transition_get_size(parent, &w, &h);
        if (w && h)
            return width ? w : h;
    }
    size = 0; // A pooled transition can have stale nonzero dimensions too.
    auto *parent = obs_filter_get_parent(tag->source);
    for (auto which : {OBS_TRANSITION_SOURCE_A, OBS_TRANSITION_SOURCE_B}) {
        auto *child = obs_transition_get_source(parent, which);
        if (child)
            size = std::max(size, width ? obs_source_get_width(child) : obs_source_get_height(child));
        obs_source_release(child);
    }
    return size;
}
uint32_t tag_width(void *data) {
    return tag_dimension(data, true);
}
uint32_t tag_height(void *data) {
    return tag_dimension(data, false);
}
// A newly created same-source Fade has not ticked yet: OBS can still have
// a zero matrix. Its public size/alignment already describe the correct image.
void render_stationary_child(obs_source_t *transition, obs_source_t *child) {
    const float cw = (float)obs_source_get_width(child), ch = (float)obs_source_get_height(child);
    uint32_t width = 0, height = 0;
    obs_transition_get_size(transition, &width, &height);
    const float w = width ? (float)width : cw, h = height ? (float)height : ch;
    if (!w || !h || !cw || !ch)
        return;
    auto scale = obs_transition_get_scale_type(transition);
    float sx = 1, sy = 1;
    if (scale == OBS_TRANSITION_SCALE_STRETCH) {
        sx = w / cw;
        sy = h / ch;
    } else if (scale == OBS_TRANSITION_SCALE_ASPECT || cw > w || ch > h)
        sx = sy = std::min(w / cw, h / ch);
    const uint32_t align = obs_transition_get_alignment(transition);
    const float dx = w - cw * sx, dy = h - ch * sy;
    const float x = (align & OBS_ALIGN_LEFT) ? 0 : (align & OBS_ALIGN_RIGHT) ? dx : dx * .5f;
    const float y = (align & OBS_ALIGN_TOP) ? 0 : (align & OBS_ALIGN_BOTTOM) ? dy : dy * .5f;
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0);
    gs_matrix_scale3f(sx, sy, 1);
    obs_source_video_render(child);
    gs_matrix_pop();
}
bool capture_stationary_transition(obs_source_t *parent) {
    auto *only = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_A);
    auto *other = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_B);
    auto *active = obs_transition_get_active_source(parent);
    const bool stationary = only && !other && active == only;
    if (stationary)
        render_stationary_child(parent, only);
    obs_source_release(only);
    obs_source_release(other);
    obs_source_release(active);
    return stationary;
}
// OBS Fade caches child textures for a whole video frame. Replay needs fresh
// child masks as well as colour. Use its own effect and public direct-render
// API, preserving its transforms and blend rather than reading private state.
bool capture_fade(Tag *tag, obs_source_t *parent) {
    struct FadePlaneScope {
        PlacementReplayState *state;
        size_t begin;
        ~FadePlaneScope() {
            if (!state)
                return;
            const auto &order = state->observed_order;
            const size_t end = state->discovering ? order.size() : state->ordinal;
            if (begin >= end || end > order.size())
                return;
            for (size_t i = begin + 1; i < end; ++i)
                if (order[i].is_subject_plane != order[begin].is_subject_plane) {
                    state->mixed_plane_fade = true;
                    capture_failed = true;
                    break;
                }
        }
    } plane_scope{placement_replay_state,
                  placement_replay_state
                      ? (placement_replay_state->discovering ? placement_replay_state->observed_order.size()
                                                             : placement_replay_state->ordinal)
                      : 0};
    const uint32_t w = obs_source_get_base_width(parent), h = obs_source_get_base_height(parent);
    const float progress = obs_transition_get_time(parent);
    if (capture_stationary_transition(parent))
        return true;
    if (progress <= 0 || progress >= 1) {
        // A zero-weight child is intentionally absent, not a failed mask hook.
        auto *hidden = obs_transition_get_source(parent, progress <= 0 ? OBS_TRANSITION_SOURCE_B
                                                                       : OBS_TRANSITION_SOURCE_A);
        for (size_t i = 0; i < capture_count; ++i)
            if (capture_layers[i].original == hidden && capture_seen)
                capture_seen[i] = 1;
        obs_source_release(hidden);
        obs_transition_video_render_direct(parent,
                                           progress >= 1 ? OBS_TRANSITION_SOURCE_B : OBS_TRANSITION_SOURCE_A);
        return true;
    }
    if (!w || !h)
        return true; // Zero size is not a capture failure.
    auto *a = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_A);
    auto *b = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_B);
    const bool linear = gs_set_linear_srgb(true), srgb = gs_framebuffer_srgb_enabled();
    const float t = obs_transition_get_time(parent);
    bool ok = true;
    gs_texture_t *inputs[2]{};
    for (int n = 0; n < 2 && ok; ++n) {
        if (!(n ? b : a))
            continue;
        auto *&target = tag->fade_inputs[n];
        if (!target)
            target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (!target) {
            ok = false;
            break;
        }
        gs_texrender_reset(target);
        if (!gs_texrender_begin(target, w, h)) {
            ok = false;
            break;
        }
        vec4 zero{};
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, (float)w, 0, (float)h, -100, 100);
        obs_transition_video_render_direct(parent, n ? OBS_TRANSITION_SOURCE_B : OBS_TRANSITION_SOURCE_A);
        gs_texrender_end(target);
        inputs[n] = gs_texrender_get_texture(target);
    }
    if (ok && (inputs[0] || inputs[1])) {
        auto *effect = tag->fade_effect;
        auto *ap = gs_effect_get_param_by_name(effect, "tex_a"),
             *bp = gs_effect_get_param_by_name(effect, "tex_b");
        // Coverage channels are linear data, not display colours. Gamma conversion
        // changes their weights and breaks subject/exclusion edges during fades.
        float mix = t;
        const char *technique = capture_mask ? "FadeLinear" : "Fade";
        gs_enable_framebuffer_srgb(!capture_mask);
        if (!inputs[0] || !inputs[1]) {
            technique = "FadeSingle";
            mix = inputs[0] ? 1 - t : t;
            if (capture_mask)
                gs_effect_set_texture(ap, inputs[0] ? inputs[0] : inputs[1]);
            else
                gs_effect_set_texture_srgb(ap, inputs[0] ? inputs[0] : inputs[1]);
        } else {
            gs_effect_set_texture(ap, inputs[0]);
            gs_effect_set_texture(bp, inputs[1]);
        }
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "fade_val"), mix);
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        while (gs_effect_loop(effect, technique))
            gs_draw_sprite(nullptr, 0, w, h);
        gs_blend_state_pop();
    }
    gs_enable_framebuffer_srgb(srgb);
    gs_set_linear_srgb(linear);
    obs_source_release(a);
    obs_source_release(b);
    return ok;
}
void tag_render(void *data, gs_effect_t *) {
    auto *tag = static_cast<Tag *>(data);
    if (move_preparation && tag->nested_move && !capture_layers) {
        auto *parent = obs_filter_get_parent(tag->source);
        auto *previous = move_preparation->current;
        if (parent != previous) {
            move_preparation->add(parent, previous);
            move_preparation->current = parent;
        }
        if (!capture_stationary_transition(parent))
            obs_source_skip_video_filter(tag->source);
        move_preparation->current = previous;
        return;
    }
    // Whole-group capture suppresses descendant treatments, but stationary
    // same-source Move still needs its public size/alignment fallback.
    if (capture_layers && capture_depth && tag->nested_move &&
        capture_stationary_transition(obs_filter_get_parent(tag->source)))
        return;
    if (capture_layers && !capture_depth) {
        auto *parent = obs_filter_get_parent(tag->source);
        if (tag->fade_effect) {
            if (!capture_fade(tag, parent))
                capture_failed = true;
            return;
        }
        obs_source_t *a = nullptr, *b = nullptr, *identity = parent;
        if (tag->nested_move) {
            if (capture_stationary_transition(parent))
                return;
            a = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_A);
            b = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_B);
            identity = (a == b || !b) ? a : !a ? b : nullptr;
            if (!identity && placement_replay_state && placement_replay_state->group_moves &&
                std::find(placement_replay_state->group_moves->begin(),
                          placement_replay_state->group_moves->end(),
                          parent) != placement_replay_state->group_moves->end()) {
                // One logical group owns its complete animated composition.
                // Its root treatment belongs to this endpoint, not to the leaves.
                auto *preferred = placement_replay_state->endpoint_index ? b : a;
                for (size_t i = 0; i < capture_count; ++i)
                    if (capture_layers[i].original == preferred) {
                        identity = preferred;

                        break;
                    }
            }
        }
        bool handled = false;
        for (size_t i = 0; i < capture_count; ++i)
            if (capture_layers[i].original == identity) {
                handled = true;
                if (capture_seen)
                    capture_seen[i] = 1;
                bool selected = true;
                if (auto *state = placement_replay_state) {
                    auto *wrapper = capture_layers[i].wrapper;
                    const bool subject =
                        wrapper != identity &&
                        static_cast<LayerView *>(obs_obj_get_data(wrapper))->is_subject_plane;
                    if (state->discovering)
                        state->observed_order.push_back({identity, subject});
                    else {
                        const size_t ordinal = state->ordinal++;
                        if (ordinal >= state->observed_order.size() ||
                            state->observed_order[ordinal].source != identity ||
                            state->observed_order[ordinal].is_subject_plane != subject)
                            capture_failed = true;
                        selected = ordinal >= state->run_begin && ordinal < state->run_end;
                    }
                }
                if (selected && capture_layers[i].visible) {
                    ++capture_depth;
                    auto *wrapper = capture_layers[i].wrapper;
                    if (wrapper == identity)
                        obs_source_skip_video_filter(tag->source);
                    else {
                        auto *view = static_cast<LayerView *>(obs_obj_get_data(wrapper));
                        auto *input = view->capture_input, *hook = view->capture_filter;
                        const bool viewport = view->viewport_capture;
                        const float offset_x = view->offset_x, offset_y = view->offset_y;
                        // Placement-run replay uses Move's own group transform, not
                        // the ordinary private scene's group-origin compensation.
                        if (placement_replay_state)
                            view->offset_x = view->offset_y = 0;
                        view->capture_input = parent;
                        view->capture_filter = tag->source;
                        if (tag->nested_move && placement_replay_state && placement_replay_state->group_moves)
                            view->viewport_capture =
                                std::find(placement_replay_state->group_moves->begin(),
                                          placement_replay_state->group_moves->end(),
                                          parent) != placement_replay_state->group_moves->end();
                        obs_source_video_render(wrapper);
                        view->capture_input = input;
                        view->capture_filter = hook;
                        view->viewport_capture = viewport;
                        view->offset_x = offset_x;
                        view->offset_y = offset_y;
                    }
                    --capture_depth;
                }
                break;
            }
        obs_source_release(a);
        obs_source_release(b);
        if (handled)
            return;
    }
    obs_source_skip_video_filter(tag->source);
}
// Move 2.12's public matched-item easing settings. These standard curves follow
// Auerhaus easing.c (2011 Auerhaus Development, WTFPL v2), also used by Move.
// https://github.com/exeldro/obs-move-transition/blob/2.12.0/easing.c
float bounce_out(float t) {
    if (t < 4.f / 11)
        return 121 * t * t / 16;
    if (t < 8.f / 11)
        return 363.f / 40 * t * t - 99.f / 10 * t + 17.f / 5;
    if (t < 9.f / 10)
        return 4356.f / 361 * t * t - 35442.f / 1805 * t + 16061.f / 1805;
    return 54.f / 5 * t * t - 513.f / 25 * t + 268.f / 25;
}
float ease_in(float t, int curve) {
    constexpr float pi = 3.14159265358979323846f;
    switch (curve) {
    case 1:
        return t * t;
    case 2:
        return t * t * t;
    case 3:
        return t * t * t * t;
    case 4:
        return t * t * t * t * t;
    case 5:
        return std::sin((t - 1) * pi * .5f) + 1;
    case 6:
        return 1 - std::sqrt(std::max(0.f, 1 - t * t));
    case 7:
        return t == 0 ? 0 : std::exp2(10 * (t - 1));
    case 8:
        return std::sin(13 * pi * .5f * t) * std::exp2(10 * (t - 1));
    case 9:
        return 1 - bounce_out(1 - t);
    case 10:
        return t * t * t - t * std::sin(t * pi);
    default:
        return t;
    }
}
float move_eased(float t, int easing, int curve) {
    if (t <= 0 || t >= 1 || easing == 0 || curve < 1 || curve > 10)
        return t;
    if (easing == 1)
        return ease_in(t, curve);
    if (easing == 2)
        return 1 - ease_in(1 - t, curve);
    if (easing == 3)
        return t < .5f ? .5f * ease_in(2 * t, curve) : 1 - .5f * ease_in(2 - 2 * t, curve);
    return t;
}
// Animate only known continuous controls. New settings conservatively keep the
// two-treatment blend until their interpolation behaviour is defined here.
enum class ControlGroup { Scene, Light, Rim, Shadow, Reactive, Screen };
struct AnimatedControl {
    const char *key;
    float Compositor::*field = nullptr;
    float LightingValues::*light_field = nullptr;
    float SceneView::*view_field = nullptr;
    float angle_wrap_period = 0;
    bool interpolate_in_stops = false;
    ControlGroup group = ControlGroup::Scene;

    constexpr AnimatedControl(const char *key, float Compositor::*field, float wrap = 0, bool stops = false,
                              ControlGroup group = ControlGroup::Scene)
        : key(key), field(field), angle_wrap_period(wrap), interpolate_in_stops(stops), group(group) {}
    constexpr AnimatedControl(const char *key, float LightingValues::*field, float wrap = 0,
                              bool stops = false, ControlGroup group = ControlGroup::Light)
        : key(key), light_field(field), angle_wrap_period(wrap), interpolate_in_stops(stops), group(group) {}
    constexpr AnimatedControl(const char *key, float SceneView::*field) : key(key), view_field(field) {}

    float read(const Compositor *compositor) const {
        if (view_field)
            return compositor->view.*view_field;
        if (light_field)
            return compositor->lighting.*light_field;
        return compositor->*field;
    }
    void write(Compositor *compositor, float value) const {
        if (view_field)
            compositor->view.*view_field = scene_view_coordinate(value);
        else if (light_field)
            compositor->lighting.*light_field = value;
        else
            compositor->*field = value;
    }
};
constexpr AnimatedControl animated_controls[] = {
    {"camera_x", &SceneView::x},
    {"camera_y", &SceneView::y},
    {"spotlight_enabled", &LightingValues::spotlight_amount, 0, false, ControlGroup::Light},
    {"light_rays", &LightingValues::rays_amount, 0, false, ControlGroup::Light},
    {"light_ray_detail", &LightingValues::rays_detail, 0, false, ControlGroup::Light},
    {"light_ray_width", &LightingValues::ray_width, 0, false, ControlGroup::Light},
    {"leaf_time_offset", &LightingValues::leaf_time_offset, 0, false, ControlGroup::Light},
    {"ray_time_offset", &LightingValues::ray_time_offset, 0, false, ControlGroup::Light},
    {"leaf_shadow_variation", &LightingValues::leaf_shadow_variation, 0, false, ControlGroup::Light},
    {"leaf_bottom_fade", &LightingValues::leaf_bottom_fade, 0, false, ControlGroup::Light},
    {"leaf_softness", &LightingValues::leaf_softness, 0, false, ControlGroup::Light},
    {"leaf_depth", &LightingValues::leaf_depth, 0, false, ControlGroup::Light},
    {"leaf_speed", &LightingValues::leaf_speed, 0, false, ControlGroup::Light},
    {"leaf_sway", &LightingValues::leaf_sway, 0, false, ControlGroup::Light},
    {"leaf_shimmer", &LightingValues::leaf_shimmer, 0, false, ControlGroup::Light},
    {"leaf_variation", &LightingValues::leaf_variation, 0, false, ControlGroup::Light},
    {"leaf_shadow_level", &LightingValues::leaf_shadow_level, 0, false, ControlGroup::Light},
    {"leaf_edge_amount", &LightingValues::leaf_edge_amount, 0, false, ControlGroup::Light},
    {"canopy_scale", &LightingValues::canopy_scale, 0, false, ControlGroup::Light},
    {"canopy_rotation", &LightingValues::canopy_rotation, 360, false, ControlGroup::Light},
    {"pattern_size", &LightingValues::pattern_size, 0, false, ControlGroup::Light},
    {"pattern_amount", &LightingValues::pattern_amount, 0, false, ControlGroup::Light},
    {"motion_drift", &LightingValues::motion_drift, 0, false, ControlGroup::Light},
    {"motion_opposing", &LightingValues::motion_opposing, 0, false, ControlGroup::Light},
    {"motion_fading", &LightingValues::motion_fading, 0, false, ControlGroup::Light},
    {"light_coverage", &LightingValues::light_coverage, 0, false, ControlGroup::Light},
    {"light_opacity", &LightingValues::light_opacity, 0, false, ControlGroup::Light},

    {"light_ray_movement", &LightingValues::ray_movement, 0, false, ControlGroup::Light},
    {"light_surface", &LightingValues::spotlight_surface, 0, false, ControlGroup::Light},
    {"spotlight_x", &LightingValues::spotlight_x, 0, false, ControlGroup::Light},
    {"spotlight_y", &LightingValues::spotlight_y, 0, false, ControlGroup::Light},
    {"spotlight_size", &LightingValues::spotlight_size, 0, false, ControlGroup::Light},
    {"spotlight_tilt", &LightingValues::spotlight_tilt, 0, false, ControlGroup::Light},
    {"spotlight_direction", &LightingValues::spotlight_direction, 360, false, ControlGroup::Light},
    {"spotlight_falloff", &LightingValues::spotlight_falloff, 0, false, ControlGroup::Light},
    {"spotlight_softness", &LightingValues::spotlight_softness, 0, false, ControlGroup::Light},
    {"spotlight_brightness", &LightingValues::spotlight_brightness, 0, false, ControlGroup::Light},
    {"spotlight_darkness", &LightingValues::spotlight_darkness, 0, false, ControlGroup::Light},
    {"subject_depth", &Compositor::subject_depth, 0, false, ControlGroup::Scene},
    {"shadow_opacity", &Compositor::shadow_amount, 0, false, ControlGroup::Shadow},
    {"shadow_size", &Compositor::shadow_size, 0, false, ControlGroup::Shadow},
    {"shadow_softness", &Compositor::shadow_softness, 0, false, ControlGroup::Shadow},
    {"shadow_distance", &Compositor::shadow_distance, 0, false, ControlGroup::Shadow},
    {"shadow_angle", &Compositor::shadow_angle, 360, false, ControlGroup::Shadow},
    {"rim_size", &Compositor::rim_size, 0, false, ControlGroup::Rim},
    {"rim_brightness", &Compositor::rim_brightness, 0, false, ControlGroup::Rim},
    {"rim_contrast", &Compositor::rim_contrast, 0, false, ControlGroup::Rim},
    {"rim_angle", &Compositor::rim_angle, 360, false, ControlGroup::Rim},
    {"rim_opacity", &Compositor::rim_opacity, 0, false, ControlGroup::Rim},
    {"subject_exposure", &Compositor::subject_exposure_gain, 0, true, ControlGroup::Scene},
    {"background_exposure", &Compositor::background_exposure_gain, 0, true, ControlGroup::Scene},
    {"exposure", &Compositor::exposure_gain, 0, true, ControlGroup::Scene},
    {"radius", &Compositor::radius, 0, false, ControlGroup::Screen},
    {"threshold", &Compositor::threshold, 0, false, ControlGroup::Screen},
    {"screen_intensity", &Compositor::screen_intensity, 0, false, ControlGroup::Screen},
    {"screen_opacity", &Compositor::screen_opacity, 0, false, ControlGroup::Screen},
    {"screen_contrast", &Compositor::screen_contrast, 0, false, ControlGroup::Screen},
    {"brightness", &Compositor::brightness, 0, false, ControlGroup::Scene},
    {"contrast", &Compositor::contrast, 0, false, ControlGroup::Scene},
    {"saturation", &Compositor::saturation, 0, false, ControlGroup::Scene},
    {"auto_blend", &Compositor::color_match_strength, 0, false, ControlGroup::Scene},
    {"light_strength", &Compositor::reactive_strength, 0, false, ControlGroup::Reactive},
    {"flare_size", &LightingValues::lightwash_size, 0, false, ControlGroup::Light},
    {"flare_intensity", &LightingValues::lightwash_intensity, 0, false, ControlGroup::Light},
    {"flare_opacity", &LightingValues::lightwash_opacity, 0, false, ControlGroup::Light},
    {"flare_angle", &LightingValues::lightwash_angle, 6.28318530718f, false, ControlGroup::Light},
};
struct Animation {
    std::vector<std::pair<float, float>> values;
    color::Transition colors[7];
    bool rim = false, layers = false, light = true;
    bool interpolate_rim = false, interpolate_shadow = false, interpolate_reactive = false,
         interpolate_screen = false;
};
bool animated_setting(const std::string &name) {
    for (const auto &control : animated_controls)
        if (name == control.key)
            return true;
    return name == "leaf_detail" || name == "leaf_density" || name == "leaf_seed" ||
           name == "contrast_adjust" || name == "saturation_adjust" || name == "rim_color" ||
           name == "color_filter" || name == "flare_color" || name == "spotlight_color" ||
           name == "spotlight_light_color" || name == "rim_enabled" || name == "shadow_enabled" ||
           name == "lightwash_enabled";
}
Animation animation_between(const Compositor *a, const Compositor *b) {
    Animation result;
    result.light = a->lighting.light_type == b->lighting.light_type &&
                   a->lighting.light_pattern == b->lighting.light_pattern &&
                   a->lighting.lightwash_type == b->lighting.lightwash_type &&
                   (a->lighting.light_pattern != 1 || same_canopy(a, b));
    result.interpolate_rim = a->rim_enabled && b->rim_enabled && a->rim_type == b->rim_type &&
                             a->rim_blend == b->rim_blend && a->rim_follow_light == b->rim_follow_light;
    result.interpolate_rim = result.interpolate_rim && (a->rim_follow_light != 2 || result.light);
    result.interpolate_shadow =
        a->shadow_amount > 0 && b->shadow_amount > 0 && a->shadow_follow == b->shadow_follow;
    result.interpolate_reactive =
        a->reactive_source_id == b->reactive_source_id && a->reactive_background == b->reactive_background &&
        a->exposure_background == b->exposure_background && a->bounded_reactive == b->bounded_reactive;
    result.interpolate_screen = a->screen_mode == b->screen_mode && a->screen_mode != 0;
    for (const auto &control : animated_controls) {
        float start = control.read(a);
        float end = control.read(b);
        if (control.field == &Compositor::rim_opacity) {
            if (!a->rim_enabled)
                start = 0;
            if (!b->rim_enabled)
                end = 0;
        }
        result.values.push_back({start, end});
    }
    result.colors[0].set(a->rim_color, b->rim_color);
    result.colors[1].set(a->color_filter, b->color_filter);
    result.colors[2].set(a->lighting.lightwash_color, b->lighting.lightwash_color);
    result.colors[3].set(a->lighting.spotlight_color, b->lighting.spotlight_color);
    result.colors[4].set(a->lighting.spotlight_light_color, b->lighting.spotlight_light_color);
    result.colors[5].set(a->lighting.leaf_shadow_color, b->lighting.leaf_shadow_color);
    result.colors[6].set(a->lighting.leaf_tint, b->lighting.leaf_tint);
    result.rim = a->rim_enabled || b->rim_enabled;
    result.layers = a->wants_layers || b->wants_layers;
    return result;
}
void animate(Compositor *f, const Animation &animation, float t) {
    size_t index = 0;
    for (const auto &control : animated_controls) {
        const auto endpoints = animation.values[index++];
        switch (control.group) {
        case ControlGroup::Light:
            if (!animation.light)
                continue;
            break;
        case ControlGroup::Rim:
            if (!animation.interpolate_rim)
                continue;
            break;
        case ControlGroup::Shadow:
            if (!animation.interpolate_shadow)
                continue;
            break;
        case ControlGroup::Reactive:
            if (!animation.interpolate_reactive)
                continue;
            break;
        case ControlGroup::Screen:
            if (!animation.interpolate_screen)
                continue;
            break;
        case ControlGroup::Scene:
            break;
        }
        float a = endpoints.first, b = endpoints.second;
        if (control.interpolate_in_stops) {
            a = std::log2(a);
            b = std::log2(b);
        }
        const float delta =
            control.angle_wrap_period ? std::remainder(b - a, control.angle_wrap_period) : b - a;
        // Preserve angular overshoot, bounded amounts stay within their endpoints.
        float value = a + delta * (control.angle_wrap_period ? t : std::clamp(t, 0.f, 1.f));
        if (control.angle_wrap_period)
            value = std::remainder(value, control.angle_wrap_period);
        control.write(f, control.interpolate_in_stops ? std::exp2(value) : value);
    }
    vec4 *colors[] = {&f->rim_color,
                      &f->color_filter,
                      &f->lighting.lightwash_color,
                      &f->lighting.spotlight_color,
                      &f->lighting.spotlight_light_color,
                      &f->lighting.leaf_shadow_color,
                      &f->lighting.leaf_tint};
    for (int i = 0; i < 7; ++i)
        if ((i != 0 || animation.interpolate_rim) && (animation.light || i < 2))
            *colors[i] = animation.colors[i].at(std::clamp(t, 0.f, 1.f));
    if (animation.interpolate_rim)
        f->rim_enabled = animation.rim;
    f->wants_layers = animation.layers;
    f->diffusion = f->screen_mode == 2 ? std::min(1.f, f->screen_intensity * f->screen_opacity) : 0;
    f->glow = f->screen_mode == 3 ? f->screen_intensity * f->screen_opacity : 0;
    f->processed = false;
}
struct TransitionEndpoint {
    RimLightGeometry rim_geometry[2];
    vec2 shadow_pivot[2] = {{.5f, .5f}, {.5f, .5f}};
    ReactiveGeometry spatial_geometry[2];
    obs_scene_t *treatment_scene = nullptr;
    obs_source_t *treatment_filter = nullptr;
    obs_source_t *endpoint_compositor_ref = nullptr;
    gs_texrender_t *output_target = nullptr;
};
struct Bridge {
    obs_source_t *source;
    obs_source_t *from = nullptr, *to = nullptr;
    TransitionEndpoint endpoints[2];
    std::vector<std::pair<obs_source_t *, obs_source_t *>> tags;
    bool attempted = false, ready = false, warned = false, identical = false;
    int easing = 0, easing_curve = 0;
    bool primed = false, animated = false, one_mode = false, interpolate = false;
    Animation animation;
    gs_effect_t *fade_effect = nullptr;
    gs_texrender_t *placement_replay = nullptr;
    bool replay_placements = false, has_group_inputs = false, has_recursive_groups = false;
    std::vector<obs_source_t *> group_moves; // Borrowed from this bridge's owned capture tags.
    uint64_t completed = 0, pair_start = 0;
};
void handoff_history(Bridge *b, obs_source_t *transition) {
    if (!b->ready || b->completed == b->pair_start)
        return;
    auto *active = obs_transition_get_active_source(transition);
    const int endpoint = active == b->to ? 1 : active == b->from ? 0 : -1;
    obs_source_release(active);
    if (endpoint < 0)
        return;
    const bool single = b->identical || (b->animated && b->one_mode);
    auto *rendered = b->endpoints[single ? 0 : endpoint].treatment_filter;
    auto *owner = b->endpoints[endpoint].endpoint_compositor_ref;
    if (rendered && owner) {
        auto *from = static_cast<Compositor *>(obs_obj_get_data(rendered));
        auto *to = static_cast<Compositor *>(obs_obj_get_data(owner));
        if (copy_color_match_history(from, to))
            to->processed = false;
    }
}
void clear(Bridge *b) {
    if (b->attempted && b->from && b->to)
        blog(LOG_DEBUG, "[Glowbox] Move pair %s -> %s: %llu composited frames", obs_source_get_name(b->from),
             obs_source_get_name(b->to), (unsigned long long)(b->completed - b->pair_start));
    b->warned = false;
    b->animated = false;
    b->interpolate = false;
    b->animation = {};
    for (auto &tag : b->tags) {
        disconnect_tag_start(static_cast<Tag *>(obs_obj_get_data(tag.second)));
        obs_source_filter_remove(tag.first, tag.second);
        obs_source_release(tag.second);
        obs_source_release(tag.first);
    }
    b->tags.clear();
    gs_texrender_destroy(b->placement_replay);
    b->placement_replay = nullptr;
    b->replay_placements = false;
    b->has_group_inputs = false;
    b->has_recursive_groups = false;
    b->group_moves.clear();
    gs_effect_destroy(b->fade_effect);
    b->fade_effect = nullptr;
    b->primed = false;
    for (auto &endpoint : b->endpoints) {
        obs_source_release(endpoint.treatment_filter);
        if (endpoint.treatment_scene)
            obs_scene_release(endpoint.treatment_scene);
        obs_source_release(endpoint.endpoint_compositor_ref);
        gs_texrender_destroy(endpoint.output_target);
        endpoint = {};
    }
    obs_source_release(b->from);
    obs_source_release(b->to);
    b->from = b->to = nullptr;
    b->ready = b->attempted = false;
}
struct Filters {
    obs_source_t *glow = nullptr;
    bool other = false;
};
void find_filter(obs_source_t *, obs_source_t *filter, void *data) {
    if (!obs_source_enabled(filter))
        return;
    auto *f = static_cast<Filters *>(data);
    const char *id = obs_source_get_unversioned_id(filter);
    if (!strcmp(id, tag_id))
        return;
    if (!strcmp(id, "glowbox_compositor_filter") && !f->glow)
        f->glow = obs_source_get_ref(filter);
    else
        f->other = true;
}
struct Item {
    obs_source_t *source;
    obs_sceneitem_t *item[2] = {};
    bool subject[2]{};
};
struct Items : std::vector<Item> {
    ~Items() {
        for (auto &entry : *this)
            for (auto *item : entry.item)
                if (item)
                    obs_sceneitem_release(item);
    }
};
struct Collect {
    std::vector<Item> *items;
    int side;
    obs_data_t *config;
    bool valid = true;
    bool camera_active = false, camera_intermediate = false, has_group_inputs = false;
    bool inherited_role = false, parent_subject = false;
    bool override_child_transition = false;
};
void find_override_child_transition(obs_source_t *, obs_source_t *filter, void *data) {
    if (!obs_source_enabled(filter) ||
        strcmp(obs_source_get_unversioned_id(filter), "move_transition_override_filter"))
        return;
    auto *settings = obs_source_get_settings(filter);
    for (const char *key : {"transition_match", "transition_in", "transition_out"}) {
        const char *name = obs_data_get_string(settings, key);
        if (*name && strcmp(name, "None"))
            *static_cast<bool *>(data) = true;
    }
    obs_data_release(settings);
}
bool collect(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    auto *c = static_cast<Collect *>(data);
    auto *source = obs_sceneitem_get_source(item);
    // Move does not draw hidden instances or audio-only inputs.
    if (!obs_sceneitem_visible(item) || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
        return true;
    const bool subject = c->inherited_role
                             ? c->parent_subject
                             : c->config && obs_data_get_bool(c->config, subject_setting_key(item).c_str());
    const bool group = obs_sceneitem_is_group(item);
    c->has_group_inputs = c->has_group_inputs || group;
    if (c->camera_active) {
        obs_source_enum_filters(source, find_override_child_transition, &c->override_child_transition);
        if (c->override_child_transition)
            c->valid = false;
        obs_sceneitem_crop crop{};
        obs_sceneitem_get_crop(item, &crop);
        c->camera_intermediate = c->camera_intermediate || group || crop.left || crop.top || crop.right ||
                                 crop.bottom || obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
        if (obs_sceneitem_get_scale_filter(item) != OBS_SCALE_DISABLE ||
            obs_sceneitem_get_blending_method(item) != OBS_BLEND_METHOD_DEFAULT ||
            (obs_source_is_scene(source) && !group))
            c->valid = false;
        if (group) {
            Collect nested = *c;
            nested.inherited_role = true;
            nested.parent_subject = subject;
            obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect, &nested);
            c->valid = c->valid && nested.valid;
            c->override_child_transition = c->override_child_transition || nested.override_child_transition;
        }
    }
    for (bool show : {false, true})
        if (obs_sceneitem_get_transition(item, show))
            c->valid = false;
    if (obs_sceneitem_get_blending_mode(item) != OBS_BLEND_NORMAL)
        c->valid = false;
    for (auto &entry : *c->items)
        if (entry.source == source) {
            if (entry.item[c->side]) {
                // One wrapper can serve every placement of a source. Move supplies
                // each transform, only conflicting per-instance subject roles
                // are ambiguous to this source-level capture hook.
                if (subject != entry.subject[c->side])
                    c->valid = false;
                return true;
            }
            obs_sceneitem_addref(item);
            entry.item[c->side] = item;
            entry.subject[c->side] = subject;
            return true;
        }
    obs_sceneitem_addref(item);
    Item entry;
    entry.source = source;
    entry.item[c->side] = item;
    entry.subject[c->side] = subject;
    c->items->push_back(entry);
    return true;
}
bool equal_settings(obs_data_t *a, obs_data_t *b, bool continuous = false) {
    if (!a || !b)
        return false;
    auto values = [continuous](obs_data_t *settings, bool &simple) {
        std::map<std::string, std::string> result;
        for (auto *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
            const std::string name = obs_data_item_get_name(item);
            // Ignore migration-only keys when comparing treatments.
            if (name == "flare_amount" || name == "lightwash_amount_curve" ||
                name == "lightwash_split_controls")
                continue;
            // Scene item IDs can differ, prepare() compares their source roles below.
            if (name.size() > 8 && name.compare(0, 8, "subject_") == 0 &&
                (name.find_first_not_of("0123456789", 8) == std::string::npos ||
                 (name.compare(0, 13, "subject_item_") == 0 &&
                  obs_data_item_gettype(item) == OBS_DATA_BOOLEAN)))
                continue;
            if (continuous && (animated_setting(name) || name == "screen_mode"))
                continue;
            std::ostringstream value;
            value << std::setprecision(17);
            auto type = obs_data_item_gettype(item);
            value << type << ":";
            if (type == OBS_DATA_STRING)
                value << obs_data_item_get_string(item);
            else if (type == OBS_DATA_BOOLEAN)
                value << obs_data_item_get_bool(item);
            else if (type == OBS_DATA_NUMBER)
                value << obs_data_item_get_double(item);
            else
                simple = false;
            result[obs_data_item_get_name(item)] = value.str();
        }
        return result;
    };
    bool simple = true;
    auto x = values(a, simple), y = values(b, simple);
    return simple && x == y;
}
struct EndpointRoles {
    const Items *items;
    obs_data_t *roles;
    int side;
};
bool copy_endpoint_role(obs_scene_t *, obs_sceneitem_t *item, void *data) {
    const auto &context = *static_cast<EndpointRoles *>(data);
    for (const auto &entry : *context.items)
        if (entry.source == obs_sceneitem_get_source(item)) {
            const int policy = entry.item[context.side] ? context.side : 1 - context.side;
            const bool subject = entry.subject[policy];
            obs_data_set_bool(context.roles, subject_setting_key(item).c_str(), subject);
            break;
        }
    return true;
}
void follow_move_light(TransitionEndpoint &endpoint, float progress) {
    auto *f = endpoint.treatment_filter
                  ? static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter))
                  : nullptr;
    if (!f)
        return;
    f->shadow_pivot = {
        endpoint.shadow_pivot[0].x + (endpoint.shadow_pivot[1].x - endpoint.shadow_pivot[0].x) * progress,
        endpoint.shadow_pivot[0].y + (endpoint.shadow_pivot[1].y - endpoint.shadow_pivot[0].y) * progress};
    f->rim_follow_ready = false;
    if (!f->rim_follow_source || (f->rim_type != 0 && f->rim_type != 2))
        return;
    const auto &a = endpoint.rim_geometry[0], &b = endpoint.rim_geometry[1];
    if (a.valid && b.valid) {
        vec2 light{a.light.x + (b.light.x - a.light.x) * progress,
                   a.light.y + (b.light.y - a.light.y) * progress};
        vec2 subject{a.subject.x + (b.subject.x - a.subject.x) * progress,
                     a.subject.y + (b.subject.y - a.subject.y) * progress};
        if (f->rim_follow_light == 2) {
            const float width =
                f->width ? (float)f->width : (float)obs_source_get_width(endpoint.endpoint_compositor_ref);
            const float height =
                f->height ? (float)f->height : (float)obs_source_get_height(endpoint.endpoint_compositor_ref);
            const auto emitter = spotlight_emitter_position(f, width / std::max(height, 1.f));
            light = {emitter.x * width, emitter.y * height};
        }
        if (scene_view_active(f->view)) {
            const float subject_distance = subject_plane_distance(f->subject_depth);
            const float scene_height =
                float(f->height ? f->height : obs_source_get_height(endpoint.endpoint_compositor_ref));
            const float light_weight =
                a.light_subject_weight +
                (b.light_subject_weight - a.light_subject_weight) * std::clamp(progress, 0.f, 1.f);
            light = scene_project_point(f->view, 1.f + (subject_distance - 1.f) * light_weight, scene_height,
                                        light);
            subject = scene_project_point(f->view, subject_distance, scene_height, subject);
        }
        f->rim_follow_ready = rim_angle_between(light, subject, f->rim_follow_angle);
    } else {
        float from = f->rim_angle, to = f->rim_angle;
        if (a.valid)
            rim_angle_between(a.light, a.subject, from);
        if (b.valid)
            rim_angle_between(b.light, b.subject, to);
        f->rim_follow_angle =
            std::remainder(from + std::remainder(to - from, 360.f) * std::clamp(progress, 0.f, 1.f), 360.f);
        f->rim_follow_ready = true;
    }
}
void move_spatial_light(TransitionEndpoint &endpoint, float progress) {
    auto *f = endpoint.treatment_filter
                  ? static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter))
                  : nullptr;
    if (!f)
        return;
    f->reactive_geometry_override = true;
    const auto &a = endpoint.spatial_geometry[0], &b = endpoint.spatial_geometry[1];
    auto g = a.valid ? a : b;
    if (a.valid && b.valid) {
        for (int i = 0; i < 2; ++i) {
            g.origin.ptr[i] = a.origin.ptr[i] + (b.origin.ptr[i] - a.origin.ptr[i]) * progress;
            g.axis_x.ptr[i] = a.axis_x.ptr[i] + (b.axis_x.ptr[i] - a.axis_x.ptr[i]) * progress;
            g.axis_y.ptr[i] = a.axis_y.ptr[i] + (b.axis_y.ptr[i] - a.axis_y.ptr[i]) * progress;
        }
        for (int i = 0; i < 4; ++i)
            g.crop.ptr[i] = a.crop.ptr[i] + (b.crop.ptr[i] - a.crop.ptr[i]) * progress;
        g.receiver_extent =
            a.receiver_extent + (b.receiver_extent - a.receiver_extent) * std::clamp(progress, 0.f, 1.f);
        g.depth = std::max(
            {.06f, .12f * std::max(std::hypot(g.axis_x.x, g.axis_x.y), std::hypot(g.axis_y.x, g.axis_y.y)),
             .25f * g.receiver_extent});
    } else
        g.amount = a.valid ? 1 - std::clamp(progress, 0.f, 1.f) : std::clamp(progress, 0.f, 1.f);
    g.subject_plane_weight = a.subject_plane_weight + (b.subject_plane_weight - a.subject_plane_weight) *
                                                          std::clamp(progress, 0.f, 1.f);
    project_reactive_geometry(f, g);
    f->reactive_geometry = g;
}
bool prepare(Bridge *b, obs_source_t *a, obs_source_t *z) {
    b->attempted = true;
    b->pair_start = b->completed;
    auto *move_settings = obs_source_get_settings(obs_filter_get_parent(b->source));
    b->easing = (int)obs_data_get_int(move_settings, "easing_match");
    b->easing_curve = (int)obs_data_get_int(move_settings, "easing_function_match");
    bool direct_children = true;
    for (const char *key : {"transition_match", "transition_in", "transition_out"}) {
        const char *name = obs_data_get_string(move_settings, key);
        direct_children = direct_children && (!*name || !strcmp(name, "None"));
    }
    obs_data_release(move_settings);
    b->from = obs_source_get_ref(a);
    b->to = obs_source_get_ref(z);
    auto *sa = obs_scene_from_source(a), *sz = obs_scene_from_source(z);
    if (!sa || !sz)
        return false;
    Filters filters[2];
    obs_source_enum_filters(a, find_filter, &filters[0]);
    obs_source_enum_filters(z, find_filter, &filters[1]);
    auto release_filters = [&]() {
        for (auto &f : filters)
            obs_source_release(f.glow);
    };
    if ((!filters[0].glow && !filters[1].glow) || filters[0].other || filters[1].other) {
        release_filters();
        return false;
    }
    obs_data_t *configs[2] = {filters[0].glow ? obs_source_get_settings(filters[0].glow) : nullptr,
                              filters[1].glow ? obs_source_get_settings(filters[1].glow) : nullptr};
    const auto camera_requested = [](obs_data_t *config) {
        return config && (scene_view_coordinate(obs_data_get_double(config, "camera_x")) != 0 ||
                          scene_view_coordinate(obs_data_get_double(config, "camera_y")) != 0);
    };
    const bool camera_active = camera_requested(configs[0]) || camera_requested(configs[1]);
    Items items;
    Collect ca{&items, 0, configs[0]}, cz{&items, 1, configs[1]};
    ca.camera_active = cz.camera_active = camera_active;
    obs_scene_enum_items(sa, collect, &ca);
    obs_scene_enum_items(sz, collect, &cz);
    b->replay_placements = camera_active && (ca.camera_intermediate || cz.camera_intermediate);
    b->has_group_inputs = ca.has_group_inputs || cz.has_group_inputs;
    if ((camera_active && !direct_children) || !ca.valid || !cz.valid) {
        if (camera_active) {
            if (ca.override_child_transition || cz.override_child_transition)
                blog(LOG_INFO, "[Glowbox] Camera parallax is unavailable for Move source override child "
                               "transitions; using ordinary Move rendering");
            else
                blog(LOG_INFO, "[Glowbox] Camera parallax is unavailable for this Move pair: unsupported "
                               "filtering, nested scene, child transition, blending or conflicting source "
                               "roles; using ordinary Move rendering");
            b->warned = true;
        } else
            blog(LOG_WARNING, "[Glowbox] Move: unsupported item transition/blending or conflicting subject "
                              "roles for repeated source");
        for (auto *config : configs)
            obs_data_release(config);
        release_filters();
        return false;
    }
    b->identical = equal_settings(configs[0], configs[1]);
    b->animated = equal_settings(configs[0], configs[1], true);
    // Legacy implicit dual-screen settings do not have one mode to animate.
    for (auto *config : configs)
        if (config && !obs_data_has_user_value(config, "screen_mode") &&
            (obs_data_has_user_value(config, "diffusion") || obs_data_has_user_value(config, "glow")))
            b->animated = false;
    for (const auto &entry : items) {
        bool roles[2]{};
        for (int n = 0; n < 2; ++n) {
            int policy = entry.item[n] ? n : 1 - n;
            roles[n] = entry.subject[policy];
        }
        b->identical = b->identical && roles[0] == roles[1];
        b->animated = b->animated && roles[0] == roles[1];
    }
    bool ok = true;
    bool has_light[2]{};
    for (int n = 0; n < 2 && ok; ++n) {
        auto &endpoint = b->endpoints[n];
        endpoint.output_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (!endpoint.output_target) {
            ok = false;
            break;
        }
        if (!configs[n])
            continue; // This endpoint has no enabled compositor: use untreated Move.
        endpoint.treatment_scene = obs_scene_create_private("Glowbox transition treatment");
        auto *config = obs_data_create();
        obs_data_apply(config, configs[n]);
        for (const auto &entry : items) {
            auto *original = entry.item[n] ? entry.item[n] : entry.item[1 - n];
            auto *copy = obs_scene_add(endpoint.treatment_scene, entry.source);
            obs_sceneitem_set_visible(copy, (entry.item[0] && obs_sceneitem_visible(entry.item[0])) ||
                                                (entry.item[1] && obs_sceneitem_visible(entry.item[1])));
            obs_transform_info transform{};
            obs_sceneitem_get_info2(original, &transform);
            obs_sceneitem_set_info2(copy, &transform);
            obs_sceneitem_crop crop{};
            obs_sceneitem_get_crop(original, &crop);
            obs_sceneitem_set_crop(copy, &crop);
            obs_sceneitem_set_scale_filter(copy, obs_sceneitem_get_scale_filter(original));
            const int policy = entry.item[n] ? n : 1 - n;
            bool subject = entry.subject[policy];
            obs_data_set_bool(config, subject_setting_key(copy).c_str(), subject);
        }
        endpoint.endpoint_compositor_ref = obs_source_get_ref(filters[n].glow);
        auto *owner = endpoint.endpoint_compositor_ref
                          ? static_cast<Compositor *>(obs_obj_get_data(endpoint.endpoint_compositor_ref))
                          : nullptr;
        endpoint.treatment_filter = create_transition_treatment(config, owner);
        obs_data_release(config);
        if (!endpoint.treatment_filter) {
            ok = false;
            break;
        }
        obs_source_filter_add(obs_scene_get_source(endpoint.treatment_scene), endpoint.treatment_filter);
        auto *state = static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter));
        has_light[n] = resolve_scene_light(state, n ? sz : sa);
        if (has_light[n]) {
            b->identical = false;
        }
    }
    // An absent light is the other endpoint's effect at zero strength, not
    // a default Spotlight. Never interpolate light into an unconfigured mode.
    if (ok && has_light[0] != has_light[1]) {
        const int absent = has_light[0] ? 1 : 0;
        if (b->endpoints[absent].treatment_filter) {
            auto *state = static_cast<Compositor *>(obs_obj_get_data(b->endpoints[absent].treatment_filter));
            resolve_scene_light(state, absent ? sa : sz);
            state->lighting.spotlight_amount = 0;
            state->lighting.rays_amount = 0;
            state->lighting.lightwash_intensity = 0;
        }
    }
    // Discrete differences require two render treatments, not two stationary
    // lights. Both treatments must follow the same continuous trajectory.
    // Keep each endpoint's discrete modes (including light/pattern type).
    if (ok && b->endpoints[0].treatment_filter && b->endpoints[1].treatment_filter && !b->identical) {
        b->interpolate = true;
        auto *from = static_cast<Compositor *>(obs_obj_get_data(b->endpoints[0].treatment_filter));
        auto *to = static_cast<Compositor *>(obs_obj_get_data(b->endpoints[1].treatment_filter));
        b->animation = animation_between(from, to);
        // Compare Scene Light modes separately from compositor settings.
        b->animated = b->animated && from->lighting.light_type == to->lighting.light_type &&
                      from->lighting.light_pattern == to->lighting.light_pattern &&
                      from->lighting.lightwash_type == to->lighting.lightwash_type &&
                      (from->lighting.light_pattern != 1 || same_canopy(from, to));
        b->animated = b->animated && from->rim_enabled == to->rim_enabled &&
                      (!from->rim_enabled || b->animation.interpolate_rim);
        b->one_mode = from->screen_mode == to->screen_mode;
    }
    if (ok)
        for (int n = 0; n < 2; ++n)
            if (b->endpoints[n].treatment_filter) {
                auto &endpoint = b->endpoints[n];
                auto *f = static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter));
                const bool shadows = (configs[0] && obs_data_get_bool(configs[0], "shadow_enabled")) ||
                                     (configs[1] && obs_data_get_bool(configs[1], "shadow_enabled"));
                if (!f->rim_follow_source && !reactive_requested(f) && !shadows)
                    continue;
                for (int endpoint_index = 0; endpoint_index < 2; ++endpoint_index) {
                    auto *scene = endpoint_index ? sz : sa;
                    auto *roles = obs_data_create();
                    EndpointRoles context{&items, roles, n};
                    obs_scene_enum_items(scene, copy_endpoint_role, &context);
                    if (shadows) {
                        endpoint.shadow_pivot[endpoint_index] = subject_centre(f, scene, roles);
                        endpoint.shadow_pivot[endpoint_index].x /=
                            std::max(1.f, (float)obs_source_get_width(obs_scene_get_source(scene)));
                        endpoint.shadow_pivot[endpoint_index].y /=
                            std::max(1.f, (float)obs_source_get_height(obs_scene_get_source(scene)));
                    }
                    endpoint.rim_geometry[endpoint_index] = rim_light_geometry(f, scene, roles);
                    endpoint.spatial_geometry[endpoint_index] = reactive_geometry(f, scene, roles, true);
                    obs_data_release(roles);
                }
            }
    for (auto *config : configs)
        obs_data_release(config);
    release_filters();
    if (!ok)
        return false;
    for (const auto &entry : items) {
        auto *tag = obs_source_create_private(tag_id, "Glowbox internal capture", nullptr);
        auto *state = tag ? static_cast<Tag *>(obs_obj_get_data(tag)) : nullptr;
        if (!state) {
            obs_source_release(tag);
            return false;
        }
        state->owned = true;
        obs_source_filter_add(entry.source, tag);
        // OBS UI bottom is the outermost filter. Capture after existing filters,
        // so their cached colour inputs cannot bypass or overwrite our masks.
        obs_source_filter_set_order(entry.source, tag, OBS_ORDER_MOVE_BOTTOM);
        b->tags.push_back({obs_source_get_ref(entry.source), tag});
    }
    return true;
}
struct ReplayContext {
    obs_source_t *transition;
    Compositor *filter = nullptr;
    std::vector<obs_source_t *> drawn;
    Bridge *bridge = nullptr;
    int endpoint_index = 0;
};
bool replay_uses_coverage(const SceneCaptureLayer *layers, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (layers[i].wrapper == layers[i].original)
            continue;
        auto *view = static_cast<LayerView *>(obs_obj_get_data(layers[i].wrapper));
        return view->mask || is_coverage_capture(view->capture_mode);
    }
    return false;
}
bool replay_capture(void *data, const SceneCaptureLayer *layers, size_t count) {
    auto *context = static_cast<ReplayContext *>(data);
    if (capture_layers)
        return false;
    std::vector<uint8_t> seen(count, 0);
    capture_failed = false;
    capture_layers = layers;
    capture_count = count;
    capture_seen = seen.data();
    capture_mask = replay_uses_coverage(layers, count);
    obs_source_video_render(context->transition);
    capture_layers = nullptr;
    capture_count = 0;
    capture_seen = nullptr;
    capture_mask = false;
    if (capture_failed)
        return false;
    if (context->filter && context->filter->scene_capture_is_base_pass) {
        context->drawn.clear();
        for (size_t i = 0; i < count; ++i)
            if (seen[i])
                context->drawn.push_back(layers[i].original);
    } else
        for (size_t i = 0; i < count; ++i) {
            // Move may select one of two matched sources, or omit an entering item.
            // Require masks for sources it actually drew in this frame's base pass.
            const bool required = !context->filter || std::find(context->drawn.begin(), context->drawn.end(),
                                                                layers[i].original) != context->drawn.end();
            if (required && layers[i].visible && !seen[i] && obs_source_get_width(layers[i].original) &&
                obs_source_get_height(layers[i].original)) {
                blog(LOG_DEBUG, "[Glowbox] Missing replay mask: %s", obs_source_get_name(layers[i].original));
                return false;
            }
        }
    return true;
}
// Source hooks run before Move's crop/bounds texture is placed. Capture completed
// placement runs instead, replaying real Move preserves its paths and draw order.
bool replay(void *data, const SceneCaptureLayer *layers, size_t count) {
    auto *context = static_cast<ReplayContext *>(data);
    auto *f = context->filter;
    auto *b = context->bridge;
    if (!b || (!b->replay_placements && !b->has_recursive_groups) || !f)
        return replay_capture(data, layers, count);
    PlacementReplayState state;
    state.endpoint_index = context->endpoint_index;
    state.group_moves = &b->group_moves;
    PlacementReplayScope scope(state);
    auto validate = [&](bool ok) {
        if (state.mixed_plane_fade) {
            if (!b->warned)
                blog(LOG_INFO, "[Glowbox] Camera parallax cannot split a Move Fade across subject and "
                               "background planes; using ordinary Move rendering");
            b->warned = true;
            b->ready = false;
        }
        return ok;
    };
    // Move also owns placement during unprojected reference sampling and a
    // camera crossing through zero, private group-origin compensation is absent.
    if (!b->replay_placements || !scene_view_active(f->view) || f->sampling_reference)
        return validate(replay_capture(data, layers, count));
    const uint32_t width = f->width, height = f->height;
    if (!width || !height)
        return false;
    if (!b->placement_replay)
        b->placement_replay = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!b->placement_replay)
        return false;
    auto &order = state.observed_order;
    auto capture = [&](vec2 offset) {
        gs_texrender_reset(b->placement_replay);
        if (!gs_texrender_begin(b->placement_replay, width, height))
            return false;
        vec4 zero{};
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, float(width), 0, float(height), -100, 100);
        gs_matrix_translate3f(offset.x, offset.y, 0);
        const bool ok = replay_capture(data, layers, count);
        gs_texrender_end(b->placement_replay);
        return validate(ok);
    };
    // Rediscover each frame: Move can change draw order. Each contiguous plane
    // run then costs one additional full replay of the real transition.
    bool ok = capture({0, 0});
    if (!ok)
        return false;
    state.discovering = false;
    const bool coverage = replay_uses_coverage(layers, count);
    const bool srgb = gs_framebuffer_srgb_enabled(), linear = gs_set_linear_srgb(false);
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    for (size_t begin = 0; begin < order.size() && ok;) {
        size_t end = begin + 1;
        while (end < order.size() && order[end].is_subject_plane == order[begin].is_subject_plane)
            ++end;
        state.ordinal = 0;
        state.run_begin = begin;
        state.run_end = end;
        const auto offset = scene_plane_translation(
            f->view, order[begin].is_subject_plane ? subject_plane_distance(f->subject_depth) : 1.f,
            float(height));
        ok = capture(offset);
        ok = ok && state.ordinal == order.size();
        if (ok) {
            auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            auto *image = gs_effect_get_param_by_name(effect, "image");
            auto *texture = gs_texrender_get_texture(b->placement_replay);
            if (coverage) {
                gs_effect_set_texture(image, texture);
            } else {
                // OBS composites scene colour in linear light. Runs are encoded
                // textures, decode them while blending, then encode the result.
                gs_set_linear_srgb(true);
                gs_enable_framebuffer_srgb(true);
                gs_effect_set_texture_srgb(image, texture);
            }
            while (gs_effect_loop(effect, "Draw"))
                gs_draw_sprite(nullptr, 0, width, height);
            // Keep every subsequent Move capture in its original render state.
            if (!coverage) {
                gs_enable_framebuffer_srgb(false);
                gs_set_linear_srgb(false);
            }
        }
        begin = end;
    }
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    gs_set_linear_srgb(linear);
    return ok;
}

bool collect_capture_transitions(void *data, obs_source_t *source) {
    if (!strcmp(obs_source_get_unversioned_id(source), "fade_transition") ||
        !strcmp(obs_source_get_unversioned_id(source), "move_transition"))
        static_cast<std::vector<obs_source_t *> *>(data)->push_back(obs_source_get_ref(source));
    return true;
}
bool source_in_treatment(Bridge *b, obs_source_t *source) {
    if (!source)
        return false;
    struct Search {
        obs_source_t *source;
        bool found = false;
    } search{source};
    auto *scene =
        b->endpoints[0].treatment_scene ? b->endpoints[0].treatment_scene : b->endpoints[1].treatment_scene;
    if (scene)
        obs_scene_enum_items(
            scene,
            [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
                auto *search = static_cast<Search *>(data);
                search->found = search->found || obs_sceneitem_get_source(item) == search->source;
                return !search->found;
            },
            &search);
    return search.found;
}
bool same_source_policy(const SourcePolicy &a, const SourcePolicy &b) {
    return a.receive == b.receive && a.color_match == b.color_match && a.reactive == b.reactive &&
           a.exposure == b.exposure && a.saturation == b.saturation &&
           a.produce_effects == b.produce_effects && a.receive_effects == b.receive_effects &&
           a.receive_spotlight == b.receive_spotlight && a.receive_rays == b.receive_rays &&
           a.match_override == b.match_override && a.reactive_override == b.reactive_override &&
           a.tint.x == b.tint.x && a.tint.y == b.tint.y && a.tint.z == b.tint.z && a.tint.w == b.tint.w;
}
bool prepare_transition_captures(Bridge *b, bool *discovered = nullptr) {
    std::vector<obs_source_t *> transitions;
    obs_enum_all_sources(collect_capture_transitions, &transitions);
    if (transitions.empty())
        return true;
    const bool needs_fade = std::any_of(transitions.begin(), transitions.end(), [](obs_source_t *s) {
        return !strcmp(obs_source_get_unversioned_id(s), "fade_transition");
    });
    gs_effect_t *effect = b->fade_effect;
    if (needs_fade && !effect) {
        auto *module = obs_get_module("obs-transitions");
        char *path = module ? obs_find_module_file(module, "fade_transition.effect") : nullptr;
        if (path) {
            effect = gs_effect_create_from_file(path, nullptr);
            bfree(path);
        }
    }
    // Stage unattached helpers locally. A failed attempt leaves the bridge and
    // its existing item captures unchanged, so the next frame can retry safely.
    std::vector<std::pair<obs_source_t *, obs_source_t *>> captures;
    bool ok = !needs_fade || effect != nullptr;
    for (auto *source : transitions) {
        const bool known = std::any_of(b->tags.begin(), b->tags.end(), [&](const auto &entry) {
            auto *tag = static_cast<Tag *>(obs_obj_get_data(entry.second));
            return entry.first == source && (tag->nested_move || tag->fade_effect);
        });
        if (ok && !known && source != obs_filter_get_parent(b->source)) {
            auto *filter = obs_source_create_private(tag_id, "Glowbox Fade capture", nullptr);
            auto *tag = filter ? static_cast<Tag *>(obs_obj_get_data(filter)) : nullptr;
            if (tag) {
                tag->owned = true;
                tag->nested_move = !strcmp(obs_source_get_unversioned_id(source), "move_transition");
                tag->fade_effect = tag->nested_move ? nullptr : effect;
                captures.push_back({obs_source_get_ref(source), filter});
            } else {
                obs_source_release(filter);
                ok = false;
            }
        }
        obs_source_release(source);
    }
    if (!ok) {
        for (auto &capture : captures) {
            obs_source_release(capture.second);
            obs_source_release(capture.first);
        }
        if (effect != b->fade_effect)
            gs_effect_destroy(effect);
        return false;
    }
    for (auto &capture : captures) {
        obs_source_filter_add(capture.first, capture.second);
        auto *tag = static_cast<Tag *>(obs_obj_get_data(capture.second));
        tag->transition_signals = obs_source_get_signal_handler(capture.first);
        signal_handler_connect(tag->transition_signals, "transition_start", observe_tag_start, tag);
    }
    if (discovered)
        *discovered = !captures.empty();
    b->tags.insert(b->tags.end(), captures.begin(), captures.end());
    b->fade_effect = effect;
    return true;
}
struct ChildLayoutSnapshot {
    std::string source, input_a, input_b;
    uint32_t width = 0, height = 0, input_aw = 0, input_ah = 0, input_bw = 0, input_bh = 0;
    bool operator==(const ChildLayoutSnapshot &other) const {
        return source == other.source && input_a == other.input_a && input_b == other.input_b &&
               width == other.width && height == other.height && input_aw == other.input_aw &&
               input_ah == other.input_ah && input_bw == other.input_bw && input_bh == other.input_bh;
    }
};
std::vector<ChildLayoutSnapshot> snapshot_child_layout(const MovePreparation &preparation) {
    std::vector<ChildLayoutSnapshot> result;
    for (const auto &node : preparation.nodes) {
        ChildLayoutSnapshot layout;
        layout.source = obs_source_get_uuid(node.source);
        auto *a = obs_transition_get_source(node.source, OBS_TRANSITION_SOURCE_A);
        auto *z = obs_transition_get_source(node.source, OBS_TRANSITION_SOURCE_B);
        obs_transition_get_size(node.source, &layout.width, &layout.height);
        if (a) {
            layout.input_a = obs_source_get_uuid(a);
            layout.input_aw = obs_source_get_width(a);
            layout.input_ah = obs_source_get_height(a);
        }
        if (z) {
            layout.input_b = obs_source_get_uuid(z);
            layout.input_bw = obs_source_get_width(z);
            layout.input_bh = obs_source_get_height(z);
        }
        obs_source_release(a);
        obs_source_release(z);
        result.push_back(std::move(layout));
    }
    std::sort(result.begin(), result.end(),
              [](const ChildLayoutSnapshot &a, const ChildLayoutSnapshot &b) {
                  return a.source < b.source;
              });
    return result;
}
bool prepare_child_layout(Bridge *b, obs_source_t *transition, uint32_t width, uint32_t height) {
    MovePreparation preparation(transition);
    bool ok = prepare_transition_captures(b), stable = false;
    unsigned passes = 0;
    for (; ok && !stable && passes < 16; ++passes) {
        auto before = snapshot_child_layout(preparation);
        auto *target = b->endpoints[0].output_target;
        gs_texrender_reset(target);
        ok = gs_texrender_begin(target, width, height);
        if (!ok)
            break;
        vec4 zero{};
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, float(width), 0, float(height), -100, 100);
        {
            MovePreparationScope scope(preparation);
            obs_source_video_render(transition);
        }
        gs_texrender_end(target);
        bool discovered = false;
        ok = prepare_transition_captures(b, &discovered);
        stable = !discovered && before == snapshot_child_layout(preparation);
    }
    if (ok && stable) {
        for (const auto &node : preparation.nodes)
            if (node.parent == obs_source_get_uuid(transition)) {
                auto *a = obs_transition_get_source(node.source, OBS_TRANSITION_SOURCE_A);
                auto *z = obs_transition_get_source(node.source, OBS_TRANSITION_SOURCE_B);
                if (((a && obs_source_is_group(a)) || (z && obs_source_is_group(z))) &&
                    source_in_treatment(b, a) && source_in_treatment(b, z)) {
                    b->group_moves.push_back(node.source);
                    b->has_recursive_groups = true;
                    if (!same_source_policy(source_policy(a), source_policy(z)))
                        b->identical = b->animated = false;
                }
                obs_source_release(a);
                obs_source_release(z);
            }
    }
    blog(LOG_DEBUG, "[Glowbox] Move public layout preparation: passes=%u nodes=%zu groups=%zu stable=%d",
         passes, preparation.nodes.size(), b->group_moves.size(), ok && stable);
    return ok && stable;
}
bool render_transition_endpoint(Bridge *b, TransitionEndpoint &endpoint, obs_source_t *transition, uint32_t w,
                                uint32_t h) {
    gs_texrender_reset(endpoint.output_target);
    if (!gs_texrender_begin(endpoint.output_target, w, h))
        return false;
    vec4 zero{};
    gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
    gs_ortho(0, (float)w, 0, (float)h, -100, 100);
    bool ok = true;
    if (endpoint.treatment_filter &&
        !compositor_neutral(static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter)))) {
        auto *f = static_cast<Compositor *>(obs_obj_get_data(endpoint.treatment_filter));
        ReplayContext context{transition, f, {}, b, &endpoint == &b->endpoints[1] ? 1 : 0};
        ok = render_with_scene_capture(f, replay, &context, !b->replay_placements);
    } else
        obs_source_video_render(transition);
    gs_texrender_end(endpoint.output_target);
    return ok;
}
void bridge_render(void *data, gs_effect_t *) {
    auto *b = static_cast<Bridge *>(data);
    auto *parent = obs_filter_get_parent(b->source);
    if (replaying || !parent || strcmp(obs_source_get_unversioned_id(parent), "move_transition")) {
        obs_source_skip_video_filter(b->source);
        return;
    }
    const float t = obs_transition_get_time(parent);
    auto *a = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_A);
    auto *z = obs_transition_get_source(parent, OBS_TRANSITION_SOURCE_B);
    if (!a || !z || t <= 0 || t >= 1) {
        if (b->attempted) {
            handoff_history(b, parent);
            clear(b);
        }
        // Move can already have both scenes while its first video tick still
        // reports zero progress. Its item renderer omits scene filters, render
        // the complete endpoint until the composited transition can take over.
        if (a && z)
            obs_transition_video_render_direct(parent,
                                               t <= 0 ? OBS_TRANSITION_SOURCE_A : OBS_TRANSITION_SOURCE_B);
        else
            obs_source_skip_video_filter(b->source);
        obs_source_release(a);
        obs_source_release(z);
        return;
    }
    if (b->from != a || b->to != z)
        clear(b);
    if (!b->attempted) {
        // Hidden endpoints may still need their prepared shaders attached.
        // Retry setup until both endpoints have an effect.
        Filters endpoints[2];
        obs_source_enum_filters(a, find_filter, &endpoints[0]);
        obs_source_enum_filters(z, find_filter, &endpoints[1]);
        bool effects_ready = true;
        for (auto &endpoint : endpoints) {
            if (endpoint.glow) {
                auto *state = static_cast<Compositor *>(obs_obj_get_data(endpoint.glow));
                if (!state || !ensure_support_effect(state))
                    effects_ready = false;
                obs_source_release(endpoint.glow);
            }
        }
        if (!effects_ready) {
            obs_transition_video_render_direct(parent, OBS_TRANSITION_SOURCE_A);
            draw_shader_preparation(obs_source_get_base_width(parent), obs_source_get_base_height(parent));
            obs_source_release(a);
            obs_source_release(z);
            return;
        }
        b->ready = prepare(b, a, z);
    }
    obs_source_release(a);
    obs_source_release(z);
    if (!b->ready) {
        if (!b->warned) {
            blog(LOG_INFO, "[Glowbox] Move compatibility: no supported pair of scene compositors; using "
                           "ordinary Move rendering");
            b->warned = true;
        }
        obs_source_skip_video_filter(b->source);
        return;
    }
    const uint32_t w = obs_source_get_base_width(parent), h = obs_source_get_base_height(parent);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    replaying = true;
    // Let Move create its private child transitions once, before mask replay.
    bool initialized = true;
    if (!b->primed && w && h) {
        if (b->replay_placements || b->has_group_inputs)
            initialized = prepare_child_layout(b, parent, w, h);
        else {
            auto *target = b->endpoints[0].output_target;
            gs_texrender_reset(target);
            initialized = gs_texrender_begin(target, w, h);
            if (initialized) {
                vec4 zero{};
                gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
                gs_ortho(0, (float)w, 0, (float)h, -100, 100);
                obs_source_video_render(parent);
                gs_texrender_end(target);
                initialized = prepare_transition_captures(b);
            }
        }
        b->primed = initialized;
    }
    const float progress = move_eased(t, b->easing, b->easing_curve);
    const bool single = b->identical || (b->animated && b->one_mode);
    if (b->interpolate && !b->identical) {
        animate(static_cast<Compositor *>(obs_obj_get_data(b->endpoints[0].treatment_filter)), b->animation,
                progress);
        if (!single)
            animate(static_cast<Compositor *>(obs_obj_get_data(b->endpoints[1].treatment_filter)),
                    b->animation, progress);
    }
    move_spatial_light(b->endpoints[0], progress);
    if (!single)
        move_spatial_light(b->endpoints[1], progress);
    follow_move_light(b->endpoints[0], progress);
    if (!single)
        follow_move_light(b->endpoints[1], progress);
    bool ok = initialized && w && h && render_transition_endpoint(b, b->endpoints[0], parent, w, h) &&
              (single || render_transition_endpoint(b, b->endpoints[1], parent, w, h));
    if (ok) {
        auto *filter = b->endpoints[0].treatment_filter ? b->endpoints[0].treatment_filter
                                                        : b->endpoints[1].treatment_filter;
        auto *f = static_cast<Compositor *>(obs_obj_get_data(filter));
        gs_effect_set_texture(shader_parameter(f, "transition_a"),
                              gs_texrender_get_texture(b->endpoints[0].output_target));
        gs_effect_set_texture(shader_parameter(f, "transition_b"),
                              gs_texrender_get_texture(b->endpoints[single ? 0 : 1].output_target));
        gs_effect_set_float(shader_parameter(f, "transition_mix"), std::clamp(progress, 0.f, 1.f));
        while (gs_effect_loop(f->effect, "TransitionBlend"))
            gs_draw_sprite(nullptr, 0, w, h);
        ++b->completed;
    }
    replaying = false;
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    if (!ok) {
        if (!b->warned) {
            blog(LOG_WARNING,
                 "[Glowbox] Move replay could not preserve the scene masks; using ordinary Move rendering");
            b->warned = true;
        }
        obs_source_skip_video_filter(b->source);
    }
}
void bridge_tick(void *data, float) {
    auto *b = static_cast<Bridge *>(data);
    auto *parent = obs_filter_get_parent(b->source);
    if (b->attempted &&
        (!parent || (obs_transition_get_time(parent) <= 0 || obs_transition_get_time(parent) >= 1))) {
        obs_enter_graphics();
        if (parent)
            handoff_history(b, parent);
        clear(b);
        obs_leave_graphics();
    }
}
const char *bridge_name(void *) {
    return "Glowbox Move compatibility (automatic)";
}
void *bridge_create(obs_data_t *, obs_source_t *source) {
    auto *b = new Bridge;
    b->source = source;
    return b;
}
void bridge_destroy(void *data) {
    auto *b = static_cast<Bridge *>(data);
    obs_enter_graphics();
    clear(b);
    obs_leave_graphics();
    delete b;
}

void find_bridge(obs_source_t *, obs_source_t *filter, void *data) {
    if (!strcmp(obs_source_get_unversioned_id(filter), bridge_id))
        static_cast<std::vector<obs_source_t *> *>(data)->push_back(obs_source_get_ref(filter));
}
struct SavedHelpers {
    bool bridge_seen = false;
    std::vector<obs_source_t *> remove;
};
void collect_saved_helpers(obs_source_t *, obs_source_t *filter, void *data) {
    auto *helpers = static_cast<SavedHelpers *>(data);
    const char *id = obs_source_get_unversioned_id(filter);
    if (!strcmp(id, tag_id)) {
        auto *tag = static_cast<Tag *>(obs_obj_get_data(filter));
        if (!tag || !tag->owned)
            helpers->remove.push_back(obs_source_get_ref(filter));
    }
    if (!strcmp(id, bridge_id)) {
        if (helpers->bridge_seen)
            helpers->remove.push_back(obs_source_get_ref(filter));
        helpers->bridge_seen = true;
    }
}
void source_loaded(void *, calldata_t *params) {
    auto *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
    if (!source)
        return;
    SavedHelpers helpers;
    obs_source_enum_filters(source, collect_saved_helpers, &helpers);
    for (auto *filter : helpers.remove) {
        obs_source_filter_remove(source, filter);
        obs_source_release(filter);
    }
}
struct Discovery {
    std::vector<obs_source_t *> moves, stale;
};
bool gather(void *data, obs_source_t *source) {
    auto *d = static_cast<Discovery *>(data);
    if (!strcmp(obs_source_get_unversioned_id(source), "move_transition"))
        d->moves.push_back(obs_source_get_ref(source));
    if (!strcmp(obs_source_get_unversioned_id(source), tag_id)) {
        auto *tag = static_cast<Tag *>(obs_obj_get_data(source));
        if (tag && !tag->owned)
            d->stale.push_back(obs_source_get_ref(source));
    }
    return true;
}
void discover(void *, float seconds) {
    static float elapsed = 1;
    elapsed += seconds;
    if (elapsed < .1f)
        return;
    elapsed = 0;
    Discovery discovery;
    obs_enum_all_sources(gather, &discovery);
    for (auto *filter : discovery.stale) {
        auto *parent = obs_filter_get_parent(filter);
        if (parent)
            obs_source_filter_remove(parent, filter);
        obs_source_release(filter);
    }
    for (auto *source : discovery.moves) {
        std::vector<obs_source_t *> helpers;
        obs_source_enum_filters(source, find_bridge, &helpers);
        if (helpers.empty()) {
            auto *filter = obs_source_create_private(bridge_id, "Glowbox Move compatibility", nullptr);
            if (filter) {
                obs_source_filter_add(source, filter);
                obs_source_release(filter);
            }
        }
        for (size_t i = 0; i < helpers.size(); ++i) {
            if (i)
                obs_source_filter_remove(source, helpers[i]);
            obs_source_release(helpers[i]);
        }
        obs_source_release(source);
    }
}
} // namespace
void start_move_integration() {
    obs_source_info tag{};
    tag.id = tag_id;
    tag.type = OBS_SOURCE_TYPE_FILTER;
    tag.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES;
    tag.get_width = tag_width;
    tag.get_height = tag_height;
    tag.get_name = tag_name;
    tag.create = tag_create;
    tag.destroy = tag_destroy;
    tag.video_render = tag_render;
    obs_register_source(&tag);
    obs_source_info bridge{};
    bridge.id = bridge_id;
    bridge.type = OBS_SOURCE_TYPE_FILTER;
    bridge.output_flags = tag.output_flags;
    bridge.get_name = bridge_name;
    bridge.create = bridge_create;
    bridge.destroy = bridge_destroy;
    bridge.video_render = bridge_render;
    bridge.video_tick = bridge_tick;
    obs_register_source(&bridge);
    signal_handler_connect(obs_get_signal_handler(), "source_load", source_loaded, nullptr);
    obs_add_tick_callback(discover, nullptr);
}
void stop_move_integration() {
    obs_remove_tick_callback(discover, nullptr);
    signal_handler_disconnect(obs_get_signal_handler(), "source_load", source_loaded, nullptr);
}
} // namespace glowbox
