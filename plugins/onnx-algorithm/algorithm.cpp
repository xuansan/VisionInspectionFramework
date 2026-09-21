#include <vision/plugin_sdk/api.hpp>
#include <vision/inference/engine.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstring>
#include <mutex>
using namespace vision;
using namespace plugin_sdk;
namespace {
class AlgorithmPlugin final : public Algorithm {
public:
    Status initialize(Bytes input) noexcept override {
        try {
            const auto p=nlohmann::json::parse(input.data,input.data+input.size);
            auto engine=std::make_unique<inference::Engine>(inference::read_config(inference::utf8_path(p.at("config").get<std::string>())));
            if(engine->config().type=="instance_segmentation") {
                mask_root_=inference::utf8_path(p.at("mask_root").get<std::string>());
                if(!std::filesystem::is_directory(mask_root_))return Status::Invalid;
            }
            std::lock_guard lock(mutex_);engine_=std::move(engine);if(stopped_)engine_->cancel();
            return stopped_?Status::Cancelled:Status::Ok;
        }catch(...) {return Status::Failed;}
    }
    void request_stop() noexcept override {
        stopped_=true;std::lock_guard lock(mutex_);if(engine_)engine_->cancel();
    }
    Status execute(Bytes input,Buffer& output) noexcept override {
        try {
            if(stopped_||!engine_)return Status::Cancelled;
            const auto p=nlohmann::json::parse(input.data,input.data+input.size);
            const auto bytes=contracts::parse_u64(p.at("slot_bytes").get<std::string>());
            const auto frame=serialization::decode_frame(p.at("read_frame").dump(),bytes);
            auto mapping=frame_transport::Mapping::open({frame.permit.run,frame.permit.pool,p.at("slot_count"),bytes},frame_transport::Access::Reader);
            inference::Output result;
            if(mapping->read(frame,10,[&](auto pixels){result=engine_->run(pixels,frame.layout);})!=frame_transport::MemoryStatus::Ok)
                return Status::Failed;
            mapping.reset();
            if(stopped_)return Status::Cancelled;
            nlohmann::json evidence{{"execution_state","Succeeded"},{"quality",result.detections.empty()?"OK":"NG"},
                {"defects",nlohmann::json::array()},{"measurements",nlohmann::json::array()},{"elapsed_ns","0"},{"model_hash",result.model_hash}};
            if(engine_->config().type=="classification")
                evidence["quality"]=!result.classification?"Unknown":*result.classification==engine_->config().ok_label?"OK":"NG";
            if(engine_->config().type=="classification"&&!result.classification) {
                evidence["execution_state"]="Failed";
                evidence["error"]={{"code","INFERENCE.LOW_CONFIDENCE"},{"category","Execution"},
                    {"message","No class reached the configured confidence threshold"},{"retryability","Never"},{"origin","vision.onnx-algorithm"}};
            }
            for(const auto& d:result.detections)evidence["defects"].push_back({{"class_id",d.class_id},{"label",d.label},{"score",d.score},
                {"box_xyxy",{d.x1,d.y1,d.x2,d.y2}},{"coordinate_space","original_pixels"}});
            if(result.classification)evidence["classification"]=*result.classification;
            for(const auto& m:result.measurements)evidence["measurements"].push_back({{"name",m.name},{"value",m.value},{"unit",m.unit}});
            if(!result.masks.empty()) {
                evidence["masks"]=nlohmann::json::array();
                for(const auto& mask:inference::store_masks(mask_root_,result.masks))
                    evidence["masks"].push_back({{"hash",mask.hash},{"width",mask.width},{"height",mask.height},
                        {"instance",mask.instance},{"storage","local_ephemeral"}});
            }
            const auto response=nlohmann::json{{"evidence",evidence}}.dump();
            if(response.size()>output.capacity)return Status::Invalid;
            std::memcpy(output.data,response.data(),response.size());output.size=static_cast<std::uint32_t>(response.size());
            return Status::Ok;
        }catch(...) {return stopped_?Status::Cancelled:Status::Failed;}
    }
private:
    std::mutex mutex_;std::unique_ptr<inference::Engine> engine_;std::atomic<bool> stopped_{};
    std::filesystem::path mask_root_;
};
template<std::size_t N> void copy(char(&out)[N],const char* text) {const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::Algorithm;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.onnx-algorithm");copy(out->plugin_version,"0.1.0");copy(out->build_id,"onnx-algorithm-v1");
    copy(out->platform,"windows");copy(out->architecture,"x64");copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(out->runtime_variant,"Debug");
#else
    copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** out) noexcept {
    if(!out)return Status::Invalid;
    try {*out=new AlgorithmPlugin;return Status::Ok;}catch(...) {*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {
    auto instance=dynamic_cast<AlgorithmPlugin*>(p);if(!instance)return Status::Invalid;delete instance;return Status::Ok;
}
