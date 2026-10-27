#include "compositor/source_participation.hpp"
#include "compositor/compositor_graphics.hpp"
#include "compositor/compositor.hpp"
#include "compositor/native_renderer.hpp"
#include "compositor/shader_preparation.hpp"
#include "effects/leaf_canopy.hpp"

namespace glowbox {
gs_eparam_t *shader_parameter(Compositor *f, const char *name) {
    return gs_effect_get_param_by_name(f->effect, name);
}
void bind_spotlight(Compositor *f, gs_effect_t *effect, gs_texture_t *fallback) {
    const auto param = [&](const char *name) { return gs_effect_get_param_by_name(effect, name); };
    gs_effect_set_bool(param("spotlight_shadow_field_ready"), f->shadow_field_ready);
    gs_effect_set_texture(param("spotlight_shadow_field"),f->shadow_field_ready
        ? gs_texrender_get_texture(f->shadow_field[0]) : fallback);
    gs_effect_set_float(param("spotlight_amount"), f->lighting.spotlight_amount);
    gs_effect_set_float(param("spotlight_surface"),f->lighting.spotlight_surface);
    gs_effect_set_float(param("rays_detail"),f->lighting.rays_detail);
    gs_effect_set_float(param("scene_light_type"),f->lighting.light_type);
    gs_effect_set_float(param("ray_width"),f->lighting.ray_width);
    gs_effect_set_float(param("leaf_shadow_variation"),f->lighting.leaf_shadow_variation);
    gs_effect_set_float(param("leaf_bottom_fade"),f->lighting.leaf_bottom_fade);
    gs_effect_set_float(param("leaf_variation"),f->lighting.leaf_variation);
    gs_effect_set_float(param("leaf_shadow_level"),f->lighting.leaf_shadow_level);
    gs_effect_set_float(param("leaf_edge_amount"),f->lighting.leaf_edge_amount);

    gs_effect_set_float(param("pattern_size"),f->lighting.pattern_size);
    gs_effect_set_float(param("pattern_amount"),f->lighting.pattern_amount);
    gs_effect_set_float(param("motion_drift"),f->lighting.motion_drift);
    gs_effect_set_float(param("motion_opposing"),f->lighting.motion_opposing);
    gs_effect_set_float(param("motion_fading"),f->lighting.motion_fading);
    gs_effect_set_float(param("light_coverage"),f->lighting.light_coverage);
    gs_effect_set_float(param("light_opacity"),f->lighting.light_opacity);
    gs_effect_set_float(param("light_energy"), f->lighting.light_type==1 ? f->lighting.lightwash_intensity : std::max(0.f,std::exp2(f->lighting.spotlight_brightness)-1.f));
    // Frame-clock time is shared by normal rendering and Move endpoint replay.
    const double seconds=light_animation_seconds();
    gs_effect_set_float(param("ray_time"),(float)(seconds*f->lighting.ray_movement+f->lighting.ray_time_offset));
    vec2 travel={-std::sin(f->lighting.spotlight_direction*.01745329252f),std::cos(f->lighting.spotlight_direction*.01745329252f)};
    gs_effect_set_vec2(param("light_travel"),&travel);
    gs_effect_set_float(param("rays_amount"),f->rays_ready ? f->lighting.rays_amount : 0.f);
    gs_effect_set_texture(param("rays_field"),f->rays_ready ? gs_texrender_get_texture(f->rays_field) : fallback);
    gs_effect_set_float(param("subject_depth"), f->subject_depth);
    const vec2 view_offset = {f->view.x, f->view.y};
    gs_effect_set_vec2(param("scene_view_offset"), &view_offset);
    gs_effect_set_float(param("spotlight_plane_scale"),spotlight_plane_scale(f));
    gs_effect_set_float(param("shadow_scale"), f->shadow_size);
    gs_effect_set_float(param("shadow_softness"), f->shadow_softness);
    gs_effect_set_float(param("shadow_follow"), f->shadow_follow ? f->lighting.spotlight_amount : 0.f);
    const float angle = f->shadow_angle * .01745329252f;
    vec2 offset = {std::sin(angle)*f->shadow_distance, -std::cos(angle)*f->shadow_distance};
    gs_effect_set_vec2(param("shadow_offset"), &offset);
    gs_effect_set_vec2(param("shadow_pivot"), &f->shadow_pivot);
    vec2 position = {f->lighting.spotlight_x, f->lighting.spotlight_y};
    vec2 canvas = {(float)f->width, (float)f->height};
    gs_effect_set_vec2(param("spotlight_position"), &position);
    gs_effect_set_vec2(param("spotlight_canvas"), &canvas);
    gs_effect_set_float(param("spotlight_size"), f->lighting.spotlight_size);
    const auto emitter=spotlight_emitter_position(f,canvas.x/std::max(canvas.y,1.f));
    const float tilt=f->lighting.spotlight_tilt*.01745329252f;
    const float direction=f->lighting.spotlight_direction*.01745329252f;
    vec3 axis;vec3_set(&axis,-std::sin(tilt)*std::sin(direction),std::sin(tilt)*std::cos(direction),-std::cos(tilt));
    gs_effect_set_vec2(param("spotlight_emitter"),&emitter);
    gs_effect_set_vec3(param("spotlight_axis"),&axis);
    gs_effect_set_float(param("spotlight_height"),2.f*std::max(.01f,f->lighting.spotlight_size));
    gs_effect_set_float(param("spotlight_falloff"),f->lighting.spotlight_falloff);
    gs_effect_set_bool(param("spotlight_source_overrides"),f->has_spotlight_overrides && f->source_spotlight_mask);
    gs_effect_set_texture(param("spotlight_source_mask"),f->source_spotlight_mask
        ? gs_texrender_get_texture(f->source_spotlight_mask) : fallback);
    gs_effect_set_float(param("spotlight_softness"), f->lighting.spotlight_softness);
    gs_effect_set_float(param("spotlight_brightness"), f->lighting.spotlight_brightness);
    gs_effect_set_float(param("spotlight_darkness"), f->lighting.spotlight_darkness);
    const auto linear = [](float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); };
    vec3 leaf_color;vec3_set(&leaf_color,linear(f->lighting.leaf_shadow_color.x),linear(f->lighting.leaf_shadow_color.y),linear(f->lighting.leaf_shadow_color.z));
    gs_effect_set_vec3(param("leaf_shadow_color"),&leaf_color);
    vec3_set(&leaf_color,linear(f->lighting.leaf_tint.x),linear(f->lighting.leaf_tint.y),linear(f->lighting.leaf_tint.z));
    gs_effect_set_vec3(param("leaf_tint"),&leaf_color);
    vec3 color; vec3_set(&color, linear(f->lighting.spotlight_color.x), linear(f->lighting.spotlight_color.y), linear(f->lighting.spotlight_color.z));
    gs_effect_set_vec3(param("spotlight_edge_color"), &color);
    vec3_set(&color,linear(f->lighting.spotlight_light_color.x),linear(f->lighting.spotlight_light_color.y),linear(f->lighting.spotlight_light_color.z));
    gs_effect_set_vec3(param("spotlight_light_color"), &color);
    auto *mask = f->subject_mask ? gs_texrender_get_texture(f->subject_mask) : nullptr;
    gs_effect_set_bool(param("spotlight_has_subject"), f->layered && mask);
    gs_effect_set_texture(param("spotlight_mask"), mask ? mask : fallback);
    gs_effect_set_float(param("spotlight_shadow"), f->layered && mask ? f->shadow_amount : 0.f);
    bind_leaf_canopy(f,effect,fallback);
}
bool prepare_spotlight_shadow(Compositor *f, gs_texture_t *original) {
    f->shadow_field_ready=false;
    if (f->lighting.light_type>0.5f || !f->layered || f->lighting.spotlight_amount<=0 || f->lighting.spotlight_surface<=0 || f->shadow_amount<=0 || !f->shadow_follow) {
        for(auto *&target : f->shadow_field){gs_texrender_destroy(target);target=nullptr;}
        return true;
    }
    // Depth animation must not resize the shadow buffer.

    const float plane=1.f+(spotlight_plane_scale(f)-1.f)*std::clamp(f->lighting.spotlight_amount,0.f,1.f);
    const float sigma=.05f*f->shadow_softness*(1.f/plane-1.f);
    const uint32_t h=std::min(f->height,270u);
    const uint32_t w=std::max(1u,(uint32_t)((uint64_t)f->width*h/f->height));
    for (auto *&target : f->shadow_field) {
        if (!target) target=gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
        if (!target) return false;
    }
    bind_spotlight(f,f->effect,original);
    vec2 texel={1.f/w,1.f/h};
    gs_effect_set_vec2(shader_parameter(f,"shadow_capture_texel"),&texel);
    if (!render_pass(f,f->shadow_field[0],original,w,h,"SpotlightShadowCapture")) return false;
    for (int axis=0;axis<2;++axis) {
        auto *input=gs_texrender_get_texture(f->shadow_field[axis]);
        vec2 step={axis==0?1.f/w:0.f,axis==1?1.f/h:0.f};
        gs_effect_set_vec2(shader_parameter(f,"shadow_blur_step"),&step);
        gs_effect_set_float(shader_parameter(f,"shadow_blur_sigma"),std::max(.5f,sigma*h));
        gs_effect_set_texture(shader_parameter(f,"shadow_blur_input"),input);
        if (!render_pass(f,f->shadow_field[1-axis],input,w,h,"SpotlightShadowBlur")) return false;
    }
    f->shadow_field_ready=true;
    return true;
}

bool render_pass(Compositor *f, gs_texrender_t *dest, gs_texture_t *input, uint32_t w, uint32_t h,
                 const char *technique) {
    gs_texrender_reset(dest);
    if (!gs_texrender_begin(dest, w, h))
        return false;
    gs_ortho(0, (float)w, 0, (float)h, -100, 100);
    gs_effect_set_texture(shader_parameter(f, "image"), input);
    vec2 texel = {1.f / gs_texture_get_width(input), 1.f / gs_texture_get_height(input)};
    // Only bind optional blur controls when the effect declares them.
    if (auto *parameter = shader_parameter(f, "texel")) gs_effect_set_vec2(parameter, &texel);
    if (auto *parameter = shader_parameter(f, "radius")) gs_effect_set_float(parameter, f->radius);
    if (auto *parameter = shader_parameter(f, "threshold")) gs_effect_set_float(parameter, f->threshold);
    while (gs_effect_loop(f->effect, technique))
        gs_draw_sprite(input, 0, w, h);
    gs_texrender_end(dest);
    return true;
}
// Copy only the 1x1 smoothed colour, not scene images or settings. Called at
// transition boundaries so the destination continues from the visible history.
bool copy_color_match_history(Compositor *from, Compositor *to) {
    auto *sample=from->color_sample?gs_texrender_get_texture(from->color_sample):nullptr;
    if(!sample || !to->effect || !to->color_sample)return true;
    if(!render_pass(to,to->color_sample,sample,1,1,"Copy"))return false;
    to->color_sample_elapsed=from->color_sample_elapsed;
    return true;
}
bool sample_color_reference(Compositor *f, gs_texture_t *fallback) {
    gs_texture_t *input = fallback;
    auto *reference = f->reference ? obs_weak_source_get_source(f->reference) : nullptr;
    if (reference && source_protection(reference) == SourceProtection::None &&
        reference != obs_filter_get_parent(f->source) &&
        reference != obs_filter_get_target(f->source)) {
        const gs_color_space preferred[] = {GS_CS_SRGB};
        uint32_t w = obs_source_get_width(reference), h = obs_source_get_height(reference);
        gs_texrender_reset(f->reference_render);
        if (w && h && obs_source_get_color_space(reference, 1, preferred) == GS_CS_SRGB &&
            gs_texrender_begin(f->reference_render, 112, 60)) {
            vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
            gs_ortho(0, (float)w, 0, (float)h, -100, 100);
            f->sampling_reference = true;
            obs_source_video_render(reference);
            f->sampling_reference = false;
            gs_texrender_end(f->reference_render);
            input = gs_texrender_get_texture(f->reference_render);
        }
    }
    if (reference)
        obs_source_release(reference);
    bool history = gs_texrender_get_texture(f->color_sample) != nullptr;
    if (history)
        history =
            render_pass(f, f->color_sample_history, gs_texrender_get_texture(f->color_sample), 1, 1, "Copy");
    gs_effect_set_texture(shader_parameter(f, "average_previous"),
                          history ? gs_texrender_get_texture(f->color_sample_history) : input);
    gs_effect_set_float(shader_parameter(f, "average_response"),
                        history ? 1.f - std::exp(-10.f * f->color_sample_elapsed) : 1.f);
    bool ok = render_pass(f, f->color_sample, input, 1, 1, "Average");
    f->color_sample_elapsed = ok ? 0.f : .1f;
    return ok;
}
bool compositor_neutral(const Compositor *f) {
    bool neutral_color = f->exposure_gain == 1 && f->contrast == 1 && f->saturation == 1 && f->brightness == 0 &&
                         f->color_match_strength == 0 && f->color_filter.x == 1 && f->color_filter.y == 1 &&
                         f->color_filter.z == 1;
    bool neutral_screen = f->screen_mode == 0 || f->screen_opacity == 0;
    bool neutral_lightwash = f->lighting.lightwash_size == 0 || f->lighting.lightwash_intensity == 0 || f->lighting.lightwash_opacity == 0;
    return !scene_view_active(f->view) && ((!f->wants_layers || (f->subject_exposure_gain == 1 && f->background_exposure_gain == 1)) &&
         neutral_screen && neutral_color && neutral_lightwash && f->lighting.spotlight_amount == 0 && f->shadow_amount == 0 && !reactive_requested(f) &&
         (!f->rim_enabled || f->rim_opacity == 0 || !f->wants_layers)) &&
        !scene_has_source_treatment(obs_scene_from_source(obs_filter_get_parent(f->source)));
}

// At most one scratch target. The last stage draws directly into OBS, earlier
// stages ping-pong with final_capture. Disabled effects allocate/run no passes.
static bool draw_final_stages(Compositor *f,gs_texture_t *original) {
    const char *stages[3];int count=0;
    if(f->has_exclusions)stages[count++]="ExclusionComposite";
    if(f->bounded_reactive && f->reactive_ready)stages[count++]="FinalReactive";
    if(f->rays_ready && f->lighting.rays_amount>0)stages[count++]="FinalRays";
    auto *input=gs_texrender_get_texture(f->final_capture);
    for(int i=0;i<count;++i) {
        const bool intermediate=i+1<count;
        auto *&target=(i%2==0)?f->final_scratch:f->final_capture;
        if(intermediate) {
            if(!target)target=gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
            if(!target)return false;
            gs_texrender_reset(target);
            if(!gs_texrender_begin(target,f->width,f->height))return false;
            gs_ortho(0,(float)f->width,0,(float)f->height,-100,100);
            gs_blend_state_push();gs_blend_function(GS_BLEND_ONE,GS_BLEND_ZERO);
        }
        if(i==0 && f->has_exclusions)draw_exclusion_output(f,original,input);
        else {
            bind_follow_brightness(f,f->effect,original);
            bind_spotlight(f,f->effect,original);
            gs_effect_set_bool(shader_parameter(f,"bounded_enabled"),f->bounded_reactive && f->reactive_ready);
            gs_effect_set_bool(shader_parameter(f,"final_has_exclusions"),f->has_exclusions);
            // Protection coverage also supplies participation when no overrides exist.
            if(f->has_exclusions)gs_effect_set_texture(shader_parameter(f,"bounded_overrides"),
                gs_texrender_get_texture(f->protection_mask));
            gs_effect_set_texture(shader_parameter(f,"image"),input);
            while(gs_effect_loop(f->effect,stages[i]))gs_draw_sprite(input,0,f->width,f->height);
        }
        if(intermediate) {
            gs_blend_state_pop();gs_texrender_end(target);
            input=gs_texrender_get_texture(target);
        }
    }
    if(count<=1){gs_texrender_destroy(f->final_scratch);f->final_scratch=nullptr;}
    return true;
}

void render_filter(void *data, gs_effect_t *) {
    auto *f = (Compositor *)data;
    if (!reactive_requested(f) && f->reactive_capture)
        release_reactive_buffers(f);
    auto *target = obs_filter_get_target(f->source);
    auto *parent = obs_filter_get_parent(f->source);
    if(target && parent && !f->effect && (f->support_pending || f->support_failed) && !f->sampling_reference && !f->rendering_layers) {
        if(!ensure_support_effect(f)) {
            obs_source_skip_video_filter(f->source);
            if(!f->scene_capture_callback)draw_shader_preparation(obs_source_get_base_width(target),obs_source_get_base_height(target),f->support_failed);
            return;
        }
    }
    if (!target || !parent || source_protection(parent) != SourceProtection::None || !f->effect || f->sampling_reference || f->rendering_layers) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    SceneLightScope scene_light(f,obs_scene_from_source(parent));
    // A scene transform or Light property can change without updating this filter.
    if (!f->scene_capture_callback && (f->lighting.spotlight_amount>0 || scene_view_active(f->view))) f->processed=false;
    if (!f->scene_capture_callback && compositor_neutral(f)) {
        // Only a top-level neutral bypass may retire the private layers. Nested
        // sampling/render callbacks above must keep the active mirror intact.
        clear_mirror(f);
        release_native_targets(f);
        gs_texrender_destroy(f->final_capture);f->final_capture=nullptr;
        gs_texrender_destroy(f->final_scratch);f->final_scratch=nullptr;
        release_exclusion_buffers(f);
        gs_texrender_destroy(f->source_emission);f->source_emission=nullptr;
        gs_texrender_destroy(f->rim_boundary_mask);f->rim_boundary_mask=nullptr;f->rim_boundary_ready=false;
        for(auto *&target:f->shadow_field){gs_texrender_destroy(target);target=nullptr;}
        f->shadow_field_ready=false;
        gs_texrender_destroy(f->rays_field);f->rays_field=nullptr;f->rays_ready=false;
        f->layered = false;
        f->processed = false;
        obs_source_skip_video_filter(f->source);
        return;
    }
    const gs_color_space preferred[] = {GS_CS_SRGB};
    if (obs_source_get_color_space(target, 1, preferred) != GS_CS_SRGB) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    uint32_t w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
    if (!w || !h) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    if (w != f->width || h != f->height)
        f->processed = false;
    f->width = w;
    f->height = h;
    f->native_active = prepare_native_renderer(f);
    if (!f->native_active) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    gs_blend_state_push();
    bool previous_srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    bool success = true;
    if (!f->processed) {
        if(++f->composition_epoch==0)++f->composition_epoch;
        auto *scene = obs_scene_from_source(parent);
        const auto features=scene_features(scene);
        const bool exclusions=features.exclusions;
        f->has_screen_overrides=features.screen;
        f->has_reactive_overrides=features.reactive_overrides;
        f->has_spotlight_overrides=(f->lighting.spotlight_amount>0 && (features.light || (f->lighting.rays_amount>0 && features.rays))) || (f->lighting.rays_amount>0 && exclusions);
        if (!exclusions && f->has_exclusions)
            release_exclusion_buffers(f);
        f->has_exclusions = exclusions;
        f->has_spill = false;
        f->layered = (scene_view_active(f->view) || f->scene_capture_callback || f->wants_layers || f->has_exclusions || f->has_screen_overrides || f->has_spotlight_overrides || features.treatment || reactive_requested(f)) && target == parent && sync_mirror(f, scene);
        if ((scene_view_active(f->view) || f->has_exclusions || f->has_screen_overrides || f->has_spotlight_overrides || features.treatment) && !f->layered) {
            if (!f->exclusion_failure_logged)
                blog(LOG_WARNING, "[Glowbox] scene composition unavailable; showing the original scene");
            f->exclusion_failure_logged = true;
            success = false;
        } else
            f->exclusion_failure_logged = false;
        if (!f->layered && f->mirror)
            clear_mirror(f);
        if(!f->scene_capture_callback)update_rim_direction(f,f->layered?scene:nullptr);
        const bool needs_match = f->color_match_strength > 0 ||
            std::any_of(f->items.begin(), f->items.end(), [](const MirrorItem &item) {
                return item.view->treatment.color_match > 0 || item.view->treatment.match_override > 0;
            });
        if (f->layered && needs_match &&
            (f->color_sample_elapsed >= .1f || !gs_texrender_get_texture(f->color_sample)))
            success = sample_background_color(f);
        if (!success && !f->sample_failure_logged)
            blog(LOG_WARNING,
                 "[Glowbox%s] background color sampling failed; showing source and retrying next frame",
                 f->native_active ? " native" : "");
        f->sample_failure_logged = !success;
        const bool source_reactive = f->layered && std::any_of(f->items.begin(), f->items.end(),
            [](const MirrorItem &item) { return item.view->treatment.reactive > 0; });
        if (success && source_reactive) {
            configure_layers(f, false);
            success = sample_reactive_light(f, gs_texrender_get_texture(f->base));
        }
        gs_texrender_reset(f->base);
        success = success && gs_texrender_begin(f->base, w, h);
        if (success) {
            vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
            gs_ortho(0, (float)w, 0, (float)h, -100, 100);
            uint32_t flags = obs_source_get_output_flags(target);
            if (f->layered) {
                configure_layers(f, false);
                f->rendering_layers = true;
                draw_compositor_scene(f, f->mirror);
                f->rendering_layers = false;
            } else if (target == parent && !(flags & OBS_SOURCE_CUSTOM_DRAW) && !(flags & OBS_SOURCE_ASYNC))
                obs_source_default_render(target);
            else
                obs_source_video_render(target);
            gs_texrender_end(f->base);
            auto *original = gs_texrender_get_texture(f->base);
            f->soft = original;
            f->light = original;
            if (success && !f->layered && f->color_match_strength > 0 &&
                (f->color_sample_elapsed >= .1f || !gs_texrender_get_texture(f->color_sample)))
                success = sample_color_reference(f, original);
            if (success && f->has_exclusions)
                success = prepare_exclusions(f);
            if (success) success = prepare_source_reactive_mask(f);
            if (success && !source_reactive)
                success = sample_reactive_light(f, original);
            if (success)
                success = render_effect_pipeline(f, f->has_exclusions
                                                  ? gs_texrender_get_texture(f->exclusion_input) : original);
            f->processed = success;
        }
    }
    gs_blend_state_pop();
    if (success && f->processed) {
        auto *original = gs_texrender_get_texture(f->base);
        auto *processed_input = f->has_exclusions ? gs_texrender_get_texture(f->exclusion_input) : original;
        const bool staged=f->has_exclusions || (f->bounded_reactive && f->reactive_ready) ||
                          (f->rays_ready && f->lighting.rays_amount>0);
        if(!staged) {
            gs_texrender_destroy(f->final_capture);f->final_capture=nullptr;
            gs_texrender_destroy(f->final_scratch);f->final_scratch=nullptr;
        }
        if (staged) {
            if (!f->final_capture)
                f->final_capture = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
            success = f->final_capture != nullptr;
            if (success) {
                gs_texrender_reset(f->final_capture);
                success = gs_texrender_begin(f->final_capture, w, h);
            }
            if (success) {
                gs_ortho(0, (float)w, 0, (float)h, -100, 100);
                gs_blend_state_push();
                gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
            }
        }
        if (success && f->native_active) {
            draw_native_output(f, processed_input);
        }
        if (staged && success) {
            gs_blend_state_pop();
            gs_texrender_end(f->final_capture);
            success = draw_final_stages(f, original);
        }
    }
    gs_enable_framebuffer_srgb(previous_srgb);
    if (!success) {
        f->processed = false;
        obs_source_skip_video_filter(f->source);
    }
}

void tick_filter(void *data, float seconds) {
    auto *f = (Compositor *)data;
    f->processed = false;
    // empty or hidden scenes may never render, so release neutral mirrors on tick
    // to stop their references from keeping removed sources alive
    if (f->mirror && !f->scene_capture_callback && !f->sampling_reference && !f->rendering_layers &&
        compositor_neutral(f) && !scene_light_present(obs_scene_from_source(obs_filter_get_parent(f->source)))) {
        // A Light source borrows its values only during rendering. Saved neutral
        // controls must not retire and rebuild the active light's mirrors every tick.
        obs_enter_graphics();
        clear_mirror(f);
        gs_texrender_destroy(f->rays_field);f->rays_field=nullptr;f->rays_ready=false;
        f->layered = false;
        obs_leave_graphics();
    }
    f->reactive_elapsed = std::min(.1f, f->reactive_elapsed + seconds);
    f->color_sample_elapsed = std::min(.1f, f->color_sample_elapsed + seconds);
}

}

namespace glowbox {

bool prepare_light_rays(Compositor *f,gs_texture_t *original) {
    f->rays_ready=false;
    if (f->lighting.rays_amount<=0 || !f->layered) {
        gs_texrender_destroy(f->rays_field);f->rays_field=nullptr;return true;
    }
    if (!f->rays_field) f->rays_field=gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
    if (!f->rays_field) return false;
    const uint32_t h=std::min(f->height,270u);
    const uint32_t w=std::max(1u,(uint32_t)((uint64_t)f->width*h/f->height));
    bind_spotlight(f,f->effect,original);
    if (!render_pass(f,f->rays_field,original,w,h,"LightRaysCapture")) return false;
    f->rays_ready=true;return true;
}
bool render_with_scene_capture(Compositor *compositor, SceneCaptureCallback capture, void *context,bool project_view) {
    if (!capture || compositor->scene_capture_callback) return false;
    compositor->scene_capture_callback = capture;
    compositor->scene_capture_context = context;
    compositor->scene_capture_project_view = project_view;
    compositor->scene_capture_failed = false;
    compositor->processed = false;
    render_filter(compositor, nullptr);
    const bool render_succeeded = compositor->processed && !compositor->scene_capture_failed;
    compositor->scene_capture_callback = nullptr;
    compositor->scene_capture_context = nullptr;
    compositor->scene_capture_project_view = false;
    compositor->processed = false;
    return render_succeeded;
}

}
