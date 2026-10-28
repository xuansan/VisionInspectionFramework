#include "engine.hpp"
#include <vision/plugin_sdk/api.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <atomic>
#include <cstring>
#include <fstream>
using namespace vision;
using namespace plugin_sdk;
namespace {
class AlgorithmPlugin final : public Algorithm {
public:
    Status initialize(Bytes input) noexcept override {
        try {
            const auto p=nlohmann::json::parse(input.data,input.data+input.size);
            if(!p.is_object()||p.size()!=1||!p.contains("config"))return Status::Invalid;
            const auto path=opencv_plugin::utf8_path(p.at("config").get<std::string>());
            if(std::filesystem::file_size(path)>65536)return Status::Invalid;
            std::ifstream file(path,std::ios::binary);nlohmann::json config;file>>config;
            engine_=std::make_unique<opencv_plugin::Engine>(config,path.parent_path());
            return stopped_?Status::Cancelled:Status::Ok;
        }catch(...) {return stopped_?Status::Cancelled:Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status execute(Bytes input,Buffer& output) noexcept override {
        if(stopped_)return Status::Cancelled;
        if(!engine_)return Status::Invalid;
        nlohmann::json evidence;
        try {
            const auto p=nlohmann::json::parse(input.data,input.data+input.size);
            const auto bytes=contracts::parse_u64(p.at("slot_bytes").get<std::string>());
            const auto budget=contracts::parse_u64(p.at("budget_ns").get<std::string>());
            if(!budget||budget>10000000000ULL||bytes>64*1024*1024)return Status::Invalid;
            const auto frame=serialization::decode_frame(p.at("read_frame").dump(),bytes);
            auto mapping=frame_transport::Mapping::open({frame.permit.run,frame.permit.pool,p.at("slot_count"),bytes},
                frame_transport::Access::Reader);
            if(mapping->read(frame,10,[&](auto pixels){evidence=engine_->run(pixels,frame.layout,stopped_,budget);})
                !=frame_transport::MemoryStatus::Ok)throw std::runtime_error("Image lease unavailable");
            mapping.reset();
        }catch(const std::exception& e) {
            if(stopped_)return Status::Cancelled;
            evidence={{"execution_state","Failed"},{"quality","Unknown"},{"defects",nlohmann::json::array()},
                {"measurements",nlohmann::json::array()},{"elapsed_ns","0"},
                {"error",{{"code","OPENCV.EXECUTION"},{"category","Execution"},{"message",std::string(e.what()).substr(0,240)},
                    {"retryability","Never"},{"origin","vision.opencv-algorithm"}}}};
        }catch(...) {return stopped_?Status::Cancelled:Status::Failed;}
        if(stopped_)return Status::Cancelled;
        try {
            const auto response=nlohmann::json{{"evidence",evidence}}.dump();
            if(!output.data||response.size()>output.capacity)return Status::Invalid;
            std::memcpy(output.data,response.data(),response.size());output.size=static_cast<std::uint32_t>(response.size());
            return Status::Ok;
        }catch(...) {return Status::Failed;}
    }
private:
    std::unique_ptr<opencv_plugin::Engine> engine_;
    std::atomic<bool> stopped_{};
};
template<std::size_t N> void copy(char(&out)[N],const char* text) {
    const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);
}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::Algorithm;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.opencv-algorithm");copy(out->plugin_version,"0.1.0");copy(out->build_id,"opencv-algorithm-v1");
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
    auto* instance=dynamic_cast<AlgorithmPlugin*>(p);if(!instance)return Status::Invalid;
    delete instance;return Status::Ok;
}
