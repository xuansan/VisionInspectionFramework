#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/replay/assets.hpp>
#include <vision/inference/engine.hpp>
#include <fstream>
#include <chrono>
using namespace vision;
TEST_CASE("immutable snapshots reuse exact assets and reject corruption missing files and wrong identity") {
    const auto root=std::filesystem::temp_directory_path()/("vision-replay-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{root};
    const auto source=std::filesystem::path(VISION_MODEL_DIR);
    const auto image_before=inference::read_file(source/"black.pgm",4096);
    const auto a=replay::create_snapshot(source/"detection.json",source/"black.pgm",root);
    const auto b=replay::create_snapshot(source/"detection.json",source/"black.pgm",root);
    CHECK(a.hash==b.hash);CHECK(a.manifest==b.manifest);
    CHECK(inference::read_file(source/"black.pgm",4096)==image_before);
    CHECK_THROWS(replay::load_snapshot(a.manifest,"sha256:"+std::string(64,'0')));
    {std::ofstream f(a.image,std::ios::binary);f<<"corrupt";}
    CHECK_THROWS(replay::load_snapshot(a.manifest,a.hash));
    CHECK_THROWS(replay::create_snapshot(source/"detection.json",source/"black.pgm",root));
    const auto c=replay::create_snapshot(source/"classification.json",source/"black.pgm",root);
    std::filesystem::remove(c.config);CHECK_THROWS(replay::load_snapshot(c.manifest,c.hash));
    const auto d=replay::create_snapshot(source/"segmentation.json",source/"black.pgm",root);
    {std::ofstream f(d.config.parent_path()/"model.onnx",std::ios::binary);f<<"bad";}
    CHECK_THROWS(replay::load_snapshot(d.manifest,d.hash));
}
