#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/algorithm/brightness.hpp>
using namespace vision;
TEST_CASE("brightness ignores padding and uses closed interval and floor") {
    std::vector<std::byte> pixels{std::byte{10},std::byte{11},std::byte{255},std::byte{255},
        std::byte{20},std::byte{21},std::byte{255},std::byte{255}};
    contracts::ImageLayout layout{2,2,4,7,8,contracts::PixelFormat::Mono8};
    auto result=algorithm::brightness(pixels,layout,15,15,[]{return false;});
    CHECK(result.mean==15);CHECK(result.pass);
    CHECK_FALSE(algorithm::brightness(pixels,layout,16,255,[]{return false;}).pass);
    CHECK_THROWS(algorithm::brightness(pixels,layout,16,15,[]{return false;}));
    CHECK_THROWS(algorithm::brightness(pixels,layout,0,255,[]{return true;}));
    CHECK_THROWS(algorithm::brightness(std::span(pixels).first(7),layout,0,255,[]{return false;}));
}
TEST_CASE("color channel mean and invalid layout") {
    std::vector<std::byte> pixels(12,std::byte{60});
    for(auto format:{contracts::PixelFormat::RGB8,contracts::PixelFormat::BGR8}) {
        contracts::ImageLayout layout{2,2,6,0,12,format};
        CHECK(algorithm::brightness(pixels,layout,60,60,[]{return false;}).pass);
        layout.stride=5;CHECK_THROWS(algorithm::brightness(pixels,layout,0,255,[]{return false;}));
    }
}
