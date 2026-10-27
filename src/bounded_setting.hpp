#pragma once
#include <obs.h>
#include <algorithm>
#include <cmath>

namespace glowbox {
inline float bounded_setting(obs_data_t *settings, const char *key, float minimum, float maximum) {
    const double value = obs_data_get_double(settings, key);
    return std::isfinite(value) ? std::clamp((float)value, minimum, maximum) : minimum;
}
}
