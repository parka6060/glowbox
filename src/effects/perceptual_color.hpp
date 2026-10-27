#pragma once
#include <algorithm>
#include <cmath>
#include <graphics/vec4.h>

namespace glowbox::color {
// Display colours enter/leave as straight sRGB. Light calculations stay linear.
// OKLab matrices: https://bottosson.github.io/posts/oklab/ (public domain).
struct Lab { float l, a, b; };
struct Linear { float r, g, b; };
inline float decode(float v) {
    return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
inline float encode(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
}
inline Lab to_lab(Linear c) {
    const float l = std::cbrt(.4122214708f*c.r + .5363325363f*c.g + .0514459929f*c.b);
    const float m = std::cbrt(.2119034982f*c.r + .6806995451f*c.g + .1073969566f*c.b);
    const float s = std::cbrt(.0883024619f*c.r + .2817188376f*c.g + .6299787005f*c.b);
    return {.2104542553f*l + .7936177850f*m - .0040720468f*s,
            1.9779984951f*l - 2.4285922050f*m + .4505937099f*s,
            .0259040371f*l + .7827717662f*m - .8086757660f*s};
}
inline Linear to_linear(Lab c) {
    float l = c.l + .3963377774f*c.a + .2158037573f*c.b;
    float m = c.l - .1055613458f*c.a - .0638541728f*c.b;
    float s = c.l - .0894841775f*c.a - 1.2914855480f*c.b;
    l *= l*l; m *= m*m; s *= s*s;
    return {4.0767416621f*l - 3.3077115913f*m + .2309699292f*s,
           -1.2684380046f*l + 2.6097574011f*m - .3413193965f*s,
           -.0041960863f*l - .7034186147f*m + 1.7076147010f*s};
}
inline bool in_gamut(Linear c) {
    return std::min({c.r,c.g,c.b}) >= -.000001f &&
           std::max({c.r,c.g,c.b}) <= 1.000001f;
}
inline Linear fit_gamut(Lab c) {
    auto rgb = to_linear(c);
    if (in_gamut(rgb)) return rgb;
    // Keep perceptual lightness and hue, reduce only chroma. CPU-only, invoked
    // for a handful of authored colours, never for each screen pixel.
    float low = 0, high = 1;
    for (int i = 0; i < 12; ++i) {
        const float scale = (low + high) * .5f;
        if (in_gamut(to_linear({c.l, c.a*scale, c.b*scale}))) low = scale;
        else high = scale;
    }
    return to_linear({c.l, c.a*low, c.b*low});
}
struct Transition {
    vec4 from{}, to{};
    Lab start{}, end{};
    void set(vec4 a, vec4 b) {
        from = a; to = b;
        start = to_lab({decode(a.x), decode(a.y), decode(a.z)});
        end = to_lab({decode(b.x), decode(b.y), decode(b.z)});
    }
    vec4 at(float t) const {
        if (t <= 0) return from;
        if (t >= 1) return to;
        if (from.x == to.x && from.y == to.y && from.z == to.z) {
            vec4 result = from; result.w += (to.w-from.w)*t; return result;
        }
        auto rgb = fit_gamut({start.l+(end.l-start.l)*t,
                             start.a+(end.a-start.a)*t,
                             start.b+(end.b-start.b)*t});
        vec4 result;
        vec4_set(&result, encode(rgb.r), encode(rgb.g), encode(rgb.b),
                 from.w+(to.w-from.w)*t);
        return result;
    }
};
}
