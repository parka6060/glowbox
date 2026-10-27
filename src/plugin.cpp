#include <obs-module.h>
#include "compositor/compositor_lifecycle.hpp"
#include "effects/leaf_canopy.hpp"
#include "plugin_registration.hpp"
#include "compositor/shader_preparation.hpp"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void) {
    return "Glowbox";
}

MODULE_EXPORT const char *obs_module_description(void) {
    return "Glowbox compositor preview";
}

MODULE_EXPORT bool obs_module_load(void) {
    glowbox::load_loading_images();
    glowbox::register_layer_source();
    glowbox::register_light_source();
    glowbox::register_source_controls_filter();
    glowbox::register_compositor_filter();
    glowbox::start_move_integration();
    return true;
}

MODULE_EXPORT void obs_module_unload(void) {
    glowbox::stop_move_integration();
    glowbox::stop_shader_preparation();
    glowbox::stop_canopy_worker();
}
