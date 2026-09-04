#include "lm/platform/probes.hpp"

#include <unistd.h>

namespace lm::platform {

bool is_elevated() { return geteuid() == 0; }

}  // namespace lm::platform
