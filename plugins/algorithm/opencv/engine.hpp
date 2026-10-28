#pragma once
#include <vision/contracts/frame.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <memory>
#include <span>

namespace vision::opencv_plugin {
// Private implementation boundary; OpenCV types never enter the plugin ABI.
class Engine {
public:
    explicit Engine(const nlohmann::json& config, const std::filesystem::path& base = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    nlohmann::json run(std::span<const std::byte> pixels, const contracts::ImageLayout& layout,
                       const std::atomic<bool>& stopped, std::uint64_t budget_ns) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::filesystem::path utf8_path(const std::string& value);
}
