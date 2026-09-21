#include <vision/plugin_sdk/api.hpp>
#include <vision/algorithm/brightness.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#define NOMINMAX
#include <windows.h>
using namespace vision;
using namespace plugin_sdk;
namespace {
class Basic final : public Algorithm {
public:
    Status initialize(Bytes parameters) noexcept override {
        try {
            auto p=nlohmann::json::parse(parameters.data,parameters.data+parameters.size);
            minimum_=p.value("minimum",0U);maximum_=p.value("maximum",255U);
            fault_=p.value("fault",std::string("none"));delay_=p.value("delay_ms",200U);
            return minimum_<=maximum_&&maximum_<=255?Status::Ok:Status::Invalid;
        } catch(...) {return Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status execute(Bytes request,Buffer& output) noexcept override {
        try {
            if(stopped_)return Status::Cancelled;
            const auto p=nlohmann::json::parse(request.data,request.data+request.size);
            const auto bytes=contracts::parse_u64(p.at("slot_bytes").get<std::string>());
            const auto frame=serialization::decode_frame(p.at("read_frame").dump(),bytes);
            const auto count=p.at("slot_count").get<std::uint32_t>();
            auto mapping=frame_transport::Mapping::open({frame.permit.run,frame.permit.pool,count,bytes},frame_transport::Access::Reader);
            bool pass=false;
            const auto status=mapping->read(frame,10,[&](auto pixels) {
                if(fault_=="crash")TerminateProcess(GetCurrentProcess(),73);
                if(fault_=="hang")for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if(fault_=="delay") {
                    const auto due=std::chrono::steady_clock::now()+std::chrono::milliseconds(delay_);
                    while(std::chrono::steady_clock::now()<due&&!stopped_)std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pass=algorithm::brightness(pixels,frame.layout,minimum_,maximum_,[&]{return stopped_.load();}).pass;
            });
            mapping.reset(); // No access may survive the terminal response.
            if(stopped_)return Status::Cancelled;
            if(status!=frame_transport::MemoryStatus::Ok)return Status::Failed;
            const auto text=nlohmann::json{{"execution_state","Succeeded"},{"quality",pass?"OK":"NG"},
                {"result_ref",frame.frame.value()},{"error_code",nullptr}}.dump();
            if(text.size()>output.capacity)return Status::Invalid;
            std::memcpy(output.data,text.data(),text.size());output.size=static_cast<std::uint32_t>(text.size());
            return Status::Ok;
        } catch(...) {return stopped_?Status::Cancelled:Status::Failed;}
    }
private:
    std::atomic<bool> stopped_{};
    std::uint32_t minimum_{},maximum_{255},delay_{200};
    std::string fault_;
};
template<std::size_t N> void copy(char(&out)[N],const char* text) {
    const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);
}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::Algorithm;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.basic-algorithm");copy(out->plugin_version,"0.1.0");copy(out->build_id,"basic-algorithm-v1");
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
    try {*out=new Basic;return Status::Ok;}catch(...) {*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {
    auto instance=dynamic_cast<Basic*>(p);if(!instance)return Status::Invalid;
    delete instance;return Status::Ok;
}
