#pragma once
#include <obs.h>
#include <cstring>

namespace glowbox {
// The caller releases the acquired source. Unresolved selections remain saved.
inline obs_source_t *acquire_source_reference(const char *id) {
    return std::strncmp(id, "uuid:", 5) == 0 ? obs_get_source_by_uuid(id + 5) : obs_get_source_by_name(id);
}
inline bool source_matches_reference(obs_source_t *source, const char *id) {
    return std::strncmp(id, "uuid:", 5) == 0 ? std::strcmp(id + 5, obs_source_get_uuid(source)) == 0
                                             : std::strcmp(id, obs_source_get_name(source)) == 0;
}
} // namespace glowbox
