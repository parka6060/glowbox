#pragma once
#include <obs.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

namespace glowbox {
struct Compositor;
// Scene-space footprint of one visible placement of the selected light source.
struct ReactiveGeometry {
    vec2 origin={}, axis_x={}, axis_y={}; // canvas-height units, preserving aspect
    vec4 crop={0,0,1,1}; // raw source UV origin and extent
    float depth=.25f, receiver_extent=1, aspect=1, amount=1;
    float subject_plane_weight = 0;
    bool valid=false;
};
struct RimLightGeometry {
    vec2 light = {}, subject = {};
    float light_subject_weight = 0;
    bool valid = false;
};
RimLightGeometry rim_light_geometry(const Compositor *, obs_scene_t *, obs_data_t *roles = nullptr);
vec2 subject_centre(const Compositor *, obs_scene_t *, obs_data_t *roles = nullptr);
bool rim_angle_between(const vec2 &, const vec2 &, float &);
void update_rim_direction(Compositor *, obs_scene_t *);
ReactiveGeometry reactive_geometry(const Compositor *, obs_scene_t *, obs_data_t *roles=nullptr, bool reference_view=false);
void project_reactive_geometry(const Compositor *, ReactiveGeometry &);
}
