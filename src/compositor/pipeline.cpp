#include "compositor/source_participation.hpp"
#include "effects/leaf_canopy.hpp"
#include "compositor/compositor.hpp"
#include "compositor/native_renderer.hpp"

namespace glowbox {
static bool render_subject_mask(Compositor *f, gs_texture_t *original) {
    if (f->layered) {
        gs_texrender_reset(f->subject_mask);
        if (!gs_texrender_begin(f->subject_mask, f->width, f->height))
            return false;
        vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, (float)f->width, 0, (float)f->height, -100, 100);
        draw_mask_scene(f, f->mask_mirror, LayerCaptureMode::SubjectCoverage);
        gs_texrender_end(f->subject_mask);
        if (std::any_of(f->items.begin(), f->items.end(),
                        [](const MirrorItem &item) { return item.view->mask_capture_failed; })) {
            if (!f->mask_failure_logged)
                blog(LOG_WARNING,
                     "[Glowbox%s] subject mask capture failed; showing source and retrying next frame",
                     f->native_active ? " native" : "");
            f->mask_failure_logged = true;
            return false;
        }
        f->mask_failure_logged = false;
    }
    f->subject_texture = f->layered ? gs_texrender_get_texture(f->subject_mask) : original;
    if (f->has_exclusions && (f->has_screen_overrides || std::any_of(f->items.begin(), f->items.end(),
                                        [](const MirrorItem &item) { return item.view->is_treated_subject; }))) {
        // Keep subject/background weights correct beneath translucent protected overlays.
        if (!f->exclusion_subject)
            f->exclusion_subject = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        // OBS clears texture inputs at technique end, bind this pass explicitly.
        gs_effect_set_texture(shader_parameter(f, "protection_mask"),
                              gs_texrender_get_texture(f->protection_mask));
        if (!f->exclusion_subject ||
            !render_pass(f, f->exclusion_subject, f->subject_texture,
                         f->width, f->height, "ExclusionSubjectMask"))
            return false;
        f->subject_texture = gs_texrender_get_texture(f->exclusion_subject);
    }
    return true;
}


bool render_effect_pipeline(Compositor *f, gs_texture_t *original) {
    prepare_leaf_canopy(f);
    if (!render_subject_mask(f, original) || !prepare_source_spotlight_mask(f) || !prepare_rim_boundary(f) ||
        !prepare_spotlight_shadow(f,original) || !prepare_light_rays(f,original))
        return false;
    if (f->native_active)
        return render_native_pipeline(f, original);
    // Let the caller show the source when rendering fails.

    return false;
}
}
