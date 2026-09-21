#include "../output-common/async.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
namespace {
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const nlohmann::json& j) override {
        if(j.size()!=2||!j.contains("root")||!j.contains("outputs_json"))throw std::invalid_argument("Storage parameters");
        root_=vision::inference::utf8_path(j.at("root").get<std::string>());
        routes_=nlohmann::json::parse(j.at("outputs_json").get<std::string>()).get<std::vector<std::string>>();
        if(!std::filesystem::is_directory(root_)||routes_.size()>8)throw std::invalid_argument("Storage root/routes");
    }
    void write(const nlohmann::json& request) override {
        // One connection per worker IO thread. Do not reopen/migrate/checkpoint on every workpiece.
        if(!store_)store_=std::make_unique<vision::storage::Store>(root_);
        store_->commit(vision::serialization::decode_result(request.at("event").dump()),routes_);
    }
    void io_stopped() noexcept override {store_.reset();}
    std::filesystem::path root_;std::vector<std::string> routes_;
    std::unique_ptr<vision::storage::Store> store_;
};
}
#define VISION_OUTPUT_ID "vision.sqlite-output"
#define VISION_OUTPUT_BUILD "sqlite-output-v1"
#include "../output-common/exports.hpp"
