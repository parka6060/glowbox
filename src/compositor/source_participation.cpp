#include "compositor/source_participation.hpp"
#include "compositor/compositor.hpp"
#include "compositor/native_renderer.hpp"

namespace glowbox {
void bind_source_screen(Compositor *f) {
    gs_effect_set_bool(shader_parameter(f, "source_screen_enabled"), f->has_screen_overrides);
    gs_effect_set_texture(shader_parameter(f, "source_screen_mask"),
                          f->has_screen_overrides ? f->subject_texture : gs_texrender_get_texture(f->base));
}
gs_texture_t *source_screen_emission(Compositor *f, gs_texture_t *input) {
    if (!f->has_screen_overrides || f->screen_mode == 0 || f->screen_opacity == 0 ||
        std::none_of(f->items.begin(), f->items.end(), [](const MirrorItem &item) {
            return obs_sceneitem_visible(item.item) && item.view->treatment.produce_effects != 1;
        })) {
        gs_texrender_destroy(f->source_emission);
        f->source_emission = nullptr;
        return input;
    }
    // Reuse the subject capture's spare channels. No additional scene capture.
    if (!f->source_emission)
        f->source_emission = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    bind_source_screen(f);
    return f->source_emission &&
                   render_pass(f, f->source_emission, input, f->width, f->height, "SourceScreenEmission")
               ? gs_texrender_get_texture(f->source_emission)
               : nullptr;
}
void release_exclusion_buffers(Compositor *f) {
    for (auto target : {&f->protected_color, &f->protection_mask, &f->exclusion_input, &f->exclusion_emission,
                        &f->exclusion_subject, &f->source_spotlight_mask}) {
        gs_texrender_destroy(*target);
        *target = nullptr;
    }
}
static void bind_exclusions(Compositor *f) {
    gs_effect_set_texture(shader_parameter(f, "protection_color"),
                          gs_texrender_get_texture(f->protected_color));
    gs_effect_set_texture(shader_parameter(f, "protection_mask"),
                          gs_texrender_get_texture(f->protection_mask));
}
static bool capture_protection(Compositor *f, gs_texrender_t *target, obs_scene_t *scene,
                               LayerCaptureMode mode) {
    gs_texrender_reset(target);
    if (!gs_texrender_begin(target, f->width, f->height))
        return false;
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
    gs_ortho(0, (float)f->width, 0, (float)f->height, -100, 100);
    draw_mask_scene(f, scene, mode);
    gs_texrender_end(target);
    return std::none_of(f->items.begin(), f->items.end(),
                        [](const MirrorItem &item) { return item.view->mask_capture_failed; });
}
bool prepare_source_spotlight_mask(Compositor *f) {
    if (!f->has_spotlight_overrides) {
        gs_texrender_destroy(f->source_spotlight_mask);
        f->source_spotlight_mask = nullptr;
        return true;
    }
    if (!f->source_spotlight_mask)
        f->source_spotlight_mask = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    return f->source_spotlight_mask && f->lighting_mask_mirror &&
           capture_protection(f, f->source_spotlight_mask, f->lighting_mask_mirror,
                              LayerCaptureMode::LightingCoverage);
}
bool prepare_source_reactive_mask(Compositor *f) {
    // Reuse protection coverage's spare blue channel. Exclusions already capture
    // it, otherwise only this mask is needed, not protected RGB or restoration.
    if (!f->has_reactive_overrides || f->has_exclusions)
        return true;
    if (!f->protection_mask)
        f->protection_mask = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    return f->protection_mask && capture_protection(f, f->protection_mask, f->protection_mask_mirror,
                                                    LayerCaptureMode::ProtectionCoverage);
}
bool prepare_exclusions(Compositor *f) {
    if (!f->protected_color)
        f->protected_color = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!f->protection_mask)
        f->protection_mask = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f->exclusion_input)
        f->exclusion_input = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f->protected_color || !f->protection_mask || !f->exclusion_input)
        return false;
    for (auto &item : f->items)
        f->has_spill |= obs_sceneitem_visible(item.item) && item.view->protection == SourceProtection::Spill;
    if (!capture_protection(f, f->protected_color, f->protected_mirror, LayerCaptureMode::ProtectedColor) ||
        !capture_protection(f, f->protection_mask, f->protection_mask_mirror,
                            LayerCaptureMode::ProtectionCoverage))
        return false;
    bind_exclusions(f);
    return render_pass(f, f->exclusion_input, gs_texrender_get_texture(f->base), f->width, f->height,
                       "ExclusionInput");
}
gs_texture_t *exclusion_emission_input(Compositor *f, gs_texture_t *input) {
    if (!f->has_exclusions)
        return input;
    if (!f->exclusion_emission)
        f->exclusion_emission = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f->exclusion_emission)
        return nullptr;
    bind_exclusions(f);
    return render_pass(f, f->exclusion_emission, input, f->width, f->height, "ExclusionEmission")
               ? gs_texrender_get_texture(f->exclusion_emission)
               : nullptr;
}
void draw_exclusion_output(Compositor *f, gs_texture_t *original, gs_texture_t *processed) {
    bind_exclusions(f);
    bind_spotlight(f, f->effect, original);
    bind_follow_brightness(f, f->effect, processed);
    gs_effect_set_bool(shader_parameter(f, "bounded_enabled"), f->bounded_reactive && f->reactive_ready);
    auto *spill = native_spill_texture(f);
    const float intensity = f->screen_intensity, opacity = f->screen_opacity;
    const float gain = f->screen_mode == 3   ? (.1f + .9f * intensity) * opacity * opacity
                       : f->screen_mode == 2 ? intensity * opacity
                                             : (.18f + .02f * intensity) * (.2f + .2f * intensity) * opacity;
    gs_effect_set_texture(shader_parameter(f, "exclusion_original"), original);
    gs_effect_set_texture(shader_parameter(f, "spill_image"), spill ? spill : original);
    gs_effect_set_float(shader_parameter(f, "spill_gain"), f->has_spill && spill ? gain : 0);
    gs_effect_set_texture(shader_parameter(f, "image"), processed);
    while (gs_effect_loop(f->effect, "ExclusionComposite"))
        gs_draw_sprite(original, 0, f->width, f->height);
}
} // namespace glowbox
