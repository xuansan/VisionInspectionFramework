#pragma once
#include <vision/contracts/frame.hpp>
#include <filesystem>
namespace vision::replay {
struct Snapshot {
    std::filesystem::path manifest,config,image;
    std::string hash,pixel_hash;
    contracts::ImageLayout layout;
};
Snapshot create_snapshot(const std::filesystem::path& config,const std::filesystem::path& image,const std::filesystem::path& root);
Snapshot load_snapshot(const std::filesystem::path& manifest,std::string_view expected_hash);
}
