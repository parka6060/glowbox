#include "effects/tone.hpp"
#include "effects/perceptual_color.hpp"
#include <algorithm>
#include <array>

namespace glowbox {
namespace {
using Coefficients = std::array<float, 4>;
template<size_t N>
vec4 prepare_curve(float control, const std::array<Coefficients, N> &weights) {
    vec4 result = {};
    for (size_t channel = 0; channel < 4; ++channel) {
        float value = 0;
        for (size_t index = N; index > 0; --index)
            value = weights[index - 1][channel] + control * value;
        result.ptr[channel] = control * value;
    }
    return result;
}
// Calibrated coefficients for the rational tone curve.
constexpr std::array<Coefficients, 2> brightness_weights = {{
    {{0, 2.182591485f, -2.389702635f, 0.207111150f}},
    {{0, 0.862404882f, 0.200833395f, -1.063238277f}}
}};
constexpr std::array<Coefficients, 2> positive_contrast = {{
    {{0, -1.053192633f, 3.171624888f, -2.159265453f}},
    {{0, 0.382111080f, -0.666449898f, 0.787550351f}}
}};
constexpr std::array<Coefficients, 4> negative_contrast = {{
    {{0.001200125f, 0.086931072f, 0.195246099f, 0.260364881f}},
    {{0.263179473f, -0.678713445f, 0.567271170f, 1.219516853f}},
    {{0.270615371f, -0.746233692f, -0.486562737f, 2.561225216f}},
    {{0.259966298f, -0.398183733f, -0.943340196f, 1.766665880f}}
}};
}

ToneModel prepare_tone_model(float brightness, float contrast, const vec4 &display_tint) {
    brightness = std::clamp(brightness, -1.f, 1.f);
    contrast = std::clamp(contrast, -1.f, 1.f);
    ToneModel model;
    if (brightness > 0)
        model.brightness_curve = prepare_curve(brightness, brightness_weights);
    else if (brightness < 0) {
        model.darken.x = brightness - 0.239433722f * brightness * brightness * (1 + brightness);
        model.darken.y = brightness * (-1.011203575f + brightness * (-0.991174800f + brightness * 0.032863851f));
    }
    model.contrast_curve = contrast >= 0 ? prepare_curve(contrast, positive_contrast)
                                         : prepare_curve(contrast, negative_contrast);
    for (int channel = 0; channel < 3; ++channel) {
        const float tint = color::decode(std::clamp(display_tint.ptr[channel], 0.f, 1.f));
        model.tint_low.ptr[channel] = 0.224215332f + tint * (1.117157634f - tint * 0.157015667f);
        model.tint_rise.ptr[channel] = 1.471483688f + tint * (-0.308698175f + tint * 0.753036287f);
    }
    return model;
}
}
