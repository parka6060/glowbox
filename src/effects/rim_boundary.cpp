#include "compositor/compositor.hpp"

namespace glowbox {
static bool subject_near_edge(const Compositor *f, uint32_t pad_x, uint32_t pad_y) {
    for (const auto &item : f->items) {
        if (!item.view->is_treated_subject || !obs_sceneitem_visible(item.mask_item)) continue;
        matrix4 box;
        obs_sceneitem_get_box_transform(item.mask_item, &box);
        float left = (float)f->width, right = 0, top = (float)f->height, bottom = 0;
        for (float x : {0.f, 1.f}) for (float y : {0.f, 1.f}) {
            vec3 point;
            vec3_set(&point, x, y, 0);
            vec3_transform(&point, &point, &box);
            if (scene_view_active(f->view)) {
                const auto projected=scene_project_point(f->view,subject_plane_distance(f->subject_depth),float(f->height),{point.x,point.y});
                point.x=projected.x;point.y=projected.y;
            }
            left = std::min(left, point.x); right = std::max(right, point.x);
            top = std::min(top, point.y); bottom = std::max(bottom, point.y);
        }
        if (right >= -(float)pad_x && left <= f->width + (float)pad_x &&
            bottom >= -(float)pad_y && top <= f->height + (float)pad_y &&
            (left < pad_x || right > f->width - pad_x ||
             top < pad_y || bottom > f->height - pad_y)) return true;
    }
    return false;
}
bool prepare_rim_boundary(Compositor *f) {
    f->rim_boundary_ready = false;
    // The longest current rim ray is .06 canvas units. Include a bilinear guard.
    const uint32_t pad_x = (uint32_t)std::ceil(f->width * .06f * f->rim_size) + 2;
    const uint32_t pad_y = (uint32_t)std::ceil(f->height * .06f * f->rim_size) + 2;
    if (!f->layered || !f->rim_enabled || f->rim_opacity <= 0 || f->rim_size <= 0 ||
        (!f->scene_capture_callback && !subject_near_edge(f, pad_x, pad_y))) {
        gs_texrender_destroy(f->rim_boundary_mask);
        f->rim_boundary_mask = nullptr;
        return true;
    }
    const uint32_t width = f->width + 2 * pad_x, height = f->height + 2 * pad_y;
    if (!f->rim_boundary_mask) f->rim_boundary_mask = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!f->rim_boundary_mask) return false;
    gs_texrender_reset(f->rim_boundary_mask);
    if (!gs_texrender_begin(f->rim_boundary_mask, width, height)) return false;
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
    gs_ortho(-(float)pad_x, (float)(f->width + pad_x),
             -(float)pad_y, (float)(f->height + pad_y), -100, 100);
    draw_mask_scene(f, f->mask_mirror, LayerCaptureMode::SubjectCoverage);
    gs_texrender_end(f->rim_boundary_mask);
    if (std::any_of(f->items.begin(), f->items.end(),
                   [](const MirrorItem &item) { return item.view->mask_capture_failed; })) return false;
    vec4_set(&f->rim_boundary_uv, (float)f->width / width, (float)f->height / height,
             (float)pad_x / width, (float)pad_y / height);
    f->rim_boundary_ready = true;
    return true;
}
}
