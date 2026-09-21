#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/capture/source.hpp>
#include <fstream>
#include <chrono>

using namespace vision::capture;
using namespace vision::contracts;
namespace {
std::vector<std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),reinterpret_cast<const std::byte*>(text.data()+text.size())};
}
const ImageLayout mono{2,2,2,0,4,PixelFormat::Mono8};
struct Files {
    std::filesystem::path root=std::filesystem::current_path()/
        ("capture-unit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Files() {std::filesystem::create_directory(root);}
    ~Files() {std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    void write(const std::filesystem::path& name,std::string_view content) {
        std::ofstream file(root/name,std::ios::binary);file.write(content.data(),static_cast<std::streamsize>(content.size()));
        REQUIRE(file.good());
    }
    SourceConfig config(SourceKind kind,std::vector<std::filesystem::path> names) {
        return {kind,2,2,1,42,root,std::move(names)};
    }
};
}
TEST_CASE("synthetic frames are repeatable bounded and sequence sensitive") {
    SourceConfig cfg;cfg.width=7;cfg.height=3;cfg.seed=41;
    Source a(cfg),b(cfg);
    CHECK(a.read(1).pixels==b.read(1).pixels);
    CHECK(a.read(1).pixels!=a.read(2).pixels);
    cfg.seed=42;CHECK(a.read(1).pixels!=Source(cfg).read(1).pixels);
    CHECK(a.read(UINT64_MAX).pixels==b.read(UINT64_MAX).pixels);
    CHECK_THROWS(a.read(0));
    cfg.channels=3;auto rgb=Source(cfg).read(1);
    CHECK(rgb.layout.format==PixelFormat::RGB8);CHECK(rgb.pixels.size()==63);
    for(auto width:{0U,4097U}) {cfg.width=width;CHECK_THROWS_AS(Source{cfg},std::invalid_argument);}
    cfg.width=4096;cfg.height=4096;CHECK_THROWS_AS(Source{cfg},std::invalid_argument);
    cfg.width=1;cfg.height=1;cfg.channels=2;CHECK_THROWS_AS(Source{cfg},std::invalid_argument);
}
TEST_CASE("PNM checks exact raster size format header and preserves whitespace pixels") {
    CHECK(decode_pnm(bytes("P5\n2 2\n255\n ab\n"),mono).pixels==bytes(" ab\n"));
    CHECK(decode_pnm(bytes("P5\r\n# comment\r\n2 2\r\n255\r\nabcd"),mono).pixels==bytes("abcd"));
    for(const auto& invalid:std::vector<std::string>{
        "P2\n2 2\n255\nabcd","P5\n2 2\n65535\nabcd","P5\n3 2\n255\nabcdef",
        "P5\n2 2\n255\nabc","P5\n2 2\n255\nabcde","P5\n-2 2\n255\nabcd",
        "P5\n4294967296 2\n255\nabcd","P5\n2 2\n255",
        "P5\n#"+std::string(4096,'x')+"\n2 2\n255\nabcd"})
        CHECK_THROWS(decode_pnm(bytes(invalid),mono));
    const ImageLayout rgb{2,1,6,0,6,PixelFormat::RGB8};
    CHECK(decode_pnm(bytes("P6\n2 1\n255\nabcdef"),rgb).pixels==bytes("abcdef"));
    CHECK_THROWS(decode_pnm(bytes("P5\n2 1\n255\nab"),rgb));
    auto invalid=mono;invalid.offset=1;CHECK_THROWS(decode_pnm(bytes("P5\n2 2\n255\nabcd"),invalid));
}
TEST_CASE("fixed and ordered replay use one bounded asset at a time") {
    Files files;
    const auto chinese=std::filesystem::path(u8"图像.pgm");
    files.write(chinese,"P5\n2 2\n255\nabcd");files.write("second.pgm","P5\n2 2\n255\nwxyz");
    Source fixed(files.config(SourceKind::Fixed,{chinese}));
    CHECK(fixed.read(1).pixels==bytes("abcd"));CHECK(fixed.read(100).pixels==bytes("abcd"));
    Source sequence(files.config(SourceKind::Sequence,{chinese,"second.pgm"}));
    CHECK(sequence.read(1).pixels==bytes("abcd"));CHECK(sequence.read(2).pixels==bytes("wxyz"));
    CHECK(sequence.read(3).pixels==bytes("abcd"));
    files.write("second.pgm","P5\n2 2\n255\nbad");
    CHECK_THROWS(sequence.read(2));
    CHECK_THROWS_AS(Source(files.config(SourceKind::Fixed,{chinese,"second.pgm"})),std::invalid_argument);
    CHECK_THROWS_AS(Source(files.config(SourceKind::Sequence,{})),std::invalid_argument);
    CHECK_THROWS_AS(Source(files.config(SourceKind::Sequence,std::vector<std::filesystem::path>(257,chinese))),std::invalid_argument);
}
TEST_CASE("replay rejects traversal absolute paths directories streams and growing assets") {
    Files files;files.write("good.pgm","P5\n2 2\n255\nabcd");
    for(const auto& path:std::vector<std::filesystem::path>{"../other.pgm",files.root/"good.pgm",".","good.pgm:stream"})
        CHECK_THROWS(Source(files.config(SourceKind::Fixed,{path})));
    auto config=files.config(SourceKind::Fixed,{"good.pgm"});Source source(config);
    std::filesystem::resize_file(files.root/"good.pgm",max_frame_bytes+max_header_bytes+1);
    CHECK_THROWS(source.read(1));
    config.kind=SourceKind::Synthetic;CHECK_THROWS_AS(Source{config},std::invalid_argument);
}
