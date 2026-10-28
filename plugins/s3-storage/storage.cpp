#include "../output-common/async.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <cstdlib>
#include <memory>
#define NOMINMAX
#include <windows.h>
namespace {
class Upload final:public vision::outputs::AsyncOutput {
public:
    ~Upload(){finish();}
private:
    void configure(const nlohmann::json& j) override {
        if(j.size()!=5)throw std::invalid_argument("S3 parameters");
        root_=vision::inference::utf8_path(j.at("root").get<std::string>());
        if(!std::filesystem::is_directory(root_)||std::filesystem::is_symlink(root_))
            throw std::invalid_argument("S3 archive root");
        endpoint_=j.at("endpoint").get<std::string>();bucket_=j.at("bucket").get<std::string>();
        auto secret=[&](const char* field){
            const auto variable=j.at(field).get<std::string>();
            if(variable.empty()||variable.size()>128)throw std::invalid_argument("S3 secret reference");
            const auto* value=std::getenv(variable.c_str());
            if(!value||!*value||std::strlen(value)>256)throw std::invalid_argument("S3 secret");
            return std::string(value);
        };
        s3_=std::make_unique<vision::storage::S3>(endpoint_,bucket_,secret("access_env"),secret("secret_env"));
    }
    void write(const nlohmann::json& request) override {
        using namespace vision;
        const auto event=serialization::decode_result(request.at("event").dump());
        struct Lock {
            HANDLE value;
            ~Lock(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        } lock{CreateFileW((root_/L".upload-lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(lock.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Uploader busy");
        storage::Store store(root_);
        const auto committed=storage::find_result(root_,event.event_id.value());
        if(committed!=serialization::encode_result(event))throw std::runtime_error("Upload requires committed result");
        const auto frames=store.frames(event.result.run_id.value(),event.result.inspection_id.value());
        if(frames.empty()||frames.size()>4)throw std::runtime_error("Archive frame count");
        for(const auto& frame:frames) {
            if(stopped_)throw std::runtime_error("Upload stopped");
            const auto image=store.image(frame.image);
            const auto hash=inference::sha256(image);
            // A content-addressed key makes identical retries safe without sharing SDK buffers.
            const auto key="frames/"+frame.image+"/"+hash.substr(7);
            store.bind_object({frame.image,endpoint_,bucket_,key,0});
            const auto state=store.image_state(frame.image);
            unsigned attempt=0;
            try {
                if(state!="Available") {
                    attempt=store.start_upload(frame.image);
                    s3_->put(key,image);
                }
                if(stopped_)throw std::runtime_error("Upload stopped");
                if(inference::sha256(s3_->get(key))!=hash)throw std::runtime_error("Remote image hash");
                if(stopped_)throw std::runtime_error("Upload stopped");
                if(store.image_state(frame.image)=="Pending")store.available(frame.image);
            }catch(...){
                if(attempt>=3)store.fail_image(frame.image);
                throw;
            }
        }
    }
    std::filesystem::path root_;std::string endpoint_,bucket_;
    std::unique_ptr<vision::storage::S3> s3_;
};
class Output final:public vision::plugin_sdk::ObjectStorage {
public:
    vision::plugin_sdk::Status initialize(vision::plugin_sdk::Bytes p) noexcept override{return upload_.initialize(p);}
    void request_stop() noexcept override{upload_.request_stop();}
    vision::plugin_sdk::Status try_operation(vision::plugin_sdk::Bytes p) noexcept override{return upload_.try_submit(p);}
    vision::plugin_sdk::Status poll_completion(vision::plugin_sdk::Buffer& p) noexcept override{return upload_.poll_report(p);}
private:
    Upload upload_;
};
}
#define VISION_OUTPUT_ID "vision.s3-storage"
#define VISION_OUTPUT_BUILD "s3-storage-v1"
#define VISION_PLUGIN_KIND ObjectStorage
#include "../output-common/exports.hpp"
