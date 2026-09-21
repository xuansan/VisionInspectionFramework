#pragma once
#include <vision/contracts/build_info.hpp>

namespace vision::application {
[[nodiscard]] contracts::BuildInfo build_info() noexcept;
}
