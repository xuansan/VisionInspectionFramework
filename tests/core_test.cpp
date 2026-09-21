#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/application/build_info.hpp>
#include <type_traits>

TEST_CASE("Build identity is usable without Qt and does not claim inspection capability") {
    const auto info = vision::application::build_info();
    CHECK_FALSE(info.version.empty());
    CHECK(info.stage == "core-foundation");
    CHECK_FALSE(info.inspection_available);
    static_assert(std::is_trivially_copyable_v<vision::contracts::BuildInfo>);
}
