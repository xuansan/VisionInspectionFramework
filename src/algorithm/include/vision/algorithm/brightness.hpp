#pragma once
#include <vision/contracts/frame.hpp>
#include <span>
#include <functional>
namespace vision::algorithm {
struct Brightness {std::uint32_t mean;bool pass;};
// Pixels span starts at layout.offset, as returned by Mapping::read.
Brightness brightness(std::span<const std::byte> pixels,const contracts::ImageLayout&,
    std::uint32_t minimum,std::uint32_t maximum,const std::function<bool()>& cancelled);
}
