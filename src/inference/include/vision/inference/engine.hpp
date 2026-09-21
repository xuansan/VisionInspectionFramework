#pragma once
#include <vision/contracts/frame.hpp>
#include <filesystem>
#include <memory>
#include <span>
namespace vision::inference {
std::filesystem::path utf8_path(std::string_view);
std::string sha256(std::span<const std::byte>);
std::vector<std::byte> read_file(const std::filesystem::path&,std::size_t limit);
struct Mask {std::uint32_t width{},height{};std::vector<std::uint8_t> pixels;};
std::vector<contracts::MaskReference> store_masks(const std::filesystem::path&,const std::vector<Mask>&);
std::vector<std::byte> load_mask(const std::filesystem::path&,const contracts::MaskReference&);
struct Output {
    std::vector<contracts::Defect> detections;
    std::vector<contracts::Measurement> measurements;
    std::optional<std::string> classification;
    std::vector<Mask> masks;
    std::string model_hash;
};
struct Config {
    std::filesystem::path model;
    std::string model_hash,type{"detection"},input{"images"},output{"output0"},prototypes{"output1"};
    std::vector<std::string> labels;
    std::string scores{"logits"},ok_label;
    std::uint32_t width{},height{},max_results{64};
    double score{0.25},iou{0.45};
    std::uint32_t roi_x{},roi_y{},roi_width{},roi_height{};
};
Config read_config(const std::filesystem::path&);
class Engine {
public:
    explicit Engine(Config);
    ~Engine();
    Output run(std::span<const std::byte>,const contracts::ImageLayout&);
    void cancel() noexcept;
    const Config& config() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
