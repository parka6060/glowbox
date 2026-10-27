#pragma once
#include <graphics/vec2.h>
#include <graphics/vec4.h>
namespace glowbox {

struct ToneModel {
    vec4 brightness_curve = {}, contrast_curve = {};
    vec2 darken = {};
    vec4 tint_low = {}, tint_rise = {};
};
ToneModel prepare_tone_model(float brightness, float contrast, const vec4 &display_tint);
}
