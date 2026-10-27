#pragma once
#include <obs-module.h>
#include <cstring>
#include "sources/source_reference.hpp"

namespace glowbox {
inline void add_glowbox_footer(obs_properties_t *properties) {
    auto *creator = obs_properties_add_button(properties, "creator", "Created by eninabox", nullptr);
    obs_property_button_set_type(creator, OBS_BUTTON_URL);
    char url[] = "https://eninabox.art";
    obs_property_button_set_url(creator, url);
}
inline bool property_source_available(obs_data_t *settings, const char *key) {
    const char *id = obs_data_get_string(settings, key);
    if (!*id) return false;
    auto *source = acquire_source_reference(id);
    const bool found = source && !obs_source_removed(source);
    obs_source_release(source);
    return found;
}
inline void add_missing_source_choice(obs_property_t *list, obs_data_t *settings, const char *key) {
    const char *id=obs_data_get_string(settings,key);
    if (*id && !property_source_available(settings,key))
        obs_property_list_add_string(list,"Source unavailable (choose another)",id);
}
inline void exposure_units(obs_properties_t *properties, const char *key) {
    auto *p = obs_properties_get(properties, key);
    obs_property_float_set_suffix(p, " stops");
    obs_property_set_long_description(p, "0 keeps the current exposure. +1 doubles the light level; -1 halves it.");
}
}
