#pragma once
#include <vision/contracts/types.hpp>
#include <limits>

namespace vision::contracts {
enum class PixelFormat : std::uint32_t { Mono8=1, RGB8=2, BGR8=3 };
struct ImageLayout {
    std::uint32_t width{},height{};
    std::uint64_t stride{},offset{},length{};
    PixelFormat format{PixelFormat::Mono8};
    bool operator==(const ImageLayout&) const = default;
};
inline std::uint64_t checked_add(std::uint64_t a,std::uint64_t b) {
    if(b>UINT64_MAX-a)throw std::invalid_argument("Image addition overflow");
    return a+b;
}
inline std::uint64_t checked_mul(std::uint64_t a,std::uint64_t b) {
    if(a&&b>UINT64_MAX/a)throw std::invalid_argument("Image multiplication overflow");
    return a*b;
}
inline void validate_layout(const ImageLayout& l,std::uint64_t slot_bytes) {
    const auto channels=l.format==PixelFormat::Mono8?1U:
        (l.format==PixelFormat::RGB8||l.format==PixelFormat::BGR8?3U:0U);
    if(!channels||!l.width||!l.height||l.stride<checked_mul(l.width,channels)||
       l.length!=checked_mul(l.stride,l.height)||checked_add(l.offset,l.length)>slot_bytes)
        throw std::invalid_argument("Invalid image layout");
}
struct FrameOwner {
    WorkerId worker;
    std::uint64_t epoch;
    bool operator==(const FrameOwner&) const = default;
};
struct FrameLease {
    RunId run;
    PoolId pool;
    FrameOwner owner;
    std::uint32_t slot{};
    std::uint64_t generation{},lease{};
    bool operator==(const FrameLease&) const = default;
};
struct FrameDescriptor {
    FrameLease permit;
    FrameId frame;
    ImageLayout layout;
    bool operator==(const FrameDescriptor&) const = default;
};
} // namespace vision::contracts
