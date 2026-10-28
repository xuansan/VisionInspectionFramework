#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "../plugins/algorithm/opencv/engine.hpp"
#include <fstream>
#include <chrono>
using namespace vision;
using J=nlohmann::json;
namespace {
std::atomic<bool> stopped{};
contracts::ImageLayout mono{12,10,16,7,160,contracts::PixelFormat::Mono8};
std::vector<std::byte> image() {
    std::vector<std::byte> p(160,std::byte{0});
    for(int y=3;y<7;++y)for(int x=2;x<7;++x)p[y*16+x]=std::byte{255};
    for(int y=0;y<10;++y)for(int x=12;x<16;++x)p[y*16+x]=std::byte{255};
    return p;
}
J run(J config,std::vector<std::byte> p=image(),contracts::ImageLayout layout=mono) {
    opencv_plugin::Engine e(config);return e.run(p,layout,stopped,3000000000ULL);
}
double measure(const J& r,const char* name) {
    for(const auto& m:r["measurements"])if(m["name"]==name)return m["value"];
    throw std::runtime_error("Missing measurement");
}
struct Template {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("vision-opencv-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count())+".pgm");
    Template(bool constant=false) {
        std::ofstream f(path,std::ios::binary);f<<"P5\n3 3\n255\n";
        const unsigned char data[9]={0,30,255,120,200,10,250,80,180};
        for(auto v:data)f.put(static_cast<char>(constant?128:v));
    }
    ~Template(){std::error_code ec;std::filesystem::remove(path,ec);}
};
}
TEST_CASE("opencv exact component geometry and immutable padded nonzero-offset image") {
    auto p=image();const auto original=p;opencv_plugin::Engine engine({{"mode","components"}});
    auto r=engine.run(p,mono,stopped,3000000000ULL);
    CHECK(r["quality"]=="OK");CHECK(measure(r,"foreground_pixels")==20);
    CHECK(measure(r,"component_count")==1);CHECK(measure(r,"component_0_width")==5);
    CHECK(measure(r,"component_0_height")==4);CHECK(measure(r,"component_0_area")==20);
    CHECK(measure(r,"component_0_x")==4);CHECK(measure(r,"component_0_y")==4.5);
    CHECK(p==original);CHECK(contracts::parse_u64(r["elapsed_ns"].get<std::string>())>0);
}
TEST_CASE("opencv NG is successful execution for missing multiple and size mismatch") {
    auto r=run({{"mode","measure"},{"min_width",6}});
    CHECK(r["quality"]=="NG");CHECK(r["execution_state"]=="Succeeded");
    CHECK(r["defects"][0]["box_xyxy"]==J::array({2,3,7,7}));
    auto p=image();p[0]=std::byte{255};r=run({{"mode","components"}},p);
    CHECK(r["quality"]=="NG");CHECK(measure(r,"component_count")==2);
    r=run({{"mode","components"}},std::vector<std::byte>(160));
    CHECK(r["quality"]=="NG");CHECK(measure(r,"component_count")==0);
}
TEST_CASE("opencv ROI original coordinates and area filter") {
    auto r=run({{"mode","components"},{"roi",{{"x",2},{"y",3},{"width",5},{"height",4}}}});
    CHECK(measure(r,"component_0_x")==4);CHECK(measure(r,"component_0_y")==4.5);
    CHECK(measure(run({{"mode","components"},{"min_area",21}}),"component_count")==0);
    CHECK_THROWS(run({{"mode","components"},{"roi",{{"x",10},{"y",9},{"width",3},{"height",2}}}}));
}
TEST_CASE("opencv morphology removes speckle and closes interior hole") {
    auto p=image();p[0]=std::byte{255};
    CHECK(measure(run({{"mode","threshold"},{"morphology","open"}},p),"component_count")==1);
    p=image();p[4*16+4]=std::byte{0};
    CHECK(measure(run({{"mode","threshold"},{"morphology","close"}},p),"foreground_pixels")==20);
}
TEST_CASE("opencv threshold closed boundary inverse and RGB BGR grayscale ordering") {
    CHECK(measure(run({{"mode","threshold"},{"threshold",255},{"min_count",0}}),"foreground_pixels")==0);
    CHECK(measure(run({{"mode","threshold"},{"invert",true},{"max_count",64}}),"foreground_pixels")==100);
    std::vector<std::byte> p(8);p[0]=p[3]=std::byte{255};
    contracts::ImageLayout l{2,1,8,0,8,contracts::PixelFormat::RGB8};
    CHECK(measure(run({{"mode","threshold"},{"threshold",50}},p,l),"foreground_pixels")==2);
    l.format=contracts::PixelFormat::BGR8;
    CHECK(measure(run({{"mode","threshold"},{"threshold",50},{"min_count",0}},p,l),"foreground_pixels")==0);
}
TEST_CASE("opencv strict config limits and layouts") {
    for(const auto& config:std::vector<J>{{{"mode","unknown"}},{{"mode","measure"},{"kernel",2}},
        {{"mode","measure"},{"threshold",256}},{{"mode","measure"},{"min_count",2},{"max_count",1}},
        {{"mode","measure"},{"typo",true}},{{"mode","measure"},{"roi",{{"x",0}}}},
        {{"mode","measure"},{"min_width",8},{"max_width",7}}})
        CHECK_THROWS(opencv_plugin::Engine(config));
    auto l=mono;l.stride=11;CHECK_THROWS(run({{"mode","measure"}},image(),l));
    CHECK_THROWS(run({{"mode","measure"}},std::vector<std::byte>(159)));
    l=mono;l.format=static_cast<contracts::PixelFormat>(99);CHECK_THROWS(run({{"mode","measure"}},image(),l));
}
TEST_CASE("opencv cancellation and deadline reject execution") {
    opencv_plugin::Engine e({{"mode","measure"}});stopped=true;
    CHECK_THROWS(e.run(image(),mono,stopped,3000000000ULL));stopped=false;
    CHECK_THROWS(e.run(image(),mono,stopped,1));
}
TEST_CASE("opencv evidence and image resource bounds") {
    contracts::ImageLayout l{32,32,32,0,1024,contracts::PixelFormat::Mono8};
    std::vector<std::byte> pixels(1024);
    for(int y=0;y<32;y+=3)for(int x=0;x<32;x+=3)pixels[y*32+x]=std::byte{255};
    CHECK_THROWS(run({{"mode","components"},{"max_count",64}},pixels,l));
    l={4096,4096,4096,0,16777216,contracts::PixelFormat::Mono8};
    CHECK_THROWS(run({{"mode","components"}},std::vector<std::byte>(16777216),l));
}
TEST_CASE("opencv fixed template localization score and missing target") {
    Template t;auto p=std::vector<std::byte>(160);
    const unsigned char data[9]={0,30,255,120,200,10,250,80,180};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x)p[(y+5)*16+x+6]=static_cast<std::byte>(data[y*3+x]);
    auto r=run({{"mode","template"},{"template",t.path.generic_string()},{"min_score",0.99}},p);
    CHECK(r["quality"]=="OK");CHECK(measure(r,"match_x")==6);CHECK(measure(r,"match_y")==5);
    CHECK(measure(r,"match_score")==doctest::Approx(1.0).epsilon(1e-5));
    CHECK(run({{"mode","template"},{"template",t.path.generic_string()},{"min_score",0.99}},
        std::vector<std::byte>(160))["quality"]=="NG");
    Template constant(true);CHECK_THROWS(opencv_plugin::Engine({{"mode","template"},{"template",constant.path.generic_string()}}));
    CHECK_THROWS(run({{"mode","template"},{"template",t.path.generic_string()},
        {"roi",{{"x",0},{"y",0},{"width",2},{"height",2}}}}));
}
