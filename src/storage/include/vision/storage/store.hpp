#pragma once
#include <vision/contracts/types.hpp>
#include <vision/contracts/frame.hpp>
#include <filesystem>
#include <memory>
#include <span>
namespace vision::storage {
struct Intent {std::string event,output,state;unsigned attempt{};};
struct DeliveryTotals {std::uint64_t total{},acknowledged{},pending{},unconfirmed{};};
struct ClaimedIntent {std::string event,output,body,token;unsigned attempt{};std::uint64_t expires_ms{};};
struct UploadJob {std::string image,endpoint,bucket,key;unsigned attempt{};};
struct HistoryRow {std::uint64_t cursor;std::string body;};
std::vector<HistoryRow> history(const std::filesystem::path&,std::uint64_t after,unsigned limit);
std::string find_result(const std::filesystem::path&,std::string event);
struct ArchivedFrame {std::string image,channel,descriptor,recipe_hash,recipe_body;};
struct ImageInfo {std::string state,hash;std::uint64_t bytes;};
ImageInfo find_image(const std::filesystem::path&,std::string id);
class Store {
public:
    explicit Store(std::filesystem::path root,std::uint64_t quota=64*1024*1024);
    ~Store();
    bool commit(const contracts::ResultEnvelope&,const std::vector<std::string>& outputs); // Exact duplicate=false; conflict throws.
    std::vector<std::string> query(std::uint64_t after,unsigned limit=50);
    std::vector<Intent> pending(unsigned limit=50);
    DeliveryTotals delivery_totals(std::string output);
    void observe(std::string event,std::string output,unsigned attempt,std::string state);
    std::optional<ClaimedIntent> claim(std::string output,std::string token,std::uint64_t now_ms);
    void settle(const ClaimedIntent&,std::string state,std::uint64_t now_ms);
    void begin_inspection(std::string run,std::string inspection);
    unsigned recover_interrupted();
    std::string stage_image(std::string id,std::span<const std::byte>);
    std::string archive_frame(const contracts::FrameDescriptor&,std::uint64_t slot_bytes,
        std::string inspection,std::string channel,std::string recipe_hash,std::string recipe_body,std::span<const std::byte>);
    std::vector<ArchivedFrame> frames(std::string run,std::string inspection);
    void bind_object(const UploadJob&);
    std::vector<UploadJob> pending_uploads(unsigned limit=5);
    unsigned start_upload(std::string id);
    void fail_image(std::string id);
    std::vector<std::byte> image(std::string id);
    void available(std::string id);
    void deleted(std::string id);
    std::string image_state(std::string id);
    void checkpoint();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
class S3 {
public:
    S3(std::string endpoint,std::string bucket,std::string access,std::string secret);
    void put(std::string key,std::span<const std::byte>);
    std::vector<std::byte> get(std::string key);
    void remove(std::string key);
private:
    std::vector<std::byte> request(const char*,std::string,std::span<const std::byte>);
    std::string endpoint_,bucket_,access_,secret_;
};
}
