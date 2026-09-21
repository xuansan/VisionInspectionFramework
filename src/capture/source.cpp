#include <vision/capture/source.hpp>
#include <charconv>
#include <fstream>
#include <cstring>

namespace vision::capture {
using namespace contracts;
namespace {
bool space(unsigned char c) {return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';}
class Header {
public:
    explicit Header(std::span<const std::byte> bytes):bytes_(bytes) {}
    std::string_view token() {
        for(;;) {
            while(position_<bytes_.size()&&space(at(position_)))advance();
            if(position_>=bytes_.size())throw std::invalid_argument("Truncated PNM header");
            if(at(position_)!='#')break;
            while(position_<bytes_.size()&&at(position_)!='\n')advance();
        }
        const auto start=position_;
        while(position_<bytes_.size()&&!space(at(position_)))advance();
        if(position_==start)throw std::invalid_argument("Empty PNM token");
        return {reinterpret_cast<const char*>(bytes_.data()+start),position_-start};
    }
    std::uint32_t number() {
        const auto value=token();std::uint32_t result{};
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())
            throw std::invalid_argument("Invalid PNM integer");
        return result;
    }
    std::size_t raster() {
        if(position_>=bytes_.size()||!space(at(position_)))throw std::invalid_argument("Missing raster delimiter");
        const auto c=at(position_);advance();
        if(c=='\r'&&position_<bytes_.size()&&at(position_)=='\n')advance();
        return position_; // Never skip raster whitespace: it is pixel data.
    }
private:
    unsigned char at(std::size_t p) const {return std::to_integer<unsigned char>(bytes_[p]);}
    void advance() {
        if(position_>=max_header_bytes)throw std::invalid_argument("PNM header limit");
        ++position_;
    }
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};
std::filesystem::path asset(const std::filesystem::path& root,const std::filesystem::path& relative) {
    if(relative.empty()||relative.is_absolute()||relative.has_root_name()||relative.has_root_directory())
        throw std::invalid_argument("Relative replay asset required");
    for(const auto& part:relative)
        if(part==".."||part.native().find(static_cast<std::filesystem::path::value_type>(':'))!=std::filesystem::path::string_type::npos)
            throw std::invalid_argument("Replay asset traversal or stream");
    const auto target=std::filesystem::canonical(root/relative);
    auto base=root.begin(),child=target.begin();
    for(;base!=root.end();++base,++child)
        if(child==target.end()||*base!=*child)throw std::invalid_argument("Replay asset escapes root");
    if(!std::filesystem::is_regular_file(target))throw std::invalid_argument("Replay asset is not a file");
    return target;
}
}
Image decode_pnm(std::span<const std::byte> bytes,ImageLayout expected) {
    validate_layout(expected,max_frame_bytes);
    if(expected.width>4096||expected.height>4096||expected.offset||
       expected.stride!=checked_mul(expected.width,expected.format==PixelFormat::Mono8?1:3)||
       expected.format==PixelFormat::BGR8||bytes.size()>max_frame_bytes+max_header_bytes)
        throw std::invalid_argument("Unsupported replay layout");
    Header h(bytes);const auto magic=h.token();
    if(magic!="P5"&&magic!="P6")throw std::invalid_argument("Only binary PGM/PPM supported");
    const auto width=h.number(),height=h.number(),maximum=h.number();
    if(width!=expected.width||height!=expected.height||maximum!=255||
       (magic=="P5")!=(expected.format==PixelFormat::Mono8))
        throw std::invalid_argument("Replay dimensions or format mismatch");
    const auto start=h.raster();
    if(bytes.size()-start!=expected.length)throw std::invalid_argument("Truncated or trailing replay data");
    return {expected,{bytes.begin()+static_cast<std::ptrdiff_t>(start),bytes.end()}};
}
Image decode_pnm(std::span<const std::byte> bytes) {
    Header header(bytes);const auto magic=header.token();
    if(magic!="P5"&&magic!="P6")throw std::invalid_argument("PNM format");
    const auto width=header.number(),height=header.number();
    const auto stride=checked_mul(width,magic=="P5"?1:3);
    return decode_pnm(bytes,{width,height,stride,0,checked_mul(stride,height),
        magic=="P5"?PixelFormat::Mono8:PixelFormat::RGB8});
}
Source::Source(SourceConfig config):config_(std::move(config)) {
    if(!config_.width||config_.width>4096||!config_.height||config_.height>4096||
       (config_.channels!=1&&config_.channels!=3))throw std::invalid_argument("Invalid source dimensions");
    validate_layout(layout(),max_frame_bytes);
    if(config_.kind==SourceKind::Synthetic) {
        if(!config_.files.empty()||!config_.root.empty())throw std::invalid_argument("Synthetic source has no assets");
    } else {
        if(config_.kind!=SourceKind::Fixed&&config_.kind!=SourceKind::Sequence)throw std::invalid_argument("Unknown source kind");
        if(config_.root.empty()||config_.files.empty()||config_.files.size()>256||
           (config_.kind==SourceKind::Fixed&&config_.files.size()!=1))throw std::invalid_argument("Invalid replay list");
        root_=std::filesystem::canonical(config_.root);
        if(!std::filesystem::is_directory(root_))throw std::invalid_argument("Replay root is not a directory");
        for(const auto& path:config_.files)(void)asset(root_,path);
    }
}
ImageLayout Source::layout() const {
    const auto stride=checked_mul(config_.width,config_.channels);
    return {config_.width,config_.height,stride,0,checked_mul(stride,config_.height),
        config_.channels==1?PixelFormat::Mono8:PixelFormat::RGB8};
}
Image Source::read(std::uint64_t sequence) const {
    if(!sequence)throw std::invalid_argument("Frame sequence starts at one");
    const auto expected=layout();
    if(config_.kind==SourceKind::Synthetic) {
        Image image{expected,std::vector<std::byte>(static_cast<std::size_t>(expected.length))};
        // Unsigned wrap is intentional and deterministic across builds.
        std::uint64_t state=(static_cast<std::uint64_t>(config_.seed)<<32)^sequence^0x9e3779b97f4a7c15ULL;
        for(auto& pixel:image.pixels) {
            state^=state>>12;state^=state<<25;state^=state>>27;
            pixel=static_cast<std::byte>((state*2685821657736338717ULL)>>56);
        }
        return image;
    }
    const auto index=config_.kind==SourceKind::Fixed?0:(sequence-1)%config_.files.size();
    const auto path=asset(root_,config_.files[static_cast<std::size_t>(index)]);
    const auto length=std::filesystem::file_size(path);
    if(length>max_frame_bytes+max_header_bytes)throw std::invalid_argument("Replay file limit");
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open replay asset");
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    if(!file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()))||
       file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Replay asset changed or truncated");
    return decode_pnm(data,expected);
}
} // namespace vision::capture
