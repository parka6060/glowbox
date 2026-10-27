#pragma once
#include <obs.h>

namespace glowbox {
struct Compositor;
void register_compositor_filter();
// The source compositor and its effect outlive the transition treatment.
obs_source_t *create_transition_treatment(obs_data_t *settings, Compositor *source_compositor);
}
