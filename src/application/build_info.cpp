#include <vision/application/build_info.hpp>
#include "build_version.hpp"

namespace vision::application {
contracts::BuildInfo build_info() noexcept {
    return {VISION_BUILD_VERSION, "core-foundation", false};
}
}
