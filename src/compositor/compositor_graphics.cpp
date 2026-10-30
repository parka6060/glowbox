#include "compositor/source_participation.hpp"
#include "compositor/compositor_graphics.hpp"
#include "compositor/compositor.hpp"
#include "effects/leaf_canopy.hpp"
#include "compositor/native_renderer.hpp"
#include "compositor/shader_preparation.hpp"
#include <util/platform.h>

namespace glowbox {

static gs_effect_t *load_compositor_effect(const char *effect_path, char **shader_error) {
    if (!effect_path)
        return nullptr;
    char *effect_source = os_quick_read_utf8_file(effect_path);
    if (!effect_source)
        return nullptr;
    std::string shader_source = "#define GLOWBOX_SUPPORT_ONLY\n";
    shader_source += effect_source;
    bfree(effect_source);
    // A named path would keep this filter-owned effect in OBS's cache.

    return gs_effect_create(shader_source.c_str(), nullptr, shader_error);
}

void prepare_compositor_graphics(Compositor *compositor, gs_effect_t *borrowed_effect) {
    char *effect_path = obs_module_file("compositor.effect");
    obs_enter_graphics();
    char *shader_error = nullptr;
    compositor->owns_effect = borrowed_effect == nullptr;
    const uint64_t preparation_started_ns = os_gettime_ns();
    blog(LOG_INFO, "[Glowbox] preparing compositor shader");
    if (!borrowed_effect) {
        char *support_shader_source = effect_path ? os_quick_read_utf8_file(effect_path) : nullptr;
        const auto preparation_state = request_shader_preparation(
            support_shader_source ? std::string("#define GLOWBOX_SUPPORT_ONLY\n") + support_shader_source
                                  : std::string());
        bfree(support_shader_source);
        compositor->support_pending = preparation_state != ShaderPreparation::Unsupported;
    }
    if (borrowed_effect) {
        compositor->effect = borrowed_effect;
    } else if (compositor->support_pending) {
        compositor->effect = nullptr;
    } else {
        compositor->effect = load_compositor_effect(effect_path, &shader_error);
    }
    if (compositor->support_pending && shader_preparation_state() == ShaderPreparation::Ready)
        ensure_support_effect(compositor);
    blog(LOG_INFO, "[Glowbox] compositor shader preparation attempt took %.1f ms",
         (os_gettime_ns() - preparation_started_ns) / 1000000.0);
    if (shader_error) {
        blog(LOG_ERROR, "[Glowbox] %s", shader_error);
        bfree(shader_error);
    }
    if (compositor->effect || compositor->support_pending) {
        compositor->base = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        compositor->subject_mask = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        compositor->color_sample = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        compositor->color_sample_history = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        compositor->reference_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        compositor->background_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    }
    obs_leave_graphics();
    bfree(effect_path);
}

void release_compositor_graphics(Compositor *compositor) {
    obs_enter_graphics();
    destroy_native_renderer(compositor);
    destroy_leaf_canopy(compositor);
    if (compositor->owns_effect)
        gs_effect_destroy(compositor->effect);
    gs_texrender_destroy(compositor->base);
    release_exclusion_buffers(compositor);
    gs_texrender_destroy(compositor->source_emission);
    release_reactive_buffers(compositor);
    gs_texrender_destroy(compositor->layout_render);
    gs_texrender_destroy(compositor->color_sample_history);
    gs_texrender_destroy(compositor->final_capture);
    gs_texrender_destroy(compositor->final_scratch);
    gs_texrender_destroy(compositor->subject_mask);
    gs_texrender_destroy(compositor->rim_boundary_mask);
    gs_texrender_destroy(compositor->rays_field);
    for (auto *target : compositor->shadow_field)
        gs_texrender_destroy(target);
    gs_texrender_destroy(compositor->color_sample);
    gs_texrender_destroy(compositor->reference_render);
    gs_texrender_destroy(compositor->background_render);
    if (compositor->reference)
        obs_weak_source_release(compositor->reference);
    obs_leave_graphics();
}


bool ensure_support_effect(Compositor *compositor) {
    if (compositor->effect)
        return true;
    if (!compositor->support_pending || shader_preparation_state() != ShaderPreparation::Ready)
        return false;
    release_shader_preparation_badge();
    char *effect_path = obs_module_file("compositor.effect"), *shader_error = nullptr;
    compositor->effect = load_compositor_effect(effect_path, &shader_error);
    bfree(effect_path);
    if (shader_error) {
        blog(LOG_ERROR, "[Glowbox] support shader: %s", shader_error);
        bfree(shader_error);
    }
    compositor->support_pending = false;
    compositor->support_failed = compositor->effect == nullptr;
    return !compositor->support_failed;
}

} // namespace glowbox
