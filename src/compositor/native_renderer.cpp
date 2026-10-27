#include "compositor/source_participation.hpp"
#include "compositor/compositor.hpp"
#include "compositor/native_renderer.hpp"
#include "effects/tone.hpp"
#include "effects/rim_model.hpp"
#include <array>
#include "compositor/shader_preparation.hpp"
#include <cstring>
#include <cstdio>
#include <memory>
#include <util/platform.h>
#include <string>
#include "renderer_shader.hpp"

namespace glowbox {
struct NativeParameters {
    gs_eparam_t *field_textures[6] = {}, *field_weights[6] = {};
    gs_eparam_t *scene_texture = nullptr;
    gs_eparam_t *scene_view_offset = nullptr;
    gs_eparam_t *input_texture = nullptr;
    gs_eparam_t *coverage_texture = nullptr;
    gs_eparam_t *source_screen_mask = nullptr, *source_screen_enabled = nullptr;
    gs_eparam_t *rim_boundary_mask = nullptr, *rim_boundary_ready = nullptr, *rim_boundary_uv = nullptr;
    gs_eparam_t *reference_texture = nullptr;
    gs_eparam_t *light_texture = nullptr;
    gs_eparam_t *highlight_texture = nullptr;
    gs_eparam_t *wash_texture = nullptr;
    gs_eparam_t *canvas_size = nullptr;
    gs_eparam_t *input_texel_size = nullptr;
    gs_eparam_t *blur_step = nullptr;
    gs_eparam_t *has_subject = nullptr;
    gs_eparam_t *use_reference_texture = nullptr;
    gs_eparam_t *reference_color = nullptr;
    gs_eparam_t *subject_exposure = nullptr;
    gs_eparam_t *background_exposure = nullptr;
    gs_eparam_t *global_exposure = nullptr;
    gs_eparam_t *color_match = nullptr;
    gs_eparam_t *tint = nullptr;
    gs_eparam_t *brightness = nullptr;
    gs_eparam_t *contrast = nullptr;
    gs_eparam_t *saturation = nullptr;
    gs_eparam_t *tone_brightness = nullptr, *tone_contrast = nullptr, *tone_darken = nullptr;
    gs_eparam_t *tone_tint_low = nullptr, *tone_tint_rise = nullptr;
    gs_eparam_t *screen_mode = nullptr;
    gs_eparam_t *effect_intensity = nullptr;
    gs_eparam_t *effect_opacity = nullptr;
    gs_eparam_t *glow_threshold = nullptr;
    gs_eparam_t *diffusion_contrast = nullptr;
    gs_eparam_t *rim_enabled = nullptr;
    gs_eparam_t *rim_style = nullptr;
    gs_eparam_t *rim_axis_single=nullptr,*rim_axis_left=nullptr,*rim_axis_right=nullptr;
    gs_eparam_t *rim_color = nullptr;
    gs_eparam_t *rim_size = nullptr;
    gs_eparam_t *rim_brightness = nullptr;
    gs_eparam_t *rim_contrast = nullptr;
    gs_eparam_t *rim_opacity = nullptr;
    gs_eparam_t *rim_blend_mode = nullptr;
    gs_eparam_t *wash_color = nullptr;
    gs_eparam_t *wash_use_reference = nullptr;
    gs_eparam_t *wash_size = nullptr;
    gs_eparam_t *wash_intensity = nullptr;
    gs_eparam_t *wash_opacity = nullptr;
    gs_eparam_t *wash_blend_mode = nullptr;
};

struct NativeFieldTargets {
    gs_texrender_t *downsampled[7] = {}, *combined[1] = {}, *reconstructed[6] = {};
};

struct NativeRenderer {
    bool ready = false;
    bool runtime_failure_logged = false;
    gs_effect_t *effect = nullptr;
    gs_texrender_t *prepared = nullptr, *authored = nullptr;
    NativeFieldTargets monitor, highlight, wash;
    gs_texture_t *scene_texture = nullptr, *light_texture = nullptr;
    gs_texture_t *highlight_texture = nullptr;
    gs_texture_t *wash_texture = nullptr;
    NativeParameters parameters;
    ToneModel tone;
    std::array<float, 5> tone_inputs = {};
    bool tone_cached = false;
    RimAxes rim_axes;
    std::array<float,2> rim_axis_inputs={};
    bool rim_axes_cached=false;
    gs_texture_t *field_textures[6] = {};
    float field_weights[6] = {};
};


static void release_field(NativeFieldTargets &field) {
    const auto release = [](auto &targets) {
        for (auto *&target : targets) {
            gs_texrender_destroy(target);
            target = nullptr;
        }
    };
    release(field.downsampled);
    release(field.combined);
    release(field.reconstructed);
}

static void release_native_targets(NativeRenderer *renderer) {
    gs_texrender_destroy(renderer->authored);
    renderer->authored = nullptr;
    gs_texrender_destroy(renderer->prepared);
    renderer->prepared = nullptr;
    release_field(renderer->monitor);
    release_field(renderer->highlight);
    release_field(renderer->wash);
    renderer->scene_texture = nullptr;
    renderer->light_texture = nullptr;
    renderer->highlight_texture = nullptr;
    renderer->wash_texture = nullptr;
    for (auto *&texture : renderer->field_textures)
        texture = nullptr;
}

void release_native_targets(Compositor *filter) {
    if (filter->native)
        release_native_targets(filter->native);
}

static void destroy_native_resources(NativeRenderer *renderer) {
    gs_effect_destroy(renderer->effect);
    release_native_targets(renderer);
    delete renderer;
}

bool prepare_native_renderer(Compositor *filter) {
    if (!filter->native_requested)
        return false;
    if (filter->native)
        return filter->native->ready;
    const uint64_t now = os_gettime_ns();
    if (now < filter->renderer_retry_after_ns)
        return false;
    // Failed attempts own no persistent GPU resources. Back off so an invalid
    // shader cannot trigger compilation and logging on every video frame.
    filter->renderer_retry_after_ns = now + 5000000000ULL;
    std::unique_ptr<NativeRenderer, decltype(&destroy_native_resources)> candidate(
        new NativeRenderer, destroy_native_resources);
    auto *renderer = candidate.get();
    char *errors = nullptr;
    const uint64_t shader_started=os_gettime_ns();
    blog(LOG_INFO,"[Glowbox] preparing native renderer shader");
    std::string effect_source=native_effect_source();
    // Embedded effects belong to this renderer. A named path would retain
    // each instance in OBS's session cache, even after the filter is removed.
    renderer->effect = gs_effect_create(effect_source.c_str(), nullptr, &errors);
    blog(LOG_INFO,"[Glowbox] native renderer shader prepared in %.1f ms",(os_gettime_ns()-shader_started)/1000000.0);
    if (errors) {
        blog(LOG_WARNING, "[Glowbox native] %s", errors);
        bfree(errors);
    }
    if (!renderer->effect) {
        blog(LOG_WARNING, "[Glowbox native] renderer shader initialization failed");
        return false;
    }
    int revision = 0;
    auto *revision_parameter = gs_effect_get_param_by_name(renderer->effect, "gb_renderer_revision");
    if (revision_parameter && gs_effect_get_default_val_size(revision_parameter) == sizeof(revision)) {
        gs_effect_param_info info = {};
        gs_effect_get_param_info(revision_parameter, &info);
        if (info.type == GS_SHADER_PARAM_INT) {
            void *value = gs_effect_get_default_val(revision_parameter);
            if (value) {
                std::memcpy(&revision, value, sizeof(revision));
                bfree(value);
            }
        }
    }
    // Some OpenGL builds omit the default for an unused interface marker.
    // The embedded source remains available for the same version check.
    if (revision == 0 && effect_source.find("uniform int gb_renderer_revision = 18;") != std::string::npos)
        revision = 18;
    if (revision != 18) {
        blog(LOG_WARNING, "[Glowbox native] unsupported shader revision %d",
             revision);
        return false;
    }
    for (const char *technique : {"SceneLight", "LightExtract", "MonitorExtract", "HighlightExtract", "Downsample", "FieldFirstDown", "FieldReconstruct", "FieldMix", "WashSpread", "Finish"}) {
        if (!gs_effect_get_technique(renderer->effect, technique)) {
            blog(LOG_WARNING, "[Glowbox native] missing technique: %s", technique);
            return false;
        }
    }
    bool valid_parameters = true;
    const auto require = [&](gs_eparam_t *&handle, const char *name, gs_shader_param_type expected) {
        handle = gs_effect_get_param_by_name(renderer->effect, name);
        gs_effect_param_info info = {};
        if (handle)
            gs_effect_get_param_info(handle, &info);
        if (!handle || info.type != expected) {
            blog(LOG_WARNING, "[Glowbox native] missing or incompatible parameter: %s", name);
            valid_parameters = false;
        }
    };
    auto &parameters = renderer->parameters;
    for (int i = 0; i < 6; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "gb_field_texture%d", i);
        require(parameters.field_textures[i], name, GS_SHADER_PARAM_TEXTURE);
        snprintf(name, sizeof(name), "gb_field_weight%d", i);
        require(parameters.field_weights[i], name, GS_SHADER_PARAM_FLOAT);
    }
    require(parameters.scene_texture, "gb_scene_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.scene_view_offset, "scene_view_offset", GS_SHADER_PARAM_VEC2);
    require(parameters.input_texture, "gb_input_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.coverage_texture, "gb_coverage_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.source_screen_mask, "source_screen_mask", GS_SHADER_PARAM_TEXTURE);
    require(parameters.source_screen_enabled, "source_screen_enabled", GS_SHADER_PARAM_BOOL);
    require(parameters.rim_boundary_mask, "gb_rim_boundary_mask", GS_SHADER_PARAM_TEXTURE);
    require(parameters.rim_boundary_ready, "gb_rim_boundary_ready", GS_SHADER_PARAM_BOOL);
    require(parameters.rim_boundary_uv, "gb_rim_boundary_uv", GS_SHADER_PARAM_VEC4);
    require(parameters.reference_texture, "gb_reference_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.light_texture, "gb_light_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.highlight_texture, "gb_highlight_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.wash_texture, "gb_wash_texture", GS_SHADER_PARAM_TEXTURE);
    require(parameters.canvas_size, "gb_canvas_size", GS_SHADER_PARAM_VEC2);
    require(parameters.input_texel_size, "gb_input_texel_size", GS_SHADER_PARAM_VEC2);
    require(parameters.blur_step, "gb_blur_step", GS_SHADER_PARAM_VEC2);
    require(parameters.has_subject, "gb_has_subject", GS_SHADER_PARAM_BOOL);
    require(parameters.use_reference_texture, "gb_use_reference_texture", GS_SHADER_PARAM_BOOL);
    require(parameters.reference_color, "gb_reference_color", GS_SHADER_PARAM_VEC4);
    require(parameters.subject_exposure, "gb_subject_exposure", GS_SHADER_PARAM_FLOAT);
    require(parameters.background_exposure, "gb_background_exposure", GS_SHADER_PARAM_FLOAT);
    require(parameters.global_exposure, "gb_global_exposure", GS_SHADER_PARAM_FLOAT);
    require(parameters.color_match, "gb_color_match", GS_SHADER_PARAM_FLOAT);
    require(parameters.tint, "gb_tint", GS_SHADER_PARAM_VEC3);
    require(parameters.brightness, "gb_brightness", GS_SHADER_PARAM_FLOAT);
    require(parameters.contrast, "gb_contrast", GS_SHADER_PARAM_FLOAT);
    require(parameters.saturation, "gb_saturation", GS_SHADER_PARAM_FLOAT);
    require(parameters.tone_brightness, "gb_tone_brightness_curve", GS_SHADER_PARAM_VEC4);
    require(parameters.tone_contrast, "gb_tone_contrast_curve", GS_SHADER_PARAM_VEC4);
    require(parameters.tone_darken, "gb_tone_darken", GS_SHADER_PARAM_VEC2);
    require(parameters.tone_tint_low, "gb_tone_tint_low", GS_SHADER_PARAM_VEC3);
    require(parameters.tone_tint_rise, "gb_tone_tint_rise", GS_SHADER_PARAM_VEC3);
    require(parameters.screen_mode, "gb_screen_mode", GS_SHADER_PARAM_INT);
    require(parameters.effect_intensity, "gb_effect_intensity", GS_SHADER_PARAM_FLOAT);
    require(parameters.effect_opacity, "gb_effect_opacity", GS_SHADER_PARAM_FLOAT);
    require(parameters.glow_threshold, "gb_glow_threshold", GS_SHADER_PARAM_FLOAT);
    require(parameters.diffusion_contrast, "gb_diffusion_contrast", GS_SHADER_PARAM_FLOAT);
    require(parameters.rim_enabled, "gb_rim_enabled", GS_SHADER_PARAM_BOOL);
    require(parameters.rim_style, "gb_rim_style", GS_SHADER_PARAM_INT);
    require(parameters.rim_axis_single,"gb_rim_axis_single",GS_SHADER_PARAM_VEC2);
    require(parameters.rim_axis_left,"gb_rim_axis_left",GS_SHADER_PARAM_VEC2);
    require(parameters.rim_axis_right,"gb_rim_axis_right",GS_SHADER_PARAM_VEC2);
    require(parameters.rim_color, "gb_rim_color", GS_SHADER_PARAM_VEC3);
    require(parameters.rim_size, "gb_rim_size", GS_SHADER_PARAM_FLOAT);
    require(parameters.rim_brightness, "gb_rim_brightness", GS_SHADER_PARAM_FLOAT);
    require(parameters.rim_contrast, "gb_rim_contrast", GS_SHADER_PARAM_FLOAT);
    require(parameters.rim_opacity, "gb_rim_opacity", GS_SHADER_PARAM_FLOAT);
    require(parameters.rim_blend_mode, "gb_rim_blend_mode", GS_SHADER_PARAM_INT);
    require(parameters.wash_color, "gb_wash_color", GS_SHADER_PARAM_VEC3);
    require(parameters.wash_use_reference, "gb_wash_use_reference", GS_SHADER_PARAM_BOOL);
    require(parameters.wash_size, "gb_wash_size", GS_SHADER_PARAM_FLOAT);
    require(parameters.wash_intensity, "gb_wash_intensity", GS_SHADER_PARAM_FLOAT);
    require(parameters.wash_opacity, "gb_wash_opacity", GS_SHADER_PARAM_FLOAT);
    require(parameters.wash_blend_mode, "gb_wash_blend_mode", GS_SHADER_PARAM_INT);
    if (!valid_parameters)
        return false;
    renderer->ready = true;
    filter->native = candidate.release();
    filter->renderer_retry_after_ns = 0;
    blog(LOG_INFO, "[Glowbox native] renderer initialized");
    return true;
}

void destroy_native_renderer(Compositor *filter) {
    if (filter->native)
        destroy_native_resources(filter->native);
    filter->native = nullptr;
    filter->renderer_retry_after_ns = 0;
}

static void bind_tone(Compositor *filter) {
    auto *renderer = filter->native;
    const auto &parameters = renderer->parameters;
    const std::array<float, 5> tone_inputs = {filter->brightness, filter->contrast - 1.f,
        filter->color_filter.x, filter->color_filter.y, filter->color_filter.z};
    if (!renderer->tone_cached || renderer->tone_inputs != tone_inputs) {
        renderer->tone = prepare_tone_model(tone_inputs[0], tone_inputs[1], filter->color_filter);
        renderer->tone_inputs = tone_inputs;
        renderer->tone_cached = true;
    }

    gs_effect_set_vec4(parameters.tone_brightness, &renderer->tone.brightness_curve);
    gs_effect_set_vec4(parameters.tone_contrast, &renderer->tone.contrast_curve);
    gs_effect_set_vec2(parameters.tone_darken, &renderer->tone.darken);
    vec3 low, rise;
    vec3_set(&low, renderer->tone.tint_low.x, renderer->tone.tint_low.y, renderer->tone.tint_low.z);
    vec3_set(&rise, renderer->tone.tint_rise.x, renderer->tone.tint_rise.y, renderer->tone.tint_rise.z);
    gs_effect_set_vec3(parameters.tone_tint_low, &low);
    gs_effect_set_vec3(parameters.tone_tint_rise, &rise);
}

static void bind_native(Compositor *filter, gs_texture_t *original, gs_texture_t *input, float blur_x = 0,
                        float blur_y = 0) {
    auto *renderer = filter->native;
    const auto &parameters = renderer->parameters;
    bind_spotlight(filter, renderer->effect, original);
    const auto texture = [&](gs_eparam_t *parameter, gs_texture_t *value) {
        gs_effect_set_texture(parameter, value ? value : original);
    };
    const auto vector2 = [](gs_eparam_t *parameter, float x, float y) {
        vec2 value = {x, y};
        gs_effect_set_vec2(parameter, &value);
    };
    const auto color3 = [](gs_eparam_t *parameter, const vec4 &value) {
        vec3 color;
        vec3_set(&color, value.x, value.y, value.z);
        gs_effect_set_vec3(parameter, &color);
    };
    // OBS clears uniforms after each technique, bind inputs on every draw.

    bool subject =
        filter->layered && std::any_of(filter->items.begin(), filter->items.end(),
                                       [](const MirrorItem &item) { return item.view->is_treated_subject; });
    auto *reference = gs_texrender_get_texture(filter->color_sample);
    for (int i = 0; i < 6; ++i) {
        texture(parameters.field_textures[i], renderer->field_textures[i]);
        gs_effect_set_float(parameters.field_weights[i], renderer->field_weights[i]);
    }
    bind_follow_brightness(filter, renderer->effect, original);
    texture(parameters.scene_texture, original);
    texture(parameters.input_texture, input);
    texture(parameters.source_screen_mask, filter->has_screen_overrides ? filter->subject_texture : original);
    gs_effect_set_bool(parameters.source_screen_enabled, filter->has_screen_overrides);
    texture(parameters.coverage_texture,
            filter->layered ? filter->subject_texture : original);
    texture(parameters.rim_boundary_mask, filter->rim_boundary_ready
        ? gs_texrender_get_texture(filter->rim_boundary_mask) : (filter->layered ? gs_texrender_get_texture(filter->subject_mask) : filter->subject_texture));
    gs_effect_set_bool(parameters.rim_boundary_ready, filter->rim_boundary_ready);
    gs_effect_set_vec4(parameters.rim_boundary_uv, &filter->rim_boundary_uv);
    texture(parameters.reference_texture, reference);
    texture(parameters.light_texture, renderer->light_texture);
    texture(parameters.highlight_texture, renderer->highlight_texture);
    texture(parameters.wash_texture, renderer->wash_texture);
    vector2(parameters.canvas_size, (float)filter->width, (float)filter->height);
    vector2(parameters.input_texel_size, 1.f / gs_texture_get_width(input),
            1.f / gs_texture_get_height(input));
    vector2(parameters.blur_step, blur_x, blur_y);
    gs_effect_set_bool(parameters.has_subject, filter->layered);
    gs_effect_set_bool(parameters.use_reference_texture, reference != nullptr);
    vec4 invalid_reference = {};
    gs_effect_set_vec4(parameters.reference_color, &invalid_reference);
    gs_effect_set_float(parameters.subject_exposure,
                        filter->layered ? std::log2(filter->subject_exposure_gain) : 0.f);
    gs_effect_set_float(parameters.background_exposure,
                        filter->layered ? std::log2(filter->background_exposure_gain) : 0.f);
    gs_effect_set_float(parameters.global_exposure, std::log2(filter->exposure_gain));
    gs_effect_set_float(parameters.color_match, filter->color_match_strength);
    color3(parameters.tint, filter->color_filter);
    gs_effect_set_float(parameters.brightness, filter->brightness);
    gs_effect_set_float(parameters.contrast, filter->contrast - 1.f);
    gs_effect_set_float(parameters.saturation, filter->saturation - 1.f);
    gs_effect_set_int(parameters.screen_mode, filter->screen_mode);
    gs_effect_set_float(parameters.effect_intensity, filter->screen_intensity);
    gs_effect_set_float(parameters.effect_opacity, filter->screen_opacity);
    gs_effect_set_float(parameters.glow_threshold, filter->threshold);
    gs_effect_set_float(parameters.diffusion_contrast, filter->screen_contrast);
    gs_effect_set_bool(parameters.rim_enabled, filter->rim_enabled && subject);
    gs_effect_set_int(parameters.rim_style, filter->rim_type);
    vec4 rim_color = filter->rim_color;
    if (filter->rim_follow_light == 2)
        for (int i=0;i<3;++i) rim_color.ptr[i] += ((filter->lighting.light_type>.5f ? filter->lighting.spotlight_light_color.ptr[i] : filter->lighting.spotlight_color.ptr[i])-rim_color.ptr[i])*filter->lighting.spotlight_amount;
    color3(parameters.rim_color, rim_color);
    gs_effect_set_float(gs_effect_get_param_by_name(renderer->effect, "gb_reactive_strength"),
                        filter->reactive_ready ? filter->reactive_strength : 0);
    gs_effect_set_texture(gs_effect_get_param_by_name(renderer->effect, "gb_reactive_signal"),
        filter->reactive_ready ? gs_texrender_get_texture(filter->reactive_history[filter->reactive_index]) : original);

    gs_effect_set_texture(gs_effect_get_param_by_name(renderer->effect,"gb_reactive_spatial"),
        filter->reactive_spatial_ready?gs_texrender_get_texture(filter->reactive_spatial[filter->reactive_index]):original);
    gs_effect_set_float(gs_effect_get_param_by_name(renderer->effect,"gb_reactive_spatial_amount"),
        filter->reactive_spatial_ready?filter->reactive_geometry.amount:0);
    gs_effect_set_float(parameters.rim_size, filter->rim_size);
    gs_effect_set_float(parameters.rim_brightness, filter->rim_brightness);
    gs_effect_set_float(parameters.rim_contrast, filter->rim_contrast);
    const float angle=effective_rim_angle(filter);
    const std::array<float,2> inputs={angle,filter->rim_size};
    if(!renderer->rim_axes_cached || renderer->rim_axis_inputs!=inputs){
        renderer->rim_axes=prepare_rim_axes(angle,filter->rim_size);
        renderer->rim_axis_inputs=inputs;renderer->rim_axes_cached=true;
    }
    gs_effect_set_vec2(parameters.rim_axis_single,&renderer->rim_axes.single);
    gs_effect_set_vec2(parameters.rim_axis_left,&renderer->rim_axes.left);
    gs_effect_set_vec2(parameters.rim_axis_right,&renderer->rim_axes.right);
    gs_effect_set_float(parameters.rim_opacity, filter->rim_opacity);
    gs_effect_set_int(parameters.rim_blend_mode, filter->rim_blend);
    color3(parameters.wash_color, filter->lighting.lightwash_color);
    gs_effect_set_bool(parameters.wash_use_reference, filter->color_match_strength > 0);
    gs_effect_set_float(parameters.wash_size, filter->lighting.lightwash_size);
    gs_effect_set_float(parameters.wash_intensity, filter->lighting.lightwash_intensity);
    gs_effect_set_float(parameters.wash_opacity, filter->lighting.lightwash_opacity);
    gs_effect_set_int(parameters.wash_blend_mode, filter->lighting.lightwash_type);
}

static void bind_native_pass(Compositor *filter, gs_texture_t *original,
                             gs_texture_t *input, const char *technique,
                             float blur_x, float blur_y) {
    auto *renderer = filter->native;
    const auto &parameters = renderer->parameters;
    if (std::strcmp(technique, "FieldMix") == 0) {
        for (int i = 0; i < 6; ++i) {
            auto *texture = renderer->field_textures[i];
            gs_effect_set_texture(parameters.field_textures[i], texture ? texture : original);
            gs_effect_set_float(parameters.field_weights[i], renderer->field_weights[i]);
        }
        return;
    }
    if (std::strcmp(technique, "FieldReconstruct") == 0) {
        gs_effect_set_texture(parameters.input_texture, input);
        const vec2 step = {blur_x, blur_y};
        gs_effect_set_vec2(parameters.blur_step, &step);
        return;
    }
    const bool monitor = std::strcmp(technique, "MonitorExtract") == 0;
    const bool highlight = std::strcmp(technique, "HighlightExtract") == 0;
    if (monitor || highlight || std::strcmp(technique, "Downsample") == 0 ||
        std::strcmp(technique, "FieldFirstDown") == 0 ||
        std::strcmp(technique, "WashSpread") == 0) {
        gs_effect_set_texture(parameters.input_texture, input);
        const vec2 texel = {1.f / gs_texture_get_width(input), 1.f / gs_texture_get_height(input)};
        gs_effect_set_vec2(parameters.input_texel_size, &texel);
        if (monitor) gs_effect_set_int(parameters.screen_mode, filter->screen_mode);
        if (highlight) gs_effect_set_float(parameters.glow_threshold, filter->threshold);
        return;
    }
    bind_native(filter, original, input, blur_x, blur_y);
}

static bool native_pass(Compositor *filter, gs_texrender_t *&target, gs_texture_t *original,
                        gs_texture_t *input, uint32_t width, uint32_t height, const char *technique,
                        float blur_x = 0, float blur_y = 0) {
    if (!target) target=gs_texrender_create(GS_RGBA16F,GS_ZS_NONE);
    if (!target) return false;
    gs_texrender_reset(target);
    if (!gs_texrender_begin(target, width, height)) {
        if (!filter->native->runtime_failure_logged) {
            blog(LOG_WARNING,
                 "[Glowbox native] runtime failure in %s; showing source and retrying next frame", technique);
            filter->native->runtime_failure_logged = true;
        }
        return false;
    }
    gs_ortho(0, (float)width, 0, (float)height, -100, 100);
    bind_native_pass(filter, original, input, technique, blur_x, blur_y);
    while (gs_effect_loop(filter->native->effect, technique))
        gs_draw_sprite(input, 0, width, height);
    gs_texrender_end(target);
    return true;
}


bool render_native_pipeline(Compositor *filter, gs_texture_t *original) {
    auto *renderer = filter->native;
    renderer->scene_texture = original;
    renderer->light_texture = original;
    renderer->highlight_texture = original;
    renderer->wash_texture = original;
    for (int i = 0; i < 6; ++i) {
        renderer->field_textures[i] = original;
        renderer->field_weights[i] = 0;
    }
    if (!native_pass(filter, renderer->prepared, original, original,
                     filter->width, filter->height, "LightExtract"))
        return false;
    auto *surface=gs_texrender_get_texture(renderer->prepared);
    const bool authored=filter->lighting.spotlight_amount>0 || filter->shadow_amount>0 ||
        (filter->lighting.lightwash_size>0 && filter->lighting.lightwash_intensity>0 && filter->lighting.lightwash_opacity>0);
    if(authored) {
        if(!native_pass(filter,renderer->authored,original,surface,
                        filter->width,filter->height,"SceneLight"))return false;
        surface=gs_texrender_get_texture(renderer->authored);
    } else {gs_texrender_destroy(renderer->authored);renderer->authored=nullptr;}
    auto *prepared = apply_reactive_light(filter, surface);
    if (!prepared) return false;
    renderer->scene_texture = prepared;
    auto *emission = exclusion_emission_input(filter, prepared);
    if (!emission)
        return false;
    auto *screen_emission = source_screen_emission(filter, emission);
    if (!screen_emission) return false;

    const auto field = [&](NativeFieldTargets &targets, float descriptor, bool highlight,
                           gs_texture_t *&output) {
        const float radius = std::clamp(descriptor, 1.f, 7.f);
        const uint32_t height = std::max(1u, std::min(filter->height,
                                      (highlight ? 1080u : 540u)));
        const uint32_t width = std::max(1u, (uint32_t)((uint64_t)filter->width * height / filter->height));
        const int levels = std::clamp((int)(std::log2((float)height) + radius - 7.f),
                                     2, std::min(7, (int)std::ceil(radius) + 1));
        if (!native_pass(filter, targets.downsampled[0], original, screen_emission, width, height,
                         highlight ? "HighlightExtract" : "MonitorExtract"))
            return false;
        uint32_t w = width, h = height;
        auto *input = gs_texrender_get_texture(targets.downsampled[0]);
        for (int level = 0; level < levels - 1; ++level) {
            w = std::max(1u, w / 2); h = std::max(1u, h / 2);
            if (!native_pass(filter, targets.downsampled[level + 1], original, input, w, h,
                             level == 0 ? "FieldFirstDown" : "Downsample"))
                return false;
            input = gs_texrender_get_texture(targets.downsampled[level + 1]);
            const float spread = std::clamp((descriptor + 1.f) * .2f, 1.f, 2.5f);
            if (!native_pass(filter, targets.reconstructed[level], original, input, w, h,
                             "FieldReconstruct", spread / w, spread / h))
                return false;
        }
        for (int level = 0; level < 6; ++level) {
            const bool present = level < levels - 1;
            renderer->field_textures[level] = present ? gs_texrender_get_texture(targets.reconstructed[level]) : prepared;
            renderer->field_weights[level] = present ? std::clamp(radius - level, 0.f, 1.f) / radius : 0.f;
        }

        if (!native_pass(filter, targets.combined[0], original, prepared, width, height, "FieldMix"))
            return false;
        output = gs_texrender_get_texture(targets.combined[0]);
        return true;
    };
    const bool screen_active = filter->screen_mode != 0 && filter->screen_opacity > 0;
    if (!screen_active) release_field(renderer->monitor);
    if (!screen_active || filter->screen_mode!=3) release_field(renderer->highlight);
    if (screen_active) {
        if (!field(renderer->monitor, filter->screen_mode == 2 ? filter->radius + 1.f : 3.f,
                   false, renderer->light_texture))
            return false;
        if (filter->screen_mode == 3) {
            if (!field(renderer->highlight, filter->radius + 1.f, true, renderer->highlight_texture))
                return false;
        }
    }
    if (filter->lighting.light_pattern < .5f && filter->lighting.lightwash_size > 0 && filter->lighting.lightwash_intensity > 0 && filter->lighting.lightwash_opacity > 0) {
        auto *input = emission;
        for (int level = 0; level < 3; ++level) {
            const uint32_t width = std::max(1u, filter->width >> (level + 2));
            const uint32_t height = std::max(1u, filter->height >> (level + 2));
            if (!native_pass(filter, renderer->wash.downsampled[level], original, input, width, height, "WashSpread"))
                return false;
            input = gs_texrender_get_texture(renderer->wash.downsampled[level]);
        }
        renderer->wash_texture = input;
    } else release_field(renderer->wash);
    renderer->runtime_failure_logged = false;
    return true;
}

gs_texture_t *native_spill_texture(Compositor *filter) {
    if (!filter->native || filter->screen_mode == 0 || filter->screen_opacity <= 0)
        return nullptr;
    return filter->screen_mode == 3 ? filter->native->highlight_texture : filter->native->light_texture;
}

void draw_native_output(Compositor *filter, gs_texture_t *original_texture) {
    auto *original = original_texture;
    bind_native(filter, original, filter->native->scene_texture);
    bind_tone(filter);
    while (gs_effect_loop(filter->native->effect, "Finish"))
        gs_draw_sprite(original, 0, filter->width, filter->height);
}
}
