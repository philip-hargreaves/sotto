#pragma once

#include <cstdlib>

namespace ambient {

// Investigative ablations are toggled by environment variables so one binary
// carries both arms; not part of the product surface
inline bool EnvFlag(const char* name) {
#pragma warning(suppress : 4996)
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

}  // namespace ambient
