#include <vision/algorithm/brightness.hpp>
namespace vision::algorithm {
Brightness brightness(std::span<const std::byte> pixels,const contracts::ImageLayout& layout,
    std::uint32_t minimum,std::uint32_t maximum,const std::function<bool()>& cancelled) {
    contracts::validate_layout(layout,contracts::checked_add(layout.offset,layout.length));
    if(minimum>maximum||maximum>255||pixels.size()!=layout.length||pixels.size()>16*1024*1024)
        throw std::invalid_argument("Brightness bounds");
    const auto channels=layout.format==contracts::PixelFormat::Mono8?1U:3U;
    const auto width=static_cast<std::uint64_t>(layout.width)*channels;
    std::uint64_t sum=0;
    for(std::uint32_t y=0;y<layout.height;++y) {
        if(cancelled())throw std::runtime_error("Cancelled");
        for(std::uint64_t x=0;x<width;++x)sum+=std::to_integer<unsigned>(pixels[y*layout.stride+x]);
    }
    const auto mean=static_cast<std::uint32_t>(sum/(width*layout.height));
    return {mean,mean>=minimum&&mean<=maximum};
}
}
