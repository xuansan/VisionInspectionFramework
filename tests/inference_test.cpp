#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/inference/engine.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
using namespace vision;
namespace {
inference::Config configuration() {return inference::read_config(std::filesystem::path(VISION_MODEL_DIR)/"detection.json");}
}
TEST_CASE("actual ONNX CPU matches independent reference including NMS") {
    const auto bytes=inference::read_file(std::filesystem::path(VISION_MODEL_DIR)/"detection-reference.json",65536);
    const auto reference=nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
    inference::Engine engine(configuration());
    for(unsigned value:{0U,128U,255U}) {
        std::vector<std::byte> pixels(64,static_cast<std::byte>(value));
        const auto out=engine.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8});
        REQUIRE(out.detections.size()==2);
        for(std::size_t i=0;i<2;++i) {
            const auto& expected=reference[std::to_string(value)][i];
            CHECK(out.detections[i].class_id==expected["class_id"].get<int>());
            CHECK(std::abs(out.detections[i].score-expected["score"].get<double>())<0.0001);
            const auto& d=out.detections[i];const double box[]={d.x1,d.y1,d.x2,d.y2};
            for(int n=0;n<4;++n)CHECK(std::abs(box[n]-expected["box"][n].get<double>())<0.000001);
        }
    }
}
TEST_CASE("empty threshold non square ROI padded color and limits") {
    auto c=configuration();c.score=.99;inference::Engine empty(c);
    std::vector<std::byte> pixels(64,std::byte{0});
    CHECK(empty.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8}).detections.empty());
    c=configuration();c.roi_x=2;c.roi_y=1;c.roi_width=4;c.roi_height=4;
    inference::Engine roi(c);auto out=roi.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8});
    REQUIRE(out.detections.size()==2);CHECK(out.detections[0].x1==4.5);CHECK(out.detections[0].y1==3.5);
    c=configuration();inference::Engine non_square(c);
    std::vector<std::byte> short_image(32,std::byte{0});
    auto clipped=non_square.run(short_image,{8,4,8,0,32,contracts::PixelFormat::Mono8});
    REQUIRE(clipped.detections.size()==2);
    CHECK(clipped.detections[0].y1==3);CHECK(clipped.detections[0].y2==4);
    for(auto format:{contracts::PixelFormat::RGB8,contracts::PixelFormat::BGR8}) {
        std::vector<std::byte> color(8*28,std::byte{255});
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<24;++x)color[y*28+x]=std::byte{0};
        auto result=non_square.run(color,{8,8,28,0,224,format});
        CHECK(std::abs(result.detections[0].score-.7)<.0001);
    }
    CHECK_THROWS(non_square.run(short_image,{8,8,8,0,64,contracts::PixelFormat::Mono8}));
    non_square.cancel();CHECK_THROWS(non_square.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8}));
}
TEST_CASE("bad hash labels shape paths rejected before Ready") {
    auto c=configuration();c.model_hash="sha256:"+std::string(64,'0');CHECK_THROWS(inference::Engine{c});
    c=configuration();c.width=9;CHECK_THROWS(inference::Engine{c});
    c=configuration();c.labels={"one"};CHECK_THROWS(inference::Engine{c});
    c=configuration();c.score=1.1;CHECK_THROWS(inference::Engine{c});
    c=configuration();c.model+="missing";CHECK_THROWS(inference::Engine{c});
}
TEST_CASE("classification stable softmax reference top k and uncertainty") {
    auto c=inference::read_config(std::filesystem::path(VISION_MODEL_DIR)/"classification.json");
    const auto bytes=inference::read_file(std::filesystem::path(VISION_MODEL_DIR)/"classification-reference.json",65536);
    const auto reference=nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
    inference::Engine engine(c);
    for(unsigned value:{0U,128U,255U}) {
        std::vector<std::byte> pixels(64,static_cast<std::byte>(value));
        const auto out=engine.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8});
        REQUIRE(out.classification);REQUIRE(out.measurements.size()==3);
        CHECK(*out.classification==(value==0?"scratch":"good"));
        double sum=0;
        for(const auto& m:out.measurements) {
            const auto it=std::find(c.labels.begin(),c.labels.end(),m.name.substr(6));
            REQUIRE(it!=c.labels.end());
            CHECK(std::abs(m.value-reference[std::to_string(value)][it-c.labels.begin()].get<double>())<0.0001);
            sum+=m.value;
        }
        CHECK(std::abs(sum-1)<0.000001);
    }
    c.score=.99;c.max_results=1;inference::Engine uncertain(c);
    std::vector<std::byte> pixels(64);
    const auto out=uncertain.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8});
    CHECK_FALSE(out.classification);CHECK(out.measurements.size()==1);
    c.scores="probabilities";inference::Engine invalid_probability(c);
    CHECK_THROWS(invalid_probability.run(pixels,{8,8,8,0,64,contracts::PixelFormat::Mono8}));
    c.ok_label="missing";CHECK_THROWS(inference::Engine{c});
}
TEST_CASE("segmentation masks match independent reference IoU and reject expansion") {
    auto c=inference::read_config(std::filesystem::path(VISION_MODEL_DIR)/"segmentation.json");
    inference::Engine engine(c);
    std::vector<std::byte> image(64);
    const auto out=engine.run(image,{8,8,8,0,64,contracts::PixelFormat::Mono8});
    REQUIRE(out.masks.size()==2);
    auto bytes=inference::read_file(std::filesystem::path(VISION_MODEL_DIR)/"segmentation-reference.json",65536);
    auto ref=nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
    for(unsigned n=0;n<2;++n) {
        unsigned intersection=0,united=0;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
            const bool actual=out.masks[n].pixels[y*8+x]!=0,expected=ref[n][y][x].get<unsigned>()!=0;
            intersection+=actual&&expected;united+=actual||expected;
        }
        REQUIRE(united>0);CHECK(static_cast<double>(intersection)/united==1);
    }
    std::vector<std::byte> large(1025*1024);
    CHECK_THROWS(engine.run(large,{1025,1024,1025,0,large.size(),contracts::PixelFormat::Mono8}));
    c.roi_x=2;c.roi_y=2;c.roi_width=4;c.roi_height=4;inference::Engine roi(c);
    const auto cropped=roi.run(image,{8,8,8,0,64,contracts::PixelFormat::Mono8});
    for(const auto& mask:cropped.masks)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
        if(x<2||x>=6||y<2||y>=6)CHECK(mask.pixels[y*8+x]==0);
    engine.cancel();CHECK_THROWS(engine.run(image,{8,8,8,0,64,contracts::PixelFormat::Mono8}));
}
TEST_CASE("bounded mask store deduplicates verifies hashes and rejects exhausted capacity") {
    const auto root=std::filesystem::temp_directory_path()/("vision-mask-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{root};
    inference::Mask mask{8,8,std::vector<std::uint8_t>(64,255)};
    auto refs=inference::store_masks(root,{mask});REQUIRE(refs.size()==1);
    CHECK(inference::load_mask(root,refs[0]).size()==64);
    CHECK(inference::store_masks(root,{mask})[0].hash==refs[0].hash);
    auto invalid=refs[0];invalid.width=9;CHECK_THROWS(inference::load_mask(root,invalid));
    {std::ofstream f(root/(refs[0].hash.substr(7)+".mask"),std::ios::binary);f<<"corrupt";}
    CHECK_THROWS(inference::load_mask(root,refs[0]));
    CHECK_THROWS(inference::store_masks(root,{mask}));
    {std::ofstream f(root/"quota.bin",std::ios::binary);f.seekp(64*1024*1024);f.put('x');}
    CHECK_THROWS(inference::store_masks(root,{mask}));
}
